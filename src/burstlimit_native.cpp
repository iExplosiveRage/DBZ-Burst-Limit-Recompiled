// Native renderer (gpu_native_renderer = true, latched at startup): hooks on
// the game's statically linked Xbox D3D library (XDK 2.0.6534, D3D9LTCG).
// C:\rex\_native_renderer\design.md has the plan; functions.md the addresses.
//
// Milestone M1, the substrate: the game's D3D calls render at API time on the
// calling thread, and the command processor thread leaves the frame.
//   - S tier (burstlimit_native_scope.cpp): when a D3D call returns to the
//     game, and before every kick, what the library wrote into its command
//     buffer runs right away (INativeGpu::ExecuteScope); sync packets stay.
//   - R tier: the ring then only holds fences, callbacks, waits and the swap.
//     The thread that kicks (820ADFB0) drains it at once, and threads waiting
//     for the GPU (820B10E8) drain it and sleep until a vblank or a kick
//     instead of polling. XE_SWAP (the present) runs on the render thread,
//     which waits for its frame's fence right behind it (BL calls Swap on its
//     game thread, which never waits for the GPU).
//   - The frame-start kick: the game thread queues the GPU-side vsync (null
//     flip 820B5F90) without a kick; the render thread kicks once at its
//     first D3D call after its frame-start callback (820E8128), so the flip's
//     vblank is picked as early as the command processor would have.
//
//   820A8A20 DrawVertices(dev, prim, start, count)
//   820A8E10 DrawIndexedVertices(dev, prim, base_index, start, count)
//   820A89D8 DrawVerticesUP(dev, prim, count, data, stride) (-> 820A8538)
//   820BF910 ClearF(dev, flags, rect, color, z f1, stencil)
//   820BB348 Resolve
//   820B64C0 Swap
//   820BA5A8 BeginTiling / 820BAA48 EndTiling (logged; 0 per frame in BL)
//   820AE690 KickOff(dev) -> write pointer; 820ADC38 new segment(dev, dwords)
//   820ADFB0 segment kick (INDIRECT_BUFFER into the primary ring + CP_RB_WPTR)
//   820AE868 InsertCallback(dev, ptr, flags, fn, ctx) -> ptr
//   820EAA20 the game's vblank callback; 820B5DF8 the swap callback (picks the
//            vblank of the null flip)
//   820E8128 / 820E8158 the GPU frame timer's callbacks (native: timed at
//            their InsertCallback, see kGpuTimer)
//
// With gpu_native_renderer off every hook is one branch to the original.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <mutex>
#include <string>
#include <string_view>

#include <fmt/format.h>

#include <rex/chrono/clock.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/memory.h>
#include <rex/memory/utils.h>
#include <rex/perf/frame_rate.h>
#include <rex/ppc.h>
#include <rex/ppc/context.h>
#include <rex/runtime.h>
#include <rex/system/interfaces/graphics.h>
#include <rex/system/interfaces/native_gpu.h>
#include <rex/ui/debug_command_pipe.h>

#include <windows.h>

REXCVAR_DEFINE_BOOL(native_pacing_stats, false, "Debug",
                    "Testing: count the game's vblank callbacks, the null flip's target vblank "
                    "and the present intervals for native_stats (both renderers)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_INT32(native_trace_frames, 0, "Debug",
                     "Testing: log every scope, kick and drain of the native renderer for this "
                     "many frames (counts down at each Swap)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

void BurstLimitNativeScope(rex::system::INativeGpu* native, uint8_t* base);
void BurstLimitNativeScopeRestart(uint8_t* base);
void BurstLimitNativeScopeSetLabel(const char* label);
void BurstLimitNativeScopeCounters(uint64_t out[8], bool reset);
bool BurstLimitNativeDrawBegin(uint8_t* base, uint32_t device);
void BurstLimitNativeDrawEnd();
void BurstLimitNativeDeviceFrame();
std::string BurstLimitNativeDeviceStats(bool reset);
bool BurstLimitNativeOpsEnabled();
bool BurstLimitNativeOpScope(rex::system::INativeGpu* native, uint8_t* base);
std::string BurstLimitNativeOpStats(bool reset);

namespace {

constexpr uint32_t kDevicePointer = 0x8437EE04;
constexpr uint32_t kFrameStartCallback = 0x820E8128;
constexpr uint32_t kFrameEndCallback = 0x820E8158;

// The game's GPU frame timer, 0x8437F570 (the renderer object 0x8437EE00 +
// 1904): 820E8128 and 820E8158, run by the GPU as InsertCallback callbacks
// at the start and the end of a frame's commands, store the start time
// (+24) and the frame's GPU time ((end - start) / [+0]) in one of two slots
// (+8 / +16, the other one than [+32]; +36 = off). Every frame sub_82119D18
// reads it (820E4F90) into a 32-frame history at 0x840D625C (a bit per
// frame over the budget), which makes sub_82126880 / sub_82126E80 pick
// cheaper techniques, and in mode 2 it throttles the CPU.
constexpr uint32_t kGpuTimer = 0x8437F570;
constexpr uint32_t kGpuTimeHistory = 0x840D625C;

bool NativeRendererRequested() {
  static const bool requested = rex::cvar::GetFlagByName("gpu_native_renderer") == "true";
  return requested;
}

// The native renderer once the backend is set up, or null (the emulated path).
// It follows what the SDK latched when its command processor started, not the
// cvar now: the settings menu saves the cvar for the next start at any time,
// also between that latch and the first D3D call, and hooks that disagreed
// with the SDK would leave the ring to the watchdog.
rex::system::INativeGpu* NativeGpu() {
  static rex::system::INativeGpu* const native = []() -> rex::system::INativeGpu* {
    auto* runtime = rex::Runtime::instance();
    auto* graphics = runtime ? runtime->graphics_system() : nullptr;
    rex::system::INativeGpu* gpu = graphics ? graphics->native_gpu() : nullptr;
    if (!gpu && !NativeRendererRequested()) {
      return nullptr;
    }
    if (!gpu || !gpu->WaitUntilReady()) {
      REXLOG_WARN("Native renderer: not available, the game renders through the emulated path");
      return nullptr;
    }
    REXLOG_INFO("Native renderer: on (D3D calls render at API time, guest threads drain the ring)");
    return gpu;
  }();
  return native;
}

// Nesting of the D3D calls that hand their commands over when they return.
thread_local int t_api_depth = 0;
// Inside KickOff / a new segment: no scopes, no frame-start kick.
thread_local bool t_in_kick = false;
// The render thread inserted its frame-start callback: kick at its next call.
thread_local bool t_frame_kick_armed = false;

struct NativeCounters {
  std::atomic<uint64_t> swaps{0};
  std::atomic<uint64_t> draws{0};
  std::atomic<uint64_t> indexed_draws{0};
  std::atomic<uint64_t> up_draws{0};
  std::atomic<uint64_t> up_draws_native{0};  // DrawVerticesUP taken as typed commands
  std::atomic<uint64_t> clears{0};
  std::atomic<uint64_t> resolves{0};
  std::atomic<uint64_t> begin_tilings{0};
  std::atomic<uint64_t> kicks{0};
  std::atomic<uint64_t> frame_kicks{0};
  std::atomic<uint64_t> segment_kicks{0};
  std::atomic<uint64_t> waits{0};
  std::atomic<uint64_t> vblank_drains{0};
  // The game thread's per-frame Sleep(1) skipped (unlocked, offline).
  std::atomic<uint64_t> frame_sleep_skips{0};
  // Frame handshake waits that returned on their timeout (sub_822A2E50 hook).
  std::atomic<uint64_t> handshake_timeouts{0};
  // native_swap_order: rendering calls that waited for the previous frame's
  // XE_SWAP, the time they waited, and waits that gave up (timeout).
  std::atomic<uint64_t> swap_order_waits{0};
  std::atomic<uint64_t> swap_order_wait_ns{0};
  std::atomic<uint64_t> swap_order_timeouts{0};
};
NativeCounters g_native;

// Pacing (native_pacing_stats, both renderers).
struct PacingCounters {
  std::mutex mutex;
  uint64_t vblank_callbacks = 0;
  int64_t first_ns = 0;
  int64_t last_ns = 0;
  // 820B5DF8: target vblank - current vblank count, 0..3 (3 = 3 or more).
  uint64_t target_offsets[4] = {};
  // The GPU frame timer as the game reads it, sampled once per frame (at
  // 820B5DF8), and the frames its over-budget history wasn't empty.
  uint64_t gpu_time_frames = 0;
  uint64_t gpu_time_sum = 0;
  uint64_t gpu_time_max = 0;
  uint64_t gpu_time_over_budget_frames = 0;
};
PacingCounters g_pacing;

// Native: the GPU timer callbacks run back to back after the vsync wait (the
// frame rendered at API time before it), so the timer would read about 0.
// They get the times their InsertCallback was made at on the render thread
// instead: the frame's commands run between those, like the command
// processor's span between the two callbacks on the emulated path. Ring
// order is insertion order, so each kind is a queue.
struct GpuTimerTimes {
  std::mutex mutex;
  uint64_t start[8] = {};
  uint64_t end[8] = {};
  uint32_t start_read = 0, start_write = 0;
  uint32_t end_read = 0, end_write = 0;
};
GpuTimerTimes g_gpu_timer_times;

void PushGpuTimerTime(bool start) {
  const uint64_t now = rex::chrono::Clock::QueryGuestTickCount();
  GpuTimerTimes& t = g_gpu_timer_times;
  std::lock_guard<std::mutex> lock(t.mutex);
  uint64_t* times = start ? t.start : t.end;
  uint32_t& read = start ? t.start_read : t.end_read;
  uint32_t& write = start ? t.start_write : t.end_write;
  if (write - read >= std::size(t.start)) {
    ++read;  // never more than a few frames queued; drop the oldest
  }
  times[write++ % std::size(t.start)] = now;
}

// The time recorded for the callback that runs now (now if none is queued:
// one inserted before the native renderer was up).
uint64_t PopGpuTimerTime(bool start) {
  GpuTimerTimes& t = g_gpu_timer_times;
  std::lock_guard<std::mutex> lock(t.mutex);
  uint64_t* times = start ? t.start : t.end;
  uint32_t& read = start ? t.start_read : t.end_read;
  const uint32_t write = start ? t.start_write : t.end_write;
  if (read == write) {
    return rex::chrono::Clock::QueryGuestTickCount();
  }
  return times[read++ % std::size(t.start)];
}

int64_t NowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

std::string NativeStatsText(bool reset) {
  std::string text;
  if (rex::system::INativeGpu* native = NativeGpu()) {
    uint64_t scope[8];
    BurstLimitNativeScopeCounters(scope, reset);
    rex::system::NativeGpuStats stats;
    native->GetStats(&stats, reset);
    // p99 of the drain time without XE_SWAP and interrupt callbacks.
    uint64_t total = 0;
    for (uint32_t count : stats.drain_histogram) {
      total += count;
    }
    uint64_t seen = 0;
    uint32_t p99_bucket = 0;
    for (uint32_t i = 0; i < rex::system::NativeGpuStats::kDrainBuckets; ++i) {
      seen += stats.drain_histogram[i];
      if (seen * 100 >= total * 99) {
        p99_bucket = i;
        break;
      }
    }
    text += fmt::format(
        "swaps {} | scopes {} packets {} dwords {} left {} failures {} resyncs {} islands {} | "
        "kicks {} frame_kicks {} segment_kicks {} waits {} vblank_drains {} | drains {} idle {} "
        "wait {} owner {} "
        "busy {} packets {} render_in_ring {} (first {:08X} at {:08X} next {:08X} depth {}) | "
        "drain p99 < {} us max {} us mean {:.1f} us | xe_swap owner {} foreign {} worker {} | "
        "watchdog_drains {} | interrupts {} worker {} badf00d {} restore_mismatches {} | "
        "scope cpu {:.2f} ms | register writes {} dwords {} out_of_order {} | {}",
        g_native.swaps.load(), scope[0], scope[1], scope[2], scope[3], scope[4], scope[5],
        scope[6], g_native.kicks.load(), g_native.frame_kicks.load(),
        g_native.segment_kicks.load(), g_native.waits.load(), g_native.vblank_drains.load(),
        stats.drains, stats.drains_idle,
        stats.drains_blocked_wait, stats.drains_blocked_owner, stats.drains_busy,
        stats.drain_packets, stats.drain_render_packets, stats.first_render_packet[0],
        stats.first_render_packet[1], stats.first_render_packet[2],
        stats.first_render_packet[3],
        (p99_bucket + 1) * rex::system::NativeGpuStats::kDrainBucketNs / 1000,
        stats.drain_max_ns / 1000,
        stats.drains ? double(stats.drain_ns) / double(stats.drains) / 1000.0 : 0.0,
        stats.swaps_owner, stats.swaps_foreign, stats.swaps_worker, stats.watchdog_drains,
        stats.interrupts, stats.interrupts_worker, stats.interrupt_badf00d_skips,
        stats.interrupt_restore_mismatches, double(stats.scope_ns) / 1.0e6,
        stats.register_writes, stats.register_dwords, scope[7],
        BurstLimitNativeDeviceStats(reset));
    text += " | " + BurstLimitNativeOpStats(reset);
    text += fmt::format(" | UP draws {} native {}", g_native.up_draws.load(),
                        g_native.up_draws_native.load());
    {
      const rex::perf::PipelineCacheStats& pc = rex::perf::GetPipelineCacheStats();
      const uint64_t st = pc.shader_translations.load(), sw = pc.storage_shader_translations.load();
      const uint64_t pl = pc.pipelines_created.load(), pw = pc.storage_pipelines_created.load();
      text += fmt::format(
          " | shader translations {} (warm-up {} first use {}) pipelines {} (warm-up {} first use {})",
          st, sw, st - sw, pl, pw, pl - pw);
      text += fmt::format(" | aot pipelines {} (warm-up {}) not eligible {} draws aot {} runtime {}",
                          pc.aot_pipelines.load(), pc.storage_aot_pipelines.load(), pc.aot_ineligible.load(), pc.aot_draws.load(),
                          pc.runtime_draws.load());
      const rex::perf::RenderTargetPoolStats& rp = rex::perf::GetRenderTargetPoolStats();
      text += fmt::format(
          " | rt pool updates {} edram map {} ownership changes {} resolve pool hits {} misses {}",
          rp.pool_updates.load(), rp.edram_updates.load(), rp.ownership_changes.load(),
          rp.resolve_pool_hits.load(), rp.resolve_pool_misses.load());
    }
    text += fmt::format(" | commands lists {} draws {} shaders {} register dwords {}",
                        stats.command_lists, stats.command_draws, stats.command_shaders,
                        stats.command_register_dwords);
    text += fmt::format(" | frame_sleep_skips {} handshake_timeouts {}",
                        g_native.frame_sleep_skips.load(), g_native.handshake_timeouts.load());
    const uint64_t order_waits = g_native.swap_order_waits.load();
    text += fmt::format(" | swap_order waits {} mean {:.3f} ms timeouts {}", order_waits,
                        order_waits ? double(g_native.swap_order_wait_ns.load()) / 1.0e6 /
                                          double(order_waits)
                                    : 0.0,
                        g_native.swap_order_timeouts.load());
  } else {
    text += "emulated";
  }
  if (REXCVAR_GET(native_pacing_stats)) {
    std::lock_guard<std::mutex> lock(g_pacing.mutex);
    const double seconds = double(g_pacing.last_ns - g_pacing.first_ns) / 1.0e9;
    text += fmt::format(" | vblank callbacks {} rate {:.3f}/s | target vblank +0 {} +1 {} +2 {} +3.. {}",
                        g_pacing.vblank_callbacks,
                        seconds > 0.0 && g_pacing.vblank_callbacks > 1
                            ? double(g_pacing.vblank_callbacks - 1) / seconds
                            : 0.0,
                        g_pacing.target_offsets[0], g_pacing.target_offsets[1],
                        g_pacing.target_offsets[2], g_pacing.target_offsets[3]);
    float intervals[512];
    const size_t count = rex::perf::GetHostPresentIntervals(intervals, 512);
    if (count > 1) {
      double sum = 0.0, sum_sq = 0.0;
      for (size_t i = 0; i < count; ++i) {
        sum += intervals[i];
        sum_sq += double(intervals[i]) * intervals[i];
      }
      const double mean = sum / double(count);
      const double variance = std::max(0.0, sum_sq / double(count) - mean * mean);
      text += fmt::format(" | present intervals {} mean {:.3f} ms sd {:.3f} ms", count, mean,
                          std::sqrt(variance));
    }
    text += fmt::format(" | gpu timer frames {} mean {:.1f} max {} over-budget history frames {}",
                        g_pacing.gpu_time_frames,
                        g_pacing.gpu_time_frames ? double(g_pacing.gpu_time_sum) /
                                                       double(g_pacing.gpu_time_frames)
                                                 : 0.0,
                        g_pacing.gpu_time_max, g_pacing.gpu_time_over_budget_frames);
    if (reset) {
      g_pacing.vblank_callbacks = 0;
      g_pacing.first_ns = g_pacing.last_ns = 0;
      std::fill(std::begin(g_pacing.target_offsets), std::end(g_pacing.target_offsets), 0);
      g_pacing.gpu_time_frames = g_pacing.gpu_time_sum = g_pacing.gpu_time_max = 0;
      g_pacing.gpu_time_over_budget_frames = 0;
    }
  }
  return text;
}

// One line every 5 seconds while native.
void ReportNative() {
  static int64_t last_ns = NowNs();
  const int64_t now_ns = NowNs();
  if (now_ns - last_ns < 5'000'000'000) {
    return;
  }
  last_ns = now_ns;
  REXLOG_INFO("Native renderer: {}", NativeStatsText(false));
}

void NativeStats(std::string_view args) {
  const bool reset = args == "reset";
  std::string text = NativeStatsText(reset);
  REXLOG_INFO("native_stats: {}", text);
  rex::ui::DeferCommandCompletion()(true, text);
}

// Calls KickOff (through its hook) from inside another hook, keeping the
// registers of the call that is in progress.
void KickFromHook(PPCContext& ctx, uint8_t* base);

}  // namespace

REXCVAR_DEFINE_COMMAND_ARGS(native_stats, NativeStats, "Debug",
                            "Testing: native renderer counters (and pacing with "
                            "native_pacing_stats); 'native_stats reset' also clears them");

extern "C" REX_FUNC(sub_820AE690);

namespace {

void KickFromHook(PPCContext& ctx, uint8_t* base) {
  const uint32_t device = rex::memory::load_and_swap<uint32_t>(base + kDevicePointer);
  if (!device) {
    return;
  }
  const PPCContext saved = ctx;
  ctx.r3.u64 = device;
  sub_820AE690(ctx, base);
  ctx = saved;
}

// --- Frame order: no rendering of frame N+1 before frame N's swap ---
// The swap (XE_SWAP, the present) stays in the ring behind the null flip's
// vblank wait, while every draw runs at API time. Without a wait the next
// frame's draws (and its DLSS / FSR pass) ran before the previous frame's
// present whenever that vblank had not come yet: 50-560 draws between two
// presents instead of ~300, presents in pairs (1 ms, then 15 ms) and a ragged
// frame time as soon as the GPU is the limit. The emulated command processor
// can't reorder like that (it stops at the wait). So a rendering call that
// finds the previous swap still queued first lets it run - the next vblank
// (1 ms at "unlocked") - the way the GPU would: it drains the ring and sleeps
// on its progress (a vblank, another thread's drain), no spinning. Bounded, so
// a missed count can't stall a frame. Off = the round 2 behaviour (A/B).
// Only while the GPU is the limit (it hasn't finished the last closed frame):
// when the CPU is the limit the reordering costs nothing, and waiting for the
// 1 kHz "unlocked" vblank would cost up to 1 ms of every frame (1440p unlocked:
// 596 -> 454 FPS measured with the wait always on).
REXCVAR_DEFINE_BOOL(native_swap_order, true, "GPU",
                    "Native renderer: while the GPU is the limit, rendering of the next frame "
                    "waits for the previous frame's present, like the emulated path (steady "
                    "frame times with DLSS / FSR / high resolutions; live)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(native_swap_order_always, false, "Debug",
                    "Testing: native_swap_order waits even when the GPU is not the limit")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(native_swap_order_timeout_us, 20000, "Debug",
                     "Testing: the longest a rendering call waits for the previous swap")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

void WaitForQueuedSwap(rex::system::INativeGpu* native) {
  if (!REXCVAR_GET(native_swap_order) || !native->SwapPending() ||
      (!REXCVAR_GET(native_swap_order_always) && !native->GpuBehindLastFrame())) {
    return;
  }
  const auto start = std::chrono::steady_clock::now();
  const auto deadline =
      start + std::chrono::microseconds(std::max(1000, REXCVAR_GET(native_swap_order_timeout_us)));
  for (;;) {
    native->TryDrain();
    if (!native->SwapPending()) {
      break;
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      native->ForgetQueuedSwaps();
      g_native.swap_order_timeouts.fetch_add(1, std::memory_order_relaxed);
      break;
    }
    native->WaitForRingProgress(1000);
  }
  g_native.swap_order_waits.fetch_add(1, std::memory_order_relaxed);
  g_native.swap_order_wait_ns.fetch_add(
      uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                   std::chrono::steady_clock::now() - start)
                   .count()),
      std::memory_order_relaxed);
}

// A D3D call that renders: its commands (and anything written before it)
// run when the outermost one returns.
template <void (*Original)(PPCContext&, uint8_t*)>
void NativeApiCall(PPCContext& ctx, uint8_t* base, rex::system::INativeGpu* native) {
  if (t_api_depth == 0 && !t_in_kick) {
    WaitForQueuedSwap(native);
  }
  if (t_api_depth == 0 && !t_in_kick && t_frame_kick_armed) {
    t_frame_kick_armed = false;
    g_native.frame_kicks.fetch_add(1, std::memory_order_relaxed);
    KickFromHook(ctx, base);
  }
  ++t_api_depth;
  Original(ctx, base);
  if (--t_api_depth == 0 && !t_in_kick) {
    BurstLimitNativeScope(native, base);
    // A vblank may have let the ring go on (the null flip's wait, then the
    // previous frame's present): run it now rather than at the next kick.
    if (native->VblankSinceLastDrain()) {
      g_native.vblank_drains.fetch_add(1, std::memory_order_relaxed);
      native->TryDrain();
    }
  }
}

// ClearF / Resolve (and Resolve's 820F8E08 wrapper, which calls it): the N
// tier as typed commands (burstlimit_native_scope.cpp, BurstLimitNativeOpScope).
// What was written before the call runs first; the call's pre-flush registers
// go natively like a draw's; then the call's own commands are handed over as
// register writes, shader loads and a direct draw / copy - no packets. Calls
// that aren't liftable run the S tier.
// The last NativeOpCall on this thread ran as typed commands (not the S tier).
thread_local bool t_last_op_native = false;

template <void (*Original)(PPCContext&, uint8_t*)>
void NativeOpCall(PPCContext& ctx, uint8_t* base, rex::system::INativeGpu* native) {
  if (t_api_depth != 0 || t_in_kick || !BurstLimitNativeOpsEnabled()) {
    NativeApiCall<Original>(ctx, base, native);
    return;
  }
  WaitForQueuedSwap(native);
  if (t_frame_kick_armed) {
    t_frame_kick_armed = false;
    g_native.frame_kicks.fetch_add(1, std::memory_order_relaxed);
    KickFromHook(ctx, base);
  }
  BurstLimitNativeScope(native, base);
  const bool native_feed = BurstLimitNativeDrawBegin(base, ctx.r3.u32);
  ++t_api_depth;
  Original(ctx, base);
  --t_api_depth;
  if (native_feed) {
    BurstLimitNativeDrawEnd();
  }
  t_last_op_native = BurstLimitNativeOpScope(native, base);
  if (!t_last_op_native) {
    BurstLimitNativeScope(native, base);
  }
  if (native->VblankSinceLastDrain()) {
    g_native.vblank_drains.fetch_add(1, std::memory_order_relaxed);
    native->TryDrain();
  }
}

}  // namespace

// DrawVertices / DrawIndexedVertices: the N tier (burstlimit_native_device.cpp)
// unless the device is predicating, tiling or in a viz query; r3 = the device.
REX_EXTERN(__imp__sub_820A8A20);
extern "C" REX_FUNC(sub_820A8A20) {
  if (rex::system::INativeGpu* native = NativeGpu()) {
    g_native.draws.fetch_add(1, std::memory_order_relaxed);
    const bool native_draw = BurstLimitNativeDrawBegin(base, ctx.r3.u32);
    NativeApiCall<__imp__sub_820A8A20>(ctx, base, native);
    if (native_draw) {
      BurstLimitNativeDrawEnd();
    }
    return;
  }
  __imp__sub_820A8A20(ctx, base);
}

REX_EXTERN(__imp__sub_820A8E10);
extern "C" REX_FUNC(sub_820A8E10) {
  if (rex::system::INativeGpu* native = NativeGpu()) {
    g_native.indexed_draws.fetch_add(1, std::memory_order_relaxed);
    const bool native_draw = BurstLimitNativeDrawBegin(base, ctx.r3.u32);
    NativeApiCall<__imp__sub_820A8E10>(ctx, base, native);
    if (native_draw) {
      BurstLimitNativeDrawEnd();
    }
    return;
  }
  __imp__sub_820A8E10(ctx, base);
}

REX_EXTERN(__imp__sub_820A89D8);
extern "C" REX_FUNC(sub_820A89D8) {
  if (rex::system::INativeGpu* native = NativeGpu()) {
    g_native.up_draws.fetch_add(1, std::memory_order_relaxed);
    // Its vertices sit in the command buffer (a packet the lift leaves
    // alone); anything not liftable runs the S tier.
    t_last_op_native = false;
    BurstLimitNativeScopeSetLabel("DrawVerticesUP");
    NativeOpCall<__imp__sub_820A89D8>(ctx, base, native);
    BurstLimitNativeScopeSetLabel(nullptr);
    if (t_last_op_native) {
      g_native.up_draws_native.fetch_add(1, std::memory_order_relaxed);
    }
    return;
  }
  __imp__sub_820A89D8(ctx, base);
}

REX_EXTERN(__imp__sub_820BF910);
extern "C" REX_FUNC(sub_820BF910) {
  if (rex::system::INativeGpu* native = NativeGpu()) {
    g_native.clears.fetch_add(1, std::memory_order_relaxed);
    BurstLimitNativeScopeSetLabel("ClearF");
    NativeOpCall<__imp__sub_820BF910>(ctx, base, native);
    BurstLimitNativeScopeSetLabel(nullptr);
    return;
  }
  __imp__sub_820BF910(ctx, base);
}

REX_EXTERN(__imp__sub_820BB348);
extern "C" REX_FUNC(sub_820BB348) {
  if (rex::system::INativeGpu* native = NativeGpu()) {
    g_native.resolves.fetch_add(1, std::memory_order_relaxed);
    BurstLimitNativeScopeSetLabel("Resolve");
    NativeOpCall<__imp__sub_820BB348>(ctx, base, native);
    BurstLimitNativeScopeSetLabel(nullptr);
    return;
  }
  __imp__sub_820BB348(ctx, base);
}

REX_EXTERN(__imp__sub_820B64C0);
extern "C" REX_FUNC(sub_820B64C0) {
  if (rex::system::INativeGpu* native = NativeGpu()) {
    if (const int32_t trace = REXCVAR_GET(native_trace_frames); trace > 0) {
      REXLOG_INFO("Native trace: Swap ({} more frames)", trace - 1);
      rex::cvar::SetFlagByName("native_trace_frames", std::to_string(trace - 1));
    }
    g_native.swaps.fetch_add(1, std::memory_order_relaxed);
    BurstLimitNativeDeviceFrame();
    NativeApiCall<__imp__sub_820B64C0>(ctx, base, native);
    ReportNative();
    return;
  }
  __imp__sub_820B64C0(ctx, base);
}

REX_EXTERN(__imp__sub_820BA5A8);
extern "C" REX_FUNC(sub_820BA5A8) {
  if (NativeGpu()) {
    if (g_native.begin_tilings.fetch_add(1, std::memory_order_relaxed) == 0) {
      REXLOG_WARN("Native renderer: BeginTiling used (tile count {}), it runs in ring order",
                  ctx.r5.u32);
    }
  }
  __imp__sub_820BA5A8(ctx, base);
}

// KickOff: the rendering written so far runs first, then the segment goes to
// the ring with only its sync packets left.
REX_EXTERN(__imp__sub_820AE690);
extern "C" REX_FUNC(sub_820AE690) {
  rex::system::INativeGpu* native = NativeGpu();
  if (!native || t_in_kick) {
    __imp__sub_820AE690(ctx, base);
    return;
  }
  g_native.kicks.fetch_add(1, std::memory_order_relaxed);
  BurstLimitNativeScope(native, base);
  t_in_kick = true;
  __imp__sub_820AE690(ctx, base);
  t_in_kick = false;
  BurstLimitNativeScopeRestart(base);
  if (REXCVAR_GET(native_trace_frames) > 0) {
    REXLOG_INFO("Native trace: KickOff -> {:08X}", ctx.r3.u32);
  }
}

// A new segment without a kick (Swap calls it directly).
REX_EXTERN(__imp__sub_820ADC38);
extern "C" REX_FUNC(sub_820ADC38) {
  rex::system::INativeGpu* native = NativeGpu();
  if (!native || t_in_kick) {
    __imp__sub_820ADC38(ctx, base);
    return;
  }
  BurstLimitNativeScope(native, base);
  t_in_kick = true;
  __imp__sub_820ADC38(ctx, base);
  t_in_kick = false;
  BurstLimitNativeScopeRestart(base);
  if (REXCVAR_GET(native_trace_frames) > 0) {
    REXLOG_INFO("Native trace: new segment -> {:08X}", ctx.r3.u32);
  }
}

// The only CP_RB_WPTR writer: the kicking thread runs the ring right away.
REX_EXTERN(__imp__sub_820ADFB0);
extern "C" REX_FUNC(sub_820ADFB0) {
  const uint32_t segment = ctx.r4.u32;
  __imp__sub_820ADFB0(ctx, base);
  if (rex::system::INativeGpu* native = NativeGpu()) {
    g_native.segment_kicks.fetch_add(1, std::memory_order_relaxed);
    const rex::system::NativeDrainState state = native->TryDrain();
    if (REXCVAR_GET(native_trace_frames) > 0) {
      REXLOG_INFO("Native trace: segment kick {:08X} ({:08X}), drain {}",
                  rex::memory::load_and_swap<uint32_t>(base + segment + 4),
                  rex::memory::load_and_swap<uint32_t>(base + segment), uint32_t(state));
    }
  }
}

REX_EXTERN(__imp__sub_820AE868);
extern "C" REX_FUNC(sub_820AE868) {
  const uint32_t callback = ctx.r6.u32;
  __imp__sub_820AE868(ctx, base);
  if (NativeGpu()) {
    if (REXCVAR_GET(native_trace_frames) > 0) {
      REXLOG_INFO("Native trace: InsertCallback {:08X}", callback);
    }
    if (callback == kFrameStartCallback) {
      t_frame_kick_armed = true;
      PushGpuTimerTime(true);
    } else if (callback == kFrameEndCallback) {
      // BL calls Swap on its game thread, right after queueing the null flip,
      // and never waits for the GPU there; the render thread waits for its
      // frame's fence next, which is behind the swap: XE_SWAP (the present)
      // runs on the render thread.
      NativeGpu()->SetSwapOwnerThread(0);
      PushGpuTimerTime(false);
    }
  }
}

// The GPU timer's frame-start callback (r3 = the timer).
REX_EXTERN(__imp__sub_820E8128);
extern "C" REX_FUNC(sub_820E8128) {
  if (!NativeGpu()) {
    __imp__sub_820E8128(ctx, base);
    return;
  }
  const uint32_t timer = ctx.r3.u32;
  const uint64_t start = PopGpuTimerTime(true);
  if (!rex::memory::load_and_swap<uint32_t>(base + timer + 36)) {
    rex::memory::store_and_swap<uint64_t>(base + timer + 24, start);
  }
}

// The frame-end callback: the frame's GPU time into the slot the game isn't
// reading.
REX_EXTERN(__imp__sub_820E8158);
extern "C" REX_FUNC(sub_820E8158) {
  if (!NativeGpu()) {
    __imp__sub_820E8158(ctx, base);
    return;
  }
  const uint32_t timer = ctx.r3.u32;
  const uint64_t end = PopGpuTimerTime(false);
  if (rex::memory::load_and_swap<uint32_t>(base + timer + 36)) {
    return;
  }
  const uint64_t start = rex::memory::load_and_swap<uint64_t>(base + timer + 24);
  const uint64_t ticks_per_unit = rex::memory::load_and_swap<uint64_t>(base + timer);
  const int32_t reading = rex::memory::load_and_swap<int32_t>(base + timer + 32);
  if (!ticks_per_unit) {
    return;
  }
  rex::memory::store_and_swap<uint64_t>(base + timer + (2 - reading) * 8,
                                        (end - start) / ticks_per_unit);
}

// A wait for the GPU (820B10E8, called in a loop by every library wait): run
// the ring, then sleep until something that can let it go on happens. True
// when handled (native), then the caller runs the original for its hang check.
bool BurstLimitNativeGpuWait() {
  rex::system::INativeGpu* native = NativeGpu();
  if (!native) {
    return false;
  }
  g_native.waits.fetch_add(1, std::memory_order_relaxed);
  const rex::system::NativeDrainState state = native->TryDrain();
  if (REXCVAR_GET(native_trace_frames) > 0) {
    REXLOG_INFO("Native trace: wait, drain {}", uint32_t(state));
  }
  native->WaitForRingProgress(2000);
  return true;
}

// The game's vblank callback (its frame tick).
REX_EXTERN(__imp__sub_820EAA20);
extern "C" REX_FUNC(sub_820EAA20) {
  if (REXCVAR_GET(native_pacing_stats)) {
    const int64_t now_ns = NowNs();
    std::lock_guard<std::mutex> lock(g_pacing.mutex);
    if (!g_pacing.vblank_callbacks++) {
      g_pacing.first_ns = now_ns;
    }
    g_pacing.last_ns = now_ns;
  }
  __imp__sub_820EAA20(ctx, base);
}

// The swap callback: picks the vblank the null flip waits for.
REX_EXTERN(__imp__sub_820B5DF8);
extern "C" REX_FUNC(sub_820B5DF8) {
  if (!REXCVAR_GET(native_pacing_stats)) {
    __imp__sub_820B5DF8(ctx, base);
    return;
  }
  const uint32_t device = rex::memory::load_and_swap<uint32_t>(base + kDevicePointer);
  __imp__sub_820B5DF8(ctx, base);
  if (device) {
    const uint32_t vblank = rex::memory::load_and_swap<uint32_t>(base + device + 16548);
    const uint32_t target = rex::memory::load_and_swap<uint32_t>(base + device + 16556);
    const uint32_t offset = std::min<uint32_t>(target - vblank, 3);
    // The GPU time the game reads now (820E4F90) and its over-budget history.
    const int32_t reading = rex::memory::load_and_swap<int32_t>(base + kGpuTimer + 32) & 1;
    const uint64_t gpu_time =
        rex::memory::load_and_swap<uint64_t>(base + kGpuTimer + 8 + reading * 8);
    const uint32_t history = rex::memory::load_and_swap<uint32_t>(base + kGpuTimeHistory);
    std::lock_guard<std::mutex> lock(g_pacing.mutex);
    ++g_pacing.target_offsets[offset];
    ++g_pacing.gpu_time_frames;
    g_pacing.gpu_time_sum += gpu_time;
    g_pacing.gpu_time_max = std::max(g_pacing.gpu_time_max, gpu_time);
    if (history) {
      ++g_pacing.gpu_time_over_budget_frames;
    }
  }
}

// --- The game thread's per-frame Sleep(1) (optimisation round 1) ---
// sub_82119D18 (the game's frame timer, once per frame on the game thread)
// starts with Sleep(1) (0x82119D38 -> 822A2B50 -> 822A4C90 ->
// KeDelayExecutionThread, 1 ms). On Windows that is ::Sleep(1), 1.0-2.0 ms.
// What paces the frame is elsewhere: at interval 2 (the 30 FPS cap) the timer
// loops on more Sleep(1)s at 0x82119DB4 until its time is up, and at interval
// 1 (every other frame rate) the frame waits for the guest vblank (the null
// flip) and for the render thread, both blocking waits. So the first Sleep(1)
// only adds 1-2 ms to every frame. With the frame rate "unlocked" (a 1 kHz
// guest vblank) that is what held the native frame (2.86 ms; profile_r2.md):
// skipping it there gives 350 -> ~500 FPS on idle frames. Nothing spins
// instead: the thread goes on to its frame and then blocks in the frame
// handshake below or the vblank wait, as before.
// Skipped only with the native renderer, frame_rate = unlocked and no game
// session open; the frame caps (30/60/120/144/display), online timing and the
// emulated path are unchanged.
// frame_timer_sleep_us (testing): -1 = automatic (above), -2 = never skip,
// 0 = always skip, N = a high resolution sleep of N microseconds instead.
REXCVAR_DEFINE_INT32(frame_timer_sleep_us, -1, "Debug",
                     "Testing: the game thread's per-frame Sleep(1) in its frame timer: -1 "
                     "automatic (skipped when native, unlocked and offline), -2 always sleep, "
                     "0 skip, N = sleep N microseconds (high resolution timer)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

bool BurstLimitFrameRateUnlockedOffline();  // burstlimit_patches.cpp

REX_EXTERN(__imp__sub_822A2B50);
extern "C" REX_FUNC(sub_822A2B50) {
  if (ctx.lr != 0x82119D3Cu) {
    __imp__sub_822A2B50(ctx, base);
    return;
  }
  int32_t us = REXCVAR_GET(frame_timer_sleep_us);
  if (us == -1) {
    us = NativeGpu() && BurstLimitFrameRateUnlockedOffline() ? 0 : -2;
  }
  if (us < 0) {
    __imp__sub_822A2B50(ctx, base);
    return;
  }
  if (us > 0) {
    thread_local HANDLE timer = CreateWaitableTimerExW(
        nullptr, nullptr, 0x00000002 /* CREATE_WAITABLE_TIMER_HIGH_RESOLUTION */, TIMER_ALL_ACCESS);
    LARGE_INTEGER due;
    due.QuadPart = -int64_t(us) * 10;
    if (timer && SetWaitableTimerEx(timer, &due, 0, nullptr, nullptr, nullptr, 0)) {
      WaitForSingleObject(timer, INFINITE);
    } else {
      SwitchToThread();
    }
  } else {
    g_native.frame_sleep_skips.fetch_add(1, std::memory_order_relaxed);
  }
  // Sleep's result (KeDelayExecutionThread: STATUS_SUCCESS).
  ctx.r3.u64 = 0;
}

// --- The frame handshake: a bounded wait (no lost wake-up hang) ---
// The game thread and the render thread hand frames over through one
// mutex/event pair of the object at 0x8438AC00 (+144 mutex, +148 event, +160
// "frame queued"): the game thread waits for +160 == 0 (820E7920, loop at
// 0x820E79C4), the render thread for +160 == 1 (820EA188, loop at 0x820EA1F0),
// and 820F0340 stores +160 and sets the event. Both waiters use the SAME
// auto-reset event with opposite conditions, through 822A2E50 ->
// NtSignalAndWaitForSingleObjectEx(mutex, event, infinite). On the console
// that call is atomic and a set event goes straight to the thread already
// waiting. On the host (SignalObjectAndWait) the release and the wait are not
// atomic, and a thread that a debugger, a profiler or an overlay suspends is
// out of its wait for a while: the game thread can then set the event and
// take it back itself in its own wait, and both threads wait for good (seen
// in profile round 2 whenever these threads were suspended, both renderers).
// With the native renderer the three waits on these objects get a timeout, so
// a lost wake-up costs one timeout instead of a hang: the callers already
// loop and re-check their condition under the mutex (822A2E50 returns
// STATUS_TIMEOUT, which they treat like a wake-up).
// handshake_wait_ms (testing): 0 = infinite as before.
REXCVAR_DEFINE_INT32(handshake_wait_ms, 8, "Debug",
                     "Testing: timeout of the game/render thread frame handshake waits with the "
                     "native renderer (0 = infinite, as the game does)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REX_EXTERN(__imp__sub_822A2E50);
extern "C" REX_FUNC(sub_822A2E50) {
  // lr: 0x820E79D4 / 0x820E7A40 game thread (820E7920), 0x820EA200 render
  // thread (820EA188). r5 = the timeout in ms, -1 = infinite.
  const uint32_t lr = uint32_t(ctx.lr);
  int32_t timeout_ms = 0;
  if (ctx.r5.u32 == 0xFFFFFFFFu &&
      (lr == 0x820E79D4u || lr == 0x820E7A40u || lr == 0x820EA200u) && NativeGpu()) {
    timeout_ms = REXCVAR_GET(handshake_wait_ms);
  }
  if (timeout_ms <= 0) {
    __imp__sub_822A2E50(ctx, base);
    return;
  }
  ctx.r5.u64 = uint32_t(timeout_ms);
  __imp__sub_822A2E50(ctx, base);
  if (ctx.r3.u32 == 0x102u /* STATUS_TIMEOUT */) {
    g_native.handshake_timeouts.fetch_add(1, std::memory_order_relaxed);
  }
}
