// The game's menu art for ImGui screens (see burstlimit_menuart.h). The font,
// 9-slice and icon drawing follow the Mods menu's (burstlimit_mods.cpp).

#include "burstlimit_menuart.h"

#include <algorithm>
#include <chrono>
#include <random>

#include <rex/logging.h>

// burstlimit_buttons.cpp: the button_icons style now (0 Xbox, 1 PlayStation,
// 2 Switch), and a UICMN_WINDOW icon redrawn in a style.
int BurstLimitButtonStyleNow();
bool BurstLimitRestyleWindowIcon(int index, int style, BurstLimitTexture& image);

namespace {

constexpr const char* kRegions[] = {"US", "UK", "FR", "IT", "SP", "DU", "JP"};

// The outlined menu font: 40-pixel cells, the visible glyph between left and
// right, neighbors overlapping by 2 pixels.
constexpr float kFontNative = 40.0f;
constexpr float kFontSpace = 10.0f;
constexpr float kFontOverlap = 2.0f;

bool ReadFix(const std::filesystem::path& root, const char* file, std::vector<uint8_t>& data) {
  for (const char* region : kRegions) {
    if (BurstLimitReadGameFile(root, std::string("PAC/") + region + "/FIX/" + file, data)) {
      return true;
    }
  }
  return BurstLimitReadGameFile(root, std::string("PAC/CMN/FIX/") + file, data);
}

bool DecodeOne(const std::vector<uint8_t>& nut, size_t index, BurstLimitTexture& out) {
  std::vector<BurstLimitNutTexture> textures;
  return BurstLimitListNut(nut, textures) && index < textures.size() &&
         BurstLimitDecodeNutTexture(textures[index], out);
}

}  // namespace

BurstLimitMenuArt* BurstLimitSharedMenuArt(const std::filesystem::path& game_data_root) {
  // Never freed: the overlays may draw with it until the process ends.
  static BurstLimitMenuArt* art = nullptr;
  if (!art && !game_data_root.empty()) {
    art = new BurstLimitMenuArt(game_data_root);
  }
  return art;
}

void BurstLimitMenuArt::Panel(ImDrawList* draw_list, ImVec2 min, ImVec2 max, float header_bottom,
                              float footer_top, float scale, float alpha) const {
  auto fade = [alpha](int r, int g, int b, int a) { return IM_COL32(r, g, b, int(float(a) * alpha)); };
  // The window pieces' 22-pixel quarter circles at 14 px of 720p.
  const float round = 0.62f * 1.5f * scale;
  const float extra = 40.0f * scale;
  NineSlice(draw_list, min, max, ImVec2(45, 1), ImVec2(88, 44), 21.5f, round,
            fade(16, 6, 10, 250), fade(64, 10, 98, 250));
  draw_list->PushClipRect(min, ImVec2(max.x, header_bottom), true);
  NineSlice(draw_list, min, ImVec2(max.x, header_bottom + extra), ImVec2(45, 1), ImVec2(88, 44),
            21.5f, round, fade(0, 0, 0, 255), fade(0, 0, 0, 255));
  draw_list->PopClipRect();
  draw_list->PushClipRect(ImVec2(min.x, footer_top), max, true);
  NineSlice(draw_list, ImVec2(min.x, footer_top - extra), max, ImVec2(45, 1), ImVec2(88, 44),
            21.5f, round, fade(0, 0, 0, 255), fade(0, 0, 0, 255));
  draw_list->PopClipRect();
  NineSlice(draw_list, ImVec2(min.x - 1.0f, min.y - 1.0f), ImVec2(max.x + 1.0f, max.y + 1.0f),
            ImVec2(1, 1), ImVec2(44, 44), 21.5f, round, fade(194, 180, 142, 255),
            fade(194, 180, 142, 255));
}

BurstLimitMenuArt::BurstLimitMenuArt(std::filesystem::path game_data_root)
    : game_data_root_(std::move(game_data_root)) {
  loader_ = std::thread([this] {
    Decode();
    decoded_.store(true, std::memory_order_release);
  });
}

BurstLimitMenuArt::~BurstLimitMenuArt() {
  if (loader_.joinable()) {
    loader_.join();
  }
}

void BurstLimitMenuArt::Decode() {
  const auto start = std::chrono::steady_clock::now();
  std::vector<uint8_t> nfh, nut;
  if (BurstLimitReadGameFile(game_data_root_, "PAC/CMN/CMN/RPRO_EB_FUCHI40_ALPHA.NFH", nfh) &&
      BurstLimitReadGameFile(game_data_root_, "PAC/CMN/CMN/RPRO_EB_FUCHI40_ALPHA.NUT", nut)) {
    std::vector<BurstLimitTexture> atlas;
    if (BurstLimitParseNfh(nfh, font_glyphs_) && BurstLimitDecodeNut(nut, atlas) &&
        !atlas.empty()) {
      font_atlas_ = std::move(atlas[0]);
    }
  }
  if (ReadFix(game_data_root_, "UICMN_WINDOW.NUT", nut)) {
    for (int index : {kButtonA, kButtonB, kButtonX, kButtonY, kArrowUp, kWindowPieces}) {
      BurstLimitTexture image;
      if (DecodeOne(nut, size_t(index), image)) {
        window_images_[index] = std::move(image);
      }
    }
  }
  if (ReadFix(game_data_root_, "UICMN_TTELG.NUT", nut)) {
    DecodeOne(nut, 0, picture_images_[kLogo]);
  }
  // The title screen's sky and aura, and the main menu's Goku.
  struct Pick {
    const char* file;
    size_t index;
  };
  const Pick pick = {"UIMIN_CR1.NUT", 0};
  if (ReadFix(game_data_root_, "UICMN_TTEBK.NUT", nut)) {
    DecodeOne(nut, 7, picture_images_[kBackground]);
    DecodeOne(nut, 8, picture_images_[kAura]);
    if (std::string_view(pick.file) == "UICMN_TTEBK.NUT") {
      DecodeOne(nut, pick.index, picture_images_[kCharacter]);
    }
  }
  if (std::string_view(pick.file) != "UICMN_TTEBK.NUT" && ReadFix(game_data_root_, pick.file, nut)) {
    DecodeOne(nut, pick.index, picture_images_[kCharacter]);
  }
  REXLOG_INFO("Menu art: decoded in {} ms (font {}, window pieces {}, logo {}, character {}:{})",
              std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now() - start)
                  .count(),
              font_atlas_.rgba.empty() ? "missing" : "ok",
              window_images_.count(kWindowPieces) ? "ok" : "missing",
              picture_images_[kLogo].rgba.empty() ? "missing" : "ok", pick.file, pick.index);
}

bool BurstLimitMenuArt::Ready(rex::ui::ImmediateDrawer* drawer) {
  if (uploaded_) {
    RefreshButtons(drawer);
    return font_ && Window(kWindowPieces);
  }
  if (!drawer || !decoded_.load(std::memory_order_acquire)) {
    return false;
  }
  loader_.join();
  uploaded_ = true;
  auto make = [drawer](const BurstLimitTexture& image) {
    return image.rgba.empty()
               ? nullptr
               : drawer->CreateTexture(image.width, image.height,
                                       rex::ui::ImmediateTextureFilter::kLinear, false,
                                       image.rgba.data());
  };
  font_ = make(font_atlas_);
  if (font_) {
    for (const BurstLimitGlyph& glyph : font_glyphs_) {
      if (glyph.code >= glyphs_.size()) {
        continue;
      }
      Glyph& g = glyphs_[glyph.code];
      g.uv0 = ImVec2(float(glyph.x) / float(font_->width), float(glyph.y) / float(font_->height));
      g.uv1 = ImVec2(float(glyph.x + glyph.width) / float(font_->width),
                     float(glyph.y + glyph.height) / float(font_->height));
      g.width = glyph.width;
      g.height = glyph.height;
      g.left = glyph.left;
      g.right = glyph.right;
    }
  }
  for (auto& [index, image] : window_images_) {
    window_[index] = make(image);
    if (index == kButtonA || index == kButtonB || index == kButtonX || index == kButtonY) {
      buttons_[index] = image;
    }
  }
  for (size_t i = 0; i < pictures_.size(); ++i) {
    pictures_[i] = make(picture_images_[i]);
  }
  font_glyphs_.clear();
  font_atlas_ = {};
  window_images_.clear();
  picture_images_ = {};
  if (!font_ || !Window(kWindowPieces)) {
    REXLOG_WARN("Menu art: the game's menu font or window pieces couldn't be read");
  }
  return font_ && Window(kWindowPieces);
}

// The A / B buttons in the button_icons style (Xbox Series, PlayStation or
// Switch) whenever it changes.
void BurstLimitMenuArt::RefreshButtons(rex::ui::ImmediateDrawer* drawer) {
  const int style = BurstLimitButtonStyleNow();
  if (!drawer || style == buttons_style_) {
    return;
  }
  buttons_style_ = style;
  for (const auto& [index, original] : buttons_) {
    BurstLimitTexture image = original;
    if (BurstLimitRestyleWindowIcon(index, style, image)) {
      window_[index] = drawer->CreateTexture(image.width, image.height,
                                             rex::ui::ImmediateTextureFilter::kLinear, false,
                                             image.rgba.data());
    }
  }
}

rex::ui::ImmediateTexture* BurstLimitMenuArt::Window(int index) const {
  auto it = window_.find(index);
  return it != window_.end() ? it->second.get() : nullptr;
}

float BurstLimitMenuArt::Text(ImDrawList* draw_list, ImVec2 position, float size,
                              std::string_view text, ImU32 color) const {
  if (!font_) {
    return 0.0f;
  }
  const float s = size / kFontNative;
  float pen = 0.0f;
  for (char c : text) {
    const uint8_t code = uint8_t(c);
    const Glyph& g = glyphs_[code];
    if (code == ' ' || g.width == 0.0f) {
      pen += kFontSpace * s;
      continue;
    }
    if (draw_list) {
      const float x = position.x + pen - g.left * s;
      draw_list->AddImage(reinterpret_cast<ImTextureID>(font_.get()), ImVec2(x, position.y),
                          ImVec2(x + g.width * s, position.y + g.height * s), g.uv0, g.uv1, color);
    }
    pen += (g.right - g.left - kFontOverlap) * s;
  }
  return pen + kFontOverlap * s;
}

std::vector<std::string> BurstLimitMenuArt::Wrap(float size, const std::string& text,
                                                 float width) const {
  std::vector<std::string> lines;
  std::string line;
  size_t pos = 0;
  while (pos < text.size()) {
    if (text[pos] == '\n') {
      lines.push_back(line);
      line.clear();
      ++pos;
      continue;
    }
    size_t end = text.find_first_of(" \n", pos);
    if (end == std::string::npos) end = text.size();
    const std::string word = text.substr(pos, end - pos);
    const std::string candidate = line.empty() ? word : line + " " + word;
    if (!line.empty() && Text(nullptr, ImVec2(), size, candidate, 0) > width) {
      lines.push_back(line);
      line = word;
    } else {
      line = candidate;
    }
    pos = end < text.size() && text[end] == ' ' ? end + 1 : end;
  }
  if (!line.empty()) lines.push_back(line);
  return lines;
}

void BurstLimitMenuArt::NineSlice(ImDrawList* draw_list, ImVec2 min, ImVec2 max, ImVec2 src0,
                                  ImVec2 src1, float corner, float scale, ImU32 top,
                                  ImU32 bottom) const {
  rex::ui::ImmediateTexture* texture = Window(kWindowPieces);
  if (!texture || max.x <= min.x || max.y <= min.y) {
    return;
  }
  const float w = float(texture->width), h = float(texture->height);
  const float cs = std::min({corner * scale, (max.x - min.x) * 0.5f, (max.y - min.y) * 0.5f});
  const float xs[4] = {min.x, min.x + cs, max.x - cs, max.x};
  const float ys[4] = {min.y, min.y + cs, max.y - cs, max.y};
  const float us[4] = {src0.x / w, (src0.x + corner) / w, (src1.x - corner) / w, src1.x / w};
  const float vs[4] = {src0.y / h, (src0.y + corner) / h, (src1.y - corner) / h, src1.y / h};
  const ImVec4 t = ImGui::ColorConvertU32ToFloat4(top), b = ImGui::ColorConvertU32ToFloat4(bottom);
  auto color = [&](float y) {
    const float f = std::clamp((y - min.y) / (max.y - min.y), 0.0f, 1.0f);
    return ImGui::ColorConvertFloat4ToU32(ImVec4(t.x + (b.x - t.x) * f, t.y + (b.y - t.y) * f,
                                                 t.z + (b.z - t.z) * f, t.w + (b.w - t.w) * f));
  };
  draw_list->PushTextureID(reinterpret_cast<ImTextureID>(texture));
  for (int row = 0; row < 3; ++row) {
    if (ys[row + 1] <= ys[row]) continue;
    const ImU32 c0 = color(ys[row]), c1 = color(ys[row + 1]);
    for (int col = 0; col < 3; ++col) {
      if (xs[col + 1] <= xs[col]) continue;
      draw_list->PrimReserve(6, 4);
      const ImDrawIdx base = ImDrawIdx(draw_list->_VtxCurrentIdx);
      draw_list->PrimWriteIdx(base);
      draw_list->PrimWriteIdx(ImDrawIdx(base + 1));
      draw_list->PrimWriteIdx(ImDrawIdx(base + 2));
      draw_list->PrimWriteIdx(base);
      draw_list->PrimWriteIdx(ImDrawIdx(base + 2));
      draw_list->PrimWriteIdx(ImDrawIdx(base + 3));
      draw_list->PrimWriteVtx(ImVec2(xs[col], ys[row]), ImVec2(us[col], vs[row]), c0);
      draw_list->PrimWriteVtx(ImVec2(xs[col + 1], ys[row]), ImVec2(us[col + 1], vs[row]), c0);
      draw_list->PrimWriteVtx(ImVec2(xs[col + 1], ys[row + 1]), ImVec2(us[col + 1], vs[row + 1]),
                              c1);
      draw_list->PrimWriteVtx(ImVec2(xs[col], ys[row + 1]), ImVec2(us[col], vs[row + 1]), c1);
    }
  }
  draw_list->PopTextureID();
}

void BurstLimitMenuArt::Icon(ImDrawList* draw_list, int index, ImVec2 min, float size, ImU32 tint,
                             int quarter_turns) const {
  rex::ui::ImmediateTexture* texture = Window(index);
  if (!texture) {
    return;
  }
  const ImVec2 corners[4] = {min, ImVec2(min.x + size, min.y), ImVec2(min.x + size, min.y + size),
                             ImVec2(min.x, min.y + size)};
  const ImVec2 uvs[4] = {ImVec2(0, 0), ImVec2(1, 0), ImVec2(1, 1), ImVec2(0, 1)};
  const int r = ((quarter_turns % 4) + 4) % 4;
  draw_list->AddImageQuad(reinterpret_cast<ImTextureID>(texture), corners[0], corners[1],
                          corners[2], corners[3], uvs[(4 - r) % 4], uvs[(5 - r) % 4],
                          uvs[(6 - r) % 4], uvs[(7 - r) % 4], tint);
}
