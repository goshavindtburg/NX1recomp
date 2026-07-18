/**
 * @file    d3d9_resources.cpp
 * @brief   Guest -> host resource translation for the native D3D9 renderer.
 */

// rex headers must precede <windows.h> (pulled in transitively by d3d9.h):
// rex/thread.h declares Sleep(std::chrono::milliseconds), which the Win32
// Sleep macro otherwise mangles.
#include <rex/cvar.h>
#include <rex/graphics/pipeline/shader/shader.h>
#include <rex/graphics/pipeline/texture/conversion.h>
#include <rex/graphics/pipeline/texture/info.h>
#include <rex/graphics/pipeline/texture/util.h>
#include <rex/graphics/xenos.h>
#include <rex/logging/macros.h>
#include <rex/string/buffer.h>
#include <rex/system/kernel_state.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <cstring>
#include <unordered_map>
#include <vector>

#include <d3dcompiler.h>
#include <xxhash.h>

#include "d3d9_resources.h"
#include "guest_d3d.h"

REXCVAR_DEFINE_BOOL(nx1_d3d9_mips, true, "GPU",
                    "Give textures a mip chain, filtered down from level 0 by the driver. Off "
                    "leaves every minified surface aliasing (the coloured speckle).")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

// TEMP DIAGNOSTIC (no shadows)
// Polarity, anchored EMPIRICALLY this time (nx1_d3d9_dbg_shadow=16 screenshot, 2026-07-17):
// a tap is `sge r14, r4.zzzz, r14` => (frag_z >= atlas_z) ? 1 : 0, and on screen 1 renders as
// LIT (white dominates open ground; the black shapes are where geometry casts). Fragment
// depths are ~0.48 and the atlas background clears to 0, so a lookup that misses everything
// lands on 0 => z >= 0 => lit, a sensible default. Therefore:
//       0 (atlas = 0.0) => every tap 1 => fully lit  => looks like the status quo (null test)
//     100 (atlas = 1.0) => every tap 0 => fully SHADOWED => the world must DARKEN by exactly
//                          the sun term's visible contribution
//     200 => flicker 0.0 <-> 1.0 every 500 ms. The two constants bracket every fragment
//            depth, so all taps flip lit<->shadowed together: if the sun-shadow term reaches
//            the frame at all, the whole world pulses. Separate 0-vs-100 runs (different
//            scene, eyeballed stills) can hide a uniform dim; a pulse cannot be missed.
// This note has been rewritten twice with opposite conclusions derived from the microcode --
// trust the mode-16 screenshot, not re-derivation.
REXCVAR_DEFINE_INT32(nx1_d3d9_shadow_test, -1, "GPU",
                     "Debug: replace shadow-atlas depth with a constant (value/100). -1 = off, "
                     "0 = fully lit (null test), 100 = fully shadowed (world must darken), "
                     "200 = flicker between the two every 500 ms.");

REXCVAR_DEFINE_BOOL(nx1_d3d9_dump_shadow, false, "GPU",
                    "Debug: dump each shadow-atlas resolve to texdump/shadow_atlas_*.bmp.");

#ifdef _WIN32

namespace nx1::d3d9 {

namespace {

// xenos::VertexFormat
enum : uint32_t {
  kFmt_8_8_8_8 = 6,
  kFmt_2_10_10_10 = 7,
  kFmt_10_11_11 = 16,
  kFmt_11_11_10 = 17,
  kFmt_16_16 = 25,
  kFmt_16_16_16_16 = 26,
  kFmt_16_16_FLOAT = 31,
  kFmt_16_16_16_16_FLOAT = 32,
  kFmt_32 = 33,
  kFmt_32_32 = 34,
  kFmt_32_32_32_32 = 35,
  kFmt_32_FLOAT = 36,
  kFmt_32_32_FLOAT = 37,
  kFmt_32_32_32_32_FLOAT = 38,
  kFmt_32_32_32_FLOAT = 57,
};

struct HostFormat {
  D3DDECLTYPE type;
  uint8_t src_size;
  uint8_t dst_size;
  bool expand;
};

/// Pick the host input-assembler format for one guest element, or fail.
///
/// The "no expand" cases all share one property: the guest bytes become the host
/// bytes under the very dword byteswap the GPU's 8in32 endian mode would apply.
/// Xenos numbers a packed element's components from the *low* bits of the
/// byteswapped dword, which is precisely where D3D9 reads component 0 from.
bool PickHostFormat(uint32_t format, bool is_signed, bool is_normalized, HostFormat* out) {
  switch (format) {
    case kFmt_32_FLOAT:
      *out = {D3DDECLTYPE_FLOAT1, 4, 4, false};
      return true;
    case kFmt_32_32_FLOAT:
      *out = {D3DDECLTYPE_FLOAT2, 8, 8, false};
      return true;
    case kFmt_32_32_32_FLOAT:
      *out = {D3DDECLTYPE_FLOAT3, 12, 12, false};
      return true;
    case kFmt_32_32_32_32_FLOAT:
      *out = {D3DDECLTYPE_FLOAT4, 16, 16, false};
      return true;
    case kFmt_16_16_FLOAT:
      *out = {D3DDECLTYPE_FLOAT16_2, 4, 4, false};
      return true;
    case kFmt_16_16_16_16_FLOAT:
      *out = {D3DDECLTYPE_FLOAT16_4, 8, 8, false};
      return true;

    case kFmt_8_8_8_8:
      if (is_signed) {
        *out = {D3DDECLTYPE_FLOAT4, 4, 16, true};
      } else {
        *out = {is_normalized ? D3DDECLTYPE_UBYTE4N : D3DDECLTYPE_UBYTE4, 4, 4, false};
      }
      return true;

    case kFmt_16_16:
      if (is_signed) {
        *out = {is_normalized ? D3DDECLTYPE_SHORT2N : D3DDECLTYPE_SHORT2, 4, 4, false};
      } else if (is_normalized) {
        *out = {D3DDECLTYPE_USHORT2N, 4, 4, false};
      } else {
        *out = {D3DDECLTYPE_FLOAT2, 4, 8, true};
      }
      return true;

    case kFmt_16_16_16_16:
      if (is_signed) {
        *out = {is_normalized ? D3DDECLTYPE_SHORT4N : D3DDECLTYPE_SHORT4, 8, 8, false};
      } else if (is_normalized) {
        *out = {D3DDECLTYPE_USHORT4N, 8, 8, false};
      } else {
        *out = {D3DDECLTYPE_FLOAT4, 8, 16, true};
      }
      return true;

    // D3D9's DEC3N/UDEC3 are optional caps that most modern drivers report as
    // unsupported, and 10_11_11 has no equivalent at all. Decode to floats.
    case kFmt_2_10_10_10:
      *out = {D3DDECLTYPE_FLOAT4, 4, 16, true};
      return true;
    case kFmt_10_11_11:
    case kFmt_11_11_10:
      *out = {D3DDECLTYPE_FLOAT3, 4, 12, true};
      return true;

    // Integer vertex inputs: vs_3_0 has none.
    case kFmt_32:
      *out = {D3DDECLTYPE_FLOAT1, 4, 4, true};
      return true;
    case kFmt_32_32:
      *out = {D3DDECLTYPE_FLOAT2, 8, 8, true};
      return true;
    case kFmt_32_32_32_32:
      *out = {D3DDECLTYPE_FLOAT4, 16, 16, true};
      return true;

    default:
      return false;
  }
}

inline uint32_t Swap32(uint32_t v) { return __builtin_bswap32(v); }
inline uint16_t Swap16(uint16_t v) { return __builtin_bswap16(v); }

struct RawVertexUsage {
  uint8_t usage;        ///< D3DDECLUSAGE
  uint8_t usage_index;
};

/// Assign a host vertex semantic to one vfetch attribute, in vfetch-listing order.
///
/// This MUST mirror GetNx1RawVertexUsage in rexglue's shader.cpp exactly: that is
/// the function XenosRecomp used to name each shader's input registers, so the
/// host declaration only binds if we reproduce the same usage/index assignment.
RawVertexUsage Nx1RawVertexUsage(size_t ordinal, uint32_t format, uint32_t& packed_count,
                                 uint32_t& texcoord_count, uint32_t& color_count) {
  if (ordinal == 0) {
    return {D3DDECLUSAGE_POSITION, 0};
  }
  switch (format) {
    case kFmt_2_10_10_10:
    case kFmt_10_11_11:
    case kFmt_11_11_10:
      switch (packed_count++) {
        case 0:
          return {D3DDECLUSAGE_NORMAL, 0};
        case 1:
          return {D3DDECLUSAGE_TANGENT, 0};
        case 2:
          return {D3DDECLUSAGE_BINORMAL, 0};
        default:
          break;
      }
      break;
    case kFmt_8_8_8_8:
      return {D3DDECLUSAGE_COLOR, uint8_t(color_count++)};
    default:
      break;
  }
  return {D3DDECLUSAGE_TEXCOORD, uint8_t(texcoord_count++)};
}

//--- Texture formats --------------------------------------------------------

#ifndef MAKEFOURCC
#define MAKEFOURCC(a, b, c, d)                                                     \
  (uint32_t(uint8_t(a)) | (uint32_t(uint8_t(b)) << 8) | (uint32_t(uint8_t(c)) << 16) | \
   (uint32_t(uint8_t(d)) << 24))
#endif

// xenos::TextureFormat values NX1's materials actually use.
enum : uint32_t {
  kTex_8 = 2,
  kTex_1_5_5_5 = 3,
  kTex_5_6_5 = 4,
  kTex_8_8_8_8 = 6,
  kTex_8_A = 8,
  kTex_8_8 = 10,
  kTex_8_8_8_8_A = 14,
  kTex_4_4_4_4 = 15,
  kTex_DXT1 = 18,
  kTex_DXT2_3 = 19,
  kTex_DXT4_5 = 20,
  kTex_DXT3A = 58,
  kTex_DXT5A = 59,
  kTex_16_FLOAT = 30,
  kTex_16_16_FLOAT = 31,
  kTex_16_16_16_16_FLOAT = 32,
  kTex_DXN = 49,
  kTex_8_8_8_8_AS_16_16_16_16 = 50,
  kTex_DXT1_AS_16_16_16_16 = 51,
  kTex_DXT2_3_AS_16_16_16_16 = 52,
  kTex_DXT4_5_AS_16_16_16_16 = 53,
};

// Single-channel Xenos BC alpha formats have no D3D9 equivalent. The guest sets a
// RRRR fetch swizzle (replicate the one channel into all four) that we cannot
// reproduce on a fixed-function D3D9 sampler, so we CPU-decode them to A8R8G8B8
// with the value written into every channel -- then any shader swizzle reads it.
enum class TexDecode { kNone, kDXT3A, kDXT5A, kColorSwizzle };

struct HostTextureFormat {
  D3DFORMAT d3d;
  /// 32-bit RGBA source landing in A8R8G8B8: apply the fetch constant's channel swizzle
  /// after the endian swap (see MakeSwizzle32). The other formats' swizzles are the
  /// natural replicating ones -- (X,X,X,Y) for 8_8 into A8L8, (X,X,X,1) for 8 into L8 --
  /// which their host format already reproduces.
  bool swizzle32;
  bool valid;
  TexDecode decode = TexDecode::kNone;
  /// A block-compressed format the D3D9 runtime does not recognise as one (a FourCC it
  /// passes straight through to the driver). It sizes such a surface as if it were one byte
  /// per texel: LockRect reports `pitch == width` rather than the block row's
  /// `blocks_wide * bytes_per_block`, and the smallest levels get less storage than a single
  /// block. Both have to be worked around -- see the upload in GetTexture.
  bool opaque_block = false;
};

HostTextureFormat PickHostTextureFormat(uint32_t xenos_format) {
  switch (xenos_format) {
    case kTex_DXT1:
    case kTex_DXT1_AS_16_16_16_16:
      return {D3DFMT_DXT1, false, true};
    case kTex_DXT2_3:
    case kTex_DXT2_3_AS_16_16_16_16:
      return {D3DFMT_DXT3, false, true};
    case kTex_DXT4_5:
    case kTex_DXT4_5_AS_16_16_16_16:
      return {D3DFMT_DXT5, false, true};
    case kTex_DXN:  // BC5 / two-channel normal maps
      return {D3DFORMAT(MAKEFOURCC('A', 'T', 'I', '2')), false, true, TexDecode::kNone,
              /*opaque_block=*/true};
    case kTex_DXT3A:  // single-channel explicit-alpha BC -> decode + replicate
      return {D3DFMT_A8R8G8B8, false, true, TexDecode::kDXT3A};
    case kTex_DXT5A:  // single-channel interpolated-alpha BC -> decode + replicate
      return {D3DFMT_A8R8G8B8, false, true, TexDecode::kDXT5A};

    case kTex_8_8_8_8:
    case kTex_8_8_8_8_A:
    case kTex_8_8_8_8_AS_16_16_16_16:
      return {D3DFMT_A8R8G8B8, true, true};
    case kTex_5_6_5:
      return {D3DFMT_R5G6B5, false, true};
    case kTex_1_5_5_5:
      return {D3DFMT_A1R5G5B5, false, true};
    case kTex_4_4_4_4:
      return {D3DFMT_A4R4G4B4, false, true};
    case kTex_8:
    case kTex_8_A:
      return {D3DFMT_L8, false, true};
    case kTex_8_8:
      return {D3DFMT_A8L8, false, true};
    case kTex_16_FLOAT:
      return {D3DFMT_R16F, false, true};
    case kTex_16_16_FLOAT:
      return {D3DFMT_G16R16F, false, true};
    case kTex_16_16_16_16_FLOAT:
      return {D3DFMT_A16B16G16R16F, false, true};

    default:
      return {D3DFMT_UNKNOWN, false, false};
  }
}

inline uint32_t Log2Exact(uint32_t v) { return v ? __builtin_ctz(v) : 0; }

/// Can the driver filter a mip chain down from level 0 for this format?
///
/// This must be tested against D3D_OK exactly, *not* with SUCCEEDED(). When a format is usable
/// but the driver will not auto-generate its mips, CheckDeviceFormat returns D3DOK_NOAUTOGEN
/// (0x0008652B) -- which is a **success** code, so SUCCEEDED() is true for it. Reading that as
/// "supported" is how every format in the game came back supported while not one of them
/// actually got a chain: we created the textures with D3DUSAGE_AUTOGENMIPMAP, called
/// GenerateMipSubLevels(), and were silently handed a single level. The whole mip path was
/// inert, and every minified surface aliased into coloured speckle.
bool SupportsAutoMips(IDirect3DDevice9Ex* device, D3DFORMAT format) {
  static std::unordered_map<uint32_t, bool> cache;
  const auto it = cache.find(uint32_t(format));
  if (it != cache.end()) {
    return it->second;
  }
  HRESULT hr = E_FAIL;
  IDirect3D9* d3d = nullptr;
  D3DDEVICE_CREATION_PARAMETERS params{};
  D3DDISPLAYMODE mode{};
  if (SUCCEEDED(device->GetDirect3D(&d3d)) && d3d) {
    if (SUCCEEDED(device->GetCreationParameters(&params)) &&
        SUCCEEDED(d3d->GetAdapterDisplayMode(params.AdapterOrdinal, &mode))) {
      hr = d3d->CheckDeviceFormat(params.AdapterOrdinal, params.DeviceType, mode.Format,
                                  D3DUSAGE_AUTOGENMIPMAP, D3DRTYPE_TEXTURE, format);
    }
    d3d->Release();
  }
  const bool supported = hr == D3D_OK;
  cache.emplace(uint32_t(format), supported);
  REXGPU_INFO("nx1_d3d9: auto mip generation for host format 0x{:08X}: {} (hr {:#010x})",
              uint32_t(format), supported ? "supported" : "NOT supported (single level)",
              static_cast<uint32_t>(hr));
  return supported;
}

/// Read a big-endian dword out of a guest buffer.
inline uint32_t LoadBE32(const uint8_t* p) {
  uint32_t v;
  std::memcpy(&v, p, sizeof(v));
  return Swap32(v);
}

inline float Snorm(uint32_t bits, uint32_t width) {
  const int32_t v = int32_t(bits << (32 - width)) >> (32 - width);
  const float f = float(v) / float((1u << (width - 1)) - 1u);
  // Xenos' default signed-repeating-fraction mode clamps -max to exactly -1.
  return f < -1.0f ? -1.0f : f;
}

inline float Unorm(uint32_t bits, uint32_t width) {
  return float(bits) / float((1u << width) - 1u);
}

/// Decode one packed element into `out[0..3]`.
void Expand(const uint8_t* src, const ConvertOp& op, float out[4]) {
  out[0] = out[1] = out[2] = 0.0f;
  out[3] = 1.0f;

  uint32_t offsets[4] = {0, 0, 0, 0};
  uint32_t widths[4] = {0, 0, 0, 0};
  uint32_t components = 0;

  switch (op.format) {
    case kFmt_2_10_10_10:
      components = 4;
      widths[0] = widths[1] = widths[2] = 10;
      widths[3] = 2;
      offsets[1] = 10;
      offsets[2] = 20;
      offsets[3] = 30;
      break;
    case kFmt_10_11_11:
      components = 3;
      widths[0] = widths[1] = 11;
      widths[2] = 10;
      offsets[1] = 11;
      offsets[2] = 22;
      break;
    case kFmt_11_11_10:
      components = 3;
      widths[0] = 10;
      widths[1] = widths[2] = 11;
      offsets[1] = 10;
      offsets[2] = 21;
      break;
    case kFmt_8_8_8_8:
      components = 4;
      widths[0] = widths[1] = widths[2] = widths[3] = 8;
      offsets[1] = 8;
      offsets[2] = 16;
      offsets[3] = 24;
      break;
    case kFmt_16_16:
    case kFmt_16_16_16_16: {
      const uint32_t n = op.format == kFmt_16_16 ? 2 : 4;
      for (uint32_t i = 0; i < n; ++i) {
        const uint32_t dword = LoadBE32(src + (i / 2) * 4);
        const uint32_t bits = (dword >> ((i & 1) * 16)) & 0xFFFF;
        out[i] = op.is_signed ? (op.is_normalized ? Snorm(bits, 16) : float(int16_t(bits)))
                              : (op.is_normalized ? Unorm(bits, 16) : float(bits));
      }
      return;
    }
    case kFmt_32:
    case kFmt_32_32:
    case kFmt_32_32_32_32: {
      const uint32_t n = op.src_size / 4;
      for (uint32_t i = 0; i < n; ++i) {
        const uint32_t bits = LoadBE32(src + i * 4);
        out[i] = op.is_signed ? float(int32_t(bits)) : float(bits);
      }
      return;
    }
    default:
      return;
  }

  const uint32_t dword = LoadBE32(src);
  for (uint32_t i = 0; i < components; ++i) {
    const uint32_t bits = (dword >> offsets[i]) & ((1u << widths[i]) - 1u);
    out[i] = op.is_signed ? (op.is_normalized ? Snorm(bits, widths[i])
                                              : float(int32_t(bits << (32 - widths[i])) >>
                                                      (32 - widths[i])))
                          : (op.is_normalized ? Unorm(bits, widths[i]) : float(bits));
  }
}

/// Byteswap `bytes` (a multiple of 4) of big-endian data into `dst`.
void SwapDwords(uint8_t* dst, const uint8_t* src, size_t bytes) {
  for (size_t i = 0; i < bytes; i += 4) {
    const uint32_t v = LoadBE32(src + i);
    std::memcpy(dst + i, &v, sizeof(v));
  }
}

const uint8_t* TranslatePhysical(uint32_t guest_physical) {
  auto* memory = rex::system::kernel_state()->memory();
  return memory->TranslatePhysical<const uint8_t*>(guest_physical);
}

// For CPU-side guest pointers (UP-draw vertex/index scratch), which are full
// effective addresses in the physical-mirror window (0xE0..) rather than raw GPU
// fetch addresses. TranslateVirtual folds in the heap host_address_offset -- the
// 0x1000 page offset TranslatePhysical drops on the 0xE0 heap on Windows.
const uint8_t* TranslateGuestPointer(uint32_t guest_addr) {
  auto* memory = rex::system::kernel_state()->memory();
  return memory->TranslateVirtual<const uint8_t*>(guest_addr);
}

uint64_t MixKey(uint64_t a, uint64_t b) {
  uint64_t h = a * 0x9E3779B97F4A7C15ull;
  h ^= b + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
  return h;
}

/// Hash everything about a layout that changes the bytes it produces -- and nothing about
/// which shader or declaration it came from. See VertexLayout::content_key.
uint64_t LayoutContentKey(const VertexLayout& layout) {
  uint64_t h = MixKey(0x5644656C6C61796Full, layout.stream_count);
  for (uint32_t s = 0; s < kMaxHostStreams; ++s) {
    h = MixKey(h, (uint64_t(layout.guest_stride[s]) << 32) | layout.host_stride[s]);
    h = MixKey(h, layout.bulk_swap[s] ? 1 : 0);
  }
  for (const ConvertOp& op : layout.ops) {
    h = MixKey(h, (uint64_t(op.src_offset) << 48) | (uint64_t(op.dst_offset) << 32) |
                      (uint64_t(op.stream) << 24) | (uint64_t(op.format) << 16) |
                      (uint64_t(op.src_size) << 8) | op.dst_size);
    h = MixKey(h, (op.is_signed ? 1u : 0u) | (op.is_normalized ? 2u : 0u) | (op.expand ? 4u : 0u));
  }
  return h;
}

// last_frame gates frame-coherent reuse: a resource bound by many draws in one
// frame is hashed/validated only on its first use that frame, not per draw.
struct VertexBufferEntry {
  IDirect3DVertexBuffer9* vb = nullptr;
  uint32_t bytes = 0;
  uint32_t vertex_count = 0;
  uint64_t content_hash = 0;
  uint64_t last_frame = 0;
};

struct IndexBufferEntry {
  IDirect3DIndexBuffer9* ib = nullptr;
  uint32_t bytes = 0;
  uint32_t index_size = 0;
  uint64_t content_hash = 0;
  uint64_t last_frame = 0;
  /// Host-order copy of the indices. A draw's fetch constant covers the whole pooled
  /// vertex buffer, not the model in it, so the only way to know how many vertices a draw
  /// actually touches is to look at its indices -- and scanning this beats re-reading
  /// big-endian guest memory.
  std::vector<uint8_t> cpu;
};

/// Memoized "highest index this draw range references", so the scan happens once per
/// distinct draw rather than once per frame per draw.
using IndexRangeMap = std::unordered_map<uint64_t, uint32_t>;

struct TextureEntry {
  IDirect3DTexture9* tex = nullptr;
  /// Volume textures are a separate D3D9 type. The composite's colour-grading LUT is a 3D
  /// texture and its tone-map curve a 1D one; leaving either unbound samples black, which
  /// blacks out the whole composited frame.
  IDirect3DVolumeTexture9* vol = nullptr;
  /// Cube maps (environment/reflection), a third D3D9 type again.
  IDirect3DCubeTexture9* cube = nullptr;
  uint64_t layout_key = 0;  ///< base address + format + dims + mips; a change rebuilds it
  uint64_t last_frame = 0;
  /// Content hash for the cube/volume paths, which still re-read on a content change. The 2D path
  /// decodes from the CPU mirror (MirrorSnapshot) and re-reads only when the write-watch fires.
  uint64_t content_hash = 0;
  /// Write-watch state. We snapshot a texture once and re-read its guest memory only when the guest
  /// actually writes it (dirty set by DrainMemoryWrites from the invalidation callback). watch_addr/
  /// size record the physical range so an invalidation can be matched back to this entry.
  bool dirty = false;
  uint32_t watch_addr = 0;
  uint32_t watch_size = 0;
  /// Commit-and-freeze. While a texture is on screen its slot holds valid bytes, and the write-watch
  /// keeps the decode tracking them (clean up close). But when it goes non-resident the streaming
  /// pool recycles its slot, dribbling high-noise garbage into the exact bytes -- and re-reading that
  /// is the confetti-on-backup. A texture drawn cleanly for kCommitFrames is settled, so we commit
  /// it: stop honouring writes and hold the good decode. good_frames counts frames it has been drawn.
  uint32_t good_frames = 0;
  bool committed = false;
};

struct ResolvedTarget {
  IDirect3DTexture9* tex = nullptr;
  uint32_t width = 0;
  uint32_t height = 0;
  bool owned = true;  ///< false when it aliases a target we don't own (a depth texture)
};

struct SurfaceSize {
  uint32_t width = 0;
  uint32_t height = 0;
};

/// One host render target standing in for a guest D3DSurface.
struct HostTarget {
  IDirect3DTexture9* tex = nullptr;      ///< null when this aliases the backbuffer
  IDirect3DSurface9* surface = nullptr;  ///< the bindable surface
  uint32_t width = 0;
  uint32_t height = 0;
  bool is_depth = false;
  bool is_backbuffer = false;
};

/// D3DFMT_INTZ: a depth-stencil format that can also be sampled as a texture. This is
/// how a D3D9 renderer gets the shadow maps and the scene depth back into a shader.
#ifndef D3DFMT_INTZ
#define D3DFMT_INTZ (D3DFORMAT(MAKEFOURCC('I', 'N', 'T', 'Z')))
#endif

using LayoutMap = std::unordered_map<uint64_t, VertexLayout>;
using VertexBufferMap = std::unordered_map<uint64_t, VertexBufferEntry>;
/// Guest-memory hash of a vertex buffer, memoized for the frame it was taken in.
struct VertexHash {
  uint64_t frame = 0;  ///< frame_ + 1, so a default-constructed entry never matches frame 0
  uint64_t hash = 0;
};
using VertexHashMap = std::unordered_map<uint64_t, VertexHash>;
using IndexBufferMap = std::unordered_map<uint64_t, IndexBufferEntry>;
using TextureMap = std::unordered_map<uint64_t, TextureEntry>;

/// The largest-resolution texture a geometry surface has bound on a given sampler. `tex` is AddRef'd
/// so it outlives eviction of the base-keyed TextureMap entry it came from. See PreferLargestForSurface.
struct BestTexture {
  IDirect3DBaseTexture9* tex = nullptr;
  uint32_t area = 0;  ///< width * height of the retained texture
  uint64_t last_frame = 0;
};
using BestTextureMap = std::unordered_map<uint64_t, BestTexture>;

using ResolveMap = std::unordered_map<uint64_t, ResolvedTarget>;
/// Keyed by EDRAM region, not by D3DSurface pointer -- see ReadSurfaceEdramKey.
using TargetMap = std::unordered_map<uint64_t, HostTarget>;

/// Physical page behind a guest address. The 360 maps the same RAM through several
/// virtual windows, and a resolve destination is not written and read through the same
/// one: the engine hands D3DDevice_Resolve a texture whose header base sits in the
/// uncached 0xA0000000 window (0xBF6D8000 for the scene depth), then samples that exact
/// same surface through a fetch constant holding the physical address (0x1F6D8000).
/// Keying the resolve map by the raw address therefore misses on every read-back -- the
/// lighting shaders sample raw untiled memory instead of the scene, the depth buffer and
/// the shadow maps. Key by the physical page and both windows agree.
constexpr uint32_t PhysicalAddress(uint32_t address) { return address & 0x1FFFFFFF; }

/// Untile (or straight-copy) one 2D mip into `dst`, tightly packed at
/// `dst_row_bytes` per block-row, applying the fetch constant's endian swap.
/// `src` points at the level's base in guest memory.
///
/// `offset_x`/`offset_y` are the level's origin *within* that image, in blocks. They are
/// non-zero only for a packed mip tail: once a level shrinks to 16 texels or less, Xenos
/// stops giving it its own image and parks it alongside its siblings inside one 32x32-block
/// tile (see TextureInfo::GetPackedTileOffset).
void DetileMip2D(uint8_t* dst, uint32_t dst_row_bytes, const uint8_t* src,
                 const rex::graphics::TextureExtent& extent, uint32_t bytes_per_block,
                 uint32_t endian, bool tiled, uint32_t offset_x = 0, uint32_t offset_y = 0) {
  namespace tu = rex::graphics::texture_util;
  namespace tc = rex::graphics::texture_conversion;
  const auto xe_endian = static_cast<rex::graphics::xenos::Endian>(endian);
  const uint32_t blocks_wide = extent.block_width;
  const uint32_t blocks_high = extent.block_height;

  if (tiled) {
    const uint32_t pitch_blocks = extent.block_pitch_h;
    const uint32_t bpb_log2 = Log2Exact(bytes_per_block);
    for (uint32_t by = 0; by < blocks_high; ++by) {
      uint8_t* dst_row = dst + size_t(by) * dst_row_bytes;
      for (uint32_t bx = 0; bx < blocks_wide; ++bx) {
        const int32_t src_off = tu::GetTiledOffset2D(int32_t(offset_x + bx), int32_t(offset_y + by),
                                                     pitch_blocks, bpb_log2);
        tc::CopySwapBlock(xe_endian, dst_row + size_t(bx) * bytes_per_block, src + src_off,
                          bytes_per_block);
      }
    }
  } else {
    // Linear: block rows are 256-byte aligned in guest memory.
    const uint32_t src_row_bytes = extent.block_pitch_h * bytes_per_block;
    const uint32_t row_copy = blocks_wide * bytes_per_block;
    const size_t src_origin = size_t(offset_y) * src_row_bytes + size_t(offset_x) * bytes_per_block;
    for (uint32_t by = 0; by < blocks_high; ++by) {
      tc::CopySwapBlock(xe_endian, dst + size_t(by) * dst_row_bytes,
                        src + src_origin + size_t(by) * src_row_bytes, row_copy);
    }
  }
}

/// Untile one 3D mip slice-by-slice into `dst`, which is laid out as `depth` slices of
/// `slice_bytes`, each `row_bytes` per block-row. Xenos tiles 3D textures in 32x32x4 blocks,
/// so a slice is not simply a 2D mip -- GetTiledOffset3D walks it.
void DetileMip3D(uint8_t* dst, uint32_t row_bytes, uint32_t slice_bytes, const uint8_t* src,
                 uint32_t blocks_wide, uint32_t blocks_high, uint32_t depth, uint32_t pitch_blocks,
                 uint32_t bytes_per_block, uint32_t endian, bool tiled) {
  namespace tu = rex::graphics::texture_util;
  namespace tc = rex::graphics::texture_conversion;
  const auto xe_endian = static_cast<rex::graphics::xenos::Endian>(endian);
  const uint32_t bpb_log2 = Log2Exact(bytes_per_block);

  for (uint32_t z = 0; z < depth; ++z) {
    uint8_t* slice = dst + size_t(z) * slice_bytes;
    for (uint32_t by = 0; by < blocks_high; ++by) {
      uint8_t* row = slice + size_t(by) * row_bytes;
      for (uint32_t bx = 0; bx < blocks_wide; ++bx) {
        const size_t src_off =
            tiled ? size_t(tu::GetTiledOffset3D(int32_t(bx), int32_t(by), int32_t(z), pitch_blocks,
                                                blocks_high, bpb_log2))
                  : ((size_t(z) * blocks_high + by) * pitch_blocks + bx) * bytes_per_block;
        tc::CopySwapBlock(xe_endian, row + size_t(bx) * bytes_per_block, src + src_off,
                          bytes_per_block);
      }
    }
  }
}

/// Decode linear 4x4 single-channel BC-alpha blocks (DXT3A explicit 4-bit, or
/// DXT5A interpolated 3-bit) into an A8R8G8B8 destination, replicating the decoded
/// value into R=G=B=A. `src` is tightly packed blocks (8 bytes each) at
/// `blocks_wide` per row; `width`/`height` bound the visible texels written.
void DecodeBCAlphaToArgb(uint8_t* dst, uint32_t dst_pitch, const uint8_t* src, uint32_t blocks_wide,
                         uint32_t blocks_high, uint32_t width, uint32_t height, bool interpolated) {
  for (uint32_t by = 0; by < blocks_high; ++by) {
    for (uint32_t bx = 0; bx < blocks_wide; ++bx) {
      const uint8_t* blk = src + (size_t(by) * blocks_wide + bx) * 8;
      uint8_t a[16];
      if (interpolated) {
        // DXT5 alpha block: two endpoints + 16 x 3-bit palette indices (LE).
        const uint32_t a0 = blk[0], a1 = blk[1];
        uint8_t pal[8];
        pal[0] = uint8_t(a0);
        pal[1] = uint8_t(a1);
        if (a0 > a1) {
          for (uint32_t i = 1; i <= 6; ++i) pal[i + 1] = uint8_t(((7 - i) * a0 + i * a1) / 7);
        } else {
          for (uint32_t i = 1; i <= 4; ++i) pal[i + 1] = uint8_t(((5 - i) * a0 + i * a1) / 5);
          pal[6] = 0;
          pal[7] = 255;
        }
        uint64_t bits = 0;
        for (uint32_t i = 0; i < 6; ++i) bits |= uint64_t(blk[2 + i]) << (8 * i);
        for (uint32_t i = 0; i < 16; ++i) a[i] = pal[(bits >> (3 * i)) & 0x7];
      } else {
        // DXT3 explicit alpha: 16 x 4-bit, two texels per byte, low nibble first.
        for (uint32_t i = 0; i < 8; ++i) {
          a[i * 2 + 0] = uint8_t((blk[i] & 0x0F) * 17);
          a[i * 2 + 1] = uint8_t((blk[i] >> 4) * 17);
        }
      }
      for (uint32_t ty = 0; ty < 4; ++ty) {
        const uint32_t py = by * 4 + ty;
        if (py >= height) break;
        uint8_t* row = dst + size_t(py) * dst_pitch;
        for (uint32_t tx = 0; tx < 4; ++tx) {
          const uint32_t px = bx * 4 + tx;
          if (px >= width) break;
          const uint8_t v = a[ty * 4 + tx];
          uint8_t* p = row + size_t(px) * 4;
          p[0] = v;  // B
          p[1] = v;  // G
          p[2] = v;  // R
          p[3] = v;  // A
        }
      }
    }
  }
}

//=============================================================================
// Mip-chain generation for the block-compressed formats
//
// The driver will not do it. CheckDeviceFormat answers D3DOK_NOAUTOGEN for DXT1, DXT3, DXT5
// and ATI2 -- only the uncompressed formats get a chain out of D3DUSAGE_AUTOGENMIPMAP. Since
// block compression covers every world albedo, normal and specular map in the game, that left
// the entire world sampling level 0 at any distance: undersampled, aliasing into the coloured
// speckle that no amount of sampler state could fix, because there was no second level to
// select. (D3DOK_NOAUTOGEN is a *success* code, so a SUCCEEDED() probe reports every format as
// supported -- see SupportsAutoMips.)
//
// So decode level 0, box-filter it down, and re-encode each level. The re-encode is a plain
// min/max endpoint fit rather than a rate-distortion search: these are the minified levels,
// each already an average of four texels, and level 0 is passed through untouched -- so the
// error lands where nothing can see it.
//=============================================================================

struct Rgba8 {
  uint8_t r, g, b, a;
};

const D3DFORMAT kFmtAti2 = D3DFORMAT(MAKEFOURCC('A', 'T', 'I', '2'));

bool IsBlockCompressed(D3DFORMAT f) {
  return f == D3DFMT_DXT1 || f == D3DFMT_DXT3 || f == D3DFMT_DXT5 || f == kFmtAti2;
}

uint32_t BcBlockBytes(D3DFORMAT f) { return f == D3DFMT_DXT1 ? 8u : 16u; }

/// How many levels to build. The chain stops at 4x4 rather than 1x1: a BC level smaller than
/// one block is where the ATI2 pitch trap bites (the runtime sizes an unrecognised FourCC at a
/// byte per texel, so a 2x2 level gets 4 bytes to hold a 16-byte block, and writing it corrupts
/// the heap). Nothing is lost -- a 4x4 mip is already a single flat colour.
uint32_t BcMipLevels(uint32_t width, uint32_t height) {
  uint32_t levels = 1;
  uint32_t w = width, h = height;
  while (w > 4 && h > 4) {
    w /= 2;
    h /= 2;
    ++levels;
  }
  return levels;
}

/// The 4-colour palette of a BC1 colour block. `punchthrough` is only true for DXT1, where
/// c0 <= c1 selects a 3-colour block plus transparent black; the colour block embedded in
/// DXT3/DXT5 is always 4-colour.
void Bc1Palette(const uint8_t* blk, bool punchthrough, Rgba8 pal[4]) {
  const uint32_t c0 = uint32_t(blk[0]) | (uint32_t(blk[1]) << 8);
  const uint32_t c1 = uint32_t(blk[2]) | (uint32_t(blk[3]) << 8);
  auto expand = [](uint32_t c) {
    Rgba8 o;
    o.r = uint8_t((((c >> 11) & 0x1F) * 255 + 15) / 31);
    o.g = uint8_t((((c >> 5) & 0x3F) * 255 + 31) / 63);
    o.b = uint8_t(((c & 0x1F) * 255 + 15) / 31);
    o.a = 255;
    return o;
  };
  pal[0] = expand(c0);
  pal[1] = expand(c1);
  if (c0 > c1 || !punchthrough) {
    pal[2] = {uint8_t((2 * pal[0].r + pal[1].r) / 3), uint8_t((2 * pal[0].g + pal[1].g) / 3),
              uint8_t((2 * pal[0].b + pal[1].b) / 3), 255};
    pal[3] = {uint8_t((pal[0].r + 2 * pal[1].r) / 3), uint8_t((pal[0].g + 2 * pal[1].g) / 3),
              uint8_t((pal[0].b + 2 * pal[1].b) / 3), 255};
  } else {
    pal[2] = {uint8_t((pal[0].r + pal[1].r) / 2), uint8_t((pal[0].g + pal[1].g) / 2),
              uint8_t((pal[0].b + pal[1].b) / 2), 255};
    pal[3] = {0, 0, 0, 0};
  }
}

/// The 8-entry palette of a BC4 block (DXT5's alpha half, and each half of ATI2).
void Bc4Palette(const uint8_t* blk, uint8_t pal[8]) {
  const uint32_t a0 = blk[0], a1 = blk[1];
  pal[0] = uint8_t(a0);
  pal[1] = uint8_t(a1);
  if (a0 > a1) {
    for (uint32_t i = 1; i <= 6; ++i) pal[i + 1] = uint8_t(((7 - i) * a0 + i * a1) / 7);
  } else {
    for (uint32_t i = 1; i <= 4; ++i) pal[i + 1] = uint8_t(((5 - i) * a0 + i * a1) / 5);
    pal[6] = 0;
    pal[7] = 255;
  }
}

void DecodeBc4Block(const uint8_t* blk, uint8_t out[16]) {
  uint8_t pal[8];
  Bc4Palette(blk, pal);
  uint64_t bits = 0;
  for (uint32_t i = 0; i < 6; ++i) bits |= uint64_t(blk[2 + i]) << (8 * i);
  for (uint32_t i = 0; i < 16; ++i) out[i] = pal[(bits >> (3 * i)) & 0x7];
}

/// Decode one 4x4 block of any of the four BC formats into RGBA. ATI2 carries two channels
/// (X in red, Y in green); the encoder below only reads those back, so the rest is filler.
void DecodeBcBlock(D3DFORMAT fmt, const uint8_t* blk, Rgba8 out[16]) {
  if (fmt == kFmtAti2) {
    uint8_t x[16], y[16];
    DecodeBc4Block(blk, x);
    DecodeBc4Block(blk + 8, y);
    for (uint32_t i = 0; i < 16; ++i) out[i] = {x[i], y[i], 0, 255};
    return;
  }
  const uint8_t* color = fmt == D3DFMT_DXT1 ? blk : blk + 8;
  Rgba8 pal[4];
  Bc1Palette(color, /*punchthrough=*/fmt == D3DFMT_DXT1, pal);
  uint32_t bits = 0;
  for (uint32_t i = 0; i < 4; ++i) bits |= uint32_t(color[4 + i]) << (8 * i);
  for (uint32_t i = 0; i < 16; ++i) out[i] = pal[(bits >> (2 * i)) & 0x3];

  if (fmt == D3DFMT_DXT3) {
    for (uint32_t i = 0; i < 8; ++i) {
      out[i * 2 + 0].a = uint8_t((blk[i] & 0x0F) * 17);
      out[i * 2 + 1].a = uint8_t((blk[i] >> 4) * 17);
    }
  } else if (fmt == D3DFMT_DXT5) {
    uint8_t a[16];
    DecodeBc4Block(blk, a);
    for (uint32_t i = 0; i < 16; ++i) out[i].a = a[i];
  }
}

/// Decode a whole BC image into RGBA. `row_bytes` is the stride of a *block* row.
void DecodeBcImage(D3DFORMAT fmt, const uint8_t* src, uint32_t row_bytes, uint32_t width,
                   uint32_t height, std::vector<Rgba8>& out) {
  const uint32_t bpb = BcBlockBytes(fmt);
  const uint32_t bw = (width + 3) / 4;
  const uint32_t bh = (height + 3) / 4;
  out.assign(size_t(width) * height, Rgba8{0, 0, 0, 255});
  Rgba8 texels[16];
  for (uint32_t by = 0; by < bh; ++by) {
    for (uint32_t bx = 0; bx < bw; ++bx) {
      DecodeBcBlock(fmt, src + size_t(by) * row_bytes + size_t(bx) * bpb, texels);
      for (uint32_t ty = 0; ty < 4; ++ty) {
        const uint32_t py = by * 4 + ty;
        if (py >= height) break;
        for (uint32_t tx = 0; tx < 4; ++tx) {
          const uint32_t px = bx * 4 + tx;
          if (px >= width) break;
          out[size_t(py) * width + px] = texels[ty * 4 + tx];
        }
      }
    }
  }
}

/// Decode a block-compressed COLOUR texture (BC1/2/3) into A8R8G8B8 while applying the fetch
/// constant's channel swizzle. A compressed host texture samples fixed RGBA, so it cannot honour a
/// swizzle that broadcasts one channel to RGB -- which is how the smoke/effect sprites are stored
/// (luminance in one channel, mask in another, e.g. swizzle 0x200 = R,G,B<-red, A<-green). Those
/// otherwise render as a single-channel, always-opaque square. `bc_fmt`/`bc_bytes` describe the
/// source blocks; component index 4 selects constant 0, 5 selects constant 1.
void DecodeBcColorSwizzledToArgb(uint8_t* dst, uint32_t dst_pitch, const uint8_t* src,
                                 uint32_t blocks_wide, uint32_t blocks_high, uint32_t width,
                                 uint32_t height, D3DFORMAT bc_fmt, uint32_t bc_bytes,
                                 uint32_t swizzle) {
  const uint8_t sel[4] = {uint8_t(swizzle & 7), uint8_t((swizzle >> 3) & 7),
                          uint8_t((swizzle >> 6) & 7), uint8_t((swizzle >> 9) & 7)};
  for (uint32_t by = 0; by < blocks_high; ++by) {
    for (uint32_t bx = 0; bx < blocks_wide; ++bx) {
      Rgba8 tex[16];
      DecodeBcBlock(bc_fmt, src + (size_t(by) * blocks_wide + bx) * bc_bytes, tex);
      for (uint32_t ty = 0; ty < 4; ++ty) {
        const uint32_t py = by * 4 + ty;
        if (py >= height) break;
        uint8_t* row = dst + size_t(py) * dst_pitch;
        for (uint32_t tx = 0; tx < 4; ++tx) {
          const uint32_t px = bx * 4 + tx;
          if (px >= width) break;
          const Rgba8& c = tex[ty * 4 + tx];
          const uint8_t comp[8] = {c.r, c.g, c.b, c.a, 0, 255, 0, 0};
          uint8_t* p = row + size_t(px) * 4;
          p[0] = comp[sel[2]];  // B <- swizzle.z
          p[1] = comp[sel[1]];  // G <- swizzle.y
          p[2] = comp[sel[0]];  // R <- swizzle.x
          p[3] = comp[sel[3]];  // A <- swizzle.w
        }
      }
    }
  }
}

uint16_t Pack565(Rgba8 c) {
  return uint16_t(((c.r >> 3) << 11) | ((c.g >> 2) << 5) | (c.b >> 3));
}

/// Fit a BC1 colour block: the endpoints are the per-channel min and max of the 16 texels,
/// and each texel takes the nearest of the four palette entries. Endpoints are forced into
/// c0 >= c1 so the block always decodes in 4-colour mode -- a 3-colour block would introduce
/// transparent texels that were never in the source.
void EncodeBc1Color(const Rgba8 in[16], uint8_t* dst) {
  Rgba8 lo = in[0], hi = in[0];
  for (uint32_t i = 1; i < 16; ++i) {
    lo.r = std::min(lo.r, in[i].r); hi.r = std::max(hi.r, in[i].r);
    lo.g = std::min(lo.g, in[i].g); hi.g = std::max(hi.g, in[i].g);
    lo.b = std::min(lo.b, in[i].b); hi.b = std::max(hi.b, in[i].b);
  }
  uint16_t c0 = Pack565(hi);
  uint16_t c1 = Pack565(lo);
  if (c0 < c1) std::swap(c0, c1);
  dst[0] = uint8_t(c0 & 0xFF);
  dst[1] = uint8_t(c0 >> 8);
  dst[2] = uint8_t(c1 & 0xFF);
  dst[3] = uint8_t(c1 >> 8);

  Rgba8 pal[4];
  Bc1Palette(dst, /*punchthrough=*/false, pal);
  uint32_t bits = 0;
  for (uint32_t i = 0; i < 16; ++i) {
    uint32_t best = 0, best_err = ~0u;
    for (uint32_t p = 0; p < 4; ++p) {
      const int dr = int(in[i].r) - int(pal[p].r);
      const int dg = int(in[i].g) - int(pal[p].g);
      const int db = int(in[i].b) - int(pal[p].b);
      const uint32_t err = uint32_t(dr * dr + dg * dg + db * db);
      if (err < best_err) {
        best_err = err;
        best = p;
      }
    }
    bits |= best << (2 * i);
  }
  for (uint32_t i = 0; i < 4; ++i) dst[4 + i] = uint8_t((bits >> (8 * i)) & 0xFF);
}

/// Fit a BC4 block over one channel, selected by `channel` (0 = R, 1 = G, 3 = A).
void EncodeBc4(const Rgba8 in[16], uint32_t channel, uint8_t* dst) {
  uint8_t v[16];
  for (uint32_t i = 0; i < 16; ++i) {
    const uint8_t* c = &in[i].r;
    v[i] = c[channel];
  }
  uint8_t lo = v[0], hi = v[0];
  for (uint32_t i = 1; i < 16; ++i) {
    lo = std::min(lo, v[i]);
    hi = std::max(hi, v[i]);
  }
  dst[0] = hi;  // a0 > a1 selects the 8-value interpolated mode
  dst[1] = lo;
  uint8_t pal[8];
  Bc4Palette(dst, pal);
  uint64_t bits = 0;
  for (uint32_t i = 0; i < 16; ++i) {
    uint32_t best = 0, best_err = ~0u;
    for (uint32_t p = 0; p < 8; ++p) {
      const int d = int(v[i]) - int(pal[p]);
      const uint32_t err = uint32_t(d * d);
      if (err < best_err) {
        best_err = err;
        best = p;
      }
    }
    bits |= uint64_t(best) << (3 * i);
  }
  for (uint32_t i = 0; i < 6; ++i) dst[2 + i] = uint8_t((bits >> (8 * i)) & 0xFF);
}

void EncodeBcBlock(D3DFORMAT fmt, const Rgba8 in[16], uint8_t* dst) {
  if (fmt == kFmtAti2) {
    EncodeBc4(in, 0, dst);      // X
    EncodeBc4(in, 1, dst + 8);  // Y
    return;
  }
  if (fmt == D3DFMT_DXT3) {
    for (uint32_t i = 0; i < 8; ++i) {
      dst[i] = uint8_t((in[i * 2 + 0].a >> 4) | (in[i * 2 + 1].a & 0xF0));
    }
  } else if (fmt == D3DFMT_DXT5) {
    EncodeBc4(in, 3, dst);
  }
  EncodeBc1Color(in, fmt == D3DFMT_DXT1 ? dst : dst + 8);
}

/// Encode an RGBA image back to BC. Texels past the edge of a partial block repeat the last
/// real one, so the fit is never dragged towards whatever the padding held.
void EncodeBcImage(D3DFORMAT fmt, const std::vector<Rgba8>& src, uint32_t width, uint32_t height,
                   uint8_t* dst, uint32_t row_bytes) {
  const uint32_t bpb = BcBlockBytes(fmt);
  const uint32_t bw = (width + 3) / 4;
  const uint32_t bh = (height + 3) / 4;
  Rgba8 texels[16];
  for (uint32_t by = 0; by < bh; ++by) {
    for (uint32_t bx = 0; bx < bw; ++bx) {
      for (uint32_t ty = 0; ty < 4; ++ty) {
        const uint32_t py = std::min(by * 4 + ty, height - 1);
        for (uint32_t tx = 0; tx < 4; ++tx) {
          const uint32_t px = std::min(bx * 4 + tx, width - 1);
          texels[ty * 4 + tx] = src[size_t(py) * width + px];
        }
      }
      EncodeBcBlock(fmt, texels, dst + size_t(by) * row_bytes + size_t(bx) * bpb);
    }
  }
}

/// Halve an RGBA image with a 2x2 box filter (odd dimensions repeat the last row/column).
void BoxFilterHalf(const std::vector<Rgba8>& src, uint32_t sw, uint32_t sh,
                   std::vector<Rgba8>& dst, uint32_t dw, uint32_t dh) {
  dst.resize(size_t(dw) * dh);
  for (uint32_t y = 0; y < dh; ++y) {
    const uint32_t y0 = std::min(y * 2, sh - 1);
    const uint32_t y1 = std::min(y * 2 + 1, sh - 1);
    for (uint32_t x = 0; x < dw; ++x) {
      const uint32_t x0 = std::min(x * 2, sw - 1);
      const uint32_t x1 = std::min(x * 2 + 1, sw - 1);
      const Rgba8& a = src[size_t(y0) * sw + x0];
      const Rgba8& b = src[size_t(y0) * sw + x1];
      const Rgba8& c = src[size_t(y1) * sw + x0];
      const Rgba8& d = src[size_t(y1) * sw + x1];
      dst[size_t(y) * dw + x] = {
          uint8_t((uint32_t(a.r) + b.r + c.r + d.r + 2) / 4),
          uint8_t((uint32_t(a.g) + b.g + c.g + d.g + 2) / 4),
          uint8_t((uint32_t(a.b) + b.b + c.b + d.b + 2) / 4),
          uint8_t((uint32_t(a.a) + b.a + c.a + d.a + 2) / 4),
      };
    }
  }
}

/// The per-texel byte permutation a fetch constant's channel swizzle implies, for a
/// 32-bit format landing in D3DFMT_A8R8G8B8.
///
/// After the endian swap a Xenos texel's bytes are its components in order: [X][Y][Z][W].
/// D3DFMT_A8R8G8B8 reads that same memory as [B][G][R][A]. The swizzle says where each
/// output channel comes from -- R = comp[swz.x], G = comp[swz.y], B = comp[swz.z],
/// A = comp[swz.w] -- so the destination byte for R (index 2) takes source byte swz.x,
/// and so on. Component values 4 and 5 are the constants 0 and 1.
///
/// This is *not* a fixed red/blue swap. NX1's textures are overwhelmingly swizzle 0x60A
/// (R<-Z, G<-Y, B<-X), which is already exactly D3D's byte order and needs no work at
/// all; the identity swizzle 0x688 is the one that needs the swap. Applying a blanket
/// R<->B swap to every 8888 texture inverted red and blue on all of them -- including the
/// colour-grading LUT the composite runs the whole frame through, which is what turned
/// the sky orange.
struct Swizzle32 {
  uint8_t src[4];  ///< per destination byte (B,G,R,A): source byte 0-3, or 4=zero, 5=one
  bool identity;
};

Swizzle32 MakeSwizzle32(uint32_t swizzle) {
  Swizzle32 s{};
  const uint8_t comp[4] = {
      uint8_t(swizzle & 0x7),         // -> R (dst byte 2)
      uint8_t((swizzle >> 3) & 0x7),  // -> G (dst byte 1)
      uint8_t((swizzle >> 6) & 0x7),  // -> B (dst byte 0)
      uint8_t((swizzle >> 9) & 0x7),  // -> A (dst byte 3)
  };
  s.src[2] = comp[0];
  s.src[1] = comp[1];
  s.src[0] = comp[2];
  s.src[3] = comp[3];
  s.identity = s.src[0] == 0 && s.src[1] == 1 && s.src[2] == 2 && s.src[3] == 3;
  return s;
}

/// Apply `s` in place over a run of 32-bit texels.
void SwizzleRow32(uint8_t* row, uint32_t texels, const Swizzle32& s) {
  for (uint32_t i = 0; i < texels; ++i) {
    uint8_t* p = row + size_t(i) * 4;
    const uint8_t in[4] = {p[0], p[1], p[2], p[3]};
    for (uint32_t d = 0; d < 4; ++d) {
      const uint8_t c = s.src[d];
      p[d] = c < 4 ? in[c] : (c == 5 ? 0xFF : 0x00);
    }
  }
}

}  // namespace

//=============================================================================

D3DPRIMITIVETYPE HostPrimitiveType(uint32_t xenos_primitive_type) {
  switch (xenos_primitive_type) {
    case 1: return D3DPT_POINTLIST;
    case 2: return D3DPT_LINELIST;
    case 3: return D3DPT_LINESTRIP;
    case 4: return D3DPT_TRIANGLELIST;
    case 5: return D3DPT_TRIANGLEFAN;
    case 6: return D3DPT_TRIANGLESTRIP;
    default: return D3DPRIMITIVETYPE(0);
  }
}

uint32_t HostPrimitiveCount(uint32_t xenos_primitive_type, uint32_t index_count) {
  switch (xenos_primitive_type) {
    case 1: return index_count;
    case 2: return index_count / 2;
    case 3: return index_count ? index_count - 1 : 0;
    case 4: return index_count / 3;
    case 5:
    case 6: return index_count >= 3 ? index_count - 2 : 0;
    default: return 0;
  }
}

//=============================================================================

ResourceTracker& ResourceTracker::Get() {
  static ResourceTracker instance;
  return instance;
}

void ResourceTracker::Initialize(IDirect3DDevice9Ex* device) {
  Shutdown();
  device_ = device;
  layouts_ = new LayoutMap();
  vertex_buffers_ = new VertexBufferMap();
  vertex_hashes_ = new VertexHashMap();
  index_buffers_ = new IndexBufferMap();
  index_ranges_ = new IndexRangeMap();
  textures_ = new TextureMap();
  best_textures_ = new BestTextureMap();
  resolves_ = new ResolveMap();
  render_targets_ = new TargetMap();

  // INTZ is what makes depth sampleable on D3D9. Without it the shadow maps and the
  // scene depth can still be *rendered*, but never read back, and the lighting that
  // depends on them stays wrong.
  intz_supported_ = false;
  IDirect3D9* d3d = nullptr;
  if (SUCCEEDED(device_->GetDirect3D(&d3d)) && d3d) {
    D3DDEVICE_CREATION_PARAMETERS cp{};
    D3DDISPLAYMODE mode{};
    if (SUCCEEDED(device_->GetCreationParameters(&cp)) &&
        SUCCEEDED(d3d->GetAdapterDisplayMode(cp.AdapterOrdinal, &mode))) {
      intz_supported_ =
          SUCCEEDED(d3d->CheckDeviceFormat(cp.AdapterOrdinal, cp.DeviceType, mode.Format,
                                           D3DUSAGE_DEPTHSTENCIL, D3DRTYPE_TEXTURE, D3DFMT_INTZ));
    }
    d3d->Release();
  }
  REXGPU_INFO("nx1_d3d9: INTZ sampleable depth {}", intz_supported_ ? "supported" : "UNAVAILABLE");

  // The neutral stand-in for textures we cannot produce yet (see white_).
  IDirect3DTexture9* staging = nullptr;
  if (SUCCEEDED(device_->CreateTexture(1, 1, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &staging,
                                       nullptr))) {
    D3DLOCKED_RECT locked;
    if (SUCCEEDED(staging->LockRect(0, &locked, nullptr, 0))) {
      *static_cast<uint32_t*>(locked.pBits) = 0xFFFFFFFFu;
      staging->UnlockRect(0);
      if (SUCCEEDED(device_->CreateTexture(1, 1, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &white_,
                                           nullptr))) {
        device_->UpdateTexture(staging, white_);
      }
    }
    staging->Release();
  }
  if (!white_) {
    REXGPU_WARN("nx1_d3d9: could not create the white fallback texture");
  }

  // Register the physical-memory write-watch. It invalidates mirror pages (below) only when the
  // guest rewrites their bytes -- the streaming pool leaves a non-resident texture's memory as
  // transient garbage, so we snapshot-and-hold rather than re-read per draw. Mirrors the reference.
  if (auto* mem = rex::system::kernel_state()->memory(); mem && !mem_watch_handle_) {
    mem_watch_handle_ = mem->RegisterPhysicalMemoryInvalidationCallback(&MemWatchThunk, this);
  }
  // Allocate the CPU snapshot mirror of guest physical memory (lazily committed) and record the
  // host pointer for guest physical 0, so MirrorSnapshot can copy pages out of live RAM.
  phys_base_ = TranslatePhysical(0);
  if (!mirror_) {
    mirror_ = static_cast<uint8_t*>(VirtualAlloc(nullptr, size_t(kMirrorPages) << 12,
                                                 MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    mirror_valid_.assign((kMirrorPages + 63) / 64, 0);
    mirror_sweep_page_ = 0;
  }
  REXGPU_INFO("nx1_d3d9: texture write-watch {}, mirror {}",
              mem_watch_handle_ ? "registered" : "FAILED",
              (mirror_ && phys_base_) ? "allocated" : "FAILED");
}

void ResourceTracker::Shutdown() {
  if (mem_watch_handle_) {
    if (auto* mem = rex::system::kernel_state()->memory()) {
      mem->UnregisterPhysicalMemoryInvalidationCallback(mem_watch_handle_);
    }
    mem_watch_handle_ = nullptr;
  }
  {
    std::lock_guard<std::mutex> lk(dirty_mu_);
    writes_pending_.clear();
  }
  if (mirror_) {
    VirtualFree(mirror_, 0, MEM_RELEASE);
    mirror_ = nullptr;
  }
  mirror_valid_.clear();
  mirror_sweep_page_ = 0;
  phys_base_ = nullptr;
  if (depth_blit_ps_) {
    depth_blit_ps_->Release();
    depth_blit_ps_ = nullptr;
  }
  if (white_) {
    white_->Release();
    white_ = nullptr;
  }
  if (layouts_) {
    auto* map = static_cast<LayoutMap*>(layouts_);
    for (auto& [key, layout] : *map) {
      if (layout.decl) layout.decl->Release();
    }
    delete map;
    layouts_ = nullptr;
  }
  if (vertex_buffers_) {
    auto* map = static_cast<VertexBufferMap*>(vertex_buffers_);
    for (auto& [key, entry] : *map) {
      if (entry.vb) entry.vb->Release();
    }
    delete map;
    vertex_buffers_ = nullptr;
  }
  if (vertex_hashes_) {
    delete static_cast<VertexHashMap*>(vertex_hashes_);
    vertex_hashes_ = nullptr;
  }
  if (index_ranges_) {
    delete static_cast<IndexRangeMap*>(index_ranges_);
    index_ranges_ = nullptr;
  }
  if (index_buffers_) {
    auto* map = static_cast<IndexBufferMap*>(index_buffers_);
    for (auto& [key, entry] : *map) {
      if (entry.ib) entry.ib->Release();
    }
    delete map;
    index_buffers_ = nullptr;
  }
  if (textures_) {
    auto* map = static_cast<TextureMap*>(textures_);
    for (auto& [key, entry] : *map) {
      if (entry.tex) entry.tex->Release();
      if (entry.vol) entry.vol->Release();
      if (entry.cube) entry.cube->Release();
    }
    delete map;
    textures_ = nullptr;
  }
  if (best_textures_) {
    auto* map = static_cast<BestTextureMap*>(best_textures_);
    for (auto& [key, b] : *map) {
      if (b.tex) b.tex->Release();
    }
    delete map;
    best_textures_ = nullptr;
  }
  if (resolves_) {
    auto* map = static_cast<ResolveMap*>(resolves_);
    for (auto& [key, entry] : *map) {
      // Non-owned entries alias a depth target released with render_targets_ below.
      if (entry.tex && entry.owned) entry.tex->Release();
    }
    delete map;
    resolves_ = nullptr;
  }
  if (render_targets_) {
    auto* map = static_cast<TargetMap*>(render_targets_);
    for (auto& [key, t] : *map) {
      if (t.surface && !t.is_backbuffer) t.surface->Release();
      if (t.tex) t.tex->Release();
    }
    delete map;
    render_targets_ = nullptr;
  }
  backbuffer_ = nullptr;
  device_ = nullptr;
}

const VertexLayout* ResourceTracker::GetVertexLayout(const uint8_t* base, uint32_t guest_device) {
  if (!device_ || !layouts_) {
    return nullptr;
  }
  const uint32_t decl_object = BoundVertexDeclaration(base, guest_device);
  if (!decl_object) {
    return nullptr;
  }

  // The declaration alone does not determine the host layout: SetStreamSource
  // supplies the strides, and two draws can share a declaration across streams
  // of different pitch.
  uint64_t key = MixKey(decl_object, VertexDeclarationUniqueness(base, decl_object));
  for (uint32_t s = 0; s < kMaxHostStreams; ++s) {
    key = MixKey(key, StreamStride(base, guest_device, s));
  }

  auto* map = static_cast<LayoutMap*>(layouts_);
  if (auto it = map->find(key); it != map->end()) {
    return it->second.decl ? &it->second : nullptr;
  }

  VertexLayout layout;
  layout.key = key;
  for (uint32_t s = 0; s < kMaxHostStreams; ++s) {
    layout.guest_stride[s] = StreamStride(base, guest_device, s);
    layout.bulk_swap[s] = true;
  }

  const uint32_t count = VertexDeclarationCount(base, decl_object);
  D3DVERTEXELEMENT9 host_elements[MAXD3DDECLLENGTH + 1] = {};
  uint32_t host_count = 0;
  bool ok = count > 0 && count <= MAXD3DDECLLENGTH;

  for (uint32_t i = 0; ok && i < count; ++i) {
    const GuestVertexElement e = ReadVertexElement(base, decl_object, i);
    if (e.stream >= kMaxHostStreams) {
      REXGPU_WARN("nx1_d3d9: vertex element on stream {} (only {} supported)", e.stream,
                  kMaxHostStreams);
      ok = false;
      break;
    }
    HostFormat host_format;
    if (!PickHostFormat(e.format, e.is_signed, e.is_normalized, &host_format)) {
      if ((unsupported_formats_++ % 100) == 0) {
        REXGPU_WARN("nx1_d3d9: unsupported Xenos vertex format {} (signed={}, normalized={})",
                    e.format, e.is_signed, e.is_normalized);
      }
      ok = false;
      break;
    }

    ConvertOp op{};
    op.src_offset = uint16_t(e.offset);
    op.dst_offset = uint16_t(layout.host_stride[e.stream]);
    op.stream = uint8_t(e.stream);
    op.format = uint8_t(e.format);
    op.src_size = host_format.src_size;
    op.dst_size = host_format.dst_size;
    op.is_signed = e.is_signed;
    op.is_normalized = e.is_normalized;
    op.expand = host_format.expand;
    layout.host_stride[e.stream] += host_format.dst_size;
    if (op.expand || op.dst_offset != op.src_offset) {
      layout.bulk_swap[e.stream] = false;
    }
    layout.ops.push_back(op);

    host_elements[host_count++] = D3DVERTEXELEMENT9{
        WORD(e.stream),           WORD(op.dst_offset), BYTE(host_format.type),
        D3DDECLMETHOD_DEFAULT, BYTE(e.usage),       BYTE(e.usage_index),
    };
    layout.stream_count = std::max(layout.stream_count, e.stream + 1);
  }

  if (ok) {
    for (uint32_t s = 0; s < kMaxHostStreams; ++s) {
      // A stream is only a bulk swap if the host vertex exactly covers the guest
      // one -- trailing guest padding would shift every subsequent vertex.
      if (layout.host_stride[s] != layout.guest_stride[s]) {
        layout.bulk_swap[s] = false;
      }
    }
    host_elements[host_count] = D3DVERTEXELEMENT9 D3DDECL_END();
    if (FAILED(device_->CreateVertexDeclaration(host_elements, &layout.decl))) {
      REXGPU_ERROR("nx1_d3d9: CreateVertexDeclaration failed ({} elements)", host_count);
      layout.decl = nullptr;
    }
  }

  // A failed layout is cached too: the same declaration will fail every draw,
  // and re-deriving it each time would be pure overhead.
  layout.content_key = LayoutContentKey(layout);
  auto& stored = map->emplace(key, std::move(layout)).first->second;
  return stored.decl ? &stored : nullptr;
}

const VertexLayout* ResourceTracker::GetShaderVertexLayout(const uint8_t* base,
                                                           uint32_t guest_device,
                                                           uint32_t stream0_stride) {
  // stream0_stride == 0 is valid: it selects bound-buffer mode (strides come from
  // m_StreamStride). Only reject a genuinely unusable tracker here.
  if (!device_ || !layouts_) {
    return nullptr;
  }
  const uint32_t vs_object = BoundVertexShader(base, guest_device);
  if (!vs_object) {
    return nullptr;
  }
  // Resolve the exact microcode the draw runs -- same pass BindShadersAndConstants
  // picks -- so the vfetch layout matches the shader the SM3 cache was keyed on.
  const bool has_ps = BoundPixelShader(base, guest_device) != 0;
  const uint32_t pass = VertexShaderPass(base, vs_object, has_ps);
  const GuestUcode ucode = ReadGuestUcode(base, vs_object, /*pixel_shader=*/false, pass);
  if (!ucode.valid()) {
    return nullptr;
  }

  // The vfetch layout is fixed by the microcode; for UP draws the host stride also
  // depends on the supplied stream-0 stride, so fold it into the key.
  const uint64_t key = MixKey(MixKey(ucode.physical_address, ucode.dword_count), stream0_stride);
  auto* map = static_cast<LayoutMap*>(layouts_);
  if (auto it = map->find(key); it != map->end()) {
    return it->second.decl ? &it->second : nullptr;
  }

  // Reuse rexglue's Xenos analyzer -- it produces exactly the vertex_bindings
  // XenosRecomp consumed to assign this shader's input semantics.
  const uint32_t* ucode_dwords =
      reinterpret_cast<const uint32_t*>(TranslatePhysical(ucode.physical_address));
  rex::graphics::Shader shader(rex::graphics::xenos::ShaderType::kVertex, ucode.physical_address,
                               ucode_dwords, ucode.dword_count, std::endian::big);
  rex::string::StringBuffer disasm;
  shader.AnalyzeUcode(disasm);

  // stream0_stride != 0 -> UP draw: one interleaved stream 0 with the supplied
  // stride. stream0_stride == 0 -> bound-buffer draw (BindStreams): each vfetch
  // binding is its own stream (95 - fetch_constant, matching VertexFetchSlotForStream)
  // fed by that stream's fetch constant, with the stride taken from the vfetch.
  const bool up_mode = stream0_stride != 0;

  VertexLayout layout;
  layout.key = key;
  for (uint32_t s = 0; s < kMaxHostStreams; ++s) {
    layout.bulk_swap[s] = true;
  }
  if (up_mode) {
    layout.guest_stride[0] = stream0_stride;
  }

  D3DVERTEXELEMENT9 host_elements[MAXD3DDECLLENGTH + 1] = {};
  uint32_t host_count = 0;
  bool ok = true;
  size_t ordinal = 0;
  uint32_t packed_count = 0, texcoord_count = 0, color_count = 0;

  for (const auto& binding : shader.vertex_bindings()) {
    const uint32_t stream = up_mode ? 0u : (95u - binding.fetch_constant);
    if (stream >= kMaxHostStreams) {
      ok = false;
      break;
    }
    if (!up_mode) {
      // The bound-buffer stride is not in the shader's vfetch (it reads 0 there);
      // the guest sets it via D3DDevice_SetStreamSource into m_StreamStride[].
      layout.guest_stride[stream] = StreamStride(base, guest_device, stream);
    }
    for (const auto& attribute : binding.attributes) {
      if (host_count >= MAXD3DDECLLENGTH) {
        ok = false;
        break;
      }
      const auto& attrs = attribute.fetch_instr.attributes;
      const uint32_t format = uint32_t(attrs.data_format);
      const bool is_signed = attrs.is_signed;
      const bool is_normalized = !attrs.is_integer;
      // The usage ordinal spans ALL bindings/attributes in listing order -- exactly
      // as WriteNx1RawVertexFetchMetadata did, so semantics match the compiled shader.
      const RawVertexUsage usage =
          Nx1RawVertexUsage(ordinal++, format, packed_count, texcoord_count, color_count);

      HostFormat host_format;
      if (!PickHostFormat(format, is_signed, is_normalized, &host_format)) {
        if ((unsupported_formats_++ % 100) == 0) {
          REXGPU_WARN("nx1_d3d9: unsupported Xenos vfetch format {} (signed={}, normalized={})",
                      format, is_signed, is_normalized);
        }
        ok = false;
        break;
      }

      ConvertOp op{};
      op.src_offset = uint16_t(attrs.offset * 4);  // vfetch offset is in dwords
      op.dst_offset = uint16_t(layout.host_stride[stream]);
      op.stream = uint8_t(stream);
      op.format = uint8_t(format);
      op.src_size = host_format.src_size;
      op.dst_size = host_format.dst_size;
      op.is_signed = is_signed;
      op.is_normalized = is_normalized;
      op.expand = host_format.expand;
      layout.host_stride[stream] += host_format.dst_size;
      if (op.expand || op.dst_offset != op.src_offset) {
        layout.bulk_swap[stream] = false;
      }
      layout.ops.push_back(op);

      host_elements[host_count++] = D3DVERTEXELEMENT9{
          WORD(stream), WORD(op.dst_offset), BYTE(host_format.type), D3DDECLMETHOD_DEFAULT,
          usage.usage,  usage.usage_index,
      };
      layout.stream_count = std::max(layout.stream_count, stream + 1);
    }
    if (!ok) {
      break;
    }
  }

  ok = ok && host_count > 0;
  if (ok) {
    for (uint32_t s = 0; s < layout.stream_count; ++s) {
      // A stream is only a bulk swap if the host vertex exactly covers the guest one.
      if (layout.host_stride[s] != layout.guest_stride[s]) {
        layout.bulk_swap[s] = false;
      }
    }
    host_elements[host_count] = D3DVERTEXELEMENT9 D3DDECL_END();
    if (FAILED(device_->CreateVertexDeclaration(host_elements, &layout.decl))) {
      REXGPU_ERROR("nx1_d3d9: shader-derived CreateVertexDeclaration failed ({} elements)",
                   host_count);
      layout.decl = nullptr;
    }
  }

  layout.content_key = LayoutContentKey(layout);
  auto& stored = map->emplace(key, std::move(layout)).first->second;
  return stored.decl ? &stored : nullptr;
}

IDirect3DVertexBuffer9* ResourceTracker::GetVertexBuffer(const uint8_t* base, uint32_t guest_device,
                                                         uint32_t stream,
                                                         const VertexLayout& layout,
                                                         uint32_t needed_vertices,
                                                         uint32_t* vertex_count) {
  *vertex_count = 0;
  if (!device_ || !vertex_buffers_ || stream >= kMaxHostStreams) {
    return nullptr;
  }
  const uint32_t guest_stride = layout.guest_stride[stream];
  const uint32_t host_stride = layout.host_stride[stream];
  const VertexFetchConstant fetch = ReadVertexFetchConstant(base, guest_device, stream);
  if (!guest_stride || !host_stride) {
    return nullptr;
  }
  if (!fetch.base_address || fetch.size_bytes < guest_stride) {
    return nullptr;
  }

  // Keyed on the layout's *content*, not on the shader that asked for it: the same pooled
  // buffer read through the same vertex format converts to the same bytes no matter which
  // of the frame's ~120 vertex shaders is bound.
  const uint64_t key = MixKey(MixKey(fetch.base_address, layout.content_key), stream);
  auto* map = static_cast<VertexBufferMap*>(vertex_buffers_);
  auto& entry = (*map)[key];

  // The fetch constant's size is the distance to the end of the buffer, which for the
  // engine's shared pools is megabytes -- not this model's vertex count. Mirroring all of
  // it hashed and converted ~78k vertices for a draw that touches a few hundred. Bound it
  // by what the draw's indices actually reference, and only ever grow: several draws share
  // an entry, and re-creating the buffer whenever one of them needs fewer vertices than the
  // last would thrash it.
  const uint32_t pool_count = fetch.size_bytes / guest_stride;
  uint32_t count = needed_vertices ? std::min(needed_vertices, pool_count) : pool_count;
  count = std::max(count, entry.vertex_count);
  const uint32_t host_bytes = count * host_stride;
  *vertex_count = count;

  // Frame-coherent reuse: already validated this frame -> skip the content hash.
  if (entry.vb && entry.last_frame == frame_ && entry.bytes == host_bytes) {
    *vertex_count = entry.vertex_count;
    return entry.vb;
  }
  entry.last_frame = frame_;

  const uint8_t* src = TranslatePhysical(fetch.base_address);

  // Hash the guest bytes at most once per buffer per frame. The hash only depends on the
  // guest memory, so every layout reading the same buffer gets the same answer -- and the
  // buffers are large enough (a shared pool runs to megabytes) that re-hashing one per
  // reader was half a gigabyte of memory traffic a frame.
  const size_t guest_bytes = size_t(count) * guest_stride;
  uint64_t content_hash = 0;
  {
    auto* hashes = static_cast<VertexHashMap*>(vertex_hashes_);
    auto& cached = (*hashes)[MixKey(fetch.base_address, guest_bytes)];
    if (cached.frame != frame_ + 1) {  // +1: a default-constructed entry must not match frame 0
      cached.frame = frame_ + 1;
      cached.hash = XXH3_64bits(src, guest_bytes);
    }
    content_hash = cached.hash;
  }
  if (entry.vb && entry.bytes == host_bytes && entry.content_hash == content_hash) {
    return entry.vb;
  }
  if (entry.vb && entry.bytes != host_bytes) {
    entry.vb->Release();
    entry.vb = nullptr;
  }
  if (!entry.vb &&
      FAILED(device_->CreateVertexBuffer(host_bytes, D3DUSAGE_WRITEONLY, 0, D3DPOOL_DEFAULT,
                                         &entry.vb, nullptr))) {
    REXGPU_ERROR("nx1_d3d9: CreateVertexBuffer({} bytes) failed", host_bytes);
    entry.vb = nullptr;
    return nullptr;
  }

  void* mapped = nullptr;
  if (FAILED(entry.vb->Lock(0, 0, &mapped, 0))) {
    return nullptr;
  }
  auto* dst = static_cast<uint8_t*>(mapped);

  if (layout.bulk_swap[stream]) {
    SwapDwords(dst, src, size_t(count) * guest_stride);
  } else {
    for (uint32_t v = 0; v < count; ++v) {
      const uint8_t* sv = src + size_t(v) * guest_stride;
      uint8_t* dv = dst + size_t(v) * host_stride;
      for (const ConvertOp& op : layout.ops) {
        if (op.stream != stream) {
          continue;
        }
        if (op.expand) {
          float decoded[4];
          Expand(sv + op.src_offset, op, decoded);
          std::memcpy(dv + op.dst_offset, decoded, op.dst_size);
        } else {
          SwapDwords(dv + op.dst_offset, sv + op.src_offset, op.src_size);
        }
      }
    }
  }
  entry.vb->Unlock();

  // TEMP DIAGNOSTIC (no sun shadows): check the packed-normal decode CONTRACT.
  //
  // XenosRecomp deletes the decode from the shader -- `tfetchR11G11B10(x)` -> `x`, commented
  // "Normal decode moves to the vertex-upload path" -- and trusts THIS code to hand the shader a
  // decoded, unit-length normal. Nothing verifies that, and the world lighting does
  //     sun = saturate(dot(normal, sunDir)) * sunColour;  oC0 = sun * shadow + ambient
  // so a bad normal reads as "lit but flat, no sun shadows" -- precisely the open bug. A mean
  // length of ~1.0 clears the decode; ~0 or wildly off is the answer.
  {
    static std::mutex nm;
    static std::vector<uint64_t> nseen;
    for (const ConvertOp& op : layout.ops) {
      if (op.stream != stream || !op.expand) {
        continue;
      }
      if (op.format != kFmt_11_11_10 && op.format != kFmt_10_11_11 &&
          op.format != kFmt_2_10_10_10) {
        continue;  // the three packed formats the renderer maps to NORMAL/TANGENT/BINORMAL
      }
      const uint64_t key = (uint64_t(op.format) << 40) ^ (uint64_t(op.src_offset) << 16) ^
                           uint64_t(guest_stride);
      std::lock_guard<std::mutex> lk(nm);
      if (std::find(nseen.begin(), nseen.end(), key) != nseen.end() || nseen.size() >= 8) {
        continue;
      }
      nseen.push_back(key);
      char b[512];
      int n = std::snprintf(b, sizeof(b),
                            "nx1_d3d9: VTXNORMAL fmt=%u off=%u signed=%u norm=%u stride=%u |",
                            op.format, op.src_offset, op.is_signed ? 1u : 0u,
                            op.is_normalized ? 1u : 0u, guest_stride);
      double sum_len = 0.0;
      uint32_t taken = 0;
      const uint32_t step = count > 6 ? count / 6 : 1;
      for (uint32_t v = 0; v < count && taken < 6; v += step, ++taken) {
        float d[4];
        Expand(src + size_t(v) * guest_stride + op.src_offset, op, d);
        const double len =
            std::sqrt(double(d[0]) * d[0] + double(d[1]) * d[1] + double(d[2]) * d[2]);
        sum_len += len;
        if (taken < 3 && n < 380) {
          n += std::snprintf(b + n, sizeof(b) - n, " (%.3f,%.3f,%.3f)len=%.3f", d[0], d[1], d[2],
                             len);
        }
      }
      std::snprintf(b + n, sizeof(b) - n, " | meanlen=%.4f over %u verts", sum_len / double(taken),
                    taken);
      REXGPU_INFO("{}", b);
    }
  }

  entry.bytes = host_bytes;
  entry.vertex_count = count;
  entry.content_hash = content_hash;
  return entry.vb;
}

void ResourceTracker::LogCacheStats() {
  if (!device_) {
    return;
  }
  const size_t textures = textures_ ? static_cast<TextureMap*>(textures_)->size() : 0;
  const size_t vbs = vertex_buffers_
                         ? static_cast<std::unordered_map<uint64_t, VertexBufferEntry>*>(
                               vertex_buffers_)->size()
                         : 0;
  const size_t ibs = index_buffers_ ? static_cast<IndexBufferMap*>(index_buffers_)->size() : 0;
  const size_t resolves = resolves_ ? static_cast<ResolveMap*>(resolves_)->size() : 0;
  REXGPU_INFO(
      "nx1_d3d9: cache frame={} textures={} vb={} ib={} resolves={} | uploads={} rebuilds={} "
      "evicted={} failures={} unsupported={} | driver texmem={} MiB",
      frame_, textures, vbs, ibs, resolves, tex_uploads_, tex_rebuilds_, tex_evicted_,
      tex_failures_, unsupported_texture_formats_,
      device_->GetAvailableTextureMem() / (1024 * 1024));
  REXGPU_INFO("nx1_d3d9: mipgen built={} auto={} skipped(no chain declared)={} skipped(fmt)={}",
              mips_built_, mips_auto_, mips_skip_nochain_, mips_skip_unsupported_);
}

// Physical-memory write-watch callback. Runs on whatever guest thread wrote the memory, under the
// memory system's global lock -- so it must not touch the texture cache or take the render lock.
// It only queues the written range; DrainMemoryWrites() applies it on the render thread. The
// returned pair is the (page-aligned) range the memory system may safely unprotect so the write
// proceeds; returning just the touched pages is correct (other pages stay watched).
std::pair<uint32_t, uint32_t> ResourceTracker::MemWatchThunk(void* ctx, uint32_t addr, uint32_t len,
                                                             bool exact) {
  return static_cast<ResourceTracker*>(ctx)->OnMemoryWrite(addr, len, exact);
}

std::pair<uint32_t, uint32_t> ResourceTracker::OnMemoryWrite(uint32_t addr, uint32_t len, bool) {
  {
    std::lock_guard<std::mutex> lk(dirty_mu_);
    writes_pending_.emplace_back(addr, len);
  }
  const uint32_t p0 = addr & ~0xFFFu;
  const uint32_t p1 = (addr + len + 0xFFFu) & ~0xFFFu;
  return std::make_pair(p0, p1 - p0);
}

// Ensure [phys_addr, phys_addr+len) is captured in the mirror; return a pointer into the mirror.
// A page is copied out of live guest RAM the first time it is requested while invalid, then held
// (and its write-watch armed) until the guest writes it. Textures decode through this so
// streaming-pool churn never reaches them between writes.
const uint8_t* ResourceTracker::MirrorSnapshot(uint32_t phys_addr, uint32_t len) {
  if (!mirror_ || !phys_base_ || uint64_t(phys_addr) + len > (uint64_t(kMirrorPages) << 12)) {
    return TranslatePhysical(phys_addr);
  }
  const uint32_t p0 = phys_addr >> 12;
  const uint32_t p1 = (phys_addr + len - 1) >> 12;
  for (uint32_t p = p0; p <= p1; ++p) {
    if (!(mirror_valid_[p >> 6] & (uint64_t(1) << (p & 63)))) {
      std::memcpy(mirror_ + (size_t(p) << 12), phys_base_ + (size_t(p) << 12), 4096);
      mirror_valid_[p >> 6] |= (uint64_t(1) << (p & 63));
      if (auto* mem = rex::system::kernel_state()->memory()) {
        mem->EnablePhysicalMemoryAccessCallbacks(p << 12, 4096, true, false);
      }
    }
  }
  return mirror_ + phys_addr;
}

// Apply queued guest writes on the render thread: invalidate the mirror pages they touched so the
// next MirrorSnapshot re-copies fresh bytes, and dirty any texture entry whose watched range
// overlaps so it re-decodes. This is how a texture that legitimately reloads (or whose slot is
// repurposed) picks up its new bytes, while an untouched texture is held from the churn.
void ResourceTracker::DrainMemoryWrites() {
  std::vector<std::pair<uint32_t, uint32_t>> writes;
  {
    std::lock_guard<std::mutex> lk(dirty_mu_);
    writes.swap(writes_pending_);
  }
  if (writes.empty()) return;
  for (const auto& [addr, len] : writes) {
    if (uint64_t(addr) + len > (uint64_t(kMirrorPages) << 12)) continue;
    const uint32_t p0 = addr >> 12;
    const uint32_t p1 = (addr + len - 1) >> 12;
    for (uint32_t p = p0; p <= p1; ++p) {
      mirror_valid_[p >> 6] &= ~(uint64_t(1) << (p & 63));  // invalidate: re-copy on next request
    }
  }
  auto* textures = static_cast<TextureMap*>(textures_);
  if (!textures) return;
  for (auto& [key, entry] : *textures) {
    if (!entry.watch_size) continue;
    const uint32_t a0 = entry.watch_addr;
    const uint32_t a1 = entry.watch_addr + entry.watch_size;
    if (entry.committed) continue;  // settled texture: frozen against the pool's later garbage
    for (const auto& [addr, len] : writes) {
      if (addr < a1 && addr + len > a0) { entry.dirty = true; break; }
    }
  }
}

void ResourceTracker::AdvanceFrame() {
  ++frame_;
  DrainMemoryWrites();

  // Proactive early snapshot: capture physical memory into the mirror a chunk at a time over the
  // first frames, mirroring the reference's full-buffer memexport request that snapshots memory
  // early while textures are resident. On-demand MirrorSnapshot captures the rest; the write-watch
  // re-captures pages as the real image streams in. ~16 MB/frame -> full 512 MB in ~2s.
  if (mirror_ && phys_base_ && mirror_sweep_page_ < kMirrorPages) {
    const uint32_t end = std::min(mirror_sweep_page_ + 4096, kMirrorPages);  // 4096 pages = 16 MB
    auto* mem = rex::system::kernel_state()->memory();
    for (uint32_t p = mirror_sweep_page_; p < end; ++p) {
      if (!(mirror_valid_[p >> 6] & (uint64_t(1) << (p & 63)))) {
        std::memcpy(mirror_ + (size_t(p) << 12), phys_base_ + (size_t(p) << 12), 4096);
        mirror_valid_[p >> 6] |= (uint64_t(1) << (p & 63));
        if (mem) mem->EnablePhysicalMemoryAccessCallbacks(p << 12, 4096, true, false);
      }
    }
    mirror_sweep_page_ = end;
  }

  // Sweep periodically rather than every frame: walking the maps is O(entries), and an
  // entry only has to survive long enough that a resource used every few frames is not
  // thrashed. kKeepFrames is generous -- this is a leak guard, not a memory budget.
  constexpr uint64_t kSweepEvery = 300;
  constexpr uint64_t kKeepFrames = 600;
  if (frame_ % kSweepEvery || frame_ < kKeepFrames) {
    return;
  }
  const uint64_t cutoff = frame_ - kKeepFrames;

  if (auto* map = static_cast<VertexBufferMap*>(vertex_buffers_)) {
    for (auto it = map->begin(); it != map->end();) {
      if (it->second.last_frame < cutoff) {
        if (it->second.vb) it->second.vb->Release();
        it = map->erase(it);
      } else {
        ++it;
      }
    }
  }
  if (auto* map = static_cast<IndexBufferMap*>(index_buffers_)) {
    for (auto it = map->begin(); it != map->end();) {
      if (it->second.last_frame < cutoff) {
        if (it->second.ib) it->second.ib->Release();
        it = map->erase(it);
      } else {
        ++it;
      }
    }
  }
  // Only textures we uploaded live here; the resolve/depth aliases we do not own are in
  // resolves_, which is left alone.
  if (auto* map = static_cast<TextureMap*>(textures_)) {
    for (auto it = map->begin(); it != map->end();) {
      if (it->second.last_frame < cutoff) {
        if (it->second.tex) it->second.tex->Release();
        if (it->second.vol) it->second.vol->Release();
        if (it->second.cube) it->second.cube->Release();
        ++tex_evicted_;
        it = map->erase(it);
      } else {
        ++it;
      }
    }
  }
  // Per-surface retained textures (PreferLargestForSurface). Each holds a reference on a texture that
  // may already be gone from textures_ above, so this is where those references are dropped.
  if (auto* map = static_cast<BestTextureMap*>(best_textures_)) {
    for (auto it = map->begin(); it != map->end();) {
      if (it->second.last_frame < cutoff) {
        if (it->second.tex) it->second.tex->Release();
        it = map->erase(it);
      } else {
        ++it;
      }
    }
  }
  if (auto* map = static_cast<VertexHashMap*>(vertex_hashes_)) {
    for (auto it = map->begin(); it != map->end();) {
      it = it->second.frame < cutoff ? map->erase(it) : std::next(it);
    }
  }
}

IDirect3DIndexBuffer9* ResourceTracker::GetIndexBuffer(const uint8_t* base, uint32_t guest_device,
                                                       uint32_t* index_size) {
  *index_size = 0;
  if (!device_ || !index_buffers_) {
    return nullptr;
  }
  const IndexBufferState state = ReadIndexBuffer(base, guest_device);
  if (!state.valid()) {
    return nullptr;
  }
  *index_size = state.index_size;

  const uint64_t key = MixKey(state.base_address, state.size_bytes);
  auto* map = static_cast<IndexBufferMap*>(index_buffers_);
  auto& entry = (*map)[key];
  // Frame-coherent reuse: already validated this frame -> skip the content hash.
  if (entry.ib && entry.last_frame == frame_ && entry.bytes == state.size_bytes) {
    *index_size = entry.index_size;
    return entry.ib;
  }
  entry.last_frame = frame_;

  const uint8_t* src = TranslatePhysical(state.base_address);
  const uint64_t content_hash = XXH3_64bits(src, state.size_bytes);
  if (entry.ib && entry.bytes == state.size_bytes && entry.content_hash == content_hash) {
    return entry.ib;
  }
  if (entry.ib && entry.bytes != state.size_bytes) {
    entry.ib->Release();
    entry.ib = nullptr;
  }
  if (!entry.ib &&
      FAILED(device_->CreateIndexBuffer(
          state.size_bytes, D3DUSAGE_WRITEONLY,
          state.index_size == 4 ? D3DFMT_INDEX32 : D3DFMT_INDEX16, D3DPOOL_DEFAULT, &entry.ib,
          nullptr))) {
    REXGPU_ERROR("nx1_d3d9: CreateIndexBuffer({} bytes) failed", state.size_bytes);
    entry.ib = nullptr;
    return nullptr;
  }

  // Byteswap once into the CPU copy, then hand that to D3D -- GetDrawMaxIndex reads it
  // back to bound each draw's vertex range.
  entry.cpu.resize(state.size_bytes);
  if (state.index_size == 4) {
    SwapDwords(entry.cpu.data(), src, state.size_bytes & ~3u);
  } else {
    auto* dst = reinterpret_cast<uint16_t*>(entry.cpu.data());
    const uint32_t n = state.size_bytes / 2;
    for (uint32_t i = 0; i < n; ++i) {
      uint16_t v;
      std::memcpy(&v, src + i * 2, sizeof(v));
      dst[i] = Swap16(v);
    }
  }

  void* mapped = nullptr;
  if (FAILED(entry.ib->Lock(0, 0, &mapped, 0))) {
    return nullptr;
  }
  std::memcpy(mapped, entry.cpu.data(), state.size_bytes);
  entry.ib->Unlock();

  entry.bytes = state.size_bytes;
  entry.index_size = state.index_size;
  entry.content_hash = content_hash;
  return entry.ib;
}

uint32_t ResourceTracker::GetDrawMaxIndex(const uint8_t* base, uint32_t guest_device,
                                          uint32_t start_index, uint32_t index_count) {
  if (!index_buffers_ || !index_ranges_ || !index_count) {
    return 0;
  }
  const IndexBufferState state = ReadIndexBuffer(base, guest_device);
  if (!state.valid()) {
    return 0;
  }
  auto* map = static_cast<IndexBufferMap*>(index_buffers_);
  auto it = map->find(MixKey(state.base_address, state.size_bytes));
  if (it == map->end() || it->second.cpu.empty()) {
    return 0;
  }
  const IndexBufferEntry& ib = it->second;

  // Keyed on the index buffer's *contents* as well as the draw range, so a rewritten
  // dynamic index buffer invalidates the memo instead of returning a stale range.
  auto* ranges = static_cast<IndexRangeMap*>(index_ranges_);
  const uint64_t key =
      MixKey(MixKey(ib.content_hash, start_index), (uint64_t(index_count) << 1) | ib.index_size);
  if (auto found = ranges->find(key); found != ranges->end()) {
    return found->second;
  }

  uint32_t max_index = 0;
  if (ib.index_size == 4) {
    const uint32_t n = uint32_t(ib.cpu.size() / 4);
    const auto* idx = reinterpret_cast<const uint32_t*>(ib.cpu.data());
    for (uint32_t i = start_index; i < start_index + index_count && i < n; ++i) {
      max_index = std::max(max_index, idx[i]);
    }
  } else {
    const uint32_t n = uint32_t(ib.cpu.size() / 2);
    const auto* idx = reinterpret_cast<const uint16_t*>(ib.cpu.data());
    for (uint32_t i = start_index; i < start_index + index_count && i < n; ++i) {
      max_index = std::max(max_index, uint32_t(idx[i]));
    }
  }
  (*ranges)[key] = max_index;
  return max_index;
}

uint32_t ResourceTracker::ConvertInlineVertices(uint32_t verts_addr, uint32_t count,
                                                uint32_t guest_stride, const VertexLayout& layout,
                                                std::vector<uint8_t>* out) {
  const uint32_t host_stride = layout.host_stride[0];
  if (!verts_addr || !count || !guest_stride || !host_stride) {
    return 0;
  }
  const uint8_t* src = TranslateGuestPointer(verts_addr);
  if (!src) {
    return 0;
  }
  out->resize(size_t(count) * host_stride);
  uint8_t* dst = out->data();

  // Bulk swap only when the host layout is byte-identical to the (passed) guest
  // stride; otherwise repack element by element, exactly as GetVertexBuffer does.
  if (layout.bulk_swap[0] && host_stride == guest_stride) {
    SwapDwords(dst, src, size_t(count) * guest_stride);
  } else {
    for (uint32_t v = 0; v < count; ++v) {
      const uint8_t* sv = src + size_t(v) * guest_stride;
      uint8_t* dv = dst + size_t(v) * host_stride;
      for (const ConvertOp& op : layout.ops) {
        if (op.stream != 0) {
          continue;
        }
        if (op.expand) {
          float decoded[4];
          Expand(sv + op.src_offset, op, decoded);
          std::memcpy(dv + op.dst_offset, decoded, op.dst_size);
        } else {
          SwapDwords(dv + op.dst_offset, sv + op.src_offset, op.src_size);
        }
      }
    }
  }
  return host_stride;
}

bool ResourceTracker::ConvertInlineIndices(uint32_t indices_addr, uint32_t index_count,
                                           uint32_t index_size, std::vector<uint8_t>* out) {
  if (!indices_addr || !index_count || (index_size != 2 && index_size != 4)) {
    return false;
  }
  const uint8_t* src = TranslateGuestPointer(indices_addr);
  if (!src) {
    return false;
  }
  out->resize(size_t(index_count) * index_size);
  if (index_size == 4) {
    SwapDwords(out->data(), src, size_t(index_count) * 4);
  } else {
    auto* dst = reinterpret_cast<uint16_t*>(out->data());
    for (uint32_t i = 0; i < index_count; ++i) {
      uint16_t v;
      std::memcpy(&v, src + size_t(i) * 2, sizeof(v));
      dst[i] = Swap16(v);
    }
  }
  return true;
}

IDirect3DBaseTexture9* ResourceTracker::GetTexture(const uint8_t* base, uint32_t guest_device,
                                                   uint32_t sampler) {
  if (!device_ || !textures_) {
    return nullptr;
  }
  const TextureFetchConstant t = ReadTextureFetchConstant(base, guest_device, sampler);
  if (!t.valid || !t.base_address || !t.width || !t.height) {
    return nullptr;
  }

  // A texture whose address is a resolve destination is served by the rendered
  // image, not by untiling stale memory. This is what makes render-to-texture
  // (scene compositing, post effects) work.
  if (resolves_) {
    auto* rmap = static_cast<ResolveMap*>(resolves_);
    if (auto it = rmap->find(PhysicalAddress(t.base_address));
        it != rmap->end() && it->second.tex) {
      return it->second.tex;
    }
  }
  // xenos::DataDimension: 0 = 1D, 1 = 2D, 2 = 3D, 3 = cube.
  //
  // The composite's pixel shader ends with a 1D tone-map curve and a 3D colour-grading LUT:
  //   r0.x   = tfetch1D(s8, ..)
  //   r0.xyz = tfetch3D(s7, r0)
  //   oC0    = r0
  // so its entire output comes from those two. An unbound D3D9 sampler reads black, which
  // blacked out the whole composited frame while the scene texture feeding it was perfect.
  //
  // 1D needs no special case: on D3D9 a 1D texture *is* a 2D texture one row tall, which is
  // exactly what ps_3_0 compiles tex1D into. Fall through with height forced to 1.
  if (t.dimension == 2) {
    return GetVolumeTexture(t, sampler);
  }
  if (t.dimension == 3) {
    return GetCubeTexture(t, sampler);
  }
  const uint32_t height = t.dimension == 0 ? 1u : t.height;

  // The layout key is derived from the fetch constant alone, so it can be built -- and the
  // cache consulted -- before any of the format/extent work below. That matters: a scene is
  // ~5000 draws x 16 samplers, so this early-out runs ~80k times a frame and everything it
  // skips is multiplied by that.
  //
  // When the key changes, the texture object is rebuilt. The swizzle is part of the key: the
  // same memory bound with a different channel swizzle is a different texture. mip_max_level and
  // packed_mips are in too -- they decide our host chain's level count.
  //
  // mip_address is deliberately NOT in the key. The texture-streaming pool relocates a texture's
  // mip tail as it pages memory around, so mip_address churns through many values for one and the
  // same image (measured: a single 512x512 surface cycled its mip_address every few frames). We
  // build our own mip chain from level 0 and never read the guest's, so mip_address describes
  // nothing about the host texture -- folding it into the key only forced a re-decode every time
  // the pool moved, and re-reading base_address mid-stream is what dissolved the surface into
  // rainbow static. base_address (also the cache-map key) already identifies the texel data.
  const uint64_t layout_key = MixKey(
      MixKey(MixKey(t.base_address, (uint64_t(t.format) << 32) | t.width),
             MixKey((uint64_t(height) << 1) | (t.tiled ? 1 : 0), t.swizzle)),
      (uint64_t(t.mip_max_level) << 1) | (t.packed_mips ? 1 : 0));

  const uint64_t key = MixKey(t.base_address, sampler);
  auto* map = static_cast<TextureMap*>(textures_);
  auto& entry = (*map)[key];
  // Count clean frames toward commit (once per new frame for a stable, already-decoded texture).
  // After kCommitFrames it is settled: DrainMemoryWrites stops honouring writes to it, so the
  // streaming pool's later garbage-recycle can't re-poison the decode (the confetti-on-backup).
  constexpr uint32_t kCommitFrames = 32;
  if (entry.tex && entry.last_frame != frame_ && entry.layout_key == layout_key &&
      !entry.committed && ++entry.good_frames >= kCommitFrames) {
    entry.committed = true;
  }
  // Frame-coherent reuse: same texture already uploaded/validated this frame ->
  // reuse it without re-hashing megabytes of guest memory (the dominant per-draw cost).
  if (entry.tex && entry.last_frame == frame_ && entry.layout_key == layout_key) {
    return entry.tex;
  }

  HostTextureFormat host = PickHostTextureFormat(t.format);
  // A block-compressed colour texture whose swizzle broadcasts one source channel to all of R,G,B
  // (a luminance/mask sprite -- the smoke and effect sprites are stored this way, e.g. swizzle
  // 0x200 = R,G,B<-red, A<-green) cannot be honoured by a compressed host texture, which samples a
  // fixed RGBA. Materialise it to A8R8G8B8 and apply the swizzle ourselves. Normal RGB textures map
  // R,G,B from distinct channels, so they stay compressed on the fast path.
  D3DFORMAT src_bc = D3DFMT_UNKNOWN;
  if (host.valid && host.decode == TexDecode::kNone &&
      (host.d3d == D3DFMT_DXT1 || host.d3d == D3DFMT_DXT3 || host.d3d == D3DFMT_DXT5)) {
    const uint32_t sx = t.swizzle & 7, sy = (t.swizzle >> 3) & 7, sz = (t.swizzle >> 6) & 7;
    if (sx == sy && sy == sz && sx < 4) {  // R,G,B all from the same data channel -> broadcast
      src_bc = host.d3d;
      host.d3d = D3DFMT_A8R8G8B8;
      host.decode = TexDecode::kColorSwizzle;
      host.opaque_block = false;
    }
  }
  if (!host.valid) {
    if ((unsupported_texture_formats_++ % 100) == 0) {
      REXGPU_WARN("nx1_d3d9: unsupported Xenos texture format {} (sampler {})", t.format, sampler);
    }
    // The guest *expects* a texture here, so leaving the sampler unbound is worse than
    // wrong: a null sample reads back 0, and for the depth/shadow maps (fmt 23) that
    // means "fully occluded" -- the whole world shades black. White reads as "far /
    // unshadowed" and keeps lighting sane until real render targets land.
    return white_;
  }
  entry.last_frame = frame_;

  const auto* fmt = rex::graphics::FormatInfo::Get(t.format);
  if (!fmt) {
    return nullptr;
  }
  const uint32_t bpb = fmt->bytes_per_block();
  const uint32_t pitch_pixels = t.pitch_pixels ? t.pitch_pixels : t.width;
  rex::graphics::TextureExtent extent = rex::graphics::TextureExtent::Calculate(
      fmt, pitch_pixels, height, /*depth=*/1, t.tiled, /*is_guest=*/true);
  // Calculate() derives block_width from the pitch, but the host texture is created
  // at the *visible* width t.width. Cap the copy width to the visible blocks so
  // DetileMip2D fills exactly the created texture and never writes past its rows
  // (a wider pitch stays in block_pitch_h, used only for source addressing). Writing
  // the padded pitch width into a t.width texture corrupts the heap.
  extent.block_width = (t.width + fmt->block_width - 1) / fmt->block_width;

  // Mips are generated on the host, not read from the guest.
  //
  // NX1's fetch constants do declare a chain (mip_max_level 6..10) with the levels in a
  // separate allocation at mip_address -- but for most textures that memory does not hold
  // this texture's mips. Dumping the guest bytes and decoding them offline showed level 1 of
  // a trash decal resolving, cleanly and at exactly the pitch and tiling the layout predicts,
  // into an entirely different image; only textures whose mip_address happens to be
  // `base_address + base_size` (one contiguous allocation) carry their own chain. The rest
  // point into a shared pool that this build never fills -- it has no imagefile to stream
  // from -- so those levels are another image's pixels, permanently. Reading them is what
  // turned every minified surface into confetti.
  //
  // We only need *a* correct chain, not the artist's: the defect being fixed is aliasing.
  // Let the driver filter one down from level 0, which is data we know is right.
  //
  // This includes the CPU-decoded BC-alpha formats. They were excluded once, which left them
  // as the only textures in the game with no mip chain at all -- and the scope's noise masks
  // are exactly that: a DXT5A grain map, tiled hard and heavily minified. With no mips it
  // aliased its own noise, which is what filled the scope lens with coloured static. They
  // decode to A8R8G8B8, which mips like anything else.
  // Two ways to get a chain: let the driver filter one (uncompressed formats only), or build
  // it here (the block-compressed ones, which the driver refuses -- see the mip-gen section).
  //
  // Deliberately *not* gated on the guest's mip_max_level. A texture whose fetch constant
  // declares no chain is not a texture that must not be filtered -- it is one whose mips never
  // arrived, because this build has no imagefile to stream them from. On the console those same
  // surfaces have mips and are filtered; honouring the declaration reproduced a statement the
  // guest only makes because its data is missing, and left ~10% of the world aliasing. We
  // filter from level 0 regardless, so the guest's chain is not something we need.
  const bool auto_mips = SupportsAutoMips(device_, host.d3d);
  const bool bc_mips = IsBlockCompressed(host.d3d);
  const bool want_mips = REXCVAR_GET(nx1_d3d9_mips) && (auto_mips || bc_mips);
  const bool build_mips = want_mips && !auto_mips;
  const uint32_t levels = build_mips ? BcMipLevels(t.width, height) : 1;

  // Track how each texture's mip chain is provided (built here / driver auto-gen / skipped), so the
  // periodic "mipgen" stats line can show the split across the whole run.
  if (build_mips) {
    ++mips_built_;
  } else if (want_mips) {
    ++mips_auto_;
  } else if (t.mip_max_level == 0) {
    ++mips_skip_nochain_;
  } else {
    ++mips_skip_unsupported_;
  }

  const size_t guest_bytes = size_t(extent.block_pitch_h) * extent.block_pitch_v * bpb;
  // Decode from the CPU snapshot mirror, NOT live guest RAM. The mirror captured these bytes while
  // the texture was resident (early sweep / first touch) and holds them; live RAM would have the
  // streaming pool's transient fill by now, which is the confetti. entry.dirty (set by the write-
  // watch, via DrainMemoryWrites, when the guest rewrites this texture) is what forces a fresh
  // decode from the re-snapshotted mirror.
  const uint8_t* src = MirrorSnapshot(t.base_address, uint32_t(guest_bytes));
  if (entry.tex && entry.layout_key == layout_key && !entry.dirty) {
    return entry.tex;
  }
  if (entry.tex && entry.layout_key != layout_key) {
    entry.tex->Release();
    entry.tex = nullptr;
    ++tex_rebuilds_;
    // A different texture now occupies this key -- restart its warm-up before it re-commits.
    entry.good_frames = 0;
    entry.committed = false;
  }

  // Set when a broadcast-swizzle sprite decodes to a fully transparent (unstreamed) image, so it is
  // not committed and keeps re-decoding until its texel pool is resident.
  bool swizzle_all_zero = false;

  // IDirect3DDevice9Ex has no D3DPOOL_MANAGED. Fill a lockable SYSTEMMEM staging
  // texture with level 0 and UpdateTexture it into a DEFAULT texture that can be sampled.
  IDirect3DTexture9* staging = nullptr;
  if (FAILED(device_->CreateTexture(t.width, height, levels, 0, host.d3d, D3DPOOL_SYSTEMMEM,
                                    &staging, nullptr))) {
    REXGPU_ERROR("nx1_d3d9: staging CreateTexture({}x{}, fmt {}) failed", t.width, height,
                 t.format);
    return nullptr;
  }

  D3DLOCKED_RECT locked;
  if (FAILED(staging->LockRect(0, &locked, nullptr, 0))) {
    staging->Release();
    return nullptr;
  }
  // The runtime's pitch for a block format it does not recognise counts texels, not the block
  // row it actually has to hold -- 4x too small for BC5. The driver reads such a level as
  // tightly packed block rows, which is exactly what its `width * height` bytes hold.
  const uint32_t dst_row_bytes =
      host.opaque_block ? extent.block_width * bpb : uint32_t(locked.Pitch);
  auto* dst = static_cast<uint8_t*>(locked.Pitch >= 0 ? locked.pBits : nullptr);
  if (dst) {
    if (host.decode == TexDecode::kColorSwizzle) {
      // Detile the compressed colour blocks, then decode to A8R8G8B8 applying the channel swizzle
      // (the compressed host format cannot honour a broadcast swizzle) -- see src_bc above.
      std::vector<uint8_t> scratch(size_t(extent.block_width) * extent.block_height * bpb);
      DetileMip2D(scratch.data(), extent.block_width * bpb, src, extent, bpb, t.endian, t.tiled);
      DecodeBcColorSwizzledToArgb(dst, uint32_t(locked.Pitch), scratch.data(), extent.block_width,
                                  extent.block_height, t.width, height, src_bc, bpb, t.swizzle);
      // A broadcast-swizzle sprite that decodes to all zero is not resident yet: its texel pool has
      // not been streamed in (the same residency gap behind the distant-surface confetti). Caching
      // that as a settled texture would leave the effect permanently invisible even after the data
      // lands, so mark it dirty to force a re-decode until it carries real content.
      uint64_t sa = 0;
      for (uint32_t y = 0; y < height; ++y) {
        const uint8_t* r = dst + size_t(y) * locked.Pitch;
        for (uint32_t x = 0; x < t.width; ++x) sa += r[x * 4 + 3];
      }
      swizzle_all_zero = sa == 0;
    } else if (host.decode != TexDecode::kNone) {
      // CPU-decode single-channel BC-alpha: detile the compressed 8-byte blocks
      // into a linear scratch buffer, then expand each block to A8R8G8B8 (v,v,v,v).
      std::vector<uint8_t> scratch(size_t(extent.block_width) * extent.block_height * bpb);
      DetileMip2D(scratch.data(), extent.block_width * bpb, src, extent, bpb, t.endian, t.tiled);
      DecodeBCAlphaToArgb(dst, uint32_t(locked.Pitch), scratch.data(), extent.block_width,
                          extent.block_height, t.width, height,
                          host.decode == TexDecode::kDXT5A);
    } else {
      DetileMip2D(dst, dst_row_bytes, src, extent, bpb, t.endian, t.tiled);
      const Swizzle32 swz = MakeSwizzle32(t.swizzle);
      if (host.swizzle32 && !swz.identity) {
        for (uint32_t by = 0; by < extent.block_height; ++by) {
          SwizzleRow32(dst + size_t(by) * dst_row_bytes, extent.block_width, swz);
        }
      }
    }
  }

  // Filter the chain down from the level 0 we just wrote, while it is still mapped: decode it
  // once, then halve-and-re-encode into each level below. UpdateTexture carries the whole
  // staging chain across, so the levels have to be in place before the unlock.
  if (build_mips && dst) {
    std::vector<Rgba8> cur, next;
    DecodeBcImage(host.d3d, dst, dst_row_bytes, t.width, height, cur);
    for (uint32_t level = 1; level < levels; ++level) {
      const uint32_t lw = std::max(1u, t.width >> level);
      const uint32_t lh = std::max(1u, height >> level);
      BoxFilterHalf(cur, std::max(1u, t.width >> (level - 1)),
                    std::max(1u, height >> (level - 1)), next, lw, lh);

      D3DLOCKED_RECT mip;
      if (SUCCEEDED(staging->LockRect(level, &mip, nullptr, 0)) && mip.pBits) {
        const uint32_t mip_row_bytes = host.opaque_block
                                           ? ((lw + 3) / 4) * BcBlockBytes(host.d3d)
                                           : uint32_t(mip.Pitch);
        EncodeBcImage(host.d3d, next, lw, lh, static_cast<uint8_t*>(mip.pBits), mip_row_bytes);
        staging->UnlockRect(level);
      }
      cur.swap(next);
    }
  }
  // TEMP DIAGNOSTIC (no shadows): dump the decoded lightmap atlas (8888, 512x2048 / 512x1024)
  // while its level 0 is still mapped. IW4 bakes the sun's shadows into the lightmaps, and
  // forcing the dynamic shadow atlas changed nothing -- so the question is whether the baked
  // shadows are present in this data at all.
  if (dst && t.format == 6 && t.width == 512 && (height == 2048 || height == 1024)) {
    static uint32_t n = 0;
    if (n++ < 3) {
      char p[128];
      std::snprintf(p, sizeof(p), "texdump/lightmap_%08X_%ux%u.bmp", t.base_address, t.width,
                    height);
      if (FILE* f = std::fopen(p, "wb")) {
        const uint32_t row = (t.width * 3 + 3) & ~3u;
        uint8_t hdr[54] = {'B', 'M'};
        const uint32_t fsz = 54 + row * height, off = 54, ih = 40, data = row * height;
        const uint16_t planes = 1, bpp = 24;
        std::memcpy(hdr + 2, &fsz, 4);  std::memcpy(hdr + 10, &off, 4);
        std::memcpy(hdr + 14, &ih, 4);  std::memcpy(hdr + 18, &t.width, 4);
        std::memcpy(hdr + 22, &height, 4); std::memcpy(hdr + 26, &planes, 2);
        std::memcpy(hdr + 28, &bpp, 2); std::memcpy(hdr + 34, &data, 4);
        std::fwrite(hdr, 1, 54, f);
        std::vector<uint8_t> line(row, 0);
        for (int y = int(height) - 1; y >= 0; --y) {
          const uint8_t* r = dst + size_t(y) * locked.Pitch;
          for (uint32_t x = 0; x < t.width; ++x) {
            line[x * 3 + 0] = r[x * 4 + 0];
            line[x * 3 + 1] = r[x * 4 + 1];
            line[x * 3 + 2] = r[x * 4 + 2];
          }
          std::fwrite(line.data(), 1, row, f);
        }
        std::fclose(f);
        REXGPU_INFO("nx1_d3d9: LIGHTMAPDUMP wrote {} (swizzle {:#05x}, tiled={})", p, t.swizzle,
                    t.tiled);
      }
    }
  }

  // TEMP DIAGNOSTIC (colour grading): dump the composite's tonemap ramp (s8 = 256x1). With a
  // valid graded LUT (proven), the composite is oC0 = tex3D(LUT, ramp(scene)) -- so this ramp
  // sets the LUT lookup coords. Log its curve at a few points, decoded as the shader sees it
  // (A8R8G8B8 in memory = [B][G][R][A]). A straight 0->255 line is identity (correct passthrough
  // into the LUT); a compressed/flat/wrong curve mis-indexes the grade.
  if (t.width == 256 && height == 1 && dst) {
    static std::mutex sm;
    static std::vector<uint32_t> sseen;
    std::lock_guard<std::mutex> lk(sm);
    if (std::find(sseen.begin(), sseen.end(), t.base_address) == sseen.end() &&
        sseen.size() < 8) {
      sseen.push_back(t.base_address);
      // fmt=2 (k_8) builds as L8 -- ONE byte per texel, sampled as luminance replicated to RGB.
      // Read the row at its real host pitch and stride by the actual bytes/texel.
      const uint8_t* row = static_cast<const uint8_t*>(locked.pBits);
      const uint32_t bpt = uint32_t(locked.Pitch) / 256u;  // bytes per texel (1 for L8)
      char b[420];
      int n = std::snprintf(b, sizeof(b), "nx1_d3d9: RAMP8 @%08X fmt=%u bpt=%u 256x1:",
                            t.base_address, t.format, bpt);
      for (uint32_t x = 0; x <= 256; x += 32) {
        const uint32_t xi = x > 255 ? 255 : x;
        n += std::snprintf(b + n, sizeof(b) - n, " [%u]=%u", xi, row[size_t(xi) * bpt]);
      }
      REXGPU_INFO("{}", b);
    }
  }

  staging->UnlockRect(0);

  // D3DUSAGE_AUTOGENMIPMAP reports a single level and keeps its sub-levels to itself: the
  // driver filters them down from the level 0 UpdateTexture writes. A chain we built ourselves
  // is the ordinary case instead -- real levels on both textures, copied across as they are.
  if (!entry.tex &&
      FAILED(device_->CreateTexture(t.width, height, auto_mips && want_mips ? 0 : levels,
                                    auto_mips && want_mips ? D3DUSAGE_AUTOGENMIPMAP : 0, host.d3d,
                                    D3DPOOL_DEFAULT, &entry.tex, nullptr))) {
    REXGPU_ERROR("nx1_d3d9: CreateTexture({}x{}, fmt {}) failed", t.width, height, t.format);
    staging->Release();
    entry.tex = nullptr;
    ++tex_failures_;
    return nullptr;
  }
  if (FAILED(device_->UpdateTexture(staging, entry.tex))) {
    REXGPU_ERROR("nx1_d3d9: UpdateTexture({}x{}, fmt {}) failed", t.width, height, t.format);
    staging->Release();
    ++tex_failures_;
    return nullptr;
  }
  staging->Release();
  ++tex_uploads_;
  if (want_mips && auto_mips) {
    entry.tex->SetAutoGenFilterType(D3DTEXF_LINEAR);
    entry.tex->GenerateMipSubLevels();
  }

  entry.layout_key = layout_key;
  entry.dirty = false;
  // A broadcast-swizzle sprite that came out fully transparent is not resident yet: keep it dirty
  // (re-decode next bind) and never let it commit, so it recovers the moment its pool streams in.
  if (swizzle_all_zero) {
    entry.dirty = true;
    entry.good_frames = 0;
    entry.committed = false;
  }
  // Record the guest byte range so DrainMemoryWrites can dirty this entry when the guest rewrites
  // it. The mirror itself armed the write-watch when it captured these pages in MirrorSnapshot.
  entry.watch_addr = t.base_address;
  entry.watch_size = uint32_t(guest_bytes);
  return entry.tex;
}

IDirect3DBaseTexture9* ResourceTracker::PreferLargestForSurface(uint64_t surface_key,
                                                                uint32_t sampler, uint32_t format,
                                                                IDirect3DBaseTexture9* tex,
                                                                uint32_t width, uint32_t height) {
  if (!surface_key || !tex || !best_textures_) return tex;
  auto* map = static_cast<BestTextureMap*>(best_textures_);
  // Key on format as well as surface+sampler: the same geometry is drawn in several passes (shadow,
  // depth pre-pass, colour) that each bind a DIFFERENT texture to the same sampler, so conflating
  // them substituted, e.g., a normal map for the albedo. Separating by format keeps each pass's
  // texture in its own lineage.
  auto& b = (*map)[MixKey(MixKey(surface_key, sampler), format)];
  b.last_frame = frame_;
  const uint32_t area = width * height;
  if (area > b.area) {
    // STRICTLY larger -> the new best. Adopt it, holding a reference so it survives the base-keyed
    // cache evicting the entry it came from. Strictly-larger (not >=) is deliberate: a same-size
    // re-read of a texture whose slot was recycled to garbage must NOT replace the clean retained one.
    if (b.tex) b.tex->Release();
    tex->AddRef();
    b.tex = tex;
    b.area = area;
    return tex;
  }
  // Same size: a fresh binding at the largest resolution (a render target, or updated content). Keep
  // the retained texture but show the current one -- nothing is frozen stale at full resolution.
  if (area == b.area) return tex;
  // Smaller than what this surface has shown before: a receding LOD. Serve the retained full-res,
  // whose host-built mip chain the driver minifies to clean pixels instead of the recycled garbage.
  return b.tex ? b.tex : tex;
}

void ResourceTracker::SetBackbuffer(IDirect3DSurface9* surface, uint32_t width, uint32_t height) {
  backbuffer_ = surface;
  backbuffer_width_ = width;
  backbuffer_height_ = height;
}

IDirect3DSurface9* ResourceTracker::GetRenderTargetSurface(const uint8_t* base,
                                                           uint32_t guest_surface) {
  if (!device_ || !render_targets_ || !guest_surface) {
    return nullptr;
  }
  SurfaceSize s{};
  if (!ReadSurfaceSize(base, guest_surface, &s.width, &s.height)) {
    return nullptr;
  }
  auto* map = static_cast<TargetMap*>(render_targets_);
  auto& t = (*map)[ReadSurfaceEdramKey(base, guest_surface)];
  if (t.surface && t.width == s.width && t.height == s.height && !t.is_depth) {
    return t.surface;
  }
  if (t.surface && !t.is_backbuffer) {
    t.surface->Release();
  }
  if (t.tex) {
    t.tex->Release();
  }
  t = HostTarget{};

  // The guest's final composite goes to a display-sized target; render it straight
  // into the backbuffer so Present shows it without an extra copy.
  if (backbuffer_ && s.width == backbuffer_width_ && s.height == backbuffer_height_) {
    t.surface = backbuffer_;
    t.width = s.width;
    t.height = s.height;
    t.is_backbuffer = true;
    return t.surface;
  }

  if (FAILED(device_->CreateTexture(s.width, s.height, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8,
                                    D3DPOOL_DEFAULT, &t.tex, nullptr)) ||
      !t.tex) {
    REXGPU_ERROR("nx1_d3d9: render target CreateTexture({}x{}) failed", s.width, s.height);
    t = HostTarget{};
    return nullptr;
  }
  if (FAILED(t.tex->GetSurfaceLevel(0, &t.surface))) {
    t.tex->Release();
    t = HostTarget{};
    return nullptr;
  }
  t.width = s.width;
  t.height = s.height;
  REXGPU_INFO("nx1_d3d9: created colour target {}x{} for guest surface 0x{:08X}", s.width, s.height,
              guest_surface);
  return t.surface;
}

IDirect3DSurface9* ResourceTracker::GetDepthSurface(const uint8_t* base, uint32_t guest_surface,
                                                    uint32_t min_width, uint32_t min_height) {
  if (!device_ || !render_targets_ || !guest_surface) {
    return nullptr;
  }
  SurfaceSize s{};
  if (!ReadSurfaceSize(base, guest_surface, &s.width, &s.height)) {
    return nullptr;
  }
  // D3D9 drops every draw whose depth-stencil is smaller than its render target, and the
  // shadow depth surfaces really are smaller than what they render: they decode to
  // 1024x1024 / 512x512 but the pass targets (and resolves) 1024x2048 / 512x2048, because
  // the guest stacks cascades into an atlas. So grow the depth to cover the bound colour
  // target -- but only in *height*. Growing the width too let a stale 1024- or 1920-wide
  // target from a previous pass drag the narrow shadow depths up with it, and every grow
  // reallocates, throwing away the contents mid-frame.
  if (min_width <= s.width) {
    s.height = std::max(s.height, min_height);
  }
  auto* map = static_cast<TargetMap*>(render_targets_);
  auto& t = (*map)[ReadSurfaceEdramKey(base, guest_surface)];
  // Reuse while it still covers what is being asked for; only ever grow.
  if (t.surface && t.is_depth && t.width >= s.width && t.height >= s.height) {
    return t.surface;
  }
  if (t.surface && t.is_depth) {
    s.width = std::max(s.width, t.width);
    s.height = std::max(s.height, t.height);
  }
  if (t.surface && !t.is_backbuffer) {
    t.surface->Release();
  }
  if (t.tex) {
    t.tex->Release();
  }
  t = HostTarget{};
  t.is_depth = true;

  // INTZ keeps the depth buffer sampleable, which is the whole point: the guest
  // resolves its shadow maps and scene depth and the lighting shaders read them back.
  // Fall back to a plain (unsampleable) depth-stencil if the driver lacks INTZ.
  if (intz_supported_ &&
      SUCCEEDED(device_->CreateTexture(s.width, s.height, 1, D3DUSAGE_DEPTHSTENCIL, D3DFMT_INTZ,
                                       D3DPOOL_DEFAULT, &t.tex, nullptr)) &&
      t.tex && SUCCEEDED(t.tex->GetSurfaceLevel(0, &t.surface))) {
    t.width = s.width;
    t.height = s.height;
    REXGPU_INFO("nx1_d3d9: created INTZ depth target {}x{} for guest surface 0x{:08X}", s.width,
                s.height, guest_surface);
    return t.surface;
  }
  if (t.tex) {
    t.tex->Release();
    t.tex = nullptr;
  }
  if (FAILED(device_->CreateDepthStencilSurface(s.width, s.height, D3DFMT_D24S8,
                                                D3DMULTISAMPLE_NONE, 0, TRUE, &t.surface,
                                                nullptr))) {
    REXGPU_ERROR("nx1_d3d9: depth target {}x{} failed", s.width, s.height);
    t = HostTarget{};
    return nullptr;
  }
  t.width = s.width;
  t.height = s.height;
  return t.surface;
}

IDirect3DTexture9* ResourceTracker::GetDepthTexture(const uint8_t* base, uint32_t guest_surface) {
  if (!render_targets_ || !guest_surface) {
    return nullptr;
  }
  auto* map = static_cast<TargetMap*>(render_targets_);
  auto it = map->find(ReadSurfaceEdramKey(base, guest_surface));
  return (it != map->end() && it->second.is_depth) ? it->second.tex : nullptr;
}

IDirect3DBaseTexture9* ResourceTracker::GetVolumeTexture(const TextureFetchConstant& t,
                                                         uint32_t sampler) {
  const HostTextureFormat host = PickHostTextureFormat(t.format);
  if (!host.valid || host.decode != TexDecode::kNone) {
    return nullptr;  // no compressed/decoded volume formats in the corpus
  }
  const auto* fmt = rex::graphics::FormatInfo::Get(t.format);
  if (!fmt || !t.depth) {
    return nullptr;
  }
  const uint32_t bpb = fmt->bytes_per_block();
  const uint32_t pitch_pixels = t.pitch_pixels ? t.pitch_pixels : t.width;
  const rex::graphics::TextureExtent extent = rex::graphics::TextureExtent::Calculate(
      fmt, pitch_pixels, t.height, t.depth, t.tiled, /*is_guest=*/true);
  const uint32_t blocks_wide = (t.width + fmt->block_width - 1) / fmt->block_width;
  const uint32_t blocks_high = (t.height + fmt->block_height - 1) / fmt->block_height;

  const uint64_t layout_key =
      MixKey(MixKey(t.base_address, (uint64_t(t.format) << 32) | t.width),
             MixKey((uint64_t(t.height) << 16) | t.depth, (uint64_t(t.swizzle) << 1) | t.tiled));
  const uint64_t key = MixKey(t.base_address, sampler | 0x8000u);  // distinct from the 2D key
  auto* map = static_cast<TextureMap*>(textures_);
  auto& entry = (*map)[key];
  if (entry.vol && entry.last_frame == frame_ && entry.layout_key == layout_key) {
    return entry.vol;
  }
  entry.last_frame = frame_;

  const size_t guest_bytes =
      size_t(extent.block_pitch_h) * extent.block_pitch_v * std::max(1u, t.depth) * bpb;
  const uint8_t* src = MirrorSnapshot(t.base_address, uint32_t(guest_bytes));
  const uint64_t content_hash = XXH3_64bits(src, guest_bytes);

  // TEMP DIAGNOSTIC (colour grading): the composite is `oC0 = tex3D(s7, ...)`, a pure LUT
  // replacement -- a truly-black LUT would make the frame black, and it is not. So log, on
  // EVERY call for a small cubic LUT (not just cache-miss), whether the guest memory is empty
  // and whether we are about to serve a cache hit. If the LUT is empty on a cache HIT, we are
  // permanently serving a black texture built once from empty RAM.
  if (t.width <= 64 && t.depth >= 8 && t.width == t.depth) {
    static std::mutex hm;
    static uint32_t hcount = 0;
    std::lock_guard<std::mutex> lk(hm);
    if ((hcount++ % 240) == 0) {
      uint32_t nz = 0;
      for (size_t bi = 0; bi < guest_bytes; ++bi) nz += src[bi] != 0 ? 1 : 0;
      const bool cache_hit =
          entry.vol && entry.layout_key == layout_key && entry.content_hash == content_hash;
      REXGPU_INFO("nx1_d3d9: LUTLIVE s{} @{:08X} {}^3 nonzero={}/{} cachehit={} hosttex={}",
                  sampler, t.base_address, t.width, nz, uint32_t(guest_bytes), cache_hit,
                  entry.vol ? "have" : "none");
    }
  }

  if (entry.vol && entry.layout_key == layout_key && entry.content_hash == content_hash) {
    return entry.vol;
  }
  if (entry.vol && entry.layout_key != layout_key) {
    entry.vol->Release();
    entry.vol = nullptr;
  }

  // TEMP DIAGNOSTIC (sun ~10x too weak): identify the volume light. The world PS turns this
  // sample into ambient as `(2*V)^2`, so V is squared straight into the frame and competes
  // with a sun of NdotL*g18 ~= 1.06. Log what we actually build: a gamma flag we honour with
  // SRGBTEXTURE (D3D9 sRGB reads are a FORMAT CAP -- drivers commonly do not support them for
  // VOLUME textures, so setting it may be silently ignored, leaving V gamma-encoded and the
  // ambient ~5x hot), and the host format (a float format could legitimately exceed 1).
  {
    static std::mutex vm;
    static std::vector<uint64_t> vseen;
    std::lock_guard<std::mutex> lk(vm);
    if (std::find(vseen.begin(), vseen.end(), layout_key) == vseen.end() && vseen.size() < 8) {
      vseen.push_back(layout_key);
      REXGPU_INFO(
          "nx1_d3d9: VOLTEX s{} {}x{}x{} guestfmt={} hostfmt=0x{:X} gamma={} tiled={} swizzle={:03X}",
          sampler, t.width, t.height, t.depth, t.format, uint32_t(host.d3d), t.gamma, t.tiled,
          t.swizzle & 0xFFF);
    }
  }

  IDirect3DVolumeTexture9* staging = nullptr;
  if (FAILED(device_->CreateVolumeTexture(t.width, t.height, t.depth, 1, 0, host.d3d,
                                          D3DPOOL_SYSTEMMEM, &staging, nullptr))) {
    REXGPU_ERROR("nx1_d3d9: staging CreateVolumeTexture({}x{}x{}, fmt {}) failed", t.width,
                 t.height, t.depth, t.format);
    return nullptr;
  }
  D3DLOCKED_BOX box;
  if (FAILED(staging->LockBox(0, &box, nullptr, 0))) {
    staging->Release();
    return nullptr;
  }
  DetileMip3D(static_cast<uint8_t*>(box.pBits), uint32_t(box.RowPitch), uint32_t(box.SlicePitch),
              src, blocks_wide, blocks_high, t.depth, extent.block_pitch_h, bpb, t.endian, t.tiled);
  const Swizzle32 swz = MakeSwizzle32(t.swizzle);
  if (host.swizzle32 && !swz.identity) {
    for (uint32_t z = 0; z < t.depth; ++z) {
      auto* slice = static_cast<uint8_t*>(box.pBits) + size_t(z) * box.SlicePitch;
      for (uint32_t y = 0; y < blocks_high; ++y) {
        SwizzleRow32(slice + size_t(y) * box.RowPitch, blocks_wide, swz);
      }
    }
  }
  // TEMP DIAGNOSTIC (colour grading): dump the corners of the small cubic LUT AS THE SHADER
  // WILL SEE IT (post-detile, post-swizzle). The composite binds it `ok`, so the grade is
  // applied -- the question is whether the content is a real warm grade or has been flattened
  // / channel-swapped in our path. Identity LUT: cell(i,i,i) ~= (i/(N-1)) grey. A real grade
  // deviates (warm = R>B at the top). BGRA in memory: byte order is [B][G][R][A].
  // Dedup on CONTENT hash, not layout_key: the LUT is built empty on frame 1 then populated,
  // and keying on layout_key only ever dumped the empty first build. Now we also capture the
  // real, populated grade.
  if (t.width <= 64 && t.depth >= 8 && t.width == t.depth) {
    static std::mutex lm;
    static std::vector<uint64_t> lseen;
    std::lock_guard<std::mutex> lk(lm);
    if (std::find(lseen.begin(), lseen.end(), content_hash) == lseen.end() && lseen.size() < 24) {
      lseen.push_back(content_hash);
      const uint32_t N = t.width;
      auto cell = [&](uint32_t x, uint32_t y, uint32_t z, char* out, size_t cap) {
        const uint8_t* px = static_cast<const uint8_t*>(box.pBits) + size_t(z) * box.SlicePitch +
                            size_t(y) * box.RowPitch + size_t(x) * 4;
        std::snprintf(out, cap, "(B%u G%u R%u A%u)", px[0], px[1], px[2], px[3]);
      };
      char c0[32], c1[32], c2[32], c3[32], c4[32];
      cell(0, 0, 0, c0, sizeof(c0));
      cell(N / 4, N / 4, N / 4, c1, sizeof(c1));
      cell(N / 2, N / 2, N / 2, c2, sizeof(c2));
      cell(3 * N / 4, 3 * N / 4, 3 * N / 4, c3, sizeof(c3));
      cell(N - 1, N - 1, N - 1, c4, sizeof(c4));
      REXGPU_INFO("nx1_d3d9: LUTDIAG {}^3 fmt={} swizzle={:03X} diag: {} {} {} {} {}", N, t.format,
                  t.swizzle & 0xFFF, c0, c1, c2, c3, c4);

      // Dump the whole cube as a (N*N) wide by N tall RGB montage (the N z-slices laid out
      // left-to-right) -- a correct colour LUT is a smooth cube; a misplaced/sparse
      // reconstruction (our 25%-nonzero suspect) shows as scattered lit cells on black.
      uint32_t diag_nz = 0;
      for (size_t bi = 0; bi < guest_bytes; ++bi) diag_nz += src[bi] != 0 ? 1 : 0;
      if (diag_nz > 0) {
        const uint32_t W = N * N, H = N;
        const uint32_t stride = (W * 3 + 3) & ~3u;
        std::vector<uint8_t> img(size_t(stride) * H, 0);
        for (uint32_t z = 0; z < N; ++z) {
          const uint8_t* slice =
              static_cast<const uint8_t*>(box.pBits) + size_t(z) * box.SlicePitch;
          for (uint32_t y = 0; y < N; ++y) {
            const uint8_t* row = slice + size_t(y) * box.RowPitch;
            for (uint32_t x = 0; x < N; ++x) {
              const uint8_t* px = row + size_t(x) * 4;  // BGRA
              uint8_t* out = img.data() + size_t(H - 1 - y) * stride + (z * N + x) * 3;
              out[0] = px[0];  // B
              out[1] = px[1];  // G
              out[2] = px[2];  // R
            }
          }
        }
        char path[128];
        std::snprintf(path, sizeof(path), "texdump/lut_%08X.bmp", t.base_address);
        if (std::FILE* f = std::fopen(path, "wb")) {
          BITMAPFILEHEADER fh{};
          BITMAPINFOHEADER ih{};
          fh.bfType = 0x4D42;
          fh.bfOffBits = sizeof(fh) + sizeof(ih);
          fh.bfSize = fh.bfOffBits + uint32_t(img.size());
          ih.biSize = sizeof(ih);
          ih.biWidth = LONG(W);
          ih.biHeight = LONG(H);
          ih.biPlanes = 1;
          ih.biBitCount = 24;
          std::fwrite(&fh, sizeof(fh), 1, f);
          std::fwrite(&ih, sizeof(ih), 1, f);
          std::fwrite(img.data(), 1, img.size(), f);
          std::fclose(f);
          REXGPU_INFO("nx1_d3d9: LUTDIAG dumped {} ({}x{})", path, W, H);
        }
      }
      // Separate "we read the wrong/empty memory" from "detile zeroed good data": hash + first
      // dwords of the RAW guest bytes, and the address/tiling we resolved from.
      uint32_t nz = 0;
      for (size_t bi = 0; bi < guest_bytes; ++bi) nz += src[bi] != 0 ? 1 : 0;
      // Is this a GPU-produced texture? GetTexture (2D) serves resolve destinations from the
      // rendered image; GetVolumeTexture does NOT. If the LUT address is a resolve dest, that
      // gap is the bug -- we read empty CPU RAM where the GPU wrote the grade.
      bool is_resolve = false;
      if (resolves_) {
        auto* rm = static_cast<ResolveMap*>(resolves_);
        is_resolve = rm->find(PhysicalAddress(t.base_address)) != rm->end();
      }
      REXGPU_INFO("nx1_d3d9: LUTRAW @{:08X} bytes={} nonzero={} resolvedest={} tiled={} bpb={} "
                  "mip@{:08X} hash={:016X} raw0={:08X}",
                  t.base_address, uint32_t(guest_bytes), nz, is_resolve, t.tiled, bpb,
                  t.mip_address, content_hash, *reinterpret_cast<const uint32_t*>(src));
    }
  }

  staging->UnlockBox(0);

  if (!entry.vol &&
      FAILED(device_->CreateVolumeTexture(t.width, t.height, t.depth, 1, 0, host.d3d,
                                          D3DPOOL_DEFAULT, &entry.vol, nullptr))) {
    REXGPU_ERROR("nx1_d3d9: CreateVolumeTexture({}x{}x{}, fmt {}) failed", t.width, t.height,
                 t.depth, t.format);
    staging->Release();
    entry.vol = nullptr;
    return nullptr;
  }
  const HRESULT hr = device_->UpdateTexture(staging, entry.vol);
  staging->Release();
  if (FAILED(hr)) {
    REXGPU_ERROR("nx1_d3d9: UpdateTexture(volume {}x{}x{}) failed", t.width, t.height, t.depth);
    return nullptr;
  }
  entry.layout_key = layout_key;
  entry.content_hash = content_hash;
  return entry.vol;
}

IDirect3DBaseTexture9* ResourceTracker::GetCubeTexture(const TextureFetchConstant& t,
                                                       uint32_t sampler) {
  const HostTextureFormat host = PickHostTextureFormat(t.format);
  // The CPU BC-alpha decode is a 2D-only path; NX1's cube maps are all DXT1/DXN, so it
  // never comes up. Skip rather than upload garbage.
  if (!host.valid || host.decode != TexDecode::kNone) {
    return nullptr;
  }
  const auto* fmt = rex::graphics::FormatInfo::Get(t.format);
  if (!fmt || !t.width || t.width != t.height) {
    return nullptr;
  }
  const uint32_t bpb = fmt->bytes_per_block();
  const uint32_t pitch_pixels = t.pitch_pixels ? t.pitch_pixels : t.width;
  rex::graphics::TextureExtent extent = rex::graphics::TextureExtent::Calculate(
      fmt, pitch_pixels, t.height, /*depth=*/1, t.tiled, /*is_guest=*/true);
  // As in the 2D path: copy only the visible blocks, so the detile fills exactly the
  // texture we create at t.width and never runs past its rows.
  extent.block_width = (t.width + fmt->block_width - 1) / fmt->block_width;

  // The six faces are consecutive array slices, and Xenos aligns every array slice to
  // 4 KB -- so the face stride is the padded face size, not the visible one.
  const uint32_t align =
      1u << rex::graphics::xenos::kTextureSubresourceAlignmentBytesLog2;
  const uint32_t face_bytes = extent.block_pitch_h * extent.block_pitch_v * bpb;
  const uint32_t face_stride = (face_bytes + align - 1) & ~(align - 1);

  const uint64_t layout_key =
      MixKey(MixKey(t.base_address, (uint64_t(t.format) << 32) | t.width),
             MixKey((uint64_t(t.height) << 1) | (t.tiled ? 1 : 0), t.swizzle));
  const uint64_t key = MixKey(t.base_address, sampler | 0x4000u);  // distinct from 2D and 3D
  auto* map = static_cast<TextureMap*>(textures_);
  auto& entry = (*map)[key];
  if (entry.cube && entry.last_frame == frame_ && entry.layout_key == layout_key) {
    return entry.cube;
  }
  entry.last_frame = frame_;

  const uint8_t* src = MirrorSnapshot(t.base_address, uint32_t(size_t(face_stride) * 6));
  const uint64_t content_hash = XXH3_64bits(src, size_t(face_stride) * 6);
  if (entry.cube && entry.layout_key == layout_key && entry.content_hash == content_hash) {
    return entry.cube;
  }
  if (entry.cube && entry.layout_key != layout_key) {
    entry.cube->Release();
    entry.cube = nullptr;
  }

  // A cube map needs its chain as much as any 2D texture -- more, in fact. These are the
  // environment reflections: the vector into them swings across most of a face from one pixel
  // to the next on a surface seen at a distance, so an unfiltered cube map returns a different
  // corner of the sky per pixel. That is the coloured sparkle, and no 2D mip chain touches it.
  const bool auto_mips = SupportsAutoMips(device_, host.d3d);
  const bool build_mips = REXCVAR_GET(nx1_d3d9_mips) && !auto_mips && IsBlockCompressed(host.d3d);
  const bool want_mips = REXCVAR_GET(nx1_d3d9_mips) && (auto_mips || build_mips);
  const uint32_t levels = build_mips ? BcMipLevels(t.width, t.width) : 1;

  IDirect3DCubeTexture9* staging = nullptr;
  if (FAILED(device_->CreateCubeTexture(t.width, levels, 0, host.d3d, D3DPOOL_SYSTEMMEM, &staging,
                                        nullptr))) {
    REXGPU_ERROR("nx1_d3d9: staging CreateCubeTexture({}, fmt {}) failed", t.width, t.format);
    return nullptr;
  }
  // Xenos face order is D3D's: +X, -X, +Y, -Y, +Z, -Z. Each face is an ordinary tiled 2D
  // image, so the 2D detiler handles it -- only the base pointer moves.
  for (uint32_t face = 0; face < 6; ++face) {
    const D3DCUBEMAP_FACES d3d_face = D3DCUBEMAP_FACES(D3DCUBEMAP_FACE_POSITIVE_X + face);
    D3DLOCKED_RECT locked;
    if (FAILED(staging->LockRect(d3d_face, 0, &locked, nullptr, 0))) {
      staging->Release();
      return nullptr;
    }
    auto* dst = static_cast<uint8_t*>(locked.pBits);
    // As in the 2D path, an unrecognised block FourCC (DXN) reports a pitch of one byte per
    // texel rather than the block row it has to hold.
    const uint32_t dst_row_bytes =
        host.opaque_block ? extent.block_width * bpb : uint32_t(locked.Pitch);
    DetileMip2D(dst, dst_row_bytes, src + size_t(face) * face_stride, extent, bpb, t.endian,
                t.tiled);
    const Swizzle32 swz = MakeSwizzle32(t.swizzle);
    if (host.swizzle32 && !swz.identity) {
      for (uint32_t by = 0; by < extent.block_height; ++by) {
        SwizzleRow32(dst + size_t(by) * dst_row_bytes, extent.block_width, swz);
      }
    }

    if (build_mips) {
      std::vector<Rgba8> cur, next;
      DecodeBcImage(host.d3d, dst, dst_row_bytes, t.width, t.width, cur);
      for (uint32_t level = 1; level < levels; ++level) {
        const uint32_t ls = std::max(1u, t.width >> level);
        BoxFilterHalf(cur, std::max(1u, t.width >> (level - 1)),
                      std::max(1u, t.width >> (level - 1)), next, ls, ls);
        D3DLOCKED_RECT mip;
        if (SUCCEEDED(staging->LockRect(d3d_face, level, &mip, nullptr, 0)) && mip.pBits) {
          const uint32_t mip_row_bytes = host.opaque_block
                                             ? ((ls + 3) / 4) * BcBlockBytes(host.d3d)
                                             : uint32_t(mip.Pitch);
          EncodeBcImage(host.d3d, next, ls, ls, static_cast<uint8_t*>(mip.pBits), mip_row_bytes);
          staging->UnlockRect(d3d_face, level);
        }
        cur.swap(next);
      }
    }
    staging->UnlockRect(d3d_face, 0);
  }

  if (!entry.cube &&
      FAILED(device_->CreateCubeTexture(t.width, auto_mips && want_mips ? 0 : levels,
                                        auto_mips && want_mips ? D3DUSAGE_AUTOGENMIPMAP : 0,
                                        host.d3d, D3DPOOL_DEFAULT, &entry.cube, nullptr))) {
    REXGPU_ERROR("nx1_d3d9: CreateCubeTexture({}, fmt {}) failed", t.width, t.format);
    staging->Release();
    entry.cube = nullptr;
    return nullptr;
  }
  const HRESULT hr = device_->UpdateTexture(staging, entry.cube);
  staging->Release();
  if (FAILED(hr)) {
    REXGPU_ERROR("nx1_d3d9: UpdateTexture(cube {}, fmt {}) failed", t.width, t.format);
    return nullptr;
  }
  if (want_mips && auto_mips) {
    entry.cube->SetAutoGenFilterType(D3DTEXF_LINEAR);
    entry.cube->GenerateMipSubLevels();
  }
  entry.layout_key = layout_key;
  entry.content_hash = content_hash;
  return entry.cube;
}

IDirect3DTexture9* ResourceTracker::GetResolvedTexture(uint32_t address) {
  if (!resolves_ || !address) {
    return nullptr;
  }
  auto* map = static_cast<ResolveMap*>(resolves_);
  auto it = map->find(PhysicalAddress(address));
  return it != map->end() ? it->second.tex : nullptr;
}

bool ResourceTracker::EnsureDepthBlit() {
  if (depth_blit_ps_) {
    return true;
  }
  if (depth_blit_failed_ || !device_) {
    return false;
  }
  depth_blit_failed_ = true;  // cleared on success

  // Reads the INTZ depth surface and writes it as a plain float. ps_2_0, not 3_0: the
  // quad is drawn with pre-transformed (XYZRHW) vertices through the fixed-function
  // vertex pipeline, and D3D9 refuses to pair that with a 3_0 pixel shader.
  // c0.x is a debug override (nx1_d3d9_shadow_test): x < 0 passes depth through, otherwise
  // the blit writes the constant c0.x so the shadow compare can be exercised end-to-end.
  static const char kSource[] =
      "sampler2D depth : register(s0);\n"
      "float4 dbg : register(c0);\n"
      "float4 main(float2 uv : TEXCOORD0) : COLOR {\n"
      "  float d = tex2D(depth, uv).r;\n"
      "  return (dbg.x < 0 ? d : dbg.x).rrrr;\n"
      "}\n";

  ID3DBlob* code = nullptr;
  ID3DBlob* errors = nullptr;
  const HRESULT hr = D3DCompile(kSource, sizeof(kSource) - 1, "depth_blit", nullptr, nullptr,
                                "main", "ps_2_0", 0, 0, &code, &errors);
  if (FAILED(hr) || !code) {
    REXGPU_ERROR("nx1_d3d9: depth-blit shader failed to compile: {}",
                 errors ? static_cast<const char*>(errors->GetBufferPointer()) : "?");
  } else if (SUCCEEDED(device_->CreatePixelShader(
                 static_cast<const DWORD*>(code->GetBufferPointer()), &depth_blit_ps_))) {
    depth_blit_failed_ = false;
  }
  if (code) {
    code->Release();
  }
  if (errors) {
    errors->Release();
  }
  return !depth_blit_failed_;
}

void ResourceTracker::ResolveDepth(uint32_t dest_address, uint32_t width, uint32_t height,
                                   IDirect3DTexture9* src_depth, const RECT& src_rect,
                                   const POINT& dest_point) {
  if (!device_ || !resolves_ || !dest_address || !width || !height || !src_depth) {
    return;
  }
  D3DSURFACE_DESC sd{};
  if (FAILED(src_depth->GetLevelDesc(0, &sd)) || !sd.Width || !sd.Height || !EnsureDepthBlit()) {
    return;
  }

  auto* map = static_cast<ResolveMap*>(resolves_);
  auto& entry = (*map)[PhysicalAddress(dest_address)];
  if (entry.tex && (!entry.owned || entry.width != width || entry.height != height)) {
    if (entry.owned) {
      entry.tex->Release();
    }
    entry.tex = nullptr;
  }
  const bool fresh = entry.tex == nullptr;
  if (fresh) {
    if (FAILED(device_->CreateTexture(width, height, 1, D3DUSAGE_RENDERTARGET, D3DFMT_R32F,
                                      D3DPOOL_DEFAULT, &entry.tex, nullptr))) {
      REXGPU_ERROR("nx1_d3d9: depth resolve target R32F {}x{} failed", width, height);
      entry.tex = nullptr;
      return;
    }
    entry.width = width;
    entry.height = height;
    entry.owned = true;
    REXGPU_INFO("nx1_d3d9: depth resolve atlas R32F {}x{} for 0x{:08X} (src surface {}x{})", width,
                height, PhysicalAddress(dest_address), sd.Width, sd.Height);
  }
  IDirect3DSurface9* dst = nullptr;
  if (FAILED(entry.tex->GetSurfaceLevel(0, &dst)) || !dst) {
    return;
  }

  RECT sr = src_rect;
  if (sr.right <= sr.left || sr.bottom <= sr.top) {  // empty => whole surface
    sr = RECT{0, 0, LONG(sd.Width), LONG(sd.Height)};
  }
  sr.right = std::min<LONG>(sr.right, LONG(sd.Width));
  sr.bottom = std::min<LONG>(sr.bottom, LONG(sd.Height));
  RECT dr{dest_point.x, dest_point.y, dest_point.x + (sr.right - sr.left),
          dest_point.y + (sr.bottom - sr.top)};
  dr.right = std::min<LONG>(dr.right, LONG(width));
  dr.bottom = std::min<LONG>(dr.bottom, LONG(height));
  if (dr.right <= dr.left || dr.bottom <= dr.top) {
    dst->Release();
    return;
  }

  // The render target and depth-stencil are the only device state the per-draw path does
  // not rewrite, so they are all that has to be put back.
  IDirect3DSurface9* old_rt = nullptr;
  IDirect3DSurface9* old_ds = nullptr;
  device_->GetRenderTarget(0, &old_rt);
  device_->GetDepthStencilSurface(&old_ds);

  device_->SetDepthStencilSurface(nullptr);  // before the target: depth must cover the RT
  device_->SetRenderTarget(0, dst);
  if (fresh) {
    // 0 is "far" under this engine's reverse-Z, i.e. nothing occluding. A slot the guest
    // has not resolved into yet then reads as unshadowed instead of fully shadowed.
    device_->Clear(0, nullptr, D3DCLEAR_TARGET, 0, 1.0f, 0);
  }
  const D3DVIEWPORT9 vp{0, 0, width, height, 0.0f, 1.0f};
  device_->SetViewport(&vp);

  device_->SetVertexShader(nullptr);
  device_->SetVertexDeclaration(nullptr);
  device_->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
  device_->SetPixelShader(depth_blit_ps_);
  // TEMP DIAGNOSTIC (no shadows): nx1_d3d9_shadow_test >= 0 replaces every shadow-atlas
  // depth with that constant. 1.0 = "nearest occluder everywhere": if the lighting
  // shaders' shadow path is live, the world must go fully shadowed.
  const int32_t shadow_test = REXCVAR_GET(nx1_d3d9_shadow_test);
  // Scope the forcing to the shadow cascades. ResolveDepth also serves the 1024x600 SCENE
  // depth, and forcing that too sent the DOF chain "everything is far" -- a full-screen blur
  // that masqueraded as a shadow-term pulse in the 200 flicker and contaminated every earlier
  // A/B. Cascade atlases are >= 1024 tall; the scene depth is 600.
  float forced = shadow_test < 0 || height < 1024 ? -1.0f : float(shadow_test) / 100.0f;
  if (shadow_test == 200 && height >= 1024) {
    forced = (GetTickCount64() / 500) & 1 ? 1.0f : 0.0f;
  }
  const float dbg[4] = {forced, 0.0f, 0.0f, 0.0f};
  device_->SetPixelShaderConstantF(0, dbg, 1);
  device_->SetTexture(0, src_depth);
  device_->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
  device_->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
  device_->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
  device_->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
  device_->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
  device_->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, FALSE);
  // The blit must copy raw depth values: the guest RT bound before this resolve is
  // usually a gamma surface, which leaves sRGB *write* conversion enabled on the device.
  // Save it across the blit -- the per-draw path does not reissue it (it is set at
  // SetRenderTarget time), so leaking FALSE back out would undo the gamma encode of
  // every draw until the next target switch.
  DWORD old_srgb_write = FALSE;
  device_->GetRenderState(D3DRS_SRGBWRITEENABLE, &old_srgb_write);
  device_->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
  device_->SetRenderState(D3DRS_ZENABLE, FALSE);
  device_->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
  device_->SetRenderState(D3DRS_STENCILENABLE, FALSE);
  device_->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
  device_->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
  device_->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
  device_->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
  device_->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);

  // Pre-transformed quad, half-pixel biased so texel centres land on pixel centres.
  struct BlitVertex {
    float x, y, z, rhw, u, v;
  };
  const float su = 1.0f / float(sd.Width);
  const float sv = 1.0f / float(sd.Height);
  const float u0 = float(sr.left) * su, u1 = float(sr.right) * su;
  const float v0 = float(sr.top) * sv, v1 = float(sr.bottom) * sv;
  const float x0 = float(dr.left) - 0.5f, x1 = float(dr.right) - 0.5f;
  const float y0 = float(dr.top) - 0.5f, y1 = float(dr.bottom) - 0.5f;
  const BlitVertex quad[4] = {{x0, y0, 0.0f, 1.0f, u0, v0},
                              {x1, y0, 0.0f, 1.0f, u1, v0},
                              {x0, y1, 0.0f, 1.0f, u0, v1},
                              {x1, y1, 0.0f, 1.0f, u1, v1}};
  device_->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(BlitVertex));

  // TEMP DIAGNOSTIC (no shadows): sample the atlas content after each blit. Degenerate
  // stats (all zero / all one) mean the cascade passes never wrote depth; varied values
  // mean the content is fine and the fault is in the lighting shader's sampling/compare.
  {
    static uint32_t n = 0;
    if ((n++ % 181) == 0) {
      IDirect3DSurface9* off = nullptr;
      if (SUCCEEDED(device_->CreateOffscreenPlainSurface(width, height, D3DFMT_R32F,
                                                         D3DPOOL_SYSTEMMEM, &off, nullptr)) &&
          off) {
        if (SUCCEEDED(device_->GetRenderTargetData(dst, off))) {
          D3DLOCKED_RECT lr{};
          if (SUCCEEDED(off->LockRect(&lr, nullptr, D3DLOCK_READONLY))) {
            float mn = 1e30f, mx = -1e30f;
            double sum = 0;
            uint64_t cnt = 0, zeros = 0;
            for (uint32_t y = 0; y < height; y += 4) {
              const float* row = reinterpret_cast<const float*>(
                  static_cast<const uint8_t*>(lr.pBits) + size_t(y) * lr.Pitch);
              for (uint32_t x = 0; x < width; x += 4) {
                const float v = row[x];
                mn = std::min(mn, v);
                mx = std::max(mx, v);
                sum += v;
                ++cnt;
                zeros += v == 0.0f ? 1 : 0;
              }
            }
            REXGPU_INFO("nx1_d3d9: SHADOWATLAS 0x{:08X} {}x{} sr=({},{},{},{}) at=({},{}) "
                        "min={:.6f} max={:.6f} mean={:.6f} zero={}% [shadow_test={}]",
                        PhysicalAddress(dest_address), width, height, sr.left, sr.top, sr.right,
                        sr.bottom, dest_point.x, dest_point.y, mn, mx,
                        sum / double(std::max<uint64_t>(1, cnt)),
                        zeros * 100 / std::max<uint64_t>(1, cnt), shadow_test);
            // Dump the atlas itself: the stats say "real depth", but only the image says
            // whether it is a *sun-view depth render of this scene* and whether both cascade
            // tiles are populated. Normalized by max -- the useful values occupy a thin slice
            // of [0,1] (the light-space depth range is ~62900 world units).
            if (REXCVAR_GET(nx1_d3d9_dump_shadow) && mx > mn) {
              static uint32_t dump_n = 0;
              char path[256];
              std::snprintf(path, sizeof(path), "texdump/shadow_atlas_%08X_%u.bmp",
                            PhysicalAddress(dest_address), dump_n++);
              if (std::FILE* f = std::fopen(path, "wb")) {
                const uint32_t stride = (width * 3 + 3) & ~3u;
                BITMAPFILEHEADER fh{};
                BITMAPINFOHEADER ih{};
                fh.bfType = 0x4D42;
                fh.bfOffBits = sizeof(fh) + sizeof(ih);
                fh.bfSize = fh.bfOffBits + stride * height;
                ih.biSize = sizeof(ih);
                ih.biWidth = LONG(width);
                ih.biHeight = LONG(height);  // positive: rows are written bottom-up
                ih.biPlanes = 1;
                ih.biBitCount = 24;
                std::fwrite(&fh, sizeof(fh), 1, f);
                std::fwrite(&ih, sizeof(ih), 1, f);
                std::vector<uint8_t> row(stride, 0);
                for (uint32_t y = 0; y < height; ++y) {
                  const float* src = reinterpret_cast<const float*>(
                      static_cast<const uint8_t*>(lr.pBits) + size_t(height - 1 - y) * lr.Pitch);
                  for (uint32_t x = 0; x < width; ++x) {
                    const float t = (src[x] - mn) / (mx - mn);
                    const uint8_t g = uint8_t((t < 0 ? 0 : t > 1 ? 1 : t) * 255.0f + 0.5f);
                    row[x * 3 + 0] = row[x * 3 + 1] = row[x * 3 + 2] = g;
                  }
                  std::fwrite(row.data(), stride, 1, f);
                }
                std::fclose(f);
                REXGPU_INFO("nx1_d3d9: SHADOWATLAS dumped {}", path);
              }
            }
            off->UnlockRect();
          }
        }
        off->Release();
      }
    }
  }

  device_->SetTexture(0, nullptr);
  device_->SetPixelShader(nullptr);
  device_->SetRenderState(D3DRS_SRGBWRITEENABLE, old_srgb_write);
  device_->SetRenderTarget(0, old_rt);
  device_->SetDepthStencilSurface(old_ds);
  if (old_rt) {
    old_rt->Release();
  }
  if (old_ds) {
    old_ds->Release();
  }
  dst->Release();
}

void ResourceTracker::ResolveColor(uint32_t dest_address, uint32_t width, uint32_t height,
                                   const RECT& src_rect, const POINT& dest_point) {
  if (!device_ || !resolves_ || !dest_address || !width || !height) {
    return;
  }
  auto* map = static_cast<ResolveMap*>(resolves_);
  auto& entry = (*map)[PhysicalAddress(dest_address)];

  if (entry.tex && (!entry.owned || entry.width != width || entry.height != height)) {
    if (entry.owned) {
      entry.tex->Release();
    }
    entry.tex = nullptr;
  }
  if (!entry.tex) {
    // Render-target texture so StretchRect can write it and shaders can sample it.
    if (FAILED(device_->CreateTexture(width, height, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8,
                                      D3DPOOL_DEFAULT, &entry.tex, nullptr))) {
      REXGPU_ERROR("nx1_d3d9: resolve target CreateTexture({}x{}) failed", width, height);
      entry.tex = nullptr;
      return;
    }
    entry.width = width;
    entry.height = height;
    entry.owned = true;
  }

  IDirect3DSurface9* src = nullptr;
  IDirect3DSurface9* dst = nullptr;
  if (SUCCEEDED(device_->GetRenderTarget(0, &src)) && src &&
      SUCCEEDED(entry.tex->GetSurfaceLevel(0, &dst)) && dst) {
    // Copy the resolved region out of whichever host target the guest is currently
    // rendering into. Under predicated tiling this is one *band* of the frame, and it
    // must land at dest_point -- blitting it over the whole destination smears the last
    // band across everything.
    const bool whole = src_rect.right <= src_rect.left || src_rect.bottom <= src_rect.top;
    D3DSURFACE_DESC sd{};
    src->GetDesc(&sd);
    RECT sr = whole ? RECT{0, 0, LONG(sd.Width), LONG(sd.Height)} : src_rect;
    // Clamp the source to the target we actually have.
    sr.right = std::min<LONG>(sr.right, LONG(sd.Width));
    sr.bottom = std::min<LONG>(sr.bottom, LONG(sd.Height));
    if (sr.right > sr.left && sr.bottom > sr.top) {
      RECT dr{dest_point.x, dest_point.y, dest_point.x + (sr.right - sr.left),
              dest_point.y + (sr.bottom - sr.top)};
      dr.right = std::min<LONG>(dr.right, LONG(width));
      dr.bottom = std::min<LONG>(dr.bottom, LONG(height));
      if (dr.right > dr.left && dr.bottom > dr.top) {
        device_->StretchRect(src, &sr, dst, &dr, D3DTEXF_POINT);
      }
    }
  }
  if (dst) dst->Release();
  if (src) src->Release();
}

}  // namespace nx1::d3d9

#endif  // _WIN32
