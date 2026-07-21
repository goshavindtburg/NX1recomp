/**
 * @file    d3d9_hooks.cpp
 * @brief   Interception of the guest's Xenon D3D entry points.
 *
 * The recompiler emits every guest function as a *weak* symbol aliased to
 * `__imp__<name>` (see DEFINE_REX_FUNC in the generated init header). Defining a
 * strong symbol here displaces the recompiled body at link time, while the
 * original stays reachable through `__imp__`.
 *
 * ADD-ON MODE (current): every hook ALWAYS calls `__imp__` first, so the guest's
 * D3D library runs exactly as it does with the flag off -- its CPU-side fence /
 * KickOff bookkeeping is intact and the Xenia command processor still services
 * the ring. Skipping `__imp__` breaks that sync and deadlocks the guest before it
 * even reaches audio. When `nx1_d3d9` is set we ADDITIONALLY drive a native D3D9
 * device (in its own window) from the same guest state, so the two render in
 * parallel and can be compared. Replacing Xenia's output comes once the D3D9 path
 * is proven.
 *
 * We hook only the *flush points*, not the state setters. See guest_d3d.h: the
 * setters merely write the guest D3D::CDevice shadow state, and several (sampler
 * address modes, the Gpu*ShaderConstantF4 fast path) are inlined into engine code
 * and have no hookable entry point at all.
 */

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging/macros.h>
#include <rex/ppc/context.h>

#include "d3d9_renderer.h"
#include "d3d9_constants.h"
#include "d3d9_resources.h"
#include "guest_d3d.h"

//=============================================================================
// Original recompiled bodies
//=============================================================================

REXCVAR_DECLARE(bool, nx1_d3d9_dbg_hwcensus);   // defined in d3d9_resources.cpp
REXCVAR_DECLARE(bool, nx1_d3d9_dbg_pageorigin);  // defined in d3d9_resources.cpp

REX_EXTERN(__imp__rex_D3DDevice_DrawIndexedVertices);
REX_EXTERN(__imp__rex_D3DDevice_DrawVertices);
REX_EXTERN(__imp__rex_D3DDevice_DrawIndexedVerticesUP);
REX_EXTERN(__imp__rex_D3DDevice_DrawVerticesUP);
REX_EXTERN(__imp__rex_D3DDevice_ClearF);
REX_EXTERN(__imp__rex_D3DDevice_Resolve);
REX_EXTERN(__imp__rex_D3DDevice_ResolveEx);
REX_EXTERN(__imp__rex_D3DDevice_SetRenderTarget);
REX_EXTERN(__imp__rex_D3DDevice_SetDepthStencilSurface);
REX_EXTERN(__imp__rex_D3DDevice_Swap);
REX_EXTERN(__imp__rex_D3DDevice_GpuBeginShaderConstantF4);
REX_EXTERN(__imp__rex_XGSetVertexBufferHeader);
REX_EXTERN(__imp__rex_XGSetIndexBufferHeader);
REX_EXTERN(__imp__rex_XGSetTextureHeader);
REX_EXTERN(__imp__rex_R_AddDrawCall_YAXPAUGfxViewInfo_I_Z);

namespace {

/// Sample the guest's float-constant dirty masks and retire any ring-owned register group
/// the guest is about to flush from its shadow.
///
/// This MUST run before the draw's `__imp__`: the guest's flush zeroes the masks, so after
/// it there is no way to tell which groups the shadow just overwrote. A set bit means the
/// shadow flush wins (it is emitted after the SET_CONSTANT packet); a clear bit means a
/// GpuBeginShaderConstantF4 record, if any, is what the GPU actually holds.
/// `dvar_t* const r_cmdbuf_worker`, and the byte offset of `dvar_t::current.enabled`.
/// Both read straight off R_AddDrawCall's disassembly (guest 0x824DA538).
inline constexpr uint32_t kRCmdBufWorkerDvarPtr = 0x841765B0;
inline constexpr uint32_t kDvarCurrentEnabled = 0x0C;

/// Turn off NX1's deferred draw-call command buffers. See the R_AddDrawCall hook.
void DisableCmdBufWorkerDvar(uint8_t* base) {
#ifdef _WIN32
  const uint32_t dvar = nx1::d3d9::GuestRead32(base, kRCmdBufWorkerDvarPtr);
  if (!dvar) {
    return;  // dvars not registered yet
  }
  uint8_t* enabled = base + dvar + kDvarCurrentEnabled;
  if (*enabled) {
    *enabled = 0;
    REXGPU_INFO("nx1_d3d9: r_cmdbuf_worker disabled; recording draw lists on the render thread");
  }
#else
  (void)base;
#endif
}

/// TEMP PROFILING: accumulated nanoseconds spent inside the guest's own D3D library on draw
/// paths, reported once a second. This is the ceiling on what bypassing `__imp__` could save.
std::atomic<uint64_t> g_imp_ns{0};
std::atomic<uint64_t> g_imp_calls{0};

#define NX1_TIME_IMP(call)                                                              \
  do {                                                                                  \
    if (!nx1::d3d9::IsEnabled()) {                                                      \
      call;                                                                             \
    } else {                                                                            \
      const auto _t0 = std::chrono::steady_clock::now();                                \
      call;                                                                             \
      g_imp_ns.fetch_add(                                                               \
          uint64_t((std::chrono::steady_clock::now() - _t0).count()),                   \
          std::memory_order_relaxed);                                                   \
      g_imp_calls.fetch_add(1, std::memory_order_relaxed);                              \
    }                                                                                   \
  } while (0)

void RetireRingConstants(const uint8_t* base, uint32_t device) {
#ifdef _WIN32
  if (!nx1::d3d9::IsEnabled() || !device) {
    return;
  }
  auto& ring = nx1::d3d9::ConstantRing::For(device);
  ring.Retire(/*pixel_stage=*/false,
              nx1::d3d9::GuestRead64(base, device + nx1::d3d9::guest_device::kAluDirtyMaskVs));
  ring.Retire(/*pixel_stage=*/true,
              nx1::d3d9::GuestRead64(base, device + nx1::d3d9::guest_device::kAluDirtyMaskPs));
#else
  (void)base;
  (void)device;
#endif
}

/// Lazily bring the host device up on first use, from whichever thread the guest
/// render backend happens to own the device on.
/// Defined further down, beside the command-buffer hooks; declared here because the draw
/// entry points appear earlier in the file. Anonymous namespaces merge within a TU.
void NoteDrawForCmdBufCensus();

bool EnsureRenderer() {
#ifdef _WIN32
  if (!nx1::d3d9::IsEnabled()) {
    return false;
  }
  auto& renderer = nx1::d3d9::Renderer::Get();
  if (!renderer.initialized()) {
    renderer.Initialize(nx1::d3d9::FindGameWindow());
  }
  return renderer.initialized();
#else
  return false;
#endif
}

}  // namespace

//=============================================================================
// Draw
//=============================================================================
//
// PPC argument registers, recovered from D3DDevice_DrawIndexedVertices and its
// call site in R_DrawIndexedPrimitive:
//     r3 = D3DDevice*        r4 = D3DPRIMITIVETYPE
//     r5 = BaseVertexIndex   r6 = StartIndex        r7 = VertexCount
// (IDA renders r5:r6 as a single 64-bit `StartIndex`; it is really two args.)

// IMPORTANT: capture the argument registers BEFORE calling __imp__. The
// recompiled body runs on `ctx` as the live PPC machine state and clobbers the
// volatile registers (r3-r12), so reading ctx.rN after __imp__ returns yields the
// function's leftovers, not its arguments.

REX_HOOK_RAW(rex_D3DDevice_DrawIndexedVertices) {
  NoteDrawForCmdBufCensus();
#ifdef _WIN32
  const uint32_t device = ctx.r3.u32, prim = ctx.r4.u32, base_vtx = ctx.r5.u32,
                 start_index = ctx.r6.u32, index_count = ctx.r7.u32;
  RetireRingConstants(base, device);
#endif
  // TEMP PROFILING: what the guest's own D3D library costs per draw -- i.e. exactly the work
  // that "taking over the ring" would delete. Everything it does here is PM4 construction for
  // a stream nobody consumes any more (Xenia's raster is already skipped), plus the CPU-side
  // fence/KickOff bookkeeping the guest genuinely needs. Measure before removing.
#ifdef _WIN32
  // Snapshot the guest's pending dirty mask BEFORE its draw body runs -- the body flushes the
  // tagged state and then zeroes the mask, so reading it afterwards (as the renderer does)
  // always sees zero. If these bits show that most draws change little, whole phases of state
  // resolution can be skipped rather than re-read every draw.
  nx1::d3d9::Renderer::Get().CaptureGuestDirtyMask(base, device);
#endif
  NX1_TIME_IMP(__imp__rex_D3DDevice_DrawIndexedVertices(ctx, base));
#ifdef _WIN32
  if (EnsureRenderer()) {
    nx1::d3d9::Renderer::Get().DrawIndexed(base, device, prim, base_vtx, start_index, index_count);
  }
#endif
}

REX_HOOK_RAW(rex_D3DDevice_DrawVertices) {
  NoteDrawForCmdBufCensus();
#ifdef _WIN32
  // r3 = device, r4 = primType, r5 = StartVertex, r6 = VertexCount
  const uint32_t device = ctx.r3.u32, prim = ctx.r4.u32, start_vtx = ctx.r5.u32,
                 vtx_count = ctx.r6.u32;
  RetireRingConstants(base, device);
#endif
  NX1_TIME_IMP(__imp__rex_D3DDevice_DrawVertices(ctx, base));
#ifdef _WIN32
  if (EnsureRenderer()) {
    nx1::d3d9::Renderer::Get().Draw(base, device, prim, start_vtx, vtx_count);
  }
#endif
}

// The *UP ("user pointer") draws stream vertices/indices straight out of guest
// memory rather than from bound buffers. This is the 2D/UI path: with no map
// loaded, the entire main menu draws through here (R_DrawTessTechnique).
//
// D3DDevice_DrawIndexedVerticesUP(device, PrimType, MinVertexIndex, NumVertices,
//     IndexCount, pIndexData, IndexFormat, pVertexData, VertexStride):
//   r3=device r4=prim r5=minVtx r6=numVerts r7=indexCount r8=indices
//   r9=indexFormat r10=verts   -- stride is the 9th arg, on the stack at [r1+0x54]
//   (verified against the callee's own `lwz r30, 0xB0+arg_54(r1)`).
REX_HOOK_RAW(rex_D3DDevice_DrawIndexedVerticesUP) {
  NoteDrawForCmdBufCensus();
#ifdef _WIN32
  const uint32_t device = ctx.r3.u32, prim = ctx.r4.u32, num_verts = ctx.r6.u32,
                 index_count = ctx.r7.u32, indices = ctx.r8.u32, index_fmt = ctx.r9.u32,
                 verts = ctx.r10.u32;
  const uint32_t stride = nx1::d3d9::GuestRead32(base, ctx.r1.u32 + 0x54);
  RetireRingConstants(base, device);
#endif
  NX1_TIME_IMP(__imp__rex_D3DDevice_DrawIndexedVerticesUP(ctx, base));
#ifdef _WIN32
  if (EnsureRenderer()) {
    nx1::d3d9::Renderer::Get().DrawIndexedUP(base, device, prim, num_verts, index_count, indices,
                                             index_fmt, verts, stride);
  }
#endif
}

// D3DDevice_DrawVerticesUP(device, PrimType, VertexCount, pVertexData, VertexStride):
//   r3=device r4=prim r5=vertexCount r6=verts r7=stride
REX_HOOK_RAW(rex_D3DDevice_DrawVerticesUP) {
  NoteDrawForCmdBufCensus();
#ifdef _WIN32
  const uint32_t device = ctx.r3.u32, prim = ctx.r4.u32, vtx_count = ctx.r5.u32, verts = ctx.r6.u32,
                 stride = ctx.r7.u32;
  RetireRingConstants(base, device);
#endif
  NX1_TIME_IMP(__imp__rex_D3DDevice_DrawVerticesUP(ctx, base));
#ifdef _WIN32
  if (EnsureRenderer()) {
    nx1::d3d9::Renderer::Get().DrawUP(base, device, prim, vtx_count, verts, stride);
  }
#endif
}

//=============================================================================
// Clear
//=============================================================================
//
// D3DDevice_ClearF(pDevice, Flags, pRect, pColor, Z, Stencil):
//   r3 = pDevice   r4 = Flags   r5 = pRect (_D3DRECT*, may be null)
//   r6 = pColor (__vector4 of floats)   f1 = Z   r8 = Stencil
//
// ClearF, *not* Clear, is the choke point: D3DDevice_Clear only unpacks its D3DCOLOR
// into a float vector and forwards here, and NX1's own frame clear
// (R_ClearScreenInternal, guest 0x825256F8) calls ClearF directly -- which is why
// hooking D3DDevice_Clear saw exactly one call in a whole session, at startup, and the
// scene's colour and depth were never cleared at all.
//
// Flags are Xenos, not desktop: bits 0-3 are colour targets 0-3 (D3D::ClearF loops
// `(1 << i) & flags` over the four render targets), 0x10 is depth, 0x20 is stencil.
// Reading them as desktop D3DCLEAR_* bits gets every one of them wrong.

REX_HOOK_RAW(rex_D3DDevice_ClearF) {
#ifdef _WIN32
  const uint32_t flags = ctx.r4.u32, rect = ctx.r5.u32, color = ctx.r6.u32, stencil = ctx.r8.u32;
  const float z = float(ctx.f1.f64);
#endif
  __imp__rex_D3DDevice_ClearF(ctx, base);
#ifdef _WIN32
  if (EnsureRenderer()) {
    nx1::d3d9::Renderer::Get().Clear(base, flags, rect, color, z, stencil);
  }
#endif
}

//=============================================================================
// Resolve (EDRAM -> texture, and the frame clear)
//=============================================================================
//
// D3DDevice_Resolve(pDevice, Flags, pSourceRect, pDestTexture, pDestPoint,
//                   DestLevel, DestSliceIndex, pClearColor, ClearZ, ClearStencil, pParams):
//   r3 = pDevice   r4 = Flags       r5 = pSourceRect   r6 = pDestTexture  r7 = pDestPoint
//   r8 = DestLevel r9 = DestSlice   r10 = pClearColor  f1 = ClearZ
//   ClearStencil is the 10th argument, in the caller's parameter save area at r1+0x5C.
//
// The guest renders through predicated tiling: the frame is drawn as a set of
// horizontal bands, each resolved into the destination at its own pDestPoint offset.
// Ignoring pDestPoint blits every band over the whole destination, so the last band
// ends up smeared across the entire frame -- honour it and the tiles compose.
//
// The Xbox 360 also *clears* EDRAM as part of the resolve, and NX1 uses only that path:
// R_ResolveAndClear_Xbox360 (guest 0x824F05C8) folds the caller's `whichToClear` into
// D3DRESOLVE_CLEARRENDERTARGET (0x100) / D3DRESOLVE_CLEARDEPTHSTENCIL (0x200) and hands
// the clear colour, Z and stencil to Resolve. D3DDevice_Clear is never called at all --
// so this is the one and only thing that clears the scene's colour and depth buffers.

REX_HOOK_RAW(rex_D3DDevice_Resolve) {
#ifdef _WIN32
  const uint32_t dest_texture = ctx.r6.u32, src_rect = ctx.r5.u32, dest_point = ctx.r7.u32;
  const uint32_t flags = ctx.r4.u32, clear_color = ctx.r10.u32;
  const uint32_t clear_stencil = nx1::d3d9::GuestRead32(base, ctx.r1.u32 + 0x5C);
  const float clear_z = float(ctx.f1.f64);
  // RESOLVE CENSUS (nx1_d3d9_dbg_hwcensus). Four documented arguments we currently discard, each
  // of which silently changes the result if the guest ever uses it:
  //  * EXPONENTBIAS (flags >> 26, signed): "Bias each component by multiplying by 2^Bias" -- the
  //    classic 360 HDR-in-8888 idiom. Ignoring a non-zero bias leaves the resolved image off by an
  //    exact power of two while the source target itself looks correct.
  //  * source select (flags & 0x7): 0..3 pick RENDERTARGET0..3, 4 is DEPTHSTENCIL. We instead
  //    infer depth-vs-colour from the DESTINATION format and always read render target 0, so an
  //    MRT resolve or a depth resolve into a colour-format destination takes the wrong source.
  //  * fragment select (flags & 0x70): MSAA fragment extraction. ALLFRAGMENTS is 0, so the common
  //    downsample is indistinguishable from "no flags" -- only a non-zero value proves it matters.
  //  * DestLevel (r8) / DestSliceOrFace (r9): we always write mip 0, face 0.
  if (REXCVAR_GET(nx1_d3d9_dbg_hwcensus)) {
    static std::atomic<uint32_t> expbias{0}, srcsel{0}, frags{0}, level{0}, slice{0};
    static std::atomic<uint64_t> n{0};
    if (int32_t(flags) >> 26) expbias.fetch_add(1, std::memory_order_relaxed);
    if (flags & 0x7) srcsel.fetch_or(1u << (flags & 0x7), std::memory_order_relaxed);
    // Record WHICH fragment selector, not merely that one is set. D3DRESOLVE_ALLFRAGMENTS is 0x00
    // and FRAGMENT0 is 0x10, and on a single-sampled buffer those two are equivalent -- so a
    // non-zero count alone cannot tell a benign "take sample 0" from a genuine multi-fragment
    // extraction (FRAGMENT1/2/3, 0x20/0x30/0x40), which we could not honour. Bit N set here means
    // selector value N*0x10 was observed.
    if (flags & 0x70) frags.fetch_or(1u << ((flags & 0x70) >> 4), std::memory_order_relaxed);
    if (ctx.r8.u32) level.fetch_add(1, std::memory_order_relaxed);
    if (ctx.r9.u32) slice.fetch_add(1, std::memory_order_relaxed);
    if ((n.fetch_add(1, std::memory_order_relaxed) % 2000) == 0) {
      REXGPU_WARN("nx1_d3d9: HWCENSUS resolve: exponent_bias!=0 {}x | source_select mask 0x{:X} "
                  "(bit4 = DEPTHSTENCIL, bits1-3 = RT1..3) | fragment_select mask 0x{:X} (bit N = "
                  "selector N*0x10; bit1 = FRAGMENT0, which is EQUIVALENT to ALLFRAGMENTS on a "
                  "single-sampled buffer and therefore harmless -- bits 2/3/4 = FRAGMENT1/2/3 are "
                  "the ones we could not honour) | dest_level!=0 {}x | dest_slice_or_face!=0 {}x",
                  expbias.load(), srcsel.load(), frags.load(), level.load(), slice.load());
    }
  }
#endif
  __imp__rex_D3DDevice_Resolve(ctx, base);
#ifdef _WIN32
  if (EnsureRenderer()) {
    nx1::d3d9::Renderer::Get().Resolve(base, dest_texture, src_rect, dest_point, flags,
                                       clear_color, clear_z, clear_stencil);
  }
#endif
}

// THE SECOND RESOLVE ENTRY POINT, never hooked until now. The guest has both Resolve and
// ResolveEx; we only ever saw the former, so our resolve map held 8 destinations while the
// reference's PM4 census showed 13. The five we missed (1DFA0000, 1E0A0000, 1E6A0000,
// 1E6A2000, 1E6C0000 -- up to 4 MB each) are render targets the game later SAMPLES AS
// TEXTURES. Never having them, we decoded stale guest memory and got structured garbage --
// and readback_resolve="full" appeared to "fix textures" only because it made the reference
// write those render targets into CPU RAM for us to decode.
//
// Registration only: flags are passed as 0 so this cannot alter EDRAM clear behaviour, which
// the primary Resolve path already handles. The argument positions match Resolve's (r3 device,
// r4 flags, r5 source rect, r6 destination texture, r7 destination point) -- confirmed against
// the recompiled prologue, and confirmable at runtime because the destinations it registers
// should be exactly the five addresses above (watch RESOLVEDST).
REX_HOOK_RAW(rex_D3DDevice_ResolveEx) {
#ifdef _WIN32
  const uint32_t dest_texture = ctx.r6.u32, src_rect = ctx.r5.u32, dest_point = ctx.r7.u32;
#endif
  __imp__rex_D3DDevice_ResolveEx(ctx, base);
#ifdef _WIN32
  if (EnsureRenderer()) {
    nx1::d3d9::Renderer::Get().Resolve(base, dest_texture, src_rect, dest_point, /*flags=*/0,
                                       /*clear_color=*/0, /*clear_z=*/0.0f, /*clear_stencil=*/0);
  }
#endif
}

//=============================================================================
// Render targets
//=============================================================================
//
// D3DDevice_SetRenderTarget(pDevice, RenderTargetIndex, pRenderTarget):
//   r3 = pDevice   r4 = RenderTargetIndex   r5 = pRenderTarget (D3DSurface*)
// D3DDevice_SetDepthStencilSurface(pDevice, pNewZStencil):
//   r3 = pDevice   r4 = pNewZStencil (D3DSurface*)
//
// The guest draws the world, its shadow maps, the bloom chain and the final composite
// into different surfaces. Each gets its own host target; a display-sized colour
// target aliases the backbuffer.

REX_HOOK_RAW(rex_D3DDevice_SetRenderTarget) {
#ifdef _WIN32
  const uint32_t index = ctx.r4.u32, surface = ctx.r5.u32;
#endif
  __imp__rex_D3DDevice_SetRenderTarget(ctx, base);
#ifdef _WIN32
  if (EnsureRenderer()) {
    nx1::d3d9::Renderer::Get().SetRenderTarget(base, index, surface);
  }
#endif
}

REX_HOOK_RAW(rex_D3DDevice_SetDepthStencilSurface) {
#ifdef _WIN32
  const uint32_t surface = ctx.r4.u32;
#endif
  __imp__rex_D3DDevice_SetDepthStencilSurface(ctx, base);
#ifdef _WIN32
  if (EnsureRenderer()) {
    nx1::d3d9::Renderer::Get().SetDepthStencil(base, surface);
  }
#endif
}

//=============================================================================
// Present
//=============================================================================

REX_HOOK_RAW(rex_D3DDevice_Swap) {
  __imp__rex_D3DDevice_Swap(ctx, base);
#ifdef _WIN32
  if (EnsureRenderer()) {
    nx1::d3d9::Renderer::Get().Present();
    nx1::d3d9::Renderer::Get().BeginFrame();
  }
  // TEMP PROFILING: the guest D3D library's own per-frame cost on the draw paths -- the exact
  // prize for bypassing __imp__. Reported next to PROF/frame so it can be read against
  // `outside` (which is guest game logic + this).
  {
    static uint32_t frames = 0;
    if (++frames >= 60) {
      const uint64_t ns = g_imp_ns.exchange(0, std::memory_order_relaxed);
      const uint64_t calls = g_imp_calls.exchange(0, std::memory_order_relaxed);
      REXGPU_INFO("nx1_d3d9: PROF/imp guest-D3D-library {:.2f} ms/frame over {} draws/frame",
                  double(ns) / (1e6 * frames), calls / frames);
      frames = 0;
    }
  }
#endif
}

//=============================================================================
// Deferred command buffers -- force the single-threaded backend
//=============================================================================
//
// R_AddDrawCall (guest 0x824DA538) is the only producer of WRKCMD_DRAW_LIT_OPAQUE, the only
// worker command that records guest D3D. When it fires, a worker thread runs the ordinary
// R_Draw* code against its own D3DDevice out of dx.cmdBufDevice[40] and the render thread
// splices the result in later via D3DDevice_RunCommandBuffer.
//
// We execute draws where they are recorded, so worker recording means several threads
// driving one host device at once: thread A's constants and shaders land between thread B's
// setup and B's draw. That is the smeared-geometry-plus-flicker signature.
//
// The engine already ships the way out. R_AddDrawCall is gated on
// r_smp_worker && r_smp_backend && r_cmdbuf_worker, and when the gate is false it does
// nothing: p_cmdBufValid[type] stays 0, R_RunCommandBuffer (0x82519C70) replays nothing, and
// RB_StandardDrawCommands' unconditional R_DrawLitOpaque/R_DepthPrepass*/... calls find a
// full draw-list iterator and record everything inline on the render thread. Suppressing the
// call reproduces that configuration exactly, without touching a dvar.
//
// Revisit if we ever replay per-device command lists at RunCommandBuffer time instead.

REX_HOOK_RAW(rex_R_AddDrawCall_YAXPAUGfxViewInfo_I_Z) {
#ifdef _WIN32
  // Suppressing this function is not enough on its own: the PPC compiler *inlined*
  // R_AddDrawCall into its callers (which is why R_GenerateSortedDrawSurfs calls
  // Sys_AddWorkerCmd directly at eight sites), and a hook cannot reach an inlined copy.
  //
  // Every copy, inlined or not, tests the same gate, so clear the gate instead. From the
  // disassembly of R_AddDrawCall (0x824DA538):
  //
  //     lwz    r10, r_cmdbuf_worker     ; dvar_t*
  //     lbz    r8,  0xC(r10)            ; ->current.enabled
  //     cmplwi cr6, r8, 0
  //     beq    cr6, skip                ; false -> queue nothing, p_cmdBufValid stays 0
  //
  // With the gate false the engine records every draw list inline on the render thread
  // (see the note above), which is the whole point.
  if (nx1::d3d9::IsEnabled()) {
    DisableCmdBufWorkerDvar(base);
  }
#endif
  __imp__rex_R_AddDrawCall_YAXPAUGfxViewInfo_I_Z(ctx, base);
}

//=============================================================================
// Shader constants -- GPU fast path
//=============================================================================
//
// D3DDevice_GpuBeginShaderConstantF4(pDevice, PixelShader, StartRegister, Count)
// returns a raw pointer *into the PM4 ring*, pre-stamped with a 0xC0002D00
// SET_CONSTANT header; the engine then writes float4s directly there. The data never
// reaches m_Constants, and the caller clears the group's dirty bit so the draw-time
// shadow flush cannot overwrite it. GpuEndShaderConstantF4 is a 4-byte no-op, so there
// is no "commit" to hook.
//
// This carries VS c4..c7 -- the object->world matrix -- on every model draw, so the
// shadow is NOT authoritative for it. Let the original run (it keeps the ring valid and
// m_pRing correct), then remember where it put the data so the draw can read it back.
// See d3d9_constants.h for the full arbitration.

REX_HOOK_RAW(rex_D3DDevice_GpuBeginShaderConstantF4) {
#ifdef _WIN32
  // r3 = pDevice, r4 = PixelShader, r5 = StartRegister, r6 = Vector4fCount. Capture before
  // __imp__: the recompiled body runs on ctx and clobbers the volatile registers -- r3 in
  // particular comes back holding the return value.
  const uint32_t device = ctx.r3.u32;
  const uint32_t pixel_shader = ctx.r4.u32;
  const uint32_t start_register = ctx.r5.u32;
  const uint32_t count = ctx.r6.u32;
#endif
  __imp__rex_D3DDevice_GpuBeginShaderConstantF4(ctx, base);
#ifdef _WIN32
  if (nx1::d3d9::IsEnabled() && device) {
    // r3 is now the returned ring pointer (0 if the reservation failed).
    nx1::d3d9::ConstantRing::For(device).Record(pixel_shader != 0, start_register, count,
                                                ctx.r3.u32);
  }
#endif
}

//=============================================================================
// Resource registration
//=============================================================================
//
// NX1 never creates vertex/index buffers through D3D -- those entry points are
// not even linked in. It allocates its own memory and stamps a header onto it.
// Textures likewise: pixels arrive from fastfiles, already Xenos-tiled.
//
// We let the guest build its header (engine code reads those fields back), then
// register the resource with our tracker.
//
// TODO(d3d9): implement the tracker -- record {guest address, size, format,
// stride, dimensions}, upload lazily at draw time with dirty tracking, and
// untile texture data on the way in.

REX_HOOK_RAW(rex_XGSetVertexBufferHeader) {
  __imp__rex_XGSetVertexBufferHeader(ctx, base);
}

REX_HOOK_RAW(rex_XGSetIndexBufferHeader) {
  __imp__rex_XGSetIndexBufferHeader(ctx, base);
}

REX_HOOK_RAW(rex_XGSetTextureHeader) {
  __imp__rex_XGSetTextureHeader(ctx, base);
}

//=============================================================================
// Command buffer playback (the unrecorded draw path)
//=============================================================================
//
// The CPFETCH/D9FETCH alignment found ~2400 ring draws per window that our draw hooks never
// record, and paired substitutions at matching positions (ring s0=10000000 where ours says a
// pool slot). Xenon command buffers carry FIXUPS -- resources patched into the recorded PM4 at
// playback -- and that is IW4's late image binding: record with the intended image, play back
// with whatever representation is currently resident. We capture the device shadow at RECORD
// time, so for command-buffered draws we bind the pre-fixup constant: the unstreamed slot,
// while the console binds the low-res proxy. That is the distance speckle at the binding
// layer. These counters establish the traffic before any capture surgery.
REX_EXTERN(__imp__rex_D3DDevice_RunCommandBuffer);
REX_EXTERN(__imp__rex_D3DDevice_InsertAsyncCommandBufferCall);
REX_EXTERN(__imp__rex_R_RunCommandBuffer_YAXPBUGfxBackEndData_IPAUGfxDrawListIter_PBUGfxViewport_PBD_Z);

namespace {
std::atomic<uint64_t> g_cb_run{0}, g_cb_async{0}, g_cb_r_run{0};

/// THE MEASUREMENT. The theory says the bake/imposter draws live inside command-buffer
/// playback, which replays PM4 directly and never enters D3DDevice_Draw* -- so we never render
/// them, our render target is empty where the bake belongs, and the writeback copies nothing.
/// Before doing surgery on that theory, prove it: mark the playback window on the thread that
/// runs it, and count how many of OUR draws land inside it. Zero inside a window the reference
/// fills with thousands of ring draws is the proof; a healthy count refutes it outright and
/// saves a large piece of misdirected work.
thread_local uint32_t g_in_cmdbuf = 0;
std::atomic<uint64_t> g_draws_in_cb{0}, g_draws_out_cb{0}, g_cb_windows_closed{0};

void NoteDrawForCmdBufCensus() {
  if (g_in_cmdbuf) {
    g_draws_in_cb.fetch_add(1, std::memory_order_relaxed);
  } else {
    g_draws_out_cb.fetch_add(1, std::memory_order_relaxed);
  }
}
void LogCmdBufTraffic() {
  static std::atomic<uint64_t> last{0};
  const uint64_t total = g_cb_run.load() + g_cb_async.load() + g_cb_r_run.load();
  if (total - last.load() >= 100) {
    last.store(total);
    REXGPU_WARN("nx1_d3d9: CMDBUF traffic run={} async={} r_run={}", g_cb_run.load(),
                g_cb_async.load(), g_cb_r_run.load());
  }
}
}  // namespace

REX_HOOK_RAW(rex_D3DDevice_RunCommandBuffer) {
  g_cb_run.fetch_add(1, std::memory_order_relaxed);
  LogCmdBufTraffic();
  __imp__rex_D3DDevice_RunCommandBuffer(ctx, base);
}

REX_HOOK_RAW(rex_D3DDevice_InsertAsyncCommandBufferCall) {
  g_cb_async.fetch_add(1, std::memory_order_relaxed);
  LogCmdBufTraffic();
  __imp__rex_D3DDevice_InsertAsyncCommandBufferCall(ctx, base);
}

REX_HOOK_RAW(rex_R_RunCommandBuffer_YAXPBUGfxBackEndData_IPAUGfxDrawListIter_PBUGfxViewport_PBD_Z) {
  g_cb_r_run.fetch_add(1, std::memory_order_relaxed);
  LogCmdBufTraffic();
  ++g_in_cmdbuf;
  __imp__rex_R_RunCommandBuffer_YAXPBUGfxBackEndData_IPAUGfxDrawListIter_PBUGfxViewport_PBD_Z(ctx,
                                                                                              base);
  --g_in_cmdbuf;
  const uint64_t w = g_cb_windows_closed.fetch_add(1, std::memory_order_relaxed) + 1;
  if ((w % 2000) == 0) {
    REXGPU_WARN("nx1_d3d9: CBDRAWS {} playbacks | our draws INSIDE={} OUTSIDE={} -- zero inside "
                "means command-buffer geometry never reaches our renderer at all",
                w, g_draws_in_cb.load(), g_draws_out_cb.load());
  }
}

//=============================================================================
// Placeholder detection (objective speckle metric)
//=============================================================================
//
// Every "this config looks better" judgement tonight was later walked back -- by both of us.
// So stop judging by eye. The engine states non-residency explicitly: GetDefaultPixels hands
// out the placeholder buffer an unloaded image is shown with. Capture its address once, and
// the renderer can then count, per frame, how many bound textures are actually placeholders.
// That converts "is the speckle better" into a number that is comparable across configs and
// across backends.
REX_EXTERN(__imp__rex_ImageCache_GetDefaultPixels_YAPBEXZ);

REX_HOOK_RAW(rex_ImageCache_GetDefaultPixels_YAPBEXZ) {
  __imp__rex_ImageCache_GetDefaultPixels_YAPBEXZ(ctx, base);
#ifdef _WIN32
  const uint32_t pixels = ctx.r3.u32;
  if (pixels && nx1::d3d9::ResourceTracker::SetDefaultPixelsAddress(pixels)) {
    REXGPU_WARN("nx1_d3d9: DEFAULTPIXELS buffer at {:08X} -- textures bound at this address are "
                "the engine's own not-resident placeholder",
                pixels);
  }
#endif
}

//=============================================================================
// ImageCache streamer pacing
//=============================================================================
//
// Measured: pure Xenia moves ~51,500 DMA copies / 711 MiB in ~72s of play; our mode fewer
// than 500 in a longer walk. The streamer is throttling itself on some signal our backend
// presents differently, and its obvious pacing input is this fence wait. Count and time it in
// both modes: a streamer spinning on a never-clearing fence shows up as huge call counts or
// huge cumulative time; a streamer that never even ASKS shows up as near-zero calls -- which
// would move the suspect upstream to the priority/visibility inputs instead.
REX_EXTERN(__imp__rex_ImageCache_WaitFence_YAHXZ);

// The two globals the streamer's DMA pipeline turns on, read straight off WaitFence's
// disassembly: r31 = 0x840F8020 (the fence value it polls) and r31-28 = 0x840F8004 (the
// "work outstanding" flag). Sampling BOTH modes says whether the streamer is even arming
// work under our backend, or arming it and having the fence retire so fast it never blocks.
// Verified arithmetic: base -2079064064 = 0x84140000 unsigned, r31 = base - 0x7FE0
// = 0x84138020. An earlier hand-computed 0x840F8020 read zeros in BOTH modes while
// ret=1 proved work WAS pending -- a wrong address reads exactly like a clean null
// result, which is the failure mode this project has hit repeatedly.
inline constexpr uint32_t kImgCachePendingFlag = 0x84138004;
inline constexpr uint32_t kImgCacheFence = 0x84138020;

namespace {
void VerifyPendingDma(const uint8_t* base);  // defined with the DMA hooks below
}  // namespace

REX_HOOK_RAW(rex_ImageCache_WaitFence_YAHXZ) {
  static std::atomic<uint64_t> calls{0}, ns{0}, armed{0};
  const uint32_t pending_before = nx1::d3d9::GuestRead32(base, kImgCachePendingFlag);
  const uint32_t fence_before = nx1::d3d9::GuestRead32(base, kImgCacheFence);
  if (pending_before) {
    armed.fetch_add(1, std::memory_order_relaxed);
  }
  const auto t0 = std::chrono::steady_clock::now();
  __imp__rex_ImageCache_WaitFence_YAHXZ(ctx, base);
  VerifyPendingDma(base);  // the guest's own "DMA retired" signal is the place to check
  const uint64_t n = calls.fetch_add(1, std::memory_order_relaxed) + 1;
  const uint64_t total =
      ns.fetch_add(uint64_t((std::chrono::steady_clock::now() - t0).count()),
                   std::memory_order_relaxed);
  if ((n % 200) == 0) {
    REXGPU_WARN("nx1_d3d9: IMGFENCE {} calls, {} armed, {} ms total | last ret={} "
                "pending={} fence={:08X} -> {:08X}",
                n, armed.load(), total / 1000000, ctx.r3.u32, pending_before, fence_before,
                nx1::d3d9::GuestRead32(base, kImgCacheFence));
  }
}

//=============================================================================
// ImageCache DMA copies
//=============================================================================
//
// The image cache moves texture data between pool slots WITH THE GPU: DmaCopy saves shader
// state, binds the source bytes as a vertex buffer, BeginExport()s the destination, and
// DrawVertices()es a shader that ferries the data across -- a memexport blit (read straight
// from the recompiled body). On console that lands in unified memory. Here it lands in the
// reference backend's GPU-side buffer, and CPU-visible guest RAM keeps whatever the slot held
// before -- which is the image cache's own DEFAULT-PIXEL placeholder for fresh slots
// (ImageCache_GetDefaultPixels): the identical structured "speckle" pattern measured across
// many textures at once. Memexport readback (readback_memexport) recovers most of these, which
// is exactly the improvement observed when it was enabled; this hook is the native,
// reference-free completion: mirror the move in CPU RAM ourselves.
//
// Mode 1 logs the arguments only. The byte count is believed to be one of the two
// ImageAllocInfo words (r5/r6) and NOTHING is written to guest memory until a logged run has
// confirmed which -- a wrong length here would corrupt the pool far worse than the bug.
// Mode 2 performs the mirror copy, page-wise through GuestPointer: image cache pointers are
// physical-mirror EAs, and a single bulk memcpy through `base +` arithmetic has already
// blacked the screen three times in this project's history.
REXCVAR_DEFINE_UINT32(nx1_d3d9_dmacopy_mirror, 2, "GPU",
                      "ImageCache_DmaCopy handling: 0=off, 1=log arguments, 2=log and mirror the "
                      "GPU copy in CPU-visible guest RAM (default). The blit is a VERBATIM byte "
                      "move, so this mirror is correct as written -- measured by snapshotting the "
                      "source at call time and comparing against the destination once the fence "
                      "retires: four full pairs 100% identical with matching byte histograms, and "
                      "11,918 of 14,500 sampled copies byte-exact. It also restores textures that "
                      "otherwise never reach CPU RAM at all (smoke grenade sprites).\n"
                      "\n"
                      "This cvar previously defaulted to 0 with a comment asserting the blit "
                      "TRANSFORMS layout in flight and that mirroring caused a block checkerboard. "
                      "Both claims were wrong. The 'transform' was inferred, never measured, then "
                      "quoted back as established; the checkerboard rested on a SINGLE screenshot "
                      "pair taken at different moments in a scene whose corruption varies "
                      "constantly. An earlier verification did appear to support a transform (24% "
                      "byte-exact) but compared LIVE guest memory 250 ms after the call, so it was "
                      "measuring the streaming pool recycling the source -- the same run reports "
                      "the source moving 24-49% while we waited. Snapshot the input to measure a "
                      "copy; do not compare against memory that anything else can write.");

// Owned by d3d9_resources.cpp. Needed here so the DMA path can report against the tracked
// texture -- without it, "no DMA touched this address" was true only because nothing looked.
REXCVAR_DECLARE(uint32_t, nx1_d3d9_dbg_track_addr);
REXCVAR_DECLARE(uint32_t, nx1_d3d9_dbg_track_bytes);
REXCVAR_DECLARE(bool, nx1_d3d9_dma_drop_clobbered);
REXCVAR_DECLARE(bool, nx1_d3d9_dma_move_verbatim);

REX_EXTERN(__imp__rex_ImageCache_DmaCopy);
REX_EXTERN(__imp__rex_ImageCache_DmaCopyDelayed_YAXPAEPBEUImageAllocInfo_Z);

namespace {

/// Mirror one image-cache move into CPU-visible guest RAM. Page-wise through GuestPointer --
/// these are physical-mirror EAs, and bulk `base +` arithmetic across them has burned this
/// project three times. Idempotent, so a delayed copy that is later flushed through the
/// immediate path simply copies the same bytes twice.
/// Is this DMA's SOURCE actually valid in CPU-visible RAM? The mirror assumes it is -- it
/// copies src->dst so the CPU sees what the GPU blit produced. But if the source is itself a
/// buffer the GPU filled (a previous memexport destination), CPU RAM holds stale bytes there
/// too and the mirror faithfully propagates garbage. That would explain why mirroring both DMA
/// paths, at verified sizes, changed nothing. Sampled, not exhaustive: zero-fraction and
/// distinct-byte count are enough to tell "never written" and "plausible texture data" apart.
bool ClassifyDmaSource(const uint8_t* base, uint32_t src, uint32_t bytes) {
  static std::atomic<uint64_t> total{0}, empty{0}, lowvar{0};
  const uint8_t* s = nx1::d3d9::GuestPointer(base, src);
  if (!s) {
    return false;
  }
  const uint32_t span = std::min(bytes, 4096u);
  uint32_t zeros = 0, distinct = 0;
  bool seen[256] = {};
  for (uint32_t i = 0; i < span; i += 3) {
    if (!s[i]) ++zeros;
    if (!seen[s[i]]) { seen[s[i]] = true; ++distinct; }
  }
  const uint32_t sampled = (span + 2) / 3;
  const uint64_t n = total.fetch_add(1, std::memory_order_relaxed) + 1;
  if (sampled && zeros * 100 / sampled >= 95) {
    empty.fetch_add(1, std::memory_order_relaxed);
  } else if (distinct < 32) {
    lowvar.fetch_add(1, std::memory_order_relaxed);
  }
  // WHERE do these copies land? Every destination sampled so far was physical 0x0B-0x0D,
  // while the textures that visibly speckle live at 0x10-0x17. If the DMA path never touches
  // that region, then mirroring it -- however correctly -- could never have fixed them, and
  // hours of work were aimed at the wrong memory. A 16 MB-region histogram settles it.
  if ((n % 2000) == 0) {
    REXGPU_WARN("nx1_d3d9: DMASRC {} copies | {} from EMPTY source, {} from low-variance "
                "source -- a high share means the mirror is propagating garbage, i.e. the data "
                "never reaches CPU RAM at all",
                n, empty.load(), lowvar.load());
  }
  // MEASURED at ~9.4% of copies (5,076 of 54,000): the source is empty in CPU-visible RAM.
  //
  // The image cache CHAINS these moves (A->B->C). On console every hop is the GPU writing
  // unified memory; here the GPU does not execute the blit, so an intermediate slot is empty
  // for us -- and mirroring it copies that emptiness onward, OVERWRITING whatever the
  // destination legitimately held. A copy with no data to move is not a copy worth making, and
  // performing it can only destroy. Skipping is strictly safer than propagating: at worst the
  // destination keeps what it had, which is what would have happened without the mirror at all.
  return sampled && zeros * 100 / sampled >= 95;
}

/// IS DmaCopy ACTUALLY A TRANSFORM? Everything written about this hook so far assumes it is --
/// that the blit re-tiles in flight, and that our verbatim memcpy therefore produced the
/// measured checkerboard. That is an INFERENCE, never a measurement, and it has been quoted
/// back as if it were established. It has a competing explanation with the same symptom: the
/// mirror may copy a source that is not valid in CPU RAM yet (ClassifyDmaSource was written for
/// exactly that worry), in which case the move is verbatim and our bug was TIMING, not layout.
///
/// Run with the reference rasterising and readback_memexport on: guest RAM then holds the real
/// post-blit bytes, so src and dst can simply be compared.
///   dst == src  -> verbatim move. The transform story is wrong; delete it. Fix the source's
///                  validity/timing instead.
///   dst != src  -> a real transform, and the pair is the ground truth to derive it from.
REXCVAR_DEFINE_BOOL(nx1_d3d9_dbg_dmachain, false, "GPU",
                    "Trace where the image-cache copy chain breaks: for every copy handed an "
                    "EMPTY source, report whether that source was itself a copy destination "
                    "(break is upstream) or an origin nothing ever filled");

REXCVAR_DEFINE_BOOL(nx1_d3d9_dbg_dmaverify, false, "GPU",
                    "Compare each DMA destination against its source AFTER the blit retires, to "
                    "settle whether ImageCache_DmaCopy transforms the layout or moves bytes "
                    "verbatim. Needs the reference rasterising so guest RAM holds real results");

/// DERIVE the blit instead of guessing at it. Four theories about what DmaCopy does have now
/// been advanced from inference and three have collapsed; this dumps the actual input and the
/// actual output so the mapping can be computed rather than argued.
///
/// MUST run with nx1_skip_reference_raster=false and readback_memexport=true: only then does the
/// reference execute the memexport draw and land the real result in guest RAM. (That config was
/// unusable before the readback map leak was fixed -- it ate ~100 MB/s.)
///
/// Writes texdump/dma_<dst>_<bytes>_src.bin and _dst.bin. Byte-identical files mean the move is
/// VERBATIM and our mirror's bug was timing; differing files are the ground truth to derive the
/// permutation from.
REXCVAR_DEFINE_UINT32(nx1_d3d9_dbg_dmadump, 0, "GPU",
                      "Dump the next N DMA source/destination pairs to texdump/ as raw .bin, for "
                      "deriving the image-cache blit's layout transform offline");

struct PendingDma {
  uint32_t dst, src, bytes;
  std::chrono::steady_clock::time_point at;
  /// The source AS IT WAS AT CALL TIME. Comparing live guest memory 250 ms later cannot
  /// distinguish "the blit transformed the data" from "the streaming pool recycled the source
  /// while we waited" -- that ambiguity has now muddied two measurements. Snapshotting here
  /// removes it: this is exactly the input the blit was handed.
  std::vector<uint8_t> src_snapshot;
};
std::mutex g_dma_verify_m;
std::deque<PendingDma> g_dma_verify;

void RecordPendingDma(const uint8_t* base, uint32_t dst, uint32_t src, uint32_t bytes) {
  if (!REXCVAR_GET(nx1_d3d9_dbg_dmaverify) || bytes < 256 || bytes > (16u << 20) || !dst || !src) {
    return;
  }
  const uint32_t snap_bytes = std::min(bytes, 256u << 10);
  std::vector<uint8_t> snap(snap_bytes);
  for (uint32_t off = 0; off < snap_bytes; off += 4096) {
    const uint32_t chunk = std::min(4096u, snap_bytes - off);
    const uint8_t* s = nx1::d3d9::GuestPointer(base, src + off);
    if (!s) {
      return;  // unreadable source -- a pair we cannot interpret is worse than no pair
    }
    std::memcpy(snap.data() + off, s, chunk);
  }
  std::lock_guard<std::mutex> lk(g_dma_verify_m);
  if (g_dma_verify.size() < 64) {  // each entry now carries up to 256 KB
    g_dma_verify.push_back(
        {dst, src, bytes, std::chrono::steady_clock::now(), std::move(snap)});
  }
}

/// Drained from the fence wait -- the guest's own "DMA retired" signal -- and only for entries
/// old enough that the reference's readback has had time to land, so a mismatch means a real
/// difference rather than a race against our own instrumentation.
void VerifyPendingDma(const uint8_t* base) {
  if (!REXCVAR_GET(nx1_d3d9_dbg_dmaverify)) {
    return;
  }
  const auto now = std::chrono::steady_clock::now();
  for (;;) {
    PendingDma e{};
    {
      std::lock_guard<std::mutex> lk(g_dma_verify_m);
      if (g_dma_verify.empty() ||
          now - g_dma_verify.front().at < std::chrono::milliseconds(250)) {
        return;
      }
      e = g_dma_verify.front();
      g_dma_verify.pop_front();
    }
    const uint32_t span = std::min<uint32_t>(e.bytes, uint32_t(e.src_snapshot.size()));
    uint32_t same = 0, compared = 0, src_moved = 0;
    int64_t first_bad = -1;
    for (uint32_t off = 0; off < span; off += 4096) {
      const uint32_t chunk = std::min(4096u, span - off);
      const uint8_t* d = nx1::d3d9::GuestPointer(base, e.dst + off);
      const uint8_t* s_live = nx1::d3d9::GuestPointer(base, e.src + off);
      if (!d) {
        break;
      }
      const uint8_t* s = e.src_snapshot.data() + off;  // the blit's ACTUAL input
      for (uint32_t i = 0; i < chunk; ++i) {
        ++compared;
        if (d[i] == s[i]) {
          ++same;
        } else if (first_bad < 0) {
          first_bad = off + i;
        }
        // Did the source itself change while we waited? If this is high, every earlier
        // live-memory comparison was measuring recycling, not the blit.
        if (s_live && s_live[i] != s[i]) {
          ++src_moved;
        }
      }
    }
    if (!compared) {
      continue;
    }
    // Dump the pair itself when asked. Prefer entries big enough to show a 2D pattern -- a
    // single 4 KB page can look like anything, while a full mip level makes a tiling obvious.
    if (const uint32_t dump_budget = REXCVAR_GET(nx1_d3d9_dbg_dmadump);
        dump_budget && e.bytes >= (16u << 10)) {
      REXCVAR_SET(nx1_d3d9_dbg_dmadump, dump_budget - 1);
      const uint32_t dump_bytes = std::min<uint32_t>(e.bytes, uint32_t(e.src_snapshot.size()));
      const std::vector<uint8_t>& sbuf = e.src_snapshot;  // input as the blit received it
      std::vector<uint8_t> dbuf(dump_bytes);
      bool ok = true;
      for (uint32_t off = 0; off < dump_bytes && ok; off += 4096) {
        const uint32_t chunk = std::min(4096u, dump_bytes - off);
        const uint8_t* d = nx1::d3d9::GuestPointer(base, e.dst + off);
        if (!d) {
          ok = false;
          break;
        }
        std::memcpy(dbuf.data() + off, d, chunk);
      }
      if (ok) {
        char path[256];
        std::snprintf(path, sizeof(path), "texdump/dma_%08X_%u_src.bin", e.dst, dump_bytes);
        if (FILE* f = std::fopen(path, "wb")) {
          std::fwrite(sbuf.data(), 1, sbuf.size(), f);
          std::fclose(f);
        }
        std::snprintf(path, sizeof(path), "texdump/dma_%08X_%u_dst.bin", e.dst, dump_bytes);
        if (FILE* f = std::fopen(path, "wb")) {
          std::fwrite(dbuf.data(), 1, dbuf.size(), f);
          std::fclose(f);
        }
        REXGPU_WARN("nx1_d3d9: DMADUMP dst={:08X} src={:08X} {} bytes -> texdump/dma_*_src.bin "
                    "and _dst.bin ({}% identical)",
                    e.dst, e.src, dump_bytes, same * 100 / compared);
      }
    }
    static std::atomic<uint64_t> n{0}, exact{0}, pct_sum{0};
    const uint32_t pct = same * 100 / compared;
    const uint64_t k = n.fetch_add(1, std::memory_order_relaxed) + 1;
    pct_sum.fetch_add(pct, std::memory_order_relaxed);
    if (pct == 100) {
      exact.fetch_add(1, std::memory_order_relaxed);
    }
    if (k <= 12 || (k % 500) == 0) {
      REXGPU_WARN("nx1_d3d9: DMAVERIFY dst={:08X} src={:08X} {} bytes | {}% identical, first "
                  "difference at {} | source itself moved {}% while we waited | running: {} "
                  "copies, {} byte-exact, {}% mean",
                  e.dst, e.src, e.bytes, pct, first_bad, src_moved * 100 / compared, k,
                  exact.load(), pct_sum.load() / k);
    }
  }
}

/// WHERE DOES THE CHAIN BREAK? ~9.4% of image-cache copies are handed a source that is EMPTY in
/// CPU-visible RAM, so the move cannot be mirrored and our view of the destination keeps its
/// PREVIOUS occupant while the guest believes the new texture is there. That is the surviving
/// explanation for the speckle: we decode the old occupant, faithfully, from memory that is
/// written, complete and stable.
///
/// The sources are themselves destinations of earlier copies, so the interesting question is
/// which link fails FIRST. Record every copy's destination; when a copy has an empty source, ask
/// whether that source was ever a destination we saw:
///   YES -> the break is upstream; that earlier copy was also unmirrorable, report its source.
///   NO  -> this source is an ORIGIN. Nothing in the DMA path ever filled it, so its data comes
///          from somewhere we do not observe at all (a resolve destination, or a load path that
///          writes GPU-side memory). That address is the thing to chase.
struct DmaOrigin {
  uint32_t src;
  uint32_t bytes;
  bool mirrored;  ///< false if THIS copy was itself skipped for an empty source
};
std::mutex g_dma_chain_m;
std::unordered_map<uint32_t, DmaOrigin> g_dma_chain;  // destination page -> what filled it

/// Record EVERY page a copy's destination covers, whether or not we could mirror it.
///
/// Two defects in the original made "zero upstream cases" unfalsifiable, and both bit exactly
/// the case the census exists to find:
///
///   1. A copy with an EMPTY source returned before recording its own destination. So when a
///      later copy read from that destination, the lookup missed and it was reported an ORIGIN
///      -- "nothing we see ever filled it". A PROPAGATING CHAIN BREAK, the single most likely
///      shape of this bug, was the one shape structurally invisible.
///   2. Only the destination's FIRST page was recorded, though a copy spans `bytes`. A source
///      pointing into the middle of an earlier destination missed. Live in the data: dst
///      ACD10000 against src ACD3A000 is 168 KB into that destination.
///
/// Recording the full span, with a flag for whether the fill actually happened, separates three
/// outcomes that the old census collapsed into "ORIGIN".
void RecordDmaDest(uint32_t dst, uint32_t src, uint32_t bytes, bool mirrored) {
  if (g_dma_chain.size() > 400000) {  // ~512 MB pool / 4 KB, so this is a runaway guard only
    return;
  }
  const uint32_t first = (dst & 0x1FFFFFFF) >> 12;
  const uint32_t last = ((dst & 0x1FFFFFFF) + (bytes ? bytes - 1 : 0)) >> 12;
  for (uint32_t p = first; p <= last && p - first < 4096; ++p) {
    g_dma_chain[p] = {src, bytes, mirrored};
  }
}

void NoteDmaChain(uint32_t dst, uint32_t src, uint32_t bytes, bool src_empty) {
  if (!REXCVAR_GET(nx1_d3d9_dbg_dmachain)) {
    return;
  }
  const uint32_t src_page = (src & 0x1FFFFFFF) >> 12;
  std::lock_guard<std::mutex> lk(g_dma_chain_m);
  if (src_empty) {
    static std::atomic<uint64_t> upstream{0}, origin{0}, reported{0}, up_skipped{0}, up_mirrored{0};
    const auto it = g_dma_chain.find(src_page);
    const bool known = it != g_dma_chain.end();
    (known ? upstream : origin).fetch_add(1, std::memory_order_relaxed);
    if (known) {
      // The decisive split. An upstream copy we SKIPPED means the break propagates through a
      // path we already observe -- fixable in the DMA path. An upstream copy we MIRRORED that
      // still reads empty means our own write did not land, which is a different bug entirely.
      (it->second.mirrored ? up_mirrored : up_skipped).fetch_add(1, std::memory_order_relaxed);
    }
    // Verdict census over EVERY empty source, not just the 24 detailed below: the detail lines
    // are the first ones to occur, which is a poor sample of a pattern that may only appear
    // once the streamer is busy.
    static std::atomic<uint64_t> v_never{0}, v_wrote{0}, v_notimage{0}, v_badfmt{0}, v_budget{0};
    {
      using WV = nx1::d3d9::ResourceTracker::WritebackVerdict;
      switch (nx1::d3d9::ResourceTracker::Get().ResolveDestVerdict(src & 0x1FFFFFFF, nullptr)) {
        case WV::kWrote: v_wrote.fetch_add(1, std::memory_order_relaxed); break;
        case WV::kNotImage: v_notimage.fetch_add(1, std::memory_order_relaxed); break;
        case WV::kBadFormat: v_badfmt.fetch_add(1, std::memory_order_relaxed); break;
        case WV::kOverBudget: v_budget.fetch_add(1, std::memory_order_relaxed); break;
        case WV::kNever: v_never.fetch_add(1, std::memory_order_relaxed); break;
      }
    }
    if (reported.fetch_add(1, std::memory_order_relaxed) < 24) {
      if (known) {
        REXGPU_WARN("nx1_d3d9: DMACHAIN dst={:08X} src={:08X} EMPTY -- that source was itself "
                    "the destination of a copy from {:08X} ({} bytes) which we {}, so the break "
                    "is UPSTREAM",
                    dst, src, it->second.src, it->second.bytes,
                    it->second.mirrored ? "DID mirror (yet it reads empty -- our write did not "
                                          "land where this copy reads)"
                                        : "SKIPPED for an empty source (the break propagates)");
      } else {
        // Has ANYTHING we can see ever written this page? Separates "written then cleared"
        // from "filled by a path outside our observation entirely".
        const uint32_t w0 = nx1::d3d9::ResourceTracker::Get().PageWriteCount(src);
        const uint32_t w1 = nx1::d3d9::ResourceTracker::Get().PageWriteCount(src + 4096);
        // Is this "origin" actually a resolve destination we DECLINED to write back? That is
        // the difference between a mechanism we do not observe and one we observe and skip --
        // and only the second is already fixable with code that exists.
        using WV = nx1::d3d9::ResourceTracker::WritebackVerdict;
        uint32_t rfmt = 0;
        const WV wv = nx1::d3d9::ResourceTracker::Get().ResolveDestVerdict(src & 0x1FFFFFFF, &rfmt);
        const char* why = "never a resolve destination either -- filled outside our observation";
        char buf[128];
        switch (wv) {
          case WV::kWrote:
            why = "a resolve destination we DID write back -- yet it reads empty, so the "
                  "writeback is not landing where the copy reads";
            break;
          case WV::kNotImage:
            why = "a resolve destination REJECTED as not-a-2D-image -- we declined to fill it";
            break;
          case WV::kBadFormat:
            std::snprintf(buf, sizeof(buf),
                          "a resolve destination REJECTED for unsupported format %u -- we "
                          "declined to fill it, and supporting that format would",
                          rfmt);
            why = buf;
            break;
          case WV::kOverBudget:
            why = "a resolve destination skipped as OVER BUDGET -- raise nx1_d3d9_writeback_max";
            break;
          case WV::kNever:
            break;
        }
        REXGPU_WARN("nx1_d3d9: DMACHAIN dst={:08X} src={:08X} EMPTY and NEVER a DMA destination "
                    "-- an ORIGIN. Observed writes to its first two pages: {} and {}. It is {}",
                    dst, src, w0, w1, why);
      }
    }
    // Record this copy's destination EVEN THOUGH we could not fill it. Omitting it is what made
    // a propagating chain break look like a field of unrelated origins.
    RecordDmaDest(dst, src, bytes, /*mirrored=*/false);
    // Every 200, not 2000: the previous run produced FEWER empty sources than the old threshold,
    // so the totals line -- the only unbiased view -- never printed at all and the conclusion had
    // to be drawn from the first 24 occurrences.
    if ((reported.load() % 200) == 0) {
      REXGPU_WARN("nx1_d3d9: DMACHAIN totals: {} empty sources traced UPSTREAM ({} to a copy we "
                  "skipped -- break propagates; {} to one we DID mirror -- our write missed), {} "
                  "are ORIGINS never written by any copy we see. As resolve destinations those "
                  "empty sources are: {} never resolved, {} written back (yet still empty), {} "
                  "rejected not-an-image, {} rejected bad format, {} over budget",
                  upstream.load(), up_skipped.load(), up_mirrored.load(), origin.load(),
                  v_never.load(), v_wrote.load(), v_notimage.load(), v_badfmt.load(),
                  v_budget.load());
    }
    return;
  }
  RecordDmaDest(dst, src, bytes, /*mirrored=*/true);
}

/// THE SOURCE MAY SIMPLY NOT BE FILLED YET.
///
/// Every ORIGIN reported by the chain census reads `first two pages: 1 and 1` -- uniformly ONE
/// observed write per page, never zero. The write-watch fires on a page's first write and then
/// leaves it writable, so "1" means "we saw it written once, then went blind". A page that was
/// written and still reads as zeros at copy time is consistent with exactly one thing: the write
/// we saw was the allocation's zero-fill, and the REAL fill has not happened yet.
///
/// That reframes the whole problem. The guest's DmaCopy only QUEUES a GPU blit; the blit executes
/// later, after whatever fills the staging buffer has run. Our mirror, by contrast, copies
/// IMMEDIATELY at hook time. So for a copy whose staging is still being filled we read zeros, skip
/// (correctly -- copying would blank the destination), and the destination keeps its previous
/// occupant. We then decode that previous occupant. Which is the speckle, exactly as described.
///
/// The test and the fix are the same code: queue the skipped copies and retry them later. If the
/// sources fill in, the timing theory is proven AND the data lands.
///
/// STALENESS GUARD. Writing a destination late is the hazard that has bitten this project before
/// (the forced re-decode ladder re-read recycled slots). Every copy stamps its destination pages
/// with a sequence number; a retry is abandoned if any newer copy has targeted the same pages,
/// so we can never land bytes over a fresher occupant.
REXCVAR_DEFINE_BOOL(nx1_d3d9_dma_retry, true, "GPU",
                    "Retry image-cache copies whose source was not yet filled at call time, "
                    "instead of dropping them. Off = the old drop-on-empty behaviour");
/// Raised from 400 ms once deferral became PER PAGE. A whole-copy deferral was waiting on a
/// staging buffer that fills in milliseconds; a per-page one waits on the streamer to deliver that
/// part of the texture, which can take far longer. Landing late is safe -- the destination stamp
/// makes a retry drop itself the moment a newer copy claims the same page -- so the only cost of a
/// longer window is queue occupancy.
REXCVAR_DEFINE_UINT32(nx1_d3d9_dma_retry_ms, 2000, "GPU",
                      "How long a deferred image-cache copy keeps retrying before it is "
                      "abandoned as genuinely empty");

struct DmaRetry {
  uint32_t dst, src, bytes, seq;
  std::chrono::steady_clock::time_point at;
  /// Did a LATER copy write into this deferred copy's SOURCE while it waited? See
  /// DrainDmaRetries -- filled in under the lock, acted on outside it.
  bool src_clobbered = false;
  uint32_t clobber_page = 0, clobber_seq = 0;
};
std::mutex g_dma_retry_m;
std::deque<DmaRetry> g_dma_retry;
std::unordered_map<uint32_t, uint32_t> g_dma_dest_seq;  // destination page -> latest copy seq
std::atomic<uint32_t> g_dma_seq{1};

/// Stamp every destination page this copy covers, so a later retry can tell whether it would be
/// writing over a fresher occupant. Independent of the debug census: correctness depends on it.
/// What the last mirrored copy into a given page actually covered. Recorded so the provenance
/// detector can answer the question its masks raise: when a texture decodes half-new/half-old,
/// did the copy that filled it cover the WHOLE texture or only part of it? A copy size smaller
/// than the texture's extent means we under-copied and left the rest holding the previous
/// occupant -- which is exactly the alternating page bands the masks show.
struct DmaCoverage {
  uint32_t base;   ///< destination base of that copy
  uint32_t bytes;  ///< size we mirrored
  uint32_t alt;    ///< the OTHER half of the packed ImageAllocInfo (hi when we used lo)
  /// SPANNED vs ACTUALLY WRITTEN -- the distinction the whole provenance argument turns on.
  /// `targeted` counts copies whose byte range covered this page; `wrote` counts the ones that
  /// really memcpy'd it. They differ because CopyLiveSourcePages SKIPS pages whose source is
  /// empty, so a copy can span a texture end to end and still leave pages holding the previous
  /// occupant. Without both numbers "covered" is unfalsifiable: it cannot be told apart from
  /// "covered and silently skipped", which is precisely the case under investigation.
  uint32_t targeted;
  uint32_t wrote;
};
std::mutex g_dma_cover_m;
std::unordered_map<uint32_t, DmaCoverage> g_dma_cover;  // page -> what covered it

bool LookupDmaCoverage(uint32_t phys, uint32_t* base, uint32_t* bytes, uint32_t* alt) {
  std::lock_guard<std::mutex> lk(g_dma_cover_m);
  const auto it = g_dma_cover.find(phys >> 12);
  if (it == g_dma_cover.end()) {
    return false;
  }
  *base = it->second.base;
  *bytes = it->second.bytes;
  *alt = it->second.alt;
  return true;
}

/// Per-PAGE verdict, which is the only form of this question worth asking. The caller that matters
/// is the provenance report, and it holds a list of FOREIGN pages -- asking whether a copy landed
/// on the texture's BASE page (which is what it used to do) answers a different question entirely
/// and was reporting "no mirrored copy recorded" for textures whose foreign pages sat 12 pages in.
/// Returns 0 = no copy ever spanned this page, 1 = spanned but never written (the empty-source
/// skip), 2 = actually written by a copy.
uint32_t DmaPageVerdict(uint32_t phys) {
  std::lock_guard<std::mutex> lk(g_dma_cover_m);
  const auto it = g_dma_cover.find(phys >> 12);
  if (it == g_dma_cover.end()) {
    return 0;
  }
  return it->second.wrote ? 2u : 1u;
}

/// Mark a page the mirror genuinely memcpy'd. Called from the copy loop, so it sees exactly the
/// pages that were NOT skipped for an empty source.
void MarkDmaPageWritten(uint32_t dst) {
  std::lock_guard<std::mutex> lk(g_dma_cover_m);
  const auto it = g_dma_cover.find((dst & 0x1FFFFFFF) >> 12);
  if (it != g_dma_cover.end()) {
    ++it->second.wrote;
  }
}

void NoteDmaCoverage(uint32_t dst, uint32_t bytes, uint32_t alt) {
  const uint32_t first = (dst & 0x1FFFFFFF) >> 12;
  const uint32_t last = ((dst & 0x1FFFFFFF) + (bytes ? bytes - 1 : 0)) >> 12;
  std::lock_guard<std::mutex> lk(g_dma_cover_m);
  // The 400,000-entry cap cannot bind for this title and is kept only as a runaway guard: the
  // destination histogram spans 16 distinct 16 MB regions, so the map tops out near 65,536 pages.
  // Counted anyway -- a silent cap is exactly the kind of instrument defect this file is full of.
  if (g_dma_cover.size() > 400000) {
    static std::atomic<uint64_t> capped{0};
    if ((capped.fetch_add(1, std::memory_order_relaxed) % 10000) == 0) {
      REXGPU_WARN("nx1_d3d9: DMACOVER map hit its {} entry cap -- coverage verdicts from here on "
                  "are UNRELIABLE (pages will read as 'never targeted' merely because we stopped "
                  "recording)",
                  g_dma_cover.size());
    }
    return;
  }
  for (uint32_t p = first; p <= last && p - first < 4096; ++p) {
    // Preserve `wrote` across re-targeting: the question is whether this page has EVER been
    // written by a mirrored copy, not whether the most recent one wrote it.
    auto& e = g_dma_cover[p];
    const uint32_t prev_wrote = e.wrote;
    e = {dst & 0x1FFFFFFF, bytes, alt, e.targeted + 1, prev_wrote};
  }
}

uint32_t StampDmaDest(uint32_t dst, uint32_t bytes) {
  const uint32_t seq = g_dma_seq.fetch_add(1, std::memory_order_relaxed);
  const uint32_t first = (dst & 0x1FFFFFFF) >> 12;
  const uint32_t last = ((dst & 0x1FFFFFFF) + (bytes ? bytes - 1 : 0)) >> 12;
  std::lock_guard<std::mutex> lk(g_dma_retry_m);
  if (g_dma_dest_seq.size() < 400000) {
    for (uint32_t p = first; p <= last && p - first < 4096; ++p) {
      g_dma_dest_seq[p] = seq;
    }
  }
  return seq;
}

/// Does ANY page of this source carry data? Bounded probe, used to decide whether a deferred copy
/// is ready. Deliberately NOT ClassifyDmaSource: that samples only the first 4 KB and calls the
/// source empty at >=95% zeros, which is a heuristic about the OPENING PAGE, not a fact about the
/// buffer.
bool SourceHasAnyData(const uint8_t* base, uint32_t src, uint32_t bytes) {
  const uint32_t pages = (bytes + 4095) / 4096;
  const uint32_t step = pages > 32 ? pages / 32 : 1;  // spread the probe over the whole buffer
  for (uint32_t p = 0; p < pages; p += step) {
    const uint32_t off = p * 4096;
    const uint32_t chunk = std::min(4096u, bytes - off);
    const uint8_t* s = nx1::d3d9::GuestPointer(base, src + off);
    if (!s) {
      return false;
    }
    for (uint32_t i = 0; i < chunk; ++i) {
      if (s[i]) {
        return true;
      }
    }
  }
  return false;
}

/// PARTIAL COPIES MAKE LATTICES. A DmaCopy means "make dst equal src over these bytes", and the
/// blit is verbatim (5ac1465). Landing only the pages that carry data leaves the rest holding the
/// PREVIOUS occupant, and in a tiled BC texture a 4 KB page is a rectangular block region -- so a
/// page-granular mixture renders as a regular grid of foreign tiles over otherwise-correct
/// texture. It also never heals: a rebuild faithfully decodes the mixture. That artifact was
/// observed directly (a lattice of squares across a wall that would not clear on approach) after
/// the 4 KB gate fix pushed ~35k more copies per run down this path.
///
/// The skip existed to avoid "erasing live texels" with an empty source, a worry from when sources
/// were believed to be routinely unfilled. They are not (PARTIALSRC 0%), and copying a genuinely
/// zero page writes zeros, which is CORRECT for a legitimately transparent region. So copy the
/// whole range, exactly as the GPU blit does.
/// DEFAULTED ON 2026-07-21. It was previously off because it CRASHED THE GAME, and that reason is
/// now stale -- read this before turning it back off.
///
/// The crash: verbatim made `InvalidateGuestRange` fire for every page rather than only non-empty
/// ones, ~8x the call rate, and that function then took NO LOCK while iterating the texture map and
/// could reallocate page_writes_ from the guest DMA thread while the render thread inserted into
/// the same map. The game died inside ImageCache_DmaCopyDelayed with a near-null read
/// (guest lr=824D3504, read fault 0x1B25). **That race was fixed in c661463**: InvalidateGuestRange
/// now only takes dirty_mu_ and pushes onto writes_pending_, with the dirtying moved to
/// DrainMemoryWrites on the render thread. 8x the traffic is now 8x a locked vector push, which is
/// cheap and safe. The crash rationale no longer applies.
///
/// WHY IT IS ON: the skip is self-amplifying. A MOVE's source is pool memory written by an EARLIER
/// copy, so skipping that earlier copy leaves the region empty and every later move reading it is
/// skipped too. Measured before the mechanism was understood: 357 of 1400 empty sources trace to an
/// earlier copy, and 350 of those 357 to a copy WE SKIPPED. Turning this on collapsed deferrals
/// from 500 landed / 1521 abandoned to 0 / 9, and removed the page-duplication, the one-page
/// displacement and the two-textures-reading-identical-fill signatures outright.
///
/// RESIDUAL CONCERN, judged acceptable rather than dismissed: the XDK documents the guest packing
/// data into the alignment gaps of texture allocations (XGAddress2DTiledExtent), so writing zeros
/// across a copy's whole range can clobber whatever the guest put in the gaps. The console's own
/// blit is a verbatim byte move over that same range, so the guest cannot be relying on anything
/// surviving inside it -- hardware would destroy it too. The sharper model is still to mirror only
/// the REFERENCED regions (XGGetTextureLayout returns them as a region list) rather than the
/// declared extent; that remains worth doing and is orthogonal to this flag.
///
/// NOTE the old help text was also wrong: this does not widen the copy's RANGE. The range is the
/// `bytes` argument either way; verbatim only stops SKIPPING all-zero source pages within it.
REXCVAR_DEFINE_BOOL(nx1_d3d9_dma_verbatim, true, "GPU",
                    "Copy every page of an image-cache copy, including all-zero source pages, as "
                    "the GPU blit does. Off skips empty source pages, which leaves the previous "
                    "occupant in place and starves later moves that read those pages");

/// `seq` is the destination stamp from StampDmaDest, so a page deferred here can be dropped if a
/// newer copy claims the same destination. Pass 0 from the retry drain itself (no re-deferral).
uint32_t CopyLiveSourcePages(uint8_t* base, uint32_t dst, uint32_t src, uint32_t bytes,
                             uint32_t seq) {
  bool verbatim = REXCVAR_GET(nx1_d3d9_dma_verbatim);
  // IS THIS A MOVE RATHER THAN A FILL?
  //
  // TRACK (run 054) caught the ImageCache compacting the pool: a 64 KB region walked DOWN by one
  // page as a chain of overlapping 16 KB copies, every one with src == dst + 0x1000. For those the
  // zero-page skip below is WRONG. Skipping is justified for a FILL from staging, where an empty
  // source page carries nothing and copying it would blank live texels. In a MOVE the zeros are
  // real content and the destination's old bytes are exactly what must be erased -- skip one page
  // and that page keeps its PRE-SHIFT content while its neighbours shift, which is a page-granular
  // displacement. The blit is measured as a verbatim byte move (11,918/14,500 copies byte-exact),
  // so zeros are data.
  //
  // Deferral is equally wrong here: a compaction chain is only correct applied IN ORDER, and
  // move N's source tail is move N+1's destination head (overlap exactly 4096 bytes). Deferring a
  // page past the next move makes it read bytes already relocated.
  const uint32_t dphys = dst & 0x1FFFFFFF, sphys = src & 0x1FFFFFFF;
  const bool is_move = bytes && dphys < sphys + bytes && sphys < dphys + bytes &&
                       REXCVAR_GET(nx1_d3d9_dma_move_verbatim);
  if (is_move) {
    verbatim = true;  // copy every page, defer nothing
    static std::atomic<uint64_t> moves{0};
    const uint64_t m = moves.fetch_add(1, std::memory_order_relaxed) + 1;
    if (m <= 8 || (m % 5000) == 0) {
      REXGPU_WARN("nx1_d3d9: DMAMOVE #{} dst={:08X} src={:08X} {} bytes (src-dst {:+d}) -- source "
                  "and destination OVERLAP, so this is a pool compaction: copying verbatim in "
                  "memmove order, skipping and deferring disabled",
                  m, dst, src, bytes, int64_t(sphys) - int64_t(dphys));
    }
  }
  uint32_t copied = 0;
  // MEMMOVE ORDER. Overlapping ranges must be walked away from the overlap: descending when the
  // destination is above the source, ascending otherwise. The observed compaction moves DOWN
  // (dst < src), which ascending already handled, but the opposite direction would have silently
  // eaten its own source a page at a time.
  const bool descending = is_move && dphys > sphys;
  const uint32_t npages = bytes ? (bytes + 4095) / 4096 : 0;
  for (uint32_t k = 0; k < npages; ++k) {
    const uint32_t off = (descending ? (npages - 1 - k) : k) * 4096;
    const uint32_t chunk = std::min(4096u, bytes - off);
    uint8_t* d = const_cast<uint8_t*>(nx1::d3d9::GuestPointer(base, dst + off));
    const uint8_t* s = nx1::d3d9::GuestPointer(base, src + off);
    if (!d || !s) {
      break;
    }
    bool any = false;
    for (uint32_t i = 0; i < chunk && !any; ++i) {
      any = s[i] != 0;
    }
    if (!any) {
      if (!verbatim) {
        // DEFER THIS PAGE. Skipping is right -- an empty source page carries nothing, and copying
        // it would blank live texels -- but skipping and FORGETTING is what leaves the previous
        // occupant's bytes in the slot forever. That is the alternating page bands the provenance
        // masks show, and why the artifact never heals.
        //
        // The existing retry could not catch this: it defers a copy only when SourceHasAnyData is
        // false for the WHOLE source, so a half-populated source passes and is copied with holes.
        // Deferral has to be per page, because that is the granularity the holes have.
        if (seq && REXCVAR_GET(nx1_d3d9_dma_retry)) {
          static std::atomic<uint64_t> dropped{0};
          std::lock_guard<std::mutex> lk(g_dma_retry_m);
          if (g_dma_retry.size() < 8192) {
            g_dma_retry.push_back(
                {dst + off, src + off, chunk, seq, std::chrono::steady_clock::now()});
          } else if ((dropped.fetch_add(1, std::memory_order_relaxed) % 5000) == 0) {
            REXGPU_WARN("nx1_d3d9: DMARETRY queue full, {} deferred pages dropped -- those slots "
                        "keep their previous occupant",
                        dropped.load());
          }
        }
        continue;
      }
      // VERBATIM BLANKS LIVE TEXELS. Copying an all-zero source page writes zeros over whatever
      // the destination held, and the destination may hold texels that already arrived -- the
      // effect measured earlier as a tracked texture going 8134/8192 nonzero -> 2033/8192. It
      // shows on screen as solid black banding, which is visibly worse in the verbatim
      // screenshots than the skipping ones. Counted so the harm is a number, not an impression.
      static std::atomic<uint64_t> blanked{0};
      const uint64_t k = blanked.fetch_add(1, std::memory_order_relaxed) + 1;
      if ((k % 20000) == 0) {
        REXGPU_WARN("nx1_d3d9: DMABLANK {} destination pages overwritten with ZEROS by verbatim "
                    "copying (empty source page). Each one erases whatever texels the slot already "
                    "held -- this is the black banding",
                    k);
      }
    }
    std::memcpy(d, s, chunk);
    // Record that THIS PAGE was really written, not merely spanned. Gated on the provenance
    // census because it is a locked map update per page copied, and the only consumer is that
    // report. See DmaCoverage::wrote.
    if (REXCVAR_GET(nx1_d3d9_dbg_pageorigin)) {
      MarkDmaPageWritten(dst + off);
    }
    nx1::d3d9::ResourceTracker::Get().InvalidateGuestRange((dst + off) & 0x1FFFFFFF, chunk);
    ++copied;
  }
  return copied;
}

/// Re-attempt deferred copies whose source has since filled in. Called from the DMA hook itself:
/// copies are frequent (~67k a run), so this paces naturally without a new thread or timer.
void DrainDmaRetries(uint8_t* base) {
  if (!REXCVAR_GET(nx1_d3d9_dma_retry)) {
    return;
  }
  const auto now = std::chrono::steady_clock::now();
  const auto max_age = std::chrono::milliseconds(REXCVAR_GET(nx1_d3d9_dma_retry_ms));
  static std::atomic<uint64_t> filled{0}, abandoned{0}, stale{0}, latency_us{0}, drains{0};
  /// Deferred copies whose SOURCE a later copy overwrote while they waited. See the check below.
  static std::atomic<uint64_t> clobbered{0};
  // BOUNDED SCAN, not head-of-line. Taking only the front meant one entry whose source never
  // fills blocked every entry behind it for the whole expiry window, throttling the retry to
  // roughly one copy per expiry -- which is indistinguishable, in the log, from a retry that
  // does not work at all.
  std::vector<DmaRetry> ready;
  {
    std::lock_guard<std::mutex> lk(g_dma_retry_m);
    size_t i = 0, examined = 0;
    while (i < g_dma_retry.size() && examined < 128) {
      const DmaRetry& e = g_dma_retry[i];
      ++examined;
      // A newer copy targeting the same destination means our bytes are obsolete: drop them
      // rather than overwrite a fresher occupant.
      const auto it = g_dma_dest_seq.find((e.dst & 0x1FFFFFFF) >> 12);
      if (it != g_dma_dest_seq.end() && it->second != e.seq) {
        stale.fetch_add(1, std::memory_order_relaxed);
        g_dma_retry.erase(g_dma_retry.begin() + i);
        continue;
      }
      if (SourceHasAnyData(base, e.src, e.bytes)) {
        // *** THE SOURCE-STALENESS CHECK. ***
        //
        // The guard above asks "did a newer copy write my DESTINATION". Nothing has ever asked
        // "did a newer copy write my SOURCE", and SourceHasAnyData only tests that the source is
        // non-empty -- never that it still holds the ORIGINAL bytes.
        //
        // That gap is aimed straight at pool compaction. TRACK (run 054) caught the ImageCache
        // moving a 64 KB region down by one page as a chain of OVERLAPPING 16 KB copies, each
        // with src == dst + 0x1000: move N's source tail IS move N+1's destination head, and the
        // overlap is exactly 4096 bytes. Defer a page of move N past move N+1 and it reads bytes
        // move N+1 already rewrote, then lands them at move N's destination -- an error of
        // EXACTLY ONE PAGE, which is the displacement proven by reproduction (15AFF000 held its
        // asset 4096 bytes from where its descriptor pointed).
        //
        // g_dma_dest_seq already carries what is needed: it stamps every destination page with
        // the seq of the last copy to target it. If any page of OUR SOURCE carries a seq higher
        // than ours, a later copy wrote it while we waited.
        DmaRetry r = e;
        const uint32_t sp0 = (e.src & 0x1FFFFFFF) >> 12;
        const uint32_t sp1 = ((e.src & 0x1FFFFFFF) + (e.bytes ? e.bytes - 1 : 0)) >> 12;
        for (uint32_t p = sp0; p <= sp1 && p - sp0 < 4096; ++p) {
          const auto sit = g_dma_dest_seq.find(p);
          // Unsigned wrap-safe ordering: seq only ever increases, so "newer" is a plain >.
          if (sit != g_dma_dest_seq.end() && sit->second > e.seq) {
            r.src_clobbered = true;
            r.clobber_page = p;
            r.clobber_seq = sit->second;
            break;
          }
        }
        ready.push_back(r);
        g_dma_retry.erase(g_dma_retry.begin() + i);
        continue;
      }
      if (now - e.at > max_age) {
        abandoned.fetch_add(1, std::memory_order_relaxed);
        g_dma_retry.erase(g_dma_retry.begin() + i);
        continue;
      }
      ++i;  // not ready yet, keep waiting
    }
  }
  for (const DmaRetry& e : ready) {
    // INSTRUMENTATION ONLY for now -- the copy still goes through, so this run's behaviour is
    // identical to the previous one and the count is a clean measurement rather than a change.
    // A nonzero count is the proof that the compaction hazard is real and not merely possible;
    // the fix (treat a MOVE as a verbatim ordered memmove, never skipped, never deferred) follows.
    //
    // ARMING: the total prints unconditionally, including zero. This investigation's repeated
    // failure is a counter that cannot fire being read as a counter reporting nothing.
    if (e.src_clobbered) {
      const uint64_t c = clobbered.fetch_add(1, std::memory_order_relaxed) + 1;
      if (c <= 16 || (c % 500) == 0) {
        REXGPU_WARN("nx1_d3d9: DMASRCCLOBBER #{} deferred copy dst={:08X} src={:08X} {} bytes "
                    "seq={} -- a LATER copy (seq {}) wrote source page {:05X} ({:08X}) while this "
                    "one waited{}",
                    c, e.dst, e.src, e.bytes, e.seq, e.clobber_seq, e.clobber_page,
                    e.clobber_page << 12,
                    REXCVAR_GET(nx1_d3d9_dma_drop_clobbered)
                        ? ", so it is DROPPED rather than planting relocated bytes"
                        : ", and it is being applied anyway (drop disabled)");
      }
      // MEASURED AT 43%: 101 of 236 landed retries had their source overwritten while queued
      // (run 055). The sampled cases are staging-source FILLS whose staging page was recycled and
      // handed to another texture -- so replaying the copy plants ANOTHER TEXTURE'S PAGE into this
      // one. That is the "foreign content" every provenance detector in this investigation kept
      // reporting; it did come from guest memory, just from a page that no longer belonged to us.
      //
      // Dropping is strictly better than applying. Leaving the destination alone keeps whatever it
      // had -- possibly stale, and the write-watch will re-dirty it when the real data lands --
      // whereas applying writes bytes we KNOW belong to someone else. Never plant known-wrong data.
      if (REXCVAR_GET(nx1_d3d9_dma_drop_clobbered)) {
        continue;
      }
    }
    // seq=0: a retry never re-defers itself. If the source is still not ready it will be
    // re-examined on the next drain until it expires.
    if (CopyLiveSourcePages(base, e.dst, e.src, e.bytes, 0)) {
      filled.fetch_add(1, std::memory_order_relaxed);
      latency_us.fetch_add(
          uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(now - e.at).count()),
          std::memory_order_relaxed);
    }
  }
  // Report on a fixed cadence regardless of how many landed. The previous every-500-landings
  // rule printed NOTHING when few or none landed, which is exactly the outcome worth seeing.
  if ((drains.fetch_add(1, std::memory_order_relaxed) % 20000) == 0) {
    const uint64_t k = filled.load();
    size_t queued = 0;
    {
      std::lock_guard<std::mutex> lk(g_dma_retry_m);
      queued = g_dma_retry.size();
    }
    // SRCCLOBBER prints even at zero, on purpose: a silent counter is indistinguishable from a
    // dead one.
    //
    // The wording here was WRONG in its first version and printed "63 of 13 landed retries",
    // because it compared the clobber count against `filled` -- and with dropping enabled a
    // clobbered retry is never applied, so it never reaches `filled` at all. The two are disjoint
    // outcomes, not a subset. Report them as disjoint.
    REXGPU_WARN("nx1_d3d9: DMARETRY SRCCLOBBER {} retries had their SOURCE overwritten while "
                "queued and were {} ({} other retries applied normally). Nonzero means a later "
                "copy recycled the staging page underneath a deferred copy",
                clobbered.load(),
                REXCVAR_GET(nx1_d3d9_dma_drop_clobbered) ? "DROPPED" : "applied anyway",
                filled.load());
    REXGPU_WARN("nx1_d3d9: DMARETRY {} deferred copies LANDED (mean {} us late), {} abandoned "
                "still-empty, {} dropped as stale, {} still queued. Landed>0 means the source "
                "was simply NOT YET FILLED at call time and we now recover it",
                k, k ? latency_us.load() / k : 0, abandoned.load(), stale.load(), queued);
  }
}

void MirrorDmaCopy(uint8_t* base, uint32_t dst, uint32_t src, uint32_t bytes) {
  // DOES ANY DMA ACTUALLY FILL THE TRACKED TEXTURE? A run tracking one permanently-77%-empty
  // texture logged only BIND and CACHED -- no writes, no copies -- which reads like "nothing ever
  // writes it". That conclusion was unsupported: this path had no tracking at all, so the absence
  // was guaranteed. It matters because the addresses differ by WINDOW, not by location: DMA
  // destinations are logged as 0xA/0xB-window EAs (B4789000) while textures are keyed physically
  // (12526000), and 0xB2526000 & 0x1FFFFFFF is exactly 12526000 -- the same memory. Compare
  // physically or the match can never happen.
  if (const uint32_t track = REXCVAR_GET(nx1_d3d9_dbg_track_addr); track && bytes) {
    const uint32_t tphys = track & 0x1FFFFFFF;
    const uint32_t dphys = dst & 0x1FFFFFFF;
    // RANGE OVERLAP, not "does the base address fall inside this copy".
    //
    // The old test was `tphys >= dphys && tphys < dphys + bytes`, which fires ONLY for a copy
    // covering the texture's BASE PAGE. Measured consequence: tracking a visibly corrupt wall
    // (10FAD000, run 053) produced 2901 TRACK lines and NOT ONE DMACOPY, which reads as "no copy
    // ever delivered this texture" when it only ever meant "no copy covered page 0".
    //
    // That blindness is aimed exactly at the artifact. Textures are 8-16 pages but copies are
    // 1-4 pages to scattered destinations (run 052's DMACOPY lines), so most of a texture is
    // delivered by copies that never touch its base page -- and the proven defect is a copy
    // landing ONE PAGE off (15AFF000 held its asset displaced by 4096 bytes). A predicate anchored
    // to the base page cannot see either.
    //
    // Track the whole texture span instead. nx1_d3d9_dbg_track_bytes defaults to 64 KB, which
    // covers every 256x256 BC texture in these captures; set it larger for bigger textures. The
    // log prints the copy's offset RELATIVE to the tracked base, because that offset is the
    // measurement: 0/4096/8192... means the copies tile the texture, and anything not a multiple
    // of 4096 -- or a repeat of an offset already seen -- is the bug, visible directly in the log.
    const uint32_t tspan = std::max(REXCVAR_GET(nx1_d3d9_dbg_track_bytes), 1u);
    if (dphys < tphys + tspan && tphys < dphys + bytes) {
      const int64_t rel = int64_t(dphys) - int64_t(tphys);
      REXGPU_WARN("nx1_d3d9: TRACK {:08X} DMACOPY dst={:08X} (phys {:08X}) src={:08X} {} bytes "
                  "-- lands on the tracked texture at offset {:+d} ({:+d} pages){}",
                  track, dst, dphys, src, bytes, rel, rel / 4096,
                  (rel % 4096) ? "  *** NOT PAGE ALIGNED ***" : "");
    }
  }
  // OUR-RENDERER ONLY. In pure Xenia mode the GPU-side buffer is authoritative for DMA
  // results; a CPU write here fires Xenia's watches and replaces the GPU's (possibly
  // TRANSFORMED) result with our raw byte copy -- observed as the reference going black.
  // That same possibility -- DmaCopy not being a verbatim move -- is an open question for
  // our own mirror's correctness.
  if (!nx1::d3d9::IsEnabled()) {
    return;
  }
  // SILENT DROPS. This gate returns without mirroring and without recording anything, so a copy
  // rejected here is indistinguishable downstream from a copy that never happened -- which is
  // exactly what the provenance cross-reference reports as "no mirrored copy recorded" for the
  // pages that render wrong. The 16 MB ceiling in particular would drop a large texture move
  // whole, leaving every page of it holding the previous occupant. Counted by reason so the
  // question is answered rather than assumed.
  if (bytes < 32 || bytes > (16u << 20) || !dst || !src) {
    static std::atomic<uint64_t> too_small{0}, too_big{0}, null_addr{0};
    if (!dst || !src) {
      null_addr.fetch_add(1, std::memory_order_relaxed);
    } else if (bytes < 32) {
      too_small.fetch_add(1, std::memory_order_relaxed);
    } else {
      const uint64_t k = too_big.fetch_add(1, std::memory_order_relaxed) + 1;
      if (k <= 8 || (k % 500) == 0) {
        REXGPU_WARN("nx1_d3d9: DMAGATE dropped a {} byte copy to {:08X} -- OVER the 16 MB mirror "
                    "ceiling, so every page of it keeps its previous occupant ({} such copies, {} "
                    "under-32, {} null)",
                    bytes, dst, k, too_small.load(), null_addr.load());
      }
    }
    return;
  }
  DrainDmaRetries(base);
  const uint32_t seq = StampDmaDest(dst, bytes);
  // Kept for its DMASRC census only. It must NOT gate the copy: it samples the first 4 KB and
  // declares the source empty at >=95% zeros, so a buffer whose OPENING page is sparse -- a
  // transparent border, a sparse atlas -- had its ENTIRE copy dropped, however much real data sat
  // in the pages behind it. Commit 305240b fixed the mirror image of this (opening page full,
  // later pages empty) and noted the 4 KB sampling without following it the other way.
  ClassifyDmaSource(base, src, bytes);
  // The exact test instead of the heuristic, and decided SEPARATELY from the copy: a source is
  // empty only when NO page anywhere in it carries data. Anything else is copied over its full
  // range -- landing part of a copy is what produces the lattice.
  const bool src_empty = !SourceHasAnyData(base, src, bytes);
  NoteDmaChain(dst, src, bytes, src_empty);
  if (src_empty) {
    static std::atomic<uint64_t> skipped{0};
    const uint64_t k = skipped.fetch_add(1, std::memory_order_relaxed) + 1;
    if ((k % 2000) == 0) {
      REXGPU_WARN("nx1_d3d9: DMASKIP {} mirrors skipped -- empty source, so the copy would only "
                  "have blanked a destination that may hold good texels",
                  k);
    }
    // Defer rather than discard: the source is very likely still being filled.
    if (REXCVAR_GET(nx1_d3d9_dma_retry)) {
      std::lock_guard<std::mutex> lk(g_dma_retry_m);
      if (g_dma_retry.size() < 4096) {
        g_dma_retry.push_back({dst, src, bytes, seq, std::chrono::steady_clock::now()});
      }
    }
    return;
  }
  {
    // Destination region histogram, keyed by physical high byte.
    static std::atomic<uint32_t> hist[256];
    static std::atomic<uint64_t> n{0};
    const uint32_t region = (dst & 0x1FFFFFFF) >> 24;
    hist[region].fetch_add(1, std::memory_order_relaxed);
    if ((n.fetch_add(1, std::memory_order_relaxed) % 20000) == 19999) {
      std::string out;
      for (uint32_t i = 0; i < 32; ++i) {
        const uint32_t c = hist[i].load(std::memory_order_relaxed);
        if (c) {
          out += fmt::format("{:02X}x:{} ", i, c);
        }
      }
      REXGPU_WARN("nx1_d3d9: DMADST regions {}", out);
    }
  }
  // PER-PAGE, not all-or-nothing. The whole-copy emptiness test above samples only the FIRST
  // 4 KB, so a copy whose opening page holds data while later pages are empty passes it and then
  // blanks the rest of the destination. Measured directly: a tracked texture went from
  // nonzero=8134/8192 to 2033/8192 -- data that had already arrived was destroyed by our own
  // mirror, which is the opposite of what this code exists to do.
  //
  // An empty source page carries nothing, so copying it can only erase. Skip those and leave the
  // destination holding whatever it legitimately had: the worst case is that we fail to propagate
  // a genuine zero-fill, which costs nothing, while the alternative demonstrably wipes live
  // texels. Counted so "the mirror is landing bytes" can be distinguished from "the mirror is
  // landing NOTHING because every page it was handed was blank".
  //
  // Invalidation happens per COPIED page inside the helper, never for the whole range: the
  // blanket call that used to follow this loop covered skipped pages too, so a copy whose source
  // was entirely empty still marked its destination "written" without writing anything -- which
  // let a never-written page pass the partial test and render as solid black instead of being
  // held back.
  static std::atomic<uint64_t> pages_copied{0}, pages_skipped{0};
  const uint32_t total_pages = (bytes + 4095) / 4096;
  const uint32_t copied = CopyLiveSourcePages(base, dst, src, bytes, seq);
  // Approximate: the helper also stops early on an untranslatable page, which counts here as
  // skipped. Both are "we did not land these bytes", which is what the counter is for.
  const uint32_t not_copied = total_pages - std::min(copied, total_pages);
  pages_copied.fetch_add(copied, std::memory_order_relaxed);
  const uint64_t before = pages_skipped.fetch_add(not_copied, std::memory_order_relaxed);
  if (not_copied && (before / 20000) != ((before + not_copied) / 20000)) {
    REXGPU_WARN("nx1_d3d9: DMAPAGESKIP {} empty source pages not copied ({} copied) -- each would "
                "have blanked a destination page that may hold live texels",
                before + not_copied, pages_copied.load());
  }
  // NOTE: invalidation now happens per COPIED page inside the loop above, not for the whole
  // range here. Landing the bytes is only half the job -- but claiming to have landed bytes we
  // skipped is worse than not claiming at all.
}

}  // namespace

namespace nx1::d3d9 {
/// Public forwarder so the texture decoder can ask what the last mirrored copy into an address
/// actually covered. Lives here because the coverage map is owned by the DMA hooks.
bool DmaCoverageFor(uint32_t phys, uint32_t* base_out, uint32_t* bytes_out, uint32_t* alt_out) {
  return LookupDmaCoverage(phys, base_out, bytes_out, alt_out);
}
uint32_t DmaPageVerdictFor(uint32_t phys) { return DmaPageVerdict(phys); }
}  // namespace nx1::d3d9

REX_HOOK_RAW(rex_ImageCache_DmaCopy) {
  const uint32_t dst = ctx.r3.u32, src = ctx.r4.u32, a5 = ctx.r5.u32, a6 = ctx.r6.u32;
  __imp__rex_ImageCache_DmaCopy(ctx, base);
#ifdef _WIN32
  RecordPendingDma(base, dst, src, a5);  // independent of the mirror mode
  const uint32_t mode = REXCVAR_GET(nx1_d3d9_dmacopy_mirror);
  if (!mode) {
    return;
  }
  // Volume counters, alongside the first-32 detail lines: the decisive comparison is DmaCopy
  // TRAFFIC between backend modes (nx1_d3d9 on vs off) for the same walk. The hook runs in
  // both modes, so identical walks produce directly comparable series -- if the pure-Xenia run
  // streams early and often where ours waits for approach, the streamer's behaviour itself
  // diverges by backend, and the far slots legitimately hold placeholder under us.
  static std::atomic<uint32_t> logged{0};
  static std::atomic<uint64_t> total_bytes{0};
  const uint32_t n = logged.fetch_add(1, std::memory_order_relaxed);
  const uint64_t tb = total_bytes.fetch_add(a5, std::memory_order_relaxed) + a5;
  if (n < 32) {
    REXGPU_WARN("nx1_d3d9: DMACOPY dst={:08X} src={:08X} a5={:08X} a6={:08X}", dst, src, a5, a6);
  }
  if ((n % 100) == 99) {  // fine-grained: the D3D9-mode run never even reached the old 500
    REXGPU_WARN("nx1_d3d9: DMACOPY volume {} calls, {} MiB total", n + 1, tb >> 20);
  }
  if (mode >= 2) {
    // a5 confirmed as the byte count by the logged run: clean page multiples, and dst/size
    // pairs that match the GPU-written census entry for entry.
    NoteDmaCoverage(dst, a5, a6);  // immediate path: a6 is the only other candidate size
    MirrorDmaCopy(base, dst, src, a5);
  }
#endif
}

// The DELAYED variant queues the move for a later batched flush. It is a separate entry point,
// and it is the leading explanation for the remaining distance-dependent speckle: the cache
// uses immediate copies for urgent (nearby) images and the delayed path for the rest, so
// mirroring only the immediate one healed exactly the surfaces you walk up to. Mirroring at
// QUEUE time is sound: the source staging is complete before the game queues it, and the flush
// re-copies the same bytes. The 8-byte ImageAllocInfo arrives packed in r5 (big-endian struct
// in a 64-bit register: size in the HIGH half) -- both halves are logged so a wrong guess is
// visible, and the copy takes whichever half passes the sanity gate.
REX_HOOK_RAW(rex_ImageCache_DmaCopyDelayed_YAXPAEPBEUImageAllocInfo_Z) {
  const uint32_t dst = ctx.r3.u32, src = ctx.r4.u32;
  const uint32_t hi = uint32_t(ctx.r5.u64 >> 32), lo = ctx.r5.u32;
  __imp__rex_ImageCache_DmaCopyDelayed_YAXPAEPBEUImageAllocInfo_Z(ctx, base);
#ifdef _WIN32
  RecordPendingDma(base, dst, src, lo);  // lo = copy size (hi is the slot allocation)
  const uint32_t mode = REXCVAR_GET(nx1_d3d9_dmacopy_mirror);
  if (!mode) {
    return;
  }
  static std::atomic<uint32_t> logged{0};
  if (logged.fetch_add(1, std::memory_order_relaxed) < 32) {
    REXGPU_WARN("nx1_d3d9: DMACOPY-DELAYED dst={:08X} src={:08X} hi={:08X} lo={:08X}", dst, src,
                hi, lo);
  }
  if (mode >= 2) {
    // LO is the copy size -- verified against the GPU-written census (dst ACAF8000 moves
    // exactly +3000, and lo=3000 where hi=5000). hi is the slot's ALLOCATION size; using it
    // overcopied past the image and trampled the neighbouring slot, which presented as some
    // textures healing while others newly broke.
    // Record BOTH halves: if the mixed textures turn out to need `hi`, the size choice
    // documented as verified for one destination is wrong for others.
    NoteDmaCoverage(dst, lo, hi);
    MirrorDmaCopy(base, dst, src, lo);
  }
#endif
}

