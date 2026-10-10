// Smoother online fights (lobby mode, both PCs on this build):
//
// - Per-frame input channel. The game sends its inputs in packs of 3 frames
//   from a 60 Hz network thread, so the first frame of every pack has only one
//   driver tick of slack and the battle is held whenever the one-way latency
//   (plus the game's polling) is above that. Here every driver tick sends the
//   local command words of the frames the other PC hasn't confirmed yet over
//   the transport's side channel (kind 16), and frames received that way are
//   written into the game's input rings right before the driver looks for them
//   - the same frame-keyed write the game's own receive path does
//   (sub_8217E420), so the copies that still come through the game's packets
//   are harmless. The network budget becomes the whole input delay.
// - Exact input delay (online_input_delay, 2-8 frames, the host's value is
//   used by both): the driver runs every frame and takes frame [M+1588] - D
//   instead of - 3; the match reset starts the frame counter at D rounded up
//   to a multiple of 3 (the game's own packs stay aligned) and pre-fills
//   neutral input below it.
// - Desync check: a hash of the battle clock and both fighters' position,
//   action and ki at every consumed frame, exchanged in the same frames; the
//   first frame where the two PCs disagree is logged.
// - Optional: during a short hold the effect / HUD time steps are zeroed, so
//   effects don't keep moving on the PC that waits (online_hold_freeze).
//
// Research and addresses: C:/rex/_online_tests/netcode_research.md.

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/memory.h>
#include <rex/memory/utils.h>
#include <rex/net/online.h>
#include <rex/net/session.h>
#include <rex/ppc/context.h>
#include <rex/runtime.h>

REXCVAR_DEFINE_STRING(online_input_delay, "4", "Patches",
                      "Online (lobby): input delay in frames, the host's value is used by both "
                      "players. Higher hides more ping (a frame is 16.7 ms: 4 frames = up to "
                      "~60 ms one way without stops). off = the game's own timing "
                      "(online_tick_sleep).")
    .allowed({"off", "2", "3", "4", "5", "6", "7", "8"})
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(online_input_channel, true, "Patches",
                    "Online (lobby): send every frame's input right away over the connection's "
                    "side channel (off = only the game's own 3-frame packs, for comparisons).")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(online_desync_check, true, "Patches",
                    "Online (lobby): compare a hash of the fight's state with the other PC every "
                    "frame and log the first difference ([OnlineDesync]).")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(online_hold_freeze, false, "Patches",
                    "Online: while the fight waits for the other player's input (short waits "
                    "only), also stop the effect and HUD timers.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

// burstlimit_forms.cpp: the other PC's start forms (side-channel kind 17).
bool BurstLimitFormsOnSide(uint8_t kind, const uint8_t* body, size_t size);

namespace {

// --- Guest addresses -----------------------------------------------------------

// Match input manager M (pointer). Per-player blocks of 784 bytes at M and
// M + 784: a ring of 32 entries x 24 bytes (+0 u64 command, +8 status A,
// +12 status B, +16 frame), then +768 read index, +772 write index, +776
// highest frame seen.
constexpr uint32_t kInputManagerPointer = 0x841B5EA4;
constexpr uint32_t kPlayerBlock = 784;
constexpr uint32_t kRingSlots = 32;
constexpr uint32_t kSlotSize = 24;
constexpr uint32_t kSlotStatusA = 8;
constexpr uint32_t kSlotStatusB = 12;
constexpr uint32_t kSlotFrame = 16;
constexpr uint32_t kRingWriteIndex = 772;
constexpr uint32_t kRingHighest = 776;
constexpr uint32_t kTickFrame = 1588;       // frame of the current driver call
constexpr uint32_t kSubmittedFrame = 1592;  // last local frame packed
constexpr uint32_t kDriverState = 1632;     // 3 = online match
constexpr uint32_t kDriverStateMatch = 3;

// The game's local input history (12 slots by frame % 12): +0 frame,
// +48 status B, +96 player, +144 status A, +208 u64 command.
constexpr uint32_t kHistory = 0x842545E8;
constexpr uint32_t kHistorySlots = 12;

// Battle object (clock at +276, flags at +260, step at +252) and the effect
// time steps the clock writes (sub_82110A80).
constexpr uint32_t kBattlePointer = 0x841B5130;
constexpr uint32_t kBattleStep = 252;
constexpr uint32_t kBattleClock = 276;
constexpr uint32_t kEffectStep = 0x840D62C0;
constexpr uint32_t kEffectStepSeconds = 0x840D6AC8;

// Player table (pointers), fighter = *(player + 44): +0x90 position x/y/z,
// +0x750 action, +1244 ki (u16) and max ki (u16).
constexpr uint32_t kPlayerTable = 0x84239C10;
constexpr uint32_t kPlayerFighter = 44;
constexpr uint32_t kFighterPosition = 0x90;
constexpr uint32_t kFighterAction = 0x750;
constexpr uint32_t kFighterKi = 1244;

// The side-channel frame: [u8 version][u8 player][u8 count][u8 flags]
// [s32 ack][s32 first][s32 sender frame][s32 hash frame][u32 hash][u8 delay]
// [3 reserved] then count x [u64 command][u32 status A][u32 status B],
// little-endian.
constexpr uint8_t kSideInput = rex::net::online::kGameSideKindFirst;  // 16
constexpr uint8_t kInputVersion = 1;
constexpr size_t kInputHeader = 28;
constexpr size_t kInputEntry = 16;
constexpr uint8_t kFlagHash = 1;
constexpr int kMaxWindow = 16;

// Hold freeze: only the first frames of a hold (a long wait shows the game's
// own "Sending data..." notice, which must still animate).
constexpr uint32_t kHoldFreezeFrames = 30;

int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

rex::memory::Memory* GuestMemory() {
  auto* runtime = rex::Runtime::instance();
  return runtime ? runtime->memory() : nullptr;
}

bool Readable(rex::memory::Memory* memory, uint32_t address, uint32_t size) {
  if (address < 0x10000) {
    return false;
  }
  auto* heap = memory->LookupHeap(address);
  rex::memory::HeapAllocationInfo info = {};
  return heap && heap->QueryRegionInfo(address, &info) &&
         (info.state & rex::memory::kMemoryAllocationCommit) &&
         uint64_t(address) + size <= uint64_t(info.base_address) + info.region_size;
}

uint32_t LoadU32(rex::memory::Memory* memory, uint32_t address) {
  return rex::memory::load_and_swap<uint32_t>(memory->TranslateVirtual<uint8_t*>(address));
}
int32_t LoadS32(rex::memory::Memory* memory, uint32_t address) {
  return static_cast<int32_t>(LoadU32(memory, address));
}
uint64_t LoadU64(rex::memory::Memory* memory, uint32_t address) {
  return rex::memory::load_and_swap<uint64_t>(memory->TranslateVirtual<uint8_t*>(address));
}
void StoreU32(rex::memory::Memory* memory, uint32_t address, uint32_t value) {
  rex::memory::store_and_swap<uint32_t>(memory->TranslateVirtual<uint8_t*>(address), value);
}
void StoreU64(rex::memory::Memory* memory, uint32_t address, uint64_t value) {
  rex::memory::store_and_swap<uint64_t>(memory->TranslateVirtual<uint8_t*>(address), value);
}

void PutU32(uint8_t* p, uint32_t v) {
  for (int i = 0; i < 4; ++i) p[i] = uint8_t(v >> (8 * i));
}
void PutU64(uint8_t* p, uint64_t v) {
  for (int i = 0; i < 8; ++i) p[i] = uint8_t(v >> (8 * i));
}
uint32_t GetU32(const uint8_t* p) {
  uint32_t v = 0;
  for (int i = 3; i >= 0; --i) v = (v << 8) | p[i];
  return v;
}
uint64_t GetU64(const uint8_t* p) {
  uint64_t v = 0;
  for (int i = 7; i >= 0; --i) v = (v << 8) | p[i];
  return v;
}

struct InputEntry {
  int32_t frame = -1;
  uint64_t command = 0;
  uint32_t status_a = 0;
  uint32_t status_b = 0;
};

// --- Received frames (network thread -> game thread) ----------------------------

struct RemoteEntry {
  int32_t frame;
  uint8_t player;
  uint64_t command;
  uint32_t status_a;
  uint32_t status_b;
};

struct Inbox {
  std::mutex mutex;
  std::vector<RemoteEntry> entries;
  std::vector<std::pair<int32_t, uint32_t>> hashes;  // frame, hash
  bool has_ack = false;
  int32_t ack = 0;
  int32_t sender_frame = 0;
  uint8_t sender_delay = 0;
  uint64_t packets = 0;
  uint64_t bad = 0;
} g_inbox;

void OnGameSide(uint8_t kind, const uint8_t* body, size_t size) {
  if (BurstLimitFormsOnSide(kind, body, size)) {
    return;
  }
  if (kind != kSideInput || size < kInputHeader || body[0] != kInputVersion) {
    std::lock_guard<std::mutex> lock(g_inbox.mutex);
    ++g_inbox.bad;
    return;
  }
  const uint8_t player = body[1];
  const uint8_t count = body[2];
  const uint8_t flags = body[3];
  if (player > 1 || count > kMaxWindow || size < kInputHeader + size_t(count) * kInputEntry) {
    std::lock_guard<std::mutex> lock(g_inbox.mutex);
    ++g_inbox.bad;
    return;
  }
  const int32_t ack = static_cast<int32_t>(GetU32(body + 4));
  const int32_t first = static_cast<int32_t>(GetU32(body + 8));
  const int32_t sender_frame = static_cast<int32_t>(GetU32(body + 12));
  const int32_t hash_frame = static_cast<int32_t>(GetU32(body + 16));
  const uint32_t hash = GetU32(body + 20);
  std::lock_guard<std::mutex> lock(g_inbox.mutex);
  ++g_inbox.packets;
  g_inbox.has_ack = true;
  g_inbox.ack = ack;
  g_inbox.sender_frame = sender_frame;
  g_inbox.sender_delay = body[24];
  if (g_inbox.entries.size() < 1024) {
    for (uint8_t i = 0; i < count; ++i) {
      const uint8_t* e = body + kInputHeader + size_t(i) * kInputEntry;
      g_inbox.entries.push_back(
          {first + i, player, GetU64(e), GetU32(e + 8), GetU32(e + 12)});
    }
  }
  if ((flags & kFlagHash) && g_inbox.hashes.size() < 256) {
    g_inbox.hashes.emplace_back(hash_frame, hash);
  }
}

// --- Match state (game thread; the match reset can also run on the game's
// network thread, hence the mutex) ------------------------------------------------

struct HashRecord {
  int32_t frame = -1;
  uint32_t hash = 0;
  float clock = 0.0f;
  float position[2][3] = {};
  uint32_t action[2] = {};
  uint32_t ki[2] = {};
};

struct Match {
  // Latched at the match reset (sub_8217E650).
  bool online = false;   // lobby session
  bool channel = false;  // per-frame input channel
  int delay = 0;         // exact delay in frames, 0 = the game's (3 driver ticks)
  int start_frame = 3;
  bool desync_check = false;
  bool hold_freeze = false;

  int local_player = -1;
  int32_t newest_local = -1;
  std::array<InputEntry, 64> local{};
  std::array<HashRecord, 128> local_hashes{};
  std::array<std::pair<int32_t, uint32_t>, 128> remote_hashes{};
  bool desync_reported = false;
  Match() {
    for (auto& h : remote_hashes) {
      h.first = -1;
    }
  }
  int32_t last_hashed = -1;

  // Diagnostics: the statuses the game puts in its input entries (fight
  // start / pause events) and the synchronized-event flag.
  uint32_t last_status_a = 0xFFFFFFFF, last_status_b = 0xFFFFFFFF;
  uint32_t last_remote_a = 0xFFFFFFFF, last_remote_b = 0xFFFFFFFF;
  uint32_t last_sync_flag = 0xFFFFFFFF;

  // Hold freeze.
  bool frozen = false;
  uint32_t hold_run = 0;
  float saved_step = 1.0f, saved_effect = 1.0f, saved_effect_seconds = 1.0f / 60.0f;

  // Stats (a line every 10 s and a match total).
  struct Stats {
    uint64_t sent = 0, received = 0, entries_sent = 0, injected_new = 0, injected_known = 0,
             stale = 0, hash_ok = 0, hash_bad = 0, local_pressed = 0, remote_pressed = 0,
             hash_changes = 0;
    int max_window = 0;
  } window, total;
  int64_t window_start_ms = 0;
};

std::mutex g_match_mutex;
Match g_match;
// Copies the hooks read without the lock.
std::atomic<int> g_delay{0};
std::atomic<int> g_start_frame{3};

void ResetMatchLocked(Match& m) {
  Match fresh;
  fresh.window_start_ms = NowMs();
  m = fresh;
  std::lock_guard<std::mutex> lock(g_inbox.mutex);
  g_inbox.entries.clear();
  g_inbox.hashes.clear();
  g_inbox.has_ack = false;
}

// sub_8217E420's frame-keyed ring write, for player `player`'s block. Returns
// 1 for a new frame, 0 when the slot already had it, -1 when it holds a newer
// frame.
int WriteRing(rex::memory::Memory* memory, uint32_t manager, int player, int32_t frame,
              uint64_t command, uint32_t status_a, uint32_t status_b) {
  const uint32_t block = manager + uint32_t(player) * kPlayerBlock;
  if (LoadS32(memory, block + kRingHighest) < frame) {
    StoreU32(memory, block + kRingHighest, uint32_t(frame));
  }
  const uint32_t slot = block + (uint32_t(frame) % kRingSlots) * kSlotSize;
  const int32_t slot_frame = LoadS32(memory, slot + kSlotFrame);
  if (slot_frame > frame) {
    return -1;
  }
  StoreU64(memory, slot, command);
  if (slot_frame == frame) {
    return 0;
  }
  StoreU32(memory, slot + kSlotStatusA, status_a);
  StoreU32(memory, slot + kSlotStatusB, status_b);
  StoreU32(memory, slot + kSlotFrame, uint32_t(frame));
  StoreU32(memory, block + kRingWriteIndex,
           (LoadU32(memory, block + kRingWriteIndex) + 1) % kRingSlots);
  return 1;
}

bool RingHas(rex::memory::Memory* memory, uint32_t manager, int player, int32_t frame) {
  const uint32_t block = manager + uint32_t(player) * kPlayerBlock;
  return LoadS32(memory, block + (uint32_t(frame) % kRingSlots) * kSlotSize + kSlotFrame) ==
         frame;
}

// FNV-1a over 32-bit words.
uint32_t Mix(uint32_t hash, uint32_t value) {
  for (int i = 0; i < 4; ++i) {
    hash = (hash ^ ((value >> (8 * i)) & 0xFF)) * 16777619u;
  }
  return hash;
}

uint32_t FloatBits(float value) {
  uint32_t bits;
  std::memcpy(&bits, &value, 4);
  return bits;
}

// The fight's state at a consumed frame. Fighters are combined order-free (in
// case the table is local-first on one PC).
bool ComputeHash(rex::memory::Memory* memory, int32_t frame, HashRecord* out) {
  const uint32_t battle = LoadU32(memory, kBattlePointer);
  if (!Readable(memory, battle + kBattleClock, 4) || !Readable(memory, kPlayerTable, 8)) {
    return false;
  }
  HashRecord record;
  record.frame = frame;
  record.clock = rex::memory::load_and_swap<float>(
      memory->TranslateVirtual<uint8_t*>(battle + kBattleClock));
  uint32_t fighters_hash = 0;
  for (int i = 0; i < 2; ++i) {
    const uint32_t player = LoadU32(memory, kPlayerTable + 4 * i);
    if (!Readable(memory, player + kPlayerFighter, 4)) {
      return false;
    }
    const uint32_t fighter = LoadU32(memory, player + kPlayerFighter);
    if (!Readable(memory, fighter, kFighterKi + 4)) {
      return false;
    }
    uint32_t h = 2166136261u;
    for (int axis = 0; axis < 3; ++axis) {
      const uint32_t bits = LoadU32(memory, fighter + kFighterPosition + 4 * axis);
      std::memcpy(&record.position[i][axis], &bits, 4);
      h = Mix(h, bits);
    }
    record.action[i] = LoadU32(memory, fighter + kFighterAction);
    record.ki[i] = LoadU32(memory, fighter + kFighterKi);
    h = Mix(h, record.action[i]);
    h = Mix(h, record.ki[i]);
    fighters_hash ^= h;
  }
  record.hash = Mix(Mix(2166136261u, FloatBits(record.clock)), fighters_hash);
  *out = record;
  return true;
}

void CompareHash(Match& m, int32_t frame) {
  const HashRecord& local = m.local_hashes[uint32_t(frame) % m.local_hashes.size()];
  const auto& remote = m.remote_hashes[uint32_t(frame) % m.remote_hashes.size()];
  if (local.frame != frame || remote.first != frame) {
    return;
  }
  if (local.hash == remote.second) {
    ++m.window.hash_ok;
    ++m.total.hash_ok;
    return;
  }
  ++m.window.hash_bad;
  ++m.total.hash_bad;
  if (!m.desync_reported) {
    m.desync_reported = true;
    REXLOG_WARN(
        "[OnlineDesync] first mismatch at input frame {}: local hash {:08X}, remote {:08X} | "
        "local clock={} p0 pos=({}, {}, {}) action={:08X} ki={:08X} | p1 pos=({}, {}, {}) "
        "action={:08X} ki={:08X}",
        frame, local.hash, remote.second, local.clock, local.position[0][0],
        local.position[0][1], local.position[0][2], local.action[0], local.ki[0],
        local.position[1][0], local.position[1][1], local.position[1][2], local.action[1],
        local.ki[1]);
  }
}

void LogWindow(Match& m, const char* label, const Match::Stats& s) {
  REXLOG_WARN(
      "[OnlineInput] {}: delay={} channel={} sent={} recv={} entries_sent={} max_window={} "
      "injected new={} known={} stale={} hash ok={} bad={} changes={} input frames local={} "
      "remote={} clock={} rtt={}ms",
      label, m.delay ? std::to_string(m.delay) + "f" : std::string("game"), m.channel ? "on" : "off",
      s.sent, s.received, s.entries_sent, s.max_window, s.injected_new, s.injected_known, s.stale,
      s.hash_ok, s.hash_bad, s.hash_changes, s.local_pressed, s.remote_pressed,
      m.last_hashed >= 0 ? m.local_hashes[uint32_t(m.last_hashed) % m.local_hashes.size()].clock
                         : 0.0f,
      rex::net::online::PeerRttMs());
}

void FlushMatchLocked(Match& m, const char* why) {
  if (!m.online) {
    return;
  }
  if (m.total.sent || m.total.received) {
    LogWindow(m, why, m.total);
  }
}

void ThawLocked(Match& m, rex::memory::Memory* memory) {
  if (!m.frozen) {
    return;
  }
  m.frozen = false;
  const uint32_t battle = LoadU32(memory, kBattlePointer);
  if (Readable(memory, battle + kBattleStep, 4)) {
    rex::memory::store_and_swap<float>(memory->TranslateVirtual<uint8_t*>(battle + kBattleStep),
                                       m.saved_step);
  }
  rex::memory::store_and_swap<float>(memory->TranslateVirtual<uint8_t*>(kEffectStep),
                                     m.saved_effect);
  rex::memory::store_and_swap<float>(memory->TranslateVirtual<uint8_t*>(kEffectStepSeconds),
                                     m.saved_effect_seconds);
}

}  // namespace

// --- Called from the rest of the game ---------------------------------------------

void BurstLimitNetInputSetup() {
  rex::net::online::SetGameSideHandler(OnGameSide);
}

/// The exact input delay of the running online match (0 = the game's timing).
int BurstLimitOnlineDelayFrames() {
  return g_delay.load(std::memory_order_relaxed);
}

/// Every game tick (BurstLimitOnlineFrame): the 10 s line, and the end of a
/// match.
void BurstLimitNetInputFrame(rex::memory::Memory* memory, bool in_match) {
  std::lock_guard<std::mutex> lock(g_match_mutex);
  Match& m = g_match;
  if (!in_match) {
    ThawLocked(m, memory);
    return;
  }
  if (!m.online) {
    return;
  }
  const int64_t now = NowMs();
  if (now - m.window_start_ms >= 10000) {
    if (m.window.sent || m.window.received) {
      LogWindow(m, "10s", m.window);
    }
    m.window = {};
    m.window_start_ms = now;
  }
}

/// The match total, when the [OnlineStalls] counter closes a match.
void BurstLimitNetInputMatchEnd(const char* why) {
  std::lock_guard<std::mutex> lock(g_match_mutex);
  FlushMatchLocked(g_match, why);
  g_match.total = {};
}

// --- Mid-asm hooks (burstlimit_manifest.toml) -------------------------------------

// sub_8217E650 (match reset), li r9,3 = the first input frame: decide this
// match's mode. Online with an exact delay D, the counter starts at D rounded
// up to a multiple of 3 (the game's packs of 3 stay aligned).
void BurstLimitOnlineResetFrame(PPCRegister& r9) {
  const bool online = rex::net::IsGameSessionOpen() && rex::net::online::IsLobbyMode();
  int delay = 0;
  if (online) {
    const std::string value = REXCVAR_GET(online_input_delay);
    if (value != "off") {
      delay = std::clamp(std::atoi(value.c_str()), 2, 8);
    }
  }
  const int start = delay ? std::max(3, (delay + 2) / 3 * 3) : 3;
  {
    std::lock_guard<std::mutex> lock(g_match_mutex);
    if (auto* memory = GuestMemory()) {
      ThawLocked(g_match, memory);
    }
    FlushMatchLocked(g_match, "reset");
    ResetMatchLocked(g_match);
    g_match.online = online;
    g_match.channel = online && REXCVAR_GET(online_input_channel);
    g_match.delay = delay;
    g_match.start_frame = start;
    g_match.desync_check = online && REXCVAR_GET(online_desync_check);
    g_match.hold_freeze = online && REXCVAR_GET(online_hold_freeze);
  }
  g_delay.store(delay);
  g_start_frame.store(start);
  if (online) {
    REXLOG_WARN("[OnlineInput] match reset: delay {} (first frame {}), input channel {}, "
                "desync check {}",
                delay ? std::to_string(delay) + " frames" : std::string("game (3 ticks)"), start,
                g_match.channel ? "on" : "off", g_match.desync_check ? "on" : "off");
  }
  if (delay) {
    r9.u64 = uint64_t(start);
  }
}

// sub_8217E650, cmpwi cr6,r31,3: neutral input is pre-filled for every frame
// below the first one.
void BurstLimitOnlineResetPrefill(PPCRegister& r31, PPCCRRegister& cr6) {
  if (g_delay.load(std::memory_order_relaxed)) {
    const int32_t value = r31.s32;
    const int32_t limit = g_start_frame.load(std::memory_order_relaxed);
    cr6.lt = value < limit;
    cr6.gt = value > limit;
    cr6.eq = value == limit;
  }
}

// sub_82179DC8 (battle start), cmpwi cr6,r3,6: the game starts the fight
// when the next input frame reaches 6 - its first frame (3) plus its delay
// (3), when the neutral input pre-filled at the reset is used up and the
// players' own input begins. With an exact delay that's first frame + D.
void BurstLimitOnlineFightStartFrame(PPCRegister& r3, PPCCRRegister& cr6) {
  const int delay = g_delay.load(std::memory_order_relaxed);
  if (!delay) {
    return;
  }
  const int32_t value = r3.s32;
  const int32_t target = g_start_frame.load(std::memory_order_relaxed) + delay;
  cr6.lt = value < target;
  cr6.gt = value > target;
  cr6.eq = value == target;
}

// sub_8217E4E8 (consume), addi r5,r10,-3: the frame taken this tick.
void BurstLimitOnlineConsumeFrame(PPCRegister& r5, PPCRegister& r10) {
  const int delay = g_delay.load(std::memory_order_relaxed);
  if (!delay) {
    return;
  }
  auto* memory = GuestMemory();
  if (!memory) {
    return;
  }
  // Only the online driver (state 3); offline (state 0) keeps the game's 3.
  const uint32_t manager = LoadU32(memory, kInputManagerPointer);
  if (manager && LoadU32(memory, manager + kDriverState) == kDriverStateMatch) {
    r5.s64 = int64_t(r10.s32) - delay;
  }
}

// sub_8217E860 (online driver, state 3), right before the consume: this
// tick's local frame goes into the local ring and out to the other PC, and
// the frames the other PC sent go into its ring.
void BurstLimitOnlineBeforeConsume() {
  auto* memory = GuestMemory();
  if (!memory) {
    return;
  }
  std::lock_guard<std::mutex> lock(g_match_mutex);
  Match& m = g_match;
  if (!m.channel) {
    return;
  }
  const uint32_t manager = LoadU32(memory, kInputManagerPointer);
  if (!Readable(memory, manager, 2 * kPlayerBlock + 64)) {
    return;
  }
  const int32_t frame = LoadS32(memory, manager + kTickFrame);
  const int delay = m.delay ? m.delay : 3;
  const int32_t consume = frame - delay;

  // 1. The local frame packed this tick (sub_8229CD28 -> history).
  if (LoadS32(memory, manager + kSubmittedFrame) == frame && frame >= 0) {
    const uint32_t index = uint32_t(frame) % kHistorySlots;
    if (LoadS32(memory, kHistory + 4 * index) == frame) {
      InputEntry entry;
      entry.frame = frame;
      entry.command = LoadU64(memory, kHistory + 208 + 8 * index);
      entry.status_a = LoadU32(memory, kHistory + 144 + 4 * index);
      entry.status_b = LoadU32(memory, kHistory + 48 + 4 * index);
      const int player = int(LoadU32(memory, kHistory + 96 + 4 * index));
      if (player == 0 || player == 1) {
        if (entry.status_a != m.last_status_a || entry.status_b != m.last_status_b) {
          m.last_status_a = entry.status_a;
          m.last_status_b = entry.status_b;
          REXLOG_INFO("[OnlineInput] local status at frame {}: A={} B={} (player {})", frame,
                      entry.status_a, entry.status_b, player);
        }
        m.local_player = player;
        if (entry.command && m.local[uint32_t(frame) % m.local.size()].frame != frame) {
          ++m.window.local_pressed;
          ++m.total.local_pressed;
        }
        m.local[uint32_t(frame) % m.local.size()] = entry;
        m.newest_local = std::max(m.newest_local, frame);
        WriteRing(memory, manager, player, frame, entry.command, entry.status_a, entry.status_b);
      }
    }
  }

  // 2. The other PC's frames.
  std::vector<RemoteEntry> entries;
  std::vector<std::pair<int32_t, uint32_t>> hashes;
  bool has_ack;
  int32_t remote_ack, sender_frame;
  uint64_t packets;
  {
    std::lock_guard<std::mutex> inbox_lock(g_inbox.mutex);
    entries.swap(g_inbox.entries);
    hashes.swap(g_inbox.hashes);
    has_ack = g_inbox.has_ack;
    remote_ack = g_inbox.ack;
    sender_frame = g_inbox.sender_frame;
    packets = g_inbox.packets;
    g_inbox.packets = 0;
  }
  m.window.received += packets;
  m.total.received += packets;
  // A packet from another match (before / after a reset) is far off.
  const bool same_match = has_ack && std::abs(sender_frame - frame) <= 64;
  for (const RemoteEntry& e : entries) {
    if (m.local_player < 0 || e.player == m.local_player || e.frame < consume ||
        e.frame > frame + delay + 1) {
      ++m.window.stale;
      ++m.total.stale;
      continue;
    }
    if (e.status_a != m.last_remote_a || e.status_b != m.last_remote_b) {
      m.last_remote_a = e.status_a;
      m.last_remote_b = e.status_b;
      REXLOG_INFO("[OnlineInput] remote status at frame {}: A={} B={} (our frame {})", e.frame,
                  e.status_a, e.status_b, frame);
    }
    const int result =
        WriteRing(memory, manager, e.player, e.frame, e.command, e.status_a, e.status_b);
    if (result > 0) {
      ++m.window.injected_new;
      ++m.total.injected_new;
      if (e.command) {
        ++m.window.remote_pressed;
        ++m.total.remote_pressed;
      }
    } else {
      ++m.window.injected_known;
      ++m.total.injected_known;
    }
  }
  if (m.desync_check) {
    for (const auto& [hash_frame, hash] : hashes) {
      if (std::abs(hash_frame - frame) > 100) {
        continue;
      }
      auto& slot = m.remote_hashes[uint32_t(hash_frame) % m.remote_hashes.size()];
      if (slot.first == hash_frame && slot.second == hash) {
        continue;  // the copy of a frame already compared
      }
      slot = {hash_frame, hash};
      CompareHash(m, hash_frame);
    }
  }

  // 3. Send what the other PC hasn't confirmed (always at least the newest).
  if (m.local_player < 0 || m.newest_local < 0) {
    return;
  }
  const int remote_player = 1 - m.local_player;
  int32_t ack = consume - 1;
  for (int32_t g = std::max(consume, 0); g <= frame + delay + 1; ++g) {
    if (!RingHas(memory, manager, remote_player, g)) {
      break;
    }
    ack = g;
  }
  // The other PC's ack can't be above our newest frame in the same match (an
  // ack from before a reset is): then the whole window goes.
  int32_t from = m.newest_local - (kMaxWindow - 1);
  if (same_match && remote_ack <= m.newest_local) {
    from = std::max(from, std::min(remote_ack + 1, m.newest_local));
  }
  from = std::max(from, 0);
  while (from <= m.newest_local && m.local[uint32_t(from) % m.local.size()].frame != from) {
    ++from;
  }
  std::array<uint8_t, kInputHeader + kMaxWindow * kInputEntry> packet{};
  packet[0] = kInputVersion;
  packet[1] = uint8_t(m.local_player);
  packet[3] = 0;
  PutU32(&packet[4], uint32_t(ack));
  PutU32(&packet[8], uint32_t(from));
  PutU32(&packet[12], uint32_t(frame));
  if (m.desync_check && m.last_hashed >= 0) {
    const HashRecord& h = m.local_hashes[uint32_t(m.last_hashed) % m.local_hashes.size()];
    if (h.frame == m.last_hashed) {
      packet[3] |= kFlagHash;
      PutU32(&packet[16], uint32_t(h.frame));
      PutU32(&packet[20], h.hash);
    }
  }
  packet[24] = uint8_t(m.delay);
  int written = 0;
  for (int32_t g = from; g <= m.newest_local && written < kMaxWindow; ++g) {
    const InputEntry& e = m.local[uint32_t(g) % m.local.size()];
    if (e.frame != g) {
      break;  // keep the entries contiguous from `from`
    }
    uint8_t* p = &packet[kInputHeader + size_t(written) * kInputEntry];
    PutU64(p, e.command);
    PutU32(p + 8, e.status_a);
    PutU32(p + 12, e.status_b);
    ++written;
  }
  packet[2] = uint8_t(written);
  if (rex::net::online::SendGameSide(kSideInput, packet.data(),
                                     kInputHeader + size_t(written) * kInputEntry, 1)) {
    ++m.window.sent;
    ++m.total.sent;
    m.window.entries_sent += uint64_t(written);
    m.total.entries_sent += uint64_t(written);
    m.window.max_window = std::max(m.window.max_window, written);
    m.total.max_window = std::max(m.total.max_window, written);
  }
}

// sub_8217E860, mr r31,r3 right after the consume (r3 = 1 when both players'
// input for the frame was there): hash the fight's state at that frame, and
// let the effect timers go again after a hold.
void BurstLimitOnlineAfterConsume(PPCRegister& r3) {
  if (r3.u32 == 0) {
    return;
  }
  auto* memory = GuestMemory();
  if (!memory) {
    return;
  }
  std::lock_guard<std::mutex> lock(g_match_mutex);
  Match& m = g_match;
  ThawLocked(m, memory);
  m.hold_run = 0;
  if (m.online) {
    const uint32_t flag = LoadU32(memory, kInputManagerPointer - 4);
    if (flag != m.last_sync_flag) {
      m.last_sync_flag = flag;
      const uint32_t mgr = LoadU32(memory, kInputManagerPointer);
      REXLOG_INFO("[OnlineInput] sync event flag {} at tick frame {}", flag,
                  mgr ? LoadS32(memory, mgr + kTickFrame) : -1);
    }
  }
  if (!m.desync_check) {
    return;
  }
  const uint32_t manager = LoadU32(memory, kInputManagerPointer);
  if (!manager) {
    return;
  }
  const int32_t consumed = LoadS32(memory, manager + kTickFrame) - (m.delay ? m.delay : 3);
  if (consumed < 0 || consumed == m.last_hashed) {
    return;
  }
  HashRecord record;
  if (ComputeHash(memory, consumed, &record)) {
    if (m.last_hashed >= 0 &&
        m.local_hashes[uint32_t(m.last_hashed) % m.local_hashes.size()].hash != record.hash) {
      ++m.window.hash_changes;
      ++m.total.hash_changes;
    }
    m.local_hashes[uint32_t(consumed) % m.local_hashes.size()] = record;
    m.last_hashed = consumed;
    CompareHash(m, consumed);
  }
}

// The online driver sub_8217E100 sets the hold bit (lis r3,0x4000 ; bl
// sub_82176F68): with online_hold_freeze, the effect / HUD steps stop too for
// the first frames of a hold in the match (state 3).
void BurstLimitOnlineHold() {
  auto* memory = GuestMemory();
  if (!memory) {
    return;
  }
  std::lock_guard<std::mutex> lock(g_match_mutex);
  Match& m = g_match;
  if (!m.hold_freeze) {
    return;
  }
  const uint32_t manager = LoadU32(memory, kInputManagerPointer);
  if (!manager || LoadU32(memory, manager + kDriverState) != kDriverStateMatch) {
    return;
  }
  ++m.hold_run;
  const uint32_t battle = LoadU32(memory, kBattlePointer);
  if (m.hold_run > kHoldFreezeFrames || !Readable(memory, battle + kBattleStep, 4)) {
    ThawLocked(m, memory);
    return;
  }
  if (!m.frozen) {
    m.frozen = true;
    m.saved_step = rex::memory::load_and_swap<float>(
        memory->TranslateVirtual<uint8_t*>(battle + kBattleStep));
    m.saved_effect =
        rex::memory::load_and_swap<float>(memory->TranslateVirtual<uint8_t*>(kEffectStep));
    m.saved_effect_seconds =
        rex::memory::load_and_swap<float>(memory->TranslateVirtual<uint8_t*>(kEffectStepSeconds));
  }
  rex::memory::store_and_swap<float>(memory->TranslateVirtual<uint8_t*>(battle + kBattleStep),
                                     0.0f);
  rex::memory::store_and_swap<float>(memory->TranslateVirtual<uint8_t*>(kEffectStep), 0.0f);
  rex::memory::store_and_swap<float>(memory->TranslateVirtual<uint8_t*>(kEffectStepSeconds), 0.0f);
}
