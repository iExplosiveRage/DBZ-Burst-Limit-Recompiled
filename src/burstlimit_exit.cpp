// "Exit Game" (GitHub issue #21): close the game without Alt+F4.
//
// - The main menu has an "Exit Game" entry after Mods (burstlimit_mods.cpp
//   adds it to the game's own list; A on it asks "Exit the game?" in the Mods
//   panel's look, No / Yes).
// - The settings menu (F1) has "Exit game" in GAME: A, then A again within
//   10 s.
// Both close the window the way its close button / Alt+F4 does (the app's
// OnClosing path: the title is terminated, then the process exits). The save
// is whatever the game last wrote - the game writes it itself (after fights,
// options...) and never keeps unsaved changes waiting for a quit.

#include <chrono>
#include <functional>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <rex/logging.h>
#include <rex/ui/overlay/quick_menu.h>

namespace {

std::mutex g_mutex;
std::function<void()> g_handler;
std::chrono::steady_clock::time_point g_armed_until{};
// The GAME section's status before this item's own (Unlock everything's).
std::function<rex::ui::QuickMenuStatus()> g_previous_status;

}  // namespace

// The app (burstlimit_app.h): how to close the window, from any thread.
void BurstLimitSetExitHandler(std::function<void()> handler) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_handler = std::move(handler);
}

// Closes the game (any thread; the close itself runs on the UI thread).
void BurstLimitExitGame(const char* from) {
  std::function<void()> handler;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    handler = g_handler;
  }
  REXLOG_INFO("Exit game: requested from {}", from);
  if (handler) {
    handler();
  }
}

// GAME tab "Exit game", with a confirm step (A, then A again).
void BurstLimitExitMenu(rex::ui::QuickMenuSection& section) {
  using Item = rex::ui::QuickMenuItem;
  g_previous_status = section.status;
  section.status = []() -> rex::ui::QuickMenuStatus {
    if (std::chrono::steady_clock::now() < g_armed_until) {
      return {"Press A again on Exit game to close the game.", false};
    }
    return g_previous_status ? g_previous_status() : rex::ui::QuickMenuStatus{};
  };
  Item& item = section.items.emplace_back();
  item.kind = Item::Kind::kAction;
  item.label = "Exit game";
  item.help =
      "Closes the game (like closing its window). What the game has saved is kept: it saves on "
      "its own, after fights and option changes.";
  // One entry: "Exit", then "Press A again" while armed.
  item.list = []() -> std::vector<std::pair<std::string, std::string>> {
    if (std::chrono::steady_clock::now() < g_armed_until) {
      return {{"confirm", "Press A again"}};
    }
    return {{"exit", "Exit"}};
  };
  item.action = [](const std::string&) {
    const auto now = std::chrono::steady_clock::now();
    if (now >= g_armed_until) {
      g_armed_until = now + std::chrono::seconds(10);
      REXLOG_INFO("Exit game: settings menu, waiting for the second A");
      return;
    }
    g_armed_until = {};
    BurstLimitExitGame("the settings menu");
  };
}
