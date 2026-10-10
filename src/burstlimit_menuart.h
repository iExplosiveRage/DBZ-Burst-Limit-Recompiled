// The game's own menu art for the recomp's screens drawn with ImGui (the
// start screen): its fonts, the window pieces and buttons of UICMN_WINDOW, the
// title screen's background, logo and characters - read from the player's game
// files (never shipped), decoded on a thread, uploaded on the UI thread.
#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <imgui.h>

#include <rex/ui/immediate_drawer.h>

#include "burstlimit_cpk.h"

class BurstLimitMenuArt;

// The one instance, made (and its decoding started) by the first call with the
// game's folder; null before that.
BurstLimitMenuArt* BurstLimitSharedMenuArt(const std::filesystem::path& game_data_root = {});

class BurstLimitMenuArt {
 public:
  // Pictures besides the fonts and window pieces.
  enum Picture {
    kBackground,  // the title screen's red sky (1280x720)
    kAura,        // its aura (1280x720, over the sky)
    kLogo,        // DRAGON BALL Z BURST LIMIT (700x320)
    kCharacter,   // the main menu's Goku
    kPictureCount
  };
  // UICMN_WINDOW textures.
  static constexpr int kButtonA = 0;
  static constexpr int kButtonB = 1;
  static constexpr int kButtonX = 2;
  static constexpr int kButtonY = 3;
  static constexpr int kArrowUp = 8;
  static constexpr int kWindowPieces = 20;

  explicit BurstLimitMenuArt(std::filesystem::path game_data_root);
  ~BurstLimitMenuArt();

  // UI thread, every frame: uploads the art once it's decoded. True when the
  // fonts and window pieces are there.
  bool Ready(rex::ui::ImmediateDrawer* drawer);

  rex::ui::ImmediateTexture* Image(Picture picture) const { return pictures_[picture].get(); }
  rex::ui::ImmediateTexture* Window(int index) const;

  // Text in the menu font (the outlined one of the game's menus), `size`
  // pixels a line, its line's top at `position`. Returns the width; draws only
  // with a draw list.
  float Text(ImDrawList* draw_list, ImVec2 position, float size, std::string_view text,
             ImU32 color) const;
  // Words wrapped to `width`.
  std::vector<std::string> Wrap(float size, const std::string& text, float width) const;
  // A 9-slice of the window pieces (`src0`-`src1`, `corner` texture pixels at
  // the corners), `scale` screen pixels per texture pixel, colored `top` to
  // `bottom`.
  void NineSlice(ImDrawList* draw_list, ImVec2 min, ImVec2 max, ImVec2 src0, ImVec2 src1,
                 float corner, float scale, ImU32 top, ImU32 bottom) const;
  // A window icon (a button, the arrow), `quarter_turns` clockwise.
  void Icon(ImDrawList* draw_list, int index, ImVec2 min, float size, ImU32 tint,
            int quarter_turns = 0) const;
  // A panel in the style of the game's main menu box and Options panels: the
  // purple body, black bars at the top (to header_bottom) and bottom (from
  // footer_top), the beige rim. `scale` = screen pixels per 1080p pixel.
  void Panel(ImDrawList* draw_list, ImVec2 min, ImVec2 max, float header_bottom,
             float footer_top, float scale, float alpha) const;

 private:
  struct Glyph {
    ImVec2 uv0, uv1;
    float width = 0.0f, height = 0.0f;
    float left = 0.0f, right = 0.0f;
  };

  void Decode();
  void RefreshButtons(rex::ui::ImmediateDrawer* drawer);

  std::filesystem::path game_data_root_;
  std::thread loader_;
  std::atomic<bool> decoded_{false};
  bool uploaded_ = false;

  // Decoded (loader thread), then uploaded and dropped.
  std::vector<BurstLimitGlyph> font_glyphs_;
  BurstLimitTexture font_atlas_;
  std::map<int, BurstLimitTexture> window_images_;
  std::array<BurstLimitTexture, kPictureCount> picture_images_;

  std::unique_ptr<rex::ui::ImmediateTexture> font_;
  std::array<Glyph, 256> glyphs_{};
  std::map<int, std::unique_ptr<rex::ui::ImmediateTexture>> window_;
  // The game's A / B (Xbox 360 look), redrawn in the button_icons style.
  std::map<int, BurstLimitTexture> buttons_;
  int buttons_style_ = -1;
  std::array<std::unique_ptr<rex::ui::ImmediateTexture>, kPictureCount> pictures_;
};
