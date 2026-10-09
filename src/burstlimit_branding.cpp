// "Online" instead of "Xbox LIVE" in the menus - the online mode here is the
// recomp's own lobby, not Xbox LIVE. Done in memory as the game runs; its
// files are left as they are.
//
// Text: the menus' text is in the LONG2_*.BIN tables of LONG2DATA_US.CPK
// (u32 count, u32 offsets, then UTF-16 big-endian strings). Every string
// comes out of one of two lookups: sub_8228F6A0 (index r3 in a screen's own
// table r4, via sub_822803E0) and sub_8228F5C0 (global id r3: the common
// Unlock / Kiyaku / MenuCommon tables). Both are hooked at their blr: when the
// string has "Xbox LIVE" in it, a copy with "Online" is handed back instead
// (made once per text, in guest memory, kept).
//
// Title art: the "Xbox LIVE BATTLE" logo of the Xbox LIVE Battle / Player
// Match / Ranked Match screens is in UIVNT_TOP.NUT: texture 0 the whole logo
// (452x272, white border), 1 "BATTLE", 2 its white silhouette, 3 "Xbox LIVE"
// (436x144), 4 its white silhouette (452x152). "ONLINE" is put together from
// the game's own lettering - the separate letters of the Z Chronicles title
// (UIMIN_TOP.NUT texture 8, red: the same hand lettering as BATTLE) - recolored
// to the blue gradient of BATTLE, slanted like it, and textures 0, 3 and 4 are
// made again from it as the file loads (the NTXR loader, like the stage
// select's thumbnails in burstlimit_stages.cpp). The art is made in the
// background at startup from the CPK.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <future>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/memory.h>
#include <rex/memory/utils.h>
#include <rex/ppc/context.h>
#include <rex/runtime.h>

#include "burstlimit_cpk.h"

REXCVAR_DEFINE_BOOL(online_branding, true, "Patches",
                    "Show \"Online\" instead of \"Xbox LIVE\" in the menus (text and the Online "
                    "Battle title art)");

namespace {

rex::memory::Memory* GuestMemory() {
  auto* runtime = rex::Runtime::instance();
  return runtime ? runtime->memory() : nullptr;
}

// ---------------------------------------------------------------------------
// Text

struct TextReplacement {
  const char16_t* from;
  const char16_t* to;
};

// Whole sentences that read better reworded; any other text has "Xbox LIVE"
// replaced by "Online".
constexpr TextReplacement kTexts[] = {
    // LONG2_KIYAKU_360 (sign-in / connection messages)
    {u"You must sign in to Xbox LIVE.", u"You must sign in to play online."},
    {u"Xbox LIVE is not available with the\ncurrent gamer profile.",
     u"Online play is not available with the\ncurrent gamer profile."},
    {u"Unable to connect to Xbox LIVE.", u"Unable to connect online."},
    {u"Xbox LIVE connection lost.", u"Online connection lost."},
    {u"Current user is not registered on Xbox LIVE.",
     u"Current user is not registered for online play."},
    {u"Xbox LIVE is not available unless\nyou are signed in.",
     u"Online play is not available unless\nyou are signed in."},
    {u"Send records to Xbox LIVE?", u"Send records online?"},
    {u"You must be signed in to Xbox LIVE\nin order to send data.",
     u"You must be signed in to play online\nin order to send data."},
    // LONG2_OPTION
    {u"Active Camera OFF.\nThis will be turned ON\nautomatically when playing\nover Xbox LIVE.",
     u"Active Camera OFF.\nThis will be turned ON\nautomatically when playing\nonline."},
};
constexpr std::u16string_view kXboxLive = u"Xbox LIVE";
constexpr std::u16string_view kOnline = u"Online";
constexpr uint32_t kMaxText = 2048;

std::mutex g_text_mutex;
// Original text -> its replacement in guest memory (0 = none could be made).
std::unordered_map<std::u16string, uint32_t> g_texts;

std::u16string Rebrand(const std::u16string& text) {
  for (const TextReplacement& replacement : kTexts) {
    if (text == replacement.from) {
      return replacement.to;
    }
  }
  std::u16string out = text;
  for (size_t pos = out.find(kXboxLive); pos != std::u16string::npos;
       pos = out.find(kXboxLive, pos + kOnline.size())) {
    out.replace(pos, kXboxLive.size(), kOnline);
  }
  return out;
}

// r3 = the text a lookup returns (UTF-16 big-endian): pointed at the
// "Online" copy when it has "Xbox LIVE" in it.
void RebrandText(PPCRegister& r3) {
  if (!REXCVAR_GET(online_branding) || !r3.u32) {
    return;
  }
  auto* memory = GuestMemory();
  if (!memory) {
    return;
  }
  const uint8_t* text = memory->TranslateVirtual<const uint8_t*>(r3.u32);
  // Quick check first (most texts don't have it): the 'X' of "Xbox LIVE".
  uint32_t length = 0;
  bool candidate = false;
  while (length < kMaxText) {
    const uint16_t c = uint16_t(text[length * 2] << 8 | text[length * 2 + 1]);
    if (!c) {
      break;
    }
    candidate |= c == u'X';
    ++length;
  }
  if (!candidate || length < kXboxLive.size() || length >= kMaxText) {
    return;
  }
  std::u16string original(length, u'\0');
  for (uint32_t i = 0; i < length; ++i) {
    original[i] = char16_t(text[i * 2] << 8 | text[i * 2 + 1]);
  }
  if (original.find(kXboxLive) == std::u16string::npos) {
    return;
  }
  std::lock_guard<std::mutex> lock(g_text_mutex);
  auto it = g_texts.find(original);
  if (it == g_texts.end()) {
    const std::u16string rebranded = Rebrand(original);
    const uint32_t address = memory->SystemHeapAlloc(uint32_t(rebranded.size() + 1) * 2);
    if (address) {
      uint8_t* out = memory->TranslateVirtual<uint8_t*>(address);
      for (size_t i = 0; i <= rebranded.size(); ++i) {
        const uint16_t c = i < rebranded.size() ? uint16_t(rebranded[i]) : 0;
        out[i * 2] = uint8_t(c >> 8);
        out[i * 2 + 1] = uint8_t(c);
      }
    }
    it = g_texts.emplace(original, address).first;
  }
  if (it->second) {
    r3.u64 = it->second;
  }
}

// ---------------------------------------------------------------------------
// Title art

// Premultiplied RGBA, 0-1.
struct Image {
  int width = 0;
  int height = 0;
  std::vector<std::array<float, 4>> pixels;

  Image() = default;
  Image(int w, int h) : width(w), height(h), pixels(size_t(w) * h, {0, 0, 0, 0}) {}
  std::array<float, 4>& at(int x, int y) { return pixels[size_t(y) * width + x]; }
  const std::array<float, 4>& at(int x, int y) const { return pixels[size_t(y) * width + x]; }
  std::array<float, 4> get(int x, int y) const {
    if (x < 0 || y < 0 || x >= width || y >= height) {
      return {0, 0, 0, 0};
    }
    return at(x, y);
  }
};

Image FromTexture(const BurstLimitTexture& texture, int x0, int y0, int w, int h) {
  Image image(w, h);
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      const uint8_t* p = &texture.rgba[(size_t(y0 + y) * texture.width + (x0 + x)) * 4];
      const float a = p[3] / 255.0f;
      image.at(x, y) = {p[0] / 255.0f * a, p[1] / 255.0f * a, p[2] / 255.0f * a, a};
    }
  }
  return image;
}

// A letter of the Z Chronicles title: its box in UIMIN_TOP texture 8 (the
// letters are apart from each other; only the biggest 8-connected shape in the
// box is kept, so a neighbour's edge can't come along).
struct LetterBox {
  char letter;
  int x, y, width, height;
};
constexpr LetterBox kLetters[] = {
    {'O', 380, 0, 67, 79},   {'N', 136, 108, 91, 99}, {'I', 228, 109, 31, 83},
    {'L', 337, 108, 63, 92}, {'E', 401, 108, 65, 92},
};

Image Letter(const BurstLimitTexture& sheet, const LetterBox& box) {
  Image image = FromTexture(sheet, box.x, box.y, box.width, box.height);
  const int w = box.width, h = box.height;
  std::vector<int> label(size_t(w) * h, 0);
  int best = 0, best_size = 0, next = 0;
  std::vector<int> stack;
  for (int start = 0; start < w * h; ++start) {
    if (label[start] || image.pixels[start][3] <= 8.0f / 255.0f) {
      continue;
    }
    ++next;
    int size = 0;
    stack.push_back(start);
    label[start] = next;
    while (!stack.empty()) {
      const int i = stack.back();
      stack.pop_back();
      ++size;
      const int x = i % w, y = i / w;
      for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
          const int nx = x + dx, ny = y + dy;
          if (nx < 0 || ny < 0 || nx >= w || ny >= h) {
            continue;
          }
          const int n = ny * w + nx;
          if (!label[n] && image.pixels[n][3] > 8.0f / 255.0f) {
            label[n] = next;
            stack.push_back(n);
          }
        }
      }
    }
    if (size > best_size) {
      best_size = size;
      best = next;
    }
  }
  for (int i = 0; i < w * h; ++i) {
    if (label[i] != best) {
      image.pixels[i] = {0, 0, 0, 0};
    }
  }
  return image;
}

// Red lettering -> the blue of BATTLE: the red letters are a white-to-red
// gradient (green = blue = how white) darkened to black at the outline (red =
// how bright); BATTLE's blue for the same whiteness m (0-255) is about
// (max(m, 26), 128 + 0.48 m, min(224 + 0.16 m, 251)).
void Recolor(Image& image) {
  for (auto& p : image.pixels) {
    const float a = p[3];
    if (a <= 0.0f) {
      continue;
    }
    const float r = p[0] / a * 255.0f, g = p[1] / a * 255.0f, b = p[2] / a * 255.0f;
    const float v = std::clamp(r / 219.3f, 0.0f, 1.0f);
    const float m = v > 0.02f ? std::min((g + b) * 0.5f / v, 255.0f) : 0.0f;
    const float out[3] = {std::max(m, 26.0f) * v, (128.0f + 0.48f * m) * v,
                          std::min(224.0f + 0.16f * m, 251.0f) * v};
    for (int c = 0; c < 3; ++c) {
      p[c] = std::clamp(out[c] / 255.0f, 0.0f, 1.0f) * a;
    }
  }
}

float CubicWeight(float t) {
  // Catmull-Rom.
  t = std::fabs(t);
  if (t < 1.0f) {
    return 1.5f * t * t * t - 2.5f * t * t + 1.0f;
  }
  if (t < 2.0f) {
    return -0.5f * t * t * t + 2.5f * t * t - 4.0f * t + 2.0f;
  }
  return 0.0f;
}

std::array<float, 4> SampleCubic(const Image& image, float x, float y) {
  // Pixel centers at integers.
  const int x0 = int(std::floor(x)), y0 = int(std::floor(y));
  std::array<float, 4> sum = {0, 0, 0, 0};
  for (int j = -1; j <= 2; ++j) {
    const float wy = CubicWeight(y - float(y0 + j));
    for (int i = -1; i <= 2; ++i) {
      const float w = wy * CubicWeight(x - float(x0 + i));
      const auto p = image.get(x0 + i, y0 + j);
      for (int c = 0; c < 4; ++c) {
        sum[c] += p[c] * w;
      }
    }
  }
  sum[3] = std::clamp(sum[3], 0.0f, 1.0f);
  for (int c = 0; c < 3; ++c) {
    sum[c] = std::clamp(sum[c], 0.0f, sum[3]);
  }
  return sum;
}

// Scaled by `scale` and slanted: the top moves right by `shear` x the new
// height (left when negative); the bottom row stays where it is.
Image Transform(const Image& image, float scale, float shear) {
  const float height = float(image.height) * scale;
  const float offset = std::max(0.0f, -shear * height);
  const int out_w = int(std::lround(image.width * scale + std::fabs(shear) * height)) + 2;
  const int out_h = int(std::lround(height));
  Image out(out_w, out_h);
  for (int y = 0; y < out_h; ++y) {
    for (int x = 0; x < out_w; ++x) {
      const float sx = (float(x) - shear * (height - float(y)) - offset) / scale;
      out.at(x, y) = SampleCubic(image, sx, float(y) / scale);
    }
  }
  return out;
}

// src over dst, src's top left at (x, y).
void Over(Image& dst, const Image& src, int x0, int y0) {
  for (int y = 0; y < src.height; ++y) {
    for (int x = 0; x < src.width; ++x) {
      const int dx = x0 + x, dy = y0 + y;
      if (dx < 0 || dy < 0 || dx >= dst.width || dy >= dst.height) {
        continue;
      }
      const auto& s = src.at(x, y);
      auto& d = dst.at(dx, dy);
      for (int c = 0; c < 4; ++c) {
        d[c] = s[c] + d[c] * (1.0f - s[3]);
      }
    }
  }
}

// The white border the game's titles have: the shape grown by a disc of
// radius 5, in white.
Image WhiteBorder(const Image& image, int radius = 5) {
  Image out(image.width, image.height);
  std::vector<std::pair<int, int>> disc;
  for (int dy = -radius; dy <= radius; ++dy) {
    for (int dx = -radius; dx <= radius; ++dx) {
      if (dx * dx + dy * dy <= radius * radius + radius / 2) {
        disc.emplace_back(dx, dy);
      }
    }
  }
  for (int y = 0; y < image.height; ++y) {
    for (int x = 0; x < image.width; ++x) {
      float a = 0.0f;
      for (const auto& [dx, dy] : disc) {
        a = std::max(a, image.get(x + dx, y + dy)[3]);
      }
      out.at(x, y) = {a, a, a, a};
    }
  }
  return out;
}

// "ONLINE" in the geometry of "BATTLE" under it: every letter 86 pixels tall
// (BATTLE's small letters are about 80; the Z Chronicles letters differ in
// height, so each is scaled to it - the outline stays about BATTLE's 7
// pixels), each letter's own lean evened out to BATTLE's slant (about 0.14:
// the shear is 0.143 minus the letter's measured lean), 2 pixels apart, the
// baseline rising 9 pixels a letter like "Xbox LIVE", placed as high and as
// far right as it goes without running into BATTLE in the whole logo; inside
// texture 3's 436x144 (x 14-385), so nothing touches its edges.
Image MakeOnlineWord(const BurstLimitTexture& sheet) {
  struct Placement {
    char letter;
    int x, bottom;
    float scale, shear;
  };
  constexpr Placement kWord[] = {
      {'O', 14, 132, 86.0f / 79.0f, -0.013f},  {'N', 79, 123, 86.0f / 99.0f, -0.126f},
      {'L', 142, 114, 86.0f / 92.0f, -0.205f}, {'I', 214, 105, 86.0f / 83.0f, 0.143f},
      {'N', 244, 96, 86.0f / 99.0f, -0.126f},  {'E', 318, 87, 86.0f / 92.0f, 0.078f},
  };
  Image word(436, 144);
  for (const Placement& placement : kWord) {
    for (const LetterBox& box : kLetters) {
      if (box.letter != placement.letter) {
        continue;
      }
      Image letter = Letter(sheet, box);
      Recolor(letter);
      const Image shaped = Transform(letter, placement.scale, placement.shear);
      Over(word, shaped, placement.x, placement.bottom - shaped.height);
    }
  }
  return word;
}

// Premultiplied -> RGBA8 (fully clear pixels get `clear` as their color, so
// filtering at the edges blends towards the edge's own color).
std::vector<uint8_t> ToRgba8(const Image& image, uint8_t clear) {
  std::vector<uint8_t> out(size_t(image.width) * image.height * 4);
  for (size_t i = 0; i < image.pixels.size(); ++i) {
    const auto& p = image.pixels[i];
    const float a = p[3];
    for (int c = 0; c < 3; ++c) {
      out[i * 4 + c] = a > 0.002f ? uint8_t(std::lround(std::clamp(p[c] / a, 0.0f, 1.0f) * 255.0f))
                                  : clear;
    }
    out[i * 4 + 3] = uint8_t(std::lround(std::clamp(a, 0.0f, 1.0f) * 255.0f));
  }
  return out;
}

// DXT5 as the NUT keeps it: linear blocks, big-endian 16-bit words.
std::vector<uint8_t> EncodeNutDxt5(const Image& image, uint8_t clear) {
  const std::vector<uint8_t> rgba = ToRgba8(image, clear);
  const int blocks_x = (image.width + 3) / 4, blocks_y = (image.height + 3) / 4;
  std::vector<uint8_t> out(size_t(blocks_x) * blocks_y * 16);
  for (int by = 0; by < blocks_y; ++by) {
    for (int bx = 0; bx < blocks_x; ++bx) {
      uint8_t pixels[16][4];
      for (int i = 0; i < 16; ++i) {
        const int x = std::min(bx * 4 + (i & 3), image.width - 1);
        const int y = std::min(by * 4 + (i >> 2), image.height - 1);
        std::memcpy(pixels[i], &rgba[(size_t(y) * image.width + x) * 4], 4);
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

// The UIVNT_TOP.NUT textures replaced: their header (NTXR version 1, 0x50
// bytes; the sizes, format, dimensions and GIDX are what's compared), and the
// FNV-1a 64 of the game's data (the US file): any other data is left alone.
struct ArtTexture {
  int index;
  uint32_t data_size;
  uint16_t width, height;
  uint32_t gidx;
  uint64_t original;
};
constexpr ArtTexture kArt[3] = {
    {0, 0x1E040, 452, 272, 0x7724, 0xACF98DA08CD719DCull},  // the whole logo
    {3, 0xF540, 436, 144, 0x7727, 0xC0105111284E4046ull},   // "Xbox LIVE"
    {4, 0x10C60, 452, 152, 0x7728, 0x8D771FAC0D4D7CF4ull},  // its white silhouette
};
constexpr uint32_t kArtHeaderSize = 0x50;

struct Art {
  bool ok = false;
  std::vector<uint8_t> data[3];  // as kArt
};

std::filesystem::path g_game_data_root;
std::shared_future<Art> g_art;
std::mutex g_art_mutex;

Art MakeArt(std::filesystem::path game_data_root) {
  Art art;
  std::vector<uint8_t> menu_nut, versus_nut;
  std::vector<BurstLimitNutTexture> menu_list, versus_list;
  BurstLimitTexture letters, battle;
  if (!BurstLimitReadGameFile(game_data_root, "PAC/US/FIX/UIMIN_TOP.NUT", menu_nut) ||
      !BurstLimitReadGameFile(game_data_root, "PAC/US/FIX/UIVNT_TOP.NUT", versus_nut) ||
      !BurstLimitListNut(menu_nut, menu_list) || !BurstLimitListNut(versus_nut, versus_list) ||
      menu_list.size() <= 8 || versus_list.size() <= 4 ||
      !BurstLimitDecodeNutTexture(menu_list[8], letters) ||
      !BurstLimitDecodeNutTexture(versus_list[1], battle) || letters.width != 540 ||
      letters.height != 208 || battle.width != 348 || battle.height != 168) {
    REXLOG_WARN("Online branding: the game's title lettering wasn't found; the Xbox LIVE Battle "
                "title stays as it is");
    return art;
  }
  const Image word = MakeOnlineWord(letters);

  // Texture 4: the word's white silhouette, 9 right and 7 down (452x152).
  Image silhouette(452, 152);
  Over(silhouette, WhiteBorder(word), 9, 7);

  // Texture 0: "ONLINE" over "BATTLE", both with the white border.
  Image logo(452, 272);
  Image online_layer(452, 272), battle_layer(452, 272);
  Over(online_layer, word, 10, 6);
  Over(battle_layer, FromTexture(battle, 0, 0, 348, 168), 86, 98);
  Over(logo, WhiteBorder(online_layer), 0, 0);
  Over(logo, WhiteBorder(battle_layer), 0, 0);
  Over(logo, online_layer, 0, 0);
  Over(logo, battle_layer, 0, 0);

  art.data[0] = EncodeNutDxt5(logo, 255);
  art.data[1] = EncodeNutDxt5(word, 0);
  art.data[2] = EncodeNutDxt5(silhouette, 255);
  for (int i = 0; i < 3; ++i) {
    if (art.data[i].size() != kArt[i].data_size) {
      return Art{};
    }
  }
  art.ok = true;
  return art;
}

uint32_t Be32(const uint8_t* p) {
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}
uint16_t Be16(const uint8_t* p) {
  return uint16_t(p[0] << 8 | p[1]);
}

uint64_t Fnv1a64(const uint8_t* data, size_t size) {
  uint64_t hash = 0xCBF29CE484222325ull;
  for (size_t i = 0; i < size; ++i) {
    hash ^= data[i];
    hash *= 0x100000001B3ull;
  }
  return hash;
}

}  // namespace

// From the app's OnPreSetup: the title art is made in the background.
void BurstLimitBrandingSetup(const std::filesystem::path& game_data_root) {
  std::lock_guard<std::mutex> lock(g_art_mutex);
  g_game_data_root = game_data_root;
  if (REXCVAR_GET(online_branding) && !g_art.valid()) {
    g_art = std::async(std::launch::async, MakeArt, g_game_data_root).share();
  }
}

// Mid-asm hook at the blr of sub_8228F6A0 (a screen's text by index):
// r3 = the text.
void BurstLimitBrandingScreenText(PPCRegister& r3) {
  RebrandText(r3);
}

// Mid-asm hook at the blr of sub_8228F5C0 (the common texts by id), the path
// that reads the Unlock / Kiyaku / MenuCommon tables: r3 = the text.
void BurstLimitBrandingCommonText(PPCRegister& r3) {
  RebrandText(r3);
}

// burstlimit_buttons.cpp: the button icons of the chosen controller, at the
// same place (one hook per address).
void BurstLimitButtonsTexture(PPCRegister& r26, PPCRegister& r29, PPCRegister& r30);

// Mid-asm hook at 0x820EB780 in sub_820EB688 (the NTXR loader), right before
// the texture is made (see BurstLimitStagesThumbnailData): r26 = the NTXR
// version, r29 = the texture's index, r30 = its header (data at +0x50). The
// button icons are redrawn here too (BurstLimitButtonsTexture).
void BurstLimitBrandingTexture(PPCRegister& r26, PPCRegister& r29, PPCRegister& r30) {
  BurstLimitButtonsTexture(r26, r29, r30);
  if (r26.u32 != 1 || !r30.u32 || !REXCVAR_GET(online_branding)) {
    return;
  }
  int which = -1;
  for (int i = 0; i < 3; ++i) {
    if (int(r29.u32) == kArt[i].index) {
      which = i;
    }
  }
  if (which < 0) {
    return;
  }
  auto* memory = GuestMemory();
  if (!memory) {
    return;
  }
  uint8_t* header = memory->TranslateVirtual<uint8_t*>(r30.u32);
  const ArtTexture& texture = kArt[which];
  if (Be32(header + 8) != texture.data_size || Be16(header + 12) != kArtHeaderSize ||
      Be16(header + 18) != 2 || Be16(header + 20) != texture.width ||
      Be16(header + 22) != texture.height || std::memcmp(header + 0x40, "GIDX", 4) != 0 ||
      Be32(header + 0x48) != texture.gidx) {
    return;
  }
  uint8_t* data = header + kArtHeaderSize;
  if (Fnv1a64(data, texture.data_size) != texture.original) {
    return;  // ours already, or another version of the file
  }
  std::shared_future<Art> art;
  {
    std::lock_guard<std::mutex> lock(g_art_mutex);
    if (!g_art.valid()) {
      g_art = std::async(std::launch::async, MakeArt, g_game_data_root).share();
    }
    art = g_art;
  }
  const Art& made = art.get();
  if (!made.ok) {
    return;
  }
  std::memcpy(data, made.data[which].data(), texture.data_size);
  REXLOG_INFO("Online branding: UIVNT_TOP texture {} is now the Online Battle title", r29.u32);
}
