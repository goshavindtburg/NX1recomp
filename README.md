# NX1recomp

A static recompilation of **NX1**, an Xbox 360 title built on an IW4-family engine, into
native Windows executables. Rather than emulating the console, the game's PowerPC code is
translated ahead of time into C++ and compiled into a real binary; its shaders and graphics
calls are likewise translated to run on a real GPU driver.

Two targets are built:

| Target | What it is |
| --- | --- |
| `nx1_sp` | Single-player. Runs on the ReXGlue graphics backend, plus a developer overlay. |
| `nx1_mp` | Multiplayer. Uses a purpose-built **native D3D9 renderer** and has working netplay. D3D9 Renderer is only enabled if nx1_d3d9 is enabled in the TOML :D |

Current release: **v0.9** — see [CHANGELOG.md](CHANGELOG.md).

---

## ⚠️ No game code or assets are in this repository

This repo contains **only** the recompilation harness: build configs, hand-written runtime
code, and tooling. It deliberately excludes anything derived from the game itself.

Not included, and gitignored so they cannot be committed by accident:

- `nx1/` — the game files (`.xex`, assets). You must supply your own legally-obtained copy.
- `generated/` — the C++ that ReXGlue emits from the game's PowerPC code.
- Shader dumps, translated HLSL, and the baked shader caches under `tools/`.

Building this project requires a game dump you already own. None is provided here.

---

## How it fits together

The pipeline has two halves — code and shaders.

**Code.** [ReXGlue](https://github.com/rexglue/rexglue-sdk) reads the game's `.xex` and emits
C++ source for every PowerPC function, which is then compiled and linked against the SDK's
runtime (its kernel, XAM, filesystem, and input layers, which are rooted in
[Xenia](https://github.com/xenia-project)). The per-target `*_manifest.toml` files drive
this: they name the entry-point `.xex`, where generated code lands, and which hook and
function-map configs to apply.

**Shaders.** The game ships Xenos GPU bytecode, which no PC driver understands.
[XenosRecomp](https://github.com/hedge-dev/XenosRecomp) converts it to HLSL; that HLSL is then
compiled either to DXIL (for the D3D12 path) or, for `nx1_mp`, rewritten to Shader Model 3
and compiled with `fxc` for the D3D9 path. The results are baked into a compressed cache the
runtime decompresses at startup.

### The `nx1_mp` native D3D9 renderer

The engine (`gfx_d3d`, per the shipped debug strings) targets what is effectively D3D9 plus
Xenos extensions. So instead of emulating the GPU's PM4 command stream, `nx1_mp` intercepts
the guest's D3D calls and drives a real `IDirect3DDevice9`.

The guest's D3D *setters* are left unhooked — they only write shadow state, emit no commands,
and several are inlined into engine code where they could not be hooked anyway. Instead the
*flush points* are hooked (`Draw*`, `Clear`, `Resolve`, `Swap`, plus constant-buffer and
resource-registration entry points), and at each draw the full state is read out of the guest
device and translated to D3D9. See `nx1_mp/src/d3d9/` — `d3d9_renderer.h` documents the
strategy in detail.

---

## Layout

```
nx1_sp/           Single-player target: CMake, manifests, hook configs, runtime source
nx1_mp/           Multiplayer target: same, plus src/d3d9/ (the native D3D9 renderer)
rexglue-sdk/      ReXGlue SDK — vendored, with NX1-specific changes (see below)
XenosRecomp/      Xenos shader → HLSL recompiler — vendored, with NX1-specific changes
tools/            Dev scripts (see below)
res/              Icon
```

`rexglue-sdk` and `XenosRecomp` are **modified forks**, not stock checkouts. Local work in
them includes the netplay stack (XLive web client, session/system-link handling), the native
shader cache, Discord presence, a debug HUD, and mouse-look input on the SDK side; and the
Shader Model 3 transform plus an `fxc` compiler backend on the XenosRecomp side.

### Tools

| Script | Purpose |
| --- | --- |
| `Start-Nx1MpDevmap.ps1` / `Start-Nx1SpDevmap.ps1` | Launch a build straight into a map with dev flags |
| `nx1_xenosrecomp_decompile.ps1` | Batch-run XenosRecomp over the shader dumps to produce HLSL |
| `sm3_feasibility/sm3_rewrite.py` | Rewrites XenosRecomp's HLSL to Shader Model 3 and compiles it with `fxc` — originally written to measure how much of the shader corpus a D3D9 backend could take |
| `Convert-MsvcMapToRexFunctions.ps1` | Turns an MSVC `.map` into a ReXGlue function table |

---

## Building

Requires CMake, a recent MSVC toolchain, and (for XenosRecomp) Clang. Each target is a
standalone CMake project with presets; builds land in `out/build/win-amd64-release/`.

**Before you can build, you must restore `thirdparty/`.** Those trees are gitignored, so a
fresh clone does not have them. They are the upstream submodules of `rexglue-sdk` and
`XenosRecomp`, and each project's own committed `.gitmodules` records the exact URLs and
commits — so they can be recovered, but they are not fetched for you.

You also need a game dump in `nx1/` and must run ReXGlue to produce `generated/`, since
neither is distributed here.

You will also need the Discord Social SDK for rich presense support.

---

## Credits

Built on the work of others:

- **[ReXGlue](https://github.com/rexglue/rexglue-sdk)** — the recompiler and runtime SDK.
- **[XenosRecomp](https://github.com/hedge-dev/XenosRecomp)** (hedge-dev) — Xenos shader recompilation.
- **[Xenia](https://github.com/xenia-project)** — the foundation both of the above build on.

Their respective licenses apply to the vendored copies in `rexglue-sdk/` and `XenosRecomp/`.
