// Quick fight (debug / automated tests): straight into a Training or Versus
// fight with the characters, start forms and stage asked for, without the
// logos, title, storage prompt, main menu or character / stage select.
//
//   --quick_fight=training,goku:ssj,krillin,plains     at startup (the boot
//                                                      screen jumps to the fight)
//   quick_fight_now versus vegeta:2 cell:perfect namek  console / command pipe:
//                                                      starts it from the boot,
//                                                      title or main menu screen
//
// Spec: [mode] <p1>[:<form>[:<color>]] [<p2>[:<form>[:<color>]]] [<stage>]
//   mode   training (default; P2 is the training dummy), versus (P2 CPU),
//          versus2p (P2 on the second controller), cpu (CPU vs CPU)
//   p1/p2  goku, kidgohan, teengohan, vegeta, krillin, trunks, piccolo, tien,
//          yamcha, raditz, nappa, ginyu, recoome, frieza, a16, a17, a18, cell,
//          saibamen, broly, bardock, a 3-letter code (GOK, KLL...) or 0-20
//   form   start form (start_forms): a number or its name - ssj, ss2, kaioken,
//          up, nail, kami, final, full, perfect, superperfect...; every form
//          is unlocked for the fight (debug)
//   stage  plains, namek, mountains, cell, archipelago, their alternate colors
//          plains2 / namek2 / mountains2 / cell2 / archipelago2, dyingnamek,
//          wasteland, seaside, or the stage id (0-24, even); "cell" after two
//          fighters is the stage, use "cellgames" to be explicit
//   nointro (anywhere) Versus without the fighters' intros (the rules'
//          cut-scenes setting off)
// Z Chronicles: --quick_fight=zc (or zc,1 / story,ep1) starts the first
// episode (Goku vs Raditz) the way the main menu's "Z Chronicles" does, then
// confirms the Z Chronicles screens and skips the scenes before the fight by
// itself (pad presses on player 1) until FIGHT!. A fresh save has only
// episode 1, so only that one is supported.
// Defaults: training, goku vs krillin, plains. The rest is what confirming
// every window with A gives (first ultimate, first Drama Piece partner), and
// no Drama Pieces. Offline only (it waits while a game session is open).
//
// How the game starts a fight from its menus (the Training screen's last
// state, sub_822651E0 at 0x82265DFC; Versus, sub_8226FEF0 at 0x82271CE8; the
// title's attract demo, sub_82238018, does the same in one frame):
//  - the battle settings, 252 bytes at 0x8424D568 (sub_822326F8() + 8), 72
//    per player: +0 character, +4 highest form, +8 player type (0 1P, 1 2P,
//    3 CPU / dummy), +10 color, +12 ultimate (index), +20..+40 Drama Pieces,
//    +56 Drama Piece partner (a character); +148 stage, +150 mode (1 Versus,
//    2 Training, 7 attract demo), +155 the unlock bytes (sub_82211F78);
//    filled from the select screens' per-side blocks (0x84249080, 0x60 each:
//    +16 character, +24.. Drama Pieces, +48 ultimate, +52 partner, +56 color,
//    +60 stage; reset by sub_82233C78), then committed by sub_82211F60;
//  - sub_82232AD8(6) queues game mode 6 (sub_82211980), whose handler,
//    sub_82213550, copies the settings and starts the battle tasks;
//  - the screen's function returns true and the screen manager (sub_8223D7A0)
//    tears the screen down.
// Screens are slots of 3488 bytes at 0x84249140 (+0 in use, +8 type); type 0
// is the boot logos (sub_82232B28), 1 the title (sub_82232E90), 2 a movie
// (sub_82233420), 4 the main menu (sub_82251868). The jump is made from one of
// those, by replacing its function (the recompiled ones are weak), when it is
// the only screen up and has settled (title waiting for Start, main menu list
// taking input; the menu movie is stopped first, like the menus' own exits).
// Before the title's Start (boot, opening movie, title) the title's set-up runs
// first: a fresh save's defaults (sub_82124558 / sub_82219CB0, what the title
// does before the storage prompt), player 1 on the first controller. The main
// menu's cursor (0x8424D684) is set to the mode, so Character Select in the
// pause menu goes back to that mode's select screen (via sub_8224E768).

#include <algorithm>
#include <atomic>
#include <chrono>
#include <thread>
#include <cctype>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <rex/cvar.h>
#include <rex/input/input_system.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/memory/utils.h>
#include <rex/net/session.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>

REXCVAR_DEFINE_STRING(quick_fight, "", "Debug",
                      "Testing: skip the menus and start a fight at boot, e.g. "
                      "training,goku:ssj,krillin,plains (mode training|versus|versus2p|cpu, "
                      "<p1>[:form[:color]], <p2>[:form[:color]], stage), or zc (Z Chronicles episode 1, "
                      "Goku vs Raditz). The command "
                      "quick_fight_now <same> starts one from the title or main menu");

// burstlimit_forms.cpp
int BurstLimitFormCount(uint32_t character);
const char* BurstLimitFormName(uint32_t character, int form);
void BurstLimitSetStartForm(uint32_t side, uint32_t character, int form);

// The game's functions this calls (weak recompiled ones; the screens' are
// replaced below and call their originals).
REX_EXTERN(sub_82124558);  // save data defaults (r3 = save + 16)
REX_EXTERN(sub_82124868);  // system flags for the menus
REX_EXTERN(sub_8210D7F0);
REX_EXTERN(sub_82219CB0);  // default unlocks (r3 = save + 16)
REX_EXTERN(sub_82280260);  // player r3 reads controller r4 (-1 any)
REX_EXTERN(sub_82293520);  // the controller the profile / storage use
REX_EXTERN(sub_822938D0);
REX_EXTERN(sub_822938C0);
REX_EXTERN(sub_8227DB90);
REX_EXTERN(sub_82233C78);  // select screens' per-side blocks to defaults
REX_EXTERN(sub_82233D98);  // time limit index -> value
REX_EXTERN(sub_82211F78);  // unlock bytes into the settings (r3 = settings + 155)
REX_EXTERN(sub_82211F60);  // commit the settings
REX_EXTERN(sub_82280438);
REX_EXTERN(sub_82280490);
REX_EXTERN(sub_82232718);  // the select screens' context (26 Training, 16-18 Versus)
REX_EXTERN(sub_82232AD8);  // queue a game mode (6 = battle)
REX_EXTERN(sub_8224EE58);  // stop the menu movie
REX_EXTERN(sub_8224ED70);  // the menu movie is still playing
REX_EXTERN(sub_82250C88);  // the menus' exit: stop their movies
REX_EXTERN(sub_82288640);  // the menus' list window with id r3 (0 = the main menu's), or 0
REX_EXTERN(sub_82289758);  // close and free a list window
REX_EXTERN(sub_82120C90);
REX_EXTERN(sub_8210A5F8);
REX_EXTERN(__imp__sub_82232B28);  // boot logos screen
REX_EXTERN(__imp__sub_82232E90);  // title screen
REX_EXTERN(__imp__sub_82233420);  // movie screen
REX_EXTERN(__imp__sub_82251868);  // main menu screen

namespace {

constexpr uint32_t kCharacterCount = 21;
constexpr uint32_t kCharacterRecords = 0x826BC2A8;  // 980 bytes per character
constexpr uint32_t kCharacterRecordStride = 980;
constexpr uint32_t kUltimateLists = 0x826DF6D8;     // per character -> {count, (text, id) x3}
constexpr uint32_t kUltimateListDefault = 0x826DF6D0;
constexpr uint32_t kPartnerOrder = 0x826DFA98;      // the partner list's order, 22 u16
constexpr uint32_t kSettings = 0x8424D568;
constexpr uint32_t kSideBlocks = 0x84249080;
constexpr uint32_t kSideStride = 0x60;
constexpr uint32_t kScreenSlots = 0x84249140;
constexpr uint32_t kScreenSlotStride = 3488;
constexpr uint32_t kScreenSlotCount = 5;
constexpr uint32_t kSavePointer = 0x825205E8;
constexpr uint32_t kMenuFlagsWord = 0x8424D6B0;
constexpr uint32_t kMenuReturnByte = 0x8424D6AC;  // 1: the select screens keep their sides
constexpr uint32_t kMenuByteAE = 0x8424D6AE;
constexpr uint32_t kMainMenuCursor = 0x8424D684;  // 0 Z Chronicles, 1 Versus, ... 4 Training

enum class Mode { kTraining, kVersus, kVersus2P, kCpu, kStory };

struct Fighter {
  uint32_t character = 0;
  int form = 0;
  uint32_t color = 0;
};

struct Request {
  Mode mode = Mode::kTraining;
  Fighter p1{0, 0, 0};
  Fighter p2{4, 0, 0};
  uint32_t stage = 8;
  bool cutscenes = true;  // Versus: the fighters' intros (and other battle cut-scenes)
  uint32_t episode = 0;   // Z Chronicles
  // Z Chronicles with a save: the title and its storage prompt first (answered
  // by StoryDriver), the jump from the main menu - the game only awards the
  // episode's own achievement ("The Story Begins") with a storage device.
  bool story_save = false;
  std::string text;
};

constexpr const char* kCodes[kCharacterCount] = {
    "GOK", "GHS", "GHM", "VGT", "KLL", "TRX", "PIC", "TSH", "YMC", "RAD", "NAP",
    "GNY", "RCM", "FRZ", "16G", "17G", "18G", "CEL", "SBM", "BRL", "BDK"};

constexpr const char* kNames[kCharacterCount] = {
    "Goku",   "Kid Gohan", "Teen Gohan", "Vegeta", "Krillin", "Trunks", "Piccolo",
    "Tien",   "Yamcha",    "Raditz",     "Nappa",  "Ginyu",   "Recoome", "Frieza",
    "Android 16", "Android 17", "Android 18", "Cell", "Saibamen", "Broly", "Bardock"};

struct Alias {
  const char* name;
  uint32_t value;
};

constexpr Alias kCharacterAliases[] = {
    {"goku", 0},      {"kidgohan", 1},   {"gohankid", 1},  {"teengohan", 2}, {"gohanteen", 2},
    {"gohan", 2},     {"vegeta", 3},     {"krillin", 4},   {"kuririn", 4},   {"trunks", 5},
    {"piccolo", 6},   {"tien", 7},       {"tenshinhan", 7}, {"yamcha", 8},   {"raditz", 9},
    {"nappa", 10},    {"ginyu", 11},     {"recoome", 12},  {"frieza", 13},   {"freeza", 13},
    {"android16", 14}, {"a16", 14},      {"android17", 15}, {"a17", 15},     {"android18", 16},
    {"a18", 16},      {"cell", 17},      {"saibamen", 18}, {"saibaman", 18}, {"broly", 19},
    {"bardock", 20},
};

struct StageName {
  const char* name;
  uint32_t id;
  const char* label;
};

constexpr StageName kStages[] = {
    {"plains", 8, "Plains"},
    {"plains2", 18, "Plains (alternate colors)"},
    {"namek", 24, "Namek"},
    {"namek2", 0, "Namek (alternate colors)"},
    {"mountains", 10, "Mountains"},
    {"rocky", 10, "Mountains"},
    {"mountains2", 20, "Mountains (alternate colors)"},
    {"cell", 14, "Cell Games arena"},
    {"cellgames", 14, "Cell Games arena"},
    {"cell2", 2, "Cell Games arena (alternate colors)"},
    {"archipelago", 4, "Archipelago"},
    {"islands", 4, "Archipelago"},
    {"archipelago2", 16, "Archipelago (alternate colors)"},
    {"dyingnamek", 6, "Dying Namek"},
    {"wasteland", 12, "Wasteland"},
    {"seaside", 22, "Seaside Cliffs"},
    {"cliffs", 22, "Seaside Cliffs"},
    // The stage files' codes.
    {"pln", 8, "Plains"},
    {"plm", 18, "Plains (alternate colors)"},
    {"nmd", 24, "Namek"},
    {"nmc", 0, "Namek (alternate colors)"},
    {"rck", 10, "Mountains"},
    {"rcl", 20, "Mountains (alternate colors)"},
    {"cem", 14, "Cell Games arena"},
    {"cel", 2, "Cell Games arena (alternate colors)"},
    {"lnd", 4, "Archipelago"},
    {"lne", 16, "Archipelago (alternate colors)"},
    {"nmh", 6, "Dying Namek"},
    {"eth", 12, "Wasteland"},
    {"eti", 22, "Seaside Cliffs"},
};

std::string Normalize(std::string_view text) {
  std::string out;
  for (char c : text) {
    if (std::isalnum(static_cast<unsigned char>(c))) {
      out += char(std::tolower(static_cast<unsigned char>(c)));
    }
  }
  return out;
}

bool ParseNumber(std::string_view text, uint32_t& value) {
  if (text.empty() || text.size() > 4) {
    return false;
  }
  uint32_t v = 0;
  for (char c : text) {
    if (c < '0' || c > '9') {
      return false;
    }
    v = v * 10 + uint32_t(c - '0');
  }
  value = v;
  return true;
}

std::optional<uint32_t> ParseCharacter(std::string_view token) {
  const std::string key = Normalize(token);
  uint32_t number;
  if (ParseNumber(key, number)) {
    return number < kCharacterCount ? std::optional<uint32_t>(number) : std::nullopt;
  }
  for (const Alias& alias : kCharacterAliases) {
    if (key == alias.name) {
      return alias.value;
    }
  }
  for (uint32_t c = 0; c < kCharacterCount; ++c) {
    if (key == Normalize(kCodes[c])) {
      return c;
    }
  }
  return std::nullopt;
}

std::optional<int> ParseForm(uint32_t character, std::string_view token) {
  const int count = std::max(BurstLimitFormCount(character), 1);
  std::string key = Normalize(token);
  uint32_t number;
  if (ParseNumber(key, number)) {
    return int(number) < count ? std::optional<int>(int(number)) : std::nullopt;
  }
  if (key == "base" || key == "normal") {
    return 0;
  }
  if (key == "ss" || key == "ssj" || key == "ss1" || key == "ssj1") {
    key = "supersaiyan";
  } else if (key == "ss2" || key == "ssj2") {
    // Vegeta's and Trunks' third form (Super Vegeta / Super Trunks) is their
    // Super Saiyan's second grade.
    if (character == 3 || character == 5) {
      return count > 2 ? std::optional<int>(2) : std::nullopt;
    }
    key = "supersaiyan2";
  } else if (key == "up") {
    key = "unlockedpotential";
  } else if (key == "full") {
    key = "fullpower";
  }
  // Exact name, then a name that starts with it, then one that contains it.
  for (int pass = 0; pass < 3; ++pass) {
    for (int form = 0; form < count; ++form) {
      const char* name = BurstLimitFormName(character, form);
      if (!name) {
        continue;
      }
      const std::string n = Normalize(name);
      if ((pass == 0 && n == key) || (pass == 1 && n.rfind(key, 0) == 0) ||
          (pass == 2 && n.find(key) != std::string::npos)) {
        return form;
      }
    }
  }
  return std::nullopt;
}

std::optional<uint32_t> ParseStage(std::string_view token) {
  const std::string key = Normalize(token);
  uint32_t number;
  if (ParseNumber(key, number)) {
    return (number <= 24 && number % 2 == 0) ? std::optional<uint32_t>(number) : std::nullopt;
  }
  for (const StageName& stage : kStages) {
    if (key == stage.name) {
      return stage.id;
    }
  }
  return std::nullopt;
}

const char* StageLabel(uint32_t id) {
  for (const StageName& stage : kStages) {
    if (stage.id == id) {
      return stage.label;
    }
  }
  return "?";
}

const char* ModeName(Mode mode) {
  switch (mode) {
    case Mode::kTraining:
      return "Training";
    case Mode::kVersus:
      return "Versus (P2 CPU)";
    case Mode::kVersus2P:
      return "Versus (2 players)";
    case Mode::kCpu:
      return "Versus (CPU vs CPU)";
    case Mode::kStory:
      return "Z Chronicles";
  }
  return "?";
}

bool ParseFighter(std::string_view token, Fighter& fighter, std::string& error) {
  std::vector<std::string_view> parts;
  size_t start = 0;
  while (start <= token.size()) {
    const size_t end = std::min(token.find(':', start), token.size());
    parts.push_back(token.substr(start, end - start));
    start = end + 1;
  }
  const auto character = ParseCharacter(parts[0]);
  if (!character) {
    error = "unknown character '" + std::string(parts[0]) + "'";
    return false;
  }
  fighter = Fighter{*character, 0, 0};
  if (parts.size() > 1 && !parts[1].empty()) {
    const auto form = ParseForm(*character, parts[1]);
    if (!form) {
      error = "unknown form '" + std::string(parts[1]) + "' for " + kNames[*character];
      return false;
    }
    fighter.form = *form;
  }
  if (parts.size() > 2 && !parts[2].empty()) {
    uint32_t color;
    if (!ParseNumber(parts[2], color) || color > 7) {
      error = "bad color '" + std::string(parts[2]) + "'";
      return false;
    }
    fighter.color = color;
  }
  return true;
}

bool ParseRequest(std::string_view text, Request& request, std::string& error) {
  std::vector<std::string_view> tokens;
  size_t i = 0;
  while (i < text.size()) {
    while (i < text.size() && (text[i] == ',' || std::isspace(static_cast<unsigned char>(text[i])))) {
      ++i;
    }
    const size_t begin = i;
    while (i < text.size() && text[i] != ',' && !std::isspace(static_cast<unsigned char>(text[i]))) {
      ++i;
    }
    if (i > begin) {
      tokens.push_back(text.substr(begin, i - begin));
    }
  }
  request = Request{};
  request.text = std::string(text);
  // Options anywhere in the list.
  for (auto it = tokens.begin(); it != tokens.end();) {
    const std::string option = Normalize(*it);
    if (option == "nointro" || option == "nocutscenes" || option == "skipintro") {
      request.cutscenes = false;
      it = tokens.erase(it);
    } else {
      ++it;
    }
  }
  size_t next = 0;
  if (next < tokens.size()) {
    const std::string mode = Normalize(tokens[next]);
    if (mode == "zc" || mode == "story" || mode == "zchronicles" || mode == "chronicles") {
      request.mode = Mode::kStory;
      request.episode = 1;
      for (size_t t = 1; t < tokens.size(); ++t) {
        std::string word = Normalize(tokens[t]);
        if (word == "save" || word == "storage") {
          request.story_save = true;
          continue;
        }
        if (word.rfind("episode", 0) == 0) {
          word = word.substr(7);
        } else if (word.rfind("ep", 0) == 0) {
          word = word.substr(2);
        }
        uint32_t number;
        if (!ParseNumber(word, number) || number != 1) {
          error = "Z Chronicles: only episode 1 (Goku vs Raditz) can be started, or 'save' ('" +
                  std::string(tokens[t]) + "')";
          return false;
        }
      }
      return true;
    }
    bool is_mode = true;
    if (mode == "training" || mode == "train") {
      request.mode = Mode::kTraining;
    } else if (mode == "versus" || mode == "vs" || mode == "vscpu" || mode == "vscom") {
      request.mode = Mode::kVersus;
    } else if (mode == "versus2p" || mode == "vs2p" || mode == "2p") {
      request.mode = Mode::kVersus2P;
    } else if (mode == "cpu" || mode == "cpuvscpu" || mode == "watch") {
      request.mode = Mode::kCpu;
    } else {
      is_mode = false;
    }
    next += is_mode ? 1 : 0;
  }
  Fighter* fighters[2] = {&request.p1, &request.p2};
  for (int side = 0; side < 2 && next < tokens.size(); ++side) {
    // A stage name where the second fighter would be: only one fighter given.
    if (side == 1 && !ParseCharacter(tokens[next].substr(0, tokens[next].find(':'))) &&
        ParseStage(tokens[next])) {
      break;
    }
    if (!ParseFighter(tokens[next], *fighters[side], error)) {
      return false;
    }
    ++next;
  }
  if (next < tokens.size()) {
    const auto stage = ParseStage(tokens[next]);
    if (!stage) {
      error = "unknown stage '" + std::string(tokens[next]) + "'";
      return false;
    }
    request.stage = *stage;
    ++next;
  }
  if (next < tokens.size()) {
    error = "unexpected '" + std::string(tokens[next]) + "'";
    return false;
  }
  return true;
}

// Guest memory, big-endian.
uint8_t* At(uint8_t* base, uint32_t address) {
  return base + address;
}
uint32_t R32(uint8_t* base, uint32_t address) {
  return rex::memory::load_and_swap<uint32_t>(At(base, address));
}
uint16_t R16(uint8_t* base, uint32_t address) {
  return rex::memory::load_and_swap<uint16_t>(At(base, address));
}
void W32(uint8_t* base, uint32_t address, uint32_t value) {
  rex::memory::store_and_swap<uint32_t>(At(base, address), value);
}
void W16(uint8_t* base, uint32_t address, uint16_t value) {
  rex::memory::store_and_swap<uint16_t>(At(base, address), value);
}
void W8(uint8_t* base, uint32_t address, uint8_t value) {
  *At(base, address) = value;
}

// A guest call with up to two arguments; returns r3.
using GuestFunction = void (*)(PPCContext&, uint8_t*);
uint32_t Call(PPCContext& ctx, uint8_t* base, GuestFunction function, int64_t a = 0, int64_t b = 0) {
  ctx.r3.s64 = a;
  ctx.r4.s64 = b;
  function(ctx, base);
  return ctx.r3.u32;
}

// The ultimate the ultimate window starts on (its first entry) for a character.
uint32_t DefaultUltimate(uint8_t* base, uint32_t character) {
  uint32_t list = R32(base, kUltimateLists + character * 4);
  if (!list) {
    list = kUltimateListDefault;
  }
  return R16(base, list) ? R16(base, list + 4) : 0;
}

// The Drama Piece partner the partner window starts on: the first of its list
// that isn't the character itself (Goku vs Krillin: Krillin and Goku, as
// confirming the windows with A gives).
uint32_t DefaultPartner(uint8_t* base, uint32_t character) {
  for (uint32_t i = 0; i < 22; ++i) {
    const uint32_t c = R16(base, kPartnerOrder + i * 2);
    if (c < kCharacterCount && c != character) {
      return c;
    }
  }
  return character == 0 ? 4 : 0;
}

uint32_t ColorCount(uint8_t* base, uint32_t character) {
  return uint32_t(R16(base, kCharacterRecords + character * kCharacterRecordStride + 2)) + 1;
}

std::mutex g_mutex;
std::optional<Request> g_pending;     // armed by quick_fight_now
std::atomic<bool> g_has_pending{false};  // g_pending is set (checked every menu frame)
std::atomic<bool> g_boot_checked{false};

// The title's set-up the boot screen skips: a fresh save's defaults, player 1
// on the first controller (what the title does when Start is pressed, before
// the storage prompt).
void TitleSetup(PPCContext& ctx, uint8_t* base) {
  const uint32_t save = R32(base, kSavePointer);
  Call(ctx, base, sub_82124868);
  Call(ctx, base, sub_8210D7F0, 0);
  if (save) {
    Call(ctx, base, sub_82124558, save + 16);
    Call(ctx, base, sub_82219CB0, save + 16);
    W32(base, save + 3296, R32(base, save + 3296) & ~0x20000000u);
  }
  Call(ctx, base, sub_82280260, 0, 0);
  Call(ctx, base, sub_82293520, 0);
  Call(ctx, base, sub_82280260, 1, -1);
  if (Call(ctx, base, sub_822938D0) != 0 && R32(base, 0x8424DB3C)) {
    Call(ctx, base, sub_8227DB90);
  }
  Call(ctx, base, sub_822938C0, 0);
}

// Fills the select screens' sides and the battle settings like the Training /
// Versus screens do when their last window closes, and queues the battle.
void StartFight(PPCContext& ctx, uint8_t* base, const Request& request, bool from_menu) {
  const uint32_t c1 = request.p1.character;
  const uint32_t c2 = request.p2.character;
  // The sides, as if every window had been confirmed with A.
  Call(ctx, base, sub_82233C78);
  const uint32_t sides[2] = {kSideBlocks, kSideBlocks + kSideStride};
  const Fighter* fighters[2] = {&request.p1, &request.p2};
  for (int side = 0; side < 2; ++side) {
    const uint32_t b = sides[side];
    const uint32_t c = fighters[side]->character;
    W32(base, b + 8, 100);  // the side's step: done
    W32(base, b + 16, c);
    W32(base, b + 48, DefaultUltimate(base, c));
    W32(base, b + 52, DefaultPartner(base, c));
    W8(base, b + 65, 0);
    W32(base, b + 56, fighters[side]->color % ColorCount(base, c));
  }
  W32(base, kSideBlocks + 60, request.stage);
  W8(base, kSideBlocks + 66, 0);

  const uint32_t s = kSettings;
  const uint32_t b0 = sides[0], b1 = sides[1];
  const bool versus = request.mode != Mode::kTraining;
  W8(base, s + 150, versus ? 1 : 2);
  W8(base, s + 151, 0);
  // Player 1.
  W16(base, s + 0, uint16_t(c1));
  W16(base, s + 10, uint16_t(R32(base, b0 + 56)));
  W16(base, s + 12, uint16_t(R32(base, b0 + 48)));
  W16(base, s + 56, uint16_t(R32(base, b0 + 52)));
  W16(base, s + 60, 0);
  // -1 after the menus' Drama Piece window (the attract demo sets it too).
  W16(base, s + 62, 0xFFFF);
  W32(base, s + 20, R32(base, b0 + 24));
  W32(base, s + 24, R32(base, b0 + 28));
  W32(base, s + 28, R32(base, b0 + 32));
  W16(base, s + 8, request.mode == Mode::kCpu ? 3 : 0);
  // Every form, not only the save's (sub_82233DF0): a debug fight.
  W16(base, s + 4, uint16_t(std::max(BurstLimitFormCount(c1), 1) - 1));
  // Player 2.
  W16(base, s + 72, uint16_t(c2));
  W16(base, s + 82, uint16_t(R32(base, b1 + 56)));
  W16(base, s + 84, uint16_t(R32(base, b1 + 48)));
  W16(base, s + 128, uint16_t(R32(base, b1 + 52)));
  W16(base, s + 132, 0);
  W16(base, s + 134, 0xFFFF);
  W32(base, s + 92, R32(base, b1 + 24));
  W32(base, s + 96, R32(base, b1 + 28));
  W32(base, s + 100, R32(base, b1 + 32));
  W16(base, s + 80, request.mode == Mode::kVersus2P ? 1 : 3);
  W16(base, s + 76, uint16_t(std::max(BurstLimitFormCount(c2), 1) - 1));
  W8(base, s + 148, uint8_t(request.stage));
  // The same character twice in the same color: the game moves player 2 on.
  if (c1 == c2 && R16(base, s + 82) == R16(base, s + 10)) {
    W16(base, s + 82, uint16_t((R16(base, s + 82) + 1) % ColorCount(base, c2)));
  }
  uint32_t back_to = 26;
  if (versus) {
    // Life, the two gauge settings, CPU level, time limit, rounds (the
    // Versus rules' defaults from sub_82233C78).
    W16(base, s + 44, uint16_t((R32(base, b0 + 68) + 1) * 1000));
    W16(base, s + 48, uint16_t(R32(base, b0 + 72) + 1));
    W16(base, s + 50, uint16_t(R32(base, b0 + 76) + 1));
    W16(base, s + 52, 0);
    W16(base, s + 116, uint16_t((R32(base, b1 + 68) + 1) * 1000));
    W16(base, s + 120, uint16_t(R32(base, b1 + 72) + 1));
    W16(base, s + 122, uint16_t(R32(base, b1 + 76) + 1));
    W16(base, s + 124, 0);
    switch (request.mode) {
      case Mode::kVersus:
        W8(base, s + 17, uint8_t(R32(base, b1 + 80) + 1));
        W8(base, s + 89, uint8_t(R32(base, b1 + 80) + 1));
        back_to = 16;
        break;
      case Mode::kVersus2P:
        W8(base, s + 17, 1);
        W8(base, s + 89, 1);
        back_to = 17;
        break;
      default:
        W8(base, s + 17, uint8_t(R32(base, b0 + 80) + 1));
        W8(base, s + 89, uint8_t(R32(base, b1 + 80) + 1));
        back_to = 18;
        break;
    }
    W8(base, s + 152, uint8_t(Call(ctx, base, sub_82233D98, R32(base, b0 + 84))));
    W8(base, s + 153, uint8_t(R32(base, b0 + 88)));
    // The rules' cut-scenes setting (the request's +214): 0 skips the intros.
    W8(base, s + 154, request.cutscenes ? uint8_t(R32(base, b0 + 92)) : 0);
  } else {
    W8(base, s + 153, 0);
    W8(base, s + 154, 1);
  }
  W8(base, s + 182, 1);
  Call(ctx, base, sub_82211F78, s + 155);
  Call(ctx, base, sub_82211F60, s);
  Call(ctx, base, sub_82280438);
  Call(ctx, base, sub_82280490);
  Call(ctx, base, sub_82232718, back_to);
  Call(ctx, base, sub_82232AD8, 6);
  if (from_menu) {
    // The title / main menu may have a movie and music running.
    Call(ctx, base, sub_8224EE58);
    Call(ctx, base, sub_82120C90);
    Call(ctx, base, sub_8210A5F8);
  }
  W32(base, kMenuFlagsWord, 0);
  W8(base, kMenuReturnByte, 1);
  // The main menu's cursor: leaving the fight through Character Select goes
  // back through the main menu to the mode under it (4 Training, 1 Versus).
  W32(base, kMainMenuCursor, versus ? 1 : 4);
  if (!versus) {
    W8(base, kMenuByteAE, 0);
  }
  // The start forms (burstlimit_forms.cpp's loader hook puts them in).
  BurstLimitSetStartForm(0, c1, request.p1.form);
  BurstLimitSetStartForm(1, c2, request.p2.form);
  REXLOG_INFO(
      "Quick fight: {} - P1 {} ({}, color {}) vs P2 {} ({}, color {}) on {} (stage {}){}: battle "
      "queued",
      ModeName(request.mode), kNames[c1],
      BurstLimitFormName(c1, request.p1.form) ? BurstLimitFormName(c1, request.p1.form) : "Normal",
      R16(base, s + 10), kNames[c2],
      BurstLimitFormName(c2, request.p2.form) ? BurstLimitFormName(c2, request.p2.form) : "Normal",
      R16(base, s + 82), StageLabel(request.stage), request.stage,
      versus && !request.cutscenes ? ", no intros" : "");
  rex::FlushLogging();
}

// Z Chronicles from the boot / title / main menu: what the main menu does
// when "Z Chronicles" is picked (sub_82251868 at 0x82251F28): the select
// screens' context 13, the Z Chronicles object's byte +62 (at 0x84248B90)
// cleared, game mode 11 queued. The Z Chronicles screens then run as usual;
// StoryDriver confirms them.
constexpr uint32_t kStoryObject = 0x84248B88;  // the Z Chronicles object (state +188)
constexpr uint32_t kBattlePointer = 0x841B5130;
constexpr uint32_t kBattlePhase = 0x841B5228;  // 0 loading, 1 READY?, 2 FIGHT!

std::atomic<bool> g_story_driver{false};
std::atomic<bool> g_story_queued{false};

void StoryDriver() {
  using namespace std::chrono;
  auto* runtime = rex::Runtime::instance();
  auto* memory = runtime ? runtime->memory() : nullptr;
  if (!memory) {
    return;
  }
  uint8_t* base = memory->TranslateVirtual<uint8_t*>(0);
  auto press = [](uint16_t buttons, uint32_t ms) {
    auto* rt = rex::Runtime::instance();
    if (auto* input = rt ? static_cast<rex::input::InputSystem*>(rt->input_system()) : nullptr) {
      input->InjectButtons(0, buttons, ms, 0, 0);
    }
  };
  const auto start = steady_clock::now();
  int64_t last_state = -2;
  uint32_t last_battle = 1;
  int presses = 0;
  while (g_story_driver.load()) {
    std::this_thread::sleep_for(milliseconds(100));
    const auto elapsed = duration_cast<milliseconds>(steady_clock::now() - start).count();
    if (elapsed > 180000) {
      REXLOG_WARN("Quick fight: Z Chronicles: no FIGHT! after 180 s, stopping the presses");
      break;
    }
    if (!g_story_queued.load()) {
      // Boot, title and its storage prompt / notices: Start at the title, A
      // (YES, OK) for the windows, until the main menu takes the jump.
      bool main_menu = false;
      for (uint32_t i = 0; i < kScreenSlotCount; ++i) {
        const uint32_t slot = kScreenSlots + i * kScreenSlotStride;
        main_menu |= R32(base, slot) != 0 && R32(base, slot + 8) == 4;
      }
      // (Not on the main menu: A there would pick a mode itself.)
      if (!main_menu && (elapsed / 100) % 8 == 0) {
        ++presses;
        press(presses % 2 ? 0x0010 : 0x1000, 70);
      }
      continue;
    }
    const uint32_t story = R32(base, kStoryObject);
    const int64_t state = story ? int64_t(R32(base, story + 188)) : -1;
    const uint32_t battle = R32(base, kBattlePointer);
    const uint32_t phase = battle ? R32(base, kBattlePhase) : 0;
    if (state != last_state || (battle != 0) != (last_battle != 0)) {
      REXLOG_INFO("Quick fight: Z Chronicles: {:.1f} s, story state {}, battle {:08X} phase {}",
                  elapsed / 1000.0, state, battle, phase);
      last_state = state;
      last_battle = battle;
    }
    if (battle && phase == 2) {
      REXLOG_INFO("Quick fight: Z Chronicles episode 1: FIGHT! after {:.1f} s ({} presses)",
                  elapsed / 1000.0, presses);
      rex::FlushLogging();
      break;
    }
    // Every 0.8 s: A confirms the Z Chronicles screens and the dialogue;
    // every third one is Start instead, which skips a scene (the opening
    // scene in the fight, phase 0/1, only takes Start).
    if ((elapsed / 100) % 8 != 0) {
      continue;
    }
    ++presses;
    const bool start_press = battle != 0 || presses % 3 == 0;
    press(start_press ? 0x0010 : 0x1000, 70);
  }
  g_story_driver.store(false);
}

void StartStory(PPCContext& ctx, uint8_t* base, bool from_menu) {
  Call(ctx, base, sub_82280438);
  Call(ctx, base, sub_82280490);
  Call(ctx, base, sub_82232718, 13);
  W8(base, 0x84248B90 + 62, 0);
  W32(base, kMainMenuCursor, 0);
  Call(ctx, base, sub_82232AD8, 11);
  if (from_menu) {
    Call(ctx, base, sub_8224EE58);
    Call(ctx, base, sub_82120C90);
    Call(ctx, base, sub_8210A5F8);
  }
  W32(base, kMenuFlagsWord, 0);
  g_story_queued.store(true);
  REXLOG_INFO("Quick fight: Z Chronicles episode 1 (Goku vs Raditz): Z Chronicles queued, "
              "confirming its screens");
  rex::FlushLogging();
  if (!g_story_driver.exchange(true)) {
    std::thread(StoryDriver).detach();
  }
}

// Whether `slot` is the only screen up (no window / popup over it).
bool OnlyScreen(uint8_t* base, uint32_t slot) {
  for (uint32_t i = 0; i < kScreenSlotCount; ++i) {
    const uint32_t other = kScreenSlots + i * kScreenSlotStride;
    if (other != slot && R32(base, other) != 0) {
      return false;
    }
  }
  return true;
}

enum class Screen { kBoot, kTitle, kMovie, kMainMenu };
enum class Outcome {
  kNone,     // nothing to do: the screen runs as usual
  kWait,     // a frame without the screen (its movie is stopping)
  kStarted,  // the fight is queued: the screen is done
};

// The screen's own step ([slot + 48]) in which a fight may start from it: the
// title waiting for Start, the opening movie playing, the main menu's list
// taking input (its intro animations have finished). The boot screen: any.
bool ScreenSettled(uint8_t* base, uint32_t slot, Screen screen) {
  const uint32_t step = R32(base, slot + 48);
  switch (screen) {
    case Screen::kBoot:
      return true;
    case Screen::kTitle:
    case Screen::kMovie:
      return step == 2;
    case Screen::kMainMenu:
      return step == 4;
  }
  return false;
}

std::atomic<int> g_logged_wait{-1};

// From a screen's function. The boot screen reads the startup cvar on its
// first frame; before the title's Start (boot, opening movie, title) the
// title's set-up runs too - the main menu already did it (and may have loaded
// a save). Leaving the title / movie / main menu goes like their own exits:
// the menu movie is stopped first (sub_82250C88) and given frames to stop.
Outcome TryStart(PPCContext& ctx, uint8_t* base, uint32_t slot, Screen screen) {
  const bool boot = screen == Screen::kBoot;
  std::optional<Request> request;
  if (boot && !g_boot_checked.exchange(true)) {
    const std::string& spec = REXCVAR_GET(quick_fight);
    if (!spec.empty()) {
      Request parsed;
      std::string error;
      if (ParseRequest(spec, parsed, error)) {
        request = parsed;
      } else {
        REXLOG_ERROR("Quick fight: --quick_fight={}: {}; booting normally", spec, error);
      }
    }
  }
  bool from_pending = false;
  if (!request) {
    if (!g_has_pending.load(std::memory_order_acquire)) {
      return Outcome::kNone;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_pending) {
      return Outcome::kNone;
    }
    request = g_pending;
    from_pending = true;
  }
  const bool story_waits_for_menu =
      request->mode == Mode::kStory && request->story_save && screen != Screen::kMainMenu;
  if (story_waits_for_menu && !g_story_driver.exchange(true)) {
    REXLOG_INFO("Quick fight: Z Chronicles with a save: through the title and the storage "
                "prompt first");
    std::thread(StoryDriver).detach();
  }
  if (rex::net::IsGameSessionOpen() || !OnlyScreen(base, slot) ||
      !ScreenSettled(base, slot, screen) || story_waits_for_menu) {
    // Online, a window over this screen or the screen still coming up: later
    // (a boot request becomes a pending one).
    if (!from_pending) {
      std::lock_guard<std::mutex> lock(g_mutex);
      g_pending = request;
      g_has_pending.store(true, std::memory_order_release);
    }
    const int key = int(screen) * 1000 + int(R32(base, slot + 48) % 1000);
    if (g_logged_wait.exchange(key) != key) {
      REXLOG_INFO("Quick fight: waiting (screen {} step {}{}{})", int(screen),
                  R32(base, slot + 48), OnlyScreen(base, slot) ? "" : ", a window is open",
                  rex::net::IsGameSessionOpen() ? ", online" : "");
    }
    return Outcome::kNone;
  }
  // The game's functions below run on the screen's thread and stack, as if
  // the screen's function had called them.
  if (!boot) {
    if (Call(ctx, base, sub_8224ED70) != 0) {
      Call(ctx, base, sub_82250C88);
      return Outcome::kWait;
    }
    Call(ctx, base, sub_82250C88);
  }
  if (from_pending) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_pending.reset();
    g_has_pending.store(false, std::memory_order_release);
  }
  if (screen != Screen::kMainMenu) {
    TitleSetup(ctx, base);
  } else if (const uint32_t list = Call(ctx, base, sub_82288640, 0)) {
    // The main menu's list window, closed like the menu does when a mode is
    // picked (sub_82251108): the screen manager only tears the screen down,
    // and the list would stay on screen over the fight, holding the input
    // (START wouldn't open the pause menu).
    Call(ctx, base, sub_82289758, list);
  }
  if (request->mode == Mode::kStory) {
    StartStory(ctx, base, !boot);
  } else {
    StartFight(ctx, base, *request, !boot);
  }
  return Outcome::kStarted;
}

// A screen function replaced by one that can start the fight first.
void ScreenWithQuickFight(PPCContext& ctx, uint8_t* base, Screen screen,
                          void (*original)(PPCContext&, uint8_t*)) {
  switch (TryStart(ctx, base, ctx.r3.u32, screen)) {
    case Outcome::kStarted:
      ctx.r3.u64 = 1;
      return;
    case Outcome::kWait:
      ctx.r3.u64 = 0;
      return;
    case Outcome::kNone:
      break;
  }
  original(ctx, base);
}

void QuickFightNow(std::string_view args) {
  const std::string word = Normalize(args);
  if (word == "cancel" || word == "off") {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_pending.reset();
    g_has_pending.store(false);
    REXLOG_WARN("quick_fight_now: cancelled");
    rex::FlushLogging();
    return;
  }
  Request request;
  std::string error;
  if (!ParseRequest(args, request, error)) {
    REXLOG_WARN("quick_fight_now: {} (usage: quick_fight_now [training|versus|versus2p|cpu] "
                "<p1>[:form[:color]] [<p2>[:form[:color]]] [stage])",
                error);
    rex::FlushLogging();
    return;
  }
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_pending = request;
    g_has_pending.store(true, std::memory_order_release);
  }
  REXLOG_WARN("quick_fight_now: armed ({}); starts from the next boot, title or main menu frame",
              args);
  rex::FlushLogging();
}

}  // namespace

REXCVAR_DEFINE_COMMAND_ARGS(quick_fight_now, QuickFightNow, "Debug",
                            "Testing: start a fight from the title or main menu (or cancel): "
                            "[training|versus|versus2p|cpu] <p1>[:form[:color]] "
                            "[<p2>[:form[:color]]] [stage]");

// The start screen (burstlimit_launcher.cpp) isn't shown for a quick fight.
bool BurstLimitQuickFightAtStartup() {
  return !REXCVAR_GET(quick_fight).empty();
}

// The screens a fight can be started from (the screen manager, sub_8223D7A0,
// calls them with r3 = the screen's slot; true in r3 = the screen is done).
extern "C" REX_FUNC(sub_82232B28) {
  ScreenWithQuickFight(ctx, base, Screen::kBoot, __imp__sub_82232B28);
}

extern "C" REX_FUNC(sub_82232E90) {
  ScreenWithQuickFight(ctx, base, Screen::kTitle, __imp__sub_82232E90);
}

extern "C" REX_FUNC(sub_82233420) {
  ScreenWithQuickFight(ctx, base, Screen::kMovie, __imp__sub_82233420);
}

extern "C" REX_FUNC(sub_82251868) {
  ScreenWithQuickFight(ctx, base, Screen::kMainMenu, __imp__sub_82251868);
}
