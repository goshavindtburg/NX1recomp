#pragma once

#include <cstdint>
#include <string>
#include <vector>

/// Result of lowering XenosRecomp's DXIL-flavoured HLSL to Shader Model 3.0.
struct Sm3Shader
{
    std::string hlsl;

    /// Xenos constant registers referenced by this shader, in slot order:
    /// the shader reads `c[i]`, the renderer must upload guest ALU constant
    /// `constantRemap[i]` (stage-relative) into host register `i`.
    /// Empty when `uncompactedConstants` is set.
    std::vector<uint16_t> constantRemap;

    /// The shader uses relative addressing (`c[a0 + n]`), so the constant file
    /// cannot be compacted: upload all 256 registers 1:1.
    bool uncompactedConstants = false;

    /// vs_3_0 only has 256 float registers and this shader needs all of them, so
    /// `g_HalfPixelOffset` was dropped. The host must fold the half-pixel bias
    /// into the projection constants for this shader.
    bool needsHostHalfPixel = false;

    /// Samplers referenced, by register index.
    std::vector<uint8_t> samplers;
};

/// Lower `source` (as emitted by ShaderRecompiler) to SM3-compatible HLSL.
///
/// `specConstantsMask` bakes the recompiler's spec constants in: SM3 has no
/// integer ops, so `g_SpecConstants() & SPEC_CONSTANT_X` cannot be evaluated at
/// runtime. Alpha test survives as a float-compare against a uniform; the
/// R11G11B10 normal decode moves to the vertex-upload path.
///
/// Returns false and fills `error` when the shader cannot be expressed.
bool transformToSm3(const std::string& source, bool isPixelShader, uint32_t specConstantsMask,
                    Sm3Shader& out, std::string& error);

/// ps_3_0 exposes v0..v9. No NX1 vertex shader writes more than 10 interpolators.
inline constexpr int kSm3MaxInterpolators = 10;
