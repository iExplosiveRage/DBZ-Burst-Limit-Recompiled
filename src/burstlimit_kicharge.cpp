// Manual Ki charge (Shin Budokai style): hold L3 while the fighter is free -
// standing, walking or hovering, not attacking, guarding, hit or in a
// cinematic - and they power up in place while the Ki gauge fills faster than
// on its own: a start, a loop while held, and an end when L3 is let go or the
// gauge is full (holding on with a full gauge doesn't start again). A hit
// interrupts it like any idle pose (normal hit reaction). The fighter can't
// move, guard or attack while charging.
//
// The look: the game's own Aura Spark power-up (common motion 26: the
// clenched-fist part played back and forth while held, the arms-up burst as
// the end on a full gauge, played back to the stance on release; in the air
// with the hovering legs of the air Aura Spark), a full-strength aura while
// charging, the game's "aura appears" / transformation-wind / aura-fades
// sound cues and the Aura Spark shout. When a mod adds dedicated charge
// motions to the common motion bank (indices 6000-6005 with 16 / 18 / 32
// frames: ground start, loop, end, then air), those are used instead, as
// three actions (start once, loop while held, end).
//
// Online-safe: the decision is made inside the battle simulation, from the
// command word the battle consumes for that player (the word both PCs
// exchange) and from the fighter's own state:
// - L3 isn't part of the game's command word (it doesn't do anything in
//   fights), so the local input task puts it in bit 31, which the game never
//   sets or reads (BurstLimitKiChargeInput, sub_821721D8 after the pad and
//   direction bits are built). L3 together with R3 (the settings menu combo)
//   is not a charge.
// - Every fighter's command step (sub_82172A30, right after the consumed word
//   is read with sub_8217E060) starts, runs or ends the charge
//   (BurstLimitKiChargeCommand) and, while charging, hands the game an empty
//   word, so nothing else happens.
// - The charge actions have ids of their own (7000-7002 ground, 7512-7514
//   air, outside every action table) that the action lookup (sub_821A1260) is
//   pointed at (BurstLimitKiChargeAction): records made at runtime.
// - Ki goes up through the game's own "add ki" (sub_821A0030: clamps at the
//   maximum and plays the gauge-full cue).
// ki_charge and ki_charge_rate are host-synced settings in online lobbies.
// The aura (BurstLimitKiChargeAura) is drawing only.
//
// Fighter fields (fighter = *(player + 0x2C), players at 0x84239C10):
// +0 player index, +1184 motion bank table (bank b at +(338 + b) * 4),
// +1244 ki (s16), +1246 ki max, +1872 action id, +1884 common #BSK,
// +1908 action frame (float), +1912 action length, +1924 action loops,
// +1944 pose frame, +2128 state (0 = neutral, also in the air), +2144 neutral
// time (float), +2148 in the air.

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/memory.h>
#include <rex/memory/utils.h>
#include <rex/ppc.h>
#include <rex/ppc/context.h>
#include <rex/runtime.h>

REXCVAR_DEFINE_BOOL(ki_charge, false, "Patches",
                    "Hold L3 in a fight to charge Ki (Shin Budokai style): the fighter powers up "
                    "in place and the Ki gauge fills faster. Online, the host's setting is used.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_INT32(ki_charge_rate, 16, "Patches",
                     "Ki added per frame while charging (the gauge holds 3000; the game adds about "
                     "5 per frame on its own). 16 fills an empty gauge in about 2.4 s. Online, the "
                     "host's value is used.")
    .range(1, 200)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REX_EXTERN(sub_821A9280);  // start the action in fighter+1872
REX_EXTERN(sub_8218F0B0);  // enter state 0 (neutral, ground or air)
REX_EXTERN(sub_821A0030);  // add ki (fighter, amount)
REX_EXTERN(sub_821A6520);  // play a sound cue (fighter, bank, mode * 1000 + cue, 0)
REX_EXTERN(sub_821A62E0);  // play a voice cue (fighter, cue, 0, 1)
REX_EXTERN(sub_821F3970);  // spawn an effect (id, &{s16 player, u16 0, u32 mode})
REX_EXTERN(sub_82146558);  // spawn an aura effect (&descriptor, see SpawnAuraEffect)
REX_EXTERN(sub_82144868);  // aura effects (kind, &descriptor): 6 = the Aura Spark power-up
REX_EXTERN(sub_821500D0);  // an aura effect object's destroy callback
REX_EXTERN(sub_821499A8);  // ground effects (player, kind 0-16, &position, 0)
REX_EXTERN(sub_82148EF0);  // stop ground effects (player, which: -1 = all)

namespace {

constexpr uint32_t kFighterBanks = 1184;
constexpr uint32_t kFighterKi = 1244;
constexpr uint32_t kFighterKiMax = 1246;
constexpr uint32_t kFighterAction = 1872;
constexpr uint32_t kFighterCommonBsk = 1884;
constexpr uint32_t kFighterFrame = 1908;
constexpr uint32_t kFighterLength = 1912;
constexpr uint32_t kFighterPoseFrame = 1944;
constexpr uint32_t kFighterIdleTime = 2144;
constexpr uint32_t kFighterState = 2128;
constexpr uint32_t kFighterAir = 2148;

constexpr uint32_t kBattlePointer = 0x841B5130;
constexpr uint32_t kBattleCutscene = 574;  // u16, cinematics / Drama Pieces
constexpr uint32_t kBattleStep = 252;      // float, this tick's time step (slow motion < 1)

// Action ids: start, loop, end on the ground; +512 in the air (the game's
// own convention). The game's motion-26 look uses only the first.
constexpr int32_t kActionGround = 7000;
constexpr int32_t kActionAir = 7512;
constexpr int32_t kActionCount = 3;
constexpr int32_t kAuraSparkGround = 20;  // common #BSK slots the motion-26 headers come from
constexpr int32_t kAuraSparkAir = 532;

// Optional charge motions in the common bank (bank 0): ground start / loop /
// end, then air start / loop / end.
constexpr uint32_t kModMotion = 6000;

// BATTLE_COMMON_SE.CSB cues (bank 1): 9 "aura appears", 83 "Aura Spark
// fades", 84 / 85 "transformation start" ground / air (3.2 s of wind and
// rumble, not looped), 102 silent. Voice 60 = "Aura Spark activated".
constexpr uint32_t kCueAura = 9;
constexpr uint32_t kCueAuraEnd = 83;
constexpr uint32_t kCueWindGround = 84;
constexpr uint32_t kCueWindAir = 85;
constexpr uint32_t kCueSilent = 102;
constexpr uint32_t kVoiceShout = 60;
constexpr int32_t kWindTicks = 180;

// The command bit L3 is carried in (the game builds bits 32-63 only).
constexpr uint64_t kChargeBit = 0x80000000ull;
// The game's own action bit of the button that drains Ki (RT by default).
constexpr uint64_t kDrainBit = 1ull << 38;

// Pad state the game builds its command word from (sub_82170480): pads of
// 212 bytes at 0x8413F5F0, buttons (XInput bits) at +0 | +16; the controller
// slot (+76 of the input object) maps to a pad through 0x8424DB58 + 28 +
// slot * 28 (sub_82280298).
constexpr uint32_t kPads = 0x8413F5F0;
constexpr uint32_t kPadSize = 212;
constexpr uint32_t kSlotTable = 0x8424DB58 + 28;
constexpr uint32_t kButtonL3 = 0x40;
constexpr uint32_t kButtonR3 = 0x80;

// Aura drawing (sub_82145E98): per-player settings at *(0x826B1964) + player
// * 704 (+680 strength); constant 0x82084138 scales it.
constexpr uint32_t kAuraTablePointer = 0x826B1964;
constexpr uint32_t kAuraTableStride = 704;
constexpr uint32_t kAuraStrength = 680;
constexpr uint32_t kAuraScaleConstant = 0x82084138;

// The motion-26 charge (frames of the Aura Spark power-up): into the loop,
// back and forth over the clenched-fist frames while held, the arms-up burst
// and back to the stance as the end on a full gauge, played back toward
// frame 0 when let go.
struct Look {
  int loop_from = 18;
  int loop_to = 42;
  int end_to = 149;
  float in_speed = 1.5f;
  float end_speed = 1.5f;
  float out_speed = 3.0f;
  int hold = -1;  // testing: show this frame only
  uint32_t serial = 1;
};
std::mutex g_look_mutex;
Look g_look;

enum class Phase { kStart, kLoop, kEnd, kSettle };

// Per player, from the simulation only (so both PCs agree). Reset when a
// charge starts.
struct PlayerCharge {
  bool active = false;  // charging last tick
  bool mod = false;     // the mod's three actions
  Phase phase = Phase::kStart;
  float time = 0.0f;    // motion-26 look: frames since the start
  float frame = 0.0f;   // motion-26 look: frame shown
  float speed = 0.0f;   // motion-26 look: frames per tick, last tick
  int32_t ticks = 0;
  float ki_part = 0.0f;  // ki not added yet (slow motion)
  uint32_t wind = 0;         // the wind effect's object and its model handle
  uint32_t wind_handle = 0;
  bool dust = false;         // the transformation's ground dust is running
};
PlayerCharge g_players[2];

struct Records {
  uint32_t serial = 0;
  uint32_t look[2] = {};     // motion-26 look: ground, air
  uint32_t mod[2][3] = {};   // mod motions: ground / air x start, loop, end
} g_records;  // game thread only

rex::memory::Memory* Memory() {
  auto* runtime = rex::Runtime::instance();
  return runtime ? runtime->memory() : nullptr;
}

bool Readable(rex::memory::Memory* memory, uint32_t address, uint32_t size) {
  if (address < 0x10000) {
    return false;
  }
  auto* heap = memory->LookupHeap(address);
  rex::memory::HeapAllocationInfo info = {};
  return heap && heap->QueryRegionInfo(address, &info) &&
         (info.state & rex::memory::kMemoryAllocationCommit) &&
         uint64_t(address) + size <= uint64_t(info.base_address) + info.region_size;
}

template <typename T>
T Load(rex::memory::Memory* memory, uint32_t address) {
  return rex::memory::load_and_swap<T>(memory->TranslateVirtual<uint8_t*>(address));
}
template <typename T>
void Store(rex::memory::Memory* memory, uint32_t address, T value) {
  rex::memory::store_and_swap<T>(memory->TranslateVirtual<uint8_t*>(address), value);
}

Look CurrentLook() {
  std::lock_guard<std::mutex> lock(g_look_mutex);
  return g_look;
}

// --- action records ------------------------------------------------------------

using Key = std::array<uint32_t, 3>;  // frame, word 0, word 1
struct Track {
  uint16_t type;
  std::vector<Key> keys;
};

// One action record (0x30 bytes, as in a loaded #BSK: +0 motion, +2 bank,
// +4 length cap, +0x18/+0x1C/+0x20 extra motions, +0x28 track count, +0x2C
// track list) followed by its tracks: 8-byte entries (type, key count, keys),
// 20-byte keys (frame, slot, on, four words).
uint32_t BuildRecord(rex::memory::Memory* memory, const uint8_t header[0x30],
                     const std::vector<Track>& tracks) {
  size_t key_count = 0;
  for (const Track& t : tracks) {
    key_count += t.keys.size();
  }
  const uint32_t size = 0x30 + uint32_t(tracks.size()) * 8 + uint32_t(key_count) * 20;
  const uint32_t record = memory->SystemHeapAlloc(size);
  if (!record) {
    return 0;
  }
  std::memcpy(memory->TranslateVirtual<uint8_t*>(record), header, 0x30);
  Store<uint32_t>(memory, record + 0x28, uint32_t(tracks.size()));
  Store<uint32_t>(memory, record + 0x2C, record + 0x30);
  uint32_t entry = record + 0x30;
  uint32_t keys = entry + uint32_t(tracks.size()) * 8;
  for (const Track& t : tracks) {
    Store<uint16_t>(memory, entry, t.type);
    Store<uint16_t>(memory, entry + 2, uint16_t(t.keys.size()));
    Store<uint32_t>(memory, entry + 4, keys);
    entry += 8;
    for (const Key& k : t.keys) {
      Store<uint16_t>(memory, keys, uint16_t(k[0]));
      Store<uint8_t>(memory, keys + 2, 0);
      Store<uint8_t>(memory, keys + 3, 1);
      Store<uint32_t>(memory, keys + 4, k[1]);
      Store<uint32_t>(memory, keys + 8, k[2]);
      Store<uint32_t>(memory, keys + 12, 0);
      Store<uint32_t>(memory, keys + 16, 0);
      keys += 20;
    }
  }
  return record;
}

// The record of a common slot (fighter+1884 = BCCMN's first #BSK).
uint32_t CommonSlot(rex::memory::Memory* memory, uint32_t fighter, int32_t slot) {
  const uint32_t bsk = Load<uint32_t>(memory, fighter + kFighterCommonBsk);
  if (!Readable(memory, bsk, 0x20) || slot >= int32_t(Load<uint32_t>(memory, bsk + 16))) {
    return 0;
  }
  const uint32_t table = Load<uint32_t>(memory, bsk + 20);
  if (!Readable(memory, table + slot * 4, 4)) {
    return 0;
  }
  const uint32_t record = Load<uint32_t>(memory, table + slot * 4);
  return Readable(memory, record, 0x30) ? record : 0;
}

// The common motion bank's (bank 0) table entry of a motion, or 0.
uint32_t CommonMotion(rex::memory::Memory* memory, uint32_t fighter, uint32_t motion) {
  const uint32_t banks = Load<uint32_t>(memory, fighter + kFighterBanks);
  if (!Readable(memory, banks + 338 * 4, 4)) {
    return 0;
  }
  const uint32_t bank = Load<uint32_t>(memory, banks + 338 * 4);
  if (!Readable(memory, bank, 0x20) || Load<uint32_t>(memory, bank + 16) <= motion) {
    return 0;
  }
  const uint32_t entry = Load<uint32_t>(memory, bank + 20) + motion * 8;
  return Readable(memory, entry, 8) && Load<uint16_t>(memory, entry) &&
                 Load<uint32_t>(memory, entry + 4)
             ? entry
             : 0;
}

// Whether the common motion bank has the mod's six charge motions.
bool HasModMotions(rex::memory::Memory* memory, uint32_t fighter) {
  for (uint32_t i = 0; i < 6; ++i) {
    if (!CommonMotion(memory, fighter, kModMotion + i)) {
      return false;
    }
  }
  return true;
}

// Type 5 (state flags: 0x3 while the action runs, cleared on its last frame)
// and type 6 (face: +442 eyes / +443 mouth, +444 the air legs) as the game's
// own idle and power-up actions have them.
Track Flags(uint32_t length) {
  return {5, {Key{0, 0x3, 0x1C}, Key{length, 0x0, 0x1F}}};
}
Track Face(std::vector<Key> keys) {
  return {6, std::move(keys)};
}

bool EnsureRecords(rex::memory::Memory* memory, uint32_t fighter) {
  const Look look = CurrentLook();
  if (g_records.look[0] && g_records.serial == look.serial &&
      (g_records.mod[1][2] || !HasModMotions(memory, fighter))) {
    return true;
  }
  const uint32_t ground = CommonSlot(memory, fighter, kAuraSparkGround);
  const uint32_t air = CommonSlot(memory, fighter, kAuraSparkAir);
  if (!ground || !air) {
    return false;
  }
  // Records stay allocated (a fighter may still be in one); only replaced
  // when testing other looks.
  uint8_t header[0x30];
  for (int a = 0; a < 2; ++a) {
    std::memcpy(header, memory->TranslateVirtual<uint8_t*>(a ? air : ground), 0x30);
    header[4] = header[5] = header[6] = header[7] = 0;  // no length cap: the frames are ours
    std::vector<Key> face = {Key{0, 0, 1}, Key{0, 1, 1}};
    if (a) {
      face.push_back(Key{0, 2, 5});  // hovering legs, as the air Aura Spark
    }
    g_records.look[a] = BuildRecord(memory, header, {Flags(150), Face(face)});
  }
  // The mod's motions (when there): start / loop / end, ground and air, the
  // faces of Shin Budokai 2's charge actions.
  for (int a = 0; a < 2 && HasModMotions(memory, fighter); ++a) {
    for (int i = 0; i < 3; ++i) {
      const uint32_t motion = kModMotion + uint32_t(a * 3 + i);
      const uint32_t frames = Load<uint16_t>(memory, CommonMotion(memory, fighter, motion));
      std::memset(header, 0, sizeof(header));
      header[0] = uint8_t(motion >> 8);
      header[1] = uint8_t(motion);  // +2 bank 0 (common)
      std::memset(header + 0x18, 0xFF, 4);
      header[0x1F] = uint8_t(a);  // +0x1C hands
      std::memset(header + 0x20, 0xFF, 4);
      std::vector<Key> face;
      const uint32_t lead = i == 0 ? frames - 16 : 0;  // the start's lead-in frames
      if (i == 0) {
        face = a ? std::vector<Key>{Key{lead + 9, 0, 4}, Key{lead + 9, 1, 4}, Key{lead + 15, 0, 1},
                                    Key{lead + 15, 1, 1}}
                 : std::vector<Key>{Key{lead + 8, 0, 1}, Key{lead + 8, 1, 1}};
      } else if (i == 1) {
        face = {Key{0, 0, 1}, Key{0, 1, 1}};
      } else {
        face = {Key{0, 0, 1}, Key{0, 1, 1}, Key{a ? 20u : 3u, 0, 0}, Key{a ? 20u : 3u, 1, 0}};
      }
      g_records.mod[a][i] = BuildRecord(memory, header, {Flags(frames), Face(face)});
    }
  }
  g_records.serial = look.serial;
  REXLOG_INFO("Ki charge: action records ready");
  return g_records.look[0] && g_records.look[1];
}

int32_t ActionIndex(int32_t action) {
  if (action >= kActionGround && action < kActionGround + kActionCount) {
    return action - kActionGround;
  }
  if (action >= kActionAir && action < kActionAir + kActionCount) {
    return action - kActionAir;
  }
  return -1;
}

bool Charging(rex::memory::Memory* memory, uint32_t fighter) {
  return Load<int32_t>(memory, fighter + kFighterState) == 0 &&
         ActionIndex(Load<int32_t>(memory, fighter + kFighterAction)) >= 0;
}

int32_t PlayerOf(rex::memory::Memory* memory, uint32_t fighter) {
  return Load<int16_t>(memory, fighter);
}

bool InAir(rex::memory::Memory* memory, uint32_t fighter) {
  return Load<int32_t>(memory, fighter + kFighterAir) != 0;
}

void Call(PPCFunc* function, uint32_t fighter) {
  rex::ppc::GuestToHostFunction<void>(function, fighter);
}

void PlaySound(uint32_t fighter, uint32_t kind, uint32_t value) {
  rex::ppc::GuestToHostFunction<void>(sub_821A6520, fighter, kind, value, uint32_t(0));
}

// Spawns one of the game's effects at a player, as a timeline's kind-4 event
// does (sub_821ABD58 -> sub_821F3970).
uint32_t g_effect_arg = 0;  // game thread only
void SpawnEffect(rex::memory::Memory* memory, int32_t player, uint32_t id, uint32_t mode = 0) {
  if (!g_effect_arg) {
    g_effect_arg = memory->SystemHeapAlloc(16);
    if (!g_effect_arg) {
      return;
    }
  }
  Store<int16_t>(memory, g_effect_arg, int16_t(player));
  Store<uint16_t>(memory, g_effect_arg + 2, 0);
  Store<uint32_t>(memory, g_effect_arg + 4, mode);
  Store<uint32_t>(memory, g_effect_arg + 8, 0);
  rex::ppc::GuestToHostFunction<void>(sub_821F3970, id, g_effect_arg);
}

// Spawns one of a player's aura effects (the per-character aura bank, ids
// listed at aura table + 512): what the game's Aura Spark (sub_82146988,
// id 11 + 3 * aura color) and full-gauge flash (sub_82146A00, 40 + color) do.
// Descriptor: +0 position offset (vec4), +4 matrix (0), +12 player, +16
// attach mode 4, +20 0, +24 alpha (float, 255 = full), +28 effect id.
uint32_t g_aura_arg = 0;  // game thread only
void SpawnAuraEffect(rex::memory::Memory* memory, int32_t player, uint32_t id, float alpha = 255.0f) {
  if (!g_aura_arg) {
    g_aura_arg = memory->SystemHeapAlloc(64);
    if (!g_aura_arg) {
      return;
    }
  }
  const uint32_t vec = g_aura_arg + 32;
  for (uint32_t i = 0; i < 64; i += 4) {
    Store<uint32_t>(memory, g_aura_arg + i, 0);
  }
  Store<uint32_t>(memory, g_aura_arg + 0, vec);
  Store<int16_t>(memory, g_aura_arg + 12, int16_t(player));
  Store<uint32_t>(memory, g_aura_arg + 16, 4);
  Store<float>(memory, g_aura_arg + 24, alpha);
  Store<int16_t>(memory, g_aura_arg + 28, int16_t(id));
  rex::ppc::GuestToHostFunction<void>(sub_82146558, g_aura_arg);
}

// The wind: the first second of the game's Aura Spark power-up effect (aura
// effect 11 + 3 * aura color, through sub_82144868 kind 6 like the Aura
// Spark's own event 318): a ki splash at the feet with dust, rising light
// and gusts curling round the body. That effect ends in the Aura Spark's
// sphere burst after about 1.2 s, so each one is taken away before that and a
// new one started (kWindEffectTicks); the charge's end takes the last one
// away. Effect objects (304 bytes, sub_8214F948) are in the list at
// 0x8415A600, newest last; +8 state (-1 = remove), +12 destroy callback,
// +16 model handle, +28 player, +30 attach mode (4 = aura).
constexpr uint32_t kEffectList = 0x8415A600;
constexpr uint32_t kEffectDestroy = 0x821500D0;
constexpr int32_t kWindEffectTicks = 54;
uint32_t g_wind_arg = 0;  // game thread only

void StopWindEffect(rex::memory::Memory* memory, int32_t player, PlayerCharge& pc) {
  const uint32_t object = pc.wind;
  pc.wind = 0;
  if (!object || !Readable(memory, object, 48) ||
      Load<int16_t>(memory, object + 28) != player || Load<uint16_t>(memory, object + 30) != 4 ||
      Load<uint32_t>(memory, object + 16) != pc.wind_handle || !pc.wind_handle ||
      Load<uint32_t>(memory, object + 12) != kEffectDestroy ||
      Load<uint32_t>(memory, object + 8) == 0xFFFFFFFFu) {
    return;  // already gone (or the slot was reused)
  }
  Store<uint32_t>(memory, object + 8, 0xFFFFFFFFu);
  rex::ppc::GuestToHostFunction<void>(sub_821500D0, object);
}

void StartWindEffect(rex::memory::Memory* memory, int32_t player, PlayerCharge& pc) {
  StopWindEffect(memory, player, pc);
  if (!g_wind_arg) {
    g_wind_arg = memory->SystemHeapAlloc(64);
    if (!g_wind_arg) {
      return;
    }
  }
  for (uint32_t i = 0; i < 64; i += 4) {
    Store<uint32_t>(memory, g_wind_arg + i, 0);
  }
  Store<uint32_t>(memory, g_wind_arg + 0, g_wind_arg + 32);  // offset (0, 0, 0, 0)
  Store<int16_t>(memory, g_wind_arg + 12, int16_t(player));
  Store<uint32_t>(memory, g_wind_arg + 16, 4);
  const uint32_t before = Load<uint32_t>(memory, kEffectList + 4);
  rex::ppc::GuestToHostFunction<void>(sub_82144868, uint32_t(6), g_wind_arg);
  const uint32_t object = Load<uint32_t>(memory, kEffectList + 4);
  if (object != before && object != kEffectList && Readable(memory, object, 48) &&
      Load<int16_t>(memory, object + 28) == player) {
    pc.wind = object;
    pc.wind_handle = Load<uint32_t>(memory, object + 16);
  }
}

// The ground dust of the game's transformations (Goku's 1260 / 1262 events
// 250, 251, 260 in sub_821AB618): sub_821499A8 kind 15 = dust swirling at the
// feet with ground shock rings, running until stopped (sub_82148EF0(player,
// -1), event 260); kind 16 = the dust blast thrown outward. Both take the
// colour of the stage's ground. Position: fighter + 256.
constexpr uint32_t kDustLoop = 15;
constexpr uint32_t kDustBlast = 16;

void GroundEffect(uint32_t fighter, int32_t player, uint32_t kind) {
  rex::ppc::GuestToHostFunction<void>(sub_821499A8, uint32_t(player), kind, fighter + 256,
                                      uint32_t(0));
}

void StopDust(int32_t player, PlayerCharge& pc) {
  if (pc.dust) {
    pc.dust = false;
    rex::ppc::GuestToHostFunction<void>(sub_82148EF0, uint32_t(player), uint32_t(0xFFFFFFFFu));
  }
}

// Testing: an effect to spawn at P0 on the next battle tick (ki_charge_fx).
std::atomic<int32_t> g_fx_request{-1};
std::atomic<uint32_t> g_fx_mode{0};

// Starts one of the charge actions (index 0-2) in the fighter's place.
void StartAction(rex::memory::Memory* memory, uint32_t fighter, int32_t index) {
  Store<int32_t>(memory, fighter + kFighterAction,
                 (InAir(memory, fighter) ? kActionAir : kActionGround) + index);
  Call(sub_821A9280, fighter);
}

// The wind runs in the fighter's mode-2 sound slot: the game's silent cue
// played in that slot stops it.
void StopWind(uint32_t fighter) {
  PlaySound(fighter, 1, 2000 + kCueSilent);
}

void Finish(rex::memory::Memory* memory, uint32_t fighter, PlayerCharge& pc, const char* why) {
  StopWindEffect(memory, PlayerOf(memory, fighter), pc);
  StopDust(PlayerOf(memory, fighter), pc);
  pc.active = false;
  REXLOG_INFO("[KiCharge] P{} end ({}) ki={}", PlayerOf(memory, fighter), why,
              Load<int16_t>(memory, fighter + kFighterKi));
}

void BeginEnd(rex::memory::Memory* memory, uint32_t fighter, PlayerCharge& pc, bool full) {
  StopWind(fighter);
  StopWindEffect(memory, PlayerOf(memory, fighter), pc);
  if (pc.dust) {
    StopDust(PlayerOf(memory, fighter), pc);
    if (full) {
      GroundEffect(fighter, PlayerOf(memory, fighter), kDustBlast);  // the gauge is full: a last blast
    }
  }
  PlaySound(fighter, 1, kCueAuraEnd);
  if (pc.mod) {
    pc.phase = Phase::kEnd;
    StartAction(memory, fighter, 2);
  } else if (full) {
    // On from the clenched-fist frame shown into the arms-up burst.
    pc.phase = Phase::kEnd;
  } else {
    pc.phase = Phase::kSettle;
  }
  REXLOG_INFO("[KiCharge] P{} {} ki={}", PlayerOf(memory, fighter),
              full ? "full: end" : "released: end", Load<int16_t>(memory, fighter + kFighterKi));
}

// Motion-26 look while held: eased into the loop (from frame 0, slowing down
// to a stop at loop_to), then a smooth back-and-forth (a sine) between
// loop_to and loop_from, so the pose never jumps and never changes direction
// abruptly. The start runs at in_speed at first; one loop takes as long as
// going over it twice at in_speed would.
float LoopFrame(const Look& look, float time) {
  if (look.hold >= 0) {
    return float(look.hold);
  }
  const float to = float(look.loop_to);
  const float entry = 2.0f * to / std::max(look.in_speed, 0.01f);
  if (time < entry || look.loop_to <= look.loop_from) {
    const float u = std::min(time / entry, 1.0f);
    return to * (1.0f - (1.0f - u) * (1.0f - u));
  }
  const float span = float(look.loop_to - look.loop_from);
  const float period = 2.0f * span / std::max(look.in_speed, 0.01f) * 1.5707963f;
  const float theta = (time - entry) / period * 6.2831853f;
  return float(look.loop_from) + span * 0.5f * (1.0f + std::cos(theta));
}

// The mod's actions: true once the current one has played to its end
// (checked before the game's own wrap back to frame 0).
bool ActionDone(rex::memory::Memory* memory, uint32_t fighter, float step) {
  const float frame = Load<float>(memory, fighter + kFighterFrame);
  const int32_t length = Load<int32_t>(memory, fighter + kFighterLength);
  return frame + step >= float(length) - 0.001f || Load<int32_t>(memory, fighter + 1924) > 0;
}

}  // namespace

// Mid-asm hook at 0x82172340 in sub_821721D8 (the local input task, after
// the controller's command word at r31+64 got its pad and direction bits and
// only when input isn't locked): L3 held (without R3) -> bit 31.
void BurstLimitKiChargeInput(PPCRegister& r31) {
  if (!REXCVAR_GET(ki_charge)) {
    return;
  }
  auto* memory = Memory();
  const uint32_t input = r31.u32;
  if (!memory || !Readable(memory, input + 64, 16)) {
    return;
  }
  const int32_t slot = Load<int32_t>(memory, input + 76);
  if (slot < 0 || slot >= 2) {
    return;
  }
  const uint32_t pad = Load<uint32_t>(memory, kSlotTable + uint32_t(slot) * 28);
  if (pad >= 4) {
    return;
  }
  const uint32_t base = kPads + pad * kPadSize;
  const uint32_t buttons = Load<uint32_t>(memory, base) | Load<uint32_t>(memory, base + 16);
  static bool logged[2] = {};
  const bool l3 = (buttons & kButtonL3) && !(buttons & kButtonR3);
  if (l3) {
    Store<uint64_t>(memory, input + 64, Load<uint64_t>(memory, input + 64) | kChargeBit);
  }
  if (l3 != logged[slot]) {
    logged[slot] = l3;
    REXLOG_INFO("[KiCharge] input slot {} pad {}: L3 {} (buttons {:08X})", slot, pad,
                l3 ? "down" : "up", buttons);
  }
}

// Mid-asm hook at 0x82172AA4 in sub_82172A30 (a fighter's command step, in
// the battle simulation, r21 = fighter): the consumed command word is at
// r1+80. Starts, runs or ends the charge.
void BurstLimitKiChargeCommand(PPCRegister& r1, PPCRegister& r21, PPCRegister& r31) {
  (void)r31;
  auto* memory = Memory();
  const uint32_t fighter = r21.u32;
  if (!memory || !Readable(memory, fighter, 0xE00)) {
    return;
  }
  const int32_t player = PlayerOf(memory, fighter);
  if (player < 0 || player > 1) {
    return;
  }
  PlayerCharge& pc = g_players[player];
  if (player == 0) {
    const int32_t fx = g_fx_request.exchange(-1);
    if (fx >= 0) {
      REXLOG_WARN("ki_charge_fx: effect {} mode {}", fx, g_fx_mode.load());
      if (g_fx_mode.load() == 99) {
        SpawnAuraEffect(memory, 0, uint32_t(fx));
      } else if (g_fx_mode.load() == 98) {
        rex::ppc::GuestToHostFunction<void>(sub_821499A8, uint32_t(0), uint32_t(fx), fighter + 256,
                                            uint32_t(0));
      } else if (g_fx_mode.load() == 97) {
        rex::ppc::GuestToHostFunction<void>(sub_82148EF0, uint32_t(0), uint32_t(0xFFFFFFFFu));
      } else {
        SpawnEffect(memory, 0, uint32_t(fx), g_fx_mode.load());
      }
    }
  }
  const uint32_t word = r1.u32 + 80;
  const uint64_t command = Load<uint64_t>(memory, word);
  const bool enabled = REXCVAR_GET(ki_charge);
  // Holding the Ki-drain button (RT by default: the game's action bit 38)
  // wins: no charge starts while it's held, and it ends a charge.
  const bool held = enabled && (command & kChargeBit) != 0 && (command & kDrainBit) == 0;
  const bool charging = Charging(memory, fighter);
  if (!charging) {
    if (pc.active) {
      // Something else took over (a hit, a cinematic, the round's end).
      StopWind(fighter);
      Finish(memory, fighter, pc, "interrupted");
    }
    if (!held) {
      return;
    }
  }
  const uint32_t battle = Load<uint32_t>(memory, kBattlePointer);
  const bool battle_ok = Readable(memory, battle + kBattleCutscene, 2);
  const bool cutscene = battle_ok && Load<uint16_t>(memory, battle + kBattleCutscene);
  float step = battle_ok ? Load<float>(memory, battle + kBattleStep) : 1.0f;
  if (!(step >= 0.0f) || step > 1.0f) {
    step = 1.0f;
  }
  const int16_t ki = Load<int16_t>(memory, fighter + kFighterKi);
  const int16_t ki_max = Load<int16_t>(memory, fighter + kFighterKiMax);
  const Look look = CurrentLook();

  if (!charging) {
    // Free: neutral (state 0, standing or hovering), walking (2, 3) or the
    // idle taunt (52), with room in the gauge.
    const int32_t state = Load<int32_t>(memory, fighter + kFighterState);
    if (cutscene || ki >= ki_max || (state != 0 && state != 2 && state != 3 && state != 52) ||
        !EnsureRecords(memory, fighter)) {
      return;
    }
    if (state != 0) {
      Call(sub_8218F0B0, fighter);
      if (Load<int32_t>(memory, fighter + kFighterState) != 0) {
        return;
      }
    }
    pc = PlayerCharge{};
    pc.active = true;
    pc.mod = HasModMotions(memory, fighter) && g_records.mod[1][2];
    StartAction(memory, fighter, 0);
    PlaySound(fighter, 1, kCueAura);
    rex::ppc::GuestToHostFunction<void>(sub_821A62E0, fighter, kVoiceShout, uint32_t(0),
                                        uint32_t(1));
    StartWindEffect(memory, player, pc);
    if (!InAir(memory, fighter)) {
      GroundEffect(fighter, player, kDustBlast);
      GroundEffect(fighter, player, kDustLoop);
      pc.dust = true;
    }
    REXLOG_INFO("[KiCharge] P{} start ({}, {}) ki={} command={:016X}", player,
                InAir(memory, fighter) ? "air" : "ground", pc.mod ? "mod motions" : "motion 26",
                ki, command);
  } else if (cutscene || !enabled) {
    StopWind(fighter);
    Call(sub_8218F0B0, fighter);
    Finish(memory, fighter, pc, "cinematic");
    return;
  } else if (!pc.active) {
    // In a charge action without our record of it (e.g. the setting was
    // switched mid-charge): back to neutral.
    Call(sub_8218F0B0, fighter);
    return;
  }

  // Phase changes.
  const bool charging_phase = pc.phase == Phase::kStart || pc.phase == Phase::kLoop;
  if (charging_phase && (!held || ki >= ki_max)) {
    BeginEnd(memory, fighter, pc, ki >= ki_max);
  } else if (pc.phase == Phase::kSettle && held && ki < ki_max) {
    pc.phase = Phase::kLoop;  // held again while settling: back into the loop from here
    pc.time = pc.frame;
  }

  if (pc.mod) {
    if (pc.phase == Phase::kStart && ActionDone(memory, fighter, step)) {
      pc.phase = Phase::kLoop;
      StartAction(memory, fighter, 1);
    } else if (pc.phase == Phase::kEnd && ActionDone(memory, fighter, step)) {
      Call(sub_8218F0B0, fighter);
      Finish(memory, fighter, pc, "done");
      Store<uint64_t>(memory, word, command & ~kChargeBit);
      return;
    }
  } else {
    if (pc.phase == Phase::kStart || pc.phase == Phase::kLoop) {
      const float before = pc.frame;
      pc.frame = LoopFrame(look, pc.time);
      pc.speed = (pc.frame - before) / std::max(step, 0.01f);
      pc.time += step;
      pc.phase = pc.time > 2.0f * float(look.loop_to) / std::max(look.in_speed, 0.01f)
                     ? Phase::kLoop
                     : Phase::kStart;
    } else {
      const bool end = pc.phase == Phase::kEnd;
      // Let go before the gauge is full: any other input ends the settle at
      // once and goes through.
      if (!end && (command & ~kChargeBit) != 0) {
        Call(sub_8218F0B0, fighter);
        Finish(memory, fighter, pc, "released");
        Store<uint64_t>(memory, word, command & ~kChargeBit);
        return;
      }
      // Speed up (or turn round) smoothly instead of at once.
      const float target = end ? look.end_speed : -look.out_speed;
      const float accel = 0.15f * step;
      pc.speed = pc.speed < target ? std::min(pc.speed + accel, target)
                                   : std::max(pc.speed - accel, target);
      pc.frame += pc.speed * step;
      if (end ? pc.frame >= float(look.end_to) : pc.frame <= 0.0f) {
        Call(sub_8218F0B0, fighter);
        Finish(memory, fighter, pc, end ? "done" : "released");
        Store<uint64_t>(memory, word, command & ~kChargeBit);
        return;
      }
    }
    Store<float>(memory, fighter + kFighterFrame, pc.frame);
    Store<float>(memory, fighter + kFighterPoseFrame, pc.frame);
  }

  // This tick belongs to the charge: nothing else of the word reaches the
  // game, the fighter keeps its place.
  Store<uint64_t>(memory, word, 0);
  Store<float>(memory, fighter + kFighterIdleTime, 0.0f);
  if (pc.phase == Phase::kStart || pc.phase == Phase::kLoop) {
    if (pc.ticks > 0 && pc.ticks % kWindEffectTicks == 0) {
      StartWindEffect(memory, player, pc);
    }
    if (pc.ticks % kWindTicks == 0) {
      // The power-up wind (3.2 s), again while held. Mode 2 replaces the
      // previous one instead of stacking.
      PlaySound(fighter, 1, 2000 + (InAir(memory, fighter) ? kCueWindAir : kCueWindGround));
    }
    ++pc.ticks;
    if (ki < ki_max) {
      // Per battle step, so slow motion fills slower like everything else.
      const int32_t rate = std::clamp<int32_t>(REXCVAR_GET(ki_charge_rate), 1, 200);
      pc.ki_part += float(rate) * step;
      const int32_t add = int32_t(pc.ki_part);
      pc.ki_part -= float(add);
      if (add > 0) {
        rex::ppc::GuestToHostFunction<void>(sub_821A0030, fighter, uint32_t(add));
      }
    }
  }
}

// Mid-asm hook at 0x821A135C in sub_821A1260 (action lookup, r31 = fighter,
// r8 = the record found or 0, r6 = its bank, r11 = its #BSK): our ids get our
// records.
void BurstLimitKiChargeAction(PPCRegister& r6, PPCRegister& r8, PPCRegister& r11,
                              PPCRegister& r31) {
  auto* memory = Memory();
  if (!memory || r8.u32 != 0) {
    return;
  }
  const uint32_t fighter = r31.u32;
  const int32_t action = Load<int32_t>(memory, fighter + kFighterAction);
  const int32_t index = ActionIndex(action);
  if (index < 0) {
    return;
  }
  const int air = action >= kActionAir ? 1 : 0;
  const int32_t player = PlayerOf(memory, fighter);
  const bool mod = player >= 0 && player <= 1 && g_players[player].mod;
  const uint32_t record = mod ? g_records.mod[air][index] : g_records.look[air];
  if (!record) {
    return;
  }
  r8.u64 = record;
  r6.s64 = Load<int16_t>(memory, record + 2);
  r11.u64 = Load<uint32_t>(memory, fighter + kFighterCommonBsk);
}

// Mid-asm hook at 0x821460F0 in sub_82145E98 (the auras, drawing; r29 =
// player, r30 = fighter, r31 = the aura's state, +40 its strength this frame,
// +44 the base): while charging, the aura burns at full strength instead of
// following the Ki gauge.
void BurstLimitKiChargeAura(PPCRegister& r29, PPCRegister& r30, PPCRegister& r31) {
  auto* memory = Memory();
  if (!memory || r29.u32 > 1 || !Readable(memory, r30.u32, 0xE00) ||
      !Readable(memory, r31.u32 + 40, 8)) {
    return;
  }
  const PlayerCharge& pc = g_players[r29.u32];
  if (!pc.active || pc.phase == Phase::kSettle || !Charging(memory, r30.u32)) {
    return;
  }
  const uint32_t table = Load<uint32_t>(memory, kAuraTablePointer);
  if (!Readable(memory, table + r29.u32 * kAuraTableStride + kAuraStrength, 4)) {
    return;
  }
  const float full = Load<float>(memory, r31.u32 + 44) *
                     Load<float>(memory, table + r29.u32 * kAuraTableStride + kAuraStrength) *
                     Load<float>(memory, kAuraScaleConstant);
  if (full > Load<float>(memory, r31.u32 + 40)) {
    Store<float>(memory, r31.u32 + 40, full);
  }
}

namespace {

// ki_charge_tune: testing the motion-26 look. key=value ...: from, to (the
// loop), end (last frame of the end), in, endspeed, out (speeds), hold
// (show one frame, -1 off).
void Tune(std::string_view args) {
  std::lock_guard<std::mutex> lock(g_look_mutex);
  std::string text(args);
  size_t start = 0;
  while (start < text.size()) {
    size_t end = text.find(' ', start);
    if (end == std::string::npos) {
      end = text.size();
    }
    const std::string part = text.substr(start, end - start);
    start = end + 1;
    const size_t eq = part.find('=');
    if (eq == std::string::npos) {
      continue;
    }
    const std::string name = part.substr(0, eq);
    const char* value = part.c_str() + eq + 1;
    if (name == "from") {
      g_look.loop_from = std::atoi(value);
    } else if (name == "to") {
      g_look.loop_to = std::atoi(value);
    } else if (name == "end") {
      g_look.end_to = std::atoi(value);
    } else if (name == "in") {
      g_look.in_speed = float(std::atof(value));
    } else if (name == "endspeed") {
      g_look.end_speed = float(std::atof(value));
    } else if (name == "out") {
      g_look.out_speed = float(std::atof(value));
    } else if (name == "hold") {
      g_look.hold = std::atoi(value);
    }
  }
  REXLOG_WARN("ki_charge_tune: loop {}-{} end {} speeds {}/{}/{} hold {}", g_look.loop_from,
              g_look.loop_to, g_look.end_to, g_look.in_speed, g_look.end_speed, g_look.out_speed,
              g_look.hold);
}

}  // namespace

namespace {

void SpawnEffectCommand(std::string_view args) {
  const std::string text(args);
  unsigned id = 0, mode = 0;
  if (std::sscanf(text.c_str(), "%u %u", &id, &mode) >= 1) {
    g_fx_mode = mode;
    g_fx_request = int32_t(id);
  }
}

}  // namespace

REXCVAR_DEFINE_COMMAND_ARGS(ki_charge_fx, SpawnEffectCommand, "Debug",
                            "Spawn a game effect at player 1 for testing: <id> [mode]");

REXCVAR_DEFINE_COMMAND_ARGS(ki_charge_tune, Tune, "Debug",
                            "Ki charge look for testing: from= to= end= in= endspeed= out= hold=");
