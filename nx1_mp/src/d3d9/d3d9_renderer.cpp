/**
 * @file    d3d9_renderer.cpp
 * @brief   Host D3D9 device management for the native NX1 renderer.
 */

// The rex headers must precede <windows.h> (pulled in by d3d9_renderer.h):
// rex/thread.h declares Sleep(std::chrono::milliseconds), which the Win32
// Sleep macro otherwise mangles.
#include <rex/cvar.h>
#include <rex/logging/macros.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/ui/window.h>

#include <algorithm>
#include <mutex>
#include <chrono>
#include <vector>

#include <xxhash.h>

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

// TEMP PROFILING: accumulate per-phase draw cost and report it once a second.
REXCVAR_DEFINE_BOOL(nx1_d3d9_profile, false, "GPU",
                    "Debug: log where per-frame renderer time goes (per-phase ms + draw count).");

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
  // Prefer the real rex host window. With host presentation suppressed (nx1_d3d9,
  // see Nx1MpApp::OnPreSetup) nothing else draws to it, so the D3D9 device renders
  // straight into its client area -- one window, not two.
  if (auto* rt = rex::Runtime::instance()) {
    if (auto* win = rt->display_window()) {
      if (void* h = win->GetNativeWindowHandle()) {
        return static_cast<HWND>(h);
      }
    }
  }
  // Fallback: enumerate this process's own top-level windows.
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

bool Renderer::Initialize(HWND output_window) {
  if (device_) {
    return true;
  }

  HRESULT hr = Direct3DCreate9Ex(D3D_SDK_VERSION, &d3d_);
  if (FAILED(hr) || !d3d_) {
    REXGPU_ERROR("nx1_d3d9: Direct3DCreate9Ex failed ({:#x})", static_cast<uint32_t>(hr));
    return false;
  }

  HWND hwnd = nullptr;
  if (output_window) {
    // Borrow the host window. rex created it and pumps its message loop on the UI
    // thread; with the Xenia presenter suppressed (nx1_d3d9) nothing else draws to
    // it, so this D3D9 device owns its client area. We must not spawn our own
    // message loop for a window we don't own, nor tear it down at shutdown.
    hwnd = output_window;
    owns_window_ = false;
    REXGPU_INFO("nx1_d3d9: rendering into the host window ({:#x})",
                reinterpret_cast<uintptr_t>(hwnd));
  } else {
    // No host window was handed to us -- fall back to our own top-level window on
    // a dedicated message-pump thread. Win32 delivers a window's messages only to
    // the thread that created it, so wait for the thread to publish the HWND (or
    // fail) before creating the device against it.
    window_thread_ = std::thread(&Renderer::WindowThreadMain, this, 1280u, 720u);
    while (!window_ready_.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
    hwnd = window_ready_hwnd_.load(std::memory_order_acquire);
    if (!hwnd) {
      REXGPU_ERROR("nx1_d3d9: failed to create output window");
      if (window_thread_.joinable()) {
        window_thread_.join();
      }
      d3d_->Release();
      d3d_ = nullptr;
      return false;
    }
    owns_window_ = true;
  }

  // Size the backbuffer to the output window's client area, else a sane default.
  RECT client{0, 0, 1280, 720};
  GetClientRect(hwnd, &client);
  uint32_t width = uint32_t(client.right - client.left);
  uint32_t height = uint32_t(client.bottom - client.top);
  if (!width || !height) {
    width = 1280;
    height = 720;
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
  // Only tear down a window we created. When we borrowed the rex host window it is
  // rex's to destroy -- closing it here would take the whole app's window down.
  // DestroyWindow must run on the thread that created the window; ask it to close
  // (DefWindowProc -> DestroyWindow -> WM_DESTROY -> WM_QUIT) and let it exit. The
  // window thread only pumps messages -- it never touches render_mutex_ -- so join
  // it outside the lock.
  if (owns_window_) {
    if (hwnd_) {
      PostMessageW(hwnd_, WM_CLOSE, 0, 0);
    }
    if (window_thread_.joinable()) {
      window_thread_.join();
    }
  }
  hwnd_ = nullptr;
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

  static auto last_present = std::chrono::steady_clock::now();
  const auto t_present = std::chrono::steady_clock::now();
  prof_frame_ns_ += uint64_t((t_present - last_present).count());
  last_present = t_present;
  device_->PresentEx(nullptr, nullptr, nullptr, nullptr, 0);
  prof_present_ns_ += uint64_t((std::chrono::steady_clock::now() - t_present).count());

  // TEMP PROFILING: report the frame's phase breakdown. Present is timed too -- if the frame
  // is GPU-bound (or vsync-blocked) the cost lands there, not in our CPU-side setup, and that
  // distinction decides whether to optimise our per-draw work or the GPU workload itself.
  // Start every frame with a real bind, so a skip chain always descends from a draw that
  // refreshed the cache entries' last_frame within this frame.
  last_sig_valid_ = false;

  const bool prof_on = REXCVAR_GET(nx1_d3d9_profile);
  ResourceTracker::Get().prof_enabled_ = prof_on;
  if (prof_on) {
    static uint32_t prof_frames = 0;
    if (++prof_frames >= 60) {
      const auto tp = ResourceTracker::Get().TakeTextureProfile();
      const double tf = 1.0 / (1e6 * prof_frames);
      // Splits the textures phase: cache resolution vs rebuild, and the rebuild into its stages.
      REXGPU_INFO("nx1_d3d9: PROF/tex lookup={:.2f} upload={:.2f} ms/frame over {:.1f} rebuilds/frame "
                  "| stage={:.2f} decode={:.2f} mipgen={:.2f} commit={:.2f} ms | {:.0f} us/rebuild",
                  tp.lookup_ns * tf, tp.upload_ns * tf, double(tp.uploads) / prof_frames,
                  tp.stage_ns * tf, tp.decode_ns * tf, tp.mipgen_ns * tf, tp.commit_ns * tf,
                  tp.uploads ? double(tp.upload_ns) / (1e3 * tp.uploads) : 0.0);
      // Why the rebuilds happen, and at what memory cost -- the two numbers that decide whether
      // the fix is the commit policy, the eviction policy, or the streaming pool.
      REXGPU_INFO("nx1_d3d9: PROF/tex why: new={:.1f} layout={:.1f} dirty={:.1f} zero={:.1f} per "
                  "frame | {:.2f} MB/frame decoded ({:.1f} GB/s effective)",
                  double(tp.why_new) / prof_frames, double(tp.why_layout) / prof_frames,
                  double(tp.why_dirty) / prof_frames, double(tp.why_zero) / prof_frames,
                  double(tp.decode_bytes) / (1024.0 * 1024.0 * prof_frames),
                  tp.decode_ns ? double(tp.decode_bytes) / double(tp.decode_ns) : 0.0);
      REXGPU_INFO("nx1_d3d9: PROF/bind skipped {:.1f}% of {:.0f} texture binds/frame",
                  prof_bind_calls_ ? 100.0 * double(prof_bind_skips_) / double(prof_bind_calls_)
                                   : 0.0,
                  double(prof_bind_calls_) / prof_frames);
      prof_bind_skips_ = prof_bind_calls_ = 0;
      const double f = 1.0 / (1e6 * prof_frames);  // ns totals -> ms per frame
      REXGPU_INFO("nx1_d3d9: PROF/frame draws={} | viewport={:.2f} shaders={:.2f} indices={:.2f} "
                  "streams={:.2f} textures={:.2f} states={:.2f} draw={:.2f} present={:.2f} ms "
                  "samp/draw={:.1f} | FRAME={:.2f} outside={:.2f}",
                  prof_draws_ / prof_frames, prof_viewport_ns_ * f, prof_shaders_ns_ * f,
                  prof_indices_ns_ * f, prof_streams_ns_ * f, prof_textures_ns_ * f,
                  prof_states_ns_ * f, prof_draw_ns_ * f, prof_present_ns_ * f,
                  prof_draws_ ? double(prof_sampler_slots_) / double(prof_draws_) : 0.0,
                  prof_frame_ns_ * f,
                  (prof_frame_ns_ - (prof_viewport_ns_ + prof_shaders_ns_ + prof_indices_ns_ +
                                     prof_streams_ns_ + prof_textures_ns_ + prof_states_ns_ +
                                     prof_draw_ns_ + prof_present_ns_)) * f);
      prof_frames = 0;
      prof_viewport_ns_ = prof_shaders_ns_ = prof_indices_ns_ = prof_streams_ns_ = 0;
      prof_textures_ns_ = prof_states_ns_ = prof_draw_ns_ = prof_present_ns_ = prof_draws_ = 0;
      prof_sampler_slots_ = 0;
      prof_frame_ns_ = 0;
      prof_sampler_slots_ = 0;
      prof_frame_ns_ = 0;
    }
  }

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
  // NOTE: a fingerprint-guarded memo was tried here and measured NO gain (shaders 4.25 ->
  // 4.06ms, inside noise) -- so the per-draw cost is the constant upload around it, not this
  // hash. Reverted rather than kept: the blob is NOT immutable (D3D::DirectShaderPatch rewrites
  // vertex microcode in place to bake in the vertex declaration), so any memo risks binding a
  // stale translated shader. Not worth a correctness footgun for an unmeasurable win.
  return XXH3_64bits(bytes, size_t(ucode.dword_count) * sizeof(uint32_t));
}

}  // namespace

void Renderer::UploadVertexUniforms(const Sm3Shader& shader, uint32_t base_reg) {
  // Packed to spend only two of vs_3_0's 256 registers:
  //   [0] = (ndcScale.xyz,  halfPixelOffset.x)
  //   [1] = (ndcOffset.xyz, halfPixelOffset.y)
  // ndcScale/ndcOffset are resolved from the guest viewport by ResolveViewport.
  // The half-pixel term is zero: this is a real D3D9 device, so the guest's own
  // D3D9-authored half-pixel handling already applies -- adding Xenia's D3D12
  // compensation would double it.
  //
  // Def-mask checked like every other host write: fxc parks `def` literals in registers whose
  // declared uniform went unused, and NDC params over a def poisons the flattened cmps.
  const float params[8] = {
      ndc_scale_[0],  ndc_scale_[1],  ndc_scale_[2],  0.0f,
      ndc_offset_[0], ndc_offset_[1], ndc_offset_[2], 0.0f,
  };
  if (!shader.IsDefRegister(base_reg)) {
    device_->SetVertexShaderConstantF(base_reg, params, 1);
  }
  if (!shader.IsDefRegister(base_reg + 1)) {
    device_->SetVertexShaderConstantF(base_reg + 1, &params[4], 1);
  }
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

void Renderer::UploadPixelUniforms(const Sm3Shader& shader, uint32_t base_reg,
                                   bool needs_inv_tex_dim, const uint8_t* base,
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
  //
  // Every write here checks the def mask: when a shader's alpha test was optimized out (or it
  // never had one), fxc reuses exactly these registers for its `def` literals, and writing the
  // threshold/compare over `def cN, 0, 1, -1, -0` feeds garbage into every flattened cmp.
  // Measured live on the sun-shadow shaders: c20=(0.502,0,0,0), c21=(7,0,0,0) -- these two
  // values -- sitting where defs belonged. That was the sun-shadow killer.
  const float threshold[4] = {alpha.threshold, 0.0f, 0.0f, 0.0f};
  const float compare[4] = {float(alpha.enabled ? alpha.compare_function : 7u), 0.0f, 0.0f, 0.0f};
  if (!shader.IsDefRegister(base_reg)) {
    device_->SetPixelShaderConstantF(base_reg, threshold, 1);
  }
  if (!shader.IsDefRegister(base_reg + 1)) {
    device_->SetPixelShaderConstantF(base_reg + 1, compare, 1);
  }

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
  for (uint32_t s = 0; s < 16; ++s) {
    if (!shader.IsDefRegister(base_reg + 2 + s)) {
      device_->SetPixelShaderConstantF(base_reg + 2 + s, &dims[s * 4], 1);
    }
  }
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
        // The RT size names what the dropped draw was producing (scene, shadow atlas, ...).
        REXGPU_WARN("nx1_d3d9: shader cache miss: vs=0x{:016X} ({}) ps=0x{:016X} ({}) rt={}x{} "
                    "-- draw dropped",
                    vs_hash, vs ? "ok" : "MISSING", ps_hash,
                    !ps_object ? "none" : (ps ? "ok" : "MISSING"), current_rt_width_,
                    current_rt_height_);
      }
    }
    return false;
  }



  device_->SetVertexShader(vs->vs);
  cache.UploadConstants(base, guest_device, *vs, /*pixel_stage=*/false);
  if (!NeedsHostNdcTransform(*vs)) {
    UploadVertexUniforms(*vs, HostConstantCount(*vs));
  }

  // Latch which sampler slots these shaders can actually read, so BindTextures skips the rest.
  // A draw with no pixel shader samples nothing at all (depth/shadow-only pass).
  active_sampler_mask_ = 0;
  if (ps) {
    active_sampler_mask_ |= ps->all_samplers ? 0xFFFFu : ps->sampler_mask;
  }
  if (vs && vs->all_samplers) {
    active_sampler_mask_ = 0xFFFFu;  // unwalkable VS: cannot rule out vertex-texture fetch
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
    UploadPixelUniforms(*ps, HostConstantCount(*ps), ps->needs_inv_tex_dim, base, guest_device);
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

void Renderer::BindTextures(const uint8_t* base, uint32_t guest_device, uint64_t surface_key) {
  auto& tracker = ResourceTracker::Get();
  // A scene is ~5000 draws over 16 samplers, and consecutive draws overwhelmingly share
  // their textures and filters -- so shadow the sampler state and only touch D3D when it
  // actually changes. Issuing all of it unconditionally was 80k SetTexture and ~500k
  // SetSamplerState calls a frame, nearly every one of them a no-op.
  //
  // Comparing texture *pointers* is sound: D3D9 holds its own reference to a bound texture,
  // so an object we release cannot be freed (and its address reused) while still bound.
  // Only the slots the bound shaders declare. Everything else cannot be sampled, so resolving
  // its texture (a map lookup, an LOD substitution and a clamp-mode read) is pure cost; the
  // stale binding left in those slots is unobservable. This is the frame's single biggest win:
  // shaders typically declare a handful of the 16 slots.
  const uint32_t sampler_mask = active_sampler_mask_;
  prof_sampler_slots_ += uint64_t(__builtin_popcount(sampler_mask & 0xFFFFu));

  // Draw-signature skip. The fetch constants are compared as raw bytes -- no byte swap, since
  // only "did this change" matters -- so the check is a handful of 24-byte memcmps against the
  // previous draw, far cheaper than the per-slot work it elides.
  ++prof_bind_calls_;
  uint32_t sig[kSigMaxDwords];
  uint32_t sig_n = 0;
  for (uint32_t sampler = 0; sampler < 16; ++sampler) {
    if (!(sampler_mask & (1u << sampler))) {
      continue;
    }
    const uint32_t addr = FetchConstantAddr(guest_device, sampler);
    const uint8_t* fc = addr >= 0xA0000000u ? GuestTranslatePhysical(addr) : base + addr;
    if (!fc) {
      sig_n = 0;  // cannot characterise this draw: fall through and bind it properly
      break;
    }
    std::memcpy(&sig[sig_n], fc, 6 * sizeof(uint32_t));
    sig_n += 6;
  }
  if (sig_n && last_sig_valid_ && last_sig_dwords_ == sig_n && last_sig_mask_ == sampler_mask &&
      last_sig_surface_ == surface_key &&
      std::memcmp(last_sig_, sig, sig_n * sizeof(uint32_t)) == 0) {
    ++prof_bind_skips_;
    return;
  }
  if (sig_n) {
    std::memcpy(last_sig_, sig, sig_n * sizeof(uint32_t));
    last_sig_dwords_ = sig_n;
    last_sig_mask_ = sampler_mask;
    last_sig_surface_ = surface_key;
    last_sig_valid_ = true;
  } else {
    last_sig_valid_ = false;
  }

  for (uint32_t sampler = 0; sampler < 16; ++sampler) {
    if (!(sampler_mask & (1u << sampler))) {
      continue;
    }
    // NOTE: a fetch-constant memo was tried here and MEASURED NET-NEGATIVE (textures 8.05 ->
    // 9.27ms). Consecutive draws are usually different materials, so the constant differs, the
    // memo misses, and the compare is pure added cost. Don't re-add it without data.
    // GetTexture hands back the fetch constant it already read; re-reading it here cost six
    // byte-swapped guest dwords a second time on every bound slot.
    TextureFetchConstant t{};
    IDirect3DBaseTexture9* tex = tracker.GetTexture(base, guest_device, sampler, &t);
    if (tex) {
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
        uint32_t(t.gamma ? TRUE : FALSE),
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

  // TEMP PROFILING: per-phase frame cost. ~2000 draws/frame at ~19us each says the bottleneck
  // is our own per-draw work, not Xenia -- but which part is guesswork until measured.
  using PClock = std::chrono::steady_clock;
  const bool prof = REXCVAR_GET(nx1_d3d9_profile);
  auto ptick = [&] { return prof ? PClock::now() : PClock::time_point{}; };
  auto padd = [&](uint64_t& slot, PClock::time_point t0) {
    if (prof) slot += uint64_t((PClock::now() - t0).count());
  };

  auto t_vp = ptick();
  ResolveViewport(base, guest_device);
  padd(prof_viewport_ns_, t_vp);

  auto t_sh = ptick();
  const bool shaders_ok = BindShadersAndConstants(base, guest_device);
  padd(prof_shaders_ns_, t_sh);
  if (!shaders_ok) {
    return;
  }

  // Indices first: how far into the vertex buffer this draw actually reaches decides how
  // much of it we have to mirror. The guest's fetch constant would have us mirror the whole
  // pool the model happens to live in.
  auto t_ib = ptick();
  uint32_t index_size = 0;
  IDirect3DIndexBuffer9* ib = ResourceTracker::Get().GetIndexBuffer(base, guest_device, &index_size);
  if (!ib) {
    padd(prof_indices_ns_, t_ib);
    return;
  }
  const uint32_t max_index =
      ResourceTracker::Get().GetDrawMaxIndex(base, guest_device, start_index, index_count);
  const uint32_t needed_vertices = max_index ? base_vertex_index + max_index + 1 : 0;
  padd(prof_indices_ns_, t_ib);

  auto t_vb = ptick();
  uint32_t vertex_count = 0;
  const bool streams_ok = BindStreams(base, guest_device, needed_vertices, &vertex_count);
  padd(prof_streams_ns_, t_vb);
  if (!streams_ok) {
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
  auto t_tex = ptick();
  BindTextures(base, guest_device, surface_key);
  padd(prof_textures_ns_, t_tex);

  auto t_rs = ptick();
  ApplyRenderStates(base, guest_device);
  padd(prof_states_ns_, t_rs);

  auto t_dr = ptick();
  const HRESULT dhr = device_->DrawIndexedPrimitive(host_prim, int(base_vertex_index), 0,
                                                    vertex_count, start_index, prim_count);
  padd(prof_draw_ns_, t_dr);
  if (prof) ++prof_draws_;
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
