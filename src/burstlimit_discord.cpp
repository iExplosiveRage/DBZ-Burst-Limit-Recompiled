// Discord Rich Presence: "Playing Dragon Ball Z: Burst Limit Recompiled" with
// what the player is doing - in the menus, a Z Chronicles battle, Training,
// Versus, or online (in a session's lobby or fighting) - and the time since
// the game started.
//
// Talks to the Discord app on this PC over its local IPC pipe
// (\\.\pipe\discord-ipc-N): a handshake with the application id, then
// SET_ACTIVITY whenever the text changes. No Discord SDK or DLL; without
// Discord running it just tries again now and then.
//
// What's going on is read from the game every 2 seconds: the battle object
// pointer at 0x841B5130 is set while a fight runs (burstlimit_camera.cpp), the
// battle mode is in the match request (burstlimit_forms.cpp) and an online
// session is open while the players are in a session's lobby or fighting.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <string>
#include <thread>
#include <vector>

#include <windows.h>

#include <nlohmann/json.hpp>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/memory.h>
#include <rex/memory/utils.h>
#include <rex/net/session.h>
#include <rex/runtime.h>

REXCVAR_DEFINE_BOOL(discord_presence, true, "Patches",
                    "Show what you're playing on your Discord profile (Rich Presence)");

// burstlimit_forms.cpp: the battle mode of the last match loaded (0 Z
// Chronicles, 2 Training, other Versus), -1 before the first.
int BurstLimitLastBattleMode();

namespace {

using json = nlohmann::json;

constexpr const char* kApplicationId = "1522060011671519383";
constexpr uint32_t kBattlePointer = 0x841B5130;

// A reply's "evt" (null in answers to commands).
std::string Event(const json& reply) {
  const auto it = reply.find("evt");
  return it != reply.end() && it->is_string() ? it->get<std::string>() : std::string();
}

class DiscordPipe {
 public:
  ~DiscordPipe() { Close(); }

  bool connected() const { return pipe_ != INVALID_HANDLE_VALUE; }

  bool Connect() {
    for (int i = 0; i < 10 && !connected(); ++i) {
      const std::wstring name = L"\\\\.\\pipe\\discord-ipc-" + std::to_wstring(i);
      pipe_ = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                          0, nullptr);
    }
    if (!connected()) {
      return false;
    }
    json reply;
    if (!Send(0, json{{"v", 1}, {"client_id", kApplicationId}}) || !Receive(reply) ||
        Event(reply) != "READY") {
      REXLOG_WARN("Discord: the handshake failed ({})", reply.dump());
      Close();
      return false;
    }
    REXLOG_INFO("Discord: connected");
    return true;
  }

  bool SetActivity(const json& activity) {
    json reply;
    if (!Send(1, json{{"cmd", "SET_ACTIVITY"},
                      {"args", {{"pid", GetCurrentProcessId()}, {"activity", activity}}},
                      {"nonce", std::to_string(++nonce_)}}) ||
        !Receive(reply)) {
      Close();
      return false;
    }
    if (Event(reply) == "ERROR") {
      REXLOG_WARN("Discord: SET_ACTIVITY refused: {}", reply.dump());
    }
    return true;
  }

  void Close() {
    if (connected()) {
      CloseHandle(pipe_);
      pipe_ = INVALID_HANDLE_VALUE;
    }
  }

 private:
  bool Send(uint32_t opcode, const json& body) {
    const std::string text = body.dump();
    std::vector<uint8_t> frame(8 + text.size());
    const uint32_t length = uint32_t(text.size());
    std::memcpy(frame.data(), &opcode, 4);
    std::memcpy(frame.data() + 4, &length, 4);
    std::memcpy(frame.data() + 8, text.data(), text.size());
    DWORD written = 0;
    return WriteFile(pipe_, frame.data(), DWORD(frame.size()), &written, nullptr) &&
           written == frame.size();
  }

  bool ReadExact(void* data, size_t size) {
    auto* out = static_cast<uint8_t*>(data);
    while (size) {
      DWORD read = 0;
      if (!ReadFile(pipe_, out, DWORD(size), &read, nullptr) || read == 0) {
        return false;
      }
      out += read;
      size -= read;
    }
    return true;
  }

  bool Receive(json& body) {
    uint32_t header[2] = {};
    if (!ReadExact(header, sizeof(header)) || header[1] > 64 * 1024) {
      return false;
    }
    std::string text(header[1], '\0');
    if (!ReadExact(text.data(), text.size())) {
      return false;
    }
    if (header[0] == 2) {  // close
      REXLOG_WARN("Discord: closed the connection: {}", text);
      return false;
    }
    body = json::parse(text, nullptr, false);
    return !body.is_discarded();
  }

  HANDLE pipe_ = INVALID_HANDLE_VALUE;
  uint64_t nonce_ = 0;
};

// What the player is doing: the presence's two lines.
std::pair<std::string, std::string> CurrentActivity() {
  auto* runtime = rex::Runtime::instance();
  rex::memory::Memory* memory = runtime ? runtime->memory() : nullptr;
  if (!memory) {
    return {"Starting up", ""};
  }
  const bool fighting =
      rex::memory::load_and_swap<uint32_t>(memory->TranslateVirtual<uint8_t*>(kBattlePointer)) != 0;
  if (rex::net::IsGameSessionOpen()) {
    return {"Online", fighting ? "In a match" : "In a lobby"};
  }
  if (!fighting) {
    return {"In the menus", ""};
  }
  switch (BurstLimitLastBattleMode()) {
    case 0:
      return {"Z Chronicles", "Story battle"};
    case 2:
      return {"Training", ""};
    default:
      return {"Versus", "Offline battle"};
  }
}

void PresenceThread() {
  const int64_t started = int64_t(std::time(nullptr));
  DiscordPipe discord;
  std::pair<std::string, std::string> shown;
  auto next_connect = std::chrono::steady_clock::now();
  for (;;) {
    std::this_thread::sleep_for(std::chrono::seconds(2));
    if (!REXCVAR_GET(discord_presence)) {
      if (discord.connected()) {
        // Closing the connection clears the presence.
        discord.Close();
        shown = {};
      }
      continue;
    }
    if (!discord.connected()) {
      if (std::chrono::steady_clock::now() < next_connect) {
        continue;
      }
      next_connect = std::chrono::steady_clock::now() + std::chrono::seconds(20);
      if (!discord.Connect()) {
        continue;
      }
      shown = {};
    }
    const auto activity = CurrentActivity();
    if (activity == shown) {
      continue;
    }
    json body = {{"details", activity.first},
                 {"timestamps", {{"start", started}}},
                 {"assets",
                  {{"large_image", "burstlimit"},
                   {"large_text", "Dragon Ball Z: Burst Limit Recompiled"}}}};
    if (!activity.second.empty()) {
      body["state"] = activity.second;
    }
    if (discord.SetActivity(body)) {
      REXLOG_INFO("Discord: {}{}{}", activity.first, activity.second.empty() ? "" : " - ",
                  activity.second);
      shown = activity;
    }
  }
}

// Nothing from Discord may end the game: an exception starts over.
void SafePresenceThread() {
  for (;;) {
    try {
      PresenceThread();
    } catch (const std::exception& e) {
      REXLOG_WARN("Discord: {} - trying again", e.what());
    }
    std::this_thread::sleep_for(std::chrono::seconds(20));
  }
}

}  // namespace

void BurstLimitDiscordSetup() {
  static std::atomic<bool> started{false};
  if (started.exchange(true)) {
    return;
  }
  std::thread(SafePresenceThread).detach();
}
