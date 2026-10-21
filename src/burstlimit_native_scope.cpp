// Native renderer: synchronous scopes (the S tier, design.md section 4).
//
// The XDK D3D library writes PM4 into command buffer segments at dev+48
// (dev+48 = the last dword written; every emitter does p = dev+48, stwu ...,
// dev+48 = p) and hands a segment to the GPU at a kick (KickOff 820AE690 ->
// 820AE430 -> 820ADFB0 = INDIRECT_BUFFER into the primary ring + CP_RB_WPTR).
// With gpu_native_renderer, everything the library wrote since the last scope
// is handed to the SDK when a D3D call returns to the game (and before every
// kick): INativeGpu::ExecuteScope runs the rendering packets right away on
// this thread and turns them into NOPs, and leaves the sync packets (fences,
// callbacks, waits, the null flip, VdSwap) where they are for the ring.
// Rendering therefore happens at API time, in API order, whatever library
// function wrote it (also emitters nobody hooked, like the window registers of
// 820A9C38 or Swap's bin reset 820A9A78): no census of emitters is needed.
//
// The range is only ever taken at a point where the library has finished
// writing (the outermost D3D call returned, or a kick is about to hand the
// segment over), so nothing patches a packet after it ran.

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
#include <rex/system/interfaces/native_gpu.h>

REXCVAR_DECLARE(int32_t, native_trace_frames);

REXCVAR_DEFINE_INT32(native_dump_calls, 0, "Debug",
                     "Testing: log the command buffer packets of the next N ClearF / Resolve "
                     "scopes (counts down)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

// The D3D call whose commands the next scope hands over (debug dumps only).
thread_local const char* t_scope_label = nullptr;
void BurstLimitNativeScopeSetLabel(const char* label) { t_scope_label = label; }

namespace {

constexpr uint32_t kDevicePointer = 0x8437EE04;
constexpr uint32_t kDevRingWrite = 48;      // last dword written
constexpr uint32_t kDevFlags = 10940;       // 0x01 recording, 0x20 tiling
constexpr uint32_t kDevSegment = 14928;     // start of the current segment
constexpr uint32_t kMaxScopeBytes = 8u << 20;

// The command buffer is the device's, and only the thread that owns the
// device (the guest critical section around it) writes it; the lock is for
// anything that calls D3D without that.
std::mutex g_scope_mutex;
// Guest address of the first dword not handed over yet (0 = take dev+48), and
// the segment it is in.
uint32_t g_scope_next = 0;
uint32_t g_scope_segment = 0;

// The N tier's register writes (burstlimit_native_device.cpp) since the last
// scope: the guest address of the dword the packet would have started at,
// and the values copied when the library would have copied them.
struct PendingWrite {
  uint32_t at;
  uint32_t first_register;
  uint32_t count;
  uint32_t value_offset;
};
std::vector<PendingWrite> g_pending_writes;
std::vector<uint32_t> g_pending_values;
std::vector<rex::system::NativeRegisterWrite> g_scope_writes;

struct ScopeCounters {
  std::atomic<uint64_t> scopes{0};
  std::atomic<uint64_t> packets{0};
  std::atomic<uint64_t> dwords{0};
  std::atomic<uint64_t> left{0};
  std::atomic<uint64_t> failures{0};
  std::atomic<uint64_t> resyncs{0};
  std::atomic<uint64_t> islands{0};
  std::atomic<uint64_t> writes_out_of_order{0};
};
ScopeCounters g_scope;

// The library's own virtual -> physical conversion (820ADFB0, 820AE430).
uint32_t GuestPhysical(uint32_t address) {
  return (address & 0x1FFFFFFF) + ((((address >> 20) & 0xFFF) + 0x200) & 0x1000);
}

// The pending writes as positions in [start, end) (dwords); clears them.
// Under g_scope_mutex.
const rex::system::NativeRegisterWrite* TakeWrites(uint32_t start, uint32_t end,
                                                   uint32_t* count) {
  g_scope_writes.clear();
  for (const PendingWrite& w : g_pending_writes) {
    rex::system::NativeRegisterWrite& out = g_scope_writes.emplace_back();
    out.at_dword = w.at <= start ? 0 : (std::min(w.at, end) - start) / 4;
    out.first_register = w.first_register;
    out.count = w.count;
    out.values = g_pending_values.data() + w.value_offset;
  }
  g_pending_writes.clear();
  *count = uint32_t(g_scope_writes.size());
  return g_scope_writes.data();
}

// Pending writes where the command buffer can't place them (an island, the
// emergency buffer): applied now, before the packets the ring runs later.
void ApplyWritesNow(rex::system::INativeGpu* native) {
  if (g_pending_writes.empty()) {
    return;
  }
  g_scope.writes_out_of_order.fetch_add(g_pending_writes.size(), std::memory_order_relaxed);
  uint32_t count = 0;
  const rex::system::NativeRegisterWrite* writes = TakeWrites(0, 0, &count);
  native->ExecuteScope(0, 0, nullptr, writes, count);
  g_pending_values.clear();
}

void DumpScope(uint8_t* base, uint32_t start, uint32_t end, uint32_t write_count) {
  const int32_t left = REXCVAR_GET(native_dump_calls);
  if (left <= 0 || !t_scope_label) {
    return;
  }
  rex::cvar::SetFlagByName("native_dump_calls", std::to_string(left - 1));
  std::string text = fmt::format("{} {:08X}..{:08X} native writes {}:", t_scope_label, start, end,
                                 write_count);
  const uint8_t* p = base + start;
  uint32_t pos = 0;
  const uint32_t dwords = (end - start) / 4;
  while (pos < dwords) {
    const uint32_t h = rex::memory::load_and_swap<uint32_t>(p + pos * 4);
    uint32_t size = 1;
    switch (h >> 30) {
      case 0:
        size = ((h >> 16) & 0x3FFF) + 2;
        text += fmt::format(" R{:04X}x{}", h & 0x7FFF, size - 1);
        for (uint32_t i = 1; i < size && i < 9 && pos + i < dwords; ++i) {
          text += fmt::format("{}{:X}", i == 1 ? "=" : ",",
                              rex::memory::load_and_swap<uint32_t>(p + (pos + i) * 4));
        }
        break;
      case 1:
        size = 3;
        text += " T1";
        break;
      case 2:
        break;
      default:
        size = ((h >> 16) & 0x3FFF) + 2;
        text += fmt::format(" P{:02X}x{}", (h >> 8) & 0x7F, size - 1);
        for (uint32_t i = 1; i < size && i < 6 && pos + i < dwords; ++i) {
          text += fmt::format("{}{:X}", i == 1 ? "=" : ",",
                              rex::memory::load_and_swap<uint32_t>(p + (pos + i) * 4));
        }
        break;
    }
    pos += size;
  }
  REXLOG_INFO("Native dump: {}", text);
}


// --- ClearF / Resolve as typed native commands ---
// The library still encodes the call into its command buffer (its bookkeeping:
// dirty bits, fences, the rectangle vertices, the allocation), but nothing of
// it runs through the packet processor: the call's register state goes to the
// backend as register writes (the pre-flush helpers' writes come natively, as
// for draws), its shaders as shader loads and its draw / copy as a direct
// IssueDraw / IssueCopy call (INativeGpu::ExecuteCommands). The packets are
// turned into NOPs; sync packets (coherency waits, fences) stay for the ring.
// Anything outside that shape falls back to the S tier (ExecuteScope).

struct OpCounters {
  std::atomic<uint64_t> calls{0};
  std::atomic<uint64_t> lifted{0};
  std::atomic<uint64_t> fallbacks{0};
  std::atomic<uint64_t> fallback_reason[6] = {};  // islands, range, packet, ib, onereg, empty
  std::atomic<uint64_t> commands{0};
  std::atomic<uint64_t> draws{0};
  std::atomic<uint64_t> ring_left{0};
};
OpCounters g_ops;

// Value sources of a lifted command before the pointers are fixed up.
enum : uint32_t { kValuesDirect = 0, kValuesPending = 1, kValuesStorage = 2 };
struct LiftedCommand {
  rex::system::NativeCommand command;
  uint32_t values_source = kValuesDirect;
  uint32_t values_index = 0;
};
std::vector<LiftedCommand> g_op_lifted;
std::vector<rex::system::NativeCommand> g_op_commands;
std::vector<uint32_t> g_op_storage;  // computed values
std::vector<uint32_t> g_op_nops;     // packets to turn into NOPs (address, dwords)

enum LiftResult { kLiftOk, kLiftPacket, kLiftIb, kLiftOneReg };

uint32_t SwapBe(uint32_t v) { return rex::memory::load_and_swap<uint32_t>(&v); }

void AddPendingWrite(const PendingWrite& w) {
  LiftedCommand& l = g_op_lifted.emplace_back();
  l.command.type = rex::system::NativeCommandType::kRegisters;
  l.command.first_register = w.first_register;
  l.command.count = w.count;
  l.values_source = kValuesPending;
  l.values_index = w.value_offset;
}

// Lifts [address, address + dwords * 4). top: packets of the call's own range
// (lifted ones become NOPs, the native pre-flush writes go in between at their
// positions); otherwise the content of an indirect buffer (left as it is).
LiftResult LiftRange(uint8_t* base, uint32_t address, uint32_t dwords, bool top,
                     size_t* next_write, uint32_t* ring_left, int depth) {
  auto load = [&](uint32_t i) {
    return rex::memory::load_and_swap<uint32_t>(base + address + i * 4);
  };
  uint32_t pos = 0;
  while (pos < dwords) {
    const uint32_t packet_address = address + pos * 4;
    if (top) {
      while (*next_write < g_pending_writes.size() &&
             g_pending_writes[*next_write].at <= packet_address) {
        AddPendingWrite(g_pending_writes[(*next_write)++]);
      }
    }
    const uint32_t h = load(pos);
    uint32_t size = 1;
    bool lifted = false;
    switch (h >> 30) {
      case 0: {
        size = ((h >> 16) & 0x3FFF) + 2;
        if (pos + size > dwords) {
          return kLiftPacket;
        }
        const uint32_t reg = h & 0x7FFF;
        if (reg < 0x2000) {
          ++*ring_left;  // CP registers: sync, the ring keeps them
          break;
        }
        if (h & 0x8000) {
          return kLiftOneReg;
        }
        LiftedCommand& l = g_op_lifted.emplace_back();
        l.command.type = rex::system::NativeCommandType::kRegisters;
        l.command.first_register = reg;
        l.command.count = size - 1;
        l.command.values = reinterpret_cast<const uint32_t*>(base + packet_address + 4);
        lifted = true;
        break;
      }
      case 1:
        size = 3;
        ++*ring_left;
        break;
      case 2:
        break;
      default: {
        size = ((h >> 16) & 0x3FFF) + 2;
        if (pos + size > dwords) {
          return kLiftPacket;
        }
        const uint32_t opcode = (h >> 8) & 0x7F;
        switch (opcode) {
          case 0x10:  // NOP
            break;
          case 0x3B:  // INVALIDATE_STATE: no effect on the host
            lifted = true;
            break;
          case 0x2B: {  // IM_LOAD_IMMEDIATE
            if (size < 3) {
              return kLiftPacket;
            }
            const uint32_t start_size = load(pos + 2);
            const uint32_t shader_dwords = start_size & 0xFFFF;
            if ((start_size >> 16) || shader_dwords > size - 3) {
              return kLiftPacket;
            }
            LiftedCommand& l = g_op_lifted.emplace_back();
            l.command.type = rex::system::NativeCommandType::kShader;
            l.command.shader_type = load(pos + 1);
            l.command.count = shader_dwords;
            l.command.values = reinterpret_cast<const uint32_t*>(base + packet_address + 12);
            l.command.guest_address = GuestPhysical(packet_address + 12);
            lifted = true;
            break;
          }
          case 0x36: {  // DRAW_INDX_2, auto-indexed
            if (size != 2 || ((load(pos + 1) >> 6) & 3) != 2) {
              return kLiftPacket;
            }
            LiftedCommand& l = g_op_lifted.emplace_back();
            l.command.type = rex::system::NativeCommandType::kDraw;
            l.command.draw_initiator = load(pos + 1);
            lifted = true;
            break;
          }
          case 0x46: {  // EVENT_WRITE (an event, no address): VGT_EVENT_INITIATOR
            if (size != 2) {
              return kLiftPacket;
            }
            g_op_storage.push_back(SwapBe(load(pos + 1) & 0x3F));
            LiftedCommand& l = g_op_lifted.emplace_back();
            l.command.type = rex::system::NativeCommandType::kRegisters;
            l.command.first_register = 0x21F9;
            l.command.count = 1;
            l.values_source = kValuesStorage;
            l.values_index = uint32_t(g_op_storage.size() - 1);
            lifted = true;
            break;
          }
          case 0x60:    // SET_BIN_MASK_LO
          case 0x61: {  // SET_BIN_MASK_HI
            if (size != 2) {
              return kLiftPacket;
            }
            g_op_storage.push_back(load(pos + 1));
            LiftedCommand& l = g_op_lifted.emplace_back();
            l.command.type = rex::system::NativeCommandType::kBinMask;
            l.command.first_register = opcode - 0x60;
            l.command.count = 1;
            l.values_source = kValuesStorage;
            l.values_index = uint32_t(g_op_storage.size() - 1);
            lifted = true;
            break;
          }
          case 0x3F:    // INDIRECT_BUFFER
          case 0x37: {  // INDIRECT_BUFFER_PFD
            if (size < 3 || depth >= 3) {
              return kLiftIb;
            }
            // The library's prebuilt blocks: lifted when everything in them
            // is liftable rendering state (the block itself stays as it is).
            const uint32_t ib_virtual = load(pos + 1) | 0xA0000000u;
            const uint32_t ib_dwords = load(pos + 2) & 0xFFFFF;
            uint32_t ib_ring = 0;
            size_t unused = 0;
            const LiftResult r =
                LiftRange(base, ib_virtual, ib_dwords, false, &unused, &ib_ring, depth + 1);
            if (r != kLiftOk || ib_ring) {
              return kLiftIb;
            }
            lifted = true;
            break;
          }
          case 0x3C:  // WAIT_REG_MEM
          case 0x54:  // INTERRUPT
          case 0x3D:  // MEM_WRITE
          case 0x58:  // EVENT_WRITE_SHD
          case 0x64:  // XE_SWAP
          case 0x45:  // COND_WRITE
          case 0x26:  // WAIT_FOR_IDLE
          case 0x21:  // REG_RMW
            // Sync, memory writes, the swap: the ring keeps them.
            ++*ring_left;
            break;
          default:
            return kLiftPacket;
        }
        break;
      }
    }
    if (lifted && top) {
      g_op_nops.push_back(packet_address);
      g_op_nops.push_back(size);
    }
    pos += size;
  }
  return kLiftOk;
}

}  // namespace

// A ClearF / Resolve returned (outermost call; what came before it was swept
// at its start): hands its commands over as typed native commands. False: not
// liftable (nothing was run), the caller runs the S tier.
bool BurstLimitNativeOpScope(rex::system::INativeGpu* native, uint8_t* base) {
  const uint32_t device = rex::memory::load_and_swap<uint32_t>(base + kDevicePointer);
  if (!device) {
    return false;
  }
  std::lock_guard<std::mutex> lock(g_scope_mutex);
  g_ops.calls.fetch_add(1, std::memory_order_relaxed);
  auto fallback = [&](int reason) {
    g_ops.fallbacks.fetch_add(1, std::memory_order_relaxed);
    g_ops.fallback_reason[reason].fetch_add(1, std::memory_order_relaxed);
    return false;
  };
  if (base[device + kDevFlags] & 0x21) {
    return fallback(0);
  }
  const uint32_t end = rex::memory::load_and_swap<uint32_t>(base + device + kDevRingWrite) + 4;
  const uint32_t segment = rex::memory::load_and_swap<uint32_t>(base + device + kDevSegment);
  const uint32_t start = (g_scope_next && segment == g_scope_segment) ? g_scope_next : segment;
  if (!start || end < start || end - start > kMaxScopeBytes) {
    return fallback(1);
  }
  if (end == start && g_pending_writes.empty()) {
    return fallback(5);
  }
  g_op_lifted.clear();
  g_op_storage.clear();
  g_op_nops.clear();
  size_t next_write = 0;
  uint32_t ring_left = 0;
  const LiftResult r =
      LiftRange(base, start, (end - start) / 4, true, &next_write, &ring_left, 0);
  if (r != kLiftOk) {
    return fallback(r == kLiftIb ? 3 : r == kLiftOneReg ? 4 : 2);
  }
  while (next_write < g_pending_writes.size()) {
    AddPendingWrite(g_pending_writes[next_write++]);
  }
  g_op_commands.clear();
  uint64_t draws = 0;
  for (LiftedCommand& l : g_op_lifted) {
    if (l.values_source == kValuesPending) {
      l.command.values = g_pending_values.data() + l.values_index;
    } else if (l.values_source == kValuesStorage) {
      l.command.values = g_op_storage.data() + l.values_index;
    }
    draws += l.command.type == rex::system::NativeCommandType::kDraw;
    g_op_commands.push_back(l.command);
  }
  g_scope_next = end;
  g_scope_segment = segment;
  native->ExecuteCommands(g_op_commands.data(), uint32_t(g_op_commands.size()));
  g_pending_writes.clear();
  g_pending_values.clear();
  // The ring skips what ran.
  for (size_t i = 0; i < g_op_nops.size(); i += 2) {
    const uint32_t address = g_op_nops[i], size = g_op_nops[i + 1];
    const uint32_t header =
        size == 1 ? 0x80000000u : (0xC0000000u | ((size - 2) << 16) | (0x10u << 8));
    rex::memory::store_and_swap<uint32_t>(base + address, header);
  }
  g_ops.lifted.fetch_add(1, std::memory_order_relaxed);
  g_ops.commands.fetch_add(g_op_commands.size(), std::memory_order_relaxed);
  g_ops.draws.fetch_add(draws, std::memory_order_relaxed);
  g_ops.ring_left.fetch_add(ring_left, std::memory_order_relaxed);
  return true;
}

std::string BurstLimitNativeOpStats(bool reset) {
  auto get = [&](std::atomic<uint64_t>& a) {
    return reset ? a.exchange(0, std::memory_order_relaxed) : a.load(std::memory_order_relaxed);
  };
  const uint64_t calls = get(g_ops.calls), lifted = get(g_ops.lifted),
                 fallbacks = get(g_ops.fallbacks), commands = get(g_ops.commands),
                 draws = get(g_ops.draws), ring_left = get(g_ops.ring_left);
  uint64_t reasons[6];
  for (int i = 0; i < 6; ++i) {
    reasons[i] = get(g_ops.fallback_reason[i]);
  }
  return fmt::format(
      "native ops (ClearF / Resolve) calls {} native {} s_tier {} (islands {} range {} packet {} "
      "ib {} onereg {} empty {}) commands {} draws {} ring_left {}",
      calls, lifted, fallbacks, reasons[0], reasons[1], reasons[2], reasons[3], reasons[4],
      reasons[5], commands, draws, ring_left);
}

namespace {

}  // namespace

// Hands everything written since the last scope to the SDK.
void BurstLimitNativeScope(rex::system::INativeGpu* native, uint8_t* base) {
  const uint32_t device = rex::memory::load_and_swap<uint32_t>(base + kDevicePointer);
  if (!device) {
    return;
  }
  std::lock_guard<std::mutex> lock(g_scope_mutex);
  const uint32_t end = rex::memory::load_and_swap<uint32_t>(base + device + kDevRingWrite) + 4;
  if (base[device + kDevFlags] & 0x21) {
    // Predicated tiling records into side buffers and replays them through
    // the ring: that island runs in ring order (0 per frame in BL).
    g_scope_next = end;
    g_scope.islands.fetch_add(1, std::memory_order_relaxed);
    ApplyWritesNow(native);
    return;
  }
  const uint32_t segment = rex::memory::load_and_swap<uint32_t>(base + device + kDevSegment);
  // A segment not seen yet (the first call, or a switch no hook saw) is
  // handed over from its start: nothing of it has been kicked.
  const uint32_t start =
      (g_scope_next && segment == g_scope_segment) ? g_scope_next : segment;
  g_scope_next = end;
  g_scope_segment = segment;
  if (!start || end < start || end - start > kMaxScopeBytes) {
    // The library writes somewhere else (its 4800-byte emergency buffer):
    // that goes through the ring in order.
    if (REXCVAR_GET(native_trace_frames) > 0) {
      REXLOG_INFO("Native trace: scope resync {:08X} -> {:08X} (segment {:08X})", start, end,
                  segment);
    }
    g_scope.resyncs.fetch_add(1, std::memory_order_relaxed);
    ApplyWritesNow(native);
    return;
  }
  uint32_t write_count = 0;
  const rex::system::NativeRegisterWrite* writes = TakeWrites(start, end, &write_count);
  if (end == start && !write_count) {
    return;
  }
  DumpScope(base, start, end, write_count);
  rex::system::NativeScopeResult result;
  native->ExecuteScope(GuestPhysical(start), (end - start) / 4, &result, writes, write_count);
  g_pending_values.clear();
  if (REXCVAR_GET(native_trace_frames) > 0) {
    REXLOG_INFO(
        "Native trace: scope {:08X}..{:08X} (segment {:08X}): ran {} left {}{} [{:08X} {:08X}, "
        "{:08X} {:08X}, {:08X} {:08X}, {:08X} {:08X}]",
        start, end, segment, result.executed_packets, result.left_packets,
        result.ok ? "" : " FAILED", result.left_headers[0], result.left_headers[1],
        result.left_headers[2], result.left_headers[3], result.left_headers[4],
        result.left_headers[5], result.left_headers[6], result.left_headers[7]);
  }
  g_scope.scopes.fetch_add(1, std::memory_order_relaxed);
  g_scope.packets.fetch_add(result.executed_packets, std::memory_order_relaxed);
  g_scope.dwords.fetch_add(result.executed_dwords, std::memory_order_relaxed);
  g_scope.left.fetch_add(result.left_packets, std::memory_order_relaxed);
  if (!result.ok && g_scope.failures.fetch_add(1, std::memory_order_relaxed) < 8) {
    REXLOG_WARN("Native renderer: scope {:08X}..{:08X} doesn't end on a packet boundary", start,
                end);
  }
}

// A native register write at the current command buffer position (the N
// tier, burstlimit_native_device.cpp): runs with the next scope, in order.
void BurstLimitNativeScopeAddWrite(uint8_t* base, uint32_t device, uint32_t first_register,
                                   const uint8_t* values, uint32_t count) {
  std::lock_guard<std::mutex> lock(g_scope_mutex);
  PendingWrite& w = g_pending_writes.emplace_back();
  w.at = rex::memory::load_and_swap<uint32_t>(base + device + kDevRingWrite) + 4;
  w.first_register = first_register;
  w.count = count;
  w.value_offset = uint32_t(g_pending_values.size());
  g_pending_values.resize(g_pending_values.size() + count);
  std::memcpy(g_pending_values.data() + w.value_offset, values, size_t(count) * 4);
}

// After a kick: the library continues in a new segment at dev+48.
void BurstLimitNativeScopeRestart(uint8_t* base) {
  const uint32_t device = rex::memory::load_and_swap<uint32_t>(base + kDevicePointer);
  if (!device) {
    return;
  }
  std::lock_guard<std::mutex> lock(g_scope_mutex);
  g_scope_next = rex::memory::load_and_swap<uint32_t>(base + device + kDevRingWrite) + 4;
  g_scope_segment = rex::memory::load_and_swap<uint32_t>(base + device + kDevSegment);
}

// Counters since the last reset: scopes, packets, dwords, left, failures,
// resyncs, islands, native writes applied out of command buffer order.
void BurstLimitNativeScopeCounters(uint64_t out[8], bool reset) {
  std::atomic<uint64_t>* counters[8] = {&g_scope.scopes,   &g_scope.packets,  &g_scope.dwords,
                                        &g_scope.left,     &g_scope.failures, &g_scope.resyncs,
                                        &g_scope.islands,  &g_scope.writes_out_of_order};
  for (int i = 0; i < 8; ++i) {
    out[i] = reset ? counters[i]->exchange(0, std::memory_order_relaxed)
                   : counters[i]->load(std::memory_order_relaxed);
  }
}
