/**
 * @file    guest_d3d.h
 * @brief   Layout of the guest Xbox 360 D3D::CDevice, and helpers to read it.
 *
 * NX1 statically links the Xenon D3D library. Its D3DDevice_* setters are a
 * *deferred* state machine: they only write shadow state into the guest
 * D3D::CDevice and set dirty bits in m_Pending.m_Mask[]. Nothing is submitted
 * until a draw, which flushes that shadow state into PM4 packets.
 *
 * We exploit that. Rather than hooking ~80 setters (several of which the engine
 * inlines, so they have no hookable entry point at all), we let the guest
 * setters run untouched and read the resulting shadow state at draw time.
 * Whatever the engine wrote -- through a call or inlined -- landed here.
 *
 * Offsets recovered from 5-nx1mp_demo.xex via IDA + the shipped PDB, and
 * cross-checked against the literal displacements in D3DDevice_DrawIndexedVertices
 * (device+1920 -> VS constants, device+6016 -> PS constants).
 */

#pragma once

#include <cstdint>

#include <rex/types.h>

namespace nx1::d3d9 {

/// Byte offsets into the guest `D3D::CDevice`.
/// `D3DDevice` (0x2B00 bytes) is embedded at offset 0; CDevice fields follow it.
namespace guest_device {

// --- D3DDevice ---
inline constexpr uint32_t kPendingMask = 0x0000;  ///< uint64 m_Mask[5] -- dirty bits
/// m_Mask[0] / m_Mask[1] tag the VS / PS float constants, one bit per group of 4
/// registers, numbered from the MSB: register r is bit 63 - (r >> 2). A draw flushes the
/// tagged groups out of the shadow and then *zeroes* the word -- so anything that needs to
/// know what was dirty must read it before the guest's draw body runs. See
/// d3d9_constants.h.
inline constexpr uint32_t kAluDirtyMaskVs = kPendingMask + 0x0000;
inline constexpr uint32_t kAluDirtyMaskPs = kPendingMask + 0x0008;
inline constexpr uint32_t kRingPtr = 0x0030;      ///< uint32* m_pRing

inline constexpr uint32_t kConstants = 0x0480;  ///< _D3DConstants (9120 bytes)

/// 32 texture fetch constants, 6 dwords (24 bytes) each.
/// Vertex streams alias the tail of this array as 2-dword slots:
/// stream 0 -> slot 95, stream 1 -> slot 94 (see D3DDevice_SetStreamSource).
inline constexpr uint32_t kFetchConstants = kConstants + 0x0000;
inline constexpr uint32_t kFetchConstantStride = 24;
inline constexpr uint32_t kFetchConstantCount = 32;

/// 512 float4 ALU constants: VS occupies c0..c255, PS occupies c256..c511.
inline constexpr uint32_t kAluConstants = kConstants + 0x0300;
inline constexpr uint32_t kVsConstants = kAluConstants;               // device + 0x0780
inline constexpr uint32_t kPsConstants = kAluConstants + 256 * 16;    // device + 0x1780
inline constexpr uint32_t kBoolLoopConstants = kConstants + 0x2300;

inline constexpr uint32_t kAluConstantCount = 256;  ///< per stage

/// GPU register shadow packets.
inline constexpr uint32_t kValuesPacket = 0x28CC;   ///< GPU_VALUESPACKET
inline constexpr uint32_t kControlPacket = 0x2934;  ///< GPU_CONTROLPACKET

inline constexpr uint32_t kAlphaRef = kValuesPacket + 0x38;      ///< float, already 0..1
inline constexpr uint32_t kColorMask = kValuesPacket + 0x10;     ///< RB_COLOR_MASK, 4 bits per RT

// GPU_CONTROLPACKET fields (all 32-bit GPU register shadows the setters write).
inline constexpr uint32_t kDepthControl = kControlPacket + 0x00;   ///< GPU_DEPTHCONTROL
inline constexpr uint32_t kBlendControl0 = kControlPacket + 0x04;  ///< GPU_BLENDCONTROL (RT0)
inline constexpr uint32_t kColorControl = kControlPacket + 0x08;   ///< GPU_COLORCONTROL
inline constexpr uint32_t kClipControl = kControlPacket + 0x10;    ///< GPU_CLIPCONTROL
inline constexpr uint32_t kModeControl = kControlPacket + 0x14;    ///< GPU_MODECONTROL (cull/fill)
inline constexpr uint32_t kVteControl = kControlPacket + 0x18;     ///< GPU_VTECONTROL

// GPU_VALUESPACKET viewport scale/offset (PA_CL_VPORT_*), floats.
inline constexpr uint32_t kVportXScale = kValuesPacket + 0x3C;
inline constexpr uint32_t kVportXOffset = kValuesPacket + 0x40;
inline constexpr uint32_t kVportYScale = kValuesPacket + 0x44;
inline constexpr uint32_t kVportYOffset = kValuesPacket + 0x48;
inline constexpr uint32_t kVportZScale = kValuesPacket + 0x4C;
inline constexpr uint32_t kVportZOffset = kValuesPacket + 0x50;

// --- D3D::CDevice (past the embedded D3DDevice) ---
inline constexpr uint32_t kVertexDeclaration = 0x2F58;  ///< D3D::CVertexDeclaration*
inline constexpr uint32_t kIndexBuffer = 0x318C;        ///< D3DIndexBuffer*
inline constexpr uint32_t kVertexBuffers = 0x31A4;      ///< D3DVertexBuffer* [16]
inline constexpr uint32_t kPixelShader = 0x328C;        ///< D3D::CPixelShader*
inline constexpr uint32_t kVertexShader = 0x3290;       ///< D3D::CVertexShader*

/// m_StreamStride[16]: one *byte* per stream, holding StrideInBytes >> 2.
/// `D3DDevice_SetStreamSource` writes it as `stb r9, 0x31E8(device + StreamNumber)`.
inline constexpr uint32_t kStreamStride = 0x31E8;

inline constexpr uint32_t kMaxVertexStreams = 16;

}  // namespace guest_device

// D3DCLEAR_* flags as the Xbox 360 defines them -- *not* the desktop D3D9 values.
// D3D::ClearF (guest 0x820E6860) loops `(1 << i) & flags` over render targets 0..3, so
// the low nibble is the colour targets and depth/stencil live above it.
inline constexpr uint32_t kClearTargetAny = 0x0F;  ///< D3DCLEAR_TARGET0..3
inline constexpr uint32_t kClearZBuffer = 0x10;
inline constexpr uint32_t kClearStencil = 0x20;

// D3DRESOLVE_* flags, as R_ResolveAndClear_Xbox360 (guest 0x824F05C8) sets them: the
// Xbox 360 can also clear EDRAM on the way out of a resolve.
inline constexpr uint32_t kResolveClearRenderTarget = 0x100;
inline constexpr uint32_t kResolveClearDepthStencil = 0x200;

/// D3DIndexBuffer, and the D3DVertexDeclaration the guest stamps for a decl.
namespace guest_resource {

// D3DIndexBuffer : D3DResource { Common, ReferenceCount, Fence, ReadFence,
//                               Identifier, BaseFlush } + Address + Size
inline constexpr uint32_t kIbCommon = 0x00;
inline constexpr uint32_t kIbAddress = 0x18;
inline constexpr uint32_t kIbSize = 0x1C;

/// `XGSetIndexBufferHeader` folds the D3DFORMAT into Common as `Format << 29`.
/// D3DFMT_INDEX32 has bit 2 set, so Common's sign bit means 32-bit indices, and
/// Common bits 29..30 are the Xenos endian swap (1 = 8in16, 2 = 8in32) that
/// `DrawIndexedVertices` copies into the draw packet.
inline constexpr uint32_t kIbIndex32Bit = 0x80000000;

// D3D::CVertexDeclaration : D3DVertexDeclaration(24) + m_Count + m_MaxStream +
//                           <16 bytes> + m_Uniqueness + m_Element[]
// Elements are the *Xbox 360* _D3DVERTEXELEMENT9, which is 12 bytes, not 8:
//   dword0 = (Stream << 16) | Offset
//   dword1 = Type            (a packed GPU fetch descriptor, not a small enum)
//   dword2 = (Method << 24) | (Usage << 16) | (UsageIndex << 8)
inline constexpr uint32_t kDeclCount = 0x18;
inline constexpr uint32_t kDeclMaxStream = 0x1C;
inline constexpr uint32_t kDeclUniqueness = 0x30;
inline constexpr uint32_t kDeclElements = 0x34;
inline constexpr uint32_t kDeclElementStride = 12;

}  // namespace guest_resource

/// Layout of D3D::CVertexShader / D3D::CPixelShader, and of the `_UCODE_HEADER`
/// each embeds at `kFunctionOffset`.
namespace guest_shader {

inline constexpr uint32_t kVsFunctionOffset = 0x368;  ///< CVertexShader::m_Function
inline constexpr uint32_t kVsPhysicalOffset = 0x20;   ///< CVertexShader::m_dwPhysical
inline constexpr uint32_t kPsFunctionOffset = 0x28;   ///< CPixelShader::m_Function
inline constexpr uint32_t kPsPhysicalOffset = 0x18;   ///< CPixelShader::m_dwPhysical

// _UCODE_HEADER { Cookie, CachedSize, PhysicalSize, DebuggerHintOffset,
//                 constantTableOffset, Pass[2] }
// _UCODE_PASS_HEADER { definitionTableOffset, microcodeOffset }
inline constexpr uint32_t kUcodePassArray = 0x14;
inline constexpr uint32_t kUcodePassStride = 8;
inline constexpr uint32_t kDefinitionTableOffsetField = 0;
inline constexpr uint32_t kUcodeMicrocodeOffsetField = 4;

/// _UCODE_HEADER::Cookie bit 0x20: this vertex shader carries a second pass,
/// the predicated-Z / depth-only variant.
inline constexpr uint32_t kCookieHasSecondPass = 0x20;

}  // namespace guest_shader

//=============================================================================
// Guest memory access
//=============================================================================
// Guest memory is big-endian. `base` is the guest *virtual* membase supplied to
// every recompiled function; a normal virtual/image address indexes into it.
//
// The catch: the guest's GPU objects -- the D3D::CDevice itself, its shader
// objects, and the buffer pointers in its fetch constants -- are addressed through
// the physical-mirror windows (0xA0000000+, e.g. the device sits at 0xE460xxxx).
// Those do NOT alias 1:1 into the virtual membase (TranslateVirtual would add a
// per-heap host_address_offset that plain `base + addr` skips), so reading them
// off `base` yields zeroes -- which silently nulled every device-shadow read.
// Resolve them through the physical membase exactly as the GPU does. The physical
// path is out-of-line (guest_d3d.cpp) so this header need not pull in the kernel
// memory system -- which drags std::min in behind <windows.h>'s min macro.
const uint8_t* GuestTranslatePhysical(uint32_t guest_addr);

/// Host pointer to a raw GPU *physical* address (< 0x20000000): a vertex-fetch base, or a
/// shader's literal-constant payload. Not the same translation as the 0xA0000000+ mirrors
/// above -- passing one of those through `base + addr` faults.
const uint8_t* GuestTranslateGpuPhysical(uint32_t gpu_physical);

inline uint32_t GuestRead32(const uint8_t* base, uint32_t guest_addr) {
  const uint8_t* host =
      guest_addr >= 0xA0000000u ? GuestTranslatePhysical(guest_addr) : base + guest_addr;
  return *reinterpret_cast<const rex::be<uint32_t>*>(host);
}

inline uint16_t GuestRead16(const uint8_t* base, uint32_t guest_addr) {
  const uint8_t* host =
      guest_addr >= 0xA0000000u ? GuestTranslatePhysical(guest_addr) : base + guest_addr;
  return *reinterpret_cast<const rex::be<uint16_t>*>(host);
}

inline uint64_t GuestRead64(const uint8_t* base, uint32_t guest_addr) {
  return (uint64_t(GuestRead32(base, guest_addr)) << 32) | GuestRead32(base, guest_addr + 4);
}

inline float GuestReadF32(const uint8_t* base, uint32_t guest_addr) {
  const uint32_t bits = GuestRead32(base, guest_addr);
  float out;
  __builtin_memcpy(&out, &bits, sizeof(out));
  return out;
}

/// Address of texture fetch constant `slot` (0..31) within the guest device.
inline uint32_t FetchConstantAddr(uint32_t device, uint32_t slot) {
  return device + guest_device::kFetchConstants + slot * guest_device::kFetchConstantStride;
}

/// Address of vertex fetch constant `slot` (2-dword slots; stream 0 == slot 95).
inline uint32_t VertexFetchConstantAddr(uint32_t device, uint32_t slot) {
  return device + guest_device::kFetchConstants + slot * 8;
}

/// Vertex stream N is bound to vertex fetch constant (95 - N).
inline constexpr uint32_t VertexFetchSlotForStream(uint32_t stream) { return 95 - stream; }

//=============================================================================
// Decoded fetch-constant fields
//=============================================================================

/// A Xenos vertex fetch constant: where a vertex stream lives in guest memory.
///
///   dword0 = type:2 | address:30   (address counted in dwords, so `& ~3` is the byte address)
///   dword1 = endian:2 | size:24    (size counted in dwords, so `& ~3` is the byte size)
///
/// `XGSetVertexBufferHeader` stamps `dword1 = Length & 0x3FFFFFC | ... | 0x10000002`,
/// i.e. endian is always 2 (8in32 -- a plain dword byteswap).
struct VertexFetchConstant {
  uint32_t base_address;  ///< guest physical byte address
  uint32_t endian;        ///< 1=8in16, 2=8in32, 3=16in32
  uint32_t size_bytes;
};

inline VertexFetchConstant ReadVertexFetchConstant(const uint8_t* base, uint32_t device,
                                                   uint32_t stream) {
  const uint32_t addr = VertexFetchConstantAddr(device, VertexFetchSlotForStream(stream));
  const uint32_t d0 = GuestRead32(base, addr + 0);
  const uint32_t d1 = GuestRead32(base, addr + 4);
  return VertexFetchConstant{d0 & ~3u, d1 & 3u, d1 & 0x03FFFFFCu};
}

/// Stride of vertex stream `stream`, in bytes.
///
/// `D3DDevice_SetStreamSource` stores `StrideInBytes >> 2` as one byte per stream
/// in the `m_StreamStride[]` byte array at +0x31E8 (verified in IDA). The device is
/// a physical-mirror EA, so the byte must be read through the mirror translation --
/// a raw `base[device + ...]` misses the 0xE0-heap page offset and reads garbage.
inline uint32_t StreamStride(const uint8_t* base, uint32_t device, uint32_t stream) {
  const uint32_t addr = device + guest_device::kStreamStride + stream;
  const uint8_t* host = addr >= 0xA0000000u ? GuestTranslatePhysical(addr) : base + addr;
  return uint32_t(*host) * 4u;
}

/// Sampler address (clamp) modes.
///
/// IMPORTANT: `R_HW_SetSamplerState` writes these *inline*, straight into
/// fetch-constant dword[0] -- `D3DDevice_SetSamplerState_AddressU/V/W` are never
/// called and hooking them would see nothing. Read them from the fetch constant.
struct SamplerClampModes {
  uint32_t u, v, w;  ///< Xenos ClampMode (0=repeat, 1=mirror, 2=clamp-to-last, 3=mirror-once, ...)
};

inline SamplerClampModes ReadSamplerClampModes(const uint8_t* base, uint32_t device,
                                               uint32_t sampler) {
  const uint32_t d0 = GuestRead32(base, FetchConstantAddr(device, sampler) + 0);
  return SamplerClampModes{
      (d0 >> 10) & 0x7,  // ClampX
      (d0 >> 13) & 0x7,  // ClampY
      (d0 >> 16) & 0x7,  // ClampZ
  };
}

//=============================================================================
// Texture fetch constant
//=============================================================================

/// A Xenos texture fetch constant (`xe_gpu_texture_fetch_t`), decoded. Written by
/// `SetTexture(sampler N)` into the 6-dword fetch constant at slot N; the shader
/// samples through it, so it is the durable description of a bound texture.
struct TextureFetchConstant {
  bool valid;             ///< type field == kTexture (2)
  uint32_t base_address;  ///< guest physical byte address (dword1.base_address << 12)
  uint32_t format;        ///< xenos::TextureFormat
  uint32_t endian;        ///< xenos::Endian (0=none,1=8in16,2=8in32,3=16in32)
  bool gamma;             ///< TextureSign::kGamma on RGB: hardware linearizes on fetch
  bool tiled;
  uint32_t pitch_pixels;  ///< row pitch in texels (dword0.pitch << 5); 0 => derive from width
  uint32_t width;         ///< texels
  uint32_t height;        ///< texels
  uint32_t depth;         ///< texels / array slices (>=1)
  uint32_t dimension;     ///< xenos::DataDimension (0=1D,1=2D/stacked,2=3D,3=cube)
  uint32_t swizzle;       ///< 12-bit XE_GPU_TEXTURE_SWIZZLE (xyzw, 3 bits each)
  /// Xenos ClampMode per axis (0=repeat, 1=mirror, 2=clamp-to-last, 3=mirror-once, ...).
  /// These live in dword0 of this very fetch constant, so a caller that already has one must
  /// NOT call ReadSamplerClampModes -- that re-reads the same dword out of guest memory, once
  /// per bound sampler slot per draw (~21k times a frame).
  uint32_t clamp_u, clamp_v, clamp_w;
  // Sampler filters (D3DTEXTUREFILTERTYPE-like: 0=point, 1=linear on Xenos).
  uint32_t mag_filter;
  uint32_t min_filter;
  uint32_t mip_filter;
  /// xenos::AnisoFilter: 0 = off, 1..5 = max 1:1, 2:1, 4:1, 8:1, 16:1. NX1 leans on this --
  /// half its binds ask for a *point* mip filter, which on Xenos is not the crude choice it
  /// looks like: aniso is what keeps a grazing surface from shimmering, and the mip step is
  /// only there to bound the work. Ignoring the field left plain bilinear behind it.
  uint32_t aniso_filter;
  /// The mip chain the guest declares. Note the renderer does NOT read the guest's mip
  /// levels: for most textures the memory at mip_address holds a different image entirely
  /// (a streaming pool this build never fills), so the host filters its own chain down from
  /// level 0 instead. Only mip_max_level (is there a chain at all?) and the addresses (which
  /// key the texture cache) are used.
  uint32_t mip_min_level;
  uint32_t mip_max_level;
  uint32_t mip_address;
  bool packed_mips;
};

/// Decode a `GPUTEXTURE_FETCH_CONSTANT` (6 dwords) at an arbitrary guest address.
/// Used both for sampler slots and for a D3DBaseTexture's embedded Format (at
/// texture + 0x1C), e.g. a resolve destination.
inline TextureFetchConstant ReadTextureFetchConstantAt(const uint8_t* base, uint32_t addr) {
  // Translate once for all six dwords. They are 24 contiguous bytes, so they cannot straddle
  // the 0xA0000000 physical window, yet GuestRead32 re-tests it and recomputes the host
  // pointer on every one -- and this runs ~21k times a frame (every bound sampler slot of
  // every draw), where it was the largest part of the texture cache lookup.
  const uint8_t* p = addr >= 0xA0000000u ? GuestTranslatePhysical(addr) : base + addr;
  const uint32_t d0 = *reinterpret_cast<const rex::be<uint32_t>*>(p + 0);
  const uint32_t d1 = *reinterpret_cast<const rex::be<uint32_t>*>(p + 4);
  const uint32_t d2 = *reinterpret_cast<const rex::be<uint32_t>*>(p + 8);
  const uint32_t d3 = *reinterpret_cast<const rex::be<uint32_t>*>(p + 12);
  const uint32_t d4 = *reinterpret_cast<const rex::be<uint32_t>*>(p + 16);
  const uint32_t d5 = *reinterpret_cast<const rex::be<uint32_t>*>(p + 20);

  TextureFetchConstant t{};
  t.valid = (d0 & 0x3) == 2;  // FetchConstantType::kTexture
  // TextureSign::kGamma (3) on the RGB channels: the hardware linearizes the sample on
  // fetch. NX1 flags its colour maps and every resolve of a k_8_8_8_8_GAMMA target this
  // way -- mirror it with D3DSAMP_SRGBTEXTURE or the whole frame reads a gamma curve dark.
  t.gamma = ((d0 >> 2) & 0x3) == 3;
  t.pitch_pixels = ((d0 >> 22) & 0x1FF) << 5;
  t.tiled = ((d0 >> 31) & 0x1) != 0;
  t.clamp_u = (d0 >> 10) & 0x7;
  t.clamp_v = (d0 >> 13) & 0x7;
  t.clamp_w = (d0 >> 16) & 0x7;
  t.format = d1 & 0x3F;
  t.endian = (d1 >> 6) & 0x3;
  t.base_address = ((d1 >> 12) & 0xFFFFF) << 12;
  t.dimension = (d5 >> 9) & 0x3;
  t.swizzle = (d3 >> 1) & 0xFFF;
  t.mag_filter = (d3 >> 19) & 0x3;
  t.min_filter = (d3 >> 21) & 0x3;
  t.mip_filter = (d3 >> 23) & 0x3;
  t.aniso_filter = (d3 >> 25) & 0x7;
  t.mip_min_level = (d4 >> 2) & 0xF;
  t.mip_max_level = (d4 >> 6) & 0xF;
  t.packed_mips = ((d5 >> 11) & 0x1) != 0;
  t.mip_address = ((d5 >> 12) & 0xFFFFF) << 12;

  // Size is stored with 1 subtracted from each component; the field layout
  // depends on the dimension.
  if (t.dimension == 2) {  // k3D
    t.width = (d2 & 0x7FF) + 1;
    t.height = ((d2 >> 11) & 0x7FF) + 1;
    t.depth = ((d2 >> 22) & 0x3FF) + 1;
  } else {  // 1D / 2D / cube
    t.width = (d2 & 0x1FFF) + 1;
    t.height = ((d2 >> 13) & 0x1FFF) + 1;
    t.depth = (t.dimension == 3) ? 6 : (((d2 >> 26) & 0x3F) + 1);  // cube => 6 faces
  }
  return t;
}

/// The texture bound to `sampler` (its fetch constant at `device + 24*sampler`).
inline TextureFetchConstant ReadTextureFetchConstant(const uint8_t* base, uint32_t device,
                                                     uint32_t sampler) {
  return ReadTextureFetchConstantAt(base, FetchConstantAddr(device, sampler));
}

/// The Format fetch constant embedded in a D3DBaseTexture (a resolve dest, say).
inline constexpr uint32_t kBaseTextureFormatOffset = 0x1C;
inline TextureFetchConstant ReadBaseTextureFormat(const uint8_t* base, uint32_t texture_object) {
  return ReadTextureFetchConstantAt(base, texture_object + kBaseTextureFormatOffset);
}

/// Dimensions of a D3DSurface (an EDRAM render target).
///
/// A surface is *not* a texture: decoding its embedded fetch constant the way a
/// D3DBaseTexture's is decoded yields nonsense (441x4066), because an EDRAM surface has
/// no texture fetch constant. Its header instead carries a packed extent at +0x24:
///
///   ((width - 1) << 18) | ((height - 1) << 3)
///
/// This is the *logical* extent, and using it matters. The row pitch (+0x18) and byte
/// size (+0x2C) also encode a size, but that one is EDRAM-*padded*: it decodes the scene
/// depth as 1024x1105 and the shadow maps as 1024x2080 / 512x2240. Those surfaces get
/// resolved and sampled back at their true 1024x600 / 1024x2048 extents, so a padded
/// host target makes every depth and shadow lookup in the lighting shaders land on the
/// wrong texel -- and, because a depth target must cover its colour target, the padding
/// also dragged every depth surface up to the display's 1920 width as passes alternated.
///
/// Checked against every target NX1 uses: 1920x1080 display, 1024x600 scene,
/// 1024x2048 / 512x2048 shadow maps, 256x150 bloom -- exact on all five.
inline constexpr uint32_t kSurfaceExtentOffset = 0x24;
inline bool ReadSurfaceSize(const uint8_t* base, uint32_t surface, uint32_t* width,
                            uint32_t* height) {
  const uint32_t packed = GuestRead32(base, surface + kSurfaceExtentOffset);
  const uint32_t w = ((packed >> 18) & 0x3FFF) + 1;
  const uint32_t h = ((packed >> 3) & 0x7FFF) + 1;
  if (w < 2 || h < 2 || w > 8192 || h > 8192) {
    return false;
  }
  *width = w;
  *height = h;
  return true;
}

/// GPU_COLORINFO (GPU_DEPTHINFO for a depth surface), the field D3DDevice_SetRenderTarget
/// copies into m_DestinationPacket.Color0Info. Bits 0..11 are the EDRAM base in tiles;
/// bits 16..19 are the format.
inline constexpr uint32_t kSurfaceColorInfoOffset = 0x1C;
inline constexpr uint32_t kColorInfoFormatMask = 0x000F0000;

/// Identity of the EDRAM region a surface describes.
///
/// A D3DSurface is a *view*, not the memory. NX1 hands us two different D3DSurface objects
/// for the scene colour target -- same EDRAM base (0x062), same 1024x600 extent, differing
/// only in the format field -- draws the world through one and resolves through the other.
/// The bloom chain does the same with its two 256x150 surfaces. On hardware that is free:
/// EDRAM is a scratchpad and both views address the same tiles. Keying host render targets
/// by the D3DSurface pointer instead splits one region into two textures, so every colour
/// draw lands in one and the resolve reads the other -- a black frame with a perfectly
/// healthy draw count.
///
/// So key by what the hardware keys by: the EDRAM base and the extent, with the format bits
/// masked out. Surfaces that name the same tiles at the same size *are* the same target.
inline uint64_t ReadSurfaceEdramKey(const uint8_t* base, uint32_t surface) {
  const uint32_t info = GuestRead32(base, surface + kSurfaceColorInfoOffset) & ~kColorInfoFormatMask;
  const uint32_t extent = GuestRead32(base, surface + kSurfaceExtentOffset);
  return (uint64_t(info) << 32) | extent;
}

//=============================================================================
// Bound shader / declaration
//=============================================================================

inline uint32_t BoundVertexShader(const uint8_t* base, uint32_t device) {
  return GuestRead32(base, device + guest_device::kVertexShader);
}
inline uint32_t BoundPixelShader(const uint8_t* base, uint32_t device) {
  return GuestRead32(base, device + guest_device::kPixelShader);
}
inline uint32_t BoundVertexDeclaration(const uint8_t* base, uint32_t device) {
  return GuestRead32(base, device + guest_device::kVertexDeclaration);
}

//=============================================================================
// Bound shader microcode
//=============================================================================

/// Where a shader's microcode lives, and how much of it there is.
struct GuestUcode {
  uint32_t physical_address = 0;
  uint32_t dword_count = 0;
  bool valid() const { return physical_address != 0 && dword_count != 0; }
};

/// Resolve the microcode the GPU would actually execute for `shader_object`.
///
/// Mirrors D3D::SetPending_Shaders, which builds the PM4 IM_LOAD packet from
/// `m_Function[0].Pass[pass].microcodeOffset`:
///     address = *(uint32*)(function + microcodeOffset)     + m_dwPhysical
///     bytes   = *(uint32*)(function + microcodeOffset + 4)
inline GuestUcode ReadGuestUcode(const uint8_t* base, uint32_t shader_object, bool pixel_shader,
                                 uint32_t pass = 0) {
  if (!shader_object) {
    return {};
  }
  using namespace guest_shader;
  const uint32_t function = shader_object + (pixel_shader ? kPsFunctionOffset : kVsFunctionOffset);
  const uint32_t physical =
      GuestRead32(base, shader_object + (pixel_shader ? kPsPhysicalOffset : kVsPhysicalOffset));
  const uint32_t microcode_offset = GuestRead32(
      base, function + kUcodePassArray + pass * kUcodePassStride + kUcodeMicrocodeOffsetField);

  // Replicate the address D3D::SetPending_Shaders writes into the IM_LOAD packet,
  // which is exactly what Xenia's command processor hashes:
  //   packet_addr = ((raw >> 20) + 512) & 0x1000)   // 0xE0-heap page bit
  //               + (raw & 0x1FFFFFFC)               // physical, 4-aligned
  // The 4-align matters for pixel shaders: the guest stores the microcode pointer
  // with its low bit set (a type flag it strips with & 0x1FFFFFFE, and the GPU with
  // & ~3). Reading `raw` unmasked hashed a misaligned blob -> a hash absent from the
  // SM3 cache, even though the vertex shader (already aligned) matched.
  const uint32_t raw = GuestRead32(base, function + microcode_offset) + physical;
  GuestUcode ucode;
  ucode.physical_address = (((raw >> 20) + 512) & 0x1000) + (raw & 0x1FFFFFFCu);
  ucode.dword_count = GuestRead32(base, function + microcode_offset + 4) >> 2;
  return ucode;
}

//=============================================================================
// Shader literal constants
//=============================================================================

/// One literal-constant record out of a shader's definition table.
struct ShaderLiteral {
  uint32_t reg;      ///< ALU constant-file index (the file is shared: VS 0..255, PS 256..511)
  uint32_t float4s;  ///< how many consecutive registers this record supplies
  uint32_t address;  ///< GPU physical address of the payload (big-endian floats)
};

/// The Xenos ALU has no immediate float operands: a shader's literals live in the constant
/// file, and the compiler parks them at the top of it (c254/c255 -- the classic `(0,1,..)`
/// register a shader tests with `select(c255.x == 0, .., c255.y)`).
///
/// They never reach `m_Constants`. `D3D::SetLiteralShaderConstants` (guest 0x820D9808) walks
/// the shader's definition table and loads them *straight into the GPU constant file* over
/// PM4 -- the same shadow bypass as GpuBeginShaderConstantF4. Reading the shadow for those
/// registers yields zero, which collapses the vertex position of every shader that folds a
/// literal into its transform. Reproduce the guest's walk so the draw can resolve them.
///
/// Definition table: a 20-byte header (dword[4] = byte size of the record area) followed by
/// records of `{ u16 reg, u16 dwordCount, u32 dataOffset }`, terminated by dwordCount == 0.
/// The payload sits at `m_dwPhysical + dataOffset`.
inline uint32_t ReadShaderLiterals(const uint8_t* base, uint32_t shader_object, bool pixel_shader,
                                   ShaderLiteral* out, uint32_t max_out) {
  if (!shader_object || !max_out) {
    return 0;
  }
  using namespace guest_shader;
  const uint32_t function = shader_object + (pixel_shader ? kPsFunctionOffset : kVsFunctionOffset);
  const uint32_t def_offset =
      GuestRead32(base, function + kUcodePassArray + kDefinitionTableOffsetField);
  if (!def_offset) {
    return 0;
  }
  const uint32_t physical =
      GuestRead32(base, shader_object + (pixel_shader ? kPsPhysicalOffset : kVsPhysicalOffset));
  const uint32_t table = function + def_offset;
  const uint32_t end = table + 20 + GuestRead32(base, table + 16);

  uint32_t n = 0;
  for (uint32_t p = table + 20; p + 8 <= end && n < max_out; p += 8) {
    const uint32_t reg = GuestRead16(base, p);
    const uint32_t dwords = GuestRead16(base, p + 2);
    if (!dwords) {
      break;  // terminator
    }
    // Same physical-address fold the guest applies (and that ReadGuestUcode replicates).
    const uint32_t raw = physical + GuestRead32(base, p + 4);
    out[n].reg = reg;
    out[n].float4s = dwords / 4;
    out[n].address = (((raw >> 20) + 512) & 0x1000) + (raw & 0x1FFFFFFCu);
    ++n;
  }
  return n;
}

/// Which of a vertex shader's two passes the GPU will execute.
///
/// In D3D::SetPending_Shaders the pass index starts at 0 and is only raised to 1
/// inside the branch taken when **no pixel shader is bound**, and then only if the
/// shader cookie has `kCookieHasSecondPass`. That is the depth-only pre-pass.
/// (The Z-pass path additionally *loads* pass 1, but the pass the draw executes is
/// still this one.)
inline uint32_t VertexShaderPass(const uint8_t* base, uint32_t vs_object,
                                 bool has_pixel_shader) {
  if (has_pixel_shader || !vs_object) {
    return 0;
  }
  const uint32_t cookie = GuestRead32(base, vs_object + guest_shader::kVsFunctionOffset);
  return (cookie & guest_shader::kCookieHasSecondPass) ? 1u : 0u;
}

//=============================================================================
// Vertex declaration
//=============================================================================

/// One element of the guest vertex declaration, with `Type` already decoded.
///
/// The Xbox 360 `D3DDECLTYPE` is not a small enum -- it is a packed descriptor
/// that `D3D::PatchVertexShaderToMatchVertexDeclaration` splices straight into
/// the shader's `vfetch` instructions:
///     vfetch.format     (dword1 bits 16..21) = Type bits 0..5
///     vfetch.comp_all   (dword1 bit 12)      = Type bit 8   -- components are signed
///     vfetch.num_fmt_all(dword1 bit 13)      = Type bit 9   -- components are *not* normalized
struct GuestVertexElement {
  uint32_t stream;
  uint32_t offset;  ///< byte offset within the stream's vertex
  uint32_t format;  ///< xenos::VertexFormat
  bool is_signed;
  bool is_normalized;
  uint32_t usage;        ///< D3DDECLUSAGE (same numbering as the PC enum)
  uint32_t usage_index;  ///< D3DDECLUSAGE index
};

inline uint32_t VertexDeclarationCount(const uint8_t* base, uint32_t decl) {
  return GuestRead32(base, decl + guest_resource::kDeclCount);
}
inline uint32_t VertexDeclarationUniqueness(const uint8_t* base, uint32_t decl) {
  return GuestRead32(base, decl + guest_resource::kDeclUniqueness);
}

inline GuestVertexElement ReadVertexElement(const uint8_t* base, uint32_t decl, uint32_t index) {
  const uint32_t addr =
      decl + guest_resource::kDeclElements + index * guest_resource::kDeclElementStride;
  const uint32_t d0 = GuestRead32(base, addr + 0);
  const uint32_t type = GuestRead32(base, addr + 4);
  const uint32_t d2 = GuestRead32(base, addr + 8);
  return GuestVertexElement{
      d0 >> 16, d0 & 0xFFFF, type & 0x3F, ((type >> 8) & 1) != 0, ((type >> 9) & 1) == 0,
      (d2 >> 16) & 0xFF, (d2 >> 8) & 0xFF,
  };
}

//=============================================================================
// Index buffer
//=============================================================================

struct IndexBufferState {
  uint32_t base_address = 0;  ///< guest physical byte address
  uint32_t size_bytes = 0;
  uint32_t index_size = 2;  ///< 2 or 4
  uint32_t endian = 1;      ///< 1 = 8in16, 2 = 8in32
  bool valid() const { return base_address != 0 && size_bytes != 0; }
};

inline IndexBufferState ReadIndexBuffer(const uint8_t* base, uint32_t device) {
  const uint32_t ib = GuestRead32(base, device + guest_device::kIndexBuffer);
  if (!ib) {
    return {};
  }
  const uint32_t common = GuestRead32(base, ib + guest_resource::kIbCommon);
  IndexBufferState state;
  state.base_address = GuestRead32(base, ib + guest_resource::kIbAddress);
  state.size_bytes = GuestRead32(base, ib + guest_resource::kIbSize);
  state.index_size = (common & guest_resource::kIbIndex32Bit) ? 4u : 2u;
  state.endian = (common >> 29) & 3u;
  return state;
}

//=============================================================================
// Alpha test
//=============================================================================

/// `D3DDevice_SetRenderState_AlphaRef` stores an already-normalized float;
/// `_AlphaFunc` stores the compare function in the low 3 bits of ColorControl,
/// and `_AlphaTestEnable` toggles bit 3.
struct AlphaTestState {
  bool enabled;
  float threshold;
  uint32_t compare_function;  ///< 0 = never .. 7 = always
};

inline AlphaTestState ReadAlphaTestState(const uint8_t* base, uint32_t device) {
  const uint32_t color_control = GuestRead32(base, device + guest_device::kColorControl);
  return AlphaTestState{
      (color_control & 0x8) != 0,
      GuestReadF32(base, device + guest_device::kAlphaRef),
      color_control & 0x7,
  };
}

//=============================================================================
// Depth / blend / cull -- GPU register shadows in the control packet
//=============================================================================
//
// On the Xbox 360 the D3D enums (D3DCMPFUNC, D3DBLEND, D3DBLENDOP, D3DCULL) are
// redefined to the raw Xenos GPU encodings, and the D3DDevice_SetRenderState_*
// setters write those bits straight into the GPU register shadows. So these
// fields already hold Xenos-encoded values; no D3DRS -> Xenos translation ran.

/// GPU_DEPTHCONTROL: bit1 = z-test enable, bit2 = z-write enable, bits4-6 = zfunc.
struct DepthState {
  bool test_enabled;
  bool write_enabled;
  uint32_t compare_function;  ///< xenos::CompareFunction (0=never .. 7=always)
};

inline DepthState ReadDepthState(const uint8_t* base, uint32_t device) {
  const uint32_t dc = GuestRead32(base, device + guest_device::kDepthControl);
  return DepthState{(dc & 0x2) != 0, (dc & 0x4) != 0, (dc >> 4) & 0x7};
}

/// GPU_BLENDCONTROL (RT0). Standard Xenos RB_BLENDCONTROL packing:
///   color_src:5 @0, color_op:3 @5, color_dst:5 @8,
///   alpha_src:5 @16, alpha_op:3 @21, alpha_dst:5 @24.
/// Fields are xenos::BlendFactor / xenos::BlendOp. AlphaBlendEnable writes
/// One/Zero/Add (0x00010001) into every RT when blending is off, so a passthrough
/// register is the "disabled" signal.
struct BlendState {
  uint32_t color_src, color_op, color_dst;
  uint32_t alpha_src, alpha_op, alpha_dst;
  bool enabled;  ///< false when both color and alpha are One/Zero/Add
};

inline BlendState ReadBlendState(const uint8_t* base, uint32_t device) {
  const uint32_t bc = GuestRead32(base, device + guest_device::kBlendControl0);
  BlendState b;
  b.color_src = bc & 0x1F;
  b.color_op = (bc >> 5) & 0x7;
  b.color_dst = (bc >> 8) & 0x1F;
  b.alpha_src = (bc >> 16) & 0x1F;
  b.alpha_op = (bc >> 21) & 0x7;
  b.alpha_dst = (bc >> 24) & 0x1F;
  const bool color_passthrough = b.color_src == 1 && b.color_dst == 0 && b.color_op == 0;
  const bool alpha_passthrough = b.alpha_src == 1 && b.alpha_dst == 0 && b.alpha_op == 0;
  b.enabled = !(color_passthrough && alpha_passthrough);
  return b;
}

/// RB_COLOR_MASK, render target 0's nibble. Xenos packs it R,G,B,A from bit 0 -- the same
/// order as D3DCOLORWRITEENABLE_*, so the nibble is already a D3D9 write mask.
///
/// The engine leans on this: the menus composite through *destination alpha*, and the passes
/// that build that alpha mask run with colour writes masked off (mask 0x8). Forcing 0xF made
/// those passes paint their blend over RGB as well -- which is what erased the map behind the
/// minimap and the menu map preview, then washed the region to white.
inline uint32_t ReadColorWriteMask(const uint8_t* base, uint32_t device) {
  return GuestRead32(base, device + guest_device::kColorMask) & 0xF;
}

/// GPU_MODECONTROL (PA_SU_SC_MODE_CNTL): bit0 cull front, bit1 cull back,
/// bit2 front-face winding (0 = CCW is front, 1 = CW is front).
struct CullState {
  bool cull_front;
  bool cull_back;
  bool front_is_cw;
};

inline CullState ReadCullState(const uint8_t* base, uint32_t device) {
  const uint32_t mc = GuestRead32(base, device + guest_device::kModeControl);
  return CullState{(mc & 0x1) != 0, (mc & 0x2) != 0, (mc & 0x4) != 0};
}

//=============================================================================
// Viewport
//=============================================================================

/// The guest viewport as the vertex-shader-visible transform: the six
/// PA_CL_VPORT_* scale/offset floats (already normalized -- an axis whose VTE
/// enable bit is clear reads as scale 1 / offset 0), plus the two clip flags.
/// `D3D::SetViewport` derives these from a D3DVIEWPORT9 as
///   XScale=W/2, XOffset=X+W/2, YScale=-H/2, YOffset=Y+H/2,
///   ZScale=MaxZ-MinZ, ZOffset=MinZ.
struct ViewportState {
  float scale_x, scale_y, scale_z;
  float offset_x, offset_y, offset_z;
  bool clip_disable;      ///< PA_CL_CLIP_CNTL.CLIP_DISABLE -- screen-space draws
  bool dx_clip_space;     ///< 0..W depth clip (Xbox 360 is Direct3D, so normally true)
};

inline ViewportState ReadViewportState(const uint8_t* base, uint32_t device) {
  const uint32_t vte = GuestRead32(base, device + guest_device::kVteControl);
  const uint32_t clip = GuestRead32(base, device + guest_device::kClipControl);
  auto f = [&](uint32_t off) { return GuestReadF32(base, device + off); };
  using namespace guest_device;
  ViewportState v;
  v.scale_x = (vte & (1u << 0)) ? f(kVportXScale) : 1.0f;
  v.offset_x = (vte & (1u << 1)) ? f(kVportXOffset) : 0.0f;
  v.scale_y = (vte & (1u << 2)) ? f(kVportYScale) : 1.0f;
  v.offset_y = (vte & (1u << 3)) ? f(kVportYOffset) : 0.0f;
  v.scale_z = (vte & (1u << 4)) ? f(kVportZScale) : 1.0f;
  v.offset_z = (vte & (1u << 5)) ? f(kVportZOffset) : 0.0f;
  v.clip_disable = (clip & (1u << 16)) != 0;
  v.dx_clip_space = (clip & (1u << 19)) != 0;
  return v;
}

}  // namespace nx1::d3d9
