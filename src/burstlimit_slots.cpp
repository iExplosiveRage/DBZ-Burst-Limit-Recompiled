// Extra character slots: a fighter that is one of the game's characters for
// all of its logic - Ultimate Gohan plays as Goku (id 0): Goku's moves list
// layout, AI tables, saves - but loads its own files: moveset, hand and lip
// poses, model and skeleton, voice, effects, HUD face. A mod brings those
// files under names of its own (code GHL: PAC/CMN/CH/BCGHL/BCGHLM0.AMB, ...)
// and a slot.toml that names them; the mods overlay adds exactly the paths a
// slot.toml names to the archive (burstlimit_mods.cpp):
//
//   [[slot]]
//   code = "GHL"                # 3-letter file code
//   base = 0                    # the character it plays as (0 Goku)
//   name = "Ultimate Gohan"     # on the character select (ASCII)
//   colours = 1                 # costumes (model / skeleton per colour)
//   max_form = 0                # highest form; 0 = it can't transform
//   card = "art/GHL_CARD.png"          # character select card, 512x120 (PNG,
//   portrait = "art/GHL_PORTRAIT.png"  # or a .NUT) and big picture, 368x720;
//                                      # missing: Teen Gohan's are shown
//   tint = 0xFF0A8CDC           # select-screen colour behind the picture
//                               # (optional: the base character's)
//   ultimates = [[85, 1]]       # the Ultimate window, up to 3: [name, the
//                               # base character's ultimate slot 0-2]; name =
//                               # a PROPERNOUN text id (85 Super Kamehameha)
//                               # or an ASCII string ("Vanishing Ball").
//                               # Always selectable (whatever the save has
//                               # unlocked for the base character), with that
//                               # slot's bar cost. None: the base character's
//                               # list
//   dramas = "off"              # Versus rival scenes (the base character's
//                               # lines) when it plays: "off" or "on"
//   [slot.files]                # any left out: the base character's file
//   m0       = "PAC/CMN/CH/BCGHL/BCGHLM0.AMB"    # moveset (and the CPU's script)
//   m1       = "PAC/CMN/CH/BCGHL/BCGHLM1.AMB"    # hand poses
//   l000     = "PAC/CMN/CH/BCGHL/BCGHLL000.AMB"  # lip poses (every form)
//   se_csb   = "SOUND/CSB/CMN/BATTLE_GHL_SE.CSB"
//   vox_csb  = "BATTLE_GHL_VOX.CSB"              # name only: in SOUND/CSB/US/ and /JP/
//   vox_amk  = "PAC/CMN/CH/BCGHL/BATTLE_GHL_VOX.AMK"
//   bsp      = "PAC/CMN/CH/BCGHL/BSP_GHL.AMB"     # effect scripts
//   esp      = "PAC/CMN/CH/BCGHL/ESPGHL.NUT"      # effect textures
//   eas      = "PAC/CMN/CH/BCGHL/EASGHL.NUT"
//   fx_model = "PAC/CMN/CH/BCGHL/GHL_MODEL.NUX"   # effect models
//   arst     = "PAC/CMN/CH/BCGHL/ARST_GHL.NUX"
//   bar      = "PAC/CMN/CH/BCGHL/BAR_GHL.AMB"
//   bufac    = "PAC/CMN/FIX/BUFAC/BUFAC_GHL.NUT"  # battle HUD face
//   model    = ["PAC/CMN/CH/BCGHL/BCGHL0{c}{m}.NUX"]  # {c} colour, {m} model index;
//   skeleton = ["PAC/CMN/CH/BCGHL/BCGHL0{c}{m}.AMB"]  # one entry per colour, or one for all
//
// Character select (Versus and Training, offline): R2 switches a side's list
// to a page with the extra characters - each with its card, big picture and
// name like the others, Y for its colours - and R2 again (or L2) goes back to
// the roster. The page is the side's list in the screen's object, swapped:
// the extra characters are list values 23-31 (21 is RANDOM, 22 the game's
// padding), padded with dark cards up to the 9 the carousel needs, and the
// cursor stops at the last real card. Picking one gives the game the base
// character (Goku) and keeps the slot for that side. When the game builds the
// match request, the choice of both sides is taken for the fight (for a side
// that picked none, the debug setting extra_character_test_player), and
// every per-player file the fight loads is the slot's: the fighter loader
// (sub_82181BB0) takes the character's 980-byte record (0x826BC2A8 + id *
// 980: moveset / hand / lip / voice-key paths, form -> model map, stats) and
// keeps it at [player + 60], where transformations read it too, so a slot
// player gets a copy with its own paths; the costume model and skeleton
// (sub_8218B818, per colour and model index) and the files loaded by id -
// sound banks, effect files, BAR, BSP, the HUD face - are swapped where the
// game passes their path on. Every load is per player (P1 and P2 never share
// a file), so a slot fighter and the real base character can meet - except
// the textures of NUX files, which the game shares by the name written
// inside the file: a slot's NUX files get a name of their own
// (BurstLimitSlotsNuxTextures).
//
// The rest of the fight treats it as its own character: no transformation
// (the request's highest form, also RB / LB's start form), no mirror-colour
// change against the base character, its own Ultimate list, and no Versus
// rival scenes written for the base character. Its name shows on the "Start
// battle with these settings?" window and the battle HUD's life bar, and its
// big picture on the screens after the picks (a NUT made from it, served as
// PAC/CMN/FIX/CHRA/UICMN_<code>.NUT).

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include <toml++/toml.hpp>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/memory.h>
#include <rex/memory/utils.h>
#include <rex/net/session.h>
#include <rex/ppc/context.h>
#include <rex/runtime.h>

#include "burstlimit_cpk.h"

// The pictures' PNG decoder. Static: the runtime and burstlimit_saves.cpp
// have their own copies.
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#define STBI_ONLY_PNG
#define STBI_NO_FAILURE_STRINGS
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#endif
#include <stb_image.h>
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

REXCVAR_DEFINE_BOOL(extra_characters, true, "Patches",
                    "Extra characters defined by the mods in use (slot.toml): R2 on the character "
                    "select shows them - a fighter that plays as one of the game's characters with "
                    "its own moves, model, voice and effects. Versus / Training, offline");

REXCVAR_DEFINE_INT32(extra_character_test_player, -1, "Patches",
                     "Debug: 0 or 1 makes that player (1P / 2P), when it is the slot's base "
                     "character (Goku for Ultimate Gohan) and picked no extra character, load the "
                     "first extra character's files (read when the next match is set up); 2 both "
                     "players; -1 off. Versus / Training, offline")
    .range(-1, 2);

namespace {

// The match request ([0x841B5138]): 0x50 bytes per player, +0 u16 character,
// +4 u16 highest form, +6 u16 / +0x12 u8 start form, +12 s16 colour, +20 six
// u32 drama pieces (1 / 2: the Versus rival scenes, 0: none); the first entry
// has the battle mode at +196 (0 Z Chronicles, 1 Versus, 2 Training). +0x12
// and +0x13 are two bytes (sub_822120A0 copies them from the session's +16 /
// +17): +0x13 is the CPU's AI level (sub_82165260; 255 a player), never
// written here.
constexpr uint32_t kRequestPointer = 0x841B5138;
constexpr uint32_t kRequestStride = 0x50;
constexpr uint32_t kRequestMaxForm = 0x4;
constexpr uint32_t kRequestStartForm = 0x6;
constexpr uint32_t kRequestColour = 0xC;
constexpr uint32_t kRequestStartForm2 = 0x12;  // u8
constexpr uint32_t kRequestDramas = 0x14;
constexpr uint32_t kRequestDramaCount = 6;
constexpr uint32_t kRequestMode = 196;
constexpr uint32_t kModeVersus = 1;
constexpr uint32_t kModeTraining = 2;
constexpr uint32_t kDramaRivalIntro = 1;
constexpr uint32_t kDramaRivalEnd = 2;

// The players (0x841B5EB0 + player * 7316): +38 u16 colour (costume).
constexpr uint32_t kPlayers = 0x841B5EB0;
constexpr uint32_t kPlayerStride = 7316;
constexpr uint32_t kPlayerColour = 38;

// The character records (21 x 980 bytes): +4 form count, +20 moveset, +84
// hand poses, +148 + form * 64 lip poses, +660 SE bank, +724 voice bank (file
// name only), +788 voice keys - 64-byte strings; +852 + form * 16, 8 forms:
// +5 the form's model index.
constexpr uint32_t kRecords = 0x826BC2A8;
constexpr uint32_t kRecordSize = 980;
constexpr uint32_t kCharacters = 21;
constexpr uint32_t kRecordFormCount = 4;
constexpr uint32_t kRecordMoveset = 20;
constexpr uint32_t kRecordHands = 84;
constexpr uint32_t kRecordLips = 148;
constexpr uint32_t kRecordSoundEffects = 660;
constexpr uint32_t kRecordVoice = 724;
constexpr uint32_t kRecordVoiceKeys = 788;
constexpr uint32_t kRecordForms = 852;
constexpr uint32_t kFormStride = 16;
constexpr uint32_t kFormModel = 5;
constexpr uint32_t kForms = 8;
constexpr uint32_t kPathField = 64;
// The game copies at most 57 bytes of a path it loads.
constexpr size_t kMaxPath = 56;
constexpr int kMaxSlots = 10;
constexpr int kMaxColours = 8;
constexpr uint32_t kMaxModels = 8;

// --- The character select (sub_82244F60 sets it up, sub_82249140 runs it,
// sub_822454D0 draws it) ---
//
// Its globals G: +12 / +16 each side's name text, +20 / +22 s16 the
// character shown, +32 the screen's object, +36 / +40 each side's 32 cards
// (8 bytes each, the sprite first).
constexpr uint32_t kSelectGlobals = 0x8424D6E4;
constexpr uint32_t kGlobalsShown = 20;
constexpr uint32_t kGlobalsCards = 36;
constexpr uint32_t kCardRecord = 8;
// The object: +8 its first UI layer, +28 its UI context (+48), per side +12
// cursor, +20 the cursor the carousel shows, +32 + 128 * side the list (32
// values), +288 its length (at least 9), +296 the cards the cursor goes
// through, +304 / +368 + 32 * side locked / NEW bytes, +440 / +448 scroll,
// +464 + 128 * side each card's colour, +720 + value * 4 the colour count of
// a list value (+804..+847, values 22-31, are free), +848 + 28 * side
// RANDOM's state, +904 Trial (side 2 all locked), +905 the picks are kept
// (sub_82244F60's r10: the menu's byte - Versus 0x8424D80D, Training
// 0x8424D7D0 - set once both sides picked and still set when B on the stage
// select, sub_822651D0 -2, brings the select back; each side then goes on
// with its character without a new A).
constexpr uint32_t kObjectLayer = 8;
constexpr uint32_t kObjectContext = 28;
constexpr uint32_t kContextOffset = 48;
constexpr uint32_t kObjectCursor = 12;
constexpr uint32_t kObjectShown = 20;
constexpr uint32_t kObjectList = 32;
constexpr uint32_t kObjectLength = 288;
constexpr uint32_t kObjectCount = 296;
constexpr uint32_t kObjectLocked = 304;
constexpr uint32_t kObjectNew = 368;
constexpr uint32_t kObjectScroll = 440;
constexpr uint32_t kObjectScroll2 = 448;
constexpr uint32_t kObjectColours = 464;
constexpr uint32_t kObjectColourCounts = 720;
constexpr uint32_t kObjectRandom = 848;
constexpr uint32_t kObjectRandomStride = 28;
constexpr uint32_t kObjectTrial = 904;
constexpr uint32_t kObjectKeep = 905;
constexpr uint32_t kListSize = 32;
constexpr uint32_t kListMinimum = 9;
constexpr uint32_t kValueRandom = 21;
constexpr uint32_t kValuePad = 22;
constexpr uint32_t kValueFirst = 23;
constexpr int kPageSlots = 9;  // values 23-31
// The menu the select is part of ([0x826DF520]): Versus 3 (vs CPU), 4 (vs
// 2P), 5 (CPU vs CPU), 6 (Xbox LIVE); Training 12; Trial 7, 20-22.
constexpr uint32_t kMenuMode = 0x826DF520;
constexpr uint32_t kMenuModes[] = {3, 4, 5, 12};
// The menu task's side blocks (0x84249080 + side * 0x60): +8 the step, 1
// while the side picks.
constexpr uint32_t kSideBlocks = 0x84249080;
constexpr uint32_t kSideBlockStride = 0x60;
constexpr uint32_t kSideStep = 8;
constexpr uint32_t kStepPicking = 1;
// The game's buttons: L2 / R2 pulled past 30 (sub_820F7200).
constexpr uint32_t kButtonL2 = 0x10000;
constexpr uint32_t kButtonR2 = 0x20000;
// The game's input per controller (0x8413F5F0 + pad * 212, 4 pads; copied each
// frame by sub_8211DB18, zeroed while not connected): +0 the buttons held
// (sub_820F7200's +28, injected ones too). The select's buttons come from it
// (sub_8227FF20).
constexpr uint32_t kPads = 0x8413F5F0;
constexpr uint32_t kPadStride = 212;
constexpr uint32_t kPadHeld = 0;
constexpr uint32_t kPadCount = 4;
// Teen Gohan's select art stands in: card parts (left / right; locked: the
// dark version) on texture 6 of UICHS_TOP.NUT, big-picture animations (parts
// 275/276 on texture 5).
constexpr uint32_t kStandInCard[2] = {397, 398};
constexpr uint32_t kStandInLockedCard[2] = {441, 442};
constexpr uint16_t kStandInCardTexture = 6;
constexpr uint32_t kStandInPortrait[2] = {1028, 1029};
constexpr uint16_t kStandInPortraitTexture = 5;
// The colour behind the big picture, per character and colour (4).
constexpr uint32_t kTints = 0x826DFE48;
// The UI layers: context + 320 + layer * 128, +36 the animation's sprites;
// each side's big picture plays on the object's first layer + 9 + side.
constexpr uint32_t kLayers = 320;
constexpr uint32_t kLayerStride = 128;
constexpr uint32_t kLayerSprite = 36;
constexpr uint32_t kPortraitLayer = 9;
// A sprite: +40 / +44 the next ones in its tree, +48 its data (+20 u16 the
// texture's index in the screen's NUT).
constexpr uint32_t kSpriteLinks[2] = {40, 44};
constexpr uint32_t kSpriteData = 48;
constexpr uint32_t kDataTexture = 20;
// The pictures appended to UICHS_TOP.NUT: 512x120 cards, 368x720 big
// pictures, DXT5, texture handles (GIDX) from 0x8D00 (the game's UI ones end
// at 0x8Axx).
constexpr uint32_t kCardWidth = 512, kCardHeight = 120;
constexpr uint32_t kPortraitWidth = 368, kPortraitHeight = 720;
constexpr uint32_t kArtHandles = 0x8D00;

// --- After the character select ---
//
// The menu task's side blocks (kSideBlocks): +16 u32 the side's character
// (+100 while RANDOM's result shows).
constexpr uint32_t kSideCharacter = 16;
constexpr uint32_t kRandomShown = 100;
// The screens after the picks (the stage select, the "Start battle with
// these settings?" window) show each side's full picture, which sub_822371B8
// loads per side through sub_82237060: PAC/CMN/FIX/CHRA/UICMN_<code>.NUT +
// _UICMN_<code>.AMA. An extra character with a big picture of its own gets a
// NUT made from it (served under a new name, Teen Gohan's layout: one 368x720
// picture, texture handles from 0x8E00); without one, Teen Gohan's (as on the
// character select).
constexpr const char* kUicmnFolder = "PAC/CMN/FIX/CHRA/";
constexpr const char* kStandInUicmn = "PAC/CMN/FIX/CHRA/UICMN_GHM.NUT";
constexpr const char* kStandInUicmnLayout = "PAC/CMN/FIX/CHRA/_UICMN_GHM.AMA";
constexpr uint32_t kUicmnHandles = 0x8E00;
// The battle HUD's texts (BTL_SYSTEM.NFX) are UTF-16BE characters ended by
// 0x2100 and 0.
constexpr uint16_t kBattleTextEnd = 0x2100;

enum Kind {
  kMoveset,
  kHands,
  kLips,
  kSoundEffects,
  kVoiceKeys,
  kEffectScript,
  kEffectTextures,
  kEffectTextures2,
  kEffectModels,
  kArst,
  kBar,
  kHudFace,
  kKindCount,
};
// slot.toml key and what it is, per kind.
constexpr const char* kKindKeys[kKindCount] = {"m0",  "m1",  "l000",     "se_csb", "vox_amk",
                                               "bsp", "esp", "eas",      "fx_model", "arst",
                                               "bar", "bufac"};
constexpr const char* kKindNames[kKindCount] = {
    "moves",          "hand poses",       "lip poses",     "sound effects",
    "voice keys",     "effect scripts",   "effect textures", "effect textures 2",
    "effect models",  "ARST",             "BAR",           "HUD face"};

struct Slot {
  std::string mod;   // the mod's folder name
  std::string code;  // GHL
  std::string name;  // Ultimate Gohan
  uint32_t base = 0;
  int colours = 1;
  int max_form = 0;
  bool dramas = false;  // the base character's Versus rival scenes
  uint32_t tint = 0;    // 0: the base character's
  std::vector<std::pair<uint16_t, uint16_t>> ultimates;  // (text, slot)
  std::vector<std::string> ultimate_names;  // per entry: "" = its text id
  // The select pictures (host paths; "" = Teen Gohan's) and, once appended to
  // UICHS_TOP.NUT, their texture there (-1: none).
  std::string card_art, portrait_art;
  int card_texture = -1, portrait_texture = -1;
  // Archive paths the archive has ("" = the base character's file).
  std::array<std::string, kKindCount> files;
  std::string voice;  // the voice bank's file name
  bool voice_us = false, voice_jp = false;
  uint32_t models = 1;  // model indices it has per colour
  std::vector<std::vector<std::string>> model, skeleton;  // [colour][model index]
  // In guest memory, made the first time a player loads the slot (kept for
  // good: the game keeps the skeleton's path).
  bool made = false;
  uint32_t record = 0;
  std::array<uint32_t, kKindCount> paths{};
  uint32_t voice_name = 0;
  std::vector<std::vector<uint32_t>> model_paths, skeleton_paths;
  std::set<std::pair<int, uint32_t>> missing_models;  // (colour, model) warned
  // For the character select (made once): its name (UTF-16BE), its tints
  // (4 colours) and its Ultimate list.
  uint32_t name_text = 0, tints = 0, ultimate_list = 0;
  std::vector<uint32_t> ultimate_texts;  // per entry: its name (0: text id)
  // Its name as the battle HUD's text (made once).
  uint32_t hud_name = 0;
  // Its full picture for the screens after the picks: the archive name of
  // the NUT made from its big picture ("" none: Teen Gohan's) and, made once,
  // the paths the game gets (NUT, layout AMA).
  std::string uicmn;
  uint32_t uicmn_nut = 0, uicmn_layout = 0;
};

// One side of the character select while it may show the page.
struct SidePage {
  bool active = false;
  // The roster as the game made it, while the page is shown.
  std::array<uint32_t, kListSize> list{}, colours{};
  std::array<uint8_t, kListSize> locked{}, fresh{};
  uint32_t length = 0, count = 0, cursor = 0, shown = 0;
  uint32_t page_cursor = 0;  // the page's cursor, kept while away
  uint32_t buttons = 0;      // L2 / R2 at the last frame
  int64_t switched_ms = 0;
  // After a switch, none until the triggers are let go (the game repeats a
  // held trigger like a held direction) - on the controllers that held it
  // then (bits 0-3), so another player's trigger doesn't hold this side.
  bool armed = true;
  uint32_t pads = 0;
};

struct Select {
  uint32_t object = 0;  // the screen's object
  bool available = false;
  std::string why;   // why there's no page
  bool told = false;  // ... logged
  std::array<SidePage, 2> sides;
  // The extra character each side picked (-1 none), for the match.
  std::array<int, 2> pick{-1, -1};
  std::array<bool, 2> ultimates_told{};
  // The slot whose own list the side's Ultimate window shows (-1 none).
  std::array<int, 2> ultimate_window{-1, -1};
};

// The extra characters of the fight being set up, taken from the match
// request once, when the game builds it (so all of one load's files come
// from the same choice, whatever changes meanwhile).
struct Fight {
  bool taken = false;
  std::array<int, 2> slot{-1, -1};
};

std::mutex g_mutex;
std::vector<Slot> g_slots;
Select g_select;
Fight g_fight;
// Kept between visits of the character select: each side's last extra
// character (its page opens on it again) and each one's colour.
std::array<int, 2> g_last_pick{-1, -1};
std::array<std::array<uint32_t, kPageSlots>, 2> g_page_colours{};
// The select pictures' build (texture handles change with each).
uint32_t g_art_build = 0;
// The side the next full picture after the picks is loaded for (set by
// BurstLimitSlotsUicmnSide, used once; -1 none).
int g_uicmn_side = -1;

int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

rex::memory::Memory* GuestMemory() {
  auto* runtime = rex::Runtime::instance();
  return runtime ? runtime->memory() : nullptr;
}

uint32_t R32(rex::memory::Memory* memory, uint32_t address) {
  return rex::memory::load_and_swap<uint32_t>(memory->TranslateVirtual<uint8_t*>(address));
}
uint16_t R16(rex::memory::Memory* memory, uint32_t address) {
  return rex::memory::load_and_swap<uint16_t>(memory->TranslateVirtual<uint8_t*>(address));
}
uint8_t R8(rex::memory::Memory* memory, uint32_t address) {
  return *memory->TranslateVirtual<uint8_t*>(address);
}
void W32(rex::memory::Memory* memory, uint32_t address, uint32_t value) {
  rex::memory::store_and_swap<uint32_t>(memory->TranslateVirtual<uint8_t*>(address), value);
}
void W16(rex::memory::Memory* memory, uint32_t address, uint16_t value) {
  rex::memory::store_and_swap<uint16_t>(memory->TranslateVirtual<uint8_t*>(address), value);
}
void W8(rex::memory::Memory* memory, uint32_t address, uint8_t value) {
  *memory->TranslateVirtual<uint8_t*>(address) = value;
}

// The controllers (bit per pad) holding any of `buttons`, as the game sees
// them (L2 / R2: pulled past its 30).
uint32_t PadsHolding(rex::memory::Memory* memory, uint32_t buttons) {
  uint32_t pads = 0;
  for (uint32_t pad = 0; pad < kPadCount; ++pad) {
    if (R32(memory, kPads + pad * kPadStride + kPadHeld) & buttons) {
      pads |= 1u << pad;
    }
  }
  return pads;
}

std::string Upper(std::string text) {
  for (char& c : text) {
    c = c == '\\' ? '/' : char(std::toupper(static_cast<unsigned char>(c)));
  }
  return text;
}

std::string PathText(const std::filesystem::path& path) {
  const std::u8string text = path.generic_u8string();
  return std::string(text.begin(), text.end());
}

std::string GuestString(rex::memory::Memory* memory, uint32_t address, size_t limit) {
  if (!address) {
    return {};
  }
  const char* text = memory->TranslateVirtual<const char*>(address);
  return std::string(text, strnlen(text, limit));
}

// A 64-byte path in guest memory (the size of the record's fields).
uint32_t GuestPath(rex::memory::Memory* memory, const std::string& path) {
  const uint32_t address = memory->SystemHeapAlloc(kPathField);
  if (address) {
    uint8_t* host = memory->TranslateVirtual<uint8_t*>(address);
    std::memset(host, 0, kPathField);
    std::memcpy(host, path.data(), std::min<size_t>(path.size(), kPathField - 1));
  }
  return address;
}

void WriteField(rex::memory::Memory* memory, uint32_t address, const std::string& text) {
  uint8_t* host = memory->TranslateVirtual<uint8_t*>(address);
  std::memset(host, 0, kPathField);
  std::memcpy(host, text.data(), std::min<size_t>(text.size(), kPathField - 1));
}

std::string Expand(std::string pattern, int colour, uint32_t model) {
  const auto replace = [&pattern](const std::string& key, char digit) {
    for (size_t at = pattern.find(key); at != std::string::npos; at = pattern.find(key, at)) {
      pattern.replace(at, key.size(), 1, digit);
    }
  };
  replace("{c}", char('0' + colour));
  replace("{m}", char('0' + model));
  replace("{C}", char('0' + colour));
  replace("{M}", char('0' + model));
  return pattern;
}

// The guest's copy of the slot's record and its paths (once, on the game's
// thread; called with g_mutex held).
bool MakeGuestData(rex::memory::Memory* memory, Slot& slot) {
  if (slot.made) {
    return slot.record != 0;
  }
  slot.made = true;
  const uint32_t base_record = kRecords + slot.base * kRecordSize;
  slot.record = memory->SystemHeapAlloc(kRecordSize);
  if (!slot.record) {
    REXLOG_ERROR("Slots: no guest memory for {}'s character record", slot.code);
    return false;
  }
  uint8_t* record = memory->TranslateVirtual<uint8_t*>(slot.record);
  std::memcpy(record, memory->TranslateVirtual<const uint8_t*>(base_record), kRecordSize);
  for (int kind = 0; kind < kKindCount; ++kind) {
    if (!slot.files[kind].empty()) {
      slot.paths[kind] = GuestPath(memory, slot.files[kind]);
    }
  }
  if (!slot.voice.empty()) {
    slot.voice_name = GuestPath(memory, slot.voice);
  }
  // The paths the record holds (the fighter loader and transformations read
  // them from it).
  if (!slot.files[kMoveset].empty()) {
    WriteField(memory, slot.record + kRecordMoveset, slot.files[kMoveset]);
  }
  if (!slot.files[kHands].empty()) {
    WriteField(memory, slot.record + kRecordHands, slot.files[kHands]);
  }
  if (!slot.files[kLips].empty()) {
    for (uint32_t form = 0; form < kForms; ++form) {
      if (record[kRecordLips + form * kPathField]) {
        WriteField(memory, slot.record + kRecordLips + form * kPathField, slot.files[kLips]);
      }
    }
  }
  if (!slot.files[kSoundEffects].empty()) {
    WriteField(memory, slot.record + kRecordSoundEffects, slot.files[kSoundEffects]);
  }
  if (!slot.voice.empty()) {
    WriteField(memory, slot.record + kRecordVoice, slot.voice);
  }
  if (!slot.files[kVoiceKeys].empty()) {
    WriteField(memory, slot.record + kRecordVoiceKeys, slot.files[kVoiceKeys]);
  }
  // One model for every form: each form shows model 0 (the base character's
  // forms would ask for models the slot doesn't have).
  if (slot.max_form == 0) {
    for (uint32_t form = 0; form < kForms; ++form) {
      record[kRecordForms + form * kFormStride + kFormModel] = 0;
    }
  }
  slot.model_paths.assign(slot.colours, std::vector<uint32_t>(slot.models, 0));
  slot.skeleton_paths.assign(slot.colours, std::vector<uint32_t>(slot.models, 0));
  for (int c = 0; c < slot.colours; ++c) {
    for (uint32_t m = 0; m < slot.models; ++m) {
      if (!slot.model[c][m].empty()) {
        slot.model_paths[c][m] = GuestPath(memory, slot.model[c][m]);
      }
      if (!slot.skeleton[c][m].empty()) {
        slot.skeleton_paths[c][m] = GuestPath(memory, slot.skeleton[c][m]);
      }
    }
  }
  // Forms it can be in whose model it doesn't have: the base character's
  // shows there (Goku's Kaioken / Super Saiyan body on Gohan's moves).
  const uint32_t forms =
      std::min<uint32_t>(R16(memory, base_record + kRecordFormCount), uint32_t(slot.max_form) + 1);
  for (uint32_t form = 0; form < std::min(forms, kForms); ++form) {
    const int8_t model = int8_t(record[kRecordForms + form * kFormStride + kFormModel]);
    if (model < 0) {
      continue;
    }
    if (uint32_t(model) >= slot.models) {
      REXLOG_WARN("Slots: {} form {} shows model {}, which it doesn't have (models 0-{}) - the "
                  "base character's model is shown in that form",
                  slot.code, form, model, slot.models - 1);
      continue;
    }
    for (int c = 0; c < slot.colours; ++c) {
      if (slot.model[c][model].empty() || slot.skeleton[c][model].empty()) {
        REXLOG_WARN("Slots: {} form {} shows model {}, but colour {} has no {} for it - the base "
                    "character's is shown in that form",
                    slot.code, form, model, c,
                    slot.model[c][model].empty() ? "model (NUX)" : "skeleton (AMB)");
      }
    }
  }
  REXLOG_INFO("Slots: {} ready in guest memory (record {:08X})", slot.code, slot.record);
  return true;
}

// The character select's data of a slot (once): its name as the game's text
// (UTF-16 big-endian), its tints and its Ultimate list
// ({u16 count, (u16 text, u16 slot) x 3}).
// An ASCII string as the game's text (UTF-16BE) in guest memory (0: none).
// '%' is left out: the game formats some texts with them (sub_8228F750).
uint32_t MakeText(rex::memory::Memory* memory, const std::string& text) {
  const uint32_t length = uint32_t(std::min<size_t>(text.size(), 64));
  const uint32_t address = memory->SystemHeapAlloc((length + 1) * 2);
  if (address) {
    for (uint32_t c = 0; c < length; ++c) {
      const uint8_t ch = uint8_t(text[c]);
      W16(memory, address + c * 2, ch >= 0x20 && ch < 0x7F && ch != '%' ? ch : '?');
    }
    W16(memory, address + length * 2, 0);
  }
  return address;
}

void MakeSelectData(rex::memory::Memory* memory, Slot& slot) {
  if (!slot.name_text) {
    slot.name_text = MakeText(memory, slot.name);
  }
  if (slot.ultimate_texts.size() != slot.ultimates.size()) {
    slot.ultimate_texts.assign(slot.ultimates.size(), 0);
    for (size_t i = 0; i < slot.ultimates.size(); ++i) {
      if (i < slot.ultimate_names.size() && !slot.ultimate_names[i].empty()) {
        slot.ultimate_texts[i] = MakeText(memory, slot.ultimate_names[i]);
      }
    }
  }
  if (!slot.tints) {
    slot.tints = memory->SystemHeapAlloc(16);
    if (slot.tints) {
      for (uint32_t c = 0; c < 4; ++c) {
        W32(memory, slot.tints + c * 4,
            slot.tint ? slot.tint : R32(memory, kTints + (slot.base * 4 + c) * 4));
      }
    }
  }
  if (!slot.ultimate_list && !slot.ultimates.empty()) {
    slot.ultimate_list = memory->SystemHeapAlloc(16);
    if (slot.ultimate_list) {
      const uint32_t count = uint32_t(std::min<size_t>(slot.ultimates.size(), 3));
      W16(memory, slot.ultimate_list, uint16_t(count));
      // The window checks 3 entries' slots whatever the count: the rest repeat
      // the first.
      for (uint32_t i = 0; i < 3; ++i) {
        const auto& entry = slot.ultimates[i < count ? i : 0];
        W16(memory, slot.ultimate_list + 2 + i * 4, entry.first);
        W16(memory, slot.ultimate_list + 4 + i * 4, entry.second);
      }
    }
  }
}

// --- The fight ---

// Takes the extra characters of the fight from the match request the game
// just built (request mode, characters) and the character select's picks.
// Called with g_mutex held.
void TakeFight(rex::memory::Memory* memory, uint32_t request) {
  g_fight.taken = true;
  g_fight.slot = {-1, -1};
  // Read once per fight, here.
  const int test = REXCVAR_GET(extra_character_test_player);
  std::array<int, 2> want = g_select.pick;
  std::array<bool, 2> from_test{};
  for (int p = 0; p < 2; ++p) {
    if (want[p] < 0 && (test == p || test == 2)) {
      want[p] = 0;
      from_test[p] = true;
    }
  }
  if (want[0] < 0 && want[1] < 0) {
    return;
  }
  std::string why;
  const uint32_t mode = R32(memory, request + kRequestMode);
  if (!REXCVAR_GET(extra_characters)) {
    why = "extra_characters is off";
  } else if (g_slots.empty()) {
    why = "no extra character defined (a mod with a slot.toml, switched on at start)";
  } else if (rex::net::IsGameSessionOpen()) {
    why = "online";
  } else if (mode != kModeVersus && mode != kModeTraining) {
    why = "battle mode " + std::to_string(mode) + " (only Versus 1 / Training 2)";
  }
  for (int p = 0; p < 2; ++p) {
    const int index = want[p];
    if (index < 0) {
      continue;
    }
    const char* source = from_test[p] ? "extra_character_test_player" : "the character select";
    if (!why.empty()) {
      REXLOG_INFO("Slots: player {} ({}P) plays its own character ({}: {})", p, p + 1, source,
                  why);
      continue;
    }
    if (index >= int(g_slots.size())) {
      continue;
    }
    const Slot& slot = g_slots[index];
    const uint16_t character = R16(memory, request + uint32_t(p) * kRequestStride);
    if (character != slot.base) {
      REXLOG_INFO("Slots: player {} ({}P) plays its own character ({}: it is character {}, not "
                  "{}'s base {})",
                  p, p + 1, source, character, slot.code, slot.base);
      continue;
    }
    g_fight.slot[p] = index;
    REXLOG_INFO("Slots: player {} ({}P) is {} ({}) this match, from {} (mode {})", p, p + 1,
                slot.name, slot.code, source, mode);
  }
}

// The slot `player` loads in the fight being loaded, or -1 (`why`, when not
// null, gets the reason a slot taken for it isn't used). Called with g_mutex
// held.
int SlotForPlayer(rex::memory::Memory* memory, uint32_t player, std::string* why) {
  if (player > 1 || !memory) {
    return -1;
  }
  const uint32_t request = R32(memory, kRequestPointer);
  if (!g_fight.taken && request) {
    TakeFight(memory, request);  // (a load without a request built since start)
  }
  const int index = g_fight.slot[player];
  if (index < 0 || index >= int(g_slots.size())) {
    return -1;
  }
  const auto refuse = [why](std::string reason) {
    if (why) {
      *why = std::move(reason);
    }
    return -1;
  };
  // The same request, still: a Z Chronicles battle after a Versus match
  // with one keeps its own characters.
  if (!REXCVAR_GET(extra_characters)) {
    return refuse("extra_characters is off");
  }
  if (rex::net::IsGameSessionOpen()) {
    return refuse("online");
  }
  if (!request) {
    return refuse("no match request");
  }
  const uint32_t mode = R32(memory, request + kRequestMode);
  if (mode != kModeVersus && mode != kModeTraining) {
    return refuse("battle mode " + std::to_string(mode) + " (only Versus 1 / Training 2)");
  }
  const uint16_t character = R16(memory, request + player * kRequestStride);
  if (character != g_slots[index].base) {
    return refuse("it is character " + std::to_string(character) + ", not " +
                  std::to_string(g_slots[index].base) + " (" + g_slots[index].code + "'s base)");
  }
  return index;
}

// A path the game is about to load for `player` (in `path`): the slot's own
// file of that kind, when the player loads a slot and the slot has one.
void SwapPath(PPCRegister& path, uint32_t player, Kind kind) {
  auto* memory = GuestMemory();
  if (!memory || player > 1) {
    return;
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  const int index = SlotForPlayer(memory, player, nullptr);
  if (index < 0) {
    return;
  }
  Slot& slot = g_slots[index];
  if (!MakeGuestData(memory, slot) || !slot.paths[kind]) {
    return;
  }
  const std::string from = GuestString(memory, path.u32, kPathField);
  path.u64 = slot.paths[kind];
  REXLOG_INFO("Slots: player {} ({}P) loads {} {}: {} (instead of {})", player, player + 1,
              slot.code, kKindNames[kind], slot.files[kind], from);
}

// The slot's name as the battle HUD's text (once; 0 when there's no memory).
uint32_t HudName(rex::memory::Memory* memory, Slot& slot) {
  if (!slot.hud_name) {
    const uint32_t length = uint32_t(std::min<size_t>(slot.name.size(), 64));
    slot.hud_name = memory->SystemHeapAlloc((length + 2) * 2);
    if (slot.hud_name) {
      for (uint32_t c = 0; c < length; ++c) {
        const uint8_t ch = uint8_t(slot.name[c]);
        W16(memory, slot.hud_name + c * 2, ch >= 0x20 && ch < 0x7F ? ch : '?');
      }
      W16(memory, slot.hud_name + length * 2, kBattleTextEnd);
      W16(memory, slot.hud_name + (length + 1) * 2, 0);
    }
  }
  return slot.hud_name;
}

// --- After the character select ---

// The extra character `side` picked on the character select, for the screens
// after it (Versus / Training, offline), while that side still has its base
// character; -1 none. Called with g_mutex held.
int PickedSlot(rex::memory::Memory* memory, uint32_t side) {
  if (side > 1 || !memory || !REXCVAR_GET(extra_characters) || rex::net::IsGameSessionOpen()) {
    return -1;
  }
  const int index = g_select.pick[side];
  if (index < 0 || index >= int(g_slots.size())) {
    return -1;
  }
  const uint32_t menu = R32(memory, kMenuMode);
  if (std::find(std::begin(kMenuModes), std::end(kMenuModes), menu) == std::end(kMenuModes)) {
    return -1;
  }
  uint32_t character = R32(memory, kSideBlocks + side * kSideBlockStride + kSideCharacter);
  if (character >= kRandomShown) {
    character -= kRandomShown;
  }
  return character == g_slots[index].base ? index : -1;
}

// The side of a side block + 16 (r29 / r31 in the screens after the picks),
// or -1.
int SideOfCharacterField(uint32_t address) {
  const uint32_t first = kSideBlocks + kSideCharacter;
  if (address < first || (address - first) % kSideBlockStride != 0 ||
      (address - first) / kSideBlockStride > 1) {
    return -1;
  }
  return int((address - first) / kSideBlockStride);
}

// --- The character select ---

int PageSlots() {
  return std::min<int>(int(g_slots.size()), kPageSlots);
}

// The slot a list value stands for on the page, or -1.
int SlotOfValue(uint32_t value) {
  return value >= kValueFirst && value < kValueFirst + uint32_t(PageSlots())
             ? int(value - kValueFirst)
             : -1;
}

bool PageShown(uint32_t side) {
  return side <= 1 && g_select.sides[side].active;
}

uint32_t ListAddress(uint32_t object, uint32_t side, uint32_t index) {
  return object + kObjectList + side * kListSize * 4 + index * 4;
}
uint32_t ColourAddress(uint32_t object, uint32_t side, uint32_t index) {
  return object + kObjectColours + side * kListSize * 4 + index * 4;
}

// The side's list, swapped for the page (its roster kept to come back to).
void ShowPage(rex::memory::Memory* memory, uint32_t object, uint32_t side) {
  SidePage& page = g_select.sides[side];
  for (uint32_t i = 0; i < kListSize; ++i) {
    page.list[i] = R32(memory, ListAddress(object, side, i));
    page.colours[i] = R32(memory, ColourAddress(object, side, i));
    page.locked[i] = R8(memory, object + kObjectLocked + side * kListSize + i);
    page.fresh[i] = R8(memory, object + kObjectNew + side * kListSize + i);
  }
  page.length = R32(memory, object + kObjectLength + side * 4);
  page.count = R32(memory, object + kObjectCount + side * 4);
  page.cursor = R32(memory, object + kObjectCursor + side * 4);
  page.shown = R32(memory, object + kObjectShown + side * 4);
  // The extra characters, then dark cards up to the carousel's 9 (the cursor
  // goes through the real ones only).
  const uint32_t count = uint32_t(PageSlots());
  const uint32_t length = std::max(count, kListMinimum);
  for (uint32_t i = 0; i < kListSize; ++i) {
    const bool real = i < count;
    W32(memory, ListAddress(object, side, i), real ? kValueFirst + i : kValuePad);
    W32(memory, ColourAddress(object, side, i),
        real ? g_page_colours[side][i] % uint32_t(g_slots[i].colours) : 0);
    W8(memory, object + kObjectLocked + side * kListSize + i, real ? 0 : 1);
    W8(memory, object + kObjectNew + side * kListSize + i, 0);
  }
  W32(memory, object + kObjectLength + side * 4, length);
  W32(memory, object + kObjectCount + side * 4, count);
  const uint32_t cursor = std::min(page.page_cursor, count - 1);
  W32(memory, object + kObjectCursor + side * 4, cursor);
  W32(memory, object + kObjectShown + side * 4, cursor);
  page.active = true;
}

// Back to the side's roster as it was.
void ShowRoster(rex::memory::Memory* memory, uint32_t object, uint32_t side) {
  SidePage& page = g_select.sides[side];
  const uint32_t count = uint32_t(PageSlots());
  page.page_cursor = std::min(R32(memory, object + kObjectCursor + side * 4), count - 1);
  for (uint32_t i = 0; i < count; ++i) {
    g_page_colours[side][i] = R32(memory, ColourAddress(object, side, i));
  }
  for (uint32_t i = 0; i < kListSize; ++i) {
    W32(memory, ListAddress(object, side, i), page.list[i]);
    W32(memory, ColourAddress(object, side, i), page.colours[i]);
    W8(memory, object + kObjectLocked + side * kListSize + i, page.locked[i]);
    W8(memory, object + kObjectNew + side * kListSize + i, page.fresh[i]);
  }
  W32(memory, object + kObjectLength + side * 4, page.length);
  W32(memory, object + kObjectCount + side * 4, page.count);
  W32(memory, object + kObjectCursor + side * 4, page.cursor);
  W32(memory, object + kObjectShown + side * 4, page.shown);
  page.active = false;
}

// Whether guest memory can be read there (committed).
bool Readable(rex::memory::Memory* memory, uint32_t address, uint32_t size) {
  if (!address) {
    return false;
  }
  auto* heap = memory->LookupHeap(address);
  rex::memory::HeapAllocationInfo info = {};
  return heap && heap->QueryRegionInfo(address, &info) &&
         (info.state & rex::memory::kMemoryAllocationCommit) &&
         uint64_t(address) + size <= uint64_t(info.base_address) + info.region_size;
}

// A sprite showing texture `from` shows `to` instead.
void Retarget(rex::memory::Memory* memory, uint32_t sprite, uint16_t from, uint16_t to) {
  if (!Readable(memory, sprite, kSpriteData + 4)) {
    return;
  }
  const uint32_t data = R32(memory, sprite + kSpriteData);
  if (Readable(memory, data, kDataTexture + 2) && R16(memory, data + kDataTexture) == from) {
    W16(memory, data + kDataTexture, to);
  }
}

// The same for a whole animation's sprites (a bounded walk of its tree).
void RetargetTree(rex::memory::Memory* memory, uint32_t root, uint16_t from, uint16_t to) {
  std::vector<uint32_t> stack;
  if (root) {
    stack.push_back(root);
  }
  for (int visited = 0; !stack.empty() && visited < 256; ++visited) {
    const uint32_t sprite = stack.back();
    stack.pop_back();
    if (!Readable(memory, sprite, kSpriteData + 4)) {
      continue;
    }
    Retarget(memory, sprite, from, to);
    for (uint32_t link : kSpriteLinks) {
      const uint32_t next = R32(memory, sprite + link);
      if (next && next != sprite) {
        stack.push_back(next);
      }
    }
  }
}

// --- slot.toml ---

// slot.toml string value, "" when missing.
std::string TomlString(const toml::table& table, const char* key) {
  return table[key].value_or(std::string());
}

// A key's paths: one string or an array of them.
std::vector<std::string> TomlPaths(const toml::table& table, const char* key) {
  std::vector<std::string> list;
  if (const toml::array* array = table[key].as_array()) {
    for (const toml::node& node : *array) {
      if (auto text = node.value<std::string>()) {
        list.push_back(*text);
      }
    }
  } else if (auto text = table[key].value<std::string>()) {
    list.push_back(*text);
  }
  return list;
}

// The voice bank's file name (vox_csb may hold a path).
std::string VoiceName(const std::string& voice) {
  const size_t slash = voice.find_last_of("/\\");
  return slash == std::string::npos ? voice : voice.substr(slash + 1);
}

bool ParseSlotFile(const std::filesystem::path& folder, const std::string& mod,
                   toml::table& table) {
  std::ifstream file(folder / "slot.toml", std::ios::binary);
  if (!file) {
    return false;
  }
  std::stringstream text;
  text << file.rdbuf();
  try {
    table = toml::parse(text.str());
  } catch (const toml::parse_error& error) {
    REXLOG_ERROR("Slots: {}'s slot.toml: {} (line {})", mod, error.description(),
                 error.source().begin.line);
    return false;
  }
  return true;
}

// A picture slot.toml names: the file in the mod folder, "" when none or
// missing (with a message).
std::string ArtPath(const toml::table& table, const char* key, const std::filesystem::path& folder,
                    const char* what, const std::string& code) {
  const std::string relative = TomlString(table, key);
  if (relative.empty()) {
    REXLOG_INFO("Slots:   {} {}: none - Teen Gohan's is shown", code, what);
    return {};
  }
  std::error_code ec;
  const std::filesystem::path path = folder / std::filesystem::path(std::u8string(
                                                  relative.begin(), relative.end()));
  if (!std::filesystem::is_regular_file(path, ec)) {
    REXLOG_WARN("Slots:   {} {} {} isn't in the mod - Teen Gohan's is shown", code, what,
                relative);
    return {};
  }
  REXLOG_INFO("Slots:   {} {}: {}", code, what, relative);
  return PathText(path);
}

// One [[slot]] of a slot.toml; false (with a message logged) when unusable.
bool ReadSlot(const toml::table& table, const std::filesystem::path& folder, const std::string& mod,
              const std::function<bool(const std::string&)>& served, Slot& slot) {
  slot.mod = mod;
  slot.code = Upper(TomlString(table, "code"));
  slot.name = TomlString(table, "name");
  const int64_t base = table["base"].value_or(int64_t(0));
  const int64_t colours =
      table["colours"].value_or(table["colors"].value_or(int64_t(1)));
  slot.max_form = int(table["max_form"].value_or(int64_t(0)));
  if (slot.code.size() != 3 || base < 0 || base >= kCharacters || colours < 1 ||
      colours > kMaxColours || slot.max_form < 0) {
    REXLOG_ERROR("Slots: {}'s slot.toml: a slot needs a 3-letter code, a base character 0-20 and "
                 "1-{} colours",
                 mod, kMaxColours);
    return false;
  }
  slot.base = uint32_t(base);
  slot.colours = int(colours);
  if (slot.name.empty()) {
    slot.name = slot.code;
  }
  const std::string dramas = TomlString(table, "dramas");
  slot.dramas = dramas == "on" || dramas == "base";
  slot.tint = uint32_t(table["tint"].value_or(int64_t(0)));
  if (const toml::array* ultimates = table["ultimates"].as_array()) {
    for (const toml::node& node : *ultimates) {
      const toml::array* pair = node.as_array();
      if (!pair || pair->size() != 2) {
        continue;
      }
      // [text id, slot] or ["Name", slot]: a name is shown instead of a
      // text (the list keeps a valid id, 84, under it).
      std::string label;
      int64_t text = -1;
      if (const toml::value<std::string>* string = (*pair)[0].as_string()) {
        label = string->get();
        text = label.empty() ? -1 : 84;
      } else {
        text = (*pair)[0].value_or(int64_t(-1));
      }
      const int64_t ultimate = (*pair)[1].value_or(int64_t(-1));
      if (text >= 0 && text < 0x10000 && ultimate >= 0 && ultimate < 8) {
        slot.ultimates.push_back({uint16_t(text), uint16_t(ultimate)});
        slot.ultimate_names.push_back(label);
      } else {
        REXLOG_WARN("Slots: {}'s slot {}: an Ultimate entry isn't [text id or \"name\", "
                    "slot 0-7] - left out",
                    mod, slot.code);
      }
    }
  }
  const toml::table* files = table["files"].as_table();
  if (!files) {
    REXLOG_ERROR("Slots: {}'s slot {} has no [slot.files]", mod, slot.code);
    return false;
  }
  // A path the game can be given: short enough, and in the archive (the
  // game's own files or the names mods add).
  const auto usable = [&](const std::string& path, const char* what) {
    if (path.size() > kMaxPath) {
      REXLOG_WARN("Slots:   {} {} is longer than {} characters - the base character's is used",
                  what, path, kMaxPath);
      return false;
    }
    if (!served(path)) {
      REXLOG_WARN("Slots:   {} {} isn't in the archive - the base character's is used", what,
                  path);
      return false;
    }
    return true;
  };
  REXLOG_INFO("Slots: {} \"{}\" from mod {}: plays as character {}, {} colour(s), forms up to {}, "
              "{} Ultimate(s) of its own, rival scenes {}",
              slot.code, slot.name, mod, slot.base, slot.colours, slot.max_form,
              slot.ultimates.size(), slot.dramas ? "on" : "off");
  int own = 0;
  for (int kind = 0; kind < kKindCount; ++kind) {
    const std::string path = TomlString(*files, kKindKeys[kind]);
    if (!path.empty() && usable(path, kKindNames[kind])) {
      slot.files[kind] = path;
      ++own;
      REXLOG_INFO("Slots:   {}: {}", kKindNames[kind], path);
    }
  }
  const std::string voice = TomlString(*files, "vox_csb");
  if (!voice.empty()) {
    const std::string name = VoiceName(voice);
    slot.voice_us = served("SOUND/CSB/US/" + name);
    slot.voice_jp = served("SOUND/CSB/JP/" + name);
    if (name.size() + 13 > kMaxPath || (!slot.voice_us && !slot.voice_jp)) {
      REXLOG_WARN("Slots:   voice {} isn't in SOUND/CSB/US/ or /JP/ - the base character's is used",
                  name);
    } else {
      slot.voice = name;
      ++own;
      REXLOG_INFO("Slots:   voice: {} ({}{})", name, slot.voice_us ? "US" : "",
                  slot.voice_jp ? (slot.voice_us ? " + JP" : "JP") : "");
    }
  }
  // Model indices: one model when it has no forms of its own, else the base
  // character's (its forms -> models map is kept).
  slot.models = 1;
  if (slot.max_form > 0) {
    slot.models = kMaxModels;
  }
  const std::vector<std::string> models = TomlPaths(*files, "model");
  const std::vector<std::string> skeletons = TomlPaths(*files, "skeleton");
  slot.model.assign(slot.colours, std::vector<std::string>(slot.models));
  slot.skeleton.assign(slot.colours, std::vector<std::string>(slot.models));
  for (int c = 0; c < slot.colours; ++c) {
    for (uint32_t m = 0; m < slot.models; ++m) {
      const auto pick = [&](const std::vector<std::string>& list, const char* what,
                            std::vector<std::string>& paths) {
        std::string& out = paths[m];
        if (list.empty()) {
          return;
        }
        const std::string path = Expand(list[size_t(c) < list.size() ? c : 0], c, m);
        // Other model indices: the same file when the pattern has no {m},
        // else only probed (quietly).
        if (m > 0 && path == Expand(list[size_t(c) < list.size() ? c : 0], c, 0)) {
          out = paths[0];
          return;
        }
        if (m > 0 && !served(path)) {
          return;
        }
        if (usable(path, what)) {
          out = path;
          ++own;
          REXLOG_INFO("Slots:   {} (colour {}, model {}): {}", what, c, m, path);
        }
      };
      pick(models, "model", slot.model[c]);
      pick(skeletons, "skeleton", slot.skeleton[c]);
    }
  }
  if (!own) {
    REXLOG_WARN("Slots: {} has none of its own files in the archive - not used", slot.code);
    return false;
  }
  slot.card_art = ArtPath(table, "card", folder, "card", slot.code);
  slot.portrait_art = ArtPath(table, "portrait", folder, "big picture", slot.code);
  return true;
}

// --- The select pictures: textures appended to UICHS_TOP.NUT ---

// A DXT5 texture entry of a NUT (NTXR v1, as the game's own: a 0x50-byte
// header with its handle, then the blocks as big-endian 16-bit words) from
// RGBA pixels.
std::vector<uint8_t> NutEntry(const std::vector<uint8_t>& rgba, uint32_t width, uint32_t height,
                              uint32_t handle) {
  const uint32_t blocks_x = (width + 3) / 4, blocks_y = (height + 3) / 4;
  const uint32_t data_size = blocks_x * blocks_y * 16;
  constexpr uint32_t kHeader = 0x50;
  std::vector<uint8_t> entry(kHeader + data_size, 0);
  const auto be32 = [&entry](size_t at, uint32_t value) {
    for (int i = 0; i < 4; ++i) {
      entry[at + i] = uint8_t(value >> (24 - 8 * i));
    }
  };
  const auto be16 = [&entry](size_t at, uint16_t value) {
    entry[at] = uint8_t(value >> 8);
    entry[at + 1] = uint8_t(value);
  };
  be32(0x00, kHeader + data_size);
  be32(0x08, data_size);
  be16(0x0C, kHeader);
  be16(0x10, 1);  // one level
  be16(0x12, 2);  // DXT5
  be16(0x14, uint16_t(width));
  be16(0x16, uint16_t(height));
  static constexpr uint8_t kTags[0x20] = {'e', 'X', 't', 0, 0, 0, 0, 0x20, 0, 0, 0, 0x10, 0, 0, 0, 0,
                                          'G', 'I', 'D', 'X', 0, 0, 0, 0x10, 0, 0, 0, 0,    0, 0, 0, 0};
  std::memcpy(&entry[0x30], kTags, sizeof(kTags));
  be32(0x48, handle);
  uint8_t pixels[16][4];
  uint8_t block[16];
  for (uint32_t by = 0; by < blocks_y; ++by) {
    for (uint32_t bx = 0; bx < blocks_x; ++bx) {
      for (uint32_t p = 0; p < 16; ++p) {
        const uint32_t x = std::min(bx * 4 + p % 4, width - 1);
        const uint32_t y = std::min(by * 4 + p / 4, height - 1);
        std::memcpy(pixels[p], &rgba[(size_t(y) * width + x) * 4], 4);
      }
      BurstLimitEncodeDxt5Block(pixels, block);
      uint8_t* out = &entry[kHeader + (size_t(by) * blocks_x + bx) * 16];
      for (int i = 0; i < 16; i += 2) {
        out[i] = block[i + 1];
        out[i + 1] = block[i];
      }
    }
  }
  return entry;
}

// RGBA pixels resized (bilinear) to width x height.
std::vector<uint8_t> Resize(const std::vector<uint8_t>& rgba, uint32_t w, uint32_t h,
                            uint32_t width, uint32_t height) {
  std::vector<uint8_t> out(size_t(width) * height * 4);
  for (uint32_t y = 0; y < height; ++y) {
    const float fy = std::max(0.0f, (float(y) + 0.5f) * float(h) / float(height) - 0.5f);
    const uint32_t y0 = std::min(uint32_t(fy), h - 1), y1 = std::min(y0 + 1, h - 1);
    const float ty = fy - float(y0);
    for (uint32_t x = 0; x < width; ++x) {
      const float fx = std::max(0.0f, (float(x) + 0.5f) * float(w) / float(width) - 0.5f);
      const uint32_t x0 = std::min(uint32_t(fx), w - 1), x1 = std::min(x0 + 1, w - 1);
      const float tx = fx - float(x0);
      for (int c = 0; c < 4; ++c) {
        const auto at = [&](uint32_t px, uint32_t py) {
          return float(rgba[(size_t(py) * w + px) * 4 + c]);
        };
        const float top = at(x0, y0) + (at(x1, y0) - at(x0, y0)) * tx;
        const float bottom = at(x0, y1) + (at(x1, y1) - at(x0, y1)) * tx;
        out[(size_t(y) * width + x) * 4 + c] = uint8_t(std::clamp(top + (bottom - top) * ty + 0.5f,
                                                                  0.0f, 255.0f));
      }
    }
  }
  return out;
}

// One picture as a texture entry: a PNG (encoded here, resized to the size
// the game's sprites expect) or a NUT (its first texture, as it is).
bool ArtEntry(const std::string& path, uint32_t width, uint32_t height, uint32_t handle,
              std::vector<uint8_t>& entry) {
  std::ifstream file(std::filesystem::path(std::u8string(path.begin(), path.end())),
                     std::ios::binary);
  std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)),
                             std::istreambuf_iterator<char>());
  if (bytes.size() >= 20 && std::memcmp(bytes.data(), "NTXR", 4) == 0) {
    const uint32_t size = uint32_t(bytes[16]) << 24 | uint32_t(bytes[17]) << 16 |
                          uint32_t(bytes[18]) << 8 | bytes[19];
    if (size < 0x50 || 16 + size_t(size) > bytes.size()) {
      REXLOG_WARN("Slots: {} isn't a NUT it can read", path);
      return false;
    }
    entry.assign(bytes.begin() + 16, bytes.begin() + 16 + size);
    // Its texture handle (GIDX, the end of the header after eXt: 0x48 with
    // one level) is the one given to this picture, not the NUT's own - which
    // may be one of the game's.
    const uint32_t header = uint32_t(entry[0x0C]) << 8 | entry[0x0D];
    if (header >= 0x50 && header <= entry.size() &&
        std::memcmp(&entry[header - 0x30], "eXt", 4) == 0 &&
        std::memcmp(&entry[header - 0x10], "GIDX", 4) == 0) {
      for (uint32_t i = 0; i < 4; ++i) {
        entry[header - 8 + i] = uint8_t(handle >> (24 - 8 * i));
      }
    } else {
      REXLOG_WARN("Slots: {} has no texture handle (eXt / GIDX) - used as it is", path);
    }
    const uint32_t w = uint32_t(entry[0x14]) << 8 | entry[0x15];
    const uint32_t h = uint32_t(entry[0x16]) << 8 | entry[0x17];
    if (w != width || h != height) {
      REXLOG_WARN("Slots: {} is {}x{}, the game shows {}x{} of it", path, w, h, width, height);
    }
    return true;
  }
  int w = 0, h = 0, channels = 0;
  stbi_uc* pixels = bytes.empty() ? nullptr
                                  : stbi_load_from_memory(bytes.data(), int(bytes.size()), &w, &h,
                                                          &channels, 4);
  if (!pixels || w <= 0 || h <= 0) {
    if (pixels) {
      stbi_image_free(pixels);
    }
    REXLOG_WARN("Slots: {} isn't a PNG (or NUT) it can read", path);
    return false;
  }
  std::vector<uint8_t> rgba(pixels, pixels + size_t(w) * size_t(h) * 4);
  stbi_image_free(pixels);
  if (uint32_t(w) != width || uint32_t(h) != height) {
    REXLOG_WARN("Slots: {} is {}x{} - resized to the {}x{} the game shows", path, w, h, width,
                height);
    rgba = Resize(rgba, uint32_t(w), uint32_t(h), width, height);
  }
  entry = NutEntry(rgba, width, height, handle);
  return true;
}

}  // namespace

// From burstlimit_mods.cpp, while it reads the mods: what a mod's slot.toml
// names - the archive paths of its files (upper case, '/'; "{C}" and "{M}"
// stand for a digit), which may be added to the archive as new names, and the
// mod's own files it reads that aren't game files (the pictures; relative to
// the mod's folder, upper case, '/').
void BurstLimitSlotsDeclared(const std::filesystem::path& folder,
                             std::vector<std::string>& archive, std::vector<std::string>& other) {
  archive.clear();
  other.clear();
  std::error_code ec;
  if (!std::filesystem::is_regular_file(folder / "slot.toml", ec)) {
    return;
  }
  const std::u8string folder_name = folder.filename().u8string();
  toml::table table;
  if (!ParseSlotFile(folder, std::string(folder_name.begin(), folder_name.end()), table)) {
    return;
  }
  const toml::array* list = table["slot"].as_array();
  if (!list) {
    return;
  }
  for (const toml::node& node : *list) {
    const toml::table* entry = node.as_table();
    if (!entry) {
      continue;
    }
    for (const char* key : {"card", "portrait"}) {
      const std::string path = TomlString(*entry, key);
      if (!path.empty()) {
        other.push_back(Upper(path));
      }
    }
    const toml::table* files = (*entry)["files"].as_table();
    if (!files) {
      continue;
    }
    for (const char* key : kKindKeys) {
      for (const std::string& path : TomlPaths(*files, key)) {
        archive.push_back(Upper(path));
      }
    }
    for (const char* key : {"model", "skeleton"}) {
      for (const std::string& path : TomlPaths(*files, key)) {
        archive.push_back(Upper(path));
      }
    }
    const std::string voice = TomlString(*files, "vox_csb");
    if (!voice.empty()) {
      archive.push_back("SOUND/CSB/US/" + Upper(VoiceName(voice)));
      archive.push_back("SOUND/CSB/JP/" + Upper(VoiceName(voice)));
    }
  }
}

// From burstlimit_mods.cpp, before the game reads the archive (at start, and
// when the start screen switches mods): the slots of the mods in use, in their
// order (mod folders alphabetically, slots as listed).
void BurstLimitSlotsReload(const std::vector<std::filesystem::path>& folders,
                           const std::function<bool(const std::string&)>& served) {
  std::vector<Slot> slots;
  for (const std::filesystem::path& folder : folders) {
    const std::u8string folder_name = folder.filename().u8string();
    const std::string mod(folder_name.begin(), folder_name.end());
    toml::table table;
    if (!ParseSlotFile(folder, mod, table)) {
      continue;
    }
    const toml::array* list = table["slot"].as_array();
    if (!list) {
      REXLOG_WARN("Slots: {}'s slot.toml has no [[slot]]", mod);
      continue;
    }
    for (const toml::node& node : *list) {
      const toml::table* entry = node.as_table();
      Slot slot;
      if (!entry || !ReadSlot(*entry, folder, mod, served, slot)) {
        continue;
      }
      if (int(slots.size()) >= kPageSlots) {
        REXLOG_WARN("Slots: more than {} extra characters - {} is left out", kPageSlots,
                    slot.code);
        continue;
      }
      slots.push_back(std::move(slot));
    }
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!slots.empty() || !g_slots.empty()) {
    REXLOG_INFO("Slots: {} extra character(s) (R2 on the Versus / Training character select); "
                "extra_character_test_player = {}",
                slots.size(), REXCVAR_GET(extra_character_test_player));
  }
  g_slots = std::move(slots);
  g_fight = Fight{};
  g_select.pick = {-1, -1};
  g_last_pick = {-1, -1};
}

// For burstlimit_mods.cpp: what the select pictures are made from (the slots
// and their files, sizes and times), "" without any - it makes them again
// when this changes.
std::string BurstLimitSlotsArtKey() {
  std::lock_guard<std::mutex> lock(g_mutex);
  std::string key;
  bool any = false;
  for (const Slot& slot : g_slots) {
    key += slot.mod + "/" + slot.code + ":";
    for (const std::string* path : {&slot.card_art, &slot.portrait_art}) {
      if (path->empty()) {
        key += "-;";
        continue;
      }
      any = true;
      std::error_code ec;
      const std::filesystem::path file(std::u8string(path->begin(), path->end()));
      const auto size = std::filesystem::file_size(file, ec);
      const auto time = std::filesystem::last_write_time(file, ec).time_since_epoch().count();
      key += *path + "|" + std::to_string(size) + "|" + std::to_string(time) + ";";
    }
  }
  return any ? key : std::string();
}

// For burstlimit_mods.cpp: each slot's select textures (card, big picture;
// -1 none) as made by BurstLimitSlotsSelectArt, to give them back when the
// same pictures are served again (the slots read again, the same files).
std::vector<std::pair<int, int>> BurstLimitSlotsArtTextures() {
  std::lock_guard<std::mutex> lock(g_mutex);
  std::vector<std::pair<int, int>> textures;
  for (const Slot& slot : g_slots) {
    textures.push_back({slot.card_texture, slot.portrait_texture});
  }
  return textures;
}
void BurstLimitSlotsSetArtTextures(const std::vector<std::pair<int, int>>& textures) {
  std::lock_guard<std::mutex> lock(g_mutex);
  for (size_t k = 0; k < g_slots.size() && k < textures.size(); ++k) {
    g_slots[k].card_texture = textures[k].first;
    g_slots[k].portrait_texture = textures[k].second;
  }
}

// For burstlimit_mods.cpp: the character select's textures (UICHS_TOP.NUT,
// `nut`) with the extra characters' pictures appended (`out`); false when
// none has any (the file stays as it is). Their indices go to the slots.
bool BurstLimitSlotsSelectArt(const std::vector<uint8_t>& nut, std::vector<uint8_t>& out) {
  std::lock_guard<std::mutex> lock(g_mutex);
  for (Slot& slot : g_slots) {
    slot.card_texture = slot.portrait_texture = -1;
  }
  if (nut.size() < 16 || std::memcmp(nut.data(), "NTXR", 4) != 0 || nut[4] != 1) {
    if (!g_slots.empty()) {
      REXLOG_WARN("Slots: UICHS_TOP.NUT isn't a NUT it can add to - Teen Gohan's pictures are "
                  "shown");
    }
    return false;
  }
  uint32_t count = uint32_t(nut[6]) << 8 | nut[7];
  // The textures end where the file does (each entry starts with its size).
  size_t end = 16;
  for (uint32_t i = 0; i < count; ++i) {
    if (end + 4 > nut.size()) {
      return false;
    }
    const uint32_t size = uint32_t(nut[end]) << 24 | uint32_t(nut[end + 1]) << 16 |
                          uint32_t(nut[end + 2]) << 8 | nut[end + 3];
    if (!size || end + size > nut.size()) {
      return false;
    }
    end += size;
  }
  out.assign(nut.begin(), nut.begin() + end);
  const uint32_t first = count;
  // (UICHS_TOP.NUT as served with these is part of the online tag like any
  // replaced file, burstlimit_mods.cpp, and its bytes change with the build's
  // handles: left as is - the extra characters are offline only.)
  const uint32_t handles = kArtHandles + (g_art_build++ % 8) * 2 * kPageSlots;
  for (size_t k = 0; k < g_slots.size(); ++k) {
    Slot& slot = g_slots[k];
    std::vector<uint8_t> entry;
    if (!slot.card_art.empty() &&
        ArtEntry(slot.card_art, kCardWidth, kCardHeight, handles + uint32_t(k) * 2, entry)) {
      out.insert(out.end(), entry.begin(), entry.end());
      slot.card_texture = int(count++);
    }
    if (!slot.portrait_art.empty() &&
        ArtEntry(slot.portrait_art, kPortraitWidth, kPortraitHeight, handles + uint32_t(k) * 2 + 1,
                 entry)) {
      out.insert(out.end(), entry.begin(), entry.end());
      slot.portrait_texture = int(count++);
    }
    if (slot.card_texture >= 0 || slot.portrait_texture >= 0) {
      REXLOG_INFO("Slots: {}'s select pictures: card texture {}, big picture texture {}",
                  slot.code, slot.card_texture, slot.portrait_texture);
    }
  }
  if (count == first || count > 0xFFFF) {
    for (Slot& slot : g_slots) {
      slot.card_texture = slot.portrait_texture = -1;
    }
    return false;
  }
  out[6] = uint8_t(count >> 8);
  out[7] = uint8_t(count);
  return true;
}

// For burstlimit_mods.cpp, with the slots just read (before the game reads
// the archive): the files the slots make themselves, to serve under new
// archive names - each slot with a big picture of its own gets
// PAC/CMN/FIX/CHRA/UICMN_<code>.NUT, that picture alone (368x720, Teen Gohan's
// layout), for the screens after the picks. `taken`: names the archive or a
// mod already has (not made; the slot shows Teen Gohan's picture there).
void BurstLimitSlotsMadeFiles(const std::function<bool(const std::string&)>& taken,
                              std::map<std::string, std::vector<uint8_t>>& files) {
  std::lock_guard<std::mutex> lock(g_mutex);
  for (size_t k = 0; k < g_slots.size(); ++k) {
    Slot& slot = g_slots[k];
    slot.uicmn.clear();
    if (slot.portrait_art.empty()) {
      continue;
    }
    const std::string name = std::string(kUicmnFolder) + "UICMN_" + slot.code + ".NUT";
    if (taken(name)) {
      REXLOG_WARN("Slots: {} is already in the archive or a mod - {}'s picture after the picks "
                  "is Teen Gohan's",
                  name, slot.code);
      continue;
    }
    std::vector<uint8_t> entry;
    if (!ArtEntry(slot.portrait_art, kPortraitWidth, kPortraitHeight,
                  kUicmnHandles + uint32_t(k), entry)) {
      continue;
    }
    // NTXR v1, one texture.
    std::vector<uint8_t> nut = {'N', 'T', 'X', 'R', 1, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0};
    nut.insert(nut.end(), entry.begin(), entry.end());
    files[name] = std::move(nut);
    slot.uicmn = name;
    REXLOG_INFO("Slots: {}'s picture after the picks: {} (made from its big picture)", slot.code,
                name);
  }
}

// --- The fighter's files ---

// Mid-asm hook at 0x82181C14 in sub_82181BB0 (the fighter loader), stw
// r21,60(r31): r21 = the character's record about to be kept at [player +
// 60], r24 = the player. Its moveset, hand poses, voice keys and lip poses
// load from it right after (and the form -> model map, the stats, and later
// transformations read it there).
void BurstLimitSlotsRecord(PPCRegister& r21, PPCRegister& r24) {
  auto* memory = GuestMemory();
  const uint32_t player = r24.u32;
  if (!memory || player > 1) {
    return;
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  std::string why;
  const int index = SlotForPlayer(memory, player, &why);
  if (index < 0) {
    if (!why.empty()) {
      REXLOG_INFO("Slots: player {} ({}P) loads its own files: {}", player, player + 1, why);
    }
    return;
  }
  Slot& slot = g_slots[index];
  if (r21.u32 != kRecords + slot.base * kRecordSize || !MakeGuestData(memory, slot)) {
    return;
  }
  r21.u64 = slot.record;
  std::string own;
  for (Kind kind : {kMoveset, kHands, kLips, kVoiceKeys}) {
    if (!slot.files[kind].empty()) {
      own += std::string(own.empty() ? "" : ", ") + kKindNames[kind] + " " + slot.files[kind];
    }
  }
  REXLOG_INFO("Slots: player {} ({}P) loads {} ({}) - record {:08X}: {}", player, player + 1,
              slot.code, slot.name, slot.record, own.empty() ? "the base character's" : own);
}

// Mid-asm hook at 0x8218B8F4 in sub_8218B818 (a costume's model and skeleton
// for a form), bl sub_82123218: r5 = the record, r11 = the model index * 64,
// r21 = the model's path, r24 = the skeleton's, r28 = the player.
void BurstLimitSlotsModel(PPCRegister& r5, PPCRegister& r11, PPCRegister& r21, PPCRegister& r24,
                          PPCRegister& r28) {
  auto* memory = GuestMemory();
  if (!memory || !r5.u32) {
    return;
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  for (Slot& slot : g_slots) {
    if (!slot.record || slot.record != r5.u32) {
      continue;
    }
    const uint32_t player = r28.u32;
    const uint32_t colour =
        player <= 1 ? R16(memory, kPlayers + player * kPlayerStride + kPlayerColour) : 0;
    const int c = int(colour % uint32_t(slot.colours));
    const uint32_t m = r11.u32 / 64;
    // Both or neither: a model on another character's skeleton (or the other
    // way round) doesn't fit, so with one missing the base character's pair.
    const bool model = m < slot.models && slot.model_paths[c][m];
    const bool skeleton = m < slot.models && slot.skeleton_paths[c][m];
    if (!model || !skeleton) {
      if (slot.missing_models.insert({c, m}).second) {
        REXLOG_WARN("Slots: {} has no {} for colour {} model {} - the base character's model and "
                    "skeleton are shown",
                    slot.code,
                    !model && !skeleton ? "model (NUX) or skeleton (AMB)"
                    : !model            ? "model (NUX)"
                                        : "skeleton (AMB)",
                    c, m);
      }
      return;
    }
    r21.u64 = slot.model_paths[c][m];
    r24.u64 = slot.skeleton_paths[c][m];
    REXLOG_INFO("Slots: player {} ({}P) loads {} colour {} model {}: {} + {}", player, player + 1,
                slot.code, c, m, GuestString(memory, r21.u32, kPathField),
                GuestString(memory, r24.u32, kPathField));
    return;
  }
}

// Mid-asm hooks at 0x821B6268 (P1, sub_821B61D0) and 0x821B6388 (P2,
// sub_821B62F0): r4 = the SE bank's path (by the request's character).
void BurstLimitSlotsSoundEffects1(PPCRegister& r4) {
  SwapPath(r4, 0, kSoundEffects);
}
void BurstLimitSlotsSoundEffects2(PPCRegister& r4) {
  SwapPath(r4, 1, kSoundEffects);
}

// Mid-asm hooks at 0x821B62C0 (P1) and 0x821B640C (P2), bl to snprintf
// ("%s%s", r6 = the language's folder SOUND/CSB/US/ or /JP/, r7 = the voice
// bank's file name).
void BurstLimitSlotsVoice(PPCRegister& r6, PPCRegister& r7, uint32_t player) {
  auto* memory = GuestMemory();
  if (!memory) {
    return;
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  const int index = SlotForPlayer(memory, player, nullptr);
  if (index < 0) {
    return;
  }
  Slot& slot = g_slots[index];
  if (slot.voice.empty() || !MakeGuestData(memory, slot) || !slot.voice_name) {
    return;
  }
  const std::string folder = Upper(GuestString(memory, r6.u32, 64));
  const bool has = folder == "SOUND/CSB/US/" ? slot.voice_us
                   : folder == "SOUND/CSB/JP/" ? slot.voice_jp
                                               : false;
  if (!has) {
    REXLOG_INFO("Slots: player {} ({}P): {} has no voice in {} - the base character's", player,
                player + 1, slot.code, folder);
    return;
  }
  r7.u64 = slot.voice_name;
  REXLOG_INFO("Slots: player {} ({}P) loads {} voice: {}{}", player, player + 1, slot.code, folder,
              slot.voice);
}
void BurstLimitSlotsVoice1(PPCRegister& r6, PPCRegister& r7) {
  BurstLimitSlotsVoice(r6, r7, 0);
}
void BurstLimitSlotsVoice2(PPCRegister& r6, PPCRegister& r7) {
  BurstLimitSlotsVoice(r6, r7, 1);
}

// Mid-asm hooks in sub_821D2BF0 (P1) and sub_821D2DD0 (P2), bl sub_82119480
// with r4 = the path (by the request's character): ESP (0x821D2C94 /
// 0x821D2E78), EAS (0x821D2CE8 / 0x821D2ECC), _MODEL (0x821D2D3C /
// 0x821D2F20) and ARST (0x821D2D8C / 0x821D2F70).
void BurstLimitSlotsEffectTextures1(PPCRegister& r4) {
  SwapPath(r4, 0, kEffectTextures);
}
void BurstLimitSlotsEffectTextures2(PPCRegister& r4) {
  SwapPath(r4, 1, kEffectTextures);
}
void BurstLimitSlotsEffectTexturesEas1(PPCRegister& r4) {
  SwapPath(r4, 0, kEffectTextures2);
}
void BurstLimitSlotsEffectTexturesEas2(PPCRegister& r4) {
  SwapPath(r4, 1, kEffectTextures2);
}
void BurstLimitSlotsEffectModels1(PPCRegister& r4) {
  SwapPath(r4, 0, kEffectModels);
}
void BurstLimitSlotsEffectModels2(PPCRegister& r4) {
  SwapPath(r4, 1, kEffectModels);
}
void BurstLimitSlotsArst1(PPCRegister& r4) {
  SwapPath(r4, 0, kArst);
}
void BurstLimitSlotsArst2(PPCRegister& r4) {
  SwapPath(r4, 1, kArst);
}

// Mid-asm hook at 0x821182B4 in sub_82118228 (a NUX file is set up: model
// file, effect models, ARST), bl sub_82132800 that registers the file's
// textures: r3 = the name of the file's texture set (#NUX +12, e.g.
// "ARST_GOK.nut"), r30 = the load request (+2 the file's path).
//
// The game keeps one texture set per NAME (sub_82132638: the records at
// 0x84147E98, 84 bytes, name at +12): a NUX whose set name is already loaded
// gets that set - its own textures are never registered - while its models
// are still made from its own data. Harmless for the game's own files (the
// same name is the same file: Goku vs Goku), but a slot's files are copies
// of the base character's and keep the names inside (ARST_BUS.NUX and
// ARST_GHL.NUX are both "ARST_GOK.nut", BCGHL000.NUX is "BCGOK020_B.NUT" like
// Goku's 3rd colour). The second fighter then draws its effect models /
// model with materials that name textures (GIDX) of a set that wasn't
// loaded: sub_82105BE0's texture lookup returns NULL and the game crashes
// (Ultimate Gohan vs Kid Buu, ~15 s in), or it gets the other fighter's
// textures (effect colours). A slot's own NUX files get a name made from
// their path instead (the game keeps the name's file name without its
// extension, sub_821243C8, so '/' and '.' become '_': "PAC_CMN_CH_BCBUS_
// ARST_BUS_NUX"): unique per file, never one of the game's names, and the
// same file still shares (two players on the same extra character).
static std::map<std::string, uint32_t> g_nux_set_names;  // path (upper case) -> guest name

void BurstLimitSlotsNuxTextures(PPCRegister& r3, PPCRegister& r30) {
  auto* memory = GuestMemory();
  if (!memory || !r30.u32) {
    return;
  }
  const uint32_t path_address = r30.u32 + 2;
  const std::string path = GuestString(memory, path_address, kMaxPath + 1);
  if (path.empty()) {
    return;
  }
  auto same = [](const std::string& a, const std::string& b) {
    if (a.size() != b.size()) {
      return false;
    }
    for (size_t i = 0; i < a.size(); ++i) {
      char x = char(std::toupper(uint8_t(a[i]))), y = char(std::toupper(uint8_t(b[i])));
      x = x == '\\' ? '/' : x;
      y = y == '\\' ? '/' : y;
      if (x != y) {
        return false;
      }
    }
    return true;
  };
  std::lock_guard<std::mutex> lock(g_mutex);
  for (const Slot& slot : g_slots) {
    bool own = false;
    for (const std::string& file : slot.files) {
      own = own || (!file.empty() && same(file, path));
    }
    for (const auto* list : {&slot.model, &slot.skeleton}) {
      for (const auto& colour : *list) {
        for (const std::string& file : colour) {
          own = own || (!file.empty() && same(file, path));
        }
      }
    }
    if (own) {
      std::string key = path;
      for (char& c : key) {
        c = char(std::toupper(uint8_t(c)));
      }
      uint32_t& set_name = g_nux_set_names[key];
      if (!set_name) {
        std::string unique = key;
        std::replace_if(
            unique.begin(), unique.end(),
            [](char c) { return c == '/' || c == '\\' || c == '.'; }, '_');
        set_name = GuestPath(memory, unique);
        if (!set_name) {
          g_nux_set_names.erase(key);
          return;
        }
      }
      const std::string inside = r3.u32 ? GuestString(memory, r3.u32, 64) : std::string();
      REXLOG_INFO("Slots: {} model file {}: its textures are its own set {} (the name inside: {})",
                  slot.code, path, GuestString(memory, set_name, kPathField), inside);
      r3.u64 = set_name;
      return;
    }
  }
}

// Mid-asm hook at 0x821F26BC in sub_821F2680 (the effect scripts, BSP_<code>):
// r4 = the path, r9 = the player * 416.
void BurstLimitSlotsEffectScript(PPCRegister& r4, PPCRegister& r9) {
  SwapPath(r4, r9.u32 / 416, kEffectScript);
}

// Mid-asm hook at 0x821446EC in sub_821446B0 (BAR_<code>): r4 = the path,
// r9 = the player * 704.
void BurstLimitSlotsBar(PPCRegister& r4, PPCRegister& r9) {
  SwapPath(r4, r9.u32 / 704, kBar);
}

// Mid-asm hooks at 0x821DD794 (P1, sub_821DD6E8) and 0x821DD854 (P2,
// sub_821DD7A8): r4 = the battle HUD's face file (BUFAC_<code>.NUT).
void BurstLimitSlotsHudFace1(PPCRegister& r4) {
  SwapPath(r4, 0, kHudFace);
}
void BurstLimitSlotsHudFace2(PPCRegister& r4) {
  SwapPath(r4, 1, kHudFace);
}

// Mid-asm hooks in sub_821E2CA0 (the battle HUD, r20 = the player) right
// after each bl sub_82141F20(2, 0x826C7B58[character]) that gets the life
// bar's name (BTL_SYSTEM.NFX text 1088 + id): 2P measured at 0x821E426C /
// 0x821E42A8 (its name is right-aligned) and drawn at 0x821E4328 /
// 0x821E43E8, 1P measured at 0x821E45F0 / 0x821E462C and drawn at 0x821E4690
// / 0x821E4738 (the smaller size when it's wider than 295). r3 = the text: a
// slot player's own name instead.
void BurstLimitSlotsHudName(PPCRegister& r3, PPCRegister& r20) {
  auto* memory = GuestMemory();
  const uint32_t player = r20.u32;
  if (!memory || player > 1 || !r3.u32) {
    return;
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  const int index = SlotForPlayer(memory, player, nullptr);
  if (index < 0) {
    return;
  }
  const uint32_t name = HudName(memory, g_slots[index]);
  if (name) {
    r3.u64 = name;
  }
}

// --- After the character select ---

// Mid-asm hook at 0x822371FC in sub_822371B8 (each side's full picture after
// the picks, bl sub_82237060; Versus 0x82271744, Training 0x82265BF8): r31 =
// the side's block + 16. The side the next picture is loaded for.
void BurstLimitSlotsUicmnSide(PPCRegister& r31) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_uicmn_side = SideOfCharacterField(r31.u32);
}

// Mid-asm hook at 0x822371A8 in sub_82237060, bl sub_8227F040 with r4 = the
// picture's NUT path (PAC/CMN/FIX/CHRA\UICMN_<code>.nut) and r5 = its layout
// (_UICMN_<code>.ama). For the side set just before (BurstLimitSlotsUicmnSide;
// the other screens that call it: none) that picked an extra character: its
// own picture, or Teen Gohan's (as on the character select).
void BurstLimitSlotsUicmn(PPCRegister& r4, PPCRegister& r5) {
  auto* memory = GuestMemory();
  std::lock_guard<std::mutex> lock(g_mutex);
  const int side = g_uicmn_side;
  g_uicmn_side = -1;
  if (!memory || side < 0) {
    return;
  }
  const int index = PickedSlot(memory, uint32_t(side));
  if (index < 0) {
    return;
  }
  Slot& slot = g_slots[index];
  if (!slot.uicmn_nut) {
    slot.uicmn_nut = GuestPath(memory, slot.uicmn.empty() ? kStandInUicmn : slot.uicmn);
    slot.uicmn_layout = GuestPath(memory, kStandInUicmnLayout);
  }
  if (!slot.uicmn_nut || !slot.uicmn_layout) {
    return;
  }
  const std::string from = GuestString(memory, r4.u32, kPathField);
  r4.u64 = slot.uicmn_nut;
  r5.u64 = slot.uicmn_layout;
  REXLOG_INFO("Slots: side {}'s picture after the picks: {} + {} (instead of {})", side,
              slot.uicmn.empty() ? kStandInUicmn : slot.uicmn, kStandInUicmnLayout, from);
}

// Mid-asm hook at 0x8223CF78 in sub_8223BE10 (the "Start battle with these
// settings?" window), after bl sub_8228F5C0 got a side's name (MENUCOMMON
// 0x826DF730[character]): r3 = the text, r29 = the side's block + 16. An extra
// character's own name.
void BurstLimitSlotsConfirmName(PPCRegister& r3, PPCRegister& r29) {
  auto* memory = GuestMemory();
  const int side = SideOfCharacterField(r29.u32);
  if (!memory || side < 0) {
    return;
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  const int index = PickedSlot(memory, uint32_t(side));
  if (index < 0) {
    return;
  }
  Slot& slot = g_slots[index];
  MakeSelectData(memory, slot);
  if (slot.name_text) {
    r3.u64 = slot.name_text;
  }
}

// Mid-asm hook at 0x8223CFC0 in sub_8223BE10, lhz r10,0(r9): r9 = the side's
// character's Ultimate list (0x826DF6D8[character]), whose entry for the
// Ultimate picked ([r29 + 32]) names it; r29 = the side's block + 16. An extra
// character with its own list: that one (as in the Ultimate window).
void BurstLimitSlotsConfirmUltimate(PPCRegister& r9, PPCRegister& r29) {
  auto* memory = GuestMemory();
  const int side = SideOfCharacterField(r29.u32);
  if (!memory || side < 0) {
    return;
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  const int index = PickedSlot(memory, uint32_t(side));
  if (index < 0 || g_slots[index].ultimates.empty()) {
    return;
  }
  Slot& slot = g_slots[index];
  MakeSelectData(memory, slot);
  if (slot.ultimate_list) {
    r9.u64 = slot.ultimate_list;
  }
}

// --- The match ---

// Mid-asm hook at 0x82212360 in sub_822120A0 (the match request is built from
// the session, r31 = the request), before the game's mirror-colour rule (P1
// and P2 the same character in the same colour: P2's colour 1 - colour,
// 0x82212360..0x8221238C). The fight's extra characters are taken here. With
// one, its rules instead (true: the game's rule is skipped):
// - colours: an extra character and its base character are two characters,
//   both keep theirs; two of the same extra character: the next of its
//   colours (when it has more than one);
// - its highest and start form: no form above its own max_form;
// - the Versus rival scenes (drama pieces 1 and 2, written for the base
//   character): none, when its slot.toml says dramas = "off".
bool BurstLimitSlotsRequest(PPCRegister& r31) {
  auto* memory = GuestMemory();
  const uint32_t request = r31.u32;
  if (!memory || !request) {
    return false;
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  TakeFight(memory, request);
  const std::array<int, 2> slot = g_fight.slot;
  if (slot[0] < 0 && slot[1] < 0) {
    return false;
  }
  bool dramas_off = false;
  for (uint32_t p = 0; p < 2; ++p) {
    if (slot[p] < 0) {
      continue;
    }
    const Slot& s = g_slots[slot[p]];
    const uint32_t entry = request + p * kRequestStride;
    const uint16_t max_form = R16(memory, entry + kRequestMaxForm);
    const uint16_t capped = uint16_t(std::min<int>(max_form, s.max_form));
    W16(memory, entry + kRequestMaxForm, capped);
    W16(memory, entry + kRequestStartForm, 0);
    W8(memory, entry + kRequestStartForm2, 0);  // (+0x13, the CPU's AI level, stays)
    REXLOG_INFO("Slots: player {} ({}P) {}: highest form {} (was {}), start form 0", p, p + 1,
                s.code, capped, max_form);
    dramas_off |= !s.dramas;
  }
  if (dramas_off && R32(memory, request + kRequestMode) == kModeVersus) {
    int removed = 0;
    for (uint32_t p = 0; p < 2; ++p) {
      for (uint32_t d = 0; d < kRequestDramaCount; ++d) {
        const uint32_t at = request + p * kRequestStride + kRequestDramas + d * 4;
        const uint32_t drama = R32(memory, at);
        if (drama == kDramaRivalIntro || drama == kDramaRivalEnd) {
          W32(memory, at, 0);
          ++removed;
        }
      }
    }
    if (removed) {
      REXLOG_INFO("Slots: the Versus rival scenes are off this match ({} drama piece(s) 1/2 "
                  "removed: they're written for the base character)",
                  removed);
    }
  }
  const uint16_t character0 = R16(memory, request);
  const uint16_t character1 = R16(memory, request + kRequestStride);
  const int16_t colour0 = int16_t(R16(memory, request + kRequestColour));
  const int16_t colour1 = int16_t(R16(memory, request + kRequestStride + kRequestColour));
  if (character0 == character1 && colour0 == colour1) {
    if (slot[0] != slot[1]) {
      REXLOG_INFO("Slots: both players are character {} in colour {}, but as different "
                  "characters: the colours stay",
                  character0, colour0);
    } else if (g_slots[slot[0]].colours > 1) {
      const int16_t next = int16_t((colour0 + 1) % g_slots[slot[0]].colours);
      W16(memory, request + kRequestStride + kRequestColour, uint16_t(next));
      REXLOG_INFO("Slots: both players are {} in colour {}: 2P wears colour {}",
                  g_slots[slot[0]].code, colour0, next);
    }
  }
  return true;
}

// Mid-asm hooks in the menus that fill the session from the character select
// when both sides picked the same character in the same colour, before they
// give 2P the next colour (Versus 0x82271DD0 / 0x82272030 / 0x822722D0,
// Training 0x82265F60): true (skip it) when only one of them, or two
// different ones, picked an extra character.
bool BurstLimitSlotsSessionColour() {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_select.pick[0] == g_select.pick[1]) {
    return false;
  }
  REXLOG_INFO("Slots: the same character and colour on both sides, but {} - the colours stay",
              g_select.pick[0] >= 0 && g_select.pick[1] >= 0 ? "two extra characters"
                                                             : "one is an extra character");
  return true;
}

// From burstlimit_forms.cpp's hook at 0x82181E24 (the fighter loader, before
// it reads the start form; `entry` = the player's match request entry): an
// extra character starts in form 0 and can't go above its max_form - RB /
// LB's start form (the base character's) isn't used. True for one.
bool BurstLimitSlotsStartForm(uint32_t player, uint32_t entry) {
  auto* memory = GuestMemory();
  if (!memory || player > 1 || !entry) {
    return false;
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  const int index = SlotForPlayer(memory, player, nullptr);
  if (index < 0) {
    return false;
  }
  const Slot& slot = g_slots[index];
  const uint16_t max_form = R16(memory, entry + kRequestMaxForm);
  if (max_form > slot.max_form) {
    W16(memory, entry + kRequestMaxForm, uint16_t(slot.max_form));
  }
  W16(memory, entry + kRequestStartForm, 0);
  W8(memory, entry + kRequestStartForm2, 0);  // (+0x13, the CPU's AI level, stays)
  REXLOG_INFO("Slots: player {} ({}P) {}: start form 0, highest form {} (no start form from RB / "
              "LB)",
              player, player + 1, slot.code, std::min<int>(max_form, slot.max_form));
  return true;
}

// --- The character select ---

// Mid-asm hook at 0x82245384 in sub_82244F60 (the character select's setup,
// r30 = its object), once the lists, colours and cards are set up and before
// the texts: whether this select has the page (Versus / Training, offline,
// with extra characters), their colour counts, and a side whose last pick was
// an extra character opens on its page again. Back from the stage select (B,
// the object's +905): the sides keep their picks and go on without a new A,
// so the extra characters picked stay too.
void BurstLimitSlotsSelectSetup(PPCRegister& r30) {
  auto* memory = GuestMemory();
  const uint32_t object = r30.u32;
  if (!memory || !object) {
    return;
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  const bool keep = R8(memory, object + kObjectKeep) != 0;
  const std::array<int, 2> kept = g_select.pick;
  g_select = Select{};
  g_select.object = object;
  if (g_slots.empty()) {
    g_select.why = "no extra characters (a mod with a slot.toml, switched on at start)";
    return;
  }
  const uint32_t menu = R32(memory, kMenuMode);
  if (!REXCVAR_GET(extra_characters)) {
    g_select.why = "extra_characters is off";
  } else if (rex::net::IsGameSessionOpen()) {
    g_select.why = "online (both PCs would need the same extra characters)";
  } else if (R8(memory, object + kObjectTrial) ||
             std::find(std::begin(kMenuModes), std::end(kMenuModes), menu) ==
                 std::end(kMenuModes)) {
    g_select.why = "only in Versus and Training (this is menu " + std::to_string(menu) + ")";
  }
  if (!g_select.why.empty()) {
    REXLOG_INFO("Slots: character select without the extra characters' page: {}", g_select.why);
    return;
  }
  g_select.available = true;
  if (keep) {
    g_select.pick = kept;
  }
  std::string names;
  for (int k = 0; k < PageSlots(); ++k) {
    Slot& slot = g_slots[k];
    MakeSelectData(memory, slot);
    W32(memory, object + kObjectColourCounts + (kValueFirst + uint32_t(k)) * 4,
        uint32_t(slot.colours));
    names += (names.empty() ? "" : ", ") + slot.name;
  }
  REXLOG_INFO("Slots: character select (menu {}): R2 shows the extra characters ({})", menu,
              names);
  for (uint32_t side = 0; side < 2; ++side) {
    const int last = g_last_pick[side];
    const uint32_t cursor = R32(memory, object + kObjectCursor + side * 4);
    if (last < 0 || last >= PageSlots() || cursor >= kListSize ||
        R32(memory, ListAddress(object, side, cursor)) != g_slots[last].base) {
      g_select.pick[side] = -1;  // (a kept pick only on its page)
      continue;
    }
    g_select.sides[side].page_cursor = uint32_t(last);
    ShowPage(memory, object, side);
    REXLOG_INFO("Slots: side {} opens on the extra characters' page ({} was its last pick{})", side,
                g_slots[last].name,
                g_select.pick[side] == last ? " - kept, back from the stage select" : "");
  }
}

// Mid-asm hook at 0x82249F2C in sub_82249140 (the character select's logic,
// a side's frame, where it checks whether its cursor's move has finished:
// r17 = the side's target (-1: its own pad), r19 = the buttons pressed, r30 =
// the side, r31 = the object). R2 switches the side between the roster and
// the extra characters' page, L2 goes back; then (true) it goes on at
// 0x82249F54 as when the cursor stops on a card: the big picture and the name
// again (sub_82248CD0), and the cards (sub_82243808: RANDOM's state set to 1
// makes it rebuild them; it's 0 again after).
bool BurstLimitSlotsSelectPage(PPCRegister& r17, PPCRegister& r19, PPCRegister& r30,
                               PPCRegister& r31) {
  const uint32_t side = r30.u32;
  const uint32_t object = r31.u32;
  if (side > 1) {
    return false;
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  auto* memory = GuestMemory();
  SidePage& page = g_select.sides[side];
  const uint32_t buttons = r19.u32 & (kButtonL2 | kButtonR2);
  const uint32_t pressed = buttons & ~page.buttons;
  page.buttons = buttons;
  if (!page.armed) {
    if (memory && (PadsHolding(memory, kButtonL2 | kButtonR2) & page.pads)) {
      return false;
    }
    page.armed = true;
  }
  if (!pressed || r17.s32 >= 0 || g_slots.empty()) {
    return false;
  }
  if (!memory || object != g_select.object) {
    return false;
  }
  if (!g_select.available || rex::net::IsGameSessionOpen()) {
    if (!g_select.told) {
      g_select.told = true;
      REXLOG_INFO("Slots: R2 on the character select does nothing here: {}",
                  g_select.available ? "online" : g_select.why);
    }
    return false;
  }
  const uint32_t step = R32(memory, kSideBlocks + side * kSideBlockStride + kSideStep);
  if (step != kStepPicking) {
    REXLOG_INFO("Slots: R2 on side {}: it isn't picking (step {})", side, step);
    return false;
  }
  if (R32(memory, object + kObjectCursor + side * 4) !=
      R32(memory, object + kObjectShown + side * 4)) {
    return false;  // the cursor is moving
  }
  const int64_t now = NowMs();
  if (now - page.switched_ms < 300) {
    return false;
  }
  bool to_page;
  if (pressed & kButtonR2) {
    to_page = !page.active;
  } else if (page.active) {
    to_page = false;
  } else {
    return false;
  }
  if (to_page) {
    ShowPage(memory, object, side);
    REXLOG_INFO("Slots: side {} shows the extra characters (R2): {} card(s)", side, PageSlots());
  } else {
    ShowRoster(memory, object, side);
    REXLOG_INFO("Slots: side {} back to the roster ({})", side, (pressed & kButtonR2) ? "R2" : "L2");
  }
  W32(memory, object + kObjectRandom + side * kObjectRandomStride, 1);
  W32(memory, object + kObjectScroll + side * 4, 0);
  W32(memory, object + kObjectScroll2 + side * 4, 0);
  page.switched_ms = now;
  // The controllers pulling it now (none: armed again at once - the 300 ms
  // above still holds).
  page.pads = PadsHolding(memory, pressed);
  page.armed = false;
  return true;
}

// Mid-asm hook at 0x82243980 in sub_82243808 (a side's cards), cmpwi r4,0:
// r4 = the card's sprite part, r27 = the side, r28 = mirrored, r30 = the
// object, r31 = the card. On the page: Teen Gohan's card parts (the dark
// ones for the padding cards), never mirrored.
void BurstLimitSlotsCardPart(PPCRegister& r4, PPCRegister& r27, PPCRegister& r28, PPCRegister& r30,
                             PPCRegister& r31) {
  const uint32_t side = r27.u32;
  const uint32_t card = r31.u32;
  if (side > 1 || card >= kListSize) {
    return;
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  auto* memory = GuestMemory();
  if (!memory || !PageShown(side) || r30.u32 != g_select.object) {
    return;
  }
  const uint32_t value = R32(memory, ListAddress(r30.u32, side, card));
  if (value == kValuePad) {
    r4.u64 = kStandInLockedCard[side];
  } else if (SlotOfValue(value) >= 0) {
    r4.u64 = kStandInCard[side];
  } else {
    return;
  }
  r28.u64 = 0;
}

// Mid-asm hook at 0x822439A4 in sub_82243808, after the card's sprite is made
// (r3): the extra character's own card texture (the padding cards: the
// first one's, dark).
void BurstLimitSlotsCardTexture(PPCRegister& r3, PPCRegister& r27, PPCRegister& r30,
                                PPCRegister& r31) {
  const uint32_t side = r27.u32;
  const uint32_t card = r31.u32;
  if (side > 1 || card >= kListSize || !r3.u32) {
    return;
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  auto* memory = GuestMemory();
  if (!memory || !PageShown(side) || r30.u32 != g_select.object) {
    return;
  }
  const uint32_t value = R32(memory, ListAddress(r30.u32, side, card));
  const int index = value == kValuePad ? 0 : SlotOfValue(value);
  if (index >= 0 && g_slots[index].card_texture >= 0) {
    Retarget(memory, r3.u32, kStandInCardTexture, uint16_t(g_slots[index].card_texture));
  }
}

// Mid-asm hook at 0x82248DF0 in sub_82248CD0 (a side's big picture and name),
// cmpwi r5,0: r5 = the picture's animation, r27 = the list value, r31 = the
// side. An extra character: Teen Gohan's (its texture is swapped every frame,
// BurstLimitSlotsSelectDraw).
void BurstLimitSlotsPortrait(PPCRegister& r5, PPCRegister& r27, PPCRegister& r31) {
  const uint32_t side = r31.u32;
  std::lock_guard<std::mutex> lock(g_mutex);
  if (PageShown(side) && SlotOfValue(r27.u32) >= 0) {
    r5.u64 = kStandInPortrait[side];
  }
}

// Mid-asm hook at 0x82248F20 in sub_82248CD0, the name's text (r3) looked up
// by the list value (r27; r31 = the side): an extra character's own name.
void BurstLimitSlotsName(PPCRegister& r3, PPCRegister& r27, PPCRegister& r31) {
  std::lock_guard<std::mutex> lock(g_mutex);
  const int index = PageShown(r31.u32) ? SlotOfValue(r27.u32) : -1;
  if (index >= 0 && g_slots[index].name_text) {
    r3.u64 = g_slots[index].name_text;
  }
}

// Mid-asm hook at 0x8224A8A4 in sub_82249140 (Y, Change Color: cmpwi
// r7,21 / bge skips characters 21 and up; r7 = the list value, r30 = the
// side): an extra character's colours too (true: on at 0x8224A8AC; its
// colour count is the object's +720 + value * 4).
bool BurstLimitSlotsColour(PPCRegister& r7, PPCRegister& r30) {
  std::lock_guard<std::mutex> lock(g_mutex);
  return PageShown(r30.u32) && SlotOfValue(r7.u32) >= 0;
}

// Mid-asm hook at 0x8224A4E4 in sub_82249140 (A on a card: mr r3,r28 returns
// the list value r28; r30 = the side). An extra character: the game gets its
// base character, and the side keeps the slot for the match.
void BurstLimitSlotsConfirm(PPCRegister& r28, PPCRegister& r30) {
  const uint32_t side = r30.u32;
  if (side > 1) {
    return;
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  auto* memory = GuestMemory();
  const int index = PageShown(side) ? SlotOfValue(r28.u32) : -1;
  if (index < 0 || !memory) {
    g_select.pick[side] = -1;
    g_last_pick[side] = -1;
    return;
  }
  const Slot& slot = g_slots[index];
  const uint32_t cursor = R32(memory, g_select.object + kObjectCursor + side * 4);
  const uint32_t colour =
      cursor < kListSize ? R32(memory, ColourAddress(g_select.object, side, cursor)) : 0;
  g_page_colours[side][index] = colour;
  g_select.pick[side] = index;
  g_last_pick[side] = index;
  g_select.ultimates_told[side] = false;
  r28.u64 = slot.base;
  REXLOG_INFO("Slots: side {} picked {} ({}, colour {}) - the game gets character {}", side,
              slot.name, slot.code, colour, slot.base);
}

// Mid-asm hook at 0x82249990 in sub_82249140, RANDOM's result (r30 = the
// side): never an extra character (they're not on the roster).
void BurstLimitSlotsRandom(PPCRegister& r30) {
  if (r30.u32 > 1) {
    return;
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  g_select.pick[r30.u32] = -1;
  g_last_pick[r30.u32] = -1;
}

// Mid-asm hook at 0x82245520 in sub_822454D0 (the character select's drawing),
// right after the UI's animations ran (bl sub_82281260, which sets each
// sprite's texture back to its part's every frame): r30 = the object. On a
// side showing the page, the extra characters' cards and big picture show
// their own textures.
void BurstLimitSlotsSelectDraw(PPCRegister& r30) {
  const uint32_t object = r30.u32;
  std::lock_guard<std::mutex> lock(g_mutex);
  auto* memory = GuestMemory();
  if (!memory || !object || object != g_select.object ||
      (!g_select.sides[0].active && !g_select.sides[1].active)) {
    return;
  }
  for (uint32_t side = 0; side < 2; ++side) {
    if (!g_select.sides[side].active) {
      continue;
    }
    const uint32_t cards = R32(memory, kSelectGlobals + kGlobalsCards + side * 4);
    const uint32_t length = std::min(R32(memory, object + kObjectLength + side * 4), kListSize);
    for (uint32_t i = 0; cards && i < length; ++i) {
      const uint32_t value = R32(memory, ListAddress(object, side, i));
      const int index = value == kValuePad ? 0 : SlotOfValue(value);
      if (index >= 0 && g_slots[index].card_texture >= 0) {
        Retarget(memory, R32(memory, cards + i * kCardRecord), kStandInCardTexture,
                 uint16_t(g_slots[index].card_texture));
      }
    }
    const int shown =
        SlotOfValue(uint32_t(int16_t(R16(memory, kSelectGlobals + kGlobalsShown + side * 2))));
    if (shown < 0 || g_slots[shown].portrait_texture < 0) {
      continue;
    }
    const uint32_t context = R32(memory, object + kObjectContext);
    if (!context) {
      continue;
    }
    const uint32_t layer = context + kContextOffset + kLayers +
                           (R32(memory, object + kObjectLayer) + kPortraitLayer + side) *
                               kLayerStride;
    RetargetTree(memory, R32(memory, layer + kLayerSprite), kStandInPortraitTexture,
                 uint16_t(g_slots[shown].portrait_texture));
  }
}

// Mid-asm hook at 0x822455D8 in sub_822454D0, lwzx r11,r11,r7: the colour
// behind a side's big picture, from the table at r7 (0x826DFE48, 4 per
// character) at r11 = (value * 4 + colour) * 4, r9 = value * 4. An extra
// character's value is past the table's end: its own tints instead.
void BurstLimitSlotsTint(PPCRegister& r7, PPCRegister& r9, PPCRegister& r11) {
  const uint32_t value = r9.u32 / 4;
  if (value < kValueFirst) {
    return;
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  const int index = SlotOfValue(value);
  if (index < 0 || (!g_select.sides[0].active && !g_select.sides[1].active) ||
      !g_slots[index].tints) {
    return;
  }
  const uint32_t colour = (r11.u32 / 4 - value * 4) % 4;
  r7.u64 = g_slots[index].tints;
  r11.u64 = colour * 4;
}

// Mid-asm hook at 0x82234350 in sub_82234140 (the Ultimate window after a
// pick): r11 = the character's list ({count, (text, ultimate) x n}), r29 =
// the character, r31 = the side. An extra character with ultimates in its
// slot.toml: its own list.
void BurstLimitSlotsUltimates(PPCRegister& r11, PPCRegister& r29, PPCRegister& r31) {
  const uint32_t side = r31.u32;
  if (side > 1) {
    return;
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  g_select.ultimate_window[side] = -1;
  const int index = g_select.pick[side];
  if (index < 0 || index >= int(g_slots.size())) {
    return;
  }
  Slot& slot = g_slots[index];
  if (slot.ultimates.empty() || r29.u32 != slot.base || !slot.ultimate_list) {
    return;
  }
  r11.u64 = slot.ultimate_list;
  g_select.ultimate_window[side] = index;
  if (!g_select.ultimates_told[side]) {
    g_select.ultimates_told[side] = true;
    REXLOG_INFO("Slots: side {}'s Ultimate window: {}'s own list ({} entr{})", side, slot.code,
                std::min<size_t>(slot.ultimates.size(), 3),
                slot.ultimates.size() == 1 ? "y" : "ies");
  }
}

// Mid-asm hook at 0x822343CC in sub_82234140, after the window's lock loop:
// r19 = the side's window block, whose bytes +16..+18 are each entry's
// "locked" (the save's +2377 byte of the base character's ultimate slot is 0:
// the name becomes "???" (text 36), no bar cost, not selectable) and +19..+21
// its NEW mark. An extra character's own list: all unlocked, none NEW.
void BurstLimitSlotsUltimatesUnlock(PPCRegister& r19, PPCRegister& r31) {
  auto* memory = GuestMemory();
  const uint32_t side = r31.u32;
  if (!memory || side > 1) {
    return;
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_select.ultimate_window[side] < 0) {
    return;
  }
  uint8_t* flags = memory->TranslateVirtual<uint8_t*>(r19.u32 + 16);
  std::fill(flags, flags + 6, uint8_t(0));
}

// Mid-asm hook at 0x82234444 in sub_82234140, after stw r3,0(r29): the name
// of entry r27 of the side's (r31) list was just stored at [r29]. An extra
// character's entry with a name of its own: that one.
void BurstLimitSlotsUltimateName(PPCRegister& r27, PPCRegister& r29, PPCRegister& r31) {
  auto* memory = GuestMemory();
  const uint32_t side = r31.u32;
  if (!memory || side > 1) {
    return;
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  const int index = g_select.ultimate_window[side];
  if (index < 0 || index >= int(g_slots.size())) {
    return;
  }
  const Slot& slot = g_slots[index];
  const uint32_t entry = r27.u32;
  if (entry < slot.ultimate_texts.size() && slot.ultimate_texts[entry]) {
    W32(memory, r29.u32, slot.ultimate_texts[entry]);
  }
}

// Mid-asm hook at 0x8223D018 in sub_8223BE10 (the "Start battle with these
// settings?" window), after bl sub_822803E0: r3 = the name of the side's
// Ultimate (the list entry whose slot is [r29 + 32]); r29 = the side's block
// + 16. An extra character's entry with a name of its own: that one.
void BurstLimitSlotsConfirmUltimateName(PPCRegister& r3, PPCRegister& r29) {
  auto* memory = GuestMemory();
  const int side = SideOfCharacterField(r29.u32);
  if (!memory || side < 0) {
    return;
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  const int index = PickedSlot(memory, uint32_t(side));
  if (index < 0) {
    return;
  }
  Slot& slot = g_slots[index];
  MakeSelectData(memory, slot);
  const uint32_t picked = R32(memory, r29.u32 + 32);
  // As the game: the first entry with that slot (of the 3 the list holds).
  const size_t count = std::min<size_t>(slot.ultimates.size(), 3);
  for (size_t i = 0; i < count; ++i) {
    if (slot.ultimates[i].second == picked) {
      if (i < slot.ultimate_texts.size() && slot.ultimate_texts[i]) {
        r3.u64 = slot.ultimate_texts[i];
      }
      return;
    }
  }
}
