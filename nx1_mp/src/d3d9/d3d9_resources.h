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
#include <vector>

#include <d3d9.h>

namespace nx1::d3d9 {

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
  uint64_t key = 0;
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
  void AdvanceFrame() { ++frame_; }

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

  /// Mirror vertex stream `stream`. `vertex_count` receives the stream's length.
  IDirect3DVertexBuffer9* GetVertexBuffer(const uint8_t* base, uint32_t guest_device,
                                          uint32_t stream, const VertexLayout& layout,
                                          uint32_t* vertex_count);

  /// Mirror the bound index buffer. `index_size` receives 2 or 4.
  IDirect3DIndexBuffer9* GetIndexBuffer(const uint8_t* base, uint32_t guest_device,
                                        uint32_t* index_size);

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

  /// Record a resolve: StretchRect the current render target (source rect) into a
  /// host texture keyed by the destination's guest address, so a later draw that
  /// samples that address gets the rendered image instead of untiled memory.
  void ResolveColor(uint32_t dest_address, uint32_t width, uint32_t height, const RECT& src_rect);

 private:
  ResourceTracker() = default;
  ~ResourceTracker() { Shutdown(); }
  ResourceTracker(const ResourceTracker&) = delete;
  ResourceTracker& operator=(const ResourceTracker&) = delete;

  IDirect3DDevice9Ex* device_ = nullptr;
  void* layouts_ = nullptr;        ///< std::unordered_map<uint64_t, VertexLayout>*
  void* vertex_buffers_ = nullptr; ///< std::unordered_map<uint64_t, VertexBufferEntry>*
  void* index_buffers_ = nullptr;  ///< std::unordered_map<uint64_t, IndexBufferEntry>*
  void* textures_ = nullptr;       ///< std::unordered_map<uint64_t, TextureEntry>*
  void* resolves_ = nullptr;       ///< std::unordered_map<uint64_t, ResolvedTarget>*
  uint64_t unsupported_formats_ = 0;
  uint64_t unsupported_texture_formats_ = 0;
  uint64_t frame_ = 0;  ///< bumped by AdvanceFrame; gates frame-coherent resource reuse
};

/// D3DPT_* on the Xbox 360 is the raw Xenos PrimitiveType, which agrees with the
/// PC enum except that fan and strip are swapped. Returns 0 for primitive types
/// D3D9 has no equivalent for (rectangle lists, quads).
D3DPRIMITIVETYPE HostPrimitiveType(uint32_t xenos_primitive_type);

/// Primitive count for `index_count` indices of `xenos_primitive_type`.
uint32_t HostPrimitiveCount(uint32_t xenos_primitive_type, uint32_t index_count);

}  // namespace nx1::d3d9

#endif  // _WIN32
