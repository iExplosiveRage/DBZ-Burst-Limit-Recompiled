#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <windows.h>

#include <rex/cvar.h>
#include <rex/graphics/draw_overrides.h>
#include <rex/graphics/frame_pacing.h>
#include <rex/memory.h>
#include <rex/memory/utils.h>
#include <rex/net/session.h>
#include <rex/ppc/context.h>
#include <rex/runtime.h>

REXCVAR_DEFINE_BOOL(patch_60fps, false, "Patches",
                    "Enable the 60 FPS patch with pause and match-exit fixes (older setting, "
                    "used while frame_rate is empty).")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_STRING(frame_rate, "", "Patches",
                      "Frame rate cap: 30 (the original), 60, 120, 144 or unlocked. Empty = "
                      "from patch_60fps and vsync.")
    .allowed({"", "30", "60", "120", "144", "unlocked"})
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(online_fast_tick, true, "Patches",
                    "Online: use online_tick_sleep for the match frame driver (off = game default 3).")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_INT32(online_tick_sleep, 1, "Patches",
                     "Online: frames the match driver sleeps between input ticks (0 = LAN, 1 = good "
                     "ping, 2 = average ping, 3 = game default). Both players must use the same value.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(online_input_delay_test, true, "Patches",
                    "Online: lower the input buffer threshold at 0x82293A40 from 6 to 2.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

// The game's blur effects sample at fixed 720p distances. Above
// draw_resolution_scale 1 that turns into lines in the background and ghost
// copies around the characters, so they are off unless asked for.
REXCVAR_DEFINE_BOOL(depth_of_field, false, "Patches",
                    "Blur the background behind the fighters (depth of field).")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(glow_blur, false, "Patches",
                    "Soft glow blur (causes the halo around the characters at high resolution).")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(motion_blur, false, "Patches", "Directional blur during fast moves.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(soft_filter, false, "Patches",
                    "The game's soft filter over the whole picture (a 4-tap average sized for "
                    "720p - above it, it blurs the picture a lot).")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_STRING(window_size, "", "Patches",
                      "Window size in windowed mode in pixels, for example \"640x480\" (empty = "
                      "the default)");

REXCVAR_DEFINE_INT32(field_of_view, 100, "Patches",
                     "Field of view in percent of the game's (100 = original, 120 = 20% wider).")
    .range(50, 200)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace {

// Ucode hashes of the game's post-processing pixel shaders (from
// --dump_shaders).
// 5-tap background blur. Skipped, the scene copy it would blur is used as is.
constexpr uint64_t kDepthOfFieldBlurShader = 0x0A9E2DB7A0032F37;
// Depth of field composite used in some scenes: lerp(sharp, blurred,
// clamp(depth factor, c2.x, c2.y)), then the screen flash.
constexpr uint64_t kDepthOfFieldCompositeShader = 0xA71B2D3254A81E3C;
// 11-tap Gaussian blur.
constexpr uint64_t kGlowBlurShader = 0x97906B6915CE1A5E;
// 17-tap directional blur with a depth mask.
constexpr uint64_t kMotionBlurShaders[] = {0x1D256B3F80DB44A7, 0xD74A5DAE8A193E23};
// The last full-screen pass before the HUD: the frame, copied, drawn back as
// the average of 4 taps half a 720p pixel apart - a soft filter at 720p that
// blurs over several pixels at higher resolutions. Skipped, the frame stays
// as it was.
constexpr uint64_t kSoftFilterShader = 0x04FCB369A8343665;

void ApplyPostEffectSettings() {
  std::vector<rex::graphics::PixelShaderDrawOverride> overrides;
  if (!REXCVAR_GET(depth_of_field)) {
    overrides.push_back({kDepthOfFieldBlurShader, true, {}});
    // A blur factor of 0 keeps the sharp image (and the flash still works).
    rex::graphics::PixelShaderDrawOverride& dof = overrides.emplace_back();
    dof.ucode_hash = kDepthOfFieldCompositeShader;
    dof.constants.push_back({2, 0b0011, {0.0f, 0.0f, 0.0f, 0.0f}});
  }
  if (!REXCVAR_GET(glow_blur)) {
    overrides.push_back({kGlowBlurShader, true, {}});
  }
  if (!REXCVAR_GET(motion_blur)) {
    for (uint64_t shader : kMotionBlurShaders) {
      overrides.push_back({shader, true, {}});
    }
  }
  if (!REXCVAR_GET(soft_filter)) {
    overrides.push_back({kSoftFilterShader, true, {}});
  }
  rex::graphics::SetPixelShaderDrawOverrides("burstlimit_post_effects", std::move(overrides));
}

// Scene effects the game draws as screen-space sprites (W = 1) - they have to
// be told to follow the free camera's roll like the perspective geometry.
// 4A56087EF49DF636: the ki aura.
constexpr uint64_t kSceneSpriteShaders[] = {0x4A56087EF49DF636};

void ApplyFieldOfView() {
  // The field of view itself is applied where the game builds its projections
  // (BurstLimitSceneProjectionFov, BurstLimitProjectionFov), so the effects it
  // places on the screen follow it - offline. The GPU side rolls the scene for
  // the free camera, and scales it for the field of view online: its
  // depth-tested draws, the effects drawn after it without depth testing, and
  // the sprites below.
  rex::graphics::SetSceneProjectionScale(1.0f);
  rex::graphics::SetSceneProjectionUndepthedTriangles(true);

  std::vector<rex::graphics::PixelShaderDrawOverride> overrides;
  for (uint64_t shader : kSceneSpriteShaders) {
    rex::graphics::PixelShaderDrawOverride& sprite = overrides.emplace_back();
    sprite.ucode_hash = shader;
    sprite.scene_projection_all_vertices = true;
  }
  rex::graphics::SetPixelShaderDrawOverrides("burstlimit_scene_sprites", std::move(overrides));
}

struct PostEffectCvarCallbacks {
  PostEffectCvarCallbacks() {
    ApplyPostEffectSettings();
    ApplyFieldOfView();
    for (const char* name : {"depth_of_field", "glow_blur", "motion_blur", "soft_filter"}) {
      rex::cvar::RegisterChangeCallback(
          name, [](std::string_view, std::string_view) { ApplyPostEffectSettings(); });
    }
  }
};

PostEffectCvarCallbacks g_post_effect_cvar_callbacks;

// One scene upscaler at a time: turning NVIDIA DLSS on turns AMD FSR off and
// the other way around (with both on, DLSS would go first).
struct SceneUpscalerCvarCallbacks {
  SceneUpscalerCvarCallbacks() {
    for (auto [name, other] : {std::pair{"dlss_mode", "fsr_mode"},
                               std::pair{"fsr_mode", "dlss_mode"}}) {
      rex::cvar::RegisterChangeCallback(
          name, [other](std::string_view, std::string_view value) {
            if (value != "off" && rex::cvar::GetFlagByName(other) != "off") {
              rex::cvar::SetFlagByName(other, "off");
            }
          });
    }
  }
};

SceneUpscalerCvarCallbacks g_scene_upscaler_cvar_callbacks;

// window_size ("640x480", empty = the default) sets window_width / window_height, which the
// window follows right away (windowed mode).
struct WindowSizeCvarCallback {
  WindowSizeCvarCallback() {
    rex::cvar::RegisterChangeCallback("window_size", [](std::string_view, std::string_view value) {
      int width = 0, height = 0;
      if (std::sscanf(std::string(value).c_str(), "%dx%d", &width, &height) != 2 || width <= 0 ||
          height <= 0) {
        width = height = 0;
      }
      // In real pixels: window_width / window_height are in Windows' scaled pixels.
      const UINT dpi = GetDpiForSystem();
      if (dpi > 96) {
        width = int((int64_t(width) * 96 + dpi / 2) / dpi);
        height = int((int64_t(height) * 96 + dpi / 2) / dpi);
      }
      rex::cvar::SetFlagByName("window_width", std::to_string(width));
      rex::cvar::SetFlagByName("window_height", std::to_string(height));
    });
  }
};

WindowSizeCvarCallback g_window_size_cvar_callback;

// Guest frame interval (vblanks per game tick). The game writes 2 (30 FPS)
// through sub_82218940; the 60 FPS patch forces 1. The pause/match-quit code
// only runs when the tick counter at [0x825205E8]+3228 is non-zero, which
// never happens with interval 1, so the Skip hooks in the generated code must
// stay in place or START/pause locks up.
// With interval 2 the game holds itself to 30 FPS with a timer of its own
// (sub_82119D18). With 1 it waits for the next vblank, and its simulation
// follows the time that really passed, so the vblank rate sets the frame rate.
constexpr uint32_t kFpsCapAddress = 0x826DE600;
constexpr uint32_t kFpsCap30 = 2;
constexpr uint32_t kFpsCap60 = 1;

std::mutex g_patch_mutex;
bool g_fps_cap_applied = false;
// Frame interval 1 wanted (any frame rate but 30).
std::atomic<bool> g_interval_one{false};

void Apply60FpsDataPatch();

void ApplyFrameRate() {
  const std::string rate = REXCVAR_GET(frame_rate);
  if (rate.empty()) {
    // The older settings: patch_60fps, paced by vsync.
    g_interval_one.store(REXCVAR_GET(patch_60fps));
    rex::graphics::SetGuestVblankRate(0.0);
  } else {
    g_interval_one.store(rate != "30");
    double vblank_rate = 60.0;
    if (rate == "120") {
      vblank_rate = 120.0;
    } else if (rate == "144") {
      vblank_rate = 144.0;
    } else if (rate == "unlocked") {
      vblank_rate = 1000.0;
    }
    rex::graphics::SetGuestVblankRate(vblank_rate);
  }
  Apply60FpsDataPatch();
}

void Apply60FpsDataPatch() {
  auto* runtime = rex::Runtime::instance();
  if (!runtime) {
    return;
  }

  auto* memory = runtime->memory();
  if (!memory) {
    return;
  }

  auto* fps_cap = memory->TranslateVirtual<uint8_t*>(kFpsCapAddress);
  if (!fps_cap) {
    return;
  }

  std::lock_guard<std::mutex> lock(g_patch_mutex);

  const uint32_t current = rex::memory::load_and_swap<uint32_t>(fps_cap);

  if (g_interval_one.load()) {
    // Only override the game's own 30 FPS value; leave 0 (not initialized
    // yet) and any other mode the game picks alone.
    if (current == kFpsCap30) {
      rex::memory::store_and_swap<uint32_t>(fps_cap, kFpsCap60);
    }
    g_fps_cap_applied = true;
  } else if (g_fps_cap_applied) {
    // Always restore the game's real default instead of a value captured
    // at an arbitrary time (it could have been 0 before the game set it).
    if (current == kFpsCap60) {
      rex::memory::store_and_swap<uint32_t>(fps_cap, kFpsCap30);
    }
    g_fps_cap_applied = false;
  }
}

struct PatchCvarCallbacks {
  PatchCvarCallbacks() {
    for (const char* name : {"patch_60fps", "frame_rate"}) {
      rex::cvar::RegisterChangeCallback(
          name, [](std::string_view, std::string_view) { ApplyFrameRate(); });
    }
  }

  ~PatchCvarCallbacks() {
    rex::cvar::UnregisterChangeCallbacks("patch_60fps");
    rex::cvar::UnregisterChangeCallbacks("frame_rate");
  }
};

PatchCvarCallbacks g_patch_cvar_callbacks;

bool Is60FpsEnabled() {
  Apply60FpsDataPatch();
  return g_interval_one.load();
}

}  // namespace

// Called once the config and the command line are applied (values set there
// don't always go through the change callbacks).
void BurstLimitApplyPostEffectSettings() {
  ApplyPostEffectSettings();
  ApplyFieldOfView();
  // The free camera takes the controller away, never start in it.
  rex::cvar::SetFlagByName("free_camera", "false");
  rex::cvar::SetFlagByName("freeze_game", "false");
  // Configs from before the frame_rate option keep what they had.
  if (REXCVAR_GET(frame_rate).empty()) {
    const char* rate = "30";
    if (REXCVAR_GET(patch_60fps)) {
      rate = rex::cvar::Query<bool>("vsync") ? "60" : "unlocked";
    }
    rex::cvar::SetFlagByName("frame_rate", rate);
  }
  ApplyFrameRate();
}

// Mid-asm hooks, wired up in burstlimit_manifest.toml.

// li r3,2 before bl sub_82218940: force frame interval 1.
void BurstLimit60FpsForceCap(PPCRegister& r3) {
  if (Is60FpsEnabled()) {
    r3.u64 = 1;
  }
}

// rlwinm r11 = battle+574 & 2 (a cutscene is playing) before the rounding of
// the cutscene motion / camera time to whole frames: above 30 FPS the time is
// sampled as it is, so poses change every tick instead of every other one.
// Off online (the poses aren't shown to be local only).
void BurstLimitDramaSmoothMotion(PPCRegister& r11) {
  if (g_interval_one.load(std::memory_order_relaxed) && !rex::net::IsGameSessionOpen()) {
    r11.u64 = 0;
  }
}

// beq on "tick counter == 0" in the pause / match-quit paths.
void BurstLimit60FpsSkipTickGate(PPCCRRegister& cr6) {
  if (Is60FpsEnabled()) {
    cr6.eq = 0;
  }
}

namespace {

// field_of_view widens a projection matrix the game built: x and y over
// tan(fov / 2), the first and sixth floats. Not online: both consoles have to
// simulate the same match, and the game's camera may use its projections - so
// there the GPU widens the drawn scene instead, like before (effects the game
// places on the screen itself are then a bit off).
void WidenProjection(uint32_t matrix_address) {
  const int32_t percent = REXCVAR_GET(field_of_view);
  auto* runtime = rex::Runtime::instance();
  auto* memory = runtime ? runtime->memory() : nullptr;
  if (percent == 100 || percent <= 0 || !matrix_address || !memory ||
      rex::net::IsGameSessionOpen()) {
    return;
  }
  const float scale = 100.0f / float(percent);
  uint8_t* matrix = memory->TranslateVirtual<uint8_t*>(matrix_address);
  for (uint32_t offset : {0u, 20u}) {
    rex::memory::store_and_swap<float>(
        matrix + offset, rex::memory::load_and_swap<float>(matrix + offset) * scale);
  }
}

}  // namespace

// The game builds its projections from the camera settings at 0x84140EE0
// (FOV as an angle of 65536 per turn, aspect ratio, near, far) in two places,
// both widened here so they keep matching:
// - sub_820E7058, the scene's (right-handed) projection, every frame, right
//   before it hands the matrix at r4 to the renderer (bl sub_820EDAC0);
// - sub_82123FC0, the one the effects use to place themselves on the screen
//   (flares, speed lines, the screen areas they distort), after it wrote the
//   matrix at r30.
void BurstLimitSceneProjectionFov(PPCRegister& r4) {
  // Every frame: online, the GPU scales the drawn scene (see WidenProjection).
  const int32_t percent = REXCVAR_GET(field_of_view);
  rex::graphics::SetSceneProjectionScale(
      rex::net::IsGameSessionOpen() && percent > 0 ? 100.0f / float(percent) : 1.0f);
  WidenProjection(r4.u32);
}

void BurstLimitProjectionFov(PPCRegister& r30) {
  WidenProjection(r30.u32);
}

int BurstLimitOnlineDelayFrames();  // burstlimit_netinput.cpp

// li r4,3 before bl sub_82122310 in the online frame driver: task sleep ticks.
// With an exact input delay (online_input_delay) the driver runs every frame.
void BurstLimitOnlineDriverSleep(PPCRegister& r4) {
  if (BurstLimitOnlineDelayFrames() > 0) {
    r4.u64 = 0;
  } else if (REXCVAR_GET(online_fast_tick)) {
    const int32_t sleep = REXCVAR_GET(online_tick_sleep);
    r4.u64 = static_cast<uint64_t>(sleep < 0 ? 0 : (sleep > 3 ? 3 : sleep));
  }
}

// cmpwi cr6,r11,6: redo the compare against 2 for the online latency test.
void BurstLimitOnlineInputDelay(PPCRegister& r11, PPCCRRegister& cr6) {
  if (REXCVAR_GET(online_input_delay_test)) {
    const int32_t value = r11.s32;
    cr6.lt = value < 2;
    cr6.gt = value > 2;
    cr6.eq = value == 2;
  }
}

// The play time (Options > Status). The game counts the frames it presents
// and adds a second every <refresh rate> frames (60), so above 60 FPS
// (frame_rate) the play time ran 2-4x too fast. Mid-asm hook at 0x822192E8,
// inside the counter's critical section (r31 + 1512 points to the counters:
// +8 frames, +12 seconds, capped at 999:59:59), jumping to its end
// (0x8221933C): real seconds instead.
void BurstLimitPlayTime(PPCRegister& r31) {
  static std::chrono::steady_clock::time_point last{};
  static double pending = 0.0;
  const auto now = std::chrono::steady_clock::now();
  if (last != std::chrono::steady_clock::time_point{}) {
    // A long gap (the window was dragged, a breakpoint) counts as one second
    // at most, like the frames the game would have missed.
    pending += std::min(std::chrono::duration<double>(now - last).count(), 1.0);
  }
  last = now;
  if (pending < 1.0) {
    return;
  }
  auto* runtime = rex::Runtime::instance();
  auto* memory = runtime ? runtime->memory() : nullptr;
  if (!memory) {
    return;
  }
  const uint32_t counters =
      rex::memory::load_and_swap<uint32_t>(memory->TranslateVirtual<uint8_t*>(r31.u32 + 1512));
  if (!counters) {
    return;
  }
  constexpr uint32_t kMaxSeconds = 999 * 3600 + 59 * 60 + 59;
  uint8_t* seconds = memory->TranslateVirtual<uint8_t*>(counters + 12);
  uint32_t value = rex::memory::load_and_swap<uint32_t>(seconds);
  while (pending >= 1.0) {
    pending -= 1.0;
    if (value < kMaxSeconds) {
      ++value;
    }
  }
  rex::memory::store_and_swap<uint32_t>(seconds, value);
}
