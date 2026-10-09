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
    // Close to the game's own menus (purple, yellow highlight), drawn here:
    // the game's art isn't installed yet.
    namespace text = rex::ui::overlay_text;
    const float u = io.DisplaySize.y / 1080.0f;
    ImDrawList* draw = ImGui::GetForegroundDrawList();
    draw->AddRectFilledMultiColor(ImVec2(0, 0), io.DisplaySize, IM_COL32(74, 22, 112, 255),
                                  IM_COL32(52, 14, 86, 255), IM_COL32(16, 4, 30, 255),
                                  IM_COL32(26, 8, 46, 255));

    const float width = std::min(io.DisplaySize.x - 60.0f * u, 1100.0f * u);
    const float pad = 44.0f * u;
    const float wrap = width - pad * 2.0f;
    const float title_size = 44.0f * u, body_size = 28.0f * u, small_size = 24.0f * u;
    auto height_of = [&](float size, const std::string& value) {
      return text::Font(size)->CalcTextSizeA(size, FLT_MAX, wrap, value.c_str()).y;
    };

    const std::string title = "Dragon Ball Z: Burst Limit Recompiled";
    std::string body, detail;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      detail = message_;
    }
    if (state == State::kWorking) {
      body = "Installing the game files. This only happens once.";
    } else {
      body =
          "The game files weren't found. Select your Dragon Ball Z: Burst Limit (USA) Xbox 360 "
          "disc image (.iso). Its files are copied next to burstlimit.exe (about 3.3 GB). "
          "This only happens once, and the .iso isn't changed.";
    }
    const float button_height = 64.0f * u;
    float height = pad + height_of(title_size, title) + 28.0f * u + height_of(body_size, body) +
                   30.0f * u;
    if (state == State::kWorking) {
      height += 40.0f * u + 16.0f * u + height_of(small_size, detail);
    } else {
      if (state == State::kError) {
        height += height_of(body_size, detail) + 30.0f * u;
      }
      height += button_height * 2.0f;
    }
    height += pad - 16.0f * u;

    const ImVec2 panel_min((io.DisplaySize.x - width) * 0.5f, (io.DisplaySize.y - height) * 0.5f);
    const ImVec2 panel_max(panel_min.x + width, panel_min.y + height);
    // The game's window: purple, lighter at the top, a thin light rim.
    const float radius = 16.0f * u;
    const ImU32 top_color = IM_COL32(92, 52, 140, 245), bottom_color = IM_COL32(40, 14, 72, 245);
    draw->AddRectFilled(panel_min, ImVec2(panel_max.x, panel_min.y + radius * 2.0f), top_color,
                        radius, ImDrawFlags_RoundCornersTop);
    draw->AddRectFilled(ImVec2(panel_min.x, panel_max.y - radius * 2.0f), panel_max,
                        bottom_color, radius, ImDrawFlags_RoundCornersBottom);
    draw->AddRectFilledMultiColor(ImVec2(panel_min.x, panel_min.y + radius),
                                  ImVec2(panel_max.x, panel_max.y - radius), top_color, top_color,
                                  bottom_color, bottom_color);
    draw->AddRect(panel_min, panel_max, IM_COL32(232, 222, 196, 230), radius, 0, 3.0f * u);

    float y = panel_min.y + pad;
    const float x = panel_min.x + pad;
    text::Draw(draw, title_size, ImVec2(x, y), IM_COL32(255, 214, 60, 255), title, wrap);
    y += height_of(title_size, title) + 12.0f * u;
    draw->AddLine(ImVec2(panel_min.x + 24.0f * u, y), ImVec2(panel_max.x - 24.0f * u, y),
                  IM_COL32(210, 196, 220, 150), 2.0f * u);
    y += 16.0f * u;
    text::Draw(draw, body_size, ImVec2(x, y), IM_COL32(240, 236, 248, 255), body, wrap);
    y += height_of(body_size, body) + 30.0f * u;

    if (state == State::kWorking) {
      const uint64_t total = total_.load();
      const uint64_t done = done_bytes_.load();
      const float fraction = total ? float(double(done) / double(total)) : 0.0f;
      const ImVec2 track_min(x, y), track_max(x + wrap, y + 40.0f * u);
      draw->AddRectFilled(track_min, track_max, IM_COL32(18, 4, 32, 255), 10.0f * u);
      if (fraction > 0.0f) {
        draw->AddRectFilledMultiColor(
            track_min, ImVec2(x + wrap * std::min(fraction, 1.0f), track_max.y),
            IM_COL32(255, 220, 60, 255), IM_COL32(255, 150, 10, 255),
            IM_COL32(255, 150, 10, 255), IM_COL32(255, 220, 60, 255));
      }
      draw->AddRect(track_min, track_max, IM_COL32(200, 150, 255, 120), 10.0f * u, 0, 2.0f * u);
      char percent[64];
      std::snprintf(percent, sizeof(percent), "%d%%   %s / %s", int(fraction * 100.0f),
                    GigaBytes(done).c_str(), GigaBytes(total).c_str());
      const ImVec2 percent_size = text::Measure(small_size, percent);
      const ImVec2 percent_pos(x + (wrap - percent_size.x) * 0.5f,
                               y + (40.0f * u - percent_size.y) * 0.5f);
      text::Draw(draw, small_size, ImVec2(percent_pos.x + 2.0f * u, percent_pos.y + 2.0f * u),
                 IM_COL32(40, 10, 60, 255), percent);
      text::Draw(draw, small_size, percent_pos, IM_COL32(255, 255, 255, 255), percent);
      y += 40.0f * u + 16.0f * u;
      text::Draw(draw, small_size, ImVec2(x, y), IM_COL32(190, 160, 220, 255), detail, wrap);
      return;
    }

    if (state == State::kError) {
      text::Draw(draw, body_size, ImVec2(x, y), IM_COL32(255, 120, 110, 255), detail, wrap);
      y += height_of(body_size, detail) + 30.0f * u;
    }
    // The choices as rows, like the game's menus: the chosen one lit up.
    const char* labels[2] = {state == State::kError ? "Choose another .iso..." : "Choose .iso...",
                             "Quit"};
    if (ImGui::IsKeyPressed(ImGuiKey_DownArrow) || ImGui::IsKeyPressed(ImGuiKey_UpArrow)) {
      selected_ = 1 - selected_;
    }
    int activated = -1;
    for (int row = 0; row < 2; ++row) {
      const ImVec2 min(panel_min.x + 10.0f * u, y), max(panel_max.x - 10.0f * u, y + button_height);
      const bool hover = io.MousePos.x >= min.x && io.MousePos.x < max.x &&
                         io.MousePos.y >= min.y && io.MousePos.y < max.y;
      if (hover && (io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f)) {
        selected_ = row;
      }
      if (hover && io.MouseClicked[0]) {
        activated = row;
      }
      if (selected_ == row) {
        draw->AddRectFilledMultiColor(min, max, IM_COL32(176, 140, 236, 235),
                                      IM_COL32(120, 80, 190, 0), IM_COL32(120, 80, 190, 0),
                                      IM_COL32(176, 140, 236, 235));
      }
      const ImVec2 label_size = text::Measure(body_size, labels[row]);
      text::Draw(draw, body_size,
                 ImVec2(x, min.y + (button_height - label_size.y) * 0.5f),
                 selected_ == row ? IM_COL32(255, 255, 255, 255) : IM_COL32(196, 186, 210, 255),
                 labels[row]);
      y += button_height;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) ||
        ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false)) {
      activated = selected_;
    }
    // The file picker is modal: open it outside this frame.
    if (activated == 0) {
      defer_([this] { ChooseImage(); });
    } else if (activated == 1) {
      ExitProcess(0);
    }
  }

 private:
  enum class State { kChoose, kWorking, kError, kDone };

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
