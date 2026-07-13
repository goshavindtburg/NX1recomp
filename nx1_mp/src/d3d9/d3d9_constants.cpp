/**
 * @file    d3d9_constants.cpp
 * @brief   See d3d9_constants.h for why this exists.
 */

#include "d3d9_constants.h"

#include <atomic>

namespace nx1::d3d9 {

namespace {

/// The main device plus dx.cmdBufDevice[40]; rounded up to a power of two so the probe can
/// mask instead of divide.
constexpr uint32_t kMaxDevices = 64;

struct Slot {
  std::atomic<uint32_t> device{0};
  ConstantRing ring;
};

Slot g_slots[kMaxDevices];

}  // namespace

ConstantRing& ConstantRing::For(uint32_t guest_device) {
  // Device objects are heap allocations, so the low nibble carries no entropy.
  uint32_t probe = (guest_device >> 4) & (kMaxDevices - 1);
  for (uint32_t i = 0; i < kMaxDevices; ++i, probe = (probe + 1) & (kMaxDevices - 1)) {
    Slot& slot = g_slots[probe];
    uint32_t owner = slot.device.load(std::memory_order_acquire);
    if (owner == guest_device) {
      return slot.ring;
    }
    if (owner == 0 &&
        (slot.device.compare_exchange_strong(owner, guest_device, std::memory_order_acq_rel) ||
         owner == guest_device)) {
      return slot.ring;
    }
  }
  // More devices than we budgeted for. Falling back to a shared table is wrong, but it is
  // the same kind of wrong we had before, and it keeps the renderer alive.
  static ConstantRing overflow;
  return overflow;
}

namespace {
std::atomic<bool> g_ever_recorded{false};  // DIAG(d3d9): see EverRecorded. TODO: drop.
}  // namespace

bool ConstantRing::EverRecorded() { return g_ever_recorded.load(std::memory_order_relaxed); }

void ConstantRing::Record(bool pixel_stage, uint32_t start_register, uint32_t count,
                          uint32_t ring_addr) {
  if (!ring_addr || start_register >= kAluRegisters) {
    return;
  }
  g_ever_recorded.store(true, std::memory_order_relaxed);
  uint32_t* addr = addr_[pixel_stage ? 1 : 0];
  const uint32_t end = start_register + count < kAluRegisters ? start_register + count
                                                              : kAluRegisters;
  for (uint32_t r = start_register; r < end; ++r) {
    addr[r] = ring_addr + (r - start_register) * 16;
  }
}

void ConstantRing::Retire(bool pixel_stage, uint64_t mask) {
  if (!mask) {
    return;
  }
  uint32_t* addr = addr_[pixel_stage ? 1 : 0];
  // One bit per group of 4 registers, numbered from the MSB: register r is bit
  // 63 - (r >> 2). See D3DTag_ShaderConstantMask (guest 0x824D0B90).
  for (uint32_t group = 0; group < kAluRegisters / 4; ++group) {
    if (mask & (uint64_t(1) << (63 - group))) {
      const uint32_t base = group * 4;
      addr[base + 0] = 0;
      addr[base + 1] = 0;
      addr[base + 2] = 0;
      addr[base + 3] = 0;
    }
  }
}

}  // namespace nx1::d3d9
