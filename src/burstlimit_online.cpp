// Online play helpers for the SDK's lobby layer (rex/net/online.h):
// - the build's version string (rooms are only listed between identical
//   builds) and the settings both players must share;
// - a short on-screen notice for lobby / connection messages;
// - an online stall counter for tests: how often the battle is held waiting
//   for the other player's input (logged as [OnlineStalls]).

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>

#include <imgui.h>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/memory.h>
#include <rex/memory/utils.h>
#include <rex/net/online.h>
#include <rex/net/session.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/overlay/overlay_text.h>

#ifndef BURSTLIMIT_VERSION
#define BURSTLIMIT_VERSION "dev"
#endif

namespace {

// Match input manager M (pointer), its driver state (3 = online match) and
// the battle object's flags, where bit 0x40000000 = held by the netcode
// (C:/rex/_online_tests/netcode_research.md 1.1).
constexpr uint32_t kInputManagerPointer = 0x841B5EA4;
constexpr uint32_t kDriverState = 1632;
constexpr uint32_t kDriverStateMatch = 3;
constexpr uint32_t kBattlePointer = 0x841B5130;
constexpr uint32_t kBattleFlags = 260;
constexpr uint32_t kBattleHeld = 0x40000000;

}  // namespace

// burstlimit_netinput.cpp
void BurstLimitNetInputSetup();
void BurstLimitNetInputFrame(rex::memory::Memory* memory, bool in_match);
void BurstLimitNetInputMatchEnd(const char* why);
int BurstLimitOnlineDelayFrames();
const char* BurstLimitOnlineHoldReason();
int BurstLimitOnlineLocalPlayer();
// burstlimit_rb2.cpp (v0.8.1 R8)
void BurstLimitOnlineGameNotice(const std::string& text, double seconds);
void BurstLimitOnlineAlert(const char* kind, const std::string& text);
std::string BurstLimitRb2MatchStats(bool reset);
bool BurstLimitRb2Active();

// v0.8.1 R8: what a player's log needs for "it is slow" - one line per hold of 2 ticks or more with its reason, and
// one summary line per match.
REXCVAR_DEFINE_BOOL(online_hold_log, true, "Patches",
                    "Online: log every hold of 2 ticks or more with its reason ([OnlineHold], at most 400 lines per match)");
REXCVAR_DEFINE_BOOL(online_match_summary, true, "Patches",
                    "Online: one [OnlineMatch] summary line when a match ends (held ticks, rollbacks, waits, gates)");
// v0.8.1 R8 (build 151): [OnlineAlert] - one warning line per kind per 10 s for what makes a match feel bad (a long
// hold, a ping spike, a slow or refused rollback, a guard or audit hit, a desync); the match summary counts them.
REXCVAR_DEFINE_BOOL(online_alerts, true, "Patches",
                    "Online: [OnlineAlert] warning lines (one per kind per 10 s) and their counts in the match summary");
// v0.8.1 R25 (build 151): a hold of this many ticks shows "Waiting for the other player..." (at most once per 20 s)
REXCVAR_DEFINE_INT32(online_wait_notice_ticks, 45, "Online",
                     "Online: a wait for the other player's input of this many ticks shows a notice (0 off)");

namespace {

int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
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

struct StallCounter {
  bool active = false;
  uint64_t ticks = 0, held = 0, holds = 0;
  uint32_t run = 0, longest = 0;
  // Window for the periodic line.
  uint64_t window_ticks = 0, window_held = 0, window_holds = 0;
  int64_t window_start_ms = 0;
  // per battle phase (0x841B5228: 0 intro, 1 READY?, 2 fight, 3 round end, 11, 4 round change, 5 post-round,
  // 13 results): ticks / held ticks / longest hold, for the match-total line
  uint64_t phase_ticks[16] = {}, phase_held[16] = {};
  uint32_t phase_longest[16] = {};
  // R8: the hold that is running (its first tick's reason and input frame), lines written, held ticks by reason
  const char* run_reason = "";
  int32_t run_frame = -1;
  uint32_t run_phase = 0;
  int64_t run_start_ms = 0;
  uint32_t hold_lines = 0;
  std::map<std::string, std::pair<uint32_t, uint32_t>> by_reason;  // reason -> holds, ticks
  bool rollback_seen = false;
} g_stalls;

// R8: a hold ended (or the match did): its line and its reason's totals
void EndHoldRun(StallCounter& s, int64_t now_ms) {
  if (s.run == 0) return;
  auto& e = s.by_reason[s.run_reason && *s.run_reason ? s.run_reason : "input not here yet"];
  ++e.first;
  e.second += s.run;
  if (s.run >= 2 && REXCVAR_GET(online_hold_log) && s.hold_lines < 400) {
    ++s.hold_lines;
    REXLOG_INFO("[OnlineHold] {} ticks ({} ms) before input frame {} in battle phase {}: {}", s.run, now_ms - s.run_start_ms, s.run_frame,
                s.run_phase, s.run_reason && *s.run_reason ? s.run_reason : "input not here yet");
  }
  s.run = 0;
}

// R8: the alert kinds of the running match, for the summary
std::mutex g_alert_mutex;
std::map<std::string, std::pair<int64_t, uint32_t>> g_alert_last;  // kind -> last line (ms), not shown since
std::map<std::string, uint32_t> g_alert_count;                     // kind -> alerts in this match
std::string AlertCounts(bool reset) {
  std::lock_guard<std::mutex> lock(g_alert_mutex);
  std::string out;
  for (const auto& [kind, n] : g_alert_count) out += fmt::format("{}{} {}", out.empty() ? "" : ", ", kind, n);
  if (reset) g_alert_count.clear();
  return out.empty() ? std::string("none") : out;
}

void FlushStalls(const char* why) {
  StallCounter& s = g_stalls;
  if (!s.active) {
    return;
  }
  const int delay = BurstLimitOnlineDelayFrames();
  REXLOG_WARN(
      "[OnlineStalls] match total ({}): ticks={} held={} ({:.1f}%) holds={} longest={} ticks "
      "delay={}",
      why, s.ticks, s.held, s.ticks ? 100.0 * double(s.held) / double(s.ticks) : 0.0, s.holds,
      s.longest, delay ? std::to_string(delay) + "f" : std::string("game"));
  std::string by_phase;
  for (int p = 0; p < 16; ++p) {
    if (!s.phase_ticks[p]) continue;
    by_phase += fmt::format(" | {}: {}/{} ({:.1f}%) longest {}", p, s.phase_held[p], s.phase_ticks[p],
                            100.0 * double(s.phase_held[p]) / double(s.phase_ticks[p]), s.phase_longest[p]);
  }
  if (!by_phase.empty()) REXLOG_WARN("[OnlineStalls] held by battle phase (held/ticks){}", by_phase);
  EndHoldRun(s, NowMs());
  if (REXCVAR_GET(online_match_summary)) {
    std::string reasons;
    for (const auto& [why_held, n] : s.by_reason) reasons += fmt::format(" | {}: {} holds {} ticks", why_held, n.first, n.second);
    const int me = BurstLimitOnlineLocalPlayer();
    REXLOG_WARN("[OnlineMatch] summary ({}): build {} | role {} | netcode {} | delay {} | ticks {} held {} ({:.1f}%) holds {} longest {} | fight "
                "(phase 2) ticks {} held {} ({:.1f}%) longest {} | held by reason{}",
                why, BURSTLIMIT_VERSION, me == 0 ? "host" : me == 1 ? "guest" : "-", s.rollback_seen ? "rollback" : "classic",
                delay ? std::to_string(delay) + "f" : std::string("game"), s.ticks, s.held,
                s.ticks ? 100.0 * double(s.held) / double(s.ticks) : 0.0, s.holds, s.longest, s.phase_ticks[2], s.phase_held[2],
                s.phase_ticks[2] ? 100.0 * double(s.phase_held[2]) / double(s.phase_ticks[2]) : 0.0, s.phase_longest[2],
                reasons.empty() ? std::string(" -") : reasons);
    if (s.rollback_seen) REXLOG_WARN("[OnlineMatch] engine: {}", BurstLimitRb2MatchStats(true));
    if (REXCVAR_GET(online_alerts)) REXLOG_WARN("[OnlineMatch] alerts: {}", AlertCounts(true));
  }
  s = StallCounter{};
  BurstLimitNetInputMatchEnd(why);
}

// A notice from the game's own code (BurstLimitOnlineGameNotice), shown like
// the online layer's when that has none.
std::mutex g_game_notice_mutex;
std::string g_game_notice;
std::chrono::steady_clock::time_point g_game_notice_at;
double g_game_notice_seconds = 0.0;

// The input delay line (changing it during a match), under the notices.
std::mutex g_delay_notice_mutex;
std::string g_delay_notice;
std::chrono::steady_clock::time_point g_delay_notice_at;
double g_delay_notice_seconds = 0.0;

bool GetDelayNotice(rex::net::online::Notice* out) {
  std::lock_guard<std::mutex> lock(g_delay_notice_mutex);
  if (g_delay_notice.empty()) {
    return false;
  }
  const double age =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - g_delay_notice_at).count();
  if (age >= g_delay_notice_seconds) {
    return false;
  }
  out->text = g_delay_notice;
  out->age_seconds = age;
  out->duration_seconds = g_delay_notice_seconds;
  return true;
}

bool GetGameNotice(rex::net::online::Notice* out) {
  std::lock_guard<std::mutex> lock(g_game_notice_mutex);
  if (g_game_notice.empty()) {
    return false;
  }
  const double age =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - g_game_notice_at).count();
  if (age >= g_game_notice_seconds) {
    return false;
  }
  out->text = g_game_notice;
  out->age_seconds = age;
  out->duration_seconds = g_game_notice_seconds;
  return true;
}

}  // namespace
bool BurstLimitOnlineOverlayLine(std::string* text, int* level);  // burstlimit_netinput.cpp
namespace {

class OnlineNotice final : public rex::ui::ImGuiDialog {
 public:
  explicit OnlineNotice(rex::ui::ImGuiDrawer* drawer) : ImGuiDialog(drawer) {}

 protected:
  void OnDraw(ImGuiIO& io) override {
    rex::net::online::Notice notice;
    if ((rex::net::online::GetNotice(&notice) && !notice.text.empty()) ||
        (GetGameNotice(&notice) && !notice.text.empty())) {
      DrawNotice(io, notice, 0.06f);
    }
    rex::net::online::Notice delay;
    if (GetDelayNotice(&delay) && !delay.text.empty()) {
      DrawNotice(io, delay, 0.215f);
    }
    // v0.8.1 R25: the connection line of an online fight (online_stats_overlay), bottom left, small
    std::string line;
    int level = 0;
    if (BurstLimitOnlineOverlayLine(&line, &level)) {
      ImGui::SetCurrentContext(io.Ctx);
      const float size = std::max(13.0f, io.DisplaySize.y * 0.019f);
      const ImVec2 text_size = rex::ui::overlay_text::Measure(size, line);
      const float pad = size * 0.4f;
      const ImVec2 position(io.DisplaySize.x * 0.012f + pad, io.DisplaySize.y * 0.988f - text_size.y - pad);
      ImDrawList* draw_list = ImGui::GetForegroundDrawList();
      draw_list->AddRectFilled(ImVec2(position.x - pad, position.y - pad * 0.5f),
                               ImVec2(position.x + text_size.x + pad, position.y + text_size.y + pad * 0.5f),
                               IM_COL32(0, 0, 0, 120), size * 0.3f);
      rex::ui::overlay_text::Draw(draw_list, size, position,
                                  level == 2 ? IM_COL32(255, 110, 110, 235) : level == 1 ? IM_COL32(255, 222, 110, 235)
                                                                                         : IM_COL32(255, 255, 255, 215),
                                  line, io.DisplaySize.x);
    }
  }

 private:
  void DrawNotice(ImGuiIO& io, const rex::net::online::Notice& notice, float top) {
    // ImGui is linked into both the runtime and this exe; draw in the
    // runtime's context.
    ImGui::SetCurrentContext(io.Ctx);
    float alpha = 1.0f;
    if (notice.age_seconds < 0.25) {
      alpha = float(notice.age_seconds / 0.25);
    } else if (notice.age_seconds > notice.duration_seconds - 0.5) {
      alpha = float((notice.duration_seconds - notice.age_seconds) / 0.5);
    }
    alpha = std::clamp(alpha, 0.0f, 1.0f);
    const float size = std::max(18.0f, io.DisplaySize.y * 0.03f);
    const float wrap = io.DisplaySize.x * 0.8f;
    const ImVec2 text_size = rex::ui::overlay_text::Measure(size, notice.text);
    const float width = std::min(text_size.x, wrap);
    const float lines = text_size.x > wrap ? std::ceil(text_size.x / wrap) : 1.0f;
    const float height = text_size.y * lines;
    const float pad = size * 0.5f;
    const ImVec2 position((io.DisplaySize.x - width) * 0.5f, io.DisplaySize.y * top);
    ImDrawList* draw_list = ImGui::GetForegroundDrawList();
    draw_list->AddRectFilled(ImVec2(position.x - pad, position.y - pad * 0.6f),
                             ImVec2(position.x + width + pad, position.y + height + pad * 0.6f),
                             IM_COL32(0, 0, 0, int(180 * alpha)), size * 0.3f);
    rex::ui::overlay_text::Draw(draw_list, size, position,
                                IM_COL32(255, 255, 255, int(255 * alpha)), notice.text, wrap);
  }
};

}  // namespace

// burstlimit_mods.cpp: "" without mods, else "mods.<hash of the files in use>".
std::string BurstLimitModsOnlineTag();

// The version string the lobby matches rooms by. Players with different mods
// would desync (other models, moves, stages), so the mods in use are part of
// it: rooms only match between identical mod sets (`mods`: "" or the mods'
// tag). Without mods the version is the build's alone. Called at startup and
// when the mods change.
void BurstLimitOnlineUpdateVersion(const std::string& mods) {
  std::string version = "burstlimit-" BURSTLIMIT_VERSION;
  if (!mods.empty()) {
    version += "+" + mods;
  }
  rex::net::online::SetGameVersion(version);
  REXLOG_INFO("Online: version {}", version);
}

// At startup (OnPreSetup, after BurstLimitModsSetup): what the lobby layer
// needs from the game.
void BurstLimitOnlineSetup() {
  BurstLimitOnlineUpdateVersion(BurstLimitModsOnlineTag());
  // A different frame driver step on the two PCs is a desync (the battle runs
  // step + 1 frames per input frame), so the guest takes the host's values.
  // The input delay and the per-frame input channel change what the driver
  // does with each frame, so they're shared the same way.
  // The Ki charge (burstlimit_kicharge.cpp) runs inside the battle, so its
  // settings must match too, and both players load the same start forms. A
  // Drama Piece (burstlimit_drama.cpp) changes the fight's timing: the host's
  // drama_pieces is used by both.
  rex::net::online::SetSyncedCvars({"online_tick_sleep", "online_fast_tick", "online_input_delay",
                                    "online_input_channel", "ki_charge", "ki_charge_rate",
                                    "start_forms", "drama_pieces", "online_netcode",
                                    "online_rollback_phase_delay",
                                    "online_rollback_skip_early_sync"});
  BurstLimitNetInputSetup();
}

// At startup, once the user data folder (the online identity) is known: in
// lobby mode the lobby connection starts in the background, so the online
// menu finds it ready (issue #22). Nothing in the LAN / VPN mode.
void BurstLimitOnlineConnectEarly() {
  rex::net::online::ConnectLobbyEarly();
}

// v0.8.1 R8: "[OnlineAlert] <kind>: <text>", at most one line per kind per 10 s (any thread; never takes the match lock)
void BurstLimitOnlineAlert(const char* kind, const std::string& text) {
  if (!REXCVAR_GET(online_alerts)) return;
  const int64_t now = NowMs();
  uint32_t hidden = 0;
  {
    std::lock_guard<std::mutex> lock(g_alert_mutex);
    ++g_alert_count[kind];
    auto& e = g_alert_last[kind];
    if (e.first != 0 && now - e.first < 10000) {
      ++e.second;
      return;
    }
    hidden = e.second;
    e = {now, 0};
  }
  REXLOG_WARN("[OnlineAlert] {}: {}{}", kind, text,
              hidden ? fmt::format(" ({} more of this kind since the last line)", hidden) : std::string());
}

void BurstLimitOnlineDelayNotice(const std::string& text, double seconds) {
  std::lock_guard<std::mutex> lock(g_delay_notice_mutex);
  g_delay_notice = text;
  g_delay_notice_at = std::chrono::steady_clock::now();
  g_delay_notice_seconds = seconds;
}

void BurstLimitOnlineGameNotice(const std::string& text, double seconds) {
  std::lock_guard<std::mutex> lock(g_game_notice_mutex);
  g_game_notice = text;
  g_game_notice_at = std::chrono::steady_clock::now();
  g_game_notice_seconds = seconds;
}

std::unique_ptr<rex::ui::ImGuiDialog> BurstLimitCreateOnlineNotice(rex::ui::ImGuiDrawer* drawer) {
  return std::make_unique<OnlineNotice>(drawer);
}

// burstlimit_online_rules.cpp: the Player Match room rules' tick.
void BurstLimitOnlineRulesFrame(rex::memory::Memory* memory);

// Every game tick (BurstLimitCameraFrame): count the frames the online battle
// is held waiting for input. One line every 10 s and a total per match.
void BurstLimitOnlineFrame(rex::memory::Memory* memory) {
  StallCounter& s = g_stalls;
  if (memory) {
    BurstLimitOnlineRulesFrame(memory);
  }
  if (!rex::net::IsGameSessionOpen() || !memory) {
    FlushStalls("session closed");
    if (memory) {
      BurstLimitNetInputFrame(memory, false);
    }
    return;
  }
  const uint32_t manager = LoadU32(memory, kInputManagerPointer);
  const uint32_t battle = LoadU32(memory, kBattlePointer);
  if (!Readable(memory, manager + kDriverState, 4) || !Readable(memory, battle + kBattleFlags, 4) ||
      LoadU32(memory, manager + kDriverState) != kDriverStateMatch) {
    FlushStalls("match ended");
    BurstLimitNetInputFrame(memory, false);
    return;
  }
  BurstLimitNetInputFrame(memory, true);
  const bool held = (LoadU32(memory, battle + kBattleFlags) & kBattleHeld) != 0;
  const int64_t now = NowMs();
  if (!s.active) {
    s.active = true;
    s.window_start_ms = now;
    BurstLimitRb2MatchStats(true);  // R8: the engine's counters of this match start here
  }
  if (BurstLimitRb2Active()) s.rollback_seen = true;
  ++s.ticks;
  ++s.window_ticks;
  const uint32_t phase = LoadU32(memory, 0x841B5228) & 15u;
  ++s.phase_ticks[phase];
  if (held) {
    ++s.phase_held[phase];
    s.phase_longest[phase] = std::max(s.phase_longest[phase], s.run + 1);
    ++s.held;
    ++s.window_held;
    if (++s.run == 1) {
      ++s.holds;
      ++s.window_holds;
      s.run_reason = BurstLimitOnlineHoldReason();
      s.run_frame = Readable(memory, manager + 1588, 4) ? int32_t(LoadU32(memory, manager + 1588)) : -1;
      s.run_phase = phase;
      s.run_start_ms = now;
    }
    s.longest = std::max(s.longest, s.run);
    // R8: a hold of 30 ticks in the fight (phase 2, the rollback engine running: not a cinematic's lockstep)
    if (s.run == 30 && phase == 2 && BurstLimitRb2Active())
      BurstLimitOnlineAlert("long hold", fmt::format("the fight waited 30 ticks for input frame {} ({}; ping {} ms)", s.run_frame,
                                                     s.run_reason && *s.run_reason ? s.run_reason : "input not here yet",
                                                     rex::net::online::PeerRttMs()));
    // R25: the player sees why the game stands still
    static int64_t last_wait_notice = 0;
    const int wait_ticks = REXCVAR_GET(online_wait_notice_ticks);
    if (wait_ticks > 0 && s.run == uint32_t(wait_ticks) && now - last_wait_notice >= 20000) {
      last_wait_notice = now;
      BurstLimitOnlineGameNotice("Waiting for the other player...", 2.5);
    }
  } else {
    EndHoldRun(s, now);
  }
  // R8: the ping, once per second: over 250 ms, or 80 ms more than a second ago
  static int64_t ping_checked = 0;
  static uint32_t ping_before = 0;
  if (now - ping_checked >= 1000) {
    ping_checked = now;
    const uint32_t rtt = rex::net::online::PeerRttMs();
    if (rtt > 250) BurstLimitOnlineAlert("high ping", fmt::format("{} ms (battle phase {})", rtt, phase));
    else if (ping_before && rtt >= ping_before + 80) BurstLimitOnlineAlert("ping jump", fmt::format("{} -> {} ms in a second (battle phase {})", ping_before, rtt, phase));
    ping_before = rtt;
  }
  if (now - s.window_start_ms >= 10000) {
    REXLOG_WARN("[OnlineStalls] 10s: ticks={} held={} ({:.1f}%) holds={} | match so far: "
                "holds={} longest={} ticks",
                s.window_ticks, s.window_held,
                s.window_ticks ? 100.0 * double(s.window_held) / double(s.window_ticks) : 0.0,
                s.window_holds, s.holds, s.longest);
    s.window_ticks = s.window_held = s.window_holds = 0;
    s.window_start_ms = now;
  }
}
