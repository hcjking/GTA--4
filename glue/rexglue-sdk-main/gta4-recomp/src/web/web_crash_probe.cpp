/**
 * @file        web/web_crash_probe.cpp
 * @brief       [web port] Diagnostic: the session-start null virtual call
 *
 * sub_82831920 walks a pointer array held by its object (data at +12, 16-bit
 * count and capacity at +16/+18) and calls each item's first virtual method.
 * On the web build one item is 0xCDCDCDCD (memory nobody wrote), so the call
 * goes to address 0. This wrapper logs the object, the array and the caller
 * whenever an item is not a plausible pointer, then runs the original.
 *
 * @license     BSD 3-Clause License
 */

#include <atomic>
#include <string>
#include <string_view>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xevent.h>
#include <rex/system/xthread.h>

#include "gta4_init.h"

// The probes cost every allocation and list append something, and were for
// a crash since fixed (web_layout.h): off unless asked for.
REXCVAR_DEFINE_BOOL(web_crash_probe, false, "Web", "Session-start crash diagnostics (slow, noisy)");

namespace {

bool Probing() { return REXCVAR_GET(web_crash_probe); }

std::atomic<int> reports{0};

bool PlausibleObject(uint32_t pointer) {
  return pointer >= 0x40000000u && pointer != 0xCDCDCDCDu;
}

}  // namespace

// The array grows one slot per append (sub_82825280: reallocate, copy, free
// the old buffer), and the caller stores the new item in the returned slot.
// For the list sub_82831920 later walks (vtable 820829E4, array at +12),
// check every slot before each append and log the first append that finds a
// bad one, with the buffer addresses either side of it.
constexpr uint32_t kListVtable = 0x820829E4;
std::atomic<int> append_reports{0};

extern "C" void sub_82825280(PPCContext& ctx, uint8_t* base) {
  if (!Probing()) {
    __imp__sub_82825280(ctx, base);
    return;
  }
  const uint32_t array = ctx.r3.u32;
  const bool watched = array >= 12 && REX_LOAD_U32(array - 12) == kListVtable;
  uint32_t old_data = 0, old_count = 0, old_capacity = 0;
  if (watched) {
    old_data = REX_LOAD_U32(array);
    old_count = REX_LOAD_U16(array + 4);
    old_capacity = REX_LOAD_U16(array + 6);
    for (uint32_t i = 0; old_data && i < old_count; i++) {
      const uint32_t item = REX_LOAD_U32(old_data + i * 4);
      if (!PlausibleObject(item) && append_reports.fetch_add(1) < 6) {
        REXLOG_ERROR("web probe: append {} to list {:08X} finds slot {} = {:08X} (buffer {:08X}, "
                     "count {}, capacity {}, caller {:08X}, thread lr chain r1={:08X})",
                     old_count, array - 12, i, item, old_data, old_count, old_capacity,
                     uint32_t(ctx.lr), ctx.r1.u32);
        break;
      }
    }
  }
  // The item stored by the previous append must not lie inside the buffer
  // (an allocator handing out memory twice would put it there).
  if (watched && old_data && old_count) {
    const uint32_t last = REX_LOAD_U32(old_data + (old_count - 1) * 4);
    if (last + 32 > old_data && last < old_data + old_capacity * 4u &&
        append_reports.fetch_add(1) < 6) {
      REXLOG_ERROR("web probe: list {:08X} item {} at {:08X} overlaps its own buffer {:08X}..{:08X}",
                   array - 12, old_count - 1, last, old_data, old_data + old_capacity * 4);
    }
  }
  __imp__sub_82825280(ctx, base);
  if (watched && old_count >= 112 && old_count <= 115) {
    const uint32_t new_data = REX_LOAD_U32(array);
    REXLOG_INFO("web probe: list {:08X} append {} -> slot {:08X}, buffer {:08X} -> {:08X}, "
                "capacity {} -> {}, previous item {:08X}",
                array - 12, old_count, ctx.r3.u32, old_data, new_data, old_capacity,
                REX_LOAD_U16(array + 6),
                old_count ? REX_LOAD_U32(old_data + (old_count - 1) * 4) : 0);
  }
}

// The list's buffer is reserved by sub_828234B8 (r4 slots, via operator new
// sub_821B3510) from sub_82831D88 at 82831E74. Remember it, and report any
// later allocation that overlaps it before it is freed (sub_821B3560): that
// allocation's owner is what overwrites the list's slots.
std::atomic<uint32_t> watched_buffer{0};
std::atomic<uint32_t> watched_size{0};
std::atomic<int> overlap_reports{0};

void ReportPreviousOwners(uint32_t buffer);

// The store watch (gta4_init.h with REX_WEB_WATCH): armed on slot 114 of each
// new 118-slot list buffer, reporting whoever writes it.
extern "C" {
uint32_t rex_web_watch_address = 0;
}
std::atomic<int> watch_hits{0};

extern "C" void rex_web_watch_hit(const char* function, uint32_t lr, uint32_t address,
                                  uint64_t value, uint32_t size) {
  // The list's own code (fill, copy, clear, store) writes the slot all the
  // time; only a write from anywhere else is the culprit.
  static constexpr std::string_view kListCode[] = {
      "__imp__sub_82812D80", "__imp__sub_82813340", "__imp__sub_828234B8",
      "__imp__sub_82831D88", "__imp__sub_828281E8", "__imp__sub_82825280"};
  for (std::string_view own : kListCode) {
    if (own == function) return;
  }
  if (watch_hits.fetch_add(1) < 60) {
    auto* thread = rex::system::XThread::GetCurrentThread();
    REXLOG_ERROR("web watch: {} (lr {:08X}, thread {:08X}) wrote {:X} ({} bytes) at {:08X}",
                 function, lr, thread ? thread->handle() : 0, value, size, address);
  }
}

extern "C" void sub_828234B8(PPCContext& ctx, uint8_t* base) {
  if (!Probing()) {
    __imp__sub_828234B8(ctx, base);
    return;
  }
  const uint32_t caller = uint32_t(ctx.lr);
  const uint32_t slots = ctx.r4.u32;
  __imp__sub_828234B8(ctx, base);
  static std::atomic<int> calls{0};
  if (calls.fetch_add(1) < 5) {
    REXLOG_INFO("web probe: sub_828234B8({}) from {:08X} -> {:08X}", slots, caller, ctx.r3.u32);
  }
  // 828252BC: sub_82825280 growing an array (here by all 118 slots at once).
  if (caller == 0x828252BC && slots == 118) {
    watched_size.store(slots * 4);
    watched_buffer.store(ctx.r3.u32);
    static std::atomic<int> owner_reports{0};
    if (owner_reports.fetch_add(1) < 40) ReportPreviousOwners(ctx.r3.u32);
    rex_web_watch_address = ctx.r3.u32 + 114 * 4;
  }
}

// Recent operator new results, so the probe can name what lived at slot 114's
// address before the list's buffer got that memory.
struct AllocationRecord {
  uint32_t block, size, caller, thread;
  uint64_t sequence;  // shared by allocations and frees
};
struct FreeRecord {
  uint32_t block, caller, thread;
  uint64_t sequence;
};
constexpr size_t kAllocationRing = 1 << 16;
AllocationRecord allocation_ring[kAllocationRing];
std::atomic<uint64_t> allocation_count{0};
FreeRecord free_ring[kAllocationRing];
std::atomic<uint64_t> free_count{0};
std::atomic<uint64_t> event_sequence{0};

// When `block` (allocated at `since`) was freed afterwards, if it was.
const FreeRecord* FindFree(uint32_t block, uint64_t since) {
  const uint64_t end = free_count.load();
  for (uint64_t n = end; n > 0 && end - n < kAllocationRing; n--) {
    const FreeRecord& record = free_ring[(n - 1) % kAllocationRing];
    if (record.sequence < since) break;
    if (record.block == block) return &record;
  }
  return nullptr;
}

void ReportPreviousOwners(uint32_t buffer) {
  const uint32_t target = buffer + 114 * 4;
  const uint64_t end = allocation_count.load();
  int found = 0;
  for (uint64_t n = end; n > 0 && end - n < kAllocationRing && found < 4; n--) {
    const AllocationRecord& record = allocation_ring[(n - 1) % kAllocationRing];
    if (record.block == buffer) continue;
    if (record.block <= target && target < record.block + record.size) {
      const FreeRecord* freed = FindFree(record.block, record.sequence);
      REXLOG_ERROR("web probe: slot 114 address {:08X} was last inside operator new({}) = {:08X} "
                   "from {:08X} on thread {:08X} ({} allocations ago); {}",
                   target, record.size, record.block, record.caller, record.thread, end - n,
                   freed ? fmt::format("freed from {:08X} on thread {:08X}", freed->caller,
                                       freed->thread)
                         : std::string("NOT freed - still live"));
      found++;
    }
  }
  if (!found) REXLOG_ERROR("web probe: no recent operator new covered {:08X}", target);
}

// Called by gta4_audio_lifetime_trace.cpp's operator new / delete wrappers.
extern "C" void rex_web_probe_alloc(uint32_t size, uint32_t caller, uint32_t block) {
  if (!Probing()) return;
  {
    auto* thread = rex::system::XThread::GetCurrentThread();
    const uint64_t n = allocation_count.fetch_add(1);
    allocation_ring[n % kAllocationRing] = {block, size, caller, thread ? thread->handle() : 0,
                                            event_sequence.fetch_add(1)};
  }
  const uint32_t buffer = watched_buffer.load(), buffer_size = watched_size.load();
  if (buffer && block && block != buffer && block < buffer + buffer_size &&
      block + size > buffer && overlap_reports.fetch_add(1) < 8) {
    auto* thread = rex::system::XThread::GetCurrentThread();
    REXLOG_ERROR("web probe: operator new({}) from {:08X} on thread {:08X} returned {:08X}, "
                 "inside the live list buffer {:08X}..{:08X}",
                 size, caller, thread ? thread->handle() : 0, block, buffer, buffer + buffer_size);
  }
}

extern "C" void rex_web_probe_free(uint32_t block, uint32_t caller) {
  if (!Probing()) return;
  if (block) {
    auto* thread = rex::system::XThread::GetCurrentThread();
    const uint64_t n = free_count.fetch_add(1);
    free_ring[n % kAllocationRing] = {block, caller, thread ? thread->handle() : 0,
                                      event_sequence.fetch_add(1)};
  }
  if (block && block == watched_buffer.load()) {
    watched_buffer.store(0);
    rex_web_watch_address = 0;
  }
}

extern "C" void sub_82831920(PPCContext& ctx, uint8_t* base) {
  if (!Probing()) {
    __imp__sub_82831920(ctx, base);
    return;
  }
  const uint32_t object = ctx.r3.u32;
  const uint32_t data = REX_LOAD_U32(object + 12);
  const uint32_t count = REX_LOAD_U16(object + 16);
  const uint32_t capacity = REX_LOAD_U16(object + 18);
  int bad = -1;
  for (uint32_t i = 0; data && i < count && i < 4096; i++) {
    if (!PlausibleObject(REX_LOAD_U32(data + i * 4))) {
      bad = int(i);
      break;
    }
  }
  if (bad >= 0 && reports.fetch_add(1) < 4) {
    std::string items;
    for (uint32_t i = 0; i < count && i < 24; i++) {
      items += fmt::format(" {:08X}", REX_LOAD_U32(data + i * 4));
    }
    std::string header;
    for (uint32_t i = 0; i < 32; i += 4) {
      header += fmt::format(" {:08X}", REX_LOAD_U32(object + i));
    }
    // The caller fills the array from the object's virtual method 1 (its item
    // factory); log the vtable's first methods to name it.
    const uint32_t vtable = REX_LOAD_U32(object);
    std::string methods;
    for (uint32_t i = 0; i < 4; i++) {
      methods += fmt::format(" {:08X}", REX_LOAD_U32(vtable + i * 4));
    }
    REXLOG_ERROR("web probe: sub_82831920 object {:08X} (caller {:08X}) array {:08X} count {} "
                 "capacity {}: item {} is bad; object:{} vtable {:08X}:{} items:{}",
                 object, uint32_t(ctx.lr), data, count, capacity, bad, header, vtable, methods,
                 items);
  }
  __imp__sub_82831920(ctx, base);
}

// The main thread's drain loop (sub_826799E0) waits until a queue of up to
// eight requests empties (pending count at +12604), polling the oldest
// request's handle (entry +12) with a zero-timeout wait (sub_82849790 from
// 82679A28) and sleeping 10 ms between polls. Every 5 s of polling, log the
// handle, its kernel object type and state, and the queue.
extern "C" void sub_82849790(PPCContext& ctx, uint8_t* base) {
  const uint32_t handle = ctx.r3.u32;
  const uint32_t caller = uint32_t(ctx.lr);
  __imp__sub_82849790(ctx, base);
  if (!Probing() || caller != 0x82679A28) return;
  static std::atomic<uint64_t> polls{0};
  if (polls.fetch_add(1) % 500 != 0) return;  // every ~5 s at 10 ms per poll
  std::string description = "unknown handle";
  if (auto* kernel_state = rex::system::KernelState::shared()) {
    auto object = kernel_state->object_table()->LookupObject<rex::system::XObject>(handle);
    if (object) {
      description = fmt::format("type {} '{}'", uint32_t(object->type()), object->name());
      if (object->type() == rex::system::XObject::Type::Event) {
        uint32_t type = 0, state = 0;
        static_cast<rex::system::XEvent*>(object.get())->Query(&type, &state);
        description += fmt::format(" event type {} state {}", type, state);
      } else if (object->type() == rex::system::XObject::Type::Thread) {
        description += fmt::format(" thread running {}",
                                   static_cast<rex::system::XThread*>(object.get())->is_running());
      }
    }
  }
  REXLOG_ERROR("web probe: drain loop polls handle {:08X} ({}) -> signaled {}", handle,
               description, ctx.r3.u32 & 0xFF);
}
