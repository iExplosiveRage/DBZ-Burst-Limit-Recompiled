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
#include <memory>
#include <string>

#include <imgui.h>

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
} g_stalls;

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
  s = StallCounter{};
  BurstLimitNetInputMatchEnd(why);
}

class OnlineNotice final : public rex::ui::ImGuiDialog {
 public:
  explicit OnlineNotice(rex::ui::ImGuiDrawer* drawer) : ImGuiDialog(drawer) {}

 protected:
  void OnDraw(ImGuiIO& io) override {
    rex::net::online::Notice notice;
    if (!rex::net::online::GetNotice(&notice) || notice.text.empty()) {
      return;
    }
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
    const ImVec2 position((io.DisplaySize.x - width) * 0.5f, io.DisplaySize.y * 0.06f);
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
  // settings must match too.
  rex::net::online::SetSyncedCvars({"online_tick_sleep", "online_fast_tick", "online_input_delay",
                                    "online_input_channel", "ki_charge", "ki_charge_rate"});
  BurstLimitNetInputSetup();
}

std::unique_ptr<rex::ui::ImGuiDialog> BurstLimitCreateOnlineNotice(rex::ui::ImGuiDrawer* drawer) {
  return std::make_unique<OnlineNotice>(drawer);
}

// Every game tick (BurstLimitCameraFrame): count the frames the online battle
// is held waiting for input. One line every 10 s and a total per match.
void BurstLimitOnlineFrame(rex::memory::Memory* memory) {
  StallCounter& s = g_stalls;
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
  }
  ++s.ticks;
  ++s.window_ticks;
  if (held) {
    ++s.held;
    ++s.window_held;
    if (++s.run == 1) {
      ++s.holds;
      ++s.window_holds;
    }
    s.longest = std::max(s.longest, s.run);
  } else {
    s.run = 0;
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
