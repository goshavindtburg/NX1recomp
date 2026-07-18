/**
 * @file    d3d9_shaders.cpp
 * @brief   SM3 shader cache loader for the native D3D9 renderer.
 */

#include "d3d9_shaders.h"

#ifdef _WIN32

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging/macros.h>

#include "d3d9_constants.h"
#include "guest_d3d.h"

#ifdef NX1_HAVE_SM3_SHADER_CACHE
#include <zstd.h>

#include "nx1_sm3_shader_cache.h"
#endif

namespace nx1::d3d9 {

namespace {
using ShaderMap = std::unordered_map<uint64_t, Sm3Shader>;
}

bool ShaderCache::Available() {
#ifdef NX1_HAVE_SM3_SHADER_CACHE
  return true;
#else
  return false;
#endif
}

ShaderCache& ShaderCache::Get() {
  static ShaderCache instance;
  return instance;
}

bool ShaderCache::Initialize(IDirect3DDevice9Ex* device) {
#ifndef NX1_HAVE_SM3_SHADER_CACHE
  (void)device;
  REXGPU_ERROR("nx1_d3d9: built without an SM3 shader cache (tools/nx1_sm3_shader_cache.cpp)");
  return false;
#else
  if (bytecode_) {
    return true;
  }
  auto blob = std::make_unique<uint8_t[]>(g_nx1Sm3CacheDecompressedSize);
  const size_t got = ZSTD_decompress(blob.get(), g_nx1Sm3CacheDecompressedSize,
                                     g_compressedNx1Sm3Cache, g_nx1Sm3CacheCompressedSize);
  if (ZSTD_isError(got) || got != g_nx1Sm3CacheDecompressedSize) {
    REXGPU_ERROR("nx1_d3d9: SM3 cache decompression failed");
    return false;
  }

  device_ = device;
  bytecode_ = blob.get();
  bytecode_storage_ = blob.release();
  shaders_ = new ShaderMap();

  REXGPU_INFO("nx1_d3d9: SM3 shader cache loaded ({} entries, {} KiB)", g_nx1Sm3CacheEntryCount,
              g_nx1Sm3CacheDecompressedSize / 1024);
  return true;
#endif
}

void ShaderCache::Shutdown() {
  if (shaders_) {
    auto* map = static_cast<ShaderMap*>(shaders_);
    for (auto& [hash, shader] : *map) {
      if (shader.vs) shader.vs->Release();
      if (shader.ps) shader.ps->Release();
    }
    delete map;
    shaders_ = nullptr;
  }
  delete[] static_cast<uint8_t*>(bytecode_storage_);
  bytecode_storage_ = nullptr;
  bytecode_ = nullptr;
  device_ = nullptr;
}

#ifdef NX1_HAVE_SM3_SHADER_CACHE
/// True when the shader reads a constant register in [lo, hi] that it does not `def` itself.
///
/// This is the test for "did the translator declare g_InvTexDim[16] at this register?". The PS
/// uniform block sits at *fixed* offsets above the constant window -- threshold, compare
/// function, then g_InvTexDim[16] -- but the translator only emits g_InvTexDim for shaders that
/// actually have an offset tfetch (`needDims` in sm3_transform.cpp). A shader without one never
/// reserves those 16 registers, and fxc parks its `def` literals in the free space.
///
/// That distinction matters because a `def` literal is NOT baked into the shader: it lives in
/// the ordinary constant file and D3D9 applies it when the shader is *set*, so any
/// Set*ShaderConstantF we issue afterwards for the same register silently overwrites it.
/// Uploading all 18 registers unconditionally clobbered `def cN, 0, 1, -1, -0` -- the register
/// every flattened `cmp` reads its 0/1 results from.
///
/// Inferring this from def *placement* does not work: fxc also places defs *below* the uniform
/// base (observed: "uniforms at c1, first def at c0"). Reading the shader's actual constant use
/// is the reliable signal.
bool ReadsUndefinedConstRange(const DWORD* code, uint32_t dword_count, uint32_t lo, uint32_t hi) {
  constexpr uint32_t kEnd = 0xFFFF, kComment = 0xFFFE, kDef = 0x51, kDcl = 0x1F;
  constexpr uint32_t kRegNumMask = 0x7FF;
  // D3DSPR_CONST is 2, not 1 (TEMP=0, INPUT=1, CONST=2). This was 1 for a day: both passes
  // matched nothing, so every shader classified as "reads no undefined constants" and the
  // "0 of 384 read g_InvTexDim" measurement built on it was VOID.
  constexpr uint32_t kConstRegType = 2;  // D3DSPR_CONST
  bool self_defined[512] = {};

  // Pass 1: the registers the shader supplies itself.
  for (uint32_t i = 1; i < dword_count;) {
    const DWORD tok = code[i];
    const uint32_t op = tok & 0xFFFF;
    if (op == kEnd) break;
    if (op == kComment) {
      i += 1 + ((tok >> 16) & 0x7FFF);
      continue;
    }
    const uint32_t len = (tok >> 24) & 0xF;  // SM2+ instruction length, in dwords
    if (len == 0) break;                     // malformed; do not spin
    if (op == kDef && i + 1 < dword_count) {
      const uint32_t reg = code[i + 1] & kRegNumMask;
      if (reg < 512) self_defined[reg] = true;
    }
    i += 1 + len;
  }

  // Pass 2: any constant read in [lo, hi] the shader did not define.
  for (uint32_t i = 1; i < dword_count;) {
    const DWORD tok = code[i];
    const uint32_t op = tok & 0xFFFF;
    if (op == kEnd) break;
    if (op == kComment) {
      i += 1 + ((tok >> 16) & 0x7FFF);
      continue;
    }
    const uint32_t len = (tok >> 24) & 0xF;
    if (len == 0) break;
    if (op != kDef && op != kDcl) {
      for (uint32_t k = 1; k <= len && i + k < dword_count; ++k) {
        const DWORD p = code[i + k];
        if (!(p & 0x80000000u)) continue;  // not a parameter token
        // Register type is split across bits 28..30 and 11..12; number is bits 0..10.
        const uint32_t type = ((p >> 28) & 0x7) | ((p >> 8) & 0x18);
        const uint32_t reg = p & kRegNumMask;
        if (type == kConstRegType && reg >= lo && reg <= hi && reg < 512 && !self_defined[reg]) {
          return true;
        }
      }
    }
    i += 1 + len;
  }
  return false;
}

/// Collect the shader's `def` registers AND values -- see Sm3Shader::def_mask / defs.
/// (D3DSIO_DEF's destination is always a float const register, so no type check is needed;
/// integer/bool defs are different opcodes.)
void CollectDefs(const DWORD* code, uint32_t dword_count, Sm3Shader& out) {
  constexpr uint32_t kEnd = 0xFFFF, kComment = 0xFFFE, kDef = 0x51;
  for (uint32_t i = 1; i < dword_count;) {
    const DWORD tok = code[i];
    const uint32_t op = tok & 0xFFFF;
    if (op == kEnd) break;
    if (op == kComment) {
      i += 1 + ((tok >> 16) & 0x7FFF);
      continue;
    }
    const uint32_t len = (tok >> 24) & 0xF;
    if (len == 0) break;
    if (op == kDef && len == 5 && i + 5 < dword_count) {
      const uint32_t reg = code[i + 1] & 0x7FF;
      if (reg < 256) {
        out.def_mask[reg >> 5] |= 1u << (reg & 31);
        if (out.def_count < 16) {
          Sm3Shader::DefLiteral& d = out.defs[out.def_count++];
          d.reg = uint16_t(reg);
          std::memcpy(d.v, &code[i + 2], 16);
        }
      }
    }
    i += 1 + len;
  }
}
#endif

const Sm3Shader* ShaderCache::Lookup(uint64_t ucode_hash) {
#ifndef NX1_HAVE_SM3_SHADER_CACHE
  (void)ucode_hash;
  return nullptr;
#else
  if (!shaders_ || !device_) {
    return nullptr;
  }
  auto* map = static_cast<ShaderMap*>(shaders_);
  if (auto it = map->find(ucode_hash); it != map->end()) {
    return &it->second;
  }

  // Entries are emitted in ascending hash order.
  const Nx1Sm3CacheEntry* found = nullptr;
  size_t lo = 0, hi = g_nx1Sm3CacheEntryCount;
  while (lo < hi) {
    const size_t mid = (lo + hi) / 2;
    if (g_nx1Sm3CacheEntries[mid].hash < ucode_hash) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  if (lo < g_nx1Sm3CacheEntryCount && g_nx1Sm3CacheEntries[lo].hash == ucode_hash) {
    found = &g_nx1Sm3CacheEntries[lo];
  }

  // A zero-size entry is a deliberate cache miss (the shader could not be
  // lowered to SM3); the caller falls back rather than rendering garbage.
  if (!found || found->bytecodeSize == 0) {
    return nullptr;
  }

  Sm3Shader shader{};
  shader.entry = found;
  const auto* code = reinterpret_cast<const DWORD*>(bytecode_ + found->bytecodeOffset);
  // fxc parks its `def` literals in any declared register whose uploads the optimizer proved
  // dead -- including registers INSIDE the remap window -- so every constant write must go
  // around them AND re-assert the values (the runtime does not reapply defs on SetShader).
  // Collected once per shader, consulted by every upload path.
  CollectDefs(code, found->bytecodeSize / 4, shader);
  // Does this pixel shader actually read g_InvTexDim[16]? It is bound at a fixed offset above
  // the constant window, but only declared for shaders with an offset tfetch -- and uploading it
  // to a shader that never declared it overwrites the `def` literals fxc put there.
  if (found->flags & NX1_SM3_PIXEL_SHADER) {
    const uint32_t base = (found->flags & NX1_SM3_UNCOMPACTED_CONSTANTS)
                              ? kMaxHostConstants
                              : (found->remapCount < 1u ? 1u : found->remapCount);
    shader.needs_inv_tex_dim =
        base + 17 < kMaxHostConstants &&
        ReadsUndefinedConstRange(code, found->bytecodeSize / 4, base + 2, base + 17);
  }

  HRESULT hr;
  if (found->flags & NX1_SM3_PIXEL_SHADER) {
    hr = device_->CreatePixelShader(code, &shader.ps);
  } else {
    hr = device_->CreateVertexShader(code, &shader.vs);
  }
  if (FAILED(hr)) {
    REXGPU_ERROR("nx1_d3d9: Create{}Shader failed for 0x{:016X} ({:#x})",
                 (found->flags & NX1_SM3_PIXEL_SHADER) ? "Pixel" : "Vertex", ucode_hash,
                 static_cast<uint32_t>(hr));
    return nullptr;
  }

  return &map->emplace(ucode_hash, shader).first->second;
#endif
}

uint32_t HostConstantCount(const Sm3Shader& shader) {
#ifndef NX1_HAVE_SM3_SHADER_CACHE
  (void)shader;
  return 0;
#else
  if (!shader.entry) {
    return 0;
  }
  if (shader.entry->flags & NX1_SM3_UNCOMPACTED_CONSTANTS) {
    return kMaxHostConstants;
  }
  // The uniforms sit directly above the shader's constant window, and the translator sizes
  // that window as max(1, remapCount) -- `float4 c[0]` is not legal HLSL (see
  // sm3_transform.cpp's constCount). A shader that reads no guest constants therefore has
  // its uniforms at c1/c2, not c0/c1, so the same clamp has to be applied here.
  //
  // Without it the alpha-test *function* register was never written at all: the threshold
  // went to c0 (where fxc had already put a `def` literal) and the function landed in c1,
  // which the shader reads as its threshold. Whatever the previous shader left in c2 then
  // decided the test -- and a stale 0 means "never", so texkill discarded every pixel. That
  // is what made the in-game menus invisible while the identical draws worked in the
  // frontend, where nothing had polluted c2 yet.
  return std::max<uint32_t>(1u, shader.entry->remapCount);
#endif
}

bool NeedsHostNdcTransform(const Sm3Shader& shader) {
#ifndef NX1_HAVE_SM3_SHADER_CACHE
  (void)shader;
  return false;
#else
  return shader.entry && (shader.entry->flags & NX1_SM3_NEEDS_HOST_HALF_PIXEL) != 0;
#endif
}

void ShaderCache::UploadConstants(const uint8_t* base, uint32_t guest_device,
                                  const Sm3Shader& shader, bool pixel_stage) {
#ifndef NX1_HAVE_SM3_SHADER_CACHE
  (void)base; (void)guest_device; (void)shader; (void)pixel_stage;
#else
  if (!device_ || !shader.entry) {
    return;
  }
  const Nx1Sm3CacheEntry& e = *shader.entry;

  float staging[kMaxHostConstants * 4];
  uint32_t count;

  // A register the engine wrote through D3DDevice_GpuBeginShaderConstantF4 lives in the
  // PM4 ring, not in the shadow constant file -- notably VS c4..c7, the object->world
  // matrix of every model draw. Resolve() hands back whichever address currently owns the
  // register. See d3d9_constants.h.
  const uint32_t guest_constants_addr =
      guest_device + (pixel_stage ? guest_device::kPsConstants : guest_device::kVsConstants);
  const ConstantRing& ring = ConstantRing::For(guest_device);

  // The shader's *literal* constants (c254/c255 and friends) are loaded straight into the GPU
  // constant file by D3D::SetLiteralShaderConstants and never reach the shadow -- so for those
  // registers the shadow reads zero. Resolve them from the shader's own definition table.
  // See ReadShaderLiterals in guest_d3d.h.
  ShaderLiteral literals[16];
  const uint32_t shader_object = pixel_stage ? BoundPixelShader(base, guest_device)
                                             : BoundVertexShader(base, guest_device);
  const uint32_t literal_count =
      ReadShaderLiterals(base, shader_object, pixel_stage, literals, 16);
  // The constant file is shared: a pixel shader's register r is file index 256 + r.
  const uint32_t file_base = pixel_stage ? 256u : 0u;

  // Read register `r` of this stage, preferring (1) a literal the shader loaded itself,
  // (2) a value the engine wrote into the PM4 ring, (3) the device's shadow constant file.
  auto read_register = [&](uint32_t r, float* dst) {
    for (uint32_t i = 0; i < literal_count; ++i) {
      const uint32_t first = literals[i].reg;
      const uint32_t index = r + file_base;
      if (index >= first && index < first + literals[i].float4s) {
        const uint8_t* src = GuestTranslateGpuPhysical(literals[i].address + (index - first) * 16);
        for (uint32_t c = 0; c < 4; ++c) {
          const uint32_t bits = *reinterpret_cast<const rex::be<uint32_t>*>(src + c * 4);
          std::memcpy(&dst[c], &bits, sizeof(float));
        }
        return;
      }
    }
    const uint32_t addr = ring.Resolve(pixel_stage, r, guest_constants_addr);
    for (uint32_t c = 0; c < 4; ++c) {
      dst[c] = GuestReadF32(base, addr + c * 4);
    }
  };

  if (e.flags & NX1_SM3_UNCOMPACTED_CONSTANTS) {
    // Relative addressing: the shader can index any register, so upload all 256.
    count = kMaxHostConstants;
    for (uint32_t r = 0; r < count; ++r) {
      read_register(r, &staging[r * 4]);
    }
  } else {
    count = e.remapCount;
    for (uint32_t i = 0; i < count; ++i) {
      read_register(g_nx1Sm3ConstantRemap[e.remapOffset + i], &staging[i * 4]);
    }
  }

  if (count == 0) {
    // No window to upload, but the defs still need asserting (see below).
    for (uint32_t d = 0; d < shader.def_count; ++d) {
      if (pixel_stage) {
        device_->SetPixelShaderConstantF(shader.defs[d].reg, shader.defs[d].v, 1);
      } else {
        device_->SetVertexShaderConstantF(shader.defs[d].reg, shader.defs[d].v, 1);
      }
    }
    return;
  }

  // Upload in runs that go AROUND the shader's own `def` registers. A def is applied when the
  // shader is set, lives in this same constant file, and fxc places one wherever a declared
  // uniform's uses were optimized away -- including inside [0, count). Writing over one feeds
  // garbage to every cmp fxc flattened: measured live, c19 def=(0,1,-1,-0) held literal-pool
  // garbage on every sun-shadow shader, which is what actually killed the sun shadows.
  uint32_t run = 0;
  while (run < count) {
    while (run < count && shader.IsDefRegister(run)) {
      ++run;
    }
    uint32_t end = run;
    while (end < count && !shader.IsDefRegister(end)) {
      ++end;
    }
    if (end > run) {
      if (pixel_stage) {
        device_->SetPixelShaderConstantF(run, &staging[run * 4], end - run);
      } else {
        device_->SetVertexShaderConstantF(run, &staging[run * 4], end - run);
      }
    }
    run = end;
  }

  // Re-assert the shader's own `def` literals. Skipping them above is necessary but NOT
  // sufficient: the runtime does not reapply defs on SetShader (measured -- see
  // Sm3Shader::defs), so a previous shader's window upload that legitimately covered these
  // registers would otherwise still be sitting in them for this shader's draw.
  for (uint32_t d = 0; d < shader.def_count; ++d) {
    if (pixel_stage) {
      device_->SetPixelShaderConstantF(shader.defs[d].reg, shader.defs[d].v, 1);
    } else {
      device_->SetVertexShaderConstantF(shader.defs[d].reg, shader.defs[d].v, 1);
    }
  }
#endif
}

}  // namespace nx1::d3d9

#endif  // _WIN32
