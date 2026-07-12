#!/usr/bin/env python3
"""
Rewrite XenosRecomp's DXIL-flavoured HLSL into Shader Model 3.0 HLSL and compile
it with fxc, to measure how much of NX1's shader corpus a D3D9 backend can take.

Key transforms:
  * bindless Texture2D/SamplerState heaps  -> sampler2D/3D/CUBE at register(sN)
  * float4 c[256] cbuffer                  -> compacted float4 c[K] : register(c0)
                                              (max 27 distinct consts per shader,
                                               so literals at c224..c255 fit fine)
  * unused interpolators                   -> pruned (SM3 has ~10 in / 12 out regs)
  * tfetchR11G11B10(uint4)                 -> plain float4 input (decode moves to
                                              the vertex-upload path)
  * SV_Position/SV_Target/SV_IsFrontFace   -> POSITION/COLOR/VFACE
"""
import argparse
import os
import re
import subprocess
import sys
import tempfile

FXC = r"C:\Program Files (x86)\Windows Kits\10\bin\10.0.22621.0\x64\fxc.exe"

# ps_3_0 exposes v0..v9. The corpus never transports more than 10 interpolators.
PS_MAX_INTERPOLATORS = 10

# NOTE: the leading lookbehind is essential -- `frac(` and `trunc(` both end in
# "c(", so an unanchored pattern rewrites their innards and (worse) makes them
# look like dynamic constant addressing.
C_REF = re.compile(r"(?<![A-Za-z0-9_])c\((\d+)\)")
C_REF_ANY = re.compile(r"(?<![A-Za-z0-9_])c\(([^()]*(?:\([^()]*\))?[^()]*)\)")
C_REF_DYNAMIC = re.compile(r"(?<![A-Za-z0-9_])c\(\s*[^)\d]")  # c(a0 + 3) etc.

SIG_PARAM = re.compile(r"^\s*(in|out)\s+(\S+)\s+(\w+)\s*:\s*(\w+)")

PRELUDE = r"""
#define FLT_MIN 1.175494351e-38f
#define FLT_MAX 3.402823466e+38f

#define SPEC_CONSTANT_R11G11B10_NORMAL 1
#define SPEC_CONSTANT_ALPHA_TEST       2

float4 dst(float4 a, float4 b) { return float4(1.0, a.y * b.y, a.z, b.w); }
float  max4(float4 v) { return max(max(v.x, v.y), max(v.z, v.w)); }

// SM3's compiler predates trunc().
float  xe_trunc(float  v) { return sign(v) * floor(abs(v)); }
float2 xe_trunc(float2 v) { return sign(v) * floor(abs(v)); }
float3 xe_trunc(float3 v) { return sign(v) * floor(abs(v)); }
float4 xe_trunc(float4 v) { return sign(v) * floor(abs(v)); }

// Xenos `cube` collects up to two direction vectors; tfetchCube then samples the
// one selected by the .z channel it returned. Indices are branch-selected rather
// than dynamically indexed -- SM3 cannot dynamically index a local array.
struct CubeMapData
{
    float3 cubeMapDirections[2];
    int cubeMapIndex;
};

float4 cube(float4 value, inout CubeMapData d)
{
    if (d.cubeMapIndex == 0) d.cubeMapDirections[0] = value.xyz;
    else                     d.cubeMapDirections[1] = value.xyz;
    float idx = (float)d.cubeMapIndex;
    d.cubeMapIndex++;
    return float4(0.0, 0.0, 0.0, idx);
}

float3 cubeDir(CubeMapData d, float sel)
{
    return sel < 0.5 ? d.cubeMapDirections[0] : d.cubeMapDirections[1];
}
"""


def sampler_decls(body, stage):
    """Emit sampler declarations only for the samplers the body actually uses."""
    kinds = {}
    for m in re.finditer(r"s(\d+)_Texture(1D|2D|3D|Cube)DescriptorIndex", body):
        kinds.setdefault(int(m.group(1)), set()).add(m.group(2))
    out = []
    for idx in sorted(kinds):
        # A slot is only ever sampled as one type in practice; pick deterministically.
        k = sorted(kinds[idx])[0]
        t = {"1D": "sampler1D", "2D": "sampler2D", "3D": "sampler3D", "Cube": "samplerCUBE"}[k]
        out.append(f"{t} s{idx} : register(s{idx});")
    return "\n".join(out), kinds


def rewrite_tfetch(body):
    # tfetch2D(sK_Texture2DDescriptorIndex, sK_SamplerDescriptorIndex, UV, float2(a, b))
    def r2d(m):
        k, uv, ox, oy = m.group(1), m.group(2).strip(), m.group(3), m.group(4)
        if ox.strip() in ("0", "0.0") and oy.strip() in ("0", "0.0"):
            return f"tex2D(s{k}, {uv})"
        return f"tex2D(s{k}, {uv} + float2({ox}, {oy}) * g_InvTexDim[{k}].xy)"

    body = re.sub(
        r"tfetch2D\(s(\d+)_Texture2DDescriptorIndex,\s*s\1_SamplerDescriptorIndex,\s*"
        r"([^,]+),\s*float2\(([^,]+),\s*([^)]+)\)\)",
        r2d, body)

    body = re.sub(
        r"tfetch1D\(s(\d+)_Texture1DDescriptorIndex,\s*s\1_SamplerDescriptorIndex,\s*([^)]+)\)",
        lambda m: f"tex1D(s{m.group(1)}, {m.group(2)})", body)

    body = re.sub(
        r"tfetch3D\(s(\d+)_Texture3DDescriptorIndex,\s*s\1_SamplerDescriptorIndex,\s*([^)]+)\)",
        lambda m: f"tex3D(s{m.group(1)}, {m.group(2)})", body)

    # tfetchCube(tex, samp, coord, cubeMapData) -> texCUBE(sK, cubeDir(...))
    body = re.sub(
        r"tfetchCube\(s(\d+)_TextureCubeDescriptorIndex,\s*s\1_SamplerDescriptorIndex,\s*"
        r"([^,]+),\s*cubeMapData\)",
        lambda m: f"texCUBE(s{m.group(1)}, cubeDir(cubeMapData, ({m.group(2)}).z))", body)

    # Normal decode moves to the vertex-upload path.
    body = re.sub(r"tfetchR11G11B10\((\w+)\)", r"\1", body)
    # Texcoord swap is decided by the vertex declaration, which we control.
    body = re.sub(r"tfetchTexcoord\(g_SwappedTexcoords,\s*(\w+),\s*\d+\)", r"\1", body)
    # Spec constants are baked per variant; this harness measures variant 0.
    body = body.replace("g_SpecConstants()", "0")
    # SM3's fxc predates trunc() and the HLSL-2021 select().
    body = re.sub(r"(?<![A-Za-z0-9_])trunc\(", "xe_trunc(", body)
    body = rewrite_select(body)
    return body


def split_args(s):
    """Split a top-level comma-separated argument list."""
    out, depth, cur = [], 0, ""
    for ch in s:
        if ch in "([":
            depth += 1
        elif ch in ")]":
            depth -= 1
        if ch == "," and depth == 0:
            out.append(cur)
            cur = ""
        else:
            cur += ch
    out.append(cur)
    return out


def rewrite_c(body):
    """Rewrite every c(...) reference. Returns (body, static_indices, dynamic).

    Static refs become __C__<n>__ placeholders so they can be remapped once we
    know the full set; dynamic refs (c(a0 + 3)) force the uncompacted layout.
    """
    static, dynamic, out, pos = set(), False, [], 0
    pat = re.compile(r"(?<![A-Za-z0-9_])c\(")
    while True:
        m = pat.search(body, pos)
        if not m:
            out.append(body[pos:])
            return "".join(out), static, dynamic
        i = m.end() - 1
        depth, j = 0, i
        while True:
            if body[j] == "(":
                depth += 1
            elif body[j] == ")":
                depth -= 1
                if depth == 0:
                    break
            j += 1
        inner = body[i + 1:j].strip()
        out.append(body[pos:m.start()])
        if inner.isdigit():
            static.add(int(inner))
            out.append(f"__C__{inner}__")
        else:
            dynamic = True
            out.append(f"c[{inner}]")
        pos = j + 1


def rewrite_select(body):
    """select(cond, a, b) -> (cond ? a : b), innermost-first."""
    while True:
        m = re.search(r"(?<![A-Za-z0-9_])select\(", body)
        if not m:
            return body
        i = m.end() - 1
        depth, j = 0, i
        while True:
            if body[j] == "(":
                depth += 1
            elif body[j] == ")":
                depth -= 1
                if depth == 0:
                    break
            j += 1
        args = split_args(body[i + 1:j])
        if len(args) != 3:
            raise ValueError("select() with %d args" % len(args))
        body = body[:m.start()] + f"(({args[0].strip()}) ? ({args[1].strip()}) : ({args[2].strip()}))" + body[j + 1:]


def parse_main(text):
    i = text.index("void main(")
    j = text.index("(", i)
    depth, k = 0, j
    while True:
        if text[k] == "(":
            depth += 1
        elif text[k] == ")":
            depth -= 1
            if depth == 0:
                break
        k += 1
    sig = text[j + 1:k]
    brace = text.index("{", k)
    body = text[brace + 1:text.rindex("}")]
    return sig, body


def build(text, is_ps):
    sig, body = parse_main(text)

    # Drop the SPIR-V arm of the iFace #ifdef, and the vk::location attributes.
    sig = re.sub(r"#ifdef __spirv__.*?#else(.*?)#endif", r"\1", sig, flags=re.S)
    sig = re.sub(r"\[\[vk::[^\]]*\]\]", "", sig)

    params = []
    for line in sig.split(","):
        m = SIG_PARAM.match(" " + line.strip())
        if m:
            params.append({"dir": m.group(1), "type": m.group(2),
                           "name": m.group(3), "sem": m.group(4)})

    # XenosRecomp conservatively seeds r0..r15 from all 16 Xenos interpolators.
    # No vertex shader in the corpus writes more than 10, so anything at or above
    # v10 is undefined on the guest too -- and ps_3_0 only has v0..v9. Zero them.
    if is_ps:
        body = re.sub(r"^(\s*float4 r(\d+) = i(?:TexCoord|Color)(\d+);)$",
                      lambda m: (m.group(1) if int(m.group(3)) < PS_MAX_INTERPOLATORS
                                 else f"\tfloat4 r{m.group(2)} = 0.0;"),
                      body, flags=re.M)

    # Collect sampler usage *before* the tfetch rewrite erases the descriptor names.
    smp, kinds = sampler_decls(body, "ps" if is_ps else "vs")

    body = rewrite_tfetch(body)

    # --- constants: compact into c[0..K) -------------------------------------
    body, static_idx, dynamic = rewrite_c(body)
    if dynamic:
        # Relative addressing (c[a0 + n]); the uncompacted file is required, and
        # these shaders really do reference c255 -- so all 256 vs_3_0 float
        # registers are spoken for and there is nowhere to put g_HalfPixelOffset.
        # Drop it here; the host must fold the half-pixel bias into the projection
        # constants at upload time for these shaders. VS only, 12 of 565.
        n_const = 256
        const_decl = f"float4 c[{n_const}] : register(c0);"
        body = re.sub(r"__C__(\d+)__", r"c[\1]", body)
        body = re.sub(r"^\s*oPos\.xy \+= g_HalfPixelOffset \* oPos\.w;\s*$", "", body, flags=re.M)
        remap = None
    else:
        used = sorted(static_idx)
        slot = {orig: i for i, orig in enumerate(used)}
        n_const = max(len(used), 1)
        const_decl = f"float4 c[{n_const}] : register(c0);"
        body = re.sub(r"__C__(\d+)__", lambda m: f"c[{slot[int(m.group(1))]}]", body)
        remap = used

    # Shared uniforms sit immediately above the constant window. A VS never needs
    # the alpha threshold and a PS never needs the half-pixel offset, so each
    # stage only pays for what it uses.
    need_dims = "g_InvTexDim" in body
    u = []
    if is_ps:
        if "g_AlphaThreshold" in body:
            u.append(f"float g_AlphaThreshold : register(c{n_const});")
        if need_dims:
            u.append(f"float4 g_InvTexDim[16] : register(c{n_const + 1});")
    else:
        if "g_HalfPixelOffset" in body:
            u.append(f"float2 g_HalfPixelOffset : register(c{n_const});")
    uniforms = "\n".join(u)

    # --- signature ------------------------------------------------------------
    ins, outs = [], []
    for p in params:
        used_in_body = re.search(r"\b" + p["name"] + r"\b", body) is not None
        if p["dir"] == "in":
            if not used_in_body:
                continue
            sem, ty = p["sem"], "float4"
            if sem == "SV_Position":
                if not is_ps:
                    continue
                sem, ty = "VPOS", "float2"
            elif sem == "SV_IsFrontFace":
                sem, ty = "VFACE", "float"
            ins.append(f"in {ty} {p['name']} : {sem}")
        else:
            sem = p["sem"]
            forced = False
            if sem == "SV_Position":
                sem, forced = "POSITION", True
            elif sem.startswith("SV_Target"):
                sem, forced = "COLOR" + (sem[9:] or "0"), True
            elif sem == "SV_Depth":
                sem, forced = "DEPTH", True
            outs.append({"name": p["name"], "sem": sem, "forced": forced})

    # An output is dead if every assignment to it (bare or swizzled) writes zero
    # and it is never read. Drop those: SM3 has far fewer varying registers.
    live = []
    for o in outs:
        n = re.escape(o["name"])
        if o["forced"]:
            live.append(o)
            continue
        zero_write = re.compile(r"^\s*" + n + r"(\.[xyzw]+)?\s*=\s*0\.0;\s*$", re.M)
        stripped = zero_write.sub("", body)
        if re.search(r"(?<![A-Za-z0-9_])" + n + r"(?![A-Za-z0-9_])", stripped):
            live.append(o)  # genuinely used
        else:
            body = stripped

    outs = [f"out float4 {o['name']} : {o['sem']}" for o in live]

    # XenosRecomp declares all 64 Xenos GPRs. ps_3_0 has 32 temps, so drop the
    # declarations for registers the body never touches and let fxc allocate.
    for k in range(64):
        decl = re.compile(r"^\s*float4 r%d = 0\.0;\s*$" % k, re.M)
        if not decl.search(body):
            continue
        rest = decl.sub("", body)
        if not re.search(r"(?<![A-Za-z0-9_])r%d(?![0-9])" % k, rest):
            body = rest

    parts = [PRELUDE, const_decl, uniforms, smp,
             "void main(\n\t" + ",\n\t".join(ins + outs) + ")\n{" + body + "}\n"]
    return "\n".join(parts), remap


def compile_one(path, keep_dir=None):
    is_ps = ".frag." in path
    text = open(path, errors="ignore").read()
    try:
        src, remap = build(text, is_ps)
    except Exception as e:
        return False, f"REWRITE: {type(e).__name__}: {e}"

    with tempfile.NamedTemporaryFile("w", suffix=".hlsl", delete=False) as f:
        f.write(src)
        tmp = f.name
    if keep_dir:
        open(os.path.join(keep_dir, os.path.basename(path) + ".sm3.hlsl"), "w").write(src)
    try:
        r = subprocess.run([FXC, "/nologo", "/T", "ps_3_0" if is_ps else "vs_3_0",
                            "/E", "main", "/Fo", "NUL", tmp],
                           capture_output=True, text=True, timeout=60)
        if r.returncode == 0:
            return True, ""
        err = (r.stdout + r.stderr).strip().split("\n")
        msg = next((l for l in err if "error" in l.lower()), err[-1] if err else "?")
        return False, "FXC: " + re.sub(r"^.*?error ", "error ", msg)[:150]
    except subprocess.TimeoutExpired:
        return False, "FXC: timeout"
    finally:
        os.unlink(tmp)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("files", nargs="+")
    ap.add_argument("--keep", help="directory to write rewritten HLSL into")
    ap.add_argument("-v", action="store_true")
    a = ap.parse_args()
    if a.keep:
        os.makedirs(a.keep, exist_ok=True)

    ok = 0
    fails = {}
    for i, p in enumerate(a.files):
        good, msg = compile_one(p, a.keep)
        if good:
            ok += 1
        else:
            fails.setdefault(msg, []).append(os.path.basename(p))
            if a.v:
                print(f"FAIL {os.path.basename(p)}: {msg}", file=sys.stderr)
        if (i + 1) % 200 == 0:
            print(f"  ...{i+1}/{len(a.files)}  ok={ok}", file=sys.stderr)

    n = len(a.files)
    print(f"\ncompiled {ok}/{n} ({100.0*ok/n:.1f}%)")
    if fails:
        print("\ntop failure classes:")
        for msg, files in sorted(fails.items(), key=lambda kv: -len(kv[1]))[:12]:
            print(f"  {len(files):5d}  {msg}")
            print(f"         e.g. {files[0]}")


if __name__ == "__main__":
    main()
