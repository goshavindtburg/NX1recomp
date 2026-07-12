#pragma once

#include <cstddef>
#include <cstdint>

namespace rex::graphics::d3d12 {

struct Nx1NativeDxilBlob {
  const uint8_t* data = nullptr;
  size_t size = 0;
  uint32_t spec_constants_mask = 0;
};

bool GetNx1NativeDxilBlob(uint64_t ucode_hash, Nx1NativeDxilBlob& blob_out);

}  // namespace rex::graphics::d3d12
