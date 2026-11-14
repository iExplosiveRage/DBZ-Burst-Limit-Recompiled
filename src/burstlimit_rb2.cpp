// Rollback phase-1 measurements (research build only, branch rollback2).
//
// --rb2_auto=<ticks>: <ticks> after FIGHT! (battle phase 2) the script below
// runs on the game thread at the main-loop tick hook (0x822198F8, from
// BurstLimitCameraFrame) and logs "[RB2]" lines:
//   1. regions: committed writable guest memory per class;
//   2. measure: 4 KB pages changed per tick per class over rb2_measure ticks
//      (memcmp against a shadow), the scan cost and the busiest 64 KB blocks;
//   3. tests (rb2_tests "depth:steps,..."): save the state S at tick t0 (full
//      copy of data / low / heap / phys), let the game play `depth` ticks
//      (fingerprint per tick), then in ONE host frame: pages changed since S
//      (= the undo log of `depth` ticks), restore S, re-simulate `depth`
//      ticks without drawing, compare the fingerprint at every tick, count
//      the pages that differ from the real state, then put the real state
//      back (so the game goes on as if nothing happened).
//   4. "[RB2] done" (the harness stops the game).
//
// Re-simulated tick = the main loop from the hook to the next hook without
// the draw tasks, the render hand-off sub_82218A10 and the vsync waits. Steps
// (rb2 steps mask): 1 sub_8210D2A8 (render list frame start), 2 sub_8212A600
// (resource release queue), 4 sub_82132550, 8 sub_82119320 (CRI server),
// 16 sub_82120CF8 (a), 32 sub_8211D890, 64 sub_82117A70, 128 sub_82120CF8
// (b), 256 sub_8210D338, 512 sub_82218950, 1024 sub_824A7DD0 (pads),
// 2048 sub_821249C0, 4096 sub_8210F600 / sub_8210D178, 8192 the whole
// scheduler sub_82123178 (update AND draw tasks) instead of the update tasks
// only, 16384 (split mode) the effects' draw band too.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <string>
#include <deque>
#include <map>
#include <set>
#include <mutex>
#include <thread>
#include <vector>
#include <optional>
#include <unordered_map>
#include <array>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/input/input_system.h>
#include <rex/logging.h>
#include <rex/memory.h>
#include <rex/memory/utils.h>
#include <rex/ppc.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>
#include <rex/perf/frame_rate.h>
#include <rex/runtime.h>
#include <rex/system/guest_locks.h>
#include <rex/system/thread_state.h>
#include <rex/thread/mutex.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <tuple>

// Store marks (scripts/patch_generated.py): per guest 4 KB page, bit 1 main thread, bit 2 other threads.
extern "C" unsigned char rb2_dirty[0x100000];
// Physical memory has three guest views (0xA0000000 64 KB pages, 0xC0000000 16 MB pages, 0xE0000000 4 KB pages at
// physical + 0x1000). The engine tracks a page in the view it was allocated in; the game also writes the same memory
// through the other views (MEASURED al_diag, Frieza vs Cell: ~29 page writes per frame through an untracked view of a
// tracked page) - those writes were invisible: never undone, and wiped out when the tracked view's page was restored.
// Every write is noted on the TRACKED view of its physical page (rb2_phys_canon, built when tracking starts).
uint32_t* rb2_phys_canon = nullptr;  // per physical page: the tracked guest page number, 0 = none
REXCVAR_DEFINE_BOOL(rb2_phys_canon_on, true, "Debug", "Rollback: writes through any view of physical memory are noted on its tracked view");
static inline uint32_t CanonAddr(uint32_t x) {
  if (x < 0xA0000000u || !rb2_phys_canon) return x;
  const uint32_t page = x >> 12;
  const uint32_t ph = page < 0xC0000 ? page - 0xA0000 : page < 0xE0000 ? page - 0xC0000 : page - 0xE0000 + 1;
  if (ph >= 0x20000) return x;
  const uint32_t c = rb2_phys_canon[ph];
  return (c && c != page) ? ((c << 12) | (x & 0xFFFu)) : x;
}
unsigned char rb2_dirty[0x100000] = {};
// Write tracking switch (patch v9): 0 until a rollback engine starts in this process -> with rollback off the
// generated stores only test this byte (v0.7.3 speed). Once on it stays on (the page images depend on it).
extern "C" unsigned char rb2_track_on;
unsigned char rb2_track_on = 0;
extern "C" thread_local unsigned char rb2_tbit;
thread_local unsigned char rb2_tbit = 2;
extern "C" unsigned char rb2_sfine[0x2000000];
unsigned char rb2_sfine[0x2000000] = {};
// Per 16-byte block: written by another thread (or by host code on its behalf: file reads) since the
// page was last logged (rollback3). A frame's logged page keeps these blocks live when it is rewound:
// a loader / sound / render thread's bytes on a page the frame also wrote are not that frame's to undo.
extern "C" unsigned char rb2_ffine[0x2000000];
unsigned char rb2_ffine[0x2000000] = {};
// The marks of the log BEFORE: a guest store is marked before it lands (mark, then store), so a foreign thread's store
// can land after the log snapshotted its page and cleared its mark - its value was missing from the snapshot AND its
// mark was gone, and the next rewind put the old bytes back (w103: the render thread's atomic AddRef on a resource
// object lost -> refcount one short -> freed while still in the cache map -> NULL vtable call, lr 820EBB54). Each
// log's marks also count for the next log of the page and for restores, so an in-flight store is always covered.
unsigned char rb2_ffine_prev[0x2000000] = {};
// Main-thread writes per 16-byte block since the page's last log (rb2_lost_marks): a block that changed since the
// snapshot with neither a main nor a foreign mark was changed by a foreign store whose mark the log cleared before
// the store landed (mark, then store) - it is foreign: kept live by restores, part of the next log's foreign mask.
// (The one-more-generation marks above kept too much live - crash C in 3 of 4 runs - and are off.)
extern "C" unsigned char rb2_mfine[0x2000000];
unsigned char rb2_mfine[0x2000000] = {};
REXCVAR_DEFINE_BOOL(rb2_lost_marks, true, "Debug", "Rollback: a block changed since its page's snapshot without any mark is a foreign store that landed after the log (kept live)");
// rb2_lost_gate (build 114: ON): a block counts as a lost foreign store only where the page's previous foreign marks
// had it - exactly the mark-then-store race (a foreign store marked, then the main thread's log took the page and
// cleared the mark, then the store landed). Ungated (builds 105-113) every unmarked change was kept live - host-thread
// writes too (GPU timer callbacks, kernel objects, the heap / sound player blocks seen online at 82725310 / 82F8E070):
// 2 of 3 long online sessions crashed (effects list torn in sub_82115920, a released handle on the render thread).
// Without the rule at all the loader crash came back (r113_lD: refcount one short, call to 0x00000001 at 820EBB54).
// Gated: offline 4 kept blocks per 200-s Frieza vs Cell stress, the rest restored like build 96. (The 107 test of the
// gate - 0 of 3 - was effects crash C, which turned out to be unrelated to rewinds; guarded since build 110.) Our own
// host writes of game state are noted as main-thread stores (BurstLimitRb2NoteHostStore), so they go back anyway.
REXCVAR_DEFINE_BOOL(rb2_lost_gate, true, "Debug", "Rollback: a block counts as a lost foreign store only if the page's previous foreign marks had it");
REXCVAR_DEFINE_INT32(rb2_lost_log, 0, "Debug", "Rollback diagnostics: log the first N blocks taken as lost foreign stores (address, frame, thread marks of the page)");
std::atomic<int> g_lost_logged{0};
int64_t g_lost_mark_blocks = 0;
REXCVAR_DEFINE_BOOL(rb2_ffine_prev_on, false, "Debug", "Rollback: foreign marks also count one log later (covers a store that lands after its page's log)");
int64_t BurstLimitRb2LoopTakeRewind();
std::string BurstLimitRb2LoopStats();
bool BurstLimitRollback2Online();  // burstlimit_netinput.cpp: a lobby match whose netcode is rollback
bool Rb2EngineOn();
// Store watch (diagnostic, rb2_watch): every store into [lo, lo + size) is reported with its function.
extern "C" unsigned int rb2_watch_lo, rb2_watch_size;
unsigned int rb2_watch_lo = 0, rb2_watch_size = 0;
extern "C" void rb2_watch_hit(unsigned int a, unsigned long long v, int n, const char* fn);

REX_EXTERN(sub_824A7DD0);
REX_EXTERN(sub_821249C0);
REX_EXTERN(sub_8210F600);
REX_EXTERN(sub_8210D178);
REX_EXTERN(sub_8210D2A8);
REX_EXTERN(sub_8212A600);
REX_EXTERN(sub_82132550);
REX_EXTERN(sub_82119320);
REX_EXTERN(sub_82120CF8);
REX_EXTERN(sub_8211D890);
REX_EXTERN(sub_82123178);
REX_EXTERN(sub_82117A70);
REX_EXTERN(sub_8210D338);
REX_EXTERN(sub_82218950);
REX_EXTERN(sub_82122450);
REX_EXTERN(sub_821228A8);
REX_EXTERN(sub_822A2CF8);
REX_EXTERN(sub_8210A0D0);  // sound manager: stop channel (index, immediate, category)
REX_EXTERN(sub_8210A308);  // sound manager: channel playing? (index, category)
REX_EXTERN(sub_8210A200);  // sound manager: channel pause flag (index, flag, category)

REXCVAR_DEFINE_INT32(rb2_auto, 0, "Debug", "Rollback research: ticks after FIGHT! before the measurement script (0 off)");
REXCVAR_DEFINE_INT32(rb2_measure, 300, "Debug", "Rollback research: ticks of the dirty page measurement");
REXCVAR_DEFINE_STRING(rb2_dmask, "0", "Debug", "Rollback research: draw tasks a resim runs (bit = index among the draw tasks)");
REXCVAR_DEFINE_BOOL(rb2_split, true, "Debug", "Rollback research: split the real tick's scheduler (updates, then draws with their own RNG)");
REXCVAR_DEFINE_STRING(rb2_tests, "", "Debug", "Rollback research: tests depth:steps,... (empty = built-in list)");

namespace {

using Clock = std::chrono::steady_clock;
double Ms(Clock::time_point a, Clock::time_point b) { return std::chrono::duration<double, std::milli>(b - a).count(); }

enum Cls : int { kData, kLow, kHeap, kPhys, kClsCount };
const char* const kClsNames[kClsCount] = {"data", "low", "heap", "phys"};
constexpr uint32_t kPage = 0x1000;
constexpr uint32_t kDataBegin = 0x82520000, kDataEnd = 0x843B7000;

int ClassOf(uint32_t a) {
  if (a >= kDataBegin && a < kDataEnd) return kData;
  if (a < 0x30000000) return kLow;
  if (a >= 0x40000000 && a < 0x70000000) return kHeap;
  if (a >= 0xA0000000) return kPhys;
  return -1;  // stacks, image, other
}

struct Range {
  uint32_t address, size;
  int cls;
  size_t offset;  // in a snapshot buffer
};

uint32_t U32(rex::memory::Memory* m, uint32_t a) { return rex::memory::load_and_swap<uint32_t>(m->TranslateVirtual<const uint8_t*>(a)); }
uint16_t U16(rex::memory::Memory* m, uint32_t a) { return rex::memory::load_and_swap<uint16_t>(m->TranslateVirtual<const uint8_t*>(a)); }
void S32(rex::memory::Memory* m, uint32_t a, uint32_t v) { rex::memory::store_and_swap<uint32_t>(m->TranslateVirtual<uint8_t*>(a), v); }
void S16(rex::memory::Memory* m, uint32_t a, uint16_t v) { rex::memory::store_and_swap<uint16_t>(m->TranslateVirtual<uint8_t*>(a), v); }

bool Committed(rex::memory::Memory* m, uint32_t a, uint32_t size) {
  if (a < 0x10000) return false;
  auto* heap = m->LookupHeap(a);
  rex::memory::HeapAllocationInfo info = {};
  return heap && heap->QueryRegionInfo(a, &info) && (info.state & rex::memory::kMemoryAllocationCommit) &&
         uint64_t(a) + size <= uint64_t(info.base_address) + info.region_size;
}

// Committed writable guest memory, split at the class borders (stacks and the
// image past .data left out; physical memory once, through its own heaps).
std::vector<Range> WritableRanges(rex::memory::Memory* m) {
  std::vector<Range> out;
  uint64_t a = 0x00010000;
  size_t offset = 0;
  while (a < 0x100000000ull) {
    if (a >= 0x30000000 && a < 0x40000000) { a = 0x40000000; continue; }
    if (a >= 0x70000000 && a < kDataBegin) { a = kDataBegin; continue; }
    if (a >= kDataEnd && a < 0xA0000000) { a = 0xA0000000; continue; }
    auto* heap = m->LookupHeap(uint32_t(a));
    if (!heap) { a += 0x10000; continue; }
    rex::memory::HeapAllocationInfo info = {};
    if (!heap->QueryRegionInfo(uint32_t(a), &info) || !info.region_size) { a += heap->page_size(); continue; }
    uint64_t end = std::min<uint64_t>(uint64_t(info.base_address) + info.region_size, 0x100000000ull);
    if ((info.state & rex::memory::kMemoryAllocationCommit) && (info.protect & rex::memory::kMemoryProtectWrite)) {
      uint64_t s = a;
      while (s < end) {
        uint64_t e = end;
        for (uint64_t border : {0x30000000ull, 0x40000000ull, 0x70000000ull, uint64_t(kDataBegin), uint64_t(kDataEnd), 0xA0000000ull}) {
          if (border > s && border < e) e = border;
        }
        const int cls = ClassOf(uint32_t(s));
        if (cls >= 0) {
          out.push_back({uint32_t(s), uint32_t(e - s), cls, offset});
          offset += size_t(e - s);
        }
        s = e;
      }
    }
    a = std::max(end, a + 4);
  }
  return out;
}

size_t TotalSize(const std::vector<Range>& r) { return r.empty() ? 0 : r.back().offset + r.back().size; }

uint64_t Fnv(uint64_t h, const uint8_t* p, size_t n) {
  for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 0x100000001B3ull;
  return h;
}

// Fight state fingerprint: battle object, global RNG, both fighters.
// Blob layout: [0, 0x400) battle, [0x400, 0x404) RNG, then fighter 0 / 1 (0x1400 each).
constexpr uint32_t kBlobBattle = 0, kBlobRng = 0x400, kBlobF0 = 0x404, kBlobF1 = 0x404 + 0x1400, kBlobP0 = 0x404 + 0x2800,
                   kBlobP1 = kBlobP0 + 0x800, kBlobSize = kBlobP1 + 0x800;  // rollback3: + the player objects (HP / gauges?)
std::vector<uint8_t> FpBlob(rex::memory::Memory* m) {
  std::vector<uint8_t> b(kBlobSize, 0);
  auto add = [&](uint32_t off, uint32_t a, uint32_t size) {
    if (Committed(m, a, size)) std::memcpy(b.data() + off, m->TranslateVirtual<const uint8_t*>(a), size);
  };
  add(kBlobBattle, 0x841B5140, 0x400);
  add(kBlobRng, 0x842F9334, 4);
  for (uint32_t side = 0; side < 2; ++side) {
    const uint32_t entry = U32(m, 0x84239C10 + 4 * side);
    if (!Committed(m, entry + 44, 4)) continue;
    add(side ? kBlobF1 : kBlobF0, U32(m, entry + 44), 0x1400);
    add(side ? kBlobP1 : kBlobP0, entry, 0x800);
  }
  return b;
}
uint64_t Fingerprint(rex::memory::Memory* m) {
  const auto b = FpBlob(m);
  return Fnv(0xCBF29CE484222325ull, b.data(), b.size());
}
std::string BlobDiff(const std::vector<uint8_t>& want, const std::vector<uint8_t>& got) {
  std::string s;
  int runs = 0;
  size_t total = 0;
  for (uint32_t i = 0; i < kBlobSize;) {
    if (want[i] == got[i]) { ++i; continue; }
    uint32_t j = i;
    while (j < kBlobSize && (want[j] != got[j] || (j + 1 < kBlobSize && want[j + 1] != got[j + 1]))) ++j;
    total += j - i;
    if (runs < 24) {
      const char* sec = i < kBlobRng ? "battle" : i < kBlobF0 ? "rng" : i < kBlobF1 ? "f0" : i < kBlobP0 ? "f1" : i < kBlobP1 ? "p0" : "p1";
      const uint32_t base = i < kBlobRng ? kBlobBattle : i < kBlobF0 ? kBlobRng : i < kBlobF1 ? kBlobF0 : i < kBlobP0 ? kBlobF1 : i < kBlobP1 ? kBlobP0 : kBlobP1;
      const uint32_t w = (i - base) & ~3u;
      const uint32_t wb = base + w;
      auto word = [&](const std::vector<uint8_t>& v) {
        return wb + 4 <= kBlobSize ? (uint32_t(v[wb]) << 24 | uint32_t(v[wb + 1]) << 16 | uint32_t(v[wb + 2]) << 8 | v[wb + 3]) : 0u;
      };
      s += fmt::format(" {}+{:X}({}B {:08X}->{:08X})", sec, i - base, j - i, word(want), word(got));
    }
    ++runs;
    i = j;
  }
  return fmt::format("{} bytes in {} runs:{}", total, runs, s);
}

// ---- re-simulated tick --------------------------------------------------------------------------
constexpr uint32_t kDefaultSteps = 4 | 8 | 16 | 64 | 128 | 256 | 512 | 1024 | 2048 | 4096 | 16384;

std::vector<uint32_t> g_resim_skip;
std::vector<uint32_t> g_resim_draw;  // draw task callbacks a resim runs too (rb2_resim_draw)  // update task callbacks a re-simulated frame leaves out (rb2_skip)
bool g_in_resim = false;
int g_trace = 0;  // the first rewinds log every step (hang hunting)
bool FxBandTask(uint32_t prio, uint32_t cb) {
  return ((prio >> 24) == 0x2C && cb != 0x82141C80) || cb == 0x82145E98 || cb == 0x821F2BB0 || cb == 0x82161BC8 ||
         std::find(g_resim_draw.begin(), g_resim_draw.end(), cb) != g_resim_draw.end();
}
void LinkAndRun(rex::memory::Memory* m, PPCContext& ctx, uint8_t* base, uint32_t list, bool band) {
  uint32_t prev = 0;
  int walked = 0;
  for (uint32_t task = U32(m, list + 32); task; task = U32(m, task + 40)) {
    if (++walked > 1024) break;
    const uint32_t prio = U32(m, task + 24), cb = U32(m, task + 32);
    const bool is_draw = prio >= 0x20000000 && cb != 0x8217E100;
    if (!band) {
      if (is_draw) continue;
      if (g_in_resim && std::find(g_resim_skip.begin(), g_resim_skip.end(), cb) != g_resim_skip.end()) continue;
    } else {
      if (!is_draw || prio == 0xFFFFFFFF) continue;
      const bool fx = FxBandTask(prio, cb);
      if (!fx) continue;
    }
    ctx.r3.u64 = list;
    ctx.r4.u64 = task;
    ctx.r5.u64 = prev;
    sub_82122450(ctx, base);
    prev = task;
  }
  ctx.r3.u64 = list;
  sub_821228A8(ctx, base);
}

// Every task in list order (update and draw) except the frame end (priority 0xFFFFFFFF), one pass.
void LinkAll(rex::memory::Memory* m, PPCContext& ctx, uint8_t* base, uint32_t list) {
  uint32_t prev = 0;
  int walked = 0;
  for (uint32_t task = U32(m, list + 32); task; task = U32(m, task + 40)) {
    if (++walked > 1024) break;
    if (U32(m, task + 24) == 0xFFFFFFFF) continue;
    ctx.r3.u64 = list;
    ctx.r4.u64 = task;
    ctx.r5.u64 = prev;
    sub_82122450(ctx, base);
    prev = task;
  }
  ctx.r3.u64 = list;
  sub_821228A8(ctx, base);
}

void DumpTasks(rex::memory::Memory* m) {
  const uint32_t list = U32(m, 0x84140E90);
  if (!list) return;
  int n = 0;
  for (uint32_t task = U32(m, list + 32); task && n < 200; task = U32(m, task + 40), ++n) {
    REXLOG_WARN("[RB2] task {:08X} prio {:08X} cb {:08X} arg {:08X} delay {}", task, U32(m, task + 24), U32(m, task + 32),
                U32(m, task + 68), int16_t(U16(m, task + 28)));
  }
}

// Draw tasks (by index among the draw tasks, frame end excluded) a resim runs too (rb2_dmask), with the
// cosmetic random number state swapped in like the real draw pass.
uint64_t g_dmask = 0;
constexpr uint32_t kRng = 0x842F9334;
bool g_split = false;       // the real tick's scheduler split (update pass / draw pass) is on
uint32_t g_cosmetic_rng = 0;  // the draw pass's random number state (host side, never rolled back)
void RngToCosmetic(rex::memory::Memory* m, uint32_t& fight) { fight = U32(m, kRng); S32(m, kRng, g_cosmetic_rng); }
void RngToFight(rex::memory::Memory* m, uint32_t fight) { g_cosmetic_rng = U32(m, kRng); S32(m, kRng, fight); }
void LinkDrawMask(rex::memory::Memory* m, PPCContext& ctx, uint8_t* base, uint32_t list, uint64_t mask) {
  uint32_t prev = 0;
  int walked = 0, index = -1;
  for (uint32_t task = U32(m, list + 32); task; task = U32(m, task + 40)) {
    if (++walked > 1024) break;
    const uint32_t prio = U32(m, task + 24), cb = U32(m, task + 32);
    const bool is_draw = prio >= 0x20000000 && cb != 0x8217E100;
    if (!is_draw || prio == 0xFFFFFFFF) continue;
    ++index;
    if (index >= 64 || !((mask >> index) & 1)) continue;
    ctx.r3.u64 = list;
    ctx.r4.u64 = task;
    ctx.r5.u64 = prev;
    sub_82122450(ctx, base);
    prev = task;
  }
  uint32_t fight = 0;
  if (g_split) RngToCosmetic(m, fight);
  ctx.r3.u64 = list;
  sub_821228A8(ctx, base);
  if (g_split) RngToFight(m, fight);
}

uint32_t SchedulerBegin(rex::memory::Memory* m) {
  const uint32_t list = U32(m, 0x84140E90);
  if (!list || int16_t(U16(m, list + 24)) <= 0) return 0;
  S32(m, list + 40, 0);
  S16(m, list + 50, 0);
  S32(m, list + 52, 0xFFFFFFFF);
  S32(m, list + 56, 0xFFFFFFFF);
  S32(m, list + 60, 4);
  return list;
}

// From the hook (0x822198F8) to the next tick's hook, no drawing.
void ResimTick(rex::memory::Memory* m, uint32_t steps) {
  PPCContext* ctx = rex::runtime::current_ppc_context();
  uint8_t* base = m->virtual_membase();
  const PPCContext saved = *ctx;
  const uint32_t system = U32(m, 0x82520000 + 1512);
  {
    const uint32_t counter = U32(m, system + 3228) + 1;
    S32(m, system + 3228, counter);
    if (!(int32_t(counter) < int32_t(U32(m, 0x826DE600)))) S32(m, system + 3228, 0);
  }
  if (steps & 1) sub_8210D2A8(*ctx, base);
  if (steps & 2) sub_8212A600(*ctx, base);
  if (steps & 4) sub_82132550(*ctx, base);
  if (steps & 8) sub_82119320(*ctx, base);
  if (steps & 16) sub_82120CF8(*ctx, base);
  if (steps & 32) sub_8211D890(*ctx, base);
  if (steps & 8192) {
    ctx->r3.u64 = 0;
    sub_82123178(*ctx, base);
  } else if (const uint32_t list = SchedulerBegin(m)) {
    LinkAndRun(m, *ctx, base, list, false);
    if (steps & 16384) LinkAndRun(m, *ctx, base, list, true);
    if (steps & 32768) LinkDrawMask(m, *ctx, base, list, g_dmask);
  }
  if (steps & 64) sub_82117A70(*ctx, base);
  if (steps & 128) sub_82120CF8(*ctx, base);
  if (steps & 256) sub_8210D338(*ctx, base);
  // (sub_82218A10, the frame end / render hand-off, and the loop top's waits: never)
  S32(m, 0x826DE5F8, U32(m, 0x826DE5FC));
  if (steps & 512) sub_82218950(*ctx, base);
  if (steps & 1024) {
    if (const uint32_t r3 = U32(m, 0x84140890)) {
      ctx->r3.u64 = r3;
      sub_824A7DD0(*ctx, base);
    }
  }
  ctx->r3.u64 = 0;
  if (steps & 2048) sub_821249C0(*ctx, base);
  if (steps & 4096) {
    if ((ctx->r3.u32 & 0xFF) && U32(m, system + 3228) == 0) sub_8210F600(*ctx, base);
    else sub_8210D178(*ctx, base);
  }
  *ctx = saved;
}

// The render thread done with its frame (the frame end's event), at most max_ms.
// The job worker (thread 820F0300, sub_820F2618 per job) and the draw worker (sub_820EA188) idle:
// their chain head (0x8438AC6C + 8) empty and the draw worker's pending flag (0x8438AC90 + 16) clear.
// The job pool (static object 0x8437F900, workers in sub_820F05B8): +8 workers awake, +16588 jobs queued.
constexpr uint32_t kJobPool = 0x8437F900;
constexpr uint32_t kPoolLo = 0x8437F000, kPoolHi = 0x84388000;  // the pool object, its node array, done list nodes (0x84386xxx, MEASURED f3)
// Ranges that are one piece of frame state whoever writes them (no live 16-byte blocks kept by a rewind):
// the job pool, and (online) the match input manager's rings - the game's network thread writes remote
// entries there too, and a 24-byte ring slot kept half live / half rewound is a torn input (on2 desync).
uint32_t g_input_mgr_lo = 0, g_input_mgr_hi = 0;
std::atomic<uint32_t> g_node_lo{0}, g_node_hi{0};  // job nodes handed out by the node allocator 0x8438D884
uint32_t g_netobj_lo = 0, g_netobj_hi = 0;  // the game's net object N (packs, acks; written by main + net thread)
REXCVAR_DEFINE_INT32(rb2_netobj, 0, "Debug", "Rollback (rollback3): the game's net object 0x84255714 -> N: 0 as other memory, 1 frame state, 2 never rewound");
bool FrameRange(uint32_t a) {
  return (a >= kPoolLo && a < kPoolHi) || (a >= g_input_mgr_lo && a < g_input_mgr_hi) ||
         (REXCVAR_GET(rb2_netobj) == 1 && a >= g_netobj_lo && a < g_netobj_hi);
}
bool NetObjExcluded(uint32_t a) { return REXCVAR_GET(rb2_netobj) == 2 && a >= g_netobj_lo && a < g_netobj_hi; }
bool g_workers_idle = true;  // the last WaitWorkers ended with everything idle (false: it timed out)
int64_t g_wait_timeouts = 0;
double WaitWorkers(rex::memory::Memory* m, int max_ms) {
  const auto t0 = Clock::now();
  g_workers_idle = true;
  while (U32(m, 0x8438AC6C + 8) != 0 || U32(m, 0x8438AC90 + 16) != 0 || U32(m, kJobPool + 8) != 0 ||
         U32(m, kJobPool + 16588) != 0) {
    if (Ms(t0, Clock::now()) > max_ms) { g_workers_idle = false; ++g_wait_timeouts; break; }
    std::this_thread::yield();
  }
  return Ms(t0, Clock::now());
}
// rollback3 quiesce: the job pool's lock (critical section at pool+24) held by the main thread while a
// frame's pages are logged or restored - no worker can pop / push / count in the middle of the copy
// (T6: the worker queue lock is taken and held around save / load).
REX_EXTERN(__imp__RtlEnterCriticalSection);
REX_EXTERN(__imp__RtlLeaveCriticalSection);
// rollback3: job object -> the pusher's write class (BurstLimitRb2WorkerJob)
std::mutex g_job_class_mutex;
std::unordered_map<uint32_t, uint8_t> g_job_class;  // job object -> the pusher's write class
std::map<std::pair<uint32_t, uint8_t>, int64_t> g_job_vt_stats;  // (vtable, class) -> pushes (diagnostics)
REXCVAR_DEFINE_BOOL(rb2_hold_pool, true, "Debug", "Rollback (rollback3): hold the job pool lock while frames are logged / restored");
// Diagnostics (rb2_pool_trace frames): the job pool's words at each step.
REXCVAR_DEFINE_INT32(rb2_pool_trace, 0, "Debug", "Rollback (rollback3): log the job pool's state for the first N engine frames");
rex::memory::Memory* g_mem_dump = nullptr;
void PoolTrace(rex::memory::Memory* m, const char* what, int64_t frame) {
  std::string w;
  for (uint32_t off : {4u, 8u, 152u, 160u, 16552u, 16556u, 16560u, 16564u, 16568u, 16572u, 16576u, 16580u, 16584u, 16588u, 16592u,
                       22748u, 22752u, 22756u, 22760u, 22764u, 22768u})
    w += fmt::format(" +{}={:X}", off, U32(m, kJobPool + off));
  // CS at +24: lock count / recursion / owner
  w += fmt::format(" cs={:08X}/{:08X}/{:08X}/{:08X}", U32(m, kJobPool + 24 + 16), U32(m, kJobPool + 24 + 20), U32(m, kJobPool + 24 + 24), U32(m, kJobPool + 24 + 12));
  REXLOG_WARN("[RB2] pool {} frame {}:{}", what, frame, w);
}
// MEASURED (g11, pool trace): another thread pushes jobs into this pool too (the lock's owner was not the
// main thread, count 1 at a frame's log right after the idle wait). So the hold takes the lock only when
// the pool is empty and no worker is awake (retrying up to 50 ms); otherwise the log is "not quiet".
bool g_pool_quiet = true;
int64_t g_pool_hold_retries = 0;
// The pool's header words plus its queue and done lists walked (node: +0 next, +4 prev, +8 job).
void PoolDump(const char* why) {
  rex::memory::Memory* m = g_mem_dump;
  if (!m) return;
  PoolTrace(m, why, -1);
  std::string q;
  for (uint32_t list : {kJobPool + 16576, kJobPool + 22748}) {  // circular lists with a sentinel node
    q += fmt::format(" list@{:08X}:", list);
    uint32_t n = U32(m, list);
    for (int k = 0; k < 12 && n != list && n >= 0x80000000u && n < 0x90000000u; ++k) {
      q += fmt::format(" {:08X}(job {:08X})", n, U32(m, n + 8));
      n = U32(m, n);
    }
  }
  REXLOG_WARN("[RB2] pool lists {}:{}", why, q);
  rex::FlushLogging();
}
struct PoolHold {
  bool held = false;
  explicit PoolHold(rex::memory::Memory* m) {
    if (!REXCVAR_GET(rb2_hold_pool) || !m) return;
    PPCContext* ctx = rex::runtime::current_ppc_context();
    if (!ctx) return;
    const PPCContext saved = *ctx;
    const auto t0 = Clock::now();
    for (;;) {
      ctx->r3.u64 = kJobPool + 24;
      __imp__RtlEnterCriticalSection(*ctx, m->virtual_membase());
      // empty queue, no worker awake, and no finished job waiting for its submitter (done list +22760: the
      // other pusher's job whose completion it has not collected yet - restoring that list hands it back)
      if (U32(m, kJobPool + 16588) == 0 && U32(m, kJobPool + 8) == 0 && U32(m, kJobPool + 22760) == 0) { g_pool_quiet = true; break; }
      ctx->r3.u64 = kJobPool + 24;
      __imp__RtlLeaveCriticalSection(*ctx, m->virtual_membase());
      ++g_pool_hold_retries;
      if (Ms(t0, Clock::now()) > 50) {  // take it anyway, but the frame is not quiet
        ctx->r3.u64 = kJobPool + 24;
        __imp__RtlEnterCriticalSection(*ctx, m->virtual_membase());
        g_pool_quiet = false;
        break;
      }
      std::this_thread::yield();
    }
    *ctx = saved;
    held = true;
  }
  ~PoolHold() {
    if (!held) return;
    PPCContext* ctx = rex::runtime::current_ppc_context();
    const PPCContext saved = *ctx;
    ctx->r3.u64 = kJobPool + 24;
    __imp__RtlLeaveCriticalSection(*ctx, g_mem_hold->virtual_membase());
    *ctx = saved;
  }
  static rex::memory::Memory* g_mem_hold;
};
rex::memory::Memory* PoolHold::g_mem_hold = nullptr;
REXCVAR_DEFINE_BOOL(rb2_wait_render, true, "Debug", "Rollback (rollback3): before a log / rewind, wait on the game's frame timer 0x84248B3C (timeout 0)");
double WaitRender(rex::memory::Memory* m, int max_ms) {
  const auto t0 = Clock::now();
  if (!REXCVAR_GET(rb2_wait_render)) return 0.0;
  PPCContext* ctx = rex::runtime::current_ppc_context();
  uint8_t* base = m->virtual_membase();
  const PPCContext saved = *ctx;
  for (int i = 0; i < max_ms * 4; ++i) {
    ctx->r3.u64 = U32(m, 0x84248B3C);
    ctx->r4.u64 = 0;
    sub_822A2CF8(*ctx, base);
    if (ctx->r3.u32 != 258) break;  // signalled
    std::this_thread::yield();
    if (Ms(t0, Clock::now()) > max_ms) break;
  }
  *ctx = saved;
  return Ms(t0, Clock::now());
}

// ---- the script ---------------------------------------------------------------------------------
struct Test {
  int depth;
  uint32_t steps;
};

std::vector<Test> ParseTests() {
  std::vector<Test> out;
  const std::string spec = REXCVAR_GET(rb2_tests);
  if (spec.empty()) {
    const uint32_t d = kDefaultSteps;
    const uint32_t live = 1u << 20;
    const uint32_t r = d & ~16384u;
    out = {{10, r | live}, {10, r}, {10, r}, {7, r}, {10, r & ~8u}, {10, d}, {30, r}, {8, r}, {1, r}, {4, r}};
    return out;
  }
  size_t i = 0;
  while (i < spec.size()) {
    size_t j = spec.find(',', i);
    if (j == std::string::npos) j = spec.size();
    const std::string item = spec.substr(i, j - i);
    const size_t c = item.find(':');
    Test t{std::atoi(item.c_str()), kDefaultSteps};
    if (c != std::string::npos) t.steps = uint32_t(std::strtoul(item.c_str() + c + 1, nullptr, 0));
    if (t.depth > 0) out.push_back(t);
    i = j + 1;
  }
  return out;
}

struct Script {
  int stage = 0;  // 0 wait, 1 measure, 2 tests, 3 done
  int fight_ticks = 0;
  uint32_t battle = 0;
  std::vector<Range> ranges;
  std::vector<uint8_t> shadow;  // measure: last tick; tests: S
  std::vector<uint8_t> ever;
  uint64_t changed[kClsCount] = {}, peak[kClsCount] = {};
  int measured = 0;
  double scan_ms = 0, scan_peak = 0, tick_ms = 0, tick_peak = 0;
  Clock::time_point last_hook{};
  // tests
  std::vector<Test> tests;
  size_t test = 0;
  int phase = 0;  // 0 gap, 1 recording
  int gap = 0, k = 0;
  std::vector<uint64_t> prints;
  std::vector<std::vector<uint8_t>> blobs;
  int live_bad = -1;
  int passed = 0, failed = 0;
  double resim_ms_sum = 0;
  int resim_ticks = 0;
} g;

void Log(const std::string& s) {
  REXLOG_WARN("[RB2] {}", s);
}

void CaptureAll(rex::memory::Memory* m, std::vector<uint8_t>& buf) {
  buf.resize(TotalSize(g.ranges));
  for (const Range& r : g.ranges) std::memcpy(buf.data() + r.offset, m->TranslateVirtual<const uint8_t*>(r.address), r.size);
}

void MeasureTick(rex::memory::Memory* m) {
  const auto t0 = Clock::now();
  uint64_t now[kClsCount] = {};
  for (const Range& r : g.ranges) {
    const uint8_t* host = m->TranslateVirtual<const uint8_t*>(r.address);
    for (uint32_t p = 0; p < r.size; p += kPage) {
      const uint32_t n = std::min(kPage, r.size - p);
      uint8_t* sh = g.shadow.data() + r.offset + p;
      if (std::memcmp(host + p, sh, n) != 0) {
        std::memcpy(sh, host + p, n);
        ++now[r.cls];
        g.ever[(r.offset + p) / kPage] = 1;
      }
    }
  }
  const double ms = Ms(t0, Clock::now());
  g.scan_ms += ms;
  g.scan_peak = std::max(g.scan_peak, ms);
  uint64_t total = 0;
  for (int c = 0; c < kClsCount; ++c) {
    g.changed[c] += now[c];
    g.peak[c] = std::max(g.peak[c], now[c]);
    total += now[c];
  }
  if (g.measured < 5 || g.measured % 60 == 0) {
    Log(fmt::format("tick {}: data {} low {} heap {} phys {} = {} pages, scan {:.2f} ms", g.measured, now[kData], now[kLow],
                    now[kHeap], now[kPhys], total, ms));
  }
}

void MeasureReport() {
  const int ticks = std::max(g.measured, 1);
  uint64_t ever[kClsCount] = {};
  struct Block { uint32_t address, pages; };
  std::vector<Block> blocks;
  for (const Range& r : g.ranges) {
    for (uint32_t p = 0; p < r.size; p += kPage) {
      if (!g.ever[(r.offset + p) / kPage]) continue;
      ++ever[r.cls];
      const uint32_t b = (r.address + p) & ~0xFFFFu;
      if (blocks.empty() || blocks.back().address != b) blocks.push_back({b, 0});
      ++blocks.back().pages;
    }
  }
  uint64_t avg_total = 0;
  for (int c = 0; c < kClsCount; ++c) {
    avg_total += g.changed[c];
    Log(fmt::format("measure {}: {:.1f} pages/tick (peak {}), union {} pages ({:.2f} MB) over {} ticks", kClsNames[c],
                    double(g.changed[c]) / ticks, g.peak[c], ever[c], double(ever[c]) * kPage / 1048576.0, ticks));
  }
  Log(fmt::format("measure all: {:.1f} pages/tick = {:.2f} MB/tick; full scan+copy {:.2f} ms avg, {:.2f} ms peak; "
                  "host tick {:.2f} ms avg, {:.2f} peak",
                  double(avg_total) / ticks, double(avg_total) / ticks * kPage / 1048576.0, g.scan_ms / ticks, g.scan_peak,
                  g.tick_ms / ticks, g.tick_peak));
  std::sort(blocks.begin(), blocks.end(), [](const Block& a, const Block& b) { return a.pages > b.pages; });
  for (size_t i = 0; i < blocks.size() && i < 32; ++i) {
    Log(fmt::format("  block {:08X} ({}): {} pages", blocks[i].address, kClsNames[ClassOf(blocks[i].address)], blocks[i].pages));
  }
}

// The rollback of one test, all in this host frame.
void RunRollback(rex::memory::Memory* m, const Test& t) {
  const uint64_t real_print = Fingerprint(m);
  const double wait_ms = WaitRender(m, 40);
  // 1. pages changed since S (the undo log of `depth` ticks) and the real state C of those pages
  const auto t0 = Clock::now();
  std::vector<uint32_t> dirty;  // offsets
  std::vector<uint8_t> c_pages;
  uint64_t per[kClsCount] = {};
  for (const Range& r : g.ranges) {
    const uint8_t* host = m->TranslateVirtual<const uint8_t*>(r.address);
    for (uint32_t p = 0; p < r.size; p += kPage) {
      const uint32_t n = std::min(kPage, r.size - p);
      if (std::memcmp(host + p, g.shadow.data() + r.offset + p, n) != 0) {
        dirty.push_back(uint32_t(r.offset + p));
        ++per[r.cls];
      }
    }
  }
  const auto t1 = Clock::now();
  c_pages.resize(dirty.size() * size_t(kPage));
  // offset -> host address
  auto host_of = [&](uint32_t off) -> uint8_t* {
    auto it = std::upper_bound(g.ranges.begin(), g.ranges.end(), off, [](uint32_t o, const Range& r) { return o < r.offset; });
    --it;
    return m->TranslateVirtual<uint8_t*>(it->address + (off - uint32_t(it->offset)));
  };
  for (size_t i = 0; i < dirty.size(); ++i) std::memcpy(c_pages.data() + i * kPage, host_of(dirty[i]), kPage);
  const auto t2 = Clock::now();
  // 2. restore S
  for (uint32_t off : dirty) std::memcpy(host_of(off), g.shadow.data() + off, kPage);
  const auto t3 = Clock::now();
  const uint64_t back_print = Fingerprint(m);
  if (t.steps & (1u << 20)) {  // control: the game's own loop replays the ticks (real ticks, drawing included)
    Log(fmt::format("test {} depth {} LIVE replay: undo pages {} = {:.2f} MB, diff scan {:.2f} ms, restore S {:.2f} ms, restored-S print {}",
                    g.test, t.depth, dirty.size(), double(dirty.size()) * kPage / 1048576.0, Ms(t0, t1), Ms(t2, t3),
                    back_print == g.prints[0] ? "ok" : "BAD"));
    g.phase = 2;
    g.k = 0;
    g.live_bad = -1;
    return;
  }
  // 3. re-simulate
  int first_bad = -1;
  std::vector<double> per_tick;
  for (int i = 0; i < t.depth; ++i) {
    const auto a = Clock::now();
    ResimTick(m, t.steps);
    per_tick.push_back(Ms(a, Clock::now()));
    if (first_bad < 0 && Fingerprint(m) != g.prints[size_t(i) + 1]) {
      first_bad = i + 1;
      Log(fmt::format("test {} resim tick {} differs: {}", g.test, first_bad, BlobDiff(g.blobs[size_t(i) + 1], FpBlob(m))));
    }
  }
  const auto t4 = Clock::now();
  const uint64_t resim_print = Fingerprint(m);
  // 4. pages that differ from the real state (C for the dirty pages, S for the rest)
  std::vector<uint8_t> is_dirty_mark;
  size_t differ = 0, repaired = 0;
  uint64_t dper[kClsCount] = {};
  std::vector<uint32_t> diff_blocks;
  {
    size_t di = 0;
    for (const Range& r : g.ranges) {
      uint8_t* host = m->TranslateVirtual<uint8_t*>(r.address);
      for (uint32_t p = 0; p < r.size; p += kPage) {
        const uint32_t off = uint32_t(r.offset + p);
        const uint32_t n = std::min(kPage, r.size - p);
        const uint8_t* want;
        if (di < dirty.size() && dirty[di] == off) {
          want = c_pages.data() + di * kPage;
          ++di;
        } else {
          want = g.shadow.data() + off;
        }
        if (std::memcmp(host + p, want, n) != 0) {
          ++differ;
          ++dper[r.cls];
          if (diff_blocks.size() < 4096) diff_blocks.push_back((r.address + p) & ~0xFFFFu);
          std::memcpy(host + p, want, n);  // 5. the real state back
          ++repaired;
        }
      }
    }
  }
  const auto t5 = Clock::now();
  double rs = 0, rpeak = 0;
  for (double x : per_tick) { rs += x; rpeak = std::max(rpeak, x); }
  g.resim_ms_sum += rs;
  g.resim_ticks += t.depth;
  const bool ok = first_bad < 0 && resim_print == real_print && back_print == g.prints[0];
  (ok ? g.passed : g.failed)++;
  Log(fmt::format("test {} depth {} steps {:#x}: {} | restored-S print {} | undo pages {} (data {} low {} heap {} phys {}) "
                  "= {:.2f} MB | render wait {:.2f} ms, diff scan {:.2f} ms, save C {:.2f} ms, restore S {:.2f} ms, "
                  "resim {:.2f} ms ({:.2f}/tick, peak {:.2f}), verify+back {:.2f} ms",
                  g.test, t.depth, t.steps, ok ? "SAME" : fmt::format("DIFFERS from tick {}", first_bad),
                  back_print == g.prints[0] ? "ok" : "BAD", dirty.size(), per[kData], per[kLow], per[kHeap], per[kPhys],
                  double(dirty.size()) * kPage / 1048576.0, wait_ms, Ms(t0, t1), Ms(t1, t2), Ms(t2, t3), Ms(t3, t4),
                  rs / t.depth, rpeak, Ms(t4, t5)));
  std::sort(diff_blocks.begin(), diff_blocks.end());
  std::string blocks;
  int shown = 0;
  for (size_t i = 0; i < diff_blocks.size() && shown < 20;) {
    size_t j = i;
    while (j < diff_blocks.size() && diff_blocks[j] == diff_blocks[i]) ++j;
    blocks += fmt::format(" {:08X}x{}", diff_blocks[i], j - i);
    ++shown;
    i = j;
  }
  Log(fmt::format("test {}: after resim {} pages differ from the real state (data {} low {} heap {} phys {}), put back;{}",
                  g.test, differ, dper[kData], dper[kLow], dper[kHeap], dper[kPhys], blocks));
  (void)repaired;
}

void TestsTick(rex::memory::Memory* m) {
  if (g.test >= g.tests.size()) {
    Log(fmt::format("tests: {} same, {} differ; resim {:.3f} ms per tick over {} ticks", g.passed, g.failed,
                    g.resim_ms_sum / std::max(g.resim_ticks, 1), g.resim_ticks));
    Log("done");
    g_split = false;
    rex::FlushLogging();
    g.stage = 3;
    return;
  }
  const Test& t = g.tests[g.test];
  if (g.phase == 0) {
    if (++g.gap < 30) return;
    g.gap = 0;
    const auto a = Clock::now();
    CaptureAll(m, g.shadow);
    const double ms = Ms(a, Clock::now());
    if (g.test == 0) Log(fmt::format("full capture of {:.1f} MB: {:.2f} ms", double(g.shadow.size()) / 1048576.0, ms));
    g.prints.assign(1, Fingerprint(m));
    g.blobs.assign(1, FpBlob(m));
    if (g.test == 0) DumpTasks(m);
    g.phase = 1;
    g.k = 0;
    return;
  }
  if (g.phase == 2) {  // live replay of the control test
    ++g.k;
    if (g.live_bad < 0 && Fingerprint(m) != g.prints[size_t(g.k)]) {
      g.live_bad = g.k;
      Log(fmt::format("test {} live tick {} differs: {}", g.test, g.k, BlobDiff(g.blobs[size_t(g.k)], FpBlob(m))));
    }
    if (g.k < t.depth) return;
    Log(fmt::format("test {} LIVE replay of {} ticks: {}", g.test, t.depth,
                    g.live_bad < 0 ? std::string("SAME") : fmt::format("DIFFERS from tick {}", g.live_bad)));
    (g.live_bad < 0 ? g.passed : g.failed)++;
    rex::FlushLogging();
    g.phase = 0;
    ++g.test;
    return;
  }
  g.prints.push_back(Fingerprint(m));
  g.blobs.push_back(FpBlob(m));
  if (++g.k < t.depth) return;
  // prints[depth] = now
  RunRollback(m, t);
  rex::FlushLogging();
  if (g.phase == 2) return;
  g.phase = 0;
  ++g.test;
}

}  // namespace

// ---- sync test engine (rb2_sync=<max depth>) -----------------------------------------------------
//
// State = the hot set (pages that changed during calibration, ~2300 pages / 9 MB in a fight), taken
// at the start of every fight UPDATE pass; a rewind puts an image back whole, except the "outside"
// words: 64-bit words that changed BETWEEN update passes during calibration (draw pass, render /
// job / sound threads, the loop's other steps) - those belong to other timelines and keep their live
// value. The fight's random number state is host-side, swapped into 0x842F9334 for every update pass
// only (the draws keep their own). A re-simulated frame = one update pass, nothing else.
// Every rb2_every updates: rewind n (1..rb2_sync, cycling), re-simulate n updates, compare the hot
// set (outside words skipped) with the image the original timeline left, continue from the
// re-simulated state.
REXCVAR_DEFINE_INT32(rb2_sync, 0, "Debug", "Rollback sync test: rewind 1..N frames every rb2_every updates (0 off)");
REXCVAR_DEFINE_INT32(rb2_every, 30, "Debug", "Rollback sync test: updates between rewinds");
REXCVAR_DEFINE_STRING(rb2_skip, "0x82146A90,0x8214AA88,0x82149C50", "Debug", "Rollback sync test: update task callbacks a resim leaves out (they run with the cosmetic RNG)");
REXCVAR_DEFINE_INT32(rb2_trace, 0, "Debug", "Rollback sync test: log every step of the first N rewinds");
REXCVAR_DEFINE_STRING(rb2_exclude, "0x83600000-0x83940000,0x40006000-0x40100000,0x8437E800-0x8437EE00,0x839CC000-0x839CE000,0x83A4E000-0x840D6000,0xBF000000-0xC0000000,0xDF000000-0xE0000000,0xFF000000-0xFFFFF000", "Debug", "Rollback sync test: guest ranges never rewound (a-b,...)");
REXCVAR_DEFINE_BOOL(rb2_mask, false, "Debug", "Rollback sync test: a rewind keeps the words the draw / other threads changed during calibration");
REXCVAR_DEFINE_BOOL(rb2_fxband, true, "Debug", "Rollback sync test: a resim runs the effects' draw halves too");
REXCVAR_DEFINE_BOOL(rb2_drawpages, true, "Debug", "Rollback sync test: pages the draw / frame end write are rewound too (whole frame state)");
REXCVAR_DEFINE_STRING(rb2_forcehot, "0x840D6000-0x84141000", "Debug", "Rollback sync test: guest ranges always in the hot set (the effects pool)");
REXCVAR_DEFINE_BOOL(rb2_track, true, "Debug", "Rollback sync test: exact undo logs from the store marks (needs the patched generated code)");
REXCVAR_DEFINE_BOOL(rb2_foreign, true, "Debug", "Rollback sync test (tracked): pages only other threads wrote are not rewound");
REXCVAR_DEFINE_STRING(rb2_resim_draw, "", "Debug", "Rollback sync test: draw task callbacks a resim runs too (a,b,...)");
REXCVAR_DEFINE_INT32(rb2_stack, 65536, "Debug", "Rollback sync test: bytes of the main thread's stack below the hook put back before each resim frame (0 off)");
REXCVAR_DEFINE_INT32(rb2_resim_sleep_us, 0, "Debug", "Rollback sync test: wait after each resim frame (other threads catch up; diagnostic)");
REXCVAR_DEFINE_STRING(rb2_watch_range, "", "Debug", "Rollback (rollback3): log stores into lo-hi (hex) with their function");
REXCVAR_DEFINE_BOOL(rb2_watch, false, "Debug", "Rollback sync test: log every store into fighter 0's +0x50..+0x90");
REXCVAR_DEFINE_BOOL(rb2_ffine_keep, true, "Debug", "Rollback (rollback3): foreign 16-byte marks of a page stay until its next log (not dropped in frames without one)");
REXCVAR_DEFINE_STRING(rb2_live_ranges, "8438AC00-8438AD00,840D6100-840D6300", "Debug", "Rollback (rollback3): guest ranges (hex a-b,...) a rewind never writes - render hand-off state shared with the draw worker (its context 0x8438AC00: event, ready flag, work item)");
REXCVAR_DEFINE_BOOL(rb2_log_diffs, false, "Debug", "Rollback (rollback3) diagnostics: log every word a resim did not reproduce (first rewinds of the sync test)");
REXCVAR_DEFINE_BOOL(rb2_effects_full, false, "Debug", "Rollback (rollback3): the effects pool 0x840D6000-0x84141000 is restored whole (no live foreign blocks)");
REXCVAR_DEFINE_BOOL(rb2_d3d_log, false, "Debug", "Rollback (rollback3) diagnostics: log the D3D devices the main thread uses (D3D entry r3) per phase");
REXCVAR_DEFINE_BOOL(rb2_pause_render, false, "Debug", "Rollback (rollback3) test: suspend the render loop / draw worker threads while a rewind restores and resimulates");
REXCVAR_DEFINE_BOOL(rb2_pool_live, true, "Debug", "Rollback (rollback3): the job pool and its node allocator are never restored (shared with the render side)");
REXCVAR_DEFINE_BOOL(rb2_log_restored, false, "Debug", "Rollback (rollback3) diagnostics: list the pages the first rewinds restore");
REXCVAR_DEFINE_BOOL(rb2_global_lock, true, "Debug", "Rollback (rollback3): hold the runtime global critical region while a rewind restores guest memory");
REXCVAR_DEFINE_BOOL(rb2_log_shared, false, "Debug", "Rollback (rollback3) diagnostics: log pages the main update and another thread both wrote in a frame");
REXCVAR_DEFINE_INT32(rb2_worker_tbit, 8, "Debug", "Rollback (rollback3) test: the write class of the job workers (8 frame, 2 foreign)");
REXCVAR_DEFINE_INT32(rb2_draw_tbit, 4, "Debug", "Rollback (rollback3) test: the write class of the draw pass (4 cosmetic window, 2 foreign)");
REXCVAR_DEFINE_INT32(rb2_rewind_sleep, 0, "Debug", "Rollback (rollback3) test: ms to sleep before a rewind frame's log (lets the draw worker finish)");
REXCVAR_DEFINE_BOOL(rb2_render_live, false, "Debug", "Rollback (rollback3): draw tasks outside the effects band write foreign (kept live) and a resim runs only the effects band");
REXCVAR_DEFINE_BOOL(rb2_resim_alldraw, true, "Debug", "Rollback sync test: a resim frame runs the whole draw pass but the frame end");
REXCVAR_DEFINE_BOOL(rb2_resim_cri, false, "Debug", "Rollback sync test: a resim frame runs the CRI sound / file server step too");
REXCVAR_DEFINE_BOOL(rb2_sound_pages, true, "Debug", "Rollback: pages the sound library code (CRI) wrote are never rewound");
REXCVAR_DEFINE_INT32(rb2_loop, 0, "Debug", "Rollback loopback test (versus2p): player 2's input arrives this many frames late and is predicted meanwhile (needs rb2_sync > 0)");
REXCVAR_DEFINE_INT32(rb2_loop_jitter, 3, "Debug", "Rollback loopback test: extra 0..N frames of late arrival per frame");
REXCVAR_DEFINE_INT32(rb2_loop_lag, 20, "Debug", "Rollback loopback test: player 2 plays player 1's command of this many frames earlier");
REXCVAR_DEFINE_BOOL(rb2_sound_fine, true, "Debug", "Rollback: pages with sound-library and game writes are rewound except the 16-byte blocks the sound library wrote");
REXCVAR_DEFINE_INT32(rb2_phases, 4, "Debug", "Rollback (rollback3): battle phases the engine runs in, bit per phase (4 = FIGHT! only, 6 = + READY?, 14 = + round end)");
REXCVAR_DEFINE_BOOL(rb2_no_cinematics, true, "Debug", "Rollback: no snapshots / rewinds while a cinematic runs (battle+574 bit 2)");
REXCVAR_DEFINE_BOOL(rb2_keep_locks, false, "Debug", "Rollback: guest critical sections / spin locks keep their live bytes across a rewind");
REXCVAR_DEFINE_INT32(rb2_calib, 60, "Debug", "Rollback sync test: calibration frames");


// Hang watch: a resim that runs over 300 ms gets the main thread's stack scanned for guest functions
// (return addresses mapped through the recompiled function table), 3 samples.
std::atomic<int64_t> g_resim_since{0};  // steady_clock ticks, 0 = not in a resim
std::atomic<int64_t> g_last_frame{0};    // steady_clock ticks of the last frame start (stall watch)
DWORD g_main_tid = 0;
rex::memory::Memory* g_mem_for_watch = nullptr;
std::atomic<int64_t> sync_frame_for_fault{0};
// rollback3: at the first hang report, every other thread of the process with guest frames on its stack
// (which thread holds what the main thread waits for), plus CRI's global lock.
// module + offset of a host address from a list taken outside any hang (GetModuleHandleEx takes the loader lock,
// which a hung thread can hold)
std::vector<std::tuple<uint64_t, uint64_t, std::string>> g_modules;
void CacheModules() {
  HMODULE mods[512];
  DWORD need = 0;
  if (!K32EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &need)) return;
  g_modules.clear();
  for (DWORD k = 0; k < need / sizeof(HMODULE) && k < 512; ++k) {
    MODULEINFO mi = {};
    char name[MAX_PATH] = {};
    if (!K32GetModuleInformation(GetCurrentProcess(), mods[k], &mi, sizeof(mi))) continue;
    K32GetModuleBaseNameA(GetCurrentProcess(), mods[k], name, MAX_PATH);
    g_modules.emplace_back(uint64_t(mi.lpBaseOfDll), uint64_t(mi.lpBaseOfDll) + mi.SizeOfImage, name);
  }
}
inline std::string ModOf(uint64_t a) {
  for (const auto& [lo, hi, name] : g_modules)
    if (a >= lo && a < hi) return fmt::format("{}+{:X}", name, a - lo);
  return fmt::format("{:X}", a);
}
// A raw, allocation-free hang dump (rb2_hang_raw): a hung thread can hold the process heap or the loader lock, so
// the dump uses only a file opened at the engine's start, snprintf into a static buffer and WriteFile.
HANDLE g_hang_raw = INVALID_HANDLE_VALUE;
void OpenHangRaw() {
  std::string path = rex::cvar::GetFlagByName("log_file");
  if (path.empty()) path = "rb2_hang";
  path += ".hangraw.txt";
  g_hang_raw = CreateFileA(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
}
void RawOut(const char* text) {
  if (g_hang_raw == INVALID_HANDLE_VALUE) return;
  DWORD written = 0;
  WriteFile(g_hang_raw, text, DWORD(std::strlen(text)), &written, nullptr);
}
const char* ModNameRaw(uint64_t a, uint64_t* off) {  // no allocation (g_modules filled at the start)
  for (const auto& [lo, hi, name] : g_modules)
    if (a >= lo && a < hi) { *off = a - lo; return name.c_str(); }
  *off = a;
  return "?";
}
DWORD g_thread_ids[512];
int g_thread_count = 0;
void CacheThreadIds() {  // outside any hang: the snapshot allocates from the process heap
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  if (snap == INVALID_HANDLE_VALUE) return;
  THREADENTRY32 te = {};
  te.dwSize = sizeof(te);
  int n = 0;
  for (BOOL ok = Thread32First(snap, &te); ok && n < 512; ok = Thread32Next(snap, &te))
    if (te.th32OwnerProcessID == GetCurrentProcessId()) g_thread_ids[n++] = te.th32ThreadID;
  CloseHandle(snap);
  g_thread_count = n;
}
template <typename F>
void DumpAllThreadsRaw(F guest_of) {
  static char buf[8192];
  const DWORD self = GetCurrentThreadId();
  // the main thread first, then the rest (newest threads first: the guest threads were made after the host ones)
  static DWORD order[513];
  int no = 0;
  order[no++] = g_main_tid;
  for (int q = g_thread_count - 1; q >= 0; --q) if (g_thread_ids[q] != g_main_tid) order[no++] = g_thread_ids[q];
  for (int q = 0; q < no; ++q) {
    struct { DWORD th32ThreadID; } te{order[q]};
    if (te.th32ThreadID == self) continue;
    HANDLE th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, te.th32ThreadID);
    if (!th) continue;
    uint32_t raw[24];
    uint64_t host[8];
    int found = 0, nhost = 0;
    SuspendThread(th);
    CONTEXT c = {};
    c.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
    uint64_t unwound[28];
    int nun = 0;
    if (GetThreadContext(th, &c)) {
      // exact frames from the modules' unwind tables (.pdata), no allocation (h7: a stack scan mixed in data
      // pointers). RtlLookupFunctionEntry only reads the loaded modules' tables.
      CONTEXT u = c;
      for (int f = 0; f < 28 && u.Rip; ++f) {
        unwound[nun++] = u.Rip;
        DWORD64 image = 0;
        PRUNTIME_FUNCTION fe = RtlLookupFunctionEntry(u.Rip, &image, nullptr);
        if (!fe) {
          uint64_t ret = 0;
          if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(u.Rsp), &ret, 8, nullptr)) break;
          u.Rip = ret;
          u.Rsp += 8;
        } else {
          PVOID hd = nullptr;
          DWORD64 est = 0;
          RtlVirtualUnwind(UNW_FLAG_NHANDLER, image, u.Rip, fe, &u, &hd, &est, nullptr);
        }
      }
    }
    if (true) {
      const uint64_t* st = reinterpret_cast<const uint64_t*>(c.Rsp);
      uint32_t last = 0;
      for (int i = 0; i < 4096 && found < 20; ++i) {
        uint64_t v = 0;
        if (!ReadProcessMemory(GetCurrentProcess(), st + i, &v, 8, nullptr)) break;
        const uint32_t g = guest_of(uintptr_t(v));
        if (g && g != last) { raw[found++] = g; last = g; }
        if (!g && nhost < 8 && v > 0x10000 && i < 1024) {
          // the cached module list, no system call: a thread suspended inside a VirtualProtect holds the address
          // space lock and VirtualQuery would wait on it forever (h1: the dump stopped after the main thread)
          uint64_t mo = 0;
          if (ModNameRaw(v, &mo)[0] != '?' && mo > 0x1000) host[nhost++] = v;
        }
      }
    }
    ResumeThread(th);
    CloseHandle(th);
    uint64_t off = 0;
    const char* mod = ModNameRaw(c.Rip, &off);
    int n = std::snprintf(buf, sizeof(buf), "thread %lu%s: rip %s+%llX (guest %08X)", (unsigned long)te.th32ThreadID,
                          te.th32ThreadID == g_main_tid ? " MAIN" : "", mod, (unsigned long long)off, guest_of(uintptr_t(c.Rip)));
    for (int k = 0; k < nhost && n < 7000; ++k) {
      const char* hm = ModNameRaw(host[k], &off);
      n += std::snprintf(buf + n, sizeof(buf) - n, " < %s+%llX", hm, (unsigned long long)off);
    }
    n += std::snprintf(buf + n, sizeof(buf) - n, " | unwound:");
    for (int k = 0; k < nun && n < 6500; ++k) {
      const char* um = ModNameRaw(unwound[k], &off);
      const uint32_t ug = guest_of(uintptr_t(unwound[k]));
      n += ug ? std::snprintf(buf + n, sizeof(buf) - n, " %s+%llX(%08X)", um, (unsigned long long)off, ug)
              : std::snprintf(buf + n, sizeof(buf) - n, " %s+%llX", um, (unsigned long long)off);
    }
    n += std::snprintf(buf + n, sizeof(buf) - n, " | guest");
    for (int k = 0; k < found && n < 8000; ++k) n += std::snprintf(buf + n, sizeof(buf) - n, " < %08X", raw[k]);
    std::snprintf(buf + n, sizeof(buf) - n, "\r\n");
    RawOut(buf);
  }
  RawOut("---- end of thread dump\r\n");
}
// hang reports also go straight to <log_file>.hang.txt (WinAPI, no logger: the logger can be what is stuck)
void HangOut(const std::string& line) {
  std::string path = rex::cvar::GetFlagByName("log_file");
  if (path.empty()) path = "rb2_hang";
  path += ".hang.txt";
  HANDLE f = CreateFileA(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                         FILE_ATTRIBUTE_NORMAL, nullptr);
  if (f == INVALID_HANDLE_VALUE) return;
  const std::string text = line + std::string(1, char(13)) + std::string(1, char(10));
  DWORD written = 0;
  WriteFile(f, text.data(), DWORD(text.size()), &written, nullptr);
  CloseHandle(f);
  REXLOG_WARN("{}", line);  // last: the logger may be what is stuck
}
template <typename F>
void DumpAllThreads(F guest_of) {
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  if (snap == INVALID_HANDLE_VALUE) return;
  THREADENTRY32 te = {};
  te.dwSize = sizeof(te);
  const DWORD pid = GetCurrentProcessId(), self = GetCurrentThreadId();
  int shown = 0;
  for (BOOL ok = Thread32First(snap, &te); ok && shown < 100; ok = Thread32Next(snap, &te)) {
    if (te.th32OwnerProcessID != pid || te.th32ThreadID == self) continue;
    HANDLE th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, te.th32ThreadID);
    if (!th) continue;
    // nothing that allocates or locks while the thread is suspended (it may hold the heap / log lock)
    uint32_t raw[24];
    uint64_t host[6];
    int found = 0, nhost = 0;
    SuspendThread(th);
    CONTEXT c = {};
    c.ContextFlags = CONTEXT_CONTROL;
    if (GetThreadContext(th, &c)) {
      const uint64_t* st = reinterpret_cast<const uint64_t*>(c.Rsp);
      uint32_t last = 0;
      for (int i = 0; i < 4096 && found < 20; ++i) {
        uint64_t v = 0;
        if (!ReadProcessMemory(GetCurrentProcess(), st + i, &v, 8, nullptr)) break;
        const uint32_t g = guest_of(uintptr_t(v));
        if (g && g != last) { raw[found++] = g; last = g; }
        // host return addresses into our modules (no symbols: module + offset), the first few
        if (!g && nhost < 6 && v > 0x10000 && i < 512) {
          MEMORY_BASIC_INFORMATION mbi = {};
          if (VirtualQuery(reinterpret_cast<LPCVOID>(v), &mbi, sizeof(mbi)) && mbi.Type == MEM_IMAGE &&
              (mbi.Protect & (PAGE_EXECUTE_READ | PAGE_EXECUTE | PAGE_EXECUTE_READWRITE))) host[nhost++] = v;
        }
      }
    }
    ResumeThread(th);
    CloseHandle(th);
    std::string chain;
    for (int k = 0; k < found; ++k) chain += fmt::format(" < {:08X}", raw[k]);
    std::string where = ModOf(c.Rip);
    for (int k = 0; k < nhost; ++k) where += " < " + ModOf(host[k]);
    HangOut(fmt::format("[RB2] HANG thread {}{}: rip in guest {:08X} | {} | guest{}", te.th32ThreadID,
                        te.th32ThreadID == g_main_tid ? " (MAIN)" : "", guest_of(uintptr_t(c.Rip)), where, chain));
    ++shown;
  }
  CloseHandle(snap);
  if (g_mem_for_watch) {
    const uint32_t csp = rex::memory::load_and_swap<uint32_t>(g_mem_for_watch->TranslateVirtual<const uint8_t*>(0x842614C8));
    if (csp >= 0x40000000) {
      const uint8_t* cs = g_mem_for_watch->TranslateVirtual<const uint8_t*>(csp);
      std::string hex;
      for (int i = 0; i < 28; ++i) hex += fmt::format("{:02X}", cs[i]);
      REXLOG_WARN("[RB2] HANG CRI lock [842614C8] = {:08X}: {}", csp, hex);
    }
    // every registered guest critical section that is not free (lock count != -1): which lock the
    // waiters wait on, and whether its bytes still make sense (count vs recursion vs owner)
    std::vector<std::pair<uint32_t, uint32_t>> locks;
    rex::system::GetGuestLocks(locks);
    int held = 0;
    for (const auto& [ga, len] : locks) {
      if (len != 28) continue;
      const uint8_t* cs = g_mem_for_watch->TranslateVirtual<const uint8_t*>(ga);
      int32_t count;
      std::memcpy(&count, cs + 16, 4);  // host-endian (atomic ops on it)
      const uint32_t rec = rex::memory::load_and_swap<uint32_t>(cs + 20);
      const uint32_t owner = rex::memory::load_and_swap<uint32_t>(cs + 24);
      if (count == -1 && rec == 0 && owner == 0) continue;
      if (++held > 40) break;
      REXLOG_WARN("[RB2] HANG lock {:08X}: count {} recursion {} owner {:08X} header {:02X} {:08X}{:08X}", ga, count, rec, owner,
                  cs[0], rex::memory::load_and_swap<uint32_t>(cs + 8), rex::memory::load_and_swap<uint32_t>(cs + 12));
    }
    REXLOG_WARN("[RB2] HANG {} registered locks, {} not free", locks.size(), held);
  }
  rex::FlushLogging();
}
// rollback3: guest call chain of a faulting thread (first-chance access violation in guest code), logged
// once per thread kind - the SDK's crash line only gives lr.
std::vector<std::pair<uintptr_t, uint32_t>> g_func_table;
uint32_t GuestOfHost(uintptr_t h) {
  const auto& table = g_func_table;
  if (table.empty() || h < table.front().first || h > table.back().first + 0x100000) return 0;
  auto it = std::upper_bound(table.begin(), table.end(), std::make_pair(h, 0xFFFFFFFFu));
  if (it == table.begin()) return 0;
  --it;
  return h - it->first < 0x40000 ? it->second : 0;
}
std::atomic<int> g_fault_logs{0};
LONG CALLBACK FaultChain(PEXCEPTION_POINTERS ep) {
  if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION) return EXCEPTION_CONTINUE_SEARCH;
  const uint32_t at = GuestOfHost(uintptr_t(ep->ContextRecord->Rip));
  if (!at || !g_mem_for_watch) return EXCEPTION_CONTINUE_SEARCH;
  // only NULL-page guest accesses (MMIO / write-watch faults are normal operation)
  const uint64_t host = ep->ExceptionRecord->ExceptionInformation[1];
  const uint64_t mb = uint64_t(uintptr_t(g_mem_for_watch->virtual_membase()));
  if (host < mb || host - mb >= 0x10000 + 0x100000000ull || (host - mb) % 0x100000000ull >= 0x10000) return EXCEPTION_CONTINUE_SEARCH;
  if (g_fault_logs.fetch_add(1) >= 6) return EXCEPTION_CONTINUE_SEARCH;
  std::string chain = fmt::format("rip-guest {:08X}", at);
  const uint64_t* st = reinterpret_cast<const uint64_t*>(ep->ContextRecord->Rsp);
  uint32_t last = at;
  int found = 0;
  ULONG_PTR lo = 0, hi = 0;
  GetCurrentThreadStackLimits(&lo, &hi);
  const int words = ep->ContextRecord->Rsp >= lo && ep->ContextRecord->Rsp < hi ? int((hi - ep->ContextRecord->Rsp) / 8) : 0;
  for (int i = 0; i < 6000 && i < words && found < 30; ++i) {
    const uint32_t g = GuestOfHost(uintptr_t(st[i]));
    if (g && g != last) { chain += fmt::format(" < {:08X}", g); last = g; ++found; }
  }
  REXLOG_WARN("[RB2] FAULT thread {} (frame {}, {}): {}", GetCurrentThreadId(), sync_frame_for_fault.load(),
              g_in_resim ? "in resim" : "live", chain);
  if (at >= 0x820F0000 && at < 0x820F1000) PoolDump("fault");
  rex::FlushLogging();
  return EXCEPTION_CONTINUE_SEARCH;
}
// Last chance (an exception nobody handled - the process is about to die): what and where, into the raw hang file
// (no allocation, no logger lock) and then the log. on43: the guest died with an access violation in recompiled
// code and left no line at all (FaultChain logs only NULL-page guest accesses).
LONG WINAPI CrashFilter(PEXCEPTION_POINTERS ep) {
  static std::atomic<int> once{0};
  if (once.fetch_add(1)) return EXCEPTION_CONTINUE_SEARCH;
  static char buf[8192];
  const auto* er = ep->ExceptionRecord;
  const uint64_t rip = ep->ContextRecord->Rip;
  uint64_t off = 0;
  const char* mod = ModNameRaw(rip, &off);
  const uint64_t data = er->NumberParameters >= 2 ? er->ExceptionInformation[1] : 0;
  const uint64_t mb = g_mem_for_watch ? uint64_t(uintptr_t(g_mem_for_watch->virtual_membase())) : 0;
  int n = std::snprintf(buf, sizeof(buf),
                        "\r\nCRASH thread %lu code %08lX at %s+%llX (guest function %08X) %s %llX (guest view +%llX) frame %lld %s | rax %llX rcx %llX rdx %llX r8 %llX r9 %llX rsp %llX\r\n",
                        GetCurrentThreadId(), er->ExceptionCode, mod, (unsigned long long)off, GuestOfHost(uintptr_t(rip)),
                        er->NumberParameters >= 1 && er->ExceptionInformation[0] ? "writing" : "reading",
                        (unsigned long long)data, (unsigned long long)(data >= mb ? data - mb : 0),
                        (long long)sync_frame_for_fault.load(), g_in_resim ? "in resim" : "live",
                        (unsigned long long)ep->ContextRecord->Rax, (unsigned long long)ep->ContextRecord->Rcx,
                        (unsigned long long)ep->ContextRecord->Rdx, (unsigned long long)ep->ContextRecord->R8,
                        (unsigned long long)ep->ContextRecord->R9, (unsigned long long)ep->ContextRecord->Rsp);
  RawOut(buf);  // the first line goes out before anything that could fault (snd_120: the stack scan below read past
                // the thread's stack and the filter itself crashed - nothing was written)
  ULONG_PTR lo = 0, hi = 0;
  GetCurrentThreadStackLimits(&lo, &hi);
  // host call chain by unwinding (.pdata, no symbols needed: burstlimit.map names the offsets)
  n = std::snprintf(buf, sizeof(buf), "unwind:");
  CONTEXT c = *ep->ContextRecord;
  for (int i = 0; i < 48 && n < int(sizeof(buf)) - 96; ++i) {
    if (c.Rip == 0 || c.Rsp < lo || c.Rsp >= hi) break;
    uint64_t o = 0;
    const char* m = ModNameRaw(c.Rip, &o);
    const uint32_t g = GuestOfHost(uintptr_t(c.Rip));
    n += g ? std::snprintf(buf + n, sizeof(buf) - n, " %s+%llX(%08X)", m, (unsigned long long)o, g)
           : std::snprintf(buf + n, sizeof(buf) - n, " %s+%llX", m, (unsigned long long)o);
    DWORD64 image = 0;
    PRUNTIME_FUNCTION fe = RtlLookupFunctionEntry(c.Rip, &image, nullptr);
    if (!fe) {  // a leaf: the return address is at rsp
      if (c.Rsp + 8 > hi) break;
      c.Rip = *reinterpret_cast<const DWORD64*>(c.Rsp);
      c.Rsp += 8;
    } else {
      PVOID handler_data = nullptr;
      DWORD64 establisher = 0;
      RtlVirtualUnwind(UNW_FLAG_NHANDLER, image, c.Rip, fe, &c, &handler_data, &establisher, nullptr);
    }
  }
  n += std::snprintf(buf + n, sizeof(buf) - n, "\r\nguest chain (stack scan):");
  // guest functions found on the stack, never past the thread's stack
  const uint64_t* st = reinterpret_cast<const uint64_t*>(ep->ContextRecord->Rsp);
  const int words = ep->ContextRecord->Rsp >= lo && ep->ContextRecord->Rsp < hi ? int((hi - ep->ContextRecord->Rsp) / 8) : 0;
  uint32_t last = GuestOfHost(uintptr_t(rip));
  for (int i = 0, found = 0; i < 6000 && i < words && found < 40 && n < int(sizeof(buf)) - 64; ++i) {
    const uint32_t g = GuestOfHost(uintptr_t(st[i]));
    if (g && g != last) { n += std::snprintf(buf + n, sizeof(buf) - n, " < %08X", g); last = g; ++found; }
  }
  std::snprintf(buf + n, sizeof(buf) - n, "\r\n");
  RawOut(buf);
  return EXCEPTION_CONTINUE_SEARCH;
}
std::atomic<bool> g_dump_request{false};
REXCVAR_DEFINE_INT32(rb2_dump_frame, 0, "Debug", "Rollback (rollback3): dump every guest thread's call chain at this engine frame (and 600 frames later), 0 off");
void WatchMain() {
  static std::vector<std::pair<uintptr_t, uint32_t>> table;
  for (size_t i = 0; PPCFuncMappings[i].guest != 0; ++i)
    table.emplace_back(reinterpret_cast<uintptr_t>(PPCFuncMappings[i].host), uint32_t(PPCFuncMappings[i].guest));
  std::sort(table.begin(), table.end());
  g_func_table = table;
  AddVectoredExceptionHandler(1, FaultChain);
  SetUnhandledExceptionFilter(CrashFilter);
  // also the LAST vectored handler: an access violation the SDK's handler (MMIO, write watches) didn't take is
  // about to kill the process - on49 the top-level filter never ran (something replaces it)
  AddVectoredExceptionHandler(0, [](PEXCEPTION_POINTERS ep) -> LONG {
    if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) CrashFilter(ep);
    return EXCEPTION_CONTINUE_SEARCH;
  });
  auto guest_of = [&](uintptr_t h) -> uint32_t {
    if (table.empty() || h < table.front().first || h > table.back().first + 0x100000) return 0;
    auto it = std::upper_bound(table.begin(), table.end(), std::make_pair(h, 0xFFFFFFFFu));
    if (it == table.begin()) return 0;
    --it;
    return h - it->first < 0x40000 ? it->second : 0;
  };
  int reported = 0;
  for (;;) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    {  // rollback3 diagnostics: the GPU thread's state every 5 s while an engine runs (the screen froze at the first
       // rewinds while the fight went on)
      static int ticks = 0;
      if (++ticks % 50 == 0) {
        auto& gs = rex::perf::GetGpuThreadState();
        const uint64_t now_ms = uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count());
        REXLOG_INFO("[RB2] gpu thread: {} for {} ms, primary buffers {}, read {} write {}, wait reg mem {:08X} ref {:08X} value {:08X} flags {:X}, fence {} / {}",
                    rex::perf::GetGpuThreadWhereName(gs.where.load()), now_ms - gs.where_since_ms.load(), gs.primary_buffers.load(),
                    gs.read_ptr.load(), gs.write_ptr.load(), gs.wait_reg_mem_address.load(), gs.wait_reg_mem_ref.load(),
                    gs.wait_reg_mem_value.load(), gs.wait_reg_mem_flags.load(), gs.fence_await.load(), gs.fence_completed.load());
      }
    }
    if (g_dump_request.exchange(false)) {  // rb2_dump_frame: every guest thread's call chain (no hang)
      REXLOG_WARN("[RB2] thread dump (rb2_dump_frame)");
      DumpAllThreads(guest_of);
      rex::FlushLogging();
    }
    const int64_t since = g_resim_since.load();
    const int64_t last = g_last_frame.load();
    if (reported >= 6) continue;
    double ms = 0;
    if (since) {
      ms = std::chrono::duration<double, std::milli>(Clock::now().time_since_epoch() - Clock::duration(since)).count();
      if (ms < 300) continue;
    } else if (last) {
      ms = std::chrono::duration<double, std::milli>(Clock::now().time_since_epoch() - Clock::duration(last)).count();
      if (ms < 1500) continue;
    } else {
      continue;
    }
    HANDLE th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, g_main_tid);
    if (!th) continue;
    if (reported == 0) {
      RawOut(since ? "HANG in resim - every thread:\r\n" : "HANG no frame start - every thread:\r\n");
      if (g_mem_for_watch) {  // the job pool's and the job-node allocator's locks (both never rewound), raw
        static char lk[512];
        auto hex32 = [](const uint8_t* q, char* o) {
          for (int k = 0; k < 32; ++k) std::snprintf(o + k * 2, 3, "%02X", q[k]);
        };
        char a[80] = {}, b[80] = {};
        hex32(g_mem_for_watch->TranslateVirtual<const uint8_t*>(0x8437F918u), a);
        hex32(g_mem_for_watch->TranslateVirtual<const uint8_t*>(0x8438D884u), b);
        std::snprintf(lk, sizeof(lk), "locks: pool CS 8437F918 %s | node allocator 8438D884 %s\r\n", a, b);
        RawOut(lk);
      }
      DumpAllThreadsRaw(guest_of);
      HangOut(fmt::format("[RB2] HANG detected ({:.0f} ms, {}), every thread (file + log), then main samples", ms, since ? "in resim" : "no frame start"));
      DumpAllThreads(guest_of);
    }
    for (int sample = 0; sample < 3; ++sample) {
      uint32_t raw[32];
      int found = 0;
      uint32_t rip = 0;
      SuspendThread(th);
      CONTEXT c = {};
      c.ContextFlags = CONTEXT_CONTROL;
      if (GetThreadContext(th, &c)) {
        rip = guest_of(uintptr_t(c.Rip));
        const uint64_t* st = reinterpret_cast<const uint64_t*>(c.Rsp);
        uint32_t last = 0;
        for (int i = 0; i < 4096 && found < 28; ++i) {
          uint64_t v = 0;
          if (!ReadProcessMemory(GetCurrentProcess(), st + i, &v, 8, nullptr)) break;
          const uint32_t g = guest_of(uintptr_t(v));
          if (g && g != last) { raw[found++] = g; last = g; }
        }
      }
      ResumeThread(th);
      std::string chain = fmt::format("rip {:08X} ({})", rip, ModOf(c.Rip));
      for (int k = 0; k < found; ++k) chain += fmt::format(" < {:08X}", raw[k]);
      HangOut(fmt::format("[RB2] HANG {} ({:.0f} ms) sample {}: {}", since ? "in resim" : "main thread (no frame start)", ms, sample, chain));
      rex::FlushLogging();
      std::this_thread::sleep_for(std::chrono::milliseconds(40));
    }
    CloseHandle(th);
    ++reported;
  }
}

rex::memory::Memory* g_mem = nullptr;
std::thread::id g_main_thread;
// CRI call boundary (below, rb2_cri)
namespace cri { void Begin(int64_t frame, bool resim); void Commit(); void Reset(); extern bool g_outside;
void Settle(rex::memory::Memory* m, PPCContext& ctx, uint8_t* base, int64_t now); extern int64_t g_now; }
void cri_excl_page(uint32_t page);
std::string CriStats();
extern "C" int rb2_cri_on;
extern "C" int rb2_d3d_watch;
extern "C" thread_local unsigned char rb2_main;
extern std::atomic<bool> g_res_main_touch;  // the resource cache hooks (below)
extern std::atomic<int64_t> g_res_calls_main, g_res_calls_other, g_res_logs_marked;
bool ResGuardOn();
int64_t BurstLimitRb2LoopPeekRewind();
bool g_draining = false;  // the fight paused with unsettled predictions: settle them, predict nothing new
int64_t BurstLimitOnlineOldestUnsettled();
bool BurstLimitOnlineEngineDelayOk();  // burstlimit_netinput.cpp: the delay is the one the fight started with  // burstlimit_netinput.cpp: oldest engine frame consumed with an unsettled prediction, -1 none
// Cosmetic random numbers (everything but the fight's logic windows): the main thread's stream is part
// of the frame state (saved / restored with the snapshots: the resim's draw pass then makes the same
// effects), other threads have their own (never rewound).
uint32_t g_cosmetic_main = 0x1234567u, g_cosmetic_other = 0x7654321u;
namespace sync {
constexpr int kRing = 64;  // online rewinds count engine frames incl. held ones (on17: depth 11 refused; rb_spike: a 500 ms
                           // lag spike = 32 frames to go back, refused at 20 -> desync). ~1 MB of undo log per frame
struct Image {
  int64_t frame = -1;
  size_t pages = 0;
  uint32_t fight_rng = 0;
  std::vector<uint8_t> bytes;
};
struct State {
  int stage = 0;  // 0 off, 1 calibrating, 2 running
  uint32_t battle = 0;
  std::vector<Range> ranges;
  std::vector<uint8_t*> host;  // per page index
  std::vector<uint32_t> addr;  // per page index: guest address
  std::vector<uint64_t> outside;  // 8 x 64 bits per page
  std::vector<uint8_t> post, base;
  bool have_post = false;
  std::vector<uint32_t> hot;
  std::vector<uint8_t> is_hot;
  std::vector<uint8_t> excluded;  // rb2_exclude
  std::vector<uint8_t> between;  // pages changed between update passes during calibration
  Image ring[kRing];
  uint32_t fight_rng = 0, cosmetic_rng = 0;
  bool in_update = false, active = false;
  int calib_left = 0;
  int64_t frame = 0;  // fight updates since calibration
  int64_t rewinds = 0, same = 0, differ = 0, late_pages = 0;
  double render_wait_ms = 0;
  double snap_ms = 0, restore_ms = 0, resim_ms = 0, verify_ms = 0, late_ms = 0;
  int64_t snaps = 0, resim_frames = 0;
  double resim_peak = 0;
  int next_depth = 0;
} s;

bool Outside(size_t page, uint32_t word) { return (s.outside[page * 8 + word / 64] >> (word % 64)) & 1; }

// Host writes into guest memory (SDK rex::system::NotifyHostWrite: file reads), on the calling guest
// thread: the main thread's are the frame's (redone by a resim), other threads' are foreign.
std::atomic<int64_t> t_host_writes{0}, t_host_write_bytes{0};
std::atomic<int64_t> g_cur_frame_for_hook{0};  // engine frame now (live), for other threads
// Lock bytes (flags & 1: critical sections, spin locks) belong to every thread's timeline: always
// foreign, so a rewind keeps them live.
REXCVAR_DEFINE_INT32(rb2_host_writes, 3, "Debug", "Rollback (rollback3): host writes marked (bit 0 file reads, bit 1 lock bytes)");
// A guest thread created at engine frame f: no rewind may go to f or before (the thread, its kernel
// blocks and whatever its creator set up for it would be taken back under it - and made again by the
// resim, a second thread; TTT2 v0.2.0 pools thread blocks for the same reason). Old rollback2 and this
// branch both crashed in threads made right after FIGHT! (0xF800013C...) at the first rewinds.
std::atomic<int64_t> g_thread_created_frame{-1};
std::atomic<int64_t> g_threads_created{0};
std::atomic<uint32_t> g_thread_last_start{0};
void HostWrite(uint32_t a, uint32_t len, uint32_t flags) {
  if (!(flags & 2)) a = CanonAddr(a);
  if (flags & 2) {
    REXLOG_WARN("[RB2] guest thread created at engine frame {} (start {:08X})", g_cur_frame_for_hook.load(), a);
    g_thread_created_frame.store(g_cur_frame_for_hook.load());
    ++g_threads_created;
    g_thread_last_start.store(a);
    return;
  }
  if (!len) return;
  if (!(REXCVAR_GET(rb2_host_writes) & ((flags & 1) ? 2 : 1))) return;
  const unsigned char bit = ((flags & 1) == 0 && (rb2_tbit == 1 || rb2_tbit == 4)) ? 1 : 2;
  const uint64_t end = uint64_t(a) + len;
  for (uint64_t p = a & ~0xFFFull; p < end; p += 0x1000) {
    unsigned char& d = rb2_dirty[(p >> 12) & 0xFFFFF];
    d |= bit;
  }
  if (bit == 2) {
    for (uint64_t b = a >> 4; b <= ((end - 1) >> 4); ++b) rb2_ffine[(b >> 3) & 0x1FFFFFF] |= uint8_t(1u << (b & 7));
  } else {
    for (uint64_t b = a >> 4; b <= ((end - 1) >> 4); ++b) rb2_mfine[(b >> 3) & 0x1FFFFFF] |= uint8_t(1u << (b & 7));
  }
  if (flags & 1) return;
  ++t_host_writes;
  t_host_write_bytes += len;
}
REXCVAR_DEFINE_BOOL(rb2_interrupt_gate, true, "Debug", "Rollback (rollback3): the GPU interrupt callbacks wait while a rewind restores guest memory (SDK gate)");
REXCVAR_DEFINE_BOOL(rb2_object_reuse, true, "Debug", "Rollback (rollback3): a guest kernel object header that lost its host handle in a rewind gets its host object back (SDK)");
void Setup(rex::memory::Memory* m) {
  rex::system::SetHostWriteHook(&HostWrite);
  rex::system::SetGuestLockRegistry(true);  // the SDK registers guest locks only from here on (costly when always on)
  rex::system::SetNativeObjectReuse(REXCVAR_GET(rb2_object_reuse));
  CacheModules();
  OpenHangRaw();
  rex::system::SetInterruptGate(REXCVAR_GET(rb2_interrupt_gate));
  CacheThreadIds();
  g_mem = m;
  g_mem_for_watch = m;
  PoolHold::g_mem_hold = m;
  g_mem_dump = m;
  g_main_tid = GetCurrentThreadId();
  std::thread(WatchMain).detach();
  s.ranges = WritableRanges(m);
  const size_t pages = TotalSize(s.ranges) / kPage;
  s.host.assign(pages, nullptr);
  s.addr.assign(pages, 0);
  for (const Range& r : s.ranges) {
    uint8_t* h = m->TranslateVirtual<uint8_t*>(r.address);
    for (uint32_t p = 0; p < r.size; p += kPage) {
      s.host[(r.offset + p) / kPage] = h + p;
      s.addr[(r.offset + p) / kPage] = r.address + p;
    }
  }
  s.outside.assign(pages * 8, 0);
  s.is_hot.assign(pages, 0);
  s.excluded.assign(pages, 0);
  auto parse_list = [](const std::string& spec) {
    std::vector<std::pair<uint32_t, uint32_t>> out;
    size_t i = 0;
    while (i < spec.size()) {
      size_t j = spec.find(',', i);
      if (j == std::string::npos) j = spec.size();
      const std::string item = spec.substr(i, j - i);
      const size_t d = item.find('-');
      const uint32_t a = uint32_t(std::strtoul(item.c_str(), nullptr, 0));
      const uint32_t b = d == std::string::npos ? a + 1 : uint32_t(std::strtoul(item.c_str() + d + 1, nullptr, 0));
      if (a) out.emplace_back(a, b);
      i = j + 1;
    }
    return out;
  };
  size_t excluded = 0;
  for (auto [a, b] : parse_list(REXCVAR_GET(rb2_exclude))) {
    for (uint64_t pg = a >> 12; pg < ((uint64_t(b) + 0xFFF) >> 12) && pg < 0x100000; ++pg) cri_excl_page(uint32_t(pg));
    for (size_t i = 0; i < pages; ++i) {
      if (s.addr[i] + kPage > a && s.addr[i] < b) { s.excluded[i] = 1; ++excluded; }
    }
  }
  size_t forced = 0;
  for (auto [a, b] : parse_list(REXCVAR_GET(rb2_forcehot))) {
    for (size_t i = 0; i < pages; ++i) {
      if (s.addr[i] + kPage > a && s.addr[i] < b && !s.excluded[i] && !s.is_hot[i]) {
        s.is_hot[i] = 1;
        s.hot.push_back(uint32_t(i));
        ++forced;
      }
    }
  }
  Log(fmt::format("sync: {} pages always hot", forced));
  g_resim_draw.clear();
  for (auto [a, b] : parse_list(REXCVAR_GET(rb2_resim_draw))) g_resim_draw.push_back(a);
  g_resim_skip.clear();
  for (auto [a, b] : parse_list(REXCVAR_GET(rb2_skip))) g_resim_skip.push_back(a);
  Log(fmt::format("sync: {} pages excluded, {} update tasks left out of resims", excluded, g_resim_skip.size()));
  s.between.assign(pages, 0);
  s.hot.clear();
  s.post.resize(pages * size_t(kPage));
  s.have_post = false;
}

// Calibration checkpoint: what changed since the last checkpoint belongs to the window that just
// ended - a logic window (A+U: frame start to the draw pass; B: after the scheduler to the frame end)
// makes its pages hot, a cosmetic window (D: the draw pass; F: the frame end and the vsync waits)
// makes its words outside.
void Checkpoint(bool logic) {
  const size_t pages = s.host.size();
  if (!s.have_post) {
    for (size_t i = 0; i < pages; ++i) std::memcpy(s.post.data() + i * kPage, s.host[i], kPage);
    s.have_post = true;
    return;
  }
  for (size_t i = 0; i < pages; ++i) {
    uint8_t* old = s.post.data() + i * kPage;
    if (std::memcmp(s.host[i], old, kPage) == 0) continue;
    if (logic && s.excluded[i]) {
      // never rewound
    } else if (logic) {
      if (!s.is_hot[i]) { s.is_hot[i] = 1; s.hot.push_back(uint32_t(i)); }
    } else {
      const uint64_t* live = reinterpret_cast<const uint64_t*>(s.host[i]);
      const uint64_t* o = reinterpret_cast<const uint64_t*>(old);
      for (uint32_t w = 0; w < kPage / 8; ++w) {
        if (live[w] != o[w]) s.outside[i * 8 + w / 64] |= 1ull << (w % 64);
      }
      s.between[i] = 1;
      if (REXCVAR_GET(rb2_drawpages) && !s.excluded[i] && !s.is_hot[i]) { s.is_hot[i] = 1; s.hot.push_back(uint32_t(i)); }
    }
    std::memcpy(old, s.host[i], kPage);
  }
}

void Snapshot(int64_t frame) {
  const auto t0 = Clock::now();
  Image& img = s.ring[frame % kRing];
  img.frame = frame;
  img.pages = s.hot.size();
  img.fight_rng = U32(g_mem, kRng);
  img.bytes.resize(img.pages * size_t(kPage));
  for (size_t k = 0; k < img.pages; ++k) std::memcpy(img.bytes.data() + k * kPage, s.host[s.hot[k]], kPage);
  s.snap_ms += Ms(t0, Clock::now());
  ++s.snaps;
}

// Pages outside the hot set that changed since the baseline join it (their earlier images are unknown).
size_t LateScan() {
  const auto t0 = Clock::now();
  size_t late = 0;
  const size_t pages = s.host.size();
  for (size_t i = 0; i < pages; ++i) {
    if (s.is_hot[i] || s.between[i] || s.excluded[i]) continue;
    uint8_t* b = s.base.data() + i * kPage;
    if (std::memcmp(s.host[i], b, kPage) == 0) continue;
    std::memcpy(b, s.host[i], kPage);
    s.is_hot[i] = 1;
    s.hot.push_back(uint32_t(i));
    if (late < 6) Log(fmt::format("late page {:08X}", s.addr[i]));
    ++late;
  }
  s.late_ms += Ms(t0, Clock::now());
  s.late_pages += late;
  return late;
}

void Restore(const Image& img) {
  for (size_t k = 0; k < img.pages; ++k) {
    const size_t i = s.hot[k];
    uint64_t* live = reinterpret_cast<uint64_t*>(s.host[i]);
    const uint64_t* want = reinterpret_cast<const uint64_t*>(img.bytes.data() + k * kPage);
    const uint64_t* mask = &s.outside[i * 8];
    bool any_mask = false;
    if (REXCVAR_GET(rb2_mask)) for (int q = 0; q < 8; ++q) any_mask |= mask[q] != 0;
    if (!any_mask) {
      std::memcpy(live, want, kPage);
      continue;
    }
    for (uint32_t w = 0; w < kPage / 8; ++w) {
      if (!((mask[w / 64] >> (w % 64)) & 1) && live[w] != want[w]) live[w] = want[w];
    }
  }
}

// Hot pages that differ from an image (outside words skipped); the first few logged.
size_t Compare(const Image& img, std::string* detail) {
  size_t bad = 0;
  int shown = 0;
  for (size_t k = 0; k < img.pages; ++k) {
    const size_t i = s.hot[k];
    const uint64_t* live = reinterpret_cast<const uint64_t*>(s.host[i]);
    const uint64_t* want = reinterpret_cast<const uint64_t*>(img.bytes.data() + k * kPage);
    if (std::memcmp(live, want, kPage) == 0) continue;
    int words = 0;
    uint32_t first = 0;
    for (uint32_t w = 0; w < kPage / 8; ++w) {
      if (live[w] != want[w] && !Outside(i, w)) {
        if (!words) first = w;
        ++words;
      }
    }
    if (!words) continue;
    ++bad;
    if (detail && shown < 12) {
      *detail += fmt::format(" {:08X}({}w {:016X}->{:016X})", s.addr[i] + first * 8, words,
                             __builtin_bswap64(want[first]), __builtin_bswap64(live[first]));
      ++shown;
    }
  }
  return bad;
}


// The fight's random numbers: the game's LCG word 0x842F9334 serves only the main thread's logic
// windows while the engine runs (sub_820E7020 replaced below); every other caller - the draw pass,
// the effects, other threads - gets a host-side cosmetic LCG.
std::atomic<bool> g_fight_window{false};
char g_phase = '?';
uint32_t g_cur_cb = 0;
std::mutex g_rng_mutex;
std::map<uint32_t, int> g_rng_real[kRing], g_rng_resim;  // fight-window callers (lr) per frame
int64_t g_rng_frame = -1;
uint64_t g_rng_cosmetic_calls = 0;
std::string RngCallers(int64_t frame, bool resim) {
  std::lock_guard<std::mutex> lock(g_rng_mutex);
  const auto& mp = resim ? g_rng_resim : g_rng_real[((frame - 1) % kRing + kRing) % kRing];
  std::string out = "[";
  for (auto& [lr, n] : mp) out += fmt::format(" {:08X}x{}", lr, n);
  return out + " ]";
}
void RngIn(rex::memory::Memory* m) { (void)m; g_fight_window.store(true); }
void RngOut(rex::memory::Memory* m) { (void)m; g_fight_window.store(false); }

// The real update pass while the engine runs: the update tasks in list order, but each run of tasks
// in the skip list (the effects) in a pass of its own with the cosmetic random numbers - a resim
// leaves them out, so they must not take the fight's numbers.
void RunTasksPass(rex::memory::Memory* m, PPCContext& ctx, uint8_t* base, uint32_t list, const std::vector<uint32_t>& tasks) {
  if (tasks.empty()) return;
  uint32_t prev = 0;
  if (REXCVAR_GET(rb2_trace) > 0) {  // diagnostic: one pass per task, the running task known
    for (uint32_t task : tasks) {
      g_cur_cb = U32(m, task + 32);
      if (g_in_resim && g_trace) { REXLOG_WARN("[RB2] step task cb {:08X}", g_cur_cb); rex::FlushLogging(); }
      ctx.r3.u64 = list; ctx.r4.u64 = task; ctx.r5.u64 = 0;
      sub_82122450(ctx, base);
      ctx.r3.u64 = list;
      sub_821228A8(ctx, base);
    }
    g_cur_cb = 0;
    return;
  }
  for (uint32_t task : tasks) {
    ctx.r3.u64 = list;
    ctx.r4.u64 = task;
    ctx.r5.u64 = prev;
    sub_82122450(ctx, base);
    prev = task;
  }
  ctx.r3.u64 = list;
  sub_821228A8(ctx, base);
}
void RealUpdate(rex::memory::Memory* m, PPCContext& ctx, uint8_t* base, uint32_t list) {
  g_phase = 'U';
  const bool engine = s.stage != 0 && s.active;
  std::vector<uint32_t> run;
  bool run_cosmetic = false;
  int walked = 0;
  auto flush = [&]() {
    if (run.empty()) return;
    if (run_cosmetic && engine) RngOut(m);
    RunTasksPass(m, ctx, base, list, run);
    if (run_cosmetic && engine) RngIn(m);
    run.clear();
  };
  for (uint32_t task = U32(m, list + 32); task && ++walked < 1024; task = U32(m, task + 40)) {
    const uint32_t prio = U32(m, task + 24), cb = U32(m, task + 32);
    if (prio >= 0x20000000 && cb != 0x8217E100) continue;
    const bool cosmetic = std::find(g_resim_skip.begin(), g_resim_skip.end(), cb) != g_resim_skip.end();
    if (cosmetic != run_cosmetic) { flush(); run_cosmetic = cosmetic; }
    run.push_back(task);
  }
  flush();
}

// One re-simulated frame: the main loop's logic from 0x822198B0 (after the vsync waits) to the frame
// end call 0x8221994C - the frame start, the hook's steps, the update tasks, the post steps - without
// the draw pass, the frame end (render hand-off) and the waits.

void T(const char* what) {
  if (g_trace > 0) {
    REXLOG_WARN("[RB2] step {}", what);
    rex::FlushLogging();
  }
}
REXCVAR_DEFINE_INT32(rb2_resim_pre, 0x1FF, "Debug", "Rollback (rollback3): the frame-start steps a resim frame runs (1 sub_82218950, 2 sub_824A7DD0, 16 8210D2A8, 32 8212A600, 64 82132550, 128 82120CF8, 256 8211D890)");
REXCVAR_DEFINE_INT32(rb2_resim_post, 7, "Debug", "Rollback (rollback3): the post steps a resim frame runs (1 sub_82117A70, 2 sub_82120CF8, 4 sub_8210D338)");
void ResimFrame(rex::memory::Memory* m, PPCContext& ctx, uint8_t* base) {
  g_in_resim = true;
  g_resim_since.store(Clock::now().time_since_epoch().count());
  RngIn(m);
  S32(m, 0x826DE5F8, U32(m, 0x826DE5FC));
  const int pre = REXCVAR_GET(rb2_resim_pre);
  if (pre & 1) { T("82218950"); sub_82218950(ctx, base); }
  if (const uint32_t r3 = U32(m, 0x84140890); r3 && (pre & 2)) {
    ctx.r3.u64 = r3;
    T("824A7DD0"); sub_824A7DD0(ctx, base);
  }
  T("821249C0"); sub_821249C0(ctx, base);
  const uint32_t system = U32(m, 0x82520000 + 1512);
  T("F600/D178");
  if ((ctx.r3.u32 & 0xFF) && U32(m, system + 3228) == 0) sub_8210F600(ctx, base);
  else sub_8210D178(ctx, base);
  const uint32_t counter = U32(m, system + 3228) + 1;
  S32(m, system + 3228, counter);
  if (!(int32_t(counter) < int32_t(U32(m, 0x826DE600)))) S32(m, system + 3228, 0);
  if (pre & 16) { T("8210D2A8"); sub_8210D2A8(ctx, base); }
  if (pre & 32) { T("8212A600"); sub_8212A600(ctx, base); }
  if (pre & 64) { T("82132550"); sub_82132550(ctx, base); }
  if (REXCVAR_GET(rb2_resim_cri)) { T("82119320"); sub_82119320(ctx, base); }  // the CRI sound / file server
  if (pre & 128) { T("82120CF8"); sub_82120CF8(ctx, base); }
  if (pre & 256) { T("8211D890"); sub_8211D890(ctx, base); }
  T("update");
  if (const uint32_t list = SchedulerBegin(m)) {
    RealUpdate(m, ctx, base, list);
    if (REXCVAR_GET(rb2_resim_alldraw) && !REXCVAR_GET(rb2_render_live)) {  // the whole draw pass but the frame end (no hand-off to the render thread)
      T("draw pass");
      RngOut(m);
      rb2_tbit = 4;
      uint32_t prev = 0;
      int walked = 0;
      for (uint32_t task = U32(m, list + 32); task && ++walked < 1024; task = U32(m, task + 40)) {
        const uint32_t prio = U32(m, task + 24), cb = U32(m, task + 32);
        if (!(prio >= 0x20000000 && cb != 0x8217E100) || prio == 0xFFFFFFFF) continue;
        ctx.r3.u64 = list; ctx.r4.u64 = task; ctx.r5.u64 = prev;
        sub_82122450(ctx, base);
        prev = task;
      }
      ctx.r3.u64 = list;
      sub_821228A8(ctx, base);
      rb2_tbit = 1;
      RngIn(m);
    } else if (REXCVAR_GET(rb2_fxband)) {  // the effects' draw halves (their update halves need them), cosmetic RNG
      T("fx band");
      if (g_trace) {
        int walked = 0, index = 0;
        for (uint32_t task = U32(m, list + 32); task && ++walked < 1024; task = U32(m, task + 40)) {
          const uint32_t prio = U32(m, task + 24), cb = U32(m, task + 32);
          if (prio >= 0x20000000 && prio != 0xFFFFFFFF && (((prio >> 24) == 0x2C && cb != 0x82141C80) || cb == 0x82145E98 || cb == 0x821F2BB0 || cb == 0x82161BC8))
            REXLOG_WARN("[RB2] step fx task {} cb {:08X}", index++, cb);
        }
        rex::FlushLogging();
      }
      RngOut(m);
      LinkAndRun(m, ctx, base, list, true);
      RngIn(m);
    }
  }
  const int post = REXCVAR_GET(rb2_resim_post);
  if (post & 1) { T("82117A70"); sub_82117A70(ctx, base); }
  if (post & 2) { T("82120CF8b"); sub_82120CF8(ctx, base); }
  if (post & 4) { T("8210D338"); sub_8210D338(ctx, base); }
  T("end");
  g_in_resim = false;
  g_resim_since.store(0);
  RngOut(m);
}

void Rewind(rex::memory::Memory* m, PPCContext& ctx, uint8_t* base, int n) {
  const int64_t now = s.frame;
  const Image& target = s.ring[(now - n) % kRing];
  const Image& expect = s.ring[now % kRing];
  if (target.frame != now - n || expect.frame != now) return;
  g_trace = s.rewinds < REXCVAR_GET(rb2_trace) ? 1 : 0;
  T("rewind");
  const double rw = WaitRender(m, 50) + WaitWorkers(m, 50);
  const size_t late = LateScan();
  const auto t0 = Clock::now();
  Restore(target);
  T("restored");
  const auto t1 = Clock::now();
  int first_bad = -1;
  std::string detail;
  double verify = 0, peak = 0;
  for (int k = 1; k <= n; ++k) {
    {
      std::lock_guard<std::mutex> lock(g_rng_mutex);
      g_rng_resim.clear();
    }
    const auto a = Clock::now();
    ResimFrame(m, ctx, base);
    const double ms = Ms(a, Clock::now());
    peak = std::max(peak, ms);
    s.resim_ms += ms;
    ++s.resim_frames;
    const auto v = Clock::now();
    const Image& img = s.ring[(now - n + k) % kRing];
    if (first_bad < 0 && img.frame == now - n + k) {
      std::string d;
      const size_t bad = Compare(img, &d);
      if (bad || (k == n && U32(m, kRng) != img.fight_rng)) {
        first_bad = k;
        detail = fmt::format("{} pages, rng {:08X} vs {:08X}, rng callers real {} resim {}:{}", bad, U32(m, kRng), img.fight_rng, RngCallers(img.frame, false), RngCallers(img.frame, true), d);
      }
    }
    verify += Ms(v, Clock::now());
  }
  s.restore_ms += Ms(t0, t1);
  s.verify_ms += verify;
  s.resim_peak = std::max(s.resim_peak, peak);
  ++s.rewinds;
  (first_bad < 0 ? s.same : s.differ)++;
  if (first_bad >= 0 || s.rewinds <= 8 || s.rewinds % 20 == 0) {
    Log(fmt::format("sync #{} frame {} rewind {}: {} | restore {:.2f} ms ({} pages), resim {:.2f} ms/frame (peak {:.2f}), "
                    "verify {:.2f} ms, render wait {:.2f} ms, late pages {}{}",
                    s.rewinds, now, n, first_bad < 0 ? "SAME" : fmt::format("DIFFERS at frame +{}", first_bad), Ms(t0, t1),
                    target.pages, s.resim_frames ? s.resim_ms / s.resim_frames : 0.0, peak, verify, rw, late,
                    first_bad < 0 ? std::string() : " - " + detail));
  }
  if (s.rewinds % 20 == 0) {
    Log(fmt::format("sync summary: {} rewinds, {} same, {} differ; hot {} pages ({:.1f} MB), snapshot {:.3f} ms/frame, "
                    "restore {:.3f} ms, resim {:.3f} ms/frame (peak {:.2f}), late pages {} (scan {:.1f} ms/rewind)",
                    s.rewinds, s.same, s.differ, s.hot.size(), double(s.hot.size()) * kPage / 1048576.0,
                    s.snap_ms / std::max<int64_t>(s.snaps, 1), s.restore_ms / s.rewinds,
                    s.resim_ms / std::max<int64_t>(s.resim_frames, 1), s.resim_peak, s.late_pages, s.late_ms / s.rewinds));
    rex::FlushLogging();
  }
}

// 0x822198B0 (after the vsync waits): closes F, the frame's snapshot, the sync test's rewinds.
void FrameA(rex::memory::Memory* m, PPCContext& ctx, uint8_t* base) {
  g_main_thread = std::this_thread::get_id();
  g_phase = 'A';
  const uint32_t battle = U32(m, 0x841B5130);
  const bool fighting = battle && U32(m, 0x841B5228) == 2;
  if (s.stage == 0) {
    if (!fighting) return;
    Setup(m);
    s.stage = 1;
    s.calib_left = std::max(2, REXCVAR_GET(rb2_calib));
    s.fight_rng = U32(m, kRng);
    s.battle = battle;
    Log(fmt::format("sync: calibrating {} frames over {} pages", s.calib_left, s.host.size()));
  }
  const bool was = s.active;
  s.active = fighting && battle == s.battle;
  if (!s.active) {
    if (battle && battle != s.battle) s.battle = battle;
    for (Image& img : s.ring) img.frame = -1;
    return;
  }
  if (!was) s.fight_rng = U32(m, kRng);
  if (s.stage == 1) {
    s.render_wait_ms += WaitRender(m, 50) + WaitWorkers(m, 50);  // the render thread's writes fall in F, not in the logic windows
    if (was) Checkpoint(false);
    else s.have_post = false, Checkpoint(false);
    if (s.calib_left <= 0) {
      s.base.resize(s.post.size());
      std::memcpy(s.base.data(), s.post.data(), s.post.size());
      size_t masked = 0;
      for (uint64_t w : s.outside) masked += size_t(__builtin_popcountll(w));
      size_t between_only = 0;
      for (size_t i = 0; i < s.host.size(); ++i) between_only += s.between[i] && !s.is_hot[i];
      s.post.clear();
      s.post.shrink_to_fit();
      Log(fmt::format("sync: calibrated, hot set {} pages ({:.1f} MB), {} outside words, {} pages only written in the "
                      "draw / frame end (never rewound)", s.hot.size(), double(s.hot.size()) * kPage / 1048576.0, masked,
                      between_only));
      rex::FlushLogging();
      s.stage = 2;
      s.frame = 0;
    } else {
      RngIn(m);
      return;
    }
  }
  s.render_wait_ms += WaitWorkers(m, 20);
  Snapshot(s.frame);
  {
    std::lock_guard<std::mutex> lock(g_rng_mutex);
    g_rng_real[s.frame % kRing].clear();
  }
  const int every = std::max(2, REXCVAR_GET(rb2_every));
  const int depth_max = std::clamp(REXCVAR_GET(rb2_sync), 1, kRing - 2);
  if (s.frame > kRing && s.frame % every == 0) Rewind(m, ctx, base, 1 + (s.next_depth++ % depth_max));
  RngIn(m);
}
// The draw pass, inside the scheduler hook (after the update tasks).
void DrawBegin(rex::memory::Memory* m) {
  rb2_tbit = uint8_t(REXCVAR_GET(rb2_draw_tbit));  // main thread, cosmetic window (4; 2 = test: foreign)
  if (s.stage == 0 || !s.active) return;
  if (s.stage == 1) Checkpoint(true);
  RngOut(m);
}
void DrawEnd(rex::memory::Memory* m) {
  g_phase = 'B';
  rb2_tbit = 1;
  if (s.stage == 0 || !s.active) return;
  if (s.stage == 1) Checkpoint(false);
  RngIn(m);
}
// 0x8221994C (before the frame end): closes B.
double g_frame_work_ms = 0, g_frame_work_peak = 0;
int64_t g_frame_work_n = 0;
Clock::time_point g_frame_a_time{};
void FrameF(rex::memory::Memory* m) {
  if (g_frame_a_time.time_since_epoch().count()) {
    const double ms = Ms(g_frame_a_time, Clock::now());
    g_frame_work_ms += ms;
    g_frame_work_peak = std::max(g_frame_work_peak, ms);
    ++g_frame_work_n;
  }
  rb2_tbit = 4;
  if (s.stage == 0 || !s.active) return;
  if (s.stage == 1) {
    Checkpoint(true);
    --s.calib_left;
  } else {
    ++s.frame;
  }
  RngOut(m);
}

// ---- tracked mode (rb2_track): exact per-frame undo logs from the generated code's store marks ----
// Every guest store marks its page in rb2_dirty (scripts/patch_generated.py). At each frame start the
// pages written during the frame that just ended go into that frame's undo log with their content at
// its start (the shadow), and the shadow takes the new content. A rewind to frame t writes the logs of
// frames >= t back, newest first. rb2_foreign: pages only other threads wrote are not logged.
int64_t g_cur_frame = 0;  // the engine frame being simulated (real or resimulated)
// track engine diagnostics: the fight-window rng callers (lr x count) of each engine frame's latest run, and of
// its previous run while a rewind re-runs it - a resim that draws other numbers names the caller (g2: rng only)
std::map<uint32_t, int> g_rng_trk[kRing], g_rng_trk_was[kRing];
std::string RngTrkDiff(int64_t from, int64_t to) {
  std::lock_guard<std::mutex> lock(g_rng_mutex);
  for (int64_t f = from; f < to; ++f) {
    const int i = int(((f % kRing) + kRing) % kRing);
    if (g_rng_trk_was[i] == g_rng_trk[i]) continue;
    std::string a, b;
    for (auto& [lr, n] : g_rng_trk_was[i]) a += fmt::format(" {:08X}x{}", lr, n);
    for (auto& [lr, n] : g_rng_trk[i]) b += fmt::format(" {:08X}x{}", lr, n);
    return fmt::format(" | rng callers differ first at frame {}: live [{} ] resim [{} ]", f, a, b);
  }
  return " | rng callers the same in every resimulated frame";
}
// No rewind into the first frames after the engine turned on (FIGHT!): the job pool, CRI's voice loads and
// the effects are busy right after the fight starts (every early crash was at the first rewinds).
REXCVAR_DEFINE_INT32(rb2_settle, 120, "Debug", "Rollback (rollback3): engine frames after FIGHT! before any rewind may target a frame");
int64_t SettleFrames() { return std::max(REXCVAR_GET(rb2_settle), 21); }  // 21 as with the old 20-frame ring: a rewind needs its logs anyway (TrkRewind refuses without them)
int64_t g_cine_frame[kRing];  // engine frame that ran in a cinematic (supers, Drama Pieces...), -1 not
int64_t g_cine_rewinds = 0;
struct Undo {
  int64_t frame = 0;
  std::vector<uint32_t> pages;
  std::vector<uint8_t> bytes;
  std::vector<uint8_t> fmask;  // per page: 32 bytes, the 16-byte blocks other threads wrote in the frame
  bool quiet = true;           // logged with the job / draw workers idle (else a rewind across it is refused)
};
REXCVAR_DEFINE_BOOL(rb2_fence_live, false, "Debug", "Rollback (rollback3) test: the job pool fence (0x8437F910) keeps its live value across a rewind");
REXCVAR_DEFINE_STRING(rb2_restore_skip, "", "Debug", "Rollback (rollback3) test: pages in this hex range are not restored by a rewind");
REXCVAR_DEFINE_STRING(rb2_restore_range, "", "Debug", "Rollback (rollback3) test: only pages in this hex range are restored by a rewind");
REXCVAR_DEFINE_BOOL(rb2_lock_skip, true, "Debug", "Rollback (rollback3): a rewind never writes guest lock bytes (critical sections, spin locks)");
REXCVAR_DEFINE_BOOL(rb2_foreign_fine, true, "Debug", "Rollback (rollback3): 16-byte blocks other threads / host file reads wrote stay live when a frame's page is rewound");
struct Trk {
  bool ready = false;
  std::vector<uint32_t> gp;  // guest page number per page index
  std::vector<uint8_t> shadow, full, ever_main;
  std::deque<Undo> logs;
  int64_t frames = 0, logged = 0, foreign = 0, rewinds = 0, same = 0, differ = 0;
  double log_ms = 0, restore_ms = 0, resim_ms = 0, resim_peak = 0, wait_ms = 0;
  int64_t resim_frames = 0;
  int next_depth = 0;
  double last_wait = 0, frame_wait_ms = 0;
  int64_t noisy_logs = 0, refused_noisy = 0, refused_thread = 0, drain_frames = 0, refused_nologs = 0;
  int64_t mixed_pages = 0, fine_restores = 0, foreign_mixed = 0, foreign_fine_restores = 0, host_writes = 0, host_write_bytes = 0;
  size_t locks_kept = 0;
  int64_t lock_blocks_skipped = 0;  // 16-byte blocks restored around live guest lock bytes
  int64_t online_rewinds = 0, online_depth_sum = 0, online_too_deep = 0, online_refused = 0;
  int online_max_depth = 0;
  double online_cost_ms = 0, online_cost_peak = 0;
  std::vector<uint8_t> blobs[kRing];
  std::vector<uint8_t> stacks[kRing];  // the main thread's stack below the hook's frame at each frame start
  uint32_t cosmetic[kRing] = {};       // the main thread's cosmetic random state at each frame start
  uint32_t stack_sp[kRing] = {};
  int64_t blob_frame[kRing] = {};
  int64_t fp_same = 0, fp_differ = 0;
} t;

REXCVAR_DEFINE_BOOL(rb2_draw_live_all, false, "Debug", "Rollback (rollback3) test: every page the draw pass wrote in a frame is kept live (also pages the update wrote)");
REXCVAR_DEFINE_BOOL(rb2_draw_live, false, "Debug", "Rollback (rollback3): pages only the draw pass wrote in a frame are kept live by a rewind (not frame state)");
// diagnostics (rb2_alias_diag): physical memory is seen through three guest views (0xA0000000 64 KB pages, 0xC0000000
// 16 MB pages, 0xE0000000 4 KB pages at physical + 0x1000). Pages written this frame through a view the engine does
// NOT track, while another view of the same physical page IS tracked (its restore can't know about those writes).
REXCVAR_DEFINE_BOOL(rb2_alias_diag, false, "Debug", "Rollback (rollback3) diagnostics: count writes through untracked physical views whose alias is tracked");
std::vector<int32_t> g_page_index;  // guest page -> tracked page index, -1
int64_t g_alias_untracked[3] = {}, g_alias_untracked_tracked_alias[3] = {}, g_alias_both_tracked_dirty = 0;
uint32_t PhysOfPage(uint32_t page) {  // guest page number (>= 0xA0000) -> physical page number, ~0u none
  if (page >= 0xA0000 && page < 0xC0000) return page - 0xA0000;
  if (page >= 0xC0000 && page < 0xE0000) return page - 0xC0000;
  if (page >= 0xE0000) return page - 0xE0000 + 1;
  return ~0u;
}
void AliasDiag() {
  if (g_page_index.empty()) {
    g_page_index.assign(0x100000, -1);
    for (size_t i = 0; i < t.gp.size(); ++i) g_page_index[t.gp[i]] = int32_t(i);
  }
  for (uint32_t page = 0xA0000; page < 0x100000; ++page) {
    unsigned char d = rb2_dirty[page];
    if (!d) continue;
    const bool tracked = g_page_index[page] >= 0;
    const uint32_t ph = PhysOfPage(page);
    const uint32_t views[3] = {0xA0000 + ph, 0xC0000 + ph, ph >= 1 ? 0xE0000 + ph - 1 : 0};
    const int v = page < 0xC0000 ? 0 : page < 0xE0000 ? 1 : 2;
    bool alias_tracked = false, alias_dirty_tracked = false;
    for (int k = 0; k < 3; ++k) {
      if (k == v || views[k] < 0xA0000 || views[k] >= 0x100000) continue;
      if (g_page_index[views[k]] >= 0) { alias_tracked = true; if (rb2_dirty[views[k]]) alias_dirty_tracked = true; }
    }
    if (!tracked) {
      ++g_alias_untracked[v];
      if (alias_tracked) ++g_alias_untracked_tracked_alias[v];
      rb2_dirty[page] = 0;
    } else if (alias_dirty_tracked) {
      ++g_alias_both_tracked_dirty;
    }
  }
}
void TrkEndFrame(int64_t frame, bool keep) {
  const auto a = Clock::now();
  if (REXCVAR_GET(rb2_alias_diag)) AliasDiag();
  PoolHold hold(g_mem);
  Undo u;
  u.frame = frame;
  const bool res_touch = g_res_main_touch.exchange(false, std::memory_order_relaxed);
  u.quiet = g_workers_idle && g_pool_quiet && !(res_touch && ResGuardOn());
  if (res_touch && ResGuardOn()) ++g_res_logs_marked;
  if (!u.quiet) ++t.noisy_logs;
  const bool foreign_mode = REXCVAR_GET(rb2_foreign);
  for (size_t i = 0; i < t.gp.size(); ++i) {
    unsigned char& d = rb2_dirty[t.gp[i]];
    if (!d) continue;
    unsigned char bits = d;
    d = 0;
    if (s.excluded[i]) continue;
    if (NetObjExcluded(s.addr[i])) { std::memcpy(t.shadow.data() + i * kPage, s.host[i], kPage); continue; }
    // the job pool (lists, counts, free nodes) is frame state whoever touched it (workers, other pushers)
    const bool pool_page = FrameRange(s.addr[i]);
    if (pool_page && REXCVAR_GET(rb2_hold_pool)) bits = uint8_t((bits | 1) & ~16);
    uint8_t* sh = t.shadow.data() + i * kPage;
    if (bits & 1) t.ever_main[i] = 1;
    // rollback3: pages only the draw pass / frame end wrote (bit 4) are not frame state - a resim doesn't run
    // them again, and rewound they took the render hand-off back (the screen froze at the first rewinds)
    if (REXCVAR_GET(rb2_draw_live) && (bits & 4) && !(bits & 9) && !pool_page) bits = uint8_t((bits & ~4) | 2);
    if (REXCVAR_GET(rb2_draw_live_all) && (bits & 4) && !pool_page) bits = 2;  // test: any draw-pass page kept live
    const bool sound_only = (bits & 16) && !(bits & 13);
    const bool sound_skip = REXCVAR_GET(rb2_sound_fine) ? sound_only : (bits & 16) != 0;
    if ((bits & 16) && (bits & 13)) ++t.mixed_pages;
    if (REXCVAR_GET(rb2_log_shared) && (bits & 1) && (bits & 2) && !pool_page) {  // main update + another thread
      static std::map<uint32_t, int> shared;
      static int64_t last_log = 0;
      ++shared[s.addr[i]];
      if (frame - last_log >= 300) {
        last_log = frame;
        std::string list;
        int k = 0;
        for (const auto& [a, c] : shared) {
          if (++k > 80) break;
          list += fmt::format(" {:08X}x{}", a, c);
        }
        Log(fmt::format("pages written by the main update AND another thread (frame {}): {} pages:{}", frame, shared.size(), list));
      }
    }
    uint8_t* ff = &rb2_ffine[size_t(s.addr[i]) >> 7];
    if (!keep || (foreign_mode && !(bits & 13)) || (REXCVAR_GET(rb2_sound_pages) && sound_skip)) {
      if (keep) ++t.foreign;
      std::memcpy(sh, s.host[i], kPage);
      // a page only other threads wrote in this frame is not logged - its foreign marks must SURVIVE until the
      // page's next log (MEASURED rnd36-rnd50: cleared here, a later restore of an older log of the page put back
      // the blocks the draw worker had written in a frame without a log -> its allocator / queue went back and
      // the screen froze at the first rewinds; the same gap undid CRI lock ops)
      if (!keep || !REXCVAR_GET(rb2_ffine_keep)) {
        std::memcpy(&rb2_ffine_prev[size_t(s.addr[i]) >> 7], ff, 32);
        std::memset(ff, 0, 32);
      }
      continue;
    }
    u.pages.push_back(uint32_t(i));
    u.bytes.insert(u.bytes.end(), sh, sh + kPage);
    uint8_t* fp = &rb2_ffine_prev[size_t(s.addr[i]) >> 7];
    const bool prev_on = REXCVAR_GET(rb2_ffine_prev_on);
    bool prev_any = false;
    if (prev_on) for (int q = 0; q < 32; ++q) prev_any |= fp[q] != 0;
    uint8_t* mf = &rb2_mfine[size_t(s.addr[i]) >> 7];
    uint8_t lost[32] = {};
    bool lost_any = false;
    const bool gate = REXCVAR_GET(rb2_lost_gate);
    if (REXCVAR_GET(rb2_lost_marks) && !pool_page) {  // changed since the snapshot, no mark at all: a foreign store that landed late
      for (int blk = 0; blk < 256; ++blk) {
        const uint8_t bm = uint8_t(1u << (blk & 7));
        if ((ff[blk >> 3] & bm) || (mf[blk >> 3] & bm) || (gate && !(fp[blk >> 3] & bm))) continue;
        if (std::memcmp(sh + blk * 16, s.host[i] + blk * 16, 16) != 0) {
          lost[blk >> 3] |= bm; lost_any = true; ++g_lost_mark_blocks;
          if (g_lost_logged.load(std::memory_order_relaxed) < REXCVAR_GET(rb2_lost_log)) {
            ++g_lost_logged;
            Log(fmt::format("lost-mark block {:08X} at log of frame {} (page bits {:X}, previous foreign mark {}): {:08X} {:08X} -> {:08X} {:08X}",
                            s.addr[i] + blk * 16, frame, int(bits), (fp[blk >> 3] & bm) ? 1 : 0,
                            rex::memory::load_and_swap<uint32_t>(sh + blk * 16), rex::memory::load_and_swap<uint32_t>(sh + blk * 16 + 4),
                            rex::memory::load_and_swap<uint32_t>(s.host[i] + blk * 16), rex::memory::load_and_swap<uint32_t>(s.host[i] + blk * 16 + 4)));
          }
        }
      }
    }
    std::memset(mf, 0, 32);
    if (((bits & 2) || prev_any || lost_any) && !pool_page) {
      uint8_t m[32];
      for (int q = 0; q < 32; ++q) m[q] = uint8_t(ff[q] | lost[q] | (prev_on ? fp[q] : 0));
      u.fmask.insert(u.fmask.end(), m, m + 32);
      ++t.foreign_mixed;
    } else {
      u.fmask.insert(u.fmask.end(), 32, 0);
    }
    std::memcpy(fp, ff, 32);  // the generation this log clears (lost-mark gate; rb2_ffine_prev_on: kept live too)
    std::memset(ff, 0, 32);
    std::memcpy(sh, s.host[i], kPage);
  }
  if (keep) {
    t.logged += int64_t(u.pages.size());
    ++t.frames;
    t.logs.push_back(std::move(u));
    while (t.logs.size() > size_t(kRing)) t.logs.pop_front();
  }
  t.log_ms += Ms(a, Clock::now());
}

// online rollback trace: the first rollbacks of a session and every one in the last frames before a pause
bool verify_online_log_done(int n, int64_t to) {
  static int shown = 0;
  if (shown < 3000) { ++shown; REXLOG_INFO("[RB2] rollback at frame {} to {} (depth {})", s.frame, to, n); }
  return true;
}
// rollback3 test (rb2_pause_render): the render-side guest threads (render loop 0x82219360, draw worker 0x820EA188)
// read the fight's memory on their own; paused (OS suspend) while a rewind restores and resimulates
std::vector<DWORD> g_render_tids;
void FindRenderThreads() {
  g_render_tids.clear();
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  if (snap == INVALID_HANDLE_VALUE) return;
  THREADENTRY32 te = {};
  te.dwSize = sizeof(te);
  const DWORD pid = GetCurrentProcessId(), self = GetCurrentThreadId();
  for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
    if (te.th32OwnerProcessID != pid || te.th32ThreadID == self || te.th32ThreadID == g_main_tid) continue;
    HANDLE th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, te.th32ThreadID);
    if (!th) continue;
    bool render = false;
    SuspendThread(th);
    CONTEXT c = {};
    c.ContextFlags = CONTEXT_CONTROL;
    if (GetThreadContext(th, &c)) {
      const uint64_t* st = reinterpret_cast<const uint64_t*>(c.Rsp);
      for (int i = 0; i < 8192 && !render; ++i) {
        uint64_t v = 0;
        if (!ReadProcessMemory(GetCurrentProcess(), st + i, &v, 8, nullptr)) break;
        const uint32_t g = GuestOfHost(uintptr_t(v));
        render = g == 0x82219360 || g == 0x820EA188;
      }
    }
    ResumeThread(th);
    CloseHandle(th);
    if (render) g_render_tids.push_back(te.th32ThreadID);
  }
  CloseHandle(snap);
  Log(fmt::format("render-side threads found: {}", g_render_tids.size()));
}
// cooperative park of the render loop thread (rb2_park_render): it waits at its loop head while a rewind runs
std::atomic<int> g_park_req{0}, g_parked{0};
std::atomic<int64_t> g_park_timeouts{0}, g_park_ok{0};
double g_park_wait_ms_sum = 0, g_park_wait_ms_peak = 0;
REXCVAR_DEFINE_BOOL(rb2_park_render, true, "Debug", "Rollback (rollback3): the render loop thread waits at its loop head while a rewind restores and resimulates");
REXCVAR_DEFINE_BOOL(rb2_park_early, true, "Debug", "Rollback (rollback3): the render loop is asked to park before the frame log of a rewind frame (shorter waits)");
REXCVAR_DEFINE_INT32(rb2_park_wait_ms, 40, "Debug", "Rollback (rollback3): how long a rewind waits for the render loop to park (then it goes on)");
struct RenderPark {
  bool on = false;
  RenderPark() {
    if (!REXCVAR_GET(rb2_park_render)) return;
    on = true;
    g_park_req.store(1);
    const auto t0 = Clock::now();
    while (!g_parked.load() && Ms(t0, Clock::now()) < REXCVAR_GET(rb2_park_wait_ms)) std::this_thread::yield();
    if (g_parked.load()) ++g_park_ok; else ++g_park_timeouts;
    const double w = Ms(t0, Clock::now());
    g_park_wait_ms_sum += w;
    g_park_wait_ms_peak = std::max(g_park_wait_ms_peak, w);
  }
  ~RenderPark() {
    if (on) g_park_req.store(0);
  }
};
struct RenderPause {
  std::vector<HANDLE> held;
  RenderPause() {
    if (!REXCVAR_GET(rb2_pause_render)) return;
    if (g_render_tids.empty()) FindRenderThreads();
    for (DWORD tid : g_render_tids) {
      if (HANDLE th = OpenThread(THREAD_SUSPEND_RESUME, FALSE, tid)) {
        SuspendThread(th);
        held.push_back(th);
      }
    }
  }
  ~RenderPause() {
    for (HANDLE th : held) {
      ResumeThread(th);
      CloseHandle(th);
    }
  }
};
bool TrkRewind(rex::memory::Memory* m, PPCContext& ctx, uint8_t* base, int n, bool verify = true) {
  const int64_t now = s.frame;
  if (t.logs.size() < size_t(n) || t.logs.back().frame != now - 1 || t.logs[t.logs.size() - n].frame != now - n) {
    ++t.refused_nologs;
    if (t.refused_nologs <= 20) Log(fmt::format("rewind REFUSED at frame {} depth {}: logs {} (newest {})", now, n, t.logs.size(),
                                                t.logs.empty() ? -1 : t.logs.back().frame));
    return false;
  }
  for (size_t q = t.logs.size() - n; q < t.logs.size(); ++q) {
    if (!t.logs[q].quiet) { ++t.refused_noisy; return false; }  // a log taken with workers busy: images may be torn
  }
  if (now - n <= g_thread_created_frame.load()) { ++t.refused_thread; return false; }
  for (int64_t f = now - n; f < now; ++f) {
    if (g_cine_frame[((f % kRing) + kRing) % kRing] == f) { ++g_cine_rewinds; break; }
  }
  const double wait = t.last_wait;
  if (verify) {  // the original timeline's state now (sync test only)
    t.full.resize(t.shadow.size());
    for (size_t i = 0; i < s.host.size(); ++i) std::memcpy(t.full.data() + i * kPage, s.host[i], kPage);
  }
  std::optional<RenderPark> render_park;  // rb2_park_render: the render loop thread waits at its loop head (restore only)
  render_park.emplace();
  // the GPU's interrupt callbacks (guest code on the GPU worker / vblank threads) wait while memory is put back
  // (MEASURED c2: one ran on half-restored state and called a NULL function pointer)
  std::optional<std::unique_lock<std::recursive_mutex>> interrupt_gate;
  if (REXCVAR_GET(rb2_interrupt_gate)) interrupt_gate.emplace(rex::system::InterruptGate());
  RenderPause render_pause;  // rb2_pause_render (test)
  const auto a = Clock::now();
  // Guest locks (critical sections, spin locks) keep their live bytes: the threads holding / waiting on
  // them are not rewound (T6 rewrites its live kernel objects the same way).
  std::vector<std::pair<uint32_t, uint32_t>> locks;
  std::vector<uint8_t> lock_bytes;
  if (REXCVAR_GET(rb2_keep_locks)) {
    rex::system::GetGuestLocks(locks);
    lock_bytes.resize(locks.size() * 32);
    for (size_t q = 0; q < locks.size(); ++q) {
      std::memcpy(lock_bytes.data() + q * 32, m->TranslateVirtual<const uint8_t*>(locks[q].first), std::min<uint32_t>(locks[q].second, 32));
    }
  }
  size_t restored = 0;
  std::optional<PoolHold> hold;
  hold.emplace(m);
  // rollback3 (T6's quiesce: "a guest vblank callback mid-load froze the game"): the runtime's global critical
  // region held while guest memory is put back - no interrupt / vblank callback or kernel object call runs on
  // half-restored memory
  std::unique_lock<std::recursive_mutex> global_lock;
  if (REXCVAR_GET(rb2_global_lock)) global_lock = rex::thread::global_critical_region::AcquireDirect();
  // ... and the node allocator's SLAB: its free list is intrusive (each free node's next pointer lives in the node),
  // so with the allocator's header live but the nodes restored, a rewind could put old job data into a free node's
  // next field (h1 / h7 hangs in the allocator, h5 / h9 "call to invalid function 0x1A200154" from a job node at
  // 0xDD948F80 - Frieza vs Cell). Slab = [+0x2C, + capacity (+0x24) x node size (+0x34)), read live.
  uint32_t node_slab_lo = 0, node_slab_hi = 0;
  if (REXCVAR_GET(rb2_pool_live)) {
    const uint32_t nb = U32(m, 0x8438D8ACu), nn = U32(m, 0x8438D8A4u), ns = U32(m, 0x8438D8B4u);
    if (nb >= 0x40000000u && nn && nn <= 0x10000u && ns >= 0x10u && ns <= 0x1000u && uint64_t(nn) * ns <= 0x1000000u) {
      node_slab_lo = nb & ~0xFFFu;
      node_slab_hi = uint32_t((uint64_t(nb) + uint64_t(nn) * ns + 0xFFFu) & ~uint64_t(0xFFFu));
    }
  }
  // Foreign blocks of ANY frame being undone stay live in EVERY log's restore (an older log's bytes
  // would otherwise bring back a block a newer frame's foreign write had moved on).
  std::unordered_map<uint32_t, std::array<uint8_t, 32>> funion;
  for (const Undo& u : t.logs) {
    if (u.frame < now - n) continue;
    for (size_t k = 0; k < u.pages.size() && u.fmask.size() >= (k + 1) * 32; ++k) {
      const uint8_t* fm = u.fmask.data() + k * 32;
      bool anyb = false;
      for (int q = 0; q < 32; ++q) anyb |= fm[q] != 0;
      if (!anyb) continue;
      auto& un = funion[u.pages[k]];
      for (int q = 0; q < 32; ++q) un[q] |= fm[q];
    }
  }
  // Guest lock bytes (critical sections, spin locks: the SDK's registry) are never written by a restore:
  // other threads (the CRI ones) take and release them at any moment, also between the last log and this
  // restore. MEASURED on32: such a lock op was undone (count / owner torn) -> CRI deadlock, 3 threads
  // waiting in RtlEnterCriticalSection. They belong to every timeline (as in T6, which keeps its live
  // kernel objects).
  // Only critical sections (28 bytes inside one page) that are one in BOTH timelines - the live bytes and the
  // image restored look like a critical section (dispatcher type 1, sane count / recursion): a registered
  // address can hold other data by now (an object freed and its memory reused: MEASURED on36, keeping such
  // bytes live desynced the fight). Spin locks (4 bytes, any value) are left to the foreign marks.
  auto looks_cs = [](const uint8_t* p) {
    if (p[0] != 1) return false;
    int32_t count;
    std::memcpy(&count, p + 16, 4);  // host order (the SDK's atomic ops)
    const uint32_t rec = rex::memory::load_and_swap<uint32_t>(p + 20);
    const uint32_t owner = rex::memory::load_and_swap<uint32_t>(p + 24);
    if (count == -1) return rec == 0 && owner == 0;  // free
    return count >= 0 && count < 256 && rec < 256;    // held / being taken (owner set a moment later)
  };
  std::unordered_map<uint32_t, std::vector<uint16_t>> lock_spans;  // guest page -> offsets of live critical sections
  if (REXCVAR_GET(rb2_lock_skip)) {
    std::vector<std::pair<uint32_t, uint32_t>> lockv;
    rex::system::GetGuestLocks(lockv);
    for (const auto& [ga, len] : lockv) {
      if (len != 28 || (ga & 0xFFF) + 28 > 0x1000) continue;
      if (!looks_cs(m->TranslateVirtual<const uint8_t*>(ga))) continue;
      lock_spans[ga >> 12].push_back(uint16_t(ga & 0xFFF));
    }
  }
  // test cvars: hex ranges "a-b,c-d" - rb2_restore_range = only these pages are restored, rb2_restore_skip = these not
  auto hex_ranges = [](const std::string& spec) {
    std::vector<std::pair<uint32_t, uint32_t>> out;
    size_t q = 0;
    while (q < spec.size()) {
      size_t e = spec.find(',', q);
      if (e == std::string::npos) e = spec.size();
      const std::string item = spec.substr(q, e - q);
      const size_t d = item.find('-');
      if (d != std::string::npos)
        out.emplace_back(uint32_t(std::strtoul(item.c_str(), nullptr, 16)), uint32_t(std::strtoul(item.c_str() + d + 1, nullptr, 16)));
      q = e + 1;
    }
    return out;
  };
  const auto live_ranges = hex_ranges(REXCVAR_GET(rb2_live_ranges));
  const auto only_ranges = hex_ranges(REXCVAR_GET(rb2_restore_range));
  const auto skip_ranges = hex_ranges(REXCVAR_GET(rb2_restore_skip));
  auto in_ranges = [](const std::vector<std::pair<uint32_t, uint32_t>>& r, uint32_t a) {
    for (const auto& [lo, hi] : r) if (a >= lo && a < hi) return true;
    return false;
  };
  // test: the job pool's 64-bit fence (pool+16, waited on by the draw worker in sub_820F8660) keeps its live value
  uint8_t fence_live[8];
  std::memcpy(fence_live, m->TranslateVirtual<const uint8_t*>(0x8437F910), 8);
  std::vector<uint32_t> restored_pages;  // rb2_log_restored
  while (!t.logs.empty() && t.logs.back().frame >= now - n) {
    Undo& u = t.logs.back();
    for (size_t k = u.pages.size(); k-- > 0;) {
      const uint32_t i = u.pages[k];
      // rb2_pool_live: the job pool (0x8437F000-0x84388000, nodes included) and its node allocator (page 0x8438D000)
      // are never restored. The render side (render loop, GPU interrupt callbacks) pushes / allocates there too:
      // put back, they handed out nodes still in use (MEASURED e1 / e2 / d1: the GPU worker wrote through a free
      // list head pointing into code, called function 0x100 / 0x1A200152). At a rewind the pool is quiet (no job
      // queued or running), so its live state is a valid one; the resim pushes its jobs again on top.
      if (REXCVAR_GET(rb2_pool_live) && ((s.addr[i] >= 0x8437F000u && s.addr[i] < 0x84388000u) ||
                                         (s.addr[i] >= 0x8438D000u && s.addr[i] < 0x8438E000u) ||
                                         (s.addr[i] >= node_slab_lo && s.addr[i] < node_slab_hi))) continue;
      if (!only_ranges.empty() && !in_ranges(only_ranges, s.addr[i])) continue;  // test: only these ranges are restored
      if (in_ranges(skip_ranges, s.addr[i])) continue;                         // test: these ranges are not restored
      const uint8_t* old = u.bytes.data() + k * kPage;
      if (rb2_watch_size && rb2_watch_lo - s.addr[i] < kPage) {  // diagnostics: what a restore puts back over the store watch
        const uint32_t off = (rb2_watch_lo - s.addr[i]) & ~15u;
        const uint8_t* now_b = s.host[i] + off;
        const uint8_t fm = rb2_ffine[(size_t(s.addr[i] + off) >> 7)], fb = uint8_t(1u << (((s.addr[i] + off) >> 4) & 7));
        const auto fu2 = funion.find(i);
        const bool kept = (fm & fb) || (fu2 != funion.end() && (fu2->second[off >> 7] & fb));
        Log(fmt::format("restore watch: page {:08X} from log of frame {} (now {}): live {:08X}{:08X} -> image {:08X}{:08X}{}", s.addr[i], u.frame, now,
                        rex::memory::load_and_swap<uint32_t>(now_b), rex::memory::load_and_swap<uint32_t>(now_b + 4),
                        rex::memory::load_and_swap<uint32_t>(old + off), rex::memory::load_and_swap<uint32_t>(old + off + 4),
                        kept ? " (block kept live: foreign)" : ""));
      }
      // 16-byte blocks the sound library wrote keep their live bytes (its own timeline), and (rollback3)
      // so do the blocks other threads / host file reads wrote in that frame - or since the last log
      // (rb2_ffine now: written by another thread while this frame ran, until this very copy)
      const uint8_t* sfine = &rb2_sfine[size_t(s.addr[i]) >> 7];
      const uint8_t* fnow = &rb2_ffine[size_t(s.addr[i]) >> 7];
      const uint8_t* fprev = &rb2_ffine_prev[size_t(s.addr[i]) >> 7];
      uint8_t* mfr = &rb2_mfine[size_t(s.addr[i]) >> 7];
      uint8_t lostr[32] = {};
      if (REXCVAR_GET(rb2_lost_marks)) {  // changed since the newest snapshot, no mark: a foreign store that landed late
        const uint8_t* shp = t.shadow.data() + size_t(i) * kPage;
        for (int blk = 0; blk < 256; ++blk) {
          const uint8_t bm = uint8_t(1u << (blk & 7));
          if ((fnow[blk >> 3] & bm) || (mfr[blk >> 3] & bm) || (REXCVAR_GET(rb2_lost_gate) && !(fprev[blk >> 3] & bm))) continue;
          if (std::memcmp(shp + blk * 16, s.host[i] + blk * 16, 16) != 0) { lostr[blk >> 3] |= bm; ++g_lost_mark_blocks; }
        }
      }
      const auto fu = funion.find(i);
      const uint8_t* ffine = fu != funion.end() ? fu->second.data() : nullptr;
      uint8_t keepmask[32];
      bool any = false, anyf = false;
      // the job pool is one piece of frame state: no live blocks in it (the CRI library pushes jobs there
      // too - its 16-byte blocks were kept live by the sound map and tore the queue: crash at the first rewind)
      const bool pool_page = FrameRange(s.addr[i]) && REXCVAR_GET(rb2_hold_pool);
      // rb2_effects_full: the effects pool's linked lists are written by the main thread AND the render loop thread;
      // restored half (main blocks back, render-thread blocks live) they became cycles - the render loop spun on them
      // forever (the screen froze at the first rewinds). The whole pool goes back; the render threads are paused
      // meanwhile (rb2_pause_render) and redo their part.
      const bool effects_page = REXCVAR_GET(rb2_effects_full) && s.addr[i] >= 0x840D6000u && s.addr[i] < 0x84141000u;
      const bool foreign_on = REXCVAR_GET(rb2_foreign_fine) && !pool_page && !effects_page;
      for (int q = 0; q < 32; ++q) {
        const uint8_t sb = (REXCVAR_GET(rb2_sound_fine) && !pool_page) ? sfine[q] : 0;
        const uint8_t fb = foreign_on ? uint8_t((ffine ? ffine[q] : 0) | fnow[q] | (REXCVAR_GET(rb2_ffine_prev_on) ? fprev[q] : 0) | lostr[q]) : 0;
        keepmask[q] = sb | fb;
        any |= keepmask[q] != 0;
        anyf |= fb != 0;
      }
      // rb2_live_ranges: 16-byte blocks that are never restored (render hand-off state shared with the draw worker)
      for (const auto& [lo, hi] : live_ranges) {
        const uint64_t page = s.addr[i], page_end = uint64_t(s.addr[i]) + kPage;
        if (hi <= page || lo >= page_end) continue;
        const uint32_t b0 = lo > page ? uint32_t((lo - page) >> 4) : 0, b1 = uint32_t(std::min<uint64_t>(page_end, hi) - page + 15) >> 4;
        for (uint32_t b = b0; b < b1 && b < 256; ++b) keepmask[b >> 3] |= uint8_t(1u << (b & 7));
        any = true;
      }
      uint16_t spans[16];
      int nspans = 0;
      if (!lock_spans.empty()) {
        const auto ls = lock_spans.find(s.addr[i] >> 12);
        if (ls != lock_spans.end()) {
          for (const uint16_t o : ls->second) {
            if (nspans < 16 && looks_cs(old + o)) spans[nspans++] = o;
          }
        }
      }
      if (!any && !nspans) {
        std::memcpy(s.host[i], old, kPage);
      } else {
        uint8_t lockblk[32] = {};
        for (int k = 0; k < nspans; ++k) {
          for (int b = spans[k] >> 4; b <= (spans[k] + 27) >> 4; ++b) lockblk[b >> 3] |= uint8_t(1u << (b & 7));
        }
        for (int blk = 0; blk < 256; ++blk) {
          if ((keepmask[blk >> 3] >> (blk & 7)) & 1) continue;
          if (!((lockblk[blk >> 3] >> (blk & 7)) & 1)) {
            std::memcpy(s.host[i] + blk * 16, old + blk * 16, 16);
            continue;
          }
          for (int q = blk * 16; q < blk * 16 + 16; ++q) {  // byte by byte around the lock bytes
            bool in_lock = false;
            for (int k = 0; k < nspans; ++k) in_lock |= q >= spans[k] && q < spans[k] + 28;
            if (!in_lock) s.host[i][q] = old[q];
          }
          ++t.lock_blocks_skipped;
        }
        ++t.fine_restores;
        if (anyf) ++t.foreign_fine_restores;
      }
      std::memcpy(t.shadow.data() + size_t(i) * kPage, s.host[i], kPage);
      std::memset(mfr, 0, 32);  // the main writes since the snapshot were just undone
      if (REXCVAR_GET(rb2_log_restored) && t.rewinds + t.online_rewinds < 4) restored_pages.push_back(s.addr[i]);
      ++restored;
    }
    t.logs.pop_back();
  }
  if (!restored_pages.empty()) {  // the restored pages as ranges
    std::sort(restored_pages.begin(), restored_pages.end());
    restored_pages.erase(std::unique(restored_pages.begin(), restored_pages.end()), restored_pages.end());
    std::string list;
    for (size_t a = 0; a < restored_pages.size();) {
      size_t b = a;
      while (b + 1 < restored_pages.size() && restored_pages[b + 1] == restored_pages[b] + kPage) ++b;
      list += b > a ? fmt::format(" {:08X}-{:08X}", restored_pages[a], restored_pages[b] + kPage) : fmt::format(" {:08X}", restored_pages[a]);
      a = b + 1;
    }
    Log(fmt::format("restored pages ({}):{}", restored_pages.size(), list));
  }
  size_t kept = 0;
  for (size_t q = 0; q < locks.size(); ++q) {
    const uint32_t ga = locks[q].first, len = std::min<uint32_t>(locks[q].second, 32);
    // Only critical sections that are one in both timelines (dispatcher type 1 at +0, live and
    // restored): a noted address can hold other data by now (freed and reused).
    uint8_t* host = m->TranslateVirtual<uint8_t*>(ga);
    const uint8_t* live = lock_bytes.data() + q * 32;
    if (len != 28 || live[0] != 1 || host[0] != 1 || std::memcmp(host, live, len) == 0) continue;
    ++kept;
    std::memcpy(host, live, len);
    // the shadow (state at the frame start) takes the live bytes too
    const auto it = std::upper_bound(s.ranges.begin(), s.ranges.end(), ga, [](uint32_t v, const Range& r) { return v < r.address; });
    if (it != s.ranges.begin()) {
      const Range& r = *(it - 1);
      if (ga >= r.address && ga + len <= r.address + r.size) std::memcpy(t.shadow.data() + r.offset + (ga - r.address), lock_bytes.data() + q * 32, len);
    }
  }
  t.locks_kept += kept;
  if (REXCVAR_GET(rb2_fence_live)) {
    std::memcpy(m->TranslateVirtual<uint8_t*>(0x8437F910), fence_live, 8);
    const auto it = std::upper_bound(s.ranges.begin(), s.ranges.end(), 0x8437F910u, [](uint32_t v, const Range& r) { return v < r.address; });
    if (it != s.ranges.begin()) {
      const Range& r = *(it - 1);
      if (0x8437F910u >= r.address && 0x8437F918u <= r.address + r.size) std::memcpy(t.shadow.data() + r.offset + (0x8437F910u - r.address), fence_live, 8);
    }
  }
  if (global_lock.owns_lock()) global_lock.unlock();
  hold.reset();
  interrupt_gate.reset();
  render_park.reset();  // the resim runs with the render loop going again (the effects code waits on it)
  if (now < REXCVAR_GET(rb2_pool_trace) || t.rewinds + t.online_rewinds < 2) PoolDump("restored");
  g_cosmetic_main = t.cosmetic[(now - n) % kRing];
  const double restore = Ms(a, Clock::now());
  t.restore_ms += restore;
  double peak = 0, sum = 0;
  cri::g_now = now;
  for (int k = 0; k < n; ++k) {
    const int64_t f = now - n + k;
    g_cur_frame = f;
    {
      std::lock_guard<std::mutex> lock(g_rng_mutex);
      g_rng_trk_was[f % kRing] = std::move(g_rng_trk[f % kRing]);
      g_rng_trk[f % kRing].clear();
    }
    const auto& st = t.stacks[f % kRing];
    if (REXCVAR_GET(rb2_stack) > 0 && st.size() && t.stack_sp[f % kRing] == ctx.r1.u32) {
      std::memcpy(m->TranslateVirtual<uint8_t*>(ctx.r1.u32 - uint32_t(st.size())), st.data(), st.size());
    }
    const auto b = Clock::now();
    cri::Begin(f, true);
    rb2_d3d_watch = 1;
    ResimFrame(m, ctx, base);
    rb2_d3d_watch = 0;
    if (now < REXCVAR_GET(rb2_pool_trace) || t.rewinds + t.online_rewinds < 2) PoolDump("resim-end");
    t.wait_ms += WaitWorkers(m, 20);
    if (now < REXCVAR_GET(rb2_pool_trace)) PoolTrace(m, "resim-idle", f);  // the jobs this resim frame started finish before the next one
    if (REXCVAR_GET(rb2_resim_sleep_us) > 0) {
      const auto until = Clock::now() + std::chrono::microseconds(REXCVAR_GET(rb2_resim_sleep_us));
      while (Clock::now() < until) std::this_thread::yield();
    }
    const double ms = Ms(b, Clock::now());
    peak = std::max(peak, ms);
    sum += ms;
    TrkEndFrame(now - n + k, true);
    t.cosmetic[(now - n + k + 1) % kRing] = g_cosmetic_main;
  }
  if (!verify) {  // sound settlement: the last resimulated frame's record closes; ghosts fade out
    cri::Commit();
    cri::Settle(m, ctx, base, now);
  }
  t.resim_ms += sum;
  t.resim_frames += n;
  t.resim_peak = std::max(t.resim_peak, peak);
  g_cur_frame = now;
  if (!verify) {
    ++t.online_rewinds;
    t.online_depth_sum += n;
    t.online_max_depth = std::max(t.online_max_depth, n);
    t.online_cost_ms += restore + sum;
    t.online_cost_peak = std::max(t.online_cost_peak, restore + sum);
    return true;
  }
  // compare with the original timeline
  size_t bad_main = 0, bad_other = 0;
  std::string detail;
  int shown = 0;
  for (size_t i = 0; i < s.host.size(); ++i) {
    if (s.excluded[i]) continue;
    const uint8_t* want = t.full.data() + i * kPage;
    if (std::memcmp(s.host[i], want, kPage) == 0) continue;
    if (!t.ever_main[i]) { ++bad_other; continue; }
    ++bad_main;
    if (REXCVAR_GET(rb2_log_diffs) && t.rewinds < 3) {  // every differing 8-byte word of the first rewinds
      const uint64_t* l = reinterpret_cast<const uint64_t*>(s.host[i]);
      const uint64_t* w = reinterpret_cast<const uint64_t*>(want);
      std::string words;
      int nw = 0;
      for (uint32_t q = 0; q < kPage / 8 && nw < 24; ++q) {
        if (l[q] != w[q]) {
          words += fmt::format(" {:03X}:{:016X}->{:016X}", q * 8, __builtin_bswap64(w[q]), __builtin_bswap64(l[q]));
          ++nw;
        }
      }
      Log(fmt::format("diff page {:08X} (bits ever-main){}", s.addr[i], words));
    }
    if (shown < 10) {
      const uint64_t* l = reinterpret_cast<const uint64_t*>(s.host[i]);
      const uint64_t* w = reinterpret_cast<const uint64_t*>(want);
      uint32_t first = 0, words = 0;
      for (uint32_t q = 0; q < kPage / 8; ++q) {
        if (l[q] != w[q]) { if (!words) first = q; ++words; }
      }
      detail += fmt::format(" {:08X}({}w {:016X}->{:016X})", s.addr[i] + first * 8, words, __builtin_bswap64(w[first]),
                            __builtin_bswap64(l[first]));
      ++shown;
    }
  }
  std::string fp = "fight state SAME";
  {
    const auto& want = t.blobs[now % kRing];
    if (t.blob_frame[now % kRing] == now && !want.empty()) {
      const auto got = FpBlob(m);
      if (got != want) { fp = "fight state DIFFERS: " + BlobDiff(want, got) + RngTrkDiff(now - n, now); ++t.fp_differ; } else ++t.fp_same;
    }
  }
  ++t.rewinds;
  (bad_main == 0 ? t.same : t.differ)++;
  if (bad_main || t.rewinds <= 10 || t.rewinds % 20 == 0) {
    Log(fmt::format("track #{} frame {} rewind {}: {} | logic pages {} | restore {:.2f} ms ({} pages), resim {:.2f} ms ({:.2f}/frame, peak "
                    "{:.2f}), wait {:.2f} ms, other-thread pages differing {}{}",
                    t.rewinds, now, n, fp, bad_main ? fmt::format("DIFFER: {}", bad_main) : std::string("same"),
                    restore, restored, sum, sum / n, peak, wait, bad_other, detail));
  }
  if (t.rewinds % 20 == 0) {
    {
      std::string words;
      for (uint32_t o = 0; o < 0x60; o += 4) words += fmt::format(" {:08X}", U32(m, 0x8438D880u + o));
      Log(fmt::format("job nodes handed out {:08X}..{:08X}; allocator 8438D880:{}", g_node_lo.load(), g_node_hi.load(), words));
    }
    Log(fmt::format("track summary: {} rewinds, fight state {} same / {} differ, logic pages {} same / {} differ; per frame {:.1f} pages logged ({:.3f} ms), foreign {}; "
                    "restore {:.3f} ms, resim {:.3f} ms/frame (peak {:.2f}), waits {:.2f} ms/rewind, frame-start worker wait {:.3f} ms/frame; "
                    "render loop parked {} / timed out {} (wait {:.2f} ms avg / {:.1f} peak); rewinds through cinematic frames {}; logs with busy workers {} (rewinds refused {}, wait timeouts {}); guest threads created {} (rewinds refused across one {}, last start {:08X}); guest locks kept live {}, lock blocks restored around {}; pages with sound + game writes {} (fine restores {}); pages with foreign + frame writes {} (restores keeping foreign blocks {}), host file writes {} ({:.1f} MB); live frame work (A..F, main thread) {:.2f} ms avg, {:.2f} peak",
                    t.rewinds, t.fp_same, t.fp_differ, t.same, t.differ, double(t.logged) / std::max<int64_t>(t.frames, 1),
                    t.log_ms / std::max<int64_t>(t.frames, 1), t.foreign, t.restore_ms / t.rewinds,
                    t.resim_ms / std::max<int64_t>(t.resim_frames, 1), t.resim_peak, t.wait_ms / t.rewinds,
                    t.frame_wait_ms / std::max<int64_t>(t.frames, 1), g_park_ok.load(), g_park_timeouts.load(), g_park_wait_ms_sum / std::max<int64_t>(g_park_ok.load() + g_park_timeouts.load(), 1), g_park_wait_ms_peak, g_cine_rewinds, t.noisy_logs, t.refused_noisy, g_wait_timeouts + g_pool_hold_retries * 0, g_threads_created.load(), t.refused_thread, g_thread_last_start.load(), t.locks_kept, t.lock_blocks_skipped, t.mixed_pages, t.fine_restores,
                    t.foreign_mixed, t.foreign_fine_restores, t_host_writes.load(), double(t_host_write_bytes.load()) / 1048576.0,
                    g_frame_work_ms / std::max<int64_t>(g_frame_work_n, 1), g_frame_work_peak));
    Log(CriStats());
    rex::FlushLogging();
  }
  return true;
}

void TrkFrameA(rex::memory::Memory* m, PPCContext& ctx, uint8_t* base) {
  g_last_frame.store(Clock::now().time_since_epoch().count());
  struct StampA { ~StampA() { g_frame_a_time = Clock::now(); } } stamp_a;  // the frame's own work starts after the engine's
  g_main_thread = std::this_thread::get_id();
  rb2_tbit = 1;
  rb2_main = 1;
  g_phase = 'A';
  const uint32_t battle = U32(m, 0x841B5130);
  // Free fighting only: not in cinematics (supers, Drama Pieces, transformations: battle+574 bit 2) -
  // those run without rollback (lockstep online), as in T6 / TTT2.
  const bool cinematic = battle && REXCVAR_GET(rb2_no_cinematics) &&
                         (U16(m, battle + 574) & 2) != 0;
  // rb2_phases: battle phases the engine runs in (bit = phase: 1 READY?, 2 FIGHT!, 3 round end / KO)
  const uint32_t phase = battle ? U32(m, 0x841B5228) : 0;
  bool fighting = battle && phase < 8 && ((REXCVAR_GET(rb2_phases) >> phase) & 1) && !cinematic &&
                  (!BurstLimitRollback2Online() || BurstLimitOnlineEngineDelayOk());  // a long cinematic's delay must be gone first
  const bool cine_now = battle && (U16(m, battle + 574) & 2) != 0;
  g_cine_frame[((s.frame % kRing) + kRing) % kRing] = cine_now ? s.frame : -1;
  {  // diagnostics: supers / Drama Pieces / KO cinematics seen live (no rewinds into them)
    static bool was_cine = false;
    static int64_t cine_from = 0, cine_count = 0;
    if (cine_now != was_cine) {
      if (cine_now) {
        cine_from = s.frame;
        ++cine_count;
        Log(fmt::format("cinematic #{} starts at engine frame {}", cine_count, s.frame));
      } else {
        Log(fmt::format("cinematic #{} ends at engine frame {} ({} frames)", cine_count, s.frame, s.frame - cine_from));
      }
      was_cine = cine_now;
    }
  }
  // Online: a cinematic / KO / round end reached on a frame whose remote input is still a prediction must stay
  // rewindable until that prediction settles (the KO may not happen in the corrected timeline). MEASURED on2/on3:
  // one PC reached KO on a mispredicted frame, the engine paused and dropped its logs, the rewind was impossible
  // -> desync at the round end. So the engine keeps logging (drains) while predictions are unsettled, up to 30
  // frames (the netinput side no longer predicts: the status words differ there, the game waits).
  static int drain = 0;
  g_draining = false;
  // ... and for a grace of 40 frames in any case: the other PC may still send a corrected entry (its own resim
  // changed its status words, e.g. found the KO) up to an RTT later (on8).
  if (!fighting && s.active && battle == s.battle && BurstLimitRollback2Online() &&
      (drain < 40 || BurstLimitOnlineOldestUnsettled() >= 0) && drain < 90) {
    g_draining = true;  // no new predictions now (on5: the status words stay equal in some cinematics)
    if (drain == 0) Log(fmt::format("track: draining at frame {} (fight paused with unsettled predictions)", s.frame));
    ++drain;
    ++t.drain_frames;
    fighting = true;
  } else if (fighting) {
    drain = 0;
  }
  if (!t.ready) {
    rb2_cri_on = 0;
    if (!fighting) return;
    Setup(m);
    t.gp.resize(s.host.size());
    for (size_t i = 0; i < s.host.size(); ++i) t.gp[i] = s.addr[i] >> 12;
    {  // the tracked view of each physical page (see rb2_phys_canon)
      static std::vector<uint32_t> canon;
      canon.assign(0x20000, 0);
      size_t views = 0, doubles = 0;
      for (uint32_t g : t.gp) {
        if (g < 0xA0000) continue;
        const uint32_t ph = g < 0xC0000 ? g - 0xA0000 : g < 0xE0000 ? g - 0xC0000 : g - 0xE0000 + 1;
        if (ph >= 0x20000) continue;
        if (!canon[ph]) { canon[ph] = g; ++views; } else if (canon[ph] != g) { ++doubles; }
      }
      rb2_phys_canon = REXCVAR_GET(rb2_phys_canon_on) ? canon.data() : nullptr;
      Log(fmt::format("physical pages tracked {} ({} tracked in two views); writes through other views are noted on them: {}", views,
                      doubles, rb2_phys_canon ? "yes" : "no"));
    }
    t.shadow.resize(s.host.size() * size_t(kPage));
    // rollback3: the first "before" images of every page come from this copy - taken with the render
    // thread / workers idle and the job pool empty and locked (a copy mid-job gave torn pool images:
    // job pool crash at the very first rewind, g13)
    WaitRender(m, 50);
    WaitWorkers(m, 100);
    rb2_track_on = 1;  // from now on every guest store is marked (patch v9)
    {
      PoolHold hold(m);
      for (uint32_t g : t.gp) rb2_dirty[g] = 0;  // before the copy: a write during it stays marked
      for (size_t i = 0; i < s.host.size(); ++i) std::memcpy(t.shadow.data() + i * kPage, s.host[i], kPage);
    }
    t.ever_main.assign(s.host.size(), 0);
    t.ready = true;
    s.stage = 2;
    s.frame = 0;
    g_thread_created_frame.store(-1);
    g_cur_frame_for_hook.store(0);
    s.battle = battle;
    s.active = true;
    Log(fmt::format("track: on, {} pages ({:.1f} MB) tracked", s.host.size(), double(t.shadow.size()) / 1048576.0));
    if (REXCVAR_GET(rb2_watch)) {  // fighter 0's +0x50..+0x90 (its facing and position)
      const uint32_t entry = U32(m, 0x84239C10);
      const uint32_t fighter = Committed(m, entry + 44, 4) ? U32(m, entry + 44) : 0;
      rb2_watch_lo = fighter + 0x50;
      rb2_watch_size = fighter ? 0x40 : 0;
      Log(fmt::format("watch: fighter 0 at {:08X}", fighter));
    }
    if (!REXCVAR_GET(rb2_watch_range).empty()) {  // rollback3 diagnostics: who writes this range
      unsigned lo = 0, hi = 0;
      if (std::sscanf(REXCVAR_GET(rb2_watch_range).c_str(), "%x-%x", &lo, &hi) == 2 && hi > lo) {
        rb2_watch_lo = lo;
        rb2_watch_size = hi - lo;
        Log(fmt::format("watch: {:08X}-{:08X}", lo, hi));
      }
    }
    RngIn(m);
    return;
  }
  const bool was = s.active;
  s.active = fighting && battle == s.battle;
  if (s.active != was) {
    Log(fmt::format("track: fight {} at frame {} (battle {:08X} phase {})", s.active ? "resumed" : "paused", s.frame, battle,
                    battle ? U32(m, 0x841B5228) : 0));
    rex::FlushLogging();
  }
  if (!s.active) {
    if (battle && battle != s.battle) s.battle = battle;
    if (was) t.logs.clear();
    if (rb2_cri_on) cri::Reset();
    rb2_cri_on = 0;
    return;
  }
  if (!was) cri::Reset();  // no record from before the pause can be matched
  if (s.frame % 600 == 0) CacheThreadIds();  // for the raw hang dump (new guest threads)
  if (s.frame % 1800 == 0) {
    Log(fmt::format("track: heartbeat frame {}, {} rewinds, fight state {} same / {} differ; refused: busy workers / resource cache {}, across a new thread {} ({} threads made, last start {:08X}); resource cache calls main {} other {}, frames marked {}; alias pages written untracked A {} C {} E {} (with a tracked alias {} / {} / {}), tracked alias pairs dirty together {}; lost-mark blocks kept {}",
                    s.frame, t.rewinds, t.fp_same, t.fp_differ, t.refused_noisy, t.refused_thread, g_threads_created.load(), g_thread_last_start.load(),
                    g_res_calls_main.load(), g_res_calls_other.load(), g_res_logs_marked.load(), g_alias_untracked[0], g_alias_untracked[1],
                    g_alias_untracked[2], g_alias_untracked_tracked_alias[0], g_alias_untracked_tracked_alias[1], g_alias_untracked_tracked_alias[2],
                    g_alias_both_tracked_dirty, g_lost_mark_blocks));
    rex::FlushLogging();
  }
  const int every = std::max(2, REXCVAR_GET(rb2_every));
  const int depth_max = std::clamp(REXCVAR_GET(rb2_sync), 1, kRing - 2);
  const bool due = s.frame > SettleFrames() + kRing && s.frame % every == 0;
  // Quiesce contract (rollback3): a rewind requested by the online / loopback path is known before the
  // frame's log, so the render thread and every worker are idle BEFORE the log too (rollback2 waited only
  // after it: a frame logged with job workers mid-write was then restored half-written - on1's job pool crash).
  const bool pending = (REXCVAR_GET(rb2_loop) > 0 || BurstLimitRollback2Online()) && BurstLimitRb2LoopPeekRewind() >= 0;
  if ((due || pending) && REXCVAR_GET(rb2_rewind_sleep) > 0)  // test: give the draw worker time to finish its frame
    std::this_thread::sleep_for(std::chrono::milliseconds(REXCVAR_GET(rb2_rewind_sleep)));
  // the render loop is asked to park now (the rewind comes right after this frame's log): it reaches its loop head
  // while the waits and the log run, so the rewind itself waits less (RenderPark, rb2_park_early)
  if ((due || pending) && REXCVAR_GET(rb2_park_render) && REXCVAR_GET(rb2_park_early)) g_park_req.store(1);
  if (due || pending) {  // the workers and the render thread idle before the frame's log and the rewind
    t.last_wait = WaitRender(m, 50) + WaitWorkers(m, 50);
    t.wait_ms += t.last_wait;
  } else {
    t.frame_wait_ms += WaitWorkers(m, 5);  // every frame's log at a quiet moment
  }
  if (s.frame < REXCVAR_GET(rb2_pool_trace)) PoolTrace(m, "live-log", s.frame);
  TrkEndFrame(s.frame - 1, was);
  t.blobs[s.frame % kRing] = FpBlob(m);
  t.cosmetic[s.frame % kRing] = g_cosmetic_main;
  if (REXCVAR_GET(rb2_stack) > 0) {  // what the frame finds below its stack pointer (leftovers it may read)
    const uint32_t size = uint32_t(REXCVAR_GET(rb2_stack));
    const uint32_t sp = ctx.r1.u32;
    auto& st = t.stacks[s.frame % kRing];
    st.resize(size);
    std::memcpy(st.data(), m->TranslateVirtual<const uint8_t*>(sp - size), size);
    t.stack_sp[s.frame % kRing] = sp;
  }
  t.blob_frame[s.frame % kRing] = s.frame;
  g_cur_frame = s.frame;
  {
    std::lock_guard<std::mutex> lock(g_rng_mutex);
    g_rng_trk[s.frame % kRing].clear();
  }
  if (REXCVAR_GET(rb2_loop) > 0 && (s.frame % 6) == 0) {  // player 1's scripted pad (moves, attacks, ki blasts)
    static const uint16_t kPattern[] = {0x0008, 0x0008, 0x4000, 0, 0x4000, 0x8000, 0, 0x2000, 0x0004, 0x4000,
                                        0x4000, 0x4000, 0x8000, 0x0001, 0x2000, 0x0002, 0x1000, 0, 0x0008, 0x0100};
    auto* runtime = rex::Runtime::instance();
    if (auto* input = runtime ? static_cast<rex::input::InputSystem*>(runtime->input_system()) : nullptr) {
      input->InjectButtons(0, kPattern[(s.frame / 6) % std::size(kPattern)], 80);
    }
  }
  if (REXCVAR_GET(rb2_loop) > 0 || BurstLimitRollback2Online()) {
    const int64_t to = BurstLimitRb2LoopTakeRewind();
    if (to >= 0) {
      const int n = int(s.frame - to);
      if (!verify_online_log_done(n, to)) {
      }
      if (to < SettleFrames()) {
        ++t.online_refused;  // into the frames logged while the engine settled (online: not before)
        if (t.online_refused <= 20) Log(fmt::format("rewind REFUSED (settling) at frame {} to {}", s.frame, to));
      } else if (n >= 1 && n <= kRing - 2) {
        t.last_wait = WaitRender(m, 50) + WaitWorkers(m, 50);
        TrkRewind(m, ctx, base, n, false);
      } else if (n > 0) {
        ++t.online_too_deep;
        if (t.online_too_deep <= 20) Log(fmt::format("rewind REFUSED (too deep) at frame {} to {} (depth {})", s.frame, to, n));
      }
    }
    if (s.frame % 600 == 0 && s.frame) {
      Log(fmt::format("loop: frame {}: {} rollbacks (through cinematic frames {}, render loop parked {} / timed out {}, park wait {:.2f} ms avg / {:.1f} peak), avg depth {:.2f}, max {}, too deep {}, refused (settling) {}, cost {:.2f} ms avg / {:.2f} peak; {}",
                      s.frame, t.online_rewinds, g_cine_rewinds, g_park_ok.load(), g_park_timeouts.load(), g_park_wait_ms_sum / std::max<int64_t>(g_park_ok.load() + g_park_timeouts.load(), 1), g_park_wait_ms_peak, double(t.online_depth_sum) / std::max<int64_t>(t.online_rewinds, 1),
                      t.online_max_depth, t.online_too_deep, t.online_refused, t.online_cost_ms / std::max<int64_t>(t.online_rewinds, 1),
                      t.online_cost_peak, BurstLimitRb2LoopStats()));
      Log(CriStats());
      rex::FlushLogging();
    }
  } else if (due) {
    TrkRewind(m, ctx, base, 1 + (t.next_depth++ % depth_max));
  }
  g_park_req.store(0);  // an early park request whose rewind was refused / not taken
  rb2_cri_on = 1;
  if (const uint32_t net = U32(m, 0x84255714)) {
    g_netobj_lo = net & ~0xFFFu;
    g_netobj_hi = (net + 0x1000 + 0xFFF) & ~0xFFFu;
  }
  if (const uint32_t mgr = U32(m, 0x841B5EA4)) {  // match input manager (2 x 784-byte player blocks + header)
    g_input_mgr_lo = mgr & ~0xFFFu;
    g_input_mgr_hi = (mgr + 2048 + 0xFFF) & ~0xFFFu;
  }
  g_cur_frame_for_hook.store(s.frame);
  if (REXCVAR_GET(rb2_d3d_log)) rb2_d3d_watch = 1;
  sync_frame_for_fault.store(s.frame);
  if (const int df = REXCVAR_GET(rb2_dump_frame); df > 0 && (s.frame == df || s.frame == df + 600)) {
    g_dump_request.store(true);
    DumpTasks(m);
    auto u64 = [&](uint32_t a) { return (uint64_t(U32(m, a)) << 32) | U32(m, a + 4); };
    {
      std::lock_guard<std::mutex> lock(g_job_class_mutex);
      std::string js;
      for (const auto& [k, v] : g_job_vt_stats) js += fmt::format(" {:08X}/{}x{}", k.first, int(k.second), v);
      Log(fmt::format("job pushes by vtable / class:{}", js));
    }
    Log(fmt::format("render hand-off: counter [82724010] {} target [82723FA8] {} | draw worker ready [8438ACA0] {} | pool awake [8437F908] {} fence [8437F910] {}",
                    u64(0x82724010), u64(0x82723FA8), u64(0x8438ACA0), U32(m, 0x8437F908), u64(0x8437F910)));
  }
  cri::Begin(s.frame, false);  // this live frame's CRI calls are recorded from here
  cri::g_outside = false;
  RngIn(m);
}
}  // namespace sync

// Mid-asm hook 0x82219938 (li r3,0 / bl sub_82123178): while the script runs, the frame's tasks run
// here in two passes - the update tasks (the fight's random number state), then the draw tasks (the
// cosmetic one) - and the game's own scheduler call is skipped (true).
REXCVAR_DEFINE_BOOL(rb2_handoff_live, false, "Debug", "Rollback (rollback3): the frame end task's writes (render hand-off) are foreign: kept live by a rewind");
bool BurstLimitRb2Scheduler() {
  const bool sync_on = Rb2EngineOn();
  if (!g_split && !sync_on) return false;
  auto* runtime = rex::Runtime::instance();
  auto* m = runtime ? runtime->memory() : nullptr;
  if (!m) return false;
  if (sync_on && U32(m, 0x84140E90) == 0) return false;
  // Only while the engine tracks frames: otherwise the game's own scheduler (MEASURED on27/on29: with the split
  // running while paused, the round-start handshake after a round end (phase 5) never came - the next round
  // never started; with rollback off it did).
  if (sync_on && !g_split && !(sync::s.stage == 2 && sync::s.active)) return false;
  PPCContext* ctx = rex::runtime::current_ppc_context();
  uint8_t* base = m->virtual_membase();
  const PPCContext saved = *ctx;
  if (sync_on) {
    const uint32_t list = SchedulerBegin(m);
    if (list) sync::RealUpdate(m, *ctx, base, list);
    sync::DrawBegin(m);
    if (list) {
      uint32_t prev = 0;
      int walked = 0;
      for (uint32_t task = U32(m, list + 32); task; task = U32(m, task + 40)) {
        if (++walked > 1024) break;
        const uint32_t prio = U32(m, task + 24), cb = U32(m, task + 32);
        if (!(prio >= 0x20000000 && cb != 0x8217E100)) continue;
        ctx->r3.u64 = list;
        ctx->r4.u64 = task;
        ctx->r5.u64 = prev;
        // the frame end task (prio 0xFFFFFFFF: the hand-off to the draw worker / GPU) is never run again by a
        // resim: its writes belong to every timeline (foreign, kept live by a rewind) - rewound, they took the
        // render hand-off n frames back and the screen froze at the first rewinds
        bool handoff = prio == 0xFFFFFFFF && REXCVAR_GET(rb2_handoff_live);
        // rb2_render_live: every draw task but the effects band (whose state the update needs and which a resim
        // runs again) renders only - its writes are foreign (kept live), and a resim doesn't run it
        if (REXCVAR_GET(rb2_render_live) && prio != 0xFFFFFFFF && !FxBandTask(prio, cb)) handoff = true;
        if (handoff) rb2_tbit = 2;
        sub_82122450(*ctx, base);
        if (handoff) rb2_tbit = 4;
        prev = task;
      }
      ctx->r3.u64 = list;
      sub_821228A8(*ctx, base);
    }
    sync::DrawEnd(m);
    *ctx = saved;
    return true;
  }
  const uint32_t list = SchedulerBegin(m);
  if (!list) return false;
  LinkAndRun(m, *ctx, base, list, false);
  // the draw pass: every other task (frame end included), cosmetic random numbers
  uint32_t prev = 0;
  int walked = 0;
  for (uint32_t task = U32(m, list + 32); task; task = U32(m, task + 40)) {
    if (++walked > 1024) break;
    const uint32_t prio = U32(m, task + 24), cb = U32(m, task + 32);
    if (!(prio >= 0x20000000 && cb != 0x8217E100)) continue;
    ctx->r3.u64 = list;
    ctx->r4.u64 = task;
    ctx->r5.u64 = prev;
    sub_82122450(*ctx, base);
    prev = task;
  }
  uint32_t fight = 0;
  RngToCosmetic(m, fight);
  ctx->r3.u64 = list;
  sub_821228A8(*ctx, base);
  RngToFight(m, fight);
  *ctx = saved;
  return true;
}

void BurstLimitRb2FrameA() {
  if (!Rb2EngineOn()) {  // a classic (lockstep) match after a rollback one: the stall watch has no frames to time
    g_last_frame.store(0);
    return;
  }
  auto* runtime = rex::Runtime::instance();
  auto* m = runtime ? runtime->memory() : nullptr;
  if (!m || U32(m, 0x84140E90) == 0) return;
  PPCContext* ctx = rex::runtime::current_ppc_context();
  const PPCContext saved = *ctx;
  if (REXCVAR_GET(rb2_track)) sync::TrkFrameA(m, *ctx, m->virtual_membase());
  else sync::FrameA(m, *ctx, m->virtual_membase());
  *ctx = saved;
}
// The game's job worker (sub_820F2618's loop) and draw worker (sub_820EA188) are frame threads: their
// writes are the frame's (bit 8), unlike the render, sound and loader threads (bit 2, never rewound
// with rb2_foreign).
void BurstLimitRb2Worker() { rb2_tbit = uint8_t(REXCVAR_GET(rb2_worker_tbit)); }
// The render loop thread (sub_82219360) reads the effects pool / render lists in its scene walk (821295A0); from
// the frame pacing call (82119D18) to the next loop head it doesn't. g_parked = "outside the scene walk": a rewind
// waits only until then (not for a whole loop turn), and the loop head holds the thread while a rewind runs.
REXCVAR_DEFINE_BOOL(rb2_park_safe_window, false, "Debug", "Rollback (rollback3) test: the render loop counts as parked from its frame pacing call to the next loop head (froze the screen: off)");
void BurstLimitRb2RenderSafe() {
  if (REXCVAR_GET(rb2_park_safe_window)) sync::g_parked.store(1);
}
void BurstLimitRb2RenderPark() {
  sync::g_parked.store(0);
  if (!sync::g_park_req.load()) return;
  sync::g_parked.store(1);
  while (sync::g_park_req.load()) std::this_thread::sleep_for(std::chrono::microseconds(100));
  sync::g_parked.store(0);
}
// rollback3: a job's writes belong to whoever pushed it. Jobs the main thread pushes (the fight's work, pushed again
// by a resim) are frame state (8); jobs the render side pushes (render thread 82219360, draw worker 820EA188 -
// never rewound) are foreign (2): rewound, their completions (e.g. the draw worker's ready flag 0x8438ACA0) were
// taken back and the render hand-off deadlocked (the screen froze at the first rewinds).
void NoteJobPush(uint32_t job, uint32_t vtable) {
  const uint8_t cls = (rb2_tbit == 1 || rb2_tbit == 4 || rb2_tbit == 8) ? 8 : 2;
  std::lock_guard<std::mutex> lock(g_job_class_mutex);
  g_job_class[job] = cls;
  ++g_job_vt_stats[{vtable, cls}];
}
REXCVAR_DEFINE_BOOL(rb2_job_class, false, "Debug", "Rollback (rollback3): job writes take the pusher's class (main thread = frame state, render side = foreign)");
void BurstLimitRb2WorkerJob(PPCRegister& r31) {
  uint8_t cls = uint8_t(REXCVAR_GET(rb2_worker_tbit));
  if (REXCVAR_GET(rb2_job_class)) {
    std::lock_guard<std::mutex> lock(g_job_class_mutex);
    const auto it = g_job_class.find(r31.u32);
    if (it != g_job_class.end() && it->second == 2) cls = 2;
  }
  rb2_tbit = cls;
}
// The draw worker renders what the draw pass built; neither is run again by a resim, so its writes (D3D state,
// the hand-off with the main thread) can't be frame state: MEASURED rnd4 - rewound, the main thread never handed
// it another frame (the draw worker waiting in sub_820F8660, no more swaps: the screen froze ~2 s into every
// rollback fight while the fight itself went on).
REXCVAR_DEFINE_INT32(rb2_draw_worker, 8, "Debug", "Rollback (rollback3): the draw worker's writes: 2 foreign (kept live by a rewind), 8 frame state (rewound)");
void BurstLimitRb2DrawWorker(PPCRegister& r27) {
  rb2_tbit = uint8_t(REXCVAR_GET(rb2_draw_worker) == 8 ? 8 : 2);
  static std::atomic<uint32_t> logged{0};
  if (logged.exchange(r27.u32) != r27.u32) REXLOG_INFO("[RB2] draw worker context {:08X}", r27.u32);  // diagnostics
}
REXCVAR_DEFINE_BOOL(rb2_worker_loop, false, "Debug", "Rollback (rollback3): the job pool worker loop's own writes (queue pops) are frame writes from the first pop on");
void BurstLimitRb2WorkerLoop() {
  if (REXCVAR_GET(rb2_worker_loop)) rb2_tbit = uint8_t(REXCVAR_GET(rb2_worker_tbit));
}

// The CRI server step (main loop step, skipped in resims): its CRI calls are "outside".
REX_EXTERN(__imp__sub_82119320);
REXCVAR_DEFINE_BOOL(rb2_cri_outside, true, "Debug", "Rollback (rollback3): CRI calls of the CRI server step are marked outside (never matched)");
extern "C" REX_FUNC(sub_82119320) {
  if (!REXCVAR_GET(rb2_cri_outside)) {
    __imp__sub_82119320(ctx, base);
    return;
  }
  const bool was = cri::g_outside;
  cri::g_outside = true;
  __imp__sub_82119320(ctx, base);
  cri::g_outside = was;
}
void BurstLimitOnlineLiveFrameEnd();  // burstlimit_netinput.cpp
// Presentation check (rollback3, "rule 0": a run only counts when the screen is alive): every 2 s, from its own
// thread, the main loop's frames, the guest's swaps and the host's presents in that window, and whether the
// rollback engine is tracking. A frozen screen = frames going on with swaps / presents at 0; a hang = frames at 0.
REXCVAR_DEFINE_BOOL(rb2_present_log, true, "Debug", "Rollback (rollback3): log main-loop frames / guest swaps / host presents every 2 s ([Present])");
std::atomic<uint64_t> g_main_frames{0};
void PresentMonitor() {
  uint64_t last_frames = 0, last_swaps = 0, last_presents = 0, last_vd = 0;
  for (;;) {
    std::this_thread::sleep_for(std::chrono::seconds(2));
    const uint64_t frames = g_main_frames.load(), swaps = rex::perf::GetGuestSwapCount(),
                   presents = rex::perf::GetHostPresentCount(), vd = rex::perf::GetGuestVdSwapCount();
    REXLOG_INFO("[Present] 2s: main frames {} guest swaps {} vdswaps {} host presents {} | totals {} / {} / {} | engine {}",
                frames - last_frames, swaps - last_swaps, vd - last_vd, presents - last_presents, frames, swaps, presents,
                sync::s.active ? "tracking" : (sync::s.stage == 2 ? "paused" : "off"));
    // a frozen screen while the engine runs (main frames on, guest swaps 0): every thread's guest call chain, twice
    // (offline stress r108_off1 froze at ~280 rewinds - the render loop thread stopped, the main loop went on)
    static int frozen_windows = 0, dumps = 0;
    if (frames - last_frames > 20 && swaps == last_swaps && sync::s.stage == 2) {
      if (++frozen_windows >= 2 && dumps < 2 && (frozen_windows % 5) == 2) {
        ++dumps;
        REXLOG_WARN("[Present] frozen screen ({} windows): thread dump {}", frozen_windows, dumps);
        g_dump_request.store(true);
      }
    } else {
      frozen_windows = 0;
    }
    last_frames = frames;
    last_swaps = swaps;
    last_presents = presents;
    last_vd = vd;
  }
}
void BurstLimitRb2FrameF() {
  g_main_frames.fetch_add(1, std::memory_order_relaxed);
  static std::once_flag present_once;
  if (REXCVAR_GET(rb2_present_log)) std::call_once(present_once, [] { std::thread(PresentMonitor).detach(); });
  cri::g_outside = true;  // frame end, vsync waits: not run again by a resim
  if (BurstLimitRollback2Online()) BurstLimitOnlineLiveFrameEnd();
  if (!Rb2EngineOn()) return;
  auto* runtime = rex::Runtime::instance();
  auto* m = runtime ? runtime->memory() : nullptr;
  if (!m) return;
  sync::FrameF(m);
}

void BurstLimitRb2Frame(rex::memory::Memory* memory) {
  const int start_after = REXCVAR_GET(rb2_auto);
  if (start_after <= 0 || g.stage == 3) return;
  const auto now = Clock::now();
  const uint32_t battle = U32(memory, 0x841B5130);
  const bool fighting = battle && U32(memory, 0x841B5228) >= 2;
  if (g.stage == 0) {
    if (battle != g.battle) { g.battle = battle; g.fight_ticks = 0; }
    if (!fighting || ++g.fight_ticks < start_after) return;
    g.ranges = WritableRanges(memory);
    uint64_t bytes[kClsCount] = {};
    size_t count[kClsCount] = {};
    for (const Range& r : g.ranges) { bytes[r.cls] += r.size; ++count[r.cls]; }
    for (int c = 0; c < kClsCount; ++c) {
      Log(fmt::format("region {}: {} ranges, {:.1f} MB", kClsNames[c], count[c], double(bytes[c]) / 1048576.0));
    }
    CaptureAll(memory, g.shadow);
    g.ever.assign(TotalSize(g.ranges) / kPage + 1, 0);
    g.stage = 1;
    g_dmask = std::strtoull(REXCVAR_GET(rb2_dmask).c_str(), nullptr, 0);
    g_cosmetic_rng = U32(memory, kRng);
    g_split = REXCVAR_GET(rb2_split);
    Log(fmt::format("scheduler split {}, draw mask {:#x}", g_split ? "on" : "off", g_dmask));
    g.last_hook = now;
    Log(fmt::format("measuring {} ticks", REXCVAR_GET(rb2_measure)));
    return;
  }
  if (!fighting) {
    Log("battle ended before the script finished");
    g_split = false;
    g.stage = 3;
    rex::FlushLogging();
    return;
  }
  if (g.stage == 1) {
    const double tick = Ms(g.last_hook, now);
    g.tick_ms += tick;
    g.tick_peak = std::max(g.tick_peak, tick);
    g.last_hook = now;
    ++g.measured;
    MeasureTick(memory);
    if (g.measured >= REXCVAR_GET(rb2_measure)) {
      MeasureReport();
      rex::FlushLogging();
      g.tests = ParseTests();
      g.stage = 2;
    }
    return;
  }
  TestsTick(memory);
}

// The game's LCG (sub_820E7020: x = x * 0x6C078965 + 12345, returns x >> 1 of 31 bits).
REX_EXTERN(__imp__sub_820E7020);
namespace {
}
extern "C" REX_FUNC(sub_820E7020) {
  using namespace sync;
  if (!Rb2EngineOn() || s.stage == 0 || (REXCVAR_GET(rb2_track) && !s.active && !g_in_resim)) {
    __imp__sub_820E7020(ctx, base);  // engine paused: the game's own random numbers, as without rollback
    return;
  }
  if (g_fight_window.load() && std::this_thread::get_id() == g_main_thread) {
    {
      std::lock_guard<std::mutex> lock(g_rng_mutex);
      auto& mp = g_in_resim ? g_rng_resim : g_rng_real[((s.frame % kRing) + kRing) % kRing];
      mp[g_in_resim ? uint32_t(ctx.lr) : (uint32_t(ctx.lr) ^ (uint32_t(uint8_t(g_phase)) << 0)) ]++;
      g_rng_trk[((g_cur_frame % kRing) + kRing) % kRing][uint32_t(ctx.lr)]++;
      if (!g_in_resim && REXCVAR_GET(rb2_trace) > 0) REXLOG_WARN("[RB2] rng real lr {:08X} phase {} task cb {:08X}", uint32_t(ctx.lr), g_phase, g_cur_cb);
    }
    __imp__sub_820E7020(ctx, base);
    return;
  }
  uint32_t& lcg = std::this_thread::get_id() == g_main_thread ? g_cosmetic_main : g_cosmetic_other;
  lcg = lcg * 0x6C078965u + 12345u;
  ctx.r3.u64 = lcg & 0x7FFFFFFFu;
  ++g_rng_cosmetic_calls;
}

// The store watch's report: function, thread kind, phase, resim or real, first lines per frame.
REXCVAR_DEFINE_BOOL(rb2_watch_chain, false, "Debug", "Rollback (rollback3) diagnostics: the store watch also logs the guest caller chain (can fault)");
REXCVAR_DEFINE_INT32(rb2_watch_tbit, 0, "Debug", "Rollback (rollback3) diagnostics: the store watch logs only stores of this write class (0 all)");
extern "C" void rb2_watch_hit(unsigned int a, unsigned long long v, int n, const char* fn) {
  if (const int only = REXCVAR_GET(rb2_watch_tbit); only && rb2_tbit != only) return;
  static std::atomic<int> lines{0};
  if (lines.fetch_add(1) > 4000) return;
  std::string chain;  // the guest callers (back chain: each frame's saved LR at [its caller's sp - 8])
  if (PPCContext* c = rex::runtime::current_ppc_context(); c && g_mem && REXCVAR_GET(rb2_watch_chain)) {
    uint32_t sp = c->r1.u32;
    for (int k = 0; k < 6 && sp >= 0x70000000u && sp < 0x80000000u && Committed(g_mem, sp, 4); ++k) {
      const uint32_t prev = rex::memory::load_and_swap<uint32_t>(g_mem->TranslateVirtual<const uint8_t*>(sp));
      if (prev <= sp || prev >= 0x80000000u || !Committed(g_mem, prev - 8, 4)) break;
      chain += fmt::format(" < {:08X}", rex::memory::load_and_swap<uint32_t>(g_mem->TranslateVirtual<const uint8_t*>(prev - 8)));
      sp = prev;
    }
  }
  REXLOG_WARN("[RB2] watch {:08X} = {:X} ({} B) by {} tbit {} phase {} {} frame {}{}", a, v, n, fn ? fn : "?", int(rb2_tbit),
              sync::g_phase, g_in_resim ? "RESIM" : "real", sync::s.frame, chain);
}

// ---- loopback online test (rb2_loop) ------------------------------------------------------------
// Versus with two pads, one PC: player 1 plays a scripted pattern; player 2's "remote" input is
// player 1's command of rb2_loop_lag frames earlier, and it "arrives" rb2_loop (+ 0..rb2_loop_jitter)
// frames after the frame it is for. Until then player 2 is predicted (its last arrived input); when the
// real one arrives and differs, the engine rewinds to the frame that consumed the prediction and
// resimulates with the real inputs. A confirmed frame that was consumed with a wrong input = a desync.
namespace loop {
constexpr int N = 1024;
uint64_t p1[N];
int32_t p1f[N];
uint64_t used[N];
int32_t usedf[N];
int64_t engf[N];
bool requested[N];
int32_t cnow = -1;
int64_t rewind_to = -1;
uint64_t predicted = 0, mispredicted = 0, confirmed = 0, bad = 0, consumes = 0;
int Arrival(int32_t c) {
  const uint32_t h = uint32_t(c) * 2654435761u;
  const int j = std::max(0, REXCVAR_GET(rb2_loop_jitter));
  return c + REXCVAR_GET(rb2_loop) + int((h >> 16) % uint32_t(j + 1));
}
uint64_t True(int32_t c) {
  const int32_t src = c - std::max(0, REXCVAR_GET(rb2_loop_lag));
  return (src >= 0 && p1f[src % N] == src) ? p1[src % N] : 0;
}
}  // namespace loop
int64_t BurstLimitRb2LoopPeekRewind() { return loop::rewind_to; }
int64_t BurstLimitRb2LoopTakeRewind() {
  const int64_t to = loop::rewind_to;
  loop::rewind_to = -1;
  return to;
}
std::string BurstLimitRb2LoopStats() {
  return fmt::format("consumes {} predicted {} mispredicted {} confirmed {} WRONG-AT-CONFIRM {}", loop::consumes,
                     loop::predicted, loop::mispredicted, loop::confirmed, loop::bad);
}
// From the consume (netinput's hook in sub_8217E4E8, offline driver): frame c is about to be consumed.
void BurstLimitRb2OnConsume(rex::memory::Memory* m, int32_t c) {
  using namespace loop;
  if (REXCVAR_GET(rb2_loop) <= 0 || REXCVAR_GET(rb2_sync) <= 0 || sync::s.stage != 2 || !sync::s.active || c < 0) return;
  const uint32_t manager = U32(m, 0x841B5EA4);
  if (!manager) return;
  auto slot = [&](int p) { return manager + uint32_t(p) * 784 + (uint32_t(c) % 32) * 24; };
  if (int32_t(U32(m, slot(0) + 16)) != c || int32_t(U32(m, slot(1) + 16)) != c) return;
  auto load64 = [&](uint32_t a) { return rex::memory::load_and_swap<uint64_t>(m->TranslateVirtual<const uint8_t*>(a)); };
  auto store64 = [&](uint32_t a, uint64_t v) { rex::memory::store_and_swap<uint64_t>(m->TranslateVirtual<uint8_t*>(a), v); };
  const bool resim = g_in_resim;
  ++consumes;
  if (!resim) {
    p1[c % N] = load64(slot(0));
    p1f[c % N] = c;
    cnow = std::max(cnow, c);
  } else if (p1f[c % N] == c) {
    store64(slot(0), p1[c % N]);
  }
  if (!resim) {
    static uint64_t seen = 0, nonzero = 0;
    ++seen;
    if (load64(slot(1))) ++nonzero;
    if (seen % 600 == 0) Log(fmt::format("loop: P2 slot commands before the override: {} of {} non-zero; P1 now {:016X}", nonzero, seen, load64(slot(0))));
  }
  uint64_t v = 0;
  bool pred = false;
  if (Arrival(c) <= cnow) {
    v = True(c);
  } else {
    pred = true;
    for (int32_t k = c - 1; k >= std::max(0, c - 64); --k) {
      if (Arrival(k) <= cnow) { v = True(k); break; }
    }
  }
  store64(slot(1), v);
  used[c % N] = v;
  usedf[c % N] = c;
  engf[c % N] = sync::g_cur_frame;
  if (!resim) {
    requested[c % N] = false;
    if (pred) ++predicted;
    // inputs that arrived by now: a consumed prediction that was wrong -> rewind there
    for (int32_t k = std::max(0, c - 40); k < c; ++k) {
      if (usedf[k % N] != k || requested[k % N] || Arrival(k) > cnow) continue;
      if (used[k % N] != True(k)) {
        requested[k % N] = true;
        ++mispredicted;
        if (rewind_to < 0 || engf[k % N] < rewind_to) rewind_to = engf[k % N];
      }
    }
    // a frame every input of which has arrived for good: must have been consumed right
    const int32_t k = c - (REXCVAR_GET(rb2_loop) + REXCVAR_GET(rb2_loop_jitter) + 12);
    if (k >= 0 && usedf[k % N] == k) {
      ++confirmed;
      if (used[k % N] != True(k)) ++bad;
    }
  }
}

// ---- CRI call boundary (rollback3, rb2_cri) ------------------------------------------------------
// The CRI sound / streaming library is never rewound (its code's writes are tagged, its work areas
// excluded), so a resimulated frame must not call it a second time for what the original frame already
// did (sounds started twice, handles released twice, streams requested twice - the suspected cause of
// the hang after 150-900 rewinds). The generated code wraps every CRI function (patch_generated.py v5,
// rb2_cri_scope): the OUTERMOST call on the main thread is
//   - in a live frame: run and recorded (function, r3-r6, f1, the r3 / f1 it returned, and the 16-byte
//     blocks it wrote outside the library's own stack frames, with their bytes before and after);
//   - in a resimulated frame: matched against the record of that frame (same function, same r3-r6 /
//     f1, the next unused one in order). A match whose blocks still hold their "before" (or "after")
//     bytes is answered from the record (results set, "after" bytes written, the library not entered);
//     anything else runs for real (new in the corrected timeline) and is recorded.
// Records of a resimulated frame replace the frame's old ones; old ones nobody matched are "discarded"
// (counted; their effects stay in the library - settlement is a later step).
// rb2_cri: 0 off, 1 record + count only, 2 record + answer in resims.
REXCVAR_DEFINE_INT32(rb2_cri, 2, "Debug", "Rollback: CRI call boundary (0 off, 1 record/count only, 2 answer resimulated calls from the record)");
extern "C" {
int rb2_cri_on = 0;
thread_local int rb2_cri_depth = 0;
thread_local unsigned char rb2_cri_rec = 0;
thread_local unsigned char rb2_main = 0;
thread_local unsigned char rb2_cri_live = 0x1F;
int rb2_d3d_watch = 0;  // also set while rb2_d3d_log (live frames)
}
namespace cri {
struct Block {
  uint32_t a;
  uint16_t mask;  // the bytes the call wrote
  uint8_t before[16];
  uint8_t after[16];
};
bool SameMasked(const uint8_t* x, const uint8_t* y, uint16_t mask) {
  for (int q = 0; q < 16; ++q) if (((mask >> q) & 1) && x[q] != y[q]) return false;
  return true;
}
struct Rec {
  bool outside = false;  // made outside the part of the frame a resim runs again (CRI server step, frame end, waits)
  uint32_t fn = 0;
  uint64_t r3 = 0, r4 = 0, r5 = 0, r6 = 0, f1 = 0;
  uint64_t out_r3 = 0, out_f1 = 0;
  bool callback = false;  // game / XDK code inside the call wrote outside the stack
  std::vector<Block> blocks;
  uint64_t r7 = 0;
  uint32_t voice = 0;     // sound settlement: the voice a play call started (rb2_sound_log / ghosts)
  uint32_t aux = 0;       // pause flag call: the channel's flag before the call
  bool claimed = false;   // a play another frame's resimulated twin took over (a shifted sound, not a ghost)
};
struct Frame {
  int64_t frame = -1;
  std::vector<Rec> recs;
};
constexpr int kSlots = 64;  // = the rewind ring (kRing): a deeper rewind found frames without a record
Frame g_slots[kSlots];
Frame g_cur;
bool g_cur_open = false, g_cur_resim = false;
const Frame* g_old = nullptr;
std::vector<uint8_t> g_used;
size_t g_cursor = 0;
Rec g_open;
bool g_recording = false;
uint32_t g_sp = 0;
std::map<uint32_t, size_t> g_block_index;
// stats
bool g_outside = false;  // live frame: between FrameF and the next FrameA, or in the CRI server step
int64_t live_outside = 0;
int64_t live_calls = 0, live_frames = 0, resim_calls = 0, answered = 0, ran_new = 0, ran_block_mismatch = 0,
        discarded = 0, callback_calls = 0, blocks_total = 0, frames_no_record = 0;
std::map<uint32_t, int64_t> g_new_fns, g_discard_fns, g_d3d, g_live_fns;
uint8_t* Host(uint32_t a) { return g_mem->TranslateVirtual<uint8_t*>(a); }
std::vector<uint8_t> g_excl_page(0x100000, 0);  // per guest page: in rb2_exclude
// ---- sound settlement (rb2_sound_settle) ----
// The game plays a sound through its sound manager: sub_8210C198 (channel r3, id r4, mode r6, volume f1) or
// sub_8210C3A0 (channel r3, class r4, id r5, mode r7 -> category); each (channel, category) is one player object
// (table [82725300] + 4 * (channel + 59 / 75 / 91 / 99)) with a status, pause, fade and stop. A resimulated frame
// answers a play its old record has (no double); a play no resimulated frame made again is a GHOST (the mispredicted
// frame's sound): after the rewind its channel is faded out (sub_8210A0D0, the game's own stop) unless the corrected
// timeline played on that channel since (it replaced the ghost) - Skullgirls-style settle. A new play in a resim
// that the old timeline made up to 4 frames away is the same sound shifted: it is answered (the voice keeps
// playing) - Slippi-style dedupe. A pause flag call (sub_8210A200) no resimulated frame made again is undone.
REXCVAR_DEFINE_INT32(rb2_sound_settle, 3, "Debug", "Rollback: sound settlement (bit 0: fade out ghost sounds of mispredicted frames / undo their pause flags, bit 1: a sound shifted by up to 4 frames keeps playing)");
REXCVAR_DEFINE_BOOL(rb2_sound_log, false, "Debug", "Rollback: log every sound play / ghost / final play per frame ([RB2Snd]) for the settlement check");
constexpr uint32_t kPlayA = 0x8210C198, kPlayB = 0x8210C3A0, kPause = 0x8210A200;
bool IsPlay(uint32_t fn) { return fn == kPlayA || fn == kPlayB; }
// the player object a play uses: (channel, category); false if the call plays nothing
bool PlayTarget(const Rec& r, uint32_t* idx, uint32_t* cat) {
  *idx = uint32_t(r.r3);
  if (r.fn == kPlayA) {  // modes 0, 1, 4 play (on channel + 59); any other returns at once
    *cat = 0;
    const uint32_t mode = uint32_t(r.r6);
    return *idx <= 15 && (mode == 0 || mode == 1 || mode == 4);
  }
  if (r.fn == kPlayB) {
    if (uint32_t(r.r4) > 15 || uint32_t(r.r7) > 4) return false;
    // its jump table on r7: 0 -> category 0, 1 -> 1, 2 -> 2, 3 -> 3, 4 -> 0 (channel + 59 / 75 / 91 / 99)
    *cat = uint32_t(r.r7) == 4 ? 0 : uint32_t(r.r7);
    return *idx <= (*cat == 1 ? 16u : *cat >= 2 ? 8u : 15u);
  }
  return false;
}
uint32_t PlayId(const Rec& r) { return uint32_t(r.fn == kPlayA ? r.r4 : r.r5); }
uint32_t PauseFlagAddr(uint32_t idx, uint32_t cat) {
  static const uint32_t off[4] = {0, 64, 128, 160};
  return 0x83625B10 + off[cat & 3] + 4 * idx;
}
struct Ghost {
  int64_t frame;
  Rec rec;
};
std::vector<Ghost> g_ghosts;
uint32_t g_voice_next = 1;
int64_t g_now = -1;  // the live frame a rewind returns to (lateness of a sound first played in a resim)
int64_t snd_ghost_stopped = 0, snd_ghost_replaced = 0, snd_ghost_idle = 0, snd_dedup = 0, snd_late = 0, snd_live = 0,
        snd_late_frames = 0, snd_pause_undone = 0;
bool SoundLog() { return REXCVAR_GET(rb2_sound_log); }
void LogPlay(const char* what, const Rec& r, int64_t frame, int64_t extra) {
  uint32_t idx = 0, cat = 0;
  const bool ok = PlayTarget(r, &idx, &cat);
  REXLOG_INFO("[RB2Snd] {} v {} f {} fn {:08X} ch {} cat {} id {:X} x {}{}", what, r.voice, frame, r.fn, idx, cat, PlayId(r), extra, ok ? "" : " (no player)");
}
void Commit() {
  if (!g_cur_open) return;
  if (g_cur_resim && g_old) {
    for (size_t i = 0; i < g_old->recs.size(); ++i) {
      if (!g_used[i] && !g_old->recs[i].outside) {
        ++discarded;
        ++g_discard_fns[g_old->recs[i].fn];
        const Rec& r = g_old->recs[i];
        if ((IsPlay(r.fn) && !r.claimed) || r.fn == kPause) g_ghosts.push_back(Ghost{g_cur.frame, r});
      }
    }
  }
  Frame& slot = g_slots[((g_cur.frame % kSlots) + kSlots) % kSlots];
  if (SoundLog() && slot.frame >= 0 && slot.frame != g_cur.frame) {  // an older frame leaves the ring: its plays are final
    for (const Rec& r : slot.recs) if (IsPlay(r.fn) && !r.outside) LogPlay("final", r, slot.frame, 0);
  }
  slot = std::move(g_cur);
  g_cur = Frame{};
  g_cur_open = false;
  g_old = nullptr;
}
void Begin(int64_t frame, bool resim) {
  Commit();
  if (!resim) g_ghosts.clear();  // settled after the rewind (online); the sync test's rewinds (verify) settle nothing
  g_cur.frame = frame;
  g_cur.recs.clear();
  g_cur_open = true;
  g_cur_resim = resim;
  g_cursor = 0;
  g_old = nullptr;
  if (resim) {
    const Frame& old = g_slots[((frame % kSlots) + kSlots) % kSlots];
    if (old.frame == frame) {
      g_old = &old;
      g_used.assign(old.recs.size(), 0);
    } else {
      ++frames_no_record;
    }
  } else {
    ++live_frames;
  }
}
void Reset() {
  if (SoundLog()) {  // the engine (re)starts after a pause: no rewind goes back past it - every recorded play is final
    Commit();
    std::vector<const Frame*> order;
    for (const Frame& f : g_slots) if (f.frame >= 0) order.push_back(&f);
    std::sort(order.begin(), order.end(), [](const Frame* a, const Frame* b) { return a->frame < b->frame; });
    for (const Frame* f : order)
      for (const Rec& r : f->recs) if (IsPlay(r.fn) && !r.outside) LogPlay("final", r, f->frame, 1);
  }
  g_cur_open = false;
  g_old = nullptr;
  g_recording = false;
  rb2_cri_rec = 0;
  for (auto& f : g_slots) f = Frame{};
  g_cur = Frame{};
  g_ghosts.clear();
}
// After a rewind's resimulated frames (the last one committed): fade out the ghosts nothing replaced, undo ghost
// pause flags. Called on the main thread at the frame start (outside any CRI call; no record is open).
void Settle(rex::memory::Memory* m, PPCContext& ctx, uint8_t* base, int64_t now) {
  (void)m;
  if (g_ghosts.empty()) return;
  if (!(REXCVAR_GET(rb2_sound_settle) & 1) || REXCVAR_GET(rb2_cri) < 2) {
    g_ghosts.clear();
    return;
  }
  // what the corrected timeline did on a channel since frame f
  auto later_on = [&](int64_t f, int fn_kind, uint32_t idx, uint32_t cat) {
    for (int64_t g = f; g < now; ++g) {
      const Frame& fr = g_slots[((g % kSlots) + kSlots) % kSlots];
      if (fr.frame != g) continue;
      for (const Rec& r : fr.recs) {
        if (r.outside) continue;
        uint32_t i2 = 0, c2 = 0;
        if (fn_kind == 0 && IsPlay(r.fn) && PlayTarget(r, &i2, &c2) && i2 == idx && c2 == cat) return true;
        if (fn_kind == 1 && r.fn == kPause && uint32_t(r.r3) == idx && uint32_t(r.r5) == cat) return true;
      }
    }
    return false;
  };
  const PPCContext saved = ctx;
  std::set<std::pair<uint32_t, uint32_t>> done_play, done_pause;
  for (const Ghost& gh : g_ghosts) {
    uint32_t idx = 0, cat = 0;
    if (IsPlay(gh.rec.fn)) {
      if (!PlayTarget(gh.rec, &idx, &cat)) continue;
      if (!done_play.insert({idx, cat}).second) {  // an earlier ghost of this rewind on the same channel decided it
        if (SoundLog()) LogPlay("ghost-samechannel", gh.rec, gh.frame, now);
        continue;
      }
      const char* what;
      if (later_on(gh.frame, 0, idx, cat)) {
        ++snd_ghost_replaced;
        what = "ghost-replaced";
      } else {
        ctx.r3.u64 = idx; ctx.r4.u64 = cat;
        sub_8210A308(ctx, base);
        const bool playing = (ctx.r3.u32 & 0xFF) != 0;
        ctx = saved;
        if (playing) {
          ctx.r3.u64 = idx; ctx.r4.u64 = 0; ctx.r5.u64 = cat;  // fade out (the game's own stop)
          sub_8210A0D0(ctx, base);
          ctx = saved;
          ++snd_ghost_stopped;
          what = "ghost-stopped";
        } else {
          ++snd_ghost_idle;
          what = "ghost-idle";
        }
      }
      if (SoundLog()) LogPlay(what, gh.rec, gh.frame, now);
    } else if (gh.rec.fn == kPause) {
      idx = uint32_t(gh.rec.r3); cat = uint32_t(gh.rec.r5);
      if (idx > 15 || cat > 3 || !done_pause.insert({idx, cat}).second) continue;  // the earliest ghost's "before" counts
      if (later_on(gh.frame, 1, idx, cat)) continue;
      ctx.r3.u64 = idx; ctx.r4.u64 = gh.rec.aux; ctx.r5.u64 = cat;
      sub_8210A200(ctx, base);
      ctx = saved;
      ++snd_pause_undone;
      if (SoundLog()) REXLOG_INFO("[RB2Snd] pause-undone f {} ch {} cat {} flag {} -> {}", gh.frame, idx, cat, uint32_t(gh.rec.r4), gh.rec.aux);
    }
  }
  g_ghosts.clear();
}
// Only the argument registers the function reads before writing them (patch_generated.py arg_liveness:
// bit 0..3 = r3..r6, bit 4 = f1); the others hold the caller's leftovers, which differ between a live
// call and its resimulated twin.
bool Matches(const Rec& r, uint32_t a, const PPCContext& c, unsigned live) {
  if (r.fn != a) return false;
  if ((live & 1) && r.r3 != c.r3.u64) return false;
  if ((live & 2) && r.r4 != c.r4.u64) return false;
  if ((live & 4) && r.r5 != c.r5.u64) return false;
  if ((live & 8) && r.r6 != c.r6.u64) return false;
  if ((live & 16) && r.f1 != c.f1.u64) return false;
  return true;
}
}  // namespace cri

extern "C" int rb2_cri_enter(unsigned int a, PPCContext& c) {
  using namespace cri;
  if (!g_cur_open || !g_mem || g_recording) return 0;
  const int mode = REXCVAR_GET(rb2_cri);
  if (mode <= 0) return 0;
  if (g_cur_resim) {
    ++resim_calls;
    if (mode >= 2 && g_old) {
      for (size_t i = g_cursor; i < g_old->recs.size(); ++i) {
        if (g_used[i] || g_old->recs[i].outside || g_old->recs[i].claimed || !Matches(g_old->recs[i], a, c, rb2_cri_live)) continue;
        const Rec& r = g_old->recs[i];
        bool ok = true;
        for (const Block& b : r.blocks) {
          if (!SameMasked(Host(b.a), b.before, b.mask) && !SameMasked(Host(b.a), b.after, b.mask)) {
            ok = false;
            break;
          }
        }
        if (!ok) {
          ++ran_block_mismatch;
          break;
        }
        for (const Block& b : r.blocks) {
          uint8_t* h = Host(b.a);
          for (int q = 0; q < 16; ++q) if ((b.mask >> q) & 1) h[q] = b.after[q];
          rb2_mfine[(b.a >> 7) & 0x1FFFFFF] |= uint8_t(1u << ((b.a >> 4) & 7));  // a main-thread write (lost-mark detection)
        }
        c.r3.u64 = r.out_r3;
        c.f1.u64 = r.out_f1;
        g_used[i] = 1;
        while (g_cursor < g_used.size() && g_used[g_cursor]) ++g_cursor;
        g_cur.recs.push_back(r);
        ++answered;
        return 1;
      }
    }
    if (g_old) {  // diagnostics: an unused old call of the same function - which argument differs
      static int shown = 0;
      for (size_t i = g_cursor; i < g_old->recs.size() && shown < 40; ++i) {
        const Rec& r = g_old->recs[i];
        if (g_used[i] || r.outside || r.fn != a) continue;
        ++shown;
        REXLOG_WARN("[RB2] cri new call {:08X} frame {}: old r3 {:X} r4 {:X} r5 {:X} r6 {:X} f1 {:X} / now r3 {:X} r4 {:X} r5 {:X} r6 {:X} f1 {:X}",
                    a, g_cur.frame, r.r3, r.r4, r.r5, r.r6, r.f1, c.r3.u64, c.r4.u64, c.r5.u64, c.r6.u64, c.f1.u64);
        break;
      }
    }
    if (mode >= 2 && IsPlay(a) && (REXCVAR_GET(rb2_sound_settle) & 2)) {
      // the same sound (function, channel, id, mode) the old timeline played up to 4 frames away: shifted, not new
      Rec probe;
      probe.fn = a; probe.r3 = c.r3.u64; probe.r4 = c.r4.u64; probe.r5 = c.r5.u64; probe.r6 = c.r6.u64; probe.r7 = c.r7.u64;
      auto same = [&](const Rec& r) {
        return r.fn == a && r.r3 == probe.r3 && PlayId(r) == PlayId(probe) && r.blocks.empty() &&
               (a == kPlayA ? r.r6 == probe.r6 : (r.r4 == probe.r4 && r.r7 == probe.r7));
      };
      const Rec* hit = nullptr;
      int64_t from = 0;
      for (size_t q = 0; q < g_ghosts.size() && !hit; ++q) {  // a ghost of an earlier resimulated frame
        if (IsPlay(g_ghosts[q].rec.fn) && g_cur.frame - g_ghosts[q].frame <= 4 && same(g_ghosts[q].rec)) {
          g_cur.recs.push_back(g_ghosts[q].rec);
          from = g_ghosts[q].frame;
          g_ghosts.erase(g_ghosts.begin() + q);
          hit = &g_cur.recs.back();
        }
      }
      for (int64_t f = g_cur.frame + 1; f <= g_cur.frame + 4 && f < g_now && !hit; ++f) {  // a later frame's, not resimulated yet
        Frame& later = g_slots[((f % kSlots) + kSlots) % kSlots];
        if (later.frame != f) continue;
        for (Rec& r : later.recs) {
          if (r.claimed || r.outside || !same(r)) continue;
          r.claimed = true;
          g_cur.recs.push_back(r);
          g_cur.recs.back().claimed = false;
          from = f;
          hit = &g_cur.recs.back();
          break;
        }
      }
      if (hit) {
        ++snd_dedup;
        if (SoundLog()) LogPlay("dedup", *hit, g_cur.frame, from);
        c.r3.u64 = hit->out_r3;
        c.f1.u64 = hit->out_f1;
        return 1;
      }
    }
    ++g_new_fns[a];
    ++ran_new;
  } else {
    ++live_calls;
    ++g_live_fns[a];
    if (g_outside) ++live_outside;
  }
  g_open = Rec{};
  g_open.outside = !g_cur_resim && g_outside;
  g_open.fn = a;
  g_open.r3 = c.r3.u64;
  g_open.r4 = c.r4.u64;
  g_open.r5 = c.r5.u64;
  g_open.r6 = c.r6.u64;
  g_open.f1 = c.f1.u64;
  g_open.r7 = c.r7.u64;
  if (IsPlay(a)) {
    g_open.voice = g_voice_next++;
    if (g_cur_resim) {
      ++snd_late;
      snd_late_frames += g_now - g_cur.frame;
    } else {
      ++snd_live;
    }
    if (SoundLog()) LogPlay(g_cur_resim ? "late" : "live", g_open, g_cur.frame, g_cur_resim ? g_now - g_cur.frame : 0);
  } else if (a == kPause && uint32_t(c.r3.u64) <= 15 && uint32_t(c.r5.u64) <= 3) {
    g_open.aux = __builtin_bswap32(*reinterpret_cast<const uint32_t*>(Host(PauseFlagAddr(uint32_t(c.r3.u64), uint32_t(c.r5.u64)))));
  }
  g_block_index.clear();
  g_sp = c.r1.u32;
  g_recording = true;
  rb2_cri_rec = 1;
  return 0;
}

extern "C" void rb2_cri_store(unsigned int x, unsigned int n, unsigned char kind) {
  using namespace cri;
  if (!g_recording) return;
  const uint32_t first = x & ~15u, last = (x + (n ? n - 1 : 0)) & ~15u;
  for (uint32_t b = first;; b += 16) {
    const bool caller_stack = b >= g_sp && b < g_sp + 0x40000;
    const bool own_frames = b < g_sp && b + 0x100000 >= g_sp;
    // library code: only the caller's stack (its own work memory is never rewound);
    // other code inside the call (callbacks, XDK): everything but the dead frames below the call
    // never-rewound memory (rb2_exclude: the sound manager's / CRI's own state) is live: not part of a record
    const bool want = (kind == 16 ? caller_stack : !own_frames) && !g_excl_page[(b >> 12) & 0xFFFFF];
    if (want) {
      // the bytes of this block the store covers (stores never span more than 16 bytes here)
      const uint32_t lo = std::max(x, b), hi = std::min<uint32_t>(x + (n ? n : 1), b + 16);
      uint16_t bytes = 0;
      for (uint32_t q = lo; q < hi; ++q) bytes |= uint16_t(1u << (q - b));
      auto it = g_block_index.find(b);
      if (it == g_block_index.end()) {
        g_block_index[b] = g_open.blocks.size();
        Block blk{};
        blk.a = b;
        blk.mask = bytes;
        std::memcpy(blk.before, Host(b), 16);
        g_open.blocks.push_back(blk);
        if (kind != 16 && !caller_stack) g_open.callback = true;
      } else {
        Block& blk = g_open.blocks[it->second];
        // bytes first written now: their "before" is what they hold now
        const uint8_t* h = Host(b);
        for (int q = 0; q < 16; ++q) if (((bytes >> q) & 1) && !((blk.mask >> q) & 1)) blk.before[q] = h[q];
        blk.mask |= bytes;
      }
    }
    if (b == last) break;
  }
}

// v10 write tracking (scripts/patch_generated.py): a guest store's notes, called only while rb2_track_on - out of line
// (v9 inlined all of this at every guest store: .text 84 MB vs 27 MB, 5-30 % CPU with rollback off). kn = kind << 16 |
// bytes, y = the value stored (for the store watch).
extern "C" void rb2_store_note(unsigned int x, unsigned int kn, unsigned long long y) {
  x = CanonAddr(x);
  const unsigned char kind = static_cast<unsigned char>(kn >> 16);
  const unsigned int n = kn & 0xFFFFu;
  if (rb2_cri_rec) rb2_cri_store(x, n, kind);
  unsigned char* d = &rb2_dirty[x >> 12];
  const unsigned char b = kind ? kind : rb2_tbit;
  if ((*d & b) != b) *d |= b;
  if (kind == 16) {
    unsigned char* f = &rb2_sfine[x >> 7];
    const unsigned char m = static_cast<unsigned char>(1u << ((x >> 4) & 7));
    if (!(*f & m)) *f |= m;
  } else if (b == 2) {  // other threads' writes per 16-byte block (kept live when a frame's page is rewound)
    const unsigned int last = x + (n ? n - 1 : 0);
    for (unsigned int q = x >> 4; q <= (last >> 4); ++q) {
      unsigned char* f = &rb2_ffine[q >> 3];
      const unsigned char m = static_cast<unsigned char>(1u << (q & 7));
      if (!(*f & m)) *f |= m;
    }
  } else if (b & 13) {  // the main thread's (rb2_mfine: lost-mark detection)
    const unsigned int last = x + (n ? n - 1 : 0);
    for (unsigned int q = x >> 4; q <= (last >> 4); ++q) {
      unsigned char* f = &rb2_mfine[q >> 3];
      const unsigned char m = static_cast<unsigned char>(1u << (q & 7));
      if (!(*f & m)) *f |= m;
    }
  }
  if (x - rb2_watch_lo < rb2_watch_size) {  // diagnostics: the store watch names the guest function of the caller
    thread_local char fn[16];
    std::snprintf(fn, sizeof(fn), "sub_%08X", GuestOfHost(uintptr_t(__builtin_return_address(0))));
    rb2_watch_hit(x, y, int(n), fn);
  }
}

extern "C" void rb2_cri_leave(unsigned int a, PPCContext& c) {
  using namespace cri;
  (void)a;
  if (!g_recording) return;
  g_recording = false;
  rb2_cri_rec = 0;
  for (Block& b : g_open.blocks) std::memcpy(b.after, Host(b.a), 16);
  g_open.out_r3 = c.r3.u64;
  g_open.out_f1 = c.f1.u64;
  if (g_open.callback) ++callback_calls;
  blocks_total += int64_t(g_open.blocks.size());
  if (g_cur_open) g_cur.recs.push_back(std::move(g_open));
}

void cri_excl_page(uint32_t page) { cri::g_excl_page[page & 0xFFFFF] = 1; }
extern "C" void rb2_d3d_hit(unsigned int a) {
  if (g_in_resim) ++cri::g_d3d[a];
  if (REXCVAR_GET(rb2_d3d_log)) {  // diagnostics: the main thread's D3D devices (r3 of the D3D entry), by phase
    static std::mutex mu;
    static std::set<std::pair<uint32_t, char>> seen;
    PPCContext* c = rex::runtime::current_ppc_context();
    const uint32_t dev = c ? c->r3.u32 : 0;
    std::lock_guard<std::mutex> lock(mu);
    if (seen.size() < 64 && seen.insert({dev, sync::g_phase}).second)
      REXLOG_INFO("[RB2] main-thread D3D entry {:08X} r3 {:08X} phase {} tbit {} {}", a, dev, sync::g_phase, int(rb2_tbit), g_in_resim ? "RESIM" : "live");
  }
}

std::string CriStats() {
  using namespace cri;
  auto top = [](const std::map<uint32_t, int64_t>& m) {
    std::vector<std::pair<int64_t, uint32_t>> v;
    for (auto& [k, n] : m) v.push_back({n, k});
    std::sort(v.rbegin(), v.rend());
    std::string out;
    for (size_t i = 0; i < v.size() && i < 6; ++i) out += fmt::format(" {:08X}x{}", v[i].second, v[i].first);
    return out.empty() ? std::string(" -") : out;
  };
  int64_t d3d = 0;
  for (auto& [k, n] : g_d3d) d3d += n;
  return fmt::format(
      "cri: live {} calls in {} frames ({:.1f}/frame, {} outside the resimulated part, {} with callback writes, {:.1f} blocks/call, top{}); resim {} calls: "
      "answered {}, new {} (block mismatch {}), discarded {}, resim frames without record {}; new:{} discarded:{}; "
      "D3D entries in resims {}:{}; sounds: {} played live, {} first played in a resim (late {:.1f} frames avg), {} shifted kept, "
      "ghosts {} faded out / {} replaced / {} already over, {} pause flags undone",
      live_calls, live_frames, double(live_calls) / std::max<int64_t>(live_frames, 1), live_outside, callback_calls,
      double(blocks_total) / std::max<int64_t>(live_calls + ran_new, 1), top(g_live_fns), resim_calls, answered, ran_new,
      ran_block_mismatch, discarded, frames_no_record, top(g_new_fns), top(g_discard_fns), d3d, top(g_d3d), snd_live, snd_late,
      double(snd_late_frames) / std::max<int64_t>(snd_late, 1), snd_dedup, snd_ghost_stopped, snd_ghost_replaced, snd_ghost_idle,
      snd_pause_undone);
}

bool Rb2EngineOn() { return REXCVAR_GET(rb2_sync) > 0 || BurstLimitRollback2Online(); }
// ---- API for the online input path (burstlimit_netinput.cpp) ----
bool BurstLimitRb2Active() { return Rb2EngineOn() && sync::s.stage == 2 && sync::s.active; }
bool BurstLimitRb2InResim() { return g_in_resim; }
// Host-side code that writes guest game state during a tracked frame (online input words, the ki charge mod, test
// hooks) notes the write like a guest store of the calling thread: on the main thread its blocks go back with a rewind.
// (Unnoted, rb2_lost_marks took such a change for a foreign store that landed late and kept it LIVE - r105_s75 /
// r105_s120 desynced on the test's full-ki write surviving a rewind over it.)
void BurstLimitRb2NoteHostStore(uint32_t a, uint32_t n) {
  if (rb2_track_on && n) sync::HostWrite(a, n, 0);
}
// Online: a prediction may be made only when a rewind to this frame will be possible - the engine is past its
// settle frames, the newest log is quiet and no guest thread was made this frame (otherwise: the game's own wait).
bool BurstLimitRb2PredictOk() {
  if (!BurstLimitRb2Active() || g_draining) return false;
  if (sync::s.frame < sync::SettleFrames() + 2) return false;
  if (sync::t.logs.empty() || !sync::t.logs.back().quiet) return false;
  if (sync::g_thread_created_frame.load() >= sync::s.frame - 1) return false;
  return true;
}
int64_t BurstLimitRb2CurrentFrame() { return sync::g_cur_frame; }
void BurstLimitRb2RequestRewind(int64_t engine_frame) {
  if (loop::rewind_to < 0 || engine_frame < loop::rewind_to) loop::rewind_to = engine_frame;
}

// sub_820F0838(pool, job): the job pool's push - who pushed it (see BurstLimitRb2WorkerJob)
REX_EXTERN(__imp__sub_820F0838);
extern "C" REX_FUNC(sub_820F0838) {
  auto* mem = rex::Runtime::instance()->memory();
  if (REXCVAR_GET(rb2_job_class) && ctx.r4.u32 && mem && Committed(mem, ctx.r4.u32, 4)) {  // test only (rollback off:
                                                                                              // no cost per job)
    NoteJobPush(ctx.r4.u32, rex::memory::load_and_swap<uint32_t>(mem->TranslateVirtual<const uint8_t*>(ctx.r4.u32)));
  }
  __imp__sub_820F0838(ctx, base);
}

// diagnostics (rollback3, rb2_log_allocators): the job-node free-list allocators (sub_820F3AE8, r3 = allocator:
// CS at +0, count +36, free list head +44) - which instances exist and which threads use them
REXCVAR_DEFINE_BOOL(rb2_log_allocators, false, "Debug", "Rollback (rollback3) diagnostics: log the job-node allocators sub_820F3AE8 is called on (and from which thread class)");
// The resource cache (object 0x82724600: allocator +4, map +128 with its critical section, queues +960 / +984) is
// shared by the main thread and the render loop thread (sub_82219360 -> sub_8210F810 -> sub_820E5C70). A rewind
// over a frame in which the MAIN thread requested / released / changed a cache entry undoes half of it (the render
// thread's writes stay live) -> the render thread found a free node as an object (NULL vtable call, lr 820EBB54,
// offline stress Frieza vs Cell) - both in 93D015F2 and now. Such a frame's log is marked not quiet (no rewind over
// it, like a frame with busy workers; online the engine then stops predicting until it can rewind again).
REXCVAR_DEFINE_BOOL(rb2_res_guard, true, "Debug", "Rollback: no rewind over a frame in which the main thread used the resource cache (0x82724600)");
bool ResGuardOn() { return REXCVAR_GET(rb2_res_guard); }
REXCVAR_DEFINE_BOOL(rb2_log_resources, false, "Debug", "Rollback (rollback3) diagnostics: log the resource cache calls per thread class");
std::atomic<bool> g_res_main_touch{false};
std::atomic<int64_t> g_res_calls_main{0}, g_res_calls_other{0}, g_res_logs_marked{0};
void ResTouch(uint32_t fn, PPCContext& ctx) {
  if (!rb2_track_on) return;
  if (rb2_main) { g_res_main_touch.store(true, std::memory_order_relaxed); ++g_res_calls_main; } else { ++g_res_calls_other; }
  if (REXCVAR_GET(rb2_log_resources)) {
    static std::atomic<int> n{0};
    if (n.fetch_add(1) < 2000)
      REXLOG_INFO("[RB2] resource cache {:08X} r3 {:08X} r4 {:08X} r5 {:08X} (tbit {}, {}, lr {:08X}, frame {}, {})", fn, ctx.r3.u32, ctx.r4.u32,
                  ctx.r5.u32, int(rb2_tbit), rb2_main ? "main" : "other", uint32_t(ctx.lr), sync_frame_for_fault.load(), g_in_resim ? "resim" : "live");
  }
}
#define RB2_RES_HOOK(addr)   REX_EXTERN(__imp__sub_##addr);   extern "C" REX_FUNC(sub_##addr) { ResTouch(0x##addr, ctx); __imp__sub_##addr(ctx, base); }
RB2_RES_HOOK(820EBB10)  // request (map lookup / create + queue)
RB2_RES_HOOK(820E5A20)
RB2_RES_HOOK(820E5CB0)  // lookup -> vtable +8
RB2_RES_HOOK(820E5CF8)  // lookup -> vtable +40
RB2_RES_HOOK(820E5D70)  // lookup -> vtable +52
RB2_RES_HOOK(820E7CD8)
// Effect script command 8214E528 (called by the script interpreter 8212AB78 with r4 = the script context; its
// [+44] = params {index, side}) -> sub_8214C238(side, index): per-index tables at 0x826B5FA8 (28 entries). The offline
// stress crashed there reading guest 0 (crash class "C", in 93D015F2 too): a context / params left inconsistent by a
// rewind. rb2_fx_guard: a command whose index is out of the tables is logged with its memory and skipped (a crash
// otherwise); diagnostics say which memory it was.
REXCVAR_DEFINE_BOOL(rb2_fx_guard, true, "Debug", "Rollback: skip (and log) an effect script command 8214E528 with an index outside its tables");
REXCVAR_DEFINE_BOOL(rb2_fx_guard_null, true, "Debug", "Rollback: skip an effect command 8214E528 whose slot array is NULL (8214C238 would crash reading guest 0)");
std::atomic<int64_t> g_fx_null_skips{0};
REXCVAR_DEFINE_BOOL(rb2_fx_guard_skip, false, "Debug", "Rollback diagnostics: also SKIP the logged effect command (changes normal play: index -1 is legit)");
REX_EXTERN(__imp__sub_8214E528);
bool TrackedPageInfo(uint32_t a, std::string& out);
extern "C" REX_FUNC(sub_8214E528) {
  // also with rollback off: build 110 crashed there with the engine OFF (off110_2, Teen Gohan vs Piccolo, 30 s in)
  rex::memory::Memory* gm = g_mem_for_watch ? g_mem_for_watch : rex::Runtime::instance()->memory();
  if (REXCVAR_GET(rb2_fx_guard) && gm) {
    auto u32 = [&](uint32_t a) { return rex::memory::load_and_swap<uint32_t>(base + a); };
    const uint32_t ctxo = ctx.r4.u32;
    const uint32_t param = ctxo ? u32(ctxo + 44) : 0;
    const uint32_t index = param ? u32(param) : 0xFFFFFFFFu, side = param ? u32(param + 4) : 0xFFFFFFFFu;
    // the word 8214C238 will take as the slot array (index -1 reads 0x826B5FA4, which is 0)
    const uint32_t eaddr = 0x826B5FA8u + 4u * index;
    const uint32_t entry = (eaddr >= 0x826B0000u && eaddr < 0x826C0000u) ? u32(eaddr) : 0;
    if (!ctxo || !param || index >= 28 || !entry || side > 1) {
      static std::atomic<int> shown{0};
      if (shown.fetch_add(1) < 20) {
        std::string ci, pi;
        TrackedPageInfo(ctxo, ci);
        TrackedPageInfo(param, pi);
        REXLOG_WARN("[RB2] effect command 8214E528 index outside the tables: context {:08X} ({}) params {:08X} ({}) index {} side {} table {:08X} | "
                    "script pos {:08X}, frame {} {}", ctxo, ci, param, pi, index, side, entry, ctxo ? u32(ctxo + 36) : 0,
                    sync_frame_for_fault.load(), g_in_resim ? "resim" : "live");
        // the script bytes around its position, the context and the params (what set index -1?)
        auto hexs = [&](uint32_t a, uint32_t n) {
          std::string h;
          if (!a || !Committed(gm, a, n)) return std::string("-");
          for (uint32_t k = 0; k < n; ++k) h += fmt::format("{:02X}{}", base[a + k], (k % 4) == 3 ? " " : "");
          return h;
        };
        const uint32_t pos = ctxo ? u32(ctxo + 36) : 0;
        REXLOG_WARN("[RB2] effect command detail: script {:08X}: {} | {} || context: {} || params: {} | lr {:08X}", pos,
                    hexs(pos >= 48 ? pos - 48 : 0, 48), hexs(pos, 32), hexs(ctxo, 96), hexs(param, 16), uint32_t(ctx.lr));
        rex::FlushLogging();
      }
      if (REXCVAR_GET(rb2_fx_guard_skip)) return;  // off: index -1 happens in normal play (no rewind at all, nr102)
      // a NULL slot array is a certain crash in 8214C238 (crash class C: r108_off3, s107fc2/3 - index -1, side 0, script
      // position DD1F36F3, one command per run, then the crash): skipped instead (nr102 ran on after such skips)
      if (!entry && REXCVAR_GET(rb2_fx_guard_null)) { ++g_fx_null_skips; return; }
    }
  }
  __imp__sub_8214E528(ctx, base);
}
REX_EXTERN(__imp__sub_820F3AE8);
extern "C" REX_FUNC(sub_820F3AE8) {
  if (REXCVAR_GET(rb2_log_allocators)) {
    static std::mutex mu;
    static std::set<std::pair<uint32_t, int>> seen;
    std::lock_guard<std::mutex> lock(mu);
    if (seen.size() < 64 && seen.insert({ctx.r3.u32, int(rb2_tbit)}).second)
      REXLOG_INFO("[RB2] job-node allocator {:08X} used by tbit {} (lr {:08X})", ctx.r3.u32, int(rb2_tbit), uint32_t(ctx.lr));
  }
  const uint32_t allocator = ctx.r3.u32;
  __imp__sub_820F3AE8(ctx, base);
  if (REXCVAR_GET(rb2_log_allocators) && (allocator == 0x8438D884u || allocator == 0x8438D880u)) {  // where its nodes are (h9:
                                                               // a garbage function pointer)
    const uint32_t node = ctx.r3.u32;
    if (node) {
      uint32_t lo = g_node_lo.load(), hi = g_node_hi.load();
      if (!lo || node < lo) g_node_lo.store(node);
      if (node > hi) g_node_hi.store(node);
      static std::mutex mu;
      static std::set<uint32_t> pages;
      std::lock_guard<std::mutex> lock(mu);
      if (pages.size() < 64 && pages.insert(node & ~0xFFFu).second)
        REXLOG_INFO("[RB2] job-node page {:08X} (node {:08X}, tbit {}, lr {:08X}, {})", node & ~0xFFFu, node, int(rb2_tbit),
                    uint32_t(ctx.lr), g_in_resim ? "resim" : "live");
    }
  }
}

// diagnostics: what the engine knows about the page of guest address a (tracked / excluded / ever written by the main
// update / the physical view it is tracked in)
bool TrackedPageInfo(uint32_t a, std::string& out) {
  using namespace sync;
  const uint32_t page = a >> 12;
  int32_t idx = -1;
  for (size_t i = 0; i < t.gp.size(); ++i) if (t.gp[i] == page) { idx = int32_t(i); break; }
  const uint32_t canon = (a >= 0xA0000000u && rb2_phys_canon) ? CanonAddr(a) : a;
  out = fmt::format("{}{}{}", idx < 0 ? "untracked" : (s.excluded[idx] ? "tracked, excluded" : "tracked"),
                    idx >= 0 && t.ever_main[idx] ? ", main writes" : "", canon != a ? fmt::format(", noted as {:08X}", canon) : "");
  return idx >= 0;
}
