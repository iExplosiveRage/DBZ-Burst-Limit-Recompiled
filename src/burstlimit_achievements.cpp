// Achievement popups: when the game unlocks one of its Xbox 360 achievements
// (XGIUserWriteAchievements -> the SDK's AchievementManager, which keeps the
// unlocks per profile in <user data>\achievements\<XUID>\424107DC.toml, so an
// achievement only pops once), a toast in the look of the game's own menus
// slides in: the purple panel with its black title bar and beige rim, the
// menu font (BurstLimitMenuArt, read from the player's game files), the
// achievement's picture from the title's XDBF data in default.xex (a drawn
// trophy when there is none), its name, description and gamerscore. Each
// stays about 4.6 s; several queue up (a burst of more than a few is folded
// into one "N more" toast).
//
// Every achievement is a trophy of a tier from its gamerscore (TierOf: bronze
// up to 15G, silver 20-35G, gold 40G and up - fitted to the game's 50, see
// "Achievement tiers:" in the log), shown by the colour of the popup's title
// and trophy. When an unlock leaves none locked (an unlock callback: only a
// new unlock, never one read back from the profile's file) a second popup,
// the platinum, follows it.
//
// A sound plays with it. achievement_sound = tiers (the default) plays the
// tier's file, achievement_sound_bronze / _silver / _gold / _platinum
// (default sounds\trophy_*.mp3 next to the exe: trophy sounds by Spade, free,
// shipped from res/sounds); an empty or missing file falls back to a chime
// made here for that tier. achievement_sound = game plays one of the
// game's own sound effects instead (achievement_game_sound = "BANK:cue",
// decoded from the player's game files by burstlimit_menusound.cpp; F1 >
// GAME > Game sound picks and previews one). The other sounds are made here (no recording, no
// sample of anyone's): "xbox" is a soft rising two-note "bloop" with a bell
// ping (also the bronze chime; silver, gold and platinum are brighter and
// longer versions of it), "playstation" a bright two-bell sparkle with a long tail - only
// similar in feel to the consoles' sounds. Their volume is
// achievement_volume times the game's own Sound Effects volume (Options >
// Sound, save byte +21, the game's volume table 0x82520268). The chime goes
// to the default output device by itself (waveOut), so it is heard while the
// game's mixer is muted only when wanted: with audio_mute on it is made but
// not played, and the log says how loud it would have been;
// achievement_sound_dump=<file.wav> writes every chime made to that file.
//
// F7 opens the list of them all in the same look (AchievementsWindow below:
// the count, gamerscore and trophies, All / Unlocked / Locked, each with its
// picture, tier and points; the controller, the keys and the mouse move
// through it). The game's locked texts of its secret achievements are all
// "<Translated text>": the list and the F1 menu show their description.
//
// Settings (F1 > GAME): achievement_popups, achievement_sound, Trophy tiers
// (A previews a tier's popup and sound), achievement_popup_position,
// achievement_volume, and the list of the game's achievements (A on one shows
// its popup again).
//
// Debug: achievement_test <id|all|any|bronze|silver|gold|platinum> shows
// popups without unlocking, achievement_tier_test <tier|all> the four tiers,
// achievement_unlock <id> unlocks like the game, achievement_list logs them
// (and the tiers, flags and locked texts), achievements_window
// [open|close|toggle] the F7 window, achievement_sound_test [xbox|playstation|<tier>|...];
// one_hit_kill (offline) gives player 2 1 HP at the battle set-up (Z
// Chronicles too), so one hit wins.

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <windows.h>
#include <mmsystem.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>

#include <imgui.h>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/input/input_system.h>
#include <rex/logging.h>
#include <rex/memory.h>
#include <rex/memory/utils.h>
#include <rex/net/session.h>
#include <rex/rex_app.h>
#include <rex/runtime.h>
#include <rex/system/achievement_manager.h>
#include <rex/system/kernel_state.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/immediate_drawer.h>
#include <rex/ui/overlay/achievement_icon_cache.h>
#include <rex/ui/overlay/achievement_notification.h>
#include <rex/ui/overlay/quick_menu.h>

#include "burstlimit_gamesound.h"
#include "burstlimit_menuart.h"

// burstlimit_menusound.cpp: the game's menu sounds (0 move, 5 a menu opens, 6 it closes).
void BurstLimitPlayMenuSound(int sound);

#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")

REXCVAR_DEFINE_BOOL(achievement_popups, true, "Patches",
                    "Shows a popup when the game unlocks an achievement")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_STRING(achievement_sound, "tiers", "Patches",
                      "The sound of an achievement popup: tiers (a sound per trophy tier - "
                      "achievement_sound_bronze, _silver, _gold, _platinum), xbox (a rising "
                      "bloop), playstation (a bell sparkle), custom (achievement_sound_file, or "
                      "sounds\\achievement.wav / .mp3 next to the exe), game (one of the game's "
                      "own sound effects, achievement_game_sound) or off. The built-in chimes are "
                      "made by the recomp, not the consoles' sounds")
    .allowed({"tiers", "xbox", "playstation", "custom", "game", "off"})
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_STRING(achievement_sound_bronze, "sounds/trophy_bronze.mp3", "Patches",
                      "achievement_sound = tiers: the sound of a bronze trophy (achievements up "
                      "to 15G), a .wav or .mp3 (a path, or relative to the exe's folder). Empty or "
                      "missing = a chime made by the recomp")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_STRING(achievement_sound_silver, "sounds/trophy_silver.mp3", "Patches",
                      "achievement_sound = tiers: the sound of a silver trophy (20-35G), a .wav "
                      "or .mp3 (a path, or relative to the exe's folder). Empty or missing = a "
                      "chime made by the recomp")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_STRING(achievement_sound_gold, "sounds/trophy_gold.mp3", "Patches",
                      "achievement_sound = tiers: the sound of a gold trophy (40G and up), a .wav "
                      "or .mp3 (a path, or relative to the exe's folder). Empty or missing = a "
                      "chime made by the recomp")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_STRING(achievement_sound_platinum, "sounds/trophy_platinum.mp3", "Patches",
                      "achievement_sound = tiers: the sound of the platinum (every achievement "
                      "unlocked), a .wav or .mp3 (a path, or relative to the exe's folder). Empty "
                      "or missing = a fanfare made by the recomp")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_STRING(achievement_popup_position, "bottom", "Patches",
                      "Where achievement popups show: top_left, top, top_right, bottom_left, "
                      "bottom, bottom_right")
    .allowed({"top_left", "top", "top_right", "bottom_left", "bottom", "bottom_right"})
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(achievement_volume, 80, "Patches",
                     "Volume of the achievement sound in percent, times the game's own Sound "
                     "Effects volume")
    .range(0, 100)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_STRING(achievement_sound_file, "", "Patches",
                      "achievement_sound = custom: a .wav or .mp3 to play (a path, or relative to "
                      "the exe's folder). Empty = sounds\\achievement.wav or .mp3 next to the exe")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_STRING(achievement_game_sound, "SYSTEM_SE:17", "Patches",
                      "achievement_sound = game: which of the game's sound effects, as BANK:cue "
                      "(SYSTEM_SE, MENU_SE or BATTLE_COMMON_SE and the cue id), e.g. SYSTEM_SE:17. "
                      "Played from your own game files")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_STRING(achievement_sound_dump, "", "Debug",
                      "Testing: writes every achievement sound made to this .wav file (also "
                      "while audio_mute is on)");
REXCVAR_DEFINE_BOOL(one_hit_kill, false, "Debug",
                    "Testing (offline): player 2 starts every fight with 1 HP, so one hit wins")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace {

using AchievementEvent = rex::system::AchievementEvent;

rex::memory::Memory* GuestMemory() {
  auto* runtime = rex::Runtime::instance();
  return runtime ? runtime->memory() : nullptr;
}

bool Readable(rex::memory::Memory* memory, uint32_t address, uint32_t size) {
  if (!memory || address < 0x10000) {
    return false;
  }
  auto* heap = memory->LookupHeap(address);
  rex::memory::HeapAllocationInfo info = {};
  return heap && heap->QueryRegionInfo(address, &info) &&
         (info.state & rex::memory::kMemoryAllocationCommit) &&
         uint64_t(address) + size <= uint64_t(info.base_address) + info.region_size;
}

uint32_t R32(rex::memory::Memory* memory, uint32_t address) {
  return rex::memory::load_and_swap<uint32_t>(memory->TranslateVirtual<uint8_t*>(address));
}

rex::system::AchievementManager* Manager() {
  auto* runtime = rex::Runtime::instance();
  auto* kernel = runtime ? runtime->kernel_state() : nullptr;
  return kernel ? &kernel->achievements() : nullptr;
}

// ---------------------------------------------------------------- trophy tiers

enum class Tier { kBronze, kSilver, kGold, kPlatinum };

// From the gamerscore, fitted to the game's 50 achievements (5, 15, 20, 35, 40
// and 100G): bronze up to 15G (30 of them), silver 20-35G (14), gold 40G and
// up (6). The platinum is all of them.
constexpr uint32_t kSilverFrom = 20;
constexpr uint32_t kGoldFrom = 40;

Tier TierOf(uint32_t gamerscore) {
  return gamerscore >= kGoldFrom ? Tier::kGold
         : gamerscore >= kSilverFrom ? Tier::kSilver
                                     : Tier::kBronze;
}

const char* TierName(Tier tier) {
  switch (tier) {
    case Tier::kSilver: return "silver";
    case Tier::kGold: return "gold";
    case Tier::kPlatinum: return "platinum";
    default: return "bronze";
  }
}

const char* TierTitle(Tier tier) {
  switch (tier) {
    case Tier::kSilver: return "Silver";
    case Tier::kGold: return "Gold";
    case Tier::kPlatinum: return "Platinum";
    default: return "Bronze";
  }
}

std::optional<Tier> TierFromName(std::string_view name) {
  for (Tier tier : {Tier::kBronze, Tier::kSilver, Tier::kGold, Tier::kPlatinum}) {
    if (name == TierName(tier)) {
      return tier;
    }
  }
  return std::nullopt;
}

// "bronze 30 (5G x24, 15G x6), silver 14 (...), gold 6 (...); platinum = all 50 (1000 G)".
std::string TierSummary(const std::vector<rex::system::AchievementInfo>& list) {
  std::map<uint32_t, uint32_t> by_score;
  uint32_t total = 0;
  for (const auto& a : list) {
    ++by_score[a.gamerscore];
    total += a.gamerscore;
  }
  std::string out;
  for (Tier tier : {Tier::kBronze, Tier::kSilver, Tier::kGold}) {
    uint32_t count = 0;
    std::string scores;
    for (const auto& [score, n] : by_score) {
      if (TierOf(score) == tier) {
        count += n;
        scores += fmt::format("{}{}G x{}", scores.empty() ? "" : ", ", score, n);
      }
    }
    out += fmt::format("{}{} {}{}", out.empty() ? "" : ", ", TierName(tier), count,
                       scores.empty() ? "" : " (" + scores + ")");
  }
  return out + fmt::format("; platinum = all {} ({} G)", list.size(), total);
}

// ---------------------------------------------------------------- descriptions

// Xbox 360 achievement flag: shown with its text while locked (without it, a
// secret one).
constexpr uint32_t kShowUnachieved = 0x8;

std::string_view Trim(std::string_view s) {
  while (!s.empty() && std::isspace(uint8_t(s.front()))) s.remove_prefix(1);
  while (!s.empty() && std::isspace(uint8_t(s.back()))) s.remove_suffix(1);
  return s;
}

// No text to show: empty, or a placeholder the game's data kept - its locked
// texts of the secret achievements are all "<Translated text>".
bool IsPlaceholder(std::string_view s) {
  s = Trim(s);
  return s.empty() || (s.front() == '<' && s.back() == '>');
}

// One paragraph: the line breaks the dashboard's narrow box wanted become
// spaces, so the words wrap to the width they're drawn at.
std::string OneParagraph(std::string_view s) {
  std::string out;
  for (char c : Trim(s)) {
    if (c == '\r' || c == '\n' || c == '\t') {
      c = ' ';
    }
    if (c == ' ' && (out.empty() || out.back() == ' ')) {
      continue;
    }
    out += c;
  }
  return out;
}

constexpr const char* kHiddenText = "Hidden achievement - keep playing to unlock";

// What an achievement says: unlocked, its description; locked, the "how to
// unlock" text, or the description when that is a placeholder (the game's
// secret ones), or the hidden-achievement line when the game has neither.
std::string ShownDescription(const rex::system::AchievementInfo& a, bool unlocked) {
  const bool has_description = !IsPlaceholder(a.description);
  const bool has_hint = !IsPlaceholder(a.unachieved_description);
  if (unlocked) {
    return OneParagraph(has_description ? a.description
                        : has_hint      ? a.unachieved_description
                                        : std::string_view{});
  }
  if (has_hint) {
    return OneParagraph(a.unachieved_description);
  }
  return has_description ? OneParagraph(a.description) : kHiddenText;
}

// "2026-10-25" (local time) of a FILETIME, empty for none.
std::string UnlockDate(uint64_t filetime) {
  if (!filetime) {
    return {};
  }
  FILETIME utc{DWORD(filetime & 0xFFFFFFFFu), DWORD(filetime >> 32)};
  SYSTEMTIME st, local;
  if (!FileTimeToSystemTime(&utc, &st)) {
    return {};
  }
  if (SystemTimeToTzSpecificLocalTime(nullptr, &st, &local)) {
    st = local;
  }
  return fmt::format("{:04}-{:02}-{:02}", st.wYear, st.wMonth, st.wDay);
}

// ---------------------------------------------------------------- the sound

// kXbox is also the bronze chime; kSilver / kGold / kPlatinum are the tiers'
// own (brighter and longer versions of it).
enum class Chime { kXbox, kPlayStation, kSilver, kGold, kPlatinum };

Chime TierChime(Tier tier) {
  switch (tier) {
    case Tier::kSilver: return Chime::kSilver;
    case Tier::kGold: return Chime::kGold;
    case Tier::kPlatinum: return Chime::kPlatinum;
    default: return Chime::kXbox;
  }
}

const char* ChimeLabel(Chime chime) {
  switch (chime) {
    case Chime::kPlayStation: return "PlayStation-style chime";
    case Chime::kSilver: return "built-in silver chime";
    case Chime::kGold: return "built-in gold chime";
    case Chime::kPlatinum: return "built-in platinum fanfare";
    default: return "Xbox-style chime";
  }
}

constexpr int kRate = 48000;
constexpr float kPi = 3.14159265358979f;

// A decaying sine partial starting at `at` seconds.
void AddPartial(std::vector<float>& out, float at, float freq, float amp, float attack,
                float decay, float glide_to = 0.0f, float glide_time = 0.0f) {
  const size_t start = size_t(at * kRate);
  float phase = 0.0f;
  for (size_t i = start; i < out.size(); ++i) {
    const float t = float(i - start) / kRate;
    float f = freq;
    if (glide_to > 0.0f && glide_time > 0.0f) {
      const float k = std::min(t / glide_time, 1.0f);
      f = freq * std::pow(glide_to / freq, 1.0f - (1.0f - k) * (1.0f - k));
    }
    phase += 2.0f * kPi * f / kRate;
    const float env = std::min(t / attack, 1.0f) * std::exp(-t / decay);
    if (env < 1e-4f && t > attack) {
      break;
    }
    out[i] += amp * env * std::sin(phase);
  }
}

// A struck bell (two-operator FM, inharmonic ratio) at `at` seconds.
void AddBell(std::vector<float>& out, float at, float freq, float amp, float decay) {
  const size_t start = size_t(at * kRate);
  for (size_t i = start; i < out.size(); ++i) {
    const float t = float(i - start) / kRate;
    const float env = std::min(t / 0.003f, 1.0f) * std::exp(-t / decay);
    if (env < 1e-4f && t > 0.01f) {
      break;
    }
    const float index = 2.2f * std::exp(-t / (decay * 0.35f));
    const float mod = index * std::sin(2.0f * kPi * freq * 1.41f * t);
    out[i] += amp * env * std::sin(2.0f * kPi * freq * t + mod);
  }
}

// The chime, stereo 16-bit at 48 kHz, peak at `gain` of full scale.
std::vector<int16_t> MakeChime(Chime chime, float gain, float& seconds) {
  std::vector<float> mono;
  if (chime == Chime::kXbox) {
    // A soft "bloop": a low body, a quick upward glide, then a bright ping
    // a fifth above with a short shimmer.
    mono.assign(size_t(0.85f * kRate), 0.0f);
    AddPartial(mono, 0.000f, 150.0f, 0.30f, 0.004f, 0.045f);
    AddPartial(mono, 0.000f, 494.0f, 0.55f, 0.006f, 0.060f, 740.0f, 0.085f);
    AddPartial(mono, 0.000f, 988.0f, 0.12f, 0.006f, 0.050f, 1480.0f, 0.085f);
    AddPartial(mono, 0.085f, 1109.0f, 0.70f, 0.004f, 0.140f);
    AddPartial(mono, 0.085f, 2218.0f, 0.20f, 0.004f, 0.080f);
    AddPartial(mono, 0.085f, 3327.0f, 0.06f, 0.004f, 0.050f);
    AddPartial(mono, 0.150f, 1661.0f, 0.22f, 0.010f, 0.160f);
  } else if (chime == Chime::kPlayStation) {
    // A sparkle: two bells a fourth apart, then a faint high one, long tail.
    mono.assign(size_t(1.70f * kRate), 0.0f);
    AddBell(mono, 0.000f, 1397.0f, 0.55f, 0.42f);
    AddBell(mono, 0.095f, 1865.0f, 0.50f, 0.48f);
    AddBell(mono, 0.190f, 2794.0f, 0.18f, 0.30f);
    AddPartial(mono, 0.095f, 932.0f, 0.18f, 0.02f, 0.40f);
  } else if (chime == Chime::kSilver) {
    // The bloop a fourth higher, its ping held longer, and two small bells.
    mono.assign(size_t(1.25f * kRate), 0.0f);
    AddPartial(mono, 0.000f, 196.0f, 0.26f, 0.004f, 0.045f);
    AddPartial(mono, 0.000f, 659.0f, 0.50f, 0.006f, 0.060f, 988.0f, 0.085f);
    AddPartial(mono, 0.000f, 1319.0f, 0.10f, 0.006f, 0.050f, 1976.0f, 0.085f);
    AddPartial(mono, 0.085f, 1480.0f, 0.66f, 0.004f, 0.180f);
    AddPartial(mono, 0.085f, 2960.0f, 0.18f, 0.004f, 0.090f);
    AddPartial(mono, 0.150f, 2217.0f, 0.24f, 0.010f, 0.200f);
    AddBell(mono, 0.200f, 2960.0f, 0.22f, 0.30f);
    AddBell(mono, 0.280f, 3729.0f, 0.12f, 0.26f);
  } else if (chime == Chime::kGold) {
    // The bloop, then three bells rising (E G# B) over a warm fifth, and a
    // high sparkle.
    mono.assign(size_t(1.90f * kRate), 0.0f);
    AddPartial(mono, 0.000f, 165.0f, 0.30f, 0.004f, 0.060f);
    AddPartial(mono, 0.000f, 659.0f, 0.45f, 0.006f, 0.060f, 988.0f, 0.085f);
    AddPartial(mono, 0.085f, 1319.0f, 0.50f, 0.004f, 0.200f);
    AddBell(mono, 0.085f, 1319.0f, 0.30f, 0.45f);
    AddBell(mono, 0.175f, 1661.0f, 0.34f, 0.45f);
    AddBell(mono, 0.265f, 1976.0f, 0.38f, 0.55f);
    AddPartial(mono, 0.265f, 659.0f, 0.18f, 0.030f, 0.55f);
    AddPartial(mono, 0.265f, 988.0f, 0.12f, 0.030f, 0.50f);
    AddBell(mono, 0.400f, 2637.0f, 0.16f, 0.40f);
    AddBell(mono, 0.480f, 3322.0f, 0.10f, 0.35f);
  } else {
    // Platinum: a low swell, a rising arpeggio of bells (C E G C), a held
    // major chord with sparkles over it, and a last high "ting".
    mono.assign(size_t(3.20f * kRate), 0.0f);
    AddPartial(mono, 0.000f, 131.0f, 0.30f, 0.010f, 0.100f);
    const float notes[] = {1047.0f, 1319.0f, 1568.0f, 2093.0f};
    for (int i = 0; i < 4; ++i) {
      AddBell(mono, 0.10f * float(i), notes[i], 0.32f, 0.50f);
      AddPartial(mono, 0.10f * float(i), notes[i], 0.26f, 0.004f, 0.16f);
    }
    for (float f : {523.0f, 659.0f, 784.0f, 1047.0f}) {
      AddPartial(mono, 0.38f, f, 0.15f, 0.060f, 0.90f);
    }
    AddBell(mono, 0.55f, 3136.0f, 0.14f, 0.60f);
    AddBell(mono, 0.75f, 4186.0f, 0.10f, 0.50f);
    AddBell(mono, 0.95f, 2637.0f, 0.12f, 0.70f);
    AddBell(mono, 1.30f, 2093.0f, 0.20f, 0.90f);
  }
  // A little room: two echoes, the right side a few ms behind the left.
  std::vector<float> left(mono.size()), right(mono.size());
  const size_t e1 = size_t(0.071f * kRate), e2 = size_t(0.133f * kRate), d = size_t(0.009f * kRate);
  for (size_t i = 0; i < mono.size(); ++i) {
    float l = mono[i], r = i >= d ? mono[i - d] : 0.0f;
    if (i >= e1) {
      l += 0.22f * mono[i - e1];
      r += 0.18f * mono[i - e1 + std::min(d, e1)];
    }
    if (i >= e2) {
      l += 0.10f * mono[i - e2];
      r += 0.13f * mono[i - e2];
    }
    left[i] = l;
    right[i] = r;
  }
  float peak = 1e-6f;
  for (size_t i = 0; i < mono.size(); ++i) {
    peak = std::max(peak, std::max(std::fabs(left[i]), std::fabs(right[i])));
  }
  const float scale = gain / peak;
  std::vector<int16_t> pcm(mono.size() * 2);
  // A fade at the very end: 2 ms, 250 ms for the tiers' longer tails.
  const float fade =
      (chime == Chime::kXbox || chime == Chime::kPlayStation ? 0.002f : 0.25f) * kRate;
  for (size_t i = 0; i < mono.size(); ++i) {
    const float tail = std::min(1.0f, float(mono.size() - i) / fade);
    pcm[i * 2] = int16_t(std::lround(std::clamp(left[i] * scale * tail, -1.0f, 1.0f) * 32767.0f));
    pcm[i * 2 + 1] =
        int16_t(std::lround(std::clamp(right[i] * scale * tail, -1.0f, 1.0f) * 32767.0f));
  }
  seconds = float(mono.size()) / kRate;
  return pcm;
}

bool WriteWav(const std::filesystem::path& path, const std::vector<int16_t>& pcm) {
  std::ofstream file(path, std::ios::binary);
  if (!file) {
    return false;
  }
  auto u32 = [&](uint32_t v) { file.write(reinterpret_cast<const char*>(&v), 4); };
  auto u16 = [&](uint16_t v) { file.write(reinterpret_cast<const char*>(&v), 2); };
  const uint32_t bytes = uint32_t(pcm.size() * 2);
  file.write("RIFF", 4);
  u32(36 + bytes);
  file.write("WAVEfmt ", 8);
  u32(16);
  u16(1);
  u16(2);
  u32(kRate);
  u32(kRate * 4);
  u16(4);
  u16(16);
  file.write("data", 4);
  u32(bytes);
  file.write(reinterpret_cast<const char*>(pcm.data()), bytes);
  return bool(file);
}

// The game's Sound Effects volume (Options > Sound, 0-7) as the factor the
// game itself uses for its effects (table at 0x82520268); 1 without a save.
float GameEffectsVolume(int* level_out = nullptr) {
  rex::memory::Memory* memory = GuestMemory();
  if (level_out) {
    *level_out = -1;
  }
  if (!memory) {
    return 1.0f;
  }
  const uint32_t save = R32(memory, 0x825205E8);
  if (!Readable(memory, save, 32)) {
    return 1.0f;
  }
  const uint8_t level = *memory->TranslateVirtual<uint8_t*>(save + 21);
  if (level > 7) {
    return 1.0f;
  }
  if (level_out) {
    *level_out = level;
  }
  const uint32_t bits = R32(memory, 0x82520268 + level * 4);
  float factor;
  std::memcpy(&factor, &bits, 4);
  return std::isfinite(factor) ? std::clamp(factor, 0.0f, 1.0f) : 1.0f;
}

class ChimePlayer {
 public:
  ~ChimePlayer() { Stop(); }

  void Play(std::vector<int16_t> pcm) {
    std::lock_guard<std::mutex> lock(mutex_);
    StopLocked();
    pcm_ = std::move(pcm);
    WAVEFORMATEX format = {};
    format.wFormatTag = WAVE_FORMAT_PCM;
    format.nChannels = 2;
    format.nSamplesPerSec = kRate;
    format.wBitsPerSample = 16;
    format.nBlockAlign = 4;
    format.nAvgBytesPerSec = kRate * 4;
    if (waveOutOpen(&device_, WAVE_MAPPER, &format, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) {
      device_ = nullptr;
      REXLOG_WARN("Achievement sound: no audio output");
      return;
    }
    header_ = {};
    header_.lpData = reinterpret_cast<LPSTR>(pcm_.data());
    header_.dwBufferLength = DWORD(pcm_.size() * 2);
    if (waveOutPrepareHeader(device_, &header_, sizeof(header_)) != MMSYSERR_NOERROR ||
        waveOutWrite(device_, &header_, sizeof(header_)) != MMSYSERR_NOERROR) {
      REXLOG_WARN("Achievement sound: the output didn't take it");
    }
  }

  // Samples the output device has played of the current chime.
  uint32_t PlayedSamples() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!device_) {
      return 0;
    }
    MMTIME time = {};
    time.wType = TIME_SAMPLES;
    return waveOutGetPosition(device_, &time, sizeof(time)) == MMSYSERR_NOERROR &&
                   time.wType == TIME_SAMPLES
               ? time.u.sample
               : 0;
  }

  void Stop() {
    std::lock_guard<std::mutex> lock(mutex_);
    StopLocked();
  }

 private:
  void StopLocked() {
    if (!device_) {
      return;
    }
    waveOutReset(device_);
    waveOutUnprepareHeader(device_, &header_, sizeof(header_));
    waveOutClose(device_);
    device_ = nullptr;
  }

  std::mutex mutex_;
  HWAVEOUT device_ = nullptr;
  WAVEHDR header_ = {};
  std::vector<int16_t> pcm_;
};

ChimePlayer& Player() {
  static ChimePlayer player;
  return player;
}

std::optional<Chime> ChimeFromName(std::string_view name) {
  if (name == "xbox") {
    return Chime::kXbox;
  }
  if (name == "playstation") {
    return Chime::kPlayStation;
  }
  return std::nullopt;
}

// ---------------------------------------------------------------- a custom sound file

std::filesystem::path ExeDirectory() {
  wchar_t buffer[MAX_PATH] = {};
  const DWORD n = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
  return n ? std::filesystem::path(std::wstring(buffer, n)).parent_path() : std::filesystem::path();
}

// achievement_sound_file, else sounds\achievement.wav / .mp3 next to the exe.
std::filesystem::path CustomSoundPath() {
  std::error_code ec;
  const std::string& setting = REXCVAR_GET(achievement_sound_file);
  if (!setting.empty()) {
    std::filesystem::path path = std::filesystem::u8path(setting);
    if (path.is_relative()) {
      path = ExeDirectory() / path;
    }
    return std::filesystem::exists(path, ec) ? path : std::filesystem::path();
  }
  for (const wchar_t* name : {L"achievement.wav", L"achievement.mp3"}) {
    const std::filesystem::path path = ExeDirectory() / L"sounds" / name;
    if (std::filesystem::exists(path, ec)) {
      return path;
    }
  }
  return {};
}

struct DecodedSound {
  std::filesystem::path path;
  std::filesystem::file_time_type time{};
  std::vector<float> stereo;  // 48 kHz, interleaved L R, -1..1
  uint32_t source_rate = 0;
  uint32_t source_channels = 0;
};

// Any file Media Foundation reads (WAV PCM / float, MP3, ...) as PCM in its
// own rate and channels, then to 48 kHz stereo (linear resampling).
bool DecodeSoundFile(const std::filesystem::path& path, DecodedSound& out, std::string& error) {
  const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_LITE))) {
    error = "Media Foundation didn't start";
    if (SUCCEEDED(co)) CoUninitialize();
    return false;
  }
  bool ok = false;
  IMFSourceReader* reader = nullptr;
  IMFMediaType* want = nullptr;
  IMFMediaType* got = nullptr;
  std::vector<int16_t> pcm;
  uint32_t rate = 0, channels = 0;
  do {
    if (FAILED(MFCreateSourceReaderFromURL(path.wstring().c_str(), nullptr, &reader))) {
      error = "can't open it";
      break;
    }
    reader->SetStreamSelection(DWORD(MF_SOURCE_READER_ALL_STREAMS), FALSE);
    reader->SetStreamSelection(DWORD(MF_SOURCE_READER_FIRST_AUDIO_STREAM), TRUE);
    MFCreateMediaType(&want);
    want->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    want->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
    want->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    bool is_float = false;
    if (FAILED(reader->SetCurrentMediaType(DWORD(MF_SOURCE_READER_FIRST_AUDIO_STREAM), nullptr,
                                           want))) {
      // A float WAV: no decoder in between, so take it as float.
      want->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_Float);
      want->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 32);
      is_float = SUCCEEDED(reader->SetCurrentMediaType(DWORD(MF_SOURCE_READER_FIRST_AUDIO_STREAM),
                                                       nullptr, want));
    }
    if (FAILED(reader->GetCurrentMediaType(DWORD(MF_SOURCE_READER_FIRST_AUDIO_STREAM), &got))) {
      error = "no audio stream it can decode";
      break;
    }
    got->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &rate);
    got->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &channels);
    if (!rate || !channels) {
      error = "unknown audio format";
      break;
    }
    while (pcm.size() < size_t(rate) * channels * 30) {  // at most 30 s
      DWORD flags = 0;
      IMFSample* sample = nullptr;
      if (FAILED(reader->ReadSample(DWORD(MF_SOURCE_READER_FIRST_AUDIO_STREAM), 0, nullptr, &flags,
                                    nullptr, &sample))) {
        break;
      }
      if (sample) {
        IMFMediaBuffer* buffer = nullptr;
        if (SUCCEEDED(sample->ConvertToContiguousBuffer(&buffer))) {
          BYTE* data = nullptr;
          DWORD length = 0;
          if (SUCCEEDED(buffer->Lock(&data, nullptr, &length))) {
            const size_t at = pcm.size();
            if (is_float) {
              const size_t count = length / 4;
              pcm.resize(at + count);
              for (size_t i = 0; i < count; ++i) {
                float v;
                std::memcpy(&v, data + i * 4, 4);
                pcm[at + i] = int16_t(std::lround(std::clamp(v, -1.0f, 1.0f) * 32767.0f));
              }
            } else {
              const size_t count = length / 2;
              pcm.resize(at + count);
              std::memcpy(pcm.data() + at, data, count * 2);
            }
            buffer->Unlock();
          }
          buffer->Release();
        }
        sample->Release();
      }
      if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
        break;
      }
    }
    ok = !pcm.empty();
    if (!ok) {
      error = "no audio in it";
    }
  } while (false);
  if (got) got->Release();
  if (want) want->Release();
  if (reader) reader->Release();
  MFShutdown();
  if (SUCCEEDED(co)) CoUninitialize();
  if (!ok) {
    return false;
  }
  const size_t frames = pcm.size() / channels;
  const size_t out_frames = size_t(double(frames) * kRate / rate);
  out.stereo.assign(out_frames * 2, 0.0f);
  auto at = [&](size_t frame, uint32_t ch) {
    frame = std::min(frame, frames - 1);
    return float(pcm[frame * channels + std::min(ch, channels - 1)]) / 32768.0f;
  };
  for (size_t i = 0; i < out_frames; ++i) {
    const double src = double(i) * rate / kRate;
    const size_t f = size_t(src);
    const float k = float(src - double(f));
    for (uint32_t ch = 0; ch < 2; ++ch) {
      out.stereo[i * 2 + ch] = at(f, ch) * (1.0f - k) + at(f + 1, ch) * k;
    }
  }
  out.source_rate = rate;
  out.source_channels = channels;
  return true;
}

// A sound file at `gain` x its own level (decoded once per file and change;
// pcm empty = only decode it).
bool FileSound(const std::filesystem::path& path, float gain, std::vector<int16_t>* pcm,
               float& seconds, std::string& what) {
  static std::mutex mutex;
  static std::map<std::filesystem::path, DecodedSound> cache;  // the custom and tier files
  std::lock_guard<std::mutex> lock(mutex);
  std::error_code ec;
  const auto time = std::filesystem::last_write_time(path, ec);
  auto it = cache.find(path);
  if (it == cache.end() || it->second.time != time) {
    DecodedSound decoded;
    std::string error;
    if (!DecodeSoundFile(path, decoded, error)) {
      what = path.string() + ": " + error;
      cache.erase(path);
      return false;
    }
    decoded.path = path;
    decoded.time = time;
    it = cache.insert_or_assign(path, std::move(decoded)).first;
  }
  const DecodedSound& sound = it->second;
  if (pcm) {
    pcm->resize(sound.stereo.size());
    for (size_t i = 0; i < pcm->size(); ++i) {
      (*pcm)[i] =
          int16_t(std::lround(std::clamp(sound.stereo[i] * gain, -1.0f, 1.0f) * 32767.0f));
    }
  }
  seconds = float(sound.stereo.size() / 2) / kRate;
  what = fmt::format("{} ({} Hz, {} ch)", path.filename().string(), sound.source_rate,
                     sound.source_channels);
  return true;
}

// The custom sound at `gain` x its own level.
bool CustomSound(float gain, std::vector<int16_t>& pcm, float& seconds, std::string& what) {
  const std::filesystem::path path = CustomSoundPath();
  if (path.empty()) {
    what = "no custom sound file (achievement_sound_file, or sounds\\achievement.wav / .mp3)";
    return false;
  }
  if (!FileSound(path, gain, &pcm, seconds, what)) {
    return false;
  }
  what = "custom file " + what;
  return true;
}

const std::string& TierSoundSetting(Tier tier) {
  switch (tier) {
    case Tier::kSilver: return REXCVAR_GET(achievement_sound_silver);
    case Tier::kGold: return REXCVAR_GET(achievement_sound_gold);
    case Tier::kPlatinum: return REXCVAR_GET(achievement_sound_platinum);
    default: return REXCVAR_GET(achievement_sound_bronze);
  }
}

// The tier's sound file (achievement_sound_<tier>, relative to the exe's
// folder); empty with `why` set when it's not set or not there.
std::filesystem::path TierSoundPath(Tier tier, std::string& why) {
  const std::string& setting = TierSoundSetting(tier);
  if (setting.empty()) {
    why = fmt::format("achievement_sound_{} is empty", TierName(tier));
    return {};
  }
  std::filesystem::path path = std::filesystem::u8path(setting);
  if (path.is_relative()) {
    path = ExeDirectory() / path;
  }
  std::error_code ec;
  if (!std::filesystem::is_regular_file(path, ec)) {
    why = fmt::format("{} not found (achievement_sound_{})", path.string(), TierName(tier));
    return {};
  }
  return path;
}

// The tier's sound at `gain`: its file, else its built-in chime (`fallback`).
bool TierSound(Tier tier, float gain, std::vector<int16_t>& pcm, float& seconds, std::string& what,
               std::string& fallback) {
  const std::filesystem::path path = TierSoundPath(tier, fallback);
  if (path.empty()) {
    return false;
  }
  if (!FileSound(path, gain, &pcm, seconds, what)) {
    fallback = what;
    return false;
  }
  what = "tier file " + what;
  return true;
}

// Decodes the tier files ahead (a worker thread at start), so the first popup
// of each doesn't wait for Media Foundation.
void PreloadTierSounds() {
  std::string line;
  for (Tier tier : {Tier::kBronze, Tier::kSilver, Tier::kGold, Tier::kPlatinum}) {
    std::string why, what;
    float seconds = 0.0f;
    const std::filesystem::path path = TierSoundPath(tier, why);
    if (!path.empty() && FileSound(path, 1.0f, nullptr, seconds, what)) {
      line += fmt::format("{}{} {} {:.2f} s", line.empty() ? "" : ", ", TierName(tier), what, seconds);
    } else {
      line += fmt::format("{}{} built-in ({})", line.empty() ? "" : ", ", TierName(tier),
                          path.empty() ? why : what);
    }
  }
  REXLOG_INFO("Achievement sound: tier sounds: {}", line);
}

// One of the game's own sounds ("BANK:cue") at `gain`, 48 kHz stereo
// (decoded once per cue and change).
bool GameSound(const std::string& key, float gain, std::vector<int16_t>& pcm, float& seconds,
               std::string& what) {
  static std::mutex mutex;
  static std::string cached_key;
  static std::vector<float> cached;  // 48 kHz mono
  std::lock_guard<std::mutex> lock(mutex);
  if (cached_key != key || cached.empty()) {
    std::vector<float> mono;
    uint32_t rate = 0;
    if (!BurstLimitDecodeGameCue(key, mono, rate, what) || !rate) {
      cached_key.clear();
      cached.clear();
      return false;
    }
    const size_t out_frames = size_t(double(mono.size()) * kRate / rate);
    cached.assign(out_frames, 0.0f);
    for (size_t i = 0; i < out_frames; ++i) {
      const double src = double(i) * rate / kRate;
      const size_t f = std::min(size_t(src), mono.size() - 1);
      const size_t g = std::min(f + 1, mono.size() - 1);
      const float k = float(src - double(f));
      cached[i] = mono[f] * (1.0f - k) + mono[g] * k;
    }
    cached_key = key;
    REXLOG_INFO("Achievement sound: game sound {} ({} Hz, {:.2f} s)", what, rate,
                float(mono.size()) / rate);
  }
  pcm.resize(cached.size() * 2);
  for (size_t i = 0; i < cached.size(); ++i) {
    const int16_t v = int16_t(std::lround(std::clamp(cached[i] * gain, -1.0f, 1.0f) * 32767.0f));
    pcm[i * 2] = v;
    pcm[i * 2 + 1] = v;
  }
  seconds = float(cached.size()) / kRate;
  what = "game sound " + key;
  return true;
}

// Makes the sound of a popup of `tier` and plays it (or only logs it while
// the game is muted); returns its length in seconds (0 = none). `mode`: one
// of achievement_sound's values instead of the setting (previews);
// `game_key`: that game sound with mode "game".
float PlayAchievementSound(Tier tier, std::string mode = {}, std::string game_key = {}) {
  if (mode.empty()) {
    mode = REXCVAR_GET(achievement_sound);
  }
  if (mode == "game" && game_key.empty()) {
    game_key = REXCVAR_GET(achievement_game_sound);
  }
  std::optional<Chime> chime = ChimeFromName(mode);
  if (!chime && mode != "tiers" && mode != "custom" && mode != "game") {
    return 0.0f;  // off
  }
  int level = -1;
  const float game_volume = GameEffectsVolume(&level);
  const float volume = float(std::clamp(REXCVAR_GET(achievement_volume), 0, 100)) / 100.0f;
  float seconds = 0.0f;
  std::vector<int16_t> pcm;
  std::string what;
  float gain = volume * game_volume;
  if (mode == "tiers") {
    std::string fallback;
    if (!TierSound(tier, gain, pcm, seconds, what, fallback)) {
      REXLOG_WARN("Achievement sound: {} - the {} instead", fallback, ChimeLabel(TierChime(tier)));
      chime = TierChime(tier);
    }
  } else if (mode == "custom" && !CustomSound(gain, pcm, seconds, what)) {
    REXLOG_WARN("Achievement sound: {} - the Xbox-style chime instead", what);
    chime = Chime::kXbox;
  } else if (mode == "game" && !GameSound(game_key, gain, pcm, seconds, what)) {
    REXLOG_WARN("Achievement sound: {} - the Xbox-style chime instead", what);
    chime = Chime::kXbox;
  }
  if (pcm.empty()) {
    gain *= 0.89f;
    pcm = MakeChime(*chime, std::max(gain, 0.0f), seconds);
    what = ChimeLabel(*chime);
  }
  int peak = 0;
  double sum = 0.0;
  for (int16_t s : pcm) {
    peak = std::max(peak, std::abs(int(s)));
    sum += double(s) * double(s);
  }
  const double rms = pcm.empty() ? 0.0 : std::sqrt(sum / double(pcm.size()));
  auto db = [](double v) { return v > 0.5 ? 20.0 * std::log10(v / 32767.0) : -120.0; };
  const bool muted = rex::cvar::Query<bool>("audio_mute");
  const std::string& dump = REXCVAR_GET(achievement_sound_dump);
  if (!dump.empty()) {
    const bool ok = WriteWav(std::filesystem::path(dump), pcm);
    REXLOG_INFO("Achievement sound: written to {}{}", dump, ok ? "" : " (failed)");
  }
  REXLOG_INFO(
      "Achievement sound: {} trophy ({}): {}, {:.2f} s, peak {:.1f} dBFS, RMS {:.1f} dBFS (volume "
      "{}% x the game's effects volume {} = {:.2f}){}",
      TierName(tier), mode, what, seconds, db(peak), db(rms), REXCVAR_GET(achievement_volume),
      level >= 0 ? std::to_string(level) + "/7" : "(no save)", game_volume,
      muted ? " - not played: audio_mute is on" : "");
  rex::FlushLogging();
  if (!muted && gain > 0.0f) {
    Player().Play(std::move(pcm));
  }
  return seconds;
}

// ---------------------------------------------------------------- the popup

enum class Anchor { kTopLeft, kTop, kTopRight, kBottomLeft, kBottom, kBottomRight };

Anchor AnchorFromCvar() {
  const std::string& v = REXCVAR_GET(achievement_popup_position);
  if (v == "top_left") return Anchor::kTopLeft;
  if (v == "top") return Anchor::kTop;
  if (v == "top_right") return Anchor::kTopRight;
  if (v == "bottom_left") return Anchor::kBottomLeft;
  if (v == "bottom_right") return Anchor::kBottomRight;
  return Anchor::kBottom;
}

constexpr float kSlideIn = 0.40f;
constexpr float kHold = 3.90f;
constexpr float kHoldPlatinum = 5.60f;  // the platinum's sound is longer
constexpr float kSlideOut = 0.35f;
constexpr float kGap = 0.15f;
constexpr size_t kMaxQueued = 4;  // more waiting -> folded into one "N more" popup

class AchievementToast;
std::mutex g_toast_mutex;
AchievementToast* g_toast = nullptr;

// The id whose unlock left none locked, set by the unlock callback for the
// popup that follows it (the same thread, right after).
std::atomic<uint32_t> g_platinum_id{0};

// The manager's unlock callback: only new unlocks come here (not the ones
// read back from the profile's file, nor ones already unlocked), so the
// platinum pops once, at the unlock that completes the list.
void OnAchievementUnlocked(const AchievementEvent& event) {
  auto* manager = Manager();
  if (!manager) {
    return;
  }
  const auto list = manager->ListAchievements();
  if (list.empty()) {
    return;
  }
  for (const auto& a : list) {
    if (!manager->IsUnlocked(a.id)) {
      return;
    }
  }
  REXLOG_INFO("Achievement platinum: {} \"{}\" was the last of the {} - every achievement unlocked",
              event.achievement.id, event.achievement.label, list.size());
  g_platinum_id = event.achievement.id;
}

// The platinum popup's text (for a preview too): all of the game's achievements.
AchievementEvent PlatinumEvent() {
  uint32_t count = 0, total = 0;
  if (auto* manager = Manager()) {
    for (const auto& a : manager->ListAchievements()) {
      ++count;
      total += a.gamerscore;
    }
  }
  AchievementEvent event;
  event.achievement.label = "Platinum";
  event.achievement.description =
      count ? fmt::format("All {} of the game's achievements unlocked ({} G). Congratulations!",
                          count, total)
            : "All of the game's achievements unlocked. Congratulations!";
  return event;
}

// ---------------------------------------------------------------- tier art

struct TierColors {
  ImU32 main, dark, shine;
};

TierColors ColorsOf(Tier tier, float alpha) {
  const int a = int(255 * alpha);
  switch (tier) {
    case Tier::kBronze:
      return {IM_COL32(214, 132, 62, a), IM_COL32(118, 62, 22, a), IM_COL32(255, 214, 176, a)};
    case Tier::kSilver:
      return {IM_COL32(206, 212, 224, a), IM_COL32(106, 112, 128, a), IM_COL32(255, 255, 255, a)};
    case Tier::kPlatinum:
      return {IM_COL32(178, 222, 255, a), IM_COL32(70, 118, 168, a), IM_COL32(255, 255, 255, a)};
    default:
      return {IM_COL32(255, 200, 40, a), IM_COL32(150, 96, 10, a), IM_COL32(255, 245, 200, a)};
  }
}

float EaseOut(float t) { return 1.0f - std::pow(1.0f - std::clamp(t, 0.0f, 1.0f), 3.0f); }

// A trophy cup in the tier's colours (the title bar's, and the picture of
// achievements without one).
void DrawTrophy(ImDrawList* dl, ImVec2 min, float size, float alpha,
                       Tier tier = Tier::kGold) {
  const TierColors colors = ColorsOf(tier, alpha);
  const ImU32 gold = colors.main;
  const ImU32 dark = colors.dark;
  const float s = size;
  const ImVec2 c(min.x + s * 0.5f, min.y);
  dl->AddRectFilled(ImVec2(c.x - s * 0.24f, min.y + s * 0.16f), ImVec2(c.x + s * 0.24f, min.y + s * 0.40f),
                    gold, s * 0.04f);
  dl->AddCircleFilled(ImVec2(c.x, min.y + s * 0.40f), s * 0.24f, gold, 24);
  dl->AddCircle(ImVec2(c.x - s * 0.27f, min.y + s * 0.32f), s * 0.10f, gold, 16, s * 0.045f);
  dl->AddCircle(ImVec2(c.x + s * 0.27f, min.y + s * 0.32f), s * 0.10f, gold, 16, s * 0.045f);
  dl->AddRectFilled(ImVec2(c.x - s * 0.05f, min.y + s * 0.60f), ImVec2(c.x + s * 0.05f, min.y + s * 0.74f), gold);
  dl->AddRectFilled(ImVec2(c.x - s * 0.20f, min.y + s * 0.74f), ImVec2(c.x + s * 0.20f, min.y + s * 0.84f), gold,
                    s * 0.02f);
  dl->AddRectFilled(ImVec2(c.x - s * 0.26f, min.y + s * 0.84f), ImVec2(c.x + s * 0.26f, min.y + s * 0.92f), dark,
                    s * 0.02f);
  dl->AddCircleFilled(ImVec2(c.x - s * 0.08f, min.y + s * 0.32f), s * 0.05f,
                      (colors.shine & ~IM_COL32_A_MASK) | (ImU32(200 * alpha) << IM_COL32_A_SHIFT),
                      12);
}

// The gamerscore's "G" coin centered at `center` (the popup's and the list's).
void DrawCoin(ImDrawList* dl, ImVec2 center, float r, float u, float alpha) {
  dl->AddCircleFilled(center, r, IM_COL32(255, 214, 60, int(255 * alpha)), 24);
  dl->AddCircle(center, r, IM_COL32(150, 96, 10, int(255 * alpha)), 24, 2.0f * u);
  // The "G" in the overlay font (the menu font's outline hides it this small).
  ImFont* font = ImGui::GetFont();
  const float fs = r * 19.0f / 12.0f;
  const ImVec2 gs = font->CalcTextSizeA(fs, FLT_MAX, 0.0f, "G");
  dl->AddText(font, fs, ImVec2(center.x - gs.x * 0.5f, center.y - gs.y * 0.5f),
              IM_COL32(70, 36, 0, int(255 * alpha)), "G");
}

class AchievementToast final : public rex::ui::AchievementNotificationDialog {
 public:
  AchievementToast(rex::ui::ImGuiDrawer* drawer, rex::ui::ImmediateDrawer* immediate_drawer)
      : AchievementNotificationDialog(drawer),
        immediate_drawer_(immediate_drawer),
        icons_(immediate_drawer, rex::Runtime::instance()) {
    if (auto* manager = Manager()) {
      unlock_listener_ = manager->RegisterUnlockCallback(OnAchievementUnlocked);
    }
    if (REXCVAR_GET(achievement_sound) == "tiers") {
      std::thread(PreloadTierSounds).detach();
    }
    std::lock_guard<std::mutex> lock(g_toast_mutex);
    g_toast = this;
  }

  ~AchievementToast() override {
    if (unlock_listener_) {
      if (auto* manager = Manager()) {
        manager->UnregisterCallback(unlock_listener_);
      }
    }
    std::lock_guard<std::mutex> lock(g_toast_mutex);
    if (g_toast == this) {
      g_toast = nullptr;
    }
  }

  // Any thread (the game's, from XGIUserWriteAchievements): a real unlock,
  // followed by the platinum when it was the last one.
  void Push(const AchievementEvent& event) override {
    uint32_t last = event.achievement.id;
    Add(event, g_platinum_id.compare_exchange_strong(last, 0));
  }

  // A popup (`then_platinum`: the platinum after it). Tests come here directly.
  void Add(const AchievementEvent& event, bool then_platinum) {
    const Tier tier = TierOf(event.achievement.gamerscore);
    REXLOG_INFO("Achievement popup: {} \"{}\" ({}G, {}){}{}", event.achievement.id,
                event.achievement.label, event.achievement.gamerscore, TierName(tier),
                then_platinum ? " - then the platinum" : "",
                REXCVAR_GET(achievement_popups) ? "" : " - popups are off");
    if (!REXCVAR_GET(achievement_popups)) {
      const float seconds = PlayAchievementSound(tier);
      if (then_platinum) {
        // The platinum's sound after this one.
        std::thread([seconds] {
          std::this_thread::sleep_for(std::chrono::milliseconds(int(seconds * 1000.0f) + 400));
          PlayAchievementSound(Tier::kPlatinum);
        }).detach();
      }
      return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (queue_.size() >= kMaxQueued && !queue_.back().platinum) {
      // Fold the rest of a burst into one popup, of the highest tier in it.
      Entry& last = queue_.back();
      if (last.more == 0) {
        last.more_gamerscore = last.event.achievement.gamerscore;
        last.more = 1;
      }
      ++last.more;
      last.more_gamerscore += event.achievement.gamerscore;
      last.tier = std::max(last.tier, tier);
    } else {
      Entry entry{event};
      entry.tier = tier;
      queue_.push_back(std::move(entry));
    }
    if (then_platinum) {
      AddPlatinumLocked();
    }
  }

  // The platinum popup alone (a preview).
  void AddPlatinum() {
    REXLOG_INFO("Achievement popup: platinum{}",
                REXCVAR_GET(achievement_popups) ? "" : " - popups are off");
    if (!REXCVAR_GET(achievement_popups)) {
      PlayAchievementSound(Tier::kPlatinum);
      return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    AddPlatinumLocked();
  }

 protected:
  void OnDraw(ImGuiIO& io) override {
    ImGui::SetCurrentContext(io.Ctx);
    const auto now = std::chrono::steady_clock::now();
    LogTiersOnce(now);
    Entry entry;
    float age = 0.0f;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      while (!queue_.empty()) {
        Entry& front = queue_.front();
        if (!front.shown) {
          if (now < next_allowed_) {
            return;
          }
          front.shown = now;
          front.sound_pending = true;
        }
        age = std::chrono::duration<float>(now - *front.shown).count();
        if (age < kSlideIn + Hold(front) + kSlideOut) {
          break;
        }
        next_allowed_ = now + std::chrono::milliseconds(int(kGap * 1000.0f));
        queue_.pop_front();
      }
      if (queue_.empty()) {
        return;
      }
      entry = queue_.front();
      queue_.front().sound_pending = false;
    }
    if (entry.sound_pending) {
      PlayAchievementSound(entry.platinum ? Tier::kPlatinum : entry.tier);
    }
    Draw(io, entry, age);
  }

 private:
  struct Entry {
    AchievementEvent event;
    Tier tier = Tier::kBronze;
    bool platinum = false;  // the "every achievement unlocked" popup
    std::optional<std::chrono::steady_clock::time_point> shown;
    bool sound_pending = false;
    uint32_t more = 0;  // > 0: this popup stands for `more` unlocks
    uint32_t more_gamerscore = 0;
  };

  static float Hold(const Entry& entry) { return entry.platinum ? kHoldPlatinum : kHold; }

  void AddPlatinumLocked() {
    Entry entry{PlatinumEvent()};
    entry.tier = Tier::kPlatinum;
    entry.platinum = true;
    queue_.push_back(std::move(entry));
  }

  // The tiers of the game's achievements, once they are read (at sign-in).
  void LogTiersOnce(std::chrono::steady_clock::time_point now) {
    if (tiers_logged_ || now < next_tier_check_) {
      return;
    }
    next_tier_check_ = now + std::chrono::seconds(1);
    if (auto* manager = Manager()) {
      const auto list = manager->ListAchievements();
      if (!list.empty()) {
        tiers_logged_ = true;
        REXLOG_INFO("Achievement tiers: {}", TierSummary(list));
      }
    }
  }

  void Draw(ImGuiIO& io, const Entry& entry, float age) {
    BurstLimitMenuArt* art = BurstLimitSharedMenuArt();
    if (art && !art->Ready(immediate_drawer_)) {
      art = nullptr;
    }
    // The game's 16:9 picture inside the window, 1080p units.
    const float pic_w = std::min(io.DisplaySize.x, io.DisplaySize.y * 16.0f / 9.0f);
    const float pic_h = pic_w * 9.0f / 16.0f;
    const ImVec2 origin((io.DisplaySize.x - pic_w) * 0.5f, (io.DisplaySize.y - pic_h) * 0.5f);
    const float u = pic_h / 1080.0f;
    const auto& a = entry.event.achievement;
    const bool folded = entry.more > 0;
    std::string name = folded ? fmt::format("{} achievements", entry.more) : a.label;
    std::string description =
        folded ? fmt::format("\"{}\" and {} more. F7 lists them all.", a.label,
                             entry.more - 1)
               : a.description;
    const float w = 640.0f * u, margin = 44.0f * u;
    const float header = 40.0f * u;
    const float pad = 18.0f * u;
    const float icon = 76.0f * u;
    const float tw = w - pad - (pad + icon + 18.0f * u);
    // The description in up to 2 lines, smaller words for a third when needed.
    float ds = 21.0f * u;
    std::vector<std::string> lines = art ? art->Wrap(ds, description, tw)
                                         : std::vector<std::string>{description};
    if (lines.size() > 2) {
      ds = 18.0f * u;
      lines = art->Wrap(ds, description, tw);
      if (lines.size() > 3) {
        lines.resize(3);
        lines[2] += "...";
      }
    }
    const float body = std::max(icon + 20.0f * u,
                                8.0f * u + 32.0f * u + 6.0f * u + float(lines.size()) * (ds + 3.0f * u) +
                                    8.0f * u);
    const float h = header + body;
    const Anchor anchor = AnchorFromCvar();
    const bool top = anchor == Anchor::kTopLeft || anchor == Anchor::kTop || anchor == Anchor::kTopRight;
    const bool left = anchor == Anchor::kTopLeft || anchor == Anchor::kBottomLeft;
    const bool center = anchor == Anchor::kTop || anchor == Anchor::kBottom;
    // Top positions sit under the fight's life gauges.
    const float y_rest = top ? origin.y + 190.0f * u : origin.y + pic_h - 250.0f * u - h;
    const float x_rest = center ? origin.x + (pic_w - w) * 0.5f
                                : left ? origin.x + margin : origin.x + pic_w - margin - w;
    // Slide in, hold, slide out (sideways at the sides, up / down in the middle).
    const float hold = Hold(entry);
    float k;
    if (age < kSlideIn) {
      k = EaseOut(age / kSlideIn);
    } else if (age < kSlideIn + hold) {
      k = 1.0f;
    } else {
      const float t = (age - kSlideIn - hold) / kSlideOut;
      k = 1.0f - std::pow(std::clamp(t, 0.0f, 1.0f), 2.0f);
    }
    const float alpha = std::clamp(k * 1.4f, 0.0f, 1.0f);
    float x = x_rest, y = y_rest;
    if (center) {
      y += (1.0f - k) * (top ? -1.0f : 1.0f) * (h * 0.8f);
    } else {
      x += (1.0f - k) * (left ? -1.0f : 1.0f) * (w + margin);
    }
    const ImVec2 min(x, y), max(x + w, y + h);
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const ImU32 white = IM_COL32(255, 255, 255, int(255 * alpha));
    const ImU32 soft = IM_COL32(222, 212, 238, int(255 * alpha));
    const ImU32 beige = IM_COL32(232, 222, 196, int(255 * alpha));
    auto text = [&](ImVec2 p, float size, std::string_view s, ImU32 color) -> float {
      if (art) {
        return art->Text(dl, p, size, s, color);
      }
      ImFont* font = ImGui::GetFont();
      dl->AddText(font, size * 0.8f, p, color, s.data(), s.data() + s.size());
      return font->CalcTextSizeA(size * 0.8f, FLT_MAX, 0.0f, s.data(), s.data() + s.size()).x;
    };
    auto measure = [&](float size, std::string_view s) -> float {
      if (art) {
        return art->Text(nullptr, ImVec2(), size, s, 0);
      }
      return ImGui::GetFont()->CalcTextSizeA(size * 0.8f, FLT_MAX, 0.0f, s.data(), s.data() + s.size()).x;
    };
    if (art) {
      art->Panel(dl, min, max, min.y + header, max.y, u, alpha);
    } else {
      dl->AddRectFilledMultiColor(min, max, IM_COL32(16, 6, 10, int(250 * alpha)),
                                  IM_COL32(16, 6, 10, int(250 * alpha)),
                                  IM_COL32(64, 10, 98, int(250 * alpha)),
                                  IM_COL32(64, 10, 98, int(250 * alpha)));
      dl->AddRectFilled(min, ImVec2(max.x, min.y + header), IM_COL32(0, 0, 0, int(255 * alpha)));
      dl->AddRect(min, max, IM_COL32(194, 180, 142, int(255 * alpha)), 10.0f * u, 0, 2.0f * u);
    }
    // The tier: its trophy and colour in the title bar (a pale blue rim too
    // for the platinum).
    const Tier tier = entry.platinum ? Tier::kPlatinum : entry.tier;
    const TierColors tier_colors = ColorsOf(tier, alpha);
    if (entry.platinum) {
      dl->AddRect(ImVec2(min.x - 2.0f * u, min.y - 2.0f * u), ImVec2(max.x + 2.0f * u, max.y + 2.0f * u),
                  tier_colors.main, 12.0f * u, 0, 3.0f * u);
    }
    const float cup = header - 8.0f * u;
    DrawTrophy(dl, ImVec2(min.x + pad - 4.0f * u, min.y + 5.0f * u), cup, alpha, tier);
    // Title bar: "Achievement Unlocked", the tier and the gamerscore.
    text(ImVec2(min.x + pad + cup + 2.0f * u, min.y + 5.0f * u), 30.0f * u,
         entry.platinum ? "All Achievements Unlocked"
         : folded       ? "Achievements Unlocked"
                        : "Achievement Unlocked",
         tier_colors.main);
    const uint32_t score = folded ? entry.more_gamerscore : a.gamerscore;
    float tier_right = max.x - pad;  // where the tier's name ends
    if (score > 0) {
      const std::string g = std::to_string(score);
      const float r = 12.0f * u;
      const float gx = max.x - pad - r;
      const float gy = min.y + header * 0.5f;
      DrawCoin(dl, ImVec2(gx, gy), r, u, alpha);
      const float sw = measure(28.0f * u, g);
      text(ImVec2(gx - r - 8.0f * u - sw, min.y + 6.0f * u), 28.0f * u, g, white);
      tier_right = gx - r - 8.0f * u - sw - 14.0f * u;
    }
    const char* tier_name = TierTitle(tier);
    text(ImVec2(tier_right - measure(24.0f * u, tier_name), min.y + 8.0f * u), 24.0f * u, tier_name,
         tier_colors.main);
    // The picture in a beige frame.
    const ImVec2 icon_min(min.x + pad, min.y + header + (h - header - icon) * 0.5f);
    const ImVec2 icon_max(icon_min.x + icon, icon_min.y + icon);
    dl->AddRectFilled(ImVec2(icon_min.x - 3 * u, icon_min.y - 3 * u),
                      ImVec2(icon_max.x + 3 * u, icon_max.y + 3 * u), beige, 6.0f * u);
    dl->AddRectFilled(icon_min, icon_max, IM_COL32(24, 6, 42, int(255 * alpha)), 4.0f * u);
    rex::ui::ImmediateTexture* picture = folded || entry.platinum ? nullptr : icons_.GetIcon(a);
    if (picture) {
      dl->AddImageRounded(reinterpret_cast<ImTextureID>(picture), icon_min, icon_max, ImVec2(0, 0),
                          ImVec2(1, 1), white, 4.0f * u);
    } else {
      DrawTrophy(dl, ImVec2(icon_min.x + icon * 0.1f, icon_min.y + icon * 0.06f), icon * 0.8f, alpha,
                 tier);
    }
    // Name and description.
    const float tx = icon_max.x + 18.0f * u;
    float name_size = 32.0f * u;
    while (name_size > 22.0f * u && measure(name_size, name) > tw) {
      name_size -= 1.0f * u;
    }
    text(ImVec2(tx, min.y + header + 8.0f * u), name_size, name, white);
    float ly = min.y + header + 8.0f * u + 32.0f * u + 6.0f * u;
    for (const std::string& line : lines) {
      text(ImVec2(tx, ly), ds, line, soft);
      ly += ds + 3.0f * u;
    }
  }

  rex::ui::ImmediateDrawer* immediate_drawer_;
  rex::ui::AchievementIconCache icons_;
  rex::system::AchievementListenerHandle unlock_listener_ = 0;
  std::mutex mutex_;
  std::deque<Entry> queue_;
  std::chrono::steady_clock::time_point next_allowed_{};
  bool tiers_logged_ = false;  // UI thread
  std::chrono::steady_clock::time_point next_tier_check_{};
};

// Tests and previews: a popup without unlocking anything.
void PushToToast(const AchievementEvent& event) {
  std::lock_guard<std::mutex> lock(g_toast_mutex);
  if (g_toast) {
    g_toast->Add(event, false);
  } else {
    REXLOG_WARN("Achievement popup: no popup window yet");
  }
}

void PushPlatinumToToast() {
  std::lock_guard<std::mutex> lock(g_toast_mutex);
  if (g_toast) {
    g_toast->AddPlatinum();
  } else {
    REXLOG_WARN("Achievement popup: no popup window yet");
  }
}

// A popup of `tier`: the first of the game's achievements of it (a made-up one
// without the game's list), or the platinum.
void ShowTierPreview(Tier tier) {
  if (tier == Tier::kPlatinum) {
    PushPlatinumToToast();
    return;
  }
  if (auto* manager = Manager()) {
    for (const auto& a : manager->ListAchievements()) {
      if (TierOf(a.gamerscore) == tier) {
        PushToToast(AchievementEvent{a, manager->GetUnlockTime(a.id)});
        return;
      }
    }
  }
  rex::system::AchievementInfo fake;
  fake.label = fmt::format("Test {} Trophy", TierTitle(tier));
  fake.description = "A popup to check how the tiers look and sound (achievement_tier_test).";
  fake.gamerscore = tier == Tier::kGold ? kGoldFrom : tier == Tier::kSilver ? kSilverFrom : 5;
  PushToToast(AchievementEvent{fake, 0});
}

// ---------------------------------------------------------------- the list (F7)

// F7 (bind_achievements; also `achievements_window`): every achievement in a
// window of the popup's look - the game's purple panel with its black bars
// and beige rim, its menu font, the tier colours, trophies and the G coin.
// Its header has the count, the gamerscore and a progress bar in the tiers'
// colours; under it the four trophies (bronze / silver / gold unlocked of
// all, the platinum's state), the All / Unlocked / Locked tabs, then the
// platinum and the achievements: picture (dimmed and locked until earned),
// name, description, tier and points, the date for the unlocked ones.
// It has the controllers while open, like the F1 menu (the game gets none of
// it): up / down (d-pad, left stick, the arrow keys) pick a row, LB / RB (left
// / right) the tab, the triggers (Page Up / Down) a page, B (Esc, F7) closes.
// The mouse wheel scrolls; a click picks a row or a tab.
std::atomic<int> g_window_request{0};  // 1 open, 2 close, 3 open or close

constexpr uint16_t kPadUp = 0x0001, kPadDown = 0x0002, kPadLeft = 0x0004, kPadRight = 0x0008;
constexpr uint16_t kPadLeftShoulder = 0x0100, kPadRightShoulder = 0x0200, kPadB = 0x2000;
constexpr uint32_t kPadLeftTrigger = 0x10000, kPadRightTrigger = 0x20000;
constexpr int kStickThreshold = 16000;
constexpr int kTriggerThreshold = 120;

class AchievementsWindow final : public rex::ui::ImGuiDialog {
 public:
  AchievementsWindow(rex::ui::ImGuiDrawer* drawer, rex::ui::ImmediateDrawer* immediate_drawer,
                     std::function<void(std::function<void()>)> defer)
      : ImGuiDialog(drawer),
        immediate_drawer_(immediate_drawer),
        defer_(std::move(defer)),
        pad_(std::make_shared<SharedPad>()) {}

  ~AchievementsWindow() override {
    if (open_) {
      Close(false);
    }
  }

 protected:
  void OnDraw(ImGuiIO& io) override {
    ImGui::SetCurrentContext(io.Ctx);
    switch (g_window_request.exchange(0)) {
      case 1:
        if (!open_) Open();
        break;
      case 2:
        if (open_) Close(true);
        break;
      case 3:
        if (open_) {
          Close(true);
        } else {
          Open();
        }
        break;
      default:
        break;
    }
    if (!open_) {
      return;
    }
    const double now = ImGui::GetTime();
    if (open_time_ < 0.0) {
      open_time_ = now;
    }
    // The controllers are read after the paint (reading them may pump window
    // events), for the next frame - as the F1 menu does.
    if (!pad_->pending && defer_) {
      pad_->pending = true;
      defer_([pad = pad_]() {
        pad->state = ReadPad();
        pad->valid = true;
        pad->pending = false;
      });
    }
    Refresh();
    HandleInput(now);
    if (!open_) {
      return;
    }
    Draw(io, now);
  }

 private:
  struct PadState {
    uint32_t buttons = 0;  // X_INPUT_GAMEPAD_* bits, kPadLeftTrigger / kPadRightTrigger
    int16_t thumb_lx = 0, thumb_ly = 0;
  };
  struct SharedPad {
    PadState state;
    bool valid = false;
    bool pending = false;
  };
  struct Item {
    rex::system::AchievementInfo info;
    bool unlocked = false;
    uint64_t time = 0;
    Tier tier = Tier::kBronze;
  };
  enum Tab { kAll, kUnlocked, kLocked, kTabCount };
  static constexpr int kPlatinumRow = -1;

  // 1080p units.
  static constexpr float kW = 1120.0f, kH = 930.0f;
  static constexpr float kHeader = 70.0f, kFooter = 60.0f, kPad = 24.0f;
  static constexpr float kSummary = 220.0f;  // count line, bar, trophies, tabs
  static constexpr float kRowH = 116.0f, kRowStep = 124.0f;
  static constexpr float kListH = kH - kHeader - kSummary - kFooter - 8.0f;

  static rex::input::InputSystem* Input() {
    auto* runtime = rex::Runtime::instance();
    return runtime ? static_cast<rex::input::InputSystem*>(runtime->input_system()) : nullptr;
  }

  // Every controller, merged: the left stick pushed the furthest wins.
  static PadState ReadPad() {
    PadState pad;
    auto* input = Input();
    if (!input) {
      return pad;
    }
    int distance = 0;
    for (uint32_t user = 0; user < rex::input::kMaxGuestUsers; ++user) {
      rex::input::X_INPUT_STATE state = {};
      if (input->GetStateForUI(user, &state) != rex::X_RESULT(0)) {
        continue;
      }
      pad.buttons |= uint16_t(state.gamepad.buttons);
      if (uint8_t(state.gamepad.left_trigger) > kTriggerThreshold) pad.buttons |= kPadLeftTrigger;
      if (uint8_t(state.gamepad.right_trigger) > kTriggerThreshold) pad.buttons |= kPadRightTrigger;
      const int16_t lx = state.gamepad.thumb_lx, ly = state.gamepad.thumb_ly;
      const int d = std::abs(int(lx)) + std::abs(int(ly));
      if (d > distance) {
        distance = d;
        pad.thumb_lx = lx;
        pad.thumb_ly = ly;
      }
    }
    return pad;
  }

  void Open() {
    open_ = true;
    open_time_ = -1.0;
    pad_seen_ = false;
    back_buttons_ = 0;
    // The last read is from the previous time it was open: wait for a fresh
    // one, so what's held as it opens isn't taken as a press.
    pad_->valid = false;
    if (auto* input = Input()) {
      input->AddUIInputBlocker();
      blocking_ = true;
    }
    Refresh();
    follow_ = true;
    REXLOG_INFO("Achievements window: opened ({} / {} unlocked)", unlocked_count_, items_.size());
    BurstLimitPlayMenuSound(5);
  }

  void Close(bool sound) {
    open_ = false;
    if (blocking_) {
      blocking_ = false;
      if (auto* input = Input()) {
        // Masks what is still held (the B that closed it) until let go.
        input->RemoveUIInputBlocker();
      }
    }
    if (sound) {
      REXLOG_INFO("Achievements window: closed");
      BurstLimitPlayMenuSound(6);
    }
  }

  // The game's list and which are unlocked (cheap: 50 entries, every frame).
  void Refresh() {
    items_.clear();
    unlocked_count_ = 0;
    earned_ = total_ = 0;
    tier_count_.fill(0);
    tier_unlocked_.fill(0);
    platinum_time_ = 0;
    auto* manager = Manager();
    if (!manager) {
      return;
    }
    for (auto& info : manager->ListAchievements()) {
      Item item;
      item.unlocked = manager->IsUnlocked(info.id);
      item.time = item.unlocked ? manager->GetUnlockTime(info.id) : 0;
      item.tier = TierOf(info.gamerscore);
      item.info = std::move(info);
      total_ += item.info.gamerscore;
      ++tier_count_[int(item.tier)];
      if (item.unlocked) {
        ++unlocked_count_;
        earned_ += item.info.gamerscore;
        ++tier_unlocked_[int(item.tier)];
        platinum_time_ = std::max(platinum_time_, item.time);
      }
      items_.push_back(std::move(item));
    }
    if (!icons_ && !items_.empty() && rex::Runtime::instance()) {
      icons_ = std::make_unique<rex::ui::AchievementIconCache>(immediate_drawer_,
                                                               rex::Runtime::instance());
    }
  }

  bool Platinum() const { return !items_.empty() && unlocked_count_ == items_.size(); }

  // The rows of the tab: the platinum first, then the game's order.
  std::vector<int> Rows(Tab tab) const {
    std::vector<int> rows;
    if (items_.empty()) {
      return rows;
    }
    if (tab == kAll || (tab == kUnlocked) == Platinum()) {
      rows.push_back(kPlatinumRow);
    }
    for (size_t i = 0; i < items_.size(); ++i) {
      if (tab == kAll || (tab == kUnlocked) == items_[i].unlocked) {
        rows.push_back(int(i));
      }
    }
    return rows;
  }

  size_t TabCount(Tab tab) const {
    switch (tab) {
      case kUnlocked: return unlocked_count_;
      case kLocked: return items_.size() - unlocked_count_;
      default: return items_.size();
    }
  }

  static const char* TabName(Tab tab) {
    switch (tab) {
      case kUnlocked: return "Unlocked";
      case kLocked: return "Locked";
      default: return "All";
    }
  }

  // The rows a page moves.
  static int PageRows() { return std::max(1, int(kListH / kRowStep)); }

  void SetTab(int tab) {
    tab_ = Tab((tab + kTabCount) % kTabCount);
    selected_ = 0;
    scroll_target_ = scroll_ = 0.0f;
    follow_ = true;
    BurstLimitPlayMenuSound(0);
    REXLOG_INFO("Achievements window: tab {} ({} rows)", TabName(tab_), Rows(tab_).size());
  }

  void Select(int row, const std::vector<int>& rows) {
    if (rows.empty()) {
      return;
    }
    row = std::clamp(row, 0, int(rows.size()) - 1);
    if (row == selected_) {
      return;
    }
    selected_ = row;
    follow_ = true;
    BurstLimitPlayMenuSound(0);
    const int index = rows[size_t(row)];
    REXLOG_INFO("Achievements window: row {} of {} ({}): {}", row + 1, rows.size(), TabName(tab_),
                index == kPlatinumRow
                    ? std::string("Platinum")
                    : fmt::format("{} \"{}\"", items_[size_t(index)].info.id,
                                  items_[size_t(index)].info.label));
  }

  void HandleInput(double now) {
    int move = 0, page = 0, tab = 0;
    bool home = false, end = false, close = false;
    // The F1 menu on top has the keys and the controllers (and a text box,
    // the console, the keys).
    const bool menu_open = rex::ui::QuickMenuDialog::IsOpen();
    if (!menu_open && !ImGui::GetIO().WantTextInput) {
      if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) move = -1;
      if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) move = 1;
      if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) tab = -1;
      if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) tab = 1;
      if (ImGui::IsKeyPressed(ImGuiKey_PageUp)) page = -1;
      if (ImGui::IsKeyPressed(ImGuiKey_PageDown)) page = 1;
      if (ImGui::IsKeyPressed(ImGuiKey_Home, false)) home = true;
      if (ImGui::IsKeyPressed(ImGuiKey_End, false)) end = true;
      if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) close = true;
    }
    if (pad_->valid) {
      const PadState& pad = pad_->state;
      uint32_t buttons = pad.buttons;
      if (pad.thumb_ly > kStickThreshold) buttons |= kPadUp;
      else if (pad.thumb_ly < -kStickThreshold) buttons |= kPadDown;
      if (pad.thumb_lx > kStickThreshold) buttons |= kPadRight;
      else if (pad.thumb_lx < -kStickThreshold) buttons |= kPadLeft;
      int direction = 0;
      if (buttons & kPadUp) direction = 1;
      else if (buttons & kPadDown) direction = 2;
      else if (buttons & kPadLeftTrigger) direction = 3;
      else if (buttons & kPadRightTrigger) direction = 4;
      else if (buttons & kPadLeft) direction = 5;
      else if (buttons & kPadRight) direction = 6;
      if (!pad_seen_ || menu_open) {
        // What's held as it opens (or while the F1 menu has the controllers)
        // isn't a press here.
        pad_seen_ = !menu_open;
        last_buttons_ = buttons;
        repeat_direction_ = direction;
        repeat_time_ = DBL_MAX;
        back_buttons_ = 0;
      }
      const uint32_t pressed = buttons & ~last_buttons_;
      last_buttons_ = buttons;
      if (!menu_open) {
        if (pressed & kPadB) back_buttons_ |= kPadB;
        // Closed once B is let go, so the game doesn't get it as a press.
        if (back_buttons_ && !(buttons & back_buttons_)) {
          back_buttons_ = 0;
          close = true;
        }
        if (pressed & kPadLeftShoulder) tab = -1;
        if (pressed & kPadRightShoulder) tab = 1;
        bool fire = false;
        if (direction != repeat_direction_) {
          repeat_direction_ = direction;
          repeat_time_ = now + 0.4;
          fire = direction != 0;
        } else if (direction && now >= repeat_time_) {
          repeat_time_ = now + (direction <= 2 ? 0.08 : 0.22);
          fire = true;
        }
        if (fire) {
          switch (direction) {
            case 1: move = -1; break;
            case 2: move = 1; break;
            case 3: page = -1; break;
            case 4: page = 1; break;
            case 5: tab = -1; break;
            case 6: tab = 1; break;
          }
        }
      }
    }
    if (close) {
      Close(true);
      return;
    }
    if (tab) {
      SetTab(int(tab_) + tab);
    }
    const std::vector<int> rows = Rows(tab_);
    if (rows.empty()) {
      selected_ = 0;
      return;
    }
    selected_ = std::clamp(selected_, 0, int(rows.size()) - 1);
    if (move) {
      // Wraps around at the ends, like the game's lists.
      Select((selected_ + move + int(rows.size())) % int(rows.size()), rows);
    }
    if (page) Select(selected_ + page * PageRows(), rows);
    if (home) Select(0, rows);
    if (end) Select(int(rows.size()) - 1, rows);
  }

  void Draw(ImGuiIO& io, double now) {
    BurstLimitMenuArt* art = BurstLimitSharedMenuArt();
    if (art && !art->Ready(immediate_drawer_)) {
      art = nullptr;
    }
    const float alpha = float(std::clamp((now - open_time_) / 0.15, 0.0, 1.0));
    // 1080p units of the game's 16:9 picture (the popup's), smaller when the
    // window is too small for the panel.
    const ImVec2 display = io.DisplaySize;
    const float pic_w = std::min(display.x, display.y * 16.0f / 9.0f);
    float u = pic_w * 9.0f / 16.0f / 1080.0f;
    u = std::max(0.3f, std::min({u, display.y * 0.96f / kH, display.x * 0.96f / kW}));
    const ImVec2 min(std::floor((display.x - kW * u) * 0.5f), std::floor((display.y - kH * u) * 0.5f));
    const ImVec2 max(min.x + kW * u, min.y + kH * u);
    auto at = [&](float x, float y) { return ImVec2(min.x + x * u, min.y + y * u); };
    auto fade = [alpha](int r, int g, int b, int a = 255) {
      return IM_COL32(r, g, b, int(float(a) * alpha));
    };
    const ImU32 white = fade(255, 255, 255), soft = fade(222, 212, 238), faint = fade(160, 136, 196);
    const ImU32 yellow = fade(255, 214, 60), beige = fade(232, 222, 196);

    // An invisible ImGui window over the panel: the mouse over it is the
    // window's (wheel, clicks), not the game's.
    ImGui::SetNextWindowPos(min);
    ImGui::SetNextWindowSize(ImVec2(max.x - min.x, max.y - min.y));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    const bool shown = ImGui::Begin("##bl_achievements", nullptr,
                                    ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                        ImGuiWindowFlags_NoSavedSettings |
                                        ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoNav |
                                        ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar(2);
    if (!shown) {
      ImGui::End();
      return;
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const bool hovered = ImGui::IsWindowHovered() && !rex::ui::QuickMenuDialog::IsOpen();
    const bool clicked = hovered && ImGui::IsMouseClicked(0);
    const ImVec2 mouse = io.MousePos;
    auto inside = [&](ImVec2 a, ImVec2 b) {
      return mouse.x >= a.x && mouse.x < b.x && mouse.y >= a.y && mouse.y < b.y;
    };

    auto text = [&](ImVec2 p, float size, std::string_view s, ImU32 color) -> float {
      if (art) {
        return art->Text(dl, p, size, s, color);
      }
      ImFont* font = ImGui::GetFont();
      dl->AddText(font, size * 0.8f, p, color, s.data(), s.data() + s.size());
      return font->CalcTextSizeA(size * 0.8f, FLT_MAX, 0.0f, s.data(), s.data() + s.size()).x;
    };
    auto measure = [&](float size, std::string_view s) -> float {
      if (art) {
        return art->Text(nullptr, ImVec2(), size, s, 0);
      }
      return ImGui::GetFont()->CalcTextSizeA(size * 0.8f, FLT_MAX, 0.0f, s.data(), s.data() + s.size()).x;
    };
    auto wrap = [&](float size, const std::string& s, float width) {
      if (art) {
        return art->Wrap(size, s, width);
      }
      return std::vector<std::string>{s};
    };
    // A shoulder or trigger button: a chip like the F1 menu's.
    auto chip = [&](ImVec2 p, float height, std::string_view name) -> float {
      const float size = height * 0.62f;
      const float w = measure(size, name) + 16.0f * u;
      dl->AddRectFilled(p, ImVec2(p.x + w, p.y + height), fade(120, 80, 190), 6.0f * u);
      text(ImVec2(p.x + 8.0f * u, p.y + (height - size) * 0.5f), size, name, fade(240, 236, 248));
      return w;
    };
    const rex::ui::ButtonGlyphs glyphs = rex::ui::GetButtonGlyphs();
    const char* lb = glyphs == rex::ui::ButtonGlyphs::kPlayStation ? "L1"
                     : glyphs == rex::ui::ButtonGlyphs::kNintendo  ? "L"
                                                                   : "LB";
    const char* rb = glyphs == rex::ui::ButtonGlyphs::kPlayStation ? "R1"
                     : glyphs == rex::ui::ButtonGlyphs::kNintendo  ? "R"
                                                                   : "RB";
    const char* lt = glyphs == rex::ui::ButtonGlyphs::kPlayStation ? "L2"
                     : glyphs == rex::ui::ButtonGlyphs::kNintendo  ? "ZL"
                                                                   : "LT";
    const char* rt = glyphs == rex::ui::ButtonGlyphs::kPlayStation ? "R2"
                     : glyphs == rex::ui::ButtonGlyphs::kNintendo  ? "ZR"
                                                                   : "RT";

    // ---- the panel
    const float footer_top = max.y - kFooter * u;
    if (art) {
      art->Panel(dl, min, max, min.y + kHeader * u, footer_top, u, alpha);
    } else {
      const float radius = 14.0f * u;
      dl->AddRectFilledMultiColor(ImVec2(min.x, min.y + kHeader * u), ImVec2(max.x, footer_top),
                                  fade(16, 6, 10, 250), fade(16, 6, 10, 250), fade(64, 10, 98, 250),
                                  fade(64, 10, 98, 250));
      dl->AddRectFilled(min, ImVec2(max.x, min.y + kHeader * u), fade(0, 0, 0), radius,
                        ImDrawFlags_RoundCornersTop);
      dl->AddRectFilled(ImVec2(min.x, footer_top), max, fade(0, 0, 0), radius,
                        ImDrawFlags_RoundCornersBottom);
      dl->AddRect(min, max, fade(194, 180, 142), radius, 0, 3.0f * u);
    }
    if (Platinum()) {
      dl->AddRect(ImVec2(min.x - 2.0f * u, min.y - 2.0f * u), ImVec2(max.x + 2.0f * u, max.y + 2.0f * u),
                  ColorsOf(Tier::kPlatinum, alpha).main, 16.0f * u, 0, 3.0f * u);
    }

    // ---- title bar: trophy, "Achievements", the gamerscore
    DrawTrophy(dl, at(kPad - 4.0f, 7.0f), 56.0f * u, alpha,
               Platinum() ? Tier::kPlatinum : Tier::kGold);
    text(at(kPad + 58.0f, 13.0f), 42.0f * u, "Achievements", yellow);
    {
      const float r = 16.0f * u;
      const ImVec2 coin(max.x - kPad * u - r, min.y + kHeader * u * 0.5f);
      DrawCoin(dl, coin, r, u, alpha);
      const std::string score = fmt::format("{} / {}", earned_, total_);
      const float sw = measure(32.0f * u, score);
      text(ImVec2(coin.x - r - 10.0f * u - sw, min.y + 18.0f * u), 32.0f * u, score, white);
    }

    // ---- summary: the count, the bar, the trophies
    float y = kHeader + 16.0f;
    const size_t count = items_.size();
    if (count) {
      text(at(kPad, y), 27.0f * u, fmt::format("{} of {} unlocked", unlocked_count_, count), soft);
      const std::string percent = fmt::format("{}%", unlocked_count_ * 100 / count);
      text(ImVec2(max.x - kPad * u - measure(27.0f * u, percent), min.y + y * u), 27.0f * u,
           percent, white);
    } else {
      text(at(kPad, y), 27.0f * u, "The game's achievements are read when the game starts", soft);
    }
    y += 36.0f;
    {
      // The bar: the unlocked bronze, silver and gold in their colours.
      const ImVec2 b0 = at(kPad, y), b1 = at(kW - kPad, y + 18.0f);
      const float radius = 9.0f * u;
      dl->AddRectFilled(b0, b1, fade(20, 6, 34), radius);
      float x = b0.x;
      const float inner = b1.x - b0.x;
      size_t done = 0;
      for (Tier tier : {Tier::kBronze, Tier::kSilver, Tier::kGold}) {
        const size_t n = tier_unlocked_[int(tier)];
        if (!n || !count) continue;
        const bool first = done == 0;
        done += n;
        const bool last = done == count;
        const float x1 = b0.x + inner * float(done) / float(count);
        const TierColors c = ColorsOf(tier, alpha);
        ImDrawFlags flags = ImDrawFlags_RoundCornersNone;
        if (first && last) flags = ImDrawFlags_RoundCornersAll;
        else if (first) flags = ImDrawFlags_RoundCornersLeft;
        else if (last) flags = ImDrawFlags_RoundCornersRight;
        dl->AddRectFilled(ImVec2(x, b0.y), ImVec2(x1, b1.y), c.main, radius, flags);
        // A shine along its top.
        dl->AddRectFilled(ImVec2(x + (first ? radius * 0.6f : 0.0f), b0.y + 3.0f * u),
                          ImVec2(x1 - (last ? radius * 0.6f : 0.0f), b0.y + 6.0f * u),
                          (c.shine & ~IM_COL32_A_MASK) | (ImU32(110 * alpha) << IM_COL32_A_SHIFT));
        x = x1;
      }
      dl->AddRect(ImVec2(b0.x - 1.0f * u, b0.y - 1.0f * u), ImVec2(b1.x + 1.0f * u, b1.y + 1.0f * u),
                  Platinum() ? ColorsOf(Tier::kPlatinum, alpha).main : fade(194, 180, 142),
                  radius + 1.0f * u, 0, 2.0f * u);
    }
    y += 18.0f + 16.0f;
    {
      // Bronze, silver and gold: unlocked of all; the platinum: its state.
      const float gap = 12.0f, ch = 66.0f;
      const float cw = (kW - 2.0f * kPad - 3.0f * gap) / 4.0f;
      for (int t = 0; t < 4; ++t) {
        const Tier tier = Tier(t);
        const float cx = kPad + float(t) * (cw + gap);
        const bool platinum = tier == Tier::kPlatinum;
        const bool any = platinum ? Platinum() : tier_unlocked_[t] > 0;
        const bool all = platinum ? Platinum() : tier_count_[t] && tier_unlocked_[t] == tier_count_[t];
        const TierColors c = ColorsOf(tier, alpha * (any ? 1.0f : 0.55f));
        dl->AddRectFilled(at(cx, y), at(cx + cw, y + ch), fade(0, 0, 0, 95), 8.0f * u);
        dl->AddRect(at(cx, y), at(cx + cw, y + ch), c.main, 8.0f * u, 0, (all ? 3.0f : 1.5f) * u);
        DrawTrophy(dl, at(cx + 8.0f, y + 8.0f), 50.0f * u, alpha * (any ? 1.0f : 0.45f), tier);
        text(at(cx + 64.0f, y + 6.0f), 25.0f * u, TierTitle(tier), ColorsOf(tier, alpha).main);
        const std::string value =
            platinum ? (Platinum() ? "Unlocked" : "Locked")
                     : fmt::format("{} / {}", tier_unlocked_[t], tier_count_[t]);
        text(at(cx + 64.0f, y + 34.0f), 26.0f * u, value,
             platinum && !Platinum() ? faint : all ? ColorsOf(tier, alpha).shine : white);
      }
      y += ch + 16.0f;
    }

    // ---- tabs: LB  All 50  Unlocked 23  Locked 27  RB
    {
      const float th = 40.0f, ts = 26.0f;
      float x = kPad;
      x += chip(at(x, y + 4.0f), 32.0f * u, lb) / u + 18.0f;
      for (int t = 0; t < kTabCount; ++t) {
        const std::string label = fmt::format("{}  {}", TabName(Tab(t)), TabCount(Tab(t)));
        const float tw = measure(ts * u, label) / u;
        const ImVec2 p0 = at(x - 14.0f, y), p1 = at(x + tw + 14.0f, y + th);
        const bool active = t == int(tab_);
        if (active) {
          dl->AddRectFilled(p0, p1, fade(150, 120, 214), th * 0.5f * u);
        }
        text(at(x, y + (th - ts) * 0.5f), ts * u, label, active ? white : fade(190, 172, 216));
        if (clicked && inside(p0, p1) && !active) {
          SetTab(t);
        }
        x += tw + 38.0f;
      }
      chip(at(x - 10.0f, y + 4.0f), 32.0f * u, rb);
      y += th + 12.0f;
    }

    // ---- the list
    const std::vector<int> rows = Rows(tab_);
    selected_ = rows.empty() ? 0 : std::clamp(selected_, 0, int(rows.size()) - 1);
    const float list_top = y;
    const ImVec2 list_min = at(16.0f, list_top), list_max = at(kW - 16.0f, list_top + kListH);
    const float content = rows.empty() ? 0.0f : float(rows.size()) * kRowStep - (kRowStep - kRowH);
    const float max_scroll = std::max(0.0f, content - kListH);
    if (hovered && io.MouseWheel != 0.0f) {
      scroll_target_ -= io.MouseWheel * kRowStep * 1.25f;
      follow_ = false;
    }
    if (follow_ && !rows.empty()) {
      // Keep the picked row in view.
      const float top = float(selected_) * kRowStep;
      if (top < scroll_target_) scroll_target_ = top;
      if (top + kRowH > scroll_target_ + kListH) scroll_target_ = top + kRowH - kListH;
      follow_ = false;
    }
    scroll_target_ = std::clamp(scroll_target_, 0.0f, max_scroll);
    scroll_ += (scroll_target_ - scroll_) * std::min(1.0f, io.DeltaTime * 16.0f);
    if (std::fabs(scroll_target_ - scroll_) < 0.5f) scroll_ = scroll_target_;
    scroll_ = std::clamp(scroll_, 0.0f, max_scroll);

    dl->PushClipRect(list_min, list_max, true);
    if (rows.empty()) {
      const char* none = items_.empty() ? "No achievements yet"
                         : tab_ == kUnlocked ? "None unlocked yet - keep playing!"
                                             : "Every achievement is unlocked!";
      text(ImVec2((list_min.x + list_max.x - measure(28.0f * u, none)) * 0.5f, list_min.y + 40.0f * u),
           28.0f * u, none, soft);
    }
    const bool scrollbar = max_scroll > 0.0f;
    const float row_right = kW - 16.0f - (scrollbar ? 16.0f : 0.0f);
    for (size_t r = 0; r < rows.size(); ++r) {
      const float top = list_top + float(r) * kRowStep - scroll_;
      if (top + kRowH < list_top || top > list_top + kListH) {
        continue;
      }
      const ImVec2 r0 = at(16.0f, top), r1 = at(row_right, top + kRowH);
      if (clicked && inside(r0, r1) && inside(list_min, list_max)) {
        Select(int(r), rows);
        follow_ = false;
      }
      DrawRow(dl, r0, r1, rows[r], int(r) == selected_, u, alpha, text, measure, wrap);
    }
    dl->PopClipRect();
    if (scrollbar) {
      const ImVec2 t0(list_max.x - 8.0f * u, list_min.y), t1(list_max.x, list_max.y);
      dl->AddRectFilled(t0, t1, fade(0, 0, 0, 110), 4.0f * u);
      const float track = t1.y - t0.y;
      const float thumb = std::max(40.0f * u, track * kListH / content);
      const float ty = t0.y + (track - thumb) * (scroll_ / max_scroll);
      dl->AddRectFilled(ImVec2(t0.x, ty), ImVec2(t1.x, ty + thumb), fade(194, 180, 142, 230), 4.0f * u);
    }

    // ---- footer: saved automatically, the button prompts (right, like the game's)
    {
      const float mid = (footer_top + max.y) * 0.5f;
      text(ImVec2(min.x + kPad * u, mid - 11.0f * u), 22.0f * u, "Unlock progress is saved automatically",
           faint);
      const float ps = 25.0f * u, icon = 30.0f * u, arrow = 22.0f * u, ch = 28.0f * u;
      struct Prompt {
        int kind;  // 0 up/down arrows, 1 two chips, 2 the B button
        const char* a;
        const char* b;
        const char* label;
      };
      const Prompt prompts[] = {{0, nullptr, nullptr, "Select"},
                                {1, lb, rb, "Tab"},
                                {1, lt, rt, "Page"},
                                {2, nullptr, nullptr, "Close"}};
      auto chip_width = [&](const char* name) { return measure(ch * 0.62f, name) + 16.0f * u; };
      auto prompt_width = [&](const Prompt& p) {
        const float lead = p.kind == 0 ? 2.0f * arrow + 4.0f * u
                           : p.kind == 1 ? chip_width(p.a) + 4.0f * u + chip_width(p.b) + 6.0f * u
                                         : icon + 4.0f * u;
        return lead + measure(ps, p.label) + 22.0f * u;
      };
      float total = 0.0f;
      for (const Prompt& p : prompts) total += prompt_width(p);
      float x = max.x - kPad * u - total + 22.0f * u;
      const bool icons = art && art->Window(BurstLimitMenuArt::kArrowUp);
      for (const Prompt& p : prompts) {
        if (p.kind == 0) {
          if (icons) {
            art->Icon(dl, BurstLimitMenuArt::kArrowUp, ImVec2(x, mid - arrow * 0.5f), arrow, yellow);
            art->Icon(dl, BurstLimitMenuArt::kArrowUp, ImVec2(x + arrow, mid - arrow * 0.5f), arrow,
                      yellow, 2);
          } else {
            dl->AddTriangleFilled(ImVec2(x + arrow * 0.5f, mid - arrow * 0.4f),
                                  ImVec2(x + arrow * 0.9f, mid + arrow * 0.3f),
                                  ImVec2(x + arrow * 0.1f, mid + arrow * 0.3f), yellow);
            dl->AddTriangleFilled(ImVec2(x + arrow * 1.1f, mid - arrow * 0.3f),
                                  ImVec2(x + arrow * 1.9f, mid - arrow * 0.3f),
                                  ImVec2(x + arrow * 1.5f, mid + arrow * 0.4f), yellow);
          }
          x += 2.0f * arrow + 4.0f * u;
        } else if (p.kind == 1) {
          x += chip(ImVec2(x, mid - ch * 0.5f), ch, p.a) + 4.0f * u;
          x += chip(ImVec2(x, mid - ch * 0.5f), ch, p.b) + 6.0f * u;
        } else {
          if (art && art->Window(BurstLimitMenuArt::kButtonB)) {
            art->Icon(dl, BurstLimitMenuArt::kButtonB, ImVec2(x, mid - icon * 0.5f), icon, white);
          } else {
            dl->AddCircleFilled(ImVec2(x + icon * 0.5f, mid), icon * 0.45f, fade(206, 44, 44), 24);
            text(ImVec2(x + icon * 0.3f, mid - icon * 0.35f), icon * 0.7f, "B", white);
          }
          x += icon + 4.0f * u;
        }
        x += text(ImVec2(x, mid - ps * 0.5f), ps, p.label, white) + 22.0f * u;
      }
    }
    ImGui::End();
  }

  template <typename TextFn, typename MeasureFn, typename WrapFn>
  void DrawRow(ImDrawList* dl, ImVec2 r0, ImVec2 r1, int index, bool selected, float u, float alpha,
               TextFn& text, MeasureFn& measure, WrapFn& wrap) {
    auto fade = [alpha](int r, int g, int b, int a = 255) {
      return IM_COL32(r, g, b, int(float(a) * alpha));
    };
    const bool platinum_row = index == kPlatinumRow;
    const Item* item = platinum_row ? nullptr : &items_[size_t(index)];
    const bool unlocked = platinum_row ? Platinum() : item->unlocked;
    const Tier tier = platinum_row ? Tier::kPlatinum : item->tier;
    const TierColors colors = ColorsOf(tier, alpha);
    const float radius = 8.0f * u;

    // The row: brighter when unlocked (with its tier's stripe), darker when
    // locked; the picked one in the game's highlight.
    if (selected) {
      dl->AddRectFilled(r0, r1, fade(130, 98, 196, 235), radius);
    } else if (unlocked) {
      dl->AddRectFilled(r0, r1, fade(255, 255, 255, 24), radius);
    } else {
      dl->AddRectFilled(r0, r1, fade(0, 0, 0, 90), radius);
    }
    if (unlocked) {
      dl->AddRectFilled(r0, ImVec2(r0.x + 7.0f * u, r1.y), colors.main, radius,
                        ImDrawFlags_RoundCornersLeft);
    }
    if (platinum_row) {
      dl->AddRect(r0, r1, ColorsOf(tier, alpha * (unlocked ? 1.0f : 0.5f)).main, radius, 0,
                  (unlocked ? 2.5f : 1.5f) * u);
    }
    if (selected) {
      dl->AddRect(r0, r1, fade(255, 214, 60), radius, 0, 2.5f * u);
    }

    // The picture in the popup's beige frame; locked: dimmed, with a lock.
    const float icon = 84.0f * u;
    const ImVec2 i0(r0.x + 20.0f * u, r0.y + (r1.y - r0.y - icon) * 0.5f);
    const ImVec2 i1(i0.x + icon, i0.y + icon);
    dl->AddRectFilled(ImVec2(i0.x - 3 * u, i0.y - 3 * u), ImVec2(i1.x + 3 * u, i1.y + 3 * u),
                      unlocked ? fade(232, 222, 196) : fade(128, 116, 104), 6.0f * u);
    dl->AddRectFilled(i0, i1, fade(24, 6, 42), 4.0f * u);
    rex::ui::ImmediateTexture* picture = item && icons_ ? icons_->GetIcon(item->info) : nullptr;
    if (picture) {
      dl->AddImageRounded(reinterpret_cast<ImTextureID>(picture), i0, i1, ImVec2(0, 0), ImVec2(1, 1),
                          unlocked ? fade(255, 255, 255) : fade(118, 112, 128), 4.0f * u);
      if (!unlocked) {
        dl->AddRectFilled(i0, i1, fade(26, 8, 44, 105), 4.0f * u);
      }
    } else {
      DrawTrophy(dl, ImVec2(i0.x + icon * 0.1f, i0.y + icon * 0.06f), icon * 0.8f,
                 alpha * (unlocked ? 1.0f : 0.4f), tier);
    }
    {
      const float br = 15.0f * u;
      const ImVec2 c(i1.x - 2.0f * u, i1.y - 2.0f * u);
      if (unlocked) {
        // A check in the tier's colour.
        dl->AddCircleFilled(c, br, colors.main, 24);
        dl->AddCircle(c, br, fade(40, 16, 60), 24, 2.0f * u);
        const ImVec2 check[3] = {ImVec2(c.x - 7.0f * u, c.y + 0.5f * u),
                                 ImVec2(c.x - 2.0f * u, c.y + 5.5f * u),
                                 ImVec2(c.x + 7.0f * u, c.y - 5.0f * u)};
        dl->AddPolyline(check, 3, fade(40, 16, 60), ImDrawFlags_None, 3.2f * u);
      } else {
        // A padlock.
        dl->AddCircleFilled(c, br, fade(30, 10, 50), 24);
        dl->AddCircle(c, br, fade(194, 180, 142), 24, 1.5f * u);
        dl->PathArcTo(ImVec2(c.x, c.y - 1.5f * u), 4.5f * u, 3.14159265f, 2.0f * 3.14159265f, 12);
        dl->PathStroke(fade(232, 222, 196), ImDrawFlags_None, 2.2f * u);
        dl->AddLine(ImVec2(c.x - 4.5f * u, c.y - 1.5f * u), ImVec2(c.x - 4.5f * u, c.y + 0.5f * u),
                    fade(232, 222, 196), 2.2f * u);
        dl->AddLine(ImVec2(c.x + 4.5f * u, c.y - 1.5f * u), ImVec2(c.x + 4.5f * u, c.y + 0.5f * u),
                    fade(232, 222, 196), 2.2f * u);
        dl->AddRectFilled(ImVec2(c.x - 6.5f * u, c.y), ImVec2(c.x + 6.5f * u, c.y + 8.0f * u),
                          fade(232, 222, 196), 1.5f * u);
      }
    }

    // The right column: points and coin, the tier, the date.
    const float right = r1.x - 18.0f * u;
    const float column = 200.0f * u;
    {
      if (platinum_row) {
        const std::string n = fmt::format("{} / {}", unlocked_count_, items_.size());
        text(ImVec2(right - measure(27.0f * u, n), r0.y + 14.0f * u), 27.0f * u, n,
             unlocked ? fade(255, 255, 255) : fade(200, 190, 220));
      } else {
        const float cr = 13.0f * u;
        const ImVec2 coin(right - cr, r0.y + 30.0f * u);
        DrawCoin(dl, coin, cr, u, alpha * (unlocked ? 1.0f : 0.6f));
        const std::string g = std::to_string(item->info.gamerscore);
        const float gw = measure(28.0f * u, g);
        text(ImVec2(coin.x - cr - 8.0f * u - gw, r0.y + 15.0f * u), 28.0f * u, g,
             unlocked ? fade(255, 255, 255) : fade(200, 190, 220));
      }
      const char* tier_name = TierTitle(tier);
      text(ImVec2(right - measure(23.0f * u, tier_name), r0.y + 50.0f * u), 23.0f * u, tier_name,
           ColorsOf(tier, alpha * (unlocked ? 1.0f : 0.7f)).main);
      const std::string when =
          unlocked ? UnlockDate(platinum_row ? platinum_time_ : item->time) : "Locked";
      if (!when.empty()) {
        text(ImVec2(right - measure(19.0f * u, when), r0.y + 82.0f * u), 19.0f * u, when,
             selected ? fade(236, 228, 248) : fade(160, 136, 196));
      }
    }

    // Name and description.
    const float tx = i1.x + 22.0f * u;
    const float tw = right - column - tx;
    const std::string name = platinum_row ? "Platinum" : item->info.label;
    float name_size = 31.0f * u;
    while (name_size > 23.0f * u && measure(name_size, name) > tw) {
      name_size -= 1.0f * u;
    }
    text(ImVec2(tx, r0.y + 12.0f * u), name_size, name,
         unlocked || selected ? fade(255, 255, 255) : fade(206, 196, 224));
    std::string description;
    bool hidden = false;
    if (platinum_row) {
      description = unlocked ? fmt::format("All {} of the game's achievements unlocked ({} G). "
                                           "Congratulations!",
                                           items_.size(), total_)
                             : fmt::format("Unlock all {} of the game's achievements ({} G) to earn "
                                           "the platinum.",
                                           items_.size(), total_);
    } else {
      description = ShownDescription(item->info, unlocked);
      hidden = description == kHiddenText;
    }
    float ds = 21.0f * u;
    std::vector<std::string> lines = wrap(ds, description, tw);
    if (lines.size() > 2) {
      ds = 18.0f * u;
      lines = wrap(ds, description, tw);
      if (lines.size() > 3) {
        lines.resize(3);
        lines[2] += "...";
      }
    }
    const ImU32 color = selected   ? fade(240, 234, 250)
                        : hidden   ? fade(150, 128, 186)
                        : unlocked ? fade(222, 212, 238)
                                   : fade(172, 158, 196);
    float ly = r0.y + 51.0f * u;
    for (const std::string& line : lines) {
      text(ImVec2(tx, ly), ds, line, color);
      ly += ds + 3.0f * u;
    }
  }

  rex::ui::ImmediateDrawer* immediate_drawer_;
  std::function<void(std::function<void()>)> defer_;
  std::shared_ptr<SharedPad> pad_;
  std::unique_ptr<rex::ui::AchievementIconCache> icons_;

  bool open_ = false;
  bool blocking_ = false;
  double open_time_ = -1.0;
  bool pad_seen_ = false;
  uint32_t last_buttons_ = 0;
  uint32_t back_buttons_ = 0;
  int repeat_direction_ = 0;
  double repeat_time_ = DBL_MAX;

  Tab tab_ = kAll;
  int selected_ = 0;
  float scroll_ = 0.0f, scroll_target_ = 0.0f;  // 1080p units
  bool follow_ = false;                         // bring the picked row into view

  std::vector<Item> items_;
  size_t unlocked_count_ = 0;
  uint32_t earned_ = 0, total_ = 0;
  std::array<size_t, 4> tier_count_{}, tier_unlocked_{};
  uint64_t platinum_time_ = 0;
};

// ---------------------------------------------------------------- commands

std::vector<std::string> Split(std::string_view args) {
  std::vector<std::string> out;
  size_t i = 0;
  while (i < args.size()) {
    while (i < args.size() && (args[i] == ' ' || args[i] == ',')) ++i;
    const size_t b = i;
    while (i < args.size() && args[i] != ' ' && args[i] != ',') ++i;
    if (i > b) out.emplace_back(args.substr(b, i - b));
  }
  return out;
}

void AchievementTest(std::string_view args) {
  auto* manager = Manager();
  const std::vector<std::string> parts = Split(args);
  if (!manager || parts.empty()) {
    REXLOG_WARN("achievement_test: usage: achievement_test <id|all|any> (no game running?)");
    return;
  }
  const std::string& what = parts[0];
  if (what == "all") {
    for (const auto& a : manager->ListAchievements()) {
      PushToToast(AchievementEvent{a, 0});
    }
    return;
  }
  if (const auto tier = TierFromName(what)) {
    ShowTierPreview(*tier);
    return;
  }
  uint32_t id = what == "any" ? 0 : uint32_t(std::strtoul(what.c_str(), nullptr, 0));
  if (auto info = id ? manager->FindAchievement(id) : std::nullopt) {
    PushToToast(AchievementEvent{*info, manager->GetUnlockTime(id)});
    return;
  }
  rex::system::AchievementInfo fake;
  fake.id = id;
  fake.label = "Test Achievement";
  fake.description = "A popup to check how they look and sound (achievement_test).";
  fake.gamerscore = 10;
  PushToToast(AchievementEvent{fake, 0});
}

// The four tiers' popups (and sounds) without unlocking anything:
// achievement_tier_test bronze|silver|gold|platinum|all.
void AchievementTierTest(std::string_view args) {
  const std::vector<std::string> parts = Split(args);
  const std::string what = parts.empty() ? "all" : parts[0];
  if (what == "all") {
    for (Tier tier : {Tier::kBronze, Tier::kSilver, Tier::kGold, Tier::kPlatinum}) {
      ShowTierPreview(tier);
    }
    return;
  }
  if (const auto tier = TierFromName(what)) {
    ShowTierPreview(*tier);
    return;
  }
  REXLOG_WARN("achievement_tier_test: usage: achievement_tier_test bronze|silver|gold|platinum|all");
}

void AchievementUnlock(std::string_view args) {
  auto* runtime = rex::Runtime::instance();
  auto* kernel = runtime ? runtime->kernel_state() : nullptr;
  const std::vector<std::string> parts = Split(args);
  if (!kernel || parts.empty()) {
    REXLOG_WARN("achievement_unlock: usage: achievement_unlock <id>");
    return;
  }
  kernel->UnlockAchievement(uint32_t(std::strtoul(parts[0].c_str(), nullptr, 0)));
}

void AchievementList() {
  auto* manager = Manager();
  if (!manager) {
    return;
  }
  const auto list = manager->ListAchievements();
  uint32_t unlocked = 0, score = 0, total = 0;
  for (const auto& a : list) {
    const bool u = manager->IsUnlocked(a.id);
    unlocked += u;
    score += u ? a.gamerscore : 0;
    total += a.gamerscore;
    REXLOG_INFO("achievement_list: {:3} {:3}G {:8} image {:3} flags {:08X}{} {} \"{}\" - {} | locked: "
                "{} | shown: {}",
                a.id, a.gamerscore, TierName(TierOf(a.gamerscore)), a.image_id, a.flags,
                (a.flags & kShowUnachieved) ? "" : " secret", u ? "UNLOCKED" : "locked  ", a.label,
                OneParagraph(a.description), OneParagraph(a.unachieved_description),
                ShownDescription(a, u));
  }
  REXLOG_INFO("achievement_list: {} / {} unlocked, {} / {} G", unlocked, list.size(), score, total);
  REXLOG_INFO("achievement_list: tiers: {}", TierSummary(list));
  rex::FlushLogging();
}

// achievements_window [open|close|toggle] (the default): the F7 window.
void AchievementsWindowCommand(std::string_view args) {
  const std::vector<std::string> parts = Split(args);
  const std::string what = parts.empty() ? "toggle" : parts[0];
  g_window_request = what == "open" ? 1 : what == "close" ? 2 : 3;
}

void AchievementSoundTest(std::string_view args) {
  const std::vector<std::string> parts = Split(args);
  if (!parts.empty() && parts[0] == "silent") {
    // The output path without a sound (tests while muted): a chime's length
    // of zeros through waveOut, then how much of it the device played.
    float seconds = 0.0f;
    std::vector<int16_t> pcm = MakeChime(Chime::kPlayStation, 0.0f, seconds);
    std::fill(pcm.begin(), pcm.end(), int16_t(0));
    Player().Play(std::move(pcm));
    std::thread([seconds] {
      std::this_thread::sleep_for(std::chrono::milliseconds(int(seconds * 1000.0f) + 200));
      REXLOG_INFO("Achievement sound: silent test: the output device played {} of {} samples",
                  Player().PlayedSamples(), int(seconds * kRate));
      rex::FlushLogging();
    }).detach();
    return;
  }
  if (!parts.empty() && parts[0] == "custom") {
    PlayAchievementSound(Tier::kBronze, "custom");
    return;
  }
  if (!parts.empty() && (parts[0] == "game" || parts[0].find(':') != std::string::npos)) {
    PlayAchievementSound(Tier::kBronze, "game",
                         parts[0] == "game" ? REXCVAR_GET(achievement_game_sound) : parts[0]);
    return;
  }
  if (!parts.empty()) {
    // A tier: its sound as achievement_sound = tiers plays it.
    if (const auto tier = TierFromName(parts[0])) {
      PlayAchievementSound(*tier, "tiers");
      return;
    }
    if (ChimeFromName(parts[0])) {
      PlayAchievementSound(Tier::kBronze, parts[0]);
      return;
    }
  }
  const std::string& setting = REXCVAR_GET(achievement_sound);
  PlayAchievementSound(Tier::kBronze, setting == "off" ? "xbox" : setting);
}

}  // namespace

REXCVAR_DEFINE_COMMAND_ARGS(achievement_test, AchievementTest, "Debug",
                            "Shows an achievement popup without unlocking it: "
                            "<id|all|any|bronze|silver|gold|platinum>");
REXCVAR_DEFINE_COMMAND_ARGS(achievement_tier_test, AchievementTierTest, "Debug",
                            "Shows a trophy tier's popup and plays its sound without unlocking "
                            "anything: bronze|silver|gold|platinum|all");
REXCVAR_DEFINE_COMMAND_ARGS(achievement_unlock, AchievementUnlock, "Debug",
                            "Unlocks an achievement like the game does: <id>");
REXCVAR_DEFINE_COMMAND(achievement_list, AchievementList, "Debug",
                       "Logs the game's achievements and which are unlocked");
REXCVAR_DEFINE_COMMAND_ARGS(achievements_window, AchievementsWindowCommand, "Debug",
                            "Opens or closes the achievements window (F7): [open|close|toggle]");
REXCVAR_DEFINE_COMMAND_ARGS(achievement_sound_test, AchievementSoundTest, "Debug",
                            "Plays the achievement sound: [xbox|playstation|bronze|silver|gold|"
                            "platinum|custom|game|BANK:cue|silent]");

std::unique_ptr<rex::ui::ImGuiDialog> BurstLimitCreateAchievementsWindow(
    rex::ui::ImGuiDrawer* drawer, rex::ui::ImmediateDrawer* immediate_drawer,
    std::function<void(std::function<void()>)> defer) {
  return std::make_unique<AchievementsWindow>(drawer, immediate_drawer, std::move(defer));
}

// F7 (the SDK's bind_achievements, through CreateAchievementsOverlay).
void BurstLimitToggleAchievementsWindow() {
  g_window_request = 3;
}

std::unique_ptr<rex::ui::AchievementNotificationDialog> BurstLimitCreateAchievementToast(
    rex::ui::ImGuiDrawer* drawer, rex::ui::ImmediateDrawer* immediate_drawer) {
  return std::make_unique<AchievementToast>(drawer, immediate_drawer);
}

// F1 > GAME: the popup settings and the list of the game's achievements.
void BurstLimitAchievementsMenu(rex::ui::QuickMenuSection& section) {
  using Item = rex::ui::QuickMenuItem;
  {
    Item item;
    item.kind = Item::Kind::kToggle;
    item.label = "Achievement popups";
    item.cvar = "achievement_popups";
    item.help =
        "A popup in the game's style when you earn one of the game's achievements: its picture, "
        "name, description and gamerscore. Each one only pops once per profile.";
    section.items.push_back(std::move(item));
  }
  {
    Item item;
    item.kind = Item::Kind::kChoice;
    item.label = "Achievement sound";
    item.cvar = "achievement_sound";
    item.choices = {{"tiers", "Trophy tiers"},
                    {"xbox", "Xbox-style"},
                    {"playstation", "PlayStation-style"},
                    {"custom", "Custom file"},
                    {"game", "Game sound"},
                    {"off", "Off"}};
    item.help =
        "The sound with a popup: Trophy tiers - a sound per trophy (bronze, silver, gold, and the "
        "platinum when you have them all; trophy sounds by Spade) - or a rising bloop (Xbox-style) "
        "or a bell sparkle (PlayStation-style), original sounds made by the recomp - or Custom "
        "file: your own sounds\\achievement.wav / .mp3 next to the exe (or achievement_sound_file) "
        "- or Game sound: one of the game's own sound effects (pick it below). Follows the game's "
        "Sound Effects volume.";
    section.items.push_back(std::move(item));
  }
  {
    Item item;
    item.kind = Item::Kind::kAction;
    item.label = "Trophy tiers";
    item.help = "Every achievement is a trophy, from its gamerscore: Bronze up to 15G, Silver "
                "20-35G, Gold 40G and up - and the Platinum pops when all of them are unlocked. "
                "Left / right to pick one, A to see its popup and hear it.";
    item.list = [] {
      return std::vector<std::pair<std::string, std::string>>{
          {"bronze", "Bronze"}, {"silver", "Silver"}, {"gold", "Gold"}, {"platinum", "Platinum"}};
    };
    item.list_help = [](const std::string& value) -> std::string {
      const auto tier = TierFromName(value);
      if (!tier) {
        return {};
      }
      std::string which;
      if (*tier == Tier::kPlatinum) {
        which = "Every achievement unlocked.";
      } else if (auto* manager = Manager()) {
        uint32_t count = 0, unlocked = 0;
        for (const auto& a : manager->ListAchievements()) {
          if (TierOf(a.gamerscore) == *tier) {
            ++count;
            unlocked += manager->IsUnlocked(a.id);
          }
        }
        which = fmt::format("{} achievements, {} unlocked.", count, unlocked);
      }
      std::string sound;
      if (REXCVAR_GET(achievement_sound) == "tiers") {
        std::string why;
        const std::filesystem::path path = TierSoundPath(*tier, why);
        sound = path.empty() ? fmt::format("Sound: the built-in {} chime.", TierName(*tier))
                             : fmt::format("Sound: {}.", path.filename().string());
      } else {
        sound = "Sound: the Achievement sound above.";
      }
      return fmt::format("{} trophy. {} {} A: see its popup and hear it.", TierTitle(*tier), which,
                         sound);
    };
    item.action = [](const std::string& value) {
      if (const auto tier = TierFromName(value)) {
        ShowTierPreview(*tier);
      }
    };
    section.items.push_back(std::move(item));
  }
  {
    Item item;
    item.kind = Item::Kind::kAction;
    item.label = "Game sound";
    item.shown_if_cvar = "achievement_sound";
    item.shown_if_values = {"game"};
    item.help = "One of the game's own sound effects for the popup, played from your game files. "
                "Left / right to look through them, A to use it and hear it.";
    item.empty_text = "Not read yet";
    item.list = [] {
      std::vector<std::pair<std::string, std::string>> out;
      const std::string current = REXCVAR_GET(achievement_game_sound);
      size_t at = 0;
      for (const BurstLimitGameCue& cue : BurstLimitListGameCues()) {
        if (cue.bank == "BATTLE_COMMON_SE") {
          continue;  // battle sounds: by hand in the config (BATTLE_COMMON_SE:<cue>)
        }
        std::string key = cue.bank + ":" + std::to_string(cue.id);
        std::string name = cue.name;
        for (const char* prefix : {"SysID", "CB_ID", "ID"}) {
          if (name.rfind(prefix, 0) == 0) {
            const size_t bar = name.find('_');
            if (bar != std::string::npos) name = name.substr(bar + 1);
            break;
          }
        }
        if (key == current) {
          at = out.size();
        }
        out.emplace_back(std::move(key), fmt::format("{} {} {}", cue.bank == "SYSTEM_SE" ? "System" : "Menu",
                                                     cue.id, name));
      }
      // The one in use first: the menu opens on it.
      std::rotate(out.begin(), out.begin() + std::min(at, out.size()), out.end());
      return out;
    };
    item.list_help = [](const std::string& value) -> std::string {
      for (const BurstLimitGameCue& cue : BurstLimitListGameCues()) {
        if (cue.bank + ":" + std::to_string(cue.id) == value) {
          const bool used = value == REXCVAR_GET(achievement_game_sound);
          return fmt::format("{} ({}, cue {}), {:.1f} s.{} A: use it and hear it.", cue.name, cue.bank,
                             cue.id, cue.seconds, used ? " In use." : "");
        }
      }
      return {};
    };
    item.action = [](const std::string& value) {
      if (value.empty()) {
        return;
      }
      rex::cvar::SetFlagByName("achievement_game_sound", value);
      rex::ReXApp::SaveConfigCommand();
      PlayAchievementSound(Tier::kBronze, "game", value);
    };
    section.items.push_back(std::move(item));
  }
  {
    Item item;
    item.kind = Item::Kind::kNumber;
    item.label = "Achievement volume";
    item.cvar = "achievement_volume";
    item.min = 0;
    item.max = 100;
    item.step = 10;
    item.format = "%g%%";
    item.help = "Volume of the achievement chime, times the game's own Sound Effects volume.";
    section.items.push_back(std::move(item));
  }
  {
    Item item;
    item.kind = Item::Kind::kChoice;
    item.label = "Popup position";
    item.cvar = "achievement_popup_position";
    item.choices = {{"bottom", "Bottom"},        {"bottom_left", "Bottom left"},
                    {"bottom_right", "Bottom right"}, {"top", "Top"},
                    {"top_left", "Top left"},    {"top_right", "Top right"}};
    item.help = "Where achievement popups appear on the screen.";
    section.items.push_back(std::move(item));
  }
  {
    Item item;
    item.kind = Item::Kind::kAction;
    item.label = "Achievements";
    item.help = "The game's achievements (F7 shows them all in a window). Left / right to look "
                "through them, A to see the popup.";
    item.empty_text = "None yet";
    item.list = [] {
      std::vector<std::pair<std::string, std::string>> out;
      auto* manager = Manager();
      if (!manager) {
        return out;
      }
      const auto list = manager->ListAchievements();
      // Unlocked first, then the rest, in the game's order.
      for (int pass = 0; pass < 2; ++pass) {
        for (const auto& a : list) {
          if (manager->IsUnlocked(a.id) == (pass == 0)) {
            out.emplace_back(std::to_string(a.id), (pass == 0 ? "" : "? ") + a.label);
          }
        }
      }
      return out;
    };
    item.list_help = [](const std::string& value) -> std::string {
      auto* manager = Manager();
      if (!manager) {
        return {};
      }
      const uint32_t id = uint32_t(std::strtoul(value.c_str(), nullptr, 10));
      const auto info = manager->FindAchievement(id);
      if (!info) {
        return {};
      }
      uint32_t unlocked = 0, count = 0, score = 0, total = 0;
      for (const auto& a : manager->ListAchievements()) {
        const bool u = manager->IsUnlocked(a.id);
        ++count;
        unlocked += u;
        score += u ? a.gamerscore : 0;
        total += a.gamerscore;
      }
      const bool u = manager->IsUnlocked(id);
      std::string when;
      if (const uint64_t ft = manager->GetUnlockTime(id)) {
        FILETIME file_time{DWORD(ft & 0xFFFFFFFFu), DWORD(ft >> 32)};
        SYSTEMTIME st;
        if (FileTimeToSystemTime(&file_time, &st)) {
          when = fmt::format(" on {:04}-{:02}-{:02}", st.wYear, st.wMonth, st.wDay);
        }
      }
      const std::string text = ShownDescription(*info, u);
      return fmt::format("{} ({}G, {}) - {}{}. {}\n{} of {} unlocked, {} / {} G.", info->label,
                         info->gamerscore, TierTitle(TierOf(info->gamerscore)),
                         u ? "unlocked" : "locked", when, text, unlocked, count, score, total);
    };
    item.action = [](const std::string& value) {
      AchievementTest(value);
    };
    section.items.push_back(std::move(item));
  }
}

// Mid-asm hook path: called from BurstLimitOnlineRulesBattleSetup (0x8217AEB4,
// the battle set-up sub_8217AEA8 before it reads the match request).
void BurstLimitOneHitKillBattleSetup() {
  if (!REXCVAR_GET(one_hit_kill) || rex::net::IsGameSessionOpen()) {
    return;
  }
  rex::memory::Memory* memory = GuestMemory();
  if (!memory) {
    return;
  }
  const uint32_t request = R32(memory, 0x841B5138);
  if (!Readable(memory, request, 216)) {
    return;
  }
  // Player 2's u16 health (+44) in the request; its maximum (+46) stays, so
  // the gauge shows it nearly empty.
  uint8_t* hp = memory->TranslateVirtual<uint8_t*>(request + 0x50 + 44);
  const uint16_t before = rex::memory::load_and_swap<uint16_t>(hp);
  rex::memory::store_and_swap<uint16_t>(hp, 1);
  REXLOG_INFO("one_hit_kill: player 2 health {} -> 1 (mode {})", before,
              R32(memory, request + 196));
}
