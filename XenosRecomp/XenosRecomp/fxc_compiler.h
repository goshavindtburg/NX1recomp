#pragma once

#include <cstdint>
#include <string>
#include <vector>

/// Compile SM3 HLSL to vs_3_0/ps_3_0 bytecode with D3DCompile.
/// Returns an empty vector on failure and fills `error` with the compiler output.
std::vector<uint8_t> compileSm3(const std::string& hlsl, bool isPixelShader, std::string& error);
