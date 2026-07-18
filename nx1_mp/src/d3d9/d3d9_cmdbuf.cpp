#include "d3d9_cmdbuf.h"

#ifdef _WIN32

#include <cstring>

namespace nx1::d3d9 {

uint32_t CommandBuffer::CurrentConstantBlock(const uint8_t* base, uint32_t guest_device,
                                             bool constants_dirty) {
  // Reuse the last block whenever the guest has not rewritten the constant file. The dirty
  // mask says that is ~94% of draws for the vertex stage and ~78% for the pixel stage, so the
  // common case costs an index and no copying at all. Correctness does not rest on the mask
  // being a perfect change oracle: a block is shared only while the mask reports nothing
  // written, and any write forces a fresh copy before the next draw references it.
  if (!constants_dirty && !dirty_block_ && !blocks_.empty()) {
    return uint32_t(blocks_.size() - 1);
  }

  blocks_.emplace_back();
  ConstantBlock& out = blocks_.back();
  dirty_block_ = false;

  // Copy both stages in one pass, byte-swapping as we go. The guest lays the two stages out
  // contiguously (VS c0..c255 then PS c0..c255), and RecordedDraw refers to registers by the
  // same indices, so downstream code needs no remapping.
  const uint32_t vs_base = guest_device + guest_device::kVsConstants;
  for (uint32_t r = 0; r < guest_device::kAluConstantCount * 2; ++r) {
    const uint32_t addr = vs_base + r * 16;
    for (uint32_t c = 0; c < 4; ++c) {
      out.regs[r][c] = GuestReadF32(base, addr + c * 4);
    }
  }
  return uint32_t(blocks_.size() - 1);
}

}  // namespace nx1::d3d9

#endif  // _WIN32
