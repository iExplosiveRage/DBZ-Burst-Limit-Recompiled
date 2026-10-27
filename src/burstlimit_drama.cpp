// Drama sequences on / off (GitHub issue #21): with drama_pieces off, no
// Drama Piece plays during a fight - the short cut-scenes that interrupt a
// match (a partner stepping in, a story moment in a Z Chronicles episode).
//
// A fight's Drama Pieces are in the match request ([0x841B5138]; 0x50 bytes
// per player): six u32 at +20, -1 or 0 = none. Slots 0-3 (+20..+32) are the ones
// that can trigger during the fight (the character select's three Drama
// Piece picks in Versus / Training, the episode's own in Z Chronicles; ids
// like 205102); slots 4 and 5 (+36 / +40, P2 +120) are the opening and
// ending scenes (Versus: 1 / 2, the rival scenes; a story fight: ids like
// 501001 / 501201) - the game's own "cut-scenes" rule (+214) already clears
// those (sub_822120A0 at 0x82212608 writes -1 to P1 +36 / +40 and P2 +120),
// so they're left to it. The battle set-up (sub_8217AEA8) reads the request
// after BurstLimitDramaBattleSetup has run, so with the option off the slots
// are emptied there, the way the game empties the opening / ending ones.
//
// Online: a Drama Piece changes the fight's timing, so both PCs must agree.
// In the lobby drama_pieces is a host-synced cvar (burstlimit_online.cpp:
// the guest takes the host's value for the session, both are locked). The
// Radmin / LAN way has no such sync: there the game's default (on) is used.
// The title's attract demo (request mode 6) is a scripted showcase of Drama
// Pieces and keeps them.

#include <cstdint>
#include <string>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/memory.h>
#include <rex/memory/utils.h>
#include <rex/net/online.h>
#include <rex/net/session.h>
#include <rex/runtime.h>

REXCVAR_DEFINE_BOOL(drama_pieces, true, "Patches",
                    "Drama Pieces: the short cut-scenes that interrupt a fight (a partner "
                    "stepping in, story moments in Z Chronicles). Off = none in any mode (the "
                    "opening / ending scenes stay, the game's cut-scenes rule decides those). "
                    "From the next fight. Online (lobby), the host's setting is used.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace {

constexpr uint32_t kRequestPointer = 0x841B5138;
constexpr uint32_t kRequestSize = 216;
constexpr uint32_t kRequestStride = 0x50;
constexpr uint32_t kRequestDramas = 20;
constexpr uint32_t kRequestDramaSlots = 6;
constexpr uint32_t kFightDramaSlots = 4;  // slots 0-3 trigger during the fight
constexpr uint32_t kRequestMode = 196;
constexpr uint32_t kModeAttractDemo = 6;
constexpr uint32_t kNone = 0xFFFFFFFFu;

rex::memory::Memory* GuestMemory() {
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

uint32_t R32(rex::memory::Memory* memory, uint32_t address) {
  return rex::memory::load_and_swap<uint32_t>(memory->TranslateVirtual<uint8_t*>(address));
}

void W32(rex::memory::Memory* memory, uint32_t address, uint32_t value) {
  rex::memory::store_and_swap<uint32_t>(memory->TranslateVirtual<uint8_t*>(address), value);
}

std::string Slots(rex::memory::Memory* memory, uint32_t entry) {
  std::string out;
  for (uint32_t d = 0; d < kRequestDramaSlots; ++d) {
    const int32_t v = int32_t(R32(memory, entry + kRequestDramas + d * 4));
    out += fmt::format("{}{}", d ? (d == kFightDramaSlots ? " | " : " ") : "", v);
  }
  return out;
}

}  // namespace

// Mid-asm hook at 0x8217AEB8 in the battle set-up sub_8217AEA8, before it
// reads the match request (right after the online room rules' hook at
// 0x8217AEB4): with drama_pieces off, the fight's Drama Piece slots are
// emptied.
void BurstLimitDramaBattleSetup() {
  rex::memory::Memory* memory = GuestMemory();
  if (!memory) {
    return;
  }
  const uint32_t request = R32(memory, kRequestPointer);
  if (!request || !Readable(memory, request, kRequestSize)) {
    return;
  }
  const uint32_t mode = R32(memory, request + kRequestMode);
  REXLOG_INFO("Drama: battle set-up, mode {}: P1 [{}] P2 [{}]", mode, Slots(memory, request),
              Slots(memory, request + kRequestStride));
  bool enabled = REXCVAR_GET(drama_pieces);
  const char* why = "the setting";
  if (rex::net::IsGameSessionOpen()) {
    if (rex::net::online::IsLobbyMode()) {
      why = "the host's setting (online)";
    } else {
      // Radmin / LAN: nothing makes the two PCs agree - the game's default.
      enabled = true;
      why = "online without the lobby: the game's default";
    }
  }
  if (enabled || mode == kModeAttractDemo) {
    return;
  }
  int cleared = 0;
  for (uint32_t p = 0; p < 2; ++p) {
    for (uint32_t d = 0; d < kFightDramaSlots; ++d) {
      const uint32_t at = request + p * kRequestStride + kRequestDramas + d * 4;
      const uint32_t drama = R32(memory, at);
      if (drama != kNone && drama != 0) {  // (0 is "none" too)
        W32(memory, at, kNone);
        ++cleared;
      }
    }
  }
  REXLOG_INFO("Drama: Drama Pieces off ({}): {} slot(s) emptied", why, cleared);
}
