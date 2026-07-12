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

#include <cstdint>

#include <rex/hook.h>
#include <rex/logging/macros.h>
#include <rex/ppc/context.h>

#include "d3d9_renderer.h"
#include "guest_d3d.h"

//=============================================================================
// Original recompiled bodies
//=============================================================================

REX_EXTERN(__imp__rex_D3DDevice_DrawIndexedVertices);
REX_EXTERN(__imp__rex_D3DDevice_DrawVertices);
REX_EXTERN(__imp__rex_D3DDevice_DrawIndexedVerticesUP);
REX_EXTERN(__imp__rex_D3DDevice_DrawVerticesUP);
REX_EXTERN(__imp__rex_D3DDevice_Clear);
REX_EXTERN(__imp__rex_D3DDevice_Resolve);
REX_EXTERN(__imp__rex_D3DDevice_Swap);
REX_EXTERN(__imp__rex_D3DDevice_GpuBeginShaderConstantF4);
REX_EXTERN(__imp__rex_XGSetVertexBufferHeader);
REX_EXTERN(__imp__rex_XGSetIndexBufferHeader);
REX_EXTERN(__imp__rex_XGSetTextureHeader);

namespace {

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
#endif
  __imp__rex_D3DDevice_DrawIndexedVertices(ctx, base);
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
#endif
  __imp__rex_D3DDevice_DrawVertices(ctx, base);
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
#endif
  __imp__rex_D3DDevice_DrawIndexedVerticesUP(ctx, base);
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
#endif
  __imp__rex_D3DDevice_DrawVerticesUP(ctx, base);
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
// D3DDevice_Clear(pDevice, Count, pRects, Flags, Color, Z, Stencil, ...):
//   r3 = pDevice   r4 = Count    r5 = pRects   r6 = Flags
//   r7 = Color     f1 = Z (double, first FP arg)   r8 = Stencil
// We ignore the rect list and clear the whole target.

REX_HOOK_RAW(rex_D3DDevice_Clear) {
#ifdef _WIN32
  const uint32_t flags = ctx.r6.u32, color = ctx.r7.u32, stencil = ctx.r8.u32;
  const float z = float(ctx.f1.f64);
#endif
  __imp__rex_D3DDevice_Clear(ctx, base);
#ifdef _WIN32
  if (EnsureRenderer()) {
    nx1::d3d9::Renderer::Get().Clear(flags, color, z, stencil);
  }
#endif
}

//=============================================================================
// Resolve (EDRAM -> texture)
//=============================================================================
//
// D3DDevice_Resolve(pDevice, Flags, pSourceRect, pDestTexture, pDestPoint, ...):
//   r3 = pDevice   r4 = Flags   r5 = pSourceRect   r6 = pDestTexture
// The render target ("EDRAM") is our backbuffer; capture the source rect into the
// host texture the destination address maps to, so later samples of it work.

REX_HOOK_RAW(rex_D3DDevice_Resolve) {
#ifdef _WIN32
  const uint32_t dest_texture = ctx.r6.u32, src_rect = ctx.r5.u32;
#endif
  __imp__rex_D3DDevice_Resolve(ctx, base);
#ifdef _WIN32
  if (EnsureRenderer()) {
    nx1::d3d9::Renderer::Get().Resolve(base, dest_texture, src_rect);
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
#endif
}

//=============================================================================
// Shader constants -- GPU fast path
//=============================================================================
//
// D3DDevice_GpuBeginShaderConstantF4(pDevice, PixelShader, StartRegister, Count)
// returns a raw pointer *into the PM4 ring*, pre-stamped with a 0xC0002D00
// SET_CONSTANT header; the engine then writes float4s directly there.
// GpuEndShaderConstantF4 is a 4-byte no-op, so there is no "commit" to hook.
//
// TODO(d3d9): hand back a guest-visible scratch buffer instead, record
// (PixelShader, StartRegister, Count, scratch_addr), and fold it into the shadow
// constant file at the next draw. Passing through for now keeps the ring valid.

REX_HOOK_RAW(rex_D3DDevice_GpuBeginShaderConstantF4) {
  __imp__rex_D3DDevice_GpuBeginShaderConstantF4(ctx, base);
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
