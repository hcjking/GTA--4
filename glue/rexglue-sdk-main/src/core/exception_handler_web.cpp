/**
 * @file        core/exception_handler_web.cpp
 * @brief       [web port] Exception handler for WebAssembly
 *
 * WebAssembly cannot catch a faulting memory access: an out-of-bounds access
 * traps and ends the program, and in-bounds accesses never fault because
 * pages cannot be protected. Handlers are therefore accepted and never
 * called. MMIO does not need them (the generated code calls
 * MMIOHandler::CheckLoad/CheckStore itself); write watches simply never fire.
 *
 * @license     BSD 3-Clause License
 */

#include <rex/exception_handler.h>

namespace rex::arch {

void ExceptionHandler::Install(Handler fn, void* data) {
  (void)fn;
  (void)data;
}

void ExceptionHandler::Uninstall(Handler fn, void* data) {
  (void)fn;
  (void)data;
}

}  // namespace rex::arch
