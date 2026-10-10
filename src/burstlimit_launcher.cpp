// The start screen: shown before the game starts (start_screen), in the style
// of the first-start installer (burstlimit_install.cpp). Play starts the game;
// Mods switches the mods on and off; Settings opens the settings menu (F1);
// when GitHub has a newer release, Update downloads it, puts its files next to
// the exe (keeping burstlimit.toml, the game files, saves, mods and textures)
// and restarts. The Online part shows what the lobby has right now - players
// connected, how many run this version, and this version's open rooms by host
// name - from the lobby's GET /v1/rooms, refreshed every 10 seconds.
//
// Mouse, keyboard (arrows, Enter, Escape) and controllers (XInput: D-pad /
// stick, A or Start, B) work; the game's own input isn't running yet.

#include <algorithm>
#include <array>
#include <atomic>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <windows.h>
#include <shellapi.h>
#include <winhttp.h>
#include <xinput.h>

#include <imgui.h>
#include <nlohmann/json.hpp>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/net/online.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/overlay/overlay_text.h>
#include <rex/ui/overlay/quick_menu.h>

#include "burstlimit_menuart.h"

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "xinput.lib")

REXCVAR_DEFINE_BOOL(start_screen, true, "Patches",
                    "Show the start screen (Play, mods, settings, updates, who's online) before "
                    "the game starts");
REXCVAR_DEFINE_STRING(
    update_check_url,
    "https://api.github.com/repos/iExplosiveRage/DBZ-Burst-Limit-Recompiled/releases/latest",
    "Patches", "Where the start screen looks for a newer release (empty = never)");

// burstlimit_menusound.cpp: 0 move, 1 confirm, 2 back, 3 not possible, 4 start.
void BurstLimitPlayMenuSound(int sound);
// burstlimit_buttons.cpp: the button_icons style now (0 Xbox, 1 PlayStation, 2 Switch).
int BurstLimitButtonStyleNow();
// burstlimit_mods.cpp
std::vector<std::pair<std::string, bool>> BurstLimitModsList();
std::string BurstLimitModsSetBeforeBoot(const std::vector<bool>& on);
// burstlimit_saves.cpp
std::string BurstLimitUnzip(
    const std::vector<uint8_t>& zip,
    const std::function<std::string(const std::string& name, const std::vector<uint8_t>& data)>&
        each);

namespace {

using json = nlohmann::json;
namespace fs = std::filesystem;

// Debug commands, for tests driven through the command pipe (the game's input,
// which they use, isn't running on this screen): start_screen_play, and
// start_screen_key up / down / a / b.
std::atomic<bool> g_play_requested{false};
std::mutex g_keys_mutex;
std::vector<std::string> g_keys;

void PlayCommand() {
  g_play_requested.store(true);
}

void KeyCommand(std::string_view args) {
  std::lock_guard<std::mutex> lock(g_keys_mutex);
  g_keys.emplace_back(args);
}

// GET over WinHTTP (http / https, redirects followed). The body goes to `sink`
// piece by piece with the Content-Length (0 when unknown); false = stop.
bool HttpGet(const std::wstring& url, int timeout_ms,
             const std::function<bool(const char* data, size_t size, uint64_t total)>& sink) {
  URL_COMPONENTS parts = {};
  parts.dwStructSize = sizeof(parts);
  wchar_t host[256] = {}, path[2048] = {};
  parts.lpszHostName = host;
  parts.dwHostNameLength = DWORD(std::size(host));
  parts.lpszUrlPath = path;
  parts.dwUrlPathLength = DWORD(std::size(path));
  wchar_t extra[2048] = {};
  parts.lpszExtraInfo = extra;
  parts.dwExtraInfoLength = DWORD(std::size(extra));
  if (!WinHttpCrackUrl(url.c_str(), 0, 0, &parts)) {
    return false;
  }
  HINTERNET session = WinHttpOpen(L"burstlimit", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                  WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (!session) {
    return false;
  }
  WinHttpSetTimeouts(session, timeout_ms, timeout_ms, timeout_ms, timeout_ms);
  HINTERNET connection = WinHttpConnect(session, host, parts.nPort, 0);
  HINTERNET request = nullptr;
  if (connection) {
    const std::wstring object = std::wstring(path) + extra;
    request = WinHttpOpenRequest(connection, L"GET", object.c_str(), nullptr, WINHTTP_NO_REFERER,
                                 WINHTTP_DEFAULT_ACCEPT_TYPES,
                                 parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
  }
  bool ok = false;
  DWORD status = 0, status_size = sizeof(status);
  if (request && WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                    WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
      WinHttpReceiveResponse(request, nullptr) &&
      WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                          WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size,
                          WINHTTP_NO_HEADER_INDEX) &&
      status == 200) {
    DWORD length = 0, length_size = sizeof(length);
    uint64_t total = 0;
    if (WinHttpQueryHeaders(request, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &length, &length_size,
                            WINHTTP_NO_HEADER_INDEX)) {
      total = length;
    }
    ok = true;
    std::vector<char> chunk;
    for (;;) {
      DWORD available = 0;
      if (!WinHttpQueryDataAvailable(request, &available)) {
        ok = false;
        break;
      }
      if (available == 0) {
        break;
      }
      chunk.resize(available);
      DWORD read = 0;
      if (!WinHttpReadData(request, chunk.data(), available, &read) || read == 0) {
        ok = false;
        break;
      }
      if (!sink(chunk.data(), read, total)) {
        ok = false;
        break;
      }
    }
  }
  if (request) WinHttpCloseHandle(request);
  if (connection) WinHttpCloseHandle(connection);
  WinHttpCloseHandle(session);
  return ok;
}

// A small text answer (JSON), or empty.
std::string HttpGetText(const std::wstring& url, int timeout_ms) {
  std::string body;
  const bool ok = HttpGet(url, timeout_ms, [&body](const char* data, size_t size, uint64_t) {
    body.append(data, size);
    return body.size() < 1024 * 1024;
  });
  return ok ? body : std::string();
}

std::wstring Widen(const std::string& text) {
  if (text.empty()) {
    return {};
  }
  const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), nullptr, 0);
  std::wstring out(size, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), out.data(), size);
  return out;
}

std::string Narrow(const fs::path& path) {
  const std::wstring& wide = path.native();
  if (wide.empty()) {
    return {};
  }
  const int size =
      WideCharToMultiByte(CP_UTF8, 0, wide.data(), int(wide.size()), nullptr, 0, nullptr, nullptr);
  std::string out(size, '\0');
  WideCharToMultiByte(CP_UTF8, 0, wide.data(), int(wide.size()), out.data(), size, nullptr, nullptr);
  return out;
}

std::string UrlEncode(const std::string& text) {
  static constexpr char kHex[] = "0123456789ABCDEF";
  std::string out;
  for (unsigned char c : text) {
    if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      out += char(c);
    } else {
      out += '%';
      out += kHex[c >> 4];
      out += kHex[c & 15];
    }
  }
  return out;
}

// The lobby's rooms URL from online_lobby_url (wss://host/v1/ws -> https://host/v1/rooms).
std::string RoomsUrl() {
  std::string url = rex::cvar::GetFlagByName("online_lobby_url");
  if (url.rfind("wss://", 0) == 0) {
    url = "https://" + url.substr(6);
  } else if (url.rfind("ws://", 0) == 0) {
    url = "http://" + url.substr(5);
  } else {
    return {};
  }
  const size_t ws = url.rfind("/ws");
  if (ws == std::string::npos) {
    return {};
  }
  return url.substr(0, ws) + "/rooms?version=" + UrlEncode(rex::net::online::LobbyVersion());
}

// "v1.2.3" anywhere in `text` (a release tag, this build's version).
bool ParseVersion(const std::string& text, std::array<int, 3>& version) {
  for (size_t p = text.find('v'); p != std::string::npos; p = text.find('v', p + 1)) {
    if (std::sscanf(text.c_str() + p, "v%d.%d.%d", &version[0], &version[1], &version[2]) == 3) {
      return true;
    }
  }
  return false;
}

bool UnderWine() {
  HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
  return ntdll && GetProcAddress(ntdll, "wine_get_version");
}

fs::path ExeDirectory() {
  wchar_t exe_path[MAX_PATH] = {};
  const DWORD length = GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
  if (length == 0 || length >= MAX_PATH) {
    return fs::current_path();
  }
  return fs::path(exe_path).parent_path();
}

std::string Lower(std::string text) {
  for (char& c : text) {
    c = char(std::tolower(static_cast<unsigned char>(c)));
  }
  return text;
}

// What an update never replaces: the player's settings and everything they
// put next to the exe.
bool KeptByUpdate(const std::string& relative) {
  const std::string path = Lower(relative);
  if (path == "burstlimit.toml") {
    return true;
  }
  for (const char* folder : {"game_data_root/", "userdata/", "mods/", "textures/", "saves/"}) {
    if (path.rfind(folder, 0) == 0) {
      return true;
    }
  }
  return false;
}

// Puts a release zip's files next to the exe. Everything is unpacked into a
// staging folder first, so a damaged download changes nothing; files in use
// (the exe, its DLLs) are renamed to *.old, which the next start removes.
std::string InstallUpdate(const std::vector<uint8_t>& zip, const fs::path& exe_directory) {
  const fs::path staging = exe_directory / "update_staging";
  std::error_code ec;
  fs::remove_all(staging, ec);
  std::vector<std::string> names;
  std::string error = BurstLimitUnzip(
      zip, [&](const std::string& name, const std::vector<uint8_t>& data) -> std::string {
        if (name.find("..") != std::string::npos || name.find(':') != std::string::npos ||
            name.empty() || name.front() == '/') {
          return "unexpected file name in the zip: " + name;
        }
        const fs::path target = staging / fs::path(Widen(name));
        std::error_code dir_ec;
        fs::create_directories(target.parent_path(), dir_ec);
        std::ofstream file(target, std::ios::binary | std::ios::trunc);
        file.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
        if (!file) {
          return "can't write " + Narrow(target);
        }
        names.push_back(name);
        return {};
      });
  if (!error.empty()) {
    fs::remove_all(staging, ec);
    return error;
  }
  // The zip's top folder (DBZ-Burst-Limit-Recompiled/) is the one with the exe.
  std::string prefix;
  bool has_exe = false;
  for (const std::string& name : names) {
    const std::string lower = Lower(name);
    if (lower == "burstlimit.exe" ||
        (lower.size() > 15 && lower.compare(lower.size() - 15, 15, "/burstlimit.exe") == 0)) {
      prefix = name.substr(0, name.size() - 14);
      has_exe = true;
      break;
    }
  }
  if (!has_exe) {
    fs::remove_all(staging, ec);
    return "the download has no burstlimit.exe";
  }
  int replaced = 0, kept = 0;
  for (const std::string& name : names) {
    if (name.rfind(prefix, 0) != 0) {
      continue;
    }
    const std::string relative = name.substr(prefix.size());
    if (relative.empty() || KeptByUpdate(relative)) {
      ++kept;
      continue;
    }
    const fs::path source = staging / fs::path(Widen(name));
    const fs::path target = exe_directory / fs::path(Widen(relative));
    fs::create_directories(target.parent_path(), ec);
    if (fs::exists(target, ec)) {
      fs::path old = target;
      old += ".old";
      std::error_code old_ec;
      fs::remove(old, old_ec);
      // Running files can be renamed, not overwritten.
      if (!MoveFileExW(target.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        fs::remove_all(staging, ec);
        return "can't replace " + relative + " (error " + std::to_string(GetLastError()) + ")";
      }
    }
    if (!MoveFileExW(source.c_str(), target.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED)) {
      fs::remove_all(staging, ec);
      return "can't write " + relative + " (error " + std::to_string(GetLastError()) + ")";
    }
    ++replaced;
  }
  fs::remove_all(staging, ec);
  REXLOG_INFO("Update: {} file(s) replaced, {} kept (settings, game files, user folders)",
              replaced, kept);
  return {};
}

// Starts the (new) exe with the same command line and ends this one.
void Restart() {
  wchar_t exe_path[MAX_PATH] = {};
  GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
  std::wstring command_line = GetCommandLineW();
  STARTUPINFOW startup = {};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process = {};
  if (CreateProcessW(exe_path, command_line.data(), nullptr, nullptr, FALSE, 0, nullptr,
                     nullptr, &startup, &process)) {
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
  } else {
    REXLOG_ERROR("Update: can't start {} again (error {})", Narrow(exe_path),
                 GetLastError());
  }
  ExitProcess(0);
}

struct RoomInfo {
  std::string host;
  bool locked = false;
};

struct LobbyInfo {
  enum class State { kOff, kLoading, kError, kOk } state = State::kLoading;
  int online = 0;
  int online_version = 0;
  int rooms_open = 0;
  int matches = 0;
  std::vector<RoomInfo> rooms;
};

struct UpdateInfo {
  enum class State {
    kOff,
    kChecking,
    kNone,
    kAvailable,
    kDownloading,
    kInstalling,
    kRestart,
    kError
  } state = State::kOff;
  std::string tag;
  std::string url;
  uint64_t size = 0;
  std::string error;
};

constexpr ImU32 kWhite = IM_COL32(240, 236, 248, 255);
constexpr ImU32 kMuted = IM_COL32(190, 172, 216, 255);
constexpr ImU32 kYellow = IM_COL32(255, 214, 60, 255);
constexpr ImU32 kGreen = IM_COL32(120, 230, 140, 255);
constexpr ImU32 kRed = IM_COL32(255, 120, 110, 255);

class StartScreen final : public rex::ui::ImGuiDialog {
 public:
  StartScreen(rex::ui::ImGuiDrawer* drawer, rex::ui::ImmediateDrawer* immediate_drawer,
              const fs::path& game_data_root, std::function<void()> play,
              std::function<void(std::function<void()>)> defer, std::function<void()> settings,
              fs::path config)
      : ImGuiDialog(drawer),
        immediate_drawer_(immediate_drawer),
        play_(std::move(play)),
        defer_(std::move(defer)),
        settings_(std::move(settings)),
        config_(std::move(config)) {
    if (immediate_drawer_ && !game_data_root.empty()) {
      art_ = BurstLimitSharedMenuArt(game_data_root);
    }
    url_ = RoomsUrl();
    if (url_.empty()) {
      info_.state = LobbyInfo::State::kOff;
    } else {
      worker_ = std::thread([this] { Poll(); });
    }
    const std::string update_url = REXCVAR_GET(update_check_url);
    if (!update_url.empty() && ParseVersion(BURSTLIMIT_VERSION, own_version_)) {
      update_.state = UpdateInfo::State::kChecking;
      update_thread_ = std::thread([this, update_url] { CheckForUpdate(update_url); });
    }
  }

  ~StartScreen() override {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stop_ = true;
    }
    wake_.notify_all();
    if (worker_.joinable()) {
      worker_.join();
    }
    if (update_thread_.joinable()) {
      update_thread_.join();
    }
  }

 protected:
  void OnDraw(ImGuiIO& io) override {
    ImGui::SetCurrentContext(io.Ctx);
    if (done_) {
      return;
    }
    // The settings menu's hints (L1 / R1, ...) for the controller in use.
    static constexpr rex::ui::ButtonGlyphs kGlyphs[3] = {
        rex::ui::ButtonGlyphs::kXbox, rex::ui::ButtonGlyphs::kPlayStation,
        rex::ui::ButtonGlyphs::kNintendo};
    const int style = BurstLimitButtonStyleNow();
    if (style >= 0 && style < 3) {
      rex::ui::SetButtonGlyphs(kGlyphs[style]);
    }
    // Behind ImGui windows: the settings menu (a window) opens over this screen.
    ImDrawList* draw = ImGui::GetBackgroundDrawList();
    // The game's own menu look once its art is read (a moment after the
    // screen shows), this screen's plain one until then or without it.
    const bool game_look = art_ && art_->Ready(immediate_drawer_);
    if (game_look) {
      DrawGameBackground(io, draw);
    } else {
      draw->AddRectFilledMultiColor(ImVec2(0, 0), io.DisplaySize, IM_COL32(74, 22, 112, 255),
                                    IM_COL32(52, 14, 86, 255), IM_COL32(16, 4, 30, 255),
                                    IM_COL32(26, 8, 46, 255));
    }
    // The settings menu open over this screen: only the background (it has
    // the keyboard and controllers then).
    if (rex::ui::QuickMenuDialog::IsOpen()) {
      Input ignored;
      ReadInput(io, ignored, true);
      return;
    }
    UpdateInfo update;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      update = update_;
    }

    // The rows of this page and the text under them.
    std::vector<Row> rows;
    std::string heading;
    std::vector<std::pair<std::string, ImU32>> lines;
    if (page_ == Page::kMain) {
      BuildMain(update, rows, heading, lines);
    } else {
      BuildMods(rows, heading, lines);
    }
    selected_ = std::clamp(selected_, 0, int(rows.size()) - 1);

    // Input: none while the settings menu is open over this screen (it has
    // the keyboard and controllers then).
    Input input;
    const bool menu_open = rex::ui::QuickMenuDialog::IsOpen();
    ReadInput(io, input, menu_open);

    if (game_look) {
      DrawGame(io, draw, rows, update, lines, input);
    } else {
      Draw(io, draw, rows, heading, lines, menu_open, input);
    }

    if (move_ != 0) {
      BurstLimitPlayMenuSound(0);
    }
    if (input.back && page_ == Page::kMods) {
      BurstLimitPlayMenuSound(2);
      page_ = Page::kMain;
      selected_ = main_mods_row_;
      return;
    }
    if (input.activate >= 0 && input.activate < int(rows.size())) {
      Activate(rows[input.activate], update);
    }
  }

 private:
  enum class Page { kMain, kMods };
  enum class Action { kPlay, kPlayNoShow, kUpdate, kMods, kSettings, kQuit, kMod, kModsFolder, kBack };

  struct Row {
    Action action;
    std::string label;
    std::string value;  // on the right (a mod's ON / OFF)
    ImU32 color = 0;    // 0 = the usual
    int index = 0;      // kMod: the mod
    bool big = false;
  };

  struct Input {
    int activate = -1;
    bool back = false;
  };

  void BuildMain(const UpdateInfo& update, std::vector<Row>& rows, std::string& heading,
                 std::vector<std::pair<std::string, ImU32>>& lines) {
    rows.push_back({Action::kPlay, "Play", "", 0, 0, true});
    char text[200];
    switch (update.state) {
      case UpdateInfo::State::kAvailable:
        std::snprintf(text, sizeof(text), "Update to %s (%.0f MB)", update.tag.c_str(),
                      double(update.size) / (1024.0 * 1024.0));
        rows.push_back({Action::kUpdate, text, "", kGreen});
        break;
      case UpdateInfo::State::kDownloading: {
        const uint64_t done = download_done_.load(), total = download_total_.load();
        if (total) {
          std::snprintf(text, sizeof(text), "Downloading %s...  %d%%", update.tag.c_str(),
                        int(done * 100 / total));
        } else {
          std::snprintf(text, sizeof(text), "Downloading %s...  %.0f MB", update.tag.c_str(),
                        double(done) / (1024.0 * 1024.0));
        }
        rows.push_back({Action::kUpdate, text, "", kGreen});
        break;
      }
      case UpdateInfo::State::kInstalling:
        rows.push_back({Action::kUpdate, "Installing " + update.tag + "...", "", kGreen});
        break;
      case UpdateInfo::State::kRestart:
        rows.push_back({Action::kUpdate, "Restarting...", "", kGreen});
        break;
      case UpdateInfo::State::kError:
        rows.push_back({Action::kUpdate, "Update failed - try again", "", kRed});
        break;
      default:
        break;
    }
    main_mods_row_ = int(rows.size());
    rows.push_back({Action::kMods, "Mods"});
    rows.push_back({Action::kSettings, "Settings"});
    rows.push_back({Action::kPlayNoShow, "Play - don't show this screen again"});
    rows.push_back({Action::kQuit, "Quit"});

    if (update.state == UpdateInfo::State::kError && !update.error.empty()) {
      lines.push_back({"Update: " + update.error, kRed});
    } else if (update.state == UpdateInfo::State::kAvailable) {
      lines.push_back({update.tag + " is out (you have " + std::string(BURSTLIMIT_VERSION) +
                           "). Update keeps your settings, saves, mods and textures.",
                       kGreen});
    }

    LobbyInfo info;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      info = info_;
    }
    heading = "Online";
    switch (info.state) {
      case LobbyInfo::State::kOff:
        lines.push_back({"Online lobby off (online_lobby_url is empty in burstlimit.toml).", kMuted});
        break;
      case LobbyInfo::State::kLoading:
        lines.push_back({"Asking the online lobby...", kMuted});
        break;
      case LobbyInfo::State::kError:
        lines.push_back({"Couldn't reach the online lobby right now.", kMuted});
        break;
      case LobbyInfo::State::kOk: {
        std::snprintf(text, sizeof(text), "%d player%s online  -  %d on your version",
                      info.online, info.online == 1 ? "" : "s", info.online_version);
        lines.push_back({text, kWhite});
        std::snprintf(text, sizeof(text), "%d match%s playing, %d room%s open", info.matches,
                      info.matches == 1 ? "" : "es", info.rooms_open,
                      info.rooms_open == 1 ? "" : "s");
        lines.push_back({text, kMuted});
        if (info.rooms.empty()) {
          lines.push_back({"No open room on your version - host one: Versus > Online Battle > "
                           "Player Match > Create Match.",
                           kMuted});
        } else {
          lines.push_back({"Open rooms on your version (Versus > Online Battle > Player Match > "
                           "Custom Match):",
                           kMuted});
          for (size_t i = 0; i < info.rooms.size() && i < 6; ++i) {
            lines.push_back(
                {"   " + info.rooms[i].host + (info.rooms[i].locked ? "  (password)" : ""),
                 kYellow});
          }
          if (info.rooms.size() > 6) {
            lines.push_back({"   and " + std::to_string(info.rooms.size() - 6) + " more", kMuted});
          }
        }
        break;
      }
    }
  }

  void BuildMods(std::vector<Row>& rows, std::string& heading,
                 std::vector<std::pair<std::string, ImU32>>& lines) {
    for (size_t i = 0; i < mods_.size(); ++i) {
      rows.push_back({Action::kMod, mods_[i].first, mods_[i].second ? "ON" : "OFF",
                      mods_[i].second ? 0u : kMuted, int(i)});
    }
    rows.push_back({Action::kModsFolder, "Open the mods folder"});
    rows.push_back({Action::kBack, "Back"});
    heading = "Mods";
    if (mods_.empty()) {
      lines.push_back({"No mods yet: put each mod in its own folder inside the mods folder next "
                       "to burstlimit.exe, then come back here.",
                       kMuted});
    } else {
      lines.push_back({"A / Enter / click switches a mod on or off. When two mods replace the "
                       "same file, the one lower in the list wins. Online, both players need the "
                       "same mods.",
                       kMuted});
    }
    if (!mods_message_.empty()) {
      lines.push_back({mods_message_, kYellow});
    }
  }

  void Activate(const Row& row, const UpdateInfo& update) {
    const bool updating = update.state == UpdateInfo::State::kDownloading ||
                          update.state == UpdateInfo::State::kInstalling ||
                          update.state == UpdateInfo::State::kRestart;
    switch (row.action) {
      case Action::kPlay:
      case Action::kPlayNoShow: {
        if (updating) {
          BurstLimitPlayMenuSound(3);
          return;
        }
        BurstLimitPlayMenuSound(4);
        REXLOG_INFO("Start screen: {}", row.action == Action::kPlay ? "Play" : "Play (don't show again)");
        done_ = true;
        if (row.action == Action::kPlayNoShow) {
          rex::cvar::SetFlagByName("start_screen", "false");
          rex::cvar::SaveConfig(config_);
        }
        // Starting the game destroys this dialog: outside the frame.
        auto play = play_;
        defer_([play] { play(); });
        break;
      }
      case Action::kUpdate:
        if (update.state == UpdateInfo::State::kAvailable ||
            update.state == UpdateInfo::State::kError) {
          BurstLimitPlayMenuSound(1);
          StartUpdate();
        } else {
          BurstLimitPlayMenuSound(3);
        }
        break;
      case Action::kMods:
        if (updating) {
          BurstLimitPlayMenuSound(3);
          return;
        }
        BurstLimitPlayMenuSound(1);
        mods_ = BurstLimitModsList();
        mods_message_.clear();
        page_ = Page::kMods;
        selected_ = 0;
        REXLOG_INFO("Start screen: Mods ({} mod(s))", mods_.size());
        break;
      case Action::kSettings: {
        if (updating) {
          BurstLimitPlayMenuSound(3);
          return;
        }
        REXLOG_INFO("Start screen: Settings");
        auto settings = settings_;
        defer_([settings] { settings(); });
        break;
      }
      case Action::kQuit:
        ExitProcess(0);
      case Action::kMod: {
        BurstLimitPlayMenuSound(1);
        mods_[row.index].second = !mods_[row.index].second;
        std::vector<bool> on;
        for (const auto& [name, enabled] : mods_) {
          on.push_back(enabled);
        }
        mods_message_ = mods_[row.index].first + (mods_[row.index].second ? " on. " : " off. ") +
                        BurstLimitModsSetBeforeBoot(on);
        break;
      }
      case Action::kModsFolder: {
        BurstLimitPlayMenuSound(1);
        const fs::path folder = ExeDirectory() / "mods";
        std::error_code ec;
        fs::create_directories(folder, ec);
        ShellExecuteW(nullptr, L"open", folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        break;
      }
      case Action::kBack:
        BurstLimitPlayMenuSound(2);
        page_ = Page::kMain;
        selected_ = main_mods_row_;
        break;
    }
  }

  void ReadInput(ImGuiIO& io, Input& input, bool menu_open) {
    (void)io;
    // Any controller SDL knows (Xbox, PlayStation, Switch, ...).
    const rex::ui::QuickMenuDialog::PadState pads = rex::ui::ReadGamepadsBeforeGame();
    WORD buttons = pads.buttons;
    const SHORT stick_y = pads.thumb_ly;
    if (stick_y > 16000) buttons |= XINPUT_GAMEPAD_DPAD_UP;
    if (stick_y < -16000) buttons |= XINPUT_GAMEPAD_DPAD_DOWN;
    const WORD pressed = WORD(buttons & ~pad_buttons_);
    pad_buttons_ = buttons;
    std::vector<std::string> keys;
    {
      std::lock_guard<std::mutex> lock(g_keys_mutex);
      keys.swap(g_keys);
    }
    if (menu_open) {
      return;
    }
    int move = 0;
    if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) ++move;
    if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) --move;
    if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) ||
        ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false)) {
      input.activate = selected_;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) ||
        ImGui::IsKeyPressed(ImGuiKey_Backspace, false)) {
      input.back = true;
    }
    if (pad_seen_) {
      // (A button still held from before the screen showed doesn't count.)
      if (pressed & XINPUT_GAMEPAD_DPAD_DOWN) ++move;
      if (pressed & XINPUT_GAMEPAD_DPAD_UP) --move;
      if (pressed & (XINPUT_GAMEPAD_A | XINPUT_GAMEPAD_START)) input.activate = selected_;
      if (pressed & XINPUT_GAMEPAD_B) input.back = true;
    }
    pad_seen_ = true;
    for (const std::string& key : keys) {
      if (key == "down") ++move;
      if (key == "up") --move;
      if (key == "a") input.activate = selected_;
      if (key == "b") input.back = true;
    }
    move_ = move;
    if (g_play_requested.exchange(false)) {
      selected_ = 0;
      input.activate = 0;
    }
  }

  void Draw(ImGuiIO& io, ImDrawList* draw, const std::vector<Row>& rows,
            const std::string& heading, const std::vector<std::pair<std::string, ImU32>>& lines,
            bool menu_open, Input& input) {
    namespace text = rex::ui::overlay_text;
    const int count = int(rows.size());
    if (move_ != 0 && count > 0) {
      selected_ = ((selected_ + move_) % count + count) % count;
    }
    const float u = io.DisplaySize.y / 1080.0f;
    const float width = std::min(io.DisplaySize.x - 60.0f * u, 1100.0f * u);
    const float pad = 44.0f * u;
    const float wrap = width - pad * 2.0f;
    const float title_size = 52.0f * u, head_size = 34.0f * u, body_size = 28.0f * u;
    const float row_h = 60.0f * u;
    auto height_of = [&](float size, const std::string& value) {
      return text::Font(size)->CalcTextSizeA(size, FLT_MAX, wrap, value.c_str()).y;
    };
    // Long mod lists scroll.
    const int visible = std::min(count, 9);
    int first = 0;
    if (count > visible) {
      first = std::clamp(selected_ - visible / 2, 0, count - visible);
    }
    const std::string title = page_ == Page::kMain ? "Dragon Ball Z: Burst Limit Recompiled"
                                                   : "Mods";
    const std::string subtitle = page_ == Page::kMain ? std::string(BURSTLIMIT_VERSION) : "";
    float lines_h = 0.0f;
    for (const auto& [line, color] : lines) {
      lines_h += height_of(body_size, line) + 6.0f * u;
    }
    const bool show_heading = page_ == Page::kMain;
    const float height = pad + height_of(title_size, title) +
                         (subtitle.empty() ? 0.0f : height_of(body_size * 0.8f, subtitle)) +
                         28.0f * u + row_h * visible + 24.0f * u +
                         (show_heading ? height_of(head_size, heading) + 12.0f * u : 0.0f) +
                         lines_h + pad;
    const ImVec2 panel_min((io.DisplaySize.x - width) * 0.5f,
                           std::max(10.0f * u, (io.DisplaySize.y - height) * 0.5f));
    const ImVec2 panel_max(panel_min.x + width, panel_min.y + height);
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
    text::Draw(draw, title_size, ImVec2(x, y), kYellow, title, wrap);
    y += height_of(title_size, title);
    if (!subtitle.empty()) {
      text::Draw(draw, body_size * 0.8f, ImVec2(x, y), kMuted, subtitle);
      y += height_of(body_size * 0.8f, subtitle);
    }
    y += 12.0f * u;
    draw->AddLine(ImVec2(panel_min.x + 24.0f * u, y), ImVec2(panel_max.x - 24.0f * u, y),
                  IM_COL32(210, 196, 220, 150), 2.0f * u);
    y += 16.0f * u;

    for (int row = first; row < first + visible; ++row) {
      const ImVec2 min(panel_min.x + 10.0f * u, y), max(panel_max.x - 10.0f * u, y + row_h);
      if (!menu_open) {
        const bool hover = io.MousePos.x >= min.x && io.MousePos.x < max.x &&
                           io.MousePos.y >= min.y && io.MousePos.y < max.y;
        if (hover && (io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f) && selected_ != row) {
          selected_ = row;
          BurstLimitPlayMenuSound(0);
        }
        if (hover && io.MouseClicked[0]) {
          input.activate = row;
        }
      }
      if (selected_ == row) {
        draw->AddRectFilledMultiColor(min, max, IM_COL32(176, 140, 236, 235),
                                      IM_COL32(120, 80, 190, 0), IM_COL32(120, 80, 190, 0),
                                      IM_COL32(176, 140, 236, 235));
      }
      const Row& r = rows[row];
      const float size = r.big ? head_size : body_size;
      const ImU32 color = r.color ? r.color
                          : selected_ == row ? IM_COL32(255, 255, 255, 255)
                                             : IM_COL32(196, 186, 210, 255);
      const ImVec2 label_size = text::Measure(size, r.label.c_str());
      text::Draw(draw, size, ImVec2(x, min.y + (row_h - label_size.y) * 0.5f), color, r.label);
      if (!r.value.empty()) {
        const ImVec2 value_size = text::Measure(size, r.value.c_str());
        text::Draw(draw, size,
                   ImVec2(panel_max.x - pad - value_size.x, min.y + (row_h - value_size.y) * 0.5f),
                   r.value == "ON" ? kGreen : kMuted, r.value);
      }
      y += row_h;
    }
    if (count > visible) {
      char more[64];
      std::snprintf(more, sizeof(more), "%d / %d", selected_ + 1, count);
      const ImVec2 size = text::Measure(body_size * 0.8f, more);
      text::Draw(draw, body_size * 0.8f, ImVec2(panel_max.x - pad - size.x, y), kMuted, more);
    }
    y += 24.0f * u;

    if (show_heading) {
      text::Draw(draw, head_size, ImVec2(x, y), kYellow, heading);
      y += height_of(head_size, heading) + 12.0f * u;
    }
    for (const auto& [line, color] : lines) {
      text::Draw(draw, body_size, ImVec2(x, y), color, line, wrap);
      y += height_of(body_size, line) + 6.0f * u;
    }
  }

  // ---- the game's look (BurstLimitMenuArt): the title screen's sky, a
  // character and the logo, the main menu's box and description band. Laid
  // out in 1280x720 units in the window's 16:9 middle.

  struct Frame {
    ImVec2 origin;
    float u = 1.0f;
    ImVec2 at(float x, float y) const { return ImVec2(origin.x + x * u, origin.y + y * u); }
  };

  static Frame FrameOf(const ImGuiIO& io) {
    const float width = std::min(io.DisplaySize.x, io.DisplaySize.y * 16.0f / 9.0f);
    const float height = width * 9.0f / 16.0f;
    return {ImVec2((io.DisplaySize.x - width) * 0.5f, (io.DisplaySize.y - height) * 0.5f),
            height / 720.0f};
  }

  void DrawGameBackground(const ImGuiIO& io, ImDrawList* draw) {
    draw->AddRectFilled(ImVec2(0, 0), io.DisplaySize, IM_COL32(0, 0, 0, 255));
    const Frame f = FrameOf(io);
    auto image = [&](BurstLimitMenuArt::Picture picture, ImVec2 min, ImVec2 max, ImU32 tint) {
      if (rex::ui::ImmediateTexture* texture = art_->Image(picture)) {
        draw->AddImage(reinterpret_cast<ImTextureID>(texture), min, max, ImVec2(0, 0),
                       ImVec2(1, 1), tint);
      }
    };
    image(BurstLimitMenuArt::kBackground, f.at(0, 0), f.at(1280, 720), IM_COL32_WHITE);
    image(BurstLimitMenuArt::kAura, f.at(0, 0), f.at(1280, 720), IM_COL32(255, 255, 255, 190));
    // The character on the right, big like the main menu draws him: past the
    // top of the screen, his cut behind the band.
    if (rex::ui::ImmediateTexture* texture = art_->Image(BurstLimitMenuArt::kCharacter)) {
      const float tw = float(texture->width), th = float(texture->height);
      const float scale = 880.0f / th;
      const float w = tw * scale, h = th * scale;
      const float right = 1250.0f, bottom = 860.0f;
      draw->AddImage(reinterpret_cast<ImTextureID>(texture), f.at(right - w, bottom - h),
                     f.at(right, bottom));
    }
    // The logo, top right.
    if (rex::ui::ImmediateTexture* texture = art_->Image(BurstLimitMenuArt::kLogo)) {
      const float w = 430.0f, h = w * float(texture->height) / float(texture->width);
      draw->AddImage(reinterpret_cast<ImTextureID>(texture), f.at(1250.0f - w, 14.0f),
                     f.at(1250.0f, 14.0f + h));
    }
  }

  std::string HelpOf(const Row& row, const UpdateInfo& update) const {
    switch (row.action) {
      case Action::kPlay:
        return "Start the game.";
      case Action::kUpdate:
        if (update.state == UpdateInfo::State::kError) {
          return "The update failed: " + update.error + ". Try again, or download it from GitHub.";
        }
        if (update.state == UpdateInfo::State::kAvailable) {
          return update.tag + " is out (you have " + std::string(BURSTLIMIT_VERSION) +
                 "). Download it and restart - your settings, saves, mods and textures stay.";
        }
        return "Getting " + update.tag + ". The game restarts by itself when it's done.";
      case Action::kMods:
        return "Switch the mods in the mods folder on or off.";
      case Action::kSettings:
        return "Graphics, display, controls and online settings (F1 in the game).";
      case Action::kPlayNoShow:
        return "Start the game and skip this screen from now on (Settings > GAME > Start screen "
               "brings it back).";
      case Action::kQuit:
        return "Close the game.";
      case Action::kMod:
        return mods_[row.index].second ? "On: it loads with the game. Press to switch it off."
                                       : "Off. Press to switch it on.";
      case Action::kModsFolder:
        return "Put each mod in its own folder in here, then come back to this screen.";
      case Action::kBack:
        return "Back to the start screen.";
    }
    return {};
  }

  std::string OnlineLine() {
    LobbyInfo info;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      info = info_;
    }
    switch (info.state) {
      case LobbyInfo::State::kOff:
        return "Online lobby off";
      case LobbyInfo::State::kLoading:
        return "Online: asking the lobby...";
      case LobbyInfo::State::kError:
        return "Online: the lobby can't be reached right now";
      case LobbyInfo::State::kOk:
        break;
    }
    char text[160];
    std::snprintf(text, sizeof(text), "Online: %d player%s (%d on your version), %d room%s open",
                  info.online, info.online == 1 ? "" : "s", info.online_version, info.rooms_open,
                  info.rooms_open == 1 ? "" : "s");
    std::string line = text;
    for (size_t i = 0; i < info.rooms.size() && i < 3; ++i) {
      line += (i ? ", " : ": ") + info.rooms[i].host;
    }
    return line;
  }

  void DrawGame(ImGuiIO& io, ImDrawList* draw, const std::vector<Row>& rows,
                const UpdateInfo& update, const std::vector<std::pair<std::string, ImU32>>& lines,
                Input& input) {
    const Frame f = FrameOf(io);
    const float u = f.u;
    constexpr ImU32 kText = IM_COL32(255, 255, 255, 255);
    constexpr ImU32 kDim = IM_COL32(150, 136, 172, 255);
    constexpr ImU32 kRim = IM_COL32(194, 180, 142, 255);
    constexpr ImU32 kBodyTop = IM_COL32(16, 6, 10, 245);
    constexpr ImU32 kBodyBottom = IM_COL32(64, 10, 98, 245);
    constexpr ImU32 kHighlightLeft = IM_COL32(150, 120, 214, 255);
    constexpr ImU32 kHighlightMid = IM_COL32(89, 47, 131, 255);
    constexpr ImU32 kHighlightRight = IM_COL32(54, 9, 85, 120);
    constexpr ImU32 kBandTop = IM_COL32(10, 3, 12, 235);
    constexpr ImU32 kBandBottom = IM_COL32(86, 24, 98, 235);
    constexpr ImU32 kArrow = IM_COL32(255, 222, 0, 255);
    constexpr ImU32 kGreenText = IM_COL32(130, 255, 150, 255);
    constexpr ImU32 kRedText = IM_COL32(255, 130, 120, 255);

    const int count = int(rows.size());
    if (move_ != 0 && count > 0) {
      selected_ = ((selected_ + move_) % count + count) % count;
    }

    // The box: the main menu's (dark top to purple, beige rim), rows 42 apart.
    const float x0 = 86.0f, x1 = 616.0f, y0 = 60.0f;
    const float first_row = y0 + 30.0f, pitch = 42.0f, bar = 38.0f, item = 30.0f;
    const int visible = std::min(count, 9);
    int first = 0;
    if (count > visible) {
      first = std::clamp(selected_ - visible / 2, 0, count - visible);
    }
    const float y1 = first_row + float(visible) * pitch + 22.0f;
    const float round_scale = 0.62f * u;
    art_->NineSlice(draw, f.at(x0, y0), f.at(x1, y1), ImVec2(45, 1), ImVec2(88, 44), 21.5f,
                    round_scale, kBodyTop, kBodyBottom);
    for (int i = 0; i < visible; ++i) {
      const int index = first + i;
      const Row& row = rows[index];
      const float top = first_row + float(i) * pitch;
      const ImVec2 min = f.at(x0 + 3.0f, top), max = f.at(x1 - 3.0f, top + bar);
      const bool hover = io.MousePos.x >= min.x && io.MousePos.x < max.x &&
                         io.MousePos.y >= min.y && io.MousePos.y < max.y;
      if (hover && (io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f) && selected_ != index) {
        selected_ = index;
        BurstLimitPlayMenuSound(0);
      }
      if (hover && io.MouseClicked[0]) {
        input.activate = index;
      }
      if (index == selected_) {
        const float mid = x0 + (x1 - x0) * 0.45f;
        draw->AddRectFilledMultiColor(f.at(x0 + 3.0f, top), f.at(mid, top + bar), kHighlightLeft,
                                      kHighlightMid, kHighlightMid, kHighlightLeft);
        draw->AddRectFilledMultiColor(f.at(mid, top), f.at(x1 - 3.0f, top + bar), kHighlightMid,
                                      kHighlightRight, kHighlightRight, kHighlightMid);
      }
      ImU32 color = kText;
      if (row.color == kGreen) color = kGreenText;
      if (row.color == kRed) color = kRedText;
      if (row.color == kMuted) color = kDim;
      const float text_top = top + (bar - item) * 0.5f + 1.0f;
      art_->Text(draw, f.at(x0 + 24.0f, text_top), item * u, row.label, color);
      if (!row.value.empty()) {
        const float w = art_->Text(nullptr, ImVec2(), item * u, row.value, 0) / u;
        art_->Text(draw, f.at(x1 - 28.0f - w, text_top), item * u, row.value,
                   row.value == "ON" ? kGreenText : kDim);
      }
    }
    if (count > visible) {
      if (first > 0) {
        art_->Icon(draw, BurstLimitMenuArt::kArrowUp, f.at(x1 - 36.0f, y0 + 6.0f), 20.0f * u,
                   kArrow);
      }
      if (first + visible < count) {
        art_->Icon(draw, BurstLimitMenuArt::kArrowUp, f.at(x1 - 36.0f, y1 - 24.0f), 20.0f * u,
                   kArrow, 2);
      }
    }
    art_->NineSlice(draw, f.at(x0 - 1.0f, y0 - 1.0f), f.at(x1 + 1.0f, y1 + 1.0f), ImVec2(1, 1),
                    ImVec2(44, 44), 21.5f, round_scale, kRim, kRim);

    // The version, small, top left; the page's title over the box.
    art_->Text(draw, f.at(14.0f, 690.0f), 20.0f * u, BURSTLIMIT_VERSION, IM_COL32(255, 255, 255, 150));
    if (page_ == Page::kMods) {
      art_->Text(draw, f.at(x0 + 4.0f, y0 - 46.0f), 40.0f * u, "Mods", kText);
    }

    // The band: a line, then the description of the selected row and, on the
    // main page, who's online; the prompts at the bottom right.
    const float band_top = 516.0f, band_bottom = 680.0f;
    draw->AddRectFilled(f.at(0, band_top - 6.0f), f.at(1280, band_top - 3.0f),
                        IM_COL32(255, 255, 255, 200));
    draw->AddRectFilledMultiColor(f.at(0, band_top), f.at(1280, band_bottom), kBandTop, kBandTop,
                                  kBandBottom, kBandBottom);
    std::string help = selected_ < count ? HelpOf(rows[selected_], update) : "";
    ImU32 help_color = kText;
    if (page_ == Page::kMods && !mods_message_.empty()) {
      help = mods_message_ + " " + help;
    }
    if (selected_ < count && rows[selected_].action == Action::kUpdate &&
        update.state == UpdateInfo::State::kError) {
      help_color = kRedText;
    }
    const float help_size = 30.0f;
    const std::vector<std::string> help_lines = art_->Wrap(help_size * u, help, 1060.0f * u);
    for (size_t i = 0; i < help_lines.size() && i < 2; ++i) {
      art_->Text(draw, f.at(104.0f, 534.0f + float(i) * 34.0f), help_size * u, help_lines[i],
                 help_color);
    }
    if (page_ == Page::kMain) {
      const std::string online = OnlineLine();
      art_->Text(draw, f.at(104.0f, 606.0f), 24.0f * u, online, IM_COL32(255, 222, 0, 255));
    } else if (!lines.empty()) {
      (void)lines;
    }

    // Prompts: arrows Select, A Confirm, B Back.
    const float prompt = 28.0f, button = 26.0f, arrow = 20.0f;
    const char* back = page_ == Page::kMods ? "Back" : "Quit";
    auto width_of = [&](const char* text) {
      return art_->Text(nullptr, ImVec2(), prompt * u, text, 0) / u;
    };
    float total = 2.0f * arrow + 2.0f + width_of("Select") + 12.0f + button + 2.0f +
                  width_of("Confirm") + 12.0f;
    if (page_ == Page::kMods) {
      total += button + 2.0f + width_of(back);
    }
    float x = 1200.0f - total;
    const float mid = 654.0f;
    art_->Icon(draw, BurstLimitMenuArt::kArrowUp, f.at(x, mid - arrow * 0.5f), arrow * u, kArrow);
    art_->Icon(draw, BurstLimitMenuArt::kArrowUp, f.at(x + arrow, mid - arrow * 0.5f), arrow * u,
               kArrow, 2);
    x += 2.0f * arrow + 2.0f;
    x += art_->Text(draw, f.at(x, mid - prompt * 0.5f), prompt * u, "Select", kText) / u + 12.0f;
    art_->Icon(draw, BurstLimitMenuArt::kButtonA, f.at(x, mid - button * 0.5f), button * u, kText);
    x += button + 2.0f;
    x += art_->Text(draw, f.at(x, mid - prompt * 0.5f), prompt * u, "Confirm", kText) / u + 12.0f;
    if (page_ == Page::kMods) {
      art_->Icon(draw, BurstLimitMenuArt::kButtonB, f.at(x, mid - button * 0.5f), button * u,
                 kText);
      x += button + 2.0f;
      art_->Text(draw, f.at(x, mid - prompt * 0.5f), prompt * u, back, kText);
    }
  }

  void Poll() {
    const std::wstring url = Widen(url_);
    for (;;) {
      const std::string body = HttpGetText(url, 5000);
      LobbyInfo info;
      try {
        const json j = json::parse(body);
        info.state = LobbyInfo::State::kOk;
        info.online = j.value("online", 0);
        info.online_version = j.value("online_version", 0);
        info.rooms_open = j.value("rooms_open", 0);
        info.matches = j.value("matches", 0);
        for (const json& room : j.value("rooms", json::array())) {
          if (room.is_object()) {
            info.rooms.push_back({room.value("host", std::string()), room.value("locked", false)});
          }
        }
      } catch (...) {
        info.state = LobbyInfo::State::kError;
      }
      std::unique_lock<std::mutex> lock(mutex_);
      if (info.state == LobbyInfo::State::kError) {
        REXLOG_WARN("Start screen: the lobby's rooms couldn't be read ({})", url_);
      }
      info_ = std::move(info);
      if (wake_.wait_for(lock, std::chrono::seconds(10), [this] { return stop_; })) {
        return;
      }
    }
  }

  // The latest GitHub release: newer than this build, with a zip for this
  // platform (the -linux one under Wine / Proton)?
  void CheckForUpdate(const std::string& url) {
    UpdateInfo found;
    found.state = UpdateInfo::State::kNone;
    try {
      const json release = json::parse(HttpGetText(Widen(url), 8000));
      found.tag = release.value("tag_name", std::string());
      std::array<int, 3> latest = {};
      if (ParseVersion(found.tag, latest) && latest > own_version_ &&
          !release.value("draft", false)) {
        const bool want_linux = UnderWine();
        for (const json& asset : release.value("assets", json::array())) {
          const std::string name = asset.value("name", std::string());
          const std::string lower = Lower(name);
          if (lower.size() < 4 || lower.compare(lower.size() - 4, 4, ".zip") != 0 ||
              (lower.find("linux") != std::string::npos) != want_linux) {
            continue;
          }
          found.url = asset.value("browser_download_url", std::string());
          found.size = asset.value("size", uint64_t(0));
          found.state = UpdateInfo::State::kAvailable;
          break;
        }
      }
      REXLOG_INFO("Start screen: latest release {} ({}){}", found.tag, BURSTLIMIT_VERSION,
                  found.state == UpdateInfo::State::kAvailable ? " - update available" : "");
    } catch (...) {
      REXLOG_WARN("Start screen: couldn't check for updates ({})", url);
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (update_.state == UpdateInfo::State::kChecking) {
      update_ = found;
    }
  }

  void StartUpdate() {
    std::string url;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (update_.url.empty()) {
        return;
      }
      update_.state = UpdateInfo::State::kDownloading;
      update_.error.clear();
      url = update_.url;
    }
    REXLOG_INFO("Update: downloading {}", url);
    download_done_ = 0;
    download_total_ = 0;
    if (update_thread_.joinable()) {
      update_thread_.join();
    }
    update_thread_ = std::thread([this, url] { DownloadAndInstall(url); });
  }

  void DownloadAndInstall(const std::string& url) {
    std::vector<uint8_t> zip;
    const bool ok =
        HttpGet(Widen(url), 30000, [this, &zip](const char* data, size_t size, uint64_t total) {
          zip.insert(zip.end(), data, data + size);
          download_done_ = zip.size();
          download_total_ = total;
          std::lock_guard<std::mutex> lock(mutex_);
          return !stop_ && zip.size() < 1024ull * 1024 * 1024;
        });
    std::string error;
    if (!ok || zip.empty()) {
      error = "the download didn't finish - check your connection";
    } else {
      {
        std::lock_guard<std::mutex> lock(mutex_);
        update_.state = UpdateInfo::State::kInstalling;
      }
      REXLOG_INFO("Update: {} bytes downloaded, installing", zip.size());
      error = InstallUpdate(zip, ExeDirectory());
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (!error.empty()) {
      REXLOG_ERROR("Update failed: {}", error);
      update_.state = UpdateInfo::State::kError;
      update_.error = error;
      return;
    }
    REXLOG_INFO("Update: installed {}, restarting", update_.tag);
    update_.state = UpdateInfo::State::kRestart;
    defer_([] { Restart(); });
  }

  rex::ui::ImmediateDrawer* immediate_drawer_;
  BurstLimitMenuArt* art_ = nullptr;
  std::function<void()> play_;
  std::function<void(std::function<void()>)> defer_;
  std::function<void()> settings_;
  fs::path config_;
  std::string url_;
  std::thread worker_;
  std::thread update_thread_;
  std::mutex mutex_;
  std::condition_variable wake_;
  bool stop_ = false;
  LobbyInfo info_;
  UpdateInfo update_;
  std::array<int, 3> own_version_ = {};
  std::atomic<uint64_t> download_done_{0};
  std::atomic<uint64_t> download_total_{0};
  Page page_ = Page::kMain;
  std::vector<std::pair<std::string, bool>> mods_;
  std::string mods_message_;
  int main_mods_row_ = 1;
  int selected_ = 0;
  int move_ = 0;
  WORD pad_buttons_ = 0;
  bool pad_seen_ = false;
  bool done_ = false;
};

}  // namespace

REXCVAR_DEFINE_COMMAND(start_screen_play, PlayCommand, "Debug",
                       "Start screen: Play (for tests through the command pipe)");
REXCVAR_DEFINE_COMMAND_ARGS(start_screen_key, KeyCommand, "Debug",
                            "Start screen: up / down / a / b (for tests through the command pipe)");

bool BurstLimitStartScreenEnabled() {
  return REXCVAR_GET(start_screen);
}

std::unique_ptr<rex::ui::ImGuiDialog> BurstLimitCreateStartScreen(
    rex::ui::ImGuiDrawer* drawer, rex::ui::ImmediateDrawer* immediate_drawer,
    const std::filesystem::path& game_data_root, std::function<void()> play,
    std::function<void(std::function<void()>)> defer, std::function<void()> settings,
    const std::filesystem::path& config) {
  return std::make_unique<StartScreen>(drawer, immediate_drawer, game_data_root, std::move(play),
                                       std::move(defer), std::move(settings), config);
}

void BurstLimitUpdateCleanup(const std::filesystem::path& exe_directory) {
  std::error_code ec;
  fs::remove_all(exe_directory / "update_staging", ec);
  int removed = 0;
  for (const fs::path& folder : {exe_directory, exe_directory / "licenses"}) {
    for (auto it = fs::directory_iterator(folder, ec); !ec && it != fs::directory_iterator();
         it.increment(ec)) {
      if (it->path().extension() == ".old" && it->is_regular_file()) {
        std::error_code remove_ec;
        removed += fs::remove(it->path(), remove_ec) ? 1 : 0;
      }
    }
    ec.clear();
  }
  if (removed) {
    REXLOG_INFO("Update: removed {} file(s) the last update replaced", removed);
  }
}
