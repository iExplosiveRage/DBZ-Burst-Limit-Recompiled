// Player Match room rules: the battle settings of offline Versus' stage select
// (Player 1 Settings / CPU Settings) for online rooms of the lobby.
//
// When a guest joins a Player Match room, the host gets a rules menu in the
// look of those panels: each player's Health (1-8 bars, 1000 HP each), Attack
// and Defense (1-5; 3 = normal), the time limit (60 / 90 / 120 / 180 /
// infinite), the rounds (1-5) and the opening / ending cut-scenes. The guest
// sees "the host is choosing" and then the chosen rules. The host's choice
// goes to the guest over the peer link (side-channel kind 18) and the host's
// menu stays open until the guest acknowledges it, so the host can't get to a
// battle before both PCs have the same rules. Right before each battle is set
// up (sub_8217AEA8), both PCs write them into the match request. The last
// rules a host picked are its defaults next time (online_rules_* in the
// toml). Ranked keeps the game's rules and shows no menu; so does the Radmin /
// LAN way (no lobby).
//
// Messages (kind 18, body[0] = version 1, body[1] = type):
//   1 REQUEST guest -> host: +2 u32 nonce (new per session), +6 name (16)
//   2 RULES   host -> guest: +2 u32 nonce, +6 seq, +7 rules (kRulesBytes), +16 host name (16)
//   3 ACK     guest -> host: +2 u32 nonce, +6 seq
//   4 MENU    host -> guest: +2 u32 nonce - the host's menu is open (every
//             300 ms while it is; the guest's "choosing" panel shows only then)

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <vector>

#include <fmt/format.h>
#include <imgui.h>

#include <rex/cvar.h>
#include <rex/input/input_system.h>
#include <rex/logging.h>
#include <rex/memory.h>
#include <rex/memory/utils.h>
#include <rex/net/online.h>
#include <rex/net/session.h>
#include <rex/ppc/context.h>
#include <rex/runtime.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/immediate_drawer.h>
#include <rex/ui/overlay/overlay_text.h>

#include "burstlimit_menuart.h"

// The host's last rules (its defaults for the next guest). The time limit and
// rounds are the room's own (the game's Create Match pages; the menu's choice
// is written back there, so the room list and View Battle Rules show it).
REXCVAR_DEFINE_INT32(online_rules_host_health, 3, "Online",
                     "Player Match rooms you host: your Health (1-8 bars), the last you picked")
    .range(1, 8);
REXCVAR_DEFINE_INT32(online_rules_host_attack, 3, "Online",
                     "Player Match rooms you host: your Attack (1-5), the last you picked")
    .range(1, 5);
REXCVAR_DEFINE_INT32(online_rules_host_defense, 3, "Online",
                     "Player Match rooms you host: your Defense (1-5), the last you picked")
    .range(1, 5);
REXCVAR_DEFINE_INT32(online_rules_guest_health, 3, "Online",
                     "Player Match rooms you host: the guest's Health (1-8 bars), the last you "
                     "picked")
    .range(1, 8);
REXCVAR_DEFINE_INT32(online_rules_guest_attack, 3, "Online",
                     "Player Match rooms you host: the guest's Attack (1-5), the last you picked")
    .range(1, 5);
REXCVAR_DEFINE_INT32(online_rules_guest_defense, 3, "Online",
                     "Player Match rooms you host: the guest's Defense (1-5), the last you picked")
    .range(1, 5);
REXCVAR_DEFINE_STRING(online_rules_cutscenes, "room", "Online",
                      "Player Match rooms you host: the opening / ending cut-scenes you last "
                      "picked")
    .allowed({"room", "on", "off"});

REXCVAR_DEFINE_BOOL(online_rules_test_ignore, false, "Debug",
                    "Test: the guest ignores the host's room rules (as if they never arrived)");

namespace {

// --------------------------------------------------------------- the game

// The match request ([0x841B5138], 216 bytes; the battle set-up sub_8217AEA8
// reads it, sub_82177740 copies it into the battle): 0x50 bytes per player -
// +44 / +46 u16 health / max health, +56 u32 attack, +60 u16 defense (index
// into the damage table 0x5 floats: 1 = x0.5, 2 = x0.75, 3 = x1, 4 = x1.25,
// 5 = x1.5; online leaves 0 = x1) - then +196 mode, +206 u16 time limit in
// frames (0xFFFF = infinite), +208 u16 rounds, +214 u16 opening / ending
// cut-scenes.
constexpr uint32_t kRequestPointer = 0x841B5138;
constexpr uint32_t kRequestSize = 216;
constexpr uint32_t kRequestStride = 0x50;
constexpr uint32_t kRequestHealth = 44;
constexpr uint32_t kRequestHealthMax = 46;
constexpr uint32_t kRequestAttack = 56;
constexpr uint32_t kRequestDefense = 60;
constexpr uint32_t kRequestMode = 196;
constexpr uint32_t kRequestTime = 206;
constexpr uint32_t kRequestRounds = 208;
constexpr uint32_t kRequestCutscenes = 214;
constexpr uint16_t kTimeInfinite = 0xFFFF;
// The match input manager M ([0x841B5EA4]): +1632 driver state, 3 = a battle runs.
constexpr uint32_t kInputManagerPointer = 0x841B5EA4;
constexpr uint32_t kDriverState = 1632;
constexpr uint32_t kDriverStateMatch = 3;
// The net manager N: +1408 X_CONTEXT_GAME_TYPE (0 Ranked, 1 Player Match),
// +1416 the room's rounds (0-4 = 1-5), +1420 its time limit (0-3 = 60-180).
constexpr uint32_t kNetManagerPointer = 0x84255714;
constexpr uint32_t kNetGameType = 1408;
constexpr uint32_t kNetRounds = 1416;
constexpr uint32_t kNetTime = 1420;
// The game's online session block: +28 this console's player + 1 (1 host, 2 guest).
// Its room settings records (sub_82212958 makes this console's at +816 and
// copies it to +32; the Session Lobby's View Battle Rules and the request
// builder sub_822120A0 read the one at +32): +152 time limit index (0-3 =
// 60-180), +153 rounds - 1, +154 Drama Pieces.
constexpr uint32_t kNetSession = 0x84245E50;
constexpr uint32_t kNetSessionLocalPlayer = 28;
constexpr uint32_t kNetSessionRules[2] = {32, 816};
constexpr uint32_t kRulesTime = 152;
constexpr uint32_t kRulesRounds = 153;

constexpr uint8_t kSideKind = 18;
constexpr uint8_t kVersion = 1;
constexpr uint8_t kTypeRequest = 1;
constexpr uint8_t kTypeRules = 2;
constexpr uint8_t kTypeAck = 3;
constexpr uint8_t kTypeMenu = 4;
constexpr size_t kNameBytes = 16;
constexpr int kTimeValues[4] = {60, 90, 120, 180};
constexpr int kTimeChoices = 5;  // + infinite

struct Rules {
  uint8_t health[2] = {3, 3};
  uint8_t attack[2] = {3, 3};
  uint8_t defense[2] = {3, 3};
  uint8_t time = 1;  // 0-3 = 60-180 s, 4 = infinite
  uint8_t rounds = 2;
  uint8_t cutscenes = 1;
};
constexpr size_t kRulesBytes = 9;

void PackRules(const Rules& r, uint8_t* out) {
  out[0] = r.health[0];
  out[1] = r.health[1];
  out[2] = r.attack[0];
  out[3] = r.attack[1];
  out[4] = r.defense[0];
  out[5] = r.defense[1];
  out[6] = r.time;
  out[7] = r.rounds;
  out[8] = r.cutscenes;
}

bool UnpackRules(const uint8_t* in, Rules* r) {
  Rules v;
  v.health[0] = in[0];
  v.health[1] = in[1];
  v.attack[0] = in[2];
  v.attack[1] = in[3];
  v.defense[0] = in[4];
  v.defense[1] = in[5];
  v.time = in[6];
  v.rounds = in[7];
  v.cutscenes = in[8];
  for (int p = 0; p < 2; ++p) {
    if (v.health[p] < 1 || v.health[p] > 8 || v.attack[p] < 1 || v.attack[p] > 5 ||
        v.defense[p] < 1 || v.defense[p] > 5) {
      return false;
    }
  }
  if (v.time >= kTimeChoices || v.rounds < 1 || v.rounds > 5 || v.cutscenes > 1) {
    return false;
  }
  *r = v;
  return true;
}

std::string TimeText(uint8_t time) {
  return time < 4 ? std::to_string(kTimeValues[time]) : std::string("Infinite");
}

std::string DescribeRules(const Rules& r) {
  return fmt::format(
      "P1 health {} attack {} defense {} | P2 health {} attack {} defense {} | time {} | rounds "
      "{} | cut-scenes {}",
      r.health[0], r.attack[0], r.defense[0], r.health[1], r.attack[1], r.defense[1],
      TimeText(r.time), r.rounds, r.cutscenes ? "on" : "off");
}

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
uint16_t LoadU16(rex::memory::Memory* memory, uint32_t address) {
  return rex::memory::load_and_swap<uint16_t>(memory->TranslateVirtual<uint8_t*>(address));
}
void StoreU16(rex::memory::Memory* memory, uint32_t address, uint16_t value) {
  rex::memory::store_and_swap<uint16_t>(memory->TranslateVirtual<uint8_t*>(address), value);
}
void StoreU32(rex::memory::Memory* memory, uint32_t address, uint32_t value) {
  rex::memory::store_and_swap<uint32_t>(memory->TranslateVirtual<uint8_t*>(address), value);
}

void PutU32(uint8_t* p, uint32_t v) {
  p[0] = uint8_t(v >> 24);
  p[1] = uint8_t(v >> 16);
  p[2] = uint8_t(v >> 8);
  p[3] = uint8_t(v);
}
uint32_t GetU32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}

void PutName(uint8_t* p, const std::string& name) {
  std::memset(p, 0, kNameBytes);
  std::memcpy(p, name.data(), std::min(name.size(), kNameBytes - 1));
}
std::string GetName(const uint8_t* p) {
  std::string name;
  for (size_t i = 0; i < kNameBytes && p[i]; ++i) {
    name += (p[i] >= 32 && p[i] < 127) ? char(p[i]) : '?';
  }
  return name;
}

// In a lobby session: this console's side (0 host, 1 guest), -1 otherwise.
int LocalSide(rex::memory::Memory* memory) {
  if (!memory || !rex::net::IsGameSessionOpen() || !rex::net::online::IsLobbyMode()) {
    return -1;
  }
  const uint32_t player = LoadU32(memory, kNetSession + kNetSessionLocalPlayer);
  return (player == 1 || player == 2) ? int(player) - 1 : -1;
}

// -1 = unknown, 0 Ranked, 1 Player Match.
int GameType(rex::memory::Memory* memory) {
  const uint32_t net = LoadU32(memory, kNetManagerPointer);
  if (!Readable(memory, net + kNetTime + 4, 4) || !Readable(memory, net + kNetGameType, 4)) {
    return -1;
  }
  return int(LoadU32(memory, net + kNetGameType));
}

// ------------------------------------------------------------- the state

enum class HostPhase { kIdle, kChoosing, kSending, kDone };
enum class GuestPhase { kIdle, kWaiting, kHave };

struct State {
  std::mutex mutex;
  // Host.
  HostPhase host = HostPhase::kIdle;
  uint32_t host_nonce = 0;  // the guest's session nonce
  std::string guest_name;
  Rules editing;            // the menu's values
  Rules sent;               // the rules sent / acknowledged
  uint8_t seq = 0;
  int64_t done_ms = 0;
  // Guest.
  GuestPhase guest = GuestPhase::kIdle;
  uint32_t guest_nonce = 0;
  std::string host_name;
  Rules received;
  int64_t have_ms = 0;
  int64_t request_sent_ms = 0;
  int64_t menu_seen_ms = 0;  // the host's last MENU message
  // The agreed time limit / rounds still to be written into the game's room
  // (and, on the host, advertised in the room's contexts).
  bool write_room = false;
  // Host: the guest's last message (a guest that went quiet left).
  int64_t contact_ms = 0;
  // Host, rules agreed: changed rules (the game's own Change Session Rules)
  // still to be acknowledged by the guest.
  bool resend = false;
  // Both: what the game thread saw last.
  bool session = false;
  bool in_battle = false;
  int side = -1;
  int game_type = -1;
  int64_t rules_sent_ms = 0;
  int64_t menu_sent_ms = 0;
  // The side read the same since (the session block can read wrong for a
  // moment while a session starts).
  int stable_side = -1;
  int64_t stable_since_ms = 0;
} g_state;

uint32_t NewNonce() {
  static std::mt19937 random{std::random_device{}()};
  uint32_t value = 0;
  while (!value) {
    value = random();
  }
  return value;
}

void SendRequest(uint32_t nonce) {
  uint8_t body[2 + 4 + kNameBytes] = {kVersion, kTypeRequest};
  PutU32(body + 2, nonce);
  PutName(body + 6, rex::net::online::PlayerName());
  rex::net::online::SendGameSide(kSideKind, body, sizeof(body));
}

void SendRules(uint32_t nonce, uint8_t seq, const Rules& rules) {
  uint8_t body[2 + 4 + 1 + kRulesBytes + kNameBytes] = {kVersion, kTypeRules};
  PutU32(body + 2, nonce);
  body[6] = seq;
  PackRules(rules, body + 7);
  PutName(body + 7 + kRulesBytes, rex::net::online::PlayerName());
  rex::net::online::SendGameSide(kSideKind, body, sizeof(body), 1);
}

void SendMenu(uint32_t nonce) {
  uint8_t body[2 + 4] = {kVersion, kTypeMenu};
  PutU32(body + 2, nonce);
  rex::net::online::SendGameSide(kSideKind, body, sizeof(body));
}

void SendAck(uint32_t nonce, uint8_t seq) {
  uint8_t body[2 + 4 + 1] = {kVersion, kTypeAck};
  PutU32(body + 2, nonce);
  body[6] = seq;
  rex::net::online::SendGameSide(kSideKind, body, sizeof(body), 1);
}

// The host's defaults: the last rules it picked, the room's Create Match pages
// for what it never picked.
Rules HostDefaults(rex::memory::Memory* memory) {
  Rules r;
  r.health[0] = uint8_t(std::clamp(REXCVAR_GET(online_rules_host_health), 1, 8));
  r.attack[0] = uint8_t(std::clamp(REXCVAR_GET(online_rules_host_attack), 1, 5));
  r.defense[0] = uint8_t(std::clamp(REXCVAR_GET(online_rules_host_defense), 1, 5));
  r.health[1] = uint8_t(std::clamp(REXCVAR_GET(online_rules_guest_health), 1, 8));
  r.attack[1] = uint8_t(std::clamp(REXCVAR_GET(online_rules_guest_attack), 1, 5));
  r.defense[1] = uint8_t(std::clamp(REXCVAR_GET(online_rules_guest_defense), 1, 5));
  const uint32_t net = memory ? LoadU32(memory, kNetManagerPointer) : 0;
  const bool room = memory && Readable(memory, net + kNetGameType, 16);
  if (room) {
    r.time = uint8_t(std::min<uint32_t>(LoadU32(memory, net + kNetTime), 4));
    r.rounds = uint8_t(std::min<uint32_t>(LoadU32(memory, net + kNetRounds), 4) + 1);
  }
  r.cutscenes = REXCVAR_GET(online_rules_cutscenes) == "off" ? 0 : 1;
  return r;
}

// The host confirmed: these are its defaults from now on.
void SaveHostDefaults(const Rules& r, const std::filesystem::path& config_path) {
  rex::cvar::SetFlagByName("online_rules_host_health", std::to_string(r.health[0]));
  rex::cvar::SetFlagByName("online_rules_host_attack", std::to_string(r.attack[0]));
  rex::cvar::SetFlagByName("online_rules_host_defense", std::to_string(r.defense[0]));
  rex::cvar::SetFlagByName("online_rules_guest_health", std::to_string(r.health[1]));
  rex::cvar::SetFlagByName("online_rules_guest_attack", std::to_string(r.attack[1]));
  rex::cvar::SetFlagByName("online_rules_guest_defense", std::to_string(r.defense[1]));
  rex::cvar::SetFlagByName("online_rules_cutscenes", r.cutscenes ? "on" : "off");
  if (!config_path.empty()) {
    rex::cvar::SaveConfig(config_path);
  }
}

}  // namespace

// --------------------------------------------------------- network thread

// burstlimit_netinput.cpp's side-channel handler: kind 18.
bool BurstLimitOnlineRulesOnSide(uint8_t kind, const uint8_t* body, size_t size) {
  if (kind != kSideKind) {
    return false;
  }
  if (size < 6 || body[0] != kVersion) {
    return true;
  }
  rex::memory::Memory* memory = GuestMemory();
  const int side = LocalSide(memory);
  const uint32_t nonce = GetU32(body + 2);
  State& s = g_state;
  std::lock_guard<std::mutex> lock(s.mutex);
  switch (body[1]) {
    case kTypeRequest: {
      if (side != 0 || size < 6 + kNameBytes || GameType(memory) != 1) {
        return true;
      }
      // Only a host gets requests: no guest state here.
      s.guest = GuestPhase::kIdle;
      s.guest_nonce = 0;
      s.contact_ms = NowMs();
      if (nonce != s.host_nonce) {
        // A new guest: the menu opens with the host's defaults.
        s.host_nonce = nonce;
        s.guest_name = GetName(body + 6);
        s.editing = HostDefaults(memory);
        s.host = HostPhase::kChoosing;
        s.resend = false;
        REXLOG_WARN("[OnlineRules] host: {} joined - rules menu open ({})", s.guest_name,
                    DescribeRules(s.editing));
      } else if (s.host == HostPhase::kDone) {
        // The guest asks again (it lost our rules): the same ones.
        SendRules(nonce, s.seq, s.sent);
      }
      return true;
    }
    case kTypeRules: {
      Rules rules;
      if (side != 1 || size < 7 + kRulesBytes + kNameBytes || nonce != s.guest_nonce ||
          !UnpackRules(body + 7, &rules) || REXCVAR_GET(online_rules_test_ignore)) {
        return true;
      }
      if (s.guest != GuestPhase::kHave || std::memcmp(&rules, &s.received, sizeof(Rules)) != 0) {
        s.received = rules;
        s.host_name = GetName(body + 7 + kRulesBytes);
        s.guest = GuestPhase::kHave;
        s.have_ms = NowMs();
        s.write_room = true;
        REXLOG_WARN("[OnlineRules] guest: rules from {}: {}", s.host_name, DescribeRules(rules));
      }
      SendAck(nonce, body[6]);
      return true;
    }
    case kTypeMenu: {
      if (side == 1 && nonce == s.guest_nonce && s.guest == GuestPhase::kWaiting) {
        s.menu_seen_ms = NowMs();
      }
      return true;
    }
    case kTypeAck: {
      if (size < 7 || side != 0 || nonce != s.host_nonce || body[6] != s.seq) {
        return true;
      }
      if (s.host == HostPhase::kDone && s.resend) {
        s.resend = false;
        REXLOG_WARN("[OnlineRules] host: {} has the changed rules", s.guest_name);
        return true;
      }
      if (s.host != HostPhase::kSending) {
        return true;
      }
      s.host = HostPhase::kDone;
      s.done_ms = NowMs();
      s.contact_ms = s.done_ms;
      s.write_room = true;
      REXLOG_WARN("[OnlineRules] host: {} has the rules: {}", s.guest_name,
                  DescribeRules(s.sent));
      return true;
    }
    default:
      return true;
  }
}

// ------------------------------------------------------------ game thread

// Every game tick (BurstLimitOnlineFrame): the session's state for the menu,
// the guest's requests and the host's sending.
void BurstLimitOnlineRulesFrame(rex::memory::Memory* memory) {
  State& s = g_state;
  const int side = LocalSide(memory);
  const bool session = side >= 0;
  const int game_type = session ? GameType(memory) : -1;
  const uint32_t manager = LoadU32(memory, kInputManagerPointer);
  const bool in_battle = session && Readable(memory, manager + kDriverState, 4) &&
                         LoadU32(memory, manager + kDriverState) == kDriverStateMatch;
  const int64_t now = NowMs();
  std::lock_guard<std::mutex> lock(s.mutex);
  // Everything is per session: reset only when the game's session is gone
  // (not when a value reads differently for a moment).
  const bool open = rex::net::IsGameSessionOpen() && rex::net::online::IsLobbyMode();
  if (!open) {
    if (s.host != HostPhase::kIdle || s.guest != GuestPhase::kIdle) {
      REXLOG_INFO("[OnlineRules] session over: rules reset");
    }
    s.host = HostPhase::kIdle;
    s.host_nonce = 0;
    s.guest = GuestPhase::kIdle;
    s.guest_nonce = 0;
    s.menu_seen_ms = 0;
  }
  if (in_battle && !s.in_battle && session && game_type == 1) {
    // What this battle uses (the battle's copy of the request, [battle + 216]).
    const uint32_t battle = LoadU32(memory, 0x841B5130);
    const uint32_t copy = Readable(memory, battle + 216, 4) ? LoadU32(memory, battle + 216) : 0;
    if (Readable(memory, copy, kRequestSize)) {
      REXLOG_WARN("[OnlineRules] battle uses: P1 hp {} atk {} def {} | P2 hp {} atk {} def {} | "
                  "time {} rounds {} cut-scenes {}",
                  LoadU16(memory, copy + kRequestHealthMax), LoadU32(memory, copy + kRequestAttack),
                  LoadU16(memory, copy + kRequestDefense),
                  LoadU16(memory, copy + kRequestStride + kRequestHealthMax),
                  LoadU32(memory, copy + kRequestStride + kRequestAttack),
                  LoadU16(memory, copy + kRequestStride + kRequestDefense),
                  LoadU16(memory, copy + kRequestTime), LoadU16(memory, copy + kRequestRounds),
                  LoadU16(memory, copy + kRequestCutscenes));
    }
  }
  s.session = session;
  s.in_battle = in_battle;
  s.side = side;
  s.game_type = game_type;
  if (side != s.stable_side) {
    s.stable_side = side;
    s.stable_since_ms = now;
  }
  if (!session || game_type != 1 || now - s.stable_since_ms < 1500) {
    return;
  }
  if (side == 0 && (s.host == HostPhase::kChoosing || s.host == HostPhase::kSending) &&
      now - s.contact_ms > 3000) {
    // The guest left (or its link died) before it had the rules: the room
    // waits for the next guest, who gets the menu again.
    REXLOG_WARN("[OnlineRules] host: {} left before the rules were agreed - menu closed",
                s.guest_name);
    s.host = HostPhase::kIdle;
    s.host_nonce = 0;
  }
  if (s.write_room) {
    // The game's room rules (Create Match pages, View Battle Rules, the next
    // Create Match) = the agreed time limit and rounds; the host also
    // advertises them (contexts 3 / 4 in the room's info: the room list).
    s.write_room = false;
    const Rules& r = side == 0 ? s.sent : s.received;
    const uint32_t net = LoadU32(memory, kNetManagerPointer);
    if (Readable(memory, net + kNetGameType, 16)) {
      StoreU32(memory, net + kNetRounds, uint32_t(r.rounds - 1));
      StoreU32(memory, net + kNetTime, r.time);
      for (uint32_t record : kNetSessionRules) {
        uint8_t* p = memory->TranslateVirtual<uint8_t*>(kNetSession + record);
        p[kRulesTime] = r.time;
        p[kRulesRounds] = uint8_t(r.rounds - 1);
      }
      if (side == 0) {
        rex::net::online::OnUserSetContext(0, 3, uint32_t(r.rounds - 1));
        rex::net::online::OnUserSetContext(0, 4, r.time);
        rex::net::online::OnHostModify();
      }
      REXLOG_WARN("[OnlineRules] {}: room rules now time {} rounds {}{}",
                  side == 0 ? "host" : "guest", TimeText(r.time), r.rounds,
                  side == 0 ? " (room info updated)" : "");
    }
  } else if (side == 0 && s.host == HostPhase::kDone) {
    // The host changed the time limit / rounds with the game's own Host
    // Actions > Change Session Rules: those are the room's rules now, for
    // both PCs.
    const uint32_t net = LoadU32(memory, kNetManagerPointer);
    if (Readable(memory, net + kNetGameType, 16)) {
      const uint32_t time = LoadU32(memory, net + kNetTime);
      const uint32_t rounds = LoadU32(memory, net + kNetRounds) + 1;
      if (time <= 4 && rounds >= 1 && rounds <= 5 &&
          (time != s.sent.time || rounds != s.sent.rounds)) {
        s.sent.time = uint8_t(time);
        s.sent.rounds = uint8_t(rounds);
        ++s.seq;
        s.resend = true;
        s.rules_sent_ms = 0;
        s.write_room = true;  // the session records / View Battle Rules too
        REXLOG_WARN("[OnlineRules] host: rules changed in the game: {}", DescribeRules(s.sent));
      }
    }
  }
  if (side == 0 && s.host == HostPhase::kDone && s.resend && now - s.rules_sent_ms >= 300) {
    s.rules_sent_ms = now;
    SendRules(s.host_nonce, s.seq, s.sent);
  }
  if (side == 0 && (s.host == HostPhase::kChoosing || s.host == HostPhase::kSending) &&
      now - s.menu_sent_ms >= 300) {
    s.menu_sent_ms = now;
    SendMenu(s.host_nonce);
  }
  if (side == 1 && s.host == HostPhase::kIdle && s.guest != GuestPhase::kHave) {
    if (!s.guest_nonce) {
      s.guest_nonce = NewNonce();
      s.guest = GuestPhase::kWaiting;
      REXLOG_INFO("[OnlineRules] guest: asking the host for the rules");
    }
    if (now - s.request_sent_ms >= 400) {
      s.request_sent_ms = now;
      SendRequest(s.guest_nonce);
    }
  }
  if (side == 0 && s.host == HostPhase::kSending && now - s.rules_sent_ms >= 300) {
    s.rules_sent_ms = now;
    SendRules(s.host_nonce, s.seq, s.sent);
  }
}

// Mid-asm hook at 0x8217AEB4, the start of the battle set-up sub_8217AEA8,
// before anything reads the match request: in a Player Match of the lobby,
// both PCs write the room's rules into it - the host its acknowledged rules,
// the guest the ones it received (the same; the host's menu doesn't close
// before the guest has them).
// burstlimit_achievements.cpp: the one_hit_kill test option (offline only).
void BurstLimitOneHitKillBattleSetup();

void BurstLimitOnlineRulesBattleSetup() {
  BurstLimitOneHitKillBattleSetup();
  rex::memory::Memory* memory = GuestMemory();
  const int side = LocalSide(memory);
  if (side < 0) {
    return;
  }
  const uint32_t request = LoadU32(memory, kRequestPointer);
  if (!Readable(memory, request, kRequestSize)) {
    return;
  }
  const int game_type = GameType(memory);
  if (game_type != 1) {
    REXLOG_WARN("[OnlineRules] game type {}: the game's rules", game_type);
    return;
  }
  Rules rules;
  {
    State& s = g_state;
    std::lock_guard<std::mutex> lock(s.mutex);
    const bool have = side == 0 ? s.host == HostPhase::kDone : s.guest == GuestPhase::kHave;
    if (!have) {
      REXLOG_WARN("[OnlineRules] {}: no rules agreed yet - the game's rules",
                  side == 0 ? "host" : "guest");
      return;
    }
    rules = side == 0 ? s.sent : s.received;
  }
  for (uint32_t p = 0; p < 2; ++p) {
    const uint32_t e = request + p * kRequestStride;
    const uint16_t hp = uint16_t(rules.health[p] * 1000);
    StoreU16(memory, e + kRequestHealth, hp);
    StoreU16(memory, e + kRequestHealthMax, hp);
    StoreU32(memory, e + kRequestAttack, rules.attack[p]);
    StoreU16(memory, e + kRequestDefense, rules.defense[p]);
  }
  StoreU16(memory, request + kRequestTime,
           rules.time < 4 ? uint16_t(kTimeValues[rules.time] * 60) : kTimeInfinite);
  StoreU16(memory, request + kRequestRounds, rules.rounds);
  StoreU16(memory, request + kRequestCutscenes, rules.cutscenes);
  std::string check;
  for (uint32_t p = 0; p < 2; ++p) {
    const uint32_t e = request + p * kRequestStride;
    check += fmt::format("P{} hp {}/{} atk {} def {} | ", p + 1,
                         LoadU16(memory, e + kRequestHealth),
                         LoadU16(memory, e + kRequestHealthMax),
                         LoadU32(memory, e + kRequestAttack),
                         LoadU16(memory, e + kRequestDefense));
  }
  REXLOG_WARN("[OnlineRules] battle set-up ({}): {}time {} rounds {} cut-scenes {} mode {}",
              side == 0 ? "host" : "guest", check, LoadU16(memory, request + kRequestTime),
              LoadU16(memory, request + kRequestRounds),
              LoadU16(memory, request + kRequestCutscenes),
              LoadU32(memory, request + kRequestMode));
}

// Mid-asm hook at 0x8226D25C in sub_8226D250 (the Session Lobby's Battle
// Rules box, View Battle Rules): r31 + 20 = rounds - 1, + 21 = time limit
// index, read right after. Both PCs show the agreed rules (infinite has no
// entry in the game's list: it shows as Random).
void BurstLimitOnlineRulesView(PPCRegister& r31) {
  rex::memory::Memory* memory = GuestMemory();
  if (!memory || LocalSide(memory) < 0) {
    return;
  }
  Rules rules;
  {
    std::lock_guard<std::mutex> lock(g_state.mutex);
    const bool host = g_state.host == HostPhase::kDone;
    const bool guest = g_state.guest == GuestPhase::kHave;
    if (!host && !guest) {
      return;
    }
    rules = host ? g_state.sent : g_state.received;
  }
  if (!Readable(memory, r31.u32, 24)) {
    return;
  }
  uint8_t* p = memory->TranslateVirtual<uint8_t*>(r31.u32);
  p[20] = uint8_t(rules.rounds - 1);
  p[21] = rules.time;
}

// Mid-asm hooks at 0x8226E304 / 0x8226F030 (the results list's Time Limit
// text): r11 = time index * 4, r4 = the text id. Index 4 (an infinite room)
// is past the 4 ids: the game's "Random" text (0x1D7), as View Battle Rules
// shows it.
void BurstLimitOnlineRulesListTime(PPCRegister& r4, PPCRegister& r11) {
  if (r11.u32 == 16) {
    r4.u64 = 0x1D7;
  }
}

// ------------------------------------------------------------- UI thread

// burstlimit_forms.cpp: the character select is on screen.
bool BurstLimitCharacterSelectActive();

namespace {

// The look of the stage select's Player 1 / CPU Settings panels.
constexpr ImU32 kBodyTop = IM_COL32(214, 160, 34, 255);
constexpr ImU32 kBodyBottom = IM_COL32(184, 122, 14, 255);
constexpr ImU32 kRim = IM_COL32(246, 232, 196, 255);
constexpr ImU32 kBar = IM_COL32(0, 0, 0, 255);
constexpr ImU32 kWhite = IM_COL32(255, 255, 255, 255);
constexpr ImU32 kDim = IM_COL32(255, 255, 255, 150);
constexpr ImU32 kYellow = IM_COL32(255, 222, 40, 255);
constexpr ImU32 kFilled = IM_COL32(128, 0, 0, 255);
constexpr ImU32 kFilledTop = IM_COL32(186, 20, 20, 255);
constexpr ImU32 kEmpty = IM_COL32(150, 150, 150, 255);
constexpr ImU32 kEdge = IM_COL32(40, 20, 0, 255);
constexpr ImU32 kHighlight = IM_COL32(255, 238, 150, 235);
constexpr ImU32 kHighlightEnd = IM_COL32(255, 220, 110, 0);
constexpr ImU32 kArrow = IM_COL32(250, 236, 200, 255);

enum Row { kHealth, kAttack, kDefense, kTime, kRounds, kCutscenes };
constexpr int kLeftRows = 6;
constexpr int kRightRows = 3;

class RulesDialog final : public rex::ui::ImGuiDialog {
 public:
  RulesDialog(rex::ui::ImGuiDrawer* drawer, rex::ui::ImmediateDrawer* immediate_drawer,
              std::filesystem::path game_data_root, std::filesystem::path config_path,
              std::function<void(std::function<void()>)> defer)
      : ImGuiDialog(drawer),
        immediate_drawer_(immediate_drawer),
        game_data_root_(std::move(game_data_root)),
        config_path_(std::move(config_path)),
        defer_(std::move(defer)),
        pad_(std::make_shared<SharedPad>()) {}

  ~RulesDialog() override { Unblock(); }

 protected:
  void OnDraw(ImGuiIO& io) override {
    ImGui::SetCurrentContext(io.Ctx);
    if (!art_) {
      art_ = BurstLimitSharedMenuArt(game_data_root_);
    }
    art_ready_ = art_ && art_->Ready(immediate_drawer_);
    // A copy of the state for this frame.
    HostPhase host;
    GuestPhase guest;
    Rules rules;
    std::string name;
    bool in_battle;
    int64_t since = 0;
    int64_t menu_seen = 0;
    {
      std::lock_guard<std::mutex> lock(g_state.mutex);
      host = g_state.host;
      guest = g_state.guest;
      in_battle = g_state.in_battle;
      menu_seen = g_state.menu_seen_ms;
      if (host != HostPhase::kIdle) {
        rules = host == HostPhase::kChoosing ? g_state.editing : g_state.sent;
        name = g_state.guest_name;
        since = g_state.done_ms;
      } else {
        rules = g_state.received;
        name = g_state.host_name;
        since = g_state.have_ms;
      }
    }
    const bool menu = !in_battle && (host == HostPhase::kChoosing || host == HostPhase::kSending);
    if (menu) {
      Block();
      if (!was_open_) {
        was_open_ = true;
        panel_ = 0;
        row_ = 0;
        held_ = 0;
        first_read_done_ = false;
        pad_->valid = false;  // a fresh read first
      }
      ReadPadLater();
      if (host == HostPhase::kChoosing) {
        HandleInput(rules);
      }
      DrawRules(io, rules, true, host == HostPhase::kSending, name);
      return;
    }
    Unblock();
    was_open_ = false;
    if (in_battle) {
      return;
    }
    // Only right after the rules are set (host: sent, guest: received), and
    // the guest's "choosing" only while the host says its menu is open.
    const int64_t now = NowMs();
    if (BurstLimitCharacterSelectActive()) {
      return;  // Only in the Session Lobby.
    }
    if (host == HostPhase::kDone && now - since < 6000) {
      DrawSummary(io, rules, true, name);
    } else if (host == HostPhase::kIdle && guest == GuestPhase::kWaiting && menu_seen &&
               now - menu_seen < 1500) {
      DrawWaiting(io);
    } else if (host == HostPhase::kIdle && guest == GuestPhase::kHave && now - since < 10000) {
      DrawSummary(io, rules, false, name);
    }
  }

 private:
  struct PadState {
    uint16_t buttons = 0;
    int16_t lx = 0, ly = 0;
  };
  struct SharedPad {
    PadState state;
    bool valid = false;
    bool pending = false;
  };

  static rex::input::InputSystem* Input() {
    auto* runtime = rex::Runtime::instance();
    return runtime ? static_cast<rex::input::InputSystem*>(runtime->input_system()) : nullptr;
  }

  void Block() {
    if (!blocking_) {
      if (auto* input = Input()) {
        input->AddUIInputBlocker();
        blocking_ = true;
      }
    }
  }
  void Unblock() {
    if (blocking_) {
      blocking_ = false;
      if (auto* input = Input()) {
        input->RemoveUIInputBlocker();
      }
    }
  }

  // The controllers are read after the paint (like the other menus).
  void ReadPadLater() {
    if (pad_->pending || !defer_) {
      return;
    }
    pad_->pending = true;
    defer_([pad = pad_]() {
      PadState state;
      if (auto* input = Input()) {
        for (uint32_t user = 0; user < rex::input::kMaxGuestUsers; ++user) {
          rex::input::X_INPUT_STATE xs = {};
          if (input->GetStateForUI(user, &xs) != rex::X_RESULT(0)) {
            continue;
          }
          state.buttons |= uint16_t(xs.gamepad.buttons);
          if (std::abs(int(xs.gamepad.thumb_lx)) > std::abs(int(state.lx))) {
            state.lx = xs.gamepad.thumb_lx;
          }
          if (std::abs(int(xs.gamepad.thumb_ly)) > std::abs(int(state.ly))) {
            state.ly = xs.gamepad.thumb_ly;
          }
        }
      }
      pad->state = state;
      pad->valid = true;
      pad->pending = false;
    });
  }

  void HandleInput(Rules rules) {
    if (!pad_->valid) {
      return;
    }
    constexpr uint16_t kUp = 0x0001, kDown = 0x0002, kLeft = 0x0004, kRight = 0x0008;
    constexpr uint16_t kA = 0x1000, kB = 0x2000;
    const PadState pad = pad_->state;
    uint16_t buttons = pad.buttons;
    if (pad.ly > 16000) buttons |= kUp;
    if (pad.ly < -16000) buttons |= kDown;
    if (pad.lx < -16000) buttons |= kLeft;
    if (pad.lx > 16000) buttons |= kRight;
    // Presses: new buttons, and the direction held repeats.
    const int64_t now = NowMs();
    uint16_t pressed = buttons & ~held_;
    const uint16_t dirs = buttons & (kUp | kDown | kLeft | kRight);
    if (dirs && dirs == (held_ & (kUp | kDown | kLeft | kRight))) {
      if (now >= repeat_at_) {
        pressed |= dirs;
        repeat_at_ = now + 110;
      }
    } else if (dirs) {
      repeat_at_ = now + 380;
    }
    if (!first_read_done_) {
      // What was held when the menu opened isn't a press.
      first_read_done_ = true;
      pressed = 0;
    }
    held_ = buttons;
    if (!pressed) {
      return;
    }
    const int rows = panel_ == 0 ? kLeftRows : kRightRows;
    if (pressed & kUp) row_ = (row_ + rows - 1) % rows;
    if (pressed & kDown) row_ = (row_ + 1) % rows;
    const int delta = (pressed & kRight) ? 1 : (pressed & kLeft) ? -1 : 0;
    if (delta) {
      const int p = panel_;
      auto step = [delta](uint8_t& v, int lo, int hi) {
        v = uint8_t(std::clamp(int(v) + delta, lo, hi));
      };
      switch (row_) {
        case kHealth: step(rules.health[p], 1, 8); break;
        case kAttack: step(rules.attack[p], 1, 5); break;
        case kDefense: step(rules.defense[p], 1, 5); break;
        case kTime: step(rules.time, 0, kTimeChoices - 1); break;
        case kRounds: step(rules.rounds, 1, 5); break;
        case kCutscenes: rules.cutscenes = delta > 0 ? 1 : 0; break;
      }
    }
    bool confirm = false;
    if (pressed & kA) {
      if (panel_ == 0) {
        panel_ = 1;
        row_ = 0;
      } else {
        confirm = true;
      }
    }
    if ((pressed & kB) && panel_ == 1) {
      panel_ = 0;
      row_ = 0;
    }
    std::lock_guard<std::mutex> lock(g_state.mutex);
    if (g_state.host != HostPhase::kChoosing) {
      return;
    }
    g_state.editing = rules;
    if (confirm) {
      g_state.sent = rules;
      ++g_state.seq;
      g_state.host = HostPhase::kSending;
      g_state.rules_sent_ms = 0;
      REXLOG_WARN("[OnlineRules] host: sending to {}: {}", g_state.guest_name,
                  DescribeRules(rules));
      // The host's defaults next time (saved after the paint).
      if (defer_) {
        defer_([rules, path = config_path_]() { SaveHostDefaults(rules, path); });
      }
    }
  }

  // ---- drawing

  float TextWidth(float size, const std::string& text) const {
    return art_ready_ ? art_->Text(nullptr, ImVec2(0, 0), size, text, 0)
                      : rex::ui::overlay_text::Measure(size, text).x;
  }
  void Text(ImDrawList* dl, ImVec2 pos, float size, const std::string& text, ImU32 color) const {
    if (art_ready_) {
      art_->Text(dl, pos, size, text, color);
    } else {
      rex::ui::overlay_text::Draw(dl, size, pos, color, text);
    }
  }

  void Triangle(ImDrawList* dl, ImVec2 center, float size, int dir, ImU32 color) const {
    // dir: 0 left, 1 right, 2 up, 3 down
    const float h = size * 0.5f;
    const float x = center.x, y = center.y;
    switch (dir) {
      case 0: dl->AddTriangleFilled({x - h, y}, {x + h, y - h}, {x + h, y + h}, color); break;
      case 1: dl->AddTriangleFilled({x + h, y}, {x - h, y + h}, {x - h, y - h}, color); break;
      case 2: dl->AddTriangleFilled({x, y - h}, {x + h, y + h}, {x - h, y + h}, color); break;
      default: dl->AddTriangleFilled({x, y + h}, {x - h, y - h}, {x + h, y - h}, color); break;
    }
  }

  // Segments like the game's meters: `filled` of `count`, right-aligned at `right`.
  void Meter(ImDrawList* dl, float right, float center_y, float s, int filled, int count) const {
    const float w = 13.0f * s, h = 22.0f * s, gap = 3.0f * s;
    float x = right - count * w - (count - 1) * gap;
    for (int i = 0; i < count; ++i) {
      const ImVec2 a(x, center_y - h * 0.5f), b(x + w, center_y + h * 0.5f);
      if (i < filled) {
        dl->AddRectFilledMultiColor(a, b, kFilledTop, kFilledTop, kFilled, kFilled);
      } else {
        dl->AddRectFilled(a, b, kEmpty);
      }
      dl->AddRect(a, b, kEdge, 2.0f * s, 0, 1.5f * s);
      x += w + gap;
    }
  }

  // One panel like the stage select's: black title bar, the rows of player
  // `p` (player 1's has the battle rows too), black footer bar.
  void Panel(ImDrawList* dl, ImVec2 min, ImVec2 max, float s, const std::string& title,
             const std::string& name, const Rules& rules, int p, bool active, bool editable,
             const std::string& footer, bool footer_buttons) const {
    const float radius = 12.0f * s;
    dl->AddRectFilledMultiColor(ImVec2(min.x, min.y + radius), ImVec2(max.x, max.y - radius),
                                kBodyTop, kBodyTop, kBodyBottom, kBodyBottom);
    const float header = 58.0f * s, footer_h = 44.0f * s;
    dl->AddRectFilled(min, ImVec2(max.x, min.y + header), kBar, radius,
                      ImDrawFlags_RoundCornersTop);
    dl->AddRectFilled(ImVec2(min.x, max.y - footer_h), max, kBar, radius,
                      ImDrawFlags_RoundCornersBottom);
    dl->AddRect(min, max, kRim, radius, 0, 3.0f * s);
    Text(dl, ImVec2(min.x + 20.0f * s, min.y + 12.0f * s), 34.0f * s, title, kWhite);
    const float ns = 26.0f * s;
    Text(dl, ImVec2(max.x - 20.0f * s - TextWidth(ns, name), min.y + 17.0f * s), ns, name,
         kYellow);
    const int rows = p == 0 ? kLeftRows : kRightRows;
    static const char* kLabels[kLeftRows] = {"Health", "Attack", "Defense", "Time Limit",
                                             "Rounds", "Opening/Ending Cut-Scene"};
    const float row_h = 48.0f * s;
    float y = min.y + header + 14.0f * s;
    const float label_size = 28.0f * s;
    const float value_right = max.x - 46.0f * s;
    for (int r = 0; r < rows; ++r) {
      const bool selected = active && editable && r == row_;
      const ImVec2 a(min.x + 3.0f * s, y), b(max.x - 3.0f * s, y + row_h);
      if (selected) {
        dl->AddRectFilledMultiColor(a, b, kHighlight, kHighlightEnd, kHighlightEnd, kHighlight);
      }
      const float cy = y + row_h * 0.5f;
      std::string label = kLabels[r];
      float size = label_size;
      if (r == kCutscenes) {
        size = 22.0f * s;
      }
      Text(dl, ImVec2(min.x + 22.0f * s, cy - size * 0.55f), size, label,
           active || !editable ? kWhite : kDim);
      float right = value_right;
      switch (r) {
        case kHealth: Meter(dl, right, cy, s, rules.health[p], 8); break;
        case kAttack: Meter(dl, right, cy, s, rules.attack[p], 5); break;
        case kDefense: Meter(dl, right, cy, s, rules.defense[p], 5); break;
        default: {
          std::string value = r == kTime ? TimeText(rules.time)
                              : r == kRounds ? std::to_string(rules.rounds)
                                             : (rules.cutscenes ? "ON" : "OFF");
          const float vs = 30.0f * s;
          Text(dl, ImVec2(right - TextWidth(vs, value), cy - vs * 0.55f), vs, value, kWhite);
        }
      }
      if (selected) {
        const float mw = r == kHealth ? 8 * 16.0f * s : r <= kDefense ? 5 * 16.0f * s
                                                                       : 90.0f * s;
        Triangle(dl, ImVec2(right - mw - 14.0f * s, cy), 18.0f * s, 0, kArrow);
        Triangle(dl, ImVec2(right + 18.0f * s, cy), 18.0f * s, 1, kArrow);
      }
      y += row_h;
    }
    // Footer.
    const float fy = max.y - footer_h * 0.5f;
    const float fs = 24.0f * s;
    float x = min.x + 24.0f * s;
    if (footer_buttons && active) {
      Triangle(dl, ImVec2(x + 6.0f * s, fy), 14.0f * s, 2, kYellow);
      Triangle(dl, ImVec2(x + 22.0f * s, fy), 14.0f * s, 3, kYellow);
      x += 36.0f * s;
      Text(dl, ImVec2(x, fy - fs * 0.55f), fs, "Select", kWhite);
      x += TextWidth(fs, "Select") + 18.0f * s;
      Triangle(dl, ImVec2(x + 6.0f * s, fy), 14.0f * s, 0, kYellow);
      Triangle(dl, ImVec2(x + 22.0f * s, fy), 14.0f * s, 1, kYellow);
      x += 36.0f * s;
      Text(dl, ImVec2(x, fy - fs * 0.55f), fs, "Change", kWhite);
      x += TextWidth(fs, "Change") + 18.0f * s;
      x = Button(dl, x, fy, s, 0, "Confirm");
      if (p == 1) {
        Button(dl, x + 14.0f * s, fy, s, 1, "Back");
      }
    } else if (!footer.empty()) {
      Text(dl, ImVec2((min.x + max.x - TextWidth(fs, footer)) * 0.5f, fy - fs * 0.55f), fs, footer,
           kWhite);
    }
  }

  float Button(ImDrawList* dl, float x, float cy, float s, int which, const char* label) const {
    const float icon = 26.0f * s, fs = 24.0f * s;
    if (art_ready_) {
      art_->Icon(dl, which == 0 ? BurstLimitMenuArt::kButtonA : BurstLimitMenuArt::kButtonB,
                 ImVec2(x, cy - icon * 0.5f), icon, kWhite);
    } else {
      dl->AddCircleFilled(ImVec2(x + icon * 0.5f, cy), icon * 0.5f,
                          which == 0 ? IM_COL32(40, 160, 40, 255) : IM_COL32(190, 30, 30, 255));
    }
    x += icon + 4.0f * s;
    Text(dl, ImVec2(x, cy - fs * 0.55f), fs, label, kWhite);
    return x + TextWidth(fs, label);
  }

  // The host's menu (editable) or the chosen rules (read-only).
  void DrawRules(ImGuiIO& io, const Rules& rules, bool editable, bool sending,
                 const std::string& other) {
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const ImVec2 d = io.DisplaySize;
    // 1280x720 layout, scaled to the window (and centered when wider).
    const float s = std::min(d.x / 1280.0f, d.y / 720.0f);
    const float ox = (d.x - 1280.0f * s) * 0.5f, oy = (d.y - 720.0f * s) * 0.5f;
    auto P = [&](float x, float y) { return ImVec2(ox + x * s, oy + y * s); };
    if (editable) {
      dl->AddRectFilled(ImVec2(0, 0), d, IM_COL32(0, 0, 0, 120));
    }
    std::string title;
    std::string subtitle;
    std::lock_guard<std::mutex> lock(g_state.mutex);
    const bool host = g_state.side == 0;
    const std::string host_name = host ? rex::net::online::PlayerName() : g_state.host_name;
    const std::string guest_name = host ? g_state.guest_name : rex::net::online::PlayerName();
    if (editable) {
      title = "Room Rules";
      subtitle = other + " joined. Choose the battle settings for this room.";
    } else {
      title = "Room Rules";
      subtitle = host ? other + " got the battle settings." : "Chosen by the host, " + other + ".";
    }
    const float ts = 46.0f * s;
    Text(dl, ImVec2(ox + (1280.0f * s - TextWidth(ts, title)) * 0.5f, oy + 70.0f * s), ts, title,
         kYellow);
    const float ss = 26.0f * s;
    Text(dl, ImVec2(ox + (1280.0f * s - TextWidth(ss, subtitle)) * 0.5f, oy + 128.0f * s), ss,
         subtitle, kWhite);
    const std::string left_footer = sending ? "Sending to " + guest_name + "..." : "";
    Panel(dl, P(90, 180), P(630, 180 + 58 + 14 + 6 * 48 + 14 + 44), s,
          "Player 1 Settings", host_name, rules, 0, editable && panel_ == 0, editable,
          left_footer, editable && !sending);
    Panel(dl, P(670, 180), P(1190, 180 + 58 + 14 + 3 * 48 + 14 + 44), s,
          "Player 2 Settings", guest_name, rules, 1, editable && panel_ == 1, editable, "",
          editable && !sending);
  }

  // The rules in a small panel in the Session Lobby's free corner (bottom
  // left, below its menu and left of the players' rows).
  void DrawSummary(ImGuiIO& io, const Rules& rules, bool host, const std::string& other) {
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const ImVec2 d = io.DisplaySize;
    const float s = std::min(d.x / 1280.0f, d.y / 720.0f);
    const float ox = (d.x - 1280.0f * s) * 0.5f, oy = (d.y - 720.0f * s) * 0.5f;
    const ImVec2 min(ox + 50.0f * s, oy + 440.0f * s), max(ox + 640.0f * s, oy + 628.0f * s);
    const float radius = 12.0f * s;
    dl->AddRectFilledMultiColor(ImVec2(min.x, min.y + radius), ImVec2(max.x, max.y - radius),
                                kBodyTop, kBodyTop, kBodyBottom, kBodyBottom);
    dl->AddRectFilled(min, ImVec2(max.x, min.y + 46.0f * s), kBar, radius,
                      ImDrawFlags_RoundCornersTop);
    dl->AddRectFilled(ImVec2(min.x, max.y - radius * 2.0f), max, kBodyBottom, radius,
                      ImDrawFlags_RoundCornersBottom);
    dl->AddRect(min, max, kRim, radius, 0, 3.0f * s);
    Text(dl, ImVec2(min.x + 18.0f * s, min.y + 8.0f * s), 30.0f * s, "Room Rules", kWhite);
    const std::string note = host ? "sent to " + other : "set by " + other;
    const float ns = 22.0f * s;
    Text(dl, ImVec2(max.x - 18.0f * s - TextWidth(ns, note), min.y + 13.0f * s), ns, note,
         kYellow);
    std::string names[2];
    {
      std::lock_guard<std::mutex> lock(g_state.mutex);
      names[0] = host ? rex::net::online::PlayerName() : g_state.host_name;
      names[1] = host ? g_state.guest_name : rex::net::online::PlayerName();
    }
    const float ls = 24.0f * s;
    float y = min.y + 58.0f * s;
    for (int p = 0; p < 2; ++p) {
      Text(dl, ImVec2(min.x + 20.0f * s, y), ls, fmt::format("P{} {}", p + 1, names[p]), kYellow);
      Text(dl, ImVec2(min.x + 220.0f * s, y), ls,
           fmt::format("Health {}  Attack {}  Defense {}", rules.health[p], rules.attack[p],
                       rules.defense[p]),
           kWhite);
      y += 40.0f * s;
    }
    Text(dl, ImVec2(min.x + 20.0f * s, y), ls,
         fmt::format("Time Limit {}   Rounds {}   Cut-Scene {}", TimeText(rules.time),
                     rules.rounds, rules.cutscenes ? "ON" : "OFF"),
         kWhite);
  }

  void DrawWaiting(ImGuiIO& io) {
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const ImVec2 d = io.DisplaySize;
    const float s = std::min(d.x / 1280.0f, d.y / 720.0f);
    const float ox = (d.x - 1280.0f * s) * 0.5f, oy = (d.y - 720.0f * s) * 0.5f;
    const ImVec2 min(ox + 60.0f * s, oy + 520.0f * s), max(ox + 640.0f * s, oy + 630.0f * s);
    const float radius = 12.0f * s;
    dl->AddRectFilledMultiColor(ImVec2(min.x, min.y + radius), ImVec2(max.x, max.y - radius),
                                kBodyTop, kBodyTop, kBodyBottom, kBodyBottom);
    dl->AddRectFilled(min, ImVec2(max.x, min.y + 46.0f * s), kBar, radius,
                      ImDrawFlags_RoundCornersTop);
    dl->AddRectFilled(ImVec2(min.x, max.y - radius * 2.0f), max, kBodyBottom, radius,
                      ImDrawFlags_RoundCornersBottom);
    dl->AddRect(min, max, kRim, radius, 0, 3.0f * s);
    Text(dl, ImVec2(min.x + 18.0f * s, min.y + 8.0f * s), 30.0f * s, "Room Rules", kWhite);
    const int dots = int((NowMs() / 400) % 4);
    Text(dl, ImVec2(min.x + 22.0f * s, min.y + 62.0f * s), 26.0f * s,
         "The host is choosing the battle settings" + std::string(size_t(dots), '.'), kWhite);
  }

  rex::ui::ImmediateDrawer* immediate_drawer_;
  std::filesystem::path game_data_root_;
  std::filesystem::path config_path_;
  std::function<void(std::function<void()>)> defer_;
  std::shared_ptr<SharedPad> pad_;
  BurstLimitMenuArt* art_ = nullptr;
  bool art_ready_ = false;
  bool blocking_ = false;
  bool was_open_ = false;
  bool first_read_done_ = false;
  int panel_ = 0;
  int row_ = 0;
  uint16_t held_ = 0;
  int64_t repeat_at_ = 0;
};

}  // namespace

std::unique_ptr<rex::ui::ImGuiDialog> BurstLimitCreateOnlineRules(
    rex::ui::ImGuiDrawer* drawer, rex::ui::ImmediateDrawer* immediate_drawer,
    const std::filesystem::path& game_data_root, const std::filesystem::path& config_path,
    std::function<void(std::function<void()>)> defer) {
  return std::make_unique<RulesDialog>(drawer, immediate_drawer, game_data_root, config_path,
                                       std::move(defer));
}
