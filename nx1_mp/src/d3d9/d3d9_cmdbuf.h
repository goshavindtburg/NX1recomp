/**
 * @file    d3d9_cmdbuf.h
 * @brief   Recorded draws, so translation can move off the guest thread.
 *
 * WHY THIS EXISTS
 *
 * The guest thread currently does everything serially: ~14 ms of our translation, then ~9.7 ms
 * of its own game logic. Measured (PROF/defer), the guest's logic is INTERLEAVED with its
 * draws -- 5.3-7.5 ms of it lands between draw calls -- so a worker doing the translation
 * concurrently turns the frame into max(ours, theirs) rather than the sum. That is ~24 ms ->
 * ~14-15 ms.
 *
 * WHAT IS CAPTURED, AND WHY SO LITTLE
 *
 * The same measurement showed draw-referenced VERTEX memory is not rewritten within a frame
 * (99.8-100% of 512 probed ranges unchanged at Present). That is what makes this affordable:
 * the worker reads vertex and index data straight out of guest memory, so we do not snapshot
 * megabytes. We capture only the state the guest REWRITES between draws:
 *
 *   - draw parameters
 *   - the 32 fetch constants (textures + the vertex streams aliased at slots 94/95)
 *   - viewport and the GPU register shadows for depth/blend/cull/alpha
 *   - the bound shader object addresses
 *   - a REFERENCE to a constant block, versioned rather than copied per draw: the guest's own
 *     dirty mask says only ~6% of draws rewrite VS constants and ~22% PS, so a block is
 *     duplicated only when it actually changes
 *
 * That is ~500 bytes per draw, ~2.6 MB/frame at 5000 draws -- about 0.26 ms of memcpy on the
 * guest thread, against the ~12 ms it lets us move off it.
 *
 * WHAT IS NOT SAFE TO DEFER
 *
 * Anything the guest may rewrite before the worker consumes it. Vertex/index bulk data is
 * proven stable within a frame; constants are NOT (hence the versioned blocks); fetch
 * constants are NOT (hence copying all 32). One probe in 448 did show a vertex range changing,
 * so the executor must tolerate that case rather than assume it away -- see kValidateRanges.
 *
 * STAGING
 *
 * Step 1 (this file's initial use) records and replays SYNCHRONOUSLY on the guest thread. That
 * buys no speed at all; it exists so that any rendering difference is a recording-completeness
 * bug and nothing else, found without threading in the picture. Only once that is clean does
 * execution move to a worker, behind a cvar.
 */

#pragma once

#include <cstdint>

#ifdef _WIN32
#include <vector>

#include "d3d9_resources.h"  // kMaxHostStreams
#include "guest_d3d.h"

namespace nx1::d3d9 {

/// A constant-file DELTA: only the register groups the guest actually rewrote, as raw guest
/// bytes.
///
/// Two earlier versions were both too expensive, and the reasons are worth keeping:
///
///   1. Decoded floats -- 512 registers x 4 components of GuestReadF32 per block, ~1550 blocks
///      a frame = 3.2M swapped reads = 22.5 ms on the guest thread.
///   2. Raw memcpy of the whole 8 KB file -- direct cost fell to 1.4 ms, but at ~1450 blocks
///      that is 14 MB/frame written through the cache. The frame got ~4-5 ms WORSE while the
///      record timer showed only 1.67 ms: the eviction of the texture and vertex working sets
///      cost more than the copy itself. Payload size matters more than payload cost.
///
/// The guest's dirty mask carries one bit per group of 4 registers, and a draw that touches
/// constants typically touches a handful of groups -- so a delta is a few hundred bytes rather
/// than 8 KB. The executor keeps a running constant file and applies deltas in order, which is
/// exactly what the guest's own shadow does.
struct ConstantDelta {
  /// One bit per group of 4 registers, numbered from the MSB as the guest numbers them:
  /// register r is bit 63 - (r >> 2). See D3DTag_ShaderConstantMask.
  uint64_t vs_mask = 0;
  uint64_t ps_mask = 0;
  /// Offset into the frame's delta byte pool, and how many bytes: the set groups' registers,
  /// vertex stage first, 64 bytes per group, raw big-endian.
  uint32_t offset = 0;
  uint32_t bytes = 0;
};

/// Everything a draw needs that the guest may overwrite before the worker gets to it.
struct RecordedDraw {
  // --- draw parameters -----------------------------------------------------------------
  uint32_t prim_type = 0;
  uint32_t base_vertex_index = 0;
  uint32_t start_index = 0;
  uint32_t index_count = 0;
  /// Non-indexed draws set this and leave index_count 0.
  uint32_t start_vertex = 0;
  uint32_t vertex_count = 0;
  bool indexed = true;

  // --- resource bindings ---------------------------------------------------------------
  /// Raw fetch constants, copied verbatim: 32 slots x 6 dwords. Textures occupy 0..15 and the
  /// vertex streams alias 94/95, which is why the whole array comes along rather than the
  /// declared slots -- knowing which slots a shader declares needs the shader resolved, and
  /// that work belongs on the worker.
  /// RAW guest bytes, big-endian, exactly as the guest stores them -- decode with
  /// ReadTextureFetchConstantAt-style reads on the worker. Not decoded at record time: see the
  /// note in ConstantBlock about what byte-swapping during capture costs.
  uint32_t fetch_constants[guest_device::kFetchConstantCount][6] = {};

  /// The raw bytes of one texture slot's fetch constant (24 B), ready for
  /// DecodeTextureFetchConstant.
  const uint8_t* texture_fetch(uint32_t sampler) const {
    return reinterpret_cast<const uint8_t*>(fetch_constants) +
           size_t(sampler) * guest_device::kFetchConstantStride;
  }
  /// The raw bytes of one vertex stream's fetch constant (8 B), ready for
  /// DecodeVertexFetchConstant. The vertex fetch constants are 2-dword slots ALIASED onto the
  /// tail of the same array -- stream 0 is slot 95, i.e. bytes 760..767 of the 768 -- which is
  /// why the recorder copies all 32 texture slots rather than just the 16 that are textures.
  const uint8_t* vertex_fetch(uint32_t stream) const {
    return reinterpret_cast<const uint8_t*>(fetch_constants) +
           size_t(VertexFetchSlotForStream(stream)) * 8;
  }
  /// Guest addresses of the bound shader objects. The worker hashes the microcode and looks up
  /// the SM3 translation; the objects themselves are stable for the frame.
  uint32_t vs_object = 0;
  uint32_t ps_object = 0;
  uint32_t vs_pass = 0;
  /// Guest D3DIndexBuffer address, and the declaration the streams were bound through.
  uint32_t index_buffer = 0;
  uint32_t vertex_declaration = 0;
  /// m_StreamStride[] for the streams we support, packed one byte per stream as the guest
  /// stores it.
  uint32_t stream_stride[kMaxHostStreams] = {};

  // --- pipeline state ------------------------------------------------------------------
  ViewportState viewport{};
  DepthState depth{};
  BlendState blend{};
  CullState cull{};
  AlphaTestState alpha{};
  uint32_t color_write_mask = 0;

  // --- constants -----------------------------------------------------------------------
  /// Index into the frame's delta list. A draw that changed no constants points at the same
  /// delta as the draw before it, so the executor simply does not re-apply anything.
  uint32_t constant_delta = 0;

  /// Stable per-surface identity for the prefer-largest LOD substitution, computed at record
  /// time because it derives from the index range.
  uint64_t surface_key = 0;
};

/// A frame's worth of recorded draws plus the constant blocks they reference.
///
/// Deliberately owns its storage and is reused frame to frame: at 5000 draws this is a few MB,
/// and reallocating it every frame would put the cost back on the thread we are trying to
/// unload.
class CommandBuffer {
 public:
  void BeginFrame() {
    draws_.clear();
    deltas_.clear();
    delta_pool_.clear();
    // The executor's running constant file carries across frames exactly as the guest's shadow
    // does, so a frame does not need to open with a full copy.
  }

  RecordedDraw& AddDraw() {
    draws_.emplace_back();
    return draws_.back();
  }

  /// Append a delta for whatever the guest just rewrote, or reuse the previous one when the
  /// masks are clear. `vs_mask`/`ps_mask` come from the draw hook, captured before the guest's
  /// body flushed and zeroed them.
  uint32_t RecordConstantDelta(const uint8_t* base, uint32_t guest_device, uint64_t vs_mask,
                               uint64_t ps_mask);

  const std::vector<RecordedDraw>& draws() const { return draws_; }
  const ConstantDelta& delta(uint32_t index) const { return deltas_[index]; }
  const uint8_t* delta_bytes(const ConstantDelta& d) const { return delta_pool_.data() + d.offset; }
  size_t delta_count() const { return deltas_.size(); }

  /// Bytes captured this frame, for the profiler -- recording cost is the price of the whole
  /// scheme and needs to stay visible.
  size_t captured_bytes() const {
    return draws_.size() * sizeof(RecordedDraw) + deltas_.size() * sizeof(ConstantDelta) +
           delta_pool_.size();
  }

 private:
  std::vector<RecordedDraw> draws_;
  std::vector<ConstantDelta> deltas_;
  std::vector<uint8_t> delta_pool_;
};

}  // namespace nx1::d3d9

#endif  // _WIN32
