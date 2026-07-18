/**
 * @file    d3d9_renderer.h
 * @brief   Native D3D9 renderer for NX1 -- replaces the Xenia PM4 backend.
 *
 * NX1's engine (IW4-family `gfx_d3d`, per the shipped debug strings) targets an
 * API that is D3D9 plus Xenos extensions. Rather than emulating the GPU command
 * stream, we intercept the guest's D3D calls and drive a real IDirect3DDevice9.
 *
 * Interception strategy (see guest_d3d.h for why):
 *   - Guest D3D *setters* run unhooked. They are pure shadow-state writers and
 *     emit no PM4. Several are inlined into engine code and cannot be hooked at
 *     all, so trusting the shadow state is the only correct option.
 *   - We hook the *flush points* -- Draw*, Clear, Resolve, Swap -- plus
 *     GpuBeginShaderConstantF4 (which hands the engine a raw pointer into the
 *     PM4 ring) and the XGSet*Header resource-registration functions.
 *   - At each draw we resolve full state out of the guest D3D::CDevice and
 *     translate it to D3D9.
 *
 * Every hook falls through to the original recompiled body when the renderer is
 * disabled, so a default build is bit-identical to the current Xenia path.
 */

#pragma once

#include <cstdint>

#ifdef _WIN32
#include <atomic>
#include <chrono>
#include <mutex>
#include <vector>
#include <thread>

#include <d3d9.h>
#include <windows.h>
#endif

#include "d3d9_cmdbuf.h"
#include "guest_d3d.h"

namespace nx1::d3d9 {

struct Sm3Shader;

/// True when the native D3D9 renderer should service guest D3D calls.
/// Backed by the `nx1_d3d9` cvar; read once at startup.
bool IsEnabled();

#ifdef _WIN32

/// Owns the host D3D9 device and the translation caches.
///
/// Single-instance; the guest drives it from the render thread
/// (`RB_RenderThread` holds device thread-ownership around its work).
class Renderer {
 public:
  static Renderer& Get();

  /// Create the D3D9 device against `hwnd`. Idempotent.
  bool Initialize(HWND hwnd);
  void Shutdown();

  bool initialized() const { return device_ != nullptr; }
  IDirect3DDevice9Ex* device() const { return device_; }

  //--- Frame lifecycle -------------------------------------------------------
  void BeginFrame();
  void Present();

  //--- Draw ------------------------------------------------------------------
  /// Translate one guest draw. `guest_device` is the guest D3D::CDevice address.
  /// Reads shader/constant/fetch/sampler state out of guest memory.
  /// `prim_type` is the raw Xenos primitive type the guest passes to D3D.
  void DrawIndexed(const uint8_t* base, uint32_t guest_device, uint32_t prim_type,
                   uint32_t base_vertex_index, uint32_t start_index, uint32_t index_count);

  void Draw(const uint8_t* base, uint32_t guest_device, uint32_t prim_type,
            uint32_t start_vertex, uint32_t vertex_count);

  /// Translate a user-pointer indexed draw (D3DDevice_DrawIndexedVerticesUP): the
  /// vertex and index data live inline in guest memory, not in bound buffers. This
  /// is the 2D/UI path (R_DrawTessTechnique) -- the only draws that happen before a
  /// map is loaded, so the whole main menu goes through here.
  void DrawIndexedUP(const uint8_t* base, uint32_t guest_device, uint32_t prim_type,
                     uint32_t num_vertices, uint32_t index_count, uint32_t guest_index_addr,
                     uint32_t index_format, uint32_t guest_vertex_addr, uint32_t vertex_stride);

  /// Translate a user-pointer non-indexed draw (D3DDevice_DrawVerticesUP).
  void DrawUP(const uint8_t* base, uint32_t guest_device, uint32_t prim_type,
              uint32_t vertex_count, uint32_t guest_vertex_addr, uint32_t vertex_stride);

  /// Handle a guest D3DDevice_ClearF. `flags` are Xenos clear bits (see kClear*),
  /// `rect_addr` is a guest _D3DRECT (0 = whole viewport) and `color_addr` a guest
  /// __vector4 of floats.
  void Clear(const uint8_t* base, uint32_t flags, uint32_t rect_addr, uint32_t color_addr, float z,
             uint32_t stencil);

  /// Bind the host target standing in for a guest render-target surface. The guest
  /// renders the world, its shadow maps, the bloom chain and the final composite into
  /// *different* surfaces; without this they all collapse onto one backbuffer and the
  /// shadow passes corrupt the depth the world pass tests against.
  void SetRenderTarget(const uint8_t* base, uint32_t index, uint32_t guest_surface);
  void SetDepthStencil(const uint8_t* base, uint32_t guest_surface);

  /// Handle a guest EDRAM->texture resolve: capture the current render target into
  /// the host texture the resolve destination maps to. `dest_texture` is the guest
  /// D3DBaseTexture address; `src_rect` is the guest _D3DRECT address (0 = whole);
  /// `dest_point` is the guest D3DPOINT address the band lands at (0 = origin).
  ///
  /// A resolve also *clears* EDRAM when `flags` carries D3DRESOLVE_CLEARRENDERTARGET /
  /// D3DRESOLVE_CLEARDEPTHSTENCIL, and NX1 clears the frame no other way -- it never
  /// calls D3DDevice_Clear. `clear_color` is the guest address of a __vector4 of floats.
  void Resolve(const uint8_t* base, uint32_t dest_texture, uint32_t src_rect,
               uint32_t dest_point, uint32_t flags, uint32_t clear_color, float clear_z,
               uint32_t clear_stencil);

  /// Capture one draw into the command buffer. Records only what the guest may overwrite
  /// before a worker consumes it -- see d3d9_cmdbuf.h for what is deliberately left out.
  void RecordDraw(const uint8_t* base, uint32_t guest_device, uint32_t prim_type,
                  uint32_t base_vertex_index, uint32_t start_index, uint32_t index_count);

  /// Fill everything in `d` that does not come from the draw call's own arguments. Split out of
  /// RecordDraw so the UP paths -- which execute inline from transient guest memory and never
  /// enter the command buffer -- can build a scratch RecordedDraw on the stack and share one
  /// definition of "what a draw's state is". `d`'s draw parameters must already be set:
  /// surface_key is derived from them.
  void CaptureDrawState(const uint8_t* base, uint32_t guest_device, RecordedDraw& d);

  /// Snapshot m_Pending.m_Mask[5] before the guest's draw body flushes and zeroes it. Must be
  /// called from the draw hook BEFORE __imp__, which is the only moment these bits are valid.
  void CaptureGuestDirtyMask(const uint8_t* base, uint32_t guest_device);

  /// Bind the SM3 shaders the record already resolved and upload their constants from the
  /// record. Returns false when resolution found no usable pair -- the draw must be skipped.
  bool BindShadersAndConstants(const RecordedDraw& d);

  /// Record-time half: hash the guest's microcode, look up the SM3 translations, and RESOLVE
  /// the constant values into the frame's pool. Must run on the guest thread -- the PM4 ring and
  /// the shader literals it reads are both transient (see ShaderCache::ResolveConstants).
  /// Leaves d.vs null on a cache miss, which is how the executor learns to skip the draw.
  void ResolveShadersAndConstants(const uint8_t* base, uint32_t guest_device, RecordedDraw& d);

 private:
  /// Translate and submit one recorded draw.
  ///
  /// STEP 2 of deferred translation. This runs SYNCHRONOUSLY on the guest thread for now --
  /// DrawIndexed records the draw and immediately executes it -- so it buys no speed at all.
  /// What it buys is a single path: there is no "live" variant reading guest memory alongside a
  /// "recorded" one that might disagree, so a field missing from RecordedDraw shows up as a
  /// rendering bug NOW, with the guest thread still parked and no threading in the picture.
  ///
  /// `base`/`guest_device` are still passed because the readers are being converted
  /// incrementally; every remaining use is a capture gap and is marked DEFERRED-GAP. The step is
  /// complete when none are left, and only then can this move to a worker.
  void ExecuteDraw(const uint8_t* base, uint32_t guest_device, const RecordedDraw& d);

  /// Execute a recorded clear. Split from Clear() so the Xenos -> D3DCLEAR_* mapping happens in
  /// COMMAND ORDER: it depends on whether a depth-stencil is bound, and a SetDepthStencil earlier
  /// in the same list may have changed that. Deciding it at record time would bake in the state
  /// as of recording rather than as of execution -- the exact class of error the ordered command
  /// list exists to prevent.
  void ExecuteClear(const RecordedCommand& c);

  /// Mirror the guest's vertex streams and bind them plus a host vertex declaration.
  /// `needed_vertices` bounds how much of each stream is mirrored (0 = all of it); see
  /// ResourceTracker::GetVertexBuffer. `vertex_count` receives the shortest stream's length.
  ///
  /// Takes no guest_device: every piece of STATE it needs comes from the record. `base` is only
  /// for bulk vertex data, which a worker reads late by design -- and dropping the parameter is
  /// what stops that quietly regressing, since re-reading device state would no longer compile.
  bool BindStreams(const uint8_t* base, const RecordedDraw& d, uint32_t needed_vertices,
                   uint32_t* vertex_count);
  /// Untile + bind every bound texture and its sampler state. No guest_device, for the same
  /// reason as BindStreams: `base` is only for texture data.
  void BindTextures(const uint8_t* base, const RecordedDraw& d, uint64_t surface_key = 0);
  /// The copy half of a resolve (EDRAM -> host texture). Caller holds render_mutex_.
  void ResolveCopy(const uint8_t* base, const RecordedCommand& c);
  /// The clear half of a resolve: wipe the bound colour target and/or depth-stencil,
  /// as the Xbox 360 does when a resolve carries the D3DRESOLVE_CLEAR* flags. Caller
  /// holds render_mutex_.
  void ClearEdram(const RecordedCommand& c);

  /// Execute halves of the remaining ordered commands. SetDepthStencil in particular MUST run in
  /// command order: it sizes the depth surface against current_rt_width_/height_, which the
  /// SetRenderTarget command before it sets.
  void ExecuteSetRenderTarget(const uint8_t* base, const RecordedCommand& c);
  void ExecuteSetDepthStencil(const uint8_t* base, const RecordedCommand& c);
  void ExecuteResolve(const uint8_t* base, const RecordedCommand& c);

  /// Translate the guest's depth/blend/cull GPU-register shadows to D3D9.
  void ApplyRenderStates(const RecordedDraw& d);
  /// Resolve the guest viewport into a host D3D9 viewport + the NDC scale/offset
  /// the translated vertex shaders fold in. Sets the D3D9 viewport as a side
  /// effect and stashes ndc_scale_/ndc_offset_ for UploadVertexUniforms.
  void ResolveViewport(const RecordedDraw& d);
  /// Upload the packed NDC/half-pixel params a vertex shader expects at c[base_reg].
  /// Skips registers in the shader's def mask (see Sm3Shader::def_mask).
  void UploadVertexUniforms(const Sm3Shader& shader, uint32_t base_reg);
  /// Upload the alpha-test uniforms a pixel shader expects at c[base_reg], c[base_reg+1].
  /// `needs_inv_tex_dim` gates the g_InvTexDim[16] block. Every write skips registers in the
  /// shader's def mask: fxc parks `def` literals in ANY declared register whose uses were
  /// optimized away -- writing over one poisons every flattened cmp in the shader.
  void UploadPixelUniforms(const Sm3Shader& shader, uint32_t base_reg, bool needs_inv_tex_dim,
                           const RecordedDraw& d);

  Renderer() = default;
  ~Renderer() { Shutdown(); }
  Renderer(const Renderer&) = delete;
  Renderer& operator=(const Renderer&) = delete;

  /// Own the output window on a dedicated thread that runs its own message loop.
  /// Win32 delivers a window's messages only to the thread that created it, and
  /// the guest presents/draws from the render-backend thread -- a different one --
  /// so pumping in Present() left the window unpumped and Windows marked it "Not
  /// responding". This thread creates the window, publishes its HWND, and pumps.
  void WindowThreadMain(uint32_t width, uint32_t height);

  IDirect3D9Ex* d3d_ = nullptr;
  IDirect3DDevice9Ex* device_ = nullptr;
  HWND hwnd_ = nullptr;
  // True only when we created hwnd_ ourselves (the fallback path). When we borrow
  // the rex host window we must neither pump nor destroy it -- rex owns it.
  bool owns_window_ = false;
  std::thread window_thread_;

  // Teardown safety. Every entry point the guest ring/GPU thread calls (draws,
  // Clear, Resolve, BeginFrame, Present) takes render_mutex_ and bails if
  // shutting_down_ is set; Shutdown() sets the flag then takes the same lock
  // before releasing the device, so it never frees the device out from under an
  // in-flight draw (a cross-thread use-after-free crashes the GPU driver).
  std::mutex render_mutex_;
  std::atomic<bool> shutting_down_{false};
  std::atomic<HWND> window_ready_hwnd_{nullptr};
  std::atomic<bool> window_ready_{false};
  uint32_t backbuffer_width_ = 0;
  uint32_t backbuffer_height_ = 0;

  // Size of the colour target currently bound to slot 0. The viewport and the clear
  // are relative to *this*, not the backbuffer: a shadow map is 1024x2048, and
  // clamping its viewport to the 1080-tall backbuffer would clip it in half.
  uint32_t current_rt_width_ = 0;
  uint32_t current_rt_height_ = 0;

  // Guest-clip-space -> host-clip-space fold, resolved per draw from the guest
  // viewport registers and applied inside the translated vertex shaders.
  float ndc_scale_[3] = {1.0f, 1.0f, 1.0f};
  float ndc_offset_[3] = {0.0f, 0.0f, 0.0f};

  uint64_t shader_cache_misses_ = 0;

  /// Sampler slots the currently bound shaders actually declare, latched by
  /// BindShadersAndConstants for BindTextures. 0xFFFF = "unknown, bind everything".
  uint32_t active_sampler_mask_ = 0xFFFFu;

  /// Signature of the last draw's texture binding: the raw fetch constants of the slots the
  /// shaders declare, plus the declared-slot mask and the LOD surface key. When a draw matches
  /// the one before it, every texture and sampler state it would bind is already on the device,
  /// so the whole per-slot loop -- a best-texture map lookup, the clamp-mode reads, the filter
  /// derivation and eight state compares per slot -- is skipped.
  ///
  /// Safe against the age sweep: a skip only ever chains back to a draw that DID bind, in this
  /// same frame, which refreshed the cache entries' last_frame. Reset every frame so that chain
  /// always starts with a real bind. Safe against mid-frame rebuilds too: entries are dirtied by
  /// DrainMemoryWrites at the frame boundary, never between two draws.
  static constexpr uint32_t kSigMaxDwords = 16 * 6;
  uint32_t last_sig_[kSigMaxDwords] = {};
  uint32_t last_sig_dwords_ = 0;
  uint32_t last_sig_mask_ = 0;
  uint64_t last_sig_surface_ = 0;
  bool last_sig_valid_ = false;
  uint64_t prof_bind_skips_ = 0;
  uint64_t prof_bind_calls_ = 0;

  /// FEASIBILITY MEASUREMENT for deferred translation. Two questions decide whether a worker
  /// thread can work at all:
  ///
  /// 1. WHERE does the guest's ~9.5 ms live? If it is all before the first draw, deferring
  ///    within a frame overlaps with nothing and only cross-frame pipelining would help --
  ///    which needs draw data to survive into the next frame, a far stronger assumption.
  /// 2. Is draw-referenced guest memory STABLE until end of frame? A worker consuming it late
  ///    reads whatever the engine has since written. This is the assumption that kills the
  ///    design if false.
  std::chrono::steady_clock::time_point prof_frame_start_{};
  std::chrono::steady_clock::time_point prof_last_draw_end_{};
  bool prof_saw_draw_ = false;
  uint64_t prof_gap_before_first_ns_ = 0;  ///< frame start -> first draw (pre-draw logic)
  uint64_t prof_gap_between_ns_ = 0;       ///< summed gaps between draws (interleaved logic)
  uint64_t prof_gap_after_last_ns_ = 0;    ///< last draw -> present (post-draw work)

  /// Vertex ranges a draw referenced this frame, re-hashed at Present to see whether the
  /// engine rewrote them behind us.
  /// Which memory class a probe covers. The original measurement only ever sampled VERTEX
  /// ranges, but the executor reads index data and shader microcode late on the same argument --
  /// which is analogy, not evidence. Each class translates differently (microcode is a GPU
  /// physical address, the other two are guest physical), so the kind has to travel with it.
  enum class ProbeKind : uint32_t { kVertex, kIndex, kUcode };
  struct StabilityProbe {
    uint32_t addr;
    uint32_t bytes;
    uint64_t hash;
    ProbeKind kind;
  };
  std::vector<StabilityProbe> prof_stability_;
  uint64_t prof_stable_ok_ = 0;
  uint64_t prof_stable_changed_ = 0;
  /// Per-class tallies, so "index data is stable" is a number rather than an assumption.
  uint64_t prof_stable_ok_kind_[3] = {};
  uint64_t prof_stable_changed_kind_[3] = {};
  void ProbeStability(ProbeKind kind, uint32_t addr, uint32_t bytes);
  /// Splits the shaders phase outside UploadConstants: microcode hashing, cache lookup, and the
  /// SetShader/uniform/colour-mask binding around them.
  uint64_t prof_hash_ns_ = 0;
  uint64_t prof_lookup_ns_ = 0;
  uint64_t prof_shbind_ns_ = 0;
  /// Hit rates for the vertex declaration / stream-source shadows. The phase total did not
  /// move when they went in, and a shadow that never hits is pure overhead -- so measure it
  /// rather than keep it on faith.
  uint64_t prof_decl_skips_ = 0;
  uint64_t prof_decl_calls_ = 0;
  uint64_t prof_stream_skips_ = 0;
  uint64_t prof_stream_calls_ = 0;
  /// The guest's constant dirty masks, captured in the draw hook before its body flushes and
  /// zeroes them. A clear mask means the guest wrote no constants for that stage since the
  /// last draw, so a previous upload of the same shader is still valid.
  uint64_t guest_dirty_vs_ = ~uint64_t(0);
  uint64_t guest_dirty_ps_ = ~uint64_t(0);
  /// What the last skippable upload was keyed on. The ring generation is essential: the dirty
  /// mask only covers writes to the SHADOW, while GpuBeginShaderConstantF4 writes go to the
  /// PM4 ring and leave it clear.
  const Sm3Shader* last_const_vs_ = nullptr;
  const Sm3Shader* last_const_ps_ = nullptr;
  uint64_t last_const_ring_gen_ = ~uint64_t(0);
  uint64_t prof_const_skipped_vs_ = 0;
  uint64_t prof_const_skipped_ps_ = 0;

  /// Guest dirty-mask statistics: how often each of m_Pending.m_Mask[5] is clear, and how
  /// often it is unchanged from the previous draw. These bound how much per-draw state
  /// resolution could be skipped outright.
  CommandBuffer cmdbuf_;
  uint64_t prof_record_ns_ = 0;

  uint64_t prof_mask_clear_[5] = {};
  uint64_t prof_mask_same_[5] = {};
  uint64_t prof_last_mask_[5] = {};
  /// Draws whose index range continues the previous draw's -- the batchable fraction.
  uint64_t prof_contiguous_draws_ = 0;
  uint32_t prof_last_index_end_ = 0xFFFFFFFFu;
  uint32_t prof_last_prim_type_ = 0xFFFFFFFFu;
  uint64_t prof_vlayout_ns_ = 0;
  uint64_t prof_vbuffer_ns_ = 0;

  /// TEMP PROFILING (nx1_d3d9_profile): per-phase nanoseconds accumulated over a frame,
  /// reported and reset in Present. Measures where the ~19us/draw actually goes.
  uint64_t prof_viewport_ns_ = 0;
  uint64_t prof_shaders_ns_ = 0;
  uint64_t prof_indices_ns_ = 0;
  uint64_t prof_streams_ns_ = 0;
  uint64_t prof_textures_ns_ = 0;
  uint64_t prof_states_ns_ = 0;
  uint64_t prof_draw_ns_ = 0;
  uint64_t prof_present_ns_ = 0;
  uint64_t prof_draws_ = 0;
  uint64_t prof_sampler_slots_ = 0;
  uint64_t prof_frame_ns_ = 0;


  // The guest depth surface currently bound, so a depth resolve knows which host
  // (INTZ) texture to publish under the resolve's destination address.
  uint32_t current_depth_surface_ = 0;

  // The guest never binds the display as a render target: it draws into EDRAM
  // surfaces and *resolves* the finished frame to one of two display buffers. So the
  // last display-sized resolve is the frame to show -- Present blits it to the
  // backbuffer. Without this, everything renders correctly into offscreen targets and
  // the window stays empty.
  uint32_t display_resolve_addr_ = 0;

  // Reported periodically from Present so a black window can be told apart from an idle
  // one. draws_attempted_ counts every guest draw we saw; draws_submitted_ counts the ones
  // that reached DrawIndexedPrimitive/DrawPrimitive.
  uint64_t frames_presented_ = 0;
  uint64_t draws_attempted_ = 0;
  uint64_t draws_submitted_ = 0;


  // Shadow of what is actually bound to each sampler, so BindTextures can skip the D3D
  // calls that would not change anything -- see BindTextures.
  //
  // Both shadows start at a value no real binding can take, so the first draw always writes
  // through: 0 is a legal D3DSAMP_* value, and nullptr is a legal texture binding, so
  // zeroing either would let a draw skip a call D3D never actually received.
  //
  // Anything that binds a texture, sampler state, shader or the colour-write mask outside
  // BindTextures/BindShadersAndConstants MUST call InvalidateStateShadow() --
  // ResourceTracker's depth blit does exactly that (sampler 0, both shader stages and
  // D3DRS_COLORWRITEENABLE, none of which it restores), and silently desyncing the shadow
  // against it corrupted every later draw that happened to want the texture the shadow
  // still believed was bound.
  static constexpr uint32_t kSamplerStates = 8;  ///< U, V, W address + mag, min, mip filter, aniso, sRGB
  static constexpr uint32_t kSamplerStateUnset = ~0u;
  /// Clamped to the adapter's D3DCAPS9::MaxAnisotropy at device creation; 1 = no aniso.
  uint32_t max_anisotropy_ = 1;
  static inline IDirect3DBaseTexture9* const kSamplerTextureUnknown =
      reinterpret_cast<IDirect3DBaseTexture9*>(~uintptr_t(0));
  IDirect3DBaseTexture9* sampler_texture_[16];
  uint32_t sampler_state_[16][kSamplerStates];

  /// The bound shader pair and colour-write mask, shadowed on the same principle as the
  /// sampler state above: BindShadersAndConstants re-issued all three on EVERY draw, and at
  /// ~5000 draws a frame that was ~1.4 ms of D3D calls that overwhelmingly changed nothing.
  /// Sentinels again pick values a real binding cannot take -- nullptr is a legal pixel
  /// shader (the depth-only pass) and 0 a legal colour mask -- so the first draw writes
  /// through rather than trusting a zeroed shadow.
  static inline IDirect3DVertexShader9* const kVertexShaderUnknown =
      reinterpret_cast<IDirect3DVertexShader9*>(~uintptr_t(0));
  static inline IDirect3DPixelShader9* const kPixelShaderUnknown =
      reinterpret_cast<IDirect3DPixelShader9*>(~uintptr_t(0));
  static constexpr uint32_t kColorWriteUnset = ~0u;
  IDirect3DVertexShader9* bound_vs_ = kVertexShaderUnknown;
  IDirect3DPixelShader9* bound_ps_ = kPixelShaderUnknown;
  uint32_t bound_color_write_ = kColorWriteUnset;

  /// The vertex declaration and stream sources, shadowed on the same principle. Note the UP
  /// draw paths (DrawPrimitiveUP / DrawIndexedPrimitiveUP) set stream 0 to NULL inside the
  /// runtime and bind their own declaration, so they invalidate this rather than track it --
  /// they are a handful of inline draws a frame and not worth the bookkeeping.
  static inline IDirect3DVertexDeclaration9* const kVertexDeclUnknown =
      reinterpret_cast<IDirect3DVertexDeclaration9*>(~uintptr_t(0));
  static inline IDirect3DVertexBuffer9* const kVertexBufferUnknown =
      reinterpret_cast<IDirect3DVertexBuffer9*>(~uintptr_t(0));
  IDirect3DVertexDeclaration9* bound_decl_ = kVertexDeclUnknown;
  IDirect3DVertexBuffer9* bound_stream_vb_[4];
  uint32_t bound_stream_stride_[4];

  /// Last state uploaded by UploadVertexUniforms / UploadPixelUniforms. These registers sit
  /// above each shader's constant window, so only a different shader's window can disturb
  /// them -- hence the shader-identity guard alongside the value compare. Pointers are stable
  /// because ShaderMap is node-based (see d3d9_shaders.cpp).
  const Sm3Shader* last_vs_uniform_shader_ = nullptr;
  float last_vs_uniform_params_[8] = {};
  const Sm3Shader* last_ps_uniform_shader_ = nullptr;
  float last_ps_alpha_[2] = {};
  float last_ps_dims_[16 * 4] = {};
  uint32_t last_ps_dims_mask_ = 0;

  /// Shadowed D3DRS_* values. ApplyRenderStates issued twelve SetRenderState calls on EVERY
  /// draw with no change detection -- ~70k of the ~85k D3D calls a frame, and consecutive
  /// draws overwhelmingly share depth/blend/cull state. Same treatment as the sampler, shader
  /// and stream shadows. Indexed by D3DRENDERSTATETYPE, which is < 256 for everything we set.
  static constexpr uint32_t kMaxRenderState = 256;
  static constexpr uint32_t kRenderStateUnset = ~0u;
  uint32_t render_state_[kMaxRenderState];

  /// Set a render state only when it differs from what the device already has.
  void SetRenderStateCached(D3DRENDERSTATETYPE state, uint32_t value);

  /// Forget the shadowed render states, so the next draw re-issues them all.
  void InvalidateRenderStateShadow();

  /// Forget the vertex declaration and stream sources only. Split out because the UP draw
  /// paths disturb exactly those and nothing else.
  void InvalidateVertexShadow();

  /// Forget what we think is bound to the samplers, shader stages and vertex streams, so the
  /// next draw re-issues all of it.
  void InvalidateStateShadow();

  // The guest colour surface currently bound to MRT slot 0.
  uint32_t current_rt_surface_ = 0;
};

/// The window the D3D9 device renders into. Prefers the real rex host window
/// (Runtime::display_window); falls back to enumerating this process's own
/// top-level windows if the runtime hasn't published one yet.
HWND FindGameWindow();

#endif  // _WIN32

}  // namespace nx1::d3d9
