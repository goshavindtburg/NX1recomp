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

REXCVAR_DEFINE_BOOL(nx1_d3d9_debug_clear, true, "GPU",
                    "Diagnostic: clear the D3D9 window to a distinctive colour each frame instead "
                    "of the guest's tiling clear. Lets you tell a live-but-empty window (solid teal "
                    "= present works, draws aren't landing) from a broken one (black). Turn off once "
                    "the renderer is producing real frames.");

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
  pp.BackBufferFormat = D3DFMT_X8R8G8B8;
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

  if (!ShaderCache::Get().Initialize(device_)) {
    REXGPU_ERROR("nx1_d3d9: shader cache unavailable; every draw would miss");
    Shutdown();
    return false;
  }
  ResourceTracker::Get().Initialize(device_);
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

  // D3D9's Clear is clipped to the current viewport, and the viewport still holds
  // whatever the previous frame's last draw left behind (a 1024x600 scene or a
  // 1024x2048 shadow-map viewport). Without resetting it, the clear only touches that
  // sub-rectangle and the rest of the target is never cleared -- undefined content
  // under SWAPEFFECT_DISCARD. Restore the full target before clearing.
  const uint32_t cw = current_rt_width_ ? current_rt_width_ : backbuffer_width_;
  const uint32_t ch = current_rt_height_ ? current_rt_height_ : backbuffer_height_;
  const D3DVIEWPORT9 full = {0, 0, cw, ch, 0.0f, 1.0f};
  device_->SetViewport(&full);

  // NX1 clears the frame through the EDRAM predicated-tiling path, not
  // D3DDevice_Clear, so nothing else clears our backbuffer. Reproduce it from the
  // guest's stashed tiling clear values (set up before the previous frame's
  // draws). This carries the reverse-Z depth clear (typically 0.0 = far).
  // TODO(d3d9): fold into real render-target/resolve handling once that exists.
  if (REXCVAR_GET(nx1_d3d9_debug_clear)) {
    // Distinctive teal so an empty-but-live window is unmistakable from a black,
    // never-presented one. Depth still clears to the reverse-Z far plane (0.0).
    device_->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL,
                   D3DCOLOR_ARGB(255, 0, 96, 96), 0.0f, 0);
  } else if (last_guest_device_ && last_base_) {
    const TilingClear clear = ReadTilingClear(last_base_, last_guest_device_);
    auto to8 = [](float c) { return uint32_t(std::clamp(c, 0.0f, 1.0f) * 255.0f + 0.5f); };
    const D3DCOLOR color = D3DCOLOR_ARGB(to8(clear.a), to8(clear.r), to8(clear.g), to8(clear.b));
    device_->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL, color,
                   clear.z, clear.stencil);
  }
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
  // DIAG(d3d9): with nx1_d3d9_debug_clear on, show the raw 1024x600 scene resolve
  // instead of the composited frame -- that says whether the world renders at all,
  // independently of whether the composite pass picks it up. TODO(d3d9): drop.
  const uint32_t present_addr =
      (REXCVAR_GET(nx1_d3d9_debug_clear) && scene_resolve_addr_) ? scene_resolve_addr_
                                                                 : display_resolve_addr_;
  if (present_addr) {
    IDirect3DTexture9* frame = ResourceTracker::Get().GetResolvedTexture(present_addr);
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
  if (++frames_presented_ % 120 == 0) {
    REXGPU_INFO("nx1_d3d9: frame {}, draws {}/{} submitted, {} shader-cache misses",
                frames_presented_, draws_submitted_, draws_attempted_, shader_cache_misses_);
    // DIAG(d3d9): which colour target did those draws actually land on? TODO(d3d9): drop.
    for (uint32_t i = 0; i < rt_tally_n_; ++i) {
      REXGPU_INFO("nx1_d3d9: [rtdiag] surface {:#010x} {}x{} -> {} draws ({} writing colour)",
                  rt_tally_[i].surface, rt_tally_[i].width, rt_tally_[i].height,
                  rt_tally_[i].draws, rt_tally_[i].color_draws);
    }
    rt_tally_n_ = 0;
  }

  // The window's messages are pumped on its own thread (see WindowThreadMain), so
  // there is nothing to pump here -- doing it from this (render) thread would be a
  // no-op anyway: Win32 delivers a window's messages only to its creator thread.
}

void Renderer::Clear(uint32_t flags, uint32_t color, float z, uint32_t stencil) {
  std::lock_guard<std::mutex> lock(render_mutex_);
  if (shutting_down_.load(std::memory_order_acquire) || !device_) {
    return;
  }
  // D3D9 Clear fails the whole call (clearing nothing) if any unknown flag bit is
  // set, and the guest packs EDRAM-specific bits alongside the standard ones.
  // Mask to what D3D9 accepts. The Xbox 360 D3DCLEAR_* values match desktop D3D9.
  const DWORD host_flags = flags & (D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL);
  if (!host_flags) {
    return;
  }
  // DIAG(d3d9): log the first clears to catch a mid-frame black clear (a post pass
  // targeting a separate EDRAM RT that collapses onto our shared backbuffer). TODO: drop.
  {
    static int clr_diag = 0;
    if (clr_diag < 40) {
      REXGPU_INFO("nx1_d3d9: [cleardiag {}] flags=0x{:X} color=0x{:08X} z={:.3f} frame={}", clr_diag,
                  host_flags, color, z, frames_presented_);
      ++clr_diag;
    }
  }
  // z is the guest's clear value verbatim -- under NX1's reverse-Z that is 0.0 for
  // the far plane, which is exactly what the D3DCMP_GREATEREQUAL depth test wants.
  device_->Clear(0, nullptr, host_flags, color, z, stencil);
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
  // DIAG(d3d9): a D3DSurface is not a D3DBaseTexture -- decoding its header as a fetch
  // constant produced 441x4066 garbage. Dump the raw header for each distinct surface
  // so we can find where the real width/height live. TODO(d3d9): drop once decoded.
  {
    static uint32_t seen[8] = {};
    static uint32_t seen_n = 0;
    bool known = false;
    for (uint32_t i = 0; i < seen_n; ++i) {
      if (seen[i] == guest_surface) { known = true; break; }
    }
    if (!known && seen_n < 8) {
      seen[seen_n++] = guest_surface;
      uint32_t d[16];
      for (uint32_t i = 0; i < 16; ++i) {
        d[i] = GuestRead32(base, guest_surface + i * 4);
      }
      REXGPU_INFO(
          "nx1_d3d9: [surfdiag] rt idx={} obj=0x{:08X} "
          "[0]{:08X} [1]{:08X} [2]{:08X} [3]{:08X} [4]{:08X} [5]{:08X} [6]{:08X} [7]{:08X} "
          "[8]{:08X} [9]{:08X} [10]{:08X} [11]{:08X} [12]{:08X} [13]{:08X} [14]{:08X} [15]{:08X}",
          index, guest_surface, d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7], d[8], d[9], d[10],
          d[11], d[12], d[13], d[14], d[15]);
    }
  }

  // DIAG(d3d9): the real bind sequence, with thread ids. The depth targets keep getting
  // grown/reallocated by a *stale* colour target from another pass, and the log shows
  // guest D3D arriving on several host threads -- this says whether passes actually
  // interleave across threads or whether the state tracking is simply wrong. TODO: drop.
  IDirect3DSurface9* surface =
      ResourceTracker::Get().GetRenderTargetSurface(base, guest_surface);
  if (surface) {
    device_->SetRenderTarget(index, surface);
    if (index == 0) {
      current_rt_surface_ = guest_surface;
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
                       uint32_t dest_point) {
  std::lock_guard<std::mutex> lock(render_mutex_);
  if (shutting_down_.load(std::memory_order_acquire) || !device_ || !dest_texture) {
    return;
  }
  const TextureFetchConstant dest = ReadBaseTextureFormat(base, dest_texture);
  // The 1024x600 scene target only exists in-game; use it to gate the world diagnostics.
  if (dest.width == 1024 && dest.height == 600) {
    saw_scene_target_.store(true, std::memory_order_release);
  }
  // DIAG(d3d9): confirm resolves fire at all, and their dest format/dims. If these
  // never appear, NX1 isn't resolving via D3DDevice_Resolve (PM4/other path). TODO: drop.
  {
    // Log each *distinct* resolve destination once, so in-game intermediate resolves
    // (post/bloom scene textures) show up instead of the first frames eating the budget.
    static uint32_t seen[32] = {};
    static uint32_t seen_n = 0;
    bool known = false;
    for (uint32_t i = 0; i < seen_n; ++i) {
      if (seen[i] == dest.base_address) { known = true; break; }
    }
    if (!known && seen_n < 32) {
      seen[seen_n++] = dest.base_address;
      REXGPU_INFO(
          "nx1_d3d9: [resolvediag] dest=0x{:08X} fmt={} {}x{} frame={} <- rt surface {:#010x} "
          "{}x{}",
          dest.base_address, dest.format, dest.width, dest.height, frames_presented_,
          current_rt_surface_, current_rt_width_, current_rt_height_);
    }
  }
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
    // DIAG(d3d9): the shadow depth surfaces decode to 1024x1024 / 512x512 but resolve
    // into 1024x2048 / 512x2048 destinations -- so the guest is stacking cascades into
    // an atlas. Log the source rect and destination point to recover that layout, and
    // how many distinct resolves each atlas takes. TODO: drop.
    static uint32_t seen[24] = {};
    static uint32_t seen_n = 0;
    bool known = false;
    for (uint32_t i = 0; i < seen_n; ++i) {
      if (seen[i] == (dest.base_address ^ uint32_t(at.y))) { known = true; break; }
    }
    if (!known && seen_n < 24) {
      seen[seen_n++] = dest.base_address ^ uint32_t(at.y);
      uint32_t dw = 0, dh = 0;
      ReadSurfaceSize(base, current_depth_surface_, &dw, &dh);
      REXGPU_INFO(
          "nx1_d3d9: [depthdiag] resolve dest=0x{:08X} {}x{} <- surface 0x{:08X} {}x{} "
          "src=({},{})-({},{}) at=({},{})",
          dest.base_address, dest.width, dest.height, current_depth_surface_, dw, dh, rect.left,
          rect.top, rect.right, rect.bottom, at.x, at.y);
    }
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
  // DIAG(d3d9): the 1024x600 scene colour resolve. Presenting this directly answers
  // whether the world is being rendered at all, independent of the composite. TODO: drop.
  if (dest.width == 1024 && dest.height == 600) {
    scene_resolve_addr_ = dest.base_address;
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

  // DIAG(d3d9): the last unverified step between correct constants and correct pixels.
  // For a full-target render this fold should come out as scale (1, 1, 1) offset (0, 0, 0)
  // -- anything else is distorting every vertex. TODO(d3d9): drop.
  {
    static uint32_t dumped = 0;
    static float last[6] = {};
    const float now[6] = {ndc_scale_[0],  ndc_scale_[1],  ndc_scale_[2],
                          ndc_offset_[0], ndc_offset_[1], ndc_offset_[2]};
    if (dumped < 12 && std::memcmp(last, now, sizeof(now)) != 0) {
      std::memcpy(last, now, sizeof(now));
      ++dumped;
      REXGPU_INFO(
          "nx1_d3d9: [ndcdiag] scale ({: .4f} {: .4f} {: .4f}) offset ({: .4f} {: .4f} {: .4f}) "
          "at reg c{} | rt {}x{}",
          now[0], now[1], now[2], now[3], now[4], now[5], base_reg, current_rt_width_,
          current_rt_height_);
    }
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

void Renderer::UploadPixelUniforms(uint32_t base_reg, const uint8_t* base, uint32_t guest_device) {
  const AlphaTestState alpha = ReadAlphaTestState(base, guest_device);

  // The shader's alpha-test branch was baked in from its spec-constant mask, so a
  // shader that has one always runs it. Disabling the test at the render-state
  // level therefore has to be expressed as "always pass" (compare function 7).
  const float threshold[4] = {alpha.threshold, 0.0f, 0.0f, 0.0f};
  const float compare[4] = {float(alpha.enabled ? alpha.compare_function : 7u), 0.0f, 0.0f, 0.0f};
  device_->SetPixelShaderConstantF(base_reg, threshold, 1);
  device_->SetPixelShaderConstantF(base_reg + 1, compare, 1);
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
    // 14 of 2923 shaders could not be lowered to SM3. Skipping the draw is wrong
    // but bounded and visible; rendering with a stale shader would not be.
    if ((shader_cache_misses_++ % 1000) == 0) {
      REXGPU_WARN("nx1_d3d9: shader cache miss (vs=0x{:016X} ps=0x{:016X}), {} so far", vs_hash,
                  ps_hash, shader_cache_misses_);
    }
    // One-time dump: is the ucode we hash even valid, and does the physical vs
    // virtual translation of its address disagree (the 0xE0 page-offset again)?
    static bool dumped = false;
    if (!dumped) {
      dumped = true;
      auto* mem = rex::system::kernel_state()->memory();
      auto dump = [&](const char* tag, const GuestUcode& u, bool present) {
        if (!present) {
          return;
        }
        const auto* w = mem->TranslatePhysical<const rex::be<uint32_t>*>(u.physical_address);
        REXGPU_WARN("nx1_d3d9: {} ucode addr=0x{:08X} dwords={} [0..3]=0x{:08X},0x{:08X},0x{:08X},"
                    "0x{:08X}",
                    tag, u.physical_address, u.dword_count, uint32_t(w[0]), uint32_t(w[1]),
                    uint32_t(w[2]), uint32_t(w[3]));
      };
      dump("vs", ReadGuestUcode(base, vs_object, /*pixel_shader=*/false, vs_pass), vs == nullptr);
      dump("ps", ReadGuestUcode(base, ps_object, /*pixel_shader=*/true), ps_object && !ps);
    }
    return false;
  }

  device_->SetVertexShader(vs->vs);
  cache.UploadConstants(base, guest_device, *vs, /*pixel_stage=*/false);
  if (!NeedsHostNdcTransform(*vs)) {
    UploadVertexUniforms(HostConstantCount(*vs));
  }

  device_->SetPixelShader(ps ? ps->ps : nullptr);
  current_draw_writes_color_ = ps != nullptr;  // DIAG(d3d9): see rt_tally_. TODO: drop.
  // A draw with no pixel shader is a depth/shadow-only pass: the Xbox exports no
  // color, so nothing is written to the render target. On D3D9, SetPixelShader(null)
  // instead activates the fixed-function pixel pipeline, which *does* write color
  // (the surface's diffuse/texture as solid fill) and paints garbage over the frame.
  // Mask color writes off for those draws so they contribute depth only.
  device_->SetRenderState(D3DRS_COLORWRITEENABLE, ps ? 0xF : 0);
  if (ps) {
    cache.UploadConstants(base, guest_device, *ps, /*pixel_stage=*/true);
    UploadPixelUniforms(HostConstantCount(*ps), base, guest_device);
    // DIAG(d3d9): the world renders black while texture-only draws (UI/decals) look
    // right -- dump the guest PS constant window for the first few draws to see if the
    // lighting constants are actually populated (all-zero => black shading). TODO: drop.
    // Only shaders that actually consume constants tell us anything -- the UI/text
    // shaders use none. A world lighting PS uses several; if its constant window reads
    // back all-zero, every light term is 0 and the surface shades black.
    // Gate on being in-game (the guest has resolved the 1024x600 scene target),
    // otherwise the budget gets eaten by menu/UI draws, which use no lighting constants.
    static int pc_diag = 0;
    const uint32_t ps_n = HostConstantCount(*ps);
    if (pc_diag < 10 && ps_n >= 4 && saw_scene_target_.load(std::memory_order_acquire)) {
      // Dump the guest PS constant window at the *remapped* registers the shader
      // actually reads, not just c0 -- a compacted shader may reference c40, c60, ...
      float c[16];
      for (uint32_t i = 0; i < 16; ++i) {
        c[i] = GuestReadF32(base, guest_device + guest_device::kPsConstants + i * 4);
      }
      // How many of the first 64 float4s in the PS window are non-zero? A populated
      // lighting window should have plenty; an all-zero one is the smoking gun.
      uint32_t nonzero = 0;
      for (uint32_t r = 0; r < 64; ++r) {
        for (uint32_t comp = 0; comp < 4; ++comp) {
          if (GuestReadF32(base, guest_device + guest_device::kPsConstants + (r * 4 + comp) * 4) !=
              0.0f) {
            ++nonzero;
            break;
          }
        }
      }
      REXGPU_INFO(
          "nx1_d3d9: [constdiag {}] ps=0x{:016X} ps_n={} nonzero_of_64={} "
          "psc0=({:.3f},{:.3f},{:.3f},{:.3f}) psc1=({:.3f},{:.3f},{:.3f},{:.3f}) "
          "psc2=({:.3f},{:.3f},{:.3f},{:.3f}) psc3=({:.3f},{:.3f},{:.3f},{:.3f})",
          pc_diag, ps_hash, ps_n, nonzero, c[0], c[1], c[2], c[3], c[4], c[5], c[6], c[7], c[8],
          c[9], c[10], c[11], c[12], c[13], c[14], c[15]);
      ++pc_diag;
    }
  }
  return true;
}

bool Renderer::BindStreams(const uint8_t* base, uint32_t guest_device, uint32_t* vertex_count) {
  auto& tracker = ResourceTracker::Get();
  // NX1 binds no CVertexDeclaration; for bound-buffer draws derive the multi-stream
  // layout from the vertex shader's vfetch (stream0_stride=0 selects bound mode).
  // Diagnostic: which sub-stage of stream binding fails for in-game draws. TODO(d3d9): drop.
  auto bail_once = [](const char* stage) {
    static const char* seen[8] = {};
    for (auto& s : seen) {
      if (s == stage) return;
      if (!s) {
        s = stage;
        REXGPU_INFO("nx1_d3d9: BindStreams bail at '{}'", stage);
        return;
      }
    }
  };
  const VertexLayout* layout = tracker.GetVertexLayout(base, guest_device);
  if (!layout) {
    layout = tracker.GetShaderVertexLayout(base, guest_device, /*stream0_stride=*/0);
  }
  if (!layout) {
    bail_once("layout");
    return false;
  }
  device_->SetVertexDeclaration(layout->decl);

  *vertex_count = 0;
  for (uint32_t stream = 0; stream < layout->stream_count; ++stream) {
    uint32_t count = 0;
    IDirect3DVertexBuffer9* vb =
        tracker.GetVertexBuffer(base, guest_device, stream, *layout, &count);
    if (!vb) {
      bail_once("vertexbuffer");
      return false;
    }
    device_->SetStreamSource(stream, vb, 0, layout->host_stride[stream]);
    // The draw's vertex range has to fit inside every stream it reads.
    *vertex_count = *vertex_count ? std::min(*vertex_count, count) : count;
  }
  if (*vertex_count == 0) {
    bail_once("vertexcount");
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

void Renderer::ApplyRenderStates(const uint8_t* base, uint32_t guest_device, bool bound_draw) {
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
  // DIAG(d3d9): log the guest cull/winding for the first few UP (menu) and bound
  // (world) draws so we can tell whether the Y-flip winding sense actually differs
  // between them, instead of guessing. TODO: drop.
  {
    static int cull_diag_up = 0, cull_diag_bound = 0;
    int& counter = bound_draw ? cull_diag_bound : cull_diag_up;
    if (counter < 6) {
      REXGPU_INFO(
          "nx1_d3d9: [culldiag {}] path={} cull_back={} cull_front={} front_cw={} mode={} "
          "ztest={} zwrite={} zfunc={}",
          counter, bound_draw ? "bound" : "up", cull.cull_back, cull.cull_front, cull.front_is_cw,
          int(mode), depth.test_enabled, depth.write_enabled, depth.compare_function);
      ++counter;
    }
  }
  device_->SetRenderState(D3DRS_CULLMODE, mode);
}

void Renderer::BindTextures(const uint8_t* base, uint32_t guest_device, bool bound_draw) {
  auto& tracker = ResourceTracker::Get();
  // DIAG(d3d9): one-shot dump of the first few bound-buffer (in-game world) draws'
  // per-sampler texture format + bind status, to see what surfaces sample. TODO: drop.
  static int tex_diag = 0;
  const bool diag = bound_draw && tex_diag < 12;
  bool diag_logged = false;
  for (uint32_t sampler = 0; sampler < 16; ++sampler) {
    IDirect3DBaseTexture9* tex = tracker.GetTexture(base, guest_device, sampler);
    device_->SetTexture(sampler, tex);
    if (diag) {
      const TextureFetchConstant td = ReadTextureFetchConstant(base, guest_device, sampler);
      if (td.valid && td.base_address) {
        REXGPU_INFO("nx1_d3d9: [texdiag {}] sampler={} fmt={} {}x{} tiled={} bound={}", tex_diag,
                    sampler, td.format, td.width, td.height, td.tiled, tex != nullptr);
        diag_logged = true;
      }
    }
    if (!tex) {
      continue;
    }
    const TextureFetchConstant t = ReadTextureFetchConstant(base, guest_device, sampler);
    const SamplerClampModes clamp = ReadSamplerClampModes(base, guest_device, sampler);
    device_->SetSamplerState(sampler, D3DSAMP_ADDRESSU, HostAddressMode(clamp.u));
    device_->SetSamplerState(sampler, D3DSAMP_ADDRESSV, HostAddressMode(clamp.v));
    device_->SetSamplerState(sampler, D3DSAMP_ADDRESSW, HostAddressMode(clamp.w));
    device_->SetSamplerState(sampler, D3DSAMP_MAGFILTER, HostFilter(t.mag_filter));
    device_->SetSamplerState(sampler, D3DSAMP_MINFILTER, HostFilter(t.min_filter));
    device_->SetSamplerState(sampler, D3DSAMP_MIPFILTER, HostFilter(t.mip_filter));
  }
  if (diag && diag_logged) {
    // Log the depth state for this *real* textured world draw. The culldiag in
    // ApplyRenderStates burns its budget on early load draws; this fires on the
    // same draws texdiag captured, so it reflects in-game geometry. TODO: drop.
    const DepthState depth = ReadDepthState(base, guest_device);
    const ViewportState vp = ReadViewportState(base, guest_device);
    const TilingClear tc = ReadTilingClear(base, guest_device);
    REXGPU_INFO(
        "nx1_d3d9: [texdiag {}] depth ztest={} zwrite={} zfunc={} | vp offz={:.4f} scalez={:.4f} "
        "clipdis={} | clear.z={:.4f}",
        tex_diag, depth.test_enabled, depth.write_enabled, depth.compare_function, vp.offset_z,
        vp.scale_z, vp.clip_disable, tc.z);
    ++tex_diag;
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
    return;
  }
  ++draws_attempted_;
  last_base_ = base;
  last_guest_device_ = guest_device;
  // Diagnostic: which stage walls the bound-buffer (in-game) draws. TODO(d3d9): drop.
  auto bail_once = [](const char* stage) {
    static const char* seen[8] = {};
    for (auto& s : seen) {
      if (s == stage) return;
      if (!s) {
        s = stage;
        REXGPU_INFO("nx1_d3d9: DrawIndexed bail at '{}'", stage);
        return;
      }
    }
  };
  // DIAG(d3d9): guest D3D is still arriving on several host threads even with the draw
  // command buffers suppressed. If an off-thread draw uses a *different* guest device, it is
  // a worker recording into its own dx.cmdBufDevice[] and racing our one host device.
  // TODO(d3d9): drop.
  {
    static std::mutex m;
    static std::vector<std::pair<uint32_t, uint32_t>> seen;
    const uint32_t tid = uint32_t(::GetCurrentThreadId());
    std::lock_guard<std::mutex> lk(m);
    if (std::find(seen.begin(), seen.end(), std::make_pair(tid, guest_device)) == seen.end() &&
        seen.size() < 16) {
      seen.emplace_back(tid, guest_device);
      REXGPU_INFO("nx1_d3d9: [devdiag] draw on thread {} with guest device {:#010x}", tid,
                  guest_device);
    }
  }
  ResolveViewport(base, guest_device);
  if (!BindShadersAndConstants(base, guest_device)) {
    bail_once("shaders");
    return;
  }
  uint32_t vertex_count = 0;
  if (!BindStreams(base, guest_device, &vertex_count)) {
    bail_once("streams");
    return;
  }

  uint32_t index_size = 0;
  IDirect3DIndexBuffer9* ib = ResourceTracker::Get().GetIndexBuffer(base, guest_device, &index_size);
  if (!ib) {
    bail_once("indexbuffer");
    return;
  }
  // DIAG(d3d9): does the draw's index range actually fit inside the vertex buffer we
  // mirrored? `vertex_count` comes from the fetch constant's size field, which for the
  // skinned-vertex cache means "bytes to the end of the 4.5 MB pool" -- not this model's
  // vertex count.
  //
  // The ring-path (rigid model) draws all came back exactly sized and in range, so aim
  // this at the draws bound to a *large* stream instead: that is the shared skinned pool,
  // whose models are CPU-skinned into world space and therefore never write a world matrix
  // through the ring. TODO(d3d9): drop.
  if (ReadVertexFetchConstant(base, guest_device, 0).size_bytes > 65536) {
    static std::atomic<uint32_t> budget{3};
    if (budget.load(std::memory_order_relaxed) > 0 &&
        budget.fetch_sub(1, std::memory_order_relaxed) > 0) {
      const IndexBufferState ibs = ReadIndexBuffer(base, guest_device);
      const VertexFetchConstant vf = ReadVertexFetchConstant(base, guest_device, 0);
      uint32_t lo = 0xFFFFFFFFu, hi = 0;
      for (uint32_t i = 0; i < index_count; ++i) {
        const uint32_t at = start_index + i;
        uint32_t idx = 0;
        if (ibs.index_size == 4) {
          idx = GuestRead32(base, ibs.base_address + at * 4);
        } else {
          const uint32_t w = GuestRead32(base, ibs.base_address + (at & ~1u) * 2);
          idx = (at & 1u) ? (w & 0xFFFFu) : (w >> 16);
        }
        lo = std::min(lo, idx);
        hi = std::max(hi, idx);
      }
      REXGPU_INFO(
          "nx1_d3d9: [rangediag] prim={} base_vtx={} start={} n={} | idx {}..{} | vbuf "
          "count={} (base=0x{:08X} size={} stride={}) | ib size={} isz={} {}",
          prim_type, base_vertex_index, start_index, index_count, lo, hi, vertex_count,
          vf.base_address, vf.size_bytes, vf.size_bytes / (vertex_count ? vertex_count : 1),
          ibs.size_bytes, ibs.index_size,
          (hi + base_vertex_index) >= vertex_count ? "*** OUT OF RANGE ***" : "ok");

      // DIAG(d3d9): the rigid (stride 32) models decode correctly, so dump how we decode
      // *this* layout -- the stride-44 world vertex -- plus the position we actually
      // produce for the first index. A garbage position here is the smear. TODO: drop.
      const VertexLayout* vl = ResourceTracker::Get().GetVertexLayout(base, guest_device);
      if (!vl) {
        vl = ResourceTracker::Get().GetShaderVertexLayout(base, guest_device, 0);
      }
      if (vl) {
        for (const ConvertOp& op : vl->ops) {
          REXGPU_INFO(
              "nx1_d3d9: [vfmtdiag] stream={} src+{} -> dst+{} fmt={} src_size={} dst_size={} "
              "signed={} norm={} expand={}",
              op.stream, op.src_offset, op.dst_offset, op.format, op.src_size, op.dst_size,
              op.is_signed, op.is_normalized, op.expand);
        }
        REXGPU_INFO("nx1_d3d9: [vfmtdiag] guest_stride={} host_stride={} bulk_swap={} ops={}",
                    vl->guest_stride[0], vl->host_stride[0], vl->bulk_swap[0], vl->ops.size());
      }
    }
  }

  device_->SetIndices(ib);
  BindTextures(base, guest_device, /*bound_draw=*/true);
  ApplyRenderStates(base, guest_device, /*bound_draw=*/true);

  const HRESULT dhr = device_->DrawIndexedPrimitive(host_prim, int(base_vertex_index), 0,
                                                    vertex_count, start_index, prim_count);
  // DIAG(d3d9): a draw can be *submitted* and still be rejected by D3D9 (a depth buffer
  // smaller than the render target does exactly that, silently). TODO(d3d9): drop.
  if (FAILED(dhr)) {
    static uint64_t fails = 0;
    if ((fails++ % 2000) == 0) {
      REXGPU_WARN("nx1_d3d9: DrawIndexedPrimitive failed ({:#x}) rt={}x{} prim={} verts={} — {} so far",
                  uint32_t(dhr), current_rt_width_, current_rt_height_, prim_count, vertex_count,
                  fails);
    }
  }
  ++draws_submitted_;
  TallyDraw();
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
    return;
  }
  ++draws_attempted_;
  last_base_ = base;
  last_guest_device_ = guest_device;
  ResolveViewport(base, guest_device);
  if (!BindShadersAndConstants(base, guest_device)) {
    return;
  }
  uint32_t stream_vertices = 0;
  if (!BindStreams(base, guest_device, &stream_vertices)) {
    return;
  }
  BindTextures(base, guest_device, /*bound_draw=*/true);
  ApplyRenderStates(base, guest_device, /*bound_draw=*/true);
  device_->DrawPrimitive(host_prim, start_vertex, prim_count);
  ++draws_submitted_;
  TallyDraw();
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
  last_base_ = base;
  last_guest_device_ = guest_device;
  ResolveViewport(base, guest_device);

  // Diagnostic: report the first time each stage rejects a UP draw so a stuck
  // "0/N submitted" tells us exactly which stage is the wall. TODO(d3d9): drop.
  auto bail_once = [](const char* stage) -> bool {
    static const char* seen[8] = {};
    for (auto& s : seen) {
      if (s == stage) return false;
      if (!s) {
        s = stage;
        REXGPU_INFO("nx1_d3d9: UP draw bail at '{}'", stage);
        return true;
      }
    }
    return false;
  };

  if (!BindShadersAndConstants(base, guest_device)) {
    if (bail_once("shaders")) {
      REXGPU_INFO("nx1_d3d9: UP shaders bail: device=0x{:08X} vs=0x{:08X} ps=0x{:08X} "
                  "dev[0]=0x{:08X}",
                  guest_device, BoundVertexShader(base, guest_device),
                  BoundPixelShader(base, guest_device), GuestRead32(base, guest_device));
    }
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
    if (bail_once("layout")) {
      const uint32_t decl = BoundVertexDeclaration(base, guest_device);
      REXGPU_INFO("nx1_d3d9: UP layout bail: device=0x{:08X} decl=0x{:08X} vs=0x{:08X} "
                  "stride={}",
                  guest_device, decl, BoundVertexShader(base, guest_device), vertex_stride);
    }
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
    if (bail_once("verts")) {
      REXGPU_INFO("nx1_d3d9: UP verts bail: addr=0x{:08X} count={} stride={} host_stride[0]={}",
                  guest_vertex_addr, num_vertices, vertex_stride, layout->host_stride[0]);
    }
    return;
  }
  const uint32_t index_size = (index_format & 0x4) ? 4 : 2;
  std::vector<uint8_t> indices;
  if (!tracker.ConvertInlineIndices(guest_index_addr, index_count, index_size, &indices)) {
    if (bail_once("indices")) {
      REXGPU_INFO("nx1_d3d9: UP indices bail: addr=0x{:08X} count={} size={}", guest_index_addr,
                  index_count, index_size);
    }
    return;
  }

  BindTextures(base, guest_device);
  ApplyRenderStates(base, guest_device);
  const HRESULT hr = device_->DrawIndexedPrimitiveUP(
      host_prim, 0, num_vertices, prim_count, indices.data(),
      index_size == 4 ? D3DFMT_INDEX32 : D3DFMT_INDEX16, verts.data(), host_stride);
  if (SUCCEEDED(hr)) {
    ++draws_submitted_;
    TallyDraw();
  } else {
    bail_once("drawcall");
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
  last_base_ = base;
  last_guest_device_ = guest_device;
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
    TallyDraw();
  }
}

// DIAG(d3d9): see rt_tally_ in the header. TODO(d3d9): drop.
void Renderer::TallyDraw() {
  const uint32_t color = current_draw_writes_color_ ? 1u : 0u;
  for (uint32_t i = 0; i < rt_tally_n_; ++i) {
    if (rt_tally_[i].surface == current_rt_surface_) {
      ++rt_tally_[i].draws;
      rt_tally_[i].color_draws += color;
      return;
    }
  }
  if (rt_tally_n_ < 12) {
    rt_tally_[rt_tally_n_++] = {current_rt_surface_, current_rt_width_, current_rt_height_, 1,
                                color};
  }
}

#endif  // _WIN32

}  // namespace nx1::d3d9
