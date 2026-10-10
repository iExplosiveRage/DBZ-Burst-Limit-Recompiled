// First start without the game files: the player picks their Dragon Ball Z:
// Burst Limit (USA) Xbox 360 disc image and its files are copied into
// game_data_root next to burstlimit.exe, then the game starts.
//
// The disc is read with the SDK's XDVDFS reader (DiscImageDevice). Only the
// USA default.xex works with this build, so its SHA-1 is checked before
// anything is copied. default.xex is written last: it's what marks the
// folder as installed (BurstlimitApp::OnConfigurePaths), so a copy that
// stops halfway just starts over next time.

#include <algorithm>
#include <atomic>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <windows.h>
#include <bcrypt.h>
#include <commdlg.h>

#include <imgui.h>

#include <rex/cvar.h>
#include <rex/filesystem/devices/disc_image_device.h>
#include <rex/filesystem/devices/disc_image_entry.h>
#include <rex/filesystem/entry.h>
#include <rex/logging.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/overlay/overlay_text.h>
#include <rex/ui/overlay/quick_menu.h>

// burstlimit_buttons.cpp: the button_icons style now (0 Xbox, 1 PlayStation, 2 Switch).
int BurstLimitButtonStyleNow();

#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "comdlg32.lib")

REXCVAR_DEFINE_STRING(install_iso, "", "Setup",
                      "First start without the game files: install from this disc image "
                      "instead of asking for one");

namespace {

// SHA-1 of the USA default.xex (media ID 036A7E66), the one the
// recompilation was made from.
constexpr char kUsaXexSha1[] = "aec598f88cf51181fc377b148e0b1ad30db4485c";

std::string Sha1Hex(const uint8_t* data, size_t size) {
  uint8_t digest[20] = {};
  if (!BCRYPT_SUCCESS(BCryptHash(BCRYPT_SHA1_ALG_HANDLE, nullptr, 0,
                                 const_cast<uint8_t*>(data), ULONG(size), digest,
                                 sizeof(digest)))) {
    return {};
  }
  static constexpr char kHex[] = "0123456789abcdef";
  std::string hex;
  for (uint8_t byte : digest) {
    hex += kHex[byte >> 4];
    hex += kHex[byte & 15];
  }
  return hex;
}

std::string GigaBytes(uint64_t bytes) {
  char text[32];
  std::snprintf(text, sizeof(text), "%.1f GB", double(bytes) / (1024.0 * 1024.0 * 1024.0));
  return text;
}

const rex::filesystem::Entry* FindChild(const rex::filesystem::Entry* entry,
                                        const char* name) {
  for (const auto& child : entry->children()) {
    if (_stricmp(child->name().c_str(), name) == 0) {
      return child.get();
    }
  }
  return nullptr;
}

struct CopyItem {
  const rex::filesystem::DiscImageEntry* entry;
  std::filesystem::path relative;
};

// Every file under `entry` except the console's system update.
void CollectFiles(const rex::filesystem::Entry* entry, const std::filesystem::path& relative,
                  std::vector<CopyItem>& out) {
  for (const auto& child : entry->children()) {
    const std::filesystem::path path = relative / std::filesystem::u8path(child->name());
    if (child->attributes() & rex::filesystem::kFileAttributeDirectory) {
      if (relative.empty() && _stricmp(child->name().c_str(), "$SystemUpdate") == 0) {
        continue;
      }
      CollectFiles(child.get(), path, out);
    } else {
      out.push_back({static_cast<const rex::filesystem::DiscImageEntry*>(child.get()), path});
    }
  }
}

class Installer final : public rex::ui::ImGuiDialog {
 public:
  Installer(rex::ui::ImGuiDrawer* drawer, std::filesystem::path target,
            std::function<void(std::filesystem::path)> done,
            std::function<void(std::function<void()>)> defer)
      : ImGuiDialog(drawer),
        target_(std::move(target)),
        done_(std::move(done)),
        defer_(std::move(defer)) {}

  // --install_iso: start right away with that image.
  void StartIfGiven() {
    const std::string image = REXCVAR_GET(install_iso);
    if (!image.empty()) {
      Start(std::filesystem::u8path(image));
    }
  }

  ~Installer() override {
    cancel_ = true;
    if (worker_.joinable()) {
      worker_.join();
    }
  }

 protected:
  void OnDraw(ImGuiIO& io) override {
    // ImGui is linked into both the runtime and this exe; draw in the
    // runtime's context.
    ImGui::SetCurrentContext(io.Ctx);
    const State state = state_.load();
    if (state == State::kDone) {
      return;
    }
    // The start screen's layout (burstlimit_launcher.cpp), drawn plainly: the
    // game's art (its font, logo, Goku) isn't installed yet. 1280x720 units in
    // the window's 16:9 middle.
    namespace text = rex::ui::overlay_text;
    // Behind ImGui windows: the settings menu (F1, a window) opens over this.
    ImDrawList* draw = ImGui::GetBackgroundDrawList();
    // The settings menu's button hints for the controller in use.
    static constexpr rex::ui::ButtonGlyphs kGlyphs[3] = {rex::ui::ButtonGlyphs::kXbox,
                                                         rex::ui::ButtonGlyphs::kPlayStation,
                                                         rex::ui::ButtonGlyphs::kNintendo};
    const int style = BurstLimitButtonStyleNow();
    if (style >= 0 && style < 3) {
      rex::ui::SetButtonGlyphs(kGlyphs[style]);
    }
    const float frame_w = std::min(io.DisplaySize.x, io.DisplaySize.y * 16.0f / 9.0f);
    const float u = frame_w / 1280.0f;
    const ImVec2 origin((io.DisplaySize.x - frame_w) * 0.5f,
                        (io.DisplaySize.y - frame_w * 9.0f / 16.0f) * 0.5f);
    auto at = [&](float x, float y) { return ImVec2(origin.x + x * u, origin.y + y * u); };

    // The title screen's red sky, roughly.
    draw->AddRectFilled(ImVec2(0, 0), io.DisplaySize, IM_COL32(0, 0, 0, 255));
    draw->AddRectFilledMultiColor(at(0, 0), at(1280, 720), IM_COL32(58, 10, 16, 255),
                                  IM_COL32(104, 26, 30, 255), IM_COL32(52, 12, 18, 255),
                                  IM_COL32(18, 4, 8, 255));
    // A soft glow where the title screen's aura is.
    for (int i = 0; i < 8; ++i) {
      draw->AddCircleFilled(at(940, 300), (360.0f - 40.0f * float(i)) * u,
                            IM_COL32(170, 70, 70, 7), 96);
    }
    // The settings menu open over this: only the background behind it.
    if (rex::ui::QuickMenuDialog::IsOpen()) {
      pad_seen_ = false;  // what closed it isn't a press here
      return;
    }

    auto outlined = [&](float size, ImVec2 pos, ImU32 color, const char* value) {
      const float o = std::max(1.5f, size * 0.06f);
      for (const ImVec2 d : {ImVec2(-o, 0), ImVec2(o, 0), ImVec2(0, -o), ImVec2(0, o),
                             ImVec2(-o, -o), ImVec2(o, -o), ImVec2(-o, o), ImVec2(o, o)}) {
        text::Draw(draw, size, ImVec2(pos.x + d.x, pos.y + d.y), IM_COL32(30, 6, 10, 255), value);
      }
      text::Draw(draw, size, pos, color, value);
    };
    // Where the logo goes, the name.
    {
      const float big_size = 74.0f * u, mid_size = 64.0f * u, small_size = 30.0f * u;
      const float right = 1240.0f;
      const ImVec2 a = text::Measure(big_size, "DRAGON BALL Z");
      const ImVec2 b = text::Measure(mid_size, "BURST LIMIT");
      const ImVec2 c = text::Measure(small_size, "RECOMPILED");
      outlined(big_size, at(right - a.x / u, 30.0f), IM_COL32(255, 214, 40, 255), "DRAGON BALL Z");
      outlined(mid_size, at(right - b.x / u, 30.0f + a.y / u - 6.0f), IM_COL32(236, 190, 90, 255),
               "BURST LIMIT");
      outlined(small_size, at(right - c.x / u, 30.0f + (a.y + b.y) / u - 6.0f),
               IM_COL32(255, 255, 255, 255), "RECOMPILED");
    }

    std::string help, detail;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      detail = message_;
    }

    // The box: dark at the top to purple, beige rim; rows 42 apart.
    const float x0 = 86.0f, x1 = 616.0f, y0 = 60.0f;
    const float first_row = y0 + 30.0f, pitch = 42.0f, bar = 38.0f, item = 26.0f;
    const bool working = state == State::kWorking;
    const int rows = working ? 1 : 2;
    const float y1 = first_row + float(rows) * pitch + (working ? 58.0f : 22.0f);
    const float radius = 14.0f * u;
    draw->AddRectFilledMultiColor(at(x0, y0 + 14.0f), at(x1, y1 - 14.0f),
                                  IM_COL32(16, 6, 10, 245), IM_COL32(16, 6, 10, 245),
                                  IM_COL32(64, 10, 98, 245), IM_COL32(64, 10, 98, 245));
    draw->AddRectFilled(at(x0, y0), at(x1, y0 + 28.0f), IM_COL32(16, 6, 10, 245), radius,
                        ImDrawFlags_RoundCornersTop);
    draw->AddRectFilled(at(x0, y1 - 28.0f), at(x1, y1), IM_COL32(64, 10, 98, 245), radius,
                        ImDrawFlags_RoundCornersBottom);
    auto highlight = [&](float top) {
      const float mid = x0 + (x1 - x0) * 0.45f;
      draw->AddRectFilledMultiColor(at(x0 + 3.0f, top), at(mid, top + bar),
                                    IM_COL32(150, 120, 214, 255), IM_COL32(89, 47, 131, 255),
                                    IM_COL32(89, 47, 131, 255), IM_COL32(150, 120, 214, 255));
      draw->AddRectFilledMultiColor(at(mid, top), at(x1 - 3.0f, top + bar),
                                    IM_COL32(89, 47, 131, 255), IM_COL32(54, 9, 85, 120),
                                    IM_COL32(54, 9, 85, 120), IM_COL32(89, 47, 131, 255));
    };
    auto row_text = [&](float top, const char* value, ImU32 color) {
      const ImVec2 size = text::Measure(item * u, value);
      outlined(item * u, ImVec2(at(x0 + 24.0f, 0).x, at(0, top).y + (bar * u - size.y) * 0.5f),
               color, value);
    };

    int activated = -1;
    const bool menu_open = rex::ui::QuickMenuDialog::IsOpen();
    if (working) {
      const uint64_t total = total_.load();
      const uint64_t done = done_bytes_.load();
      const float fraction = total ? float(double(done) / double(total)) : 0.0f;
      highlight(first_row);
      char line[96];
      std::snprintf(line, sizeof(line), "Installing...  %d%%", int(fraction * 100.0f));
      row_text(first_row, line, IM_COL32(255, 255, 255, 255));
      const ImVec2 track_min = at(x0 + 24.0f, first_row + pitch + 6.0f);
      const ImVec2 track_max = at(x1 - 24.0f, first_row + pitch + 26.0f);
      draw->AddRectFilled(track_min, track_max, IM_COL32(18, 4, 32, 255), 8.0f * u);
      if (fraction > 0.0f) {
        draw->AddRectFilledMultiColor(
            track_min, ImVec2(track_min.x + (track_max.x - track_min.x) * std::min(fraction, 1.0f),
                              track_max.y),
            IM_COL32(255, 220, 60, 255), IM_COL32(255, 150, 10, 255),
            IM_COL32(255, 150, 10, 255), IM_COL32(255, 220, 60, 255));
      }
      draw->AddRect(track_min, track_max, IM_COL32(194, 180, 142, 160), 8.0f * u, 0, 1.5f * u);
      help = "Installing the game files: " + GigaBytes(done) + " of " + GigaBytes(total) +
             ". This only happens once.";
    } else {
      // The choices: the chosen one lit up. Keyboard, mouse and any
      // controller (SDL: Xbox, PlayStation, Switch).
      const char* labels[2] = {state == State::kError ? "Choose another .iso..." : "Choose .iso...",
                               "Quit"};
      const rex::ui::QuickMenuDialog::PadState pads = rex::ui::ReadGamepadsBeforeGame();
      uint16_t buttons = pads.buttons;
      if (pads.thumb_ly > 16000) buttons |= 0x0001;
      if (pads.thumb_ly < -16000) buttons |= 0x0002;
      const uint16_t pressed = pad_seen_ ? uint16_t(buttons & ~pad_buttons_) : 0;
      pad_buttons_ = buttons;
      pad_seen_ = true;
      if (!menu_open && (ImGui::IsKeyPressed(ImGuiKey_DownArrow) ||
                         ImGui::IsKeyPressed(ImGuiKey_UpArrow) || (pressed & 0x0003))) {
        selected_ = 1 - selected_;
      }
      for (int row = 0; row < 2; ++row) {
        const float top = first_row + float(row) * pitch;
        const ImVec2 min = at(x0 + 3.0f, top), max = at(x1 - 3.0f, top + bar);
        const bool hover = io.MousePos.x >= min.x && io.MousePos.x < max.x &&
                           io.MousePos.y >= min.y && io.MousePos.y < max.y;
        if (!menu_open && hover && (io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f)) {
          selected_ = row;
        }
        if (!menu_open && hover && io.MouseClicked[0]) {
          activated = row;
        }
        if (selected_ == row) {
          highlight(top);
        }
        row_text(top, labels[row], IM_COL32(255, 255, 255, 255));
      }
      if (!menu_open && (ImGui::IsKeyPressed(ImGuiKey_Enter, false) ||
                         ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false) ||
                         (pressed & (0x1000 | 0x0010)))) {  // A (Cross) or Start
        activated = selected_;
      }
      help = selected_ == 0
                 ? "The game files weren't found. Choose your Dragon Ball Z: Burst Limit (USA) "
                   "Xbox 360 disc image (.iso): its files are copied next to burstlimit.exe "
                   "(about 3.3 GB). This only happens once, and the .iso isn't changed."
                 : "Close the game.";
    }
    draw->AddRect(at(x0, y0), at(x1, y1), IM_COL32(194, 180, 142, 255), radius, 0, 3.0f * u);

    // The band: a line, the description (and what went wrong), the prompts.
    const float band_top = 516.0f, band_bottom = 680.0f;
    draw->AddRectFilled(at(0, band_top - 6.0f), at(1280, band_top - 3.0f),
                        IM_COL32(255, 255, 255, 200));
    draw->AddRectFilledMultiColor(at(0, band_top), at(1280, band_bottom),
                                  IM_COL32(10, 3, 12, 235), IM_COL32(10, 3, 12, 235),
                                  IM_COL32(86, 24, 98, 235), IM_COL32(86, 24, 98, 235));
    const float help_size = 24.0f * u;
    text::Draw(draw, help_size, at(104.0f, 532.0f), IM_COL32(255, 255, 255, 255), help,
               1070.0f * u);
    if (!detail.empty() && state != State::kChoose) {
      text::Draw(draw, 20.0f * u, at(104.0f, 604.0f),
                 state == State::kError ? IM_COL32(255, 130, 120, 255) : IM_COL32(200, 180, 230, 255),
                 detail, 1070.0f * u);
    }
    if (!working) {
      // Arrows Select, the confirm button Confirm.
      const float prompt = 26.0f * u, r = 13.0f * u;
      const ImVec2 select_size = text::Measure(prompt, "Select");
      const ImVec2 confirm_size = text::Measure(prompt, "Confirm");
      const float total = 2.0f * r * 2.0f + 6.0f * u + select_size.x + 18.0f * u + 2.0f * r +
                          6.0f * u + confirm_size.x;
      float px = at(1200.0f, 0).x - total;
      const float mid = at(0, 654.0f).y;
      for (int i = 0; i < 2; ++i) {
        const float cx = px + r + float(i) * 2.0f * r, h = r * 0.9f;
        const ImU32 yellow = IM_COL32(255, 222, 0, 255);
        if (i == 0) {
          draw->AddTriangleFilled(ImVec2(cx, mid - h), ImVec2(cx + h * 0.8f, mid + h * 0.2f),
                                  ImVec2(cx - h * 0.8f, mid + h * 0.2f), yellow);
          draw->AddRectFilled(ImVec2(cx - h * 0.3f, mid), ImVec2(cx + h * 0.3f, mid + h), yellow);
        } else {
          draw->AddTriangleFilled(ImVec2(cx, mid + h), ImVec2(cx - h * 0.8f, mid - h * 0.2f),
                                  ImVec2(cx + h * 0.8f, mid - h * 0.2f), yellow);
          draw->AddRectFilled(ImVec2(cx - h * 0.3f, mid - h), ImVec2(cx + h * 0.3f, mid), yellow);
        }
      }
      px += 4.0f * r + 6.0f * u;
      outlined(prompt, ImVec2(px, mid - select_size.y * 0.5f), IM_COL32(255, 255, 255, 255),
               "Select");
      px += select_size.x + 18.0f * u;
      // The bottom face button, like the controller in use.
      const ImVec2 c(px + r, mid);
      if (style == 1) {
        draw->AddCircleFilled(c, r, IM_COL32(34, 38, 46, 255), 32);
        draw->AddCircle(c, r - 0.75f, IM_COL32(92, 100, 114, 255), 32, 1.5f);
        const float k = r * 0.46f, t = std::max(1.5f, r * 0.15f);
        draw->AddLine(ImVec2(c.x - k, c.y - k), ImVec2(c.x + k, c.y + k), IM_COL32(124, 178, 236, 255), t);
        draw->AddLine(ImVec2(c.x - k, c.y + k), ImVec2(c.x + k, c.y - k), IM_COL32(124, 178, 236, 255), t);
      } else {
        draw->AddCircleFilled(c, r, style == 2 ? IM_COL32(58, 60, 66, 255) : IM_COL32(22, 150, 62, 255), 32);
        const char* letter = style == 2 ? "B" : "A";
        const ImVec2 ls = text::Measure(r * 1.27f, letter);
        text::Draw(draw, r * 1.27f, ImVec2(c.x - ls.x * 0.5f, c.y - ls.y * 0.5f),
                   IM_COL32(255, 255, 255, 255), letter);
      }
      px += 2.0f * r + 6.0f * u;
      outlined(prompt, ImVec2(px, mid - confirm_size.y * 0.5f), IM_COL32(255, 255, 255, 255),
               "Confirm");
    }
    const std::string version = std::string("Recompiled ") + BURSTLIMIT_VERSION;
    text::Draw(draw, 18.0f * u, at(14.0f, 694.0f), IM_COL32(255, 255, 255, 150), version);
    // The file picker is modal: open it outside this frame.
    if (activated == 0) {
      defer_([this] { ChooseImage(); });
    } else if (activated == 1) {
      ExitProcess(0);
    }
  }

 private:
  enum class State { kChoose, kWorking, kError, kDone };
  uint16_t pad_buttons_ = 0;
  bool pad_seen_ = false;

  void ChooseImage() {
    if (choosing_ || state_ == State::kWorking) {
      return;
    }
    choosing_ = true;
    wchar_t file[MAX_PATH * 4] = {};
    OPENFILENAMEW dialog = {};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = GetActiveWindow();
    dialog.lpstrFilter = L"Xbox 360 disc image (*.iso)\0*.iso\0All files\0*.*\0";
    dialog.lpstrFile = file;
    dialog.nMaxFile = DWORD(std::size(file));
    dialog.lpstrTitle = L"Select your Dragon Ball Z: Burst Limit (USA) disc image";
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER;
    const bool picked = GetOpenFileNameW(&dialog) != 0;
    choosing_ = false;
    if (picked) {
      Start(std::filesystem::path(file));
    }
  }

  void Start(const std::filesystem::path& image) {
    if (worker_.joinable()) {
      worker_.join();
    }
    total_ = 0;
    done_bytes_ = 0;
    SetMessage("Reading the disc image...");
    state_ = State::kWorking;
    worker_ = std::thread([this, image] { Install(image); });
  }

  void SetMessage(std::string message) {
    std::lock_guard<std::mutex> lock(mutex_);
    message_ = std::move(message);
  }

  void Fail(std::string message) {
    REXLOG_ERROR("Install: {}", message);
    SetMessage(std::move(message));
    state_ = State::kError;
  }

  void Install(const std::filesystem::path& image) {
    REXLOG_INFO("Install: from {}", image.string());
    rex::filesystem::DiscImageDevice disc("\\Device\\BurstLimitInstall", image);
    if (!disc.Initialize() || !disc.root()) {
      Fail("This file isn't an Xbox 360 disc image.");
      return;
    }
    const auto* xex = static_cast<const rex::filesystem::DiscImageEntry*>(
        FindChild(disc.root(), "default.xex"));
    if (!xex) {
      Fail("This disc image has no default.xex - it isn't an Xbox 360 game disc.");
      return;
    }
    const uint8_t* xex_data = xex->mmap()->data() + xex->data_offset();
    const std::string sha1 = Sha1Hex(xex_data, xex->data_size());
    if (sha1 != kUsaXexSha1) {
      REXLOG_ERROR("Install: default.xex SHA-1 {} (size {})", sha1, xex->data_size());
      Fail("This isn't the Dragon Ball Z: Burst Limit USA (NTSC-U) disc. The recompilation "
           "only works with the USA version.");
      return;
    }

    std::vector<CopyItem> files;
    CollectFiles(disc.root(), {}, files);
    // default.xex last (it marks the install as complete); the rest in disc
    // order, so a hard drive reads the image front to back.
    std::stable_sort(files.begin(), files.end(), [xex](const CopyItem& a, const CopyItem& b) {
      if ((a.entry == xex) != (b.entry == xex)) {
        return b.entry == xex;
      }
      return a.entry->data_offset() < b.entry->data_offset();
    });
    uint64_t total = 0;
    for (const CopyItem& item : files) {
      total += item.entry->data_size();
    }
    total_ = total;

    std::error_code error;
    std::filesystem::create_directories(target_, error);
    const auto space = std::filesystem::space(target_, error);
    if (!error && space.available < total + (64ull << 20)) {
      Fail("Not enough free space next to burstlimit.exe: the game needs " + GigaBytes(total) +
           ", " + GigaBytes(space.available) + " is free.");
      return;
    }

    HANDLE source = CreateFileW(image.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (source == INVALID_HANDLE_VALUE) {
      Fail("Couldn't open the disc image.");
      return;
    }
    std::vector<uint8_t> buffer(8u << 20);
    bool ok = true;
    for (const CopyItem& item : files) {
      if (cancel_) {
        ok = false;
        break;
      }
      SetMessage(item.relative.generic_string());
      const std::filesystem::path path = target_ / item.relative;
      std::filesystem::create_directories(path.parent_path(), error);
      HANDLE out = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
      if (out == INVALID_HANDLE_VALUE) {
        Fail("Couldn't write " + path.string());
        ok = false;
        break;
      }
      LARGE_INTEGER position;
      position.QuadPart = LONGLONG(item.entry->data_offset());
      SetFilePointerEx(source, position, nullptr, FILE_BEGIN);
      uint64_t left = item.entry->data_size();
      while (left && !cancel_) {
        const DWORD chunk = DWORD(std::min<uint64_t>(left, buffer.size()));
        DWORD read = 0, written = 0;
        if (!ReadFile(source, buffer.data(), chunk, &read, nullptr) || read != chunk) {
          Fail("Couldn't read the disc image (" + item.relative.generic_string() + ").");
          ok = false;
          break;
        }
        if (!WriteFile(out, buffer.data(), chunk, &written, nullptr) || written != chunk) {
          Fail("Couldn't write " + path.string() + " - is the drive full?");
          ok = false;
          break;
        }
        left -= chunk;
        done_bytes_ += chunk;
      }
      CloseHandle(out);
      if (!ok || cancel_) {
        // Don't leave a half-written default.xex looking installed.
        std::filesystem::remove(path, error);
        break;
      }
    }
    CloseHandle(source);
    if (!ok || cancel_) {
      return;
    }
    // The release zip's placeholder.
    std::filesystem::remove(target_ / "PUT_GAME_FILES_HERE.txt", error);
    REXLOG_INFO("Install: {} files, {} copied to {}", files.size(), GigaBytes(total),
                target_.string());
    state_ = State::kDone;
    // On the UI thread. `done` may destroy this dialog, so it runs from copies.
    defer_([this] {
      auto done = done_;
      auto target = target_;
      done(target);
    });
  }

  std::filesystem::path target_;
  std::function<void(std::filesystem::path)> done_;
  std::function<void(std::function<void()>)> defer_;
  std::atomic<State> state_{State::kChoose};
  std::atomic<uint64_t> total_{0};
  std::atomic<uint64_t> done_bytes_{0};
  std::atomic<bool> cancel_{false};
  bool choosing_ = false;
  int selected_ = 0;
  std::mutex mutex_;
  std::string message_;
  std::thread worker_;
};

}  // namespace

std::unique_ptr<rex::ui::ImGuiDialog> BurstLimitCreateInstaller(
    rex::ui::ImGuiDrawer* drawer, const std::filesystem::path& target,
    std::function<void(std::filesystem::path)> done,
    std::function<void(std::function<void()>)> defer) {
  auto installer = std::make_unique<Installer>(drawer, target, std::move(done), std::move(defer));
  installer->StartIfGiven();
  return installer;
}
