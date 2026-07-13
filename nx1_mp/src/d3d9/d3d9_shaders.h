/**
 * @file    d3d9_shaders.h
 * @brief   Xenos microcode -> vs_3_0/ps_3_0, via the SM3 cache XenosRecomp emits.
 *
 * Lookup is keyed by the same Xenos microcode hash as the DXIL cache, so the two
 * caches stay in lockstep.
 *
 * The important wrinkle is constants. Xenos gives a shader 256 float4 registers
 * per stage; ps_3_0 only exposes 224, and NX1's shaders reference c224..c255
 * heavily (those are the literals the Xenon runtime writes into the guest ALU
 * constant file). XenosRecomp therefore *compacts* each shader's constants -- no
 * shader touches more than 27 distinct registers -- and hands us a remap table.
 * `UploadConstants` walks it.
 */

#pragma once

#include <cstdint>

#ifdef _WIN32
#include <d3d9.h>
#endif

struct Nx1Sm3CacheEntry;

namespace nx1::d3d9 {

#ifdef _WIN32

/// A translated guest shader, ready to bind.
struct Sm3Shader {
  IDirect3DVertexShader9* vs = nullptr;
  IDirect3DPixelShader9* ps = nullptr;
  const Nx1Sm3CacheEntry* entry = nullptr;
};

class ShaderCache {
 public:
  static ShaderCache& Get();

  /// Decompress the embedded cache. Safe to call repeatedly.
  bool Initialize(IDirect3DDevice9Ex* device);
  void Shutdown();

  /// True when a cache was compiled into this build.
  static bool Available();

  /// Look up (and lazily create) the host shader for a guest microcode hash.
  /// Returns nullptr on a cache miss -- the caller must skip or fall back.
  const Sm3Shader* Lookup(uint64_t ucode_hash);

  /// Copy the guest's ALU constants into the host constant registers this shader actually
  /// reads. Registers come from the stage's shadow (device+0x780 / +0x1780, big-endian; see
  /// guest_d3d.h) unless the device's ConstantRing says the ring owns them.
  void UploadConstants(const uint8_t* base, uint32_t guest_device, const Sm3Shader& shader,
                       bool pixel_stage);

 private:
  ShaderCache() = default;
  ~ShaderCache() { Shutdown(); }
  ShaderCache(const ShaderCache&) = delete;
  ShaderCache& operator=(const ShaderCache&) = delete;

  IDirect3DDevice9Ex* device_ = nullptr;
  const uint8_t* bytecode_ = nullptr;  ///< decompressed blob
  void* bytecode_storage_ = nullptr;
  void* shaders_ = nullptr;  ///< hash -> Sm3Shader (opaque; see .cpp)
};

/// Xenos ALU constant registers are 256 float4 per stage. A shader's compacted
/// register file never exceeds this, so the staging buffer is fixed size.
inline constexpr uint32_t kMaxHostConstants = 256;

/// Number of host float4 registers `shader` reads: its compacted constant window.
/// The shader's uniforms sit immediately above it, at fixed offsets:
///   VS: c[N]   = (ndcScale.xyz,  halfPixel.x)
///       c[N+1] = (ndcOffset.xyz, halfPixel.y)
///   PS: c[N]   = alpha threshold
///       c[N+1] = alpha compare function
///       c[N+2] = inverse texture dimensions [16]
uint32_t HostConstantCount(const Sm3Shader& shader);

/// True when the shader's constant file could not be compacted, so there was no
/// room for the NDC/half-pixel uniforms and the host must fold that transform in.
bool NeedsHostNdcTransform(const Sm3Shader& shader);

#endif  // _WIN32

}  // namespace nx1::d3d9
