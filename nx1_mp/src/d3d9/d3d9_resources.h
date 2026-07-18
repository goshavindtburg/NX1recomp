/**
 * @file    d3d9_resources.h
 * @brief   Guest -> host resource translation for the native D3D9 renderer.
 *
 * NX1 never creates buffers through D3D. The engine allocates raw physical
 * memory and stamps a header over it with XGSetVertexBufferHeader /
 * XGSetIndexBufferHeader, then points a fetch constant at it. So there is no
 * creation hook to latch onto: the buffers simply exist in guest memory, in
 * big-endian, in Xenos vertex formats.
 *
 * This tracker mirrors them into real D3D9 buffers at draw time, keyed by guest
 * address and validated by a content hash. Most Xenos vertex formats survive a
 * plain dword byteswap (the endian swap the GPU would have applied), but the
 * packed ones -- 2_10_10_10 above all, which is how every normal and tangent is
 * stored -- have no D3D9 input-assembler equivalent and are expanded to floats.
 */

#pragma once

#include <cstdint>

#ifdef _WIN32
#include <mutex>
#include <utility>
#include <vector>

#include <d3d9.h>

namespace nx1::d3d9 {

struct TextureFetchConstant;  // guest_d3d.h
struct VertexFetchConstant;   // guest_d3d.h
struct IndexBufferState;      // guest_d3d.h

inline constexpr uint32_t kMaxHostStreams = 4;

/// How one guest vertex element becomes one host vertex element.
struct ConvertOp {
  uint16_t src_offset;  ///< byte offset in the guest vertex
  uint16_t dst_offset;  ///< byte offset in the host vertex
  uint8_t stream;
  uint8_t format;  ///< xenos::VertexFormat
  uint8_t src_size;
  uint8_t dst_size;
  bool is_signed;
  bool is_normalized;
  bool expand;  ///< true: decode to floats; false: byteswap dwords in place
};

/// A host vertex declaration plus everything needed to repack the guest streams.
struct VertexLayout {
  IDirect3DVertexDeclaration9* decl = nullptr;
  std::vector<ConvertOp> ops;
  uint32_t guest_stride[kMaxHostStreams] = {};
  uint32_t host_stride[kMaxHostStreams] = {};
  /// Set when a stream's host layout is byte-identical to the guest's, so the
  /// whole stream can be byteswapped in one sweep instead of per element.
  bool bulk_swap[kMaxHostStreams] = {};
  uint32_t stream_count = 0;
  /// Cache key for the *layout object*: the declaration or the vertex shader that produced
  /// it. Two shaders with identical vertex formats get two entries here, which is fine --
  /// building one is cheap.
  uint64_t key = 0;
  /// Hash of what actually determines the converted bytes: the ops and the strides. Two
  /// layouts with the same content share it even when they came from different shaders.
  ///
  /// The mirrored vertex buffers are keyed on *this*, not on `key`. Keying them on the
  /// shader meant the same pooled buffer was hashed and converted once per shader that
  /// read it -- half a gigabyte hashed and 3.3M vertices converted per frame, for
  /// byte-identical results.
  uint64_t content_key = 0;
};

/// Owns the mirrored buffers and the vertex-declaration cache.
class ResourceTracker {
 public:
  static ResourceTracker& Get();

  void Initialize(IDirect3DDevice9Ex* device);
  void Shutdown();

  /// Advance the frame counter. Call once per frame (BeginFrame) so the vertex/
  /// index/texture caches hash+validate each resource at most once per frame
  /// instead of once per draw (a scene has ~1000 draws sharing far fewer resources).
  /// Also evicts resources no draw has touched for a while -- the guest allocates its
  /// dynamic buffers out of a moving pool, so their addresses churn and the caches would
  /// otherwise grow without bound (measured: 22k live vertex buffers and still climbing).
  void AdvanceFrame();

  /// Translate the guest's bound D3D::CVertexDeclaration. Returns nullptr if any
  /// element uses a Xenos vertex format we cannot express or decode.
  ///
  /// Takes the declaration object address and the per-stream strides rather than the device, so
  /// a deferred executor can pass what it recorded. The declaration OBJECT is still read out of
  /// guest memory -- it is a separate allocation with its own lifetime, in the same
  /// read-it-late class as the shader microcode.
  const VertexLayout* GetVertexLayout(const uint8_t* base, uint32_t decl_object,
                                      const uint32_t* stream_stride);

  /// Derive a host vertex layout from the bound vertex shader's `vfetch`
  /// instructions, for draws that bind no D3D::CVertexDeclaration (all NX1 menu
  /// UP draws -- ReXGlue's recompiled shaders carry the declaration baked into
  /// their vfetch, so there is no CVertexDeclaration object). Uses the exact same
  /// analysis XenosRecomp ran to build each shader's input signature, so the
  /// declaration matches the compiled SM3 vertex shader. `stream0_stride` is the
  /// guest byte stride of the single UP stream. Returns nullptr on failure.
  /// `vs_object`/`vs_pass` identify the microcode to analyse -- the same pair
  /// BindShadersAndConstants resolved, so the layout matches the SM3 shader that will run.
  /// `stream_stride` supplies the bound-buffer strides (used only when stream0_stride == 0):
  /// the vfetch reads 0 there, so they come from the guest's m_StreamStride[].
  const VertexLayout* GetShaderVertexLayout(const uint8_t* base, uint32_t vs_object,
                                            uint32_t vs_pass, uint32_t stream0_stride,
                                            const uint32_t* stream_stride);

  /// Mirror vertex stream `stream`. `vertex_count` receives the mirrored length.
  ///
  /// `needed_vertices` caps how much of the buffer is mirrored (0 = all of it). The guest's
  /// fetch constant reports the distance to the end of the buffer, and its dynamic geometry
  /// lives in shared pools -- so for a skinned model that is megabytes, and mirroring it all
  /// meant hashing and converting ~78k vertices for a draw that touches a few hundred. Pass
  /// the draw's actual vertex reach (see GetDrawMaxIndex).
  /// `fetch` is the stream's decoded vertex fetch constant (recorded, not re-read).
  IDirect3DVertexBuffer9* GetVertexBuffer(const uint8_t* base, const VertexFetchConstant& fetch,
                                          uint32_t stream, const VertexLayout& layout,
                                          uint32_t needed_vertices, uint32_t* vertex_count);

  /// Mirror the bound index buffer. `index_size` receives 2 or 4.
  IDirect3DIndexBuffer9* GetIndexBuffer(const uint8_t* base, const IndexBufferState& state,
                                        uint32_t* index_size);

  /// The highest vertex index the draw range [start_index, start_index + index_count)
  /// references, or 0 if it cannot be determined. Memoized per (index-buffer contents,
  /// range), so the scan runs once per distinct draw rather than every frame.
  /// GetIndexBuffer must have been called for this draw first.
  uint32_t GetDrawMaxIndex(const IndexBufferState& state, uint32_t start_index,
                           uint32_t index_count);

  /// Convert an inline (user-pointer) vertex stream for a *UP draw: `count`
  /// vertices at guest address `verts_addr`, guest stride `guest_stride`, through
  /// stream 0 of `layout`, into `out` (host bytes). Returns the host stride, or 0
  /// on failure. Unlike GetVertexBuffer this reads a raw pointer, not a fetch
  /// constant, and produces a transient CPU buffer for DrawIndexedPrimitiveUP.
  uint32_t ConvertInlineVertices(uint32_t verts_addr, uint32_t count, uint32_t guest_stride,
                                 const VertexLayout& layout, std::vector<uint8_t>* out);

  /// Byteswap `index_count` inline indices of `index_size` bytes (2 or 4) at guest
  /// address `indices_addr` into `out`. Returns false on failure.
  bool ConvertInlineIndices(uint32_t indices_addr, uint32_t index_count, uint32_t index_size,
                            std::vector<uint8_t>* out);

  /// The guest vertex range the most recent GetVertexBuffer read. Used only by the deferral
  /// feasibility probe, which re-hashes it at Present to see whether the engine rewrote memory
  /// a worker thread would still have been consuming.
  bool LastStreamRange(uint32_t* addr, uint32_t* bytes) const {
    if (!last_stream_bytes_) {
      return false;
    }
    *addr = last_stream_addr_;
    *bytes = last_stream_bytes_;
    return true;
  }
  /// Host pointer for a guest physical address, for the same probe.
  const uint8_t* PhysicalPointer(uint32_t phys_addr) const;

  /// Untile + upload the texture `t` describes, or nullptr if that fetch constant holds no
  /// texture or an unsupported format. `sampler` is the slot it is bound to -- it keys the cache
  /// and names the slot in diagnostics, but nothing here reads it back out of guest memory.
  ///
  /// Takes the DECODED fetch constant rather than (guest_device, sampler) so the caller's copy
  /// is the only one: the caller needs it anyway for the LOD substitution and the sampler state,
  /// and reading it twice per bound slot was six byte-swapped guest dwords across ~15k slots a
  /// frame. It is also what lets a deferred executor call this with a RECORDED constant -- the
  /// guest's live fetch constants are long overwritten by the time a worker runs.
  IDirect3DBaseTexture9* GetTexture(const uint8_t* base, const TextureFetchConstant& t,
                                    uint32_t sampler);

  /// Prefer-largest-resolution substitution, keyed on a draw's STABLE geometry surface. The engine
  /// streams world textures at CPU-side LODs: as a surface recedes it binds sampler 0/4/5 to a
  /// physically smaller texture allocation, and in this build that far-LOD memory is recycled to
  /// garbage (the distance "confetti"). This retains the largest texture a surface has decoded (the
  /// full-res captured up close, which carries our host-generated mip chain) and substitutes it when
  /// the engine later swaps to a smaller LOD, so the driver minifies clean pixels instead of garbage.
  /// `surface_key` 0 disables it (UI / inline-geometry draws). `format` is the guest texture format,
  /// mixed into the retention key so a different-format texture the same surface binds to this sampler
  /// in another pass (e.g. a normal map in the depth pre-pass vs the albedo in the colour pass) is
  /// never substituted for this one. Returns the texture to actually bind.
  /// `base_address` is the guest allocation this binding names. It is what distinguishes a
  /// texture updating in place from the streaming pool swapping in a different allocation at
  /// the same declared size -- see the equal-area branch.
  IDirect3DBaseTexture9* PreferLargestForSurface(uint64_t surface_key, uint32_t sampler,
                                                 uint32_t format, IDirect3DBaseTexture9* tex,
                                                 uint32_t width, uint32_t height,
                                                 uint32_t base_address);

  /// Untile + upload a 3D (volume) texture. The composite's colour-grading LUT is one, and an
  /// unbound sampler reads black -- which blacks out the entire composited frame.
  IDirect3DBaseTexture9* GetVolumeTexture(const TextureFetchConstant& t, uint32_t sampler);

  /// Untile + upload a cube map (the environment/reflection maps). The six faces are stored
  /// as consecutive array slices, each 4 KB aligned, and each is an ordinary tiled 2D image.
  IDirect3DBaseTexture9* GetCubeTexture(const TextureFetchConstant& t, uint32_t sampler);

  /// Record a resolve: StretchRect the current render target (source rect) into a
  /// host texture keyed by the destination's guest address, so a later draw that
  /// samples that address gets the rendered image instead of untiled memory.
  void ResolveColor(uint32_t dest_address, uint32_t width, uint32_t height, const RECT& src_rect,
                    const POINT& dest_point);

  //--- Render targets --------------------------------------------------------
  //
  // The guest does not render to one surface: it draws the world into a 1024x600
  // colour target, its shadow maps into 1024x2048 / 512x2048 depth targets, a bloom
  // chain into 256x150, then composites into the 1920x1080 display buffers. Backing
  // all of those with a single backbuffer + depth buffer (as this renderer first did)
  // makes the shadow passes scribble light's-eye depth into the buffer the world
  // colour pass tests against, so the world cannot shade correctly.
  //
  // Each guest surface therefore gets its own host target, keyed by the guest
  // D3DSurface pointer. A colour target whose size matches the backbuffer *is* the
  // backbuffer, so the guest's final composite lands where Present can show it.

  /// Host colour surface for a guest render-target surface, or nullptr.
  IDirect3DSurface9* GetRenderTargetSurface(const uint8_t* base, uint32_t guest_surface);

  /// Host depth-stencil surface for a guest depth surface, or nullptr. Backed by a
  /// D3DFMT_INTZ texture where supported, so the shadow maps and the scene depth stay
  /// *sampleable* -- the guest resolves them and the lighting shaders read them back.
  ///
  /// `min_width`/`min_height` are the bound colour target's extent. D3D9 rejects every
  /// draw whose depth-stencil is smaller than its render target, and the guest's depth
  /// surfaces decode to a *different* padded height than their colour partners
  /// (scene: 1024x1218 colour vs 1024x1105 depth), so the depth target is grown to
  /// cover the colour one rather than trusted verbatim.
  IDirect3DSurface9* GetDepthSurface(const uint8_t* base, uint32_t guest_surface,
                                     uint32_t min_width, uint32_t min_height);

  /// The sampleable texture behind a depth surface (INTZ), or nullptr.
  IDirect3DTexture9* GetDepthTexture(const uint8_t* base, uint32_t guest_surface);

  /// The host texture a previous resolve published under `address`, or nullptr.
  IDirect3DTexture9* GetResolvedTexture(uint32_t address);

  /// Copy one tile of a depth surface into the resolve destination's texture.
  ///
  /// A depth resolve cannot be a StretchRect (D3D9 will not blit depth-stencil), and it
  /// cannot be an alias either: the guest renders its shadow atlas in EDRAM-sized tiles.
  /// The 1024x2048 atlas is drawn through a *1024x1024* depth surface, one cascade at a
  /// time, each with the same 0..1024 viewport, and each resolved to its own slot. Point
  /// the atlas address at that shared surface and every cascade but the last is gone.
  ///
  /// So each resolve blits `src_rect` of the (INTZ, sampleable) depth surface into an
  /// R32F texture at `dest_point`, and the cascades accumulate the way they do in guest
  /// memory. `src_rect` empty means the whole surface.
  void ResolveDepth(uint32_t dest_address, uint32_t width, uint32_t height,
                    IDirect3DTexture9* src_depth, const RECT& src_rect, const POINT& dest_point);

  void SetBackbuffer(IDirect3DSurface9* surface, uint32_t width, uint32_t height);

  /// Size, churn and remaining driver texture memory, for the frame heartbeat. Textures are
  /// the only resource the renderer both allocates unboundedly and evicts, so when the image
  /// looks wrong it is worth being able to see the cache's trend rather than infer one from
  /// screenshots -- an entry count that oscillates with the scene is healthy; one that only
  /// grows, or a nonzero failure count, is not.
  void LogCacheStats();

 private:
  ResourceTracker() = default;
  ~ResourceTracker() { Shutdown(); }
  ResourceTracker(const ResourceTracker&) = delete;
  ResourceTracker& operator=(const ResourceTracker&) = delete;

  /// Compile the depth-blit pixel shader on first use. Returns false once it has failed,
  /// so a driver without it degrades to no shadows rather than retrying every resolve.
  bool EnsureDepthBlit();

  IDirect3DDevice9Ex* device_ = nullptr;
  IDirect3DPixelShader9* depth_blit_ps_ = nullptr;
  bool depth_blit_failed_ = false;
  /// 1x1 opaque white, bound in place of a texture we cannot yet produce (the
  /// resolved scene depth and the shadow maps -- Xenos fmt 23 -- which need real
  /// render-target support). Sampling an *unbound* sampler returns 0, which reads as
  /// "fully occluded / fully shadowed" and shades the whole world black; white reads
  /// as "far / nothing occluding", so lighting resolves to unshadowed instead.
  IDirect3DTexture9* white_ = nullptr;

  // Physical-memory write-watch. We snapshot a texture's guest bytes into the mirror once and
  // re-read them only when the guest actually writes them, mirroring the reference backend. The
  // callback fires on whatever guest thread wrote the memory (not the render thread) and under the
  // memory global lock, so it must NOT touch the texture cache -- it only queues the written range,
  // which DrainMemoryWrites() applies from AdvanceFrame on the render thread.
  void* mem_watch_handle_ = nullptr;  ///< RegisterPhysicalMemoryInvalidationCallback handle
  std::mutex dirty_mu_;               ///< guards writes_pending_
  std::vector<std::pair<uint32_t, uint32_t>> writes_pending_;  ///< (phys_addr, len) queued by callback
  void DrainMemoryWrites();  ///< apply queued guest writes: invalidate mirror pages, dirty entries
  static std::pair<uint32_t, uint32_t> MemWatchThunk(void* ctx, uint32_t addr, uint32_t len,
                                                     bool exact);
  std::pair<uint32_t, uint32_t> OnMemoryWrite(uint32_t addr, uint32_t len, bool exact);

  // CPU snapshot mirror of guest physical memory. Textures decode from THIS, not from live guest
  // RAM. On this build the streaming pool leaves a texture's bytes valid only transiently (streamed
  // in early, overwritten with fill later), so reading live memory at first-draw gets garbage. We
  // capture each page the first time it is touched -- and proactively via an early sweep, mirroring
  // the reference SharedMemory's full-buffer memexport request -- then HOLD it, refreshing a page
  // only when the guest actually writes it. Snapshot-and-hold is the only thing that renders these
  // streamed textures cleanly; it is exactly what the reference backend does.
  uint8_t* mirror_ = nullptr;         ///< 512 MB VirtualAlloc, lazily committed
  const uint8_t* phys_base_ = nullptr;///< host pointer for guest physical 0
  std::vector<uint64_t> mirror_valid_;///< 1 bit per 4 KB page: captured and held
 public:
  /// Learn every texture address bound over the next `frames` frames, then report each NEW
  /// address as it first appears. A single frame binds ~1000 textures, so a two-frame diff is
  /// almost all noise from geometry entering view; learning a baseline while the effect is
  /// absent and then watching for newcomers isolates it. Stand still, learn, then fire.
  void LearnTextureBaseline(uint32_t frames);

  /// Log newcomers ranked by episode count -- the repeat-appearance effects first.
  void ReportEffectCandidates();

 private:
  uint32_t baseline_frames_ = 0;   ///< frames left to learn
  bool baseline_ready_ = false;    ///< learning finished; report newcomers
  std::vector<uint32_t> baseline_addrs_;  ///< sorted, for binary search on the bind path
  /// Newcomers, with the frame span over which they stayed bound. An effect sprite is bound
  /// for a frame or two and vanishes; world geometry persists. Reporting the SHORT-LIVED ones
  /// separates a muzzle flash from the hundreds of materials that scroll into view when the
  /// camera moves, which is what made the raw newcomer list unreadable.
  struct NewTex {
    uint32_t addr;
    uint32_t width, height, format, sampler;
    uint64_t first_frame, last_frame;
    bool reported;
    /// How many separate times this texture went from unbound to bound. THE discriminator:
    /// fire twelve shots and the muzzle flash has twelve short episodes, while a world
    /// material that scrolled into view has one long one. Lifetime alone cannot tell them
    /// apart, because streaming and LOD swaps produce plenty of brief world textures too.
    uint32_t episodes;
    uint64_t frames_bound;
  };
  std::vector<NewTex> baseline_new_;

  /// Flat mirror of the resolve map, scanned instead of hashed on the texture bind path.
  /// There are only ever a handful of resolve targets (8 in practice), but GetTexture probed
  /// the hash map for every bound sampler slot of every draw -- ~16.6k times a frame, each a
  /// likely cache miss into a node/table lookup, to search eight entries. The keys pack into
  /// two cache lines, so a linear scan is far cheaper than hashing. Rebuilt whenever the map
  /// changes; kResolveFlatMax is a safety cap, above which the hash map is used as before.
  static constexpr uint32_t kResolveFlatMax = 32;
  uint32_t resolve_flat_addr_[kResolveFlatMax] = {};
  IDirect3DTexture9* resolve_flat_tex_[kResolveFlatMax] = {};
  uint32_t resolve_flat_count_ = 0;
  bool resolve_flat_valid_ = false;

  /// Rebuild the flat mirror from the resolve map. Cheap; called only when a resolve lands.
  void RebuildResolveFlat();

  /// 1 bit per 4 KB page: the guest-write callback is enabled for it. Tracked separately from
  /// mirror_valid_ because the live-read path needs the watch WITHOUT the snapshot -- the
  /// watch used to be armed only as a side effect of capturing a page into the mirror, so
  /// reading live meant no page was ever watched, no texture was ever dirtied, and a texture
  /// decoded before its data arrived stayed empty forever.
  std::vector<uint64_t> watch_armed_;
  uint32_t mirror_sweep_page_ = 0;    ///< proactive early-sweep cursor
  static constexpr uint32_t kMirrorPages = 0x20000000u >> 12;  ///< 512 MB / 4 KB
  /// Ensure [phys_addr, phys_addr+len) is captured in the mirror and return a pointer into it. A
  /// page is copied out of live guest RAM the first time it is requested while invalid, then held
  /// (and its write-watch armed) until the guest writes it. Falls back to live memory if out of
  /// range or the mirror is unavailable.
  const uint8_t* MirrorSnapshot(uint32_t phys_addr, uint32_t len);

  /// Enable the guest-write callback for every page covering [phys_addr, +len), without
  /// copying anything. DrainMemoryWrites then dirties any texture whose watched range overlaps
  /// a write, which is what makes a live-read decode re-run once the data actually lands.
  void ArmWriteWatch(uint32_t phys_addr, uint32_t len);

  void* layouts_ = nullptr;        ///< std::unordered_map<uint64_t, VertexLayout>*
  void* vertex_buffers_ = nullptr; ///< std::unordered_map<uint64_t, VertexBufferEntry>*
  void* vertex_hashes_ = nullptr;  ///< std::unordered_map<uint64_t, VertexHash>*
  void* index_buffers_ = nullptr;  ///< std::unordered_map<uint64_t, IndexBufferEntry>*
  void* index_ranges_ = nullptr;   ///< std::unordered_map<uint64_t, uint32_t>* (draw -> max index)
  void* textures_ = nullptr;       ///< std::unordered_map<uint64_t, TextureEntry>*
  void* best_textures_ = nullptr;  ///< std::unordered_map<uint64_t, BestTexture>* (surface^sampler -> largest)
  void* resolves_ = nullptr;       ///< std::unordered_map<uint64_t, ResolvedTarget>*
  void* render_targets_ = nullptr; ///< std::unordered_map<uint32_t, HostTarget>*
  IDirect3DSurface9* backbuffer_ = nullptr;
  uint32_t backbuffer_width_ = 0;
  uint32_t backbuffer_height_ = 0;
  bool intz_supported_ = false;
  uint64_t unsupported_formats_ = 0;
  uint64_t unsupported_texture_formats_ = 0;
  uint64_t frame_ = 0;  ///< bumped by AdvanceFrame; gates frame-coherent resource reuse
  /// TEMP PROFILING: split the textures phase into "resolved from cache" vs "actually rebuilt".
  /// The phase total tracks neither draw count nor upload count, so which of the two dominates
  /// has to be measured rather than reasoned about. Drained by TakeTextureProfile.
 public:
  /// Driven from the renderer's nx1_d3d9_profile cvar. Gating matters: the clock reads would
  /// otherwise cost ~0.5 ms/frame across ~11k GetTexture calls and perturb what they measure.
  bool prof_enabled_ = false;

  /// Per-frame texture cost breakdown, drained and reset by the reporter.
  struct TextureProfile {
    uint64_t lookup_ns = 0;   ///< binds resolved from cache
    uint64_t upload_ns = 0;   ///< total rebuild cost
    uint64_t uploads = 0;     ///< rebuilds entered
    uint64_t stage_ns = 0;    ///< staging CreateTexture + LockRect
    uint64_t decode_ns = 0;   ///< detile + format decode of level 0
    uint64_t mipgen_ns = 0;   ///< CPU BC mip chain (decode/filter/encode per level)
    uint64_t commit_ns = 0;   ///< DEFAULT CreateTexture + UpdateTexture (driver/VRAM)
    /// Why each rebuild happened. A rebuild costs ~500 us and is memory-bound (it streams the
    /// source out of the mirror), so it cannot get much cheaper -- the only remaining lever is
    /// doing fewer, and that needs the cause named rather than guessed.
    uint64_t why_new = 0;      ///< no host texture yet (first sight, or evicted and re-bound)
    uint64_t why_layout = 0;   ///< layout_key changed under the same base address
    uint64_t why_dirty = 0;    ///< write-watch dirtied it before it could commit
    uint64_t why_zero = 0;     ///< never-resident broadcast-swizzle sprite, on its backoff retry
    uint64_t decode_bytes = 0; ///< source bytes streamed out of the mirror, to check the
                               ///< memory-bound theory against real bandwidth
  };
  TextureProfile TakeTextureProfile() {
    TextureProfile p = prof_tex_;
    prof_tex_ = {};
    return p;
  }

 private:
  TextureProfile prof_tex_;

 public:
  /// TEMP PROFILING: the streams phase has resisted two optimisations (eliding ~9100 D3D
  /// calls a frame, and memoizing the layout lookup) with no measurable change, so where its
  /// ~2.2 ms actually goes has to be measured rather than reasoned about.
  struct VertexProfile {
    uint64_t fast_ns = 0;      ///< fetch-constant read, key, map probe, frame-coherent early-out
    uint64_t hash_ns = 0;      ///< XXH3 over the guest vertices (memoized per buffer per frame)
    uint64_t convert_ns = 0;   ///< CreateVertexBuffer + Lock + byteswap/convert + Unlock
    uint64_t calls = 0;
    uint64_t hashes = 0;       ///< buffers actually hashed (memo misses)
    uint64_t converts = 0;     ///< buffers actually rebuilt
    uint64_t hash_bytes = 0;
    uint64_t convert_bytes = 0;
    uint64_t dynamic_converts = 0;  ///< of those, rebuilds into a DYNAMIC (DISCARD-able) buffer
  };
  VertexProfile TakeVertexProfile() {
    VertexProfile p = prof_vtx_;
    prof_vtx_ = {};
    return p;
  }

 private:
  VertexProfile prof_vtx_;

  /// Last layout GetVertexLayout resolved, memoized so a repeat draw skips the map probe.
  /// See the comment there for why holding this pointer is safe.
  uint32_t last_stream_addr_ = 0;
  uint32_t last_stream_bytes_ = 0;
  uint64_t last_layout_key_ = 0;
  const VertexLayout* last_layout_ = nullptr;

  /// Detile scratch for the CPU-decode formats, reused across rebuilds. It was a local
  /// std::vector, which malloc'd AND zero-filled up to a texture's worth of bytes on every
  /// rebuild only for DetileMip2D to overwrite every one of them. Grow-only, never shrunk, so
  /// after warm-up the resize is a no-op.
  std::vector<uint8_t> detile_scratch_;
  uint8_t* DetileScratch(size_t bytes) {
    if (detile_scratch_.size() < bytes) detile_scratch_.resize(bytes);
    return detile_scratch_.data();
  }

  uint64_t tex_uploads_ = 0;    ///< level-0 uploads (new texture or content changed)
  uint64_t tex_rebuilds_ = 0;   ///< layout changed -> host texture recreated
  uint64_t tex_failures_ = 0;   ///< CreateTexture/UpdateTexture failed
  uint64_t tex_evicted_ = 0;    ///< released by the age sweep

 public:
  /// TEMP PROFILING: why PreferLargestForSurface is or is not substituting. The confetti
  /// appears when BACKING AWAY from a surface, which is exactly the case this is supposed to
  /// cover, so the question is which of its branches the receding draw actually takes.
  struct LodProfile {
    uint64_t calls = 0;
    uint64_t no_surface = 0;   ///< surface_key 0 -- UI/inline draw, lineage not tracked
    uint64_t fresh = 0;        ///< first time this surface+sampler+format was seen
    uint64_t adopt = 0;        ///< strictly larger -> becomes the retained texture
    uint64_t equal = 0;        ///< same area -> pass the current one through
    /// Of those, the ones where the current texture is a DIFFERENT object from the retained
    /// one. equal_same is just the same texture rebound and is harmless; equal_diff is the
    /// only case where this branch can hand back a different (possibly unstreamed) allocation
    /// while bypassing a known-good retained texture. If this is ~0 the branch is innocent.
    uint64_t equal_same = 0;
    uint64_t equal_diff = 0;
    uint64_t substitute = 0;   ///< smaller, retained texture served instead (the fix working)
  };
  LodProfile TakeLodProfile() {
    LodProfile p = prof_lod_;
    prof_lod_ = {};
    return p;
  }

 private:
  LodProfile prof_lod_;
  uint64_t mips_built_ = 0;            ///< chain generated here (block-compressed)
  uint64_t mips_auto_ = 0;             ///< chain left to the driver (uncompressed)
  uint64_t mips_skip_nochain_ = 0;     ///< guest declares mip_max_level == 0
  uint64_t mips_skip_unsupported_ = 0; ///< no autogen and not block-compressed
};

/// D3DPT_* on the Xbox 360 is the raw Xenos PrimitiveType, which agrees with the
/// PC enum except that fan and strip are swapped. Returns 0 for primitive types
/// D3D9 has no equivalent for (rectangle lists, quads).
D3DPRIMITIVETYPE HostPrimitiveType(uint32_t xenos_primitive_type);

/// Primitive count for `index_count` indices of `xenos_primitive_type`.
uint32_t HostPrimitiveCount(uint32_t xenos_primitive_type, uint32_t index_count);

}  // namespace nx1::d3d9

#endif  // _WIN32
