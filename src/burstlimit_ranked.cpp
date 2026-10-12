// Ranked Match and the online leaderboard (the lobby side: C:/rex/burstlimit_online_service
// RANKED.md; the SDK side: rex/net/online_stats.h; design C:/rex/_online_tests/RANKED_DESIGN.md).
//
// The game itself keeps Xbox LIVE's Ranked Match: Power Level and Grade come from the Ranked
// leaderboard (view 145) when a ranked session is joined, and the Versus results screen changes
// them with the game's own formula. Here:
// - the results screen of every online battle reports the outcome (winner, rounds, characters)
//   to the lobby, which counts it once both consoles agree and applies the same Power Level
//   formula (sub_8224C6C0, C:/rex/_online_tests/ranked/RANKED_RE.md 1.4);
// - XUserReadStats needs to know whose record the game reads (everyone shares the profile XUID
//   in this recomp): this player's own record or the opponent's;
// - arbitration (Ranked only) drops peers missing from the registration results, which the
//   recomp can't fill: every peer counts as registered;
// - an overlay adds what the game's screens have no room for: battles, wins, losses and the
//   favourite character next to each leaderboard row, gamer cards, and a rankings page in the
//   settings menu (ONLINE).

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <fmt/format.h>
#include <imgui.h>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/memory.h>
#include <rex/memory/utils.h>
#include <rex/net/online.h>
#include <rex/net/online_stats.h>
#include <rex/net/session.h>
#include <rex/ppc/context.h>
#include <rex/runtime.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/overlay/overlay_text.h>
#include <rex/ui/overlay/quick_menu.h>

#include "burstlimit_menuart.h"

namespace stats = rex::net::online::stats;

REXCVAR_DEFINE_BOOL(ranked_overlay, true, "Online",
                    "Show battles, wins, losses and the favourite character next to the online "
                    "leaderboard, and gamer cards");

namespace {

// Versus results object V (sub_8224C6C0): +8 rounds played (s8), +9+i player 1's round codes,
// +18+i player 2's (1 / 5 = won by KO, 3 = won at time up, 0 = lost), +92 winner (s8: 1 = player 1,
// 0 = player 2, -1 = draw), +95 mode (0 Ranked, 1 Player Match).
constexpr uint32_t kResultRounds = 8;
constexpr uint32_t kResultCodesP1 = 9;
constexpr uint32_t kResultCodesP2 = 18;
constexpr uint32_t kResultWinner = 92;
constexpr uint32_t kResultMode = 95;
// The match request ([0x841B5138]): 0x50 bytes per player, +0 u16 character.
constexpr uint32_t kRequestPointer = 0x841B5138;
constexpr uint32_t kRequestStride = 0x50;
// The battle ([0x841B5130] != 0 while one is loaded).
constexpr uint32_t kBattlePointer = 0x841B5130;
// The net manager N: its own player record at +824, the record's location at +184.
constexpr uint32_t kNetManagerPointer = 0x84255714;
constexpr uint32_t kOwnRecord = 824;
constexpr uint32_t kRecordLocation = 184;
// The match input manager M ([0x841B5EA4]) and its driver state at +1632 (3 = battle).
constexpr uint32_t kInputManagerPointer = 0x841B5EA4;
constexpr uint32_t kDriverState = 1632;
constexpr uint32_t kDriverStateBattle = 3;
// The leaderboard screen's object (sub_8225F1A0).
constexpr uint32_t kLeaderboardPointer = 0x8424D788;

rex::memory::Memory* GuestMemory() {
  auto* runtime = rex::Runtime::instance();
  return runtime ? runtime->memory() : nullptr;
}

uint32_t LoadU32(rex::memory::Memory* memory, uint32_t address) {
  return rex::memory::load_and_swap<uint32_t>(memory->TranslateVirtual<uint8_t*>(address));
}

int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

bool OnlineLobbySession() {
  return rex::net::IsGameSessionOpen() && rex::net::online::IsLobbyMode();
}

std::atomic<int64_t> g_last_report_ms{0};

}  // namespace

// At startup: where the online identity is kept (the user data folder).
void BurstLimitRankedSetup(const std::filesystem::path& user_data_root) {
  if (!user_data_root.empty()) {
    stats::SetDataFolder(user_data_root);
  }
}

// Mid-asm hook at 0x8224CAE4 in the Versus results screen sub_8224C6C0, right after the winner is
// decided (r21 / r22 = rounds won by player 1 / 2, r23 = the results object), before the Power
// Level changes. Online only: report the battle to the lobby.
void BurstLimitRankedResults(PPCRegister& r21, PPCRegister& r22, PPCRegister& r23) {
  auto* memory = GuestMemory();
  if (!memory || !r23.u32 || !OnlineLobbySession()) {
    return;
  }
  // The screen's set-up runs once per battle; never report twice within a few seconds.
  const int64_t now = NowMs();
  if (now - g_last_report_ms.load() < 5000) {
    return;
  }
  g_last_report_ms.store(now);
  const uint8_t* v = memory->TranslateVirtual<const uint8_t*>(r23.u32);
  stats::BattleReport report;
  const int played = std::clamp<int>(int8_t(v[kResultRounds]), 0, 9);
  report.played = played;
  report.won[0] = std::clamp<int>(int(r21.u32 & 0xFF), 0, 9);
  report.won[1] = std::clamp<int>(int(r22.u32 & 0xFF), 0, 9);
  for (int i = 0; i < played; ++i) {
    report.ko5[0] += v[kResultCodesP1 + i] == 5 ? 1 : 0;
    report.ko5[1] += v[kResultCodesP2 + i] == 5 ? 1 : 0;
  }
  const int winner = int8_t(v[kResultWinner]);
  // Player 1 is the host (side 0) on both consoles.
  report.winner = winner > 0 ? 0 : winner == 0 ? 1 : 2;
  const uint32_t request = LoadU32(memory, kRequestPointer);
  if (request) {
    for (int side = 0; side < 2; ++side) {
      const uint16_t character = rex::memory::load_and_swap<uint16_t>(
          memory->TranslateVirtual<uint8_t*>(request + side * kRequestStride));
      report.chars[side] = character < 21 ? int(character) : -1;
    }
  }
  const uint32_t net = LoadU32(memory, kNetManagerPointer);
  if (net) {
    const uint32_t location = LoadU32(memory, net + kOwnRecord + kRecordLocation);
    report.country = location <= 7 ? int(location) : -1;
  }
  REXLOG_INFO("[Ranked] results screen: mode {} winner {} rounds {} codes {}{}{}{}{} / {}{}{}{}{}",
              v[kResultMode], winner, played, v[9], v[10], v[11], v[12], v[13], v[18], v[19],
              v[20], v[21], v[22]);
  if (report.chars[0] < 0 || report.chars[1] < 0) {
    REXLOG_WARN("[Ranked] results screen: characters unknown ({}, {}), not reported",
                report.chars[0], report.chars[1]);
    stats::SetBattleInProgress(false);
    return;
  }
  stats::ReportBattle(report);
}

// Mid-asm hook at 0x82295E0C in sub_82295DF8 (reads a player record's Xbox LIVE stats when a
// session member joins): r31 = the net manager, r28 = the record. Its own record is at +824.
void BurstLimitRankedReadTarget(PPCRegister& r28, PPCRegister& r31) {
  stats::SetReadTarget(r28.u32 == r31.u32 + kOwnRecord ? 0 : 1);
}

// Mid-asm hook at 0x822983EC in sub_822982B8 (the host after arbitration registration): r30 = the
// peer record vector (+4 first, +8 end, 256 bytes each). Peers whose machine id isn't in the
// registration results (+176 = 0) are dropped next: the recomp's results list nobody, so every
// peer is marked registered.
void BurstLimitRankedArbitration(PPCRegister& r30) {
  auto* memory = GuestMemory();
  if (!memory || !r30.u32) {
    return;
  }
  const uint32_t begin = LoadU32(memory, r30.u32 + 4);
  const uint32_t end = LoadU32(memory, r30.u32 + 8);
  int count = 0;
  for (uint32_t record = begin; record && record < end && count < 8; record += 256, ++count) {
    rex::memory::store_and_swap<uint32_t>(memory->TranslateVirtual<uint8_t*>(record + 176), 1);
  }
  REXLOG_INFO("[Ranked] arbitration: {} peer(s) marked registered", count);
}

namespace {

// --- overlay ---------------------------------------------------------------------------------

constexpr ImU32 kWhite = IM_COL32(255, 255, 255, 255);
constexpr ImU32 kYellow = IM_COL32(255, 222, 0, 255);
constexpr ImU32 kDim = IM_COL32(200, 200, 220, 255);

std::string Thousands(int value) {
  std::string digits = std::to_string(std::abs(value));
  std::string out;
  for (size_t i = 0; i < digits.size(); ++i) {
    if (i && (digits.size() - i) % 3 == 0) {
      out += ',';
    }
    out += digits[i];
  }
  return value < 0 ? "-" + out : out;
}

std::string Battles(int count) {
  return fmt::format("{} battle{}", count, count == 1 ? "" : "s");
}

std::string RecordText(const stats::PlayerStats& p) {
  return p.draws ? fmt::format("{}  {} W / {} L / {} D", Battles(p.games), p.wins, p.losses, p.draws)
                 : fmt::format("{}  {} W / {} L", Battles(p.games), p.wins, p.losses);
}

struct Rankings {
  std::mutex mutex;
  bool loading = false;
  bool loaded = false;
  std::string error;
  stats::LeaderboardPage ranked;
  stats::LeaderboardPage all;
  stats::PlayerStats me;
  bool have_me = false;
  int64_t opened_ms = 0;
};

std::atomic<bool> g_rankings_open{false};
std::shared_ptr<Rankings> g_rankings = std::make_shared<Rankings>();

void LoadRankings() {
  auto state = g_rankings;
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    if (state->loading) {
      return;
    }
    state->loading = true;
    state->opened_ms = NowMs();
  }
  std::thread([state] {
    stats::LeaderboardPage ranked = stats::FetchLeaderboard("ranked", 0, 10, false, -1, 5000);
    stats::LeaderboardPage all = stats::FetchLeaderboard("all", 0, 10, false, -1, 5000);
    stats::PlayerStats me;
    const bool have_me = stats::Me(&me);
    std::lock_guard<std::mutex> lock(state->mutex);
    state->ranked = std::move(ranked);
    state->all = std::move(all);
    state->me = me;
    state->have_me = have_me;
    state->error = state->ranked.ok ? "" : state->ranked.error;
    state->loading = false;
    state->loaded = true;
  }).detach();
}

class RankedOverlay final : public rex::ui::ImGuiDialog {
 public:
  explicit RankedOverlay(rex::ui::ImGuiDrawer* drawer) : ImGuiDialog(drawer) {}

 protected:
  void OnDraw(ImGuiIO& io) override {
    TrackBattle();
    std::string id;
    stats::PlayerStats card;
    if (stats::TakeCardRequest(&id, &card)) {
      card_ = card;
      card_ms_ = NowMs();
      if (!id.empty()) {
        // Fresh numbers and the per-character list, off the UI thread.
        auto slot = card_fetch_;
        std::thread([slot, id] {
          stats::PlayerStats full;
          if (stats::FetchPlayer(id, &full, 4000)) {
            std::lock_guard<std::mutex> lock(slot->mutex);
            slot->card = full;
            slot->id = id;
            slot->ready = true;
          }
        }).detach();
      }
    }
    {
      std::lock_guard<std::mutex> lock(card_fetch_->mutex);
      if (card_fetch_->ready) {
        card_fetch_->ready = false;
        if (card_fetch_->id == card_.id || card_.id.empty()) {
          card_ = card_fetch_->card;
        }
      }
    }
    if (!REXCVAR_GET(ranked_overlay) && !g_rankings_open.load()) {
      return;
    }
    ImGui::SetCurrentContext(io.Ctx);
    // The game's 16:9 picture, letterboxed in the window; 720p units.
    width_ = std::min(io.DisplaySize.x, io.DisplaySize.y * 16.0f / 9.0f);
    const float height = width_ * 9.0f / 16.0f;
    origin_ = ImVec2((io.DisplaySize.x - width_) * 0.5f, (io.DisplaySize.y - height) * 0.5f);
    u_ = height / 720.0f;
    ImDrawList* draw_list = ImGui::GetForegroundDrawList();
    if (REXCVAR_GET(ranked_overlay)) {
      DrawBoardExtras(draw_list);
      if (NowMs() - card_ms_ < 12000 && !card_.id.empty()) {
        DrawCard(draw_list);
      }
    }
    if (g_rankings_open.load()) {
      DrawRankings(draw_list);
    }
  }

 private:
  struct CardFetch {
    std::mutex mutex;
    bool ready = false;
    std::string id;
    stats::PlayerStats card;
  };

  ImVec2 At(float x, float y) const { return ImVec2(origin_.x + x * u_, origin_.y + y * u_); }

  void Text(ImDrawList* draw_list, float x, float y, float size, const std::string& text,
            ImU32 color) {
    rex::ui::overlay_text::Draw(draw_list, size * u_, At(x, y), color, text);
  }

  float TextWidth(float size, const std::string& text) {
    return rex::ui::overlay_text::Measure(size * u_, text).x / u_;
  }

  void Box(ImDrawList* draw_list, float x0, float y0, float x1, float y1, int alpha = 215) {
    draw_list->AddRectFilled(At(x0, y0), At(x1, y1), IM_COL32(20, 12, 60, alpha), 14.0f * u_);
    draw_list->AddRect(At(x0, y0), At(x1, y1), IM_COL32(222, 200, 150, 255), 14.0f * u_, 0,
                       2.0f * u_);
  }

  // A battle of an online session is running: if the session ends before its results screen,
  // the SDK reports it as abandoned by the opponent.
  void TrackBattle() {
    auto* memory = GuestMemory();
    if (!memory) {
      return;
    }
    // The match input driver's state: 3 while a battle is fought, 4 on its results and the
    // post-battle menu (Fight Again starts a new 3 without unloading the battle).
    const uint32_t driver = LoadU32(memory, kInputManagerPointer);
    const uint32_t battle = driver ? LoadU32(memory, driver + kDriverState) : 0;
    const bool fighting = battle == kDriverStateBattle && LoadU32(memory, kBattlePointer) != 0;
    const bool online = OnlineLobbySession();
    if (fighting && !last_battle_ && online) {
      int chars[2] = {-1, -1};
      if (const uint32_t request = LoadU32(memory, kRequestPointer)) {
        for (int side = 0; side < 2; ++side) {
          const uint16_t character = rex::memory::load_and_swap<uint16_t>(
              memory->TranslateVirtual<uint8_t*>(request + side * kRequestStride));
          chars[side] = character < 21 ? int(character) : -1;
        }
      }
      stats::SetBattleInProgress(true, chars[0], chars[1]);
    }
    last_battle_ = fighting;
  }

  // The game's Ranked leaderboard shows six rows (Position / Gamertag / Country / Power Level /
  // Grade); the page the game asked for is the lobby's page, so each of its rows gets the
  // player's battles, wins / losses and favourite character underneath.
  void DrawBoardExtras(ImDrawList* draw_list) {
    auto* memory = GuestMemory();
    if (!memory || !LoadU32(memory, kLeaderboardPointer)) {
      return;
    }
    stats::LeaderboardPage page;
    int64_t age = 0;
    if (!stats::LastBoardPage(&page, &age) || age > 120000) {
      return;
    }
    for (size_t i = 0; i < page.rows.size() && i < 6; ++i) {
      const stats::PlayerStats& p = page.rows[i];
      std::string text = RecordText(p);
      if (p.fav_char >= 0) {
        text += "  Fav: " + std::string(stats::CharacterName(p.fav_char));
      }
      // In the empty middle of the row, between the gamertag and the country flag.
      const float y = kBoardFirstRowY + float(i) * kBoardRowStep;
      const float w = TextWidth(kBoardTextSize, text);
      const float x = kBoardGapRight - w;
      draw_list->AddRectFilled(At(x - 8.0f, y - 2.0f), At(kBoardGapRight + 6.0f, y + kBoardTextSize + 4.0f),
                               IM_COL32(0, 0, 0, 140), 6.0f * u_);
      Text(draw_list, x, y, kBoardTextSize, text, kYellow);
    }
  }

  void DrawCard(ImDrawList* draw_list) {
    const stats::PlayerStats& p = card_;
    const float x0 = 790.0f, y0 = 120.0f, x1 = 1240.0f;
    std::vector<std::pair<std::string, ImU32>> lines;
    lines.push_back({fmt::format("Power Level {}  (Grade {})", Thousands(p.power), p.grade), kWhite});
    lines.push_back({p.rank > 0 ? fmt::format("Ranked position #{}", p.rank)
                                : std::string("Not ranked yet"),
                     kDim});
    lines.push_back({fmt::format("Ranked: {}, {} W / {} L", Battles(p.ranked_games), p.ranked_wins,
                                 p.ranked_losses),
                     kWhite});
    lines.push_back({fmt::format("All online: {}, {} W / {} L", Battles(p.games), p.wins, p.losses),
                     kWhite});
    lines.push_back({p.fav_char >= 0 ? fmt::format("Favourite: {} ({})",
                                                   stats::CharacterName(p.fav_char),
                                                   Battles(p.fav_games))
                                     : std::string("Favourite: -"),
                     kYellow});
    for (size_t i = 0; i < p.characters.size() && i < 3; ++i) {
      const auto& c = p.characters[i];
      lines.push_back({fmt::format("   {}: {}, {} W", c.name, Battles(c.games), c.wins), kDim});
    }
    const float y1 = y0 + 62.0f + float(lines.size()) * 30.0f;
    Box(draw_list, x0, y0, x1, y1, 235);
    Text(draw_list, x0 + 20.0f, y0 + 14.0f, 32.0f, p.name.empty() ? "Player" : p.name, kYellow);
    for (size_t i = 0; i < lines.size(); ++i) {
      Text(draw_list, x0 + 20.0f, y0 + 58.0f + float(i) * 30.0f, 22.0f, lines[i].first,
           lines[i].second);
    }
  }

  void DrawRankings(ImDrawList* draw_list) {
    auto state = g_rankings;
    std::lock_guard<std::mutex> lock(state->mutex);
    if (NowMs() - state->opened_ms > 45000) {
      g_rankings_open.store(false);
      return;
    }
    const float x0 = 90.0f, y0 = 70.0f, x1 = 1190.0f, y1 = 660.0f;
    Box(draw_list, x0, y0, x1, y1, 240);
    Text(draw_list, x0 + 24.0f, y0 + 16.0f, 34.0f, "Online Rankings", kYellow);
    if (state->have_me) {
      const auto& me = state->me;
      Text(draw_list, x0 + 380.0f, y0 + 24.0f, 22.0f,
           fmt::format("You: {}  Power Level {} ({})  {}  Fav: {}", me.name, Thousands(me.power),
                       me.grade, RecordText(me),
                       me.fav_char >= 0 ? stats::CharacterName(me.fav_char) : "-"),
           kWhite);
    }
    if (state->loading && !state->loaded) {
      Text(draw_list, x0 + 24.0f, y0 + 80.0f, 24.0f, "Asking the online lobby...", kDim);
      return;
    }
    if (!state->error.empty()) {
      Text(draw_list, x0 + 24.0f, y0 + 80.0f, 24.0f,
           "The online lobby can't be reached: " + state->error, kDim);
      return;
    }
    const float cols[] = {x0 + 24.0f, x0 + 80.0f, x0 + 330.0f, x0 + 500.0f, x0 + 580.0f,
                          x0 + 700.0f, x0 + 800.0f, x0 + 900.0f};
    const char* heads[] = {"#", "Player", "Power Level", "Grade", "Battles", "Wins", "Losses",
                           "Favourite"};
    float y = y0 + 74.0f;
    Text(draw_list, x0 + 24.0f, y, 22.0f, "Ranked Match (Power Level)", kYellow);
    y += 32.0f;
    for (int c = 0; c < 8; ++c) {
      Text(draw_list, cols[c], y, 20.0f, heads[c], kDim);
    }
    y += 28.0f;
    if (state->ranked.rows.empty()) {
      Text(draw_list, cols[1], y, 22.0f, "No ranked battles yet", kDim);
      y += 28.0f;
    }
    for (const auto& p : state->ranked.rows) {
      const ImU32 color = state->have_me && p.id == state->me.id ? kYellow : kWhite;
      const std::string fields[] = {
          std::to_string(p.rank), p.name, Thousands(p.power), p.grade,
          std::to_string(p.ranked_games), std::to_string(p.ranked_wins),
          std::to_string(p.ranked_losses),
          p.fav_char >= 0 ? stats::CharacterName(p.fav_char) : "-"};
      for (int c = 0; c < 8; ++c) {
        Text(draw_list, cols[c], y, 22.0f, fields[c], color);
      }
      y += 28.0f;
      if (y > y1 - 40.0f) {
        break;
      }
    }
    Text(draw_list, x0 + 24.0f, y1 - 34.0f, 18.0f,
         fmt::format("{} ranked players. All online battles (Player Match too) count in Battles / "
                     "Wins / Losses on the cards. Closes by itself.",
                     state->ranked.total),
         kDim);
  }

  // Where the game draws its leaderboard rows (720p units), measured on its screen.
  static constexpr float kBoardFirstRowY = 254.0f;  // text top in row 1 (rows 60 apart)
  static constexpr float kBoardRowStep = 60.0f;
  static constexpr float kBoardGapRight = 770.0f;    // left of the country flag
  static constexpr float kBoardTextSize = 19.0f;

  ImVec2 origin_;
  float width_ = 0.0f;
  float u_ = 1.0f;
  bool last_battle_ = false;
  stats::PlayerStats card_;
  int64_t card_ms_ = 0;
  std::shared_ptr<CardFetch> card_fetch_ = std::make_shared<CardFetch>();
};

}  // namespace

std::unique_ptr<rex::ui::ImGuiDialog> BurstLimitCreateRankedOverlay(rex::ui::ImGuiDrawer* drawer) {
  return std::make_unique<RankedOverlay>(drawer);
}

namespace {
void ToggleRankings() {
  if (g_rankings_open.exchange(false)) {
    return;
  }
  {
    std::lock_guard<std::mutex> lock(g_rankings->mutex);
    g_rankings->loaded = false;
  }
  LoadRankings();
  g_rankings_open.store(true);
}
}  // namespace

REXCVAR_DEFINE_COMMAND(ranked_rankings, ToggleRankings, "Online",
                       "Show / hide the online rankings page (also in the settings menu, ONLINE)");

// The settings menu (ONLINE): your ranked line and the rankings page.
void BurstLimitRankedMenu(rex::ui::QuickMenuConfig& menu) {
  using Item = rex::ui::QuickMenuItem;
  rex::ui::QuickMenuSection* online = nullptr;
  for (auto& section : menu.sections) {
    if (section.title == "ONLINE") {
      online = &section;
    }
  }
  if (!online) {
    online = &menu.sections.emplace_back();
    online->title = "ONLINE";
  }
  online->status = []() {
    rex::ui::QuickMenuStatus status;
    status.text = stats::IdentityStatus();
    return status;
  };
  Item& rankings = online->items.emplace_back();
  rankings.kind = Item::Kind::kAction;
  rankings.label = "Online rankings";
  rankings.action_label = "Show";
  rankings.help =
      "The online leaderboard: Ranked Match Power Level, and each player's battles, wins, "
      "losses and favourite character. Shown over the game for 45 seconds (choose again to hide "
      "it). In the game: Versus > Online Battle > Ranked Match > Leaderboard.";
  rankings.action = [](const std::string&) { ToggleRankings(); };
  Item& overlay = online->items.emplace_back();
  overlay.kind = Item::Kind::kToggle;
  overlay.label = "Leaderboard details";
  overlay.cvar = "ranked_overlay";
  overlay.help =
      "On the game's Ranked leaderboard, show each player's battles, wins / losses and favourite "
      "character; Gamer Card shows the player's online card.";
}
