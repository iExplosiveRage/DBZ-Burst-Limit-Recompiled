// Button icons of the controller in use: the game only has small Xbox 360
// buttons in its pictures; they are redrawn with Xbox Series, PlayStation or
// Nintendo Switch buttons (button_icons), by physical position (Xbox A = the
// bottom face button = PlayStation Cross = Switch B, and so on), and every
// redrawn texture also gets a sharp version several times its size as an
// in-memory replacement texture. Done in memory as the game loads its
// textures; its files are left as they are.
//
// Where the game has its buttons (LONG2DATA_US.CPK, all DXT5, one mip):
// - UICMN_WINDOW.NUT (US) textures 0-7: A, B, X, Y, RT, RB, LB, LT, 32x32.
//   Every button prompt of the menus ("Select / Confirm / Back"), the icons
//   in text (tutorial speech bubbles, Drama Piece and command lists, popups)
//   and the training menu draw these. 8 (the up arrow) and 9 (the D-pad) are
//   the same on every controller and stay.
// - BUCPT_FRAME.NUT 28, 32, 33, 34: the fight HUD's "press repeatedly" A, B,
//   X, Y buttons (4 frames of a press each, beside a burst), 180x72.
// - BUTUT_360.NUT 0 (also UIOPT_360.NUT 0: the same picture) - the Xbox 360
//   controller of the tutorial and the controller settings - and 1 (its
//   A / X / B / Y highlights). Picture 0 is replaced as a whole by an Xbox
//   Series controller, a DualSense or a Joy-Con pair (see Op::kPicture).
// The rest of every texture is kept block for block. Textures are recognized
// by their data's hash (the US files'); another version of a file is left
// alone.
//
// The icons are Xelu's Free Controller Prompts (public domain, CC0), built
// into the exe (burstlimit_button_icons.inc, scripts/make_button_icons.py).
//
// Changing the setting: a texture made from now on gets the new icons at the
// NTXR loader; one the game already made (UICMN_WINDOW stays loaded for good)
// is redrawn in place in guest memory - the GPU side notices the write - as
// long as its memory still holds exactly the picture put there.

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <rex/cvar.h>
#include <rex/graphics/draw_overrides.h>
#include <rex/input/input_system.h>
#include <rex/logging.h>
#include <rex/memory.h>
#include <rex/memory/utils.h>
#include <rex/ppc/context.h>
#include <rex/runtime.h>
#include <rex/ui/image_decode.h>
#include <rex/ui/overlay/quick_menu.h>

#include "burstlimit_cpk.h"

REXCVAR_DEFINE_STRING(button_icons, "auto", "Patches",
                      "Button icons in the game's prompts: auto (the controller in use), xbox, "
                      "playstation or switch")
    .allowed({"auto", "xbox", "playstation", "switch"})
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace {

#include "burstlimit_button_icons.inc"

enum Style : int { kXbox = 0, kPlayStation = 1, kSwitch = 2, kStyleCount = 3 };
constexpr const char* kStyleNames[kStyleCount] = {"xbox", "playstation", "switch"};

// The Xbox buttons, by physical position.
enum class Button { kA, kB, kX, kY, kLB, kRB, kLT, kRT, kCount };

struct Png {
  const uint8_t* data;
  size_t size;
};

// The icon drawn for a button (none: the button is only cleared).
Png IconPng(int style, Button button) {
  if (style == kXbox) {
    switch (button) {
      case Button::kA: return {kXboxAPng, sizeof(kXboxAPng)};
      case Button::kB: return {kXboxBPng, sizeof(kXboxBPng)};
      case Button::kX: return {kXboxXPng, sizeof(kXboxXPng)};
      case Button::kY: return {kXboxYPng, sizeof(kXboxYPng)};
      case Button::kLB: return {kXboxLBPng, sizeof(kXboxLBPng)};
      case Button::kRB: return {kXboxRBPng, sizeof(kXboxRBPng)};
      case Button::kLT: return {kXboxLTPng, sizeof(kXboxLTPng)};
      case Button::kRT: return {kXboxRTPng, sizeof(kXboxRTPng)};
      default: return {nullptr, 0};
    }
  }
  if (style == kPlayStation) {
    switch (button) {
      case Button::kA: return {kPs5CrossPng, sizeof(kPs5CrossPng)};
      case Button::kB: return {kPs5CirclePng, sizeof(kPs5CirclePng)};
      case Button::kX: return {kPs5SquarePng, sizeof(kPs5SquarePng)};
      case Button::kY: return {kPs5TrianglePng, sizeof(kPs5TrianglePng)};
      case Button::kLB: return {kPs5L1Png, sizeof(kPs5L1Png)};
      case Button::kRB: return {kPs5R1Png, sizeof(kPs5R1Png)};
      case Button::kLT: return {kPs5L2Png, sizeof(kPs5L2Png)};
      case Button::kRT: return {kPs5R2Png, sizeof(kPs5R2Png)};
      default: return {nullptr, 0};
    }
  }
  if (style == kSwitch) {
    switch (button) {
      case Button::kA: return {kSwitchBPng, sizeof(kSwitchBPng)};
      case Button::kB: return {kSwitchAPng, sizeof(kSwitchAPng)};
      case Button::kX: return {kSwitchYPng, sizeof(kSwitchYPng)};
      case Button::kY: return {kSwitchXPng, sizeof(kSwitchXPng)};
      case Button::kLB: return {kSwitchLPng, sizeof(kSwitchLPng)};
      case Button::kRB: return {kSwitchRPng, sizeof(kSwitchRPng)};
      case Button::kLT: return {kSwitchZLPng, sizeof(kSwitchZLPng)};
      case Button::kRT: return {kSwitchZRPng, sizeof(kSwitchZRPng)};
      default: return {nullptr, 0};
    }
  }
  return {nullptr, 0};
}

// ---------------------------------------------------------------------------
// Pictures

// Premultiplied RGBA, 0-1.
struct Image {
  int width = 0;
  int height = 0;
  std::vector<std::array<float, 4>> pixels;

  Image() = default;
  Image(int w, int h) : width(w), height(h), pixels(size_t(w) * h, {0, 0, 0, 0}) {}
  std::array<float, 4>& at(int x, int y) { return pixels[size_t(y) * width + x]; }
  const std::array<float, 4>& at(int x, int y) const { return pixels[size_t(y) * width + x]; }
};

Image FromRgba8(const uint8_t* rgba, int width, int height) {
  Image image(width, height);
  for (size_t i = 0; i < image.pixels.size(); ++i) {
    const float a = rgba[i * 4 + 3] / 255.0f;
    image.pixels[i] = {rgba[i * 4] / 255.0f * a, rgba[i * 4 + 1] / 255.0f * a,
                       rgba[i * 4 + 2] / 255.0f * a, a};
  }
  return image;
}

void ToRgba8(const Image& image, uint8_t* rgba) {
  for (size_t i = 0; i < image.pixels.size(); ++i) {
    const auto& p = image.pixels[i];
    const float a = std::clamp(p[3], 0.0f, 1.0f);
    for (int c = 0; c < 3; ++c) {
      rgba[i * 4 + c] =
          a > 0.002f ? uint8_t(std::lround(std::clamp(p[c] / a, 0.0f, 1.0f) * 255.0f)) : 0;
    }
    rgba[i * 4 + 3] = uint8_t(std::lround(a * 255.0f));
  }
}

// Half the size (2x2 box average; an odd last row / column is averaged in).
Image HalfSize(const Image& src) {
  Image out(std::max(1, (src.width + 1) / 2), std::max(1, (src.height + 1) / 2));
  for (int y = 0; y < out.height; ++y) {
    for (int x = 0; x < out.width; ++x) {
      std::array<float, 4> sum = {0, 0, 0, 0};
      for (int dy = 0; dy < 2; ++dy) {
        for (int dx = 0; dx < 2; ++dx) {
          const auto& p = src.at(std::min(src.width - 1, x * 2 + dx),
                                 std::min(src.height - 1, y * 2 + dy));
          for (int c = 0; c < 4; ++c) {
            sum[c] += p[c] * 0.25f;
          }
        }
      }
      out.at(x, y) = sum;
    }
  }
  return out;
}

// `src` made `factor` times smaller (box filter).
Image Shrink(const Image& src, int factor) {
  Image out(src.width / factor, src.height / factor);
  const float weight = 1.0f / float(factor * factor);
  for (int y = 0; y < out.height; ++y) {
    for (int x = 0; x < out.width; ++x) {
      std::array<float, 4> sum = {0, 0, 0, 0};
      for (int dy = 0; dy < factor; ++dy) {
        for (int dx = 0; dx < factor; ++dx) {
          const auto& p = src.at(x * factor + dx, y * factor + dy);
          for (int c = 0; c < 4; ++c) {
            sum[c] += p[c] * weight;
          }
        }
      }
      out.at(x, y) = sum;
    }
  }
  return out;
}

float CatmullRom(float t) {
  t = std::fabs(t);
  if (t < 1.0f) {
    return 1.5f * t * t * t - 2.5f * t * t + 1.0f;
  }
  if (t < 2.0f) {
    return -0.5f * t * t * t + 2.5f * t * t - 4.0f * t + 2.0f;
  }
  return 0.0f;
}

// `src` made `factor` times bigger (Catmull-Rom, premultiplied, clamped).
Image Enlarge(const Image& src, int factor) {
  const int w = src.width * factor, h = src.height * factor;
  // Rows first, then columns.
  Image rows(w, src.height);
  for (int y = 0; y < src.height; ++y) {
    for (int x = 0; x < w; ++x) {
      const float sx = (float(x) + 0.5f) / float(factor) - 0.5f;
      const int x0 = int(std::floor(sx));
      std::array<float, 4> sum = {0, 0, 0, 0};
      for (int i = -1; i <= 2; ++i) {
        const float weight = CatmullRom(sx - float(x0 + i));
        const auto& p = src.at(std::clamp(x0 + i, 0, src.width - 1), y);
        for (int c = 0; c < 4; ++c) {
          sum[c] += p[c] * weight;
        }
      }
      rows.at(x, y) = sum;
    }
  }
  Image out(w, h);
  for (int y = 0; y < h; ++y) {
    const float sy = (float(y) + 0.5f) / float(factor) - 0.5f;
    const int y0 = int(std::floor(sy));
    for (int x = 0; x < w; ++x) {
      std::array<float, 4> sum = {0, 0, 0, 0};
      for (int j = -1; j <= 2; ++j) {
        const float weight = CatmullRom(sy - float(y0 + j));
        const auto& p = rows.at(x, std::clamp(y0 + j, 0, src.height - 1));
        for (int c = 0; c < 4; ++c) {
          sum[c] += p[c] * weight;
        }
      }
      sum[3] = std::clamp(sum[3], 0.0f, 1.0f);
      for (int c = 0; c < 3; ++c) {
        sum[c] = std::clamp(sum[c], 0.0f, sum[3]);
      }
      out.at(x, y) = sum;
    }
  }
  return out;
}

// An icon of the pack, the box of its visible pixels, and smaller copies of
// it (each half the one before), so it's drawn small without aliasing.
struct Icon {
  std::vector<Image> levels;
  int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
  bool ok = false;
};

std::mutex g_icons_mutex;
std::array<std::array<Icon, size_t(Button::kCount)>, kStyleCount> g_icons;
std::array<std::array<bool, size_t(Button::kCount)>, kStyleCount> g_icons_tried = {};

const Icon* GetIcon(int style, Button button) {
  std::lock_guard<std::mutex> lock(g_icons_mutex);
  Icon& icon = g_icons[style][size_t(button)];
  bool& tried = g_icons_tried[style][size_t(button)];
  if (!tried) {
    tried = true;
    const Png png = IconPng(style, button);
    if (png.data) {
      int width = 0, height = 0;
      const std::vector<uint8_t> rgba = rex::ui::DecodeImageRGBA(png.data, png.size, width, height);
      if (!rgba.empty()) {
        icon.levels.push_back(FromRgba8(rgba.data(), width, height));
        while (icon.levels.back().width > 4 && icon.levels.back().height > 4) {
          icon.levels.push_back(HalfSize(icon.levels.back()));
        }
        icon.x0 = width;
        icon.y0 = height;
        for (int y = 0; y < height; ++y) {
          for (int x = 0; x < width; ++x) {
            if (icon.levels[0].at(x, y)[3] > 8.0f / 255.0f) {
              icon.x0 = std::min(icon.x0, x);
              icon.y0 = std::min(icon.y0, y);
              icon.x1 = std::max(icon.x1, x + 1);
              icon.y1 = std::max(icon.y1, y + 1);
            }
          }
        }
        icon.ok = icon.x1 > icon.x0 && icon.y1 > icon.y0;
      }
      if (!icon.ok) {
        REXLOG_WARN("Button icons: an icon of the {} set couldn't be decoded", kStyleNames[style]);
      }
    }
  }
  return icon.ok ? &icon : nullptr;
}

// The controller pictures are made 4 times the size of the game's.
constexpr int kPictureScale = 4;

// The style's controller picture, at the
// game's size (scale 1) or kPictureScale.
const Image* GetPicture(int style, int scale) {
  static std::array<Image, kStyleCount> pictures;
  static std::array<Image, kStyleCount> small_pictures;
  static std::array<bool, kStyleCount> tried = {};
  std::lock_guard<std::mutex> lock(g_icons_mutex);
  if (!tried[style]) {
    tried[style] = true;
    const Png png = style == kXbox          ? Png{kXboxPadPng, sizeof(kXboxPadPng)}
                    : style == kPlayStation ? Png{kPs5PadPng, sizeof(kPs5PadPng)}
                    : style == kSwitch    ? Png{kSwitchPadPng, sizeof(kSwitchPadPng)}
                                          : Png{nullptr, 0};
    if (png.data) {
      int width = 0, height = 0;
      const std::vector<uint8_t> rgba = rex::ui::DecodeImageRGBA(png.data, png.size, width, height);
      if (!rgba.empty() && width % kPictureScale == 0 && height % kPictureScale == 0) {
        pictures[style] = FromRgba8(rgba.data(), width, height);
        small_pictures[style] = Shrink(pictures[style], kPictureScale);
      } else {
        REXLOG_WARN("Button icons: the {} controller picture couldn't be decoded",
                    kStyleNames[style]);
      }
    }
  }
  const Image& picture = scale == kPictureScale ? pictures[style] : small_pictures[style];
  return picture.width ? &picture : nullptr;
}

std::array<float, 4> Bilinear(const Image& image, float x, float y) {
  x -= 0.5f;
  y -= 0.5f;
  const int x0 = int(std::floor(x)), y0 = int(std::floor(y));
  const float fx = x - float(x0), fy = y - float(y0);
  std::array<float, 4> out = {0, 0, 0, 0};
  for (int dy = 0; dy < 2; ++dy) {
    const int yy = y0 + dy;
    if (yy < 0 || yy >= image.height) {
      continue;
    }
    const float wy = dy ? fy : 1.0f - fy;
    for (int dx = 0; dx < 2; ++dx) {
      const int xx = x0 + dx;
      if (xx < 0 || xx >= image.width) {
        continue;
      }
      const float w = wy * (dx ? fx : 1.0f - fx);
      const auto& p = image.at(xx, yy);
      for (int c = 0; c < 4; ++c) {
        out[c] += p[c] * w;
      }
    }
  }
  return out;
}

struct RectF {
  float x, y, w, h;
};

// Draws the icon's visible box into `rect` over `dst` (4x4 samples a pixel):
// `fit` keeps its shape (centered), otherwise it's stretched to the rect.
// `shadow`: drawn as a flat silhouette of that color.
void DrawIcon(Image& dst, const Icon& icon, RectF rect, bool fit, const float* shadow = nullptr) {
  const float iw = float(icon.x1 - icon.x0), ih = float(icon.y1 - icon.y0);
  float sx = rect.w / iw, sy = rect.h / ih;
  if (fit) {
    const float s = std::min(sx, sy);
    rect.x += (rect.w - iw * s) * 0.5f;
    rect.y += (rect.h - ih * s) * 0.5f;
    rect.w = iw * s;
    rect.h = ih * s;
    sx = sy = s;
  }
  // The level whose texels are no more than 2 per pixel drawn.
  size_t level = 0;
  float level_scale = 1.0f;
  while (level + 1 < icon.levels.size() && std::min(sx, sy) * level_scale < 0.5f) {
    ++level;
    level_scale *= 2.0f;
  }
  const Image& source = icon.levels[level];
  const float to_level = 1.0f / level_scale;
  const int kSamples = std::min(sx, sy) * level_scale >= 0.9f ? 2 : 4;
  const int px0 = std::max(0, int(std::floor(rect.x)));
  const int py0 = std::max(0, int(std::floor(rect.y)));
  const int px1 = std::min(dst.width, int(std::ceil(rect.x + rect.w)));
  const int py1 = std::min(dst.height, int(std::ceil(rect.y + rect.h)));
  for (int py = py0; py < py1; ++py) {
    for (int px = px0; px < px1; ++px) {
      std::array<float, 4> sum = {0, 0, 0, 0};
      for (int j = 0; j < kSamples; ++j) {
        const float y = float(py) + (float(j) + 0.5f) / kSamples;
        if (y < rect.y || y >= rect.y + rect.h) {
          continue;
        }
        for (int i = 0; i < kSamples; ++i) {
          const float x = float(px) + (float(i) + 0.5f) / kSamples;
          if (x < rect.x || x >= rect.x + rect.w) {
            continue;
          }
          const auto p = Bilinear(source, (float(icon.x0) + (x - rect.x) / sx) * to_level,
                                  (float(icon.y0) + (y - rect.y) / sy) * to_level);
          for (int c = 0; c < 4; ++c) {
            sum[c] += p[c];
          }
        }
      }
      for (float& c : sum) {
        c /= float(kSamples * kSamples);
      }
      if (shadow) {
        for (int c = 0; c < 3; ++c) {
          sum[c] = shadow[c] * sum[3];
        }
      }
      auto& d = dst.at(px, py);
      for (int c = 0; c < 4; ++c) {
        d[c] = sum[c] + d[c] * (1.0f - sum[3]);
      }
    }
  }
}

struct RectI {
  int x, y, w, h;
};

void Clear(Image& image, RectI r) {
  for (int y = std::max(0, r.y); y < std::min(image.height, r.y + r.h); ++y) {
    for (int x = std::max(0, r.x); x < std::min(image.width, r.x + r.w); ++x) {
      image.at(x, y) = {0, 0, 0, 0};
    }
  }
}

// The box of the visible pixels in `r` (the game's icon there).
bool VisibleBox(const Image& image, RectI r, RectF& box) {
  int x0 = r.x + r.w, y0 = r.y + r.h, x1 = r.x, y1 = r.y;
  for (int y = std::max(0, r.y); y < std::min(image.height, r.y + r.h); ++y) {
    for (int x = std::max(0, r.x); x < std::min(image.width, r.x + r.w); ++x) {
      if (image.at(x, y)[3] > 16.0f / 255.0f) {
        x0 = std::min(x0, x);
        y0 = std::min(y0, y);
        x1 = std::max(x1, x + 1);
        y1 = std::max(y1, y + 1);
      }
    }
  }
  if (x1 <= x0 || y1 <= y0) {
    return false;
  }
  box = {float(x0), float(y0), float(x1 - x0), float(y1 - y0)};
  return true;
}

// ---------------------------------------------------------------------------
// The game's textures with buttons

enum class Op {
  // The game's icon in the rect is cleared and the new one drawn in the box
  // its visible pixels took (same size and padding).
  kReplace,
  // kReplace with a thin light rim around the icon: the prompts sit on black
  // bars, where the pack's dark grey buttons (Switch, L1 / R2...) would be
  // lost; the game's own icons are white or bright.
  kReplaceRimmed,
  // A HUD button of the "press repeatedly" animation (its frame in the rect):
  // the button's top over a dark base, as deep as the game's frame.
  kHudButton,
  // The whole picture becomes the style's controller (the Xbox 360 pad of
  // Control Settings and the tutorial -> a DualSense / Joy-Cons on a grip,
  // made by scripts/make_button_icons.py with their face buttons where the
  // Xbox ones are, so the game's highlight rings still land on them).
  kPicture,
};

struct Draw {
  Op op;
  Button button;
  RectI rect;
};

struct Slot {
  const char* name;
  uint32_t data_size;
  uint16_t width, height;
  // FNV-1a 64 of the game's data (the US files).
  uint64_t original;
  // UICMN_WINDOW's texture index (the overlays draw these too), -1 others.
  int window_index;
  std::vector<Draw> draws;
};

const std::vector<Slot>& Slots() {
  static const std::vector<Slot> slots = [] {
    std::vector<Slot> s;
    constexpr RectI kWhole = {0, 0, 32, 32};
    struct WindowIcon {
      Button button;
      uint64_t original;
      const char* name;
    };
    constexpr WindowIcon kWindow[8] = {
        {Button::kA, 0xF814F36E3BC0F16Dull, "UICMN_WINDOW 0 (A)"},
        {Button::kB, 0x33F276AE5A85D530ull, "UICMN_WINDOW 1 (B)"},
        {Button::kX, 0x42DE427F434D669Full, "UICMN_WINDOW 2 (X)"},
        {Button::kY, 0x7DA2EA16E9FC924Cull, "UICMN_WINDOW 3 (Y)"},
        {Button::kRT, 0xC0D5715E71EDFEECull, "UICMN_WINDOW 4 (RT)"},
        {Button::kRB, 0x12F57A4358CBBFE9ull, "UICMN_WINDOW 5 (RB)"},
        {Button::kLB, 0x3B952460B77006BCull, "UICMN_WINDOW 6 (LB)"},
        {Button::kLT, 0x28DD0230E7100D00ull, "UICMN_WINDOW 7 (LT)"},
    };
    for (int i = 0; i < 8; ++i) {
      s.push_back({kWindow[i].name, 0x400, 32, 32, kWindow[i].original, i,
                   {{Op::kReplaceRimmed, kWindow[i].button, kWhole}}});
    }
    // The 4 frames of a press: raised, then pressed, in a 2x2 grid.
    constexpr RectI kFrames[4] = {{10, 0, 31, 36}, {60, 3, 31, 33}, {10, 41, 31, 30},
                                  {60, 43, 31, 28}};
    struct Hud {
      Button button;
      uint64_t original;
      const char* name;
    };
    constexpr Hud kHud[4] = {
        {Button::kA, 0xDD7BC20215C1E7FCull, "BUCPT_FRAME 28 (A)"},
        {Button::kB, 0x2A55A3820CCEB05Bull, "BUCPT_FRAME 32 (B)"},
        {Button::kX, 0x9A157AEEB8E8F9F9ull, "BUCPT_FRAME 33 (X)"},
        {Button::kY, 0x0FA550D690366254ull, "BUCPT_FRAME 34 (Y)"},
    };
    for (const Hud& hud : kHud) {
      Slot slot{hud.name, 0x32A0, 180, 72, hud.original, -1, {}};
      for (const RectI& frame : kFrames) {
        slot.draws.push_back({Op::kHudButton, hud.button, frame});
      }
      s.push_back(std::move(slot));
    }
    // The controller picture (its face buttons Y X B A around (379, 168)).
    s.push_back({"BUTUT_360 0 / UIOPT_360 0 (controller)",
                 0x24400,
                 464,
                 320,
                 0x510E72AAE108A92Aull,
                 -1,
                 {{Op::kPicture, Button::kCount, {0, 0, 464, 320}}}});
    s.push_back({"BUTUT_360 1 (A X B Y)",
                 0x8FF0,
                 196,
                 188,
                 0x7E9246CEDA23C5C9ull,
                 -1,
                 {{Op::kReplace, Button::kA, {103, 60, 37, 36}},
                  {Op::kReplace, Button::kX, {139, 60, 37, 36}},
                  {Op::kReplace, Button::kB, {103, 96, 37, 36}},
                  {Op::kReplace, Button::kY, {139, 96, 37, 36}}}});
    return s;
  }();
  return slots;
}

// Draws one style's buttons over `image` (the game's picture, or it made
// `scale` times bigger: the slot's places are scaled); `dirty` gets the 4x4
// blocks that changed (null: not needed).
void Restyle(Image& image, const Slot& slot, int style, std::vector<bool>* dirty, int scale = 1) {
  const float k = float(scale);
  const int blocks_x = (image.width + 3) / 4;
  auto touch = [&](RectI r) {
    if (!dirty) {
      return;
    }
    const int bx0 = std::max(0, (r.x - 1) / 4), by0 = std::max(0, (r.y - 1) / 4);
    const int bx1 = std::min(blocks_x - 1, (r.x + r.w + 1) / 4);
    const int by1 = std::min((image.height + 3) / 4 - 1, (r.y + r.h + 1) / 4);
    for (int by = by0; by <= by1; ++by) {
      for (int bx = bx0; bx <= bx1; ++bx) {
        (*dirty)[size_t(by) * blocks_x + bx] = true;
      }
    }
  };
  for (const Draw& draw : slot.draws) {
    const RectI r = {draw.rect.x * scale, draw.rect.y * scale, draw.rect.w * scale,
                     draw.rect.h * scale};
    const Icon* icon = draw.button != Button::kCount ? GetIcon(style, draw.button) : nullptr;
    switch (draw.op) {
      case Op::kReplace:
      case Op::kReplaceRimmed: {
        RectF box;
        if (!icon || !VisibleBox(image, r, box)) {
          break;
        }
        Clear(image, r);
        if (draw.op == Op::kReplaceRimmed) {
          const float kRim = 1.25f * k, kDiagonal = 0.9f * k;
          constexpr float kLight[3] = {232.0f / 255.0f, 232.0f / 255.0f, 236.0f / 255.0f};
          box = {box.x + kRim, box.y + kRim, box.w - 2.0f * kRim, box.h - 2.0f * kRim};
          const float kOffsets[8][2] = {{kRim, 0},           {-kRim, 0},
                                            {0, kRim},           {0, -kRim},
                                            {kDiagonal, kDiagonal},   {-kDiagonal, kDiagonal},
                                            {kDiagonal, -kDiagonal},  {-kDiagonal, -kDiagonal}};
          for (const auto& offset : kOffsets) {
            DrawIcon(image, *icon, {box.x + offset[0], box.y + offset[1], box.w, box.h}, true,
                     kLight);
          }
        }
        DrawIcon(image, *icon, box, true);
        touch(r);
        break;
      }
      case Op::kHudButton: {
        if (!icon) {
          break;
        }
        const RectI cleared = {r.x - 2 * scale, r.y - 2 * scale, r.w + 4 * scale,
                               r.h + 4 * scale};
        Clear(image, cleared);
        // The game's frames: the deeper the base under the button's top, the
        // less it's pressed.
        const int depth = std::clamp(draw.rect.h - draw.rect.w + 3, 2, 6) * scale;
        constexpr float kBase[3] = {16.0f / 255.0f, 16.0f / 255.0f, 20.0f / 255.0f};
        DrawIcon(image, *icon,
                 {float(r.x), float(r.y + depth), float(r.w), float(r.h - depth)}, false, kBase);
        DrawIcon(image, *icon, {float(r.x), float(r.y), float(r.w), float(r.h - depth)}, false);
        touch(cleared);
        break;
      }
      case Op::kPicture: {
        const Image* picture = GetPicture(style, scale == 1 ? 1 : kPictureScale);
        if (!picture || picture->width != image.width || picture->height != image.height) {
          break;
        }
        image.pixels = picture->pixels;
        touch(r);
        break;
      }
    }
  }
}

// The game's data (DXT5 in big-endian 16-bit words) with `style`'s buttons:
// only the blocks that changed are encoded again.
std::vector<uint8_t> MakeData(const Slot& slot, const std::vector<uint8_t>& original, int style) {
  BurstLimitNutTexture nut;
  nut.width = slot.width;
  nut.height = slot.height;
  nut.format = 2;
  nut.data = original.data();
  nut.base_size = original.size();
  BurstLimitTexture decoded;
  if (!BurstLimitDecodeNutTexture(nut, decoded)) {
    return {};
  }
  Image image = FromRgba8(decoded.rgba.data(), int(decoded.width), int(decoded.height));
  const int blocks_x = (slot.width + 3) / 4, blocks_y = (slot.height + 3) / 4;
  std::vector<bool> dirty(size_t(blocks_x) * blocks_y, false);
  Restyle(image, slot, style, &dirty);
  std::vector<uint8_t> rgba(image.pixels.size() * 4);
  ToRgba8(image, rgba.data());
  std::vector<uint8_t> out = original;
  for (int by = 0; by < blocks_y; ++by) {
    for (int bx = 0; bx < blocks_x; ++bx) {
      if (!dirty[size_t(by) * blocks_x + bx]) {
        continue;
      }
      uint8_t pixels[16][4];
      for (int i = 0; i < 16; ++i) {
        const int x = std::min(bx * 4 + (i & 3), int(slot.width) - 1);
        const int y = std::min(by * 4 + (i >> 2), int(slot.height) - 1);
        std::memcpy(pixels[i], &rgba[(size_t(y) * slot.width + x) * 4], 4);
      }
      uint8_t block[16];
      BurstLimitEncodeDxt5Block(pixels, block);
      uint8_t* dst = &out[(size_t(by) * blocks_x + bx) * 16];
      for (int i = 0; i < 16; i += 2) {
        dst[i] = block[i + 1];
        dst[i + 1] = block[i];
      }
    }
  }
  return out;
}

// How many times bigger than the game's texture the sharp version is made:
// the 32x32 icons 8 times (256x256), the others kPictureScale.
int HighResScale(const Slot& slot) {
  return slot.width <= 32 ? 8 : kPictureScale;
}

// Clear texels take the color of the visible ones next to them (a few texels
// out), so filtering at the edges of the icons doesn't darken them.
void BleedColors(std::vector<uint8_t>& rgba, int width, int height) {
  std::vector<uint8_t> known(size_t(width) * height);
  for (size_t i = 0; i < known.size(); ++i) {
    known[i] = rgba[i * 4 + 3] != 0;
  }
  for (int pass = 0; pass < 4; ++pass) {
    std::vector<uint8_t> next = known;
    for (int y = 0; y < height; ++y) {
      for (int x = 0; x < width; ++x) {
        const size_t i = size_t(y) * width + x;
        if (known[i]) {
          continue;
        }
        int sum[3] = {0, 0, 0}, count = 0;
        for (int dy = -1; dy <= 1; ++dy) {
          for (int dx = -1; dx <= 1; ++dx) {
            const int nx = x + dx, ny = y + dy;
            if (nx < 0 || ny < 0 || nx >= width || ny >= height) {
              continue;
            }
            const size_t n = size_t(ny) * width + nx;
            if (known[n]) {
              for (int c = 0; c < 3; ++c) {
                sum[c] += rgba[n * 4 + c];
              }
              ++count;
            }
          }
        }
        if (count) {
          for (int c = 0; c < 3; ++c) {
            rgba[i * 4 + c] = uint8_t(sum[c] / count);
          }
          next[i] = 1;
        }
      }
    }
    known.swap(next);
  }
}

// The sharp version of a slot in `style` (RGBA8, HighResScale times the
// size): the game's picture enlarged, the buttons drawn at that size.
std::vector<uint8_t> MakeHighRes(const Slot& slot, const std::vector<uint8_t>& original,
                                 int style) {
  BurstLimitNutTexture nut;
  nut.width = slot.width;
  nut.height = slot.height;
  nut.format = 2;
  nut.data = original.data();
  nut.base_size = original.size();
  BurstLimitTexture decoded;
  if (!BurstLimitDecodeNutTexture(nut, decoded)) {
    return {};
  }
  const int scale = HighResScale(slot);
  Image image =
      Enlarge(FromRgba8(decoded.rgba.data(), int(decoded.width), int(decoded.height)), scale);
  Restyle(image, slot, style, nullptr, scale);
  std::vector<uint8_t> rgba(image.pixels.size() * 4);
  ToRgba8(image, rgba.data());
  BleedColors(rgba, image.width, image.height);
  return rgba;
}

uint64_t Fnv1a64(const uint8_t* data, size_t size) {
  uint64_t hash = 0xCBF29CE484222325ull;
  for (size_t i = 0; i < size; ++i) {
    hash ^= data[i];
    hash *= 0x100000001B3ull;
  }
  return hash;
}

uint32_t Be32(const uint8_t* p) {
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}
uint16_t Be16(const uint8_t* p) {
  return uint16_t(p[0] << 8 | p[1]);
}

// Xenos 2D tiling (XGAddress2DTiledOffset) of a block, in bytes.
uint32_t TiledOffset(uint32_t x, uint32_t y, uint32_t pitch_blocks) {
  constexpr uint32_t kBytesPerBlockLog2 = 4;  // DXT5
  const uint32_t pitch = (pitch_blocks + 31) & ~31u;
  const uint32_t macro = ((x >> 5) + (y >> 5) * (pitch >> 5)) << (kBytesPerBlockLog2 + 7);
  const uint32_t micro = ((x & 7) + ((y & 0xE) << 2)) << kBytesPerBlockLog2;
  const uint32_t offset = macro + ((micro & ~0xFu) << 1) + (micro & 0xF) + ((y & 1) << 4);
  return ((offset & ~0x1FFu) << 3) + ((y & 16) << 7) + ((offset & 0x1C0) << 2) +
         (((((y & 8) >> 2) + (x >> 3)) & 3) << 6) + (offset & 0x3F);
}

// ---------------------------------------------------------------------------
// State

struct SlotState {
  std::vector<uint8_t> original;  // the game's data, once seen
  std::array<std::vector<uint8_t>, kStyleCount> made;
  std::array<uint64_t, kStyleCount> made_hash = {};
  bool logged = false;
};

// A texture made from one of ours: where the game tiled its picture.
struct LiveTexture {
  size_t slot;
  int style;
  uint32_t address;
};

std::mutex g_mutex;
std::vector<SlotState> g_states;
// NTXR header -> slot and style written there, until the texture is made.
std::unordered_map<uint32_t, std::pair<size_t, int>> g_pending;
std::vector<LiveTexture> g_live;
std::filesystem::path g_game_data_root;

std::atomic<int> g_style{kXbox};       // the style in effect
std::atomic<int> g_auto_style{kXbox};  // auto: the last controller's
int g_applied_style = kXbox;           // game thread: what live textures show
uint32_t g_frame = 0;

// The data of `slot` for `style` (empty when it can't be made). g_mutex held.
// The sharp versions being made (on other threads). Never freed: a job may
// still run while the program exits.
auto& g_high_res_jobs = *new std::vector<std::future<void>>;

// Makes the sharp version of a slot's `made` data in the background and gives
// it to the texture cache as an in-memory replacement of that texture (keyed
// by its data as the GPU reads it): the game samples it instead of the small
// DXT5 picture, with or without a texture pack. g_mutex held.
void QueueHighRes(size_t index, int style, const std::vector<uint8_t>& made,
                  const std::vector<uint8_t>& original) {
  const Slot& slot = Slots()[index];
  const uint64_t key =
      rex::graphics::TiledTexture2DReplacementKey(made.data(), slot.width, slot.height, 16);
  if (!key) {
    return;
  }
  std::erase_if(g_high_res_jobs, [](const std::future<void>& job) {
    return job.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
  });
  g_high_res_jobs.push_back(std::async(std::launch::async, [index, style, key, original] {
    const Slot& slot = Slots()[index];
    const int scale = HighResScale(slot);
    std::vector<uint8_t> rgba = MakeHighRes(slot, original, style);
    if (rgba.empty()) {
      return;
    }
    rex::graphics::SetTextureMemoryReplacement(key, slot.width * scale, slot.height * scale,
                                               std::move(rgba));
    REXLOG_INFO("Button icons: {} ({}) at {}x{} (key {:016x})", slot.name, kStyleNames[style],
                slot.width * scale, slot.height * scale, key);
  }));
}

const std::vector<uint8_t>& DataFor(size_t index, int style) {
  SlotState& state = g_states[index];
  std::vector<uint8_t>& made = state.made[style];
  if (made.empty() && !state.original.empty()) {
    made = MakeData(Slots()[index], state.original, style);
    state.made_hash[style] = made.empty() ? 0 : Fnv1a64(made.data(), made.size());
    if (made.empty()) {
      REXLOG_WARN("Button icons: {} couldn't be redrawn", Slots()[index].name);
    } else {
      QueueHighRes(index, style, made, state.original);
    }
  }
  return made;
}

int StyleFromGamepadType(uint8_t type) {
  switch (type) {
    case 4:  // PS3
    case 5:  // PS4
    case 6:  // PS5
      return kPlayStation;
    case 7:   // Switch Pro
    case 8:   // Joy-Con left
    case 9:   // Joy-Con right
    case 10:  // Joy-Con pair
      return kSwitch;
    case 0:  // unknown, the keyboard or nothing: as before
      return -1;
    default:
      return kXbox;
  }
}

int ResolveStyle(bool poll_controller) {
  const std::string value = REXCVAR_GET(button_icons);
  if (value == "xbox") {
    return kXbox;
  }
  if (value == "playstation") {
    return kPlayStation;
  }
  if (value == "switch") {
    return kSwitch;
  }
  if (poll_controller) {
    auto* runtime = rex::Runtime::instance();
    auto* input =
        runtime ? static_cast<rex::input::InputSystem*>(runtime->input_system()) : nullptr;
    if (input) {
      const int style = StyleFromGamepadType(input->GetGamepadType(input->GetLastUsedUser()));
      if (style >= 0 && style != g_auto_style.load()) {
        g_auto_style.store(style);
        REXLOG_INFO("Button icons: auto - {} controller", kStyleNames[style]);
      }
    }
  }
  return g_auto_style.load();
}

rex::ui::ButtonGlyphs Glyphs(int style) {
  return style == kPlayStation ? rex::ui::ButtonGlyphs::kPlayStation
         : style == kSwitch    ? rex::ui::ButtonGlyphs::kNintendo
                               : rex::ui::ButtonGlyphs::kXbox;
}

// Whether `memory` at a live texture still holds `data`'s blocks, tiled.
bool HoldsTiled(const uint8_t* memory, const Slot& slot, const std::vector<uint8_t>& data) {
  const uint32_t blocks_x = (slot.width + 3) / 4, blocks_y = (slot.height + 3) / 4;
  for (uint32_t by = 0; by < blocks_y; ++by) {
    for (uint32_t bx = 0; bx < blocks_x; ++bx) {
      if (std::memcmp(memory + TiledOffset(bx, by, blocks_x),
                      &data[(size_t(by) * blocks_x + bx) * 16], 16) != 0) {
        return false;
      }
    }
  }
  return true;
}

void WriteTiled(uint8_t* memory, const Slot& slot, const std::vector<uint8_t>& data) {
  const uint32_t blocks_x = (slot.width + 3) / 4, blocks_y = (slot.height + 3) / 4;
  for (uint32_t by = 0; by < blocks_y; ++by) {
    for (uint32_t bx = 0; bx < blocks_x; ++bx) {
      std::memcpy(memory + TiledOffset(bx, by, blocks_x),
                  &data[(size_t(by) * blocks_x + bx) * 16], 16);
    }
  }
}

rex::memory::Memory* GuestMemory() {
  auto* runtime = rex::Runtime::instance();
  return runtime ? runtime->memory() : nullptr;
}

// Whether the whole tiled picture of `slot` at `address` is committed memory
// (a texture freed since may not be).
bool Committed(rex::memory::Memory* memory, uint32_t address, const Slot& slot) {
  const uint32_t blocks_x = (slot.width + 3) / 4, blocks_y = (slot.height + 3) / 4;
  const uint32_t size = ((blocks_x + 31) & ~31u) * ((blocks_y + 31) & ~31u) * 16;
  for (uint32_t at = address; at < address + size;) {
    auto* heap = memory->LookupHeap(at);
    rex::memory::HeapAllocationInfo info = {};
    if (!heap || !heap->QueryRegionInfo(at, &info) ||
        !(info.state & rex::memory::kMemoryAllocationCommit) || !info.region_size) {
      return false;
    }
    at = info.base_address + info.region_size;
  }
  return true;
}

}  // namespace

// The style in effect: 0 Xbox, 1 PlayStation, 2 Switch. Any thread.
int BurstLimitButtonStyle() {
  return g_style.load();
}

// One of UICMN_WINDOW.NUT's textures (`index`, decoded as the game has it)
// with `style`'s button, made HighResScale times bigger (sharp at any size
// the overlays draw it); false when it has no button to redraw.
bool BurstLimitRestyleWindowIcon(int index, int style, BurstLimitTexture& image) {
  if (image.rgba.empty()) {
    return false;
  }
  for (const Slot& slot : Slots()) {
    if (slot.window_index != index || image.width != slot.width || image.height != slot.height) {
      continue;
    }
    const int scale = HighResScale(slot);
    Image picture =
        Enlarge(FromRgba8(image.rgba.data(), int(image.width), int(image.height)), scale);
    Restyle(picture, slot, style, nullptr, scale);
    image.width = uint32_t(picture.width);
    image.height = uint32_t(picture.height);
    image.rgba.assign(picture.pixels.size() * 4, 0);
    ToRgba8(picture, image.rgba.data());
    std::vector<uint8_t> rgba = std::move(image.rgba);
    BleedColors(rgba, picture.width, picture.height);
    image.rgba = std::move(rgba);
    return true;
  }
  return false;
}

// From the app's OnPreSetup.
void BurstLimitButtonsSetup(const std::filesystem::path& game_data_root) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_game_data_root = game_data_root;
  g_states.resize(Slots().size());
  g_style.store(ResolveStyle(false));
  rex::ui::SetButtonGlyphs(Glyphs(g_style.load()));
}

// Every game tick (from BurstLimitCameraFrame, game thread): follows the
// setting and the controller in use, and redraws the textures already made.
void BurstLimitButtonsFrame(rex::memory::Memory* memory) {
  // The controller is looked at a few times a second.
  const int style = ResolveStyle(g_frame++ % 15 == 0);
  if (style != g_style.load()) {
    g_style.store(style);
    rex::ui::SetButtonGlyphs(Glyphs(style));
    REXLOG_INFO("Button icons: {}", kStyleNames[style]);
  }
  if (style == g_applied_style || !memory) {
    return;
  }
  g_applied_style = style;
  std::lock_guard<std::mutex> lock(g_mutex);
  size_t redrawn = 0, gone = 0;
  for (auto it = g_live.begin(); it != g_live.end();) {
    const Slot& slot = Slots()[it->slot];
    const std::vector<uint8_t>& shown = DataFor(it->slot, it->style);
    const std::vector<uint8_t>& wanted = DataFor(it->slot, style);
    uint8_t* host = memory->TranslateVirtual<uint8_t*>(it->address);
    if (shown.empty() || wanted.empty() || !Committed(memory, it->address, slot) ||
        !HoldsTiled(host, slot, shown)) {
      // Freed, or something else is there now.
      it = g_live.erase(it);
      ++gone;
      continue;
    }
    WriteTiled(host, slot, wanted);
    it->style = style;
    ++redrawn;
    ++it;
  }
  REXLOG_INFO("Button icons: {} loaded textures redrawn ({} no longer loaded)", redrawn, gone);
}

// From the NTXR loader hook at 0x820EB780 (BurstLimitBrandingTexture), before
// the texture is made: r26 = the NTXR version, r29 = the texture's index, r30
// = its header (data at +0x50).
void BurstLimitButtonsTexture(PPCRegister& r26, PPCRegister& r29, PPCRegister& r30) {
  (void)r29;
  if (r26.u32 != 1 || !r30.u32) {
    return;
  }
  auto* memory = GuestMemory();
  if (!memory) {
    return;
  }
  uint8_t* header = memory->TranslateVirtual<uint8_t*>(r30.u32);
  constexpr uint32_t kHeaderSize = 0x50;
  if (Be16(header + 12) != kHeaderSize || Be16(header + 16) != 1 || Be16(header + 18) != 2) {
    return;  // not one mip of DXT5
  }
  const uint32_t data_size = Be32(header + 8);
  const uint16_t width = Be16(header + 20), height = Be16(header + 22);
  const std::vector<Slot>& slots = Slots();
  bool candidate = false;
  for (const Slot& slot : slots) {
    candidate |= slot.data_size == data_size && slot.width == width && slot.height == height;
  }
  if (!candidate) {
    return;
  }
  uint8_t* data = header + kHeaderSize;
  const uint64_t hash = Fnv1a64(data, data_size);
  const int style = g_style.load();
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_states.size() != slots.size()) {
    g_states.resize(slots.size());
  }
  for (size_t i = 0; i < slots.size(); ++i) {
    const Slot& slot = slots[i];
    if (slot.data_size != data_size || slot.width != width || slot.height != height) {
      continue;
    }
    SlotState& state = g_states[i];
    // The game's picture, or ours from an earlier load of the same file.
    bool ours = hash == slot.original;
    for (int s = 0; s < kStyleCount && !ours; ++s) {
      ours = state.made_hash[s] && hash == state.made_hash[s];
    }
    if (!ours) {
      continue;
    }
    if (state.original.empty() && hash == slot.original) {
      state.original.assign(data, data + data_size);
    }
    const std::vector<uint8_t>& wanted = DataFor(i, style);
    if (wanted.size() != data_size) {
      return;
    }
    if (std::memcmp(data, wanted.data(), data_size) != 0) {
      std::memcpy(data, wanted.data(), data_size);
    }
    if (!state.logged) {
      state.logged = true;
      REXLOG_INFO("Button icons: {} ({})", slot.name, kStyleNames[style]);
    }
    if (g_pending.size() > 64) {
      g_pending.clear();
    }
    g_pending[r30.u32] = {i, style};
    return;
  }
}

// Mid-asm hook at 0x820F8B54 in sub_820F89F8 (fills a texture: allocates its
// memory, [r31 + 80], and tiles the data of the NTXR header r28 into it with
// sub_82512BC0), right after the tiling: remembers where our pictures went.
void BurstLimitButtonsTextureMade(PPCRegister& r28, PPCRegister& r31) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_pending.empty()) {
    return;
  }
  auto it = g_pending.find(r28.u32);
  if (it == g_pending.end()) {
    return;
  }
  auto* memory = GuestMemory();
  if (!memory || !r31.u32) {
    return;
  }
  const uint32_t address =
      rex::memory::load_and_swap<uint32_t>(memory->TranslateVirtual<uint8_t*>(r31.u32 + 80));
  const auto [slot, style] = it->second;
  g_pending.erase(it);
  if (!address) {
    return;
  }
  // The same memory again (the file loaded anew): only the latest counts.
  std::erase_if(g_live, [address](const LiveTexture& live) { return live.address == address; });
  if (g_live.size() >= 64) {
    g_live.erase(g_live.begin());
  }
  g_live.push_back({slot, style, address});
}

namespace {

// button_icons_dump <folder>: every texture of the table, as the game has it
// and in each style, as raw RGBA (<name>_<style>_<w>x<h>.rgba), from the
// game's files - to check the pictures without playing.
void ButtonIconsDump(std::string_view args) {
  std::filesystem::path folder(std::string(args.empty() ? "button_icons_dump" : args));
  std::error_code error;
  std::filesystem::create_directories(folder, error);
  struct File {
    const char* path;
    std::vector<int> indices;
    const char* tag;
  };
  const File files[] = {
      {"PAC/US/FIX/UICMN_WINDOW.NUT", {0, 1, 2, 3, 4, 5, 6, 7}, "window"},
      {"PAC/CMN/FIX/BUCPT_FRAME.NUT", {28, 32, 33, 34}, "hud"},
      {"PAC/CMN/FIX/BUTUT_360.NUT", {0, 1}, "tutorial"},
  };
  std::filesystem::path root;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    root = g_game_data_root;
  }
  int written = 0;
  for (const File& file : files) {
    std::vector<uint8_t> nut;
    std::vector<BurstLimitNutTexture> textures;
    if (!BurstLimitReadGameFile(root, file.path, nut) || !BurstLimitListNut(nut, textures)) {
      REXLOG_WARN("button_icons_dump: can't read {}", file.path);
      continue;
    }
    for (int index : file.indices) {
      if (size_t(index) >= textures.size()) {
        continue;
      }
      const BurstLimitNutTexture& texture = textures[size_t(index)];
      const uint64_t hash = Fnv1a64(texture.data, texture.base_size);
      for (const Slot& slot : Slots()) {
        if (slot.original != hash || slot.data_size != texture.base_size) {
          continue;
        }
        const std::vector<uint8_t> original(texture.data, texture.data + texture.base_size);
        {
          BurstLimitTexture image;
          if (BurstLimitDecodeNutTexture(texture, image)) {
            std::ofstream stream(folder / (std::string(file.tag) + "_" + std::to_string(index) +
                                           "_game_" + std::to_string(image.width) + "x" +
                                           std::to_string(image.height) + ".rgba"),
                                 std::ios::binary);
            stream.write(reinterpret_cast<const char*>(image.rgba.data()),
                         std::streamsize(image.rgba.size()));
            ++written;
          }
        }
        for (int style = 0; style < kStyleCount; ++style) {
          const std::vector<uint8_t> data = MakeData(slot, original, style);
          BurstLimitNutTexture made = texture;
          made.data = data.data();
          BurstLimitTexture image;
          if (data.empty() || !BurstLimitDecodeNutTexture(made, image)) {
            continue;
          }
          const std::filesystem::path out =
              folder / (std::string(file.tag) + "_" + std::to_string(index) + "_" +
                        kStyleNames[style] + "_" + std::to_string(image.width) + "x" +
                        std::to_string(image.height) + ".rgba");
          std::ofstream stream(out, std::ios::binary);
          stream.write(reinterpret_cast<const char*>(image.rgba.data()),
                       std::streamsize(image.rgba.size()));
          ++written;
          // The sharp version the texture cache gets, and its key.
          const std::vector<uint8_t> sharp = MakeHighRes(slot, original, style);
          const int scale = HighResScale(slot);
          const uint64_t key =
              rex::graphics::TiledTexture2DReplacementKey(data.data(), slot.width, slot.height, 16);
          std::ofstream sharp_stream(
              folder / (std::string(file.tag) + "_" + std::to_string(index) + "_" +
                        kStyleNames[style] + "_sharp_" + std::to_string(slot.width * scale) +
                        "x" + std::to_string(slot.height * scale) + ".rgba"),
              std::ios::binary);
          sharp_stream.write(reinterpret_cast<const char*>(sharp.data()),
                             std::streamsize(sharp.size()));
          REXLOG_INFO("button_icons_dump: {} {} key {:016x}", slot.name, kStyleNames[style], key);
          ++written;
        }
      }
    }
  }
  REXLOG_INFO("button_icons_dump: {} pictures written to {}", written, folder.string());
  rex::FlushLogging();
}

}  // namespace

REXCVAR_DEFINE_COMMAND_ARGS(button_icons_dump, ButtonIconsDump, "Debug",
                            "Write the button icon textures (game's and redrawn) as raw RGBA "
                            "files: [folder]");

// The style the button_icons setting picks right now (auto: the last controller
// seen in the game, Xbox before it runs) - for the start screen.
int BurstLimitButtonStyleNow() {
  // Auto before the game's input runs: the controller the start screen sees.
  if (REXCVAR_GET(button_icons) == "auto" && !rex::Runtime::instance()) {
    const int seen = rex::ui::GamepadStyleBeforeGame();
    if (seen >= 0) {
      return seen;
    }
  }
  return ResolveStyle(false);
}
