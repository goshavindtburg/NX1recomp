/**
 * Lower XenosRecomp's DXIL-flavoured HLSL to Shader Model 3.0, for the native
 * D3D9 renderer.
 *
 * The transforms here were validated against the full NX1 shader corpus with the
 * real fxc: 564/565 VS and 2345/2358 PS compile (99.5%). See
 * tools/sm3_feasibility/sm3_rewrite.py for the prototype and the measurements.
 *
 * The three things that make this possible:
 *   1. ps_3_0 has 224 float constants and most shaders reference c224..c255 --
 *      but no shader references more than 27 *distinct* constants, so the
 *      constant file is compacted and a remap table handed to the renderer. The
 *      high registers are shader literals that the Xenon runtime already wrote
 *      into the guest ALU constant file, so they need no special handling.
 *   2. ps_3_0 has 10 input registers against Xenos's 16 interpolators -- but no
 *      NX1 vertex shader writes more than 10, so capping is lossless.
 *   3. The unpatched HLSL already uses real IA semantics, so D3D9's input
 *      assembler can be used directly (no SV_VertexID / vfetch emulation).
 */

#include "sm3_transform.h"

#include <algorithm>
#include <map>
#include <regex>
#include <set>

namespace
{

const char* const kPrelude = R"(
#define FLT_MIN 1.175494351e-38f
#define FLT_MAX 3.402823466e+38f

#define SPEC_CONSTANT_R11G11B10_NORMAL 1
#define SPEC_CONSTANT_ALPHA_TEST       2

float4 dst(float4 a, float4 b) { return float4(1.0, a.y * b.y, a.z, b.w); }
float  max4(float4 v) { return max(max(v.x, v.y), max(v.z, v.w)); }

// fxc predates trunc().
float  xe_trunc(float  v) { return sign(v) * floor(abs(v)); }
float2 xe_trunc(float2 v) { return sign(v) * floor(abs(v)); }
float3 xe_trunc(float3 v) { return sign(v) * floor(abs(v)); }
float4 xe_trunc(float4 v) { return sign(v) * floor(abs(v)); }

// Xenos `cube` collects up to two direction vectors; tfetchCube then samples the
// one selected by the .z channel it returned. Branch-selected, because SM3
// cannot dynamically index a local array.
struct CubeMapData
{
    float3 cubeMapDirections[2];
    int cubeMapIndex;
};

// The returned selector is 1-BASED, and must stay that way. Hardware returns a non-zero .w
// here (the face id / major axis), and NX1's world lighting shaders do not merely carry it to
// tfetchCube -- they predicate on it:
//     cube r2, r11.xxzy, r11.yzxx
//     setp_ne_push r4.___w, c252.xxxx, r2.wwww   ; p0 = (c252.x == 0) && (r2.w != 0)
//     (p0) tfetch2D r7.x___, r16.xy, tf5         ; the eight sun shadow taps
// A 0-based index made the first (and usually only) cube in a shader return .w = 0, so p0 was
// false on every pixel and every shadow tap was dead code -- the world rendered lit but
// perfectly flat. Keep this in step with cubeDir's threshold below.
float4 cube(float4 value, inout CubeMapData d)
{
    if (d.cubeMapIndex == 0) d.cubeMapDirections[0] = value.xyz;
    else                     d.cubeMapDirections[1] = value.xyz;
    float idx = (float)d.cubeMapIndex;
    d.cubeMapIndex++;
    return float4(0.0, 0.0, 0.0, idx + 1.0);
}

float3 cubeDir(CubeMapData d, float sel)
{
    return sel < 1.5 ? d.cubeMapDirections[0] : d.cubeMapDirections[1];
}

// Same semantics as shader_common.h's Nx1AlphaTestClip, but the compare function
// arrives as a float: ps_3_0 has no integer constant registers.
// NOTE: `pass` is a reserved keyword in fxc (effect-file syntax), hence `alphaPass`.
void Nx1AlphaTestClip(float alpha, float threshold, float compareFunction)
{
    bool alphaPass = false;
    if      (compareFunction < 0.5) alphaPass = false;         // never
    else if (compareFunction < 1.5) alphaPass = alpha <  threshold;
    else if (compareFunction < 2.5) alphaPass = alpha == threshold;
    else if (compareFunction < 3.5) alphaPass = alpha <= threshold;
    else if (compareFunction < 4.5) alphaPass = alpha >  threshold;
    else if (compareFunction < 5.5) alphaPass = alpha != threshold;
    else if (compareFunction < 6.5) alphaPass = alpha >= threshold;
    else                            alphaPass = true;          // always
    clip(alphaPass ? 1.0 : -1.0);
}
)";

// Mirrors shader_common.h.
constexpr uint32_t kSpecConstantR11G11B10Normal = 1u << 0;
constexpr uint32_t kSpecConstantAlphaTest = 1u << 1;

bool isIdentChar(char c)
{
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}

/// Find the matching ')' for the '(' at `open`.
size_t matchParen(const std::string& s, size_t open)
{
    int depth = 0;
    for (size_t i = open; i < s.size(); ++i)
    {
        if (s[i] == '(')
            ++depth;
        else if (s[i] == ')' && --depth == 0)
            return i;
    }
    return std::string::npos;
}

/// Find `name(` where `name` is not part of a longer identifier.
/// This matters: `frac(` and `trunc(` both end in "c(".
size_t findCall(const std::string& s, const std::string& name, size_t from)
{
    const std::string needle = name + "(";
    for (size_t i = s.find(needle, from); i != std::string::npos; i = s.find(needle, i + 1))
    {
        if (i == 0 || !isIdentChar(s[i - 1]))
            return i;
    }
    return std::string::npos;
}

std::vector<std::string> splitTopLevelArgs(const std::string& s)
{
    std::vector<std::string> out;
    std::string cur;
    int depth = 0;
    for (char c : s)
    {
        if (c == '(' || c == '[')
            ++depth;
        else if (c == ')' || c == ']')
            --depth;

        if (c == ',' && depth == 0)
        {
            out.push_back(cur);
            cur.clear();
        }
        else
        {
            cur += c;
        }
    }
    out.push_back(cur);
    return out;
}

std::string trim(const std::string& s)
{
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos)
        return {};
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

/// select(cond, a, b) -> ((cond) ? (a) : (b)). HLSL 2021 intrinsic; fxc lacks it.
bool rewriteSelect(std::string& body, std::string& error)
{
    for (size_t i = findCall(body, "select", 0); i != std::string::npos;
         i = findCall(body, "select", 0))
    {
        size_t open = i + 6;
        size_t close = matchParen(body, open);
        if (close == std::string::npos)
        {
            error = "unbalanced select()";
            return false;
        }
        auto args = splitTopLevelArgs(body.substr(open + 1, close - open - 1));
        if (args.size() != 3)
        {
            error = "select() with " + std::to_string(args.size()) + " args";
            return false;
        }
        body.replace(i, close - i + 1,
                     "((" + trim(args[0]) + ") ? (" + trim(args[1]) + ") : (" + trim(args[2]) + "))");
    }
    return true;
}

/// Rewrite every c(...) reference into a __C__<n>__ placeholder (static) or a
/// direct c[expr] (dynamic / relative addressing).
void rewriteConstants(std::string& body, std::set<uint16_t>& staticRefs, bool& dynamic)
{
    dynamic = false;
    size_t pos = 0;
    while (true)
    {
        size_t i = findCall(body, "c", pos);
        if (i == std::string::npos)
            return;
        size_t open = i + 1;
        size_t close = matchParen(body, open);
        if (close == std::string::npos)
            return;

        const std::string inner = trim(body.substr(open + 1, close - open - 1));
        const bool numeric = !inner.empty() &&
            std::all_of(inner.begin(), inner.end(), [](char c) { return std::isdigit((unsigned char)c); });

        std::string replacement;
        if (numeric)
        {
            staticRefs.insert(static_cast<uint16_t>(std::stoi(inner)));
            replacement = "__C__" + inner + "__";
        }
        else
        {
            dynamic = true;
            replacement = "c[" + inner + "]";
        }
        body.replace(i, close - i + 1, replacement);
        pos = i + replacement.size();
    }
}

std::string regexReplace(const std::string& s, const std::string& pattern, const std::string& fmt)
{
    return std::regex_replace(s, std::regex(pattern), fmt);
}

bool referencesIdent(const std::string& body, const std::string& name)
{
    for (size_t i = body.find(name); i != std::string::npos; i = body.find(name, i + 1))
    {
        const bool leftOk = (i == 0) || !isIdentChar(body[i - 1]);
        const size_t end = i + name.size();
        const bool rightOk = (end >= body.size()) || !isIdentChar(body[end]);
        if (leftOk && rightOk)
            return true;
    }
    return false;
}

struct Param
{
    std::string dir, type, name, sem;
};

}  // namespace

bool transformToSm3(const std::string& source, bool isPixelShader, uint32_t specConstantsMask,
                    Sm3Shader& out, std::string& error)
{
    const size_t mainPos = source.find("void main(");
    if (mainPos == std::string::npos)
    {
        error = "no main()";
        return false;
    }
    const size_t sigOpen = source.find('(', mainPos);
    const size_t sigClose = matchParen(source, sigOpen);
    const size_t bodyOpen = source.find('{', sigClose);
    const size_t bodyClose = source.rfind('}');
    if (sigClose == std::string::npos || bodyOpen == std::string::npos || bodyClose <= bodyOpen)
    {
        error = "malformed main()";
        return false;
    }

    std::string sig = source.substr(sigOpen + 1, sigClose - sigOpen - 1);
    std::string body = source.substr(bodyOpen + 1, bodyClose - bodyOpen - 1);

    // Keep the non-SPIR-V arm of the iFace #ifdef, drop the vk::location attrs.
    sig = std::regex_replace(sig, std::regex(R"(#ifdef __spirv__[\s\S]*?#else([\s\S]*?)#endif)"), "$1");
    sig = regexReplace(sig, R"(\[\[vk::[^\]]*\]\])", "");

    std::vector<Param> params;
    for (const std::string& piece : splitTopLevelArgs(sig))
    {
        std::smatch m;
        const std::string line = trim(piece);
        if (std::regex_match(line, m, std::regex(R"((in|out)\s+(\S+)\s+(\w+)\s*:\s*(\w+))")))
            params.push_back({m[1], m[2], m[3], m[4]});
    }

    // XenosRecomp seeds r0..r15 from all 16 Xenos interpolators. ps_3_0 has ten
    // input registers, and no NX1 vertex shader writes more than ten, so the
    // rest are undefined on the guest as well: zero them.
    if (isPixelShader)
    {
        std::regex seed(R"(float4 r(\d+) = i(?:TexCoord|Color)(\d+);)");
        std::string rebuilt;
        auto begin = std::sregex_iterator(body.begin(), body.end(), seed);
        size_t last = 0;
        for (auto it = begin; it != std::sregex_iterator(); ++it)
        {
            const std::smatch& m = *it;
            rebuilt.append(body, last, m.position() - last);
            if (std::stoi(m[2]) < kSm3MaxInterpolators)
                rebuilt.append(m.str());
            else
                rebuilt.append("float4 r" + m[1].str() + " = 0.0;");
            last = m.position() + m.length();
        }
        rebuilt.append(body, last, std::string::npos);
        body = std::move(rebuilt);
    }

    // Unwritten interpolator COMPONENTS must reach the pixel shader as (0, 0, 0, 1) -- the
    // hardware default. NX1's world VS exports only oTexCoord5.xyz, and its lighting PS gates
    // the entire sun/shadow path on `albedo.a * v5.w != 0`: a zero default makes that predicate
    // false on every pixel, silently swapping dynamic lighting for the baked fallback (world
    // lit but flat, no sun shadows). Upgrade the zero init to float4(0,0,0,1) ONLY for
    // interpolators the shader actually writes: fully-unwritten ones keep `= 0.0;` so fxc still
    // eliminates them (vs_3_0 has 12 output registers against our 18 declared outputs).
    {
        std::regex initRe(R"((oTexCoord\d+|oColor\d+) = 0\.0;)");
        std::string rebuilt;
        rebuilt.reserve(body.size() + 256);
        auto begin = std::sregex_iterator(body.begin(), body.end(), initRe);
        size_t last = 0;
        for (auto it = begin; it != std::sregex_iterator(); ++it)
        {
            const std::string name = (*it)[1];
            // The init itself is one occurrence; any second occurrence in the body is a write.
            size_t occurrences = 0;
            for (size_t pos = body.find(name); pos != std::string::npos && occurrences < 2;
                 pos = body.find(name, pos + name.size()))
            {
                // Whole-name match only: oTexCoord1 must not count oTexCoord10..15.
                const size_t end = pos + name.size();
                if (end >= body.size() || !std::isdigit(static_cast<unsigned char>(body[end])))
                    ++occurrences;
            }
            rebuilt.append(body, last, it->position() - last);
            if (occurrences >= 2)
                rebuilt.append(name + " = float4(0.0, 0.0, 0.0, 1.0);");
            else
                rebuilt.append(it->str());
            last = it->position() + it->length();
        }
        rebuilt.append(body, last, std::string::npos);
        body = std::move(rebuilt);
    }

    // Sampler usage must be collected before the tfetch rewrite erases the names.
    std::map<uint8_t, std::string> samplerType;
    {
        std::regex re(R"(s(\d+)_Texture(1D|2D|3D|Cube)DescriptorIndex)");
        for (auto it = std::sregex_iterator(body.begin(), body.end(), re);
             it != std::sregex_iterator(); ++it)
        {
            const uint8_t idx = static_cast<uint8_t>(std::stoi((*it)[1]));
            const std::string kind = (*it)[2];
            const std::string ty = kind == "1D" ? "sampler1D"
                                 : kind == "2D" ? "sampler2D"
                                 : kind == "3D" ? "sampler3D"
                                                : "samplerCUBE";
            samplerType.emplace(idx, ty);
        }
    }

    // --- texture fetches ------------------------------------------------------
    body = regexReplace(body,
        R"(tfetch2D\(s(\d+)_Texture2DDescriptorIndex,\s*s\1_SamplerDescriptorIndex,\s*([^,]+),\s*float2\(0,\s*0\)\))",
        "tex2D(s$1, $2)");
    body = regexReplace(body,
        R"(tfetch2D\(s(\d+)_Texture2DDescriptorIndex,\s*s\1_SamplerDescriptorIndex,\s*([^,]+),\s*(float2\([^)]*\))\))",
        "tex2D(s$1, $2 + $3 * g_InvTexDim[$1].xy)");
    body = regexReplace(body,
        R"(tfetch1D\(s(\d+)_Texture1DDescriptorIndex,\s*s\1_SamplerDescriptorIndex,\s*([^)]+)\))",
        "tex1D(s$1, $2)");
    body = regexReplace(body,
        R"(tfetch3D\(s(\d+)_Texture3DDescriptorIndex,\s*s\1_SamplerDescriptorIndex,\s*([^)]+)\))",
        "tex3D(s$1, $2)");
    body = regexReplace(body,
        R"(tfetchCube\(s(\d+)_TextureCubeDescriptorIndex,\s*s\1_SamplerDescriptorIndex,\s*([^,]+),\s*cubeMapData\))",
        "texCUBE(s$1, cubeDir(cubeMapData, ($2).z))");

    // Normal decode moves to the vertex-upload path; the texcoord swap is decided
    // by the vertex declaration, which the renderer owns.
    body = regexReplace(body, R"(tfetchR11G11B10\((\w+)\))", "$1");
    body = regexReplace(body, R"(tfetchTexcoord\(g_SwappedTexcoords,\s*(\w+),\s*\d+\))", "$1");

    // SM3 has no integer ops, so spec-constant tests cannot be evaluated at
    // runtime -- bake them from the shader's own mask. Alpha test stays live
    // (the compare function is a float uniform); the R11G11B10 decode has
    // already been moved to the vertex-upload path above.
    body = regexReplace(body, R"(g_SpecConstants\(\) & SPEC_CONSTANT_ALPHA_TEST)",
                        (specConstantsMask & kSpecConstantAlphaTest) ? "true" : "false");
    body = regexReplace(body, R"(g_SpecConstants\(\) & SPEC_CONSTANT_R11G11B10_NORMAL)", "false");
    body = regexReplace(body, R"(g_SpecConstants\(\))", "0");
    body = regexReplace(body, R"((^|[^A-Za-z0-9_])trunc\()", "$1xe_trunc(");
    if (!rewriteSelect(body, error))
        return false;

    // --- constants ------------------------------------------------------------
    std::set<uint16_t> staticRefs;
    bool dynamic = false;
    rewriteConstants(body, staticRefs, dynamic);

    int constCount;
    if (dynamic)
    {
        // These shaders reference c255 as well, so all 256 vs_3_0 float registers
        // are spoken for and there is nowhere to put the NDC / half-pixel params.
        // Drop those lines; the host must fold the transform into the projection
        // constants for these draws instead.
        constCount = 256;
        body = regexReplace(body, R"(__C__(\d+)__)", "c[$1]");
        body = regexReplace(body, R"(\n[^\n]*oPos\.xy \+= g_HalfPixelOffset \* oPos\.w;)", "");
        body = regexReplace(body,
            R"(\n[^\n]*oPos\.xyz = oPos\.xyz \* g_NdcScale \+ g_NdcOffset \* oPos\.w;)", "");
        out.uncompactedConstants = true;
        out.needsHostHalfPixel = true;
    }
    else
    {
        std::map<uint16_t, int> slot;
        for (uint16_t reg : staticRefs)
        {
            const int s = static_cast<int>(out.constantRemap.size());
            slot[reg] = s;
            out.constantRemap.push_back(reg);
        }
        constCount = std::max<int>(1, static_cast<int>(out.constantRemap.size()));

        std::string rebuilt;
        std::regex ph(R"(__C__(\d+)__)");
        auto begin = std::sregex_iterator(body.begin(), body.end(), ph);
        size_t last = 0;
        for (auto it = begin; it != std::sregex_iterator(); ++it)
        {
            const std::smatch& m = *it;
            rebuilt.append(body, last, m.position() - last);
            rebuilt.append("c[" + std::to_string(slot[(uint16_t)std::stoi(m[1])]) + "]");
            last = m.position() + m.length();
        }
        rebuilt.append(body, last, std::string::npos);
        body = std::move(rebuilt);
    }

    // --- signature ------------------------------------------------------------
    std::vector<std::string> ins, outs;
    for (const Param& p : params)
    {
        if (p.dir == "in")
        {
            if (!referencesIdent(body, p.name))
                continue;
            std::string sem = p.sem, ty = "float4";
            if (sem == "SV_Position")
            {
                if (!isPixelShader)
                    continue;  // VS position is an output only
                sem = "VPOS";
                ty = "float2";
            }
            else if (sem == "SV_IsFrontFace")
            {
                sem = "VFACE";
                ty = "float";
            }
            ins.push_back("in " + ty + " " + p.name + " : " + sem);
        }
        else
        {
            std::string sem = p.sem;
            bool forced = false;
            if (sem == "SV_Position")
            {
                sem = "POSITION";
                forced = true;
            }
            else if (sem.rfind("SV_Target", 0) == 0)
            {
                sem = "COLOR" + (sem.size() > 9 ? sem.substr(9) : std::string("0"));
                forced = true;
            }
            else if (sem == "SV_Depth")
            {
                sem = "DEPTH";
                forced = true;
            }

            if (!forced)
            {
                // Dead if every write is a literal zero and it is never read: SM3
                // has far fewer varying registers than Xenos.
                const std::string stripped = std::regex_replace(
                    body, std::regex(R"(\n[ \t]*)" + p.name + R"((\.[xyzw]+)?[ \t]*=[ \t]*0\.0;)"), "");
                if (!referencesIdent(stripped, p.name))
                {
                    body = stripped;
                    continue;
                }
            }
            outs.push_back("out float4 " + p.name + " : " + sem);
        }
    }

    // XenosRecomp declares all 64 Xenos GPRs; ps_3_0 has 32 temps. Drop the
    // declarations the body never touches and let fxc allocate.
    for (int k = 0; k < 64; ++k)
    {
        const std::string decl = "\n\tfloat4 r" + std::to_string(k) + " = 0.0;";
        const size_t at = body.find(decl);
        if (at == std::string::npos)
            continue;
        std::string stripped = body;
        stripped.erase(at, decl.size());
        if (!referencesIdent(stripped, "r" + std::to_string(k)))
            body = std::move(stripped);
    }

    // --- assemble -------------------------------------------------------------
    const bool needDims = body.find("g_InvTexDim") != std::string::npos;

    // Uniform registers sit at *fixed* offsets above the constant window, whether
    // or not this particular shader declares them. The host only knows
    // `remapCount`, so a sequentially-packed layout would be unaddressable.
    std::string uniforms;
    if (isPixelShader)
    {
        if (body.find("g_AlphaThreshold") != std::string::npos)
            uniforms += "float g_AlphaThreshold : register(c" + std::to_string(constCount) + ");\n";
        if (body.find("g_AlphaTestFunction") != std::string::npos)
            uniforms += "float g_AlphaTestFunction : register(c" + std::to_string(constCount + 1) + ");\n";
        if (needDims)
            uniforms += "float4 g_InvTexDim[16] : register(c" + std::to_string(constCount + 2) + ");\n";
    }
    else if (body.find("g_NdcScale") != std::string::npos ||
             body.find("g_HalfPixelOffset") != std::string::npos)
    {
        // Pack the NDC transform and the half-pixel bias into two registers, so a
        // vertex shader spends at most two of its 256 on them.
        //   [0] = float4(ndcScale.xyz,  halfPixelOffset.x)
        //   [1] = float4(ndcOffset.xyz, halfPixelOffset.y)
        uniforms += "float4 g_Nx1VsParams[2] : register(c" + std::to_string(constCount) + ");\n";
        uniforms += "#define g_NdcScale        g_Nx1VsParams[0].xyz\n";
        uniforms += "#define g_NdcOffset       g_Nx1VsParams[1].xyz\n";
        uniforms += "#define g_HalfPixelOffset float2(g_Nx1VsParams[0].w, g_Nx1VsParams[1].w)\n";
    }

    std::string samplers;
    for (const auto& [idx, ty] : samplerType)
    {
        samplers += ty + " s" + std::to_string(idx) + " : register(s" + std::to_string(idx) + ");\n";
        out.samplers.push_back(idx);
    }

    std::string sigOut;
    for (size_t i = 0; i < ins.size(); ++i)
        sigOut += (i ? ",\n\t" : "\t") + ins[i];
    for (size_t i = 0; i < outs.size(); ++i)
        sigOut += (ins.empty() && i == 0 ? "\t" : ",\n\t") + outs[i];

    out.hlsl = std::string(kPrelude) +
               "\nfloat4 c[" + std::to_string(constCount) + "] : register(c0);\n" +
               uniforms + samplers +
               "\nvoid main(\n" + sigOut + ")\n{" + body + "}\n";
    return true;
}
