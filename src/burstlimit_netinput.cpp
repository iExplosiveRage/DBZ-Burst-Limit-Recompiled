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
// - Changing the delay during a match (online_live_delay): either player asks
//   with Back + D-pad up / down or F5 / F6, and both PCs switch on the same
//   tick frame E. The request travels in the header of every input packet, E
//   = the asker's frame + the delay + kLiveLeadFrames. The other PC can't
//   reach tick E before it has the asker's input of frame E - D, and the
//   packets that carry those frames also carry the request, so it arrives
//   first whatever the latency (the fight waits instead). The tick frame keeps
//   counting; only which input frame a tick delivers changes (last_consumed):
//   up by k = k ticks repeat the last delivered words (the words are button
//   levels and the battle counts edges itself, sub_82170F20, so a repeat is a
//   held button, not a new press); down by k = one tick delivers k + 1 frames
//   merged: the newest word, plus the buttons that went down in the frames it
//   skips, plus their synchronized event. Both PCs compute this from the same
//   frames. Nobody asks = none of it runs.
//
// Research and addresses: C:/rex/_online_tests/netcode_research.md.

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <climits>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/memory.h>
#include <rex/memory/utils.h>
#include <rex/net/online.h>
#include <rex/net/session.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>
#include <rex/runtime.h>

REXCVAR_DEFINE_STRING(online_input_delay, "auto", "Patches",
                      "Online (lobby): input delay in frames, the host's value is used by both "
                      "players. Higher hides more ping (a frame is 16.7 ms: 4 frames = up to "
                      "~60 ms one way without stops). off = the game's own timing "
                      "(online_tick_sleep). auto = from the ping (the host measures it when the match starts; with "
                      "rollback: 2 in the fight, more only at very high pings, and the ping's delay outside the fight; "
                      "lockstep: the ping's delay, Back + D-pad up / down still change it). Default auto (was 4).")
    .allowed({"off", "auto", "2", "3", "4", "5", "6", "7", "8"})
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(online_live_delay, true, "Patches",
                    "Online (lobby): either player can change the input delay during a match "
                    "(Back + D-pad up / down, or F6 / F5), for both. Off on either PC = the delay "
                    "stays as it was picked for the match.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(online_live_delay_test_sync, false, "Debug",
                    "Test: this PC jumps the delay (2 <-> 8) right over every synchronized event, "
                    "as soon as its own input says so.");

REXCVAR_DEFINE_STRING(input_delay, "reduced", "Patches",
                      "Offline fights (Training, Versus, Z Chronicles...): original = the "
                      "game's own input delay of 3 frames (50 ms), as on the Xbox 360; reduced = "
                      "none, a press acts on the next game frame. Applies from the next fight. "
                      "Online always uses its own delay (online_input_delay).")
    .allowed({"original", "reduced"})
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(online_input_channel, true, "Patches",
                    "Online (lobby): send every frame's input right away over the connection's "
                    "side channel (off = only the game's own 3-frame packs, for comparisons).")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(online_desync_check, true, "Patches",
                    "Online (lobby): compare a hash of the fight's state with the other PC every "
                    "frame and log the first difference ([OnlineDesync]).")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_STRING(online_test_slow, "", "Debug", "Online test: <from tick>:<ticks>:<ms> - this PC sleeps that long every tick in that window (its frame rate drops)");
REXCVAR_DEFINE_INT32(online_test_full_ki, 0, "Debug", "Online test (both PCs): both fighters' ki is set to the maximum every N input frames of the fight, 0 off");
REXCVAR_DEFINE_INT32(online_force_desync_frame, 0, "Debug", "Online test: the host moves its fighter 0.5 along x at this input frame (forces a desync), 0 off");
REXCVAR_DEFINE_BOOL(online_desync_end, true, "Patches", "Online rollback: a confirmed desync ends the match on both PCs (notice, inputs stop, the game leaves) instead of going on in lockstep");
REXCVAR_DEFINE_BOOL(online_rollback_consume_fix, true, "Patches", "Online rollback: the local word consumed on a live tick is always the command recorded and sent for that frame");
REXCVAR_DEFINE_BOOL(online_rollback_cine_delay, true, "Patches", "Online rollback (phase delay): a cinematic in the fight longer than half a second runs with the ping's delay (it runs without rollback)");
REXCVAR_DEFINE_INT32(online_rollback_ready_switch, 130, "Patches", "Online rollback (phase delay): ticks into READY? (180 long) when the host asks for the fight delay (60-150; was 90)");
// v0.8.1 R25 (build 151): a rollback match changes its delay by itself 2-4 times per round (phase delay, cinematics);
// the players can't act on those notices (and "Opponent: input delay a -> b" read like the other player's choice)
REXCVAR_DEFINE_BOOL(online_rollback_delay_notices, false, "Online",
                    "Online rollback: show a notice for every automatic input delay change (off: only Classic matches show them)");
REXCVAR_DEFINE_BOOL(online_rollback_phase_delay, true, "Patches", "Online rollback: outside the fight (intros, READY?, KO / time-up scenes, round change, results) the match runs with an input delay that covers the ping, so those lockstep parts don't wait; back to the fight delay before FIGHT! (the host decides, both switch on the same tick)");
// v0.8.1 R37 / R36a (build 136). The game's NETWORK THREAD (sub_8229F548 -> sub_8229DC60 -> sub_8229D8E8, its
// packet sender) copies the last three local history entries into the local ring whenever it sends - at any time,
// also between the tick hook's ForceRing and the consume a few instructions later. A resim tick packs its local
// frame again with the pads of that moment, so the history can hold another command than the one recorded and
// sent when the frame ran live. MEASURED r135_long (RTT 235): the guest's resim consumed 0 for its own frames
// 4088 / 2020 while 4010.. / 2008.. had been sent (the host used the sent ones; the hashes stayed equal by luck),
// and four times the frame-end check took the ring's word for the truth, changed an entry that was already sent
// and sent it again ("recorded X, ring Y - corrected") - the other PC rewound 13 frames each time, and one of
// them flipped 2008.. -> 0 -> 2008.. there because an older packet arrived after the newer one.
// Now the entry recorded when the frame was first packed is the only truth for the local command: it is what
// both PCs consume (the consume's output word is set to it in resimulated ticks too) and it never changes.
REXCVAR_DEFINE_BOOL(online_rollback_consume_fix_resim, true, "Patches",
                    "Online rollback: the local word consumed in a resimulated tick is also the command recorded and sent "
                    "for that frame (off = builds up to 135: live ticks only)");
REXCVAR_DEFINE_BOOL(online_rollback_frame_end_fix, false, "Patches",
                    "Online rollback: at the end of a live frame a local entry that differs from its ring slot takes the "
                    "ring's command and is sent again (on = builds up to 135; off: the sent command stays and the ring gets it)");
// build 137: the same for the REMOTE player's word. The game's network thread also runs the game's own receiver
// (sub_82298498 -> sub_8217E630): the other PC's game packets carry its last three history entries (after one of
// its resims: pads of that moment) and are written into our ring at any time. (1) A tick that holds (no known
// entry, no prediction allowed) consumed such a slot anyway, with no record to compare it with later ("consumed
// without any check", on15); (2) a write between the hook and the consume replaces the word this PC chose.
// Now a held tick takes the frame out of the slot, and after the consume the remote word is set to the entry
// that was chosen (the confirmed one, else the prediction); a consume nobody chose is recorded as a prediction,
// so the real entry is compared with it when it arrives.
REXCVAR_DEFINE_BOOL(online_rollback_consume_fix_remote, true, "Patches",
                    "Online rollback: the remote word consumed is the confirmed entry or this PC's prediction for that "
                    "frame, and a held tick never consumes a ring slot written by the game's own packets (off = builds up to 136)");
REXCVAR_DEFINE_BOOL(online_rollback_seq, true, "Patches",
                    "Online rollback: input packets carry a sequence number and an entry from an older packet never "
                    "replaces what a newer packet said about the same frame (off = builds up to 135)");
REXCVAR_DEFINE_BOOL(online_record_first_pack, true, "Patches", "Online rollback: a local frame first packed in a resim (the live run held there) is recorded and sent as the game consumes it; a frame packed again keeps its first (sent) command");
REXCVAR_DEFINE_BOOL(online_desync_block, true, "Patches", "Online rollback desync end: also drop the game's own datagrams to the peer so its session sees a lost connection and leaves (cleared when the session is deleted)");
REXCVAR_DEFINE_STRING(online_stats_overlay, "ping", "Online",
                      "Online fights: a small connection line in the bottom left corner - off, ping (netcode, ping, "
                      "input delay) or full (also how many frames the fight runs ahead of the other player's inputs)");
REXCVAR_DEFINE_BOOL(online_stats_overlay_test, false, "Debug", "Testing: the connection line is drawn all the time (offline too)");
REXCVAR_DEFINE_BOOL(online_time_sync, true, "Patches", "Online rollback: the PC running ahead waits a tick now and then (time sync)");
REXCVAR_DEFINE_BOOL(online_hash_log, false, "Debug", "Online: log every consumed frame's desync hash (diagnostics)");
REXCVAR_DEFINE_BOOL(online_desync_dump, true, "Debug", "Online rollback diagnostics (on by default since v0.8.1: 28 KB copied per consumed frame): keep the battle / player / fighter objects of the last 64 consumed frames and write them to <log_file>.desync.bin when a desync is found or reported");
REXCVAR_DEFINE_BOOL(online_hold_freeze, false, "Patches",
                    "Online: while the fight waits for the other player's input (short waits "
                    "only), also stop the effect and HUD timers.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

// burstlimit_forms.cpp: the other PC's start forms (side-channel kind 17).
bool BurstLimitFormsOnSide(uint8_t kind, const uint8_t* body, size_t size);
// burstlimit_online_rules.cpp: the Player Match room rules (side-channel kind 18).
bool BurstLimitOnlineRulesOnSide(uint8_t kind, const uint8_t* body, size_t size);
// burstlimit_online.cpp: the on-screen line under the lobby notices.
void BurstLimitOnlineDelayNotice(const std::string& text, double seconds);
// burstlimit_slots.cpp: the online pick exchange (kind 19).
bool BurstLimitSlotsOnSide(uint8_t kind, const uint8_t* body, size_t size);
// burstlimit_online.cpp: a line in the online notice area.
void BurstLimitOnlineGameNotice(const std::string& text, double seconds);
void BurstLimitOnlineAlert(const char* kind, const std::string& text);  // burstlimit_online.cpp (v0.8.1 R8)

void BurstLimitRb2OnConsume(rex::memory::Memory* memory, int32_t frame);
// Rollback engine (burstlimit_rb2.cpp, branch rollback2).
bool BurstLimitRb2Active();
void BurstLimitRb2NewMatch();  // burstlimit_rb2.cpp (R17)
bool BurstLimitRb2InResim();
std::string BurstLimitDiagPath(const char* exe_name, const char* suffix);  // burstlimit_rb2.cpp (review L5)
void BurstLimitRb2NoteHostStore(uint32_t a, uint32_t n);
int64_t BurstLimitRb2CurrentFrame();
void BurstLimitRb2RequestRewind(int64_t engine_frame);
bool BurstLimitRb2PredictOk();

// The netcode of a lobby match is decided when the match starts (BurstLimitOnlineResetFrame), the same on both PCs:
// in a Player Match room the host's Room Rules row "Netcode" (sent to the guest with the other room rules, before
// any battle); in matches without room rules (Quick Match, Ranked) rollback. online_netcode forces one for tests -
// it is a synced setting (the guest takes the host's value), so the two PCs can't disagree.
REXCVAR_DEFINE_STRING(online_netcode, "auto", "Debug",
                      "Online (lobby) netcode: auto = the Player Match room rule (rollback in matches without room "
                      "rules); rollback / classic force it for tests (the host's value is used by both players)")
    .allowed({"auto", "rollback", "classic"})
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
// This match's netcode (true = rollback, false = classic lockstep), decided at the match reset.
std::atomic<bool> g_match_rollback{true};
bool RollbackMatch() { return g_match_rollback.load(std::memory_order_relaxed); }
// burstlimit_online_rules.cpp: the agreed Player Match room rules' netcode (1 rollback, 0 classic); false = none agreed.
bool BurstLimitOnlineRulesNetcode(int* netcode);
std::atomic<bool> g_rb2_match{false};  // a lobby match with the input channel (set by the driver hook)

namespace {

// --- Guest addresses -----------------------------------------------------------

// Match input manager M (pointer). Per-player blocks of 784 bytes at M and
// M + 784: a ring of 32 entries x 24 bytes (+0 u64 command, +8 status A,
// +12 status B, +16 frame), then +768 read index, +772 write index, +776
// highest frame seen.
constexpr uint32_t kInputManagerPointer = 0x841B5EA4;
constexpr uint32_t kSyncEventFlag = 0x841B5EA0;  // set by the consume for its frame
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
constexpr uint32_t kConsumedWords = 1616;   // the consume's output, two u64 by player
constexpr uint32_t kDriverState = 1632;     // 3 = online match
constexpr uint32_t kDriverStateMatch = 3;
constexpr uint32_t kDriverStateOffline = 0;  // sub_8217E808: local input, consumed 3 frames late

// The game's local input history (12 slots by frame % 12): +0 frame,
// +48 status B, +96 player, +144 status A, +208 u64 command.
constexpr uint32_t kHistory = 0x842545E8;
constexpr int32_t kHashSettleFrames = 24;  // rollback: hashes compared this many input frames back (final by then)
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

// The side-channel frame: [u8 version][u8 player][u8 count][u8 flags: bit 0 hash, bit 1 desync, bits 2-7 rollback
// time sync = average prediction depth in quarter frames]
// [s32 ack][s32 first][s32 sender frame][s32 hash frame][u32 hash][u8 delay]
// [u8 live: bit 0 = may change the delay, high nibble = the delay last asked
// for, 0 = none][u16 tick frame it starts at, low bits]
// then count x [u64 command][u32 status A][u32 status B], little-endian.
constexpr uint8_t kSideInput = rex::net::online::kGameSideKindFirst;  // 16
constexpr uint8_t kInputVersion = 1;
constexpr size_t kInputHeader = 28;
constexpr size_t kInputEntry = 16;
constexpr uint8_t kFlagHash = 1;
constexpr uint8_t kFlagDesync = 2;  // rollback: this PC found a desync (the match ends on both)
constexpr int kMaxWindow = 16;

// Hold freeze: only the first frames of a hold (a long wait shows the game's
// own "Sending data..." notice, which must still animate).
constexpr uint32_t kHoldFreezeFrames = 30;

// Changing the delay during a match.
// How far past the frame the other PC must have anyway (asker's frame + delay)
// a change starts: a margin for frames that get there in the game's own packs
// while every side-channel packet around them is lost. 15 frames = 250 ms.
constexpr int kLiveLeadFrames = 15;
constexpr int kLiveMinDelay = 2;
constexpr int kLiveMaxDelay = 8;
// Quick presses add up to one change.
constexpr int64_t kLivePressSettleMs = 150;
// Pad buttons (XInput bits), pads as in burstlimit_kicharge.cpp.
constexpr uint32_t kPads = 0x8413F5F0;
constexpr uint32_t kPadSize = 212;
constexpr uint32_t kPadUp = 0x1, kPadDown = 0x2, kPadLeft = 0x4, kPadRight = 0x8, kPadBack = 0x20;
// The command word's direction bits (sub_82170480 / sub_821706B0): the D-pad
// (58-61), the stick moved (54-55) and the directions made from both (32-37,
// 46-49). A merge takes those from the newest frame only.
constexpr uint64_t kCommandDirections =
    (0xFull << 58) | (0x3ull << 54) | (0xFull << 46) | (0x3Full << 32);
// What the D-pad part of Back + D-pad must not do in the fight.
constexpr uint64_t kCommandDpadDirections = (0xFull << 58) | (0xFull << 46) | (0x3Full << 32);
constexpr int32_t kNoConsumeOverride = INT32_MIN;

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
// Writes into guest game state are noted for the rollback's write tracking (a rewind puts them back like the
// game's own stores; unnoted, a rewind could keep them live).
void StoreU32(rex::memory::Memory* memory, uint32_t address, uint32_t value) {
  rex::memory::store_and_swap<uint32_t>(memory->TranslateVirtual<uint8_t*>(address), value);
  BurstLimitRb2NoteHostStore(address, 4);
}
void StoreU64(rex::memory::Memory* memory, uint32_t address, uint64_t value) {
  rex::memory::store_and_swap<uint64_t>(memory->TranslateVirtual<uint8_t*>(address), value);
  BurstLimitRb2NoteHostStore(address, 8);
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
  uint32_t seq = 0;  // the sender's packet number (0: a packet without the trailer = a build before 136)
};

struct Inbox {
  std::mutex mutex;
  std::vector<RemoteEntry> entries;
  std::vector<std::pair<int32_t, uint32_t>> hashes;  // frame, hash
  bool has_ack = false;
  int32_t ack = 0;
  int32_t sender_frame = 0;
  uint8_t remote_pred4 = 0;  // the other PC's average prediction depth in quarter frames (time sync)
  bool remote_desync = false;  // the other PC found a desync
  uint8_t sender_delay = 0;
  bool peer_live = false;
  uint8_t peer_target = 0;
  int32_t peer_effective = 0;
  uint64_t packets = 0;
  uint64_t bad = 0;
} g_inbox;

void OnGameSide(uint8_t kind, const uint8_t* body, size_t size) {
  if (BurstLimitFormsOnSide(kind, body, size) || BurstLimitOnlineRulesOnSide(kind, body, size) ||
      BurstLimitSlotsOnSide(kind, body, size)) {
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
  g_inbox.remote_pred4 = uint8_t((flags >> 2) & 63);
  if (flags & kFlagDesync) g_inbox.remote_desync = true;
  g_inbox.peer_live = (body[25] & 1) != 0;
  g_inbox.peer_target = body[25] >> 4;
  if (g_inbox.peer_target) {
    // The low 16 bits, nearest to the sender's frame.
    const uint16_t low = uint16_t(body[26] | (body[27] << 8));
    g_inbox.peer_effective = sender_frame + int16_t(uint16_t(low - uint16_t(sender_frame)));
  }
  // build 136: four more bytes after the entries = the sender's packet number (older builds ignore them: their
  // size check is "at least header + entries")
  const size_t entries_end = kInputHeader + size_t(count) * kInputEntry;
  const uint32_t seq = size >= entries_end + 4 ? GetU32(body + entries_end) : 0;
  if (g_inbox.entries.size() < 1024) {
    for (uint8_t i = 0; i < count; ++i) {
      const uint8_t* e = body + kInputHeader + size_t(i) * kInputEntry;
      g_inbox.entries.push_back(
          {first + i, player, GetU64(e), GetU32(e + 8), GetU32(e + 12), seq});
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
  int start_delay = 0;   // the delay at the reset: the key of the desync hashes
  int start_frame = 3;
  bool desync_check = false;
  bool hold_freeze = false;

  int local_player = -1;
  int32_t newest_local = -1;
  int32_t resend_from = -1;  // a local entry changed by a resim: sent again from this frame
  int32_t rb_ack_floor = 0;
  uint32_t last_phase = 0, last_battle = 0;  // frames below are never acked as missing (before rollback started)
  // Time sync (idea: GGPO's frame-advantage rule, pond3r/ggpo MIT; own code): each PC averages how many frames
  // ahead of the newest confirmed remote input it consumes; both send theirs. The PC that predicts clearly more
  // than the other is running ahead and waits one tick (no prediction there) - at most once per 20 ticks.
  float pred_avg = 0.0f, remote_pred_avg = 0.0f;
  int32_t ts_tick = 0;
  int32_t ts_last_hold = -1000;
  uint64_t ts_holds = 0;
  std::array<InputEntry, 64> local{};
  std::array<HashRecord, 128> local_hashes{};
  std::array<std::pair<int32_t, uint32_t>, 128> remote_hashes{};
  bool desync_reported = false;
  bool desync_notice = false;  // rollback: the "match ends" notice shown
  Match() {
    for (auto& h : remote_hashes) {
      h.first = -1;
    }
  }
  int32_t last_hashed = -1;

  // Rollback (the match's netcode): confirmed remote inputs and the predictions consumed for missing ones.
  struct Prediction {
    int32_t frame = -1;
    uint64_t command = 0;
    uint32_t status_a = 0, status_b = 0;
    int64_t engine_frame = 0;
    bool settled = false;
  };
  std::array<InputEntry, 128> remote_known{};
  std::array<uint32_t, 128> remote_seq{};  // the packet number that last set each remote_known entry (0: unknown)
  uint64_t rb_stale_entries = 0, rb_consume_fixes_resim = 0, rb_frame_end_kept = 0, rb_consume_fixes_remote = 0,
           rb_unchosen_consumes = 0, rb_foreign_slots = 0;
  std::array<Prediction, 128> predicted{};
  std::array<std::pair<int32_t, int64_t>, 128> consumed_at{};  // remote input frame -> engine frame that consumed it
  InputEntry newest_remote{};
  uint64_t rb_gap_holds = 0, rb_old_resends = 0, rb_hole_repairs = 0, rb_local_used_mismatch = 0, rb_first_packs = 0, rb_repacks_kept = 0, rb_after_desync = 0, rb_local_ring_fix = 0, rb_local_late = 0, rb_local_corrections = 0, rb_remote_corrections = 0, rb_predicted = 0, rb_mispredicted = 0, rb_too_old = 0, rb_not_ready = 0, rb_no_base = 0, rb_status_diff = 0, rb_status_sample = 0, rb_consume_fixes = 0;
  // Changing the delay during a match.
  struct DelayChange {
    int32_t effective;
    int target;
    int player;
  };
  bool live_local = false;     // this PC allows it (latched at the reset)
  bool live_peer = false;      // the other PC's packets say it does
  bool live_used = false;      // the delay has changed in this match
  bool live_notified = false;  // logged that it isn't available
  std::vector<DelayChange> pending;
  int own_target = 0;  // sent in every packet from when it's first asked
  int32_t own_effective = 0;
  int32_t peer_last_effective = INT32_MIN;
  int desired = 0;  // where this player's presses lead
  bool press_active = false;
  int64_t last_press_ms = 0;
  uint64_t delay_changes = 0, delay_late = 0, filler_ticks = 0, merged_ticks = 0,
           merged_frames = 0;
  // What the coming consume delivers.
  enum class Tick { kNormal, kFiller, kMerge } tick = Tick::kNormal;
  int32_t last_consumed = -1;  // newest input frame delivered
  uint64_t delivered[2] = {};  // the words delivered last
  uint64_t merged[2] = {};
  uint32_t merged_flag = 0;
  int32_t merge_to = 0;
  // Checks for the match line: a digest of everything delivered (must be the
  // same on both PCs) and the buttons that went down, as sampled here and as
  // delivered for each player.
  uint32_t delivered_digest = 2166136261u;
  uint64_t delivered_presses[2] = {};
  uint64_t local_presses = 0;
  uint64_t last_local_word = 0;

  // Diagnostics: the statuses the game puts in its input entries (fight
  // start / pause events) and the synchronized-event flag.
  uint32_t last_status_a = 0xFFFFFFFF, last_status_b = 0xFFFFFFFF;
  struct Dump {
    int32_t frame = -1;
    int64_t engine = -1;
    bool resim = false;
    std::vector<uint8_t> bytes;
  };
  std::array<Dump, 64> dumps{};  // online_desync_dump: by consumed frame
  bool dumps_written = false;
  int32_t diag_hold_tick = -1;  // diagnostics: the tick a hold sits at, and for how many hold calls
  int diag_hold_n = 0;
  // online_rollback_phase_delay (rollback matches): the fight's delay and the one used outside the fight
  bool auto_phase = false;
  bool auto_delay = false;     // online_input_delay = auto (the host picks the delay from the ping)
  bool rollback_match = false;
  bool ap_lockstep_done = false;
  bool window_flip = false;
  int32_t remote_contig = -1;  // rollback: newest remote frame with every one before it known
  int fight_delay = 0;
  uint32_t ap_phase = 0xFFFFFFFF;
  int32_t ap_since = 0, ap_last_fight = -100000;
  int32_t ap_cine_since = -1;  // a cinematic in the fight (supers / Ultimates / transformations) since this tick, -1 none
  int engine_delay = 0;        // the delay the current fight started with: the rollback engine runs only at it
  bool cine_up = false;        // the host raised the delay for a cinematic: back to the fight delay after it
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
int g_sync_guard_waits = 0, g_sync_guard_skips = 0, g_sync_guard_timeouts = 0;  // the sync-event guard (SyncGuard)
Match g_match;
// Copies the hooks read without the lock.
std::atomic<int> g_delay{0};
// v0.8.1 R25 (first part): what the corner line of an online fight shows; written by the tick hook, read by the UI
std::atomic<int64_t> g_ov_tick_ms{0};
std::atomic<int> g_ov_delay{0}, g_ov_ahead10{0};
std::atomic<bool> g_ov_rollback{false};
}  // namespace
// The line and its colour level (0 white, 1 yellow, 2 red); false = nothing to draw. UI thread; no lock taken.
bool BurstLimitOnlineOverlayLine(std::string* text, int* level) {
  const std::string mode = REXCVAR_GET(online_stats_overlay);
  const bool test = REXCVAR_GET(online_stats_overlay_test);
  if ((mode == "off" || mode.empty()) && !test) return false;
  const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
  const int64_t tick = g_ov_tick_ms.load(std::memory_order_relaxed);
  if (!test && (!tick || now - tick > 1500)) return false;  // no online fight is ticking
  const uint32_t rtt = test && !tick ? 61 : rex::net::online::PeerRttMs();
  const bool rollback = test && !tick ? true : g_ov_rollback.load(std::memory_order_relaxed);
  const int delay = test && !tick ? 2 : g_ov_delay.load(std::memory_order_relaxed);
  *text = fmt::format("{}  ping {} ms  delay {}f", rollback ? "Rollback" : "Classic", rtt, delay);
  if (rollback && (mode == "full" || test)) *text += fmt::format("  ahead {:.1f}f", double(g_ov_ahead10.load(std::memory_order_relaxed)) / 10.0);
  *level = rtt > 250 ? 2 : rtt > 150 ? 1 : 0;
  return true;
}
namespace {
std::atomic<int> g_start_frame{3};
std::atomic<int> g_start_delay{0};
// The frame the coming consume takes when it isn't tick frame - delay.
std::atomic<int32_t> g_consume_override{kNoConsumeOverride};
// Changing the delay: the local presses (any thread) and whether the pad combo
// is live (the input task's mask).
std::atomic<int> g_live_steps{0};
std::atomic<int> g_live_absolute{0};
std::atomic<bool> g_live_active{false};

void ResetMatchLocked(Match& m) {
  Match fresh;
  fresh.window_start_ms = NowMs();
  m = fresh;
  std::lock_guard<std::mutex> lock(g_inbox.mutex);
  g_inbox.entries.clear();
  g_inbox.hashes.clear();
  g_inbox.has_ack = false;
  g_inbox.remote_desync = false;
  g_inbox.peer_live = false;
  g_inbox.peer_target = 0;
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

// online_desync_dump: the objects a consumed frame leaves (overwritten by every later run of the frame, so the
// copy kept is the final one once the frame is older than any rollback)
void CaptureDump(Match& m, rex::memory::Memory* memory, int32_t frame) {
  Match::Dump& d = m.dumps[uint32_t(frame) % m.dumps.size()];
  d.frame = frame;
  d.engine = BurstLimitRb2CurrentFrame();
  d.resim = BurstLimitRb2InResim();
  d.bytes.clear();
  auto add = [&](uint32_t address, uint32_t size) {
    const uint8_t header[8] = {uint8_t(address >> 24), uint8_t(address >> 16), uint8_t(address >> 8), uint8_t(address),
                               uint8_t(size >> 24), uint8_t(size >> 16), uint8_t(size >> 8), uint8_t(size)};
    d.bytes.insert(d.bytes.end(), header, header + 8);
    if (address && Readable(memory, address, size)) {
      const uint8_t* p = memory->TranslateVirtual<const uint8_t*>(address);
      d.bytes.insert(d.bytes.end(), p, p + size);
    } else {
      d.bytes.insert(d.bytes.end(), size, 0);
    }
  };
  add(0x842F9334u, 4);  // the fight RNG
  add(LoadU32(memory, kBattlePointer), 0x2000);
  for (int i = 0; i < 2; ++i) {
    const uint32_t player = LoadU32(memory, kPlayerTable + 4 * i);
    add(player, 0x800);
    add(Readable(memory, player + kPlayerFighter, 4) ? LoadU32(memory, player + kPlayerFighter) : 0, 0x2000);
  }
}
void WriteDumps(Match& m, const char* why) {
  if (m.dumps_written || !REXCVAR_GET(online_desync_dump)) return;
  m.dumps_written = true;
  const std::string path = BurstLimitDiagPath("online", ".desync.bin");  // review L5: next to the log / the exe
  FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) return;
  int n = 0;
  for (const Match::Dump& d : m.dumps) {
    if (d.frame < 0) continue;
    const int32_t hdr[4] = {d.frame, int32_t(d.engine), d.resim ? 1 : 0, int32_t(d.bytes.size())};
    std::fwrite(hdr, sizeof(hdr), 1, f);
    std::fwrite(d.bytes.data(), 1, d.bytes.size(), f);
    ++n;
  }
  std::fclose(f);
  REXLOG_WARN("[OnlineDesync] {} frames of state written to {} ({})", n, path, why);
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
    BurstLimitOnlineAlert("desync", "the two games no longer match (found here)");
    WriteDumps(m, "found here");
    if (RollbackMatch())
      REXLOG_WARN("[OnlineDesync] rollback stops for the rest of this match (lockstep from here)");
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

uint32_t NewPresses(uint64_t word, uint64_t previous) {
  return uint32_t(__builtin_popcountll(word & ~previous & ~kCommandDirections));
}

int FinalDelay(const Match& m) {
  // In the order they start, the later player last on the same frame.
  const Match::DelayChange* last = nullptr;
  for (const Match::DelayChange& c : m.pending) {
    if (!last || c.effective > last->effective ||
        (c.effective == last->effective && c.player > last->player)) {
      last = &c;
    }
  }
  return last ? last->target : m.delay;
}

std::string DelayLine(int delay) {
  return "Input delay: " + std::to_string(delay) + " frames   (ping " +
         std::to_string(rex::net::online::PeerRttMs()) + " ms)";
}

// Every driver tick, before this tick's consume: the local presses, the other
// PC's request, starting the changes that are due, and what the consume is to
// deliver. `peer_*` come from the last packets.
void LiveDelayTick(Match& m, rex::memory::Memory* memory, uint32_t manager, int32_t frame,
                   bool same_match, bool peer_live, int peer_target, int32_t peer_effective) {
  if (!m.live_local) {
    return;
  }
  if (same_match) {
    m.live_peer = peer_live;
  }
  g_live_active.store(m.live_peer, std::memory_order_relaxed);
  int steps = g_live_steps.exchange(0, std::memory_order_relaxed);
  int absolute = g_live_absolute.exchange(0, std::memory_order_relaxed);
  if (m.rollback_match && (steps || absolute)) {  // rollback matches: no delay changes by hand (R25: say so)
    BurstLimitOnlineDelayNotice("Input delay is automatic with rollback (now " + std::to_string(m.delay) + " frames, ping " +
                                    std::to_string(rex::net::online::PeerRttMs()) + " ms)",
                                3.0);
    steps = absolute = 0;
  }
  const bool delay_notices = !m.rollback_match || REXCVAR_GET(online_rollback_delay_notices);

  // The other PC's request (each one is repeated in all its packets).
  const int remote_player = m.local_player >= 0 ? 1 - m.local_player : -1;
  if (same_match && peer_target >= kLiveMinDelay && peer_target <= kLiveMaxDelay &&
      peer_effective > m.peer_last_effective && remote_player >= 0) {
    m.peer_last_effective = peer_effective;
    m.pending.push_back({peer_effective, peer_target, remote_player});
    REXLOG_WARN("[OnlineDelay] other player asks: delay {} from tick frame {} (we are at {}, "
                "delay {}, rtt {} ms)",
                peer_target, peer_effective, frame, m.delay, rex::net::online::PeerRttMs());
    if (delay_notices)
      BurstLimitOnlineDelayNotice("Opponent: input delay " + std::to_string(m.delay) + " -> " +
                                      std::to_string(peer_target),
                                  3.0);
  }

  // The local presses.
  if (steps || absolute) {
    if (!m.live_peer) {
      if (m.local_player >= 0 && !m.live_notified) {
        m.live_notified = true;
        REXLOG_WARN("[OnlineDelay] asked, but the other PC doesn't allow live changes");
      }
      BurstLimitOnlineDelayNotice("Input delay can't change now (the other player has it off)",
                                  3.0);
    } else {
      if (!m.press_active) {
        m.desired = FinalDelay(m);
      }
      const int wanted =
          std::clamp(absolute ? absolute : m.desired + steps, kLiveMinDelay, kLiveMaxDelay);
      m.desired = wanted;
      m.press_active = true;
      m.last_press_ms = NowMs();
      std::string line = "Input delay " + std::to_string(m.delay) + " -> " + std::to_string(wanted);
      if (wanted == kLiveMinDelay && steps < 0) {
        line += " (lowest)";
      } else if (wanted == kLiveMaxDelay && steps > 0) {
        line += " (highest)";
      }
      BurstLimitOnlineDelayNotice(line, 3.0);
    }
  }

  // Rollback matches (online_rollback_phase_delay): the host asks for the ping delay outside the fight and for the
  // fight delay from 90 ticks into READY? (done before FIGHT!: kLiveLeadFrames + the delay < 90), never while the
  // fight runs or within 60 ticks after it (the rollback engine maps frames with a fixed delay and drains 30).
  if ((m.auto_phase || m.auto_delay) && m.live_peer && m.local_player == 0) {
    const uint32_t phase = Readable(memory, 0x841B5228, 4) ? LoadU32(memory, 0x841B5228) : 0;
    if (phase != m.ap_phase) {
      m.ap_phase = phase;
      m.ap_since = frame;
    }
    if (phase == 2) m.ap_last_fight = frame;
    // in-fight cinematics run without rollback (lockstep): a long one gets the ping's delay too (r96_s120: a 17 s
    // cinematic at 120 ms with delay 2 held 66 % of its ticks), asked once the engine has paused for it; afterwards
    // the fight delay again, and the engine resumes only when that has landed (BurstLimitOnlineEngineDelayOk)
    const uint32_t bt = LoadU32(memory, kBattlePointer);
    const bool cine = phase == 2 && bt && Readable(memory, bt + 572, 4) && (LoadU32(memory, bt + 572) & 2) != 0;
    if (cine) { if (m.ap_cine_since < 0) m.ap_cine_since = frame; } else { m.ap_cine_since = -1; }
    const bool long_cine = cine && frame - m.ap_cine_since >= 30 && REXCVAR_GET(online_rollback_cine_delay);
    const bool engine_paused = !BurstLimitRb2Active();
    const uint32_t rtt = rex::net::online::PeerRttMs();
    // frames the other PC's input needs to arrive (one way + 12 ms of jitter margin)
    const int latency_frames = int((rtt / 2 + 12) / 16.667) + 1;
    // "auto": the fight delay keeps a rollback within ~7 frames (2 up to ~100 ms one way)
    const int fight = !m.rollback_match ? std::clamp(latency_frames, kLiveMinDelay, kLiveMaxDelay)  // lockstep: all of it
                      : m.auto_delay ? std::clamp(latency_frames - 6, 2, kLiveMaxDelay) : m.fight_delay;
    const int phase_delay = m.auto_phase ? std::clamp(latency_frames, fight, kLiveMaxDelay) : fight;
    int want = phase_delay;
    // READY? lasts 180 ticks; the fight delay is asked for at READY? + online_rollback_ready_switch (was 90: the rest of
    // READY? then ran in lockstep with the fight delay, ~30 % of its ticks held at 75 ms); the change lands
    // kLiveLeadFrames + the delay (<= 23) ticks later, still before FIGHT!
    const int ready_switch = std::clamp(REXCVAR_GET(online_rollback_ready_switch), 60, 150);
    if (m.auto_phase && (phase == 2 || (phase == 1 && frame - m.ap_since >= ready_switch) || frame - m.ap_last_fight < 60)) want = fight;
    if (m.auto_phase && m.rollback_match && long_cine && engine_paused) want = phase_delay;
    bool own_pending = false;
    for (const Match::DelayChange& c : m.pending) own_pending = own_pending || c.player == m.local_player;
    // hysteresis outside the fight: the ping's delay moves only by 2 or more (rb_spike: jitter flipped it 6 / 7 / 6...)
    const int current = FinalDelay(m);
    const bool worth = want == fight || current == fight || std::abs(want - current) >= 2;
    // lockstep "auto" asks ONCE per match (then the players' Back + D-pad / F5 / F6 changes stand - ls_live: an "up"
    // in the intro was put back to the ping's delay 23 ticks later); rollback asks per phase
    if (!cine && FinalDelay(m) == fight) m.cine_up = false;
    const bool fight_cine_ask = m.auto_phase && phase == 2 && engine_paused && (long_cine || (!cine && m.cine_up && FinalDelay(m) != fight));
    const bool may_ask = m.rollback_match ? ((phase != 2 && frame - m.ap_last_fight >= 60) || fight_cine_ask) : !m.ap_lockstep_done;
    if (may_ask && !m.rollback_match && rtt > 0 && want == current) m.ap_lockstep_done = true;  // nothing to change: decided
    if (may_ask && !own_pending &&
        want != current && worth && rtt > 0) {
      m.ap_lockstep_done = !m.rollback_match;  // lockstep "auto": picked once per match
      if (m.rollback_match && phase == 2 && long_cine) m.cine_up = true;
      m.own_target = want;
      m.own_effective = frame + m.delay + kLiveLeadFrames;
      m.pending.push_back({m.own_effective, m.own_target, m.local_player});
      REXLOG_WARN("[OnlineDelay] phase {}: delay {} from tick frame {} (we are at {}, delay {}, rtt {} ms)", phase,
                  m.own_target, m.own_effective, frame, m.delay, rtt);
    }
  }

  // Our own request: one at a time, once the presses have settled.
  if (m.press_active && m.live_peer && m.local_player >= 0 &&
      NowMs() - m.last_press_ms >= kLivePressSettleMs) {
    bool own_pending = false;
    for (const Match::DelayChange& c : m.pending) {
      own_pending = own_pending || c.player == m.local_player;
    }
    if (!own_pending) {
      m.press_active = false;
      if (m.desired != FinalDelay(m)) {
        m.own_target = m.desired;
        m.own_effective = frame + m.delay + kLiveLeadFrames;
        m.pending.push_back({m.own_effective, m.own_target, m.local_player});
        REXLOG_WARN("[OnlineDelay] asking: delay {} from tick frame {} (we are at {}, delay {})",
                    m.own_target, m.own_effective, frame, m.delay);
      }
    }
  }

  // Start what's due: both PCs go through the same list in the same order.
  while (!m.pending.empty()) {
    auto next = m.pending.end();
    for (auto it = m.pending.begin(); it != m.pending.end(); ++it) {
      if (it->effective <= frame &&
          (next == m.pending.end() || it->effective < next->effective ||
           (it->effective == next->effective && it->player < next->player))) {
        next = it;
      }
    }
    if (next == m.pending.end()) {
      break;
    }
    const Match::DelayChange change = *next;
    m.pending.erase(next);
    if (change.effective < frame) {
      // Only if the request lost a race it can't lose (see kLiveLeadFrames):
      // the desync check will say what became of it.
      ++m.delay_late;
      REXLOG_WARN("[OnlineDelay] LATE request: tick frame {} but we are at {}", change.effective,
                  frame);
    }
    REXLOG_WARN("[OnlineDelay] change at tick frame {}: delay {} -> {} (asked by P{}, "
                "last delivered {}, rtt {} ms)",
                frame, m.delay, change.target, change.player, m.last_consumed,
                rex::net::online::PeerRttMs());
    if (change.target != m.delay) {
      m.live_used = true;
      ++m.delay_changes;
    }
    m.delay = change.target;
    g_delay.store(m.delay);
    if (!m.press_active) {
      m.desired = FinalDelay(m);
    }
    if (delay_notices) BurstLimitOnlineDelayNotice(DelayLine(m.delay), 3.0);
  }

  // What the consume delivers.
  m.tick = Match::Tick::kNormal;
  if (!m.live_used || m.last_consumed < 0) {
    return;
  }
  // rollback matches: while the rollback engine runs (the fight) the delay is fixed - a plain tick, nothing merged or
  // repeated (the merge reads ring slots before the recorded local entries are put back after a resim)
  if (m.rollback_match && BurstLimitRb2Active()) {
    return;
  }
  const int32_t want = frame - m.delay;
  if (want <= m.last_consumed) {
    // The delay went up: no new frame yet. The consume runs on a frame the
    // rings still have, its output is replaced by the last words.
    m.tick = Match::Tick::kFiller;
    g_consume_override.store(m.last_consumed, std::memory_order_relaxed);
    return;
  }
  if (want == m.last_consumed + 1) {
    return;
  }
  // The delay went down: this tick delivers several frames. All of them must
  // be here (a frame out of a game pack can overtake older ones), else the
  // fight waits for them like for any frame: a frame number the rings never
  // hold.
  for (int32_t g = m.last_consumed + 1; g <= want; ++g) {
    if (!RingHas(memory, manager, 0, g) || !RingHas(memory, manager, 1, g)) {
      g_consume_override.store(want + 64, std::memory_order_relaxed);
      return;
    }
  }
  uint32_t flag = 0;
  for (int p = 0; p < 2; ++p) {
    const uint32_t block = manager + uint32_t(p) * kPlayerBlock;
    uint64_t previous = m.delivered[p];
    uint64_t pressed = 0;
    uint64_t word = previous;
    for (int32_t g = m.last_consumed + 1; g <= want; ++g) {
      word = LoadU64(memory, block + (uint32_t(g) % kRingSlots) * kSlotSize);
      pressed |= word & ~previous & ~kCommandDirections;
      previous = word;
    }
    m.merged[p] = word | pressed;
  }
  // The synchronized event of the skipped frames (as in sub_8217E4E8).
  for (int32_t g = m.last_consumed + 1; g <= want; ++g) {
    const uint32_t slot = (uint32_t(g) % kRingSlots) * kSlotSize;
    if (LoadU32(memory, manager + slot + kSlotStatusA) == 2 &&
        LoadU32(memory, manager + kPlayerBlock + slot + kSlotStatusA) == 2 &&
        std::max(LoadS32(memory, manager + slot + kSlotStatusB),
                 LoadS32(memory, manager + kPlayerBlock + slot + kSlotStatusB)) == g) {
      flag = 1;
    }
  }
  m.merged_flag = flag;
  m.merge_to = want;
  m.tick = Match::Tick::kMerge;
  g_consume_override.store(want, std::memory_order_relaxed);
}

}  // namespace

// --- Called from the rest of the game ---------------------------------------------

void BurstLimitNetInputSetup() {
  rex::net::online::SetGameSideHandler(OnGameSide);
}

// Offline fights without the game's 3-frame input delay (input_delay =
// reduced), latched at the match input reset.
static std::atomic<bool> g_offline_reduced{false};

/// The exact input delay of the running online match (0 = the game's timing).
int BurstLimitOnlineDelayFrames() {
  return g_delay.load(std::memory_order_relaxed);
}

/// F5 / F6, or the debug command: a step of the input delay of the running
/// online match (nothing outside of one).
void BurstLimitOnlineDelayStep(int step) {
  g_live_steps.fetch_add(step, std::memory_order_relaxed);
}
void BurstLimitOnlineDelaySet(int delay) {
  g_live_absolute.store(delay, std::memory_order_relaxed);
}

/// The pad combo, read every game tick: Back + D-pad up / down. Holding Back
/// alone shows the delay.
static void LiveDelayPads(rex::memory::Memory* memory, Match& m) {
  static uint32_t previous = 0;
  static int back_ticks = 0;
  uint32_t down = 0;
  bool back = false;
  if (m.live_local) {
    for (uint32_t pad = 0; pad < 4; ++pad) {
      const uint32_t base = kPads + pad * kPadSize;
      const uint32_t buttons = LoadU32(memory, base) | LoadU32(memory, base + 16);
      if (buttons & kPadBack) {
        back = true;
        down |= buttons & (kPadUp | kPadDown);
      }
    }
  }
  if (down & ~previous & kPadUp) {
    g_live_steps.fetch_add(1, std::memory_order_relaxed);
  }
  if (down & ~previous & kPadDown) {
    g_live_steps.fetch_add(-1, std::memory_order_relaxed);
  }
  previous = down;
  back_ticks = back ? back_ticks + 1 : 0;
  if (back_ticks == 20 && !down && !m.press_active && m.live_peer) {
    BurstLimitOnlineDelayNotice(DelayLine(m.delay) + (m.rollback_match ? std::string("   automatic with rollback")
                                                                      : std::string("   Back + D-pad up / down changes it")), 3.0);
  }
}

/// Every game tick (BurstLimitOnlineFrame): the 10 s line, and the end of a
/// match.
void BurstLimitNetInputFrame(rex::memory::Memory* memory, bool in_match) {
  std::lock_guard<std::mutex> lock(g_match_mutex);
  Match& m = g_match;
  if (!in_match) {
    ThawLocked(m, memory);
    g_live_steps.store(0, std::memory_order_relaxed);
    g_live_absolute.store(0, std::memory_order_relaxed);
    g_live_active.store(false, std::memory_order_relaxed);
    return;
  }
  if (!m.online) {
    g_live_steps.store(0, std::memory_order_relaxed);
    g_live_absolute.store(0, std::memory_order_relaxed);
    return;
  }
  LiveDelayPads(memory, m);
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
  if (g_match.online && g_match.live_local) {
    const int me = g_match.local_player;
    REXLOG_WARN("[OnlineDelay] match total ({}): changes={} late={} filler ticks={} merges={} "
                "frames merged={} delay now={} last delivered={} digest={:08X} presses sampled={} "
                "delivered us={} them={}",
                why, g_match.delay_changes, g_match.delay_late, g_match.filler_ticks,
                g_match.merged_ticks, g_match.merged_frames, g_match.delay, g_match.last_consumed,
                g_match.delivered_digest, g_match.local_presses,
                me >= 0 ? g_match.delivered_presses[me] : 0,
                me >= 0 ? g_match.delivered_presses[1 - me] : 0);
  }
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
    if (value == "auto") {
      delay = 2;  // the start; the host moves it to the ping's delay right away (live-delay protocol, same tick on both)
    } else if (value != "off") {
      delay = std::clamp(std::atoi(value.c_str()), 2, 8);
    }
  }
  const int start = delay ? std::max(3, (delay + 2) / 3 * 3) : 3;
  // This match's netcode, the same on both PCs (see online_netcode).
  bool rollback = true;
  std::string netcode_why = "no room rules";
  if (online) {
    const std::string force = REXCVAR_GET(online_netcode);
    int rule = 1;
    if (force == "rollback" || force == "classic") {
      rollback = force == "rollback";
      netcode_why = "forced by online_netcode (the host's)";
    } else if (BurstLimitOnlineRulesNetcode(&rule)) {
      rollback = rule != 0;
      netcode_why = "room rule";
    }
  }
  g_match_rollback.store(rollback, std::memory_order_relaxed);
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
    g_match.start_delay = delay;
    g_match.desired = delay;
    g_match.start_frame = start;
    g_match.desync_check = online && REXCVAR_GET(online_desync_check);
    g_match.hold_freeze = online && REXCVAR_GET(online_hold_freeze);
    g_match.rollback_match = RollbackMatch();
    g_match.auto_phase = g_match.channel && delay > 0 && RollbackMatch() &&
                         REXCVAR_GET(online_rollback_phase_delay);
    g_match.auto_delay = g_match.channel && delay > 0 && REXCVAR_GET(online_input_delay) == "auto";
    g_match.fight_delay = delay;
    // rollback matches: no delay changes by hand; with online_rollback_phase_delay automatic ones outside the fight
    g_match.live_local = g_match.channel && delay > 0 &&
                         (RollbackMatch() ? (g_match.auto_phase || g_match.auto_delay)
                                                        : (REXCVAR_GET(online_live_delay) || g_match.auto_delay));
  }
  g_delay.store(delay);
  g_start_delay.store(delay);
  g_start_frame.store(start);
  g_consume_override.store(kNoConsumeOverride);
  g_live_active.store(false);
  g_live_steps.store(0);
  g_live_absolute.store(0);
  // Offline: the input delay option, fixed for the whole fight.
  g_offline_reduced.store(!rex::net::IsGameSessionOpen() && REXCVAR_GET(input_delay) == "reduced");
  if (online) {
    BurstLimitRb2NewMatch();  // v0.8.1 R17 (build 142): the engine's tracked-set check runs at this match's first tracked frame
    REXLOG_WARN("[OnlineInput] match reset: delay {} (first frame {}), input channel {}, "
                "desync check {}",
                delay ? std::to_string(delay) + " frames" : std::string("game (3 ticks)"), start,
                g_match.channel ? "on" : "off", g_match.desync_check ? "on" : "off");
    REXLOG_WARN("[OnlineInput] netcode: {} ({})", rollback ? "Rollback" : "Classic (lockstep)", netcode_why);
    BurstLimitOnlineDelayNotice(rollback ? "Netcode: Rollback"
                                         : "Netcode: Classic   Back + D-pad up / down changes the delay",
                                4.0);
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
  const int delay = g_start_delay.load(std::memory_order_relaxed);
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
    // Offline (driver state 0, no game session): input_delay reduced takes
    // the frame the driver has just written (sub_8217E808 writes both
    // players' commands at the tick frame, then consumes) instead of the one
    // 3 frames back. Online (state 3, or any open session) is never touched.
    if (g_offline_reduced.load(std::memory_order_relaxed) && !rex::net::IsGameSessionOpen()) {
      if (auto* memory = GuestMemory()) {
        const uint32_t manager = LoadU32(memory, kInputManagerPointer);
        if (manager && LoadU32(memory, manager + kDriverState) == kDriverStateOffline) {
          r5.s64 = int64_t(r10.s32);
        }
      }
    }
    // Rollback research (branch rollback2): the loopback test's player 2 input.
    if (auto* memory = GuestMemory()) BurstLimitRb2OnConsume(memory, r5.s32);
    return;
  }
  auto* memory = GuestMemory();
  if (!memory) {
    return;
  }
  // Only the online driver (state 3); offline (state 0) keeps the game's 3.
  const uint32_t manager = LoadU32(memory, kInputManagerPointer);
  if (manager && LoadU32(memory, manager + kDriverState) == kDriverStateMatch) {
    const int32_t override_frame = g_consume_override.load(std::memory_order_relaxed);
    r5.s64 = override_frame != kNoConsumeOverride ? int64_t(override_frame)
                                                  : int64_t(r10.s32) - delay;
  }
}

// In the local input task (BurstLimitKiChargeInput's hook, the word just
// built): the D-pad of Back + D-pad belongs to the delay, not to the fight.
void BurstLimitOnlineDelayMaskInput(rex::memory::Memory* memory, uint32_t input) {
  if (!g_live_active.load(std::memory_order_relaxed)) {
    return;
  }
  for (uint32_t pad = 0; pad < 4; ++pad) {
    const uint32_t base = kPads + pad * kPadSize;
    const uint32_t buttons = LoadU32(memory, base) | LoadU32(memory, base + 16);
    if ((buttons & kPadBack) && (buttons & (kPadUp | kPadDown | kPadLeft | kPadRight))) {
      StoreU64(memory, input + 64, LoadU64(memory, input + 64) & ~kCommandDpadDirections);
      return;
    }
  }
}

// The ring slot of `frame` set to this entry whatever it held (a prediction, an older frame).
void ForceRing(rex::memory::Memory* memory, uint32_t manager, int player, int32_t frame, uint64_t command,
               uint32_t status_a, uint32_t status_b) {
  if (WriteRing(memory, manager, player, frame, command, status_a, status_b) == 0) {
    // the slot already holds this frame (a prediction written earlier, or the game's own copy): overwrite
    // the command too - MEASURED on9: only the status words were replaced, a rolled-back frame kept its
    // predicted command and the two PCs diverged at the first misprediction the ring still held
    const uint32_t slot = manager + uint32_t(player) * kPlayerBlock + (uint32_t(frame) % kRingSlots) * kSlotSize;
    StoreU64(memory, slot, command);
    StoreU32(memory, slot + kSlotStatusA, status_a);
    StoreU32(memory, slot + kSlotStatusB, status_b);
  }
}

// Rollback: a local frame the game packed that nothing recorded yet (a resim can reach ticks the live run never
// did - the live run HELD there and the resim consumed through those engine frames. on44: the guest's resim packed
// its frames 3135-3136 from the pads of that moment, nothing recorded or sent them, the host used 0 and the fights
// split at 3137). Recorded from the ring slot the game will consume (first pack wins), sent after the resim.
void RecordFirstPacks(Match& m, rex::memory::Memory* memory, uint32_t manager, int32_t upto, const char* where) {
  if (m.local_player < 0 || upto < 0) return;
  for (int32_t f = std::max(0, upto - 8); f <= upto; ++f) {  // holes below the newest too: the sender stops at the first one
    InputEntry& le = m.local[uint32_t(f) % m.local.size()];
    if (le.frame == f) continue;
    const uint32_t slot = manager + uint32_t(m.local_player) * kPlayerBlock + (uint32_t(f) % kRingSlots) * kSlotSize;
    if (LoadS32(memory, slot + kSlotFrame) != f) continue;
    le.frame = f;
    le.command = LoadU64(memory, slot);
    le.status_a = LoadU32(memory, slot + kSlotStatusA);
    le.status_b = LoadU32(memory, slot + kSlotStatusB);
    m.newest_local = std::max(m.newest_local, f);
    ++m.rb_first_packs;
    if (m.rb_first_packs <= 40)
      REXLOG_INFO("[OnlineRollback] local frame {} first packed in a {} tick (the live run held there): recorded from the ring, cmd {:X}",
                  f, where, le.command);
  }
}

// Rollback: a local frame that is due (consume .. upto) but neither in our ring nor recorded can never come back - the
// game waits for its own input forever and the other PC waits for us (rb_burst: an 18-frame rewind put back a ring
// without frame 2268 and nothing had recorded it -> 10 s, session lost). It gets the previous frame's input, recorded
// and sent like any other, so both PCs consume the same.
void RepairLocalHoles(Match& m, rex::memory::Memory* memory, uint32_t manager, int32_t consume, int32_t upto) {
  if (m.local_player < 0) return;
  for (int32_t f = std::max(consume, 1); f <= upto; ++f) {
    InputEntry& le = m.local[uint32_t(f) % m.local.size()];
    if (le.frame == f) continue;
    const uint32_t slot = manager + uint32_t(m.local_player) * kPlayerBlock + (uint32_t(f) % kRingSlots) * kSlotSize;
    if (LoadS32(memory, slot + kSlotFrame) == f) continue;  // in the ring: RecordFirstPacks takes it
    const InputEntry& prev = m.local[uint32_t(f - 1) % m.local.size()];
    le.frame = f;
    le.command = prev.frame == f - 1 ? prev.command : 0;
    le.status_a = prev.frame == f - 1 ? prev.status_a : 0;
    le.status_b = prev.frame == f - 1 ? prev.status_b : 0;
    m.newest_local = std::max(m.newest_local, f);
    ForceRing(memory, manager, m.local_player, f, le.command, le.status_a, le.status_b);
    if (m.resend_from < 0 || f < m.resend_from) m.resend_from = f;
    ++m.rb_hole_repairs;
    if (m.rb_hole_repairs <= 30)
      REXLOG_WARN("[OnlineRollback] local frame {} was lost (not in the ring, never recorded): the previous input {:X} is used and sent", f,
                  le.command);
  }
}

// v0.8.1 R30 (the file gate of burstlimit_rb2.cpp): the newest input frame consumed with a prediction and the frontier
// up to which every remote input is known. A prediction above the frontier is unsettled: it can still turn out
// wrong and ask for a rewind to its frame.
std::atomic<int32_t> g_rb_last_pred{-1}, g_rb_contig{-1};
bool BurstLimitOnlineUnsettled() {
  return g_rb_last_pred.load(std::memory_order_relaxed) > g_rb_contig.load(std::memory_order_relaxed);
}
// v0.8.1 R8: why the last remote input that was not there yet got no prediction (the fight then waits: a held tick).
// Read by the hold lines of burstlimit_online.cpp ("[OnlineHold]").
const char* BurstLimitRb2HoldReason();
const char* g_net_hold_reason = "";
// review L7: a Classic (lockstep) match has no rollback hold reasons - the last rollback match's one is not reported
const char* BurstLimitOnlineHoldReason() { return RollbackMatch() ? g_net_hold_reason : ""; }
// 0 = this PC is the host of the running rollback match, 1 = the guest, -1 = no rollback match (main thread; the
// file gate's stagger of burstlimit_rb2.cpp asks from inside the consume, so no lock here)
int BurstLimitOnlineLocalPlayer() { return (g_match.online && RollbackMatch()) ? g_match.local_player : -1; }

// Rollback: a remote entry received (live frames only). A prediction consumed for its frame that
// differs asks the engine to go back to the frame that consumed it.
bool RollbackReceivedLocked(Match& m, const RemoteEntry& e) {
  InputEntry& known = m.remote_known[uint32_t(e.frame) % m.remote_known.size()];
  uint32_t& known_seq = m.remote_seq[uint32_t(e.frame) % m.remote_seq.size()];
  if (known.frame != e.frame) {
    known.frame = e.frame;
    known.command = e.command;
    known.status_a = e.status_a;
    known.status_b = e.status_b;
    known_seq = e.seq;
  } else if (e.seq && known_seq && int32_t(e.seq - known_seq) < 0 && REXCVAR_GET(online_rollback_seq)) {
    // an older packet (a delayed copy, a reordered datagram) after a newer one: what it says about this frame is
    // out of date. Before build 136 it was taken as a correction: the entry flipped back to the old value and
    // stayed there if no later packet carried the frame again (r135_long, frame 1631: 2008.. -> 0 -> 2008..).
    if (known.command != e.command || known.status_a != e.status_a || known.status_b != e.status_b) {
      ++m.rb_stale_entries;
      if (m.rb_stale_entries <= 40)
        REXLOG_INFO("[OnlineRollback] remote entry of frame {} from an older packet ignored: {:X}/{}/{} (packet {} after {}), kept {:X}/{}/{}",
                    e.frame, e.command, e.status_a, e.status_b, e.seq, known_seq, known.command, known.status_a, known.status_b);
    }
    return false;
  } else if (known.command != e.command || known.status_a != e.status_a || known.status_b != e.status_b) {
    // the other PC corrected this frame's entry after one of its rollbacks
    known_seq = e.seq;
    known.command = e.command;
    known.status_a = e.status_a;
    known.status_b = e.status_b;
    const auto& c = m.consumed_at[uint32_t(e.frame) % m.consumed_at.size()];
    ++m.rb_remote_corrections;
    if (m.rb_remote_corrections <= 200)
      REXLOG_INFO("[OnlineRollback] remote entry of frame {} corrected: {:X}/{}/{} (consumed at engine {})", e.frame, e.command,
                  e.status_a, e.status_b, c.first == e.frame ? c.second : -1);
    if (c.first == e.frame) BurstLimitRb2RequestRewind(c.second);
  }
  if (e.seq && int32_t(e.seq - known_seq) > 0) known_seq = e.seq;
  if (e.frame > m.newest_remote.frame) m.newest_remote = known;
  // the contiguous frontier: every remote frame up to it is known (a lost one keeps it back until it is resent)
  // (re)started when it is far off: the first entry of a fight - remote_known is only filled while the engine runs, so
  // the last fight's frontier is stale after a round change (64: the sender can't run that far past a gap)
  if (m.remote_contig < 0 || e.frame > m.remote_contig + 64 || e.frame < m.remote_contig - 100) {
    m.remote_contig = e.frame - 1;
    g_rb_last_pred.store(-1, std::memory_order_relaxed);  // a new fight's frame numbers
  }
  while (m.remote_known[uint32_t(m.remote_contig + 1) % m.remote_known.size()].frame == m.remote_contig + 1) ++m.remote_contig;
  g_rb_contig.store(m.remote_contig, std::memory_order_relaxed);
  Match::Prediction& p = m.predicted[uint32_t(e.frame) % m.predicted.size()];
  if (p.frame == e.frame && !p.settled) {
    p.settled = true;
    if (p.command != e.command || p.status_a != e.status_a || p.status_b != e.status_b) {
      ++m.rb_mispredicted;
      if (m.rb_mispredicted <= 3000)
        REXLOG_INFO("[OnlineRollback] mispredicted input frame {} (engine {}): predicted {:X}/{}/{} real {:X}/{}/{}", e.frame,
                    p.engine_frame, p.command, p.status_a, p.status_b, e.command, e.status_a, e.status_b);
      BurstLimitRb2RequestRewind(p.engine_frame);
    }
  }
  return true;
}

// Rollback: the remote input of the frame consumed now - the confirmed one, or (only while both
// sides fight: same status words) a prediction = the newest confirmed input.
bool RollbackRemoteChooseLocked(Match& m, rex::memory::Memory* memory, uint32_t manager, int32_t consume) {
  if (m.local_player < 0 || consume < 0) return true;
  const int remote = 1 - m.local_player;
  const InputEntry& known = m.remote_known[uint32_t(consume) % m.remote_known.size()];
  m.consumed_at[uint32_t(consume) % m.consumed_at.size()] = {consume, BurstLimitRb2CurrentFrame()};
  if (known.frame == consume) {
    ForceRing(memory, manager, remote, consume, known.command, known.status_a, known.status_b);
    if (BurstLimitRb2InResim()) {
      static int shown = 0;
      if (shown++ < 60) {
        const uint32_t slot = manager + uint32_t(remote) * kPlayerBlock + (uint32_t(consume) % kRingSlots) * kSlotSize;
        REXLOG_INFO("[OnlineRollback] resim consume {}: known {:X}, ring slot frame {} cmd {:X} | read idx {} write idx {} highest {} | tick {}",
                    consume, known.command, LoadS32(memory, slot + kSlotFrame), LoadU64(memory, slot),
                    LoadS32(memory, manager + uint32_t(remote) * kPlayerBlock + 768), LoadS32(memory, manager + uint32_t(remote) * kPlayerBlock + 772),
                    LoadS32(memory, manager + uint32_t(remote) * kPlayerBlock + 776), LoadS32(memory, manager + kTickFrame));
      }
    }
    return true;
  }
  // Not the game's own packets: with rollback the other PC's game also packs frames during ITS resims, with
  // the pads of that moment, and its packets can put those into our ring for a frame our channel hasn't
  // delivered yet - consumed without any check (on15: an input replayed as 0 for good). Only the channel's
  // entries (recorded live on the other PC) count; anything else is a prediction.
  // newest confirmed input before this frame
  const InputEntry* base = nullptr;
  for (int32_t k = consume - 1; k >= std::max(0, consume - 60); --k) {
    const InputEntry& e = m.remote_known[uint32_t(k) % m.remote_known.size()];
    if (e.frame == k) { base = &e; break; }
  }
  if (!base) { ++m.rb_no_base; g_net_hold_reason = "no confirmed input to predict from"; return false; }
  // both in plain fighting: status A = 0 on both sides (A = 1 / 2 are round end and sync-event requests).
  // Status B is per player (MEASURED on13: 6 on one PC, 7 on the other for a whole round) - not compared.
  if (base->status_a != 0 || m.last_status_a != 0) {  // not both fighting: hold
    ++m.rb_status_diff;
    m.rb_status_sample = (uint64_t(base->status_a) << 48) ^ (uint64_t(base->status_b) << 32) ^ (uint64_t(m.last_status_a) << 16) ^ m.last_status_b;
    g_net_hold_reason = "status words (sync event / round flow)";
    return false;
  }
  if (!BurstLimitRb2PredictOk()) { ++m.rb_not_ready; g_net_hold_reason = BurstLimitRb2HoldReason(); return false; }  // a rewind there would be refused: hold
  // Safety net: after a confirmed desync no more predictions in this match (plain lockstep: no rollback on
  // top of a state that already differs; the match goes on and ends normally).
  if (m.desync_reported) { ++m.rb_after_desync; g_net_hold_reason = "after a desync (no more predictions)"; return false; }
  if (!BurstLimitRb2InResim() && REXCVAR_GET(online_time_sync) && m.pred_avg - m.remote_pred_avg >= 1.5f &&
      m.ts_tick - m.ts_last_hold >= 20) {
    m.ts_last_hold = m.ts_tick;  // running ahead: wait one tick for the other PC (time sync)
    ++m.ts_holds;
    g_net_hold_reason = "time sync (running ahead)";
    return false;
  }
  if (consume - base->frame > 8) { ++m.rb_too_old; g_net_hold_reason = "more than 8 frames ahead of the newest input"; return false; }  // too far ahead: hold (the game's wait)
  // ... also past the oldest frame still missing (rb_burst: frames lost in a burst arrived when a rewind to them was
  // deeper than the engine keeps; or never - then their predictions stayed): wait for the resend
  if (m.remote_contig >= 0 && consume - m.remote_contig > 9 && consume - m.remote_contig < 64) { ++m.rb_gap_holds; g_net_hold_reason = "gap in the received inputs (waiting for a resend)"; return false; }
  ForceRing(memory, manager, remote, consume, base->command, base->status_a, base->status_b);
  Match::Prediction& p = m.predicted[uint32_t(consume) % m.predicted.size()];
  p.frame = consume;
  p.command = base->command;
  p.status_a = base->status_a;
  p.status_b = base->status_b;
  p.engine_frame = BurstLimitRb2CurrentFrame();
  p.settled = false;
  ++m.rb_predicted;
  if (consume > g_rb_last_pred.load(std::memory_order_relaxed)) g_rb_last_pred.store(consume, std::memory_order_relaxed);
  return true;
}

// The tick's remote input: chosen (above), or the tick holds - then the slot must not be consumed. Before build 137
// a slot the game's own receiver had filled (network thread, the other PC's game packets) or a prediction left
// from before a rewind was consumed in a held tick, unknown to the prediction records.
void RollbackRemoteLocked(Match& m, rex::memory::Memory* memory, uint32_t manager, int32_t consume) {
  if (RollbackRemoteChooseLocked(m, memory, manager, consume)) return;
  if (!REXCVAR_GET(online_rollback_consume_fix_remote) || m.local_player < 0 || consume < 0) return;
  const uint32_t slot = manager + uint32_t(1 - m.local_player) * kPlayerBlock + (uint32_t(consume) % kRingSlots) * kSlotSize;
  if (LoadS32(memory, slot + kSlotFrame) != consume) return;
  StoreU32(memory, slot + kSlotFrame, 0xFFFFFFFFu);
  ++m.rb_foreign_slots;
  if (m.rb_foreign_slots <= 40)
    REXLOG_INFO("[OnlineRollback] held tick: remote ring slot of frame {} (cmd {:X}, status {}/{}) was not chosen by this PC - taken out ({}{})",
                consume, LoadU64(memory, slot), LoadU32(memory, slot + kSlotStatusA), LoadU32(memory, slot + kSlotStatusB),
                g_net_hold_reason, BurstLimitRb2InResim() ? "; resim" : "");
}

// v0.8.1 R19 (build 138): received inputs were looked at only in the driver hook, after the engine's frame start -
// a misprediction found in tick T was rewound at the frame start of T + 1 (the real match of 2026-11-14: all 153
// rollbacks ran 16-18 ms after the detection). Called at the engine's frame start (live frames only), this takes
// what has arrived since the last tick: the wrong prediction is rewound in this tick, one frame shallower, and the
// corrected picture is shown a frame sooner. The entries stay in the inbox; the driver hook hands them over as
// before (they are known by then: no second correction, no second rewind).
REXCVAR_DEFINE_BOOL(online_rollback_peek, true, "Patches",
                    "Online rollback: inputs that arrived since the last tick are examined when the frame starts, so a wrong "
                    "prediction is rewound one tick earlier (off = builds up to 137)");
static std::atomic<uint64_t> g_peek_calls{0}, g_peek_entries{0}, g_peek_rewinds{0};
int64_t BurstLimitRb2LoopPeekRewind();
void BurstLimitOnlinePeekInbox() {
  if (!REXCVAR_GET(online_rollback_peek) || !RollbackMatch() || !BurstLimitRb2Active() || BurstLimitRb2InResim()) return;
  std::lock_guard<std::mutex> lock(g_match_mutex);
  Match& m = g_match;
  if (!m.channel || !m.online || m.local_player < 0) return;
  std::vector<RemoteEntry> peek;
  {
    std::lock_guard<std::mutex> inbox_lock(g_inbox.mutex);
    if (g_inbox.entries.empty()) return;
    peek = g_inbox.entries;
  }
  const int64_t before = BurstLimitRb2LoopPeekRewind();
  ++g_peek_calls;
  for (const RemoteEntry& e : peek) {
    if (e.player == m.local_player || e.frame < 0) continue;
    ++g_peek_entries;
    RollbackReceivedLocked(m, e);
  }
  const int64_t after = BurstLimitRb2LoopPeekRewind();
  if (after >= 0 && (before < 0 || after < before)) ++g_peek_rewinds;
}
// v0.8.1 R38: the game's network thread writes both input rings through sub_8217E630 (two callers only: its packet
// sender sub_8229D8E8 at 0x8229DAC8 copies three local history entries, its receiver sub_82298498 at 0x82298664
// the other PC's game packets; both ignore the result). While the rollback engine tracks, the rings are written by
// this file's hooks (ForceRing: local frames from the first recorded entry, remote frames from the channel or a
// prediction); builds 136 / 137 only repair what those foreign writes do. With the switch on they are dropped.
// OFF by default: needs its acceptance run (T2: "consumed unchosen" 0, "slots taken out" 0, frames identical).
REXCVAR_DEFINE_BOOL(online_rollback_own_rings, false, "Patches",
                    "Online rollback (experimental): while the rollback engine tracks, the game's own network thread no "
                    "longer writes the input rings (only the rollback input layer does)");
static std::atomic<uint64_t> g_game_ring_writes_dropped{0}, g_game_ring_writes_kept{0};
REX_EXTERN(__imp__sub_8217E630);
extern "C" REX_FUNC(sub_8217E630) {
  if (RollbackMatch() && BurstLimitRb2Active()) {
    if (REXCVAR_GET(online_rollback_own_rings)) {
      g_game_ring_writes_dropped.fetch_add(1, std::memory_order_relaxed);
      ctx.r3.u64 = 0;
      return;
    }
    g_game_ring_writes_kept.fetch_add(1, std::memory_order_relaxed);
  }
  __imp__sub_8217E630(ctx, base);
}
std::string BurstLimitOnlinePeekStats() {
  return fmt::format("peek at the frame start: {} calls with inputs, {} entries, {} rewinds asked a tick earlier | the game's own ring "
                     "writes while tracking: {} made, {} dropped (own rings)", g_peek_calls.load(),
                     g_peek_entries.load(), g_peek_rewinds.load(), g_game_ring_writes_kept.load(), g_game_ring_writes_dropped.load());
}

// The pending flag of the event (0x84254730, counter 0x84254738: sub_822938E0 ends the session above 600 net ticks)
// shares its 16-byte block with the counter the net tick writes, so a rewind can put net+1728 back to 0 and leave
// the flag set: a ghost request (long2: "Connection with other player has been lost" 10 s later). No event is
// open when net+1728 is 0, so the flag is cleared then.
REXCVAR_DEFINE_BOOL(online_rollback_sync_reconcile, true, "Patches",
                    "Online rollback: a pending synchronized-event flag left set by a rewind (no event open) is cleared "
                    "instead of timing the session out");
static std::atomic<uint32_t> g_early_sync_skipped{0}, g_sync_reconciled{0}, g_sync_counter_peak{0};
// sub_8217E860 (online driver, state 3), right before the consume: this
// tick's local frame goes into the local ring and out to the other PC, and
// the frames the other PC sent go into its ring.
void BurstLimitOnlineBeforeConsume() {
  g_consume_override.store(kNoConsumeOverride, std::memory_order_relaxed);
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
  g_rb2_match.store(m.online && m.channel, std::memory_order_relaxed);
  const bool rollback = RollbackMatch() && BurstLimitRb2Active();
  const bool resim = rollback && BurstLimitRb2InResim();
  if (RollbackMatch() && !resim && Readable(memory, 0x84255714, 4)) {  // v0.8.1 R5: ghost sync-event flag
    const uint32_t net = LoadU32(memory, 0x84255714);
    const uint32_t counter = LoadU32(memory, 0x84254738);
    if (counter > g_sync_counter_peak.load(std::memory_order_relaxed)) g_sync_counter_peak.store(counter, std::memory_order_relaxed);
    static int stale_ticks = 0;
    if (net && Readable(memory, net + 1728, 4) && LoadU32(memory, net + 1728) == 0 && LoadU32(memory, 0x84254730) != 0) {
      // two ticks in a row: never between the two stores of a request that is being made right now
      if (++stale_ticks >= 2 && REXCVAR_GET(online_rollback_sync_reconcile)) {
        rex::memory::store_and_swap<uint32_t>(memory->TranslateVirtual<uint8_t*>(0x84254730), 0);
        rex::memory::store_and_swap<uint32_t>(memory->TranslateVirtual<uint8_t*>(0x84254738), 0);
        const uint32_t n = ++g_sync_reconciled;
        REXLOG_WARN("[OnlineInput] sync flag reconciled at frame {} (counter {}; no event open, {} so far)", frame, counter, n);
        stale_ticks = 0;
      }
    } else {
      stale_ticks = 0;
    }
  }
  if (!resim) {  // test (online_test_slow = <from tick>:<ticks>:<ms>): this PC's frame rate drops for a while
    static int slow_from = -2, slow_n = 0, slow_ms = 0;
    if (slow_from == -2) {
      slow_from = -1;
      const std::string v = REXCVAR_GET(online_test_slow);
      if (std::sscanf(v.c_str(), "%d:%d:%d", &slow_from, &slow_n, &slow_ms) != 3) slow_from = -1;
    }
    if (slow_from >= 0 && frame >= slow_from && frame < slow_from + slow_n && slow_ms > 0)
      std::this_thread::sleep_for(std::chrono::milliseconds(slow_ms));
  }
  if (resim) {
    const int delay = m.delay ? m.delay : 3;  // rollback matches never change the delay (no live delay)
    const int32_t consume = frame - delay;
    // A re-simulated frame: the local input recorded when it ran live (the pads now are another
    // frame's), the remote input confirmed since or predicted again; nothing is sent.
    if (m.local_player >= 0 && frame >= 0 && m.local[uint32_t(frame) % m.local.size()].frame == frame) {
      InputEntry& e = m.local[uint32_t(frame) % m.local.size()];
      // The command is the player's (recorded live); the status words are the GAME's (round end / sync
      // event requests: A=2 B=frame) and follow the corrected timeline. MEASURED on6: the guest's resim
      // found the KO but the recorded status (A=0) was forced -> no sync event there, the host ended the
      // match alone. A changed status is sent again (the other PC rolls back to it).
      const uint32_t index = uint32_t(frame) % kHistorySlots;
      if (LoadS32(memory, manager + kSubmittedFrame) == frame && LoadS32(memory, kHistory + 4 * index) == frame) {
        const uint32_t sa = LoadU32(memory, kHistory + 144 + 4 * index);
        const uint32_t sb = LoadU32(memory, kHistory + 48 + 4 * index);
        if (sa != e.status_a || sb != e.status_b) {
          ++m.rb_local_corrections;
          if (m.rb_local_corrections <= 200)
            REXLOG_INFO("[OnlineRollback] local status of frame {} corrected by a resim: {}/{} -> {}/{}", frame, e.status_a,
                        e.status_b, sa, sb);
          e.status_a = sa;
          e.status_b = sb;
          if (m.resend_from < 0 || frame < m.resend_from) m.resend_from = frame;
        }
      }
      ForceRing(memory, manager, m.local_player, frame, e.command, e.status_a, e.status_b);
    }
    // ... and the local frames up to the one consumed now: the game's own pack can run after this hook in a
    // resim tick and put the CURRENT pads into its ring slot (on14: the host replayed its own frame 1480 with 0)
    if (m.local_player >= 0) {
      for (int32_t f = std::max(consume, 0); f < frame; ++f) {
        const InputEntry& le = m.local[uint32_t(f) % m.local.size()];
        if (le.frame == f) ForceRing(memory, manager, m.local_player, f, le.command, le.status_a, le.status_b);
      }
    }
    if (REXCVAR_GET(online_record_first_pack)) RecordFirstPacks(m, memory, manager, frame, "resim");
    if (REXCVAR_GET(online_record_first_pack)) RepairLocalHoles(m, memory, manager, consume, frame - 1);
    RollbackRemoteLocked(m, memory, manager, consume);
    return;
  }
  // a frame the last resim tick packed after its hook
  if (rollback && REXCVAR_GET(online_record_first_pack)) {
    RecordFirstPacks(m, memory, manager, frame - 1, "resim");
    RepairLocalHoles(m, memory, manager, frame - (m.delay ? m.delay : 3), frame - 1);
  }
  // ... and a local frame the last resim tick packed AGAIN after its hook (its own tick: the resim branch forces only
  // the frames below it) holds the pads of that moment, not what was recorded and sent (long3 / snd_120c: the guest
  // consumed 280208.. for frame 5556, the host had 0 - the su_tr2 mismatch): every recorded local frame still to be
  // consumed gets its sent command back
  if (rollback && m.local_player >= 0) {
    const int32_t from = std::max(frame - (m.delay ? m.delay : 3) - 1, 0);
    for (int32_t f = from; f < frame; ++f) {
      const InputEntry& le = m.local[uint32_t(f) % m.local.size()];
      if (le.frame != f) continue;
      const uint32_t slot = manager + uint32_t(m.local_player) * kPlayerBlock + (uint32_t(f) % kRingSlots) * kSlotSize;
      if (LoadS32(memory, slot + kSlotFrame) != f || LoadU64(memory, slot) == le.command) continue;
      ++m.rb_local_ring_fix;
      if (m.rb_local_ring_fix <= 40)
        REXLOG_INFO("[OnlineRollback] local frame {}: ring slot {:X} (packed again in the last resim tick) -> the sent {:X}", f,
                    LoadU64(memory, slot), le.command);
      ForceRing(memory, manager, m.local_player, f, le.command, le.status_a, le.status_b);
    }
  }

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
      if (rollback && (player == 0 || player == 1)) {
        // rollback: what we send must be what our own game consumes - the ring slot (MEASURED on18/on19:
        // the history command and the ring command of the same frame can differ)
        const uint32_t slot = manager + uint32_t(player) * kPlayerBlock + (uint32_t(frame) % kRingSlots) * kSlotSize;
        if (LoadS32(memory, slot + kSlotFrame) == frame) entry.command = LoadU64(memory, slot);
      }
      if (rollback && REXCVAR_GET(online_record_first_pack) && (player == 0 || player == 1) &&
          m.local[uint32_t(frame) % m.local.size()].frame == frame) {
        // packed again (a resim ended at an earlier tick than the live run): the first pack was sent - keep its
        // command (the statuses are the game's and follow the corrected timeline)
        const InputEntry& first = m.local[uint32_t(frame) % m.local.size()];
        if (first.command != entry.command) {
          ++m.rb_repacks_kept;
          if (m.rb_repacks_kept <= 40)
            REXLOG_INFO("[OnlineRollback] local frame {} packed again: keeping the sent command {:X} (pads now {:X})", frame,
                        first.command, entry.command);
        }
        if (entry.status_a != first.status_a || entry.status_b != first.status_b) {
          if (m.resend_from < 0 || frame < m.resend_from) m.resend_from = frame;
        }
        entry.command = first.command;
      }
      if (player == 0 || player == 1) {
        if (entry.status_a == 2 && m.last_status_a != 2 && m.live_local && m.live_peer &&
            REXCVAR_GET(online_live_delay_test_sync)) {
          // The frame goes out with this tick's packet and the other PC needs
          // it to reach this tick + delay: the shortest lead that still holds.
          m.own_target = m.delay >= 5 ? kLiveMinDelay : kLiveMaxDelay;
          m.own_effective = frame + m.delay;
          m.pending.push_back({m.own_effective, m.own_target, player});
          REXLOG_WARN("[OnlineDelay] test: delay {} from tick frame {} over the event at {}",
                      m.own_target, m.own_effective, frame);
        }
        if (entry.status_a != m.last_status_a || entry.status_b != m.last_status_b) {
          m.last_status_a = entry.status_a;
          m.last_status_b = entry.status_b;
          REXLOG_INFO("[OnlineInput] local status at frame {}: A={} B={} (player {})", frame,
                      entry.status_a, entry.status_b, player);
        }
        m.local_player = player;
        if (m.local[uint32_t(frame) % m.local.size()].frame != frame) {
          m.local_presses += NewPresses(entry.command, m.last_local_word);
          m.last_local_word = entry.command;
        }
        if (entry.command && m.local[uint32_t(frame) % m.local.size()].frame != frame) {
          ++m.window.local_pressed;
          ++m.total.local_pressed;
        }
        m.local[uint32_t(frame) % m.local.size()] = entry;
        m.newest_local = std::max(m.newest_local, frame);
        if (rollback) ForceRing(memory, manager, player, frame, entry.command, entry.status_a, entry.status_b);
        else WriteRing(memory, manager, player, frame, entry.command, entry.status_a, entry.status_b);
      }
    }
  }

  // 2. The other PC's frames.
  std::vector<RemoteEntry> entries;
  std::vector<std::pair<int32_t, uint32_t>> hashes;
  bool has_ack, peer_live;
  int32_t remote_ack, sender_frame, peer_effective;
  int peer_target;
  uint64_t packets;
  {
    std::lock_guard<std::mutex> inbox_lock(g_inbox.mutex);
    entries.swap(g_inbox.entries);
    hashes.swap(g_inbox.hashes);
    has_ack = g_inbox.has_ack;
    remote_ack = g_inbox.ack;
    sender_frame = g_inbox.sender_frame;
    m.remote_pred_avg = g_inbox.remote_pred4 / 4.0f;
    if (g_inbox.remote_desync && !m.desync_reported && rollback) {
      m.desync_reported = true;
      BurstLimitOnlineAlert("desync", "the two games no longer match (found by the other PC)");
      REXLOG_WARN("[OnlineDesync] the other PC found a desync at its frame {} - this match ends", g_inbox.sender_frame);
      WriteDumps(m, "reported by the other PC");
    }
    packets = g_inbox.packets;
    g_inbox.packets = 0;
    peer_live = g_inbox.peer_live;
    peer_target = g_inbox.peer_target;
    peer_effective = g_inbox.peer_effective;
  }
  m.window.received += packets;
  m.total.received += packets;
  // A packet from another match (before / after a reset) is far off.
  const bool same_match = has_ack && std::abs(sender_frame - frame) <= 64;
  LiveDelayTick(m, memory, manager, frame, same_match, peer_live, peer_target, peer_effective);
  const int delay = m.delay ? m.delay : 3;
  const int32_t consume = frame - delay;
  // After a change the frames still to deliver aren't simply tick - delay, and
  // around the switch the other PC can be ahead by the larger of the delays.
  const bool live_window = m.live_used || !m.pending.empty();
  const int32_t lowest = m.live_used ? std::min(consume, m.last_consumed + 1) : consume;
  const int32_t highest = frame + (live_window ? kLiveMaxDelay : delay) + 1;
  for (const RemoteEntry& e : entries) {
    if (rollback && m.local_player >= 0 && e.player != m.local_player && e.frame >= 0) {
      if (!RollbackReceivedLocked(m, e)) continue;  // from an older packet than the one this frame is known from
    }
    if (m.local_player < 0 || e.player == m.local_player || e.frame < lowest ||
        e.frame > highest) {
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
  g_ov_tick_ms.store(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(),
                     std::memory_order_relaxed);
  g_ov_delay.store(m.delay ? m.delay : 3, std::memory_order_relaxed);
  g_ov_rollback.store(m.rollback_match, std::memory_order_relaxed);
  g_ov_ahead10.store(int(m.pred_avg * 10.0f + 0.5f), std::memory_order_relaxed);
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
      // rollback: compared once the frame is final here too - a hash that arrives after our own frame went final
      // is compared now (at 75 ms the other PC's hash of frame F lands after our tick for F: on40 compared almost
      // nothing during the fight and found a desync ~40 s late)
      if (!rollback || hash_frame <= consume - kHashSettleFrames) CompareHash(m, hash_frame);
    }
  }

  if (rollback && m.local_player >= 0) {
    // live tick right after a resim: our ring slots of the last resimulated ticks were rewritten by the game's
    // own pack there (pads of that moment) - put the recorded entries back before they are consumed (on22)
    for (int32_t f = std::max(consume, 0); f < frame; ++f) {
      const InputEntry& le = m.local[uint32_t(f) % m.local.size()];
      if (le.frame == f) ForceRing(memory, manager, m.local_player, f, le.command, le.status_a, le.status_b);
    }
  }
  if (rollback) {
    // desync check on final frames only (GekkoNet style): a frame older than any rollback can reach
    if (m.desync_check && consume - kHashSettleFrames >= 0) CompareHash(m, consume - kHashSettleFrames);
    if (m.newest_remote.frame >= 0 && consume >= 0) {
      const float ahead = float(std::clamp(consume - m.newest_remote.frame, 0, 20));
      m.pred_avg += (ahead - m.pred_avg) / 30.0f;
    }
    m.ts_tick = frame;
    RollbackRemoteLocked(m, memory, manager, consume);
    if (frame > 0 && frame % 600 == 0) {
      REXLOG_INFO("[OnlineRollback] frame {}: predicted {} mispredicted {} too far ahead {} engine not ready {} no base {} status differs {} (remote A/B vs local A/B {:016X}) corrections local {} remote {} local entries recorded late {} ring fixes {} | time sync: ahead {:.2f} vs remote {:.2f}, holds {} (delay {}) | gap holds {} old-frame resends {} lost local frames {} first packs {} | sync requests: waited {} skipped {} timed out {} | consume fixes {} (resim {}) sent command kept at frame end {} entries from older packets ignored {} | remote word fixes {} consumed unchosen {} slots taken out in held ticks {} | {}", frame,
                  m.rb_predicted, m.rb_mispredicted, m.rb_too_old, m.rb_not_ready, m.rb_no_base, m.rb_status_diff, m.rb_status_sample,
                  m.rb_local_corrections, m.rb_remote_corrections, m.rb_local_late, m.rb_local_ring_fix, m.pred_avg,
                  m.remote_pred_avg, m.ts_holds, delay, m.rb_gap_holds, m.rb_old_resends, m.rb_hole_repairs, m.rb_first_packs,
                  g_sync_guard_waits, g_sync_guard_skips, g_sync_guard_timeouts, m.rb_consume_fixes, m.rb_consume_fixes_resim,
                  m.rb_frame_end_kept, m.rb_stale_entries, m.rb_consume_fixes_remote, m.rb_unchosen_consumes, m.rb_foreign_slots, BurstLimitOnlinePeekStats());
    }
  }

  // 3. Send what the other PC hasn't confirmed (always at least the newest).
  if (m.local_player < 0 || m.newest_local < 0) {
    return;
  }
  // Rollback and a confirmed desync (here or on the other PC): the two games no longer agree and can't be put
  // back together - the match ends on both: no more inputs go out (only the flag), both fights wait, and the
  // game's own "connection lost" ends the session; a notice says why.
  const bool ending = rollback && m.desync_reported && REXCVAR_GET(online_desync_end);
  if (ending && !m.desync_notice) {
    m.desync_notice = true;
    REXLOG_WARN("[OnlineDesync] the match ends (inputs stop; the game leaves the session by itself)");
    BurstLimitOnlineGameNotice("Desync: the two games no longer match - this match has to end", 12.0);
    // on40: with only the inputs stopped the game's own traffic kept the session up - both fights crawled on
    // with frozen remote inputs to a time-out and stalled there. Dropping the game's own datagrams too makes
    // its session layer see a lost connection (the side channel, with the desync flag, still works).
    if (REXCVAR_GET(online_desync_block)) rex::net::online::BlockGameTraffic(true);
  }
  const int remote_player = 1 - m.local_player;
  // rollback: walk from well before the consume (a predicted, never-delivered frame there must be resent)
  const int32_t ack_from = rollback ? std::max(consume - 24, m.rb_ack_floor) : lowest;
  int32_t ack = ack_from - 1;
  for (int32_t g = std::max(ack_from, 0); g <= highest; ++g) {
    // rollback: the ring also holds our PREDICTIONS - acking those stopped the other PC from sending the real
    // entry and the misprediction was never seen (on20). Only entries the channel delivered count.
    const bool have = rollback ? m.remote_known[uint32_t(g) % m.remote_known.size()].frame == g
                               : RingHas(memory, manager, remote_player, g);
    if (!have) {
      break;
    }
    ack = g;
  }
  // The other PC's ack can't be above our newest frame in the same match (an
  // ack from before a reset is): then the whole window goes.
  int32_t from = m.newest_local - (kMaxWindow - 1);
  if (same_match && remote_ack <= m.newest_local) {
    from = std::max(from, std::min(remote_ack + 1, m.newest_local));
    // the other PC still misses frames older than one window (a loss burst while we ran ahead): every other packet
    // starts at the oldest it lacks - before, those frames were never sent again and it kept its predictions for
    // them (rb_burst: 8 frames never corrected) - as long as our ring still has them
    if (remote_ack + 1 < m.newest_local - (kMaxWindow - 1) && remote_ack + 1 > m.newest_local - int32_t(m.local.size()) + 1 &&
        (m.window_flip = !m.window_flip)) {
      from = std::max(remote_ack + 1, 0);
      ++m.rb_old_resends;
    }
  }
  if (m.resend_from >= 0) {
    if (m.resend_from >= m.newest_local - (kMaxWindow - 1)) from = std::min(from, m.resend_from);
    m.resend_from = -1;
  }
  from = std::max(from, 0);
  while (from <= m.newest_local && m.local[uint32_t(from) % m.local.size()].frame != from) {
    ++from;
  }
  std::array<uint8_t, kInputHeader + kMaxWindow * kInputEntry + 4> packet{};
  packet[0] = kInputVersion;
  packet[1] = uint8_t(m.local_player);
  packet[3] = 0;
  PutU32(&packet[4], uint32_t(ack));
  PutU32(&packet[8], uint32_t(from));
  PutU32(&packet[12], uint32_t(frame));
  if (m.desync_check && m.last_hashed >= 0) {
    // rollback: the hash of a final frame (kHashSettleFrames back), not of the newest (it may still change)
    const int32_t hf = rollback ? consume - kHashSettleFrames : m.last_hashed;
    const HashRecord& h = m.local_hashes[uint32_t(std::max(hf, 0)) % m.local_hashes.size()];
    if (hf >= 0 && h.frame == hf) {
      packet[3] |= kFlagHash;
      PutU32(&packet[16], uint32_t(h.frame));
      PutU32(&packet[20], h.hash);
    }
  }
  packet[24] = uint8_t(m.delay);
  // rollback time sync: our average prediction depth in quarter frames, flags bits 2-7 (bytes 25-27 are live delay's)
  if (rollback) packet[3] |= uint8_t(std::clamp(int(m.pred_avg * 4.0f + 0.5f), 0, 63) << 2);
  packet[25] = uint8_t((m.live_local ? 1 : 0) | (m.own_target << 4));
  packet[26] = uint8_t(m.own_effective);
  packet[27] = uint8_t(m.own_effective >> 8);
  int written = 0;
  if (ending) packet[3] |= kFlagDesync;
  for (int32_t g = from; !ending && g <= m.newest_local && written < kMaxWindow; ++g) {
    const InputEntry& e = m.local[uint32_t(g) % m.local.size()];
    if (e.frame != g) {
      static int holes = 0;  // a hole stops every later frame: the other PC runs "too far ahead" and waits (on41/on43)
      if (holes++ < 20)
        REXLOG_WARN("[OnlineInput] send window stops at a hole: local frame {} not recorded (newest local {}, from {})", g,
                    m.newest_local, from);
      break;  // keep the entries contiguous from `from`
    }
    uint8_t* p = &packet[kInputHeader + size_t(written) * kInputEntry];
    PutU64(p, e.command);
    PutU32(p + 8, e.status_a);
    PutU32(p + 12, e.status_b);
    ++written;
  }
  packet[2] = uint8_t(written);
  // build 136: the packet number, after the entries (never 0; one counter for the whole process, so a new match
  // never starts below what the other PC has stored)
  static uint32_t send_seq = 0;
  if (++send_seq == 0) ++send_seq;
  PutU32(&packet[kInputHeader + size_t(written) * kInputEntry], send_seq);
  if (rex::net::online::SendGameSide(kSideInput, packet.data(),
                                     kInputHeader + size_t(written) * kInputEntry + 4, 1)) {
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
  // Rollback, live tick: the local word just consumed must be the command recorded and SENT for that frame. After a
  // resim the game can pack a local frame again after the tick hook (with the pads of that moment) - the forced ring
  // slot is overwritten before the consume (r105_s75: the guest consumed 2008.. for frame 1086, the host had 0 -> the
  // fights split, desync at 1739). The consume's output is corrected here, as live delay corrects it for its ticks.
  // v0.8.1 R37: in resimulated ticks too (the game's network thread can put a history word into the ring between
  // the tick hook and the consume: r135_long, guest frames 4088 and 2020).
  if (RollbackMatch() && BurstLimitRb2Active() && m.local_player >= 0 && REXCVAR_GET(online_rollback_consume_fix) &&
      (!BurstLimitRb2InResim() || REXCVAR_GET(online_rollback_consume_fix_resim))) {
    const uint32_t mgr = LoadU32(memory, kInputManagerPointer);
    if (mgr && Readable(memory, mgr, 2 * kPlayerBlock + 64)) {
      const int32_t consumed = LoadS32(memory, mgr + kTickFrame) - (m.delay ? m.delay : 3);
      const InputEntry& le = m.local[uint32_t(std::max(consumed, 0)) % m.local.size()];
      const uint32_t w = mgr + kConsumedWords + 8 * uint32_t(m.local_player);
      if (consumed >= 0 && le.frame == consumed && LoadU64(memory, w) != le.command) {
        ++m.rb_consume_fixes;
        if (BurstLimitRb2InResim()) ++m.rb_consume_fixes_resim;
        if (m.rb_consume_fixes <= 40)
          REXLOG_WARN("[OnlineRollback] local frame {}: the game consumed {:X}, the sent command {:X} is used{}", consumed,
                      LoadU64(memory, w), le.command, BurstLimitRb2InResim() ? " (resim)" : "");
        StoreU64(memory, w, le.command);
      }
    }
  }
  // build 137: ... and the remote word is the entry this PC chose for the frame (see online_rollback_consume_fix_remote)
  if (RollbackMatch() && BurstLimitRb2Active() && m.local_player >= 0 && REXCVAR_GET(online_rollback_consume_fix_remote)) {
    const uint32_t mgr = LoadU32(memory, kInputManagerPointer);
    if (mgr && Readable(memory, mgr, 2 * kPlayerBlock + 64)) {
      const int32_t consumed = LoadS32(memory, mgr + kTickFrame) - (m.delay ? m.delay : 3);
      const int remote = 1 - m.local_player;
      const uint32_t w = mgr + kConsumedWords + 8 * uint32_t(remote);
      if (consumed >= 0) {
        const uint64_t used = LoadU64(memory, w);
        const InputEntry& rk = m.remote_known[uint32_t(consumed) % m.remote_known.size()];
        Match::Prediction& pr = m.predicted[uint32_t(consumed) % m.predicted.size()];
        if (rk.frame == consumed || pr.frame == consumed) {
          const uint64_t chosen = rk.frame == consumed ? rk.command : pr.command;
          if (used != chosen) {
            ++m.rb_consume_fixes_remote;
            if (m.rb_consume_fixes_remote <= 40)
              REXLOG_WARN("[OnlineRollback] remote frame {}: the game consumed {:X}, the {} command {:X} is used{}", consumed, used,
                          rk.frame == consumed ? "confirmed" : "predicted", chosen, BurstLimitRb2InResim() ? " (resim)" : "");
            StoreU64(memory, w, chosen);
          }
        } else {
          // nobody chose this frame's remote input (a slot filled between the hold decision and the consume): from
          // now on it is a prediction, compared with the real entry when that arrives
          const uint32_t slot = mgr + uint32_t(remote) * kPlayerBlock + (uint32_t(consumed) % kRingSlots) * kSlotSize;
          pr.frame = consumed;
          pr.command = used;
          pr.status_a = LoadU32(memory, slot + kSlotStatusA);
          pr.status_b = LoadU32(memory, slot + kSlotStatusB);
          pr.engine_frame = BurstLimitRb2CurrentFrame();
          pr.settled = false;
          if (consumed > g_rb_last_pred.load(std::memory_order_relaxed)) g_rb_last_pred.store(consumed, std::memory_order_relaxed);
          ++m.rb_unchosen_consumes;
          if (m.rb_unchosen_consumes <= 40)
            REXLOG_WARN("[OnlineRollback] remote frame {} was consumed without a confirmed entry or a prediction: {:X}/{}/{} - recorded as a prediction{}",
                        consumed, used, pr.status_a, pr.status_b, BurstLimitRb2InResim() ? " (resim)" : "");
        }
      }
    }
  }
  if (m.live_local && !BurstLimitRb2InResim()) {  // a resimulated consume is not a delivery (live delay's bookkeeping)
    const uint32_t tick_manager = LoadU32(memory, kInputManagerPointer);
    if (tick_manager) {
      const uint32_t words = tick_manager + kConsumedWords;
      if (m.tick == Match::Tick::kFiller) {
        StoreU64(memory, words, m.delivered[0]);
        StoreU64(memory, words + 8, m.delivered[1]);
        StoreU32(memory, kSyncEventFlag, 0);
        ++m.filler_ticks;
      } else {
        if (m.tick == Match::Tick::kMerge) {
          StoreU64(memory, words, m.merged[0]);
          StoreU64(memory, words + 8, m.merged[1]);
          StoreU32(memory, kSyncEventFlag, m.merged_flag);
          ++m.merged_ticks;
          m.merged_frames += uint64_t(m.merge_to - m.last_consumed);
        }
        m.last_consumed = LoadS32(memory, tick_manager + kTickFrame) - m.delay;
        for (int p = 0; p < 2; ++p) {
          const uint64_t word = LoadU64(memory, words + 8 * p);
          m.delivered_presses[p] += NewPresses(word, m.delivered[p]);
          m.delivered[p] = word;
        }
      }
      m.delivered_digest = Mix(Mix(Mix(m.delivered_digest, uint32_t(m.delivered[0] >> 32)),
                                   uint32_t(m.delivered[1] >> 32)),
                               LoadU32(memory, kSyncEventFlag));
      m.tick = Match::Tick::kNormal;
    }
  }
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
  // Keyed by the delay the match started with, so the same tick has the same
  // key on both PCs whatever the delay has become. (= the frame consumed until
  // the delay changes; rollback matches never change it.)
  const int32_t consumed =
      LoadS32(memory, manager + kTickFrame) - (m.start_delay ? m.start_delay : 3);
  {  // battle phase changes (round start / end flow), live frames only
    const uint32_t battle = LoadU32(memory, kBattlePointer);
    const uint32_t phase = Readable(memory, 0x841B5228, 4) ? LoadU32(memory, 0x841B5228) : 0;
    if (m.online && !BurstLimitRb2InResim() && (phase != m.last_phase || battle != m.last_battle)) {
      if (phase == 2 && m.last_phase != 2) m.engine_delay = m.delay;  // the fight delay (READY?'s switch has landed)
      REXLOG_INFO("[OnlineInput] battle phase {} -> {} at input frame {} (battle {:08X}, sync flag {})", m.last_phase, phase,
                  consumed, battle, LoadU32(memory, kInputManagerPointer - 4));
      m.last_phase = phase;
      m.last_battle = battle;
    }
  }
  if (consumed < 0 || (consumed == m.last_hashed && !RollbackMatch())) {
    return;
  }
  // Test (online_force_desync_frame): the host's fighter 0 is moved 0.5 along x whenever it consumes this input frame
  // (live or resimulated, so a rewind can't undo it) - the two PCs then disagree, to check the desync safety path
  // (detect, notice, the match ends on both). (on49: the old test added 0x100 to the ki word for 30 frames - its low
  // half is the MAXIMUM ki - and the game crashed.)
  if (const int force = REXCVAR_GET(online_force_desync_frame); force > 0 && consumed == force && m.local_player == 0) {
    const uint32_t player = LoadU32(memory, kPlayerTable);
    const uint32_t fighter = Readable(memory, player + kPlayerFighter, 4) ? LoadU32(memory, player + kPlayerFighter) : 0;
    if (fighter && Readable(memory, fighter + kFighterPosition, 4)) {
      const float x = rex::memory::load_and_swap<float>(memory->TranslateVirtual<uint8_t*>(fighter + kFighterPosition));
      rex::memory::store_and_swap<float>(memory->TranslateVirtual<uint8_t*>(fighter + kFighterPosition), x + 0.5f);
      BurstLimitRb2NoteHostStore(fighter + kFighterPosition, 4);
      REXLOG_WARN("[OnlineDesync] TEST: host fighter 0 moved +0.5 x at input frame {}{}", consumed,
                  BurstLimitRb2InResim() ? " (resim)" : "");
    }
  }
  // Test (online_test_full_ki = N): both fighters' ki is set to its maximum every N consumed frames of the fight, on
  // both PCs at the same input frames, live or resimulated (deterministic) - so supers, ultimates and
  // transformations can be fired by the test inputs.
  if (const int every = REXCVAR_GET(online_test_full_ki); every > 0 && consumed % every == 0 &&
      Readable(memory, 0x841B5228, 4) && LoadU32(memory, 0x841B5228) == 2) {
    for (int i = 0; i < 2; ++i) {
      const uint32_t player = LoadU32(memory, kPlayerTable + 4 * i);
      const uint32_t fighter = Readable(memory, player + kPlayerFighter, 4) ? LoadU32(memory, player + kPlayerFighter) : 0;
      if (fighter && Readable(memory, fighter + kFighterKi, 4)) {
        const uint32_t w = LoadU32(memory, fighter + kFighterKi);  // ki (high half) | maximum (low half)
        StoreU32(memory, fighter + kFighterKi, (w & 0xFFFFu) << 16 | (w & 0xFFFFu));
      }
    }
  }
  // rollback diagnostics: the local word the game consumed must be the one recorded and sent for that frame
  // (su_tr2: the guest consumed A at 4717 on the live tick right after a resim, the host got 0)
  if (RollbackMatch() && BurstLimitRb2Active() && m.local_player >= 0 && consumed >= 0 &&
      m.start_delay == m.delay) {
    const InputEntry& le = m.local[uint32_t(consumed) % m.local.size()];
    const uint64_t used = LoadU64(memory, manager + kConsumedWords + 8 * uint32_t(m.local_player));
    if (le.frame == consumed && le.command != used) {
      ++m.rb_local_used_mismatch;
      if (m.rb_local_used_mismatch <= 30) {
        const uint32_t slot = manager + uint32_t(m.local_player) * kPlayerBlock + (uint32_t(consumed) % kRingSlots) * kSlotSize;
        REXLOG_WARN("[OnlineRollback] local frame {} consumed {:X} but {:X} was recorded and sent ({}; ring slot frame {} cmd {:X}, "
                    "tick {}, newest local {})", consumed, used, le.command, BurstLimitRb2InResim() ? "resim" : "live",
                    LoadS32(memory, slot + kSlotFrame), LoadU64(memory, slot), LoadS32(memory, manager + kTickFrame), m.newest_local);
      }
    }
  }
  // rollback: every consumed frame is hashed (a wrong prediction is replayed and hashed again; hashes are
  // compared only once final, kHashSettleFrames back)
  HashRecord record;
  if (ComputeHash(memory, consumed, &record)) {
    if (m.last_hashed >= 0 &&
        m.local_hashes[uint32_t(m.last_hashed) % m.local_hashes.size()].hash != record.hash) {
      ++m.window.hash_changes;
      ++m.total.hash_changes;
    }
    m.local_hashes[uint32_t(consumed) % m.local_hashes.size()] = record;
    m.last_hashed = consumed;
    if (REXCVAR_GET(online_desync_dump) && m.rollback_match) CaptureDump(m, memory, consumed);  // review L5: rollback matches only
    const bool rb_on = RollbackMatch();
    if (REXCVAR_GET(online_hash_log)) {  // diagnostics: per frame, plus a wider local-only hash (small integers of
                                         // both player objects and fighters: HP / gauges) to find a divergence
      uint32_t wide = 2166136261u;
      for (int i = 0; i < 2; ++i) {
        const uint32_t player = LoadU32(memory, kPlayerTable + 4 * i);
        const uint32_t fighter = LoadU32(memory, player + kPlayerFighter);
        for (uint32_t base : {player, fighter}) {
          if (!Readable(memory, base, 0x800)) continue;
          for (uint32_t o = 0; o < 0x800; o += 4) {
            const uint32_t v = LoadU32(memory, base + o);
            if (v < 0x10000) wide = Mix(wide, v ^ (o << 16));
          }
        }
      }
      REXLOG_INFO("[OnlineHash] frame {} hash {:08X} wide {:08X} clock {} cmd {:X} {:X} ring {:X} {:X}{} rng {:08X} engine {} act {} {}", consumed, record.hash, wide, record.clock,
                  LoadU64(memory, manager + 1616), LoadU64(memory, manager + 1624),
                  LoadU64(memory, manager + (uint32_t(consumed) % kRingSlots) * kSlotSize),
                  LoadU64(memory, manager + kPlayerBlock + (uint32_t(consumed) % kRingSlots) * kSlotSize),
                  BurstLimitRb2InResim() ? " resim" : "", LoadU32(memory, 0x842F9334u), BurstLimitRb2CurrentFrame(), record.action[0], record.action[1]);
    }
    if (!rb_on) CompareHash(m, consumed);
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
  {  // diagnostics: a long hold says what the tick waits for (on41: both PCs held ~10 s near the round's end,
     // then the game dropped the session)
    const uint32_t mgr = LoadU32(memory, kInputManagerPointer);
    if (mgr && Readable(memory, mgr, 2 * kPlayerBlock + 64)) {
      const int32_t tick = LoadS32(memory, mgr + kTickFrame);
      const int delay = m.delay ? m.delay : 3;
      const int32_t consume = tick - delay;
      if (tick != m.diag_hold_tick) { m.diag_hold_tick = tick; m.diag_hold_n = 0; }
      if (++m.diag_hold_n == 60 || (m.diag_hold_n % 300) == 0) {
        auto slot_frame = [&](int player, int32_t f) {
          return LoadS32(memory, mgr + uint32_t(player) * kPlayerBlock + (uint32_t(f) % kRingSlots) * kSlotSize + kSlotFrame);
        };
        int32_t newest_known = -1;
        for (int32_t k = consume + 40; k >= std::max(0, consume - 60); --k) {
          if (m.remote_known[uint32_t(k) % m.remote_known.size()].frame == k) { newest_known = k; break; }
        }
        const int lp = m.local_player >= 0 ? m.local_player : 0;
        REXLOG_WARN("[OnlineHoldDiag] held {} ticks at tick {} (consume {}): ring slot frames local {} / remote {} (consume), "
                    "local {} / remote {} (tick) | remote newest known {} | local newest {} | local status A={} B={} | "
                    "rollback {} predict ok {} | status diff {} not ready {} too far ahead {} no base {} after desync {} | "
                    "remote read/write/highest {} {} {} | driver state {}",
                    m.diag_hold_n, tick, consume, slot_frame(lp, consume), slot_frame(1 - lp, consume), slot_frame(lp, tick),
                    slot_frame(1 - lp, tick), newest_known, m.newest_local, m.last_status_a, m.last_status_b,
                    RollbackMatch() && BurstLimitRb2Active(), BurstLimitRb2PredictOk(), m.rb_status_diff,
                    m.rb_not_ready, m.rb_too_old, m.rb_no_base, m.rb_after_desync,
                    LoadS32(memory, mgr + uint32_t(1 - lp) * kPlayerBlock + 768), LoadS32(memory, mgr + uint32_t(1 - lp) * kPlayerBlock + 772),
                    LoadS32(memory, mgr + uint32_t(1 - lp) * kPlayerBlock + 776), LoadU32(memory, mgr + kDriverState));
      }
    }
  }
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

// Oldest engine frame that consumed a still-unsettled prediction (-1: none) - the engine keeps such frames
// rewindable even when the fight pauses (KO / cinematic).
int64_t BurstLimitOnlineOldestUnsettled() {
  std::lock_guard<std::mutex> lock(g_match_mutex);
  int64_t oldest = -1;
  const int64_t now = BurstLimitRb2CurrentFrame();
  for (const auto& p : g_match.predicted) {
    if (p.frame < 0 || p.settled || p.engine_frame < now - 10 || p.engine_frame > now) continue;
    if (oldest < 0 || p.engine_frame < oldest) oldest = p.engine_frame;
  }
  return oldest;
}

// End of a LIVE frame (engine FrameF hook, never in a resim): any local frame the game packed this frame that the
// driver hook missed (packed after it) is recorded now - later, a resim tick would overwrite the history slot with
// the pads of that moment (on16: the guest recorded and sent 0 for a frame it had played with a real command).
void BurstLimitOnlineLiveFrameEnd() {
  if (!RollbackMatch()) return;
  auto* memory = GuestMemory();
  if (!memory) return;
  std::lock_guard<std::mutex> lock(g_match_mutex);
  Match& m = g_match;
  if (!m.channel || m.local_player < 0) return;
  const uint32_t manager = LoadU32(memory, kInputManagerPointer);
  if (!Readable(memory, manager, 2 * kPlayerBlock + 64)) return;
  const int32_t frame = LoadS32(memory, manager + kTickFrame);
  // The frame of this live tick: our recorded (and sent) entry must equal the ring slot our game consumes;
  // if the game wrote it after the hook, correct it and send it again.
  {
    const int32_t f = frame;
    InputEntry& e = m.local[uint32_t(std::max(f, 0)) % m.local.size()];
    const uint32_t slot = manager + uint32_t(m.local_player) * kPlayerBlock + (uint32_t(std::max(f, 0)) % kRingSlots) * kSlotSize;
    if (f >= 0 && e.frame == f && LoadS32(memory, slot + kSlotFrame) == f) {
      const uint64_t cmd = LoadU64(memory, slot);
      if (cmd != e.command && REXCVAR_GET(online_rollback_frame_end_fix)) {
        ++m.rb_local_ring_fix;
        if (m.rb_local_ring_fix <= 100)
          REXLOG_INFO("[OnlineRollback] local frame {}: recorded {:X}, ring {:X} - corrected", f, e.command, cmd);
        e.command = cmd;
        if (m.resend_from < 0 || f < m.resend_from) m.resend_from = f;
      } else if (cmd != e.command) {
        // v0.8.1 R36a: the recorded entry was sent (it may be consumed on the other PC already); the ring's word is
        // a later pack of the same frame (pads of that moment, put there by a resim tick or by the game's network
        // thread). The sent command stays; the ring gets it back, and the consume takes it in any case.
        ++m.rb_frame_end_kept;
        if (m.rb_frame_end_kept <= 40)
          REXLOG_INFO("[OnlineRollback] local frame {}: ring {:X} at the frame's end, the sent command {:X} stays", f, cmd, e.command);
        ForceRing(memory, manager, m.local_player, f, e.command, e.status_a, e.status_b);
      }
    }
  }
  for (uint32_t index = 0; index < kHistorySlots; ++index) {
    const int32_t f = LoadS32(memory, kHistory + 4 * index);
    if (f < 0 || f != frame || m.local[uint32_t(f) % m.local.size()].frame == f) continue;
    if (int(LoadU32(memory, kHistory + 96 + 4 * index)) != m.local_player) continue;
    InputEntry entry;
    entry.frame = f;
    entry.command = LoadU64(memory, kHistory + 208 + 8 * index);
    entry.status_a = LoadU32(memory, kHistory + 144 + 4 * index);
    entry.status_b = LoadU32(memory, kHistory + 48 + 4 * index);
    const uint32_t rslot = manager + uint32_t(m.local_player) * kPlayerBlock + (uint32_t(f) % kRingSlots) * kSlotSize;
    if (LoadS32(memory, rslot + kSlotFrame) == f) entry.command = LoadU64(memory, rslot);  // what our game consumes
    m.local[uint32_t(f) % m.local.size()] = entry;
    m.newest_local = std::max(m.newest_local, f);
    ++m.rb_local_late;
  }
}

// The rollback engine runs in lobby matches whose netcode is rollback (burstlimit_rb2.cpp).
bool BurstLimitRollback2Online() {
  return RollbackMatch() && g_rb2_match.load(std::memory_order_relaxed);
}

// Diagnostics (rollback3): the game's synchronized-event requests (sub_82292318 / sub_82292328, the round
// flow's "both PCs at frame N" handshake) - who asks and when.
REX_EXTERN(__imp__sub_82292318);
REX_EXTERN(__imp__sub_82292328);
// the guest call chain above a leaf call: back chain [sp], each frame's saved LR at [its caller's sp - 8]
REXCVAR_DEFINE_BOOL(online_sync_log_chain, false, "Debug", "Online diagnostics: the sync-event log lines also walk the guest call chain (crashed the game once: off)");
std::string GuestChain(PPCContext& ctx, uint8_t* base) {
  std::string out;
  if (!REXCVAR_GET(online_sync_log_chain)) return out;
  uint32_t sp = ctx.r1.u32;
  // MEASURED 2026-11-12: walking past the outermost frame read the stack's end (0x70190000, not committed) and
  // crashed the main thread in the round flow (t10) - only frames well inside the stack are read now
  auto* mem = rex::Runtime::instance() ? rex::Runtime::instance()->memory() : nullptr;
  auto committed = [&](uint32_t a) {
    if (!mem) return false;
    auto* heap = mem->LookupHeap(a);
    rex::memory::HeapAllocationInfo info = {};
    return heap && heap->QueryRegionInfo(a, &info) && (info.state & rex::memory::kMemoryAllocationCommit) &&
           uint64_t(a) + 8 <= uint64_t(info.base_address) + info.region_size;
  };
  for (int k = 0; k < 6 && sp >= 0x70000000u && sp < 0x80000000u && committed(sp); ++k) {
    const uint32_t prev = rex::memory::load_and_swap<uint32_t>(base + sp);
    if (prev <= sp || prev >= 0x80000000u || !committed(prev) || !committed(prev - 8)) break;
    out += fmt::format(" < {:08X}", rex::memory::load_and_swap<uint32_t>(base + prev - 8));
    sp = prev;
  }
  return out;
}
// The rollback engine runs only at the delay the fight started with (a delay change lands only while it is paused).
bool BurstLimitOnlineEngineDelayOk() {
  if (!RollbackMatch()) return true;
  std::lock_guard<std::mutex> lock(g_match_mutex);
  const Match& m = g_match;
  if (!m.rollback_match || !m.auto_phase || m.engine_delay <= 0) return true;
  return m.delay == m.engine_delay;
}
// Rollback: the request (sub_82292318 -> sub_82298698) sends the game's own handshake message to the other PC at
// once - the message can't be taken back, while a rewind does put back the game's own state of it (main-thread
// memory). Made in a frame played with a predicted input that turns out wrong, the request is a GHOST: long2 match 3,
// the guest asked at input frame 3147 in a mispredicted frame, the rewind's frames didn't ask again, and 10 s later
// the guest's game ended the session ("Connection with other player has been lost"). So in a rollback match the
// request waits (main thread, up to 1.5 s) until every prediction this frame was played with is confirmed: all
// right -> it goes out; one wrong -> the frame is rewound right after, it is skipped (the resimulated frame asks
// again if the corrected frames still do). The caller ignores the result.
REXCVAR_DEFINE_BOOL(online_rollback_sync_guard, true, "Patches",
                    "Online rollback: the game's synchronized-event requests go out only from frames whose predicted "
                    "inputs are confirmed (a request from a wrongly guessed frame made the other PC's game time out)");
int64_t BurstLimitRb2LoopPeekRewind();
// v0.8.1 R11: on a time-out the request used to go out anyway ("asked anyway") - from a frame that could still be
// rewound: the ghost again. Now the wait is shorter (400 ms: under the hang watcher's limit) and on a time-out the
// request is SKIPPED and the frame is rewound (the resimulated frame asks again, and waits again if its inputs are
// still missing) - the same path as "one prediction was wrong".
REXCVAR_DEFINE_INT32(online_rollback_sync_guard_timeout_ms, 400, "Patches",
                     "Online rollback: how long a synchronized-event request waits for its frame's predictions (v0.8.0: 1500)");
REXCVAR_DEFINE_BOOL(online_rollback_sync_guard_skip_on_timeout, true, "Patches",
                    "Online rollback: a synchronized-event request whose frame is still unconfirmed after the wait is skipped and "
                    "its frame rewound (off = v0.8.0: asked anyway)");
REXCVAR_DEFINE_BOOL(online_rollback_sync_guard_skip_check, true, "Patches",
                    "Online rollback (review L3): a timed-out synchronized-event request is skipped only if its frame's rewind can "
                    "be taken (settled, logs kept); otherwise it is asked anyway (off = builds up to 152: skipped in any case)");
bool BurstLimitRb2RewindPossible(int64_t engine_frame);
// Round 6: the time-out path was never reached in a test (0 time-outs even with loss, bursts and 500 ms spikes),
// so a test switch forces it: every N-th LIVE request that finds unconfirmed predictions "times out" at once.
REXCVAR_DEFINE_INT32(online_rollback_sync_guard_test, 0, "Debug",
                     "Online rollback test (R11): every N-th synchronized-event request of a live frame with unconfirmed "
                     "predictions is treated as timed out at once (0 off; one PC or both)");
static std::atomic<uint32_t> g_sync_guard_test_seen{0}, g_sync_guard_test_forced{0};
// 1: the frame's predictions are all confirmed (or none / not a rollback match), 0: one was wrong, -1: timed out
int SyncGuard(double* waited_ms) {
  *waited_ms = 0;
  if (!REXCVAR_GET(online_rollback_sync_guard) || !RollbackMatch() || !BurstLimitRb2Active()) return 1;
  const auto t0 = std::chrono::steady_clock::now();
  bool first_pass = true;
  for (;;) {
    bool unsettled = false;
    const int64_t now = BurstLimitRb2CurrentFrame();
    {
      std::lock_guard<std::mutex> lock(g_match_mutex);
      Match& m = g_match;
      // the entries received since the last tick go through the rollback bookkeeping now (settle predictions, ask
      // for rewinds); the next tick hands them to it again, which changes nothing (known / settled already)
      std::vector<RemoteEntry> peek;
      {
        std::lock_guard<std::mutex> inbox_lock(g_inbox.mutex);
        peek = g_inbox.entries;
      }
      for (const RemoteEntry& e : peek)
        if (m.local_player >= 0 && e.player != m.local_player && e.frame >= 0) RollbackReceivedLocked(m, e);
      for (const auto& p : m.predicted)
        if (p.frame >= 0 && !p.settled && p.engine_frame <= now && p.engine_frame >= now - 64) { unsettled = true; break; }
    }
    const int64_t to = BurstLimitRb2LoopPeekRewind();
    *waited_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    if (to >= 0 && to <= now) return 0;
    if (!unsettled) return 1;
    if (first_pass) {
      first_pass = false;
      if (const int every = REXCVAR_GET(online_rollback_sync_guard_test); every > 0 && !BurstLimitRb2InResim() &&
          (g_sync_guard_test_seen++ % uint32_t(every)) == 0) {
        REXLOG_WARN("[OnlineInput] TEST: sync-event guard time-out forced at engine frame {} ({} forced so far)", now,
                    ++g_sync_guard_test_forced);
        return -1;
      }
    }
    if (*waited_ms > double(std::clamp(REXCVAR_GET(online_rollback_sync_guard_timeout_ms), 50, 5000))) return -1;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}
// v0.8.1 (plan_v81.md R5): the transformation (sub_82195AC0 state 0, call at 0x82195BC8) asks for the synchronized
// event on its FIRST frame. The other PC ignores that request (message 15 subtype 1 is stored at net+1732, which
// nothing reads), but status A = 1 then travels in every input frame for the 83-231 frames of the transformation,
// and no input is predicted while a status A is non-zero: the whole fight ran in lockstep at delay 2 (47 % speed at
// 150 ms, 40 % at 200 ms, and from ~230 ms the game's own 10 s watchdog ended the session). State 2 of the
// transformation negotiates the event by itself (sub_82292328, 0 -> 1 -> 2 -> 0 in about 4 frames - the path the
// second fighter of a double transformation always took). So in a rollback match the early request is not made.
// Deterministic on both PCs: the return address and the match's netcode. Synced cvar (it changes the status words).
REXCVAR_DEFINE_BOOL(online_rollback_skip_early_sync, true, "Patches",
                    "Online rollback: a transformation does not ask for the synchronized event on its first frame "
                    "(the fight ran in lockstep - slow motion - for the whole transformation); off = v0.8.0 behaviour");
extern "C" REX_FUNC(sub_82292318) {
  if (uint32_t(ctx.lr) == 0x82195BCCu && RollbackMatch() && REXCVAR_GET(online_rollback_skip_early_sync)) {
    const uint32_t n = ++g_early_sync_skipped;
    if (n <= 8 || (n & (n - 1)) == 0)
      REXLOG_INFO("[OnlineInput] transformation: early sync request skipped (fighter r31={:X}, engine frame {}{}; {} so far)",
                  ctx.r31.u32, BurstLimitRb2CurrentFrame(), BurstLimitRb2InResim() ? " resim" : "", n);
    return;
  }
  double waited = 0;
  int guard = SyncGuard(&waited);
  if (waited > 0.5) ++g_sync_guard_waits;
  if (guard == 0) ++g_sync_guard_skips;
  if (guard < 0) ++g_sync_guard_timeouts;
  bool timeout_skip = false;
  // R11: not "asked anyway" - but only when the rewind can be taken (review L3: a refused rewind lost the request)
  if (guard < 0 && REXCVAR_GET(online_rollback_sync_guard_skip_on_timeout) &&
      (!REXCVAR_GET(online_rollback_sync_guard_skip_check) || BurstLimitRb2RewindPossible(BurstLimitRb2CurrentFrame()))) {
    timeout_skip = true;
    BurstLimitRb2RequestRewind(BurstLimitRb2CurrentFrame());
  }
  if (guard < 0)
    BurstLimitOnlineAlert("sync guard time-out", fmt::format("a synchronized event waited {:.0f} ms for its frame's predictions ({}) at engine "
                                                             "frame {}", waited, timeout_skip ? "skipped, rewind requested" : "asked anyway",
                                                             BurstLimitRb2CurrentFrame()));
  REXLOG_INFO("[OnlineInput] sync event call 82292318 r30={:X} r31={:X} lr={:08X}{}{}{}", ctx.r30.u32, ctx.r31.u32, uint32_t(ctx.lr),
              GuestChain(ctx, base), BurstLimitRb2InResim() ? " (resim)" : "",
              guard == 0 ? fmt::format(" - SKIPPED: a prediction of this frame was wrong (waited {:.1f} ms; the rewind asks again)", waited)
              : timeout_skip ? fmt::format(" - SKIPPED after {:.0f} ms: predictions not confirmed, rewind requested (the frame asks again)", waited)
              : guard < 0 ? fmt::format(" - predictions not confirmed after {:.0f} ms, asked anyway", waited)
              : waited > 0.5 ? fmt::format(" - waited {:.1f} ms for this frame's predictions", waited) : std::string());
  if (guard != 0 && !timeout_skip) __imp__sub_82292318(ctx, base);
}
void BurstLimitRb2NoteFxSync();
extern "C" REX_FUNC(sub_82292328) {
  if (uint32_t(ctx.lr) == 0x8214E5B4u) BurstLimitRb2NoteFxSync();
  REXLOG_INFO("[OnlineInput] sync event call 82292328 r3={:X} r4={:X} lr={:08X}{}{}", ctx.r3.u32, ctx.r4.u32, uint32_t(ctx.lr),
              GuestChain(ctx, base), BurstLimitRb2InResim() ? " (resim)" : "");
  __imp__sub_82292328(ctx, base);
}
// v0.8.1: the transformation's state 2 polls sub_82180BB0 ("the new form's resources are ready") from 0x82195C24;
// on "ready" it asks for the synchronized event (status A = 1) - one log line per transformation with the polls
// that answered "not ready".
//
// The ready gate (rb2_ready_gate). The answer follows the loader and the render thread, not the fight: a resimulated
// frame saw "ready" where the same frame had seen "not ready" when it ran, the status words of frames already sent
// changed ("local status of frame N corrected by a resim"), both PCs corrected each other with rewinds up to 24
// deep, and the event ran on one PC only (MEASURED r125_res2: desync at input frame 999, p0 action 1262 on the
// host, 1261 on the guest; 26 / 33 remote corrections against 0 in every good run). With the gate
//   - a resimulated frame gets the answer the frame got when it ran live ("not ready" if it did not ask then);
//   - the first "ready" of a transformation is given only in a live frame with no predicted input unsettled and
//     no rewind pending; until then the answer is "not ready" (the game's own wait) and no prediction is made
//     (BurstLimitRb2ReadyHold), so that frame is never rewound over. From it on the status words hold predictions.
REXCVAR_DEFINE_INT32(rb2_ready_gap, 60, "Debug",
                     "Rollback: engine frames without a \"resources ready\" poll of a fighter after which its next poll counts as a "
                     "new transformation and the ready gate applies again (0 = builds up to 134: only a \"not ready\" poll closed it)");
REXCVAR_DEFINE_BOOL(rb2_ready_gate, true, "Debug",
                    "Rollback: the transformation's \"resources ready\" answer is the same in a resimulated frame as it was "
                    "live, and its first \"ready\" waits for a frame no rewind can cross (off = build 125)");
bool BurstLimitRb2Unsettled();
void BurstLimitRb2ReadyHold();
void BurstLimitRb2ReadyHoldEnd();
namespace {
struct ReadyRec { int64_t frame = -1; uint32_t who = 0; uint8_t ready = 0; };
ReadyRec g_ready_rec[128];
uint32_t g_ready_next = 0;
int64_t g_ready_last_live = -1;
uint32_t g_ready_open[2] = {0, 0};  // the fighters whose "ready" has been given
int64_t g_ready_open_frame[2] = {-1000, -1000};  // ... and the last live engine frame in which each of them polled
uint64_t g_ready_reclosed = 0;
int g_ready_deferred_run = 0;
uint64_t g_ready_deferred = 0, g_ready_forced = 0, g_ready_replayed = 0, g_ready_replay_changed = 0;
}  // namespace
std::string BurstLimitReadyGateStats() {
  return fmt::format("ready gate: first \"ready\" deferred {} polls (forced {}), resim polls answered {} ({} differed from the live state), "
                     "closed again for a new transformation {}",
                     g_ready_deferred, g_ready_forced, g_ready_replayed, g_ready_replay_changed, g_ready_reclosed);
}
REX_EXTERN(__imp__sub_82180BB0);
extern "C" REX_FUNC(sub_82180BB0) {
  const uint32_t lr = uint32_t(ctx.lr), who = ctx.r3.u32;
  __imp__sub_82180BB0(ctx, base);
  if (lr != 0x82195C28u) return;
  if (REXCVAR_GET(rb2_ready_gate) && BurstLimitRb2Active()) {
    const int64_t frame = BurstLimitRb2CurrentFrame();
    const bool real = (ctx.r3.u32 & 0xFF) != 0;
    bool ready = real;
    if (BurstLimitRb2InResim()) {
      ready = false;
      for (const ReadyRec& r : g_ready_rec) {
        if (r.frame == frame && r.who == who) { ready = r.ready != 0; break; }
      }
      ++g_ready_replayed;
      if (ready != real) ++g_ready_replay_changed;
    } else {
      if (frame < g_ready_last_live) {  // a new fight: the engine's frames start again
        for (ReadyRec& r : g_ready_rec) r = ReadyRec{};
        g_ready_open[0] = g_ready_open[1] = 0;
        g_ready_open_frame[0] = g_ready_open_frame[1] = -1000;
      }
      g_ready_last_live = frame;
      // v0.8.1 R36: "open" used to end only at a poll that answered "not ready". A fighter's NEXT transformation whose
      // resources are ready at its first poll (Frieza / Cell: the next form is loaded already) then found the gate
      // open from the one before, got "ready" in a frame with predictions unsettled, and the two PCs corrected each
      // other's status words until the game's 10 s watchdog closed the session (r134_long, input frame 1563: "after 0
      // not-ready polls", 222 event calls, 22 / 20 corrections). The state-2 poll runs every few engine frames while
      // one transformation negotiates (gaps of up to 8 seen), and the next transformation of the same fighter is
      // hundreds of frames away: a gap of more than rb2_ready_gap frames closes the gate again.
      int slot = g_ready_open[0] == who ? 0 : g_ready_open[1] == who ? 1 : -1;
      if (slot >= 0 && REXCVAR_GET(rb2_ready_gap) > 0 && frame - g_ready_open_frame[slot] > REXCVAR_GET(rb2_ready_gap)) {
        g_ready_open[slot] = 0;
        slot = -1;
        const uint64_t n = ++g_ready_reclosed;
        if (n <= 8 || (n & (n - 1)) == 0)
          REXLOG_INFO("[OnlineInput] ready gate: closed again for object {:08X} (a new transformation, engine frame {}; {} so far)", who, frame, n);
      }
      const bool open = slot >= 0;
      if (open) g_ready_open_frame[slot] = frame;
      if (!real) {
        if (g_ready_open[0] == who) g_ready_open[0] = 0;  // the next transformation of this fighter starts closed
        if (g_ready_open[1] == who) g_ready_open[1] = 0;
      } else if (!open) {
        if (BurstLimitRb2Unsettled() && g_ready_deferred_run < 90) {
          ready = false;
          ++g_ready_deferred_run;
          ++g_ready_deferred;
          BurstLimitRb2ReadyHold();
        } else {
          if (g_ready_deferred_run >= 90) {
            ++g_ready_forced;
            REXLOG_WARN("[OnlineInput] ready gate: \"ready\" given after {} deferred polls with predictions still unsettled (engine frame {})",
                        g_ready_deferred_run, frame);
          }
          g_ready_deferred_run = 0;
          BurstLimitRb2ReadyHoldEnd();
          const int s2 = g_ready_open[0] == 0 ? 0 : 1;
          g_ready_open[s2] = who;
          g_ready_open_frame[s2] = frame;
        }
      }
      g_ready_rec[g_ready_next++ % 128] = ReadyRec{frame, who, uint8_t(ready ? 1 : 0)};
    }
    if (!ready) ctx.r3.u64 = 0;
    else if (!real) ctx.r3.u64 = 1;
  }
  static uint32_t not_ready[2] = {0, 0}, last_who[2] = {0, 0};
  const int slot = (last_who[0] == who || last_who[0] == 0) ? 0 : 1;
  last_who[slot] = who;
  if ((ctx.r3.u32 & 0xFF) == 0) {
    ++not_ready[slot];
    return;
  }
  if (not_ready[slot] || !BurstLimitRb2InResim())
    REXLOG_INFO("[OnlineInput] transformation resources ready (object {:08X}) after {} not-ready polls, engine frame {}{}", who,
                not_ready[slot], BurstLimitRb2CurrentFrame(), BurstLimitRb2InResim() ? " resim" : "");
  not_ready[slot] = 0;
}
// Diagnostics (rollback3): the round flow's "resources ready" check before its sync step (sub_82140CF8 ->
// sub_8213CD68: the round's sound loads, via the sound manager's sub_8210D3E8, and sub_82141998) - logged when
// its answer changes and every 300 calls while it says no.
REX_EXTERN(__imp__sub_8213CD68);
extern "C" REX_FUNC(sub_8213CD68) {
  __imp__sub_8213CD68(ctx, base);
  static uint32_t calls = 0, last = 2;
  const uint32_t ready = ctx.r3.u32;
  ++calls;
  if (ready == last && !(ready == 0 && calls % 300 == 0)) return;
  last = ready;
  auto u32 = [&](uint32_t a) { return a ? rex::memory::load_and_swap<uint32_t>(base + a) : 0u; };
  const uint32_t obj = u32(0x8414C730);
  std::string s;
  auto entry = [&](uint32_t p, uint32_t handle_off, uint32_t flag_off) {
    if (!p) return;
    s += fmt::format(" {:08X}:{}/{}", p, int32_t(u32(p + handle_off)), base[p + flag_off]);
  };
  if (obj) {
    entry(u32(obj + 172), 76, 80);
    entry(u32(obj + 304), 76, 80);
    for (uint32_t k = 0; k < 32; ++k) entry(u32(obj + 176 + 4 * k), 80, 84);
  }
  // sub_82141910 / sub_82141998: one more load, picked by the byte at 0x841570BA (32-byte table at 0x841572E0)
  const uint32_t pick = base[0x841570BA];
  s += fmt::format(" | pick {}", pick);
  entry(u32(0x841572E0 + pick * 32), 76, 80);
  REXLOG_INFO("[OnlineInput] round resources ready={} (call {}, lr {:08X}) loads (handle/done):{}", ready, calls,
              uint32_t(ctx.lr), s);
}

// Debug command: online_delay up | down | <2-8>: like the keys / the pad combo.
static void OnlineDelayCommand(std::string_view args) {
  const std::string arg(args);
  if (arg.empty()) {
    REXLOG_INFO("online_delay: {} frames now", g_delay.load());
  } else if (arg == "up") {
    BurstLimitOnlineDelayStep(1);
  } else if (arg == "down") {
    BurstLimitOnlineDelayStep(-1);
  } else {
    BurstLimitOnlineDelaySet(std::clamp(std::atoi(arg.c_str()), kLiveMinDelay, kLiveMaxDelay));
  }
}
REXCVAR_DEFINE_COMMAND_ARGS(online_delay, OnlineDelayCommand, "Debug",
                            "The input delay of the running online match: up | down | <2-8>");
