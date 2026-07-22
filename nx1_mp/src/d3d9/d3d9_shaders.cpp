/**
 * @file    d3d9_shaders.cpp
 * @brief   SM3 shader cache loader for the native D3D9 renderer.
 */

#include "d3d9_shaders.h"

#ifdef _WIN32

#include <chrono>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging/macros.h>

#include "d3d9_log.h"

#include "d3d9_constants.h"

#include "guest_d3d.h"

#ifdef NX1_HAVE_SM3_SHADER_CACHE
#include <zstd.h>

#include "nx1_sm3_shader_cache.h"
#endif

REXCVAR_DEFINE_UINT32(nx1_d3d9_dbg_dump_sm3_lo32, 0, "GPU",
                      "Debug: write the translated SM3 bytecode of the shader whose microcode "
                      "hash has these low 32 bits, to sm3_<hash>.bin next to the exe");

REXCVAR_DEFINE_UINT32(nx1_d3d9_dbg_const_obj, 0, "GPU",
                      "Debug: guest shader object whose constant register to report");
REXCVAR_DEFINE_UINT32(nx1_d3d9_dbg_const_reg, 0, "GPU",
                      "Debug: HOST constant register index to report for dbg_const_obj");

REXCVAR_DEFINE_UINT32(nx1_d3d9_orphan_texcoords, 0, "GPU",
                      "Bitmask of pixel-shader TEXCOORD indices that no vertex shader produces. "
                      "Reads of those inputs are rewritten to a constant instead of being left "
                      "unbound and undefined by D3D9's semantic linkage");

REXCVAR_DEFINE_UINT32(nx1_d3d9_dbg_oc0_lo32, 0, "GPU",
                      "Debug: for the shader whose microcode hash has these low 32 bits, force the "
                      "final colour write to output dbg_oc0_reg instead of the shaded result");
REXCVAR_DEFINE_UINT32(nx1_d3d9_dbg_oc0_reg, 0, "GPU",
                      "Debug: temp register index to display via dbg_oc0_lo32");
REXCVAR_DEFINE_UINT32(nx1_d3d9_dbg_solid_lo32, 0, "GPU",
                      "Debug: draw the shader with these low-32 hash bits as SOLID MAGENTA, body "
                      "discarded. Wireframe highlighting is easy to miss on a large flat surface; "
                      "this is not. Keyed on the microcode hash, so it survives a restart -- "
                      "unlike ps_object, which is a guest address reassigned on every fastfile load");

REXCVAR_DEFINE_UINT32(nx1_d3d9_dbg_force_branch, 0, "GPU",
                      "Debug: for the dbg_oc0_lo32 shader, force the final colour select to one "
                      "side -- 1 = always the first operand (the lit path), 2 = always the second "
                      "(the fallback). Only the SELECTOR changes, so every colour and the alpha "
                      "channel are still computed normally: this answers 'is the branch choice "
                      "the bug' without disturbing the values the branch chooses between");

REXCVAR_DEFINE_UINT32(nx1_d3d9_dbg_oc0_type, 0, "GPU",
                      "Debug: what dbg_oc0_reg names -- 0 = temp register rN, 1 = INPUT register "
                      "vN (an interpolator). Lets a varying be displayed directly instead of "
                      "inferred from the arithmetic that consumes it");
REXCVAR_DEFINE_UINT32(nx1_d3d9_dbg_oc0_neg, 0, "GPU",
                      "Debug: negate the displayed value. A greyscale display clamps everything "
                      "<= 0 to black, so zero and negative look identical -- and `cmp` treats "
                      "0 >= 0 as TRUE while a negative fails, which is exactly the distinction "
                      "that matters for a sign gate. Black normally + WHITE negated means the "
                      "value is genuinely negative; black in both means it is zero");

REXCVAR_DEFINE_UINT32(nx1_d3d9_dbg_oc0_swz, 0, "GPU",
                      "Debug: component to broadcast when displaying -- 0 = xyzw as colour, "
                      "1/2/3/4 = x/y/z/w as greyscale. Needed to read a scalar such as a lerp "
                      "factor, which is invisible in an xyzw display");

REXCVAR_DEFINE_UINT32(nx1_d3d9_dbg_oc0_at, 0, "GPU",
                      "Debug: truncate the shader at the first instruction at or after this dword "
                      "and output dbg_oc0_reg there. Registers are reused, so displaying one at "
                      "the END of the shader shows a later value than the instruction under "
                      "investigation consumed -- this reads it at the point that matters");

REXCVAR_DEFINE_BOOL(nx1_d3d9_orphan_paramgen, false, "GPU",
                    "Treat an orphan pixel-shader input as the PARAM GEN register rather than an "
                    "interpolator: substitute (0,0,0,0) as the reference does, not the "
                    "interpolator default (0,0,0,1). The reference's signature shows this shader "
                    "family uses SV_Position where ours invents a phantom TEXCOORD");

REXCVAR_DEFINE_UINT32(nx1_d3d9_dbg_literals, 0, "GPU",
                      "Debug: report what c252-c255 resolve to for the first N distinct shaders, "
                      "and whether each value came from the shader literal table or fell back to "
                      "the shadow. Set it IN GAME -- at launch the budget is spent on menu shaders");

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

  NX1_LOGI_MISC("nx1_d3d9: SM3 shader cache loaded ({} entries, {} KiB)", g_nx1Sm3CacheEntryCount,
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

/// Collect, in one walk, the shader's `def` registers + values (see Sm3Shader::def_mask / defs)
/// and the sampler slots it declares (Sm3Shader::sampler_mask).
/// D3DSIO_DEF's destination is always a float const register, so no type check is needed there;
/// integer/bool defs are different opcodes. A `dcl` (0x1F) with length 2 is
/// [opcode][usage token][dst param]; sampler declarations carry dst register type
/// D3DSPR_SAMPLER (10, split-encoded across bits 28..30 and 11..12) and the slot in bits 0..10.
/// Walk validated against fxc reference bytecode (sampler dcl -> type=10 reg=N; inputs type=1).
void CollectDefs(const DWORD* code, uint32_t dword_count, Sm3Shader& out) {
  constexpr uint32_t kEnd = 0xFFFF, kComment = 0xFFFE, kDef = 0x51, kDcl = 0x1F;
  constexpr uint32_t kSamplerRegType = 10;
  bool walked_to_end = false;
  for (uint32_t i = 1; i < dword_count;) {
    const DWORD tok = code[i];
    const uint32_t op = tok & 0xFFFF;
    if (op == kEnd) {
      walked_to_end = true;
      break;
    }
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
    } else if (op == kDcl && len == 2 && i + 2 < dword_count) {
      const DWORD dst = code[i + 2];
      const uint32_t type = ((dst >> 28) & 7) | ((dst >> 8) & 0x18);
      const uint32_t reg = dst & 0x7FF;
      if (type == kSamplerRegType && reg < 16) {
        out.sampler_mask |= uint16_t(1u << reg);
      }
    }
    i += 1 + len;
  }
  // Only trust the mask if the walk reached D3DSIO_END; anything else means we may have
  // stopped early and missed declarations, so fall back to binding every slot.
  out.all_samplers = !walked_to_end;
}
#endif

/// Xenos routes interpolators POSITIONALLY: pixel-shader slot i reads whatever the vertex shader
/// exported to o_i, and a slot the vertex shader never wrote reads the hardware default (0,0,0,1).
/// D3D9 links by SEMANTIC instead, so a pixel-shader input whose TEXCOORDn has no producer is left
/// unbound -- undefined contents, not the default.
///
/// Those orphans are systematic rather than incidental. The recompiler zero-initialises all 18 of
/// Xenos's interpolators in every vertex shader precisely so unwritten ones read as the default,
/// and the SM3 lowering then strips the ones that are only ever zero (SM3 offers 12 vertex outputs,
/// not 18). So a semantic no vertex shader actually computes is orphaned for EVERY pairing, which
/// is what makes this fixable here, on the pixel shader alone, without knowing the pair.
///
/// Rewrites reads of an orphan input to a `def` register that already holds both 0.0 and 1.0,
/// remapping the swizzle per component so the shader still sees (0,0,0,1). Returns false (leaving
/// `patched` untouched) when there is nothing to do or no usable def, so the caller uses the
/// original bytecode.
bool RewriteOrphanInputs(const DWORD* code, uint32_t dword_count, uint32_t texcoord_mask,
                         std::vector<DWORD>& patched, uint32_t& reads_out) {
  constexpr uint32_t kEnd = 0xFFFF, kComment = 0xFFFE, kDef = 0x51, kDcl = 0x1F;
  constexpr uint32_t kInputRegType = 1, kConstRegType = 2;
  constexpr uint32_t kUsageTexCoord = 5;

  // Pass 1: the orphan input registers, and a def register carrying both 0.0 and 1.0.
  uint32_t orphan_regs = 0;  // bit n = input register vn is an orphan
  uint32_t def_reg = 0, comp_zero = 0, comp_one = 0;
  bool have_def = false;
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
    if (op == kDcl && len == 2 && i + 2 < dword_count) {
      const DWORD usage_tok = code[i + 1], dst = code[i + 2];
      const uint32_t type = ((dst >> 28) & 7) | ((dst >> 8) & 0x18);
      const uint32_t reg = dst & 0x7FF;
      if (type == kInputRegType && reg < 32 && (usage_tok & 0xF) == kUsageTexCoord) {
        const uint32_t index = (usage_tok >> 16) & 0xF;
        if (texcoord_mask & (1u << index)) orphan_regs |= 1u << reg;
      }
    } else if (!have_def && op == kDef && len == 5 && i + 5 < dword_count) {
      float v[4];
      std::memcpy(v, &code[i + 2], sizeof(v));
      bool zero_found = false, one_found = false;
      for (uint32_t c = 0; c < 4; ++c) {
        // -0.0 reads as zero and is just as good a source for it.
        if (!zero_found && v[c] == 0.0f) { comp_zero = c; zero_found = true; }
        if (!one_found && v[c] == 1.0f) { comp_one = c; one_found = true; }
      }
      if (zero_found && one_found) {
        def_reg = code[i + 1] & 0x7FF;
        have_def = true;
      }
    }
    i += 1 + len;
  }
  if (!orphan_regs || !have_def) return false;

  // Pass 2: repoint every source read of an orphan at the def, mapping .xyz to the def's zero
  // component and .w to its one -- the (0,0,0,1) the hardware would have supplied.
  patched.assign(code, code + dword_count);
  reads_out = 0;
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
    // `def` holds raw floats and `dcl` holds a usage token, so neither has source parameters to
    // walk; every other instruction's sources start one dword past its destination.
    if (op != kDef && op != kDcl) {
      for (uint32_t k = 2; k <= len && i + k < dword_count; ++k) {
        const DWORD src = code[i + k];
        const uint32_t type = ((src >> 28) & 7) | ((src >> 8) & 0x18);
        const uint32_t reg = src & 0x7FF;
        if (type != kInputRegType || reg >= 32 || !(orphan_regs & (1u << reg))) continue;
        DWORD out = src;
        out = (out & ~0x7FFu) | (def_reg & 0x7FF);
        out = (out & ~(7u << 28)) | ((kConstRegType & 7u) << 28);
        out = (out & ~(0x18u << 8)) | (((kConstRegType >> 3) & 3u) << 11);
        // An interpolator the vertex shader never wrote reads (0,0,0,1) on Xenos, so .w maps to
        // the def's 1.0. But the PARAM GEN register -- screen position, which the reference
        // supplies from SV_Position -- is (pos.xy, 0, 0), so there .w is 0 like everything else.
        // Which one an orphan actually is decides the value, and only one of them can be right.
        const bool paramgen = REXCVAR_GET(nx1_d3d9_orphan_paramgen);
        uint32_t swizzle = 0;
        for (uint32_t c = 0; c < 4; ++c) {
          const uint32_t sel = (src >> (16 + 2 * c)) & 3;
          const uint32_t pick = (sel == 3 && !paramgen) ? comp_one : comp_zero;
          swizzle |= (pick & 3u) << (2 * c);
        }
        out = (out & ~(0xFFu << 16)) | (swizzle << 16);
        patched[i + k] = out;
        ++reads_out;
      }
    }
    i += 1 + len;
  }
  return reads_out != 0;
}

/// Repoint every source of the instruction that writes oC0 at one temp register, so the shader
/// displays that register instead of its shaded result. Six hypotheses about this glass have now
/// been wrong; rather than guess a seventh, make the shader show which intermediate is already
/// saturated and bisect the ALU chain by eye.
///
/// The final write is a `cmp` (three sources) and every source is one dword, so rewriting them
/// in place keeps the bytecode length identical -- no insertion, no relocation. Register type is
/// forced to TEMP and the swizzle to .xyzw so the display is the raw register.
bool RewriteColorOutput(const DWORD* code, uint32_t dword_count, uint32_t reg,
                        std::vector<DWORD>& patched) {
  constexpr uint32_t kEnd = 0xFFFF, kComment = 0xFFFE, kDef = 0x51, kDcl = 0x1F;
  constexpr uint32_t kColorOutRegType = 8, kTempRegType = 0;
  bool hit = false;
  patched.assign(code, code + dword_count);
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
    if (op != kDef && op != kDcl && len >= 2 && i + 1 < dword_count) {
      const DWORD dst = code[i + 1];
      const uint32_t dtype = ((dst >> 28) & 7) | ((dst >> 8) & 0x18);
      if (dtype == kColorOutRegType && (dst & 0x7FF) == 0) {
        for (uint32_t k = 2; k <= len && i + k < dword_count; ++k) {
          DWORD s = code[i + k];
          s = (s & ~0x7FFu) | (reg & 0x7FF);
          s = (s & ~(7u << 28)) | ((kTempRegType & 7u) << 28);
          s = (s & ~(0x18u << 8)) | (((kTempRegType >> 3) & 3u) << 11);
          s = (s & ~(0xFFu << 16)) | (0xE4u << 16);  // .xyzw
          s &= ~(0xFu << 24);                        // drop any source modifier
          patched[i + k] = s;
        }
        hit = true;
      }
    }
    i += 1 + len;
  }
  return hit;
}

/// Truncate the shader at the first instruction at or after `at_dword`, replacing it with
/// `mov oC0, rN` followed by END. Gives the value of a register AT A POINT rather than at the end
/// of the shader -- essential once registers are reused, because the final contents of a register
/// are usually not what the instruction under investigation read from it.
///
/// ps_3_0 requires all four components of COLOR0 to be written, so the emitted mov is unmasked.
/// Replace a shader's entire body with a solid MAGENTA output, so the surfaces it draws are
/// unmistakable on screen.
///
/// Wireframe highlighting is too easy to miss: a large flat wall is only an outline and one
/// diagonal, and picking the wrong row then reads as "the picker returned the wrong material".
/// This leaves nothing to interpret.
///
/// No `def` is inserted -- fxc already emits one containing both 1.0 and 0.0 (typically
/// `def cN, 1, -0, -1, 0`), so magenta is just a swizzle of it: (1, 0, 1, 1).
bool RewriteSolidColor(const DWORD* code, uint32_t dword_count, std::vector<DWORD>& patched) {
  constexpr uint32_t kEnd = 0xFFFF, kComment = 0xFFFE, kDef = 0x51, kDcl = 0x1F;
  constexpr uint32_t kColorOutRegType = 8, kConstRegType = 2;

  uint32_t def_reg = 0, c_one = 0, c_zero = 0;
  bool have_def = false;
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
    if (!have_def && op == kDef && len == 5 && i + 5 < dword_count) {
      float v[4];
      std::memcpy(v, &code[i + 2], sizeof(v));
      bool z = false, o = false;
      for (uint32_t c = 0; c < 4; ++c) {
        if (!z && v[c] == 0.0f) { c_zero = c; z = true; }
        if (!o && v[c] == 1.0f) { c_one = c; o = true; }
      }
      if (z && o) {
        def_reg = code[i + 1] & 0x7FF;
        have_def = true;
      }
    }
    // First instruction that is neither a declaration nor a literal: the body starts here, and
    // everything the shader would have done can be thrown away.
    if (op != kDef && op != kDcl && have_def) {
      if (i + 3 >= dword_count) return false;
      patched.assign(code, code + dword_count);
      patched[i] = (2u << 24) | 1u;  // mov, length 2
      patched[i + 1] = 0x80000000u | (0xFu << 16) | ((kColorOutRegType >> 3) << 11) | 0u;
      const uint32_t swz = (c_one & 3u) | ((c_zero & 3u) << 2) | ((c_one & 3u) << 4) |
                           ((c_one & 3u) << 6);  // (1, 0, 1, 1) = magenta
      patched[i + 2] = 0x80000000u | (swz << 16) | ((kConstRegType & 7u) << 28) |
                       (((kConstRegType >> 3) & 3u) << 11) | (def_reg & 0x7FF);
      patched[i + 3] = kEnd;
      return true;
    }
    i += 1 + len;
  }
  return false;
}

bool RewriteEarlyOut(const DWORD* code, uint32_t dword_count, uint32_t reg, uint32_t at_dword,
                     uint32_t reg_type, uint32_t swz, uint32_t negate,
                     std::vector<DWORD>& patched, uint32_t& at_out) {
  constexpr uint32_t kEnd = 0xFFFF, kComment = 0xFFFE;
  constexpr uint32_t kColorOutRegType = 8;
  // swz 0 keeps .xyzw; 1..4 broadcast x/y/z/w so a scalar is readable as greyscale.
  const uint32_t sel = swz ? (swz - 1u) & 3u : 0u;
  const uint32_t swizzle = swz ? (sel | (sel << 2) | (sel << 4) | (sel << 6)) : 0xE4u;
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
    if (i >= at_dword) {
      if (i + 3 >= dword_count) return false;  // no room for mov + END
      patched.assign(code, code + dword_count);
      patched[i] = (2u << 24) | 1u;  // mov, length 2
      // dst: oC0.xyzw -- type 8 splits as low3=0 (bits 28..30), high2=1 (bits 11..12)
      patched[i + 1] = 0x80000000u | (0xFu << 16) | ((kColorOutRegType >> 3) << 11) | 0u;
      // src: rN / vN, with the requested swizzle. Register type splits across bits 28..30 and
      // 11..12 exactly as everywhere else in this file.
      patched[i + 2] = 0x80000000u | (swizzle << 16) | ((reg_type & 7u) << 28) |
                       (((reg_type >> 3) & 3u) << 11) | (reg & 0x7FF) |
                       (negate ? (1u << 24) : 0u);  // source modifier 1 = negate
      patched[i + 3] = kEnd;
      at_out = i;
      return true;
    }
    i += 1 + len;
  }
  return false;
}

// Defined below, next to the other bytecode rewrites; declared here because
// PollDebugRebuild precedes them in this file.
bool RewriteForceBranch(const DWORD* code, uint32_t dword_count, uint32_t side,
                        std::vector<DWORD>& patched);
bool RewriteSolidColor(const DWORD* code, uint32_t dword_count, std::vector<DWORD>& patched);

void ShaderCache::PollDebugRebuild() {
#ifdef NX1_HAVE_SM3_SHADER_CACHE
  if (!shaders_ || !device_) {
    return;
  }
  auto* map = static_cast<ShaderMap*>(shaders_);
  // The oC0 probe rewrites bytecode at CREATE time, but a shader is created once and cached for
  // the session -- so changing which register to display would otherwise do nothing until a
  // restart, and the screen would keep showing the first register picked. Drop the probed
  // shader whenever the selection changes so the next lookup rebuilds it, making the sweep
  // usable from the overlay.
  //
  // The shader object is REPLACED IN PLACE rather than erased: Renderer::shader_memo_ caches the
  // Sm3Shader* it got back, so removing the map node would leave that memo dangling.
  {
    static std::mutex om;
    static uint32_t last_lo = 0, last_reg = 0xFFFFFFFFu, last_at = 0xFFFFFFFFu,
                   last_mix = 0xFFFFFFFFu, last_dump = 0xFFFFFFFFu,
                   last_solid = 0xFFFFFFFFu;
    const uint32_t oc0_lo = REXCVAR_GET(nx1_d3d9_dbg_oc0_lo32);
    const uint32_t oc0_reg = REXCVAR_GET(nx1_d3d9_dbg_oc0_reg);
    // EVERY input the rewrite depends on has to be watched here, not just the register: leaving
    // dbg_oc0_at out meant changing it rebuilt nothing and the screen kept showing the previous
    // mode, while the log line said so and was not read.
    const uint32_t oc0_at_watch = REXCVAR_GET(nx1_d3d9_dbg_oc0_at);
    const uint32_t solid_lo = REXCVAR_GET(nx1_d3d9_dbg_solid_lo32);
    const uint32_t oc0_mix = (REXCVAR_GET(nx1_d3d9_dbg_oc0_neg) << 24) |
                             (REXCVAR_GET(nx1_d3d9_dbg_force_branch) << 16) |
                             (REXCVAR_GET(nx1_d3d9_dbg_oc0_type) << 8) |
                             REXCVAR_GET(nx1_d3d9_dbg_oc0_swz);
    // The dump cvar has to participate in change detection too, or toggling it does nothing.
    const uint32_t dump_watch = REXCVAR_GET(nx1_d3d9_dbg_dump_sm3_lo32);
    std::lock_guard<std::mutex> olk(om);
    if (oc0_lo != last_lo || oc0_reg != last_reg || oc0_at_watch != last_at ||
        oc0_mix != last_mix || dump_watch != last_dump || solid_lo != last_solid) {
      last_solid = solid_lo;
      last_dump = dump_watch;
      last_mix = oc0_mix;
      // Announce the selection BEFORE acting on it, and count what matched. Silence here is
      // ambiguous in the one way that has cost the most time today: "the cvar never changed",
      // "the shader is not in the cache yet" and "the rewrite failed" all previously looked
      // identical from the log, and each was misread as a result about the shader.
      REXGPU_INFO("nx1_d3d9: OC0PROBE selection lo32={:#010x} {}{} at={} swz={} (cache holds {})",
                  oc0_lo, REXCVAR_GET(nx1_d3d9_dbg_oc0_type) ? "v" : "r", oc0_reg,
                  oc0_at_watch, REXCVAR_GET(nx1_d3d9_dbg_oc0_swz), map->size());
      last_at = oc0_at_watch;
      uint32_t matched = 0, rewritten = 0;
      const uint32_t dump_lo = REXCVAR_GET(nx1_d3d9_dbg_dump_sm3_lo32);
      static uint32_t last_solid_seen = 0;
      if (solid_lo) last_solid_seen = solid_lo;
      for (auto& [hash, sh] : *map) {
        const uint32_t lo32 = uint32_t(hash & 0xFFFFFFFFu);
        // The DUMP target is its own selection: gating it behind the oC0 probe's filter meant a
        // shader could only be dumped once that probe had already been pointed at it, which is
        // backwards -- the dump is how you find the offsets the probe needs.
        if ((lo32 != last_lo && lo32 != oc0_lo && lo32 != dump_lo && lo32 != solid_lo &&
             lo32 != last_solid_seen) || !sh.entry ||
            !(sh.entry->flags & NX1_SM3_PIXEL_SHADER)) {
          continue;
        }
        const auto* code = reinterpret_cast<const DWORD*>(bytecode_ + sh.entry->bytecodeOffset);
        // Dump from HERE, not from Lookup: a shader is created once and then served from the
        // cache (and the renderer's memo), so a dump that only runs on a cache miss can never be
        // triggered again without a restart -- and a stale dump is worse than none, because every
        // instruction offset read off it silently addresses the wrong instruction.
        if (REXCVAR_GET(nx1_d3d9_dbg_dump_sm3_lo32) == lo32) {
          char path[128];
          std::snprintf(path, sizeof(path), "sm3_%016llX.bin",
                        static_cast<unsigned long long>(hash));
          if (FILE* f = std::fopen(path, "wb")) {
            std::fwrite(code, 1, sh.entry->bytecodeSize, f);
            std::fclose(f);
            REXGPU_INFO("nx1_d3d9: SM3DUMP wrote {} ({} bytes, {} dwords) from the LIVE cache",
                        path, sh.entry->bytecodeSize, sh.entry->bytecodeSize / 4);
          }
        }
        const DWORD* use = code;
        std::vector<DWORD> shown;
        uint32_t at = 0;
        const uint32_t oc0_at = REXCVAR_GET(nx1_d3d9_dbg_oc0_at);
        const uint32_t oc0_ty = REXCVAR_GET(nx1_d3d9_dbg_oc0_type);
        const uint32_t oc0_sw = REXCVAR_GET(nx1_d3d9_dbg_oc0_swz);
        const uint32_t force_side = REXCVAR_GET(nx1_d3d9_dbg_force_branch);
        ++matched;
        if (oc0_lo && lo32 == oc0_lo) {
          const bool ok =
              (solid_lo && lo32 == solid_lo)
                  ? RewriteSolidColor(code, sh.entry->bytecodeSize / 4, shown)
              : force_side ? RewriteForceBranch(code, sh.entry->bytecodeSize / 4, force_side, shown)
              : oc0_at   ? RewriteEarlyOut(code, sh.entry->bytecodeSize / 4, oc0_reg, oc0_at,
                                           oc0_ty, oc0_sw,
                                           REXCVAR_GET(nx1_d3d9_dbg_oc0_neg), shown, at)
                         : RewriteColorOutput(code, sh.entry->bytecodeSize / 4, oc0_reg, shown);
          if (ok) {
            use = shown.data();
            ++rewritten;
          } else {
            REXGPU_WARN("nx1_d3d9: OC0PROBE rewrite FAILED for 0x{:016X} (at={} size={} dwords) "
                        "-- shader left as-is",
                        hash, oc0_at, sh.entry->bytecodeSize / 4);
          }
        }
        IDirect3DPixelShader9* replacement = nullptr;
        if (SUCCEEDED(device_->CreatePixelShader(use, &replacement))) {
          if (sh.ps) sh.ps->Release();
          sh.ps = replacement;
          if (use == code) {
            REXGPU_INFO("nx1_d3d9: OC0PROBE ps 0x{:016X} restored to its shaded result", hash);
          } else if (REXCVAR_GET(nx1_d3d9_dbg_force_branch)) {
            REXGPU_INFO("nx1_d3d9: OC0PROBE ps 0x{:016X} colour select FORCED to operand {} "
                        "(values untouched)",
                        hash, REXCVAR_GET(nx1_d3d9_dbg_force_branch));
          } else if (oc0_at) {
            REXGPU_INFO("nx1_d3d9: OC0PROBE ps 0x{:016X} truncated at dw{} -- displaying r{} "
                        "AS OF THAT INSTRUCTION",
                        hash, at, oc0_reg);
          } else {
            REXGPU_INFO("nx1_d3d9: OC0PROBE ps 0x{:016X} displaying r{} at end of shader", hash,
                        oc0_reg);
          }
        }
      }
      if (oc0_lo && !matched) {
        REXGPU_WARN("nx1_d3d9: OC0PROBE lo32={:#010x} matched NO shader in the cache -- the "
                    "selection is wrong or that shader has not been used yet, so nothing on "
                    "screen reflects this setting",
                    oc0_lo);
      } else if (oc0_lo && !rewritten) {
        REXGPU_WARN("nx1_d3d9: OC0PROBE matched {} shader(s) but rewrote none", matched);
      }
      last_lo = oc0_lo;
      last_reg = oc0_reg;
    }
  }
#endif
}

/// Force the shader's final colour select to one side by rewriting ONLY the selector operand of
/// the `cmp` that writes oC0's colour channels. `cmp dst, a, b, c` is `(a >= 0) ? b : c`, so
/// pointing `a` at a def component holding 0.0 takes the first operand and one holding -1.0 takes
/// the second.
///
/// Everything else -- both candidate colours, the alpha channel, the blend -- is left exactly as
/// the shader computed it. That is the point: substituting a *value* (a white texture, a zeroed
/// sampler) changes what the branches produce and cannot separate "the branch chose wrong" from
/// "the chosen branch is wrong". Changing only the choice can.
bool RewriteForceBranch(const DWORD* code, uint32_t dword_count, uint32_t side,
                        std::vector<DWORD>& patched) {
  constexpr uint32_t kEnd = 0xFFFF, kComment = 0xFFFE, kDef = 0x51, kDcl = 0x1F;
  constexpr uint32_t kColorOutRegType = 8, kConstRegType = 2, kCmp = 0x58;

  uint32_t def_reg = 0, comp_zero = 0, comp_neg = 0;
  bool have_def = false;
  for (uint32_t i = 1; i < dword_count && !have_def;) {
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
      float v[4];
      std::memcpy(v, &code[i + 2], sizeof(v));
      bool z = false, n = false;
      for (uint32_t c = 0; c < 4; ++c) {
        // A negative zero still compares >= 0, so it cannot serve as the "take the second
        // operand" source; require a strictly negative value.
        if (!z && v[c] == 0.0f && !std::signbit(v[c])) { comp_zero = c; z = true; }
        if (!n && v[c] < 0.0f) { comp_neg = c; n = true; }
      }
      if (z && n) {
        def_reg = code[i + 1] & 0x7FF;
        have_def = true;
      }
    }
    i += 1 + len;
  }
  if (!have_def) return false;

  const uint32_t pick = (side == 2) ? comp_neg : comp_zero;
  patched.assign(code, code + dword_count);
  bool hit = false;
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
    if (op == kCmp && len == 4 && i + 1 < dword_count) {
      const DWORD dst = code[i + 1];
      const uint32_t dtype = ((dst >> 28) & 7) | ((dst >> 8) & 0x18);
      // Colour channels only: a .w-only write is the alpha path and must keep its own selector.
      if (dtype == kColorOutRegType && (dst & 0x7FF) == 0 && ((dst >> 16) & 0x7u) != 0) {
        DWORD s = code[i + 2];
        s = (s & ~0x7FFu) | (def_reg & 0x7FF);
        s = (s & ~(7u << 28)) | ((kConstRegType & 7u) << 28);
        s = (s & ~(0x18u << 8)) | (((kConstRegType >> 3) & 3u) << 11);
        s = (s & ~(0xFFu << 16)) | ((pick | (pick << 2) | (pick << 4) | (pick << 6)) << 16);
        s &= ~(0xFu << 24);  // no source modifier: the negation is what we are replacing
        patched[i + 2] = s;
        hit = true;
      }
    }
    i += 1 + len;
  }
  return hit;
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

  // Dump the TRANSLATED bytecode for one shader, selected by the low 32 bits of its microcode
  // hash (cvars are 32-bit; the full hash does not fit). Everything upstream of the translation
  // -- texture data, decode, literal constants, guest microcode -- has been verified correct for
  // the glass material, so what the translator actually emitted is the remaining unknown, and it
  // cannot be read off a log line.
  if (const uint32_t want_lo = REXCVAR_GET(nx1_d3d9_dbg_dump_sm3_lo32);
      want_lo && uint32_t(ucode_hash & 0xFFFFFFFFu) == want_lo) {
    char path[128];
    std::snprintf(path, sizeof(path), "sm3_%016llX.bin", static_cast<unsigned long long>(ucode_hash));
    if (FILE* f = std::fopen(path, "wb")) {
      std::fwrite(code, 1, found->bytecodeSize, f);
      std::fclose(f);
      REXGPU_INFO("nx1_d3d9: SM3DUMP wrote {} ({} bytes, {} dwords, flags={:#x}, remapCount={})",
                  path, found->bytecodeSize, found->bytecodeSize / 4, found->flags,
                  found->remapCount);
    } else {
      REXGPU_ERROR("nx1_d3d9: SM3DUMP could not open {}", path);
    }
  }
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

  // Give pixel-shader inputs that no vertex shader produces the value the hardware would have,
  // rather than whatever D3D9 leaves in an unbound register. CreateShader copies the bytecode,
  // so the patched buffer only has to outlive the call.
  std::vector<DWORD> patched;
  const DWORD* create_code = code;
  if (const uint32_t orphan_mask = REXCVAR_GET(nx1_d3d9_orphan_texcoords);
      orphan_mask && (found->flags & NX1_SM3_PIXEL_SHADER)) {
    uint32_t reads = 0;
    if (RewriteOrphanInputs(code, found->bytecodeSize / 4, orphan_mask, patched, reads)) {
      create_code = patched.data();
      REXGPU_INFO("nx1_d3d9: ORPHAN ps 0x{:016X} rewrote {} read(s) of unproduced TEXCOORDs "
                  "(mask {:#x}) to (0,0,0,1)",
                  ucode_hash, reads, orphan_mask);
    }
  }

  if (const uint32_t oc0_lo = REXCVAR_GET(nx1_d3d9_dbg_oc0_lo32);
      oc0_lo && uint32_t(ucode_hash & 0xFFFFFFFFu) == oc0_lo &&
      (found->flags & NX1_SM3_PIXEL_SHADER)) {
    const uint32_t reg = REXCVAR_GET(nx1_d3d9_dbg_oc0_reg);
    const uint32_t at_req = REXCVAR_GET(nx1_d3d9_dbg_oc0_at);
    std::vector<DWORD> shown;
    uint32_t at = 0;
    const bool ok = at_req ? RewriteEarlyOut(create_code, found->bytecodeSize / 4, reg, at_req,
                                             REXCVAR_GET(nx1_d3d9_dbg_oc0_type),
                                             REXCVAR_GET(nx1_d3d9_dbg_oc0_swz),
                                             REXCVAR_GET(nx1_d3d9_dbg_oc0_neg), shown, at)
                           : RewriteColorOutput(create_code, found->bytecodeSize / 4, reg, shown);
    if (ok) {
      patched = std::move(shown);
      create_code = patched.data();
      REXGPU_INFO("nx1_d3d9: OC0PROBE ps 0x{:016X} now displays r{} instead of its shaded result",
                  ucode_hash, reg);
    } else {
      REXGPU_WARN("nx1_d3d9: OC0PROBE ps 0x{:016X} -- no oC0 write found, shader left alone",
                  ucode_hash);
    }
  }

  HRESULT hr;
  if (found->flags & NX1_SM3_PIXEL_SHADER) {
    hr = device_->CreatePixelShader(create_code, &shader.ps);
  } else {
    hr = device_->CreateVertexShader(create_code, &shader.vs);
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

uint32_t ShaderCache::ResolveConstants(const uint8_t* base, uint32_t guest_device,
                                       const Sm3Shader& shader, bool pixel_stage, float* staging) {
#ifndef NX1_HAVE_SM3_SHADER_CACHE
  (void)base; (void)guest_device; (void)shader; (void)pixel_stage; (void)staging;
  return 0;
#else
  if (!device_ || !shader.entry) {
    return 0;
  }
  const Nx1Sm3CacheEntry& e = *shader.entry;

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
  const auto pmark = [this] {
    return prof_enabled_ ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
  };
  const auto padd = [this](uint64_t& sink, std::chrono::steady_clock::time_point t0) {
    if (prof_enabled_) sink += uint64_t((std::chrono::steady_clock::now() - t0).count());
  };
  if (prof_enabled_) ++prof_.calls;

  const auto t_lit = pmark();
  ShaderLiteral literals[16];
  const uint32_t shader_object = pixel_stage ? BoundPixelShader(base, guest_device)
                                             : BoundVertexShader(base, guest_device);
  const uint32_t literal_count =
      ReadShaderLiterals(base, shader_object, pixel_stage, literals, 16);
  padd(prof_.literals_ns, t_lit);
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
    // Translate once for the whole float4. GuestReadF32 re-tests the 0xA0000000 physical
    // window and recomputes the host pointer on every component, and this loop runs ~82k
    // times a frame (10 registers x ~8.6k uploads).
    //
    // This MUST mirror GuestRead32 exactly: GuestTranslatePhysical, not the GpuPhysical
    // variant the literal path above uses. Getting that wrong reads garbage for every
    // constant the ring owns -- including the world matrix -- and renders the frame black.
    const uint8_t* src = addr >= 0xA0000000u ? GuestTranslatePhysical(addr) : base + addr;
    for (uint32_t c = 0; c < 4; ++c) {
      const uint32_t bits = *reinterpret_cast<const rex::be<uint32_t>*>(src + c * 4);
      std::memcpy(&dst[c], &bits, sizeof(float));
    }
  };

  // Literal-constant probe. 36 of the 46 pixel shaders that can output a varying alpha in this
  // scene take that alpha from c252-c255 (typically `cndeq r7.___w, ..., c253.yyyy`), so what
  // those four registers resolve to decides whether a premultiplied surface comes out
  // transparent or opaque. They are loaded straight into the GPU constant file by
  // D3D::SetLiteralShaderConstants and never reach the shadow, so if the shader's own literal
  // table does not cover them the fallback path reads the shadow's ZERO -- a silent wrong value
  // that looks like a legitimate constant. Report the PROVENANCE, not just the number: "0 from a
  // literal" and "0 because nothing supplied it" are different bugs and identical in a value dump.
  if (const uint32_t lit_budget = REXCVAR_GET(nx1_d3d9_dbg_literals)) {
    static std::mutex lm;
    static std::vector<uint32_t> seen;
    std::lock_guard<std::mutex> lk(lm);
    if (std::find(seen.begin(), seen.end(), shader_object) == seen.end() && seen.size() < lit_budget) {
      seen.push_back(shader_object);
      for (uint32_t r = 252; r <= 255; ++r) {
        const uint32_t index = r + file_base;
        int from_literal = -1;
        for (uint32_t i = 0; i < literal_count; ++i) {
          if (index >= literals[i].reg && index < literals[i].reg + literals[i].float4s) {
            from_literal = int(i);
            break;
          }
        }
        float v[4] = {};
        read_register(r, v);
        REXGPU_INFO("nx1_d3d9: LITERAL {} obj={:08X} c{} = {} {} {} {}  source={}",
                    pixel_stage ? "PS" : "VS", shader_object, r, v[0], v[1], v[2], v[3],
                    from_literal >= 0 ? "shader literal table" : "SHADOW/RING (likely zero)");
      }
      // literal_count==0 for EVERY shader would mean the literal path is dead rather than that
      // these shaders have no literals, so report the field ReadShaderLiterals gives up on.
      // def_offset==0 means the guest's definition-table pointer read as null: either the shader
      // genuinely has no table, or the offsets we walk it with are wrong.
      const uint32_t fn = shader_object + (pixel_stage ? guest_shader::kPsFunctionOffset
                                                       : guest_shader::kVsFunctionOffset);
      const uint32_t def_off = GuestRead32(
          base, fn + guest_shader::kUcodePassArray + guest_shader::kDefinitionTableOffsetField);
      const uint32_t phys = GuestRead32(base, shader_object + (pixel_stage
                                                                   ? guest_shader::kPsPhysicalOffset
                                                                   : guest_shader::kVsPhysicalOffset));
      REXGPU_INFO("nx1_d3d9: LITERAL {} obj={:08X} literal_count={} file_base={} def_offset={:#x} "
                  "physical={:08X} remapCount={}",
                  pixel_stage ? "PS" : "VS", shader_object, literal_count, file_base, def_off, phys,
                  e.remapCount);
    }
  }

  const auto t_read = pmark();
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

  // Report what ONE host register of ONE shader actually receives. The c252-c255 probe above is
  // fixed to the literal window; this answers the general question, which is what a shader
  // disassembly actually raises ("colour = r0 * c8.w + r4 -- so what is c8.w?").
  //
  // Host vs guest matters: the translated shader indexes HOST registers, and the remap decides
  // which guest register fills each one, so reporting a guest value against a host index would be
  // meaningless. Report both, plus whether the shader s the register itself -- a def is
  // supplied by the shader and our upload deliberately skips it, so the staging value would not
  // be what the shader sees.
  if (const uint32_t obj = REXCVAR_GET(nx1_d3d9_dbg_const_obj);
      obj && shader_object == obj) {
    const uint32_t hostreg = REXCVAR_GET(nx1_d3d9_dbg_const_reg);
    static std::mutex cm;
    static uint32_t last_obj = 0, last_reg = 0xFFFFFFFFu;
    std::lock_guard<std::mutex> clk(cm);
    if (last_obj != obj || last_reg != hostreg) {
      last_obj = obj;
      last_reg = hostreg;
      const bool uncompacted = (e.flags & NX1_SM3_UNCOMPACTED_CONSTANTS) != 0;
      const uint32_t guestreg =
          uncompacted ? hostreg
          : (hostreg < e.remapCount ? g_nx1Sm3ConstantRemap[e.remapOffset + hostreg] : 0xFFFFFFFFu);
      const bool is_def = hostreg < 256 && (shader.def_mask[hostreg >> 5] & (1u << (hostreg & 31)));
      if (hostreg < count) {
        const float* v = &staging[hostreg * 4];
        REXGPU_INFO("nx1_d3d9: CONSTPROBE {} obj={:08X} host c{} = {} {} {} {} | guest c{} "
                    "remapCount={} def={} ({})",
                    pixel_stage ? "PS" : "VS", obj, hostreg, v[0], v[1], v[2], v[3], guestreg,
                    e.remapCount, is_def ? 1 : 0,
                    is_def ? "SHADER-SUPPLIED def, our upload skips it" : "uploaded by us");
      } else {
        REXGPU_INFO("nx1_d3d9: CONSTPROBE {} obj={:08X} host c{} is OUTSIDE the uploaded window "
                    "(count={}, remapCount={}, def={}) -- the shader reads whatever is left there",
                    pixel_stage ? "PS" : "VS", obj, hostreg, count, e.remapCount, is_def ? 1 : 0);
      }
    }
  }

  padd(prof_.read_ns, t_read);
  if (prof_enabled_) prof_.registers += count;
  return count;
#endif
}

void ShaderCache::ApplyConstants(const Sm3Shader& shader, bool pixel_stage, const float* staging,
                                 uint32_t count) {
#ifndef NX1_HAVE_SM3_SHADER_CACHE
  (void)shader; (void)pixel_stage; (void)staging; (void)count;
#else
  if (!device_) {
    return;
  }
  const auto pmark = [this] {
    return prof_enabled_ ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
  };
  const auto padd = [this](uint64_t& sink, std::chrono::steady_clock::time_point t0) {
    if (prof_enabled_) sink += uint64_t((std::chrono::steady_clock::now() - t0).count());
  };

  // Only when this stage's shader changed. Our own window upload deliberately steps AROUND
  // this shader's def registers, so while the same shader stays bound nothing we issue can
  // disturb them; it is a DIFFERENT shader's window covering those registers that makes the
  // re-assert necessary at all. The two stages have separate D3D9 constant files, so a pixel
  // upload can never clobber a vertex def or vice versa -- hence one slot per stage.
  //
  // Keys on the Sm3Shader address, which is stable because ShaderMap is a node-based
  // std::unordered_map. Do not convert that map to FlatMap without revisiting this.
  const uint32_t stage = pixel_stage ? 1u : 0u;
  if (count == 0) {
    // No window to upload, but the defs still need asserting (see below).
    if (last_def_shader_[stage] != &shader) {
      for (uint32_t d = 0; d < shader.def_count; ++d) {
        if (pixel_stage) {
          device_->SetPixelShaderConstantF(shader.defs[d].reg, shader.defs[d].v, 1);
        } else {
          device_->SetVertexShaderConstantF(shader.defs[d].reg, shader.defs[d].v, 1);
        }
      }
      last_def_shader_[stage] = &shader;
      if (prof_enabled_) prof_.def_writes += shader.def_count;
    }
    return;
  }

  // Upload in runs that go AROUND the shader's own `def` registers. A def is applied when the
  // shader is set, lives in this same constant file, and fxc places one wherever a declared
  // uniform's uses were optimized away -- including inside [0, count). Writing over one feeds
  // garbage to every cmp fxc flattened: measured live, c19 def=(0,1,-1,-0) held literal-pool
  // garbage on every sun-shadow shader, which is what actually killed the sun shadows.
  const auto t_win = pmark();
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

  padd(prof_.window_ns, t_win);
  const auto t_defs = pmark();

  // Re-assert the shader's own `def` literals. Skipping them above is necessary but NOT
  // sufficient: the runtime does not reapply defs on SetShader (measured -- see
  // Sm3Shader::defs), so a previous shader's window upload that legitimately covered these
  // registers would otherwise still be sitting in them for this shader's draw.
  if (last_def_shader_[stage] != &shader) {
    for (uint32_t d = 0; d < shader.def_count; ++d) {
      if (pixel_stage) {
        device_->SetPixelShaderConstantF(shader.defs[d].reg, shader.defs[d].v, 1);
      } else {
        device_->SetVertexShaderConstantF(shader.defs[d].reg, shader.defs[d].v, 1);
      }
    }
    last_def_shader_[stage] = &shader;
    if (prof_enabled_) prof_.def_writes += shader.def_count;
  }
  padd(prof_.defs_ns, t_defs);
#endif
}

void ShaderCache::UploadConstants(const uint8_t* base, uint32_t guest_device,
                                  const Sm3Shader& shader, bool pixel_stage) {
  float staging[kMaxHostConstants * 4];
  const uint32_t count = ResolveConstants(base, guest_device, shader, pixel_stage, staging);
  ApplyConstants(shader, pixel_stage, staging, count);
}

}  // namespace nx1::d3d9

#endif  // _WIN32
