// Stage transitions (GitHub issue #33): the short cinematic that sends the fight from a stage to its aerial
// version (a blow that sends the opponent into a "transition wall" of the stage: Namek 24 -> 25, the space above
// Namek with Frieza's ship, ...).
//
// How the game does it (measured, v0.8.2 round 10): the stage's own script (the #SPX in the stage's "C" pack,
// run by the script interpreter sub_8212AB78) plays the cinematic and calls four commands of the effect manager
// E = 0x841584E0 (two "scenes": +96 the current one, +100 + 4 * s the stage loaded into scene s, -1 none; at the
// fight's start sub_8214C6B8 loads the fight's stage into scene 1 for an even stage id, 0 for an odd one):
//   0x14B sub_8214E670  the paired stage of the current one: sub_8214B2F8(stage)
//   0x148 sub_8214E528  load a stage into a scene: sub_8214C238(scene, stage) (writes E+100[scene])
//   0x149 sub_8214E7B8  wait until that scene's resources are loaded (+ a synchronized event online)
//   0x146 sub_8214E4C8  switch to that scene: sub_8214BDF0(scene, 0) (stage drawing, effects, collision bounds,
//                       battle+1160 bit 0x10 "ground stage")
// The paired stage (sub_8214B2F8): in Versus / Training / the attract demo (request mode 1 / 2 / 6 - online
// matches are mode 1 too) the game walks the story chain 0x8200AEF8 from the current stage to the first stage of
// the other kind (ground <-> air) that is UNLOCKED in this console's save (battle settings +155, 27 bytes);
// -1 if none is. Other modes use the fixed table 0x8200AFD0.
//
// What went wrong:
//  - v0.8.1's rb2_fx_scene_fix (R40) kept the fight's first scene on EVERY script switch: the transition's own
//    switch never happened, the fighters stayed at the aerial start positions above the old stage and the next
//    hit started the transition again (the loop of issue #33; the next round started on the ground).
//  - R40 itself (the stage vanishing in the test runs): the test saves have no aerial stage unlocked, so the
//    paired stage was -1, nothing was loaded (the load of stage -1 is skipped by rb2_fx_guard_null - the game
//    itself would read through a NULL table entry there) and the switch went to an empty scene.
//  - Online the paired stage depended on each console's own save: two PCs with different unlocks would load
//    different aerial stages (a desync).
// The fixes (stage_transition_fix, default on):
//  - the paired stage of a Versus / Training fight never depends on the save's unlocks when it would be -1, and
//    never online: the stage's own aerial / ground version is used (what a 100 % save gets); deterministic;
//  - a script switch to a scene with no stage loaded (E+100[scene] == -1) keeps the current scene (the R40 safety
//    net; with the paired-stage fix it does not happen on the game's stages).
// What starts one (sub_82187610, a fighter hitting the stage's walls, every battle tick): a fighter in state 33
// (blown away, +2128) that hits a wall entry with flag 0x10 ("transition wall", +32 of the 48-byte entries of the
// stage's collision list) fast enough (+1036 >= 10.0), with no cinematic running (battle+574 == 0), no event
// (sub_82173A98() == -1, [0x841B5098]) and the global flag [[0x825205E8]+3296] bit 0 clear, posts battle message 0
// with the current scene's stage script (sub_8214B758) - the transition cinematic - and puts the fighter in state 4.
// Any other wall hit takes the wall-hit reaction sub_8218DF20(fighter, 43). On the aerial stage sub_82197728 (state
// 34) posts the stage script the same way (the way back).
// An ultimate clash (both fighters' ultimates) also ends in the stage script's transition sequence (measured: the
// sequence runs without a wall hit, r10_g1 frame 4125).
// The option (stage_transitions, default on), off:
//  - the two posts never happen: the hit is the game's own wall hit (reaction 43) / nothing. The gate is the
//    sub_82173A98 call of each site (lr 0x8218779C / 0x82197768): it answers "an event is running" there, so the
//    game takes its own no-transition branch;
//  - a script that still asks for the paired stage (the ultimate clash) gets -1: nothing is loaded (the load of
//    -1 is skipped, rb2.cpp) and the switch keeps the current scene: the cinematic plays, the fight stays.

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <string>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/memory.h>
#include <rex/memory/utils.h>
#include <rex/net/online.h>
#include <rex/net/session.h>
#include <rex/ppc/context.h>
#include <rex/ppc/function.h>
#include <rex/runtime.h>

REXCVAR_DEFINE_BOOL(stage_transitions, true, "Patches",
                    "Stage transitions: the short cinematic that sends the fight to the stage's aerial version after "
                    "a big hit. Off = the fight stays on its stage (the hit is a normal hit). Online, the host's "
                    "setting is used (in Player Match rooms: the room rule).")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(stage_transition_fix, true, "Patches",
                    "Stage transitions always have a stage to go to (the stage's own aerial / ground version when the "
                    "save has none unlocked, and always online, so both PCs agree), and a cinematic never switches to "
                    "an empty scene (the stage vanished). Off = the game's own pairing (v0.8.1 and older)");

REXCVAR_DEFINE_STRING(rb2_force_transition, "", "Debug",
                      "Test (issue #33, the online check): N or N:P - at input tick N (then every P ticks) the stage "
                      "transition starts as if player 2's fighter had been blown into a transition wall (the game's own "
                      "post of sub_82187610), when the game itself could start one there (no cinematic, no event, stage "
                      "transitions on). Online both PCs need the same value. Empty = off");

// burstlimit_online_rules.cpp: the agreed Player Match room rules' stage transitions (1 on, 0 off); false = none.
bool BurstLimitOnlineRulesStageTransitions(int* on);
void BurstLimitRb2NoteHostStore(uint32_t a, uint32_t n);  // burstlimit_rb2.cpp
int64_t BurstLimitRb2CurrentFrame();
bool BurstLimitRb2InResim();

namespace {

constexpr uint32_t kFxManager = 0x841584E0u;
constexpr uint32_t kRequestPointer = 0x841B5138u;
constexpr uint32_t kRequestMode = 196;
constexpr uint32_t kStoryChain = 0x8200AEF8u;  // 27 x {stage, next}

rex::memory::Memory* Mem() {
  auto* runtime = rex::Runtime::instance();
  return runtime ? runtime->memory() : nullptr;
}
uint32_t R32(rex::memory::Memory* m, uint32_t a) {
  return rex::memory::load_and_swap<uint32_t>(m->TranslateVirtual<const uint8_t*>(a));
}
bool Odd(int32_t stage) { return stage >= 0 && (stage & 1) != 0; }

// The walk of sub_8214B2F8 for modes 1 / 2 / 6 as if every stage were unlocked: the first stage after `stage` on the
// story chain whose kind (ground / air) differs. Namek 24 -> 25, Plains 8 -> 9, ...
int32_t ChainPair(rex::memory::Memory* m, int32_t stage) {
  int32_t s = stage;
  for (int i = 0; i < 27; ++i) {
    int32_t next = -1;
    for (uint32_t k = 0; k < 27; ++k) {
      if (int32_t(R32(m, kStoryChain + 8 * k)) == s) {
        next = int32_t(R32(m, kStoryChain + 8 * k + 4));
        break;
      }
    }
    if (next < 0) return -1;
    s = next;
    if (Odd(s) != Odd(stage)) return s;
  }
  return -1;
}

std::atomic<int> g_pair_lines{0};
std::atomic<int> g_keep_lines{0};
std::atomic<int> g_gate_lines{0};

}  // namespace

// Stage transitions on in this fight? Offline: stage_transitions. Online: the Player Match room rule when the
// rooms' rules were agreed, else stage_transitions - a host-synced setting in the lobby (the guest has the
// host's value), so both PCs answer the same; Radmin / LAN without the lobby: on (the game's default).
bool BurstLimitStageTransitionsOn() {
  if (rex::net::IsGameSessionOpen()) {
    int on = 1;
    if (BurstLimitOnlineRulesStageTransitions(&on)) return on != 0;
    // Radmin / LAN (no lobby): nothing makes the two PCs agree - the game's default (as drama_pieces)
    if (!rex::net::online::IsLobbyMode()) return true;
  }
  return REXCVAR_GET(stage_transitions);
}

// From the effect manager's scene switch (burstlimit_rb2.cpp, sub_8214BDF0 called by the script command at
// 0x8214E4E4): the scene to switch to. A scene with no stage loaded keeps the current one (rb2_fx_scene_fix, and
// always with stage transitions off).
int32_t BurstLimitStageSceneSwitch(int32_t target, bool safety_net) {
  rex::memory::Memory* m = Mem();
  if (!m || !(safety_net || !BurstLimitStageTransitionsOn()) || target < 0 || target > 1) return target;
  const int32_t cur = int32_t(R32(m, kFxManager + 96));
  const int32_t stage = int32_t(R32(m, kFxManager + 100 + 4 * uint32_t(target)));
  if (target != cur && stage < 0 && cur >= 0 && cur <= 1) {
    const int n = ++g_keep_lines;
    if (n <= 20 || (n & (n - 1)) == 0)
      REXLOG_WARN("[Stage] a cinematic switches to scene {} with no stage loaded - scene {} (stage {}) kept ({} so far)", target, cur,
                  int32_t(R32(m, kFxManager + 100 + 4 * uint32_t(cur))), n);
    return cur;
  }
  return target;
}

// The paired stage (script command 0x14B -> sub_8214B2F8).
REX_EXTERN(__imp__sub_8214B2F8);
extern "C" REX_FUNC(sub_8214B2F8) {
  const int32_t stage = ctx.r3.s32;
  __imp__sub_8214B2F8(ctx, base);
  rex::memory::Memory* m = Mem();
  if (!m || stage < 0 || stage > 26) return;
  const uint32_t request = R32(m, kRequestPointer);
  const int32_t got = ctx.r3.s32;
  if (!BurstLimitStageTransitionsOn()) {  // off: no stage to go to - the load is skipped, the switch keeps the stage
    const int n = ++g_gate_lines;
    if (n <= 20 || (n & (n - 1)) == 0)
      REXLOG_WARN("[Stage] stage transitions off: a cinematic asks for the stage paired with {} - none (the fight stays on its stage; {} so far)",
                  stage, n);
    ctx.r3.s64 = -1;
    return;
  }
  if (!request || !REXCVAR_GET(stage_transition_fix)) return;
  const uint32_t mode = R32(m, request + kRequestMode);
  if (mode != 1 && mode != 2 && mode != 6) return;  // the other modes use a fixed table (no save involved)
  const bool online = rex::net::IsGameSessionOpen();
  if (!online && got >= 0) return;  // offline with an unlocked stage to go to: the game's own choice
  const int32_t pair = ChainPair(m, stage);
  if (pair != got) {
    const int n = ++g_pair_lines;
    if (n <= 20)
      REXLOG_WARN("[Stage] transition from stage {}: the {} gives {} - stage {} is used ({})", stage,
                  online ? "save's unlocks" : "save (no aerial stage unlocked)", got, pair, online ? "online: the same on both PCs" : "offline");
    ctx.r3.s64 = pair;
  }
}

// The gate: sub_82173A98 (the running event id, -1 none) as asked by the two transition sites.
REX_EXTERN(__imp__sub_82173A98);
extern "C" REX_FUNC(sub_82173A98) {
  const uint32_t lr = uint32_t(ctx.lr);
  __imp__sub_82173A98(ctx, base);
  if ((lr == 0x8218779Cu || lr == 0x82197768u) && ctx.r3.s32 == -1 && !BurstLimitStageTransitionsOn()) {
    const int n = ++g_gate_lines;
    if (n <= 20 || (n & (n - 1)) == 0)
      REXLOG_WARN("[Stage] stage transitions off: a {} transition is not started - {} ({} so far)", lr == 0x8218779Cu ? "ground -> air" : "air -> ground",
                  lr == 0x8218779Cu ? "the game's wall hit instead" : "the fight stays", n);
    ctx.r3.s64 = 0;
  }
}

// ---- test switch rb2_force_transition (v0.8.2): a transition on demand ----------------------------------------
// No harness input reliably blows a fighter into a transition wall (round 10: about one loopback run in four had a
// transition, no two-instance run had one). This makes the game's own post at a chosen input tick: from the entry of
// the wall check sub_82187610 for player 2's fighter (the main thread, every battle tick, resimulated frames too),
// the steps of 0x821877B0..0x82187810 - clear bit 4 of +3132 of both fighters, battle message 0 {the current scene's
// stage script, the opponent's player number, 0} (sub_82176710), the fighter to state 4 (sub_8218C9F8). The tick is
// guest state ([[0x841B5EA4] + 1588], the input manager's tick), so both PCs post at the same frame; a rewind over
// the tick posts again in the resimulated pass (one try per pass: tick + rewind count).
REX_EXTERN(__imp__sub_82187610);
REX_EXTERN(sub_821766A0);  // the battle
REX_EXTERN(sub_821897F8);  // a fighter's opponent
REX_EXTERN(sub_8214B758);  // the current scene's stage script
REX_EXTERN(sub_82123218);  // clear (buffer, size)
REX_EXTERN(sub_82176710);  // post a battle message (kind, &message)
REX_EXTERN(sub_8218C9F8);  // fighter state (state, fighter, 0)

namespace {
int64_t g_force_n = -2;  // -2 not read yet, -1 off
int64_t g_force_p = 0;
int64_t g_fired_tick = -1;
uint64_t g_fired_gen = ~0ull;
uint32_t g_force_msg = 0;
std::atomic<uint64_t> g_rewind_gen{0};
int g_force_lines = 0;

uint16_t R16(rex::memory::Memory* m, uint32_t a) { return rex::memory::load_and_swap<uint16_t>(m->TranslateVirtual<const uint8_t*>(a)); }
void W32(rex::memory::Memory* m, uint32_t a, uint32_t v) {
  rex::memory::store_and_swap<uint32_t>(m->TranslateVirtual<uint8_t*>(a), v);
  BurstLimitRb2NoteHostStore(a, 4);
}

void ForceLine(const std::string& text) {
  if (++g_force_lines <= 40) REXLOG_WARN("[Stage] TEST rb2_force_transition: {}", text);
}

void ForceTransition(rex::memory::Memory* m, uint32_t fighter_arg) {
  const uint32_t manager = R32(m, 0x841B5EA4u);
  if (!manager) return;
  const int64_t tick = int32_t(R32(m, manager + 1588));
  if (tick < g_force_n || (g_force_p > 0 ? (tick - g_force_n) % g_force_p : tick - g_force_n) != 0) return;
  const uint32_t player = R32(m, 0x84239C10u + 4);
  const uint32_t fighter = player ? R32(m, player + 44) : 0;
  if (!fighter || fighter_arg != fighter) return;
  const uint64_t gen = g_rewind_gen.load(std::memory_order_relaxed);
  if (tick == g_fired_tick && gen == g_fired_gen) return;  // tried in this pass already
  g_fired_tick = tick;
  g_fired_gen = gen;
  const std::string at = fmt::format("input tick {} (engine frame {}{})", tick, BurstLimitRb2CurrentFrame(), BurstLimitRb2InResim() ? ", resim" : "");
  if (!BurstLimitStageTransitionsOn()) return ForceLine(at + ": stage transitions are off - nothing posted");
  const uint32_t battle = rex::ppc::GuestToHostFunction<uint32_t>(sub_821766A0);
  if (!battle) return ForceLine(at + ": no battle");
  if (R16(m, battle + 574)) return ForceLine(at + ": a cinematic is running - nothing posted");
  const uint32_t global = R32(m, 0x825205E8u);
  if (global && (R32(m, global + 3296) & 1)) return ForceLine(at + ": the global no-transition flag is set - nothing posted");
  if (rex::ppc::GuestToHostFunction<int32_t>(sub_82173A98) != -1) return ForceLine(at + ": an event is running - nothing posted");
  const uint32_t opponent = rex::ppc::GuestToHostFunction<uint32_t>(sub_821897F8, fighter);
  if (!opponent) return ForceLine(at + ": no opponent");
  if (!g_force_msg) g_force_msg = m->SystemHeapAlloc(32);
  if (!g_force_msg) return ForceLine(at + ": no message buffer");
  W32(m, fighter + 3132, R32(m, fighter + 3132) & ~4u);
  W32(m, opponent + 3132, R32(m, opponent + 3132) & ~4u);
  rex::ppc::GuestToHostFunction<void>(sub_82123218, g_force_msg, uint32_t(24));
  const uint32_t script = rex::ppc::GuestToHostFunction<uint32_t>(sub_8214B758);
  W32(m, g_force_msg + 0, script);
  W32(m, g_force_msg + 4, uint32_t(int32_t(int16_t(R16(m, opponent)))));
  W32(m, g_force_msg + 8, 0);
  rex::ppc::GuestToHostFunction<void>(sub_82176710, uint32_t(0), g_force_msg);
  rex::ppc::GuestToHostFunction<void>(sub_8218C9F8, uint32_t(4), fighter, uint32_t(0));
  ForceLine(fmt::format("{}: transition posted (fighter {:08X}, opponent player {}, stage script {:08X}, scene {} stage {})", at, fighter,
                        int16_t(R16(m, opponent)), script, int32_t(R32(m, kFxManager + 96)),
                        int32_t(R32(m, kFxManager + 100 + 4 * (R32(m, kFxManager + 96) & 1)))));
}
}  // namespace

// From the rollback engine's rewind (burstlimit_rb2.cpp): a new pass over the frames after the restore point.
void BurstLimitStageRewind() { g_rewind_gen.fetch_add(1, std::memory_order_relaxed); }

extern "C" REX_FUNC(sub_82187610) {
  if (g_force_n == -2) {
    long long n = -1, p = 0;
    const std::string v = REXCVAR_GET(rb2_force_transition);
    if (std::sscanf(v.c_str(), "%lld:%lld", &n, &p) < 1 || n < 0) n = -1;
    g_force_n = n;
    g_force_p = p > 0 ? p : 0;
    if (n >= 0) REXLOG_WARN("[Stage] TEST rb2_force_transition armed: input tick {}{}", n, p > 0 ? fmt::format(", then every {}", p) : "");
  }
  if (g_force_n >= 0) {
    if (rex::memory::Memory* m = Mem()) ForceTransition(m, ctx.r3.u32);
  }
  __imp__sub_82187610(ctx, base);
}
