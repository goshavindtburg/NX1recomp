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
#include <mutex>
#include <thread>

#include <d3d9.h>
#include <windows.h>
#endif

namespace nx1::d3d9 {

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

  void Clear(uint32_t flags, uint32_t color, float z, uint32_t stencil);

  /// Handle a guest EDRAM->texture resolve: capture the current render target into
  /// the host texture the resolve destination maps to. `dest_texture` is the guest
  /// D3DBaseTexture address; `src_rect` is the guest _D3DRECT address (0 = whole).
  void Resolve(const uint8_t* base, uint32_t dest_texture, uint32_t src_rect);

  /// Resolve the guest's bound shaders, bind their SM3 translations, and upload
  /// their constants. Returns false on a cache miss -- the draw must be skipped.
  bool BindShadersAndConstants(const uint8_t* base, uint32_t guest_device);

 private:
  /// Mirror the guest's vertex streams and bind them plus a host vertex
  /// declaration. `vertex_count` receives the shortest stream's length.
  bool BindStreams(const uint8_t* base, uint32_t guest_device, uint32_t* vertex_count);
  /// Untile + bind every bound texture and its sampler state.
  void BindTextures(const uint8_t* base, uint32_t guest_device, bool bound_draw = false);
  /// Translate the guest's depth/blend/cull GPU-register shadows to D3D9.
  void ApplyRenderStates(const uint8_t* base, uint32_t guest_device, bool bound_draw = false);
  /// Resolve the guest viewport into a host D3D9 viewport + the NDC scale/offset
  /// the translated vertex shaders fold in. Sets the D3D9 viewport as a side
  /// effect and stashes ndc_scale_/ndc_offset_ for UploadVertexUniforms.
  void ResolveViewport(const uint8_t* base, uint32_t guest_device);
  /// Upload the packed NDC/half-pixel params a vertex shader expects at c[base_reg].
  void UploadVertexUniforms(uint32_t base_reg);
  /// Upload the alpha-test uniforms a pixel shader expects at c[base_reg], c[base_reg+1].
  void UploadPixelUniforms(uint32_t base_reg, const uint8_t* base, uint32_t guest_device);
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

  // Guest-clip-space -> host-clip-space fold, resolved per draw from the guest
  // viewport registers and applied inside the translated vertex shaders.
  float ndc_scale_[3] = {1.0f, 1.0f, 1.0f};
  float ndc_offset_[3] = {0.0f, 0.0f, 0.0f};

  // Last guest device seen by a draw/clear, so BeginFrame can read the EDRAM
  // tiling clear values (the game clears through tiling, not D3DDevice_Clear).
  const uint8_t* last_base_ = nullptr;
  uint32_t last_guest_device_ = 0;

  uint64_t shader_cache_misses_ = 0;

  // Diagnostics, reported periodically from Present so a black window can be told
  // apart from an idle one. draws_attempted_ counts every guest draw we saw;
  // draws_submitted_ counts the ones that reached DrawIndexedPrimitive/DrawPrimitive.
  uint64_t frames_presented_ = 0;
  uint64_t draws_attempted_ = 0;
  uint64_t draws_submitted_ = 0;
};

/// Best-effort discovery of the game's top-level window.
/// TODO(d3d9): plumb the real `rex::ui::Window` handle through instead.
HWND FindGameWindow();

#endif  // _WIN32

}  // namespace nx1::d3d9
