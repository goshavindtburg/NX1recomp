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
 *
 * THREADING CONTRACT
 *
 * Decided before writing the queue, because it determines what has to be queued at all.
 *
 * The device is created with D3DCREATE_MULTITHREADED, so the D3D9 runtime serialises concurrent
 * calls for us. That solves THREAD-SAFETY and nothing else -- it does not give ORDERING, and
 * ordering is the actual requirement: a resolve that runs before the draws it is meant to capture
 * produces a correct-but-empty surface, and the shadow-cascade and composite passes depend
 * entirely on that sequencing. So the rule is not "make the calls safe", it is:
 *
 *   EVERY command that affects the render stream -- draws, Clear, Resolve, SetRenderTarget,
 *   SetDepthStencil -- goes through this ONE ordered buffer. The worker is the only thing that
 *   executes them.
 *
 * The frame boundary does NOT need queueing, which is what keeps this small:
 *
 *   - Present and the ImGui overlay stay on the guest thread. They run AFTER the guest waits for
 *     the worker to drain, so the worker is idle and no ordering question arises. Neither needs
 *     any change.
 *   - ResourceTracker becomes worker-owned for the duration of a frame. Nothing on the guest
 *     thread touches it mid-frame: the XGSet*Header registration hooks are pure pass-throughs,
 *     and the write-watch callback only queues ranges under its own lock (see MemWatchThunk).
 *   - BeginFrame / DrainMemoryWrites run at the boundary, i.e. after the drain, for the same
 *     reason.
 *
 * The drain barrier at Present is therefore the single synchronisation point, and the overlap
 * being bought is the guest's between-draw logic (measured 5.3-7.5 ms) running while the worker
 * translates.
 */

#pragma once

#include <cstdint>

#ifdef _WIN32
#include <vector>

#include "d3d9_resources.h"  // kMaxHostStreams
#include "guest_d3d.h"

namespace nx1::d3d9 {

struct Sm3Shader;  // d3d9_shaders.h

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

  /// RESOLVED constant values, ready to hand straight to SetVertex/PixelShaderConstantF.
  ///
  /// Resolution stays on the guest thread and only the upload is deferred -- see
  /// ShaderCache::ResolveConstants for why that is forced rather than chosen (the PM4 ring and
  /// the shader literals are both transient, and the guest's dirty mask does not even report ring
  /// writes). Offsets index the frame's constant pool; count is REGISTERS, so the float span is
  /// count*4. count == 0 means this stage had nothing to upload, which is the common case: the
  /// clean-constant skip elides ~53% of vertex and ~16% of pixel uploads.
  uint32_t vs_const_offset = 0;
  uint32_t vs_const_count = 0;
  uint32_t ps_const_offset = 0;
  uint32_t ps_const_count = 0;
  /// Whether resolution actually RAN for this stage, which is NOT the same as count != 0 and
  /// must not be collapsed into it. ApplyConstants(count == 0) is not a no-op: it still
  /// re-asserts the shader's `def` literals, and the runtime does not reapply those on SetShader
  /// -- so a shader with an empty constant window still needs the call, or the previous shader's
  /// window upload stays sitting in its def registers. Conflating the two skipped that call and
  /// produced band-shaped artifacts, the same signature as the DOF bands and the sun-shadow bug.
  bool vs_const_valid = false;
  bool ps_const_valid = false;

  /// The translated shaders this draw runs, resolved at record time. Pointers are stable because
  /// ShaderMap is node-based (see d3d9_shaders.cpp) -- do not convert it to FlatMap.
  const Sm3Shader* vs = nullptr;
  const Sm3Shader* ps = nullptr;

  /// Stable per-surface identity for the prefer-largest LOD substitution, computed at record
  /// time because it derives from the index range.
  uint64_t surface_key = 0;
};

/// What kind of GPU command a RecordedCommand is.
///
/// Draws are not the only thing that has to be ordered. A resolve that runs before the draws it
/// captures produces a correct-but-EMPTY surface, and a render-target bind that lands mid-batch
/// sends a pass to the wrong place -- the shadow cascades and the final composite both depend on
/// this sequencing. So every one of these shares a single ordered list, and the worker is the only
/// thing that executes any of them.
enum class CommandKind : uint32_t {
  kDraw,
  kClear,
  kResolve,
  kSetRenderTarget,
  kSetDepthStencil,
};

/// One ordered GPU command. Draws live in their own array (RecordedDraw is ~1 KB, far too big to
/// inline into a struct a clear also uses) and are referenced by index; everything else carries
/// its own small payload here.
///
/// Payloads hold RESOLVED values, not guest addresses, wherever the source is transient: a clear's
/// colour arrives as a guest __vector4 and its rect as a guest _D3DRECT, both typically on the
/// caller's stack, so reading them late would be reading whatever the guest has since pushed.
struct RecordedCommand {
  CommandKind kind = CommandKind::kDraw;
  /// kDraw: index into draws().
  uint32_t draw_index = 0;

  // --- kClear --------------------------------------------------------------------------
  /// Xenos clear bits, NOT desktop D3DCLEAR_* -- the mapping happens at execute time because it
  /// also depends on whether a depth-stencil is bound, which an earlier command may change.
  uint32_t clear_flags = 0;
  bool has_rect = false;
  /// The guest supplied no colour vector, which suppresses D3DCLEAR_TARGET independently of the
  /// flag bits.
  bool has_color = false;
  int32_t rect[4] = {};
  float clear_color[4] = {};  ///< r, g, b, a as the guest stored them
  float clear_z = 0.0f;
  uint32_t clear_stencil = 0;

  // --- kResolve ------------------------------------------------------------------------
  /// DEFERRED-GAP: these are still guest ADDRESSES (src rect, dest point, clear colour), read at
  /// execute time. Safe while execution is synchronous. Before the worker lands they need the
  /// same treatment the clear payload got above -- resolve to values at record time.
  uint32_t dest_texture = 0;
  uint32_t src_rect = 0;
  uint32_t dest_point = 0;
  uint32_t resolve_flags = 0;
  uint32_t resolve_clear_color = 0;
  float resolve_clear_z = 0.0f;
  uint32_t resolve_clear_stencil = 0;

  // --- kSetRenderTarget / kSetDepthStencil ---------------------------------------------
  uint32_t rt_index = 0;
  uint32_t surface = 0;
};

/// A frame's worth of recorded draws plus the constant blocks they reference.
///
/// Deliberately owns its storage and is reused frame to frame: at 5000 draws this is a few MB,
/// and reallocating it every frame would put the cost back on the thread we are trying to
/// unload.
class CommandBuffer {
 public:
  /// Append `count` registers (count*4 floats) of resolved constants; returns the pool offset.
  ///
  /// !! POINTER INVALIDATION -- MUST BE FIXED BEFORE THE WORKER LANDS !!
  /// constants() hands back a pointer INTO this vector. Today that is safe only because the flow
  /// is synchronous and strictly append-then-read within one draw: nothing appends between a
  /// draw's AddConstants and its ApplyConstants. The moment a worker reads frame data while the
  /// guest thread keeps recording, a growth reallocation frees the block the worker is reading --
  /// a use-after-free that will present as random constant corruption or a driver crash, not as
  /// an obvious fault. Same hazard the FlatMap notes describe, with a worse failure mode.
  /// Fix before step 3: either reserve a hard cap and refuse to grow, or hand the executor
  /// offsets into a chunked pool whose blocks never move.
  uint32_t AddConstants(const float* values, uint32_t count) {
    const uint32_t offset = uint32_t(const_pool_.size());
    const_pool_.insert(const_pool_.end(), values, values + size_t(count) * 4);
    return offset;
  }
  const float* constants(uint32_t offset) const { return const_pool_.data() + offset; }

  void BeginFrame() {
    draws_.clear();
    commands_.clear();
    deltas_.clear();
    delta_pool_.clear();
    const_pool_.clear();
    // The executor's running constant file carries across frames exactly as the guest's shadow
    // does, so a frame does not need to open with a full copy.
  }

  RecordedDraw& AddDraw() {
    draws_.emplace_back();
    RecordedCommand& c = AddCommand(CommandKind::kDraw);
    c.draw_index = uint32_t(draws_.size() - 1);
    return draws_.back();
  }

  RecordedCommand& AddCommand(CommandKind kind) {
    commands_.emplace_back();
    commands_.back().kind = kind;
    return commands_.back();
  }

  const std::vector<RecordedCommand>& commands() const { return commands_; }
  RecordedDraw& draw(uint32_t index) { return draws_[index]; }

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
           delta_pool_.size() + const_pool_.size() * sizeof(float);
  }

 private:
  std::vector<RecordedDraw> draws_;
  std::vector<RecordedCommand> commands_;
  std::vector<ConstantDelta> deltas_;
  std::vector<uint8_t> delta_pool_;
  std::vector<float> const_pool_;
};

}  // namespace nx1::d3d9

#endif  // _WIN32
