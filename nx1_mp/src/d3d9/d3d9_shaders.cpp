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

void ShaderCache::UploadConstants(const uint8_t* base, uint32_t guest_constants_addr,
                                  const Sm3Shader& shader, bool pixel_stage) {
#ifndef NX1_HAVE_SM3_SHADER_CACHE
  (void)base; (void)guest_constants_addr; (void)shader; (void)pixel_stage;
#else
  if (!device_ || !shader.entry) {
    return;
  }
  const Nx1Sm3CacheEntry& e = *shader.entry;

  float staging[kMaxHostConstants * 4];
  uint32_t count;

  if (e.flags & NX1_SM3_UNCOMPACTED_CONSTANTS) {
    // Relative addressing: the shader can index any register, so upload all 256.
    count = kMaxHostConstants;
    for (uint32_t i = 0; i < count * 4; ++i) {
      staging[i] = GuestReadF32(base, guest_constants_addr + i * 4);
    }
  } else {
    count = e.remapCount;
    for (uint32_t i = 0; i < count; ++i) {
      const uint32_t src = g_nx1Sm3ConstantRemap[e.remapOffset + i];
      for (uint32_t c = 0; c < 4; ++c) {
        staging[i * 4 + c] = GuestReadF32(base, guest_constants_addr + (src * 4 + c) * 4);
      }
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
