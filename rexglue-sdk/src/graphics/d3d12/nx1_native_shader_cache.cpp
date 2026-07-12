#include <rex/graphics/d3d12/nx1_native_shader_cache.h>

#include <algorithm>
#include <mutex>
#include <vector>

#include <rex/logging.h>

#if REX_HAS_NX1_NATIVE_SHADER_CACHE
#include <zstd.h>

#include "shader_cache.h"
#endif

namespace rex::graphics::d3d12 {

#if REX_HAS_NX1_NATIVE_SHADER_CACHE
namespace {

std::once_flag dxil_cache_once;
std::vector<uint8_t> dxil_cache;
bool dxil_cache_ready = false;

void InitializeDxilCache() {
  dxil_cache.resize(g_dxilCacheDecompressedSize);
  const size_t decompressed_size =
      ZSTD_decompress(dxil_cache.data(), dxil_cache.size(), g_compressedDxilCache,
                      g_dxilCacheCompressedSize);
  if (ZSTD_isError(decompressed_size)) {
    REXGPU_ERROR("NX1 native shader cache: DXIL decompression failed: {}",
                 ZSTD_getErrorName(decompressed_size));
    dxil_cache.clear();
    return;
  }
  if (decompressed_size != g_dxilCacheDecompressedSize) {
    REXGPU_ERROR(
        "NX1 native shader cache: DXIL decompressed size mismatch (got {}, expected {})",
        decompressed_size, g_dxilCacheDecompressedSize);
    dxil_cache.clear();
    return;
  }
  dxil_cache_ready = true;
  REXGPU_INFO("NX1 native shader cache: loaded {} DXIL shaders ({} bytes)",
              g_shaderCacheEntryCount, g_dxilCacheDecompressedSize);
}

}  // namespace
#endif

bool GetNx1NativeDxilBlob(uint64_t ucode_hash, Nx1NativeDxilBlob& blob_out) {
  blob_out = {};
#if REX_HAS_NX1_NATIVE_SHADER_CACHE
  std::call_once(dxil_cache_once, InitializeDxilCache);
  if (!dxil_cache_ready) {
    return false;
  }

  const ShaderCacheEntry* begin = g_shaderCacheEntries;
  const ShaderCacheEntry* end = g_shaderCacheEntries + g_shaderCacheEntryCount;
  const ShaderCacheEntry* entry = std::lower_bound(
      begin, end, ucode_hash,
      [](const ShaderCacheEntry& candidate, uint64_t hash) { return candidate.hash < hash; });
  if (entry == end || entry->hash != ucode_hash || entry->dxilSize == 0) {
    return false;
  }
  if (entry->dxilOffset + entry->dxilSize > dxil_cache.size()) {
    REXGPU_ERROR("NX1 native shader cache: invalid DXIL range for shader {:016X}", ucode_hash);
    return false;
  }

  blob_out.data = dxil_cache.data() + entry->dxilOffset;
  blob_out.size = entry->dxilSize;
  blob_out.spec_constants_mask = entry->specConstantsMask;
  return true;
#else
  return false;
#endif
}

}  // namespace rex::graphics::d3d12
