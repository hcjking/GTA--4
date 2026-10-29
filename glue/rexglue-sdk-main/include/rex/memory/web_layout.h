/**
 * @file        memory/web_layout.h
 * @brief       [web port] Guest memory layout for WebAssembly
 *
 * WebAssembly has one linear memory and no way to map the same pages at two
 * addresses, so the Xbox 360's mirrored views cannot be made with mmap. The
 * guest instead lives in one 4.5 GiB backing block laid out like the
 * desktop mapping file (map_info in xmemory.cpp): virtual memory at its own
 * address, physical memory at 0x100000000. Accesses to the three physical
 * windows are redirected in software, by the generated code's
 * REX_PHYS_HOST_OFFSET and by Memory::TranslateVirtual:
 *
 *   0xA0000000-0xBFFFFFFF  physical 64 KiB pages  -> 0x100000000 + (addr & 0x1FFFFFFF)
 *   0xC0000000-0xDFFFFFFF  physical 16 MiB pages  -> 0x100000000 + (addr & 0x1FFFFFFF)
 *   0xE0000000-0xFFFFFFFF  physical 4 KiB pages   -> 0x100001000 + (addr & 0x1FFFFFFF)
 *
 * The other two desktop aliases are not reproduced: 0x7F000000 (GPU
 * writeback) and 0x90000000 (xex 4 KiB pages) get memory of their own.
 *
 * @license     BSD 3-Clause License
 */

#pragma once

#include <cstdint>

namespace rex::memory::web {

// Size of the backing block: virtual space plus 512 MiB of physical memory.
inline constexpr uint64_t kBackingSize = 0x120000000ull;
inline constexpr uint64_t kPhysicalBase = 0x100000000ull;

// Host offset added to a guest virtual address, indexed by its top 3 bits.
inline constexpr uint64_t kHostOffset[8] = {
    0, 0, 0, 0, 0,
    kPhysicalBase - 0xA0000000ull,
    kPhysicalBase - 0xC0000000ull,
    kPhysicalBase + 0x1000ull - 0xE0000000ull,
};

inline constexpr uint64_t HostOffset(uint32_t guest_address) {
  return kHostOffset[guest_address >> 29];
}

// A host pointer into physical memory does not say which of the three
// windows the guest used for it, but native code handed a guest pointer (a
// hook's arguments) mostly returns a pointer into the same window: memcpy
// returns its destination. So each thread remembers the window of the last
// physical-window pointer it translated, and converting back uses it.
// Answering with a fixed window instead hands the game an alias of its own
// buffer (0xBD.. for an 0xDD.. allocation), which its allocator then cannot
// recognise when it is freed.
inline thread_local uint32_t last_physical_window = 0xA0000000u;

inline void NoteGuestAddress(uint32_t guest_address) {
  if (guest_address >= 0xA0000000u) {
    last_physical_window = guest_address & 0xE0000000u;
  }
}

// Guest address for an offset (host pointer minus the base) at or above
// kPhysicalBase, in the thread's last physical window.
inline uint32_t GuestFromPhysicalOffset(uint64_t offset) {
  const uint32_t window = last_physical_window;
  const uint64_t skew = window == 0xE0000000u ? 0x1000u : 0u;
  return window | uint32_t((offset - kPhysicalBase - skew) & 0x1FFFFFFFu);
}

}  // namespace rex::memory::web
