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
#include <string>

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
  __imp__rex_R_RunCommandBuffer_YAXPBUGfxBackEndData_IPAUGfxDrawListIter_PBUGfxViewport_PBD_Z(ctx,
                                                                                              base);
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

REX_HOOK_RAW(rex_ImageCache_WaitFence_YAHXZ) {
  static std::atomic<uint64_t> calls{0}, ns{0}, armed{0};
  const uint32_t pending_before = nx1::d3d9::GuestRead32(base, kImgCachePendingFlag);
  const uint32_t fence_before = nx1::d3d9::GuestRead32(base, kImgCacheFence);
  if (pending_before) {
    armed.fetch_add(1, std::memory_order_relaxed);
  }
  const auto t0 = std::chrono::steady_clock::now();
  __imp__rex_ImageCache_WaitFence_YAHXZ(ctx, base);
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
REXCVAR_DEFINE_UINT32(nx1_d3d9_dmacopy_mirror, 1, "GPU",
                      "ImageCache_DmaCopy handling: 0=off, 1=log arguments (first 32), "
                      "2=log and mirror the GPU copy in CPU-visible guest RAM");

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
void ClassifyDmaSource(const uint8_t* base, uint32_t src, uint32_t bytes) {
  static std::atomic<uint64_t> total{0}, empty{0}, lowvar{0};
  const uint8_t* s = nx1::d3d9::GuestPointer(base, src);
  if (!s) {
    return;
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
}

void MirrorDmaCopy(uint8_t* base, uint32_t dst, uint32_t src, uint32_t bytes) {
  // OUR-RENDERER ONLY. In pure Xenia mode the GPU-side buffer is authoritative for DMA
  // results; a CPU write here fires Xenia's watches and replaces the GPU's (possibly
  // TRANSFORMED) result with our raw byte copy -- observed as the reference going black.
  // That same possibility -- DmaCopy not being a verbatim move -- is an open question for
  // our own mirror's correctness.
  if (!nx1::d3d9::IsEnabled()) {
    return;
  }
  if (bytes < 32 || bytes > (16u << 20) || !dst || !src) {
    return;
  }
  ClassifyDmaSource(base, src, bytes);
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
  for (uint32_t off = 0; off < bytes; off += 4096) {
    const uint32_t chunk = std::min(4096u, bytes - off);
    uint8_t* d = const_cast<uint8_t*>(nx1::d3d9::GuestPointer(base, dst + off));
    const uint8_t* s = nx1::d3d9::GuestPointer(base, src + off);
    if (!d || !s) {
      return;
    }
    std::memcpy(d, s, chunk);
  }
  // The write above bypassed guest page protection, so nothing was invalidated by it. Tell the
  // tracker explicitly, or every texture cached over this range keeps serving its pre-copy
  // contents -- landing the bytes is only half the job.
  nx1::d3d9::ResourceTracker::Get().InvalidateGuestRange(dst & 0x1FFFFFFF, bytes);
}

}  // namespace

REX_HOOK_RAW(rex_ImageCache_DmaCopy) {
  const uint32_t dst = ctx.r3.u32, src = ctx.r4.u32, a5 = ctx.r5.u32, a6 = ctx.r6.u32;
  __imp__rex_ImageCache_DmaCopy(ctx, base);
#ifdef _WIN32
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
    MirrorDmaCopy(base, dst, src, lo);
  }
#endif
}

