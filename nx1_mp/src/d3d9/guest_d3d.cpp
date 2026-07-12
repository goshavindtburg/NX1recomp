/**
 * @file    guest_d3d.cpp
 * @brief   Out-of-line guest memory helpers for the D3D device reader.
 */

// This TU deliberately does NOT include <windows.h>/<d3d9.h>: it exists so the
// physical-mirror translation can reach the kernel memory system (which pulls in
// std::min) without the <windows.h> min/max macros clobbering it in the headers
// that include guest_d3d.h after d3d9.h.
#include <rex/system/kernel_state.h>

#include "guest_d3d.h"

namespace nx1::d3d9 {

const uint8_t* GuestTranslatePhysical(uint32_t guest_addr) {
  // Use TranslateVirtual, not TranslatePhysical: the guest's D3D device sits in the
  // 0xE0 uncached physical-mirror window, and on Windows that heap is file-mapped at
  // a 0x1000 offset (see rex::memory::detail::PhysicalHostOffset). TranslatePhysical
  // masks & 0x1FFFFFFF and misses that offset -- reading a page early (all zeroes).
  // TranslateVirtual folds in the heap's host_address_offset, which is exactly the
  // translation the recompiled guest uses to read the same object.
  return rex::system::kernel_state()->memory()->TranslateVirtual<const uint8_t*>(guest_addr);
}

}  // namespace nx1::d3d9
