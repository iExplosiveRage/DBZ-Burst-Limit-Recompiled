// v0.8.1 (plan_v81.md R2 / R3): the texture handle registry.
//
// The game resolves texture handles through the handle map 0x8438D900 (sub_820F37F8) and calls through the result
// without a NULL check in twelve places. Five were guarded in build 115 (BL_DRAW_GUARD, burstlimit_patches.cpp);
// a player's v0.8.0-beta crash (draw worker, sub_82105BE0 at 0x82105D18 reading guest 0) is one of the other seven.
// Here:
//  - guards at those seven dereferences (mid-asm hooks, bl_tex_guard): a dead object costs one skipped draw / bind;
//  - fault injection (bl_tex_fault = "S1:20000"): the n-th pass through a site behaves as a lookup miss - the
//    reproduction of the player's record with the guard off, the test of each guard with it on;
//  - an "obituary" ring of the last registry events (group releases, resource commands) with counters per command
//    type in live and resimulated frames, printed with a guard skip, an audit failure and the crash record;
//  - an audit of the registry (rb2_tex_audit): the map's 101 bucket trees, its free list and the group lists must
//    describe one consistent set after a rollback rewind.
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

#include <windows.h>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/memory.h>
#include <rex/memory/utils.h>
#include <rex/ppc.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>
#include <rex/runtime.h>

REXCVAR_DEFINE_BOOL(bl_tex_guard, true, "Patches",
                    "A texture whose handle was released is skipped by the seven draw / bind paths that would "
                    "otherwise read a NULL object and crash (v0.8.0-beta player crash at 82105BE0)");
REXCVAR_DEFINE_STRING(bl_tex_fault, "", "Debug",
                      "Test: Sx:n - the n-th pass through texture site Sx (1-7) behaves as a handle-map miss; n counts "
                      "while the rollback engine tracks, with a trailing b (S5:300b) from process start");
REXCVAR_DEFINE_INT32(rb2_tex_audit, 1, "Debug",
                     "Rollback: audit of the texture registry (0 off, 1 after every rewind / at engine pauses / every "
                     "600 frames, 2 every frame)");

// R13a (H2, measured in r119_ltr1: delete-class commands 1 and 6 were sent again by resimulated frames, 18 for 9
// live ones): the command channel to the render loop is never rewound, while the slot tables whose countdowns send
// the deletes are - so a rewind over the frame of a delete sent it a second time, and a second delete of a key /
// group that was created again in between released textures that were in use (the lookup miss of the crash).
REXCVAR_DEFINE_INT32(rb2_res_boundary, 1, "Debug",
                     "Rollback: a resource command a resimulated frame makes again (the same command the rewound "
                     "frames already sent) is answered from the record and not sent twice (0 off = v0.8.0)");

extern "C" thread_local unsigned char rb2_main;
bool BurstLimitRb2Active();
bool BurstLimitRb2InResim();
int64_t BurstLimitRb2CurrentFrame();
void BurstLimitOnlineAlert(const char* kind, const std::string& text);  // burstlimit_online.cpp (v0.8.1 R8, build 151)
void BurstLimitRb2FileEvent();  // R30: the file step started a read / sent a create (burstlimit_rb2.cpp, the file gate)

REX_EXTERN(__imp__RtlEnterCriticalSection);
REX_EXTERN(__imp__RtlLeaveCriticalSection);
REX_EXTERN(__imp__sub_8210D238);
REX_EXTERN(__imp__sub_820E57A0);
REX_EXTERN(__imp__sub_820E5830);

namespace {

constexpr uint32_t kMapLock = 0x8438D900u;   // the handle map's critical section
constexpr uint32_t kMap = 0x8438D980u;       // +0 node array, +4 free head, +8 capacity, +12 count, +16 nil node, +128 buckets
constexpr uint32_t kGroups = 0x8438EE80u;    // group g: list head +60 + 8g, entry count +64 + 8g; lock +28
constexpr int kSites = 7;

std::atomic<uint64_t> g_hits[kSites + 1];
std::atomic<uint64_t> g_skips[kSites + 1];
std::atomic<uint64_t> g_skips_total{0};

uint8_t* Base() {
  static uint8_t* base = nullptr;
  if (!base) {
    auto* runtime = rex::Runtime::instance();
    auto* memory = runtime ? runtime->memory() : nullptr;
    if (memory) base = memory->virtual_membase();
  }
  return base;
}
inline uint32_t U32(const uint8_t* base, uint32_t a) { return rex::memory::load_and_swap<uint32_t>(base + a); }

// ---- obituary ring -------------------------------------------------------------------------------------------
struct Obit {
  uint8_t kind;    // 'C' resource command, 'G' group release (sub_820E57A0), 'F' deferred flush (sub_820E5830)
  uint8_t resim;
  uint16_t thread;
  int32_t result;
  uint32_t a, b, c, d, e, lr;
  int64_t frame;
};
constexpr uint32_t kRing = 64;
Obit g_ring_cmd[kRing];
Obit g_ring_grp[kRing];
std::atomic<uint32_t> g_ring_cmd_n{0}, g_ring_grp_n{0};
std::atomic<uint64_t> g_cmd_count[2][32];   // [live / resim][type]
std::atomic<uint64_t> g_cmd_busy[2];        // result -3 (buffer in use)
std::atomic<uint64_t> g_cmd_full[2];        // result -1
std::atomic<uint64_t> g_grp_count[2];

void Push(Obit* ring, std::atomic<uint32_t>& n, const Obit& o) { ring[n.fetch_add(1, std::memory_order_relaxed) % kRing] = o; }

int FormatRing(char* out, int cap, const char* name, const Obit* ring, uint32_t n) {
  int w = std::snprintf(out, size_t(cap), "%s (newest last, %u in all):", name, n);
  const uint32_t have = n < kRing ? n : kRing;
  for (uint32_t i = 0; i < have && w < cap - 120; ++i) {
    const Obit& o = ring[(n - have + i) % kRing];
    w += std::snprintf(out + w, size_t(cap - w), " [%c f%lld%s t%u lr %08X: %X %X %X %X %X -> %d]", o.kind, (long long)o.frame,
                       o.resim ? "r" : "", unsigned(o.thread), o.lr, o.a, o.b, o.c, o.d, o.e, int(o.result));
  }
  if (w < cap - 2) w += std::snprintf(out + w, size_t(cap - w), "\r\n");
  return w;
}

// ---- resource command boundary (R13a) ---------------------------------------------------------------------------
struct CmdRec {
  uint32_t a, b, c, d, e;
  int32_t result;
  uint64_t used_gen;  // the rewind that matched it (0: sent live, not matched yet)
};
constexpr int kCmdPerFrame = 24;
constexpr int kCmdFrames = 64;  // the engine's ring
struct FrameCmds {
  int64_t frame = -1;
  int n = 0;
  CmdRec rec[kCmdPerFrame];
};
FrameCmds g_fc[kCmdFrames];
uint64_t g_rewind_gen = 1;
int64_t g_rewind_from = -1, g_rewind_to = -1;  // the frames the running rewind resimulates [from, to)
std::atomic<uint64_t> g_bnd_answered{0}, g_bnd_answered_other_frame{0}, g_bnd_new_in_resim{0}, g_bnd_overflow{0};
std::atomic<uint64_t> g_bnd_ghost[32];  // sent by a frame that was rewound and not made again, by type
// R30: a create (file 0, texture set 4) a LIVE frame sends although the identical create went out in the last 64
// frames - the file step handled the same finished load twice (its state was taken back by a rewind)
std::atomic<uint64_t> g_bnd_recreate[32];
std::atomic<uint64_t> g_file_starts{0}, g_file_restarts{0};  // sub_82118820: reads started / the same path again within 600 frames

FrameCmds& FrameOf(int64_t frame, bool create) {
  FrameCmds& f = g_fc[uint64_t(frame) % kCmdFrames];
  if (f.frame != frame && create) {
    f.frame = frame;
    f.n = 0;
  }
  return f;
}
CmdRec* Match(FrameCmds& f, int64_t frame, const Obit& o) {
  if (f.frame != frame) return nullptr;
  for (int i = 0; i < f.n; ++i) {
    CmdRec& r = f.rec[i];
    if (r.used_gen != g_rewind_gen && r.a == o.a && r.b == o.b && r.c == o.c && r.d == o.d && r.e == o.e) return &r;
  }
  return nullptr;
}

// ---- lag injection (round 6) ---------------------------------------------------------------------------------
// The released-handle family is a use-after-release between whoever frees a texture and a thread that still draws
// with it. This switch makes the drawing thread late on purpose: every N-th pass through site Sx sleeps M ms, so
// the rest of its batch runs that much later - with the guards on, a "texture skipped" line then shows the race
// without any fault injection (and with rollback off: that it is the game's own race).
REXCVAR_DEFINE_STRING(bl_tex_lag, "", "Debug",
                      "Test: Sx:n:ms - every n-th pass through texture site Sx (1-7) sleeps ms milliseconds (a late draw worker)");
struct Lag {
  int site = 0;
  uint64_t every = 0;
  uint32_t ms = 0;
  std::atomic<uint64_t> slept{0};
};
Lag g_lag;
void ParseLag() {
  unsigned site = 0, ms = 0;
  unsigned long long every = 0;
  if (std::sscanf(REXCVAR_GET(bl_tex_lag).c_str(), "S%u:%llu:%u", &site, &every, &ms) == 3 && site >= 1 && site <= 7 && every > 0 && ms > 0) {
    g_lag.site = int(site);
    g_lag.every = every;
    g_lag.ms = std::min(ms, 2000u);
    REXLOG_WARN("[TexGuard] lag injection armed: every {}th pass through S{} sleeps {} ms", every, site, g_lag.ms);
  }
}

// ---- fault injection -----------------------------------------------------------------------------------------
struct Fault {
  int site = 0;
  uint64_t n = 0;
  bool from_boot = false;
  std::atomic<uint64_t> count{0};
};
Fault g_fault;
std::once_flag g_fault_once;
void ParseFault() {
  const std::string s = REXCVAR_GET(bl_tex_fault);
  unsigned site = 0;
  unsigned long long n = 0;
  if (std::sscanf(s.c_str(), "S%u:%llu", &site, &n) == 2 && site >= 1 && site <= unsigned(kSites) && n > 0) {
    g_fault.site = int(site);
    g_fault.n = n;
    g_fault.from_boot = !s.empty() && s.back() == 'b';
    REXLOG_WARN("[TexGuard] fault injection armed: site S{} pass {} ({})", site, n,
                g_fault.from_boot ? "from process start" : "counted while the rollback engine tracks");
  }
}

const char* const kSiteName[kSites + 1] = {"?", "82105BE0 mesh", "82103EB8 mesh", "82105FD0 bind", "821061D8 bind",
                                          "821014A0 draw", "820F4160 group release", "820F3CB0 create"};

// True: the object the site is about to call through is dead (NULL, or its first word is not a vtable of the image -
// a freed 320-byte block has its free-list link there).
inline bool DeadObject(const uint8_t* base, uint32_t object) {
  if (object < 0x10000u) return true;
  return U32(base, object) - 0x82000000u >= 0x01000000u;
}

bool Guard(int site, PPCRegister& r3, uint32_t item) {
  uint8_t* base = Base();
  if (!base) return false;
  const uint64_t hit = g_hits[site].fetch_add(1, std::memory_order_relaxed) + 1;
  std::call_once(g_fault_once, [] { ParseFault(); ParseLag(); });
  if (g_lag.site == site && hit % g_lag.every == 0) {  // test: a late draw worker
    g_lag.slept.fetch_add(1, std::memory_order_relaxed);
    Sleep(g_lag.ms);
  }
  if (g_fault.site == site && (g_fault.from_boot || BurstLimitRb2Active())) {
    if (g_fault.count.fetch_add(1, std::memory_order_relaxed) + 1 == g_fault.n) {
      REXLOG_WARN("[TexGuard] FAULT INJECTED at S{} ({}) pass {}: object {:08X} -> 0", site, kSiteName[site], g_fault.n, r3.u32);
      rex::FlushLogging();
      r3.u64 = 0;
    }
  }
  if (!DeadObject(base, r3.u32)) return false;
  if (!REXCVAR_GET(bl_tex_guard)) return false;  // build 118: the read of guest 0 kills the process
  const uint64_t count = g_skips[site].fetch_add(1, std::memory_order_relaxed) + 1;
  g_skips_total.fetch_add(1, std::memory_order_relaxed);
  BurstLimitOnlineAlert("texture guard", fmt::format("a released texture was skipped at S{} (engine frame {})", site, BurstLimitRb2CurrentFrame()));
  if (count <= 8 || (count & (count - 1)) == 0) {
    const uint32_t object = r3.u32;
    const uint32_t w0 = item >= 0x10000u ? U32(base, item) : 0, w1 = item >= 0x10000u ? U32(base, item + 4) : 0,
                   w2 = item >= 0x10000u ? U32(base, item + 8) : 0;
    REXLOG_WARN("[TexGuard] texture skipped at S{} ({}): object {:08X} (first word {:08X}), item {:08X} = {:08X} {:08X} {:08X}, "
                "engine frame {}{} ({} at this site)",
                site, kSiteName[site], object, object >= 0x10000u ? U32(base, object) : 0, item, w0, w1, w2,
                BurstLimitRb2CurrentFrame(), BurstLimitRb2InResim() ? " resim" : "", count);
    static char buf[12000];
    static std::mutex mutex;
    std::lock_guard<std::mutex> lock(mutex);
    FormatRing(buf, int(sizeof(buf)), "group releases", g_ring_grp, g_ring_grp_n.load());
    REXLOG_WARN("[TexGuard] {}", buf);
    FormatRing(buf, int(sizeof(buf)), "resource commands", g_ring_cmd, g_ring_cmd_n.load());
    REXLOG_WARN("[TexGuard] {}", buf);
  }
  if (site == 7) r3.u64 = 0;  // sub_820F3CB0 returns "no object" (its caller checks)
  return true;
}

// ---- audit -----------------------------------------------------------------------------------------------------
struct AuditState {
  uint64_t runs = 0, failures = 0;
  bool have_base = false;
  int64_t free_delta = 0;      // capacity - count - free nodes at the first audit (the list's terminal node)
  bool key_field_ok = true;    // [object + 8] == key held for every node at the first audit
  uint32_t logged_kinds = 0;
  uint32_t last_count = 0, last_free = 0, last_group_entries = 0;
};
AuditState g_audit;

}  // namespace

// The seven guards. r3 = the object about to be dereferenced; the second register is the item that named it.
bool BurstLimitTexGuardS1(PPCRegister& r3, PPCRegister& r30) { return Guard(1, r3, r30.u32); }
bool BurstLimitTexGuardS2(PPCRegister& r3, PPCRegister& r30) { return Guard(2, r3, r30.u32); }
bool BurstLimitTexGuardS3(PPCRegister& r3, PPCRegister& r30) { return Guard(3, r3, r30.u32); }
bool BurstLimitTexGuardS4(PPCRegister& r3, PPCRegister& r30) { return Guard(4, r3, r30.u32); }
bool BurstLimitTexGuardS5(PPCRegister& r3, PPCRegister& r31) { return Guard(5, r3, r31.u32); }
bool BurstLimitTexGuardS6(PPCRegister& r3, PPCRegister& r31) { return Guard(6, r3, r31.u32); }
bool BurstLimitTexGuardS7(PPCRegister& r3) { return Guard(7, r3, 0); }

uint64_t BurstLimitTexGuardSkips() { return g_skips_total.load(std::memory_order_relaxed); }

// One line for the heartbeat / the match summary.
std::string BurstLimitTexStats() {
  std::string s = "texture sites hits/skips:";
  for (int i = 1; i <= kSites; ++i) s += fmt::format(" S{} {}/{}", i, g_hits[i].load(), g_skips[i].load());
  s += " | resource commands live/resim by type:";
  for (int t = 0; t < 32; ++t) {
    const uint64_t l = g_cmd_count[0][t].load(), r = g_cmd_count[1][t].load();
    if (l || r) s += fmt::format(" {}:{}/{}", t, l, r);
  }
  s += fmt::format(" | resim commands answered {} (+{} from another frame of the rewind), new in resims {}, overflow {}, ghosts by type:",
                   g_bnd_answered.load(), g_bnd_answered_other_frame.load(), g_bnd_new_in_resim.load(), g_bnd_overflow.load());
  for (int t = 0; t < 32; ++t)
    if (const uint64_t g = g_bnd_ghost[t].load()) s += fmt::format(" {}:{}", t, g);
  s += fmt::format(" | creates sent again 0:{} 4:{} | file reads started {}, the same path again within 600 frames {}",
                   g_bnd_recreate[0].load(), g_bnd_recreate[4].load(), g_file_starts.load(), g_file_restarts.load());
  s += fmt::format(" busy(-3) {}/{} full(-1) {}/{} | group releases {}/{} | audit runs {} failures {} (map {} nodes, {} free, {} group entries)",
                   g_cmd_busy[0].load(), g_cmd_busy[1].load(), g_cmd_full[0].load(), g_cmd_full[1].load(), g_grp_count[0].load(),
                   g_grp_count[1].load(), g_audit.runs, g_audit.failures, g_audit.last_count, g_audit.last_free,
                   g_audit.last_group_entries);
  return s;
}

// From TrkRewind (main thread): the frames [from, to) are about to be resimulated / have been resimulated.
void BurstLimitTexRewindBegin(int64_t from, int64_t to) {
  g_rewind_from = from;
  g_rewind_to = to;
}
void BurstLimitTexRewindEnd() {
  if (g_rewind_from >= 0 && REXCVAR_GET(rb2_res_boundary) > 0) {
    for (int64_t f = g_rewind_from; f < g_rewind_to; ++f) {  // sent before, not made again by the corrected frames
      FrameCmds& fc = g_fc[uint64_t(f) % kCmdFrames];
      if (fc.frame != f) continue;
      for (int i = 0; i < fc.n; ++i) {
        if (fc.rec[i].used_gen == g_rewind_gen) continue;
        // only the commands a resimulated frame can make are judged: the deletes of the countdown steps
        // (sub_8212A600 type 1, sub_82132550 type 6). Creates (0, 4) come from live-only code (the file step
        // sub_82119320, the text bitmap tasks), which no resim runs: never "made again" by design.
        if (fc.rec[i].a != 1 && fc.rec[i].a != 6) continue;
        const uint64_t n = g_bnd_ghost[fc.rec[i].a & 31].fetch_add(1) + 1;
        if (n <= 8 || (n & (n - 1)) == 0)
          REXLOG_WARN("[RB2] resource command GHOST: type {} args {:X} {:X} {:X} {:X} of frame {} was sent, the corrected frames "
                      "did not make it again ({} of this type)", fc.rec[i].a, fc.rec[i].b, fc.rec[i].c, fc.rec[i].d, fc.rec[i].e, f, n);
      }
    }
  }
  ++g_rewind_gen;
  g_rewind_from = g_rewind_to = -1;
}

// For the crash record: both rings, no allocation, no logger.
int BurstLimitTexRingsRaw(char* out, int cap) {
  int w = std::snprintf(out, size_t(cap), "texture guard skips %llu, audit runs %llu failures %llu\r\n",
                        (unsigned long long)g_skips_total.load(), (unsigned long long)g_audit.runs,
                        (unsigned long long)g_audit.failures);
  if (w < cap - 200) w += FormatRing(out + w, cap - w, "group releases", g_ring_grp, g_ring_grp_n.load());
  if (w < cap - 200) w += FormatRing(out + w, cap - w, "resource commands", g_ring_cmd, g_ring_cmd_n.load());
  return w;
}

// Main thread only (after a rewind, at an engine pause, every 600 frames). Takes the map's critical section for the
// walk (microseconds). Returns false when the registry is torn; logs "[RB2] TEXTURE REGISTRY TORN" once per kind.
bool BurstLimitTexAudit(PPCContext& ctx, uint8_t* base, const char* why) {
  if (REXCVAR_GET(rb2_tex_audit) <= 0 || !base) return true;
  const PPCContext saved = ctx;
  ctx.r3.u64 = kMapLock;
  __imp__RtlEnterCriticalSection(ctx, base);
  const uint32_t nodes = U32(base, kMap + 0), free_head = U32(base, kMap + 4), capacity = U32(base, kMap + 8),
                 count = U32(base, kMap + 12), nil = kMap + 16;
  uint32_t kinds = 0;        // bit per failure kind
  uint32_t detail_a = 0, detail_b = 0;
  uint32_t reachable = 0, free_len = 0, group_entries = 0, group_missing = 0, key_mismatch = 0;
  static uint8_t seen[0x1000];  // 1 reachable, 2 on the free list
  const bool sane = nodes >= 0x40000000u && capacity > 0 && capacity <= 0x1000u && count <= capacity;
  auto index_of = [&](uint32_t node) -> int {
    if (node < nodes || node >= nodes + capacity * 16u || ((node - nodes) & 15u)) return -1;
    return int((node - nodes) >> 4);
  };
  auto lookup = [&](uint32_t key) -> uint32_t {  // as sub_820F37F8 (without the nil-key trick), bounded
    uint32_t node = U32(base, kMap + 128 + 4 * (key % 101u));
    for (int guard = 0; guard < 4096 && node != nil; ++guard) {
      if (index_of(node) < 0) return 0;
      const uint32_t k = U32(base, node + 8);
      if (k == key) return U32(base, node + 12);
      node = U32(base, node + (key > k ? 0 : 4));
    }
    return 0;
  };
  if (!sane) {
    kinds |= 1;
    detail_a = nodes;
    detail_b = capacity;
  } else {
    std::memset(seen, 0, capacity);
    static uint32_t stack[0x1000];
    for (uint32_t b = 0; b < 101 && !(kinds & 2); ++b) {
      int sp = 0;
      uint32_t root = U32(base, kMap + 128 + 4 * b);
      if (root != nil) stack[sp++] = root;
      while (sp > 0) {
        const uint32_t node = stack[--sp];
        const int i = index_of(node);
        if (i < 0) { kinds |= 2; detail_a = node; detail_b = b; break; }          // a link out of the node array
        if (seen[i]) { kinds |= 4; detail_a = node; detail_b = b; continue; }      // reached twice (cycle / shared node)
        seen[i] = 1;
        ++reachable;
        const uint32_t key = U32(base, node + 8), value = U32(base, node + 12);
        if (key % 101u != b) { kinds |= 8; detail_a = node; detail_b = key; }      // a node in the wrong bucket
        if (value < 0x10000u || U32(base, value) - 0x82000000u >= 0x01000000u) { kinds |= 16; detail_a = key; detail_b = value; }
        else if (U32(base, value + 8) != key) ++key_mismatch;
        const uint32_t l = U32(base, node), r = U32(base, node + 4);
        if (l != nil && sp < 0xFFE) stack[sp++] = l;
        if (r != nil && sp < 0xFFE) stack[sp++] = r;
      }
    }
    if (reachable != count) { kinds |= 32; detail_a = reachable; detail_b = count; }
    uint32_t node = free_head;
    for (uint32_t guard = 0; node && guard <= capacity; ++guard) {
      const int i = index_of(node);
      if (i < 0) { kinds |= 64; detail_a = node; detail_b = free_len; break; }    // the free list leaves the array
      if (seen[i] == 1) { kinds |= 128; detail_a = node; detail_b = U32(base, node + 8); break; }  // a live node is free
      if (seen[i] == 2) { kinds |= 256; detail_a = node; break; }                 // a cycle in the free list
      seen[i] = 2;
      ++free_len;
      node = U32(base, node);
    }
  }
  ctx.r3.u64 = kMapLock;
  __imp__RtlLeaveCriticalSection(ctx, base);
  if (sane) {  // the group lists: every entry flagged "has an object" must be in the map
    ctx.r3.u64 = kGroups + 28;
    __imp__RtlEnterCriticalSection(ctx, base);
    for (uint32_t g = 0; g < 256; ++g) {
      const uint32_t n = U32(base, kGroups + 64 + 8 * g);
      uint32_t node = U32(base, kGroups + 60 + 8 * g);
      if (n == 0 || n > 0x4000u) continue;
      for (uint32_t i = 0; i < n && node >= 0x40000000u; ++i) {
        ++group_entries;
        if ((U32(base, node + 16) & 0x00800000u) && !lookup(U32(base, node + 12))) {
          ++group_missing;
          kinds |= 512;
          detail_a = g;
          detail_b = U32(base, node + 12);
        }
        node = U32(base, node + 4);
      }
    }
    ctx.r3.u64 = kGroups + 28;
    __imp__RtlLeaveCriticalSection(ctx, base);
  }
  ctx = saved;
  AuditState& a = g_audit;
  ++a.runs;
  a.last_count = count;
  a.last_free = free_len;
  a.last_group_entries = group_entries;
  if (sane && !a.have_base && !kinds) {
    a.have_base = true;
    a.free_delta = int64_t(capacity) - int64_t(count) - int64_t(free_len);
    a.key_field_ok = key_mismatch == 0;
    REXLOG_WARN("[RB2] texture registry audit baseline ({}): map {} nodes of {}, free list {} (delta {}), group entries {}, "
                "[object+8] == key for {}",
                why, count, capacity, free_len, a.free_delta, group_entries, a.key_field_ok ? "all" : "NOT all (check off)");
  } else if (sane && a.have_base) {
    if (int64_t(capacity) - int64_t(count) - int64_t(free_len) != a.free_delta) { kinds |= 1024; detail_a = free_len; detail_b = count; }
    if (a.key_field_ok && key_mismatch) { kinds |= 2048; detail_a = key_mismatch; }
  }
  if (!kinds) return true;
  ++a.failures;
  const uint32_t fresh = kinds & ~a.logged_kinds;
  if (fresh || a.failures <= 4 || (a.failures & (a.failures - 1)) == 0) {
    a.logged_kinds |= kinds;
    REXLOG_WARN("[RB2] TEXTURE REGISTRY TORN ({}, engine frame {}): kinds {:#x} (1 header, 2 link out of array, 4 node reached twice, "
                "8 wrong bucket, 16 dead object, 32 count, 64 free list out of array, 128 live node on free list, 256 free cycle, "
                "512 group entry not in map, 1024 free count, 2048 key field) detail {:08X} {:08X} | reachable {} count {} free {} "
                "capacity {} group entries {} missing {} key mismatches {} (failure {})",
                why, BurstLimitRb2CurrentFrame(), kinds, detail_a, detail_b, reachable, count, free_len, capacity, group_entries,
                group_missing, key_mismatch, a.failures);
    static char buf[12000];
    FormatRing(buf, int(sizeof(buf)), "group releases", g_ring_grp, g_ring_grp_n.load());
    REXLOG_WARN("[RB2] {}", buf);
    FormatRing(buf, int(sizeof(buf)), "resource commands", g_ring_cmd, g_ring_cmd_n.load());
    REXLOG_WARN("[RB2] {}", buf);
    rex::FlushLogging();
  }
  return false;
}

// Resource command sub_8210D238(type, a, b, c, d): queued for the render loop (-3: the buffer is being read, -1: full).
extern "C" REX_FUNC(sub_8210D238) {
  Obit o{};
  o.kind = 'C';
  o.resim = BurstLimitRb2InResim() ? 1 : 0;
  o.thread = uint16_t(GetCurrentThreadId());
  o.a = ctx.r3.u32;
  o.b = ctx.r4.u32;
  o.c = ctx.r5.u32;
  o.d = ctx.r6.u32;
  o.e = ctx.r7.u32;
  o.lr = uint32_t(ctx.lr);
  o.frame = BurstLimitRb2CurrentFrame();
  // R13a: on the main thread, while the engine tracks
  const bool boundary = rb2_main && REXCVAR_GET(rb2_res_boundary) > 0 && (o.resim || BurstLimitRb2Active());
  if (boundary && !o.resim && (o.a == 0 || o.a == 4)) {  // R30 counter (no behaviour)
    BurstLimitRb2FileEvent();
    for (int64_t f = o.frame; f > o.frame - kCmdFrames && f >= 0; --f) {
      const FrameCmds& fc = g_fc[uint64_t(f) % kCmdFrames];
      if (fc.frame != f) continue;
      bool hit = false;
      for (int i = 0; i < fc.n && !hit; ++i)
        hit = fc.rec[i].a == o.a && fc.rec[i].b == o.b && fc.rec[i].c == o.c && fc.rec[i].d == o.d && fc.rec[i].e == o.e;
      if (!hit) continue;
      const uint64_t n = g_bnd_recreate[o.a & 31].fetch_add(1) + 1;
      if (n <= 8 || (n & (n - 1)) == 0)
        REXLOG_WARN("[RB2] resource create SENT AGAIN: type {} args {:X} {:X} {:X} {:X} at frame {} (the same create went out at "
                    "frame {}; {} of this type)", o.a, o.b, o.c, o.d, o.e, o.frame, f, n);
      break;
    }
  }
  if (boundary && o.resim && g_rewind_from >= 0) {
    CmdRec* r = Match(FrameOf(o.frame, false), o.frame, o);
    bool other = false;
    for (int64_t f = g_rewind_from; !r && f < g_rewind_to; ++f) {  // the same command, a frame earlier / later
      if (f == o.frame) continue;
      r = Match(g_fc[uint64_t(f) % kCmdFrames], f, o);
      other = r != nullptr;
    }
    if (r) {  // already sent when these frames ran before: answered, not sent twice
      r->used_gen = g_rewind_gen;
      ctx.r3.s64 = r->result;
      (other ? g_bnd_answered_other_frame : g_bnd_answered).fetch_add(1, std::memory_order_relaxed);
      return;
    }
    g_bnd_new_in_resim.fetch_add(1, std::memory_order_relaxed);
  }
  __imp__sub_8210D238(ctx, base);
  o.result = ctx.r3.s32;
  if (boundary) {
    FrameCmds& f = FrameOf(o.frame, true);
    if (f.n < kCmdPerFrame) {
      f.rec[f.n++] = CmdRec{o.a, o.b, o.c, o.d, o.e, o.result, o.resim ? g_rewind_gen : 0};
    } else {
      g_bnd_overflow.fetch_add(1, std::memory_order_relaxed);
    }
  }
  g_cmd_count[o.resim][o.a & 31].fetch_add(1, std::memory_order_relaxed);
  if (o.result == -3) g_cmd_busy[o.resim].fetch_add(1, std::memory_order_relaxed);
  if (o.result == -1) g_cmd_full[o.resim].fetch_add(1, std::memory_order_relaxed);
  Push(g_ring_cmd, g_ring_cmd_n, o);
}

// R30 diagnostics: sub_82118820(request, path) starts the read of a file the request queue asked for (called by the
// file step's "next request" sub_82118B90). The same path started again shortly after = the request was made twice
// (a resimulated frame asked again for what the rewound frame had asked for).
REX_EXTERN(__imp__sub_82118820);
extern "C" REX_FUNC(sub_82118820) {
  if (rb2_main && BurstLimitRb2Active() && ctx.r4.u32 >= 0x10000u) {
    struct Started { char path[64]; int64_t frame; };
    static Started last[32];
    static uint32_t next = 0;
    Started now{};
    const char* p = reinterpret_cast<const char*>(base + ctx.r4.u32);
    for (int i = 0; i < 63 && p[i]; ++i) now.path[i] = p[i];
    now.frame = BurstLimitRb2CurrentFrame();
    g_file_starts.fetch_add(1, std::memory_order_relaxed);
    for (const Started& l : last) {
      if (l.path[0] && now.frame - l.frame < 600 && now.frame > l.frame && std::strcmp(l.path, now.path) == 0) {
        const uint64_t n = g_file_restarts.fetch_add(1) + 1;
        if (n <= 8 || (n & (n - 1)) == 0)
          REXLOG_WARN("[RB2] file read STARTED AGAIN: {} at frame {} (before: frame {}; request {:08X}; {} in all)", now.path, now.frame,
                      l.frame, ctx.r3.u32, n);
        break;
      }
    }
    last[next++ % 32] = now;
    BurstLimitRb2FileEvent();
  }
  __imp__sub_82118820(ctx, base);
}

// Group release sub_820E57A0(group): every handle of the group's list is released (sub_820F4160 = guard site S6).
extern "C" REX_FUNC(sub_820E57A0) {
  Obit o{};
  o.kind = 'G';
  o.resim = BurstLimitRb2InResim() ? 1 : 0;
  o.thread = uint16_t(GetCurrentThreadId());
  o.a = ctx.r3.u32;
  o.b = ctx.r3.u32 < 256 ? U32(base, kGroups + 64 + 8 * ctx.r3.u32) : 0;  // entries at the call
  o.c = U32(base, kMap + 12);
  o.lr = uint32_t(ctx.lr);
  o.frame = BurstLimitRb2CurrentFrame();
  __imp__sub_820E57A0(ctx, base);
  o.d = U32(base, kMap + 12);  // map count after
  o.result = int32_t(o.d) - int32_t(o.c);
  g_grp_count[o.resim].fetch_add(1, std::memory_order_relaxed);
  Push(g_ring_grp, g_ring_grp_n, o);
}

// Deferred flush sub_820E5830: only the calls that changed the map are recorded.
extern "C" REX_FUNC(sub_820E5830) {
  const uint32_t before = U32(base, kMap + 12);
  const uint32_t lr = uint32_t(ctx.lr);
  __imp__sub_820E5830(ctx, base);
  const uint32_t after = U32(base, kMap + 12);
  if (after == before) return;
  Obit o{};
  o.kind = 'F';
  o.resim = BurstLimitRb2InResim() ? 1 : 0;
  o.thread = uint16_t(GetCurrentThreadId());
  o.c = before;
  o.d = after;
  o.lr = lr;
  o.frame = BurstLimitRb2CurrentFrame();
  o.result = int32_t(after) - int32_t(before);
  Push(g_ring_grp, g_ring_grp_n, o);
}
