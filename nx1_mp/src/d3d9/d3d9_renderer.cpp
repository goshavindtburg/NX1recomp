/**
 * @file    d3d9_renderer.cpp
 * @brief   Host D3D9 device management for the native NX1 renderer.
 */

// The rex headers must precede <windows.h> (pulled in by d3d9_renderer.h):
// rex/thread.h declares Sleep(std::chrono::milliseconds), which the Win32
// Sleep macro otherwise mangles.
#include <rex/cvar.h>
#include <rex/logging/macros.h>
#include <rex/system/kernel_state.h>

#include <algorithm>
#include <mutex>
#include <chrono>
#include <cstdio>
#include <unordered_set>
#include <vector>

#include <xxhash.h>

#include <d3dcompiler.h>

#include "d3d9_renderer.h"
#include "d3d9_constants.h"
#include "d3d9_resources.h"
#include "d3d9_shaders.h"
#include "guest_d3d.h"

REXCVAR_DEFINE_BOOL(nx1_d3d9, false, "GPU",
                    "Render NX1 through a native D3D9 device instead of the Xenia PM4 backend. "
                    "Launch-time only: the Xenia backend is disabled once at GPU init and the D3D9 "
                    "device is never torn down, so this cannot be toggled mid-run -- set it before "
                    "starting the game.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

// TEMP DIAGNOSTIC: the gamma work turned on D3DSAMP_SRGBTEXTURE for TextureSign::kGamma
// fetches, but many NX1 shaders linearize themselves (`mul r0.xyz, r0, r0`). If both happen the
// sample is decoded twice. Off = raw fetch, shaders' own decode only.
REXCVAR_DEFINE_BOOL(nx1_d3d9_srgb_tex, true, "GPU",
                    "Debug: honour TextureSign::kGamma with D3DSAMP_SRGBTEXTURE.");

// TEMP DIAGNOSTIC (no shadows): show the sun shadow lookup on the world, bypassing the guest
// pixel shader's p0 predication. Only applies to draws with the depth atlas on s5.
//   1 = stored atlas depth at the fragment's shadow UV   2 = same, x2 for visibility
//   3 = the fragment's own light-space depth             4 = frac(shadow UV) as red/green
//   5 = shadow UV unwrapped (>1 or <0 saturates => out of range)  6 = the lit/shadowed compare
//   7 = compare with wrapped u (tests "the sampler should REPEAT")
//   8 = compare with u halved  (tests "the u row's scale is 2x wrong")
//   9 = half-u UV as red/green (range check for 8)
// 10+ hijack only shader 4E127BDFABD81721 and show the CASCADE FADE -- the term that multiplies
// the shadow result to zero, and the reason forcing the atlas can never change the image:
//  10 = red = cascade 0 fade, green = cascade 1 fade (black => coords out of range => the bug)
//  11 = the raw shadow UV      12 = the shadow UV halved
REXCVAR_DEFINE_INT32(nx1_d3d9_dbg_shadow, 0, "GPU",
                     "Debug: replace the world pixel shader with the sun shadow lookup (1-9), or "
                     "show the cascade fade on one known shader (10-12).");

namespace nx1::d3d9 {

bool IsEnabled() {
  // Read live rather than caching in a static: a static local would latch
  // whatever the flag was on the very first call, which can precede config load.
  // (kRequiresRestart already means the value is stable within a run.)
  return REXCVAR_GET(nx1_d3d9);
}

#ifdef _WIN32

namespace {

struct FindWindowCtx {
  DWORD pid;
  HWND result;
};

BOOL CALLBACK EnumWindowsProc(HWND hwnd, LPARAM lparam) {
  auto* ctx = reinterpret_cast<FindWindowCtx*>(lparam);
  DWORD wnd_pid = 0;
  GetWindowThreadProcessId(hwnd, &wnd_pid);
  if (wnd_pid == ctx->pid && GetWindow(hwnd, GW_OWNER) == nullptr && IsWindowVisible(hwnd)) {
    ctx->result = hwnd;
    return FALSE;
  }
  return TRUE;
}

}  // namespace

HWND FindGameWindow() {
  FindWindowCtx ctx{GetCurrentProcessId(), nullptr};
  EnumWindows(EnumWindowsProc, reinterpret_cast<LPARAM>(&ctx));
  return ctx.result;
}

namespace {

LRESULT CALLBACK Nx1D3d9WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
  if (msg == WM_DESTROY) {
    PostQuitMessage(0);  // unblock the owning thread's GetMessage loop
    return 0;
  }
  return DefWindowProcW(hwnd, msg, wparam, lparam);
}

/// Create a dedicated top-level window for the D3D9 output. Keeping it separate
/// from the guest's window means our present can never fight the Xenia presenter
/// for the same HWND, and the two frames can be compared side by side.
HWND CreateOutputWindow(uint32_t width, uint32_t height) {
  static const wchar_t* kClassName = L"Nx1D3d9Output";
  static bool registered = false;
  HINSTANCE inst = GetModuleHandleW(nullptr);
  if (!registered) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = Nx1D3d9WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW));
    wc.lpszClassName = kClassName;
    RegisterClassExW(&wc);
    registered = true;
  }
  RECT r{0, 0, LONG(width), LONG(height)};
  AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
  HWND hwnd = CreateWindowExW(0, kClassName, L"NX1 D3D9 (nx1_d3d9)", WS_OVERLAPPEDWINDOW,
                              CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top,
                              nullptr, nullptr, inst, nullptr);
  if (hwnd) {
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
  }
  return hwnd;
}

}  // namespace

Renderer& Renderer::Get() {
  static Renderer instance;
  return instance;
}

bool Renderer::Initialize(HWND reference_window) {
  if (device_) {
    return true;
  }

  HRESULT hr = Direct3DCreate9Ex(D3D_SDK_VERSION, &d3d_);
  if (FAILED(hr) || !d3d_) {
    REXGPU_ERROR("nx1_d3d9: Direct3DCreate9Ex failed ({:#x})", static_cast<uint32_t>(hr));
    return false;
  }

  // Size to the guest window if we found one, else a sane default.
  RECT client{0, 0, 1280, 720};
  if (reference_window) {
    GetClientRect(reference_window, &client);
  }
  uint32_t width = uint32_t(client.right - client.left);
  uint32_t height = uint32_t(client.bottom - client.top);
  if (!width || !height) {
    width = 1280;
    height = 720;
  }

  // Our own window on its own thread -- never share the guest's HWND with the
  // Xenia presenter, and never leave it pumped from a foreign thread. Wait for the
  // thread to publish the HWND (or fail) before creating the device against it.
  window_thread_ = std::thread(&Renderer::WindowThreadMain, this, width, height);
  while (!window_ready_.load(std::memory_order_acquire)) {
    std::this_thread::yield();
  }
  HWND hwnd = window_ready_hwnd_.load(std::memory_order_acquire);
  if (!hwnd) {
    REXGPU_ERROR("nx1_d3d9: failed to create output window");
    if (window_thread_.joinable()) {
      window_thread_.join();
    }
    d3d_->Release();
    d3d_ = nullptr;
    return false;
  }

  D3DPRESENT_PARAMETERS pp{};
  pp.BackBufferWidth = width;
  pp.BackBufferHeight = height;
  // A8, not X8: a guest surface the size of the display is rendered into the backbuffer
  // directly, and the menus composite through *destination alpha*. With no alpha channel
  // the destination reads as 1.0, so a DESTALPHA/INVDESTALPHA pass writes its source
  // unmasked -- which is what painted the minimap and the menu map preview solid white.
  pp.BackBufferFormat = D3DFMT_A8R8G8B8;
  pp.BackBufferCount = 1;
  pp.MultiSampleType = D3DMULTISAMPLE_NONE;
  pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
  pp.hDeviceWindow = hwnd;
  pp.Windowed = TRUE;
  pp.EnableAutoDepthStencil = TRUE;
  pp.AutoDepthStencilFormat = D3DFMT_D24S8;
  pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;

  hr = d3d_->CreateDeviceEx(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
                            D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE |
                                D3DCREATE_MULTITHREADED,
                            &pp, nullptr, &device_);
  if (FAILED(hr) || !device_) {
    REXGPU_ERROR("nx1_d3d9: CreateDeviceEx failed ({:#x})", static_cast<uint32_t>(hr));
    d3d_->Release();
    d3d_ = nullptr;
    return false;
  }

  hwnd_ = hwnd;
  backbuffer_width_ = pp.BackBufferWidth;
  backbuffer_height_ = pp.BackBufferHeight;
  current_rt_width_ = pp.BackBufferWidth;
  current_rt_height_ = pp.BackBufferHeight;
  REXGPU_INFO("nx1_d3d9: device created ({}x{})", pp.BackBufferWidth, pp.BackBufferHeight);

  // The guest asks for anisotropy per sampler, but the Xenos ratios go to 16:1 and a host
  // adapter is free to cap lower -- MAXANISOTROPY above MaxAnisotropy is an invalid state.
  D3DCAPS9 caps = {};
  if (SUCCEEDED(device_->GetDeviceCaps(&caps)) &&
      (caps.TextureFilterCaps & D3DPTFILTERCAPS_MINFANISOTROPIC)) {
    max_anisotropy_ = caps.MaxAnisotropy < 1 ? 1 : caps.MaxAnisotropy;
  }
  REXGPU_INFO("nx1_d3d9: max anisotropy {}", max_anisotropy_);

  if (!ShaderCache::Get().Initialize(device_)) {
    REXGPU_ERROR("nx1_d3d9: shader cache unavailable; every draw would miss");
    Shutdown();
    return false;
  }
  ResourceTracker::Get().Initialize(device_);
  InvalidateSamplerShadow();

  // A guest colour target the size of the display *is* the backbuffer, so the final
  // composite lands where Present can show it instead of in an orphaned texture.
  {
    IDirect3DSurface9* back = nullptr;
    if (SUCCEEDED(device_->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back)) && back) {
      ResourceTracker::Get().SetBackbuffer(back, backbuffer_width_, backbuffer_height_);
      back->Release();  // the device keeps it alive; we only cache the pointer
    }
  }

  // Open the first scene now. The Swap hook is Present()-then-BeginFrame(), so
  // without this the draws issued before the first Swap would fall outside any
  // BeginScene/EndScene pair and D3D9 would silently drop them.
  BeginFrame();
  return true;
}

void Renderer::WindowThreadMain(uint32_t width, uint32_t height) {
  HWND hwnd = CreateOutputWindow(width, height);
  window_ready_hwnd_.store(hwnd, std::memory_order_release);
  window_ready_.store(true, std::memory_order_release);
  if (!hwnd) {
    return;
  }
  // Own the message loop for the life of the window. WM_DESTROY posts WM_QUIT,
  // which makes GetMessageW return 0 and ends the thread.
  MSG msg;
  while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }
}

void Renderer::Shutdown() {
  // Stop new render calls, then take render_mutex_ so any in-flight draw/present
  // on the ring thread finishes before we release the device. Releasing the
  // D3D9Ex device while another thread is mid-draw is a use-after-free that takes
  // the GPU driver down with it.
  shutting_down_.store(true, std::memory_order_release);
  {
    std::lock_guard<std::mutex> lock(render_mutex_);
    ResourceTracker::Get().Shutdown();
    ShaderCache::Get().Shutdown();
    if (device_) {
      device_->Release();
      device_ = nullptr;
    }
    if (d3d_) {
      d3d_->Release();
      d3d_ = nullptr;
    }
  }
  // DestroyWindow must run on the thread that created the window; ask it to close
  // (DefWindowProc -> DestroyWindow -> WM_DESTROY -> WM_QUIT) and let it exit. The
  // window thread only pumps messages -- it never touches render_mutex_ -- so join
  // it outside the lock.
  if (hwnd_) {
    PostMessageW(hwnd_, WM_CLOSE, 0, 0);
    hwnd_ = nullptr;
  }
  if (window_thread_.joinable()) {
    window_thread_.join();
  }
}

void Renderer::BeginFrame() {
  std::lock_guard<std::mutex> lock(render_mutex_);
  if (shutting_down_.load(std::memory_order_acquire) || !device_) {
    return;
  }
  // New frame: let the resource caches hash each texture/buffer at most once now.
  ResourceTracker::Get().AdvanceFrame();
  device_->BeginScene();

  // Nothing is cleared here. NX1 clears its colour and depth buffers through
  // D3DDevice_ClearF (see Renderer::Clear) and, where the guest asks for it, on the way
  // out of a resolve (Renderer::ClearEdram) -- always the target the guest names, at the
  // point it names it. A blanket clear at frame start would instead wipe whichever target
  // happened to be bound at swap time.
}

void Renderer::Present() {
  std::lock_guard<std::mutex> lock(render_mutex_);
  if (shutting_down_.load(std::memory_order_acquire) || !device_) {
    return;
  }
  device_->EndScene();

  // Copy the frame the guest resolved to its display buffer onto the backbuffer. The
  // guest renders into EDRAM surfaces and resolves out, so nothing it draws ever lands
  // on the backbuffer by itself.
  if (display_resolve_addr_) {
    IDirect3DTexture9* frame = ResourceTracker::Get().GetResolvedTexture(display_resolve_addr_);
    IDirect3DSurface9* src = nullptr;
    IDirect3DSurface9* back = nullptr;
    if (frame && SUCCEEDED(frame->GetSurfaceLevel(0, &src)) && src &&
        SUCCEEDED(device_->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back)) && back) {
      device_->StretchRect(src, nullptr, back, nullptr, D3DTEXF_LINEAR);
    }
    if (src) src->Release();
    if (back) back->Release();
  }

  device_->PresentEx(nullptr, nullptr, nullptr, nullptr, 0);

  // Heartbeat so a black window is diagnosable from the log: is the guest even
  // issuing draws (draws_attempted), and are any landing (draws_submitted)?
  if (++frames_presented_ % 600 == 0) {
    REXGPU_INFO("nx1_d3d9: frame {}, draws {}/{} submitted, {} shader-cache misses",
                frames_presented_, draws_submitted_, draws_attempted_, shader_cache_misses_);
    ResourceTracker::Get().LogCacheStats();
  }

  // The window's messages are pumped on its own thread (see WindowThreadMain), so
  // there is nothing to pump here -- doing it from this (render) thread would be a
  // no-op anyway: Win32 delivers a window's messages only to its creator thread.
}

void Renderer::Clear(const uint8_t* base, uint32_t flags, uint32_t rect_addr, uint32_t color_addr,
                     float z, uint32_t stencil) {
  std::lock_guard<std::mutex> lock(render_mutex_);
  if (shutting_down_.load(std::memory_order_acquire) || !device_) {
    return;
  }
  // Xenos clear flags, not desktop ones: bits 0-3 select colour targets 0-3, 0x10 is
  // depth, 0x20 is stencil. We bind a single colour target, so any of the four means
  // "clear it". Passing the guest's bits to D3D9 unmasked would fail the whole call.
  DWORD host_flags = 0;
  if (flags & kClearTargetAny) {
    host_flags |= D3DCLEAR_TARGET;
  }
  if (flags & kClearZBuffer) {
    host_flags |= D3DCLEAR_ZBUFFER;
  }
  if (flags & kClearStencil) {
    host_flags |= D3DCLEAR_STENCIL;
  }
  // D3D9 fails a depth or stencil clear outright when no depth-stencil is bound.
  if (!current_depth_surface_) {
    host_flags &= ~(D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL);
  }
  if (!host_flags) {
    return;
  }

  // The guest hands the clear colour as a __vector4 of floats, and an optional _D3DRECT
  // ({x1,y1,x2,y2}, the same layout D3D9 uses). Both the guest and D3D9 clip the clear
  // to the current viewport, so leave the viewport alone -- it is already the guest's.
  D3DCOLOR color = 0;
  if ((host_flags & D3DCLEAR_TARGET) && color_addr) {
    auto to8 = [](float c) { return uint32_t(std::clamp(c, 0.0f, 1.0f) * 255.0f + 0.5f); };
    color = D3DCOLOR_ARGB(to8(GuestReadF32(base, color_addr + 12)),  // a
                          to8(GuestReadF32(base, color_addr + 0)),   // r
                          to8(GuestReadF32(base, color_addr + 4)),   // g
                          to8(GuestReadF32(base, color_addr + 8)));  // b
  }
  D3DRECT rect = {};
  if (rect_addr) {
    rect.x1 = int32_t(GuestRead32(base, rect_addr + 0));
    rect.y1 = int32_t(GuestRead32(base, rect_addr + 4));
    rect.x2 = int32_t(GuestRead32(base, rect_addr + 8));
    rect.y2 = int32_t(GuestRead32(base, rect_addr + 12));
  }

  // z is the guest's clear value verbatim -- under NX1's reverse-Z that is 0.0 for
  // the far plane, which is exactly what the D3DCMP_GREATEREQUAL depth test wants.
  if (FAILED(device_->Clear(rect_addr ? 1 : 0, rect_addr ? &rect : nullptr, host_flags, color, z,
                            stencil)) &&
      (host_flags & D3DCLEAR_STENCIL)) {
    // A depth surface without stencil rejects D3DCLEAR_STENCIL, and D3D9 then fails the
    // whole call -- clearing nothing. Drop back rather than lose the depth clear.
    device_->Clear(rect_addr ? 1 : 0, rect_addr ? &rect : nullptr,
                   host_flags & ~D3DCLEAR_STENCIL, color, z, 0);
  }
}

void Renderer::SetRenderTarget(const uint8_t* base, uint32_t index, uint32_t guest_surface) {
  std::lock_guard<std::mutex> lock(render_mutex_);
  if (shutting_down_.load(std::memory_order_acquire) || !device_ || index >= 4) {
    return;
  }
  if (!guest_surface) {
    if (index) {
      device_->SetRenderTarget(index, nullptr);  // MRT slot 0 must always stay bound
    }
    return;
  }
  IDirect3DSurface9* surface =
      ResourceTracker::Get().GetRenderTargetSurface(base, guest_surface);
  if (surface) {
    device_->SetRenderTarget(index, surface);
    if (index == 0) {
      current_rt_surface_ = guest_surface;
      // Honour the surface's colour format: NX1 draws the world through a
      // k_8_8_8_8_GAMMA view (the hardware gamma-encodes on write) and its shaders,
      // post chain and scanout are all balanced around that. Writing linear instead
      // leaves the whole frame a gamma curve darker -- a permanent dusk.
      const uint32_t color_fmt =
          (GuestRead32(base, guest_surface + kSurfaceColorInfoOffset) >> 16) & 0xF;
      device_->SetRenderState(D3DRS_SRGBWRITEENABLE, color_fmt == 1 ? TRUE : FALSE);
      uint32_t w = 0, h = 0;
      if (ReadSurfaceSize(base, guest_surface, &w, &h)) {
        current_rt_width_ = w;
        current_rt_height_ = h;
        // Deliberately does *not* resize the depth still bound from the previous pass:
        // the guest always sets the target before its depth, and growing the old depth
        // here made every depth surface converge on the largest target (1920x2240),
        // which also breaks sampling the resolved shadow maps (their UVs assume the
        // real extent). SetDepthStencil sizes against this target instead.
      }
    }
  }
}

void Renderer::SetDepthStencil(const uint8_t* base, uint32_t guest_surface) {
  std::lock_guard<std::mutex> lock(render_mutex_);
  if (shutting_down_.load(std::memory_order_acquire) || !device_) {
    return;
  }
  current_depth_surface_ = guest_surface;
  if (!guest_surface) {
    device_->SetDepthStencilSurface(nullptr);
    return;
  }
  IDirect3DSurface9* surface = ResourceTracker::Get().GetDepthSurface(
      base, guest_surface, current_rt_width_, current_rt_height_);
  if (surface) {
    device_->SetDepthStencilSurface(surface);
  }
}

void Renderer::Resolve(const uint8_t* base, uint32_t dest_texture, uint32_t src_rect,
                       uint32_t dest_point, uint32_t flags, uint32_t clear_color, float clear_z,
                       uint32_t clear_stencil) {
  std::lock_guard<std::mutex> lock(render_mutex_);
  if (shutting_down_.load(std::memory_order_acquire) || !device_) {
    return;
  }
  ResolveCopy(base, dest_texture, src_rect, dest_point);
  // A depth resolve blits through the pipeline, binding a texture and sampler state on
  // sampler 0 and leaving it unbound afterwards. BindTextures' shadow cannot see that, so
  // tell it to stop trusting itself. Resolves are a handful per frame; re-issuing the
  // sampler state on the next draw costs nothing next to getting it wrong.
  InvalidateSamplerShadow();
  ClearEdram(base, flags, clear_color, clear_z, clear_stencil);
}

void Renderer::ResolveCopy(const uint8_t* base, uint32_t dest_texture, uint32_t src_rect,
                           uint32_t dest_point) {
  if (!dest_texture) {
    return;
  }
  const TextureFetchConstant dest = ReadBaseTextureFormat(base, dest_texture);
  if (!dest.base_address || !dest.width || !dest.height) {
    return;
  }
  // A depth resolve (k_24_8 / k_24_8_FLOAT) publishes the *currently bound* depth
  // target under the destination address: the guest renders a shadow map (or the
  // scene depth), resolves it, then samples it back while lighting. The host depth
  // buffer is an INTZ texture precisely so it can be sampled -- no copy needed, we
  // just point the address at it. This is what the world's lighting was missing.
  RECT rect{0, 0, 0, 0};  // empty => whole surface
  if (src_rect) {
    rect.left = int32_t(GuestRead32(base, src_rect + 0));
    rect.top = int32_t(GuestRead32(base, src_rect + 4));
    rect.right = int32_t(GuestRead32(base, src_rect + 8));
    rect.bottom = int32_t(GuestRead32(base, src_rect + 12));
  }
  // Where this tile lands in the destination. D3DPOINT is {x, y}.
  POINT at{0, 0};
  if (dest_point) {
    at.x = int32_t(GuestRead32(base, dest_point + 0));
    at.y = int32_t(GuestRead32(base, dest_point + 4));
  }

  if (dest.format == 22 || dest.format == 23) {
    auto& tracker = ResourceTracker::Get();
    if (IDirect3DTexture9* depth = tracker.GetDepthTexture(base, current_depth_surface_)) {
      tracker.ResolveDepth(dest.base_address, dest.width, dest.height, depth, rect, at);
    }
    return;
  }

  ResourceTracker::Get().ResolveColor(dest.base_address, dest.width, dest.height, rect, at);

  // A display-sized colour resolve is the finished frame; remember it for Present.
  if (dest.width == backbuffer_width_ && dest.height == backbuffer_height_) {
    display_resolve_addr_ = dest.base_address;
  }
}

void Renderer::ClearEdram(const uint8_t* base, uint32_t flags, uint32_t clear_color,
                          float clear_z, uint32_t clear_stencil) {
  // The Xbox 360 clears EDRAM on the way out of a resolve, and NX1 clears the frame no
  // other way -- D3DDevice_Clear is never called once. Without this the scene's colour
  // and depth buffers carry over frame to frame: under reverse-Z a stale depth buffer
  // still passes GREATEREQUAL for a stationary camera (same fragments, same depths), so
  // a static view looks right, but the moment the camera moves, geometry that lands
  // behind last frame's depth is rejected and last frame's colour stays on screen.
  DWORD host_flags = 0;
  if (flags & kResolveClearRenderTarget) {
    host_flags |= D3DCLEAR_TARGET;
  }
  if ((flags & kResolveClearDepthStencil) && current_depth_surface_) {
    host_flags |= D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL;
  }
  if (!host_flags) {
    return;
  }

  D3DCOLOR color = 0;
  if ((host_flags & D3DCLEAR_TARGET) && clear_color) {
    auto to8 = [](float c) { return uint32_t(std::clamp(c, 0.0f, 1.0f) * 255.0f + 0.5f); };
    color = D3DCOLOR_ARGB(to8(GuestReadF32(base, clear_color + 12)),  // a
                          to8(GuestReadF32(base, clear_color + 0)),   // r
                          to8(GuestReadF32(base, clear_color + 4)),   // g
                          to8(GuestReadF32(base, clear_color + 8)));  // b
  }

  // D3D9 clips Clear to the viewport, and the viewport still holds whatever the last
  // draw left (a 1024x600 scene or a 1024x2048 shadow band). Cover the whole target.
  D3DVIEWPORT9 saved = {};
  const bool have_saved = SUCCEEDED(device_->GetViewport(&saved));
  if (current_rt_width_ && current_rt_height_) {
    const D3DVIEWPORT9 full = {0, 0, current_rt_width_, current_rt_height_, 0.0f, 1.0f};
    device_->SetViewport(&full);
  }

  // A depth surface without stencil rejects D3DCLEAR_STENCIL, and D3D9 then fails the
  // whole call -- clearing nothing. Drop back to a depth-only clear rather than lose it.
  if (FAILED(device_->Clear(0, nullptr, host_flags, color, clear_z, clear_stencil)) &&
      (host_flags & D3DCLEAR_STENCIL)) {
    device_->Clear(0, nullptr, host_flags & ~D3DCLEAR_STENCIL, color, clear_z, 0);
  }

  if (have_saved) {
    device_->SetViewport(&saved);
  }
}

namespace {

/// Hash the microcode the GPU would actually execute.
///
/// This must match Xenia's `Shader::ucode_data_hash()` -- XXH3 over the raw guest
/// bytes -- because the SM3 cache is keyed by it.
///
/// It also has to be done *here*, at draw time, and not when the shader is
/// created: `D3D::DirectShaderPatch` rewrites a vertex shader's microcode in
/// place to bake in the vertex declaration, so the blob handed to
/// CreateVertexShader hashes differently from the one that runs.
uint64_t HashGuestUcode(const nx1::d3d9::GuestUcode& ucode) {
  if (!ucode.valid()) {
    return 0;
  }
  auto* memory = rex::system::kernel_state()->memory();
  // ReadGuestUcode already folded the guest's IM_LOAD address arithmetic (0xE0-heap
  // page bit + 4-align) into physical_address, so it is now a raw physical address --
  // read it exactly as Xenia's command processor does, via TranslatePhysical.
  const uint8_t* bytes = memory->TranslatePhysical<const uint8_t*>(ucode.physical_address);
  return XXH3_64bits(bytes, size_t(ucode.dword_count) * sizeof(uint32_t));
}

}  // namespace

void Renderer::UploadVertexUniforms(uint32_t base_reg) {
  // Packed to spend only two of vs_3_0's 256 registers:
  //   [0] = (ndcScale.xyz,  halfPixelOffset.x)
  //   [1] = (ndcOffset.xyz, halfPixelOffset.y)
  // ndcScale/ndcOffset are resolved from the guest viewport by ResolveViewport.
  // The half-pixel term is zero: this is a real D3D9 device, so the guest's own
  // D3D9-authored half-pixel handling already applies -- adding Xenia's D3D12
  // compensation would double it.
  const float params[8] = {
      ndc_scale_[0],  ndc_scale_[1],  ndc_scale_[2],  0.0f,
      ndc_offset_[0], ndc_offset_[1], ndc_offset_[2], 0.0f,
  };
  device_->SetVertexShaderConstantF(base_reg, params, 2);
}

void Renderer::ResolveViewport(const uint8_t* base, uint32_t guest_device) {
  // Replicates rex::graphics::GetHostViewportInfo for a real D3D9 host: produce
  // an integer host viewport plus the guest-clip -> host-clip NDC fold that the
  // translated vertex shaders apply. We cannot call GetHostViewportInfo directly
  // -- it reads a populated RegisterFile, which only exists when the (now
  // disabled) PM4 backend runs -- so it is reproduced here from the shadow regs.
  const ViewportState v = ReadViewportState(base, guest_device);
  // Relative to the bound target, not the backbuffer -- the world renders into a
  // 1024x600 target and the shadow maps into 1024x2048 ones.
  const float max_x = float(current_rt_width_ ? current_rt_width_ : backbuffer_width_);
  const float max_y = float(current_rt_height_ ? current_rt_height_ : backbuffer_height_);

  const float scale_xy[2] = {v.scale_x, v.scale_y};
  const float offset_xy[2] = {v.offset_x, v.offset_y};
  const float axis_max[2] = {max_x, max_y};
  float host_offset[2] = {0.0f, 0.0f};
  float host_extent[2] = {max_x, max_y};

  if (v.clip_disable) {
    // Screen-space draws (clears, some UI): huge host viewport, everything folded
    // into NDC. NDC maps guest pixel coords straight onto the render target.
    for (uint32_t i = 0; i < 2; ++i) {
      const float extent = axis_max[i];
      const float pixels_to_ndc = extent ? 2.0f / extent : 0.0f;
      host_offset[i] = 0.0f;
      host_extent[i] = extent;
      ndc_scale_[i] = scale_xy[i] * pixels_to_ndc;
      ndc_offset_[i] = (offset_xy[i] - extent * 0.5f) * pixels_to_ndc;
    }
    ndc_scale_[2] = v.scale_z;
    ndc_offset_[2] = v.offset_z;
  } else {
    for (uint32_t i = 0; i < 2; ++i) {
      const float scale_abs = std::abs(scale_xy[i]);
      const float a0 = std::clamp(offset_xy[i] - scale_abs, 0.0f, axis_max[i]);
      const float a1 = std::clamp(offset_xy[i] + scale_abs, 0.0f, axis_max[i]);
      const float extent = a1 - a0;
      host_offset[i] = a0;
      host_extent[i] = extent;
      if (extent > 0.0f) {
        ndc_scale_[i] = scale_xy[i] * 2.0f / extent;
        ndc_offset_[i] = (offset_xy[i] - (a0 + extent * 0.5f)) * 2.0f / extent;
      } else {
        ndc_scale_[i] = 1.0f;
        ndc_offset_[i] = 0.0f;
      }
    }
    // Depth: Xbox 360 is Direct3D 0..W clip space, so the guest Z passes straight
    // through and the host viewport carries MinZ..MaxZ.
    ndc_scale_[2] = 1.0f;
    ndc_offset_[2] = 0.0f;
  }

  // Match GetHostViewportInfo's origin_bottom_left path, which the D3D12 backend
  // passes as true: it negates the Y NDC so guest-top maps to host-top. Without
  // this the whole frame renders vertically mirrored (menu header lands at the
  // bottom, upside down).
  ndc_scale_[1] = -ndc_scale_[1];
  ndc_offset_[1] = -ndc_offset_[1];

  const float z_min = std::clamp(v.offset_z, 0.0f, 1.0f);
  const float z_max = std::clamp(v.offset_z + v.scale_z, 0.0f, 1.0f);

  // TEMP DIAGNOSTIC (no shadows): the shadow-cascade passes render into tall (1024x2048)
  // targets; log their guest viewport, especially the Z mapping, to compare the depth the
  // pass writes against the light-space depth the lighting shaders reconstruct.
  if (current_rt_height_ > current_rt_width_ && current_rt_height_ >= 1024) {
    static uint32_t n = 0;
    if ((n++ % 499) == 0) {
      REXGPU_INFO("nx1_d3d9: SHADOWVP rt={}x{} clip_disable={} scale=({:.4g},{:.4g},{:.6g}) "
                  "offset=({:.4g},{:.4g},{:.6g}) -> vpz=[{:.6g},{:.6g}]",
                  current_rt_width_, current_rt_height_, v.clip_disable, v.scale_x, v.scale_y,
                  v.scale_z, v.offset_x, v.offset_y, v.offset_z, z_min, z_max);
    }
  }

  D3DVIEWPORT9 vp;
  vp.X = DWORD(std::max(0.0f, host_offset[0]));
  vp.Y = DWORD(std::max(0.0f, host_offset[1]));
  vp.Width = DWORD(std::max(0.0f, host_extent[0]));
  vp.Height = DWORD(std::max(0.0f, host_extent[1]));
  vp.MinZ = z_min;
  vp.MaxZ = z_max;
  if (vp.Width && vp.Height) {
    device_->SetViewport(&vp);
  }
}

void Renderer::UploadPixelUniforms(uint32_t base_reg, bool needs_inv_tex_dim, const uint8_t* base,
                                   uint32_t guest_device) {
  // ps_3_0 only exposes 224 float registers.
  if (base_reg + 2 > 224) {
    return;
  }
  const AlphaTestState alpha = ReadAlphaTestState(base, guest_device);

  // The translator binds these at *fixed* offsets above the constant window:
  //   [0]     = alpha threshold
  //   [1]     = alpha compare function
  //   [2..17] = g_InvTexDim[16], (1/width, 1/height) per sampler
  //
  // The shader's alpha-test branch was baked in from its spec-constant mask, so a
  // shader that has one always runs it. Disabling the test at the render-state
  // level therefore has to be expressed as "always pass" (compare function 7).
  const float threshold[4] = {alpha.threshold, 0.0f, 0.0f, 0.0f};
  const float compare[4] = {float(alpha.enabled ? alpha.compare_function : 7u), 0.0f, 0.0f, 0.0f};
  device_->SetPixelShaderConstantF(base_reg, threshold, 1);
  device_->SetPixelShaderConstantF(base_reg + 1, compare, 1);

  // g_InvTexDim feeds offset tfetches: `tex2D(s, uv + offset * g_InvTexDim[i].xy)`, and the
  // translator only *declares* it for shaders that have one. Writing it regardless walks over 16
  // registers the shader never reserved -- which is where fxc parked its `def` literals, and D3D9
  // keeps those in the same constant file (a Set*ShaderConstantF after SetPixelShader wins).
  // Clobbering `def cN, 0, 1, -1, -0` -- the 0/1 source of every flattened `cmp` -- kills every
  // predicated block in the shader. So upload it only for shaders that actually read it.
  if (!needs_inv_tex_dim || base_reg + 18 > 224) {
    return;
  }
  float dims[16 * 4] = {};
  for (uint32_t s = 0; s < 16; ++s) {
    const TextureFetchConstant t = ReadTextureFetchConstant(base, guest_device, s);
    if (t.valid && t.width && t.height) {
      dims[s * 4 + 0] = 1.0f / float(t.width);
      dims[s * 4 + 1] = 1.0f / float(t.height);
    }
  }
  device_->SetPixelShaderConstantF(base_reg + 2, dims, 16);
}

bool Renderer::BindShadersAndConstants(const uint8_t* base, uint32_t guest_device) {
  auto& cache = ShaderCache::Get();

  const uint32_t vs_object = BoundVertexShader(base, guest_device);
  const uint32_t ps_object = BoundPixelShader(base, guest_device);
  if (!vs_object) {
    return false;
  }

  // A pixel shader is optional: the depth pre-pass binds none, and that is exactly
  // when a vertex shader runs its second (predicated-Z) pass. Hashing pass 0 there
  // would look up microcode the GPU never executes.
  const uint32_t vs_pass = VertexShaderPass(base, vs_object, /*has_pixel_shader=*/ps_object != 0);
  const uint64_t vs_hash =
      HashGuestUcode(ReadGuestUcode(base, vs_object, /*pixel_shader=*/false, vs_pass));
  const Sm3Shader* vs = vs_hash ? cache.Lookup(vs_hash) : nullptr;

  const uint64_t ps_hash =
      ps_object ? HashGuestUcode(ReadGuestUcode(base, ps_object, /*pixel_shader=*/true)) : 0;
  const Sm3Shader* ps = ps_hash ? cache.Lookup(ps_hash) : nullptr;

  if (!vs || (ps_object && !ps)) {
    // A handful of shaders do not lower to SM3, and a draw that wants one is silently
    // dropped. Name each distinct pair once so a missing draw can be traced back to the
    // shader that has to be fixed.
    ++shader_cache_misses_;
    {
      static std::mutex m;
      static std::vector<uint64_t> seen;
      std::lock_guard<std::mutex> lk(m);
      const uint64_t key = vs_hash ^ (ps_hash * 0x9E3779B97F4A7C15ull);
      if (std::find(seen.begin(), seen.end(), key) == seen.end() && seen.size() < 32) {
        seen.push_back(key);
        REXGPU_WARN("nx1_d3d9: shader cache miss: vs=0x{:016X} ({}) ps=0x{:016X} ({}) -- draw dropped",
                    vs_hash, vs ? "ok" : "MISSING", ps_hash,
                    !ps_object ? "none" : (ps ? "ok" : "MISSING"));
      }
    }
    return false;
  }



  device_->SetVertexShader(vs->vs);
  cache.UploadConstants(base, guest_device, *vs, /*pixel_stage=*/false);
  if (!NeedsHostNdcTransform(*vs)) {
    UploadVertexUniforms(HostConstantCount(*vs));
  }

  // TEMP DIAGNOSTIC (no shadows): replace the world pixel shader with one that does the sun
  // shadow lookup *itself* and shows the result. The guest VS puts the shadow lookup
  // (world -> shadow atlas UV + light-space depth) in TEXCOORD4, and the shadow atlas is on
  // sampler 5. This bypasses the guest PS's p0 predication entirely, so it isolates the data
  // (atlas + UV + depth) from the shader logic that consumes it:
  //   mode 1: red = stored atlas depth at the fragment's shadow UV (should look like a depth
  //           render of the scene from the sun; uniform => UV wrong)
  //   mode 2: green = the fragment's own light-space depth (should be a smooth gradient)
  //   mode 3: white/black = the actual compare, i.e. the shadow itself
  static IDirect3DPixelShader9* debug_ps = nullptr;
  static bool debug_ps_tried = false;
  if (!debug_ps_tried) {
    debug_ps_tried = true;
    const char* kSrc =
        // The guest PS uses v4.xy directly as the atlas UV (`add r14, c3, v4.xyxy`) -- there is
        // no perspective divide, and o4.w is a 4th linear function, not a divisor.
        "sampler2D shadowMap : register(s5);\n"
        "float4 mode : register(c0);\n"
        "float4 main(float4 t4 : TEXCOORD4) : COLOR {\n"
        "  float2 uv = t4.xy;\n"
        "  float d = tex2D(shadowMap, uv).r;\n"
        "  if (mode.x < 1.5) return float4(d, d, d, 1);\n"          // atlas depth (dark: 0..0.5)
        "  if (mode.x < 2.5) return float4(d * 2, d * 2, d * 2, 1);\n"  // amplified
        "  if (mode.x < 3.5) return float4(t4.z, t4.z, t4.z, 1);\n"  // fragment light depth
        "  if (mode.x < 4.5) return float4(frac(uv.x), frac(uv.y), 0, 1);\n"  // the UV itself
        "  if (mode.x < 5.5) return float4(uv.x, uv.y, 0, 1);\n"     // UV unwrapped (range check)
        "  if (mode.x < 6.5) { float l = (t4.z >= d) ? 1.0 : 0.0; return float4(l, l, l, 1); }\n"
        // 7/8 are the A/B for why u overshoots ~2x. If either renders a real shadow pattern
        // (and not noise), that is the answer:
        //   7: the sampler is meant to WRAP  -> folding u back into range fixes it
        //   8: the u row's scale is 2x wrong -> halving u fixes it
        "  if (mode.x < 7.5) {\n"
        "    float dw = tex2D(shadowMap, frac(uv)).r;\n"
        "    float l = (t4.z >= dw) ? 1.0 : 0.0; return float4(l, l, l, 1);\n"
        "  }\n"
        "  if (mode.x < 8.5) {\n"
        "    float dh = tex2D(shadowMap, float2(uv.x * 0.5, uv.y)).r;\n"
        "    float l = (t4.z >= dh) ? 1.0 : 0.0; return float4(l, l, l, 1);\n"
        "  }\n"
        "  return float4(frac(uv.x * 0.5), uv.y, 0, 1);\n"           // 9: half-u UV range check
        "}";
    ID3DBlob* code = nullptr;
    ID3DBlob* err = nullptr;
    if (SUCCEEDED(D3DCompile(kSrc, strlen(kSrc), nullptr, nullptr, nullptr, "main", "ps_3_0", 0, 0,
                             &code, &err)) &&
        code) {
      device_->CreatePixelShader(static_cast<const DWORD*>(code->GetBufferPointer()), &debug_ps);
      code->Release();
    } else if (err) {
      REXGPU_ERROR("nx1_d3d9: shadow debug PS failed: {}",
                   static_cast<const char*>(err->GetBufferPointer()));
    }
    if (err) err->Release();
  }
  // Only hijack draws that really do have the shadow atlas on s5 (a depth-format fetch) --
  // every other world draw has an ordinary texture there and would just show its red channel.
  // TEMP DIAGNOSTIC (no shadows): show the CASCADE FADE, the thing that multiplies the shadow
  // term to zero. Read straight out of the emitted SM3 HLSL for shader 4E127BDFABD81721:
  //
  //   r8.xyzw = r8.xywz + c[3].wzzz;          cascade-space coords
  //   r0.yw   = max(abs(r8.yz), abs(r8.xw));  box distance
  //   r0.xy   = saturate(-r0.yw + c[16].xx);  <-- FADE; 0 here erases the shadow
  //   r0.x    = r0.z * r0.x + r0.w;           PCF result * FADE
  //
  // The guest's own constants are still in the D3D9 constant file when we swap the shader in
  // (SetPixelShader does not clear it), so this reads the REAL c1/c3/c16 rather than guesses.
  // That makes the register numbers shader-specific, hence the exact-hash gate: for this shader
  // c1<-g2, c3<-g4=(16,32,-8,-24), c16<-g254=(8,..). Cascade 0 survives u in (0,1), v in (0,0.5)
  // (the atlas's top tile); cascade 1 survives u in (0,4), v in (0,1.5) and remaps into the
  // bottom tile. If both fades are black on screen, the coordinates are out of range and that is
  // the bug; if they are lit, the fade is innocent and the fault is downstream of it.
  static IDirect3DPixelShader9* fade_ps = nullptr;
  static bool fade_ps_tried = false;
  if (!fade_ps_tried) {
    fade_ps_tried = true;
    const char* kFadeSrc =
        // Fed by the host from each shader's own remap (guest g2/g4), not by register number:
        // the same engine constant lands in a different host register in every shader.
        "float4 c1 : register(c25);\n"
        "float4 c3 : register(c26);\n"
        "float4 c16 : register(c27);\n"
        "float4 mode : register(c24);\n"
        "float4 main(float4 t4 : TEXCOORD4) : COLOR {\n"
        "  float u = t4.x, v = t4.y;\n"
        // cascade 0: the coords used directly (r8.z = 16u-8, r8.w = 32v-8)
        "  float d0 = max(abs(c3.x * u + c3.z), abs(c3.y * v + c3.z));\n"
        "  float fade0 = saturate(-d0 + c16.x);\n"
        // cascade 1: coords rescaled by c1 first (r8.y = 16*(.25u+c1.x)-8, r8.x = 32*(.25v+c1.y)-24)
        "  float d1 = max(abs(c3.x * (u * c1.w + c1.x) + c3.z),\n"
        "                 abs(c3.y * (v * c1.w + c1.y) + c3.w));\n"
        "  float fade1 = saturate(-d1 + c16.x);\n"
        "  if (mode.x < 10.5) return float4(fade0, fade1, 0, 1);\n"  // 10: red=c0 green=c1
        "  if (mode.x < 11.5) return float4(saturate(u), saturate(v), 0, 1);\n"  // 11: raw uv
        "  return float4(saturate(u * 0.5), saturate(v * 0.5), 0, 1);\n"  // 12: uv halved
        "}";
    ID3DBlob* code = nullptr;
    ID3DBlob* err = nullptr;
    if (SUCCEEDED(D3DCompile(kFadeSrc, strlen(kFadeSrc), nullptr, nullptr, nullptr, "main",
                             "ps_3_0", 0, 0, &code, &err)) &&
        code) {
      device_->CreatePixelShader(static_cast<const DWORD*>(code->GetBufferPointer()), &fade_ps);
      code->Release();
    } else if (err) {
      REXGPU_ERROR("nx1_d3d9: fade debug PS failed: {}",
                   static_cast<const char*>(err->GetBufferPointer()));
    }
    if (err) err->Release();
    // Say so unconditionally: a probe that silently does not exist is indistinguishable from a
    // probe that ran and found nothing -- and that ambiguity has already cost runs here.
    REXGPU_INFO("nx1_d3d9: FADEPROBE shader built={} cvar={}", fade_ps != nullptr,
                REXCVAR_GET(nx1_d3d9_dbg_shadow));
  }

  const int32_t dbg_shadow = REXCVAR_GET(nx1_d3d9_dbg_shadow);
  if (dbg_shadow >= 10 && ps && fade_ps) {
    // Every draw that binds the depth atlas on s5 -- the signature of a sun-shadow draw. Gating
    // on one shader hash covered too few pixels to see (it drew, but nothing changed on screen).
    const TextureFetchConstant s5 = ReadTextureFetchConstant(base, guest_device, 5);
    const int r_c1 = cache.HostRegisterForGuest(*ps, 2);  // cascade-1 rescale
    const int r_c3 = cache.HostRegisterForGuest(*ps, 4);  // cascade scale/offset (16,32,-8,-24)
    if (s5.valid && (s5.format == 22 || s5.format == 23) && r_c1 >= 0 && r_c3 >= 0) {
      // Upload the guest's own constants FIRST: the normal path does this below, after the point
      // we return from, so c1/c3 would otherwise still hold the previous draw's values -- the
      // exact stale-register trap that produced a false theory once already.
      cache.UploadConstants(base, guest_device, *ps, /*pixel_stage=*/true);
      float c1v[4] = {}, c3v[4] = {};
      device_->GetPixelShaderConstantF(r_c1, c1v, 1);
      device_->GetPixelShaderConstantF(r_c3, c3v, 1);
      device_->SetPixelShader(fade_ps);  // fade_ps has no `def`s, so it cannot clobber these
      device_->SetPixelShaderConstantF(25, c1v, 1);
      device_->SetPixelShaderConstantF(26, c3v, 1);
      const float fade_edge[4] = {8.0f, 0, 0, 0};  // c16.xx; a per-shader literal, 8.0 here
      device_->SetPixelShaderConstantF(27, fade_edge, 1);
      const float m[4] = {float(dbg_shadow), 0, 0, 0};
      device_->SetPixelShaderConstantF(24, m, 1);
      device_->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
      static std::mutex fm;
      static std::vector<uint64_t> fseen;
      std::lock_guard<std::mutex> lk(fm);
      if (std::find(fseen.begin(), fseen.end(), ps_hash) == fseen.end() && fseen.size() < 8) {
        fseen.push_back(ps_hash);
        REXGPU_INFO("nx1_d3d9: FADEPROBE {:016X} mode={} c1(g2)=c{}=({:.4g},{:.4g},{:.4g},{:.4g}) "
                    "c3(g4)=c{}=({:.4g},{:.4g},{:.4g},{:.4g})",
                    ps_hash, dbg_shadow, r_c1, c1v[0], c1v[1], c1v[2], c1v[3], r_c3, c3v[0],
                    c3v[1], c3v[2], c3v[3]);
      }
      return true;
    }
  }
  if (dbg_shadow > 0 && dbg_shadow < 10 && ps && debug_ps && current_rt_width_ == 1024 &&
      current_rt_height_ == 600) {
    const TextureFetchConstant s5 = ReadTextureFetchConstant(base, guest_device, 5);
    if (s5.valid && (s5.format == 22 || s5.format == 23)) {
      device_->SetPixelShader(debug_ps);
      const float m[4] = {float(dbg_shadow), 0, 0, 0};
      device_->SetPixelShaderConstantF(0, m, 1);
      device_->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
      return true;
    }
  }
  device_->SetPixelShader(ps ? ps->ps : nullptr);
  // A draw with no pixel shader is a depth/shadow-only pass: the Xbox exports no
  // color, so nothing is written to the render target. On D3D9, SetPixelShader(null)
  // instead activates the fixed-function pixel pipeline, which *does* write color
  // (the surface's diffuse/texture as solid fill) and paints garbage over the frame.
  // Mask color writes off for those draws so they contribute depth only.
  //
  // Otherwise honour the guest's own mask: it masks colour off to write destination alpha
  // alone (see ReadColorWriteMask).
  device_->SetRenderState(D3DRS_COLORWRITEENABLE, ps ? ReadColorWriteMask(base, guest_device) : 0);
  if (ps) {
    cache.UploadConstants(base, guest_device, *ps, /*pixel_stage=*/true);
    UploadPixelUniforms(HostConstantCount(*ps), ps->needs_inv_tex_dim, base, guest_device);
  }

  return true;
}

bool Renderer::BindStreams(const uint8_t* base, uint32_t guest_device, uint32_t needed_vertices,
                           uint32_t* vertex_count) {
  auto& tracker = ResourceTracker::Get();
  // NX1 binds no CVertexDeclaration; for bound-buffer draws derive the multi-stream
  // layout from the vertex shader's vfetch (stream0_stride=0 selects bound mode).
  const VertexLayout* layout = tracker.GetVertexLayout(base, guest_device);
  if (!layout) {
    layout = tracker.GetShaderVertexLayout(base, guest_device, /*stream0_stride=*/0);
  }
  if (!layout) {
    return false;
  }
  device_->SetVertexDeclaration(layout->decl);

  *vertex_count = 0;
  for (uint32_t stream = 0; stream < layout->stream_count; ++stream) {
    uint32_t count = 0;
    IDirect3DVertexBuffer9* vb =
        tracker.GetVertexBuffer(base, guest_device, stream, *layout, needed_vertices, &count);
    if (!vb) {
      return false;
    }
    device_->SetStreamSource(stream, vb, 0, layout->host_stride[stream]);
    // The draw's vertex range has to fit inside every stream it reads.
    *vertex_count = *vertex_count ? std::min(*vertex_count, count) : count;
  }
  return *vertex_count != 0;
}

namespace {

// Xenos ClampMode (fetch constant) -> D3DTEXTUREADDRESS.
D3DTEXTUREADDRESS HostAddressMode(uint32_t clamp_mode) {
  switch (clamp_mode) {
    case 0: return D3DTADDRESS_WRAP;        // repeat
    case 1: return D3DTADDRESS_MIRROR;      // mirrored repeat
    case 2: return D3DTADDRESS_CLAMP;       // clamp to last texel
    case 3: return D3DTADDRESS_MIRRORONCE;  // mirror once
    case 4: return D3DTADDRESS_CLAMP;       // clamp to halfway (approx)
    case 5: return D3DTADDRESS_MIRRORONCE;  // mirror clamp to halfway (approx)
    case 6:
    case 7: return D3DTADDRESS_BORDER;      // clamp/mirror to border
    default: return D3DTADDRESS_WRAP;
  }
}

// Xenos TextureFilter: 0 = point, 1 = linear (2 = "keep"/base, treated as linear).
D3DTEXTUREFILTERTYPE HostFilter(uint32_t xenos_filter) {
  return xenos_filter == 0 ? D3DTEXF_POINT : D3DTEXF_LINEAR;
}

// The mip filter has one more state than the mag/min ones: xenos::TextureFilter::kBaseMap (2)
// means "sample the base level and no other", which on D3D9 is D3DTEXF_NONE rather than a
// filter mode.
//
// We do not honour it. The guest asks for base-map-only on exactly the textures whose mips
// never streamed in -- this build has no imagefile to stream them from -- so pairing it with
// the chains we generate ourselves left those surfaces reading level 0 at any distance,
// aliasing into coloured speckle. On the console they have mips and are filtered, which is what
// the scene was authored for. For a texture that genuinely has one level this is a no-op.
D3DTEXTUREFILTERTYPE HostMipFilter(uint32_t xenos_filter) {
  return xenos_filter == 0 ? D3DTEXF_POINT : D3DTEXF_LINEAR;
}

/// The maximum anisotropy the guest is asking for: xenos::AnisoFilter counts ratios, not
/// samples -- 0 = disabled, then 1:1, 2:1, 4:1, 8:1, 16:1. Anything above the adapter's cap
/// is an invalid sampler state, so clamp.
uint32_t HostMaxAnisotropy(uint32_t xenos_aniso, uint32_t device_max) {
  if (xenos_aniso == 0 || xenos_aniso > 5) {
    return 1;  // disabled, or kUseFetchConst (7) leaking out of a shader-side field
  }
  const uint32_t ratio = 1u << (xenos_aniso - 1);
  return ratio > device_max ? device_max : ratio;
}

// xenos::CompareFunction (0..7) maps 1:1 onto D3DCMPFUNC shifted by one.
D3DCMPFUNC HostCompare(uint32_t xenos_func) {
  return D3DCMPFUNC((xenos_func & 0x7) + 1);
}

// xenos::BlendFactor -> D3DBLEND.
D3DBLEND HostBlendFactor(uint32_t f) {
  switch (f) {
    case 0:  return D3DBLEND_ZERO;
    case 1:  return D3DBLEND_ONE;
    case 4:  return D3DBLEND_SRCCOLOR;
    case 5:  return D3DBLEND_INVSRCCOLOR;
    case 6:  return D3DBLEND_SRCALPHA;
    case 7:  return D3DBLEND_INVSRCALPHA;
    case 8:  return D3DBLEND_DESTCOLOR;
    case 9:  return D3DBLEND_INVDESTCOLOR;
    case 10: return D3DBLEND_DESTALPHA;
    case 11: return D3DBLEND_INVDESTALPHA;
    case 12: return D3DBLEND_BLENDFACTOR;      // constant color
    case 13: return D3DBLEND_INVBLENDFACTOR;
    case 14: return D3DBLEND_BLENDFACTOR;      // constant alpha (no D3D9 distinction)
    case 15: return D3DBLEND_INVBLENDFACTOR;
    case 16: return D3DBLEND_SRCALPHASAT;
    default: return D3DBLEND_ONE;
  }
}

// xenos::BlendOp -> D3DBLENDOP.
D3DBLENDOP HostBlendOp(uint32_t op) {
  switch (op) {
    case 0:  return D3DBLENDOP_ADD;
    case 1:  return D3DBLENDOP_SUBTRACT;
    case 2:  return D3DBLENDOP_MIN;
    case 3:  return D3DBLENDOP_MAX;
    case 4:  return D3DBLENDOP_REVSUBTRACT;
    default: return D3DBLENDOP_ADD;
  }
}

}  // namespace

void Renderer::ApplyRenderStates(const uint8_t* base, uint32_t guest_device) {
  // Depth. NX1 renders reverse-Z, so the guest's stored zfunc is GREATER_EQUAL --
  // we read it rather than hardcoding, so a draw that flips the test still works.
  const DepthState depth = ReadDepthState(base, guest_device);
  device_->SetRenderState(D3DRS_ZENABLE, depth.test_enabled ? D3DZB_TRUE : D3DZB_FALSE);
  device_->SetRenderState(D3DRS_ZWRITEENABLE, depth.write_enabled ? TRUE : FALSE);
  device_->SetRenderState(D3DRS_ZFUNC, HostCompare(depth.compare_function));

  // Blend. Separate color/alpha factors, exactly as Xenos stores them.
  const BlendState blend = ReadBlendState(base, guest_device);
  device_->SetRenderState(D3DRS_ALPHABLENDENABLE, blend.enabled ? TRUE : FALSE);
  device_->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, TRUE);
  device_->SetRenderState(D3DRS_SRCBLEND, HostBlendFactor(blend.color_src));
  device_->SetRenderState(D3DRS_DESTBLEND, HostBlendFactor(blend.color_dst));
  device_->SetRenderState(D3DRS_BLENDOP, HostBlendOp(blend.color_op));
  device_->SetRenderState(D3DRS_SRCBLENDALPHA, HostBlendFactor(blend.alpha_src));
  device_->SetRenderState(D3DRS_DESTBLENDALPHA, HostBlendFactor(blend.alpha_dst));
  device_->SetRenderState(D3DRS_BLENDOPALPHA, HostBlendOp(blend.alpha_op));

  // Cull. D3D9 folds the front-face winding into the cull direction (it has no
  // separate "front face" state), so resolve the Xenos (cull, winding) pair here.
  const CullState cull = ReadCullState(base, guest_device);
  D3DCULL mode = D3DCULL_NONE;
  if (cull.cull_back) {
    mode = cull.front_is_cw ? D3DCULL_CCW : D3DCULL_CW;
  } else if (cull.cull_front) {
    mode = cull.front_is_cw ? D3DCULL_CW : D3DCULL_CCW;
  }
  device_->SetRenderState(D3DRS_CULLMODE, mode);
}

void Renderer::InvalidateSamplerShadow() {
  for (uint32_t s = 0; s < 16; ++s) {
    sampler_texture_[s] = kSamplerTextureUnknown;
    for (uint32_t i = 0; i < kSamplerStates; ++i) {
      sampler_state_[s][i] = kSamplerStateUnset;
    }
  }
}

// TEMP DIAGNOSTIC (world darkness): on scene-pass draws, log which samplers carry a valid fetch
// constant but no host texture (a lightmap that fails to materialize samples as black -> the
// whole world drops to ambient), and dump the first few distinct world pixel shaders with their
// constants to identify the lightmap sampler and any sun/exposure uniforms.
void Renderer::ProbeWorldDraw(const uint8_t* base, uint32_t guest_device) {
  if (current_rt_width_ == backbuffer_width_ || current_rt_height_ < 400) {
    return;  // scene pass only: offscreen, world-sized
  }
  static uint32_t tick = 0;
  if ((tick++ % 397) != 0) {
    return;
  }
  auto& tracker = ResourceTracker::Get();
  uint32_t depth_mask = 0;  // samplers bound to a depth (f22/f23) fetch this draw
  char b[900];
  int n = std::snprintf(b, sizeof(b), "nx1_d3d9: WORLDTEX rt=%ux%u", current_rt_width_,
                        current_rt_height_);
  for (uint32_t s = 0; s < 16 && n < 830; ++s) {
    const TextureFetchConstant t = ReadTextureFetchConstant(base, guest_device, s);
    if (!t.valid || !t.base_address) {
      continue;
    }
    IDirect3DBaseTexture9* tex = tracker.GetTexture(base, guest_device, s);
    n += std::snprintf(b + n, sizeof(b) - n, " s%u:%ux%u f%u sw%03X %s", s, t.width, t.height,
                       t.format, t.swizzle & 0xFFF, tex ? "ok" : "NULL");
    if ((t.format == 22 || t.format == 23) && n < 810) {  // depth fetch: the shadow atlas
      depth_mask |= 1u << s;
      // The shadow lookup UV overshoots [0,1] on one axis (u~1.97, v~0.93 for a known object),
      // so what the guest asks for here decides whether that is a bug or by design: WRAP (0)
      // folds u back into range, CLAMP/BORDER does not.
      const SamplerClampModes cm = ReadSamplerClampModes(base, guest_device, s);
      n += std::snprintf(b + n, sizeof(b) - n, "@%08X clamp(u%u,v%u)->host(%u,%u)",
                         t.base_address, cm.u, cm.v, uint32_t(HostAddressMode(cm.u)),
                         uint32_t(HostAddressMode(cm.v)));
    }
  }
  REXGPU_INFO("{}", b);

  // The lit path (shadow taps, volume light, env cube) is predicated on p0, which needs
  // `albedo.a * vertexColour.a >= c252.x`. If our vertex layout does not deliver COLOR, that
  // alpha is 0 and the whole block is skipped -- which is exactly what forcing the shadow
  // atlas proved. Read back the bound declaration and dump the COLOR element of a few real
  // vertices.
  if (depth_mask) {
    static uint32_t vc = 0;
    if ((vc++ % 211) == 0) {
      IDirect3DVertexDeclaration9* decl = nullptr;
      IDirect3DVertexBuffer9* vb = nullptr;
      UINT off = 0, stride = 0;
      if (SUCCEEDED(device_->GetVertexDeclaration(&decl)) && decl &&
          SUCCEEDED(device_->GetStreamSource(0, &vb, &off, &stride)) && vb && stride) {
        D3DVERTEXELEMENT9 els[MAXD3DDECLLENGTH + 1] = {};
        UINT n_els = 0;
        char eb[700];
        int en = std::snprintf(eb, sizeof(eb), "nx1_d3d9: WORLDVTX stride=%u decl:", stride);
        int color_off = -1, color_type = -1;
        if (SUCCEEDED(decl->GetDeclaration(els, &n_els))) {
          for (UINT i = 0; i + 1 < n_els && en < 600; ++i) {
            en += std::snprintf(eb + en, sizeof(eb) - en, " [s%u+%u u%u/%u t%u]", els[i].Stream,
                                els[i].Offset, els[i].Usage, els[i].UsageIndex, els[i].Type);
            if (els[i].Usage == D3DDECLUSAGE_COLOR && els[i].Stream == 0) {
              color_off = int(els[i].Offset);
              color_type = int(els[i].Type);
            }
          }
        }
        en += std::snprintf(eb + en, sizeof(eb) - en, " | COLOR off=%d type=%d", color_off,
                            color_type);
        // The NORMAL is guest FMT_2_10_10_10 signed, expanded by us to FLOAT4. Garbage or
        // constant normals make N.L constant -> exactly the flat, shadowless lighting we see,
        // even with a correct sun. Dump them as floats; they must be unit-length and vary.
        int norm_off = -1, norm_type = -1;
        for (UINT i = 0; i + 1 < n_els; ++i) {
          if (els[i].Usage == D3DDECLUSAGE_NORMAL && els[i].Stream == 0) {
            norm_off = int(els[i].Offset);
            norm_type = int(els[i].Type);
          }
        }
        en += std::snprintf(eb + en, sizeof(eb) - en, " NORMAL off=%d type=%d", norm_off,
                            norm_type);
        void* data = nullptr;
        if (SUCCEEDED(vb->Lock(off, 4 * stride, &data, D3DLOCK_READONLY)) && data) {
          for (uint32_t v = 0; v < 4 && en < 560; ++v) {
            const uint8_t* base_v = static_cast<const uint8_t*>(data) + size_t(v) * stride;
            if (color_off >= 0) {
              const uint8_t* p = base_v + color_off;
              en += std::snprintf(eb + en, sizeof(eb) - en, " v%u:c[%02X %02X %02X %02X]", v, p[0],
                                  p[1], p[2], p[3]);
            }
            if (norm_off >= 0 && norm_type == D3DDECLTYPE_FLOAT4) {
              const float* nf = reinterpret_cast<const float*>(base_v + norm_off);
              en += std::snprintf(eb + en, sizeof(eb) - en, " n(%.3f,%.3f,%.3f,%.3f)", nf[0], nf[1],
                                  nf[2], nf[3]);
            }
          }
          vb->Unlock();
        }
        REXGPU_INFO("{}", eb);
      }
      if (vb) vb->Release();
      if (decl) decl->Release();
    }
  }

  // Only dump shaders that actually sample a shadow/depth map this draw -- those hold the
  // shadow compare we're after.
  static uint32_t dumped = 0;
  static std::unordered_set<void*> seen;
  if (dumped >= 10 || !depth_mask) {
    return;
  }
  IDirect3DPixelShader9* ps = nullptr;
  if (FAILED(device_->GetPixelShader(&ps)) || !ps) {
    return;
  }
  if (!seen.insert(ps).second) {
    ps->Release();
    return;
  }
  ++dumped;
  float pc[24 * 4] = {};
  device_->GetPixelShaderConstantF(0, pc, 24);
  char cb[900];
  int cn = 0;
  for (uint32_t i = 0; i < 24 && cn < 850; ++i) {
    cn += std::snprintf(cb + cn, sizeof(cb) - cn, " c%u(%.3g,%.3g,%.3g,%.3g)", i, pc[i * 4],
                        pc[i * 4 + 1], pc[i * 4 + 2], pc[i * 4 + 3]);
  }
  const uint32_t ps_object = BoundPixelShader(base, guest_device);
  const uint64_t ps_hash =
      ps_object ? HashGuestUcode(ReadGuestUcode(base, ps_object, /*pixel_shader=*/true)) : 0;
  const uint32_t vs_object = BoundVertexShader(base, guest_device);
  const uint64_t vs_hash = vs_object
      ? HashGuestUcode(ReadGuestUcode(base, vs_object, /*pixel_shader=*/false,
                                      VertexShaderPass(base, vs_object, ps_object != 0)))
      : 0;
  REXGPU_INFO("nx1_d3d9: WORLDPS #{} ps={} pshash=0x{:016X} vshash=0x{:016X} depthmask={:#x} "
              "consts:{}",
              dumped, (void*)ps, ps_hash, vs_hash, depth_mask, cb);
  UINT fsize = 0;
  if (SUCCEEDED(ps->GetFunction(nullptr, &fsize)) && fsize) {
    std::vector<uint8_t> fn(fsize);
    if (SUCCEEDED(ps->GetFunction(fn.data(), &fsize))) {
      ID3DBlob* text = nullptr;
      if (SUCCEEDED(D3DDisassemble(fn.data(), fsize, 0, nullptr, &text)) && text) {
        REXGPU_INFO("nx1_d3d9: WORLDPS-ASM #{}:\n{}", dumped,
                    static_cast<const char*>(text->GetBufferPointer()));
        text->Release();
      }
    }
  }
  ps->Release();

  // The shadow UV + light-space depth (v4 in the PS) are computed in the VERTEX shader from
  // the guest's shadow lookup matrix. Forcing the atlas to "occluded everywhere" changed
  // nothing, so that varying is off-scale -- dump the VS and its constants to find the matrix.
  IDirect3DVertexShader9* vs = nullptr;
  if (SUCCEEDED(device_->GetVertexShader(&vs)) && vs) {
    float vc[48 * 4] = {};
    device_->GetVertexShaderConstantF(0, vc, 48);
    char vb[1600];
    int vn = 0;
    for (uint32_t i = 0; i < 48 && vn < 1500; ++i) {
      vn += std::snprintf(vb + vn, sizeof(vb) - vn, " c%u(%.4g,%.4g,%.4g,%.4g)", i, vc[i * 4],
                          vc[i * 4 + 1], vc[i * 4 + 2], vc[i * 4 + 3]);
    }
    REXGPU_INFO("nx1_d3d9: WORLDVS #{} vsconsts:{}", dumped, vb);
    UINT vsz = 0;
    if (SUCCEEDED(vs->GetFunction(nullptr, &vsz)) && vsz) {
      std::vector<uint8_t> fn(vsz);
      if (SUCCEEDED(vs->GetFunction(fn.data(), &vsz))) {
        ID3DBlob* text = nullptr;
        if (SUCCEEDED(D3DDisassemble(fn.data(), vsz, 0, nullptr, &text)) && text) {
          REXGPU_INFO("nx1_d3d9: WORLDVS-ASM #{}:\n{}", dumped,
                      static_cast<const char*>(text->GetBufferPointer()));
          text->Release();
        }
      }
    }
    vs->Release();
  }
}

void Renderer::BindTextures(const uint8_t* base, uint32_t guest_device, uint64_t surface_key) {
  auto& tracker = ResourceTracker::Get();
  // A scene is ~5000 draws over 16 samplers, and consecutive draws overwhelmingly share
  // their textures and filters -- so shadow the sampler state and only touch D3D when it
  // actually changes. Issuing all of it unconditionally was 80k SetTexture and ~500k
  // SetSamplerState calls a frame, nearly every one of them a no-op.
  //
  // Comparing texture *pointers* is sound: D3D9 holds its own reference to a bound texture,
  // so an object we release cannot be freed (and its address reused) while still bound.
  for (uint32_t sampler = 0; sampler < 16; ++sampler) {
    IDirect3DBaseTexture9* tex = tracker.GetTexture(base, guest_device, sampler);
    // Read the fetch constant once (needed for both the LOD substitution and the sampler state).
    TextureFetchConstant t{};
    if (tex) {
      t = ReadTextureFetchConstant(base, guest_device, sampler);
      // For a static-geometry world draw, hold the highest-res texture this surface has shown and
      // substitute it when the engine swaps sampler to a receding (garbage) LOD -- see
      // PreferLargestForSurface. surface_key 0 (UI / inline draws) leaves the binding untouched.
      tex = tracker.PreferLargestForSurface(surface_key, sampler, t.format, tex, t.width, t.height);
    }
    if (tex != sampler_texture_[sampler]) {
      device_->SetTexture(sampler, tex);
      sampler_texture_[sampler] = tex;
    }
    if (!tex) {
      continue;
    }
    const SamplerClampModes clamp = ReadSamplerClampModes(base, guest_device, sampler);
    // Anisotropy is minification-only, so it replaces the min filter and nothing else. Half of
    // NX1's binds ask for a point *mip* filter, which is only sane on hardware that filters
    // anisotropically underneath it: without aniso that is bilinear plus a hard mip step, and
    // every grazing surface -- floors, walls, foliage at distance -- shimmers.
    const uint32_t aniso = HostMaxAnisotropy(t.aniso_filter, max_anisotropy_);
    const D3DTEXTUREFILTERTYPE min_filter =
        aniso > 1 ? D3DTEXF_ANISOTROPIC : HostFilter(t.min_filter);
    // Pair smooth minification with a linear (trilinear) mip filter. NX1 often asks for a POINT mip
    // filter, which on Xenos is fine because anisotropy filters underneath it -- but on desktop D3D9 a
    // point mip filter under linear/aniso minification leaves hard mip-level steps, visible as bands
    // sweeping across smooth gradients (the sky dome above all). Keep point mip only when the
    // minification itself is point (a deliberately crisp surface).
    const D3DTEXTUREFILTERTYPE mip_filter =
        min_filter == D3DTEXF_POINT ? HostMipFilter(t.mip_filter) : D3DTEXF_LINEAR;
    const uint32_t states[kSamplerStates] = {
        uint32_t(HostAddressMode(clamp.u)), uint32_t(HostAddressMode(clamp.v)),
        uint32_t(HostAddressMode(clamp.w)), uint32_t(HostFilter(t.mag_filter)),
        uint32_t(min_filter),               uint32_t(mip_filter),
        aniso,
        uint32_t(t.gamma && REXCVAR_GET(nx1_d3d9_srgb_tex) ? TRUE : FALSE),
    };
    static constexpr D3DSAMPLERSTATETYPE kTypes[kSamplerStates] = {
        D3DSAMP_ADDRESSU,  D3DSAMP_ADDRESSV,  D3DSAMP_ADDRESSW,
        D3DSAMP_MAGFILTER, D3DSAMP_MINFILTER, D3DSAMP_MIPFILTER,
        D3DSAMP_MAXANISOTROPY, D3DSAMP_SRGBTEXTURE,
    };
    for (uint32_t i = 0; i < kSamplerStates; ++i) {
      if (states[i] != sampler_state_[sampler][i]) {
        device_->SetSamplerState(sampler, kTypes[i], states[i]);
        sampler_state_[sampler][i] = states[i];
      }
    }
  }
}

void Renderer::DrawIndexed(const uint8_t* base, uint32_t guest_device, uint32_t prim_type,
                           uint32_t base_vertex_index, uint32_t start_index,
                           uint32_t index_count) {
  std::lock_guard<std::mutex> lock(render_mutex_);
  if (shutting_down_.load(std::memory_order_acquire)) {
    return;
  }
  const D3DPRIMITIVETYPE host_prim = HostPrimitiveType(prim_type);
  const uint32_t prim_count = HostPrimitiveCount(prim_type, index_count);
  if (!device_ || !host_prim || !prim_count) {
    // An unsupported primitive type (rectangle/quad lists) has no D3D9 equivalent, so the
    // draw is dropped. Say so once per type: silently losing a whole class of draws is the
    // kind of thing that costs days to find.
    if (device_ && !host_prim) {
      static uint32_t seen_mask = 0;
      if (prim_type < 32 && !(seen_mask & (1u << prim_type))) {
        seen_mask |= 1u << prim_type;
        REXGPU_WARN("nx1_d3d9: DrawIndexed SKIPPED: unsupported Xenos primitive type {} "
                    "({} indices) -- these draws never render",
                    prim_type, index_count);
      }
    }
    return;
  }
  ++draws_attempted_;

  ResolveViewport(base, guest_device);
  if (!BindShadersAndConstants(base, guest_device)) {
    return;
  }

  // Indices first: how far into the vertex buffer this draw actually reaches decides how
  // much of it we have to mirror. The guest's fetch constant would have us mirror the whole
  // pool the model happens to live in.
  uint32_t index_size = 0;
  IDirect3DIndexBuffer9* ib = ResourceTracker::Get().GetIndexBuffer(base, guest_device, &index_size);
  if (!ib) {
    return;
  }
  const uint32_t max_index =
      ResourceTracker::Get().GetDrawMaxIndex(base, guest_device, start_index, index_count);
  const uint32_t needed_vertices = max_index ? base_vertex_index + max_index + 1 : 0;

  uint32_t vertex_count = 0;
  if (!BindStreams(base, guest_device, needed_vertices, &vertex_count)) {
    return;
  }

  device_->SetIndices(ib);
  // Stable per-surface identity for prefer-largest LOD substitution: a static world surface draws
  // the same index-buffer range every frame (the index buffers plateau at a few hundred addresses),
  // so (ib address, draw range) keys the surface across its near/far LOD swaps. Mix in a golden-ratio
  // constant so start_index/index_count don't collide across nearby ranges.
  const uint32_t ib_base = ReadIndexBuffer(base, guest_device).base_address;
  const uint64_t surface_key =
      uint64_t(ib_base) ^ (uint64_t(start_index) * 0x9E3779B185EBCA87ull) ^
      (uint64_t(index_count) << 40) ^ (uint64_t(base_vertex_index) * 0xD1B54A32D192ED03ull);
  BindTextures(base, guest_device, surface_key);
  ApplyRenderStates(base, guest_device);
  ProbeWorldDraw(base, guest_device);

  const HRESULT dhr = device_->DrawIndexedPrimitive(host_prim, int(base_vertex_index), 0,
                                                    vertex_count, start_index, prim_count);
  // A draw can be *submitted* and still be rejected by D3D9 -- a depth buffer smaller than
  // the render target does exactly that, silently.
  if (FAILED(dhr)) {
    static uint64_t fails = 0;
    if ((fails++ % 2000) == 0) {
      REXGPU_WARN("nx1_d3d9: DrawIndexedPrimitive failed ({:#x}) rt={}x{} prim={} verts={} -- {} so far",
                  uint32_t(dhr), current_rt_width_, current_rt_height_, prim_count, vertex_count,
                  fails);
    }
  }
  ++draws_submitted_;
}

void Renderer::Draw(const uint8_t* base, uint32_t guest_device, uint32_t prim_type,
                    uint32_t start_vertex, uint32_t vertex_count) {
  std::lock_guard<std::mutex> lock(render_mutex_);
  if (shutting_down_.load(std::memory_order_acquire)) {
    return;
  }
  const D3DPRIMITIVETYPE host_prim = HostPrimitiveType(prim_type);
  const uint32_t prim_count = HostPrimitiveCount(prim_type, vertex_count);
  if (!device_ || !host_prim || !prim_count) {
    if (device_ && !host_prim) {  // see DrawIndexed
      static uint32_t seen_mask = 0;
      if (prim_type < 32 && !(seen_mask & (1u << prim_type))) {
        seen_mask |= 1u << prim_type;
        REXGPU_WARN("nx1_d3d9: Draw SKIPPED: unsupported Xenos primitive type {} ({} vertices) "
                    "-- these draws never render",
                    prim_type, vertex_count);
      }
    }
    return;
  }
  ++draws_attempted_;
  ResolveViewport(base, guest_device);
  if (!BindShadersAndConstants(base, guest_device)) {
    return;
  }
  // A non-indexed draw reads exactly [start_vertex, start_vertex + vertex_count), so its
  // reach into the (possibly pooled) buffer is known without consulting any indices.
  uint32_t stream_vertices = 0;
  if (!BindStreams(base, guest_device, start_vertex + vertex_count, &stream_vertices)) {
    return;
  }
  BindTextures(base, guest_device);
  ApplyRenderStates(base, guest_device);
  device_->DrawPrimitive(host_prim, start_vertex, prim_count);
  ++draws_submitted_;
}

void Renderer::DrawIndexedUP(const uint8_t* base, uint32_t guest_device, uint32_t prim_type,
                             uint32_t num_vertices, uint32_t index_count, uint32_t guest_index_addr,
                             uint32_t index_format, uint32_t guest_vertex_addr,
                             uint32_t vertex_stride) {
  std::lock_guard<std::mutex> lock(render_mutex_);
  if (shutting_down_.load(std::memory_order_acquire)) {
    return;
  }
  const D3DPRIMITIVETYPE host_prim = HostPrimitiveType(prim_type);
  const uint32_t prim_count = HostPrimitiveCount(prim_type, index_count);
  if (!device_ || !host_prim || !prim_count || !num_vertices) {
    return;
  }
  ++draws_attempted_;
  ResolveViewport(base, guest_device);

  if (!BindShadersAndConstants(base, guest_device)) {
    return;
  }

  auto& tracker = ResourceTracker::Get();
  // NX1 UP draws bind no CVertexDeclaration -- the vertex layout is baked into the
  // vertex shader's vfetch. Prefer a declaration if one is set (rare), otherwise
  // derive the layout from the bound shader, keyed on the UP stream stride.
  const VertexLayout* layout = tracker.GetVertexLayout(base, guest_device);
  if (!layout) {
    layout = tracker.GetShaderVertexLayout(base, guest_device, vertex_stride);
  }
  if (!layout) {
    return;
  }
  device_->SetVertexDeclaration(layout->decl);

  // The vertex/index payloads live inline in guest memory; repack them into
  // transient host buffers. DrawIndexedPrimitiveUP copies them internally, so the
  // vectors only need to outlive the call.
  std::vector<uint8_t> verts;
  const uint32_t host_stride =
      tracker.ConvertInlineVertices(guest_vertex_addr, num_vertices, vertex_stride, *layout, &verts);
  if (!host_stride) {
    return;
  }
  const uint32_t index_size = (index_format & 0x4) ? 4 : 2;
  std::vector<uint8_t> indices;
  if (!tracker.ConvertInlineIndices(guest_index_addr, index_count, index_size, &indices)) {
    return;
  }

  BindTextures(base, guest_device);
  ApplyRenderStates(base, guest_device);
  const HRESULT hr = device_->DrawIndexedPrimitiveUP(
      host_prim, 0, num_vertices, prim_count, indices.data(),
      index_size == 4 ? D3DFMT_INDEX32 : D3DFMT_INDEX16, verts.data(), host_stride);
  if (SUCCEEDED(hr)) {
    ++draws_submitted_;
  }
}

void Renderer::DrawUP(const uint8_t* base, uint32_t guest_device, uint32_t prim_type,
                      uint32_t vertex_count, uint32_t guest_vertex_addr, uint32_t vertex_stride) {
  std::lock_guard<std::mutex> lock(render_mutex_);
  if (shutting_down_.load(std::memory_order_acquire)) {
    return;
  }
  const D3DPRIMITIVETYPE host_prim = HostPrimitiveType(prim_type);
  const uint32_t prim_count = HostPrimitiveCount(prim_type, vertex_count);
  if (!device_ || !vertex_count) {
    return;
  }
  ++draws_attempted_;
  // The other UP producer (R_DrawIndexedPrimitiveUP's primType==8 branch) is a
  // Xenos rectangle list, which D3D9 has no equivalent for -- it needs the implied
  // 4th vertex synthesized per rect. Not yet handled; count it so the log shows it.
  if (!host_prim || !prim_count) {
    static bool warned = false;
    if (!warned) {
      REXGPU_INFO("nx1_d3d9: DrawUP prim_type {} (rect list?) not translated yet", prim_type);
      warned = true;
    }
    return;
  }
  ResolveViewport(base, guest_device);
  if (!BindShadersAndConstants(base, guest_device)) {
    return;
  }

  auto& tracker = ResourceTracker::Get();
  // Same as DrawIndexedUP: NX1 binds no CVertexDeclaration, so derive the layout
  // from the bound vertex shader's vfetch when none is set.
  const VertexLayout* layout = tracker.GetVertexLayout(base, guest_device);
  if (!layout) {
    layout = tracker.GetShaderVertexLayout(base, guest_device, vertex_stride);
  }
  if (!layout) {
    return;
  }
  device_->SetVertexDeclaration(layout->decl);

  std::vector<uint8_t> verts;
  const uint32_t host_stride =
      tracker.ConvertInlineVertices(guest_vertex_addr, vertex_count, vertex_stride, *layout, &verts);
  if (!host_stride) {
    return;
  }

  BindTextures(base, guest_device);
  ApplyRenderStates(base, guest_device);
  if (SUCCEEDED(device_->DrawPrimitiveUP(host_prim, prim_count, verts.data(), host_stride))) {
    ++draws_submitted_;
  }
}

#endif  // _WIN32

}  // namespace nx1::d3d9
