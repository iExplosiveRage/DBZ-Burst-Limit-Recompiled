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
#include <filesystem>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <new>
#include <exception>
#include <string>
#include <deque>
#include <map>
#include <set>
#include <mutex>
#include <thread>
#include <vector>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <array>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/input/input_system.h>
#include <rex/logging.h>
#include <rex/logging/assert.h>
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
#include <timeapi.h>
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
// v0.8.1 R31 ROOT CAUSE (build 140). A frame's log wrote a page's foreign block marks into its keep mask only when
// the page's dirty byte had the foreign bit IN THAT FRAME. The dirty bytes are collected at the start of the
// frame-end work and the pages are logged one by one after that (milliseconds): a store-note of another thread in
// between sets the block mark at once, but its page bit counts for the NEXT frame. If the page was logged for
// main / worker writes of this frame, the mark was read as "no foreign bit, no mask", then cleared by the take -
// and the page's image in THIS log (the bytes before the store) went back at the next rewind over the frame.
// MEASURED r139_d3: restore at frame 2551 changed 21 words of the resource cache's node lists (9 nodes back in
// the queued list, the header's counts live): every block "logs undone with its page: 2549:restored P
// 2547:restored P" = not in any keep mask, mark in the previous generation, page dirty byte 08 (worker class).
// That is the frozen screen (render loop in sub_820EBCB0 on a list without its sentinel: r137_d1, r125_c1,
// r108_off1) - torn in 4 of 10 loopback runs with transformations. Now the marks are taken first (atomic) and
// what was taken is the log's keep mask whatever the page byte says.
REXCVAR_DEFINE_BOOL(rb2_ffine_all, true, "Debug",
                    "Rollback: a frame's log keeps every foreign block mark of a logged page, also when the page's dirty byte has "
                    "no foreign bit in that frame (off = builds up to 139)");
int64_t g_ffine_nobit_pages = 0, g_ffine_nobit_blocks = 0;
// v0.8.1 R39 ROOT CAUSE (builds 145 / 146): the AGE of a foreign block mark. A page that only other threads wrote
// in a frame is not logged, and its block marks wait in rb2_ffine for the page's next log (rb2_ffine_keep) -
// however long that takes. The log that finally takes them belongs to a frame in which the MAIN thread wrote the
// page, and a rewind over that frame keeps every such block live: main-thread writes of an undone frame.
// That is harmless (and wanted) for memory that belongs to the render side - its blocks are written again and
// again, also by host code that leaves no mark, and the sticky mark is what protects them. It is WRONG for data a
// loader thread writes ONCE and the main thread owns from then on: a transformation's files. The file decoder
// (job sub_824EB808 on the codec worker thread sub_82513E38: sub_82513280 writes the decoded bytes backward into
// a game heap block) marked every block of the new form's files; ~100 frames later the main thread's frames turn
// the file's offsets into pointers and set the form up - the first main-thread writes on those pages, logged with
// the whole stale mask. MEASURED r145_b1 (loopback, 4 transformations, build 144's behaviour): 7 rewinds over such
// frames kept 12681 blocks of the game heap live that differed from their images ("STALE foreign mark kept block
// D689D000 live in the rewind 1171 -> 1165 ... last foreign store in frame 1063; image 00000000 00020001 00002368
// 000037F4, kept 00000000 00020001 D689DB88 D689F014": the offsets were already pointers when the resimulated
// frame set the form up again). Outside the heap: 1 block in the same run. That is R39 (r144_ltr: malloc met a
// header that was file data; r142_t3: the new form's set-up read through a NULL resource object).
// rb2_ffine_age = 1 (build 146): the decoder's job and file reads of other threads write with the "fill" class
// (rb2_tbit 32, marks in rb2_lfine): their marks are taken at the end of the frame they were made in and stay with
// that frame's log (Undo::fonly / fmask) - a rewind keeps them live only if it undoes that frame. Everything
// else keeps its sticky marks.
// rb2_ffine_age = 2 (build 145, test): every foreign mark is aged like that. r145_a1: the stage was gone after
// the first cinematic (black background, shots 006-008) and no further transformation completed - the render
// side's unmarked host writes lost their protection ("late stores 5573").
REXCVAR_DEFINE_INT32(rb2_ffine_age, 1, "Debug",
                     "Rollback: block marks of data a loader thread writes once (the file decoder, file reads) belong to the "
                     "frame they were made in - a rewind keeps them live only if it undoes that frame (1); 0 = builds up to "
                     "144: they waited for the page's next log, however old; 2 = test: every foreign mark is aged");
// the fill class's block marks since the last frame end (taken by every frame end)
unsigned char rb2_lfine[0x2000000] = {};
std::atomic<int64_t> g_fill_jobs{0};
REXCVAR_DEFINE_INT32(rb2_stale_log, 12, "Debug",
                     "Rollback diagnostics (R39): log the first N blocks a restore kept live on a foreign mark although no other "
                     "thread wrote the page in the undone frames, and whose bytes differ from the image");
REXCVAR_DEFINE_BOOL(rb2_lost_gate, true, "Debug", "Rollback: a block counts as a lost foreign store only if the page's previous foreign marks had it");
REXCVAR_DEFINE_INT32(rb2_lost_log, 0, "Debug", "Rollback diagnostics: log the first N blocks taken as lost foreign stores (address, frame, thread marks of the page)");
std::atomic<int> g_lost_logged{0};
int64_t g_lost_mark_blocks = 0;
REXCVAR_DEFINE_BOOL(rb2_ffine_prev_on, false, "Debug", "Rollback: foreign marks also count one log later (covers a store that lands after its page's log)");
int64_t BurstLimitRb2LoopTakeRewind();
std::string BurstLimitRb2LoopStats();
// burstlimit_texreg.cpp (v0.8.1): texture registry audit, guard / command counters, the obituary rings
bool BurstLimitTexAudit(PPCContext& ctx, uint8_t* base, const char* why);
void BurstLimitTexRewindBegin(int64_t from, int64_t to);  // R13a: the resource command boundary of resimulated frames
void BurstLimitRb2FileGateRewind(int64_t from, int64_t to);  // R30: a rewind over the frames [from, to)
// v0.8.1 R39: the game heap (see "namespace heap")
void BurstLimitRb2HeapAudit(rex::memory::Memory* m, const char* point, int64_t frame, int k);
void BurstLimitRb2HeapRewind(int64_t now, int n);
bool BurstLimitRb2HeapPage(uint32_t page_addr);
void BurstLimitRb2HeapKept(uint32_t block_addr, int why);
std::string BurstLimitRb2HeapStats();
void BurstLimitRb2RequestRewind(int64_t engine_frame);
void BurstLimitRb2DropRewind();
extern std::mutex g_d3d_callers_mu;  // R40 diagnostics (rb2_d3d_callers)
extern std::map<std::pair<uint32_t, uint32_t>, uint64_t> g_d3d_callers;
int64_t g_gate_rw_at = -1;  // test (rb2_gate_rw_test): the engine frame of the forced rewind
std::string BurstLimitRb2HeapBlocks(uint32_t a, uint32_t bytes);  // R16b: the words / marks / last restore of [a, a + bytes)
extern std::atomic<int64_t> g_fx_id_skips;  // R16 (rb2_fx_guard_id)
int g_gate_rw_depth = 0;
bool BurstLimitRb2FileGateWorked(int64_t from, int64_t to);  // R31: the file queue worked in one of the frames [from, to)
// R7 (crash record v2): the last rewind, for the record's engine state
int64_t g_last_rw_frame = -1;
int g_last_rw_depth = 0;
uint64_t g_last_rw_tick = 0;
void BurstLimitTexRewindEnd();
std::string BurstLimitTexStats();
int BurstLimitTexRingsRaw(char* out, int cap);
std::string BurstLimitRb2PaceStats();
REXCVAR_DECLARE(int32_t, rb2_tex_audit);
bool BurstLimitRollback2Online();  // burstlimit_netinput.cpp: a lobby match whose netcode is rollback
void BurstLimitOnlineAlert(const char* kind, const std::string& text);  // burstlimit_online.cpp (v0.8.1 R8, build 151)
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
int g_diff_fx_left = 0;  // R40 diagnostics (rb2_diff_fx)
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
// v0.8.1 R34: the second range ended at 840D6300 up to build 132 and so covered the first 14 slots of the effects
// system's node free ring (0x840D62C8.., FIFO, 512 slots; the effects block starts at 0x840D62C0: sub_82110858).
// A rewind whose undone frames freed more nodes than were in use at its target (3-6 times per harsh loopback
// run: r132_a1..) left nodes of the abandoned timeline in valid free slots there -> the same node handed out
// twice -> emitters updated / freed twice -> particles in two lists (the R34 crashes and hangs). The render
// side state (command list pointers, GPU time history) ends below 840D62C0.
REXCVAR_DEFINE_STRING(rb2_live_ranges, "8438AC00-8438AD00,840D6100-840D62C0", "Debug", "Rollback (rollback3): guest ranges (hex a-b,...) a rewind never writes - render hand-off state shared with the draw worker (its context 0x8438AC00: event, ready flag, work item) and the render command list pointers (builds up to 132: 840D6100-840D6300, which tore the effects node ring)");
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
REXCVAR_DEFINE_INT32(rb2_loop_tr, 0, "Debug", "Rollback loopback test: every N engine frames player 1 presses LB + RB (transformation) and both fighters get full ki (0 off)");
REXCVAR_DEFINE_BOOL(rb2_stress_gate, true, "Debug", "Rollback offline stress (rb2_sync without rb2_loop): a forced rewind that would cross a frame of file queue work is skipped - online the file gate guarantees that (off = builds up to 126: reads started again, creates sent again, one frozen screen)");
REXCVAR_DEFINE_INT32(rb2_loop_ult, 0, "Debug", "Rollback test (R40): every N engine frames player 1's scripted pad presses RT + Y + B (the ultimate) for 12 frames and both fighters get full ki (0 off; with rb2_stress_pad or rb2_loop)");
REXCVAR_DEFINE_INT32(rb2_fx_cine, 0, "Debug", "Rollback (R40): an effect-script cinematic (script tasks 8212AB78 / 133000xx) counts as a cinematic - the engine pauses in it (0 off)");
REXCVAR_DEFINE_STRING(rb2_snap_cine, "", "Debug", "Rollback diagnostics (R40): a,b - snapshots of every tracked page at the first effect-script cinematic's start, a main ticks later and b ticks after its end (<log>.snapN.bin)");
REXCVAR_DEFINE_INT32(rb2_d3d_callers, 0, "Debug", "Rollback diagnostics (R40): every N engine frames log the render side's D3D library entries by function and guest caller (0 off)");
REXCVAR_DEFINE_BOOL(rb2_task_log, false, "Debug", "Rollback diagnostics (R40): log the scheduler's tasks (callback / priority) whenever the set changes (checked every 10 live engine frames)");
REXCVAR_DEFINE_INT32(rb2_diff_fx, 0, "Debug", "Rollback diagnostics (R40): the offline stress's first N rewinds after an effect script's sync point (8214E5B4) log every word the resimulation left different (0 off)");
REXCVAR_DEFINE_STRING(rb2_diff_frames, "", "Debug", "Rollback diagnostics (R40): offline stress rewinds at engine frames a-b log every word the resimulation left different");
REXCVAR_DEFINE_BOOL(rb2_stress_pad, false, "Debug", "Rollback offline stress test: player 1's pad is scripted as in the loopback (with rb2_loop_tr: transformations under forced rewinds)");
std::atomic<int64_t> g_stress_gate_skips{0};
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
REXCVAR_DEFINE_BOOL(rb2_crash_first_chance_filter, true, "Debug",
                    "Crash records (review L1): the first-chance handler records only faults in the game's own modules (others go "
                    "to the unhandled-exception filter) and a stack overflow writes one fixed line (off = builds up to 152)");
// review L6: the list is built once (at the crash handlers' install, or at the first engine start if that comes
// first); a crash record reading it on another thread never sees it half rebuilt
void CacheModules() {
  static std::atomic<bool> built{false};
  if (built.load() && REXCVAR_GET(rb2_crash_first_chance_filter)) return;
  HMODULE mods[512];
  DWORD need = 0;
  if (!K32EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &need)) return;
  if (built.exchange(true) && REXCVAR_GET(rb2_crash_first_chance_filter)) return;
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
// review L5: the file sits next to the log (<log_file>.hangraw.txt) or, without a log file, next to the exe
// (burstlimit.hangraw.txt) - never in the working directory; it is created only when there is something to write
// (the path is prepared here, outside any crash, so the first write needs no allocation).
char g_hang_raw_path[1024] = {};
std::string BurstLimitDiagPath(const char* exe_name, const char* suffix) {
  std::string path = rex::cvar::GetFlagByName("log_file");
  if (path.empty()) {
    char exe[MAX_PATH] = {};
    const DWORD len = GetModuleFileNameA(nullptr, exe, MAX_PATH);
    std::string dir = len > 0 && len < MAX_PATH ? std::string(exe, len) : std::string();
    const size_t slash = dir.find_last_of("/\\");
    dir = slash == std::string::npos ? std::string() : dir.substr(0, slash + 1);
    path = dir + exe_name;
  }
  return path + suffix;
}
void OpenHangRaw() {
  if (g_hang_raw_path[0]) return;
  const std::string path = BurstLimitDiagPath("burstlimit", ".hangraw.txt");
  std::snprintf(g_hang_raw_path, sizeof(g_hang_raw_path), "%s", path.c_str());
}
void RawOut(const char* text) {
  if (g_hang_raw == INVALID_HANDLE_VALUE && g_hang_raw_path[0])
    g_hang_raw = CreateFileA(g_hang_raw_path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
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
//
// v0.8.1 R7, crash record v2 (rb2_crash_record): the handlers are installed at the first main-loop frame (v0.8.0:
// at the first rollback fight), for every fatal exception code and for abort / terminate / pure call / invalid
// parameter (every REX_FATAL of the SDK ends in std::abort: "Call to invalid or unregistered function" left no
// record at all); up to 4 records per process; each record adds the thread's role, all host registers, the
// thread's guest registers and guest back chain, and the engine state.
REXCVAR_DEFINE_BOOL(rb2_crash_record, true, "Debug",
                    "Crash records (<log>.hangraw.txt) from game start, for every fatal exception and abort, with guest "
                    "registers and the engine state (off = v0.8.0: access violations only, after the first rollback fight)");
REXCVAR_DEFINE_STRING(rb2_crash_test, "", "Debug",
                      "Test: av | null_call | abort | fatal - the game crashes on purpose at main-loop frame "
                      "rb2_crash_test_frame on the thread rb2_crash_test_thread");
REXCVAR_DEFINE_INT32(rb2_crash_test_frame, 900, "Debug", "Test: the main-loop frame (from process start) of rb2_crash_test");
REXCVAR_DEFINE_STRING(rb2_crash_test_thread, "main", "Debug", "Test: main | render (the render loop thread)");
thread_local const char* t_role = nullptr;  // set by the thread hooks: "render loop", "draw worker", "job worker"
extern "C" thread_local unsigned char rb2_main;
uint64_t BurstLimitTexGuardSkips();
int EngineStateRaw(char* out, int cap);  // at the end of this file (the engine's state, no allocation)
int PointerNotesRaw(char* out, int cap, const uint32_t* ptrs, int count);  // at the end of this file (R34)
const char* ThreadRole() { return rb2_main ? "main" : (t_role ? t_role : "other"); }
// Readable host memory, asked from the OS (no lock of ours; r124_g0: the heap's own region query called the end of a
// guest stack committed, the back chain walk read past it and the record stopped after its third line)
bool HostReadable(const void* p, size_t n) {
  MEMORY_BASIC_INFORMATION mbi;
  if (!VirtualQuery(p, &mbi, sizeof(mbi))) return false;
  if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return false;
  return uintptr_t(p) + n <= uintptr_t(mbi.BaseAddress) + mbi.RegionSize;
}
void CrashRecord(const char* what, const CONTEXT* cr, const EXCEPTION_RECORD* er) {
  static thread_local bool busy = false;
  static std::atomic<int> count{0};
  static std::atomic<uint64_t> last_key{0};
  if (busy) return;
  const bool v2 = REXCVAR_GET(rb2_crash_record);
  const uint64_t rip = cr->Rip;
  const DWORD code = er ? er->ExceptionCode : 0xE0000001u;  // E0000001: abort / terminate (no exception)
  // the same fault comes through the last vectored handler and the top-level filter
  const uint64_t key = (uint64_t(GetCurrentThreadId()) << 40) ^ rip ^ (uint64_t(code) << 8) ^ cr->Rsp;
  if (last_key.exchange(key) == key) return;
  const int number = count.fetch_add(1) + 1;
  if (number > (v2 ? 4 : 1)) return;
  busy = true;
  static std::mutex mu;
  std::lock_guard<std::mutex> lock(mu);
  static char buf[8192];
  uint64_t off = 0;
  const char* mod = ModNameRaw(rip, &off);
  const uint64_t data = er && er->NumberParameters >= 2 ? er->ExceptionInformation[1] : 0;
  auto* runtime = rex::Runtime::instance();
  rex::memory::Memory* mem = g_mem_for_watch ? g_mem_for_watch : (runtime ? runtime->memory() : nullptr);
  const uint64_t mb = mem ? uint64_t(uintptr_t(mem->virtual_membase())) : 0;
  int n = std::snprintf(buf, sizeof(buf),
                        "\r\nCRASH thread %lu code %08lX at %s+%llX (guest function %08X) %s %llX (guest view +%llX) frame %lld %s | rax %llX rcx %llX rdx %llX r8 %llX r9 %llX rsp %llX\r\n",
                        GetCurrentThreadId(), code, mod, (unsigned long long)off, GuestOfHost(uintptr_t(rip)),
                        er && er->NumberParameters >= 1 && er->ExceptionInformation[0] ? "writing" : "reading",
                        (unsigned long long)data, (unsigned long long)(data >= mb ? data - mb : 0),
                        (long long)sync_frame_for_fault.load(), g_in_resim ? "in resim" : "live",
                        (unsigned long long)cr->Rax, (unsigned long long)cr->Rcx, (unsigned long long)cr->Rdx,
                        (unsigned long long)cr->R8, (unsigned long long)cr->R9, (unsigned long long)cr->Rsp);
  RawOut(buf);  // the first line goes out before anything that could fault (snd_120: the stack scan below read past
                // the thread's stack and the filter itself crashed - nothing was written)
  if (v2) {
    SYSTEMTIME st;
    GetSystemTime(&st);
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    n = std::snprintf(buf, sizeof(buf),
                      "record %d: %s | thread role: %s | %04d-%02d-%02dT%02d:%02d:%02dZ pid %lu cpus %lu\r\n"
                      "host: rax %llX rbx %llX rcx %llX rdx %llX rsi %llX rdi %llX rbp %llX rsp %llX r8 %llX r9 %llX r10 %llX r11 %llX "
                      "r12 %llX r13 %llX r14 %llX r15 %llX rip %llX eflags %lX\r\n",
                      number, what, ThreadRole(), st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
                      GetCurrentProcessId(), si.dwNumberOfProcessors, (unsigned long long)cr->Rax, (unsigned long long)cr->Rbx,
                      (unsigned long long)cr->Rcx, (unsigned long long)cr->Rdx, (unsigned long long)cr->Rsi,
                      (unsigned long long)cr->Rdi, (unsigned long long)cr->Rbp, (unsigned long long)cr->Rsp,
                      (unsigned long long)cr->R8, (unsigned long long)cr->R9, (unsigned long long)cr->R10,
                      (unsigned long long)cr->R11, (unsigned long long)cr->R12, (unsigned long long)cr->R13,
                      (unsigned long long)cr->R14, (unsigned long long)cr->R15, (unsigned long long)cr->Rip, cr->EFlags);
    RawOut(buf);
    // the guest registers of this thread (the context the recompiled code works on; inside the faulting function a
    // register can lag behind its host copy) and the guest back chain (saved lr at back chain - 8)
    if (PPCContext* g = rex::runtime::current_ppc_context()) {
      n = std::snprintf(buf, sizeof(buf),
                        "guest: lr %08X ctr %08X r1 %08X r3 %08X r4 %08X r5 %08X r6 %08X r7 %08X r8 %08X r9 %08X r10 %08X r11 %08X "
                        "r12 %08X r23 %08X r24 %08X r25 %08X r26 %08X r27 %08X r28 %08X r29 %08X r30 %08X r31 %08X last indirect "
                        "target %08X\r\nguest back chain:",
                        uint32_t(g->lr), g->ctr.u32, g->r1.u32, g->r3.u32, g->r4.u32, g->r5.u32, g->r6.u32, g->r7.u32, g->r8.u32,
                        g->r9.u32, g->r10.u32, g->r11.u32, g->r12.u32, g->r23.u32, g->r24.u32, g->r25.u32, g->r26.u32, g->r27.u32,
                        g->r28.u32, g->r29.u32, g->r30.u32, g->r31.u32, g->last_indirect_target);
      uint32_t sp = g->r1.u32;
      for (int i = 0; i < 24 && mem && sp >= 0x10000u && n < int(sizeof(buf)) - 32; ++i) {
        if (!HostReadable(mem->TranslateVirtual<const uint8_t*>(sp), 4)) break;
        const uint32_t back = rex::memory::load_and_swap<uint32_t>(mem->TranslateVirtual<const uint8_t*>(sp));
        if (back <= sp || back - sp > 0x100000u || !HostReadable(mem->TranslateVirtual<const uint8_t*>(back - 8), 8)) break;
        n += std::snprintf(buf + n, sizeof(buf) - n, " < %08X",
                           rex::memory::load_and_swap<uint32_t>(mem->TranslateVirtual<const uint8_t*>(back - 8)));
        sp = back;
      }
      std::snprintf(buf + n, sizeof(buf) - n, "\r\n");
      RawOut(buf);
      if (v2) {  // v0.8.1 R34: what the engine knows about the memory the crashing code was looking at
        const uint32_t ptrs[] = {g->r3.u32, g->r4.u32, g->r5.u32, g->r6.u32, g->r7.u32, g->r8.u32, g->r9.u32, g->r10.u32, g->r11.u32,
                                 g->r23.u32, g->r24.u32, g->r25.u32, g->r26.u32, g->r27.u32, g->r28.u32, g->r29.u32, g->r30.u32, g->r31.u32};
        PointerNotesRaw(buf, int(sizeof(buf)), ptrs, int(std::size(ptrs)));
        RawOut(buf);
      }
    } else {
      RawOut("guest: this thread has no guest context\r\n");
    }
  }
  ULONG_PTR lo = 0, hi = 0;
  GetCurrentThreadStackLimits(&lo, &hi);
  // host call chain by unwinding (.pdata, no symbols needed: burstlimit.map names the offsets)
  n = std::snprintf(buf, sizeof(buf), "unwind:");
  CONTEXT c = *cr;
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
  const uint64_t* st = reinterpret_cast<const uint64_t*>(cr->Rsp);
  const int words = cr->Rsp >= lo && cr->Rsp < hi ? int((hi - cr->Rsp) / 8) : 0;
  uint32_t last = GuestOfHost(uintptr_t(rip));
  for (int i = 0, found = 0; i < 6000 && i < words && found < 40 && n < int(sizeof(buf)) - 64; ++i) {
    const uint32_t g = GuestOfHost(uintptr_t(st[i]));
    if (g && g != last) { n += std::snprintf(buf + n, sizeof(buf) - n, " < %08X", g); last = g; ++found; }
  }
  std::snprintf(buf + n, sizeof(buf) - n, "\r\n");
  RawOut(buf);
  if (v2) {
    EngineStateRaw(buf, int(sizeof(buf)));
    RawOut(buf);
  }
  BurstLimitTexRingsRaw(buf, int(sizeof(buf)));  // v0.8.1: what released / requested textures last
  RawOut(buf);
  FlushFileBuffers(g_hang_raw);
  busy = false;
}
// review L1: a stack overflow is recorded with one fixed line only (the full record needs ~10 KB of the exhausted stack)
void StackOverflowLine(const EXCEPTION_POINTERS* ep) {
  static std::atomic<bool> once{false};
  if (once.exchange(true)) return;
  char line[160];
  uint64_t off = 0;
  const char* mod = ModNameRaw(ep->ContextRecord->Rip, &off);
  std::snprintf(line, sizeof(line), "\r\nCRASH thread %lu STACK OVERFLOW at %s+%llX (guest function %08X)\r\n", GetCurrentThreadId(), mod,
                (unsigned long long)off, GuestOfHost(uintptr_t(ep->ContextRecord->Rip)));
  RawOut(line);
}
LONG WINAPI CrashFilter(PEXCEPTION_POINTERS ep) {
  if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_STACK_OVERFLOW && REXCVAR_GET(rb2_crash_first_chance_filter)) {
    StackOverflowLine(ep);
    return EXCEPTION_CONTINUE_SEARCH;
  }
  CrashRecord("unhandled exception", ep->ContextRecord, ep->ExceptionRecord);
  return EXCEPTION_CONTINUE_SEARCH;
}
// review L1: the last vectored handler sees FIRST-CHANCE exceptions, also those a driver / overlay DLL catches itself.
// It records only faults in our own code (the exe = recompiled guest code and the game, rexruntime, rexgpu); a
// fault anywhere else is left to the unhandled-exception filter (it is recorded only if nobody handles it).
uint64_t g_own_lo[3] = {}, g_own_hi[3] = {};
void CacheOwnModules() {
  const char* names[3] = {nullptr, "rexruntime.dll", "rexgpu-xenos.dll"};
  for (int k = 0; k < 3; ++k) {
    HMODULE h = names[k] ? GetModuleHandleA(names[k]) : GetModuleHandleA(nullptr);
    MODULEINFO mi = {};
    if (h && K32GetModuleInformation(GetCurrentProcess(), h, &mi, sizeof(mi))) {
      g_own_lo[k] = uint64_t(mi.lpBaseOfDll);
      g_own_hi[k] = g_own_lo[k] + mi.SizeOfImage;
    }
  }
}
bool OwnCode(uint64_t rip) {
  for (int k = 0; k < 3; ++k)
    if (rip >= g_own_lo[k] && rip < g_own_hi[k]) return true;
  return false;
}
thread_local bool t_abort_recorded = false;
void AbortRecord(const char* what) {
  if (t_abort_recorded) return;
  t_abort_recorded = true;
  CONTEXT c;
  RtlCaptureContext(&c);
  CrashRecord(what, &c, nullptr);
}
void BuildFuncTable() {
  if (!g_func_table.empty()) return;
  std::vector<std::pair<uintptr_t, uint32_t>> table;
  for (size_t i = 0; PPCFuncMappings[i].guest != 0; ++i)
    table.emplace_back(reinterpret_cast<uintptr_t>(PPCFuncMappings[i].host), uint32_t(PPCFuncMappings[i].guest));
  std::sort(table.begin(), table.end());
  g_func_table = std::move(table);
}
void CacheModules();
void CrashInstall() {
  static std::atomic<bool> installed{false};
  if (installed.exchange(true)) return;
  BuildFuncTable();
  CacheModules();
  CacheOwnModules();
  OpenHangRaw();
  SetUnhandledExceptionFilter(CrashFilter);
  // also the LAST vectored handler: an access violation the SDK's handler (MMIO, write watches) didn't take is
  // about to kill the process - on49 the top-level filter never ran (something replaces it)
  AddVectoredExceptionHandler(0, [](PEXCEPTION_POINTERS ep) -> LONG {
    const DWORD code = ep->ExceptionRecord->ExceptionCode;
    const bool fatal = code == EXCEPTION_ACCESS_VIOLATION ||
                       (REXCVAR_GET(rb2_crash_record) &&
                        (code == EXCEPTION_STACK_OVERFLOW || code == EXCEPTION_ILLEGAL_INSTRUCTION || code == EXCEPTION_IN_PAGE_ERROR ||
                         code == EXCEPTION_INT_DIVIDE_BY_ZERO || code == EXCEPTION_PRIV_INSTRUCTION));
    if (fatal && REXCVAR_GET(rb2_crash_first_chance_filter)) {
      if (code == EXCEPTION_STACK_OVERFLOW) { StackOverflowLine(ep); return EXCEPTION_CONTINUE_SEARCH; }
      if (!OwnCode(ep->ContextRecord->Rip)) return EXCEPTION_CONTINUE_SEARCH;  // a foreign DLL's own (maybe handled) fault
    }
    if (fatal) CrashRecord("exception (last vectored handler)", ep->ContextRecord, ep->ExceptionRecord);
    return EXCEPTION_CONTINUE_SEARCH;
  });
  if (REXCVAR_GET(rb2_crash_record)) {
    std::signal(SIGABRT, [](int) { AbortRecord("abort (REX_FATAL / assert / std::abort)"); });
    std::set_terminate([]() {
      AbortRecord("std::terminate (an exception nobody caught)");
      std::abort();
    });
    _set_purecall_handler([]() {
      AbortRecord("pure virtual call");
      std::abort();
    });
    _set_invalid_parameter_handler([](const wchar_t*, const wchar_t*, const wchar_t*, unsigned, uintptr_t) {
      AbortRecord("invalid parameter (C runtime)");
      std::abort();
    });
  }
}
// rb2_crash_test: the test of the record (main thread: from the main-loop hook; render: the render loop head)
std::atomic<int> g_crash_test_render{0};
void CrashTestFire() {
  const std::string kind = REXCVAR_GET(rb2_crash_test);
  REXLOG_WARN("[RB2] rb2_crash_test: {} on the {} thread", kind, ThreadRole());
  rex::FlushLogging();
  auto* runtime = rex::Runtime::instance();
  auto* m = runtime ? runtime->memory() : nullptr;
  if (!m) return;
  if (kind == "av") {
    volatile uint32_t v = *reinterpret_cast<volatile uint32_t*>(m->virtual_membase());  // guest address 0
    (void)v;
  } else if (kind == "null_call") {
    PPCContext* c = rex::runtime::current_ppc_context();
    if (c) {
      c->last_indirect_target = 0;
      rex::runtime::ResolveIndirectFunction(0)(*c, m->virtual_membase());
    }
  } else if (kind == "abort") {
    std::abort();
  } else if (kind == "fatal") {
    REX_FATAL("rb2_crash_test fatal (frame {})", REXCVAR_GET(rb2_crash_test_frame));
  }
}
void CrashTestTick(bool main_thread) {
  static const bool armed = !REXCVAR_GET(rb2_crash_test).empty();
  if (!armed) return;
  static std::atomic<int64_t> frames{0};
  if (main_thread) {
    if (frames.fetch_add(1) + 1 != REXCVAR_GET(rb2_crash_test_frame)) return;
    if (REXCVAR_GET(rb2_crash_test_thread) == "render") g_crash_test_render.store(1);
    else CrashTestFire();
  } else if (g_crash_test_render.exchange(0)) {
    CrashTestFire();
  }
}
std::atomic<bool> g_dump_request{false};
REXCVAR_DEFINE_INT32(rb2_dump_frame, 0, "Debug", "Rollback (rollback3): dump every guest thread's call chain at this engine frame (and 600 frames later), 0 off");
void WatchMain() {
  BuildFuncTable();
  static std::vector<std::pair<uintptr_t, uint32_t>> table;
  table = g_func_table;
  AddVectoredExceptionHandler(1, FaultChain);
  CrashInstall();  // (already done at the first main-loop frame with rb2_crash_record)
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
void BurstLimitOnlinePeekInbox();  // burstlimit_netinput.cpp (R19)
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
// v0.8.1 R10: the mark bytes are shared by every thread (page byte: one bit per write class; fine masks: one bit per
// 16-byte block). `if (!(*d & b)) *d |= b` from two threads could lose one of two first marks of a byte in the same
// frame (a page became "foreign only" and a main-thread write was never undone, or a foreign block was restored).
// The OR is atomic in the slow path only (bit not set yet): no cost in the steady state.
inline void MarkOr(unsigned char* d, unsigned char b) {
  if ((__atomic_load_n(d, __ATOMIC_RELAXED) & b) != b) __atomic_fetch_or(d, b, __ATOMIC_RELAXED);
}
// take-and-clear of a 32-byte fine mask (TrkEndFrame): a bit another thread sets meanwhile is either taken or kept
inline void MarkTake32(uint8_t* mask, uint8_t* out) {
  for (int q = 0; q < 4; ++q) {
    const uint64_t v = __atomic_exchange_n(reinterpret_cast<uint64_t*>(mask + q * 8), uint64_t(0), __ATOMIC_RELAXED);
    std::memcpy(out + q * 8, &v, 8);
  }
}
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
  unsigned char bit = ((flags & 1) == 0 && (rb2_tbit == 1 || rb2_tbit == 4)) ? 1 : 2;
  // R39: a file read of another thread (flags & 4, the SDK's NtReadFile notes) and any host write of a thread in
  // the fill class are data written once: this frame's marks (rb2_lfine), not the sticky foreign ones
  if (bit == 2 && (flags & 1) == 0 && (rb2_tbit == 32 || ((flags & 4) && REXCVAR_GET(rb2_ffine_age) == 1))) bit = 32;
  const uint64_t end = uint64_t(a) + len;
  for (uint64_t p = a & ~0xFFFull; p < end; p += 0x1000) {
    MarkOr(&rb2_dirty[(p >> 12) & 0xFFFFF], bit);
  }
  if (bit == 32) {
    for (uint64_t b = a >> 4; b <= ((end - 1) >> 4); ++b) MarkOr(&rb2_lfine[(b >> 3) & 0x1FFFFFF], uint8_t(1u << (b & 7)));
  } else if (bit == 2) {
    for (uint64_t b = a >> 4; b <= ((end - 1) >> 4); ++b) MarkOr(&rb2_ffine[(b >> 3) & 0x1FFFFFF], uint8_t(1u << (b & 7)));
  } else {
    for (uint64_t b = a >> 4; b <= ((end - 1) >> 4); ++b) MarkOr(&rb2_mfine[(b >> 3) & 0x1FFFFFF], uint8_t(1u << (b & 7)));
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
// v0.8.1 R15: 120 up to build 133 (2 s of lockstep at delay 2 after every FIGHT! = slow motion at a high ping). The
// early crashes that made it 120 were the effects node ring (R34: at the start of a fight its read index is in the
// slots a rewind did not restore). With the ring restored, rewinds from frame 21 on are clean on four pairs (r133_s21_*).
REXCVAR_DEFINE_INT32(rb2_settle, 30, "Debug", "Rollback (rollback3): engine frames after FIGHT! before any rewind may target a frame (120 up to build 133)");
int64_t SettleFrames() { return std::max(REXCVAR_GET(rb2_settle), 21); }  // 21 as with the old 20-frame ring: a rewind needs its logs anyway (TrkRewind refuses without them)
int64_t g_cine_frame[kRing];  // engine frame that ran in a cinematic (supers, Drama Pieces...), -1 not
int64_t g_cine_rewinds = 0;
struct Undo {
  int64_t frame = 0;
  std::vector<uint32_t> pages;
  std::vector<uint8_t> bytes;
  std::vector<uint8_t> fmask;  // per page: 32 bytes, the 16-byte blocks other threads wrote in the frame
  std::vector<uint32_t> fonly_pages;  // R39 (rb2_ffine_age): pages only other threads wrote in the frame (not logged) ...
  std::vector<uint8_t> fonly;         // ... and their 16-byte blocks, 32 bytes per page
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
  int match_max_depth = 0;       // R8: since the last match summary
  double match_cost_peak = 0;
  int64_t match_over_budget = 0;  // rollbacks whose restore + resim took more than a frame (16.7 ms)
  std::vector<uint8_t> blobs[kRing];
  std::vector<uint8_t> stacks[kRing];  // the main thread's stack below the hook's frame at each frame start
  uint32_t cosmetic[kRing] = {};       // the main thread's cosmetic random state at each frame start
  uint32_t stack_sp[kRing] = {};
  int64_t blob_frame[kRing] = {};
  int64_t fp_same = 0, fp_differ = 0;
  std::vector<int32_t> last_fgn;  // R39: per tracked page, the last frame whose log found the foreign page bit
  int64_t fonly_pages = 0, fonly_lost = 0;
  std::vector<std::pair<uint32_t, std::array<uint8_t, 32>>> fill_recheck;  // fill pages of the frame before (late stores)
  int64_t fill_pages = 0, fill_blocks = 0, fill_late = 0, sound_heap_blocks = 0, sound_heap_logged = 0;
  int64_t stale_blocks = 0, stale_heap_blocks = 0, stale_rewinds = 0, stale_logged = 0;
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
// v0.8.1 R18: the undo log is the engine's only allocation that grows with the frame (about 200 pages = 0.8 MB per
// frame, 64 frames kept). It used to grow by doubling from nothing in every frame, and a std::bad_alloc there
// ended the process without a record. Now: the buffers are reserved from the size of the previous frame, and a
// failed allocation ends ROLLBACK (not the game) until the engine's next pause: the logs are dropped, the shadow
// is copied whole again (the body stopped in the middle), no log is kept and nothing is predicted - the fight
// waits for the real inputs, as it does in the settle frames.
REXCVAR_DEFINE_INT32(rb2_fail_alloc_at, 0, "Debug", "Rollback test (R18): the undo log of this engine frame fails with std::bad_alloc after 40 pages (0 off)");
REXCVAR_DEFINE_BOOL(rb2_oom_clear_fill, true, "Debug", "Rollback (review L2): the out-of-memory recovery also clears the fill-class block marks of the tracked pages (off = builds up to 152)");
std::atomic<bool> g_log_oom{false};
int64_t g_log_oom_count = 0, g_log_oom_frame = -1;
size_t g_log_reserve = 256;
void TrkEndFrameBody(int64_t frame, bool keep);
void TrkEndFrame(int64_t frame, bool keep) {
  if (g_log_oom.load(std::memory_order_relaxed)) keep = false;
  try {
    TrkEndFrameBody(frame, keep);
  } catch (const std::bad_alloc&) {
    g_log_oom.store(true, std::memory_order_relaxed);
    ++g_log_oom_count;
    g_log_oom_frame = frame;
    // review L2: nothing in this handler may allocate (a second bad_alloc here would end the process): the logs go
    // by swap (frees, allocates nothing), and the line is a fixed buffer. The fill-class block marks are cleared with
    // the dirty bytes (they are taken only with their page's fill bit, which is cleared here: left set, every later
    // rewind kept those blocks live - the stale-mark tear of R39).
    decltype(t.logs)().swap(t.logs);
    t.fill_recheck.clear();
    {
      PoolHold hold(g_mem);
      for (uint32_t g : t.gp) rb2_dirty[g] = 0;  // before the copy: a write during it stays marked
      if (REXCVAR_GET(rb2_oom_clear_fill))
        for (size_t i = 0; i < s.host.size(); ++i) std::memset(&rb2_lfine[size_t(s.addr[i]) >> 7], 0, 32);
      for (size_t i = 0; i < s.host.size(); ++i) std::memcpy(t.shadow.data() + i * kPage, s.host[i], kPage);
    }
    g_log_reserve = 64;
    try {
      REXLOG_ERROR("[RB2] OUT OF MEMORY while logging engine frame {} (failure {} of this run): rollback is off until the "
                   "engine's next pause (round end / cinematic) - no predictions, no rewinds; the fight goes on with the real inputs",
                   frame, g_log_oom_count);
      rex::FlushLogging();
    } catch (...) {
    }
  }
}
void TrkEndFrameBody(int64_t frame, bool keep) {
  const auto a = Clock::now();
  if (REXCVAR_GET(rb2_alias_diag)) AliasDiag();
  PoolHold hold(g_mem);
  Undo u;
  u.frame = frame;
  if (keep) {
    u.pages.reserve(g_log_reserve);
    u.bytes.reserve(g_log_reserve * size_t(kPage));
    u.fmask.reserve(g_log_reserve * 32);
  }
  const int fail_at = REXCVAR_GET(rb2_fail_alloc_at);
  const bool res_touch = g_res_main_touch.exchange(false, std::memory_order_relaxed);
  u.quiet = g_workers_idle && g_pool_quiet && !(res_touch && ResGuardOn());
  if (res_touch && ResGuardOn()) ++g_res_logs_marked;
  if (!u.quiet) ++t.noisy_logs;
  const bool foreign_mode = REXCVAR_GET(rb2_foreign);
  const bool age_on = REXCVAR_GET(rb2_ffine_age) >= 2 && REXCVAR_GET(rb2_ffine_keep);
  // R39: the fill pages of the frame before - a store marked before that frame's take that landed after its page's
  // copy (mark, then store): the copy takes the block now, and the block counts as written in this frame
  std::unordered_map<uint32_t, std::array<uint8_t, 32>> late;
  if (!t.fill_recheck.empty()) {
    for (const auto& [pi, mask] : t.fill_recheck) {
      uint8_t* sh = t.shadow.data() + size_t(pi) * kPage;
      const size_t fo = size_t(s.addr[pi]) >> 7;
      for (int blk = 0; blk < 256; ++blk) {
        const uint8_t bm = uint8_t(1u << (blk & 7));
        if (!(mask[blk >> 3] & bm) || ((rb2_mfine[fo + (blk >> 3)] | rb2_lfine[fo + (blk >> 3)] | rb2_ffine[fo + (blk >> 3)] |
                                         rb2_sfine[fo + (blk >> 3)]) & bm)) continue;
        if (std::memcmp(sh + blk * 16, s.host[pi] + blk * 16, 16) == 0) continue;
        if (t.fill_late < 24)
          Log(fmt::format("fill: block {:08X} changed after its page's copy without a mark (frame {}): {:08X} {:08X} -> {:08X} {:08X}",
                          s.addr[pi] + uint32_t(blk) * 16, frame, rex::memory::load_and_swap<uint32_t>(sh + blk * 16),
                          rex::memory::load_and_swap<uint32_t>(sh + blk * 16 + 4), rex::memory::load_and_swap<uint32_t>(s.host[pi] + blk * 16),
                          rex::memory::load_and_swap<uint32_t>(s.host[pi] + blk * 16 + 4)));
        std::memcpy(sh + blk * 16, s.host[pi] + blk * 16, 16);
        late[pi][blk >> 3] |= bm;
        ++t.fill_late;
      }
    }
    t.fill_recheck.clear();
  }
  for (size_t i = 0; i < t.gp.size(); ++i) {
    unsigned char& d = rb2_dirty[t.gp[i]];
    if (!d) continue;
    unsigned char bits = __atomic_exchange_n(&d, static_cast<unsigned char>(0), __ATOMIC_RELAXED);
    if (bits & 34) t.last_fgn[i] = int32_t(frame);
    uint8_t ltaken[32] = {};  // R39: the fill class's blocks of this frame on this page
    bool lany = false;
    if (bits & 32) {
      MarkTake32(&rb2_lfine[size_t(s.addr[i]) >> 7], ltaken);
      for (int q = 0; q < 32; ++q) lany |= ltaken[q] != 0;
      if (lany && keep && !s.excluded[i]) {
        std::array<uint8_t, 32> mk;
        std::memcpy(mk.data(), ltaken, 32);
        t.fill_recheck.emplace_back(uint32_t(i), mk);
        ++t.fill_pages;
        for (int q = 0; q < 32; ++q) t.fill_blocks += __builtin_popcount(ltaken[q]);
      }
    }
    if (!late.empty()) {
      if (const auto it = late.find(uint32_t(i)); it != late.end()) {
        for (int q = 0; q < 32; ++q) ltaken[q] |= it->second[q];
        lany = true;
        late.erase(it);
      }
    }
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
      if (keep && (age_on || lany)) {  // R39: block marks that stay with this frame (the page is not logged)
        uint8_t rec[32];
        std::memcpy(rec, ltaken, 32);
        bool any = lany;
        if (age_on) {  // test (rb2_ffine_age = 2): every foreign mark of the page
          uint8_t* fp = &rb2_ffine_prev[size_t(s.addr[i]) >> 7];
          uint8_t taken[32];
          MarkTake32(ff, taken);
          bool tany = false, prev_any = false;
          for (int q = 0; q < 32; ++q) { tany |= taken[q] != 0; prev_any |= fp[q] != 0; }
          if (prev_any && REXCVAR_GET(rb2_lost_marks) && !pool_page) {
            for (int blk = 0; blk < 256; ++blk) {
              const uint8_t bm = uint8_t(1u << (blk & 7));
              if (!(fp[blk >> 3] & bm) || (taken[blk >> 3] & bm)) continue;
              if (std::memcmp(sh + blk * 16, s.host[i] + blk * 16, 16) != 0) { taken[blk >> 3] |= bm; tany = true; ++t.fonly_lost; }
            }
          }
          if (tany) {
            std::memcpy(fp, taken, 32);  // the generation the lost-mark gate of the page's next log looks at
            for (int q = 0; q < 32; ++q) rec[q] |= taken[q];
            any = true;
          }
        }
        if (any) {
          u.fonly_pages.push_back(uint32_t(i));
          u.fonly.insert(u.fonly.end(), rec, rec + 32);
          ++t.fonly_pages;
        }
      }
      std::memcpy(sh, s.host[i], kPage);
      // a page only other threads wrote in this frame is not logged - its foreign marks must SURVIVE until the
      // page's next log (MEASURED rnd36-rnd50: cleared here, a later restore of an older log of the page put back
      // the blocks the draw worker had written in a frame without a log -> its allocator / queue went back and
      // the screen froze at the first rewinds; the same gap undid CRI lock ops)
      if (!keep || !REXCVAR_GET(rb2_ffine_keep)) MarkTake32(ff, &rb2_ffine_prev[size_t(s.addr[i]) >> 7]);
      continue;
    }
    if (fail_at > 0 && keep && frame == fail_at && u.pages.size() == 40) throw std::bad_alloc();  // test (R18)
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
    const bool ff_all = REXCVAR_GET(rb2_ffine_all);
    uint8_t fp_old[32], taken[32];
    std::memcpy(fp_old, fp, 32);
    if (ff_all) {
      MarkTake32(ff, fp);  // first: a mark set from here on belongs to the next log
      std::memcpy(taken, fp, 32);
    } else {
      std::memcpy(taken, ff, 32);
    }
    bool ff_any = false;
    for (int q = 0; q < 32; ++q) ff_any |= taken[q] != 0;
    if (ff_all && ff_any && !(bits & 2) && !prev_any && !lost_any && !pool_page) {
      ++g_ffine_nobit_pages;
      for (int q = 0; q < 32; ++q) g_ffine_nobit_blocks += __builtin_popcount(taken[q]);
    }
    if (((bits & 2) || prev_any || lost_any || (ff_all && ff_any) || lany) && !pool_page) {
      uint8_t m[32];
      for (int q = 0; q < 32; ++q) m[q] = uint8_t(taken[q] | lost[q] | (prev_on ? fp_old[q] : 0) | ltaken[q]);  // R39: + this frame's fill blocks
      u.fmask.insert(u.fmask.end(), m, m + 32);
      ++t.foreign_mixed;
    } else {
      u.fmask.insert(u.fmask.end(), 32, 0);
    }
    if (!ff_all) MarkTake32(ff, fp);  // the generation this log clears (lost-mark gate; rb2_ffine_prev_on: kept live too)
    std::memcpy(sh, s.host[i], kPage);
  }
  if (keep) {
    for (const auto& [pi, mask] : late) {  // R39: late fill stores on pages nobody wrote in this frame
      u.fonly_pages.push_back(pi);
      u.fonly.insert(u.fonly.end(), mask.begin(), mask.end());
    }
    g_log_reserve = std::max<size_t>(64, u.pages.size() + u.pages.size() / 4 + 16);
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
double g_park_wait_ms_sum = 0, g_park_wait_ms_peak = 0, g_park_wait_ms_peak_match = 0;
REXCVAR_DEFINE_BOOL(rb2_park_render, true, "Debug", "Rollback (rollback3): the render loop thread waits at its loop head while a rewind restores and resimulates");
REXCVAR_DEFINE_BOOL(rb2_park_early, true, "Debug", "Rollback (rollback3): the render loop is asked to park before the frame log of a rewind frame (shorter waits)");
REXCVAR_DEFINE_INT32(rb2_park_wait_ms, 250, "Debug", "Rollback (rollback3): how long a rewind waits for the render loop to park (v0.8.0: 40, then it went on unparked)");
// v0.8.1 R9: (1) the hand-shake carries a generation: the main thread waits for the park of ITS request (with the
// two plain flags a request made right after a release could read the stale "parked" of the previous one while the
// render loop was leaving its park: memory restored under a running scene walk). (2) Strict: a rewind whose park
// did not come in time is NOT run unparked - it stays pending (no new predictions meanwhile) and is retried at the
// next frame; it runs unparked only when its target is about to leave the engine's ring.
REXCVAR_DEFINE_BOOL(rb2_park_strict, true, "Debug", "Rollback: a rewind never restores memory under a running render loop - it waits for the park and is retried next frame (off = v0.8.0: go on after rb2_park_wait_ms)");
std::atomic<uint64_t> g_park_gen{0}, g_parked_gen{0};
std::atomic<uint64_t> g_park_deferred{0};
bool g_park_deferred_now = false;  // a rewind is pending because the render loop was not parked in time
// v0.8.1 review M1: a deferred rewind keeps g_park_req = 1 so that the render loop parks at its next loop head. If the
// engine stops tracking before the rewind runs (pause for a cinematic / KO / round end, the match or the battle scene
// ends), nothing cleared it: the render loop stayed parked for good (frozen picture). Every path that stops tracking
// releases it now (the logs are dropped at a pause, the rewind cannot run any more), and the render side gives up a
// park after rb2_park_max_ms in any case.
REXCVAR_DEFINE_BOOL(rb2_park_release, true, "Debug", "Rollback (review M1): a park request left by a deferred rewind is released when the engine stops tracking (off = builds up to 152)");
REXCVAR_DEFINE_INT32(rb2_park_max_ms, 2000, "Debug", "Rollback (review M1): the render loop leaves a park after this many ms whatever the request says (0 = no limit, builds up to 152)");
REXCVAR_DEFINE_INT32(rb2_park_defer_test, 0, "Debug", "Rollback test (review M1): at this live engine frame a deferred rewind is simulated (park requested, rewind pending) and the engine stops tracking for the rest of the fight (0 off)");
std::atomic<uint64_t> g_park_released{0}, g_park_max_hits{0};
bool g_park_defer_test_stop = false;
void ReleaseDeferredPark(const char* why);
double g_park_pre_ms = 0;          // waited in TrkFrameA already (strict), added to the rewind's park wait
std::atomic<uint64_t> g_park_timed_out_gen{0};  // review M1: the request generation the render loop gave up on
void ParkRequest() {  // idempotent while a request is outstanding (a new one after the render loop gave up on it)
  if (!g_park_req.load() || g_park_gen.load() == g_park_timed_out_gen.load()) {
    g_park_gen.fetch_add(1);
    g_park_req.store(1);
  }
}
bool ParkedNow() {
  if (!g_parked.load()) return false;
  return !REXCVAR_GET(rb2_park_strict) || g_parked_gen.load() == g_park_gen.load();
}
void ReleaseDeferredPark(const char* why) {
  if (!REXCVAR_GET(rb2_park_release) || (!g_park_deferred_now && !g_park_req.load())) return;
  const bool deferred = g_park_deferred_now;
  g_park_deferred_now = false;
  g_park_req.store(0);
  const uint64_t n = ++g_park_released;
  if (n <= 20 || (n & (n - 1)) == 0)
    Log(fmt::format("park request released ({}): {} ({} so far)", deferred ? "a deferred rewind" : "an early request", why, n));
}
struct RenderPark {
  bool on = false;
  RenderPark() {
    if (!REXCVAR_GET(rb2_park_render)) return;
    on = true;
    ParkRequest();
    const auto t0 = Clock::now();
    while (!ParkedNow() && Ms(t0, Clock::now()) < REXCVAR_GET(rb2_park_wait_ms)) std::this_thread::yield();
    if (ParkedNow()) ++g_park_ok; else { ++g_park_timeouts; BurstLimitOnlineAlert("park time-out", fmt::format("the render loop did not park within {} ms", REXCVAR_GET(rb2_park_wait_ms))); }
    const double w = Ms(t0, Clock::now()) + g_park_pre_ms;
    g_park_pre_ms = 0;
    g_park_wait_ms_sum += w;
    g_park_wait_ms_peak = std::max(g_park_wait_ms_peak, w);
    g_park_wait_ms_peak_match = std::max(g_park_wait_ms_peak_match, w);
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
// ---- v0.8.1 R34: effects particle list audit (diagnostics, rb2_fx_audit) ------------------------------------
// The effects system (pool 0x840D6000-0x84141000): groups from [0x840DFCE0] to the sentinel 0x8413DD70 (next at +0),
// each with a NULL-ended node list (+92 first, +96 last; node: +0 next, +4 prev, +8 emitter); an emitter has three
// lists with a head sentinel (next at +H, prev at +H+4) and a tail sentinel 8 bytes on: children +40, particles
// waiting +60 (count u16 +92), particles alive +76 (count u16 +94). The crashes / hangs of R34 are such a list
// with a NULL / garbage link or a cycle right after a rewind. The audit walks them all at three points (live frame
// start, after a rewind's restore, after each resimulated frame) and logs the first failures with every block
// involved: its words, whether the last rewind restored its page, and why a block was kept live.
REXCVAR_DEFINE_INT32(rb2_fx_audit, 0, "Debug", "Rollback diagnostics (R34): walk every effect emitter's particle lists at each live frame start, after each rewind's restore and after each resimulated frame (0 off, 1 on)");
constexpr uint32_t kFxLo = 0x840D6000u, kFxHi = 0x84141000u;
std::unordered_map<uint32_t, uint16_t> g_fx_kept;      // 16-byte block (address >> 4) -> reasons | kept count << 8, last rewind
std::unordered_map<uint32_t, uint8_t> g_fx_restored;   // effects pool page (address >> 12) -> logs that restored it, last rewind
int64_t g_fx_audit_runs = 0, g_fx_audit_fails = 0, g_fx_audit_counts = 0, g_fx_rw_frame = -1;
int g_fx_rw_depth = 0;
// The node free ring (0x840D62C8, 512 slots, FIFO): its first 14 slots lay inside the never-restored range
// 840D6100-840D6300 up to build 132. A FIFO ring survives that only while the frames a rewind undoes freed no more
// nodes than were in use at the frame it goes back to (their frees then only wrote stale slots); if they freed
// more, a slot that is a valid free entry of the restored state keeps a node of the abandoned timeline: the same
// node is handed out twice (r132_a4, frame 2575: "free slots 8 and 20 both hold 840D73D8", two frames later a
// group list with that node linked twice). Counted at every rewind (three guest words): how often the bound is
// broken, and whether a slot it would hit is still in a live range (must be 0 with the default ranges).
int64_t g_fx_ring_hazards = 0, g_fx_ring_hazards_live = 0;
int g_fx_ring_max_frees = 0, g_fx_ring_min_inuse = 512;
uint32_t g_fx_node_w_before = 0;
void FxRingHazard(rex::memory::Memory* m, int64_t now, int n) {
  const uint32_t w_f = U32(m, 0x840D8AD8u), r_f = U32(m, 0x840D6ACCu);
  if (w_f >= 512 || r_f >= 512 || g_fx_node_w_before >= 512) return;
  const int frees = int((g_fx_node_w_before + 512 - w_f) % 512), in_use = int((r_f + 512 - w_f) % 512);
  g_fx_ring_max_frees = std::max(g_fx_ring_max_frees, frees);
  g_fx_ring_min_inuse = std::min(g_fx_ring_min_inuse, in_use);
  if (frees <= in_use) return;
  ++g_fx_ring_hazards;
  static std::vector<std::pair<uint32_t, uint32_t>> live_ranges;  // rb2_live_ranges, 16-byte blocks as the restore keeps them
  static std::string parsed;
  if (const std::string spec = REXCVAR_GET(rb2_live_ranges); spec != parsed || (live_ranges.empty() && !spec.empty())) {
    parsed = spec;
    live_ranges.clear();
    size_t q = 0;
    while (q < spec.size()) {
      size_t e = spec.find(',', q);
      if (e == std::string::npos) e = spec.size();
      const std::string item = spec.substr(q, e - q);
      if (const size_t d = item.find('-'); d != std::string::npos)
        live_ranges.emplace_back(uint32_t(std::strtoul(item.c_str(), nullptr, 16)) & ~15u,
                                 (uint32_t(std::strtoul(item.c_str() + d + 1, nullptr, 16)) + 15u) & ~15u);
      q = e + 1;
    }
  }
  bool live = false;  // one of the overwritten valid slots is never restored
  for (int q = 0; q < frees - in_use; ++q) {
    const uint32_t slot_addr = 0x840D62C8u + ((r_f + uint32_t(q)) % 512) * 4;
    for (const auto& [lo, hi] : live_ranges) live = live || (slot_addr >= lo && slot_addr < hi);
  }
  if (live) ++g_fx_ring_hazards_live;
  if (g_fx_ring_hazards <= 4 || live)
    Log(fmt::format("fx node ring: rewind at frame {} depth {} undid {} node frees with only {} nodes in use at its target (R {} W {}): {} valid free "
                    "slots from {} were written by the abandoned frames{}", now, n, frees, in_use, r_f, w_f, frees - in_use, r_f,
                    live ? " - AND one of them is in a never-restored range (rb2_live_ranges): the ring is torn" : " (all restored)"));
}
std::string FxBlock(rex::memory::Memory* m, uint32_t a) {
  if (a < kFxLo || a >= kFxHi) return fmt::format(" [{:08X}: outside the pool]", a);
  const uint32_t b = a & ~15u;
  const size_t blk = size_t(b) >> 4;
  const int bit = int(blk & 7);
  std::string why = "-";
  int kept_n = 0;
  if (const auto it = g_fx_kept.find(uint32_t(blk)); it != g_fx_kept.end()) {
    why.clear();
    const int r = it->second & 0xFF;
    kept_n = it->second >> 8;
    if (r & 1) why += "S";   // sound library block
    if (r & 2) why += "F";   // foreign mark in an undone frame's log
    if (r & 4) why += "N";   // foreign mark since the last log
    if (r & 8) why += "L";   // lost-mark block (changed without any mark)
    if (r & 16) why += "R";  // live range
  }
  const auto rp = g_fx_restored.find(b >> 12);
  return fmt::format(" [{:08X}: {:08X} {:08X} {:08X} {:08X} | last rewind: page in {} logs, block kept {} x{} | marks now f{} p{} m{} s{} page bits {:02X}]", a,
                     U32(m, b), U32(m, b + 4), U32(m, b + 8), U32(m, b + 12), rp != g_fx_restored.end() ? int(rp->second) : 0, why, kept_n,
                     (rb2_ffine[blk >> 3] >> bit) & 1, (rb2_ffine_prev[blk >> 3] >> bit) & 1, (rb2_mfine[blk >> 3] >> bit) & 1,
                     (rb2_sfine[blk >> 3] >> bit) & 1, unsigned(rb2_dirty[b >> 12]));
}
// ---- v0.8.1 R31 (rb2_res_audit): the resource cache's two node lists across a restore --------------------------
// Cache 0x82724600: list headers at +960 (free nodes) and +984 (queued objects), each {.., .., .., count +12,
// pointer to the head word +16, pointer to the tail word +20}, the header's address is the lists' sentinel; nodes
// {prev +0, next +4, object +8}. Only the render loop writes them (push sub_820EBB10, remove sub_820EBCB0, under
// the lock cache+928). The frozen screens (r125_c1, r137_d1: 0 rewinds over file queue work, 0 creates sent again)
// are the render loop in sub_820EBCB0 walking +984 without ever meeting its sentinel. The render loop is parked
// at a restore, so the lists can be read there: 1 = compare them before and after every restore and log what
// the restore changed (it should change nothing: nobody but the render loop writes them), 2 = also put the
// words the render loop left back (guest memory and the engine's shadow).
REXCVAR_DEFINE_INT32(rb2_res_audit, 1, "Debug",
                     "Rollback (R31): the resource cache's node lists before / after every restore: 0 off, 1 log what a restore "
                     "changed, 2 also keep the render loop's words");
struct ResSnap {
  std::vector<std::pair<uint32_t, uint32_t>> words;
  bool whole = true;
  int n[2] = {0, 0};
  std::string err;
};
int64_t g_res_audit_runs = 0, g_res_restore_events = 0, g_res_words_changed = 0, g_res_torn_before = 0, g_res_torn_by_restore = 0,
        g_res_words_kept = 0;
ResSnap ResLists(rex::memory::Memory* m) {
  ResSnap r;
  const uint32_t cache = 0x82724600u;
  if (!Committed(m, cache + 928, 96)) { r.whole = false; r.err = " cache not mapped"; return r; }
  for (uint32_t a = cache + 960; a < cache + 1008; a += 4) r.words.emplace_back(a, U32(m, a));
  int idx = 0;
  for (uint32_t hdr : {cache + 960, cache + 984}) {
    int n = 0;
    uint32_t prev = hdr;
    const uint32_t head_at = U32(m, hdr + 16);
    bool ok = Committed(m, head_at, 4);
    for (uint32_t node = ok ? U32(m, head_at) : hdr; node != hdr && U32(m, hdr + 12) != 0; node = U32(m, node + 4)) {
      if (!Committed(m, node, 12) || (node & 3) || ++n > 4096) {
        ok = false;
        r.err += fmt::format(" list +{}: bad node {:08X} after {:08X} (#{})", hdr - cache, node, prev, n);
        break;
      }
      if (prev != hdr && U32(m, node) != prev) {
        ok = false;
        r.err += fmt::format(" list +{}: node {:08X} prev {:08X}, expected {:08X}", hdr - cache, node, U32(m, node), prev);
      }
      r.words.emplace_back(node, U32(m, node));
      r.words.emplace_back(node + 4, U32(m, node + 4));
      r.words.emplace_back(node + 8, U32(m, node + 8));
      prev = node;
    }
    if (ok && int32_t(U32(m, hdr + 12)) != n) {
      ok = false;
      r.err += fmt::format(" list +{}: {} nodes walked, count {}", hdr - cache, n, U32(m, hdr + 12));
    }
    r.whole = r.whole && ok;
    r.n[idx++] = n;
  }
  return r;
}
ResSnap g_res_pre;
// the keep decision of every undone log for the pages the lists live in (filled by the restore loop)
struct ResRec { int64_t frame; uint32_t page; uint8_t keep[32], funion[32], fnow[32], fprev[32], lost[32], sfine[32], mfine[32]; uint8_t flags; };
std::vector<ResRec> g_res_recs;
std::vector<uint32_t> g_res_pages;
bool ResPage(uint32_t page) { return std::find(g_res_pages.begin(), g_res_pages.end(), page) != g_res_pages.end(); }
std::string ResWhy(uint32_t a) {
  std::string out = fmt::format(" [{:08X} marks now f{} p{} m{} dirty {:02X}; logs undone with its page:", a,
                                (rb2_ffine[a >> 7] >> ((a >> 4) & 7)) & 1, (rb2_ffine_prev[a >> 7] >> ((a >> 4) & 7)) & 1,
                                (rb2_mfine[a >> 7] >> ((a >> 4) & 7)) & 1, unsigned(rb2_dirty[a >> 12]));
  const int blk = int((a & 0xFFFu) >> 4), q = blk >> 3;
  const uint8_t bm = uint8_t(1u << (blk & 7));
  int n = 0;
  for (const ResRec& r : g_res_recs) {
    if (r.page != (a & ~0xFFFu)) continue;
    ++n;
    out += fmt::format(" {}:{}{}{}{}{}{}{}{}", r.frame, (r.keep[q] & bm) ? "KEPT" : "restored", (r.funion[q] & bm) ? " F" : "", (r.fnow[q] & bm) ? " N" : "",
                       (r.fprev[q] & bm) ? " P" : "", (r.lost[q] & bm) ? " L" : "", (r.sfine[q] & bm) ? " S" : "", (r.mfine[q] & bm) ? " M" : "",
                       (r.flags & 1) ? " pool" : (r.flags & 2) ? "" : " foreign-off");
  }
  if (!n) out += " none";
  return out + "]";
}
void ResAuditBefore(rex::memory::Memory* m, int64_t now) {
  if (REXCVAR_GET(rb2_res_audit) <= 0) return;
  ++g_res_audit_runs;
  g_res_recs.clear();
  g_res_pre = ResLists(m);
  g_res_pages.clear();
  for (const auto& [a, v] : g_res_pre.words) {
    if (g_res_pages.size() < 16 && !ResPage(a & ~0xFFFu)) g_res_pages.push_back(a & ~0xFFFu);
  }
  if (!g_res_pre.whole) {
    const int64_t k = ++g_res_torn_before;
    if (k <= 6 || (k & (k - 1)) == 0)
      Log(fmt::format("res audit: the cache lists are NOT whole BEFORE the restore at frame {} (#{}; free {} queued {}):{}", now, k, g_res_pre.n[0],
                      g_res_pre.n[1], g_res_pre.err));
  }
}
void ResAuditAfter(rex::memory::Memory* m, int64_t now, int depth) {
  if (REXCVAR_GET(rb2_res_audit) <= 0) return;
  int changed = 0;
  std::string list;
  for (const auto& [a, v] : g_res_pre.words) {
    const uint32_t cur = U32(m, a);
    if (cur == v) continue;
    ++changed;
    if (changed <= 8) list += fmt::format(" {:08X}: {:08X} -> {:08X}", a, v, cur);
    if (changed <= 3 || changed == 12) list += ResWhy(a);
    if (REXCVAR_GET(rb2_res_audit) >= 2) {  // the render loop's word stays (guest memory and the shadow of the frame start)
      uint8_t* host = m->TranslateVirtual<uint8_t*>(a);
      rex::memory::store_and_swap<uint32_t>(host, v);
      const auto it = std::upper_bound(s.ranges.begin(), s.ranges.end(), a, [](uint32_t x, const Range& r) { return x < r.address; });
      if (it != s.ranges.begin()) {
        const Range& r = *(it - 1);
        if (a >= r.address && a + 4 <= r.address + r.size) std::memcpy(t.shadow.data() + r.offset + (a - r.address), host, 4);
      }
      ++g_res_words_kept;
    }
  }
  if (!changed) return;
  const int64_t k = ++g_res_restore_events;
  g_res_words_changed += changed;
  const ResSnap post = ResLists(m);
  const bool tore = g_res_pre.whole && !post.whole;
  if (tore) ++g_res_torn_by_restore;
  if (k <= 12 || (k & (k - 1)) == 0 || tore)
    Log(fmt::format("res audit: the restore at frame {} (depth {}) changed {} words of the cache lists (#{}; before: free {} queued {} {}; after{}: free {} "
                    "queued {} {}{}):{}", now, depth, changed, k, g_res_pre.n[0], g_res_pre.n[1], g_res_pre.whole ? "whole" : "TORN",
                    REXCVAR_GET(rb2_res_audit) >= 2 ? " (the words were put back)" : "", post.n[0], post.n[1], post.whole ? "whole" : "TORN", post.err, list));
}
std::string ResAuditStats() {
  return fmt::format("res audit: runs {} restores that changed the cache lists {} ({} words, {} put back), lists torn by a restore {}, torn before one {}; "
                     "foreign marks kept on logged pages without the foreign bit: {} pages {} blocks",
                     g_res_audit_runs, g_res_restore_events, g_res_words_changed, g_res_words_kept, g_res_torn_by_restore, g_res_torn_before,
                     g_ffine_nobit_pages, g_ffine_nobit_blocks);
}
// true = every list is whole. point: "live" / "restore" / "resim"; k = the resimulated frame's index
bool FxAudit(rex::memory::Memory* m, const char* point, int64_t frame, int k) {
  ++g_fx_audit_runs;
  auto ok_ptr = [](uint32_t a) { return a >= kFxLo && a + 96 <= kFxHi && !(a & 3); };
  int fails = 0, groups = 0, emitters = 0;
  int64_t elements = 0;
  static std::vector<uint32_t> used_nodes, used_emitters, used_particles;
  used_nodes.clear();
  used_emitters.clear();
  used_particles.clear();
  auto fail = [&](const std::string& what, std::initializer_list<uint32_t> blocks) {
    ++fails;
    ++g_fx_audit_fails;
    if (g_fx_audit_fails > 24 && (g_fx_audit_fails & (g_fx_audit_fails - 1)) != 0) return;
    std::string bl;
    for (uint32_t a : blocks) bl += FxBlock(m, a);
    Log(fmt::format("fx audit: FAIL #{} at {} (frame {}, resim index {}; last rewind at frame {} depth {}): {} |{}", g_fx_audit_fails, point, frame, k,
                    g_fx_rw_frame, g_fx_rw_depth, what, bl));
  };
  for (uint32_t g = U32(m, 0x840DFCE0u); g != 0x8413DD70u && fails < 6; g = U32(m, g)) {
    if (!ok_ptr(g) || ++groups > 4096) { fail(fmt::format("group list: bad group {:08X} (#{})", g, groups), {0x840DFCE0u, g}); break; }
    uint32_t prevn = 0;
    int nodes = 0;
    for (uint32_t node = U32(m, g + 92); node && fails < 6; node = U32(m, node)) {
      if (!ok_ptr(node) || ++nodes > 4096) { fail(fmt::format("group {:08X}: bad node {:08X} (#{}, after {:08X})", g, node, nodes, prevn), {g + 92, prevn, node}); break; }
      if (U32(m, node + 4) != prevn) fail(fmt::format("group {:08X}: node {:08X} prev {:08X}, expected {:08X}", g, node, U32(m, node + 4), prevn), {g + 92, prevn, node});
      prevn = node;
      used_nodes.push_back(node);
      const uint32_t e = U32(m, node + 8);
      if (!ok_ptr(e)) { fail(fmt::format("group {:08X} node {:08X}: bad emitter {:08X}", g, node, e), {node}); continue; }
      ++emitters;
      used_emitters.push_back(e);
      if (const uint32_t one = U32(m, e + 36); one >= 0x840EDD70u && one < 0x8413DD70u) used_particles.push_back(one);
      static const struct { uint32_t head; const char* name; } lists[3] = {{40, "children"}, {60, "waiting"}, {76, "alive"}};
      int len[3] = {};
      bool whole = true;
      for (int li = 0; li < 3 && fails < 6; ++li) {
        const uint32_t head = e + lists[li].head, end = head + 8;
        uint32_t prev = head, p = U32(m, head);
        while (p != end) {
          if (!ok_ptr(p)) {
            fail(fmt::format("emitter {:08X} (flags {:08X}, def {:08X}) list {}: element #{} is {:08X} (after {:08X})", e, U32(m, e + 8), U32(m, e + 24),
                             lists[li].name, len[li], p, prev), {head, end, prev, p});
            whole = false;
            break;
          }
          if (++len[li] > 4096) {
            fail(fmt::format("emitter {:08X} list {}: no end after 4096 elements (a cycle), at {:08X}", e, lists[li].name, p), {head, end, prev, p});
            whole = false;
            break;
          }
          if (U32(m, p + 4) != prev) {
            fail(fmt::format("emitter {:08X} (flags {:08X}) list {}: element #{} {:08X} has prev {:08X}, expected {:08X}", e, U32(m, e + 8), lists[li].name,
                             len[li], p, U32(m, p + 4), prev), {head, end, prev, p, U32(m, p + 4)});
            whole = false;
          }
          if (li > 0 && p >= 0x840EDD70u && p < 0x8413DD70u) used_particles.push_back(p);
          prev = p;
          p = U32(m, p);
        }
        if (p == end && U32(m, end + 4) != prev) {
          fail(fmt::format("emitter {:08X} (flags {:08X}) list {}: the tail's prev is {:08X}, the last element {:08X} ({} elements)", e, U32(m, e + 8),
                           lists[li].name, U32(m, end + 4), prev, len[li]), {head, end, prev, U32(m, end + 4)});
          whole = false;
        }
        elements += len[li];
      }
      // counts (not a failure by themselves: the meaning of +92 differs for emitters of emitters)
      if (whole && (U16(m, e + 92) != len[1] || U16(m, e + 94) != len[2]) && U16(m, e + 92) != len[0] + len[1]) {
        if (++g_fx_audit_counts <= 12)
          Log(fmt::format("fx audit: count mismatch #{} at {} (frame {}, resim index {}): emitter {:08X} flags {:08X} counts +92 {} +94 {}, lists children {} waiting {} alive {} |{}{}",
                          g_fx_audit_counts, point, frame, k, e, U32(m, e + 8), U16(m, e + 92), U16(m, e + 94), len[0], len[1], len[2], FxBlock(m, e + 60), FxBlock(m, e + 76)));
      }
    }
  }
  // The free rings (FIFO: alloc reads ring[R++], free writes ring[W++], no count): the free entries [R, W) must be
  // distinct and none of them may be in use (in a list) - else the same node / emitter / particle is handed out twice.
  struct Ring { const char* name; uint32_t base, n, r, w; const std::vector<uint32_t>* used; };
  const Ring rings[3] = {{"node", 0x840D62C8u, 512, 0x840D6ACCu, 0x840D8AD8u, &used_nodes},
                         {"emitter", 0x840DF4E0u, 512, 0x840D6AD4u, 0x840D8ADCu, &used_emitters},
                         {"particle", 0x840D8CE0u, 2560, 0x840DFD64u, 0x840DFD60u, &used_particles}};
  std::string ring_state;
  for (const Ring& rg : rings) {
    const uint32_t r = U32(m, rg.r), w = U32(m, rg.w);
    ring_state += fmt::format(" {} R {} W {} used {}", rg.name, r, w, rg.used->size());
    if (r >= rg.n || w >= rg.n) { fail(fmt::format("{} ring: index out of range R {} W {}", rg.name, r, w), {rg.r, rg.w}); continue; }
    if (fails) continue;  // the lists are torn already: the used sets are not complete
    uint32_t free_n = (w + rg.n - r) % rg.n;
    if (free_n == 0 && rg.used->empty()) free_n = rg.n;
    std::unordered_map<uint32_t, uint32_t> seen;  // entry -> slot
    for (uint32_t v : *rg.used) seen.emplace(v, 0xFFFFFFFFu);
    for (uint32_t q = 0; q < free_n && fails < 6; ++q) {
      const uint32_t slot = (r + q) % rg.n, v = U32(m, rg.base + slot * 4);
      const auto ins = seen.emplace(v, slot);
      if (ins.second) continue;
      if (ins.first->second == 0xFFFFFFFFu)
        fail(fmt::format("{} ring: free slot {} (R {} W {}) holds {:08X}, which is IN USE (in a list)", rg.name, slot, r, w, v), {rg.base + slot * 4, v, rg.r, rg.w});
      else
        fail(fmt::format("{} ring: free slots {} and {} (R {} W {}) both hold {:08X}", rg.name, ins.first->second, slot, r, w, v),
             {rg.base + ins.first->second * 4, rg.base + slot * 4, rg.r, rg.w});
    }
  }
  if (g_fx_audit_runs <= 3 || g_fx_audit_runs % 3000 == 0)
    Log(fmt::format("fx audit: run {} at {} (frame {}): {} groups, {} emitters, {} list elements; rings:{}; failures so far {}, count mismatches {}", g_fx_audit_runs,
                    point, frame, groups, emitters, elements, ring_state, g_fx_audit_fails, g_fx_audit_counts));
  return fails == 0;
}

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
  BurstLimitRb2FileGateRewind(now - n, now);  // R30: counts a rewind over a frame of file queue work (R31: the stress's too)
  g_last_rw_frame = now;
  g_last_rw_depth = n;
  g_last_rw_tick = GetTickCount64();
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
  g_fx_node_w_before = U32(m, 0x840D8AD8u);  // R34: the node ring's write index before the restore
  const bool fx_audit = REXCVAR_GET(rb2_fx_audit) > 0;
  if (fx_audit) {
    g_fx_kept.clear();
    g_fx_restored.clear();
    g_fx_rw_frame = now;
    g_fx_rw_depth = n;
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
    for (size_t k = 0; k < u.fonly_pages.size() && u.fonly.size() >= (k + 1) * 32; ++k) {  // R39: pages the frame did not log
      const uint8_t* fm = u.fonly.data() + k * 32;
      auto& un = funion[u.fonly_pages[k]];
      for (int q = 0; q < 32; ++q) un[q] |= fm[q];
    }
  }
  std::unordered_set<uint32_t> stale_seen;  // R39 diagnostics: blocks kept on a stale foreign mark, this rewind
  BurstLimitRb2HeapRewind(now, n);
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
  ResAuditBefore(m, now);  // R31
  BurstLimitRb2HeapAudit(m, "before the restore", now, -1);  // R39
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
      uint8_t keepmask[32], fbmask[32];
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
        const uint8_t fb = foreign_on ? uint8_t((ffine ? ffine[q] : 0) | fnow[q] | (REXCVAR_GET(rb2_ffine_prev_on) ? fprev[q] : 0) | lostr[q] |
                                                rb2_lfine[(size_t(s.addr[i]) >> 7) + q]) : 0;
        keepmask[q] = sb | fb;
        fbmask[q] = fb;
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
      if (!g_res_pages.empty() && ResPage(s.addr[i]) && g_res_recs.size() < 64) {  // R31 diagnostics
        ResRec r{};
        r.frame = u.frame;
        r.page = s.addr[i];
        std::memcpy(r.keep, keepmask, 32);
        if (ffine) std::memcpy(r.funion, ffine, 32);
        std::memcpy(r.fnow, fnow, 32);
        std::memcpy(r.fprev, fprev, 32);
        std::memcpy(r.lost, lostr, 32);
        std::memcpy(r.sfine, sfine, 32);
        std::memcpy(r.mfine, mfr, 32);
        r.flags = uint8_t((pool_page ? 1 : 0) | (foreign_on ? 2 : 0));
        g_res_recs.push_back(r);
      }
      if (fx_audit && s.addr[i] >= kFxLo && s.addr[i] < kFxHi) {  // R34 diagnostics: why a block of the effects pool stays live
        ++g_fx_restored[s.addr[i] >> 12];
        for (int blk = 0; blk < 256; ++blk) {
          const int q = blk >> 3;
          const uint8_t bm = uint8_t(1u << (blk & 7));
          if (!(keepmask[q] & bm)) continue;
          uint16_t why = 0;
          if (REXCVAR_GET(rb2_sound_fine) && !pool_page && (sfine[q] & bm)) why |= 1;
          if (foreign_on) {
            if (ffine && (ffine[q] & bm)) why |= 2;
            if (fnow[q] & bm) why |= 4;
            if (lostr[q] & bm) why |= 8;
          }
          if (!why) why = 16;
          uint16_t& rec = g_fx_kept[(s.addr[i] >> 4) + uint32_t(blk)];
          rec = uint16_t((rec & 0xFF) | why | ((((rec >> 8) + 1) & 0xFF) << 8));
        }
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
      if (any && BurstLimitRb2HeapPage(s.addr[i])) {
        // R39 diagnostics: game heap blocks kept live on a SOUND-library mark only (those marks are never cleared)
        // whose bytes differ from the image
        for (int blk = 0; blk < 256; ++blk) {
          const uint8_t bm = uint8_t(1u << (blk & 7));
          if (!(keepmask[blk >> 3] & bm) || (fbmask[blk >> 3] & bm) || !(sfine[blk >> 3] & bm)) continue;
          if (std::memcmp(s.host[i] + blk * 16, old + blk * 16, 16) == 0) continue;
          ++t.sound_heap_blocks;
          if (t.sound_heap_logged < 8) {
            ++t.sound_heap_logged;
            const uint8_t* lv = s.host[i] + blk * 16;
            const uint8_t* ov = old + blk * 16;
            Log(fmt::format("SOUND mark kept game heap block {:08X} live in the rewind {} -> {} (log of frame {}): image {:08X} {:08X} {:08X} "
                            "{:08X}, kept {:08X} {:08X} {:08X} {:08X}", s.addr[i] + uint32_t(blk) * 16, now, now - n, u.frame,
                            rex::memory::load_and_swap<uint32_t>(ov), rex::memory::load_and_swap<uint32_t>(ov + 4),
                            rex::memory::load_and_swap<uint32_t>(ov + 8), rex::memory::load_and_swap<uint32_t>(ov + 12),
                            rex::memory::load_and_swap<uint32_t>(lv), rex::memory::load_and_swap<uint32_t>(lv + 4),
                            rex::memory::load_and_swap<uint32_t>(lv + 8), rex::memory::load_and_swap<uint32_t>(lv + 12)));
          }
        }
      }
      if (anyf) {
        // R39 diagnostics: a block kept live on a foreign mark, whose bytes differ from the image, on a page that no
        // other thread wrote in any frame this rewind undoes (nor in the running one) = a write of the main thread
        // (or of a job) that this restore does not take back.
        const bool fresh = t.last_fgn[i] >= int32_t(now - n) || (rb2_dirty[s.addr[i] >> 12] & 34) != 0;
        const bool heap_page = BurstLimitRb2HeapPage(s.addr[i]);
        if (!fresh || heap_page) {
          for (int blk = 0; blk < 256; ++blk) {
            const uint8_t bm = uint8_t(1u << (blk & 7));
            if (!(fbmask[blk >> 3] & bm) || std::memcmp(s.host[i] + blk * 16, old + blk * 16, 16) == 0) continue;
            int why = 0;
            if (ffine && (ffine[blk >> 3] & bm)) why |= 2;
            if (fnow[blk >> 3] & bm) why |= 4;
            if (lostr[blk >> 3] & bm) why |= 8;
            if (!fresh) why |= 32;
            if (heap_page) BurstLimitRb2HeapKept(s.addr[i] + uint32_t(blk) * 16, why);
            if (fresh || !stale_seen.insert((s.addr[i] >> 4) + uint32_t(blk)).second) continue;
            ++t.stale_blocks;
            if (heap_page) ++t.stale_heap_blocks;
            if (t.stale_logged < REXCVAR_GET(rb2_stale_log)) {
              ++t.stale_logged;
              const uint8_t* lv = s.host[i] + blk * 16;
              const uint8_t* ov = old + blk * 16;
              Log(fmt::format("STALE foreign mark kept block {:08X} live in the rewind {} -> {} (log of frame {}): the page's last foreign "
                              "store was in frame {}{}; image {:08X} {:08X} {:08X} {:08X}, kept {:08X} {:08X} {:08X} {:08X} (reasons{}{}{})",
                              s.addr[i] + uint32_t(blk) * 16, now, now - n, u.frame, t.last_fgn[i], heap_page ? ", GAME HEAP" : "",
                              rex::memory::load_and_swap<uint32_t>(ov), rex::memory::load_and_swap<uint32_t>(ov + 4),
                              rex::memory::load_and_swap<uint32_t>(ov + 8), rex::memory::load_and_swap<uint32_t>(ov + 12),
                              rex::memory::load_and_swap<uint32_t>(lv), rex::memory::load_and_swap<uint32_t>(lv + 4),
                              rex::memory::load_and_swap<uint32_t>(lv + 8), rex::memory::load_and_swap<uint32_t>(lv + 12),
                              (why & 2) ? " log-mask" : "", (why & 4) ? " marks-now" : "", (why & 8) ? " lost-mark" : ""));
            }
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
  FxRingHazard(m, now, n);
  ResAuditAfter(m, now, int(n));  // R31
  if (fx_audit) FxAudit(m, "restore", now, -1);  // R34: the state the restore left, before any resimulated frame
  if (!stale_seen.empty()) ++t.stale_rewinds;
  BurstLimitRb2HeapAudit(m, "after the restore", now, -1);  // R39
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
  BurstLimitTexRewindBegin(now - n, now);
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
    if (fx_audit) FxAudit(m, "resim", now, k);
    BurstLimitRb2HeapAudit(m, "after resim frame", now, k);  // R39
    const double ms = Ms(b, Clock::now());
    peak = std::max(peak, ms);
    sum += ms;
    TrkEndFrame(now - n + k, true);
    t.cosmetic[(now - n + k + 1) % kRing] = g_cosmetic_main;
  }
  BurstLimitTexRewindEnd();
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
    t.match_max_depth = std::max(t.match_max_depth, n);
    t.match_cost_peak = std::max(t.match_cost_peak, restore + sum);
    if (restore + sum > 16.7) ++t.match_over_budget;
    if (restore + sum > 33.0)
      BurstLimitOnlineAlert("slow rollback", fmt::format("a rollback of depth {} took {:.1f} ms (restore {:.1f}, resimulation {:.1f}, slowest "
                                                         "frame {:.1f}) at engine frame {}", n, restore + sum, restore, sum, peak, now));
    return true;
  }
  // compare with the original timeline
  size_t bad_main = 0, bad_other = 0;
  // R40 diagnostics: every differing word (also of pages only other threads wrote and of excluded pages) of the
  // rewinds after an effect script's sync point (rb2_diff_fx) or in a frame window (rb2_diff_frames)
  bool diff_all = false;
  if (g_diff_fx_left > 0) { diff_all = true; --g_diff_fx_left; }
  if (!diff_all && !REXCVAR_GET(rb2_diff_frames).empty()) {
    static int64_t lo = -1, hi = -1;
    if (lo < 0) {
      unsigned long long a = 0, b = 0;
      if (std::sscanf(REXCVAR_GET(rb2_diff_frames).c_str(), "%llu-%llu", &a, &b) == 2) { lo = int64_t(a); hi = int64_t(b); } else { lo = hi = 0; }
    }
    diff_all = now >= lo && now <= hi && hi > 0;
  }
  if (diff_all) {
    size_t pages = 0, words_total = 0;
    for (size_t i = 0; i < s.host.size(); ++i) {
      const uint8_t* want = t.full.data() + i * kPage;
      if (std::memcmp(s.host[i], want, kPage) == 0) continue;
      ++pages;
      const uint64_t* l = reinterpret_cast<const uint64_t*>(s.host[i]);
      const uint64_t* w = reinterpret_cast<const uint64_t*>(want);
      std::string words;
      int nw = 0, all = 0;
      for (uint32_t q = 0; q < kPage / 8; ++q) {
        if (l[q] == w[q]) continue;
        ++all;
        if (nw < 32) {
          words += fmt::format(" {:03X}:{:016X}->{:016X}", q * 8, __builtin_bswap64(w[q]), __builtin_bswap64(l[q]));
          ++nw;
        }
      }
      words_total += size_t(all);
      Log(fmt::format("R40 diff #{} frame {} depth {} page {:08X} {}{} words {}:{}", t.rewinds + 1, now, n, s.addr[i],
                      s.excluded[i] ? "excluded" : t.ever_main[i] ? "main" : "other", "", all, words));
    }
    Log(fmt::format("R40 diff #{} frame {} depth {}: {} pages, {} words differ", t.rewinds + 1, now, n, pages, words_total));
  }
  std::string detail;
  int shown = 0;
  for (size_t i = 0; i < s.host.size(); ++i) {
    if (s.excluded[i]) continue;
    const uint8_t* want = t.full.data() + i * kPage;
    if (std::memcmp(s.host[i], want, kPage) == 0) continue;
    if (!t.ever_main[i]) { ++bad_other; continue; }
    ++bad_main;
    if ((REXCVAR_GET(rb2_log_diffs) && t.rewinds < 3) || diff_all) {  // every differing 8-byte word of the first rewinds
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

// ---- v0.8.1 R17 step 1: is the tracked page set still the committed writable guest memory? -----------------
// Setup runs once per process (the first rollback fight): memory the game commits later (another stage, other
// characters, mod slots) is never logged or restored. Log-only check at every new fight.
REXCVAR_DEFINE_INT32(rb2_rescan, 1, "Debug", "Rollback (R17): at each new fight compare the committed writable guest memory with the tracked page set and log what was added / removed (0 off, 1 log only)");
int64_t g_rescan_added_kb = 0, g_rescan_removed_kb = 0, g_rescan_runs = 0;
bool g_no_battle_seen = false;  // no battle (or no battle scene) since the last check, or a new online match
REXCVAR_DEFINE_INT32(rb2_rescan_at, 0, "Debug", "Rollback test: run the tracked-set check at this engine frame (0 off)");
void RescanLog(rex::memory::Memory* m, const char* why) {
  if (REXCVAR_GET(rb2_rescan) <= 0 || !t.ready) return;
  const auto a = Clock::now();
  const std::vector<Range> now_r = WritableRanges(m);
  auto diff = [](const std::vector<Range>& x, const std::vector<Range>& y, std::string& list) {  // bytes of x that y lacks
    uint64_t total = 0;
    int shown = 0;
    size_t j = 0;
    for (const Range& r : x) {
      uint64_t lo = r.address;
      const uint64_t hi = uint64_t(r.address) + r.size;
      while (lo < hi) {
        while (j < y.size() && uint64_t(y[j].address) + y[j].size <= lo) ++j;
        if (j < y.size() && y[j].address <= lo) { lo = std::min<uint64_t>(hi, uint64_t(y[j].address) + y[j].size); continue; }
        const uint64_t e = j < y.size() ? std::min<uint64_t>(hi, y[j].address) : hi;
        total += e - lo;
        if (shown++ < 12) list += fmt::format(" {:08X}-{:08X}", lo, e);
        lo = e;
      }
    }
    return total;
  };
  std::string added, removed;
  const uint64_t add = diff(now_r, s.ranges, added), rem = diff(s.ranges, now_r, removed);
  ++g_rescan_runs;
  g_rescan_added_kb = int64_t(add >> 10);
  g_rescan_removed_kb = int64_t(rem >> 10);
  if (!add && !rem) {
    Log(fmt::format("tracked set check ({}): unchanged, {} ranges, {:.1f} MB ({:.1f} ms)", why, s.ranges.size(), double(TotalSize(s.ranges)) / 1048576.0,
                    Ms(a, Clock::now())));
  } else {
    Log(fmt::format("tracked set check ({}): CHANGED since the engine's setup - committed now but NOT tracked {} KB:{}{} | tracked but no longer committed {} KB:{}{} ({:.1f} ms)",
                    why, add >> 10, added.empty() ? " -" : added, add ? " ..." : "", rem >> 10, removed.empty() ? " -" : removed, rem ? " ..." : "",
                    Ms(a, Clock::now())));
  }
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
  bool cinematic = battle && REXCVAR_GET(rb2_no_cinematics) &&
                   (U16(m, battle + 574) & 2) != 0;
  // rb2_phases: battle phases the engine runs in (bit = phase: 1 READY?, 2 FIGHT!, 3 round end / KO)
  const uint32_t phase = battle ? U32(m, 0x841B5228) : 0;
  // R40: an effect-script cinematic (supers' / ultimates' cut-scenes: script tasks 8212AB78 of priority 133000xx) -
  // these do not set battle+574 bit 2, so the engine tracked and rewound through them
  bool fx_cine = false;
  if (battle && (REXCVAR_GET(rb2_fx_cine) > 0 || !REXCVAR_GET(rb2_snap_cine).empty())) {
    if (const uint32_t list = U32(m, 0x84140E90)) {
      int walked = 0;
      for (uint32_t task = U32(m, list + 32); task && ++walked < 400; task = U32(m, task + 40))
        if (U32(m, task + 32) == 0x8212AB78u && (U32(m, task + 24) & 0xFFFFFF00u) == 0x13300000u) { fx_cine = true; break; }
    }
    static bool was_fx = false;
    static int64_t ticks = 0, start_tick = -1, end_tick = -1;
    ++ticks;
    unsigned snap_a = 0, snap_b = 0;
    const bool snap_on = !REXCVAR_GET(rb2_snap_cine).empty() && t.ready &&
                         std::sscanf(REXCVAR_GET(rb2_snap_cine).c_str(), "%u,%u", &snap_a, &snap_b) == 2 && snap_a > 0;
    static std::vector<uint8_t> snap0;  // the tracked pages at the start of the current cinematic (R40 diagnostics)
    static bool snapped = false;        // the first cinematic that lasted snap_a ticks has its snapshots
    if (fx_cine != was_fx) {
      std::string ids;  // the effect resource table (0x841584E0 + 2440, 40 x 24 bytes: id, in use, ...) - R16b
      for (uint32_t e = 0x841584E0u + 2440u; e < 0x841584E0u + 3400u; e += 24)
        if (U32(m, e + 4) != 0) ids += fmt::format(" {}:{:08X}", int32_t(U32(m, e)), U32(m, e + 8));
      Log(fmt::format("R40: effect-script cinematic {} at engine frame {} (main tick {}); effect resource ids:{}", fx_cine ? "starts" : "ends", s.frame, ticks, ids));
      if (fx_cine) start_tick = ticks;
      if (!fx_cine && snapped && end_tick < 0) end_tick = ticks;
      was_fx = fx_cine;
      if (fx_cine && snap_on && !snapped) {
        snap0.resize(s.host.size() * size_t(kPage));
        for (size_t i = 0; i < s.host.size(); ++i) std::memcpy(snap0.data() + i * kPage, s.host[i], kPage);
      }
    }
    if (snap_on) {  // R40 diagnostics: snapshots of every tracked page (start, start + a ticks, end + b ticks)
      auto write = [&](int which, const uint8_t* (*page)(size_t, const std::vector<uint8_t>&)) {
        const std::string path = BurstLimitDiagPath("burstlimit", fmt::format(".snap{}.bin", which).c_str());
        if (FILE* f = std::fopen(path.c_str(), "wb")) {
          for (size_t i = 0; i < s.host.size(); ++i) {
            const uint32_t addr = s.addr[i];
            std::fwrite(&addr, 4, 1, f);
            std::fwrite(page(i, snap0), 1, kPage, f);
          }
          std::fclose(f);
          Log(fmt::format("R40: snapshot {} ({} pages) written at engine frame {} (main tick {}): {}", which, s.host.size(), s.frame, ticks, path));
        }
      };
      if (!snapped && fx_cine && start_tick >= 0 && ticks == start_tick + int64_t(snap_a) && !snap0.empty()) {
        snapped = true;
        write(0, [](size_t i, const std::vector<uint8_t>& b) { return b.data() + i * kPage; });
        write(1, [](size_t i, const std::vector<uint8_t>&) { return static_cast<const uint8_t*>(s.host[i]); });
        std::vector<uint8_t>().swap(snap0);
      }
      if (snapped && end_tick >= 0 && snap_b > 0 && ticks == end_tick + int64_t(snap_b))
        write(2, [](size_t i, const std::vector<uint8_t>&) { return static_cast<const uint8_t*>(s.host[i]); });
    }
  }
  {  // round 6 (harness): one line when the battle reaches its results (phase 13): run_rb2.ps1 -AfterEndSec waits for it
    static uint32_t last_phase = 0;
    if (phase != last_phase) {
      if (phase >= 13 && last_phase < 13 && last_phase != 0) {
        Log(fmt::format("battle ended (phase {} -> {}, engine frame {})", last_phase, phase, s.frame));
        rex::FlushLogging();
      }
      last_phase = phase;
    }
  }
  if (fx_cine && REXCVAR_GET(rb2_fx_cine) > 0) cinematic = true;  // R40 test: the engine pauses in them
  bool fighting = battle && phase < 8 && ((REXCVAR_GET(rb2_phases) >> phase) & 1) && !cinematic &&
                  (!BurstLimitRollback2Online() || BurstLimitOnlineEngineDelayOk());  // a long cinematic's delay must be gone first
  if (g_park_defer_test_stop) fighting = false;  // test (rb2_park_defer_test): the engine stops tracking
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
    t.last_fgn.assign(s.host.size(), INT32_MIN);
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
  // v0.8.1 R17: the game reuses the battle object's address from match to match (r135_long: no check line in three
  // matches), so a new fight = the first battle object after the pointer was 0
  // (build 141: the engine's frame start is not even called while there is no battle scene - r140_long had no
  // check line in three matches - so the flag is also set by the frame hook's early return)
  if (const int at = REXCVAR_GET(rb2_rescan_at); at > 0 && s.frame == at) g_no_battle_seen = true;  // test
  if (!battle) {
    g_no_battle_seen = true;
  } else if (g_no_battle_seen) {
    g_no_battle_seen = false;
    RescanLog(m, "new fight");  // log only
  }
  const bool was = s.active;
  s.active = fighting && battle == s.battle;
  if (s.active != was) {
    Log(fmt::format("track: fight {} at frame {} (battle {:08X} phase {})", s.active ? "resumed" : "paused", s.frame, battle,
                    battle ? U32(m, 0x841B5228) : 0));
    rex::FlushLogging();
  }
  if (!s.active) {
    ReleaseDeferredPark("engine paused");  // review M1
    if (was && REXCVAR_GET(rb2_park_release)) BurstLimitRb2DropRewind();  // the logs are dropped: nothing to rewind to
    if (battle && battle != s.battle) {
      s.battle = battle;
      RescanLog(m, "new battle object");  // R17: log only
    }
    if (was) BurstLimitTexAudit(ctx, base, "engine pause");
    if (was) t.logs.clear();
    if (was && g_log_oom.exchange(false)) Log(fmt::format("out of memory at frame {}: rollback may be used again from the next resume", g_log_oom_frame));
    if (rb2_cri_on) cri::Reset();
    rb2_cri_on = 0;
    return;
  }
  if (!was) cri::Reset();  // no record from before the pause can be matched
  if (REXCVAR_GET(rb2_fx_audit) > 0) FxAudit(m, "live", s.frame, -1);  // R34 diagnostics: the lists as the last live frame left them
  BurstLimitRb2HeapAudit(m, "live frame start", s.frame, -1);  // R39 (rb2_heap_audit)
  if (s.frame % 600 == 0) CacheThreadIds();  // for the raw hang dump (new guest threads)
  if (s.frame % 1800 == 0) {
    Log(fmt::format("track: heartbeat frame {}, {} rewinds, fight state {} same / {} differ; refused: busy workers / resource cache {}, across a new thread {} ({} threads made, last start {:08X}); resource cache calls main {} other {}, frames marked {}; alias pages written untracked A {} C {} E {} (with a tracked alias {} / {} / {}), tracked alias pairs dirty together {}; lost-mark blocks kept {}",
                    s.frame, t.rewinds, t.fp_same, t.fp_differ, t.refused_noisy, t.refused_thread, g_threads_created.load(), g_thread_last_start.load(),
                    g_res_calls_main.load(), g_res_calls_other.load(), g_res_logs_marked.load(), g_alias_untracked[0], g_alias_untracked[1],
                    g_alias_untracked[2], g_alias_untracked_tracked_alias[0], g_alias_untracked_tracked_alias[1], g_alias_untracked_tracked_alias[2],
                    g_alias_both_tracked_dirty, g_lost_mark_blocks));
    Log(BurstLimitTexStats());
    rex::FlushLogging();
  }
  const int every = std::max(2, REXCVAR_GET(rb2_every));
  const int depth_max = std::clamp(REXCVAR_GET(rb2_sync), 1, kRing - 2);
  const bool due = s.frame > SettleFrames() + kRing && s.frame % every == 0;
  // Quiesce contract (rollback3): a rewind requested by the online / loopback path is known before the
  // frame's log, so the render thread and every worker are idle BEFORE the log too (rollback2 waited only
  // after it: a frame logged with job workers mid-write was then restored half-written - on1's job pool crash).
  if (BurstLimitRollback2Online()) BurstLimitOnlinePeekInbox();  // v0.8.1 R19: what arrived since the last tick
  const bool pending = (REXCVAR_GET(rb2_loop) > 0 || BurstLimitRollback2Online()) && BurstLimitRb2LoopPeekRewind() >= 0;
  if ((due || pending) && REXCVAR_GET(rb2_rewind_sleep) > 0)  // test: give the draw worker time to finish its frame
    std::this_thread::sleep_for(std::chrono::milliseconds(REXCVAR_GET(rb2_rewind_sleep)));
  // the render loop is asked to park now (the rewind comes right after this frame's log): it reaches its loop head
  // while the waits and the log run, so the rewind itself waits less (RenderPark, rb2_park_early)
  if ((due || pending) && REXCVAR_GET(rb2_park_render) && REXCVAR_GET(rb2_park_early)) ParkRequest();
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
  if (const int every = REXCVAR_GET(rb2_d3d_callers); every > 0) {  // R40 diagnostics
    rb2_d3d_watch = 2;
    static int64_t last = -1;
    if (s.frame / every != last) {
      last = s.frame / every;
      std::map<std::pair<uint32_t, uint32_t>, uint64_t> got;
      {
        std::lock_guard<std::mutex> lock(g_d3d_callers_mu);
        got.swap(g_d3d_callers);
      }
      std::string line;
      int shown = 0;
      for (const auto& [k, n] : got) {
        if (shown++ >= 120) break;
        line += fmt::format(" {:08X}<{:08X}:{}", k.first, k.second, n);
      }
      Log(fmt::format("R40 D3D callers up to engine frame {} ({} pairs):{}", s.frame, got.size(), line));
    }
  }
  if (REXCVAR_GET(rb2_task_log) && (s.frame % 10) == 0) {  // R40 diagnostics: the scheduler's task set, logged when it changes
    static std::vector<std::pair<uint32_t, uint32_t>> last;
    std::vector<std::pair<uint32_t, uint32_t>> now_tasks;
    if (const uint32_t list = U32(m, 0x84140E90)) {
      int walked = 0;
      for (uint32_t task = U32(m, list + 32); task && ++walked < 400; task = U32(m, task + 40))
        now_tasks.push_back({U32(m, task + 32), U32(m, task + 24)});
    }
    std::sort(now_tasks.begin(), now_tasks.end());
    if (now_tasks != last) {
      std::string add, del;
      for (const auto& x : now_tasks) if (!std::binary_search(last.begin(), last.end(), x)) add += fmt::format(" {:08X}/{:08X}", x.first, x.second);
      for (const auto& x : last) if (!std::binary_search(now_tasks.begin(), now_tasks.end(), x)) del += fmt::format(" {:08X}/{:08X}", x.first, x.second);
      Log(fmt::format("R40 tasks at engine frame {} ({} tasks): +{} -{}", s.frame, now_tasks.size(), add.substr(0, 1500), del.substr(0, 1500)));
      last = std::move(now_tasks);
    }
  }
  if ((REXCVAR_GET(rb2_loop) > 0 || REXCVAR_GET(rb2_stress_pad)) && (s.frame % 6) == 0) {  // player 1's scripted pad (moves, attacks, ki blasts)
    static const uint16_t kPattern[] = {0x0008, 0x0008, 0x4000, 0, 0x4000, 0x8000, 0, 0x2000, 0x0004, 0x4000,
                                        0x4000, 0x4000, 0x8000, 0x0001, 0x2000, 0x0002, 0x1000, 0, 0x0008, 0x0100};
    auto* runtime = rex::Runtime::instance();
    if (auto* input = runtime ? static_cast<rex::input::InputSystem*>(runtime->input_system()) : nullptr) {
      // rb2_loop_tr = N: every N engine frames the pad presses LB + RB (transformation) for 12 frames
      const int tr = REXCVAR_GET(rb2_loop_tr);
      const bool press_tr = tr > 0 && s.frame >= tr && (s.frame % tr) < 12;
      const int ult = REXCVAR_GET(rb2_loop_ult);  // R40 test: the ultimate (RT + Y + B) and its effect-script cinematic
      const bool press_ult = !press_tr && ult > 0 && s.frame >= ult && (s.frame % ult) < 12;
      if (press_ult) {
        input->InjectButtons(0, uint16_t(0x8000 | 0x2000), 120, 0, 255);
        // offline (versus2p): player 2's pad fires at the same frames - the two ultimates clash (R40 reproducer)
        if (REXCVAR_GET(rb2_loop) <= 0) input->InjectButtons(1, uint16_t(0x8000 | 0x2000), 120, 0, 255);
      }
      else
        input->InjectButtons(0, press_tr ? uint16_t(0x0300) : kPattern[(s.frame / 6) % std::size(kPattern)], press_tr ? 120 : 80);
    }
  }
  if (g_gate_rw_at >= 0 && s.frame >= g_gate_rw_at) {  // test (rb2_gate_rw_test): a rewind across a file request's first frames
    Log(fmt::format("TEST: forced rewind at frame {} to {} (rb2_gate_rw_test)", s.frame, s.frame - g_gate_rw_depth));
    BurstLimitRb2RequestRewind(s.frame - g_gate_rw_depth);
    g_gate_rw_at = -1;
  }
  bool park_deferred = false;
  if (REXCVAR_GET(rb2_loop) > 0 || BurstLimitRollback2Online()) {
    if (const int64_t want = BurstLimitRb2LoopPeekRewind();
        want >= 0 && REXCVAR_GET(rb2_park_strict) && REXCVAR_GET(rb2_park_render) && want >= SettleFrames() &&
        s.frame - want >= 1 && s.frame - want <= kRing - 2) {
      ParkRequest();
      const auto t0 = Clock::now();
      const double limit = g_park_deferred_now ? 20.0 : double(REXCVAR_GET(rb2_park_wait_ms));
      while (!ParkedNow() && Ms(t0, Clock::now()) < limit) std::this_thread::yield();
      g_park_pre_ms += Ms(t0, Clock::now());
      if (!ParkedNow() && s.frame - want < kRing - 6) {  // the rewind stays pending: retried at the next frame
        park_deferred = true;
        const uint64_t c = ++g_park_deferred;
        if (c <= 20 || (c & (c - 1)) == 0)
          Log(fmt::format("rewind DEFERRED at frame {} to {}: the render loop did not park within {:.0f} ms ({} deferred so far)",
                          s.frame, want, limit, c));
        BurstLimitOnlineAlert("rewind deferred", fmt::format("the render loop did not park within {:.0f} ms (engine frame {} to {})", limit, s.frame, want));
      }
    }
    g_park_deferred_now = park_deferred;
    if (const int pt = REXCVAR_GET(rb2_park_defer_test); pt > 0 && s.frame == pt && !g_park_defer_test_stop) {
      ParkRequest();  // test (review M1): what a deferred rewind leaves - the request set (kept at the frame's end) ...
      park_deferred = true;
      g_park_deferred_now = true;
      g_park_defer_test_stop = true;  // ... and the engine stops tracking from the next frame on (as at a pause / match end)
      Log(fmt::format("TEST: deferred rewind simulated at engine frame {}; the engine stops tracking (rb2_park_defer_test)", s.frame));
    }
    const int64_t to = park_deferred ? -1 : BurstLimitRb2LoopTakeRewind();
    if (to >= 0) {
      const int n = int(s.frame - to);
      if (!verify_online_log_done(n, to)) {
      }
      if (to < SettleFrames()) {
        ++t.online_refused;  // into the frames logged while the engine settled (online: not before)
        if (t.online_refused <= 20) Log(fmt::format("rewind REFUSED (settling) at frame {} to {}", s.frame, to));
        BurstLimitOnlineAlert("rewind refused", fmt::format("settling: engine frame {} to {}", s.frame, to));
      } else if (n >= 1 && n <= kRing - 2) {
        t.last_wait = WaitRender(m, 50) + WaitWorkers(m, 50);
        TrkRewind(m, ctx, base, n, false);
      } else if (n > 0) {
        ++t.online_too_deep;
        if (t.online_too_deep <= 20) Log(fmt::format("rewind REFUSED (too deep) at frame {} to {} (depth {})", s.frame, to, n));
        BurstLimitOnlineAlert("rewind refused", fmt::format("too deep: engine frame {} to {} (depth {})", s.frame, to, n));
      }
    }
    if (s.frame % 600 == 0 && s.frame) {
      Log(fmt::format("loop: frame {}: {} rollbacks (through cinematic frames {}, render loop parked {} / timed out {}, park wait {:.2f} ms avg / {:.1f} peak), avg depth {:.2f}, max {}, too deep {}, refused (settling) {}, cost {:.2f} ms avg / {:.2f} peak; {}",
                      s.frame, t.online_rewinds, g_cine_rewinds, g_park_ok.load(), g_park_timeouts.load(), g_park_wait_ms_sum / std::max<int64_t>(g_park_ok.load() + g_park_timeouts.load(), 1), g_park_wait_ms_peak, double(t.online_depth_sum) / std::max<int64_t>(t.online_rewinds, 1),
                      t.online_max_depth, t.online_too_deep, t.online_refused, t.online_cost_ms / std::max<int64_t>(t.online_rewinds, 1),
                      t.online_cost_peak, BurstLimitRb2LoopStats()));
      Log(CriStats());
      Log(BurstLimitTexStats());
      rex::FlushLogging();
    }
  } else if (due) {
    // R31: the offline stress obeys the invariant the engine guarantees online (the file gate): no rewind over a
    // frame in which the file request queue worked. It makes no prediction that could be held, so the rewind
    // itself is skipped (r125_c1: the CPU's Frieza transformed, forced rewinds crossed the queue's work, 4 reads
    // were started again, 9 creates sent again, and 40 frames later the render loop span in the resource cache's
    // queue = the frozen screen). The depth counter moves on: the schedule of the other rewinds is unchanged.
    const int n = 1 + (t.next_depth++ % depth_max);
    if (REXCVAR_GET(rb2_stress_gate) && BurstLimitRb2FileGateWorked(s.frame - n, s.frame)) {
      const int64_t c = ++g_stress_gate_skips;
      if (c <= 8 || (c & (c - 1)) == 0)
        Log(fmt::format("stress gate: rewind of depth {} at frame {} skipped - the file queue worked in a frame it would cross ({} skipped so far)",
                        n, s.frame, c));
    } else {
      TrkRewind(m, ctx, base, n);
    }
  }
  {  // v0.8.1 R2: the texture registry must be one consistent set after a rewind (H1: half live, half restored)
    static int64_t audited_rewinds = -1;
    const int64_t rewinds = t.rewinds + t.online_rewinds;
    const int audit = REXCVAR_GET(rb2_tex_audit);
    if (audit > 0 && (audit >= 2 || rewinds != audited_rewinds || s.frame % 600 == 0))
      BurstLimitTexAudit(ctx, base, audited_rewinds < 0 ? "engine start" : rewinds != audited_rewinds ? "after a rewind" : "periodic");
    audited_rewinds = rewinds;
  }
  if (!park_deferred) g_park_req.store(0);  // an early park request whose rewind was refused / not taken (a deferred
                                            // rewind keeps its request: the render loop parks at its next loop head)
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
REXCVAR_DEFINE_BOOL(rb2_pad_noengine, false, "Debug", "Rollback test (R40 control): the loopback's scripted pad, ki refill and player 2 replay without the engine (use with rb2_phases=0: never tracks)");
REXCVAR_DEFINE_BOOL(rb2_live_game_sched, false, "Debug", "Rollback test (R40): live engine frames run the game's own scheduler (only valid without rewinds: rb2_settle huge)");
REXCVAR_DEFINE_BOOL(rb2_sync_every_used, false, "Debug", "Rollback test (R40): internal guard of rb2_live_game_sched (leave false)");
REXCVAR_DEFINE_BOOL(rb2_draw_known, false, "Debug", "Rollback (R40 candidate): the live draw pass links only the tasks present at the frame's start (as the game's scheduler)");
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
  if (sync_on && REXCVAR_GET(rb2_live_game_sched) && !REXCVAR_GET(rb2_sync_every_used)) return false;  // R40 test (no rewinds only)
  PPCContext* ctx = rex::runtime::current_ppc_context();
  uint8_t* base = m->virtual_membase();
  const PPCContext saved = *ctx;
  if (sync_on) {
    const uint32_t list = SchedulerBegin(m);
    // R40 candidate (rb2_draw_known): the game's scheduler links every task before running any, so a draw task made by
    // an update task of this frame first runs in the NEXT frame; the split draw pass walked the list again after the
    // updates and ran such a task one frame early. Only the tasks present at the frame's start are linked.
    std::vector<std::pair<uint32_t, uint32_t>> known;
    const bool draw_known = REXCVAR_GET(rb2_draw_known);
    if (list && draw_known) {
      int walked = 0;
      for (uint32_t task = U32(m, list + 32); task && ++walked < 1024; task = U32(m, task + 40)) known.push_back({task, U32(m, task + 32)});
      std::sort(known.begin(), known.end());
    }
    if (list) sync::RealUpdate(m, *ctx, base, list);
    sync::DrawBegin(m);
    if (list) {
      uint32_t prev = 0;
      int walked = 0;
      for (uint32_t task = U32(m, list + 32); task; task = U32(m, task + 40)) {
        if (++walked > 1024) break;
        const uint32_t prio = U32(m, task + 24), cb = U32(m, task + 32);
        if (!(prio >= 0x20000000 && cb != 0x8217E100)) continue;
        if (draw_known && !std::binary_search(known.begin(), known.end(), std::make_pair(task, cb))) {
          static uint64_t late = 0;
          if (++late <= 20 || (late & (late - 1)) == 0)
            Log(fmt::format("draw pass: task {:08X} (cb {:08X} prio {:08X}) made by this frame's updates runs from the next frame ({} so far)", task, cb, prio, late));
          continue;
        }
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

// ---- v0.8.1 (plan_v81.md R6): the main loop keeps 60 ticks per second while the engine tracks -------------------
// sub_822197D0 arms its frame timers in sub_82218950 (call at 0x822198B8) RELATIVE TO NOW, after the engine's FrameA
// hook ran: timer A [0x84247B1C] = X, B = 2X, C = 0.8X with X = [0x826DE5F8] (100 ns units). Every millisecond the
// engine spent in FrameA (frame log, park wait, restore, resimulation) was therefore added to that frame's period
// and never paid back: 57.6 fps in a real 60 ms match, 52 fps at 150 ms. Here the live call gets a shortened X so
// that the next frame starts on an absolute 60 Hz schedule (host clock); a rollback's cost comes out of the idle
// time of the frame. Bounds: X never below 4 ms (below the frame's own work time timer A would fire before the
// loop head, whose poll eats it); a debt above two frames restarts the schedule. Only while the engine tracks in
// a rollback match or the loopback test; never offline, never in Classic matches, never inside a resim.
REXCVAR_DEFINE_BOOL(rb2_pace, true, "Patches",
                    "Online rollback: the main loop keeps an absolute 60 Hz schedule while the rollback engine tracks "
                    "(rollback work no longer slows the fight down); off = v0.8.0 behaviour");
REX_EXTERN(__imp__sub_82218950);
namespace pace {
bool on = false;
int64_t next_due = 0;  // steady clock, ns
uint64_t frames = 0, shortened = 0, resets = 0, floor_hits = 0;
double debt_max_ms = 0, short_sum_ms = 0;
constexpr int64_t kPeriod = 16'666'667;
constexpr int64_t kFloor = 4'000'000;
}  // namespace pace
std::string BurstLimitRb2PaceStats() {
  return fmt::format("pace: frames {} shortened {} (avg {:.2f} ms), at the 4 ms floor {}, debt max {:.1f} ms, resets {}", pace::frames,
                     pace::shortened, pace::short_sum_ms / std::max<uint64_t>(pace::shortened, 1), pace::floor_hits, pace::debt_max_ms,
                     pace::resets);
}
extern "C" REX_FUNC(sub_82218950) {
  if (uint32_t(ctx.lr) != 0x822198BCu || g_in_resim) {  // a resimulated frame's call: as before (the live call re-arms)
    __imp__sub_82218950(ctx, base);
    return;
  }
  const bool tracking = REXCVAR_GET(rb2_pace) && Rb2EngineOn() && sync::s.stage == 2 && sync::s.active &&
                        (REXCVAR_GET(rb2_loop) > 0 || BurstLimitRollback2Online());
  if (!tracking) {
    pace::on = false;
    __imp__sub_82218950(ctx, base);
    return;
  }
  const int64_t now = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
  const uint32_t x = rex::memory::load_and_swap<uint32_t>(base + 0x826DE5F8u);
  const int64_t x_ns = int64_t(x) * 100;
  if (!pace::on) {  // the schedule starts with this frame
    static bool logged = false;
    if (!logged) {
      logged = true;
      Log(fmt::format("pace: on, the game's frame period X = {} x 100 ns ([826DE5FC] = {}), schedule 16.667 ms", x,
                      rex::memory::load_and_swap<uint32_t>(base + 0x826DE5FCu)));
    }
    pace::on = true;
    pace::next_due = now + pace::kPeriod;
    __imp__sub_82218950(ctx, base);
    return;
  }
  ++pace::frames;
  pace::next_due += pace::kPeriod;
  int64_t remain = pace::next_due - now;
  if (remain < -2 * pace::kPeriod) {  // more than two frames behind (a hitch, a stopped process): start again
    ++pace::resets;
    pace::next_due = now + pace::kPeriod;
    remain = pace::kPeriod;
  }
  if (remain < pace::kPeriod) pace::debt_max_ms = std::max(pace::debt_max_ms, double(pace::kFloor - remain) / 1e6);
  if (remain > x_ns) {  // ahead of the schedule: no credit is kept
    pace::next_due = now + x_ns;
    remain = x_ns;
  }
  if (remain < pace::kFloor) {
    ++pace::floor_hits;
    remain = pace::kFloor;
  }
  if (x_ns - remain > 1'000'000) {
    ++pace::shortened;
    pace::short_sum_ms += double(x_ns - remain) / 1e6;
  }
  // the shortened period is in guest memory only for the call (the game writes X there again every frame)
  rex::memory::store_and_swap<uint32_t>(base + 0x826DE5F8u, uint32_t(remain / 100));
  __imp__sub_82218950(ctx, base);
  rex::memory::store_and_swap<uint32_t>(base + 0x826DE5F8u, x);
}
void BurstLimitRb2FrameA() {
  if (REXCVAR_GET(rb2_crash_record)) CrashInstall();  // R7: from the first main-loop frame on
  CrashTestTick(true);
  if (!Rb2EngineOn()) {  // a classic (lockstep) match after a rollback one: the stall watch has no frames to time
    g_last_frame.store(0);
    sync::ReleaseDeferredPark("engine off");  // review M1
    return;
  }
  auto* runtime = rex::Runtime::instance();
  auto* m = runtime ? runtime->memory() : nullptr;
  if (m && REXCVAR_GET(rb2_pad_noengine) && U32(m, 0x841B5130) && U32(m, 0x841B5228) == 2) {  // R40 control: the scripted pad
    static int64_t f = 0;
    ++f;
    static const uint16_t kPatternNe[] = {0x0008, 0x0008, 0x4000, 0, 0x4000, 0x8000, 0, 0x2000, 0x0004, 0x4000,
                                          0x4000, 0x4000, 0x8000, 0x0001, 0x2000, 0x0002, 0x1000, 0, 0x0008, 0x0100};
    if (f % 6 == 0) {
      if (auto* input = static_cast<rex::input::InputSystem*>(runtime->input_system())) {
        const int ult = REXCVAR_GET(rb2_loop_ult);
        if (ult > 0 && f >= ult && (f % ult) < 12) input->InjectButtons(0, uint16_t(0x8000 | 0x2000), 120, 0, 255);
        else input->InjectButtons(0, kPatternNe[(f / 6) % std::size(kPatternNe)], 80);
      }
    }
  }
  if (!m || U32(m, 0x84140E90) == 0) {
    sync::g_no_battle_seen = true;  // R17: between matches
    sync::ReleaseDeferredPark("no battle scene");  // review M1
    return;
  }
  PPCContext* ctx = rex::runtime::current_ppc_context();
  const PPCContext saved = *ctx;
  if (REXCVAR_GET(rb2_track)) sync::TrkFrameA(m, *ctx, m->virtual_membase());
  else sync::FrameA(m, *ctx, m->virtual_membase());
  *ctx = saved;
}
// The game's job worker (sub_820F2618's loop) and draw worker (sub_820EA188) are frame threads: their
// writes are the frame's (bit 8), unlike the render, sound and loader threads (bit 2, never rewound
// with rb2_foreign).
void BurstLimitRb2Worker() {
  rb2_tbit = uint8_t(REXCVAR_GET(rb2_worker_tbit));
  t_role = "job worker";
}
// The render loop thread (sub_82219360) reads the effects pool / render lists in its scene walk (821295A0); from
// the frame pacing call (82119D18) to the next loop head it doesn't. g_parked = "outside the scene walk": a rewind
// waits only until then (not for a whole loop turn), and the loop head holds the thread while a rewind runs.
REXCVAR_DEFINE_BOOL(rb2_park_safe_window, false, "Debug", "Rollback (rollback3) test: the render loop counts as parked from its frame pacing call to the next loop head (froze the screen: off)");
void BurstLimitRb2RenderSafe() {
  if (REXCVAR_GET(rb2_park_safe_window)) sync::g_parked.store(1);
}
// round 6 test tool: a hitch of the render loop (every N-th pass of its loop head sleeps M ms) - the game's main
// thread goes on ticking meanwhile, as it does when the render side stalls on a slow PC (see bl_tex_lag)
REXCVAR_DEFINE_STRING(bl_render_hitch, "", "Debug", "Test: n:ms - every n-th pass of the render loop head sleeps ms milliseconds");
void BurstLimitRb2RenderPark() {
  using namespace sync;
  t_role = "render loop";
  CrashTestTick(false);
  {
    static int every = -1, ms = 0;
    static uint64_t pass = 0, hitches = 0;
    if (every < 0) {
      every = 0;
      unsigned a = 0, b = 0;
      if (std::sscanf(REXCVAR_GET(bl_render_hitch).c_str(), "%u:%u", &a, &b) == 2 && a > 0 && b > 0) {
        every = int(a);
        ms = int(std::min(b, 2000u));
        REXLOG_WARN("[RB2] render loop hitch armed: every {}th loop head sleeps {} ms", every, ms);
      }
    }
    if (every > 0 && ++pass % uint64_t(every) == 0) {
      ++hitches;
      std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    }
  }
  for (;;) {
    // not parked. The request is read AFTER this store: a request made meanwhile either sees "not parked" and
    // waits, or is seen here and parked for (never the stale flag of the park that just ended)
    g_parked.store(0);
    g_parked_gen.store(0);
    if (!g_park_req.load()) return;
    const uint64_t gen = g_park_gen.load();
    if (gen == g_park_timed_out_gen.load()) return;  // review M1: a request that hit rb2_park_max_ms is not parked for again
    g_parked_gen.store(gen);
    g_parked.store(1);
    const auto t0 = Clock::now();
    const int max_ms = REXCVAR_GET(rb2_park_max_ms);
    while (g_park_req.load() && g_park_gen.load() == gen) {
      if (max_ms > 0 && Ms(t0, Clock::now()) > double(max_ms)) {  // review M1: second safety - never parked for good
        const uint64_t n = ++g_park_max_hits;
        if (n <= 8 || (n & (n - 1)) == 0) REXLOG_WARN("[RB2] render loop left a park after {} ms (request still set; {} so far)", max_ms, n);
        BurstLimitOnlineAlert("park limit", fmt::format("the render loop left a park after {} ms", max_ms));
        g_park_timed_out_gen.store(gen);
        g_parked.store(0);
        g_parked_gen.store(0);
        return;
      }
      // a rewind takes a few ms: yield first (sleep_for(100 us) is 1-15 ms on Windows without a raised timer period)
      if (Ms(t0, Clock::now()) < 2.0) std::this_thread::yield();
      else std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
  }
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
  t_role = "draw worker";
  static std::atomic<uint32_t> logged{0};
  if (logged.exchange(r27.u32) != r27.u32) REXLOG_INFO("[RB2] draw worker context {:08X}", r27.u32);  // diagnostics
}
REXCVAR_DEFINE_BOOL(rb2_worker_loop, false, "Debug", "Rollback (rollback3): the job pool worker loop's own writes (queue pops) are frame writes from the first pop on");
void BurstLimitRb2WorkerLoop() {
  if (REXCVAR_GET(rb2_worker_loop)) rb2_tbit = uint8_t(REXCVAR_GET(rb2_worker_tbit));
}

// The CRI server / file step (main loop step 0x8221992C, skipped in resims): its CRI calls are "outside".
//
// v0.8.1 R30: the step also runs the file request queue (sub_8210AC30, sub_82118D88: manager 0x8413DE08, countdown
// +12, current request +28, request state +100; sub_82118B90 picks the next request up, sub_82118820 starts its read)
// and the handlers of finished loads (sub_82118228: file slot / texture set creates = resource commands 0 and 4).
// It follows host I/O, which no rewind takes back, and no resimulated frame runs it. A rewind over a frame in which
// it worked put the queue some steps back while the read behind it went on: the next live frames started the same
// read again and handled the same finished load again (MEASURED r122_d0, 150 s loopback with 3 transformations: 28
// reads started again, 70 creates sent again; r120_ltr1: a second read's garbage taken as the file -> crash in
// sub_82118228). Keeping the step's stores live instead (tested, r122_d1) crashed at once: the step allocates from
// the game heap, whose header the fight's allocations rewind.
// rb2_file_gate: no rewind ever crosses a frame in which the queue worked -
//   - the queue (sub_82118D88) starts to work on a waiting request only when no predicted input is unsettled and no
//     rewind is pending, and then works on until it is empty (build 123 gated only the pick-up sub_82118B90: requests
//     of the other kind never pass through it, and one rewind crossed their first work frame, r123_g1);
//   - no prediction is made while a request is pending or in work, nor in a frame in which the queue worked
//     (BurstLimitRb2PredictOk: the fight waits for the real input, as it does in the settle frames).
// Requests the fight makes stay plain frame state until they are picked up (a rewind takes them back, the
// corrected frames make them again).
REX_EXTERN(__imp__sub_82119320);
REXCVAR_DEFINE_BOOL(rb2_cri_outside, true, "Debug", "Rollback (rollback3): CRI calls of the CRI server step are marked outside (never matched)");
REXCVAR_DEFINE_BOOL(rb2_file_gate, true, "Debug",
                    "Rollback: no rewind over a frame in which the file request queue worked - a file request is picked up "
                    "only when no predicted input is unsettled, and no prediction is made while one is pending or in work "
                    "(off = v0.8.0: reads started again and creates sent again after such a rewind)");
REXCVAR_DEFINE_STRING(rb2_gate_rw_test, "", "Debug",
                      "Rollback test (R39): K:D - K engine frames after the file gate first sees a request, a rewind of depth D is "
                      "forced (loopback / online)");
REXCVAR_DEFINE_BOOL(rb2_fs_diag, false, "Debug",
                    "Rollback diagnostics: which 16-byte blocks the file step writes and which other main-thread code writes "
                    "the same blocks (report every 1800 engine frames)");
namespace fsdiag {
bool g_on = false;
bool g_in = false;  // main thread, inside the live file step
std::vector<uint8_t> g_page;  // pages the file step wrote
struct W { uint32_t fn = 0; uint32_t n = 0; uint32_t fn2 = 0; uint32_t resim = 0; };
std::unordered_map<uint32_t, W> g_fs, g_other;  // by block address
uint64_t g_fs_stores = 0, g_other_stores = 0;
int64_t g_last_report = 0;
void Note(uint32_t x, uintptr_t ret) {
  if (x >= 0x70000000u && x < 0x80000000u) return;  // thread stacks
  if (g_page.empty()) g_page.assign(0x100000, 0);
  const uint32_t blk = x & ~15u;
  if (g_in) {
    g_page[x >> 12] = 1;
    ++g_fs_stores;
    if (g_fs.size() < 60000 || g_fs.count(blk)) {
      W& w = g_fs[blk];
      const uint32_t fn = GuestOfHost(ret);
      if (!w.fn) w.fn = fn; else if (fn != w.fn) w.fn2 = fn;
      ++w.n;
    }
  } else if (g_page[x >> 12]) {
    ++g_other_stores;
    if (g_other.size() < 60000 || g_other.count(blk)) {
      W& w = g_other[blk];
      const uint32_t fn = GuestOfHost(ret);
      if (!w.fn) w.fn = fn; else if (fn != w.fn) w.fn2 = fn;
      ++w.n;
      if (g_in_resim) ++w.resim;
    }
  }
}
void Report(int64_t frame) {
  if (!g_on || frame - g_last_report < 1800) return;
  g_last_report = frame;
  std::vector<uint32_t> shared;
  for (const auto& [blk, w] : g_fs) if (g_other.count(blk)) shared.push_back(blk);
  std::sort(shared.begin(), shared.end());
  size_t pages = 0;
  for (uint8_t b : g_page) pages += b;
  REXLOG_WARN("[RB2] file step diag at frame {}: {} stores into {} blocks on {} pages; other main-thread stores into those pages {} "
              "({} blocks); blocks written by BOTH: {}", frame, g_fs_stores, g_fs.size(), pages, g_other_stores, g_other.size(), shared.size());
  std::string line;
  int shown = 0;
  for (uint32_t blk : shared) {
    const W& a = g_fs[blk];
    const W& b = g_other[blk];
    line += fmt::format(" [{:08X}: step {:08X}{} x{} | other {:08X}{} x{} ({} in resims)]", blk, a.fn, a.fn2 ? fmt::format("/{:08X}", a.fn2) : "",
                        a.n, b.fn, b.fn2 ? fmt::format("/{:08X}", b.fn2) : "", b.n, b.resim);
    if (++shown % 12 == 0) { REXLOG_WARN("[RB2] file step diag shared:{}", line); line.clear(); }
    if (shown >= 240) break;
  }
  if (!line.empty()) REXLOG_WARN("[RB2] file step diag shared:{}", line);
  // the step's own blocks by writer function (what it is made of)
  std::map<uint32_t, std::pair<uint32_t, uint32_t>> by_fn;  // fn -> blocks, stores
  for (const auto& [blk, w] : g_fs) { auto& e = by_fn[w.fn]; ++e.first; e.second += w.n; }
  line.clear();
  for (const auto& [fn, e] : by_fn) line += fmt::format(" {:08X}: {} blocks {} stores,", fn, e.first, e.second);
  REXLOG_WARN("[RB2] file step diag writers (first writer of each block):{}", line);
  rex::FlushLogging();
}
}  // namespace fsdiag
bool BurstLimitOnlineUnsettled();  // burstlimit_netinput.cpp: a predicted remote input whose real one has not arrived
std::string BurstLimitReadyGateStats();  // burstlimit_netinput.cpp
int BurstLimitOnlineLocalPlayer();       // burstlimit_netinput.cpp: 0 host, 1 guest, -1 no online match
// v0.8.1 R32: both PCs make the same file request in the same frame, so both stopped predicting in the same frame
// and each waited for the other's input (MEASURED r126_res, RTT 157: the host stood 3 ticks at frame 1672, the guest
// 7 ticks at frame 1675 - its own wait plus the host's). With the stagger the guest goes on predicting for
// rb2_file_stagger engine frames after a request appears and only then closes its predictions and starts its
// queue; the host starts at once. Each PC then waits once, while the other one still sends its inputs on time.
// The queue's timing is host-local anyway (it follows the disk); a waiting request is plain frame state.
// v0.8.1 R32 / R36b (build 136). A deferred "resources ready" poll held predictions for its own engine frame and
// the next one only. The held tick does not run the fight, so the poll came again one frame later, in a frame
// that had just consumed a NEW prediction: unsettled again, deferred again. The fight ran every other tick until
// its consume happened to reach the frontier of the received inputs - and with both fighters in a transformation
// both PCs did that and neither caught up (MEASURED r135_long, RTT 235: 614 deferred polls for 18 transformations,
// "ready gate: 189-219 holds" of one tick each per round, fight speed 35 % for 1-2 s per transformation).
// Now the hold stays until the gate gives "ready" (or for this many engine frames after the last deferred poll,
// if the fighter stops polling): the fight waits once for the inputs that are already on their way, the next
// poll finds nothing unsettled. 1 = builds 126-135.
REXCVAR_DEFINE_INT32(rb2_ready_hold, 12, "Debug",
                     "Rollback: engine frames a deferred \"resources ready\" poll keeps predictions held (ends at once when "
                     "\"ready\" is given; 1 = builds up to 135: this frame and the next)");
// v0.8.1 round 7 (build 150): default 0. Up to build 147 the read started inside the frame that made the request, on
// both PCs in the same frame, and the stagger only moved the guest's prediction stop. With rb2_file_pickup_gate
// the read starts when the queue starts: a guest that starts 12 frames late is "ready" 12 frames after the host,
// and the host sits in its status window (no predictions) for those frames. Both PCs now stop predicting at the
// request and start their read as soon as their predictions have settled (a few frames, the same on both).
REXCVAR_DEFINE_INT32(rb2_file_stagger, 0, "Debug",
                     "Rollback: the guest's file request queue starts this many engine frames after a request appears "
                     "(the host's starts at once), so the two PCs do not wait for each other's inputs in the same frames "
                     "(0 = both stop predicting from the request's first frame; 12 = builds 127-149)");
REXCVAR_DEFINE_INT32(rb2_file_stagger_role, -1, "Debug",
                     "Rollback test: which side staggers its file queue start: -1 the guest (default), 0 nobody, 1 this PC "
                     "whatever its role (loopback test)");
const char* g_hold_reason = "";  // R8: why BurstLimitRb2PredictOk said no the last time
std::atomic<int64_t> g_pickup_deferred_live{0}, g_pickup_deferred_resim{0};  // R39 (rb2_file_pickup_gate)
namespace loop { bool Unsettled(); extern int64_t rewind_to; }
namespace filegate {
constexpr uint32_t kMgr = 0x8413DE08u;
int64_t g_work_frame[64];         // engine frames in which the queue worked (ring; frame + 1)
int64_t g_work_frames = 0, g_pickups = 0, g_pickup_held = 0, g_pickup_forced = 0, g_pred_held = 0, g_rewinds_over_work = 0,
        g_work_ungated = 0;
int g_held_run = 0;
int64_t g_pending_since = -1;     // the live step first saw the waiting request in this engine frame (R32 stagger)
int64_t g_stagger_frames = 0;
bool g_open = false;              // the queue is in a work period (started with nothing unsettled)
bool g_event = false;             // the queue stepped with a request / a read was started / a create was sent in this step
bool g_in_step = false;
inline bool On() { return REXCVAR_GET(rb2_file_gate); }
inline uint32_t Word(uint32_t a) {
  return g_mem_for_watch ? rex::memory::load_and_swap<uint32_t>(g_mem_for_watch->TranslateVirtual<const uint8_t*>(a)) : 0;
}
// a request is waiting or in work (the countdown +12 is the queue's own short pause between two requests)
inline bool Pending() { return Word(kMgr + 28) != 0 || Word(kMgr + 12) != 0; }
inline bool WorkedIn(int64_t frame) { return frame >= 0 && g_work_frame[uint64_t(frame) % 64] == frame + 1; }
inline void MarkWork(int64_t frame) {
  if (frame < 0 || WorkedIn(frame)) return;
  g_work_frame[uint64_t(frame) % 64] = frame + 1;
  ++g_work_frames;
}
inline bool Unsettled() { return BurstLimitOnlineUnsettled() || loop::Unsettled() || loop::rewind_to >= 0 || sync::g_park_deferred_now; }
// the prediction gate (BurstLimitRb2PredictOk, the loopback's consume) - LIVE frames only: a resimulated frame
// consumes what it consumed when it ran (holding one there would shift every later input frame; the queue does
// not step in a resim, and it will not start while the frame's new prediction is unsettled)
int64_t g_ready_hold_frame = -10;  // the "resources ready" answer was deferred in this engine frame (ready gate)
// R32: the queue has a request it has not started to work on (no work period open), on the side that staggers
// its queue start, and the request is younger than rb2_file_stagger engine frames (or not yet seen by the live
// step: it was made in the last frame). Callers ask only while Pending(). (Build 127 also asked for request state
// 0 and no countdown: the request never looked like that at its first step - "staggered 0" in r127_gs1.)
inline bool Staggering() {
  const int n = REXCVAR_GET(rb2_file_stagger);
  if (n <= 0 || g_open) return false;
  const int role = REXCVAR_GET(rb2_file_stagger_role);
  if (role == 0 || (role < 0 && BurstLimitOnlineLocalPlayer() != 1)) return false;
  return g_pending_since < 0 || sync::g_cur_frame - g_pending_since < n;
}
bool Hold() {
  if (g_in_resim || !g_mem_for_watch) return false;
  if (g_ready_hold_frame >= sync::g_cur_frame - std::clamp(REXCVAR_GET(rb2_ready_hold), 1, 60)) { g_hold_reason = "ready gate"; return true; }
  if (!On()) return false;
  g_hold_reason = "file gate";
  if (WorkedIn(sync::g_cur_frame)) return true;
  return Pending() && !Staggering();
}
std::string Stats() {
  return fmt::format("file gate: frames with queue work {}, work periods {} (start held {} frames, forced {}, staggered {}), predictions held {}, "
                     "rewinds over queue work {}, work without a request {}, stress rewinds skipped {}, pick-ups left to the file step {} "
                     "(+{} in resims)", g_work_frames, g_pickups, g_pickup_held,
                     g_pickup_forced, g_stagger_frames, g_pred_held, g_rewinds_over_work, g_work_ungated, g_stress_gate_skips.load(),
                     g_pickup_deferred_live.load(), g_pickup_deferred_resim.load());
}
}  // namespace filegate
void BurstLimitRb2FileEvent() {  // burstlimit_texreg.cpp: a file read started / a create sent
  if (filegate::g_in_step) filegate::g_event = true;
}
std::string BurstLimitRb2FileGateStats() { return filegate::Stats(); }
// for the ready gate (burstlimit_netinput.cpp)
bool BurstLimitRb2Unsettled() { return filegate::Unsettled(); }
void BurstLimitRb2ReadyHold() { filegate::g_ready_hold_frame = sync::g_cur_frame; }
void BurstLimitRb2ReadyHoldEnd() { filegate::g_ready_hold_frame = -100; }  // "ready" was given: predictions again
// v0.8.1 R17 (build 142): online the battle object and its scene stay alive from match to match (r140_long, r141_bad:
// no check line in five matches), so the new-fight signal is the input layer's match reset.
void BurstLimitRb2NewMatch() { sync::g_no_battle_seen = true; }
bool BurstLimitRb2FileGateWorked(int64_t from, int64_t to) {
  for (int64_t f = from; f < to; ++f) if (filegate::WorkedIn(f)) return true;
  return false;
}
void BurstLimitRb2FileGateRewind(int64_t from, int64_t to) {
  for (int64_t f = from; f < to; ++f) {
    if (!filegate::WorkedIn(f)) continue;
    const int64_t n = ++filegate::g_rewinds_over_work;
    if (n <= 8 || (n & (n - 1)) == 0)
      REXLOG_WARN("[RB2] rewind over file queue work: frames {}..{}, the queue worked in frame {} ({} such rewinds, file gate {})", from,
                  to - 1, f, n, filegate::On() ? "on" : "off");
    break;
  }
}
// ---- v0.8.1 R39: the game heap -----------------------------------------------------------------------------
// sub_8211A0C8(size, tag, kind, name, ?, name2) is the game's malloc (441 call sites) and sub_8211A338(payload) its
// free (217). Manager [0x825205C4] (0x8413E240): +0 the heap's base, +776 its size (0x7400000 at 0xD6000000); 64-byte
// list anchors at +4 (used, kind 0: allocated from the front), +68 (used, kind 1 / 2: from the back), +132 (free
// list head; walked forward, sorted by address) and +196 (its tail; walked backward); eight tag list anchors
// from +260; +772 blocks in use, +780 bytes in use. A block: 64-byte header {+0 footer, +4 prev, +8 next, +12 tag
// prev, +16 tag next, +20 payload size, +28 kind, +32 flags (0x4000 = free), +36 tag, +60 0xDEADC0DE}, the payload,
// a 64-byte footer {+0 0xEDEDEDED (not on every free block), +4 header}. malloc walks the whole used list to its
// tail (0x8211A188), free walks the free list to its place. There is no lock (sub_82178200 is empty): the heap
// belongs to the main thread - the frame's code (rewound and resimulated with it: the fighters allocate and free
// temporary arrays all the time), the file step (live only: never rewound over, rb2_file_gate) and the post step
// sub_82120CF8 (resimulated).
// r144_ltr / r142_t3 (R39): malloc met a used-list node whose header was file data.
// rb2_heap_audit walks every block and every list at each live frame start, before and after every restore and
// after each resimulated frame; the first failures are logged with the blocks involved (their words, whether
// the last restore kept them live and why) and the last heap calls.
// On by default since build 148: 8.8 us per run (r147_a1: 7903 runs in 60 s, peak 98 us), and a torn heap is then in
// the log with its blocks and the last heap calls BEFORE the crash it leads to.
REXCVAR_DEFINE_INT32(rb2_heap_audit, 1, "Debug",
                     "Rollback diagnostics (R39): walk the game heap (every block, the used / free / tag lists) at each live "
                     "frame start, before and after every restore and after each resimulated frame (0 off, 1 on)");
namespace heap {
constexpr uint32_t kMgrPtr = 0x825205C4u;
struct Op { int64_t frame; uint32_t ptr, size, lr; uint8_t op, cls; };
Op g_ops[256];
std::atomic<uint32_t> g_op_n{0};
std::atomic<int64_t> g_count[2][6];
const char* const kCls[6] = {"frame", "resim", "file step", "job", "other thread", "engine paused"};
int64_t g_audit_runs = 0, g_audit_fails = 0, g_audit_logged = 0, g_rw_frame = -1;
double g_audit_ms = 0, g_audit_ms_peak = 0;
int g_rw_depth = 0;
std::unordered_map<uint32_t, uint8_t> g_kept;  // 16-byte block (address >> 4) -> why the last restore kept it live (and it differed)
std::atomic<uint32_t> g_lo{0}, g_hi{0};         // the heap's range, for the restore's page test
inline int Class() {
  if (!rb2_main) return rb2_tbit == 8 ? 3 : 4;
  if (g_in_resim) return 1;
  if (filegate::g_in_step) return 2;
  return (sync::s.stage == 2 && sync::s.active) ? 0 : 5;
}
inline void Note(int op, uint32_t ptr, uint32_t size, uint32_t lr) {
  const int c = Class();
  g_count[op][c].fetch_add(1, std::memory_order_relaxed);
  Op& o = g_ops[g_op_n.fetch_add(1, std::memory_order_relaxed) & 255];
  o.frame = sync::g_cur_frame; o.ptr = ptr; o.size = size; o.lr = lr; o.op = uint8_t(op); o.cls = uint8_t(c);
}
std::string Ops(uint32_t count) {
  std::string out;
  const uint32_t n = g_op_n.load();
  for (uint32_t q = n > count ? n - count : 0; q < n; ++q) {
    const Op& o = g_ops[q & 255];
    out += fmt::format(" [f{} {} {} {:08X}{} lr {:08X}]", o.frame, kCls[o.cls % 6], o.op ? "free" : "alloc", o.ptr,
                       o.op ? std::string() : fmt::format(" {} B", o.size), o.lr);
  }
  return out;
}
std::string Block(rex::memory::Memory* m, uint32_t a) {
  const uint32_t b = a & ~15u;
  if (!Committed(m, b, 16)) return fmt::format(" [{:08X}: not committed]", a);
  const size_t blk = size_t(b) >> 4;
  const int bit = int(blk & 7);
  std::string why = "no";
  if (const auto it = g_kept.find(uint32_t(blk)); it != g_kept.end()) {
    why.clear();
    if (it->second & 2) why += "F";   // foreign mark in an undone frame's log
    if (it->second & 4) why += "N";   // foreign mark since the last log
    if (it->second & 8) why += "L";   // lost-mark block
    if (it->second & 32) why += " STALE";  // no other thread wrote the page in the undone frames
  }
  return fmt::format(" [{:08X}: {:08X} {:08X} {:08X} {:08X} | kept live by the last restore (and differing): {} | marks now f{} p{} m{} page "
                     "bits {:02X}]", b, U32(m, b), U32(m, b + 4), U32(m, b + 8), U32(m, b + 12), why, (rb2_ffine[blk >> 3] >> bit) & 1,
                     (rb2_ffine_prev[blk >> 3] >> bit) & 1, (rb2_mfine[blk >> 3] >> bit) & 1, unsigned(rb2_dirty[b >> 12]));
}
bool Audit(rex::memory::Memory* m, const char* point, int64_t frame, int k) {
  const uint32_t mgr = U32(m, kMgrPtr);
  if (mgr < 0x80000000u || !Committed(m, mgr, 784)) return true;
  const uint32_t base = U32(m, mgr), size = U32(m, mgr + 776);
  if (base < 0x40000000u || size < 0x10000u || size > 0x20000000u || !Committed(m, base, 64) || !Committed(m, base + size - 64, 64)) return true;
  g_lo.store(base);
  g_hi.store(base + size);
  ++g_audit_runs;
  std::string err;
  std::vector<uint32_t> inv;
  auto bad = [&](std::string e, std::initializer_list<uint32_t> nodes) {
    if (!err.empty()) return;
    err = std::move(e);
    inv.assign(nodes);
  };
  auto in = [&](uint32_t a) { return a >= base && uint64_t(a) + 64 <= uint64_t(base) + size && !(a & 3); };
  // 1. the blocks, in address order
  static std::vector<std::pair<uint32_t, uint8_t>> blocks;  // header, 1 free | 2 seen in a list | 4 seen in a tag list
  blocks.clear();
  uint32_t prev = 0;
  for (uint32_t p = base; p < base + size;) {
    const uint32_t foot = U32(m, p), sz = U32(m, p + 20);
    if (U32(m, p + 60) != 0xDEADC0DEu) { bad(fmt::format("block {:08X} (after block {:08X}) has no header magic", p, prev), {p, prev}); break; }
    if (!in(foot) || foot != p + 64 + sz) { bad(fmt::format("block {:08X}: footer pointer {:08X} does not fit its size {}", p, foot, sz), {p, prev}); break; }
    if (U32(m, foot + 4) != p) { bad(fmt::format("block {:08X}: its footer {:08X} points back to {:08X}", p, foot, U32(m, foot + 4)), {p, foot}); break; }
    blocks.emplace_back(p, uint8_t((U32(m, p + 32) & 0x4000u) ? 1 : 0));
    prev = p;
    p = foot + 64;
    if (blocks.size() > 400000) { bad("more than 400000 blocks", {p}); break; }
  }
  auto find = [&](uint32_t node) -> int {
    const auto it = std::lower_bound(blocks.begin(), blocks.end(), node, [](const std::pair<uint32_t, uint8_t>& b, uint32_t v) { return b.first < v; });
    return (it != blocks.end() && it->first == node) ? int(it - blocks.begin()) : -1;
  };
  // 2. the used lists and the free list
  struct L { uint32_t anchor; const char* name; bool free; };
  const L lists[3] = {{mgr + 4, "used list 0", false}, {mgr + 68, "used list 1", false}, {mgr + 132, "free list", true}};
  int nused = 0, nfree = 0;
  for (const L& l : lists) {
    if (!err.empty()) break;
    uint32_t pv = l.anchor;
    int steps = 0;
    bool tail = !l.free;
    for (uint32_t node = U32(m, l.anchor + 8); node; node = U32(m, node + 8)) {
      if (l.free && node == mgr + 196) {
        if (U32(m, node + 4) != pv) bad(fmt::format("free list: the tail anchor's prev is {:08X}, reached from {:08X}", U32(m, node + 4), pv), {pv, node});
        tail = true;
        break;
      }
      const int bi = in(node) ? find(node) : -1;
      if (bi < 0) { bad(fmt::format("{}: node {:08X} (after {:08X}) is not a block header", l.name, node, pv), {pv, node}); break; }
      if (U32(m, node + 4) != pv) { bad(fmt::format("{}: node {:08X} has prev {:08X} but is reached from {:08X}", l.name, node, U32(m, node + 4), pv), {pv, node}); break; }
      if (bool(blocks[bi].second & 1) != l.free) { bad(fmt::format("{}: block {:08X} is {}", l.name, node, l.free ? "not marked free" : "marked free"), {node, pv}); break; }
      if (blocks[bi].second & 2) { bad(fmt::format("{}: block {:08X} is linked twice", l.name, node), {node, pv}); break; }
      blocks[bi].second |= 2;
      ++(l.free ? nfree : nused);
      pv = node;
      if (++steps > 400000) { bad(fmt::format("{}: a cycle", l.name), {node}); break; }
    }
    if (err.empty() && !tail) bad(fmt::format("free list: ends at {:08X} without its tail anchor", pv), {pv});
  }
  if (err.empty()) {
    for (const auto& [hdr, fl] : blocks) {
      if (!(fl & 2)) { bad(fmt::format("block {:08X} ({}) is in no list", hdr, (fl & 1) ? "free" : "used"), {hdr}); break; }
    }
  }
  // 3. the tag lists (every used block is in one)
  for (int tag = 0; tag < 8 && err.empty(); ++tag) {
    uint32_t pv = mgr + 260 + uint32_t(tag) * 64;
    int steps = 0;
    for (uint32_t node = U32(m, pv + 16); node; node = U32(m, node + 16)) {
      const int bi = in(node) ? find(node) : -1;
      if (bi < 0) { bad(fmt::format("tag list {}: node {:08X} (after {:08X}) is not a block header", tag, node, pv), {pv, node}); break; }
      if (U32(m, node + 12) != pv) { bad(fmt::format("tag list {}: node {:08X} has prev {:08X} but is reached from {:08X}", tag, node, U32(m, node + 12), pv), {pv, node}); break; }
      if (blocks[bi].second & 1) { bad(fmt::format("tag list {}: block {:08X} is free", tag, node), {node, pv}); break; }
      if (blocks[bi].second & 4) { bad(fmt::format("tag list {}: block {:08X} is linked twice", tag, node), {node, pv}); break; }
      blocks[bi].second |= 4;
      pv = node;
      if (++steps > 400000) { bad(fmt::format("tag list {}: a cycle", tag), {node}); break; }
    }
  }
  if (err.empty()) {
    for (const auto& [hdr, fl] : blocks) {
      if (!(fl & 1) && !(fl & 4)) { bad(fmt::format("used block {:08X} is in no tag list", hdr), {hdr}); break; }
    }
  }
  if (err.empty() && U32(m, mgr + 772) != uint32_t(nused))
    bad(fmt::format("the manager counts {} blocks in use, the lists hold {}", U32(m, mgr + 772), nused), {mgr + 768});
  if (err.empty()) {
    if (g_audit_runs <= 2 || g_audit_runs % 6000 == 0)
      Log(fmt::format("heap audit: run {} at {} (frame {}): {} blocks, {} used, {} free, {} bytes in use; failures so far {}", g_audit_runs, point,
                      frame, blocks.size(), nused, nfree, U32(m, mgr + 780), g_audit_fails));
    return true;
  }
  ++g_audit_fails;
  BurstLimitOnlineAlert("heap audit", fmt::format("FAIL at {} (frame {}): {}", point, frame, err));
  if (g_audit_logged < 8) {
    ++g_audit_logged;
    std::string bl;
    for (uint32_t a : inv) {
      if (!a) continue;
      for (uint32_t o = 0; o < 64; o += 16) bl += Block(m, a + o);
    }
    Log(fmt::format("heap audit: FAIL at {} (frame {}{}; last rewind at frame {} depth {}): {} | walked {} blocks, lists: {} used, {} free, "
                    "manager count {} |{}", point, frame, k >= 0 ? fmt::format(", resim frame +{}", k) : std::string(), g_rw_frame, g_rw_depth,
                    err, blocks.size(), nused, nfree, U32(m, mgr + 772), bl));
    Log("heap calls (newest last):" + Ops(48));
    rex::FlushLogging();
  }
  return false;
}
}  // namespace heap
void BurstLimitRb2HeapAudit(rex::memory::Memory* m, const char* point, int64_t frame, int k) {
  if (REXCVAR_GET(rb2_heap_audit) <= 0) return;
  const auto a = Clock::now();
  heap::Audit(m, point, frame, k);
  const double ms = std::chrono::duration<double, std::milli>(Clock::now() - a).count();
  heap::g_audit_ms += ms;
  heap::g_audit_ms_peak = std::max(heap::g_audit_ms_peak, ms);
}
void BurstLimitRb2HeapRewind(int64_t now, int n) {
  heap::g_rw_frame = now;
  heap::g_rw_depth = n;
  if (!heap::g_kept.empty()) heap::g_kept.clear();
  if (!heap::g_hi.load(std::memory_order_relaxed) && g_mem_for_watch) {  // the range, once (the restore's page test)
    auto* m = g_mem_for_watch;
    const uint32_t mgr = U32(m, heap::kMgrPtr);
    if (mgr >= 0x80000000u && Committed(m, mgr, 784)) {
      const uint32_t base = U32(m, mgr), size = U32(m, mgr + 776);
      if (base >= 0x40000000u && size >= 0x10000u && size <= 0x20000000u) { heap::g_lo.store(base); heap::g_hi.store(base + size); }
    }
  }
}
bool BurstLimitRb2HeapPage(uint32_t a) { return a >= heap::g_lo.load(std::memory_order_relaxed) && a < heap::g_hi.load(std::memory_order_relaxed); }
std::string BurstLimitRb2HeapBlocks(uint32_t a, uint32_t bytes) {
  std::string out;
  if (!g_mem_for_watch) return out;
  for (uint32_t o = 0; o < bytes; o += 16) out += heap::Block(g_mem_for_watch, a + o);
  return out;
}
void BurstLimitRb2HeapKept(uint32_t block_addr, int why) {
  if (REXCVAR_GET(rb2_heap_audit) > 0 && heap::g_kept.size() < 200000) heap::g_kept[block_addr >> 4] |= uint8_t(why);
}
std::string BurstLimitRb2HeapStats() {
  using namespace heap;
  std::string calls;
  for (int c = 0; c < 6; ++c) {
    if (g_count[0][c].load() || g_count[1][c].load()) calls += fmt::format(" {} {}/{}", kCls[c], g_count[0][c].load(), g_count[1][c].load());
  }
  return fmt::format("game heap: allocs/frees by caller:{}; audit runs {} failures {} ({:.1f} us avg, {:.0f} us peak); foreign marks: age mode {}, decoder jobs in the fill "
                     "class {}, fill pages {} ({} blocks, {} late stores), pages recorded without a log {} (late stores {}), blocks kept live "
                     "on a STALE mark and differing {} (game heap {}) in {} rewinds, on a sound mark in the game heap {}; effect "
                     "instances skipped for an unknown resource id {}",
                     calls.empty() ? " -" : calls, g_audit_runs, g_audit_fails, g_audit_runs ? g_audit_ms * 1000.0 / double(g_audit_runs) : 0.0,
                     g_audit_ms_peak * 1000.0, REXCVAR_GET(rb2_ffine_age), g_fill_jobs.load(),
                     sync::t.fill_pages, sync::t.fill_blocks, sync::t.fill_late, sync::t.fonly_pages, sync::t.fonly_lost, sync::t.stale_blocks,
                     sync::t.stale_heap_blocks, sync::t.stale_rewinds, sync::t.sound_heap_blocks, g_fx_id_skips.load());
}
// The file decoder's job (codec worker thread sub_82513E38 -> sub_82513D70 -> sub_82513CB8 -> job callback
// sub_824EB808: codec = sub_82513110([job]), decode through its vtable +8 = sub_82513280, which writes the decoded
// bytes backward into the destination, a game heap block): data written once, owned by the main thread afterwards.
REX_EXTERN(__imp__sub_824EB808);
extern "C" REX_FUNC(sub_824EB808) {
  const unsigned char was = rb2_tbit;
  const bool fill = rb2_track_on && was == 2 && REXCVAR_GET(rb2_ffine_age) == 1;
  uint32_t job = 0, dst = 0, src_size = 0, dst_size = 0;
  if (fill) {
    rb2_tbit = 32;
    job = ctx.r3.u32;
    if (job >= 0x10000u) {  // decode(codec, src = [job + 12], [job + 16], dst = [job + 12], [job + 20]): in place
      dst = rex::memory::load_and_swap<uint32_t>(base + job + 12);
      src_size = rex::memory::load_and_swap<uint32_t>(base + job + 16);
      dst_size = rex::memory::load_and_swap<uint32_t>(base + job + 20);
    }
  }
  __imp__sub_824EB808(ctx, base);
  if (fill) {
    // Everything the job produced is fill, also bytes that got there without a store mark (r146_a1: 135 blocks
    // changed after their page's copy with no mark): the range is noted once more when the job is done, so its
    // pages are copied at this frame's end whatever wrote them.
    const uint32_t len = std::max(src_size, dst_size);
    if (dst >= 0x40000000u && len && len <= 0x4000000u) sync::HostWrite(dst, len, 4);
    const int64_t nj = g_fill_jobs.fetch_add(1, std::memory_order_relaxed);
    if (nj < 16)
      REXLOG_INFO("[RB2] fill: decoder job {} at engine frame {}: buffer {:08X}, packed {} B, unpacked {} B, result {} (job {:08X})", nj + 1,
                  sync::g_cur_frame_for_hook.load(), dst, src_size, dst_size, rex::memory::load_and_swap<uint32_t>(base + job + 28), job);
  }
  rb2_tbit = was;
}
REX_EXTERN(__imp__sub_8211A0C8);
extern "C" REX_FUNC(sub_8211A0C8) {
  if (!rb2_track_on) { __imp__sub_8211A0C8(ctx, base); return; }
  const uint32_t size = ctx.r3.u32, lr = uint32_t(ctx.lr);
  __imp__sub_8211A0C8(ctx, base);
  heap::Note(0, ctx.r3.u32, size, lr);
}
REX_EXTERN(__imp__sub_8211A338);
extern "C" REX_FUNC(sub_8211A338) {
  if (rb2_track_on) heap::Note(1, ctx.r3.u32, 0, uint32_t(ctx.lr));
  __imp__sub_8211A338(ctx, base);
}
// v0.8.1 R39 ROOT CAUSE (build 148): the file gate was blind to the read's START. The request functions
// sub_82119350 / sub_82119480 (called by the fight's own frame when a transformation is ordered) link the request
// into the queue and call the pick-up sub_82118B90 themselves: with an idle queue the pick-up runs
// sub_82118820 at once - the buffer is taken from the game heap and the read is started INSIDE THE FIGHT'S FRAME, a
// frame that can still be rewound (every "request first seen" line showed "state 2": already reading when the
// file step first met it). The gate only held the queue's later steps. A rewind over that frame took the
// request and its buffer back while the read (and then the decoder) went on; when the corrected frames made the
// request again, the heap handed out another address: the first read's data went into memory that was free or
// someone else's by then (r147_fa2: "heap audit: FAIL at live frame start (frame 3018; last rewind at frame 3015
// depth 7): block D6862940: its footer D6882980 points back to 00000000" = file data over a footer, decoder job
// "result 0", then the malloc crash of r144_ltr with the same registers; r142_t3: the same frames).
// rb2_file_pickup_gate: while the engine tracks, a pick-up asked for by the main thread OUTSIDE the live file step
// (a fight frame, live or resimulated) does nothing. The request stays what the gate's design always assumed - plain
// frame state, taken back by a rewind and made again by the corrected frames - and the queue's own step
// (sub_82118D88 at 0x82118DD8: head waiting -> pick-up) starts the read, in the live file step, when no prediction
// is unsettled. The read starts one frame later (more while predictions settle / the guest staggers).
REXCVAR_DEFINE_BOOL(rb2_file_pickup_gate, true, "Debug",
                    "Rollback: a file request made by a fight frame is picked up (its read started) by the file step, never "
                    "inside the frame that made it (off = builds up to 147: the read started in a frame that could be rewound)");
REX_EXTERN(__imp__sub_82118B90);
extern "C" REX_FUNC(sub_82118B90) {
  if (rb2_main && rb2_track_on && !filegate::g_in_step && sync::s.stage == 2 && sync::s.active && filegate::On() &&
      REXCVAR_GET(rb2_file_pickup_gate)) {
    const uint32_t head = filegate::Word(filegate::kMgr + 28);
    if (head >= 0x40000000u && filegate::Word(head + 100) == 0) {  // a request that this call would have started
      const int64_t n = (g_in_resim ? g_pickup_deferred_resim : g_pickup_deferred_live).fetch_add(1, std::memory_order_relaxed) + 1;
      if (n <= 12 && !g_in_resim)
        REXLOG_INFO("[RB2] file gate: request {:08X} made in engine frame {} is left to the file step (pick-up not run in the frame, lr {:08X})",
                    head, sync::g_cur_frame, uint32_t(ctx.lr));
    }
    return;
  }
  __imp__sub_82118B90(ctx, base);
}
// sub_82118D88: the queue's step (one call per live frame, from the file step)
REX_EXTERN(__imp__sub_82118D88);
extern "C" REX_FUNC(sub_82118D88) {
  using namespace filegate;
  const bool gated = g_in_step && On();
  const bool pending = g_in_step && Pending();
  if (gated) {
    if (!pending) {
      g_open = false;
      g_pending_since = -1;
    } else if (!g_open) {
      if (g_pending_since < 0) {
        g_pending_since = sync::g_cur_frame;
        static int shown = 0;  // diagnostics: what a request looks like at its first step
        if (const std::string& spec = REXCVAR_GET(rb2_gate_rw_test); !spec.empty()) {
          int after = 0, depth = 0;
          if (std::sscanf(spec.c_str(), "%d:%d", &after, &depth) == 2 && after > 0 && depth > 0) {
            g_gate_rw_at = sync::g_cur_frame + after;
            g_gate_rw_depth = depth;
          }
        }
        if (shown++ < 12) {
          const uint32_t rq = Word(kMgr + 28);
          REXLOG_INFO("[RB2] file gate: request first seen at engine frame {}: countdown {} request {:08X} state {} type {}", sync::g_cur_frame,
                      Word(kMgr + 12), rq, rq >= 0x40000000u ? Word(rq + 100) : 0, rq >= 0x40000000u ? Word(rq) >> 24 : 0);
        }
      }
      if (Staggering()) {  // R32: the guest's start waits; it predicts on meanwhile (the request is plain frame state)
        ++g_stagger_frames;
        return;
      }
      if (Unsettled() && g_held_run < 90) {  // a rewind could still go back over this frame: the queue does not step
        ++g_held_run;
        ++g_pickup_held;
        return;
      }
      if (g_held_run >= 90) {
        ++g_pickup_forced;
        REXLOG_WARN("[RB2] file gate: the queue starts after {} held frames with predictions still unsettled (engine frame {})",
                    g_held_run, sync::g_cur_frame);
      }
      g_held_run = 0;
      g_open = true;
      ++g_pickups;
    }
  }
  __imp__sub_82118D88(ctx, base);
  if (g_in_step && (pending || Pending())) g_event = true;  // it stepped with a request: a frame of queue work
  if (gated && !Pending()) {
    g_open = false;
    g_pending_since = -1;
  }
}
extern "C" REX_FUNC(sub_82119320) {
  // R30: the live step of the main thread while an engine runs (rb2_tbit 1 = the frame's own stores)
  const bool live_step = rb2_main && rb2_track_on && !g_in_resim && rb2_tbit == 1 && sync::s.stage == 2 && sync::s.active;
  bool pending0 = false;
  if (live_step) {
    pending0 = filegate::Pending();
    filegate::g_event = false;
    filegate::g_in_step = true;
  }
  if (live_step) {
    fsdiag::g_on = REXCVAR_GET(rb2_fs_diag);
    if (fsdiag::g_on) { fsdiag::Report(sync::g_cur_frame); fsdiag::g_in = true; }
  }
  if (!REXCVAR_GET(rb2_cri_outside)) {
    __imp__sub_82119320(ctx, base);
  } else {
    const bool was = cri::g_outside;
    cri::g_outside = true;
    __imp__sub_82119320(ctx, base);
    cri::g_outside = was;
  }
  fsdiag::g_in = false;
  if (live_step) {
    filegate::g_in_step = false;
    if (filegate::g_event) filegate::MarkWork(sync::g_cur_frame);
    if (filegate::g_event && !pending0 && !filegate::Pending()) ++filegate::g_work_ungated;  // diagnostics: work the gate does not see
  }
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
bool predf[N];      // consumed with a prediction
int32_t heldf[N];   // R30: consumed with the real input although it had not "arrived" (the file gate held the prediction)
int32_t cnow = -1;
int64_t rewind_to = -1;
uint64_t predicted = 0, mispredicted = 0, confirmed = 0, bad = 0, consumes = 0, held_file = 0;
int Arrival(int32_t c) {
  const uint32_t h = uint32_t(c) * 2654435761u;
  const int j = std::max(0, REXCVAR_GET(rb2_loop_jitter));
  return c + REXCVAR_GET(rb2_loop) + int((h >> 16) % uint32_t(j + 1));
}
uint64_t True(int32_t c) {
  const int32_t src = c - std::max(0, REXCVAR_GET(rb2_loop_lag));
  return (src >= 0 && p1f[src % N] == src) ? p1[src % N] : 0;
}
// R30: a consumed prediction whose real input has not arrived yet (it can still turn out wrong -> a rewind there)
bool Unsettled() {
  if (REXCVAR_GET(rb2_loop) <= 0 || cnow < 0) return false;
  for (int32_t k = std::max(0, cnow - 40); k <= cnow; ++k) {
    if (usedf[k % N] == k && predf[k % N] && Arrival(k) > cnow) return true;
  }
  return false;
}
}  // namespace loop
int64_t BurstLimitRb2LoopPeekRewind() { return loop::rewind_to; }
int64_t BurstLimitRb2LoopTakeRewind() {
  const int64_t to = loop::rewind_to;
  loop::rewind_to = -1;
  return to;
}
// v0.8.1 R8: the engine's numbers of ONE match (since the last call with reset = true), for the
// "[OnlineMatch] summary" line of burstlimit_online.cpp. Main thread only.
uint64_t BurstLimitTexGuardSkips();
std::string BurstLimitRb2MatchStats(bool reset) {
  struct Snap {
    int64_t rewinds = 0, depth = 0, too_deep = 0, refused = 0, noisy = 0, thread = 0, nologs = 0, park_ok = 0, park_to = 0, deferred = 0,
            over_work = 0, pred_held = 0, work_frames = 0, periods = 0, start_held = 0, forced = 0, stagger = 0, cine = 0, frames = 0;
    double cost = 0, park_ms = 0;
    uint64_t skips = 0;
  };
  static Snap base;
  using sync::t;
  Snap n;
  n.rewinds = t.online_rewinds; n.depth = t.online_depth_sum; n.too_deep = t.online_too_deep; n.refused = t.online_refused;
  n.noisy = t.refused_noisy; n.thread = t.refused_thread; n.nologs = t.refused_nologs;
  n.park_ok = sync::g_park_ok.load(); n.park_to = sync::g_park_timeouts.load(); n.deferred = int64_t(sync::g_park_deferred.load());
  n.over_work = filegate::g_rewinds_over_work; n.pred_held = filegate::g_pred_held; n.work_frames = filegate::g_work_frames;
  n.periods = filegate::g_pickups; n.start_held = filegate::g_pickup_held; n.forced = filegate::g_pickup_forced;
  n.stagger = filegate::g_stagger_frames; n.cine = sync::g_cine_rewinds; n.frames = t.frames;
  n.cost = t.online_cost_ms; n.park_ms = sync::g_park_wait_ms_sum; n.skips = BurstLimitTexGuardSkips();
  const int64_t rw = n.rewinds - base.rewinds, parks = (n.park_ok - base.park_ok) + (n.park_to - base.park_to);
  const int64_t frames = n.frames - base.frames;
  std::string out = fmt::format(
      "engine frames {} | rollbacks {} ({:.1f} per minute), depth avg {:.2f} max {}, cost avg {:.2f} ms peak {:.2f}, over 16.7 ms {} | park wait avg "
      "{:.2f} ms peak {:.1f}, timed out {}, rewinds deferred {} | refused: settling {} too deep {} busy log {} new thread {} no logs {} | "
      "through cinematic frames {} | file gate: work frames {} periods {} start held {} forced {} staggered {} predictions held {} rewinds over "
      "queue work {} | texture guard skips {} | fx node ring: frees past the in-use bound {} (never-restored slot hit {}), fx audit failures {}",
      frames, rw, frames ? double(rw) * 3600.0 / double(frames) : 0.0, rw ? double(n.depth - base.depth) / double(rw) : 0.0, t.match_max_depth,
      rw ? (n.cost - base.cost) / double(rw) : 0.0, t.match_cost_peak, t.match_over_budget,
      parks ? (n.park_ms - base.park_ms) / double(parks) : 0.0, sync::g_park_wait_ms_peak_match, n.park_to - base.park_to, n.deferred - base.deferred,
      n.refused - base.refused, n.too_deep - base.too_deep, n.noisy - base.noisy, n.thread - base.thread, n.nologs - base.nologs, n.cine - base.cine,
      n.work_frames - base.work_frames, n.periods - base.periods, n.start_held - base.start_held, n.forced - base.forced, n.stagger - base.stagger,
      n.pred_held - base.pred_held, n.over_work - base.over_work, n.skips - base.skips, sync::g_fx_ring_hazards, sync::g_fx_ring_hazards_live,
      sync::g_fx_audit_fails);
  out += " | " + BurstLimitRb2HeapStats();  // R39 (cumulative for the process)
  if (reset) {
    base = n;
    t.match_max_depth = 0;
    t.match_cost_peak = 0;
    t.match_over_budget = 0;
    sync::g_park_wait_ms_peak_match = 0;
  }
  return out;
}
std::string BurstLimitRb2LoopStats() {
  return fmt::format("consumes {} predicted {} mispredicted {} confirmed {} WRONG-AT-CONFIRM {}; rewinds deferred (park) {}; {}; {} "
                     "(loopback: {} consumes held); {}; fx node ring: frees past the in-use bound {} (hitting a never-restored slot {}), most frees undone {}, least in use {}; "
                     "fx audit runs {} failures {}; {}",
                     loop::consumes, loop::predicted, loop::mispredicted, loop::confirmed, loop::bad, sync::g_park_deferred.load(),
                     BurstLimitRb2PaceStats(), filegate::Stats(), loop::held_file, BurstLimitReadyGateStats(), sync::g_fx_ring_hazards,
                     sync::g_fx_ring_hazards_live, sync::g_fx_ring_max_frees, sync::g_fx_ring_min_inuse, sync::g_fx_audit_runs, sync::g_fx_audit_fails, sync::ResAuditStats()) + "; " + BurstLimitRb2HeapStats();
}
// From the consume (netinput's hook in sub_8217E4E8, offline driver): frame c is about to be consumed.
void BurstLimitRb2OnConsume(rex::memory::Memory* m, int32_t c) {
  using namespace loop;
  const bool loop_on = REXCVAR_GET(rb2_loop) > 0;  // rb2_stress_pad: only player 1's record / replay and the ki refill
  const bool noeng = REXCVAR_GET(rb2_pad_noengine);  // R40 control: the loopback's inputs with the engine never tracking
  if (noeng ? c < 0 : ((!loop_on && !REXCVAR_GET(rb2_stress_pad)) || REXCVAR_GET(rb2_sync) <= 0 || sync::s.stage != 2 || !sync::s.active || c < 0)) return;
  const uint32_t manager = U32(m, 0x841B5EA4);
  if (!manager) return;
  auto slot = [&](int p) { return manager + uint32_t(p) * 784 + (uint32_t(c) % 32) * 24; };
  if (int32_t(U32(m, slot(0) + 16)) != c || int32_t(U32(m, slot(1) + 16)) != c) return;
  auto load64 = [&](uint32_t a) { return rex::memory::load_and_swap<uint64_t>(m->TranslateVirtual<const uint8_t*>(a)); };
  auto store64 = [&](uint32_t a, uint64_t v) { rex::memory::store_and_swap<uint64_t>(m->TranslateVirtual<uint8_t*>(a), v); };
  const bool resim = g_in_resim;
  ++consumes;
  if ((REXCVAR_GET(rb2_loop_tr) > 0 || REXCVAR_GET(rb2_loop_ult) > 0) && c % 30 == 0 && U32(m, 0x841B5228) == 2) {  // both fighters: full ki (live and resim alike)
    for (int i = 0; i < 2; ++i) {
      const uint32_t player = U32(m, 0x84239C10 + 4 * i);
      const uint32_t fighter = player && Committed(m, player + 44, 4) ? U32(m, player + 44) : 0;
      if (fighter && Committed(m, fighter + 1244, 4)) {
        const uint32_t w = U32(m, fighter + 1244);  // ki (high half) | maximum (low half)
        rex::memory::store_and_swap<uint32_t>(m->TranslateVirtual<uint8_t*>(fighter + 1244), (w & 0xFFFFu) << 16 | (w & 0xFFFFu));
        if (rb2_track_on) sync::HostWrite(fighter + 1244, 4, 0);
      }
    }
  }
  if (!resim) {
    p1[c % N] = load64(slot(0));
    p1f[c % N] = c;
    cnow = std::max(cnow, c);
  } else if (p1f[c % N] == c) {
    store64(slot(0), p1[c % N]);
  }
  if (noeng) {  // player 2 = player 1's command of rb2_loop_lag frames earlier, at once (no prediction, no engine)
    const int32_t k = c - REXCVAR_GET(rb2_loop_lag);
    store64(slot(1), k >= 0 && p1f[k % N] == k ? p1[k % N] : 0);
    return;
  }
  if (!loop_on) return;
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
  } else if (resim ? heldf[c % N] == c : (filegate::Hold() || sync::g_log_oom.load(std::memory_order_relaxed))) {  // R18: no prediction after a failed log either
    // R30: the file queue has work - the fight waits for the real input (here: it is used at once), in the frame's
    // resimulations too
    v = True(c);
    if (!resim) { heldf[c % N] = c; ++held_file; ++filegate::g_pred_held; }
  } else {
    pred = true;
    for (int32_t k = c - 1; k >= std::max(0, c - 64); --k) {
      if (Arrival(k) <= cnow) { v = True(k); break; }
    }
  }
  store64(slot(1), v);
  used[c % N] = v;
  usedf[c % N] = c;
  predf[c % N] = pred;
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
REXCVAR_DEFINE_BOOL(rb2_cri_dirty, true, "Debug", "Rollback: the bytes a CRI answer writes in a resimulated frame mark their page dirty (off = v0.8.0: fine mark only)");
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
          // v0.8.1 R10: noted like a main-thread host store (page dirty bit + fine mark). With the fine mark only,
          // the page could miss this resim frame's log: its shadow stayed stale and a later rewind to a frame
          // after the call put back the state from before it.
          if (REXCVAR_GET(rb2_cri_dirty)) {
            sync::MarkOr(&rb2_dirty[(b.a >> 12) & 0xFFFFF], 1);
            sync::MarkOr(&rb2_mfine[(b.a >> 7) & 0x1FFFFFF], uint8_t(1u << ((b.a >> 4) & 7)));
          } else {
            rb2_mfine[(b.a >> 7) & 0x1FFFFFF] |= uint8_t(1u << ((b.a >> 4) & 7));  // a main-thread write (lost-mark detection)
          }
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
  const unsigned char b = kind ? kind : rb2_tbit;
  sync::MarkOr(&rb2_dirty[x >> 12], b);  // v0.8.1 R10: atomic when the bit is not set yet
  if (kind == 16) {
    sync::MarkOr(&rb2_sfine[x >> 7], static_cast<unsigned char>(1u << ((x >> 4) & 7)));
  } else if (b == 2) {  // other threads' writes per 16-byte block (kept live when a frame's page is rewound)
    const unsigned int last = x + (n ? n - 1 : 0);
    for (unsigned int q = x >> 4; q <= (last >> 4); ++q) sync::MarkOr(&rb2_ffine[q >> 3], static_cast<unsigned char>(1u << (q & 7)));
  } else if (b & 13) {  // the main thread's (rb2_mfine: lost-mark detection)
    const unsigned int last = x + (n ? n - 1 : 0);
    for (unsigned int q = x >> 4; q <= (last >> 4); ++q) sync::MarkOr(&rb2_mfine[q >> 3], static_cast<unsigned char>(1u << (q & 7)));
  } else if (b == 32) {  // R39: the fill class (data a loader thread writes once): this frame's marks
    const unsigned int last = x + (n ? n - 1 : 0);
    for (unsigned int q = x >> 4; q <= (last >> 4); ++q) sync::MarkOr(&rb2_lfine[q >> 3], static_cast<unsigned char>(1u << (q & 7)));
  }
  if (__builtin_expect(fsdiag::g_on, 0) && rb2_main && kind != 16) fsdiag::Note(x, uintptr_t(__builtin_return_address(0)));  // R30 diagnostics
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
// R40 diagnostics (rb2_d3d_callers): D3D library entries of the other threads (render loop, draw worker) by function
// and guest caller, logged per period - which caller stops drawing when the stage goes
std::mutex g_d3d_callers_mu;
std::map<std::pair<uint32_t, uint32_t>, uint64_t> g_d3d_callers;
extern "C" void rb2_d3d_hit(unsigned int a) {
  if (rb2_d3d_watch > 1 && !rb2_main) {
    PPCContext* c = rex::runtime::current_ppc_context();
    std::lock_guard<std::mutex> lock(g_d3d_callers_mu);
    ++g_d3d_callers[{a, c ? uint32_t(c->lr) : 0}];
    return;
  }
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
// R40 diagnostics: an effect script reached a sync point (sub_8214E560 -> sub_82292328, lr 8214E5B4) in a live frame
void BurstLimitRb2NoteFxSync() {
  if (g_in_resim) return;
  const int n = REXCVAR_GET(rb2_diff_fx);
  if (n > 0 && g_diff_fx_left <= 0) {
    g_diff_fx_left = n;
    Log(fmt::format("R40: effect script sync point at engine frame {}: the next {} stress rewinds log their differing words", sync::g_cur_frame, n));
  }
}
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
  if (!BurstLimitRb2Active() || g_draining) { g_hold_reason = "engine paused"; return false; }
  if (sync::g_park_deferred_now) { g_hold_reason = "rewind pending (park)"; return false; }  // R9: a rewind is pending (render loop not parked): no new predictions
  if (filegate::Hold()) { ++filegate::g_pred_held; return false; }  // R30: the file queue has work (never rewound over); sets the reason
  if (sync::s.frame < sync::SettleFrames() + 2) { g_hold_reason = "settle frames"; return false; }
  if (sync::g_log_oom.load(std::memory_order_relaxed)) { g_hold_reason = "out of memory (no rollback until the next pause)"; return false; }  // R18
  if (sync::t.logs.empty() || !sync::t.logs.back().quiet) { g_hold_reason = "log not quiet (workers / resource cache)"; return false; }
  if (sync::g_thread_created_frame.load() >= sync::s.frame - 1) { g_hold_reason = "new guest thread"; return false; }
  return true;
}
const char* BurstLimitRb2HoldReason() { return g_hold_reason; }
int64_t BurstLimitRb2CurrentFrame() { return sync::g_cur_frame; }
void BurstLimitRb2RequestRewind(int64_t engine_frame) {
  if (loop::rewind_to < 0 || engine_frame < loop::rewind_to) loop::rewind_to = engine_frame;
}
// review L3: can a rewind to this frame be taken at all (engine tracking, past the settle frames, logs kept since)?
// The sync guard's time-out skips a request only when it can; otherwise the request goes out (v0.8.0 behaviour)
// instead of being lost with a refused rewind.
void BurstLimitRb2DropRewind() { loop::rewind_to = -1; }
bool BurstLimitRb2RewindPossible(int64_t engine_frame) {
  if (!BurstLimitRb2Active() || sync::g_log_oom.load(std::memory_order_relaxed)) return false;
  if (engine_frame < sync::SettleFrames()) return false;
  return !sync::t.logs.empty() && sync::t.logs.front().frame <= engine_frame;
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
// v0.8.1 R16 (build 149): sub_8214E0C0(id, ..) makes an instance of an effect resource: it looks the id up in the
// table 0x84158E68 (40 entries of 24 bytes {id, used, .., object +12, +16, +20}; no store into it in a whole fight:
// r148_wt1, store watch), takes a free slot of the eight at 0x841593BC (76 bytes) and reads the entry - without a
// check for "id not found": r11 = 0, `lwz r10,12(r11)` reads guest 0xC. That is the crash of r142_t1 (frame 4134, 63 ms
// after a rewind) and r148_fw1 (frame 4883, 79 ms after a rewind, id 30 asked by the script command 8214E248 of
// context DD3048C0; no transformation in that run, heap audit clean) - the "unexplained interpreter crash in
// 8214E0C0" of the old builds. With no free slot the function already returns 0, so its callers know that
// answer: an id that is not in the table now gets it too, with a log line. WHY a script asks for an id that was
// never registered is NOT known (a script context that a rewind left inconsistent is the suspect: both cases
// came within 80 ms of a rewind) - guarded, not root-caused.
REXCVAR_DEFINE_BOOL(rb2_fx_guard_id, true, "Debug",
                    "Rollback: an effect instance asked for a resource id that is not in the table (sub_8214E0C0 would read "
                    "through NULL) gets \"no instance\" and a log line");
std::atomic<int64_t> g_fx_id_skips{0};
REX_EXTERN(__imp__sub_8214E0C0);
extern "C" REX_FUNC(sub_8214E0C0) {
  if (REXCVAR_GET(rb2_fx_guard_id)) {
    constexpr uint32_t kTable = 0x841584E0u + 2440u, kEnd = 0x841584E0u + 3400u;
    auto u32 = [&](uint32_t a) { return rex::memory::load_and_swap<uint32_t>(base + a); };
    const uint32_t id = ctx.r3.u32;
    bool found = false;
    for (uint32_t e = kTable; e < kEnd && !found; e += 24) found = u32(e + 4) != 0 && u32(e) == id;
    if (!found) {
      const int64_t n = g_fx_id_skips.fetch_add(1, std::memory_order_relaxed) + 1;
      BurstLimitOnlineAlert("effect id guard", fmt::format("an effect asked for resource id {} that is not in the table (engine frame {})", int32_t(id),
                                                           BurstLimitRb2CurrentFrame()));
      if (n <= 20 || (n & (n - 1)) == 0) {
        std::string ids;
        for (uint32_t e = kTable; e < kEnd; e += 24) if (u32(e + 4) != 0) ids += fmt::format(" {}", int32_t(u32(e)));
        const uint32_t sc = ctx.r31.u32, pr = ctx.r11.u32;  // the caller's script context / parameter block (8214E248)
        std::string words;
        if (sc >= 0x40000000u && g_mem_for_watch && Committed(g_mem_for_watch, sc, 64))
          for (uint32_t o = 0; o < 64; o += 4) words += fmt::format(" {:08X}", u32(sc + o));
        std::string params;
        if (pr >= 0x40000000u && g_mem_for_watch && Committed(g_mem_for_watch, pr, 16))
          for (uint32_t o = 0; o < 16; o += 4) params += fmt::format(" {:08X}", u32(pr + o));
        REXLOG_WARN("[RB2] effect instance of resource id {} SKIPPED: the id is not in the table (ids in use:{}) | lr {:08X}, {} frame {}, last "
                    "rewind at frame {} depth {} {} ms ago | caller r31 {:08X}:{} | r11 {:08X}:{} ({} so far)", int32_t(id), ids,
                    uint32_t(ctx.lr), g_in_resim ? "resim" : "live", sync::g_cur_frame_for_hook.load(), g_last_rw_frame, g_last_rw_depth,
                    g_last_rw_tick ? int64_t(GetTickCount64() - g_last_rw_tick) : -1, sc, words, pr, params, n);
        if (n <= 6 && sc >= 0x40000000u)  // R16b: did the last restore keep blocks of the context / the parameters live?
          REXLOG_WARN("[RB2] effect instance skip {}: context blocks{} | parameter blocks{}", n, BurstLimitRb2HeapBlocks(sc, 64),
                      pr >= 0x40000000u ? BurstLimitRb2HeapBlocks(pr, 16) : std::string());
        rex::FlushLogging();
      }
      ctx.r3.u64 = 0;
      return;
    }
  }
  __imp__sub_8214E0C0(ctx, base);
}
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

// R7: the engine state of a crash record (no allocation, no logger; read from whatever thread crashed).
int EngineStateRaw(char* out, int cap) {
  auto* runtime = rex::Runtime::instance();
  rex::memory::Memory* mem = runtime ? runtime->memory() : nullptr;
  const uint32_t phase = mem && HostReadable(mem->TranslateVirtual<const uint8_t*>(0x841B5228u), 4)
                             ? rex::memory::load_and_swap<uint32_t>(mem->TranslateVirtual<const uint8_t*>(0x841B5228u)) : 0xFFFFFFFFu;
  const uint64_t now = GetTickCount64();
  int n = std::snprintf(out, size_t(cap),
                        "engine: frame %lld stage %d active %d in-resim %d draining %d online-rollback %d tracking %d | battle phase %u | "
                        "rollbacks %lld (max depth %d, too deep %lld, refused %lld), last rewind at frame %lld depth %d %lld ms ago | "
                        "park ok %lld timed out %lld deferred %lld | texture guard skips %llu\r\n",
                        (long long)sync::s.frame, int(sync::s.stage), int(sync::s.active), int(g_in_resim), int(g_draining),
                        int(BurstLimitRollback2Online()), int(rb2_track_on), phase, (long long)sync::t.online_rewinds,
                        sync::t.online_max_depth, (long long)sync::t.online_too_deep, (long long)sync::t.online_refused,
                        (long long)g_last_rw_frame, g_last_rw_depth, g_last_rw_tick ? (long long)(now - g_last_rw_tick) : -1LL,
                        (long long)sync::g_park_ok.load(), (long long)sync::g_park_timeouts.load(), (long long)sync::g_park_deferred.load(),
                        (unsigned long long)BurstLimitTexGuardSkips());
  if (n < 0 || n >= cap - 300) return n;
  n += std::snprintf(out + n, size_t(cap - n),
                     "file gate: on %d, queue work frames %lld, work periods %lld, start held %lld (forced %lld), predictions held %lld, "
                     "rewinds over queue work %lld | loopback: predicted %llu mispredicted %llu\r\n",
                     int(filegate::On()), (long long)filegate::g_work_frames, (long long)filegate::g_pickups,
                     (long long)filegate::g_pickup_held, (long long)filegate::g_pickup_forced, (long long)filegate::g_pred_held,
                     (long long)filegate::g_rewinds_over_work, (unsigned long long)loop::predicted,
                     (unsigned long long)loop::mispredicted);
  if (n < 0 || n >= cap - 1400) return n;
  n += std::snprintf(out + n, size_t(cap - n),
                     "game heap: audit runs %lld failures %lld | stale-mark blocks kept %lld (heap %lld) in %lld rewinds, age mode %d | "
                     "calls (newest last):",
                     (long long)heap::g_audit_runs, (long long)heap::g_audit_fails, (long long)sync::t.stale_blocks,
                     (long long)sync::t.stale_heap_blocks, (long long)sync::t.stale_rewinds, int(REXCVAR_GET(rb2_ffine_age)));
  const uint32_t ops = heap::g_op_n.load(std::memory_order_relaxed);
  for (uint32_t q = ops > 14 ? ops - 14 : 0; q < ops && n > 0 && n < cap - 120; ++q) {
    const heap::Op& o = heap::g_ops[q & 255];
    n += std::snprintf(out + n, size_t(cap - n), " [f%lld %s %s %08X lr %08X]", (long long)o.frame, heap::kCls[o.cls % 6],
                       o.op ? "free" : "alloc", o.ptr, o.lr);
  }
  if (n > 0 && n < cap - 4) n += std::snprintf(out + n, size_t(cap - n), "\r\n");
  return n;
}

// ---- v0.8.1 R21: one-time settings migration ----------------------------------------------------
// The Update button keeps the old burstlimit.toml. v0.7.3 and older wrote online_input_delay = "4" (the lockstep
// default); with it every rollback fight runs at a fixed 4-frame delay instead of the 2 the rollback netcode picks.
// Once per installation (settings_epoch < 81): "4" becomes "auto". A "4" chosen later stays.
REXCVAR_DEFINE_INT32(settings_epoch, 0, "Internal", "Settings migration marker (81 = v0.8.1); not a setting");
REXCVAR_DECLARE(std::string, online_input_delay);
void BurstLimitSettingsMigrate(const std::filesystem::path& config_path) {
  if (REXCVAR_GET(settings_epoch) >= 81) return;
  // review L4: only the toml's value is migrated - a "4" given on the command line (a shortcut, a launcher) wins over
  // the toml for this start and must neither be overridden nor turned into a saved "auto"
  const rex::cvar::Source delay_src = rex::cvar::GetFlagSource("online_input_delay");
  if (delay_src != rex::cvar::Source::kConfig && delay_src != rex::cvar::Source::kDefault) {
    REXLOG_INFO("[Settings] online_input_delay comes from the command line / environment: not migrated, settings_epoch not written");
    return;
  }
  if (REXCVAR_GET(online_input_delay) == "4") {
    rex::cvar::SetFlagByName("online_input_delay", "auto");
    REXLOG_WARN("[Settings] online_input_delay 4 -> auto (the old lockstep default; rollback picks its delay from the ping)");
  }
  rex::cvar::SetFlagByName("settings_epoch", "81");
  if (!config_path.empty()) rex::cvar::SaveConfig(config_path);
  REXLOG_INFO("[Settings] settings_epoch 81 written");
}

// ---- v0.8.1 R12c: 1 ms timer period ----------------------------------------------------------------
// Nothing asked Windows for a finer timer: Sleep(1) / sleep_for(1 ms) (the sync-event guard, the park fallback, the
// net thread) can last up to 15.6 ms, and Windows 11 ignores a raised period for a window that is not in front
// unless the process opts out of that throttling. One line with the measured values before and after.
REXCVAR_DEFINE_BOOL(timer_period_1ms, true, "Patches",
                    "Ask Windows for a 1 ms timer period while the game runs (a 1 ms sleep then lasts 1-2 ms instead of up to "
                    "15.6 ms; also when the window is not in front)");
#pragma comment(lib, "winmm.lib")
namespace {
double MeasureSleep1() {
  const auto t0 = std::chrono::steady_clock::now();
  for (int i = 0; i < 8; ++i) Sleep(1);
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / 8.0;
}
double TimerResolutionMs() {
  using Fn = LONG(WINAPI*)(PULONG, PULONG, PULONG);
  HMODULE nt = GetModuleHandleW(L"ntdll.dll");
  Fn fn = nt ? reinterpret_cast<Fn>(GetProcAddress(nt, "NtQueryTimerResolution")) : nullptr;
  ULONG lo = 0, hi = 0, cur = 0;
  if (!fn || fn(&lo, &hi, &cur) != 0) return 0;
  return double(cur) / 10000.0;
}
}  // namespace
void BurstLimitTimerPeriodLog(const char* when) {
  REXLOG_INFO("[Timer] {}: timer period {:.2f} ms, Sleep(1) lasts {:.2f} ms", when, TimerResolutionMs(), MeasureSleep1());
}
void BurstLimitTimerPeriodSetup() {
  const double res0 = TimerResolutionMs(), sleep0 = MeasureSleep1();
  bool raised = false, unthrottled = false;
  if (REXCVAR_GET(timer_period_1ms)) {
    raised = timeBeginPeriod(1) == 0;
    // Windows 11: honour the period also when the window is occluded / in the background
    struct { ULONG Version, ControlMask, StateMask; } state = {1, 0x4 /* IGNORE_TIMER_RESOLUTION */, 0};
    unthrottled = SetProcessInformation(GetCurrentProcess(), static_cast<PROCESS_INFORMATION_CLASS>(4) /* ProcessPowerThrottling */,
                                        &state, sizeof(state)) != 0;
  }
  REXLOG_INFO("[Timer] start: timer period {:.2f} ms -> {:.2f} ms, Sleep(1) {:.2f} ms -> {:.2f} ms (timer_period_1ms {}, raised {}, "
              "background throttling off {})",
              res0, TimerResolutionMs(), sleep0, MeasureSleep1(), REXCVAR_GET(timer_period_1ms), raised, unthrottled);
}

// ---- v0.8.1 R34: crash record, "pointers (engine view)" -------------------------------------------
// For every guest register of the crashing thread that points at data: is its page tracked by the engine
// (restored by a rewind) or not, excluded or not, the page's write-class bits of the current frame, the 16-byte
// block's marks (foreign = another thread wrote it since the last log and a rewind keeps it live; prev = the
// generation before; main = the main thread wrote it), and the block's four words. No allocation.
int PointerNotesRaw(char* out, int cap, const uint32_t* ptrs, int count) {
  auto* runtime = rex::Runtime::instance();
  rex::memory::Memory* mem = runtime ? runtime->memory() : nullptr;
  int n = std::snprintf(out, size_t(cap), "pointers (engine view):");
  if (!mem || !rb2_track_on || sync::t.gp.empty()) {
    n += std::snprintf(out + n, size_t(cap - n), " the engine is not tracking\r\n");
    return n;
  }
  uint32_t seen[24];
  int ns = 0;
  for (int i = 0; i < count && n < cap - 260; ++i) {
    const uint32_t a = ptrs[i];
    if (a < 0x40000000u || (a >= 0x70000000u && a < 0x80000000u)) continue;  // not a data pointer / a thread stack
    bool dup = false;
    for (int k = 0; k < ns; ++k) dup = dup || seen[k] == (a & ~15u);
    if (dup) continue;
    if (ns < 24) seen[ns++] = a & ~15u;
    const uint32_t page = a >> 12;
    int idx = -1;
    for (size_t k = 0; k < sync::t.gp.size(); ++k) {
      if (sync::t.gp[k] == page) { idx = int(k); break; }
    }
    const uint8_t* h = mem->TranslateVirtual<const uint8_t*>(a & ~15u);
    const size_t blk = size_t(a) >> 4;
    const int bit = int(blk & 7);
    n += std::snprintf(out + n, size_t(cap - n), " [%08X: %s%s, page bits %02X, block foreign %d prev %d main %d", a,
                       idx >= 0 ? "tracked" : "UNTRACKED", idx >= 0 && sync::s.excluded[size_t(idx)] ? " EXCLUDED" : "", unsigned(rb2_dirty[page]),
                       (rb2_ffine[blk >> 3] >> bit) & 1, (rb2_ffine_prev[blk >> 3] >> bit) & 1, (rb2_mfine[blk >> 3] >> bit) & 1);
    if (HostReadable(h, 16)) {
      n += std::snprintf(out + n, size_t(cap - n), ", %08X %08X %08X %08X]", rex::memory::load_and_swap<uint32_t>(h),
                         rex::memory::load_and_swap<uint32_t>(h + 4), rex::memory::load_and_swap<uint32_t>(h + 8),
                         rex::memory::load_and_swap<uint32_t>(h + 12));
    } else {
      n += std::snprintf(out + n, size_t(cap - n), ", not readable]");
    }
  }
  n += std::snprintf(out + n, size_t(cap - n), "\r\n");
  return n;
}

// ---- v0.8.1 R40 / R16b diagnostics: the effect manager's resource table (0x841584E0 + 2440, 40 x 24 bytes) -----------
// sub_8214D7A0 clears it (callers sub_8214B9B0, sub_8214BDF0), sub_8214DB30 -> sub_8214D9C8 registers an entry.
REXCVAR_DEFINE_BOOL(rb2_fx_table_log, false, "Debug", "Rollback diagnostics (R40 / R16b): log the effect resource table's resets and registrations (thread, frame, live / resim)");
REX_EXTERN(__imp__sub_8214B9B0);
REX_EXTERN(__imp__sub_8214BDF0);
REX_EXTERN(__imp__sub_8214DB30);
namespace {
std::atomic<int> g_fx_table_lines{0};
void FxTableLog(const char* what, PPCContext& ctx, uint32_t extra) {
  if (!REXCVAR_GET(rb2_fx_table_log) || g_fx_table_lines.fetch_add(1) >= 400) return;
  REXLOG_WARN("[RB2] R40 fx table {} lr {:08X} r3 {:08X} r4 {:08X} r5 {:08X} -> {:08X} | {} thread, engine frame {} {}", what, uint32_t(ctx.lr), ctx.r3.u32,
              ctx.r4.u32, ctx.r5.u32, extra, ThreadRole(), sync::g_cur_frame_for_hook.load(), g_in_resim ? "RESIM" : "live");
}
}  // namespace
extern "C" REX_FUNC(sub_8214B9B0) {
  FxTableLog("reset (8214B9B0)", ctx, 0);
  __imp__sub_8214B9B0(ctx, base);
}
// v0.8.1 R40: the stage vanishes after some effect-script cinematics (Cell's ultimate, an ultimate clash) - also
// with the rollback engine never tracking (r158_n2). The fight's start sets the effect manager's scene with
// sub_8214BDF0(scene, 1) from 0x8214C7F8: scene = 0 for the odd ("alternate") stage ids, 1 for the others
// (sub_8214B150); the scene's effect pack is registered as resource id 30. The cinematic's script command
// sub_8214E4C8 ends it with sub_8214BDF0(<script parameter>, 0) - parameter 0 - so on an even stage the manager
// switches to scene 0, whose pack was never loaded: the registration gets no pack (r5 = 0x28, id 0), id 30 is gone
// from the table (the R16b "effect instance of resource id 30" lookups) and the stage's scene is no longer drawn.
// rb2_fx_scene_fix: a script reset to a scene other than the fight's own keeps the fight's scene. Deterministic
// (the fight's scene is a function of the stage), the same on every PC, rollback on or off.
REXCVAR_DEFINE_BOOL(rb2_fx_scene_fix, true, "Patches",
                    "Effect-script cinematics (Cell's ultimate, ultimate clashes) end in the fight's own effect scene - without it the "
                    "stage vanished for the rest of the fight on the normal (even) stages");
namespace { int32_t g_fx_fight_scene = -1; std::atomic<int> g_fx_scene_fixes{0}; }
extern "C" REX_FUNC(sub_8214BDF0) {
  FxTableLog("reset (8214BDF0)", ctx, 0);
  const uint32_t lr = uint32_t(ctx.lr);
  if (lr == 0x8214C7FCu) g_fx_fight_scene = ctx.r3.s32;  // the fight's start: its scene
  if (lr == 0x8214E4E4u && REXCVAR_GET(rb2_fx_scene_fix) && g_fx_fight_scene >= 0 && ctx.r3.s32 != g_fx_fight_scene) {
    const int n = ++g_fx_scene_fixes;
    if (n <= 20 || (n & (n - 1)) == 0)
      REXLOG_WARN("[RB2] effect scene: a cinematic's script switches to scene {} - the fight's scene {} is kept (engine frame {}{}; {} so far)",
                  ctx.r3.s32, g_fx_fight_scene, sync::g_cur_frame_for_hook.load(), g_in_resim ? ", resim" : "", n);
    ctx.r3.s64 = g_fx_fight_scene;
  }
  __imp__sub_8214BDF0(ctx, base);
}
extern "C" REX_FUNC(sub_8214DB30) {
  const PPCRegister r3 = ctx.r3, r4 = ctx.r4, r5 = ctx.r5;
  const uint64_t lr = ctx.lr;
  __imp__sub_8214DB30(ctx, base);
  if (REXCVAR_GET(rb2_fx_table_log)) {
    PPCContext c2 = ctx;
    c2.r3 = r3; c2.r4 = r4; c2.r5 = r5; c2.lr = lr;
    FxTableLog("register (8214DB30)", c2, ctx.r3.u32);
  }
}
