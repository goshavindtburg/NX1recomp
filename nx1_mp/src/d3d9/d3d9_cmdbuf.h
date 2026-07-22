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
 * Anything the guest may rewrite before the worker consumes it. Constants are NOT stable (hence
 * resolving them on the guest thread); fetch constants are NOT (hence copying all 32).
 *
 * MEASURED, per class, sampled at a stride across the whole frame:
 *
 *   vertex   1-2 changed of ~19500 probed   (~6% of ~311k candidates)
 *   index    0 changed of ~19500 probed
 *   ucode    0 changed of ~19800 probed
 *
 * The nonzero VERTEX figure is the control and the reason the other two are believable: it
 * proves the probe can detect mutation at all, so zero for index and microcode is a real zero
 * rather than a stuck instrument. An earlier version of this measurement reported a flawless
 * 100% for all three and was pure artifact -- it had saturated a per-frame cap on the opening
 * shadow passes and never looked at the rest of the frame. Never trust a stability result that
 * has no class showing churn.
 *
 * So index data and microcode may be read late. Vertex data is imperfect and is ACCEPTED rather
 * than fixed: the probe re-hashes at PRESENT, while the worker consumes a draw within
 * milliseconds of it being recorded, so ~0.01% is an upper bound on the real exposure, not an
 * estimate of it. The failure mode is one model showing stale geometry for a single frame;
 * snapshotting vertex data or re-hashing every draw at execute time both cost more than this
 * whole scheme saves.
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
 *
 * OUTCOME
 *
 * A/B toggled in a fixed spot, same scene and same ~4860 draws, with only the cvar changing:
 *
 *   nx1_d3d9_async 0   45 FPS
 *   nx1_d3d9_async 1   60 FPS (vsync-locked)
 *
 * The 60 is a DISPLAY floor, not the renderer's ceiling -- FRAME reads 16.5 ms, which is 60.6 Hz
 * to within noise, so the true async frame time is unmeasured and lower. Claim "at least
 * 45 -> 60", not a specific ms figure.
 *
 * The toggle is what makes this attributable. The raw before/after (24.4 ms at 4960 draws in
 * step 1 vs 16.5 ms at 4858 now) spans a lot of other work -- feeding textures and streams from
 * the record removed a duplicate fetch-constant decode per bound slot, and the resolve/apply
 * split moved constant work -- so only the same-scene toggle isolates the worker itself.
 *
 * WHAT THE THREADING COST, for the next person tempted to skip the staging
 *
 * Four bugs, none of which synchronous testing could have found, and all of the same shape --
 * "this is safe because the synchronous path never does that":
 *
 *   1. guest_device read from a shared slot at DEQUEUE time, so a draw queued behind a clear or
 *      resolve executed against device 0. Bad draws, then a crash.
 *   2. The constant pool's chunks were made immovable while the vector of pointers TO them was
 *      left free to reallocate. Immovable blocks behind a movable index is not immovable storage.
 *   3. draws_/commands_ as std::deque, on "push_back does not invalidate references" -- true, but
 *      it covers references taken BEFORE the push, not operator[] racing push_back. A real
 *      guarantee that did not mean what it needed to mean.
 *   4. The index-buffer BINDING re-read at execute time. Device state that changes every draw,
 *      in the same category as the fetch constants. Missed because the stability probe measured
 *      index DATA and that was taken as clearing index reads generally.
 *
 * WHY THERE IS A DRAIN BARRIER AND NOT FRAME PIPELINING
 *
 * PROF/bound shows both threads idle ~3.4 ms per frame at DIFFERENT times: the worker starves at
 * frame start with nothing recorded yet, the guest blocks at frame end draining. Guest busy
 * ~12.8 ms, worker busy ~10.4 ms, so a perfectly pipelined frame would be ~12.8 ms against ~16
 * measured. That bubble is worth ~3.4 ms, roughly 62 -> 78 FPS, and closing it means overlapping
 * frame N's execution with N+1's recording.
 *
 * MEASURED AND REJECTED. Carrying the stability probes one frame and re-hashing them at the next
 * Present:
 *
 *   vertex   in-frame 99.91% unchanged   CROSS-FRAME 95.78%
 *   index    in-frame 100%               CROSS-FRAME 100%
 *   ucode    in-frame 100%               CROSS-FRAME 100%
 *
 * Vertex churn is ~46x worse across a frame boundary. Scaled back through the 1/16 sampling
 * stride that is ~230 draws a frame reading vertex memory that is rewritten before the next
 * frame ends -- the engine's dynamic geometry comes from recycled pools (the same behaviour that
 * forced the vertex-buffer eviction sweep). Pipelining would therefore corrupt animated players,
 * weapons and effects persistently, not occasionally.
 *
 * Making it correct needs vertex snapshots, which is what this design rejected as costing more
 * than it saves -- and it would land on the guest thread, already the longer pole. So the drain
 * barrier stays and the ~3.4 ms is left on the table deliberately, not by oversight.
 *
 * Also worth keeping: two instruments silently became nonsense when their assumptions broke --
 * PROF/defer differencing timestamps from two threads, and the stability probe saturating its
 * cap on the frame's opening passes and reporting a confident 100%. Check that a measurement can
 * still detect the thing it is looking for before believing a clean result.
 */

#pragma once

#include <cstdint>

#ifdef _WIN32
#include <cstring>
#include <memory>
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

  /// Sampler-0 texture address and a fingerprint of its first bytes, taken AT DRAW TIME on the
  /// guest thread. The decode happens later on the worker; comparing this against a fresh
  /// fingerprint there counts how often the streamer recycled the slot inside that window. See
  /// GuestTextureFingerprint. Zero address or zero fingerprint means "not captured".
  uint32_t s0_addr = 0;
  uint64_t s0_fingerprint = 0;

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
  /// The bound index buffer's DESCRIPTOR, captured -- not the address to re-read it from.
  ///
  /// This is device state that changes every draw, exactly like the fetch constants. Reading it
  /// at execute time meant the worker mirrored whatever index buffer the guest had bound by
  /// THEN, tens of draws later, and drew one mesh's indices through another's vertices: the
  /// whole world rendered as exploded spikes. The mistake was measuring that index DATA is
  /// stable within a frame and crossing off index reads generally -- data and binding are
  /// different things, and only the data was ever measured.
  IndexBufferState index_buffer{};
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
  PolyOffsetState poly_offset{};
  StencilState stencil{};
  ScissorState scissor{};
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

  /// The guest D3D::CDevice this command was recorded against.
  ///
  /// PER COMMAND, deliberately. This started life as one shared slot on the renderer that
  /// SubmitCommand overwrote -- which is correct synchronously (no gap between submit and
  /// execute) and silently wrong under a worker, which reads it at DEQUEUE time and so sees
  /// whatever the most recent submit wrote. Clear/Resolve/SetRenderTarget submit with 0, so any
  /// draw still queued behind one of those executed with guest_device == 0 and read its
  /// index-buffer descriptor out of address 0. Bad draws, then a crash on the wild index range.
  /// A bug that literally could not appear until the queue existed.
  uint32_t guest_device = 0;

  // --- kClear and kResolve (shared: `kind` discriminates, and a command is never both) ---
  /// Xenos clear bits, NOT desktop D3DCLEAR_* -- the mapping happens at execute time because it
  /// also depends on whether a depth-stencil is bound, which an earlier command may change.
  uint32_t clear_flags = 0;
  bool has_rect = false;
  /// The guest supplied no colour vector, which suppresses D3DCLEAR_TARGET independently of the
  /// flag bits.
  bool has_color = false;
  bool has_dest_point = false;
  /// kClear: the _D3DRECT to clear. kResolve: the source rect to copy out of EDRAM.
  int32_t rect[4] = {};
  /// kResolve only: where the band lands in the destination.
  int32_t dest_point[2] = {};
  float clear_color[4] = {};  ///< r, g, b, a as the guest stored them
  float clear_z = 0.0f;
  uint32_t clear_stencil = 0;

  // --- kResolve ------------------------------------------------------------------------
  /// The destination D3DBaseTexture. Left as a guest OBJECT address deliberately: unlike the
  /// rect/point/colour above -- which the guest passes on its stack and which would be gone by
  /// the time a worker read them -- a texture object is a persistent allocation, the same
  /// read-it-late class as the shader objects and surface descriptors.
  uint32_t dest_texture = 0;
  /// kResolve: write the resolved pixels back into guest RAM at the destination, so the
  /// game's CPU-side reads (texture bakes) see real data. Decided on the GUEST thread at
  /// record time -- the budget map is not thread-safe, and the guest thread must also know
  /// the answer to drain the worker before returning (the bake pattern is resolve -> fence ->
  /// CPU read, so the writeback has to land before the guest resumes).
  bool resolve_writeback = false;

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
  /// Backed by a CHUNKED pool, and that is load-bearing rather than an optimisation. This was a
  /// plain std::vector, which was safe only because the synchronous flow appends and reads within
  /// one draw. Under a worker -- reading frame data while the guest thread keeps recording -- a
  /// growth reallocation frees the block the worker is mid-read of. That is a use-after-free that
  /// presents as random constant corruption or a driver crash, not as a clean fault, and it would
  /// have been miserable to find through a thread. Chunks are never resized and never moved, so a
  /// pointer handed out stays valid for the whole frame no matter how much is appended after it.
  uint32_t AddConstants(const float* values, uint32_t count) {
    const size_t n = size_t(count) * 4;
    // A single append is at most kMaxHostConstants*4 = 1024 floats, far below a chunk, so a
    // request never straddles two chunks.
    // The chunk_index_ == -1 test is NOT redundant with the size test, and dropping it crashed
    // the worker. BeginFrame rewinds to (index = -1, used = kChunkFloats) and relied on the size
    // test to step onto chunk 0 -- but a frame whose FIRST append has n == 0 evaluates
    // kChunkFloats + 0 > kChunkFloats as false, never advances, and indexes chunks_[-1]. The
    // memcpy is guarded on n so nothing faults there; instead the returned offset is
    // astronomical and the worker dies later inside constants(). n == 0 is common -- it is the
    // zero-register constant window of a shader whose whole window was optimised out.
    if (chunk_index_ == size_t(-1) || chunk_used_ + n > kChunkFloats) {
      if (++chunk_index_ >= chunks_.size()) {
        // chunks_ is RESERVED to kMaxChunks so this push_back never reallocates. That matters:
        // the chunk BLOCKS never move, but the vector holding the pointers to them would, and
        // the worker dereferences chunks_[i] concurrently. Making the blocks immovable while
        // leaving their index movable is a half-fix, and it corrupted every deferred draw.
        if (chunk_index_ >= kMaxChunks) {
          chunk_index_ = kMaxChunks - 1;  // bounded rather than reallocating; see full()
        } else {
          chunks_.emplace_back(new float[kChunkFloats]);
        }
      }
      chunk_used_ = 0;
    }
    float* dst = chunks_[chunk_index_].get() + chunk_used_;
    if (n) {
      std::memcpy(dst, values, n * sizeof(float));
    }
    const uint32_t offset = uint32_t(chunk_index_ * kChunkFloats + chunk_used_);
    chunk_used_ += n;
    return offset;
  }
  const float* constants(uint32_t offset) const {
    return chunks_[offset / kChunkFloats].get() + (offset % kChunkFloats);
  }

  void BeginFrame() {
    // Reserve once, up front. clear() keeps capacity, so from here on push_back never
    // reallocates and the worker can index safely while the guest appends.
    if (draws_.capacity() < kMaxDraws) {
      draws_.reserve(kMaxDraws);
      commands_.reserve(kMaxCommands);
      chunks_.reserve(kMaxChunks);
    }
    draws_.clear();
    commands_.clear();
    deltas_.clear();
    delta_pool_.clear();
    // Chunks are RETAINED, only rewound: reallocating a few hundred KB every frame would put
    // cost back on the thread this whole scheme exists to unload. chunk_index_ starts at -1 so
    // the first AddConstants steps onto chunk 0.
    chunk_index_ = size_t(-1);
    chunk_used_ = kChunkFloats;  // forces the first append to step to a chunk
    // The executor's running constant file carries across frames exactly as the guest's shadow
    // does, so a frame does not need to open with a full copy.
  }

  /// CAPACITY IS THE CORRECTNESS PROPERTY HERE, not a tuning knob.
  ///
  /// The worker indexes draws_/commands_ while the guest thread keeps appending. That is only
  /// safe if the backing buffer NEVER MOVES: with capacity reserved, push_back writes past the
  /// end and bumps size_, so an index the worker already holds keeps pointing at the same
  /// memory. Grow the vector and the worker is reading freed storage.
  ///
  /// std::deque was tried here first, on the strength of "push_back does not invalidate
  /// references". That guarantee is about references obtained BEFORE the push; calling
  /// operator[] CONCURRENTLY with push_back still races the deque's internal block map. It
  /// rendered the whole world as exploded geometry. Reserved vectors do not have that problem
  /// because nothing inside them is written except the new tail.
  static constexpr size_t kMaxDraws = 32768;
  static constexpr size_t kMaxCommands = 49152;
  static constexpr size_t kMaxChunks = 256;  // x 256 KB = 64 MB ceiling, ~4 chunks/frame in use

  bool full() const { return draws_.size() >= kMaxDraws || commands_.size() >= kMaxCommands; }

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
  RecordedCommand& command(uint32_t index) { return commands_[index]; }
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
           delta_pool_.size() + (chunk_index_ + 1) * kChunkFloats * sizeof(float);
  }

 private:
  std::vector<RecordedDraw> draws_;
  std::vector<RecordedCommand> commands_;
  std::vector<ConstantDelta> deltas_;
  std::vector<uint8_t> delta_pool_;

  /// Chunked constant storage -- see AddConstants for why the blocks must never move.
  static constexpr size_t kChunkFloats = 64 * 1024;  // 256 KB per chunk
  std::vector<std::unique_ptr<float[]>> chunks_;
  size_t chunk_index_ = size_t(-1);
  size_t chunk_used_ = kChunkFloats;
};

}  // namespace nx1::d3d9

#endif  // _WIN32
