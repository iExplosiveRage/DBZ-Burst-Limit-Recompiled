#pragma once

// The game's own sound effects, read from the player's game files
// (SOUND/CSB/CMN/{SYSTEM_SE,MENU_SE,BATTLE_COMMON_SE}.CSB in the CPK archive,
// never shipped), for achievement_sound = game. Defined in
// burstlimit_menusound.cpp; ready once the menu sounds' loading thread has
// read them (empty / false before).

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

struct BurstLimitGameCue {
  std::string bank;  // SYSTEM_SE, MENU_SE, BATTLE_COMMON_SE
  uint32_t id = 0;   // the cue id in that cue sheet
  std::string name;  // its sound's name, e.g. SysID17_Clear_Success
  float seconds = 0.0f;
};

std::vector<BurstLimitGameCue> BurstLimitListGameCues();

// "BANK:cue" (e.g. "SYSTEM_SE:17") to mono PCM at its own rate, -1..1: the
// cue's sounds mixed at their delays and volumes. `what` names it, or says
// why it can't.
bool BurstLimitDecodeGameCue(std::string_view key, std::vector<float>& mono, uint32_t& rate,
                             std::string& what);
