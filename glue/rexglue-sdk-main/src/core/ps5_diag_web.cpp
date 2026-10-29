/**
 * @file        core/ps5_diag_web.cpp
 * @brief       [web port] Progress counters and kernel-wait slots for the web
 *
 * Shared runtime code bumps rex::ps5diag counters unconditionally, and the
 * web build records each guest thread's kernel wait in a slot as the PS5
 * build does (xobject.cpp). The PS5's ps5_diag.cpp (sampling profiler, crash
 * hooks) needs FreeBSD headers, so only these parts are here; the monitor
 * that reports long waits is rex_web_start_wait_monitor.
 *
 * @license     BSD 3-Clause License
 */

#include <rex/ps5_diag.h>

#include <chrono>
#include <map>
#include <thread>

#include <emscripten/html5.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/thread_state.h>
#include <rex/system/xthread.h>

namespace rex::ps5diag {

std::atomic<uint64_t> counters[kCount];
WaitSlot wait_slots[kWaitSlots];

namespace {
std::atomic<int> next_slot{0};
}

WaitSlot* CurrentWaitSlot() {
  thread_local int index = -1;
  if (index < 0) index = next_slot.fetch_add(1);
  return index < kWaitSlots ? &wait_slots[index] : nullptr;
}

uint64_t NowMs() {
  // Wall-clock time, the same on every worker (each worker's monotonic clock
  // counts from its own start, so stamps from different threads disagree).
  return uint64_t(emscripten_date_now());
}

const char* ObjectTypeName(uint32_t type) {
  static const char* const kNames[] = {"Undefined", "Enumerator", "Event",     "File",
                                       "IOCompletion", "Module",  "Mutant",    "NotifyListener",
                                       "Semaphore", "Session",    "Socket",    "SymbolicLink",
                                       "Thread",    "Timer"};
  return type < sizeof(kNames) / sizeof(kNames[0]) ? kNames[type] : "?";
}

}  // namespace rex::ps5diag

namespace {

// Each guest thread's link register (the return address of its last call),
// with "(same)" when it has not moved since the last sample: a thread
// spinning in game code keeps calling from the same place.
void SampleGuestThreads() {
  auto* kernel_state = rex::system::KernelState::shared();
  if (!kernel_state) return;
  static std::map<uint32_t, uint32_t> last_lr;
  auto threads = kernel_state->object_table()->GetObjectsByType<rex::system::XThread>();
  std::string line;
  for (auto& thread : threads) {
    auto* state = thread->thread_state();
    if (!state || !state->context()) continue;
    const uint32_t lr = uint32_t(state->context()->lr);
    const bool same = last_lr.count(thread->handle()) && last_lr[thread->handle()] == lr;
    last_lr[thread->handle()] = lr;
    line += fmt::format(" {:08X}:lr={:08X}{}", thread->handle(), lr, same ? "(same)" : "");
    // A stuck thread's stack frame often holds the name it is working on
    // (r1+80 is GTA IV's archive lookup buffer): show it when it is text.
    if (same) {
      const uint32_t sp = state->context()->r1.u32;
      auto* text = kernel_state->memory()->TranslateVirtual<const char*>(sp + 80);
      std::string name;
      for (int i = 0; i < 96 && text[i]; i++) {
        if (text[i] < 32 || text[i] > 126) { name.clear(); break; }
        name += text[i];
      }
      line += fmt::format("[sp={:08X}{}", sp, name.size() > 3 ? " '" + name + "'" : "");
      // Callers: each frame's back chain is at 0(r1) and the caller's saved
      // link register 8 bytes below the frame it points to (GTA IV's prologues).
      auto* memory = kernel_state->memory();
      uint32_t frame = sp;
      for (int depth = 0; depth < 8 && frame >= 0x70000000 && frame < 0x80000000; depth++) {
        const uint32_t back = rex::byte_swap(*memory->TranslateVirtual<const uint32_t*>(frame));
        if (back <= frame || back >= 0x80000000) break;
        const uint32_t saved_lr = rex::byte_swap(*memory->TranslateVirtual<const uint32_t*>(back - 8));
        line += fmt::format(" <{:08X}", saved_lr);
        frame = back;
      }
      line += "]";
    }
  }
  REXLOG_INFO("web threads:{}", line);
}

}  // namespace

// [web port] Every 10 s, lists guest threads that have been in one kernel
// wait for more than 5 s: which thread, on what, and the game code (link
// register) that waited.
extern "C" void rex_web_start_wait_monitor() {
  using namespace rex::ps5diag;
  std::thread([] {
    for (;;) {
      std::this_thread::sleep_for(std::chrono::seconds(10));
      const uint64_t now = NowMs();
      int reported = 0;
      for (int i = 0; i < kWaitSlots && i < next_slot.load(); i++) {
        const WaitSlot& slot = wait_slots[i];
        const uint64_t since = slot.since_ms.load(std::memory_order_acquire);
        if (!since || now - since < 5000) continue;
        REXLOG_INFO("web wait: thread {:08X} has waited {} s on {} {} handle {:08X}{} from game "
                    "code {:08X}{}",
                    slot.thread_id, (now - since) / 1000, slot.count > 1 ? slot.count : 1,
                    ObjectTypeName(slot.object_type), slot.handle,
                    slot.timeout_ms == 0xFFFFFFFFu ? " (no timeout)" : "", slot.guest_lr,
                    slot.critical_section ? " in a critical section" : "");
        reported++;
      }
      if (!reported) REXLOG_INFO("web wait: no guest thread is in a long kernel wait");
      SampleGuestThreads();
    }
  }).detach();
}
