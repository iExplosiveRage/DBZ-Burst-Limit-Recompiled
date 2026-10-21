// Widescreen / ultrawide output (GitHub issue #13).
//
// The game keeps rendering into its 1280x720 frontbuffer, but with a wider
// view: the projection's X is narrowed by k = (16/9) / aspect (Hor+, the
// vertical view stays the game's), and the presenter shows the picture
// stretched back to `aspect` (anamorphic). The horizontal draw resolution
// scale is raised to match (draw_resolution_scale_x), so the stretch costs no
// sharpness. The 2D (HUD, menus, movies) is drawn back at its own proportions
// in the middle 16:9 of the screen through the GPU's overlay placement - the
// life bars can be moved out to the sides (widescreen_hud = edges).

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

#include <rex/cvar.h>
#include <rex/graphics/wide_aspect.h>
#include <rex/logging.h>
#include <rex/memory.h>
#include <rex/memory/utils.h>
#include <rex/net/session.h>
#include <rex/runtime.h>

REXCVAR_DEFINE_STRING(aspect_ratio, "16:9", "Patches",
                      "Picture shape: 16:9 (the original), 21:9 or 32:9 (ultrawide - wider view, "
                      "HUD kept in shape), or auto (the window's or screen's shape).")
    .allowed({"16:9", "21:9", "32:9", "auto"})
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_STRING(widescreen_hud, "edges", "Patches",
                      "HUD in a wider picture: edges (the life bars and the corners move out to "
                      "the sides) or center (all of it in the middle 16:9).")
    .allowed({"edges", "center"})
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_INT32(widescreen_debug_log, 0, "Debug",
                     "Log the next N 2D draws the widescreen option places.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(widescreen_debug_gpu_projection, false, "Debug",
                    "Widen the 3D scene on the GPU like online, also offline (for testing).")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace {

constexpr float kGuestAspect = 16.0f / 9.0f;

// k = guest aspect / output aspect, 1 = off. Read by the projection hooks
// (game thread) and the overlay placement (GPU thread).
std::atomic<float> g_scale_x{1.0f};
std::atomic<int32_t> g_debug_log_left{0};
std::atomic<bool> g_hud_edges{true};

float ConfiguredAspect() {
  const std::string mode = rex::cvar::GetFlagByName("aspect_ratio");
  if (mode == "21:9") {
    return 21.0f / 9.0f;
  }
  if (mode == "32:9") {
    return 32.0f / 9.0f;
  }
  if (mode == "auto") {
    uint32_t width = 0, height = 0;
    rex::graphics::GetOutputSurfaceSize(width, height);
    if (width && height) {
      return std::max(kGuestAspect, float(width) / float(height));
    }
  }
  return kGuestAspect;
}

// draw_resolution_scale_x for the picture's shape: as many guest pixels
// across as the output shows (Y scale * aspect / (16/9)), whole, at most 7 -
// exactly the Y scale at 16:9. The one place the X scale is made: the
// Resolution (burstlimit_patches.cpp), the graphics presets, a Y scale or
// aspect ratio change and an X scale set by hand all end here, so an X scale
// left from an earlier Resolution (8K -> 1440p kept 6x) can't stay behind.
// NVIDIA DLSS / AMD FSR take the X and Y scales each on its own.
int DrawScaleXForY(int y, float aspect) {
  y = std::clamp(y, 1, 7);
  if (aspect <= kGuestAspect * 1.001f) {
    return y;
  }
  return std::clamp(int(std::ceil(float(y) * aspect / kGuestAspect - 0.05f)), y, 7);
}

void ReconcileDrawScale(float aspect) {
  // The Y scale as the GPU takes it: resolution_scale when only that is set.
  const char* y_name = rex::cvar::HasNonDefaultValue("resolution_scale") &&
                               !rex::cvar::HasNonDefaultValue("draw_resolution_scale_y")
                           ? "resolution_scale"
                           : "draw_resolution_scale_y";
  int y = 1;
  try {
    y = std::stoi(rex::cvar::GetFlagByName(y_name));
  } catch (...) {
    return;
  }
  const int x = DrawScaleXForY(y, aspect);
  int current = 0;
  try {
    current = std::stoi(rex::cvar::GetFlagByName("draw_resolution_scale_x"));
  } catch (...) {
  }
  if (current != x) {
    REXLOG_INFO("Widescreen: aspect {:.3f}, draw_resolution_scale_x {} -> {} (y {})", aspect,
                current, x, y);
    rex::cvar::SetFlagByName("draw_resolution_scale_x", std::to_string(x));
  }
}

// Update() and BurstLimitSyncDrawScaleX(): from the polling thread and from
// cvar callbacks (also the ones the X scale change sets off, on the same
// thread).
std::recursive_mutex g_update_mutex;

// Online (and with widescreen_debug_gpu_projection) the GPU widens the
// scene instead of the game's projections.
bool GpuProjection() {
  return rex::net::IsGameSessionOpen() || REXCVAR_GET(widescreen_debug_gpu_projection);
}

void Update() {
  std::lock_guard<std::recursive_mutex> lock(g_update_mutex);
  const float aspect = ConfiguredAspect();
  const bool wide = aspect > kGuestAspect * 1.001f;
  const float scale_x = wide ? kGuestAspect / aspect : 1.0f;
  g_scale_x.store(scale_x, std::memory_order_relaxed);
  g_hud_edges.store(rex::cvar::GetFlagByName("widescreen_hud") != "center",
                    std::memory_order_relaxed);
  // Always explicit: the SDK's video mode follows window_width/height (the
  // Window size option), which would stretch this game's 1280x720 picture to
  // the window even with Keep aspect ratio on.
  rex::graphics::SetGuestDisplayAspect(wide ? aspect : kGuestAspect);
  // Online the GPU widens the scene (the game's camera stays as on the
  // other console - see WidenProjection in burstlimit_patches.cpp).
  rex::graphics::SetSceneProjectionAspectScale(GpuProjection() ? scale_x : 1.0f);
  ReconcileDrawScale(aspect);
}

// ---------------------------------------------------------------- 2D placement

// The game's 2D, with the sprite pixel shader EC9B5278:
// - sprites (rectangles, vs 4666C5A9): corner (+-0.5, +-0.5) * c0 / c1 + c3,
//   in normalized device coordinates (c3.x = the center);
// - text and shapes (triangle lists, vs 13B01BEA): position (float3 at the
//   start of a 48-byte vertex) * c0-c2 + c3, in pixels;
// - effects placed with the 3D camera (vs 1069B2C3: speed lines, flares) -
//   they already follow the wider projection, so they stay as they are.
// And the menu backgrounds (ps D9BA81CE).
constexpr uint64_t kSpritePixelShader = 0xEC9B52789DD888F2;
constexpr uint64_t kMenuPixelShader = 0xD9BA81CEA30CD4C4;
constexpr uint64_t kSpriteVertexShader = 0x4666C5A9092B03DD;
constexpr uint64_t kShapeVertexShader = 0x13B01BEA94678566;
// Menu pictures (ps D9BA81CE): position (float2 at the start of a 16-byte
// vertex) * c1, c2 + c4.
constexpr uint64_t kPictureVertexShader = 0x36FB87D39CDD870F;
constexpr uint64_t kProjectedVertexShader = 0x1069B2C3FEB5EFAE;
// The movies (OPENING.SFD, the endings): drawn with a depth test.
constexpr uint64_t kMoviePixelShader = 0xB6A55DAC2FB38CB8;
// Battle object pointer (non-zero during a match).
constexpr uint32_t kBattleObjectPointer = 0x841B5130;

bool IsOverlayPixelShader(uint64_t hash) {
  return hash == kSpritePixelShader || hash == kMenuPixelShader || hash == kMoviePixelShader;
}

struct BattleState {
  bool active = false;
  bool paused = false;
};

// In a match (its HUD may be up), and stopped (the pause menu - but also KO,
// time up, drama pieces and Ultimates freeze the match with the same bit, so
// the side HUD doesn't go by it: the HUD whitelist keeps the menus in place).
BattleState BattleShown() {
  BattleState state;
  auto* runtime = rex::Runtime::instance();
  auto* memory = runtime ? runtime->memory() : nullptr;
  if (!memory) {
    return state;
  }
  const uint32_t battle = rex::memory::load_and_swap<uint32_t>(
      memory->TranslateVirtual<const uint8_t*>(kBattleObjectPointer));
  if (!battle) {
    return state;
  }
  state.active = true;
  // Battle flags (+260): bit 0 = paused.
  state.paused =
      (rex::memory::load_and_swap<uint32_t>(memory->TranslateVirtual<const uint8_t*>(battle + 260)) &
       1) != 0;
  return state;
}

// What the draw covers, in normalized device coordinates (Y up).
struct Extent {
  float left = 0.0f, right = 0.0f, bottom = 0.0f, top = 0.0f;
};

bool OverlayExtent(const rex::graphics::OverlayDrawInfo& info, Extent& e) {
  const float* c = info.vertex_constants;
  if (info.vertex_shader_hash == kSpriteVertexShader) {
    const float half_x = 0.5f * (std::abs(c[0]) + std::abs(c[4]));
    const float half_y = 0.5f * (std::abs(c[1]) + std::abs(c[5]));
    e = {c[12] - half_x, c[12] + half_x, c[13] - half_y, c[13] + half_y};
    return true;
  }
  if (info.vertex_shader_hash == kPictureVertexShader && info.vertex_data) {
    constexpr uint32_t kStride = 16;
    const uint32_t count = std::min(info.vertex_count, info.vertex_data_size / kStride);
    if (!count) {
      return false;
    }
    e = {1e9f, -1e9f, 1e9f, -1e9f};
    for (uint32_t i = 0; i < count; ++i) {
      const uint8_t* vertex = info.vertex_data + i * kStride;
      const float x = rex::memory::load_and_swap<float>(vertex);
      const float y = rex::memory::load_and_swap<float>(vertex + 4);
      const float ndc_x = x * c[4] + y * c[8] + c[16];
      const float ndc_y = x * c[5] + y * c[9] + c[17];
      e.left = std::min(e.left, ndc_x);
      e.right = std::max(e.right, ndc_x);
      e.bottom = std::min(e.bottom, ndc_y);
      e.top = std::max(e.top, ndc_y);
    }
    return true;
  }
  if (info.vertex_shader_hash == kShapeVertexShader && info.vertex_data) {
    constexpr uint32_t kStride = 48;
    const uint32_t count = std::min(info.vertex_count, info.vertex_data_size / kStride);
    if (!count) {
      return false;
    }
    e = {1e9f, -1e9f, 1e9f, -1e9f};
    for (uint32_t i = 0; i < count; ++i) {
      const uint8_t* vertex = info.vertex_data + i * kStride;
      const float x = rex::memory::load_and_swap<float>(vertex);
      const float y = rex::memory::load_and_swap<float>(vertex + 4);
      const float z = rex::memory::load_and_swap<float>(vertex + 8);
      const float ndc_x = x * c[0] + y * c[4] + z * c[8] + c[12];
      const float ndc_y = x * c[1] + y * c[5] + z * c[9] + c[13];
      e.left = std::min(e.left, ndc_x);
      e.right = std::max(e.right, ndc_x);
      e.bottom = std::min(e.bottom, ndc_y);
      e.top = std::max(e.top, ndc_y);
    }
    return true;
  }
  return false;
}

// The pieces of the battle HUD that belong to a side (from the draws'
// logged with widescreen_debug_log): sprites of the life bar / portrait /
// ki gauge textures in the top and bottom bands, and the bars, names and
// gauge shapes drawn there as triangles, the ki-full lightning (152x72 sprite
// flickering over a full ki gauge) and the button prompts above the ki gauge
// (32x32 pictures, the Training input row), and each side's combo counter
// under its life bar ("4 HITS / 493 DAMAGE": 384x128 digits and words, the
// 1280x120 "DAMAGE", its 32x32 panel, the 384x128 sprite sliding in) - the
// band y 0.2-0.52, away from the middle.
bool IsSideHudPiece(const rex::graphics::OverlayDrawInfo& info, const Extent& e) {
  const uint32_t w = info.texture0_width, h = info.texture0_height;
  auto is = [w, h](uint32_t tw, uint32_t th) { return w == tw && h == th; };
  // Drama piece activation: the icon and its flash pieces flying out of the
  // life bar (drawn as sprites, pictures or - while turning - triangles).
  const bool drama = is(56, 36) || is(132, 68) || is(204, 60) || is(152, 48) || is(228, 64) ||
                     is(96, 64);
  // Which side: the piece reaches out towards that side's edge.
  const bool outer = (e.right <= 0.0f && e.left <= -0.6f) || (e.left >= 0.0f && e.right >= 0.6f);
  // The combo counter's band, under the life bar on its side.
  const bool combo = e.bottom >= 0.2f && e.top <= 0.52f && (e.right <= -0.3f || e.left >= 0.3f);
  if (info.vertex_shader_hash == kSpriteVertexShader ||
      info.vertex_shader_hash == kPictureVertexShader) {
    // Sprites and pictures (the portrait's hurt / KO faces are pictures).
    static constexpr uint32_t kTextures[][2] = {
        {516, 96},   // life bar
        {236, 268},  // portrait (all its faces)
        {108, 32},   // bar next to the timer
        {256, 256},  // ki gauge dragon
        {268, 128},  // ki gauge frame
        {512, 68},   // ki gauge segments
        {512, 32},   // ki gauge last segment
        {80, 80},    // ki gauge orb
        {152, 72},   // ki gauge full: the lightning over it
    };
    bool known = false;
    for (const auto& size : kTextures) {
      known |= is(size[0], size[1]);
    }
    // Button prompts (the Training input row) over the ki gauge: 32x32
    // pictures in the bottom band.
    const bool prompt = info.vertex_shader_hash == kPictureVertexShader && is(32, 32) &&
                        e.top <= -0.55f && e.bottom >= -0.95f;
    return (known && (e.bottom >= 0.45f || e.top <= -0.55f)) || prompt ||
           (info.vertex_shader_hash == kSpriteVertexShader && is(384, 128) && combo) ||
           (drama && e.bottom >= 0.4f);
  }
  if (info.vertex_shader_hash == kShapeVertexShader) {
    if (drama) {
      return e.bottom >= 0.4f;
    }
    // The combo counter (text, and the panel behind its damage line).
    if ((is(384, 128) || is(1280, 120) || is(32, 32)) && combo) {
      return true;
    }
    // Text: the names over the bars and a drama piece's name under its bar -
    // only when it reaches out to the side (a pause menu's title near the
    // middle stays in the menu).
    if (is(1280, 120) || is(512, 512) || is(384, 128)) {
      return e.bottom >= 0.5f && outer;
    }
    // Life bar fills at the top, ki gauge fills at the bottom.
    return e.bottom >= 0.55f || (e.top <= -0.7f && is(512, 68));
  }
  return false;
}

rex::graphics::OverlayPlacement PlaceOverlay(const rex::graphics::OverlayDrawInfo& info) {
  rex::graphics::OverlayPlacement placement;
  const float k = g_scale_x.load(std::memory_order_relaxed);
  if (k >= 0.999f) {
    return placement;
  }
  if ((!info.depth_test && !IsOverlayPixelShader(info.pixel_shader_hash)) ||
      info.vertex_shader_hash == kProjectedVertexShader) {
    return placement;  // the 3D scene's passes and effects
  }
  Extent e;
  const bool has_extent = OverlayExtent(info, e);
  const float left = e.left, right = e.right;
  // The copy of the 3D picture (frontbuffer-sized, or 1/2, 1/4) drawn back
  // without a depth test by a mesh that isn't one of the 2D shapes: a scene
  // effect with the sprite shader - the aura dome / burst, the dash and
  // vanish afterimages, distortion (vs 50BBA97B, camera-projected) - placed in
  // the wide view already. Squeezed into the 16:9 middle, the picture it
  // samples landed beside itself, and the frames that feed it back repeated
  // it in vertical strips.
  if (!has_extent && !info.depth_test && info.frontbuffer_width && info.frontbuffer_height) {
    for (uint32_t divisor : {1u, 2u, 4u}) {
      if (info.texture0_width == info.frontbuffer_width / divisor &&
          info.texture0_height == info.frontbuffer_height / divisor) {
        return placement;
      }
    }
  }
  const BattleState battle = BattleShown();
  // A screen-wide rectangle of a flat (tiny) texture - a fade, a dimmed
  // background - or the copy of the 3D picture drawn back without a depth
  // test (frontbuffer-sized, or 1/2, 1/4): it covers the whole wide screen.
  // Outside a match also the menus' screen-wide backdrops and bands (the
  // character select's background): stretched to the sides, the cards and
  // the text in front of them keep their shape.
  bool fill = false;
  if (has_extent && left <= -0.98f && right >= 0.98f) {
    const uint32_t w = info.texture0_width, h = info.texture0_height;
    // (Menu backdrops cover exactly the screen - not text sweeping across
    // it, like the Z Chronicles saga titles, which keeps its shape.)
    const bool screen_sized = left >= -1.02f && right <= 1.06f;
    fill = (w <= 32 && h <= 32) || (!battle.active && info.depth_test && screen_sized);
    for (uint32_t divisor : {1u, 2u, 4u}) {
      fill |= !info.depth_test && info.frontbuffer_width &&
              w == info.frontbuffer_width / divisor && h == info.frontbuffer_height / divisor;
    }
  }
  if (!fill) {
    placement.scale_x = k;
    // The battle HUD's sides - life bars, names, portraits at the top, the
    // ki gauges at the bottom - go to the edges of the wide screen. The rest
    // (timer, middle messages) stays where it is in
    // the 16:9 middle.
    if (has_extent && g_hud_edges.load(std::memory_order_relaxed) && battle.active &&
        IsSideHudPiece(info, e)) {
      if (right <= 0.0f) {
        placement.offset_x = -(1.0f - k);
      } else if (left >= 0.0f) {
        placement.offset_x = 1.0f - k;
      }
    }
  }
  if (int32_t log_left = g_debug_log_left.load(std::memory_order_relaxed); log_left > 0) {
    g_debug_log_left.store(log_left - 1, std::memory_order_relaxed);
    const float* c = info.vertex_constants;
    REXLOG_INFO(
        "Widescreen 2D: vs={:016X} ps={:016X} z={} prim={} n={} tex={}x{} c0=({:.3f} {:.3f}) "
        "c3=({:.3f} {:.3f} {:.3f} {:.3f}) x=[{:.3f} {:.3f}] y=[{:.3f} {:.3f}]{} -> {:.3f} {:+.3f}",
        info.vertex_shader_hash, info.pixel_shader_hash, int(info.depth_test),
        info.primitive_type, info.vertex_count, info.texture0_width, info.texture0_height, c[0],
        c[1], c[12], c[13], c[14], c[15], left, right, e.bottom, e.top, has_extent ? "" : "?",
        placement.scale_x,
        placement.offset_x);
  }
  return placement;
}

struct WidescreenSetup {
  WidescreenSetup() {
    rex::graphics::SetOverlayPlacementCallback(PlaceOverlay);
    rex::graphics::SetOverlayDepthTestedPixelShaders(&kMoviePixelShader, 1);
    for (const char* name : {"aspect_ratio", "draw_resolution_scale_y", "draw_resolution_scale_x",
                             "resolution_scale", "widescreen_hud"}) {
      rex::cvar::RegisterChangeCallback(name, [](std::string_view, std::string_view) { Update(); });
    }
    rex::cvar::RegisterChangeCallback("widescreen_debug_log",
                                      [](std::string_view, std::string_view value) {
                                        int n = 0;
                                        std::sscanf(std::string(value).c_str(), "%d", &n);
                                        g_debug_log_left.store(n, std::memory_order_relaxed);
                                      });
    // Follows the window's shape (auto) and online sessions.
    std::thread([] {
      for (;;) {
        Update();
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
      }
    }).detach();
  }
};

WidescreenSetup g_widescreen_setup;

}  // namespace

// The X scale for the game's projections (1 = 16:9), applied offline by
// WidenProjection (burstlimit_patches.cpp).
float BurstLimitWidescreenProjectionScaleX() {
  return GpuProjection() ? 1.0f : g_scale_x.load(std::memory_order_relaxed);
}

// draw_resolution_scale_x from the current Y scale and aspect ratio
// (DrawScaleXForY), for the Resolution and the graphics presets.
void BurstLimitSyncDrawScaleX() {
  std::lock_guard<std::recursive_mutex> lock(g_update_mutex);
  ReconcileDrawScale(ConfiguredAspect());
}
