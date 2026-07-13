/**
 * @file    d3d9_constants.h
 * @brief   Live ALU constant file: reconciling the guest's TWO constant producers.
 *
 * The premise the rest of this renderer rests on -- "the guest setters only write shadow
 * state, so reading D3D::CDevice at draw time reconstructs the GPU" -- is true for
 * everything except float shader constants, where the guest has two producers:
 *
 *   1. D3DDevice_SetVertexShaderConstantF / SetPixelShaderConstantF
 *        -> writes m_Constants.Alu (the shadow at device+0x780 / +0x1780) and tags a
 *           dirty bit. The next draw flushes the tagged groups to the GPU.
 *
 *   2. D3DDevice_GpuBeginShaderConstantF4  (guest 0x820EAEB8)
 *        -> writes a PM4 SET_CONSTANT packet *straight into the command ring* and hands
 *           the engine a raw ring pointer to fill. The data NEVER reaches the shadow, and
 *           the caller *clears* that group's dirty bit (an inlined GpuOwnVertexShaderConstantF)
 *           precisely so the draw-time flush cannot overwrite what it wrote.
 *
 * Producer 2 carries **VS c4..c7 -- the per-instance object->world matrix** (and c8..c11
 * for the lit/ambient paths) on every R_DrawXModel*NoPrepass and
 * R_DrawStaticModelArray*NoPrepass. Reading those registers from the shadow yields
 * whatever an unrelated earlier draw happened to leave there, so every model gets a
 * garbage transform -- geometry smeared out of a vanishing point -- while UI and other
 * shadow-path draws render perfectly.
 *
 * Arbitration is per *group of 4 registers*, because that is the dirty-bit granularity
 * (register r lives in bit 63 - (r >> 2) of m_Pending.m_Mask[0] for VS, [1] for PS):
 *
 *     dirty bit SET   -> the guest is about to flush the shadow over this group, and that
 *                        flush is emitted *after* the SET_CONSTANT packet. Shadow wins;
 *                        drop any ring record.
 *     dirty bit CLEAR -> whatever a GpuBegin put in the ring is what the GPU holds.
 *
 * The mask MUST be sampled before the guest's draw body runs, because its flush zeroes it.
 * The draw hooks call Retire() ahead of __imp__ for exactly that reason.
 *
 * There is one table per *guest device*, not one globally. NX1's worker threads each record
 * into their own D3DDevice out of dx.cmdBufDevice[40] (see R_BeginCommandBuffer), each with
 * its own ring; a single global table would have them overwriting each other's records. The
 * device EA is the identity, and the guest only ever lets one thread record into a device at
 * a time, so the per-device table needs no lock of its own.
 *
 * Note we deliberately do NOT redirect the returned ring pointer into the shadow. The
 * engine's R_IsVertexShaderConstantUpToDate redundancy cache assumes the shadow still
 * holds what R_SetVertexShaderConstantFromCode last put in c4..c7, and relies on the
 * matching GpuDisown to re-dirty the group so D3D re-flushes that shadow value after the
 * model loop. Clobbering the shadow behind its back corrupts the next draw that "knows"
 * those registers are already current.
 */

#pragma once

#include <cstdint>

namespace nx1::d3d9 {

/// Xenos gives each stage 256 float4 ALU registers.
inline constexpr uint32_t kAluRegisters = 256;

class ConstantRing {
 public:
  /// The table owned by one guest D3DDevice. The reference is stable for the process
  /// lifetime; the slot is claimed on first use.
  static ConstantRing& For(uint32_t guest_device);

  ConstantRing() = default;

  /// Record a D3DDevice_GpuBeginShaderConstantF4. `ring_addr` is the guest EA the guest
  /// returned -- it points at the float4 for `start_register`, with the rest following.
  void Record(bool pixel_stage, uint32_t start_register, uint32_t count, uint32_t ring_addr);

  /// Apply a pre-flush dirty mask (m_Pending.m_Mask[0] for VS, [1] for PS). Every group
  /// the guest is about to flush from the shadow drops its ring record.
  void Retire(bool pixel_stage, uint64_t mask);

  /// DIAG(d3d9): true once any device has taken a ring record, i.e. we are in-game and
  /// past the menu. Lets a diagnostic skip the menu draws. TODO(d3d9): drop.
  static bool EverRecorded();

  /// Guest EA holding register `reg`, or 0 when the shadow is authoritative.
  uint32_t Lookup(bool pixel_stage, uint32_t reg) const {
    return reg < kAluRegisters ? addr_[pixel_stage ? 1 : 0][reg] : 0;
  }

  /// Guest EA to read register `reg` from: the ring if it owns it, else the shadow.
  uint32_t Resolve(bool pixel_stage, uint32_t reg, uint32_t shadow_base) const {
    const uint32_t ring = Lookup(pixel_stage, reg);
    return ring ? ring : shadow_base + reg * 16;
  }

 private:
  ConstantRing(const ConstantRing&) = delete;
  ConstantRing& operator=(const ConstantRing&) = delete;

  /// Per stage, per register: the guest EA of that register's float4 inside the ring, or
  /// 0 if the shadow owns it. Only ever touched by the thread currently recording into the
  /// owning device, so a plain array is sufficient.
  uint32_t addr_[2][kAluRegisters] = {};
};

}  // namespace nx1::d3d9
