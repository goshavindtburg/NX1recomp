#include "d3d9_cmdbuf.h"

#ifdef _WIN32

#include <cstring>

namespace nx1::d3d9 {

namespace {

/// Copy the register groups `mask` marks dirty out of one stage's constant file, raw.
/// Returns bytes appended. Group g covers registers [g*4, g*4+4), i.e. 64 bytes, and is
/// numbered from the MSB the way the guest numbers it.
size_t AppendDirtyGroups(std::vector<uint8_t>& pool, const uint8_t* stage_base, uint64_t mask) {
  size_t appended = 0;
  for (uint32_t group = 0; group < guest_device::kAluConstantCount / 4; ++group) {
    if (!(mask & (uint64_t(1) << (63 - group)))) {
      continue;
    }
    const uint8_t* src = stage_base + size_t(group) * 4 * 16;
    pool.insert(pool.end(), src, src + 4 * 16);
    appended += 4 * 16;
  }
  return appended;
}

}  // namespace

uint32_t CommandBuffer::RecordConstantDelta(const uint8_t* base, uint32_t guest_device,
                                            uint64_t vs_mask, uint64_t ps_mask) {
  // Nothing rewritten: point at the previous delta. The executor applies a delta only when the
  // index changes, so a run of draws sharing constants costs nothing at all.
  if (!vs_mask && !ps_mask && !deltas_.empty()) {
    return uint32_t(deltas_.size() - 1);
  }

  deltas_.emplace_back();
  ConstantDelta& d = deltas_.back();
  d.vs_mask = vs_mask;
  d.ps_mask = ps_mask;
  d.offset = uint32_t(delta_pool_.size());

  // Raw bytes, in guest order, vertex stage first. No byte-swapping here on purpose: swapping
  // during capture is what made the first version cost 22.5 ms/frame, and it is work the
  // worker can do just as well.
  // GuestPointer, NOT base + guest_device + offset -- the device is a physical-mirror EA. The
  // same mistake in the fetch-constant copy rendered the entire game black; it stayed invisible
  // until something actually read the captured bytes, so it is fixed here BEFORE the executor
  // starts consuming these deltas rather than after it goes black too.
  size_t bytes = 0;
  if (vs_mask) {
    bytes += AppendDirtyGroups(
        delta_pool_, GuestPointer(base, guest_device + guest_device::kVsConstants), vs_mask);
  }
  if (ps_mask) {
    bytes += AppendDirtyGroups(
        delta_pool_, GuestPointer(base, guest_device + guest_device::kPsConstants), ps_mask);
  }
  d.bytes = uint32_t(bytes);
  return uint32_t(deltas_.size() - 1);
}

}  // namespace nx1::d3d9

#endif  // _WIN32
