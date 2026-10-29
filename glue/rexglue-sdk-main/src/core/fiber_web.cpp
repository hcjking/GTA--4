/**
 * @file        core/fiber_web.cpp
 * @brief       [web port] Fibers for WebAssembly
 *
 * WebAssembly cannot switch stacks: there is no getcontext/swapcontext, and
 * Emscripten's own fibers need Asyncify, which would rewrite the whole
 * recompiled game. GTA IV never creates a fiber (its generated code imports
 * no fiber function), so only the thread fiber exists: converting a thread
 * works, creating a second fiber fails.
 *
 * @license     BSD 3-Clause License
 */

#include <rex/logging.h>
#include <rex/thread/fiber.h>

namespace rex::thread {

thread_local Fiber* Fiber::tls_current_ = nullptr;

Fiber* Fiber::ConvertCurrentThread() {
  auto* f = new Fiber();
  f->is_thread_fiber_ = true;
  tls_current_ = f;
  return f;
}

Fiber* Fiber::Create(size_t stack_size, void (*entry)(void*), void* arg) {
  (void)stack_size;
  (void)entry;
  (void)arg;
  REXLOG_ERROR("Fiber::Create: fibers are not supported on the web");
  return nullptr;
}

/*static*/ void Fiber::Trampoline() {}

void Fiber::SwitchTo(Fiber* target) {
  // Only the thread fiber exists, so switching can only be to the current one.
  if (target != tls_current_) {
    REXLOG_ERROR("Fiber::SwitchTo: fibers are not supported on the web");
  }
}

void Fiber::Destroy() {
  if (is_thread_fiber_) {
    tls_current_ = nullptr;
  }
  delete this;
}

}  // namespace rex::thread
