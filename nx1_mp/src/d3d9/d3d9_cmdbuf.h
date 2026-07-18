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

/// One captured shader-constant file. Duplicated only when the guest's dirty mask says the
/// contents changed, so consecutive draws that share constants share a block.
struct ConstantBlock {
  /// Both stages, laid out exactly as the guest's ALU constant file so existing readers can be
  /// pointed at this instead of guest memory: [0..255] vertex, [256..511] pixel.
  float regs[512][4];
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
  uint32_t fetch_constants[guest_device::kFetchConstantCount][6] = {};
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
  /// Index into the frame's constant-block pool. Draws that changed nothing share one.
  uint32_t constant_block = 0;

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
    blocks_.clear();
    dirty_block_ = true;  // first draw of a frame always materialises a block
  }

  RecordedDraw& AddDraw() {
    draws_.emplace_back();
    return draws_.back();
  }

  /// Return the block index for the current constants, materialising a new block only when the
  /// guest has actually rewritten them since the last one.
  uint32_t CurrentConstantBlock(const uint8_t* base, uint32_t guest_device, bool constants_dirty);

  const std::vector<RecordedDraw>& draws() const { return draws_; }
  const ConstantBlock& block(uint32_t index) const { return blocks_[index]; }
  size_t block_count() const { return blocks_.size(); }

  /// Bytes captured this frame, for the profiler -- recording cost is the price of the whole
  /// scheme and needs to stay visible.
  size_t captured_bytes() const {
    return draws_.size() * sizeof(RecordedDraw) + blocks_.size() * sizeof(ConstantBlock);
  }

 private:
  std::vector<RecordedDraw> draws_;
  std::vector<ConstantBlock> blocks_;
  bool dirty_block_ = true;
};

}  // namespace nx1::d3d9

#endif  // _WIN32
