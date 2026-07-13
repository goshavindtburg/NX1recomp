# The NX1 Guest D3D Pipeline

A complete reverse-engineering of how NX1 talks to the Xbox 360 GPU, and what a native
D3D9 renderer must do to stand in for it.

Everything here was recovered from `5-nx1mp_demo.xex` (PowerPC 64, big-endian) with full
PDB symbols. Addresses are guest virtual addresses in the XEX image. Struct offsets are
hex byte offsets. Where the disassembly did not settle a question, it says so.

---

## 0. The shape of the thing

NX1 is an IW4-family Call of Duty engine (the shipped source paths say `src/gfx_d3d/...`).
It renders through **three** layers:

```
  R_* / RB_*            the engine renderer (gfx_d3d)     — draw lists, passes, post
      |
  D3DDevice_*           the Xenon D3D library, STATICALLY LINKED into the XEX
      |
  PM4 command ring      packets consumed by the Xenos GPU
```

The middle layer is the important one, and it is not a driver in the PC sense. **Almost
every `D3DDevice_*` entry point is a pure shadow-state writer.** It stores into a
`D3D::CDevice` struct and sets a dirty bit. Nothing reaches the GPU until a *flush point* —
a draw, a clear, a resolve, or a swap — at which point the accumulated shadow state is
converted to PM4 packets and appended to a ring buffer.

That is what makes a native renderer possible at all: we let the guest setters run
untouched, hook only the flush points, and reconstruct GPU state by reading `CDevice` at
draw time.

### 0.1 The complete list of functions that emit GPU work

Established by taking xrefs to the two ring-reservation helpers (`KickOff` and
`BeginRingBig`) and closing over the callers. **Anything not on this list writes shadow
state and nothing else.**

| Category | Functions |
|---|---|
| Draw | `D3DDevice_DrawVertices` (0x820EA628), `D3DDevice_DrawIndexedVertices` (0x820EAA40), `D3DDevice_BeginVertices` (0x820E9B40), `D3DDevice_BeginIndexedVertices` (0x820EA048) |
| State flush | `D3D::SetPending_{AluConstants, Shaders, Predicated, RenderStates, FetchConstants, ClipPlanes, HiZEnable, Split}` — only ever called *from* the draw functions |
| Shader load | `D3D::SetLiteralShaderConstants` (0x820D9808), `D3D::IncrementalShaderPatchAndLoad` |
| Resolve / clear | `D3DDevice_ResolveEx` (0x820E8850), `D3D::ClearF` (0x820E6860), `ClearSurface` (0x820E62C0), `ClearRects` (0x820E5DB0) |
| Present | `D3DDevice_Swap` (0x820E78E8) and its scaler/gamma helpers |
| Scissor | **`D3D::SetSurfaceClip` (0x820E2930)** — emits PM4 *directly*, is not shadowed |
| Constants fast path | **`D3DDevice_GpuBeginShaderConstantF4` (0x820EAEB8)** — see §6.2 |
| Sync / query / misc | `InsertFence`, `InsertCallback`, `BlockUntilIdle`, `SetPredication`, `InvalidateCaches`, `BeginCommandBuffer`, `RunCommandBuffer`, `D3DQuery_*`, `BeginExport`/`EndExport` |

Pure shadow, zero GPU work — **do not hook these, just read what they wrote**:
every `SetRenderState_*`, every `SetSamplerState_*`, `SetTexture`, `SetStreamSource`,
`SetIndices`, `SetVertexShader`, `SetPixelShader`, `SetVertexDeclaration`, `SetViewport`,
`SetVertexShaderConstantF`, `SetPixelShaderConstantF`, `SetRenderTarget`,
`SetDepthStencilSurface`.

This is the single most important structural fact in the document, and it validates the
whole interception design.

---

## 1. Memory: address windows and the one formula

Get this wrong and everything downstream reads garbage or faults. There is exactly one
translation formula, and it appears verbatim at ~20 sites in the D3D library:

```c
gpu_phys = (((ea >> 20) + 512) & 0x1000) + (ea & 0x1FFFFFFF);
```

`((ea >> 20) + 0x200) & 0x1000` is a branchless way of writing `ea >= 0xE0000000 ? 0x1000 : 0`.
So:

> **`gpu_phys = (ea & 0x1FFFFFFF) + (ea >= 0xE0000000 ? 0x1000 : 0)`**

### 1.1 The windows

Physical RAM is 512 MB (`0x00000000`–`0x1FFFFFFF`) and is aliased through several virtual
windows that differ only in page size and cacheability:

| Window | Page size | Cacheability | Physical mapping |
|---|---|---|---|
| `0x00000000`–`0x3FFFFFFF` | 4 KB | cached | **plain virtual heap — NOT a physical alias** |
| `0x40000000`–`0x7EFFFFFF` | 64 KB | cached | plain virtual heap |
| `0x7FC80000` | — | GPU MMIO aperture (`CP_RB_WPTR` at `0x7FC80714`) | — |
| `0x80000000`–`0x9FFFFFFF` | — | the XEX image | — |
| `0xA0000000`–`0xBFFFFFFF` | 64 KB | cached | `phys = ea & 0x1FFFFFFF` |
| `0xC0000000`–`0xDFFFFFFF` | 16 MB | write-combined | `phys = ea & 0x1FFFFFFF` |
| `0xE0000000`–`0xFFCFFFFF` | 4 KB | uncached | `phys = (ea & 0x1FFFFFFF) + 0x1000` |

The `+0x1000` on the `0xE0` window is a **real property of the console's address map**, not
a host artifact. The guest applies it itself. `rexglue` mirrors it by mapping the
`0xE0000000` view at physical base + `0x1000` (and, on Windows, by carrying a
`host_address_offset_` of `0x1000` that `TranslateVirtual` folds in).

Which allocator lands where:
- `XPhysicalAlloc` → `MmAllocatePhysicalMemoryEx`, default **4 KB pages** → the `0xE0` window.
  This is why `D3D::CDevice` lives at `0xE47014D0` and the front buffers at `0xE3E02000` /
  `0xE360A000`.
- `XMemAlloc` with `X_MEM_LARGE_PAGES` (64 KB) → the `0xA0` window. This is why the
  resolve-destination textures sit at `0xBF6D8000`, `0xBE2A0000`, `0xBDEA0000`.

### 1.2 The two address classes — and the trap

**Class A — guest EA.** Object headers and pointers: the `CDevice` itself, `D3DBaseTexture`,
`D3DSurface`, `D3DVertexBuffer`, `D3DIndexBuffer`, `CVertexShader`, `CPixelShader`,
`CVertexDeclaration`, and *the address fields stored inside a texture header*
(`tex->Format.dword[1]`, `dword[5]`). Read with `TranslateVirtual`.

**Class B — GPU physical.** Always `< 0x20000000`. Everything the GPU consumes: every
address in a **device fetch constant**, the index-buffer address in a `DRAW_INDX` packet,
shader microcode addresses, `RB_COPY_DEST_BASE`, all PM4 payloads. Read with
`TranslatePhysical` — **no page offset, ever**; the guest already baked it in when it
converted the EA.

> The trap: a texture's *header* holds `0xBF6D8000` (class A), but the moment `SetTexture`
> copies that header into the device's sampler fetch constant, it becomes `0x1F6D8000`
> (class B). **Same page, two encodings.** Resolve destinations and texture samplers
> therefore never appear to match unless you canonicalise both to physical.

A safe discriminator: `addr < 0x20000000` ⇒ physical; otherwise ⇒ EA. There is no
legitimate guest EA below `0x20000000` in this title's D3D data.

### 1.3 The PM4 ring

There are **two** buffers:

- **Primary** (`m_pPrimaryRingBuffer` @ CDevice+0x3B44, default 32 KB) — the actual hardware
  ring. It contains *only* `INDIRECT_BUFFER` packets. Registered with
  `VdInitializeRingBuffer`; the GPU read pointer is written back into
  `m_pWriteBacks->PrimaryRingBufferReadIndex`.
- **Secondary** (`m_pSecondaryRingBuffer` @ 0x3B4C, default 2 MB, cut into 32 segments) —
  where all real PM4 is written. `m_pRing` walks a segment of this.

| Offset | Field | Note |
|---|---|---|
| 0x0030 | `m_pRing` | **points at the LAST written dword**, not the next slot |
| 0x0034 | `m_pRingLimit` | end of the current segment |
| 0x0038 | `m_pRingGuarantee` | `m_pRingLimit - 40` — the reserve watermark |

The emit idiom, inlined everywhere:

```c
p = m_pRing;
if (p > m_pRingGuarantee) p = KickOff(dev);
*++p = header; *++p = data; ...
m_pRing = p;
```

Because the guarantee is a constant 40 dwords, **any packet ≤ 40 dwords needs no size
check**. Larger packets go through `BeginRingBig` (0x820DD5B0); bulk *data* (resolve
vertices, immediate constants) is bump-allocated downward from the segment top by
`BeginRingAlloc` (0x820DDBA0).

Submission (`AddCallsToPrimaryBuffer`, 0x820DE0C8) writes `INDIRECT_BUFFER` triplets into
the primary ring and publishes the write pointer with a raw MMIO store:

```c
MEMORY[0x7FC80714] = idx;   // CP_RB_WPTR, a dword index
```

---

## 2. `D3D::CDevice` — the shadow struct

`D3D::CDevice` is **24448 bytes (0x5F80)**. `D3DDevice` (**11008 = 0x2B00**) is embedded at
offset 0; the engine's own fields follow it. Both types are in the PDB.

### 2.1 `D3DDevice` (0x0000 – 0x2B00)

```
0x0000  _D3DTAGCOLLECTION m_Pending          // uint64 m_Mask[5]  — the dirty bits
0x0028  uint64  m_Predicated_PendingMask2
0x0030  uint32* m_pRing
0x0034  uint32* m_pRingLimit
0x0038  uint32* m_pRingGuarantee
0x003C  uint32  m_ReferenceCount
0x0040  m_SetRenderStateCall[101]            // fn-ptr table; SetRenderState indexes THIS
0x01D4  m_SetSamplerStateCall[20]
0x0224  m_GetRenderStateCall[101]
0x03B8  m_GetSamplerStateCall[20]
0x0480  _D3DConstants m_Constants  (9120 B)  // see below
0x2820  float m_ClipPlanes[6][4]
0x2880  GPU_DESTINATIONPACKET m_DestinationPacket (64)
0x28C0  GPU_WINDOWPACKET      m_WindowPacket      (12)
0x28CC  GPU_VALUESPACKET      m_ValuesPacket      (84)
0x2920  GPU_PROGRAMPACKET     m_ProgramPacket     (20)
0x2934  GPU_CONTROLPACKET     m_ControlPacket     (48)
0x2964  GPU_TESSELLATORPACKET m_TessellatorPacket (84)
0x29B8  GPU_MISCPACKET        m_MiscPacket        (152)
0x2A50  GPU_POINTPACKET       m_PointPacket       (32)
0x2A70  uint8 m_MaxAnisotropy[26]
0x2A8A  uint8 m_ZFilter[26]
```

The `GPU_*PACKET` blocks are **verbatim Xenos register shadows**. The Xbox 360 D3D enums
*are* the hardware encodings — no translation happens anywhere in the library. Each packet
maps to a contiguous GPU register range and is flushed as a PM4 type-0 run.

### 2.2 `_D3DConstants` (device + 0x0480)

```
+0x0000  GPUFETCH_CONSTANT Fetch[32]      768 B   // union, see §7.1
+0x0300  __vector4         Alu[512]      8192 B   // VS c0..255 then PS c0..255
+0x2300  uint32            Flow[40]       160 B   // bool + loop constants
```

Absolute addresses that matter:

| What | Device offset |
|---|---|
| Fetch constant *N* (24 B stride) | `0x0480 + 24*N` |
| VS float constants `c0..c255` | **0x0780** |
| PS float constants `c0..c255` | **0x1780** |
| Bool / loop constants | 0x2780 |

Constants are stored as **raw big-endian IEEE-754** — the setter is a verbatim `lvx/stvx`
vector copy, no swizzling.

### 2.3 `D3D::CDevice` tail (past 0x2B00) — what a renderer needs

```
0x2B3C  bit0 m_PredicateDrawPrims · bit4 m_TilingBracket · bit5 m_TilingSurfaceOverride
0x2F58  CVertexDeclaration* m_pVertexDeclaration
0x2F68  uint8  m_CurrentVertexShaderStride[16]   // strides patched into the VS's vfetch
0x2F78  GPU_BLENDCONTROL m_StatePacket.BlendControl  // the app's blend, pre-collapse
0x2F7C  bit30 SeparateAlphaBlendEnable · bit31 AlphaBlendEnable
0x2F80  int    m_ScissorTestEnable
0x2F84  int    m_ColorEnable[4]
0x2F94  int    m_ZEnable
0x2FC4  uint8  m_MaxMipLevel[26]
0x2FDE  uint8  m_MinMipLevel[26]
0x318C  D3DIndexBuffer*  m_pIndexBuffer
0x3190  D3DSurface*      m_pRenderTarget[4]        // <-- the bound colour targets
0x31A0  D3DSurface*      m_pDepthStencilSurface    // <-- the bound depth target
0x31A4  D3DVertexBuffer* m_pVertexBuffer[16]
0x31E8  uint8            m_StreamStride[16]        // StrideInBytes >> 2, one byte each
0x31F8  D3DBaseTexture*  m_Textures[26]
0x3260  _D3DVIEWPORTF9   m_Viewport                // float X,Y,W,H,MinZ,MaxZ; uint Flags
0x327C  RECT             m_Scissor
0x328C  CPixelShader*    m_pPixelShader
0x3290  CVertexShader*   m_pVertexShader
0x32A4  uint32           m_GpuPacket3Predication   // OR'd into every type-3 header
0x3B10  m_FrontBuffer                              // copy of the last Swap texture
```

---

## 3. The dirty-bit machine

`m_Pending.m_Mask[5]` (five `uint64`, at device + 0x00/0x08/0x10/0x18/0x20). Setters OR in a
bit; the draw/clear/resolve path walks the bits with `cntlzd`, emits PM4, and zeroes the
word.

`D3D::SetPending_RenderStates(dev, mask, regBase, pData)` (0x820D86A0) walks the mask
**MSB-first**, emitting type-0 runs: `((runlen-1) << 16) | (regBase + dwordIndex)` followed
by `runlen` dwords copied straight out of the shadow. **Within a group, the highest tag bit
is dword index 0.**

| Mask | Group | Shadow | GPU reg base |
|---|---|---|---|
| `m_Mask[0]` | VS float constants | 0x0780 | 0x4000 |
| `m_Mask[1]` | PS float constants | 0x1780 | 0x4400 |
| `m_Mask[2]` bits 0–11 | ControlPacket | 0x2934 | 0x2200 |
| `m_Mask[2]` bits 12–16 | ProgramPacket | 0x2920 | 0x2180 |
| `m_Mask[2]` bits 17–20 | shader / HiZ / alpha re-evaluation | — | — |
| `m_Mask[2]` bits 21–41 | ValuesPacket | 0x28CC | 0x2100 |
| `m_Mask[2]` bits 42–57 | DestinationPacket | 0x2880 | 0x2000 |
| `m_Mask[3]` bits 0–31 | **fetch constants**, index = `31 − bit` | 0x0480 | `0x4800 + 6*N` |
| `m_Mask[3]` bits 34–54 | TessellatorPacket | 0x2964 | 0x2280 |
| `m_Mask[4]` bits 0–37 | MiscPacket | 0x29B8 | 0x2300 |
| `m_Mask[4]` bits 38–45 | PointPacket | 0x2A50 | 0x2380 |
| `m_Mask[4]` bits 49–54 | clip planes | 0x2820 | — |
| `m_Mask[4]` bit 56 | bool/loop constants | 0x2780 | 0x4900 |
| `m_Mask[4]` bits 62–63 | force `WaitUntilIdleOrFlushCaches` | — | — |

A native renderer does not need to replay this — it reads the shadows directly. But it
*does* need to know the mask exists, because **two code paths deliberately clear bits to
suppress the flush** (§6.2, §6.3), and those are exactly the registers that end up stale.

---

## 4. Render state — the registers you actually need

All bitfields are LSB-first within each big-endian dword.

### `GPU_CONTROLPACKET` @ 0x2934 → reg base 0x2200

| Offset | Register | Fields |
|---|---|---|
| 0x2934 | `RB_DEPTHCONTROL` | `StencilEnable`@0, **`ZEnable`@1**, **`ZWriteEnable`@2**, **`ZFunc`:3@4**, `BackFaceEnable`@7, `StencilFunc`:3@8, `StencilFail`:3@11, `StencilZPass`:3@14, `StencilZFail`:3@17, …BF variants @20/23/26/29 |
| 0x2938 | `RB_BLENDCONTROL0` | `ColorSrcBlend`:5@0, `ColorBlendOp`:3@5, `ColorDestBlend`:5@8, `AlphaSrcBlend`:5@16, `AlphaBlendOp`:3@21, `AlphaDestBlend`:5@24 |
| 0x293C | `RB_COLORCONTROL` | **`AlphaFunc`:3@0**, **`AlphaTestEnable`@3**, `AlphaToMaskEnable`@4, `AlphaToMaskOffset0..3`:2 @24/26/28/30 |
| 0x2940 | `HiControl` | HiZ/HiStencil |
| 0x2944 | `PA_CL_CLIP_CNTL` | `ClipPlaneEnable0..5`@0–5, `ClipPlaneMode`:2@14, **`ClipDisable`@16**, `BoundaryEdgeFlagEnable`@18, **`DxClipSpaceDef`@19**, `VtxKillOr`@21 |
| 0x2948 | `PA_SU_SC_MODE_CNTL` | **`CullMode`:3@0**, **`PolyMode`:2@3**, `PolyModeFrontPType`:3@5, `PolyModeBackPType`:3@8, `PolyOffsetFrontEnable`@11, `PolyOffsetBackEnable`@12, `MsaaEnable`@15, `VtxWindowOffsetEnable`@16, `ProvokingVtxLast`@19 |
| 0x294C | `PA_CL_VTE_CNTL` | `Vport{X,Y,Z}{Scale,Offset}Enable`@0–5, `VtxXyFmt`@8, `VtxZFmt`@9, `VtxW0Fmt`@10 |
| 0x2954 | `RB_MODECONTROL` | EDRAM mode (5 = depth-only) |
| 0x2958/5C/60 | `RB_BLENDCONTROL1/2/3` | **always identical to BlendControl0** |

### `GPU_VALUESPACKET` @ 0x28CC → reg base 0x2100

```
0x28DC  ColorMask     Write0:4@0  Write1:4@4  Write2:4@8  Write3:4@12
0x28E0  BlendRed   0x28E4 BlendGreen  0x28E8 BlendBlue  0x28EC BlendAlpha   (floats)
0x2900  StencilRefMask     0x28FC StencilRefMaskBF
0x2904  AlphaRef      (float, already divided by 255)
0x2908  VportXScale   0x290C VportXOffset  0x2910 VportYScale
0x2914  VportYOffset  0x2918 VportZScale   0x291C VportZOffset
```

### `GPU_DESTINATIONPACKET` @ 0x2880 → reg base 0x2000

```
0x2880  SurfaceInfo   SurfacePitch:14@0 (pixels) · MsaaSamples:2@16 · HiZPitch:14@18
0x2884  Color0Info    0x2888 DepthInfo   0x288C/90/94 Color1/2/3Info
0x28B8  ScreenScissorTL   0x28BC ScreenScissorBR
```

### `GPU_WINDOWPACKET` @ 0x28C0 → reg base 0x2080

`WindowOffset`, `WindowScissorTL` (0x28C4), `WindowScissorBR` (0x28C8). **These are not
tagged** — `D3D::SetSurfaceClip` (0x820E2930) writes the PM4 straight into the ring at
`SetScissorRect` time. The shadows are still correct to read; just know they can be pushed
out of band.

### `GPU_POINTPACKET` @ 0x2A50 → reg base 0x2380

`PolyOffsetFrontScale`/`Offset` @ 0x2A50/0x2A54, back @ 0x2A58/0x2A5C. **Slope-scale is
already multiplied by 16.0 by the guest — do not scale it again.**

### Notable setter behaviour

- **Blend is indirect.** `SetRenderState_SrcBlend` etc. write only into
  `m_StatePacket.BlendControl` (0x2F78). Whichever setter runs last recomputes
  `BlendControl0..3` — collapsing separate-alpha when disabled, and forcing `0x00010001`
  (ONE/ZERO/ADD) when blending is off. **The reliable "is blending on" signal is bit 31 of
  0x2F7C**, not the register value.
- **Cull.** `SetRenderState_CullMode` stores `Value & 7` verbatim into `ModeControl[2:0]`.
  NX1's `s_cullTable` (0x82090E28) contains only `{0, 0, 6, 2}` — the engine **never sets
  cull-front**; it expresses front/back by flipping the *face* bit. (This is why
  "correcting" the winding globally breaks the UI.)
- **Colour write mask** is gated: `ColorMask` nibble *N* is forced to 0 when
  `m_pRenderTarget[N]` is NULL.
- **Clip disable** comes only from `SetRenderState_ViewportEnable`: `FALSE` ⇒
  `VteControl = 0x400` (all scale/offset enables cleared, `VtxW0Fmt` set) **and**
  `ClipControl.ClipDisable` = 1. That is the screen-space path.

### Viewport

`D3DDevice_SetViewport` (0x820E5290) → `D3D::SetViewport` (0x820E2C20):

1. Clamp `X/Y/W/H` to the bound render target's extent, read from **`surface + 0x24`**.
2. Store the clamped rect in `m_Viewport` (0x3260).
3. Re-derive and *immediately emit* the window scissor.
4. Write:
   ```
   VportZScale = MaxZ - MinZ    VportZOffset = MinZ
   VportXScale = W/2            VportXOffset = W/2 + X
   VportYScale = -(H/2)         VportYOffset = H/2 + Y
   ```

**`SetRenderTarget` resets the viewport.** `D3D::SetSurfaceInfo` (0x820E28A0), called from
`SetRenderTarget(0, …)`, resets the scissor to `0,0,65535,65535` and the viewport to
`D3D::g_DefaultViewport`. The engine *relies* on this: `R_HW_SetRenderTarget` (0x82526C58)
updates its own cached `state->viewport` to the new target's full extent **without emitting
a `SetViewport`**, and `R_SetViewport` then skips the D3D call because the cache already
matches. A host renderer that does not replicate the implicit reset will rasterise the next
pass through the *previous* pass's viewport. (§11.3.)

---

## 5. Textures and samplers

### 5.1 `D3DDevice_SetTexture` (0x820D6E48)

Sampler *N*'s fetch constant is at **`device + 0x480 + 24*N`**. `SetTexture` **merges** the
texture's own 6-dword header (at `D3DBaseTexture + 0x1C`) into the device's fetch constant,
*preserving the sampler-owned bits*:

| dword | Kept from the shadow (sampler state) | Taken from the texture |
|---|---|---|
| 0 | bits 10–21: **ClampX/Y/Z** | Type, Sign\*, Pitch, Tiled |
| 1 | bit 11: ClampPolicy | DataFormat, Endian, **BaseAddress (→ physical!)** |
| 2 | — | Size |
| 3 | bits 19–30: **Mag/Min/Mip/AnisoFilter** | NumFormat, **Swizzle**, ExpAdjust |
| 4 | AnisoWalk, LODBias, GradExpAdjust | MinMipLevel/MaxMipLevel (clamped per-sampler) |
| 5 | bits 0–8: BorderColor, TriClamp, AnisoBias | Dimension, PackedMips, **MipAddress (→ physical!)** |

**Consequence: the fetch constant at draw time is the complete, authoritative description
of both the bound texture and its sampler.** Nothing else is needed. Note `SetTexture(NULL)`
only clears the 2-bit `Type` field and does **not** tag the dirty mask.

### 5.2 `GPUTEXTURE_FETCH_CONSTANT` — full layout

```
d0: Type:2@0 · SignX/Y/Z/W:2 @2/4/6/8 · ClampX:3@10 · ClampY:3@13 · ClampZ:3@16
    · Pitch:9@22 (row pitch / 32 texels) · Tiled:1@31
d1: DataFormat:6@0 · Endian:2@6 · RequestSize:2@8 · Stacked:1@10 · ClampPolicy:1@11
    · BaseAddress:20@12 (page number; byte address = field << 12)
d2: Size — layout depends on Dimension:
      1D   : Width:24@0
      2D   : Width:13@0 · Height:13@13 · Depth:6@26
      3D   : Width:11@0 · Height:11@11 · Depth:10@22
      Cube : Width:13@0 · Height:13@13 · Depth:6@26
    each field stores (dim - 1 - 2*BorderSize)
d3: NumFormat:1@0 · SwizzleX/Y/Z/W:3 @1/4/7/10 · ExpAdjust:6s@13
    · MagFilter:2@19 · MinFilter:2@21 · MipFilter:2@23 · AnisoFilter:3@25 · BorderSize:1@31
d4: VolMagFilter@0 · VolMinFilter@1 · MinMipLevel:4@2 · MaxMipLevel:4@6
    · MagAnisoWalk@10 · MinAnisoWalk@11 · LODBias:10s@12 (5 fractional)
    · GradExpAdjustH:5s@22 · GradExpAdjustV:5s@27
d5: BorderColor:2@0 · ForceBCWToMax@2 · TriClamp:2@3 · AnisoBias:4s@5
    · Dimension:2@9 · PackedMips@11 · MipAddress:20@12
```

### 5.3 The `D3DFORMAT` is itself a packed descriptor

On Xbox 360 `D3DFORMAT` is **not an enum ordinal**. `D3D::SetTextureHeader` (0x820D6488)
unpacks it bit-for-bit into the fetch constant:

| D3DFORMAT bits | meaning |
|---|---|
| 0–5 | `DataFormat` (GPUTEXTUREFORMAT) |
| 6–7 | `Endian` |
| 8 | `Tiled` |
| 9–16 | `SignX/Y/Z/W` |
| 17 | `NumFormat` |
| 18–29 | `SwizzleX/Y/Z/W` |

**This is where the DXT3A/DXT5A "RRRR" swizzle comes from.** There is no special case
anywhere for single-channel formats — the swizzle is baked into the `D3DFMT_DXT5A`
constant itself (all four channels = `GPUSWIZZLE_X`) and copied into fetch `d3` bits 1–12.
A renderer gets RRRR for free by **honouring the fetch constant's swizzle field**; do not
hardcode a per-format swizzle table.

### 5.4 Sampler state is written *inline*

`R_HW_SetSamplerState` (0x82526670) calls the D3D setter only for MinFilter, MagFilter and
MaxAnisotropy. **Mip filter and all three address modes are written straight into the fetch
constant**, bypassing `D3DDevice_SetSamplerState_*` entirely:

```
mipFilter -> Fetch[N].d3 |= (s << 7)  & 0x01800000   // bits 23-24
addressU  -> Fetch[N].d0 |= (s >> 10) & 0x00000C00   // ClampX
addressV  -> Fetch[N].d0 |= (s >> 9)  & 0x00006000   // ClampY
addressW  -> Fetch[N].d0 |= (s >> 8)  & 0x00030000   // ClampZ
m_Mask[3] |= 1 << (31 - N);
```

Hooking the sampler setters would see nothing. Read the fetch constant.

---

## 6. Shaders and constants

### 6.1 Shader objects

```
D3D::CVertexShader (0x368 + microcode)        D3D::CPixelShader (40 + microcode)
  0x000 D3DVertexShader (D3DResource, 24)       0x00 D3DPixelShader (24)
  0x020 m_dwPhysical                            0x18 m_dwPhysical
  0x024 m_Uniqueness                            0x1c m_Uniqueness
  0x028 PassData m_Pass[2]  (416 each)          0x28 _UCODE_HEADER m_Function[]
  0x368 _UCODE_HEADER m_Function[]

_UCODE_HEADER (36):
  0x00 Cookie · 0x04 CachedSize · 0x08 PhysicalSize · 0x0C DebuggerHintOffset
  0x10 constantTableOffset
  0x14 _UCODE_PASS_HEADER Pass[2] { u32 definitionTableOffset; u32 microcodeOffset }
```

Microcode address resolution (identical for VS and PS bar the mask):

```c
raw  = *(u32*)(m_Function + Pass[pass].microcodeOffset) + m_dwPhysical;
size = *(u32*)(m_Function + Pass[pass].microcodeOffset + 4) >> 2;   // dwords
addr = (((raw >> 20) + 512) & 0x1000) + (raw & MASK);               // VS: 0x1FFFFFFF
                                                                    // PS: 0x1FFFFFFE, then |1
```

**The two passes.** `D3D::SetPending_Shaders` (0x820D9A88) selects pass 1 **only** when no
pixel shader is bound *and* the ucode cookie has bit `0x20`. Pass 1 is the **depth-only
variant** (it also sets `EdramModeControl` to 5). So "no PS bound" ⇒ depth/shadow pass ⇒
different microcode. A translator must key its cache on the pass, and a host renderer must
mask colour writes for those draws (D3D9's `SetPixelShader(NULL)` would otherwise engage
the fixed-function pipe and *write colour*).

**The microcode is patched in place.** `D3D::DirectShaderPatch` /
`IncrementalShaderPatchAndLoad` rewrite the `vfetch` instructions in the live microcode
buffer to match the bound vertex declaration, caching the originals in
`PassData::m_OriginalFetchInstructions[32]`. The bytes at `m_Function + microcodeOffset` at
draw time are **declaration-specific**, not the on-disk blob.

### 6.2 ⚠ `GpuBeginShaderConstantF4` — the shadow is NOT authoritative

`D3DDevice_GpuBeginShaderConstantF4` (0x820EAEB8) writes a PM4 `SET_CONSTANT` packet
**directly into the ring** and returns a raw ring pointer for the engine to fill:

```c
__vector4 *GpuBeginShaderConstantF4(CDevice *dev, int PixelShader,
                                    uint StartRegister, uint Vector4fCount)
{
  uint *r = BeginRingBig(dev, 4*Vector4fCount + 5);
  ...
  hdr[0] = dev->m_GpuPacket3Predication | ((Vector4fCount*4) << 16) | 0xC0002D00; // SET_CONSTANT
  hdr[1] = ((StartRegister - (PixelShader << 8)) & 0x1FF) << 2;   // ALU index, in dwords
  dev->m_pRing = (uint*)((char*)(hdr+2) + 16*Vector4fCount - 4);
  return (__vector4*)(hdr + 2);          // <-- raw ring pointer handed to the engine
}
```

**The data never touches `m_Constants`.** Worse, the callers *deliberately clear the
matching dirty bits* (an inlined `GpuOwnVertexShaderConstantF`) so the draw-time shadow
flush cannot overwrite what they wrote, then re-set them on the way out.

What it bypasses, at all 11 call sites:

| Registers | Written by | Content |
|---|---|---|
| **VS c4–c7** | every `GpuBegin(dev, 0, 4, 4)` | the per-instance **object→world matrix** |
| **VS c8–c11** | the Lit / Ambient paths | model lighting coords / ambient SH |

Callers: `R_DrawXModelUnlitNoPrepass` (0x8253AA98), `…LitNoPrepass` (0x8253B298),
`…AmbientNoPrepass` (0x8253BA28), `R_DrawStaticModelArray_{Unlit,Lit,Ambient}NoPrepass`
(0x8253C400 / 0x8253C628 / 0x8253C960), and friends.

Every call site passes `PixelShader = 0`, so **PS constants are never ring-written** —
`device + 0x1780` remains authoritative for pixel shaders.

`GpuEndShaderConstantF4` (0x8253A040) is literally a single `blr`. There is no commit step.

> **Consequence for a native renderer.** Reading VS c4–c7 out of the shadow at draw time
> yields whatever the last unrelated `SetVertexShaderConstantF` left there. Every XModel and
> static-model-array draw gets a **garbage world matrix**, projecting its geometry to
> nowhere, while UI, decal and other shadow-path draws render perfectly. Symptomatically:
> stretched triangles fanning from a vanishing point, islands of correct geometry, flicker.

### 6.3 The second bypass: shader literals

`D3D::SetLiteralShaderConstants` (0x820D9808), called from `SetPending_Shaders` on every
shader change, emits `LOAD_ALU_CONSTANT` (PM4 type-3, opcode 0x2F) to pull the shader's
literal float4s **from the shader blob straight into the GPU constant file**. And
`SetVertexShader` explicitly clears those register groups from the dirty mask:

```
820db1c4  ld   r9,  0(r11)   ; defTable[0] = uint64 group mask of literal-defined regs
820db1c8  andc r10, r10, r9  ; m_Mask[0] &= ~literalMask
```

So **any register a shader defines as a literal is stale-by-construction in the shadow.**

### 6.4 The correct model

Maintain a host-side **live ALU constant file** (512 × float4) and emulate both producers in
PM4 order. At draw time, per group of 4 registers:

- dirty bit **set** in `m_Mask[0]`/`[1]` → the shadow flush wins (it is emitted *after* the
  `SET_CONSTANT`); use the shadow.
- dirty bit **clear** and a ring record exists → read the float4s back from the recorded
  ring address; that is what the GPU holds.
- otherwise → the shadow (unchanged since the last flush).

Plus: replay `SetLiteralShaderConstants` on shader bind, or make the translator source
literals from `_UCODE_HEADER::constantTableOffset`.

Do **not** simply redirect the returned ring pointer into `m_Constants`: the engine's
`R_IsVertexShaderConstantUpToDate` redundancy cache assumes the shadow still holds what
`R_SetVertexShaderConstantFromCode` last put there, and relies on the `Disown` dirty-bit
re-set to have D3D re-flush that value after the model loop.

---

## 7. Vertex and index data

### 7.1 The fetch-constant union

```c
union GPUFETCH_CONSTANT {              // 24 bytes
    GPUTEXTURE_FETCH_CONSTANT Texture; // 6 dwords
    GPUVERTEX_FETCH_CONSTANT  Vertex[3];  // 3 × 2 dwords
};
```

32 fetch constants × 3 = **96 vertex slots**. There are **26 texture samplers**; slots 26–31
are used as vertex streams. `D3DDevice_SetStreamSource`'s thunk proves the mapping outright
— it computes `(95 - StreamNumber) / 3` and dirties *that texture fetch constant*.

> **Stream *N* → vertex slot `95 - N` → `device + 0x480 + 8*(95-N)`.**
> Stream 0 = slot 95 = `Fetch[31].Vertex[2]` = device + 0x778.

Prefer taking the slot index from the shader's own `vfetch` binding (`fetch_constant` field)
rather than assuming `95 - stream`.

```
GPUVERTEX_FETCH_CONSTANT (8 bytes):
  d0: Type:2@0 (==3 for vertex) · BaseAddress:30@2   → byte address = d0 & ~3, PHYSICAL
  d1: Endian:2@0 · Size:24@2 · AddressClamp@26 · RequestSize:2@28 · ClampDisable:2@30
                                                    → byte size = d1 & 0x03FFFFFC
```

### 7.2 ⚠ `Size` is a bounds clamp, not a vertex count

`D3DDevice_SetStreamSource` (0x820E5310):

```
820e5334  lwz  r10, 0x18(r5)   ; vb->Format.dword0 = vaddr | 3
820e533c  lwz  r5,  0x1C(r5)   ; vb->Format.dword1 = (Length & 0x03FFFFFC) | 0x10000002
820e5340  add  r10, r10, r6    ; dword0 += OffsetInBytes
820e5368  subf r6,  r6, r5     ; dword1 -= OffsetInBytes      <<<<<<
```

`Size` occupies bits 2–25, so subtracting a 4-aligned byte offset decrements it by exactly
that many dwords. Therefore:

> **`size_bytes == VertexBufferLength − OffsetInBytes`** — "bytes from this mesh's base to
> the end of the owning buffer." The GPU uses it only for `vfetch` out-of-bounds clamping.
> `DrawIndexedVertices` never reads the vertex fetch constants at all.

**The real vertex range comes from the draw:** `IB[StartIndex .. StartIndex+IndexCount-1] +
BaseVertexIndex`. Deriving a vertex count as `size / stride` is meaningless and will mirror
the entire remaining buffer for every mesh.

Practical consequence: all meshes sharing a VB have the *same* `base + size` (the buffer's
end address). Key a host mirror on that end address, or bound the copy by the draw's index
range.

### 7.3 The shared vertex buffer is the skinned-vertex cache

- `R_CreateDynamicBuffers` (0x82519208) allocates two pool entries of **`0x480000` = 4.5 MB**
  each (double-buffered across SMP frames).
- `R_AllocSkinnedCachedVerts` (0x825305B8) is an `lwarx/stwcx.` atomic bump allocator:
  `offset = fetch_add(used, 32 * vertCount)`. **Stride is hard-coded 32 bytes.**
- `R_DrawXModelSkinnedCached` (0x82533130) binds it as
  `R_SetStreamSource(&skinnedCacheVb->buffer, skinnedCachedOffset, 32)`.
- `R_ToggleSmpFrame` flips the pool and resets `used = 0`.

So it is **one persistent 4.5 MB vertex buffer** with `OffsetInBytes` advancing per skinned
model. D3D9's `SetStreamSource` takes that offset natively — mirror the buffer once, do not
treat each base as a new buffer. Worker threads write the CPU-side data *before* the render
thread replays the draw, so the whole pool must be uploaded before `RunCommandBuffer`.

### 7.4 `D3DDevice_DrawIndexedVertices` (0x820EAA40)

```c
void D3DDevice_DrawIndexedVertices(
    D3DDevice*       pDevice,          // r3
    D3DPRIMITIVETYPE PrimitiveType,    // r4
    INT              BaseVertexIndex,  // r5   <-- exists ONLY here; never shadowed
    UINT             StartIndex,       // r6
    UINT             IndexCount);      // r7   <-- an INDEX count, not a primitive count
```

After the inlined pending-state flush it emits:

1. type-0, reg `0x2102` (`VGT_INDX_OFFSET`) ← `BaseVertexIndex`
2. `PM4_DRAW_INDX` (`0xC0032201`):
   ```
   dword0 = viz query
   dword1 = (Count << 16) | (PrimitiveType & 0x3F) | (Index32 ? 0x800 : 0)
   dword2 = gpu_phys(ib->Address + StartIndex * indexSize)
   dword3 = (Index32 ? 2*Count : Count) | (endian << 30)
   ```

Because `num_indices` is 16 bits, draws with `IndexCount > 65535` are **split into multiple
packets** using the step/shared table at `0x8209D388`:

`{step, shared}` — `[1]={1,0}` POINTLIST · `[2]={2,0}` LINELIST · `[3]={1,1}` LINESTRIP ·
`[4]={3,0}` TRIANGLELIST · `[5]/[6]={1,2}` STRIP/FAN · `[8]={3,0}` **RECTLIST** ·
`[13]={4,0}` QUADLIST.

Host primitive count = `(IndexCount − shared) / step`.

**There is no validation.** No null check on the index buffer, no bounds check of
`StartIndex`/`IndexCount` against the IB's `Size` field (which the draw never reads).

`D3DDevice_DrawVertices` (0x820EA628) is the same shape with `source_select = auto_index`
and `StartVertex` in `VGT_INDX_OFFSET`.

### 7.5 Index buffers

`D3DDevice_SetIndices` (0x820E54B8) is one instruction: `stw r4, 0x318C(r3)`. No fetch
constant, no dirty bit — the draw reads the pointer directly.

```
D3DIndexBuffer (32 bytes):
  +0x00 D3DResource { Common, RefCount, Fence, ReadFence, Identifier, BaseFlush }
  +0x18 UINT Address     // a VIRTUAL EA (physicalised by the draw)
  +0x1C UINT Size        // never read by the draw path
```

- `Common & 0x80000000` → 32-bit indices.
- `(Common >> 29) & 3` → Xenos endian (INDEX16 → 1 = 8in16; INDEX32 → 2 = 8in32).

Note the asymmetry: **the index-buffer address is virtual; the vertex fetch base is
physical.**

### 7.6 The UP (inline-data) path

`DrawVerticesUP` / `DrawIndexedVerticesUP` build the draw *in the ring with a hole*, memcpy
the caller's data into the hole, and commit. `BeginVertices` synthesises a vertex fetch
constant for fetch slot 95 on the fly, and temporarily overrides `m_StreamStride[0]`.

This is the 2D/UI path (`R_DrawTessTechnique`). Vertex stride is **32 bytes** (`GfxVertex`:
float4 position, packed normal DWORD, GfxColor, float2 texcoord). The UI path explicitly
calls `SetIndices(NULL)` and `R_ClearAllStreamSources` — it guarantees no VB/IB is bound.

---

## 8. Surfaces, EDRAM and render targets

### 8.1 `D3DSurface` — 48 bytes

There is no `XGSetSurfaceHeader` in the binary; the equivalent is `D3D::SetSurfaceHeader`
(0x820D6780), called from `D3DDevice_CreateSurface` (0x820D6C00).

```
+0x00  Common          // D3DRESOURCE; |0x80000000 if EDRAM-allocated
+0x14  BaseFlush       // 0xFFFF0000
+0x18  union { GPU_SURFACEINFO SurfaceInfo;   // EDRAM surfaces
               D3DBaseTexture *Parent; }      // texture-level surfaces
+0x1C  union { GPU_DEPTHINFO DepthInfo; GPU_COLORINFO ColorInfo;
               struct { u32 :22; ArrayIndex:6; MipLevel:4; } }
+0x20  GPU_HICONTROL HiControl
+0x24  u32 :3 ; Height:15 ; Width:14        // <-- the logical extent
+0x28  D3DFORMAT Format
+0x2C  DWORD Size       // 5120 * <EDRAM tile count> — an EDRAM footprint, NOT a linear size
```

**The extent at +0x24** (`SetSurfaceHeader` @ 0x820D6908):

```c
*(DWORD*)(surface + 0x24) = ((8*(Height-1)) & 0x3FFF8) | ((Width-1) << 18) | (old & 7);
```

> **`width = ((v >> 18) & 0x3FFF) + 1`, `height = ((v >> 3) & 0x7FFF) + 1`.**
> Bits 0–2 are left uninitialised (a read-modify-write over `XMemAlloc` garbage) — mask them.

Independently confirmed in `D3DDevice_ClearF` and `D3D::SetViewport`, both of which decode
it the same way.

`+0x18` **is not a row pitch** and `+0x2C` **is not a byte size**. `SurfaceInfo` is the
`RB_SURFACE_INFO` shadow (`SurfacePitch`:14@0 in *pixels*, `MsaaSamples`:2@16,
`HiZPitch`:14@18); `Size` is `5120 × EDRAM tiles`. Deriving an extent from them yields the
EDRAM-padded nonsense (scene depth as 1024×1105, shadow maps as 1024×2080).

**The EDRAM base lives at +0x1C**, in `ColorInfo`/`DepthInfo` bits 0–11, in units of
5120-byte tiles (one tile = 80×16 samples × 4 B).

### 8.2 Two shapes of `D3DSurface` — guard on this

`D3DTexture_GetSurfaceLevel` (0x820D6A00) produces a **texture-level** surface and writes
*only* `Common`, `RefCount`, `BaseFlush`, `+0x18` (parent texture pointer) and `+0x1C`
(mip/slice). **`+0x20`, `+0x24`, `+0x28`, `+0x2C` are heap garbage.**

> Discriminator: **`Common & 0x40000000` set ⇒ texture-level surface** — no extent, no EDRAM
> base. Guard on it before trusting `+0x24`.

The engine holds texture-level surfaces in the `GfxRenderTarget.surface.color` slot for the
"resolved" targets, which are never bound.

### 8.3 Binding

- `D3DDevice_SetRenderTarget` (0x820E5550) → `m_pRenderTarget[N]` at **device + 0x3190**.
  Copies the surface's `ColorInfo` into `m_DestinationPacket.Color{N}Info`. For index 0 it
  calls `D3D::SetSurfaceInfo`, which copies `SurfaceInfo`, derives the MSAA config, and
  **resets the scissor and viewport** (§4).
- `D3DDevice_SetDepthStencilSurface` (0x820E58E0) → `m_pDepthStencilSurface` at **device +
  0x31A0**. Copies `DepthInfo` and `HiControl`.
  **It calls `SetSurfaceInfo` only when `m_pRenderTarget[0] == NULL`** — i.e.
  **`RB_SURFACE_INFO` (pitch, MSAA, HiZ pitch) is taken from the COLOUR target when one is
  bound, never from the depth surface.** This is the mechanism behind the shadow-atlas
  oddity in §10.

---

## 9. Resolves

`D3DDevice_Resolve` (0x820E9928) is a pure forwarder to `D3DDevice_ResolveEx` (0x820E8850).

```c
void D3DDevice_Resolve(D3DDevice*, DWORD Flags, const D3DRECT *pSourceRect,
                       D3DBaseTexture *pDestTexture, const D3DPOINT *pDestPoint,
                       UINT DestLevel, UINT DestSliceOrFace,
                       const D3DVECTOR4 *pClearColor, float ClearZ, DWORD ClearStencil,
                       const D3DRESOLVE_PARAMETERS*);
// r3 dev · r4 Flags · r5 pSourceRect · r6 pDestTexture · r7 pDestPoint · r8 DestLevel …
```

### 9.1 Flags

| bits | meaning |
|---|---|
| `0x00000007` | **source select**: 0–3 = `RENDERTARGET0..3`, **4 = `DEPTHSTENCIL`** |
| `0x00000070` | fragment select. If zero, filled in from the source's MSAA mode (1x→0x10, 2x→0x50, 4x→0x70) |
| `0x00000100` | `CLEARRENDERTARGET` |
| `0x00000200` | `CLEARDEPTHSTENCIL` |
| `0x00002000` | suppress the copy (`CopyCommand = kNull`); only the clear runs |
| `0x00004000` | skip the post-resolve cache flush |
| `0x00008000` | skip the pre-resolve state setup |
| `0xFC000000` | exponent bias (signed 6-bit at shift 26) |

### 9.2 It cannot scale

The resolve is literally a **3-vertex `RECTLIST` auto-indexed draw** (`DRAW_INDX_2`,
`0xC0003600`) over the source rect's corners, using `g_CopyVertexShaderProgram`
(0x8209AE5C). It is a strict **1:1** EDRAM→memory copy with optional format conversion,
exponent bias and MSAA resolve. **Any stretching in NX1 is a separate textured draw.**

Because a resolve *is* a draw, hooking `DrawIndexedVertices` will not see it — `ResolveEx`
must be hooked separately.

### 9.3 Format conversion

`ResolveEx` remaps the destination texture's GPU format for the copy engine. Notably
**`22 (k_24_8)` and `23 (k_24_8_FLOAT)` → `6 (k_8_8_8_8)`** — that is how a depth surface
gets resolved into an RGBA8 texture.

### 9.4 The destination address

```c
delta = (destPoint.y - srcRect.y1) * pitch + 32 * (destPoint.x - srcRect.x1);
RB_COPY_DEST_BASE = gpu_phys(texBase) + delta * bytesPerBlock;
```

> **Only `destPoint − srcRect.topLeft` matters.** The GPU rasterises the source rect in
> EDRAM coordinates and pre-biases the destination address by the delta. The `32 * dx` term
> means the bias is exact only when the deltas are multiples of 32 (the Xenos macro-tile).

### 9.5 Attached clears

`D3DRESOLVE_CLEARRENDERTARGET` / `CLEARDEPTHSTENCIL` write `m_MiscPacket.ColorClear`
(device + 0x2A30), `ColorClearLo` (+0x2A34), `DepthClear` (+0x2A2C, packed as
`(depth24 << 8) | stencil`) and `HiClear` (+0x2A28). **They are not the `m_TilingClear*`
fields** — see §12.

---

## 10. Predicated tiling: NX1 does not use it

`D3DDevice_BeginTiling` and `D3DDevice_EndTiling` **are not linked into this binary.** The
tiling state exists in `CDevice` (`m_Tiles` @ 0x32C4, `m_TileRects[15]` @ 0x32C8,
`m_TilingWidth/Height` @ 0x3474/0x3478, `m_TilingClear*` @ 0x3480/0x3490/0x3494) because the
struct is shared, but **nothing in the image writes it**. `m_Tiles` is always 0, every
`if (predicated)` branch takes the simple path, and `m_TilingClear*` is uninitialised
garbage.

`D3DDevice_SetPredication` (0x820EB448) **ignores its argument entirely** — it is a
disable-predication routine in this build, and the game never calls it.

The consumers are still decodable, and they answer the window-offset question definitively:
had tiling been used, `D3D::SetSurfaceClip` would emit, per tile,

```
PA_SC_WINDOW_OFFSET = ((-tileOffset.y) << 16) | (-tileOffset.x)
```

> **The hardware applies `PA_SC_WINDOW_OFFSET = −tileOrigin`.** The guest keeps its viewport
> and scissor in *logical target* space and the window transform re-aims them into EDRAM.
> The guest never re-aims its own viewport.

NX1 does not need this, because it never tiles. What *looks* like tiling — the shadow atlas
— is something else entirely.

---

## 11. The engine (IW4 `gfx_d3d`)

### 11.1 Frame skeleton

`RB_RenderThread` (0x824F9E88):

```
Sys_SetDeviceOwner(); D3DDevice_AcquireThreadOwnership(dx.device);   // held all frame
data = Sys_RendererSleep();
RB_BeginFrame(data)                       0x824F8650
RB_Draw3D()                               0x824F9CE8
RB_CallExecuteRenderCommands()            0x824F6C80
RB_EndFrame() -> RB_SwapBuffers()         0x824F6A28 / 0x824F6978
```

The engine's own GPU-span names (`s_GPURecords` @ 0x825A79E8) give the pass taxonomy:
`total · sun_shadow · spot_shadow · depth_prepass · lit · xray · emissive/fx · render ·
postfx · swap_wait`.

**Pass order**, from `RB_StandardDrawCommands` (0x8251C948), per view:

1. **Sun shadow maps** — `RB_SunShadowMaps` (0x82537730)
2. **Spot shadow maps** — `RB_SpotShadowMaps` (0x82537AC0)
3. **Pre-3D HUD** → `R_RENDERTARGET_UI_TEXTURE` (128×128)
4. `R_SetAndClearSceneTarget` → binds **SCENE (1024×600)**, clears colour+depth
5. **Z-prepass**: `depth hack`, `opaque prepass`, `light map opaque{,1,2} prepass`, `trans prepass`
6. **Lit/colour**: `opaque`, `lightmap`, `lightmap1`, `lightmap2`, camo, `trans`
7. **XRay**
8. **Depth resolve** → `R_RENDERTARGET_FLOAT_Z` (1024×600 D24FS8)
9. **Sun** (occlusion query + flare)
10. **Emissive/fx**, half-res particles, sun post-effects
11. **`RB_EndSceneResolve`** → `R_RENDERTARGET_RESOLVED_SCENE` (1024×600 A8R8G8B8)

Then `RB_CallExecuteRenderCommands`:

12. `RB_ExecPostFXDisplayCmds` — HUD drawn *into the scene RT*, resolved back
13. **`RB_DoResampleAndPostEffects`** (0x8252C1E0): radial blur, bind **FRAME_BUFFER
    (1920×1080)**, **`RB_ApplyMergedPostEffects` ← the scene→display composite**, bloom
    (ping-pong across `POST_EFFECT_0/1`, both **256×150**), blur, water sheeting
14. `RB_ExecDisplayCmds` — the main 2D HUD
15. The global 2D list (menus, console, debug)
16. **`D3DDevice_Resolve`** of the whole 1920×1080 framebuffer into
    `dx.frontBufferTexture[i]`, then `D3DDevice_Swap`

### 11.2 Render targets (`R_InitRenderTargets_Xbox360`, 0x8251E310)

With `scene = 1024×600`, `display = 1920×1080`:

| id | name | size |
|---|---|---|
| 1 | `FRAME_BUFFER` | **1920×1080** |
| 2 | `SCENE` | **1024×600** |
| 5 | `RESOLVED_SCENE` | 1024×600 texture (surfaces **aliased to SCENE's EDRAM**) |
| 6 | `FLOAT_Z` | 1024×600 D24FS8 |
| 17/18 | `POST_EFFECT_0/1` | **256×150** (scene/4) — the bloom pair |
| 19 | `SHADOWMAP_LARGE` | **1024×2048** atlas |
| 20 | `SHADOWMAP_SMALL` | **512×2048** atlas |
| 22 | `UI_TEXTURE` | 128×128 |

`R_SetRenderTargetSize` and `R_SetRenderTarget` are **separate**: `RESOLVED_SCENE` /
`SCENE_GAMMA` / `RESOLVED_POST_SUN` share SCENE's actual surfaces, so binding them is a
no-op at the D3D level — only the *logical* width/height for viewport and 2D math changes.

### 11.3 The scene → display composite

**There is no stretch-resolve.** `RB_ApplyMergedPostEffects` (0x8252AD60) ends with:

```c
R_SetRenderTargetSize(&state, R_RENDERTARGET_FRAME_BUFFER);   // 1920x1080
R_SetRenderTarget(..., R_RENDERTARGET_FRAME_BUFFER);
RB_ViewportFilter(rgp.postFxMaterial[idx], &viewInfo->displayViewport);
```

`RB_ViewportColoredFilter` (0x824F02A8) draws a **fullscreen quad**: vertices span
`(0,0)…(1920,1080)` in an ortho projection, texcoords are the viewport normalised by the
*framebuffer* size → `(0,0)…(1,1)`, and the material samples `codeImages[11]` =
the 1024×600 `RESOLVED_SCENE` texture. So the upscale is **plain bilinear filtering in the
pixel shader**.

> ⚠ **`R_HW_SetRenderTarget` (0x82526C58) sets `state->viewport = {0,0,rtW,rtH}` WITHOUT
> emitting a `SetViewport`** — it relies on the hardware resetting the viewport when a
> render target is bound (§4). `R_SetViewport` then skips the D3D call because the cache
> already matches. **The engine never issues a viewport change for the composite pass.**
> A host renderer that leaves the previous pass's 1024×600 viewport bound will rasterise
> that fullscreen quad through it, landing the scene 1:1 in the top-left corner of the
> 1920×1080 display.

### 11.4 Threading — worker threads use *separate devices*

The real `dx.device` is protected by a ticket lock (`Sys_SetDeviceOwner`, 0x822F1D28) plus
`D3DDevice_AcquireThreadOwnership`, and **only one thread ever touches it**.

Guest D3D calls nevertheless arrive on ~3 host threads, because of **deferred command
buffers**:

- `DxGlobals` holds `D3DDevice *cmdBufDevice[40]` — 40 *separate* `D3DDevice` objects.
- Each `GfxDrawCallOutput` gets one, plus a `D3DCommandBuffer` from
  `D3DDevice_CreateGrowableCommandBuffer`.
- Worker threads (`R_DrawLitOpaqueCmd`, 0x8250CD68 — only CPUs 1 and 2 qualify) CAS a draw
  list, call `R_InitCmdBuf` → `AcquireThreadOwnership(cmdBuf->device)` +
  **`D3DDevice_BeginCommandBuffer`**, run the ordinary `R_Draw*` code against their *private*
  device, then `EndCommandBuffer`.
- The render thread later calls **`D3DDevice_RunCommandBuffer(dx.device, buf, 0)`** at the
  exact point in the frame shown in §11.1, splicing the recorded packets into the real ring.

> **A single global "currently bound" state is not safe.** Worker threads emit
> `SetRenderTarget`, `SetDepthStencilSurface`, `SetStreamSource`, `SetIndices`, shaders,
> samplers and viewports into their *recording* device; those must not take effect until
> `RunCommandBuffer`. Executing them immediately on one shared host device interleaves
> worker state with render-thread state.

Two viable strategies:
- **(a) Faithful:** keep per-guest-`D3DDevice` state and a per-device recorded command list;
  replay at `RunCommandBuffer` time.
- **(b) Simple:** force the single-threaded path (`r_cmdbuf_worker` / `r_smp_backend` false),
  which makes every draw list run inline on the render thread against `dx.device`. Then one
  global bound-state is correct.

`R_InitCmdBufState` (0x82531600) fully re-syncs state at the start of each recorded block
and `R_ShutdownCmdBufState` (0x82525C10) unbinds everything at the end — that bracketing is
what makes the splicing safe on hardware.

---

## 12. Shadow maps — not tiling, cascade-then-resolve

`R_InitShadowmapRenderTarget` (0x8251DB80):

```c
rt->surface.color        = D3DDevice_CreateSurface(tileRes, tileRowCount*tileRes,
                                                   D3DFMT_A8R8G8B8, NONE, params);
rt->surface.depthStencil = D3DDevice_CreateSurface(tileRes, tileRes,
                                                   dx.depthStencilFormat, NONE, params);
```

- **Sun** (`tileRes=1024, rows=2`): colour surface **1024×2048**, depth surface **1024×1024**,
  atlas image 1024×2048.
- **Spot** (`tileRes=512, rows=4`): colour 512×2048, depth **512×512**, atlas 512×2048.

Because `pParameters != NULL`, `CreateSurface` **skips EDRAM allocation entirely** — both
surfaces get EDRAM base 0. **The 1024×2048 colour "render target" is a dummy**: colour
writes are masked off for the shadow pass so it never touches EDRAM. It exists only because
`SetRenderTarget(0, …)` must have an RT0 — and, per §8.3, `RB_SURFACE_INFO` is taken from
RT0, so the atlas-shaped colour surface is what defines the surface pitch, while the
**1024×1024 depth surface is where the cascade is actually rendered.**

The loop (`R_DrawSunShadowMapCallback`, 0x82538108) — sun has exactly **2 cascades**:

```c
R_SetRenderTarget(shadowmapRT);              // 1024x2048 colour + 1024x1024 depth
R_ClearScreen(0xB0, ...);                    // depth+stencil only
R_RunCommandBuffer("sun shadow map");
R_EnableScissor(cascadeViewport);            // TILE-LOCAL: (0,0,1024,1024) every cascade
R_DrawSurfs_Unsorted(...);
R_ResolveShadowmap(state, cascadeViewport, destX=0, destY=cascadeIndex * tileRes);
```

`R_ResolveShadowmap` (0x824F0D80):

```c
srcRect   = { vp.x & ~7, vp.y & ~7, (vp.x+vp.w+7) & ~7, (vp.y+vp.h+7) & ~7 };
destPoint = { (vp.x & ~7) + destX, (vp.y & ~7) + destY };
R_ResolveAndClear_Xbox360(dev, atlasImage, /*flags*/ 0x14, ...);
//                                          0x14 = DEPTHSTENCIL | FRAGMENT0
```

> **So it is not predicated tiling.** The guest re-renders each cascade into the *same*
> small EDRAM depth surface with the *same* EDRAM-local viewport (hence the constant
> `offset=(512,512) scale=(512,-512)` observed for every cascade), then **resolves the depth
> surface into a different row of the atlas texture**, with the copy engine converting
> `k_24_8` → `k_8_8_8_8`.
>
> The observed dest points `(0,0)` and `(64,1024)` decompose as
> `destPoint = srcRect.topLeft + (destX, destY)`: the second is `srcRect.x1 = 64` (the
> cascade viewport is inset) plus `destY = 1 × 1024`. Only the *delta* is used, so the real
> translation is `(0, +1024)` — one atlas row down.

For a host renderer: bind a **1024×1024** depth target (not 1024×2048), treat the colour
surface as inert, and drive the resolve from the **depth** surface, copying each cascade
into its atlas slot.

Spot shadows are the same shape with 4 data-driven tiles (`destX`/`destY` from the light
struct).

---

## 13. Present

`D3DDevice_Swap(CDevice*, D3DBaseTexture *pFrontBuffer, const D3DVIDEO_SCALER_PARAMETERS*)`
(0x820E78E8) takes a **texture**, not a surface. It copies the texture's 6-dword fetch
constant, patches the presentation format bits, and hands it to `VdSwap`, which programs the
scaler/CRTC front-buffer base register with `gpu_phys(textureBase)`.

`RB_SwapBuffers` (0x824F6978):

```c
D3DDevice_Swap(dx.device, dx.frontBufferTexture[dx.frontBufferTextureIndex], NULL);
dx.frontBufferTextureIndex ^= 1;
```

The front buffers are ordinary `GfxImage`s allocated with `XPhysicalAlloc` (4 KB pages ⇒ the
`0xE0` window ⇒ `0xE3E02000` / `0xE360A000`).

> **The guest never binds the display as a render target.** There is no D3D9-style swap
> chain. The "back buffer" is a *resolve destination texture in main RAM*, and the flip is a
> register write pointing the scaler at it.

For a host renderer: intercept the `ResolveEx` whose destination is one of the two front
buffers and treat that as "blit to the host swap chain"; then no-op the `Swap` itself.

---

## 14. Consequences for the native D3D9 renderer

The architecture is sound — §0.1 proves that hooking only the flush points and reading the
shadow struct is correct. These are the specific places our implementation diverges from
the guest.

### Correctness

1. **VS c4–c7 (the object→world matrix) are read from the wrong place.** They are written
   into the PM4 ring by `GpuBeginShaderConstantF4`, not into `m_Constants`, and the callers
   clear the dirty bits so the shadow flush cannot correct it. Every XModel and
   static-model-array draw therefore gets a stale matrix. **This is the cause of the smeared
   world geometry with islands of correct rendering.** Fix per §6.4.

2. **Shader literal constants are stale by construction** (§6.3). Either replay
   `SetLiteralShaderConstants` or source literals from the ucode's constant table.

3. **Address translation is inverted.** `GuestTranslatePhysical()` actually calls
   `TranslateVirtual`, and `GuestRead32`'s `>= 0xA0000000` test misclassifies every physical
   address — sending fetch-constant bases into the uncommitted `v00000000` virtual heap.
   Use `addr < 0x20000000 ⇒ TranslatePhysical`, else `TranslateVirtual` (§1.2).

4. **Canonicalise every GPU address to physical** before using it as a cache key:
   `phys = (addr & 0x1FFFFFFF) + (((addr >> 20) + 512) & 0x1000)`. Otherwise a resolve
   destination (`0xBF6D8000`, from the texture header) never matches the sampler that reads
   it back (`0x1F6D8000`, from the fetch constant).

5. **`vertexCount = fetchSize / stride` is meaningless** (§7.2). `Size` is a bounds clamp
   meaning "bytes to the end of the buffer". Bound the draw by its index range; mirror the
   skinned-vertex pool once and honour `OffsetInBytes` natively.

6. **Reset the D3D9 viewport on every render-target bind** (§4, §11.3). The guest relies on
   the implicit reset and never emits a viewport for the composite pass. This is why the
   scene lands 1:1 in the top-left instead of upscaled.

7. **The shadow depth target is 1024×1024, not 1024×2048** (§12). The 1024×2048 colour
   surface is a dummy with no EDRAM backing. The "depth must cover the colour target" rule
   should not be applied to it.

8. **Guard on `Common & 0x40000000`** before reading a surface's extent — texture-level
   surfaces have garbage at `+0x24` (§8.2).

9. **Worker threads record into separate `D3DDevice` objects** (§11.4). Executing their
   calls immediately against one shared host device is not safe. Either replay per-device
   command lists at `RunCommandBuffer`, or force the single-threaded path.

10. **Honour the fetch constant's swizzle field** rather than hand-decoding DXT3A/DXT5A to
    RRRR (§5.3) — the swizzle is already correct in the constant.

11. **`ReadTilingClear()` returns garbage** — `m_TilingClear*` is only written by
    `BeginTiling`, which is not linked (§10). NX1 clears through `D3DDevice_Clear`
    (`R_ClearScreen` → `D3D::ClearF` → `ClearSurface`, itself a rect draw into EDRAM).

12. **Blending-enabled should be read from bit 31 of device+0x2F7C**, not inferred from the
    `0x00010001` register value (§4).

13. **Depth bias slope-scale is already ×16** — do not scale it again (§4).

### Performance

14. The 5 MB-per-mesh vertex mirroring falls out of (5). One 4.5 MB buffer, mirrored once
    per frame, replaces hundreds of full-pool copies.

### Not needed

15. **Predicated tiling does not exist in this build** (§10). The tile-rect / dest-point
    resolve machinery is still required for the *shadow atlas* (§12), but not for the
    display, which is resolved whole.
