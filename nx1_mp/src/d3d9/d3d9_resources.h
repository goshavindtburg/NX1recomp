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
  const VertexLayout* GetVertexLayout(const uint8_t* base, uint32_t guest_device);

  /// Derive a host vertex layout from the bound vertex shader's `vfetch`
  /// instructions, for draws that bind no D3D::CVertexDeclaration (all NX1 menu
  /// UP draws -- ReXGlue's recompiled shaders carry the declaration baked into
  /// their vfetch, so there is no CVertexDeclaration object). Uses the exact same
  /// analysis XenosRecomp ran to build each shader's input signature, so the
  /// declaration matches the compiled SM3 vertex shader. `stream0_stride` is the
  /// guest byte stride of the single UP stream. Returns nullptr on failure.
  const VertexLayout* GetShaderVertexLayout(const uint8_t* base, uint32_t guest_device,
                                            uint32_t stream0_stride);

  /// Mirror vertex stream `stream`. `vertex_count` receives the mirrored length.
  ///
  /// `needed_vertices` caps how much of the buffer is mirrored (0 = all of it). The guest's
  /// fetch constant reports the distance to the end of the buffer, and its dynamic geometry
  /// lives in shared pools -- so for a skinned model that is megabytes, and mirroring it all
  /// meant hashing and converting ~78k vertices for a draw that touches a few hundred. Pass
  /// the draw's actual vertex reach (see GetDrawMaxIndex).
  IDirect3DVertexBuffer9* GetVertexBuffer(const uint8_t* base, uint32_t guest_device,
                                          uint32_t stream, const VertexLayout& layout,
                                          uint32_t needed_vertices, uint32_t* vertex_count);

  /// Mirror the bound index buffer. `index_size` receives 2 or 4.
  IDirect3DIndexBuffer9* GetIndexBuffer(const uint8_t* base, uint32_t guest_device,
                                        uint32_t* index_size);

  /// The highest vertex index the draw range [start_index, start_index + index_count)
  /// references, or 0 if it cannot be determined. Memoized per (index-buffer contents,
  /// range), so the scan runs once per distinct draw rather than every frame.
  /// GetIndexBuffer must have been called for this draw first.
  uint32_t GetDrawMaxIndex(const uint8_t* base, uint32_t guest_device, uint32_t start_index,
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

  /// Untile + upload the texture bound to `sampler`, or nullptr if that fetch
  /// constant holds no texture or an unsupported format.
  IDirect3DBaseTexture9* GetTexture(const uint8_t* base, uint32_t guest_device, uint32_t sampler);

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
  uint32_t mirror_sweep_page_ = 0;    ///< proactive early-sweep cursor
  static constexpr uint32_t kMirrorPages = 0x20000000u >> 12;  ///< 512 MB / 4 KB
  /// Ensure [phys_addr, phys_addr+len) is captured in the mirror and return a pointer into it. A
  /// page is copied out of live guest RAM the first time it is requested while invalid, then held
  /// (and its write-watch armed) until the guest writes it. Falls back to live memory if out of
  /// range or the mirror is unavailable.
  const uint8_t* MirrorSnapshot(uint32_t phys_addr, uint32_t len);

  void* layouts_ = nullptr;        ///< std::unordered_map<uint64_t, VertexLayout>*
  void* vertex_buffers_ = nullptr; ///< std::unordered_map<uint64_t, VertexBufferEntry>*
  void* vertex_hashes_ = nullptr;  ///< std::unordered_map<uint64_t, VertexHash>*
  void* index_buffers_ = nullptr;  ///< std::unordered_map<uint64_t, IndexBufferEntry>*
  void* index_ranges_ = nullptr;   ///< std::unordered_map<uint64_t, uint32_t>* (draw -> max index)
  void* textures_ = nullptr;       ///< std::unordered_map<uint64_t, TextureEntry>*
  void* resolves_ = nullptr;       ///< std::unordered_map<uint64_t, ResolvedTarget>*
  void* render_targets_ = nullptr; ///< std::unordered_map<uint32_t, HostTarget>*
  IDirect3DSurface9* backbuffer_ = nullptr;
  uint32_t backbuffer_width_ = 0;
  uint32_t backbuffer_height_ = 0;
  bool intz_supported_ = false;
  uint64_t unsupported_formats_ = 0;
  uint64_t unsupported_texture_formats_ = 0;
  uint64_t frame_ = 0;  ///< bumped by AdvanceFrame; gates frame-coherent resource reuse
  uint64_t tex_uploads_ = 0;    ///< level-0 uploads (new texture or content changed)
  uint64_t tex_rebuilds_ = 0;   ///< layout changed -> host texture recreated
  uint64_t tex_failures_ = 0;   ///< CreateTexture/UpdateTexture failed
  uint64_t tex_evicted_ = 0;    ///< released by the age sweep
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
