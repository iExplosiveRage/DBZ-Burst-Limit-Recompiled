// Native renderer, milestone M2: the N tier for DrawVertices / DrawIndexedVertices
// (design.md section 5, built at the flush helpers).
//
// The draws' state flush (820A8A20 / 820A8E10) turns the device's dirty bits
// into PM4 with five helpers. The three that copy register shadow into the
// command buffer - the bulk of what a frame writes - are replaced while a
// native draw runs: instead of type-0 packets the register values go to the
// SDK as NativeRegisterWrites, tagged with the command buffer position the
// packet would have had, and the scope that hands the draw over applies them
// in that order (INativeGpu::ExecuteScope). Everything else of the draw stays
// the library's own code: the dirty-bit bookkeeping (the callers clear the
// bits), the vertex shader binder 820B06F8 and its constant blocks, the
// derived registers 820AF228, the clip planes 820AF680 (they carry a COHER
// wait), the draw packet, the splits and the predicate. So the effective
// register state at every draw is the emulated one by construction, as long
// as each write equals the packet it replaces - which verify mode checks.
//
//   820AF5C0 (dev, bits, first register, shadow): bit 63-p set -> register
//            first + p = shadow[p] (registers, the bool / loop constants)
//   820AF960 (dev, bits, first register, shadow): bit 63-p -> 16 registers
//            first + 16p from shadow + 64p (float constants)
//   820AF800 (dev, bits): bit 63-s -> fetch constant s (0x4800 + 6s, 6 dwords
//            from dev+1152+24s), then SHADER_CONSTANT_FLUSH_FETCH 0x5000-0x5002
//
// gpu_native_calls = none turns the N tier off (the M1 path: every helper
// writes PM4 and the scope runs it); it's read at each Swap.
// gpu_native_verify runs the original helper as well, decodes what it wrote and
// compares it with the native writes (which are then dropped: the frame
// renders from the original packets), plus the device bytes around it. Device
// bytes that may differ are counted by class, not as mismatches: X2 the
// command buffer allocation fields (native writes no PM4), X6 fields other
// threads change meanwhile (the vblank / flip block, critical sections).
// Late CPU writes can't happen here: the values are copied at the moment the
// library would have copied them into the command buffer.

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/ppc.h>
#include <rex/ppc/context.h>

REXCVAR_DEFINE_STRING(gpu_native_calls, "all", "GPU",
                      "Native renderer: which D3D calls take the native (N) tier - 'all' "
                      "(DrawVertices / DrawIndexedVertices, ClearF, Resolve), 'draws' (only the "
                      "draws) or 'none' (every call runs the library's command buffer code); "
                      "applied at the next frame");

REXCVAR_DEFINE_BOOL(gpu_native_verify, false, "Debug",
                    "Testing: native renderer verify mode - every native register feed is also "
                    "run the library's way and compared (the library's packets render)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DECLARE(int32_t, native_trace_frames);

void BurstLimitNativeScopeAddWrite(uint8_t* base, uint32_t device, uint32_t first_register,
                                   const uint8_t* values, uint32_t count);

namespace {

constexpr uint32_t kDevRingWrite = 48;
constexpr uint32_t kDevFlags = 10940;
constexpr uint32_t kDevVizId = 12187;
constexpr uint32_t kDevFetchConstants = 1152;
constexpr uint32_t kDeviceSize = 0x5780;
constexpr uint32_t kFetchConstantBase = 0x4800;
constexpr uint32_t kFlushFetchBase = 0x5000;

std::atomic<bool> g_calls_all{true};
// ClearF / Resolve as typed native commands (gpu_native_calls all).
std::atomic<bool> g_calls_ops{true};

// This thread is inside a native draw (the helpers write natively).
thread_local bool t_native_draw = false;

struct DeviceCounters {
  std::atomic<uint64_t> native_draws{0};
  std::atomic<uint64_t> fallback_flags{0};   // dev+10940 & 0xE1 (predication, tiling, variants)
  std::atomic<uint64_t> fallback_viz{0};     // dev+12187 != 0 (a viz query)
  std::atomic<uint64_t> fallback_off{0};     // gpu_native_calls none
  std::atomic<uint64_t> helper_calls{0};
  std::atomic<uint64_t> helper_dwords{0};
  // Verify mode.
  std::atomic<uint64_t> verify_calls{0};
  std::atomic<uint64_t> verify_matches{0};
  std::atomic<uint64_t> verify_mismatches{0};
  std::atomic<uint64_t> verify_device_mismatches{0};
  std::atomic<uint64_t> verify_skipped{0};   // the original kicked / changed segment (X2)
  std::atomic<uint64_t> verify_x2{0};        // device bytes that differ only in allocation fields
  std::atomic<uint64_t> verify_x6{0};        // and in fields the vblank interrupt wrote meanwhile
};
DeviceCounters g_device;

// One register write as (register, value) pairs, for the comparison.
struct RegisterValue {
  uint32_t reg;
  uint32_t value;
  bool operator==(const RegisterValue& other) const {
    return reg == other.reg && value == other.value;
  }
};

void AddNative(std::vector<RegisterValue>* list, uint32_t first, const uint8_t* values,
               uint32_t count) {
  for (uint32_t i = 0; i < count; ++i) {
    list->push_back({first + i, rex::memory::load_and_swap<uint32_t>(values + i * 4)});
  }
}

// Bits MSB first as runs: fn(first position, run length).
template <typename Fn>
void ForEachRun(uint64_t bits, Fn&& fn) {
  uint32_t position = 0;
  while (bits) {
    const uint32_t skip = uint32_t(__builtin_clzll(bits));
    bits <<= skip;
    position += skip;
    const uint32_t run = (~bits == 0) ? 64 : uint32_t(__builtin_clzll(~bits));
    fn(position, run);
    bits = run >= 64 ? 0 : bits << run;
    position += run;
  }
}

// The writes a helper makes: sink(first register, guest values, count).
template <typename Sink>
void HelperRegisters(uint8_t* base, uint32_t device, uint32_t helper, uint64_t bits,
                     uint32_t first_register, uint32_t shadow, Sink&& sink) {
  switch (helper) {
    case 0x820AF5C0:
      ForEachRun(bits, [&](uint32_t p, uint32_t n) {
        sink(first_register + p, base + shadow + p * 4, n);
      });
      break;
    case 0x820AF960:
      ForEachRun(bits, [&](uint32_t p, uint32_t n) {
        sink(first_register + p * 16, base + shadow + p * 64, n * 16);
      });
      break;
    case 0x820AF800: {
      ForEachRun(bits, [&](uint32_t s, uint32_t n) {
        sink(kFetchConstantBase + s * 6, base + device + kDevFetchConstants + s * 24, n * 6);
      });
      // The library's trailer (as the packet lays it out: 0, 0x25000, 0).
      static const uint8_t kFlushFetch[12] = {0, 0, 0, 0, 0, 2, 0x50, 0, 0, 0, 0, 0};
      sink(kFlushFetchBase, kFlushFetch, 3);
    } break;
  }
}

// The type-0 register writes in [start, end) (guest addresses of dwords);
// false when something else than type-0 packets and NOPs is there.
bool DecodeRegisterPackets(uint8_t* base, uint32_t start, uint32_t end,
                           std::vector<RegisterValue>* out) {
  uint32_t p = start;
  while (p < end) {
    const uint32_t header = rex::memory::load_and_swap<uint32_t>(base + p);
    switch (header >> 30) {
      case 0: {
        const uint32_t count = ((header >> 16) & 0x3FFF) + 1;
        const uint32_t reg = header & 0x7FFF;
        const bool one_reg = (header >> 15) & 1;
        if (p + 4 + count * 4 > end) {
          return false;
        }
        for (uint32_t i = 0; i < count; ++i) {
          out->push_back({one_reg ? reg : reg + i,
                          rex::memory::load_and_swap<uint32_t>(base + p + 4 + i * 4)});
        }
        p += 4 + count * 4;
      } break;
      case 2:
        p += 4;
        break;
      case 3:
        if (((header >> 8) & 0x7F) != 0x10) {  // only PM4_NOP
          return false;
        }
        p += 4 + (((header >> 16) & 0x3FFF) + 1) * 4;
        break;
      default:
        return false;
    }
  }
  return p == end;
}

// X6: fields other threads write meanwhile (the helpers only store dev+48):
// the vblank / flip block of the interrupt handler (vblank count / time, flip
// queue, vblank spinlock, last flip time), dev+10988 (820B7450 / 820B8DC0) and
// the device's critical sections at dev+14944 / 14972.
bool InterruptField(uint32_t offset) {
  return (offset >= 16540 && offset < 16716) || (offset >= 21696 && offset < 21704) ||
         (offset >= 10988 && offset < 10992) || (offset >= 14944 && offset < 15000);
}

bool AllocationField(uint32_t offset) {
  return (offset >= 48 && offset < 60) || (offset >= 13444 && offset < 13448) ||
         (offset >= 13516 && offset < 13520) || (offset >= 14896 && offset < 14944);
}

void LogMismatch(uint32_t helper, const std::vector<RegisterValue>& native,
                 const std::vector<RegisterValue>& original) {
  const uint64_t n = g_device.verify_mismatches.fetch_add(1, std::memory_order_relaxed);
  if (n >= 32) {
    return;
  }
  size_t i = 0;
  while (i < native.size() && i < original.size() && native[i] == original[i]) {
    ++i;
  }
  REXLOG_WARN(
      "Native verify: {:08X} mismatch: native {} writes, library {}; first difference at {}: "
      "native {:04X}={:08X}, library {:04X}={:08X}",
      helper, native.size(), original.size(), i, i < native.size() ? native[i].reg : 0,
      i < native.size() ? native[i].value : 0, i < original.size() ? original[i].reg : 0,
      i < original.size() ? original[i].value : 0);
}

// A helper hook: the native writes instead of the packets, or (verify) both.
template <void (*Original)(PPCContext&, uint8_t*)>
void NativeHelper(PPCContext& ctx, uint8_t* base, uint32_t helper) {
  const uint32_t device = ctx.r3.u32;
  const uint64_t bits = ctx.r4.u64;
  const uint32_t first_register = ctx.r5.u32;
  const uint32_t shadow = ctx.r6.u32;
  g_device.helper_calls.fetch_add(1, std::memory_order_relaxed);
  if (!REXCVAR_GET(gpu_native_verify)) {
    uint64_t dwords = 0;
    HelperRegisters(base, device, helper, bits, first_register, shadow,
                    [&](uint32_t reg, const uint8_t* values, uint32_t count) {
                      BurstLimitNativeScopeAddWrite(base, device, reg, values, count);
                      dwords += count;
                    });
    g_device.helper_dwords.fetch_add(dwords, std::memory_order_relaxed);
    return;
  }

  // Verify: what native would write, then the original into the command buffer.
  std::vector<RegisterValue> native;
  HelperRegisters(base, device, helper, bits, first_register, shadow,
                  [&](uint32_t reg, const uint8_t* values, uint32_t count) {
                    AddNative(&native, reg, values, count);
                  });
  static thread_local std::vector<uint8_t> device_before;
  device_before.assign(base + device, base + device + kDeviceSize);
  const uint32_t before = rex::memory::load_and_swap<uint32_t>(base + device + kDevRingWrite) + 4;
  const uint32_t segment_before = rex::memory::load_and_swap<uint32_t>(base + device + 14928);
  Original(ctx, base);
  const uint32_t after = rex::memory::load_and_swap<uint32_t>(base + device + kDevRingWrite) + 4;
  const uint32_t segment_after = rex::memory::load_and_swap<uint32_t>(base + device + 14928);
  g_device.verify_calls.fetch_add(1, std::memory_order_relaxed);
  if (segment_after != segment_before || after < before) {
    // The original ran out of segment space and kicked: nothing to compare.
    g_device.verify_skipped.fetch_add(1, std::memory_order_relaxed);
    return;
  }

  bool x2 = false;
  bool x6 = false;
  bool device_ok = true;
  for (uint32_t offset = 0; offset < kDeviceSize; ++offset) {
    if (device_before[offset] != base[device + offset]) {
      if (AllocationField(offset)) {
        x2 = true;
      } else if (InterruptField(offset)) {
        x6 = true;
      } else {
        device_ok = false;
        if (g_device.verify_device_mismatches.fetch_add(1, std::memory_order_relaxed) < 32) {
          REXLOG_WARN("Native verify: {:08X} changed device byte +{} ({:02X} -> {:02X})", helper,
                      offset, device_before[offset], base[device + offset]);
        }
        break;
      }
    }
  }
  if (x2) {
    g_device.verify_x2.fetch_add(1, std::memory_order_relaxed);
  }
  if (x6) {
    g_device.verify_x6.fetch_add(1, std::memory_order_relaxed);
  }
  if (!device_ok) {
    return;
  }
  std::vector<RegisterValue> original;
  if (!DecodeRegisterPackets(base, before, after, &original) || native != original) {
    LogMismatch(helper, native, original);
    return;
  }
  g_device.verify_matches.fetch_add(1, std::memory_order_relaxed);
}

}  // namespace

// Called by the Draw hooks: true when this draw takes the N tier (the helper
// hooks then write natively until BurstLimitNativeDrawEnd).
bool BurstLimitNativeDrawBegin(uint8_t* base, uint32_t device) {
  if (!g_calls_all.load(std::memory_order_relaxed)) {
    g_device.fallback_off.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  if (base[device + kDevFlags] & 0xE1) {
    g_device.fallback_flags.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  if (base[device + kDevVizId]) {
    g_device.fallback_viz.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  g_device.native_draws.fetch_add(1, std::memory_order_relaxed);
  t_native_draw = true;
  return true;
}

void BurstLimitNativeDrawEnd() { t_native_draw = false; }

// At each Swap (game thread): picks up gpu_native_calls.
void BurstLimitNativeDeviceFrame() {
  const std::string calls = REXCVAR_GET(gpu_native_calls);
  g_calls_all.store(calls != "none", std::memory_order_relaxed);
  g_calls_ops.store(calls == "all", std::memory_order_relaxed);
}

bool BurstLimitNativeOpsEnabled() { return g_calls_ops.load(std::memory_order_relaxed); }

std::string BurstLimitNativeDeviceStats(bool reset) {
  std::atomic<uint64_t>* c[] = {
      &g_device.native_draws,      &g_device.fallback_flags,  &g_device.fallback_viz,
      &g_device.fallback_off,      &g_device.helper_calls,    &g_device.helper_dwords,
      &g_device.verify_calls,      &g_device.verify_matches,  &g_device.verify_mismatches,
      &g_device.verify_device_mismatches, &g_device.verify_skipped, &g_device.verify_x2,
      &g_device.verify_x6};
  uint64_t v[std::size(c)];
  for (size_t i = 0; i < std::size(c); ++i) {
    v[i] = reset ? c[i]->exchange(0, std::memory_order_relaxed)
                 : c[i]->load(std::memory_order_relaxed);
  }
  return fmt::format(
      "native draws {} fallback flags {} viz {} off {} | helpers {} dwords {} | verify calls {} "
      "match {} mismatch {} device {} skipped {} x2 {} x6 {}",
      v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[8], v[9], v[10], v[11], v[12]);
}

REX_EXTERN(__imp__sub_820AF5C0);
extern "C" REX_FUNC(sub_820AF5C0) {
  if (!t_native_draw) {
    __imp__sub_820AF5C0(ctx, base);
    return;
  }
  NativeHelper<__imp__sub_820AF5C0>(ctx, base, 0x820AF5C0);
}

REX_EXTERN(__imp__sub_820AF960);
extern "C" REX_FUNC(sub_820AF960) {
  if (!t_native_draw) {
    __imp__sub_820AF960(ctx, base);
    return;
  }
  NativeHelper<__imp__sub_820AF960>(ctx, base, 0x820AF960);
}

REX_EXTERN(__imp__sub_820AF800);
extern "C" REX_FUNC(sub_820AF800) {
  if (!t_native_draw) {
    __imp__sub_820AF800(ctx, base);
    return;
  }
  NativeHelper<__imp__sub_820AF800>(ctx, base, 0x820AF800);
}
