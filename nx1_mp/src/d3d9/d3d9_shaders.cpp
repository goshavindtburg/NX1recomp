/**
 * @file    d3d9_shaders.cpp
 * @brief   SM3 shader cache loader for the native D3D9 renderer.
 */

#include "d3d9_shaders.h"

#ifdef _WIN32

#include <memory>
#include <unordered_map>
#include <vector>

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
  return (shader.entry->flags & NX1_SM3_UNCOMPACTED_CONSTANTS) ? kMaxHostConstants
                                                               : shader.entry->remapCount;
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

  if (e.flags & NX1_SM3_UNCOMPACTED_CONSTANTS) {
    // Relative addressing: the shader can index any register, so upload all 256.
    count = kMaxHostConstants;
    for (uint32_t r = 0; r < count; ++r) {
      const uint32_t addr = ring.Resolve(pixel_stage, r, guest_constants_addr);
      for (uint32_t c = 0; c < 4; ++c) {
        staging[r * 4 + c] = GuestReadF32(base, addr + c * 4);
      }
    }
  } else {
    count = e.remapCount;
    for (uint32_t i = 0; i < count; ++i) {
      const uint32_t src = g_nx1Sm3ConstantRemap[e.remapOffset + i];
      const uint32_t addr = ring.Resolve(pixel_stage, src, guest_constants_addr);
      for (uint32_t c = 0; c < 4; ++c) {
        staging[i * 4 + c] = GuestReadF32(base, addr + c * 4);
      }
    }
  }

  // DIAG(d3d9): vertices, indices, layouts and the rigid-model ring matrices all check out,
  // so the suspect is a ring record outliving its owner. A rigid model writes c4..c7 through
  // the ring; the record only retires when the guest re-dirties the group. If a later draw
  // (the world, whose vertices are already in world space) reads c4..c7 expecting the
  // *shadow* value and the engine's redundancy cache skipped re-setting them, we hand it the
  // previous model's matrix. Log which registers each VS reads and who owns them.
  // TODO(d3d9): drop.
  if (!pixel_stage && !(e.flags & NX1_SM3_UNCOMPACTED_CONSTANTS)) {
    // Skip the menu entirely -- it drains any sample budget before the map loads, and it
    // never touches the ring. Start once some device has taken a ring record.
    static uint32_t seen = 0;
    static uint32_t dumped = 0;
    if (ConstantRing::EverRecorded() && (seen++ % 400) == 0 && dumped < 24) {
      ++dumped;
      char regs[256];
      int n = 0;
      uint32_t ring_owned = 0;
      for (uint32_t i = 0; i < e.remapCount && n < 200; ++i) {
        const uint32_t src = g_nx1Sm3ConstantRemap[e.remapOffset + i];
        const bool owned = ring.Lookup(false, src) != 0;
        ring_owned += owned ? 1 : 0;
        n += snprintf(regs + n, sizeof(regs) - n, "%s%u%s", n ? " " : "", src, owned ? "*" : "");
      }
      REXGPU_INFO("nx1_d3d9: [regdiag] vs reads [{}] ({} of {} from ring; * = ring)", regs,
                  ring_owned, e.remapCount);
      // Everything upstream of the transform is verified, so print what the draw actually
      // multiplies by: c0..c3 should be a view-projection (a projection has a lone +-1 in
      // the last row and 0 in its .w except for z), c4..c7 the object->world.
      for (uint32_t r = 0; r < 8; ++r) {
        const uint32_t a = ring.Resolve(false, r, guest_constants_addr);
        REXGPU_INFO("nx1_d3d9: [cdump]   c{} [{: 10.3f} {: 10.3f} {: 10.3f} {: 10.3f}] {}", r,
                    GuestReadF32(base, a + 0), GuestReadF32(base, a + 4),
                    GuestReadF32(base, a + 8), GuestReadF32(base, a + 12),
                    ring.Lookup(false, r) ? "(ring)" : "(shadow)");
      }
      REXGPU_INFO("nx1_d3d9: [cdump]   ndc scale/offset applied by the host fold follows");
    }
  }

  if (count == 0) {
    return;
  }
  if (pixel_stage) {
    device_->SetPixelShaderConstantF(0, staging, count);
  } else {
    device_->SetVertexShaderConstantF(0, staging, count);
  }
#endif
}

}  // namespace nx1::d3d9

#endif  // _WIN32
