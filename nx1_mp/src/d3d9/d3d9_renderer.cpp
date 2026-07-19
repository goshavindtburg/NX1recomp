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

#include "d3d9_overlay.h"
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
REXCVAR_DEFINE_BOOL(nx1_d3d9_record, false, "GPU",
                    "Report the deferred-translation recorder's per-frame cost (PROF/record). "
                    "Recording itself is unconditional as of step 2 -- every draw executes from "
                    "its record -- so this gates the log line only");

/// Step 3: execute the recorded command stream on a worker thread. Off by default until it has
/// earned trust -- the correctness argument rests on the threading contract in d3d9_cmdbuf.h and
/// on the measured stability of index/microcode/vertex memory, and a race here would present as
/// intermittent corruption rather than a clean failure.
REXCVAR_DEFINE_BOOL(nx1_d3d9_async, false, "GPU",
                    "Translate recorded GPU commands on a worker thread, overlapping our "
                    "translation with the guest's own between-draw logic");

// Target a blend config by VALUE (the Xenos colour factor pair), never by discovery index.
// Indices shift between runs because they are assigned in first-seen order, which already cost
// two runs and produced one wrong conclusion. src/dst are the numbers BLENDCFG prints.
REXCVAR_DEFINE_INT32(nx1_d3d9_dbg_blend_src, -1, "GPU",
                     "Xenos colour SRC factor of the draws to isolate/verify; -1 = off");
REXCVAR_DEFINE_INT32(nx1_d3d9_dbg_blend_dst, -1, "GPU",
                     "Xenos colour DST factor of the draws to isolate/verify; -1 = off");

REXCVAR_DECLARE(uint32_t, nx1_d3d9_dbg_track_addr);  // defined in d3d9_resources.cpp

/// Narrow the blend isolate to a SINGLE material, keyed by its guest pixel-shader object.
///
/// Every identifier this investigation used before was unstable, and each one produced a
/// confident wrong answer: BLENDCFG discovery indices shift between runs, guest texture addresses
/// move between launches, and sampler slots move between draws IN THE SAME FRAME -- the same four
/// textures appear at s9..s12 in one draw and s8..s11 in the next, which made a "sampler 10 moved"
/// report fire on every unrelated draw and look like pool reallocation. The shader object is fixed
/// for the life of the loaded fastfile, so it names a material rather than a position.
///
/// It also fixes the isolate's real weakness: `1->7` alone hides glass AND hud AND minimap
/// together, so "the glass vanished" never distinguished the glass from everything sharing its
/// blend mode. Take a value from the BLENDPS lines to hide exactly one material.
REXCVAR_DEFINE_UINT32(nx1_d3d9_dbg_blend_ps, 0, "GPU",
                      "Restrict the blend isolate/verify to draws using this guest pixel-shader "
                      "object (from the BLENDPS log lines); 0 = all matching draws");

/// Binary search over the material list. One blend mode covers 32+ materials, so testing them one
/// at a time is 32 runs; halving the shader-object range is five. Shader objects live in the
/// loaded fastfile, so their ADDRESSES are stable across launches -- which is what makes a range
/// search valid here when an index search would not be (discovery order shifts every run, and that
/// has already produced one retracted conclusion in this investigation).
REXCVAR_DEFINE_UINT32(nx1_d3d9_dbg_shaderid_n, 0, "GPU",
                      "Debug: log the microcode hash of the first N distinct pixel-shader "
                      "objects drawn. The hash names tools/new_shader_dump/shader_<HASH>.ucode.frag");

REXCVAR_DEFINE_UINT32(nx1_d3d9_dbg_blend_ps_lo, 0, "GPU",
                      "Restrict the blend isolate/verify to draws whose pixel-shader object is >= "
                      "this value; 0 = no lower bound");
REXCVAR_DEFINE_UINT32(nx1_d3d9_dbg_blend_ps_hi, 0, "GPU",
                      "Restrict the blend isolate/verify to draws whose pixel-shader object is <= "
                      "this value; 0 = no upper bound");

REXCVAR_DEFINE_INT32(nx1_d3d9_dbg_blend_track_sampler, -1, "GPU",
                     "With blend verify on, automatically point nx1_d3d9_dbg_track_addr at this "
                     "sampler's texture in the matched draw. Avoids hand-copying an address "
                     "between runs, which is invalid -- guest allocations move every launch");

REXCVAR_DEFINE_BOOL(nx1_d3d9_dbg_blend_verify, false, "GPU",
                    "With dbg_blend_src/dst set, read the device's ACTUAL render states back "
                    "for that config instead of hiding it -- tells apart \"we never issued the "
                    "blend\" from \"we issued it and it did not take\"");

REXCVAR_DEFINE_BOOL(nx1_d3d9_dbg_blend_log, false, "GPU",
                    "Log every distinct blend configuration the guest asks for, once each "
                    "(glass-transparency hunt)");

REXCVAR_DEFINE_BOOL(nx1_d3d9_skip_clean_constants, true, "GPU",
                    "Skip the shader constant upload when the guest's dirty mask, the PM4 "
                    "ring generation and the bound shader all say nothing changed");

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
  // Our own ImGui context on this device. The SDK's overlay renders through a D3D12
  // ImmediateDrawer into a swapchain our PresentEx covers, so it can never be seen.
  Overlay::Get().Initialize(device_, hwnd);
  backbuffer_width_ = pp.BackBufferWidth;
  backbuffer_height_ = pp.BackBufferHeight;
  current_rt_width_ = pp.BackBufferWidth;
  current_rt_height_ = pp.BackBufferHeight;
  REXGPU_INFO("nx1_d3d9: device created ({}x{})", pp.BackBufferWidth, pp.BackBufferHeight);

  // The translation worker. Started unconditionally so nx1_d3d9_async can be toggled at runtime
  // without a device reset; it parks on the condition variable and costs nothing while async is
  // off, because BeginFrame only latches worker_active_ when the cvar says so.
  worker_running_ = true;
  worker_thread_ = std::thread([this] { WorkerMain(); });

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
  InvalidateStateShadow();

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
  // Join the worker BEFORE taking render_mutex_ and before anything is released: it executes
  // D3D calls and touches ResourceTracker, so tearing those down underneath it is the same
  // cross-thread use-after-free this function already guards against for the ring thread. Done
  // outside the lock because the worker does not take render_mutex_ and waiting on it while
  // holding one would be a deadlock waiting to happen.
  if (worker_thread_.joinable()) {
    {
      std::lock_guard<std::mutex> lk(queue_mutex_);
      worker_running_ = false;
    }
    queue_cv_.notify_all();
    worker_thread_.join();
  }
  {
    std::lock_guard<std::mutex> lock(render_mutex_);
    // Before the device goes: imgui holds device objects, and the WndProc subclass must be
    // unhooked while the window is still ours to unhook from.
    Overlay::Get().Shutdown();
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
  // THE synchronisation point. Everything below -- the display blit, the overlay, PresentEx --
  // runs on the guest thread with the worker idle, which is why none of them needed queueing.
  DrainWorker();
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

  if (REXCVAR_GET(nx1_d3d9_profile)) {
    const auto tl_now = std::chrono::steady_clock::now();
    if (prof_saw_draw_ && prof_last_draw_end_.time_since_epoch().count()) {
      prof_gap_after_last_ns_ += uint64_t((tl_now - prof_last_draw_end_).count());
    }
    auto& tracker_probe = ResourceTracker::Get();
    for (const auto& probe : prof_stability_) {
      const uint8_t* pp = probe.kind == ProbeKind::kUcode
                              ? GuestTranslateGpuPhysical(probe.addr)
                              : tracker_probe.PhysicalPointer(probe.addr);
      if (!pp) {
        continue;
      }
      const bool same = XXH3_64bits(pp, probe.bytes) == probe.hash;
      const uint32_t k = uint32_t(probe.kind);
      (same ? prof_stable_ok_ : prof_stable_changed_) += 1;
      (same ? prof_stable_ok_kind_[k] : prof_stable_changed_kind_[k]) += 1;
    }
    // The PREVIOUS frame's probes, re-checked a full frame late. This is the question frame-level
    // pipelining actually asks -- would data still be valid if the worker were consuming frame N
    // while the guest records N+1 -- and it is NOT what the within-frame number above answers.
    for (const auto& probe : prof_stability_prev_) {
      const uint8_t* pp = probe.kind == ProbeKind::kUcode
                              ? GuestTranslateGpuPhysical(probe.addr)
                              : tracker_probe.PhysicalPointer(probe.addr);
      if (!pp) {
        continue;
      }
      const uint32_t k = uint32_t(probe.kind);
      (XXH3_64bits(pp, probe.bytes) == probe.hash ? prof_xframe_ok_[k]
                                                  : prof_xframe_changed_[k]) += 1;
    }
    prof_stability_prev_ = prof_stability_;
    prof_stability_.clear();
    for (uint32_t k = 0; k < 3; ++k) {
      prof_probe_offered_[k] += prof_probe_seen_[k];
      prof_probe_seen_[k] = prof_probe_taken_[k] = 0;
    }
    prof_saw_draw_ = false;
    prof_frame_start_ = tl_now;
  }

  static auto last_present = std::chrono::steady_clock::now();
  const auto t_present = std::chrono::steady_clock::now();
  const uint64_t this_frame_ns = uint64_t((t_present - last_present).count());
  prof_frame_ns_ += this_frame_ns;
  // WORST frame in the window, not just the mean. A hitch is invisible in an average -- one
  // 80 ms frame among sixty 16 ms ones moves the mean by a millisecond -- and "ran well, some
  // hitches" is a report about the tail, not the middle.
  if (this_frame_ns > prof_frame_max_ns_) {
    prof_frame_max_ns_ = this_frame_ns;
    prof_frame_max_drain_ns_ = prof_drain_wait_frame_ns_;
    prof_frame_max_record_ns_ = prof_record_frame_ns_;
  }
  prof_drain_wait_frame_ns_ = 0;
  prof_record_frame_ns_ = 0;
  last_present = t_present;
  // Last thing before the flip, so it sits on top of the finished frame.
  Overlay::Get().Render();
  device_->PresentEx(nullptr, nullptr, nullptr, nullptr, 0);
  prof_present_ns_ += uint64_t((std::chrono::steady_clock::now() - t_present).count());

  // TEMP PROFILING: report the frame's phase breakdown. Present is timed too -- if the frame
  // is GPU-bound (or vsync-blocked) the cost lands there, not in our CPU-side setup, and that
  // distinction decides whether to optimise our per-draw work or the GPU workload itself.
  // Start every frame with a real bind, so a skip chain always descends from a draw that
  // refreshed the cache entries' last_frame within this frame.
  last_sig_valid_ = false;
  if (REXCVAR_GET(nx1_d3d9_record) && REXCVAR_GET(nx1_d3d9_profile) && !cmdbuf_.draws().empty()) {
    REXGPU_INFO("nx1_d3d9: PROF/record {} draws, {} constant deltas, {:.2f} MB, {:.2f} ms",
                cmdbuf_.draws().size(), cmdbuf_.delta_count(),
                double(cmdbuf_.captured_bytes()) / (1024.0 * 1024.0), prof_record_ns_ / 1e6);
  }
  prof_record_ns_ = 0;
  // UNCONDITIONAL. Every draw now executes from its record, so the buffer fills whatever the
  // cvar says; leaving the reset behind nx1_d3d9_record would grow it without bound for the
  // whole session. The cvar gates the report and nothing else.
  //
  // Drain again before clearing. Present normally does it, but Present early-returns when the
  // device is gone or shutdown has begun -- and clearing the buffer while the worker still holds
  // a reference into it is a use-after-free. Cheap: after a normal Present the queue is already
  // empty and this returns immediately.
  DrainWorker();
  cmdbuf_.BeginFrame();
  // The shader memo is valid for ONE frame. Microcode is measured stable within a frame, not
  // across frames, and a shader object the guest repurposes between frames must not be served
  // its old translation. 512 slots is a cheap wipe against ~5000 draws of savings.
  std::memset(shader_memo_, 0, sizeof(shader_memo_));
  queue_head_ = queue_tail_ = 0;
  // Latched once per frame rather than read per command, so the mode cannot change underneath a
  // half-submitted frame -- which would leave commands queued that nothing ever drains.
  worker_active_ = REXCVAR_GET(nx1_d3d9_async) && worker_running_;

  const bool prof_on = REXCVAR_GET(nx1_d3d9_profile);
  ResourceTracker::Get().prof_enabled_ = prof_on;
  ShaderCache::Get().prof_enabled_ = prof_on;
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
      // How often the clean-constant skip actually fires. The dirty mask says ~94%/78% of
      // draws write no constants, so a much lower skip rate means another condition is
      // blocking it -- most likely the ring generation, which bumps on every
      // GpuBeginShaderConstantF4 (i.e. every model's world matrix).
      REXGPU_INFO("nx1_d3d9: PROF/const skipped vs={:.0f}% ps={:.0f}% of {:.0f} draws/frame",
                  100.0 * prof_const_skipped_vs_ / (prof_draws_ ? prof_draws_ : 1),
                  100.0 * prof_const_skipped_ps_ / (prof_draws_ ? prof_draws_ : 1),
                  double(prof_draws_) / prof_frames);
      prof_const_skipped_vs_ = prof_const_skipped_ps_ = 0;

      // What fraction of draws leave each guest dirty mask untouched, and what fraction
      // continue the previous draw's index range. These are the two ceilings: skipping
      // unchanged state, and merging adjacent draws.
      REXGPU_INFO("nx1_d3d9: PROF/dirty clear%: m0={:.0f} m1={:.0f} m2={:.0f} m3={:.0f} m4={:.0f} "
                  "| unchanged%: m0={:.0f} m1={:.0f} m2={:.0f} m3={:.0f} m4={:.0f} "
                  "| contiguous draws {:.0f}%",
                  100.0 * prof_mask_clear_[0] / (prof_draws_ ? prof_draws_ : 1),
                  100.0 * prof_mask_clear_[1] / (prof_draws_ ? prof_draws_ : 1),
                  100.0 * prof_mask_clear_[2] / (prof_draws_ ? prof_draws_ : 1),
                  100.0 * prof_mask_clear_[3] / (prof_draws_ ? prof_draws_ : 1),
                  100.0 * prof_mask_clear_[4] / (prof_draws_ ? prof_draws_ : 1),
                  100.0 * prof_mask_same_[0] / (prof_draws_ ? prof_draws_ : 1),
                  100.0 * prof_mask_same_[1] / (prof_draws_ ? prof_draws_ : 1),
                  100.0 * prof_mask_same_[2] / (prof_draws_ ? prof_draws_ : 1),
                  100.0 * prof_mask_same_[3] / (prof_draws_ ? prof_draws_ : 1),
                  100.0 * prof_mask_same_[4] / (prof_draws_ ? prof_draws_ : 1),
                  100.0 * prof_contiguous_draws_ / (prof_draws_ ? prof_draws_ : 1));
      for (uint32_t i = 0; i < 5; ++i) {
        prof_mask_clear_[i] = 0;
        prof_mask_same_[i] = 0;
      }
      prof_contiguous_draws_ = 0;
      REXGPU_INFO("nx1_d3d9: PROF/shd outer hash={:.2f} lookup={:.2f} bind+upload={:.2f} ms/frame",
                  prof_hash_ns_ * tf, prof_lookup_ns_ * tf, prof_shbind_ns_ * tf);
      prof_hash_ns_ = prof_lookup_ns_ = prof_shbind_ns_ = 0;
      const auto sp = ShaderCache::Get().TakeShaderProfile();
      REXGPU_INFO("nx1_d3d9: PROF/shd literals={:.2f} read={:.2f} window={:.2f} defs={:.2f} ms/frame "
                  "| {:.0f} uploads/frame, {:.1f} regs + {:.1f} defs each",
                  sp.literals_ns * tf, sp.read_ns * tf, sp.window_ns * tf, sp.defs_ns * tf,
                  double(sp.calls) / prof_frames,
                  sp.calls ? double(sp.registers) / double(sp.calls) : 0.0,
                  sp.calls ? double(sp.def_writes) / double(sp.calls) : 0.0);
      const auto vp = ResourceTracker::Get().TakeVertexProfile();
      REXGPU_INFO("nx1_d3d9: PROF/vb layout={:.2f} buffer={:.2f} ms/frame | inside buffer: "
                  "fast={:.2f} hash={:.2f} convert={:.2f} ms over {:.0f} calls, "
                  "{:.1f} hashes ({:.2f} MB) {:.1f} converts ({:.2f} MB) per frame",
                  prof_vlayout_ns_ * tf, prof_vbuffer_ns_ * tf,
                  vp.fast_ns * tf, vp.hash_ns * tf, vp.convert_ns * tf,
                  double(vp.calls) / prof_frames,
                  double(vp.hashes) / prof_frames,
                  double(vp.hash_bytes) / (1024.0 * 1024.0 * prof_frames),
                  double(vp.converts) / prof_frames,
                  double(vp.convert_bytes) / (1024.0 * 1024.0 * prof_frames));
      REXGPU_INFO("nx1_d3d9: PROF/vb dynamic converts {:.1f} of {:.1f} per frame",
                  double(vp.dynamic_converts) / prof_frames,
                  double(vp.converts) / prof_frames);
      prof_vlayout_ns_ = prof_vbuffer_ns_ = 0;
      REXGPU_INFO("nx1_d3d9: PROF/vtx decl skipped {:.1f}% of {:.0f} | stream skipped {:.1f}% of {:.0f} per frame",
                  prof_decl_calls_ ? 100.0 * double(prof_decl_skips_) / double(prof_decl_calls_) : 0.0,
                  double(prof_decl_calls_) / prof_frames,
                  prof_stream_calls_ ? 100.0 * double(prof_stream_skips_) / double(prof_stream_calls_) : 0.0,
                  double(prof_stream_calls_) / prof_frames);
      prof_decl_skips_ = prof_decl_calls_ = prof_stream_skips_ = prof_stream_calls_ = 0;
      const auto lp = ResourceTracker::Get().TakeLodProfile();
      REXGPU_INFO("nx1_d3d9: PROF/lod calls={:.0f} no_surface={:.0f} fresh={:.1f} adopt={:.1f} "
                  "equal={:.1f} (same={:.1f} diff={:.1f}) substitute={:.1f} per frame",
                  double(lp.calls) / prof_frames, double(lp.no_surface) / prof_frames,
                  double(lp.fresh) / prof_frames, double(lp.adopt) / prof_frames,
                  double(lp.equal) / prof_frames, double(lp.equal_same) / prof_frames,
                  double(lp.equal_diff) / prof_frames, double(lp.substitute) / prof_frames);
      if (prof_blend_match_draws_) {
        REXGPU_INFO("nx1_d3d9: PROF/blendmatch {:.1f} draws/frame match the isolate filter",
                    double(prof_blend_match_draws_) / prof_frames);
        prof_blend_match_draws_ = 0;
      }
      // Report the hit rate, not just the timing. Several shadows in this renderer measured
      // exactly zero benefit and were only caught by a counter -- a memo that never hits is pure
      // added cost, and the phase timing alone cannot tell the difference.
      {
        const uint64_t total = prof_shader_memo_hits_ + prof_shader_memo_misses_;
        REXGPU_INFO("nx1_d3d9: PROF/shmemo {:.1f}% hit of {:.0f} resolves/frame",
                    total ? 100.0 * double(prof_shader_memo_hits_) / double(total) : 0.0,
                    double(total) / prof_frames);
        prof_shader_memo_hits_ = prof_shader_memo_misses_ = 0;
      }
      if (worker_active_) {
        // Names the limiting side outright. Guest-wait high => the worker is the bottleneck and
        // more translation work should come off it. Worker-idle high => the guest thread is,
        // i.e. its own game logic plus our recording tax, and shaving translation buys nothing.
        const double drain_ms = prof_drain_wait_ns_ * tf;
        const double idle_ms = prof_worker_idle_ns_.exchange(0, std::memory_order_relaxed) * tf;
        // NOT tf. tf is 1/(1e6 * prof_frames) -- it turns an ACCUMULATED total into a per-frame
        // average, and dividing a MAXIMUM by the frame count made every hitch read ~60x too
        // small (a 276 ms stall printed as 4.6 ms). These two are per-frame peaks already, so
        // they need a plain ns -> ms conversion.
        static constexpr double kNsToMs = 1.0 / 1e6;
        REXGPU_INFO("nx1_d3d9: PROF/hitch worst frame {:.1f} ms ({:.1f} ms of it blocked on the "
                    "worker, {:.1f} ms recording)",
                    prof_frame_max_ns_ * kNsToMs, prof_frame_max_drain_ns_ * kNsToMs,
                    prof_frame_max_record_ns_ * kNsToMs);
        prof_frame_max_ns_ = prof_frame_max_drain_ns_ = prof_frame_max_record_ns_ = 0;
        REXGPU_INFO("nx1_d3d9: PROF/bound guest waited {:.2f} ms/frame for the worker, worker "
                    "starved {:.2f} ms/frame -- limited by {}",
                    drain_ms, idle_ms,
                    drain_ms > idle_ms * 2.0   ? "TRANSLATION (worker)"
                    : idle_ms > drain_ms * 2.0 ? "the GUEST THREAD (game logic + recording)"
                                               : "both roughly equally");
        prof_drain_wait_ns_ = 0;
      }
      // Only meaningful synchronously. The gap timers difference a mark taken on the guest
      // thread against one taken in ExecuteDraw, which under async runs on the WORKER -- the
      // subtraction then measures the distance between two unrelated clocks and reported
      // between-draws=886 ms/frame. An instrument that silently becomes nonsense when its
      // assumption breaks is worse than no instrument, so it stays quiet instead.
      if (!worker_active_) {
        REXGPU_INFO("nx1_d3d9: PROF/defer guest work: pre-draw={:.2f} between-draws={:.2f} "
                    "post-draw={:.2f} ms/frame",
                    prof_gap_before_first_ns_ * tf, prof_gap_between_ns_ * tf,
                    prof_gap_after_last_ns_ * tf);
      }
      // Reported PER CLASS. A combined number is dominated by the vertex probes and would let a
      // genuinely unstable index or microcode range hide inside a 99.9% aggregate -- which is
      // exactly the assumption the executor is resting on.
      static const char* const kKindName[3] = {"vertex", "index", "ucode"};
      for (uint32_t k = 0; k < 3; ++k) {
        const uint64_t total = prof_stable_ok_kind_[k] + prof_stable_changed_kind_[k];
        const uint64_t xtotal = prof_xframe_ok_[k] + prof_xframe_changed_[k];
        REXGPU_INFO("nx1_d3d9: PROF/stability {:<6} in-frame {:.2f}% unchanged ({} of {}) | "
                    "CROSS-FRAME {:.2f}% unchanged ({} changed of {})",
                    kKindName[k],
                    total ? 100.0 * double(prof_stable_ok_kind_[k]) / double(total) : 0.0,
                    prof_stable_changed_kind_[k], total,
                    xtotal ? 100.0 * double(prof_xframe_ok_[k]) / double(xtotal) : 0.0,
                    prof_xframe_changed_[k], xtotal);
        prof_stable_ok_kind_[k] = prof_stable_changed_kind_[k] = 0;
        prof_xframe_ok_[k] = prof_xframe_changed_[k] = 0;
        prof_probe_offered_[k] = 0;
      }
      prof_gap_before_first_ns_ = prof_gap_between_ns_ = prof_gap_after_last_ns_ = 0;
      prof_stable_ok_ = prof_stable_changed_ = 0;
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
  // Record, then execute. The guest hands the clear colour as a __vector4 of floats and an
  // optional _D3DRECT, both typically on the caller's stack -- so they are resolved to VALUES
  // here rather than deferred as addresses. What is deliberately NOT resolved here is the Xenos
  // -> D3DCLEAR_* mapping: it depends on whether a depth-stencil is bound, and an earlier
  // command in this same list may have changed that. See ExecuteClear.
  RecordedCommand& c = cmdbuf_.AddCommand(CommandKind::kClear);
  c.clear_flags = flags;
  c.clear_z = z;
  c.clear_stencil = stencil;
  c.has_rect = rect_addr != 0;
  if (rect_addr) {
    for (uint32_t i = 0; i < 4; ++i) {
      c.rect[i] = int32_t(GuestRead32(base, rect_addr + i * 4));
    }
  }
  if (color_addr) {
    for (uint32_t i = 0; i < 4; ++i) {
      c.clear_color[i] = GuestReadF32(base, color_addr + i * 4);
    }
  }
  c.has_color = color_addr != 0;
  SubmitCommand(base, 0, uint32_t(cmdbuf_.commands().size() - 1));
}

void Renderer::ExecuteClear(const RecordedCommand& c) {
  // Xenos clear flags, not desktop ones: bits 0-3 select colour targets 0-3, 0x10 is
  // depth, 0x20 is stencil. We bind a single colour target, so any of the four means
  // "clear it". Passing the guest's bits to D3D9 unmasked would fail the whole call.
  const uint32_t flags = c.clear_flags;
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

  // The recorded colour is the guest's __vector4 (r,g,b,a) and the rect its _D3DRECT
  // ({x1,y1,x2,y2}, the same layout D3D9 uses). Both the guest and D3D9 clip the clear
  // to the current viewport, so leave the viewport alone -- it is already the guest's.
  D3DCOLOR color = 0;
  if ((host_flags & D3DCLEAR_TARGET) && c.has_color) {
    auto to8 = [](float v) { return uint32_t(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
    color = D3DCOLOR_ARGB(to8(c.clear_color[3]), to8(c.clear_color[0]), to8(c.clear_color[1]),
                          to8(c.clear_color[2]));
  }
  const D3DRECT rect = {c.rect[0], c.rect[1], c.rect[2], c.rect[3]};
  const DWORD rect_count = c.has_rect ? 1u : 0u;
  const D3DRECT* rect_ptr = c.has_rect ? &rect : nullptr;

  // z is the guest's clear value verbatim -- under NX1's reverse-Z that is 0.0 for
  // the far plane, which is exactly what the D3DCMP_GREATEREQUAL depth test wants.
  if (FAILED(device_->Clear(rect_count, rect_ptr, host_flags, color, c.clear_z, c.clear_stencil)) &&
      (host_flags & D3DCLEAR_STENCIL)) {
    // A depth surface without stencil rejects D3DCLEAR_STENCIL, and D3D9 then fails the
    // whole call -- clearing nothing. Drop back rather than lose the depth clear.
    device_->Clear(rect_count, rect_ptr, host_flags & ~D3DCLEAR_STENCIL, color, c.clear_z, 0);
  }
}

void Renderer::SetRenderTarget(const uint8_t* base, uint32_t index, uint32_t guest_surface) {
  std::lock_guard<std::mutex> lock(render_mutex_);
  if (shutting_down_.load(std::memory_order_acquire) || !device_ || index >= 4) {
    return;
  }
  RecordedCommand& c = cmdbuf_.AddCommand(CommandKind::kSetRenderTarget);
  c.rt_index = index;
  c.surface = guest_surface;
  SubmitCommand(base, 0, uint32_t(cmdbuf_.commands().size() - 1));
}

void Renderer::ExecuteSetRenderTarget(const uint8_t* base, const RecordedCommand& c) {
  const uint32_t index = c.rt_index;
  const uint32_t guest_surface = c.surface;
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
      SetRenderStateCached(D3DRS_SRGBWRITEENABLE, color_fmt == 1 ? TRUE : FALSE);
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
  RecordedCommand& c = cmdbuf_.AddCommand(CommandKind::kSetDepthStencil);
  c.surface = guest_surface;
  SubmitCommand(base, 0, uint32_t(cmdbuf_.commands().size() - 1));
}

void Renderer::ExecuteSetDepthStencil(const uint8_t* base, const RecordedCommand& c) {
  // Ordering-critical: GetDepthSurface sizes against current_rt_width_/height_, which the
  // kSetRenderTarget command immediately before this one sets. The guest always binds its target
  // then its depth, and honouring that order is exactly why these share one command list.
  const uint32_t guest_surface = c.surface;
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
  // Resolve the guest's stack-borne arguments to VALUES here -- the src rect, dest point and
  // clear colour are all pointers into whatever the caller pushed, so a worker reading them late
  // would read something else entirely. The destination TEXTURE stays an object address: that is
  // a persistent allocation, the read-it-late class.
  RecordedCommand& c = cmdbuf_.AddCommand(CommandKind::kResolve);
  c.dest_texture = dest_texture;
  c.clear_flags = flags;
  c.clear_z = clear_z;
  c.clear_stencil = clear_stencil;
  c.has_rect = src_rect != 0;
  if (src_rect) {
    for (uint32_t i = 0; i < 4; ++i) {
      c.rect[i] = int32_t(GuestRead32(base, src_rect + i * 4));
    }
  }
  c.has_dest_point = dest_point != 0;
  if (dest_point) {
    c.dest_point[0] = int32_t(GuestRead32(base, dest_point + 0));
    c.dest_point[1] = int32_t(GuestRead32(base, dest_point + 4));
  }
  c.has_color = clear_color != 0;
  if (clear_color) {
    for (uint32_t i = 0; i < 4; ++i) {
      c.clear_color[i] = GuestReadF32(base, clear_color + i * 4);
    }
  }
  SubmitCommand(base, 0, uint32_t(cmdbuf_.commands().size() - 1));
}

void Renderer::ExecuteResolve(const uint8_t* base, const RecordedCommand& c) {
  ResolveCopy(base, c);
  // A depth resolve blits through the pipeline, binding a texture and sampler state on
  // sampler 0 and leaving it unbound afterwards. BindTextures' shadow cannot see that, so
  // tell it to stop trusting itself. Resolves are a handful per frame; re-issuing the
  // sampler state on the next draw costs nothing next to getting it wrong.
  InvalidateStateShadow();
  ClearEdram(c);
}

void Renderer::ResolveCopy(const uint8_t* base, const RecordedCommand& c) {
  const uint32_t dest_texture = c.dest_texture;

  if (!dest_texture) {
    return;
  }
  const TextureFetchConstant dest = ReadBaseTextureFormat(base, dest_texture);
  // Log every distinct resolve destination once. If a texture that renders wrong never
  // appears here, the guest is not resolving into it and it is produced some other way; if it
  // DOES appear, the destination is reaching us and the fault is downstream in how we register
  // or key it.
  {
    static std::mutex m;
    static std::vector<uint32_t> seen;
    std::lock_guard<std::mutex> lk(m);
    if (std::find(seen.begin(), seen.end(), dest.base_address) == seen.end() &&
        seen.size() < 64) {
      seen.push_back(dest.base_address);
      REXGPU_INFO("nx1_d3d9: RESOLVEDST {:08X} {}x{} fmt={} tiled={} (dest_texture {:08X})",
                  dest.base_address, dest.width, dest.height, dest.format, dest.tiled ? 1 : 0,
                  dest_texture);
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
  if (c.has_rect) {
    rect.left = c.rect[0];
    rect.top = c.rect[1];
    rect.right = c.rect[2];
    rect.bottom = c.rect[3];
  }
  // Where this tile lands in the destination. D3DPOINT is {x, y}.
  POINT at{0, 0};
  if (c.has_dest_point) {
    at.x = c.dest_point[0];
    at.y = c.dest_point[1];
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

void Renderer::ClearEdram(const RecordedCommand& c) {
  const uint32_t flags = c.clear_flags;

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
  if ((host_flags & D3DCLEAR_TARGET) && c.has_color) {
    auto to8 = [](float v) { return uint32_t(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
    color = D3DCOLOR_ARGB(to8(c.clear_color[3]), to8(c.clear_color[0]), to8(c.clear_color[1]),
                          to8(c.clear_color[2]));
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
  if (FAILED(device_->Clear(0, nullptr, host_flags, color, c.clear_z, c.clear_stencil)) &&
      (host_flags & D3DCLEAR_STENCIL)) {
    device_->Clear(0, nullptr, host_flags & ~D3DCLEAR_STENCIL, color, c.clear_z, 0);
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
  // Skip when the same shader is still bound and the viewport has not moved. Sound because
  // these registers sit ABOVE this shader's constant window (base_reg == count, window is
  // [0, count)), so the shader's own uploads never reach them -- and a DIFFERENT shader's
  // window covering them is exactly what the identity check rules out. ndc_scale_/ndc_offset_
  // only move when the viewport changes, so in practice this elides two D3D calls per draw.
  if (&shader == last_vs_uniform_shader_ &&
      std::memcmp(last_vs_uniform_params_, params, sizeof(params)) == 0) {
    return;
  }
  if (!shader.IsDefRegister(base_reg)) {
    device_->SetVertexShaderConstantF(base_reg, params, 1);
  }
  if (!shader.IsDefRegister(base_reg + 1)) {
    device_->SetVertexShaderConstantF(base_reg + 1, &params[4], 1);
  }
  last_vs_uniform_shader_ = &shader;
  std::memcpy(last_vs_uniform_params_, params, sizeof(params));
}

void Renderer::ResolveViewport(const RecordedDraw& d) {
  // Replicates rex::graphics::GetHostViewportInfo for a real D3D9 host: produce
  // an integer host viewport plus the guest-clip -> host-clip NDC fold that the
  // translated vertex shaders apply. We cannot call GetHostViewportInfo directly
  // -- it reads a populated RegisterFile, which only exists when the (now
  // disabled) PM4 backend runs -- so it is reproduced here from the shadow regs.
  const ViewportState& v = d.viewport;
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
                                   bool needs_inv_tex_dim, const RecordedDraw& d) {
  // ps_3_0 only exposes 224 float registers.
  if (base_reg + 2 > 224) {
    return;
  }
  const AlphaTestState& alpha = d.alpha;

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
  // Same reasoning as the vertex side: above the window, so only another shader can disturb
  // these, and same-shader means none did.
  const bool same_ps = &shader == last_ps_uniform_shader_;
  if (!same_ps || last_ps_alpha_[0] != threshold[0] || last_ps_alpha_[1] != compare[0]) {
    if (!shader.IsDefRegister(base_reg)) {
      device_->SetPixelShaderConstantF(base_reg, threshold, 1);
    }
    if (!shader.IsDefRegister(base_reg + 1)) {
      device_->SetPixelShaderConstantF(base_reg + 1, compare, 1);
    }
    last_ps_alpha_[0] = threshold[0];
    last_ps_alpha_[1] = compare[0];
  }
  last_ps_uniform_shader_ = &shader;

  // g_InvTexDim feeds offset tfetches: `tex2D(s, uv + offset * g_InvTexDim[i].xy)`, and the
  // translator only *declares* it for shaders that have one. Writing it regardless walks over 16
  // registers the shader never reserved -- which is where fxc parked its `def` literals, and D3D9
  // keeps those in the same constant file (a Set*ShaderConstantF after SetPixelShader wins).
  // Clobbering `def cN, 0, 1, -1, -0` -- the 0/1 source of every flattened `cmp` -- kills every
  // predicated block in the shader. So upload it only for shaders that actually read it.
  if (!needs_inv_tex_dim || base_reg + 18 > 224) {
    return;
  }
  // Only the slots this shader declares. g_InvTexDim[i] is read by an offset tfetch on
  // sampler i, so a slot the shader never dcl's is a register it can never read -- and
  // resolving it cost a full fetch-constant decode plus a single-register D3D call, 16 of
  // each per draw where ~4 were meaningful. The leftover registers keep another shader's
  // values, which is unobservable for exactly the same reason.
  const uint32_t mask = active_sampler_mask_;
  float dims[16 * 4] = {};
  for (uint32_t s = 0; s < 16; ++s) {
    if (!(mask & (1u << s))) {
      continue;
    }
    const TextureFetchConstant t = DecodeTextureFetchConstant(d.texture_fetch(s));
    if (t.valid && t.width && t.height) {
      dims[s * 4 + 0] = 1.0f / float(t.width);
      dims[s * 4 + 1] = 1.0f / float(t.height);
    }
  }
  // The dimensions only change when a bound texture does, so compare before issuing: a
  // 256-byte memcmp against up to sixteen single-register D3D calls.
  if (same_ps && mask == last_ps_dims_mask_ &&
      std::memcmp(last_ps_dims_, dims, sizeof(dims)) == 0) {
    return;
  }
  for (uint32_t s = 0; s < 16; ++s) {
    if ((mask & (1u << s)) && !shader.IsDefRegister(base_reg + 2 + s)) {
      device_->SetPixelShaderConstantF(base_reg + 2 + s, &dims[s * 4], 1);
    }
  }
  last_ps_dims_mask_ = mask;
  std::memcpy(last_ps_dims_, dims, sizeof(dims));
}

void Renderer::ResolveShadersAndConstants(const uint8_t* base, uint32_t guest_device,
                                          RecordedDraw& d) {
  auto& cache = ShaderCache::Get();

  const uint32_t vs_object = d.vs_object;
  const uint32_t ps_object = d.ps_object;
  if (!vs_object) {
    return;
  }

  // A pixel shader is optional: the depth pre-pass binds none, and that is exactly
  // when a vertex shader runs its second (predicated-Z) pass. Hashing pass 0 there
  // would look up microcode the GPU never executes.
  // TEMP PROFILING: hashing the microcode vs looking the result up. HashGuestUcode XXH3s the
  // whole shader twice per draw (~10k times a frame), which is the suspect for the ~2 ms of the
  // shaders phase that UploadConstants does not account for.
  const bool sprof = REXCVAR_GET(nx1_d3d9_profile);
  const auto smark = [sprof] {
    return sprof ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
  };
  const auto sadd = [sprof](uint64_t& sink, std::chrono::steady_clock::time_point t0) {
    if (sprof) sink += uint64_t((std::chrono::steady_clock::now() - t0).count());
  };

  const uint32_t vs_pass = d.vs_pass;

  // Memoized resolve: (object, pass, stage) -> translated shader, valid for this frame only.
  // Skips BOTH the microcode hash and the map lookup, which is the bulk of what recording costs
  // on the guest thread. A miss is cached too -- a shader with no SM3 translation would
  // otherwise re-hash its whole microcode on every draw that wants it, which is the worst case
  // rather than the rare one.
  const auto resolve = [&](uint32_t object, uint32_t pass, bool pixel_stage,
                           const GuestUcode* ucode_out) -> const Sm3Shader* {
    const uint64_t key = (uint64_t(object) << 8) ^ (uint64_t(pass) << 1) ^ (pixel_stage ? 1u : 0u) ^
                         0x9E3779B97F4A7C15ull;
    ShaderMemo& slot = shader_memo_[(key * 0xD1B54A32D192ED03ull) >> 52];
    if (slot.key == key && slot.resolved) {
      ++prof_shader_memo_hits_;
      return slot.shader;
    }
    const auto t_hash = smark();
    const GuestUcode ucode = ReadGuestUcode(base, object, pixel_stage, pass);
    if (ucode_out && REXCVAR_GET(nx1_d3d9_profile) && ucode.valid()) {
      // Microcode is read late by the worker on the argument that shader objects live for the
      // frame -- the same argument this memo rests on, so keep probing it.
      ProbeStability(ProbeKind::kUcode, ucode.physical_address, ucode.dword_count * 4);
    }
    const uint64_t hash = HashGuestUcode(ucode);
    sadd(prof_hash_ns_, t_hash);
    const auto t_look = smark();
    const Sm3Shader* resolved = hash ? cache.Lookup(hash) : nullptr;
    sadd(prof_lookup_ns_, t_look);
    slot = {key, resolved, true};
    ++prof_shader_memo_misses_;
    return resolved;
  };

  const GuestUcode probe_marker{};
  const Sm3Shader* vs = resolve(vs_object, vs_pass, /*pixel_stage=*/false, &probe_marker);
  const Sm3Shader* ps = ps_object ? resolve(ps_object, 0, /*pixel_stage=*/true, nullptr) : nullptr;

  // Name the microcode behind the isolated material. tools/new_shader_dump holds the readable
  // disassembly as shader_<HASH>.ucode.frag, so the hash is what turns "this material renders
  // wrong" into a shader you can actually read. One-shot per object, off unless the isolate is set.
  // Two modes. With dbg_blend_ps set, name that one material. With dbg_shaderid_n set, name the
  // first N DISTINCT materials seen -- which is what you want when the ps_object from a previous
  // session no longer matches anything, since shader objects move between runs. Cross-reference
  // the resulting list against BLENDPS by ps value; no bisection needed.
  const uint32_t dbg_ps = REXCVAR_GET(nx1_d3d9_dbg_blend_ps);
  const uint32_t shaderid_n = REXCVAR_GET(nx1_d3d9_dbg_shaderid_n);
  if (ps_object && ((dbg_ps && ps_object == dbg_ps) || shaderid_n)) {
    static std::mutex sid_m;
    static std::vector<uint32_t> sid_seen;
    std::lock_guard<std::mutex> sid_lk(sid_m);
    const bool fresh =
        std::find(sid_seen.begin(), sid_seen.end(), ps_object) == sid_seen.end();
    if (fresh && sid_seen.size() < (dbg_ps ? 1u : shaderid_n)) {
      sid_seen.push_back(ps_object);
      const GuestUcode pu = ReadGuestUcode(base, ps_object, /*pixel_stage=*/true, 0);
      const GuestUcode vu = ReadGuestUcode(base, vs_object, /*pixel_stage=*/false, vs_pass);
      REXGPU_INFO("nx1_d3d9: SHADERID ps={:08X} ucode_hash={:016X} dwords={} sm3={} | "
                  "vs={:08X} ucode_hash={:016X} dwords={} sm3={}",
                  ps_object, HashGuestUcode(pu), pu.dword_count, ps ? 1 : 0, vs_object,
                  HashGuestUcode(vu), vu.dword_count, vs ? 1 : 0);
    }
  }
  // Only used by the cache-miss diagnostic below, and only on the slow path.
  const uint64_t vs_hash = vs ? 1 : 0;
  const uint64_t ps_hash = ps ? 1 : 0;

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
    return;
  }
  d.vs = vs;
  d.ps = ps;

  // Skip the upload when the guest says nothing changed. Measured: 94% of draws write no
  // vertex constants and 78% write no pixel constants, and UploadConstants re-reads and
  // re-uploads ~10 registers each time regardless.
  //
  // Three conditions, all necessary. The dirty mask covers writes to the SHADOW. The ring
  // generation covers GpuBeginShaderConstantF4 writes, which go to the PM4 ring and leave the
  // mask clear -- that is the object->world matrix of every model draw, so missing it would
  // weld the world into one pose. And the shader must match, because a different shader reads
  // a different compacted register window even when the constants themselves are untouched.
  const uint64_t ring_gen = ConstantRing::For(guest_device).generation();
  const bool skip_vs_constants = REXCVAR_GET(nx1_d3d9_skip_clean_constants) &&
                                 guest_dirty_vs_ == 0 && ring_gen == last_const_ring_gen_ &&
                                 vs == last_const_vs_;
  // A skipped stage records count 0, which is exactly what the executor needs: the values are
  // already sitting in the device's constant file from the draw that did upload them.
  float staging[kMaxHostConstants * 4];
  if (skip_vs_constants) {
    ++prof_const_skipped_vs_;
  } else {
    const uint32_t n = cache.ResolveConstants(base, guest_device, *vs, /*pixel_stage=*/false,
                                              staging);
    d.vs_const_count = n;
    d.vs_const_valid = true;
    d.vs_const_offset = cmdbuf_.AddConstants(staging, n);
    last_const_vs_ = vs;
  }
  if (ps) {
    const bool skip_ps_constants = REXCVAR_GET(nx1_d3d9_skip_clean_constants) &&
                                   guest_dirty_ps_ == 0 && ring_gen == last_const_ring_gen_ &&
                                   ps == last_const_ps_;
    if (skip_ps_constants) {
      ++prof_const_skipped_ps_;
    } else {
      const uint32_t n = cache.ResolveConstants(base, guest_device, *ps, /*pixel_stage=*/true,
                                                staging);
      d.ps_const_count = n;
      d.ps_const_valid = true;
      d.ps_const_offset = cmdbuf_.AddConstants(staging, n);
      last_const_ps_ = ps;
    }
  }
  last_const_ring_gen_ = ring_gen;
}

bool Renderer::BindShadersAndConstants(const RecordedDraw& d) {
  auto& cache = ShaderCache::Get();
  // Resolution already happened at record time and found no usable shader pair (no vertex
  // shader bound, or a cache miss that was reported there). Nothing to draw.
  if (!d.vs) {
    return false;
  }
  const Sm3Shader* const vs = d.vs;
  const Sm3Shader* const ps = d.ps;

  const bool sprof = REXCVAR_GET(nx1_d3d9_profile);
  const auto t_bind = sprof ? std::chrono::steady_clock::now()
                            : std::chrono::steady_clock::time_point{};

  if (vs->vs != bound_vs_) {
    device_->SetVertexShader(vs->vs);
    bound_vs_ = vs->vs;
  }
  if (d.vs_const_valid) {
    cache.ApplyConstants(*vs, /*pixel_stage=*/false, cmdbuf_.constants(d.vs_const_offset),
                         d.vs_const_count);
  }
  if (!NeedsHostNdcTransform(*vs)) {
    UploadVertexUniforms(*vs, HostConstantCount(*vs));
  }

  // Latch which sampler slots these shaders can actually read, so BindTextures skips the rest.
  // A draw with no pixel shader samples nothing at all (depth/shadow-only pass).
  active_sampler_mask_ = 0;
  if (ps) {
    active_sampler_mask_ |= ps->all_samplers ? 0xFFFFu : ps->sampler_mask;
  }
  if (vs->all_samplers) {
    active_sampler_mask_ = 0xFFFFu;  // unwalkable VS: cannot rule out vertex-texture fetch
  }

  IDirect3DPixelShader9* const want_ps = ps ? ps->ps : nullptr;
  if (want_ps != bound_ps_) {
    device_->SetPixelShader(want_ps);
    bound_ps_ = want_ps;
  }
  // A draw with no pixel shader is a depth/shadow-only pass: the Xbox exports no
  // color, so nothing is written to the render target. On D3D9, SetPixelShader(null)
  // instead activates the fixed-function pixel pipeline, which *does* write color
  // (the surface's diffuse/texture as solid fill) and paints garbage over the frame.
  // Mask color writes off for those draws so they contribute depth only.
  //
  // Otherwise honour the guest's own mask: it masks colour off to write destination alpha
  // alone (see ReadColorWriteMask).
  const uint32_t want_color_write = ps ? d.color_write_mask : 0;
  if (want_color_write != bound_color_write_) {
    SetRenderStateCached(D3DRS_COLORWRITEENABLE, want_color_write);
    bound_color_write_ = want_color_write;
  }
  if (ps) {
    if (d.ps_const_valid) {
      cache.ApplyConstants(*ps, /*pixel_stage=*/true, cmdbuf_.constants(d.ps_const_offset),
                           d.ps_const_count);
    }
    UploadPixelUniforms(*ps, HostConstantCount(*ps), ps->needs_inv_tex_dim, d);
  }
  if (sprof) {
    prof_shbind_ns_ += uint64_t((std::chrono::steady_clock::now() - t_bind).count());
  }
  return true;
}

bool Renderer::BindStreams(const uint8_t* base, const RecordedDraw& d,
                           uint32_t needed_vertices, uint32_t* vertex_count) {
  auto& tracker = ResourceTracker::Get();
  // NX1 binds no CVertexDeclaration; for bound-buffer draws derive the multi-stream
  // layout from the vertex shader's vfetch (stream0_stride=0 selects bound mode).
  const bool vprof = REXCVAR_GET(nx1_d3d9_profile);
  const auto vmark = [vprof] {
    return vprof ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
  };
  const auto vadd = [vprof](uint64_t& sink, std::chrono::steady_clock::time_point t0) {
    if (vprof) sink += uint64_t((std::chrono::steady_clock::now() - t0).count());
  };
  const auto t_layout = vmark();
  const VertexLayout* layout = tracker.GetVertexLayout(base, d.vertex_declaration, d.stream_stride);
  if (!layout) {
    layout = tracker.GetShaderVertexLayout(base, d.vs_object, d.vs_pass, /*stream0_stride=*/0,
                                           d.stream_stride);
  }
  vadd(prof_vlayout_ns_, t_layout);
  if (!layout) {
    return false;
  }
  ++prof_decl_calls_;
  if (layout->decl != bound_decl_) {
    device_->SetVertexDeclaration(layout->decl);
    bound_decl_ = layout->decl;
  } else {
    ++prof_decl_skips_;
  }

  *vertex_count = 0;
  for (uint32_t stream = 0; stream < layout->stream_count; ++stream) {
    uint32_t count = 0;
    const auto t_vb = vmark();
    IDirect3DVertexBuffer9* vb =
        tracker.GetVertexBuffer(base, DecodeVertexFetchConstant(d.vertex_fetch(stream)), stream,
                                *layout, needed_vertices, &count);
    if (!vb) {
      return false;
    }
    vadd(prof_vbuffer_ns_, t_vb);
    ++prof_stream_calls_;
    if (vb != bound_stream_vb_[stream] ||
        layout->host_stride[stream] != bound_stream_stride_[stream]) {
      device_->SetStreamSource(stream, vb, 0, layout->host_stride[stream]);
      bound_stream_vb_[stream] = vb;
      bound_stream_stride_[stream] = layout->host_stride[stream];
    } else {
      ++prof_stream_skips_;
    }
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


void Renderer::SetRenderStateCached(D3DRENDERSTATETYPE state, uint32_t value) {
  const uint32_t idx = uint32_t(state);
  if (idx < kMaxRenderState) {
    if (render_state_[idx] == value) {
      return;
    }
    render_state_[idx] = value;
  }
  device_->SetRenderState(state, value);
}

void Renderer::ApplyRenderStates(const RecordedDraw& d) {
  // Depth. NX1 renders reverse-Z, so the guest's stored zfunc is GREATER_EQUAL --
  // we read it rather than hardcoding, so a draw that flips the test still works.
  const DepthState& depth = d.depth;
  SetRenderStateCached(D3DRS_ZENABLE, depth.test_enabled ? D3DZB_TRUE : D3DZB_FALSE);
  SetRenderStateCached(D3DRS_ZWRITEENABLE, depth.write_enabled ? TRUE : FALSE);
  SetRenderStateCached(D3DRS_ZFUNC, HostCompare(depth.compare_function));

  // Blend. Separate color/alpha factors, exactly as Xenos stores them.
  const BlendState& blend = d.blend;
  SetRenderStateCached(D3DRS_ALPHABLENDENABLE, blend.enabled ? TRUE : FALSE);
  SetRenderStateCached(D3DRS_SEPARATEALPHABLENDENABLE, TRUE);
  SetRenderStateCached(D3DRS_SRCBLEND, HostBlendFactor(blend.color_src));
  SetRenderStateCached(D3DRS_DESTBLEND, HostBlendFactor(blend.color_dst));
  SetRenderStateCached(D3DRS_BLENDOP, HostBlendOp(blend.color_op));
  SetRenderStateCached(D3DRS_SRCBLENDALPHA, HostBlendFactor(blend.alpha_src));
  SetRenderStateCached(D3DRS_DESTBLENDALPHA, HostBlendFactor(blend.alpha_dst));
  SetRenderStateCached(D3DRS_BLENDOPALPHA, HostBlendOp(blend.alpha_op));

  // GLASS HUNT, round 2. Constant blend factors were the first theory and PROF/constblend
  // measured them at exactly 0 draws/frame, so D3DRS_BLENDFACTOR is not it.
  //
  // Enumerate what the guest ACTUALLY asks for instead of guessing again: every distinct blend
  // configuration, with a draw count. If intact glass turns out to be drawn with blending
  // DISABLED, the fault is upstream of blending entirely (the material, or our reading of the
  // register). If it is enabled with SRCALPHA/INVSRCALPHA, then the source alpha is arriving as
  // 1.0 and the fault is in the pixel shader's alpha or the texture's alpha channel -- two very
  // different investigations, and this says which.
  // Either cvar arms this block. The isolate check used to be nested inside the log check, so
  // setting isolate alone did nothing -- the index bookkeeping it depends on never ran. A debug
  // tool that silently no-ops is worse than no tool: it reads as "the theory is wrong" rather
  // than "the switch was off".
  // Arm on EITHER factor, matching the wildcard rule inside. This gate previously tested only
  // dbg_blend_src, so `dst 7` with src left at -1 never entered the block at all -- the same
  // unarmed-filter failure as before, fixed one level too shallow. The inner condition was
  // corrected while the gate that decides whether it runs was not.
  if (REXCVAR_GET(nx1_d3d9_dbg_blend_log) ||
      int32_t(REXCVAR_GET(nx1_d3d9_dbg_blend_src)) >= 0 ||
      int32_t(REXCVAR_GET(nx1_d3d9_dbg_blend_dst)) >= 0) {
    const uint64_t sig = uint64_t(blend.enabled) | (uint64_t(blend.color_src) << 1) |
                         (uint64_t(blend.color_dst) << 7) | (uint64_t(blend.color_op) << 13) |
                         (uint64_t(blend.alpha_src) << 17) | (uint64_t(blend.alpha_dst) << 23) |
                         (uint64_t(blend.alpha_op) << 29) | (uint64_t(d.color_write_mask) << 33);
    static std::mutex m;
    static std::vector<std::pair<uint64_t, uint64_t>> seen;  // sig -> draw count
    std::lock_guard<std::mutex> lk(m);
    auto it = std::find_if(seen.begin(), seen.end(),
                           [sig](const auto& e) { return e.first == sig; });
    if (it != seen.end()) {
      ++it->second;
    } else if (seen.size() < 32) {
      seen.push_back({sig, 1});
      it = seen.end() - 1;  // so a freshly discovered config can be isolated on its first draw
      REXGPU_INFO("nx1_d3d9: BLENDCFG #{} enabled={} colour {}->{} op={} alpha {}->{} op={} "
                  "writemask={:#x}",
                  seen.size() - 1, blend.enabled ? 1 : 0, blend.color_src, blend.color_dst,
                  blend.color_op, blend.alpha_src, blend.alpha_dst, blend.alpha_op,
                  d.color_write_mask);
    }
    // Attribution. Set dbg_blend_src/dst to a factor pair from the BLENDCFG lines: with verify
    // off those draws write no colour and visibly vanish (so you can confirm which surface uses
    // them); with verify on they render normally and report their real device state and textures.
    // Matched by VALUE, not by discovery index. Indices are assigned in the order configs are
    // first seen, so they shift between runs -- and that already cost two runs and one wrong
    // conclusion: a verify captured when #2 meant ONE->INVSRCALPHA was read against an
    // identification made when #2 meant ZERO->SRCCOLOR. The Xenos factor pair is stable, so
    // target that instead and the mistake cannot recur.
    // -1 means "any" for either factor, so a single one can be targeted. Requiring both to be set
    // meant `dst 7` alone silently matched nothing, which reads as "that config is not the glass"
    // rather than "the filter was never armed".
    const int32_t want_src = int32_t(REXCVAR_GET(nx1_d3d9_dbg_blend_src));
    const int32_t want_dst = int32_t(REXCVAR_GET(nx1_d3d9_dbg_blend_dst));
    bool value_match = (want_src >= 0 || want_dst >= 0) &&
                       (want_src < 0 || uint32_t(want_src) == blend.color_src) &&
                       (want_dst < 0 || uint32_t(want_dst) == blend.color_dst);
    // Enumerate the distinct MATERIALS behind the matching draws before narrowing to one, so the
    // list is complete even while a filter is active.
    if (value_match) {
      static std::mutex pm;
      static std::vector<std::pair<uint32_t, uint64_t>> ps_seen;  // ps_object -> draw count
      std::lock_guard<std::mutex> plk(pm);
      auto pit = std::find_if(ps_seen.begin(), ps_seen.end(),
                              [&d](const auto& e) { return e.first == d.ps_object; });
      if (pit != ps_seen.end()) {
        ++pit->second;
      } else if (ps_seen.size() < 128) {
        ps_seen.push_back({d.ps_object, 1});
        REXGPU_INFO("nx1_d3d9: BLENDPS #{} ps={:08X} vs={:08X} -- set nx1_d3d9_dbg_blend_ps to "
                    "this ps value to isolate this material by itself",
                    ps_seen.size() - 1, d.ps_object, d.vs_object);
        // Say when the list is truncated. A capped list that does not announce itself reads as a
        // COMPLETE enumeration, and a search over it would silently exclude the answer -- the
        // same class of quiet-truncation error as an unarmed filter reading as a real negative.
        if (ps_seen.size() == 128) {
          REXGPU_INFO("nx1_d3d9: BLENDPS list FULL at 128 -- more materials exist but are not "
                      "listed; a search over this list is NOT exhaustive");
        }
      }
    }
    if (const uint32_t want_ps = REXCVAR_GET(nx1_d3d9_dbg_blend_ps);
        want_ps && d.ps_object != want_ps) {
      value_match = false;
    }
    const uint32_t ps_lo = REXCVAR_GET(nx1_d3d9_dbg_blend_ps_lo);
    const uint32_t ps_hi = REXCVAR_GET(nx1_d3d9_dbg_blend_ps_hi);
    if ((ps_lo && d.ps_object < ps_lo) || (ps_hi && d.ps_object > ps_hi)) {
      value_match = false;
    }
    if (value_match) {
      ++prof_blend_match_draws_;
    }
    if (value_match) {
      if (REXCVAR_GET(nx1_d3d9_dbg_blend_verify)) {
        // Ask D3D what it ACTUALLY has, rather than trusting our shadow.
        //
        // Glass is BLENDCFG #2: colour ZERO->SRCCOLOR, i.e. dest * srcColour -- multiply
        // blending. A multiply blend CANNOT produce an opaque surface: white output would leave
        // the scenery untouched and a tint would colour it. Opaque means the blend is not being
        // applied at all, and the likeliest reason is SetRenderStateCached skipping a call
        // because its shadow disagrees with the device (the hazard InvalidateStateShadow exists
        // for). Reading the state back is the only way to tell "we never issued it" apart from
        // "we issued it and it did not take".
        // Keyed on the TARGET, not a plain one-shot. A bare `static bool reported` latched on the
        // first match ever, so changing dbg_blend_src/dst mid-session silently kept reporting the
        // old configuration -- a stale line read as if it described the new target. Same class of
        // error as the discovery-order indices: state from one configuration leaking into a
        // reading of another.
        static std::mutex vm;
        static uint64_t reported_for = ~uint64_t(0);
        // The address the tracker was last pointed at. A pure one-shot armed once and then went
        // stale the moment the streaming pool handed this material a different allocation -- every
        // TRACK line afterwards described a dead address that is never bound, which reads exactly
        // like "the texture is fine, nothing is writing it". Re-arming on CHANGE keeps the tracker
        // on the live texture AND turns the move itself into the signal we are hunting.
        static uint32_t last_armed_addr = 0;
        std::lock_guard<std::mutex> vlk(vm);
        // Latch on the target AND the sampler being tracked, and only once the tracker has
        // actually been armed. Latching on the first match anywhere meant a MENU draw claimed the
        // one-shot -- it matched 1->7 but had no sampler 10, so it reported nothing, marked the
        // target done, and silently ignored every in-game draw afterwards. Two runs lost to that.
        // A one-shot must fire on the first USEFUL event, not the first event.
        const int32_t track_slot_key = int32_t(REXCVAR_GET(nx1_d3d9_dbg_blend_track_sampler));
        // The MATERIAL FILTER has to be part of the latch key. Without it, narrowing ps_lo/ps_hi
        // left the one-shot believing it had already reported -- so five bisection rounds ran with
        // the verify silently producing nothing, and the only lines in the log were stale ones
        // describing a different set of draws. Same failure as the target_key fix that preceded it:
        // a latch is only safe when its key covers everything that changes what it would report.
        const uint64_t target_key = (uint64_t(uint32_t(want_src)) << 40) |
                                    (uint64_t(uint32_t(want_dst)) << 32) |
                                    (uint64_t(uint32_t(track_slot_key)) << 24) |
                                    (uint64_t(ps_lo) ^ (uint64_t(ps_hi) << 12) ^
                                     (uint64_t(REXCVAR_GET(nx1_d3d9_dbg_blend_ps)) << 20));
        // Require a REAL texture in that slot, not just a mask bit. active_sampler_mask_ is
        // 0xFFFF whenever the shader could not be walked ("bind everything"), so testing the bit
        // said yes for slot 10 on a draw that had nothing bound there -- it latched, printed
        // samplers 0..8, and armed nothing. Decoding the fetch constant is the actual question.
        const bool want_arm = track_slot_key >= 0;
        bool armable = !want_arm;
        uint32_t probe_addr = 0;
        if (want_arm && (active_sampler_mask_ & (1u << (track_slot_key & 0xF)))) {
          const TextureFetchConstant probe =
              DecodeTextureFetchConstant(d.texture_fetch(uint32_t(track_slot_key) & 0xF));
          armable = probe.valid && probe.base_address != 0;
          probe_addr = probe.base_address;
        }
        const bool moved = probe_addr != 0 && probe_addr != last_armed_addr;
        // REPORTING and ARMING are separate concerns. Requiring `armable` to report meant that
        // naming a sampler the matched material does not bind silenced the verify entirely -- the
        // tool answered "nothing matched" when it had matched 18 draws a frame and simply declined
        // to speak. Report on the first draw for a target regardless; arm only when the requested
        // slot really holds a texture, and re-report when that texture's address changes.
        if (reported_for != target_key || (moved && armable)) {
          // Only meaningful once ONE material is selected. Sampler slots are per-draw, not per
          // material, so without the filter this compares slot 10 across unrelated draws and
          // reports every one as a reallocation -- it fired continuously within a single
          // millisecond and looked exactly like the pool churning.
          if (moved && last_armed_addr != 0 &&
              (REXCVAR_GET(nx1_d3d9_dbg_blend_ps) || ps_lo || ps_hi)) {
            REXGPU_INFO("nx1_d3d9: BLENDVERIFY sampler{} MOVED {:08X} -> {:08X} -- this MATERIAL's "
                        "texels are at a new guest address; the old one is now someone else's data",
                        track_slot_key, last_armed_addr, probe_addr);
          }
          last_armed_addr = probe_addr;
          reported_for = target_key;
          DWORD be = 0, sb = 0, db = 0, sep = 0, cw = 0, af = 0;
          device_->GetRenderState(D3DRS_ALPHABLENDENABLE, &be);
          device_->GetRenderState(D3DRS_SRCBLEND, &sb);
          device_->GetRenderState(D3DRS_DESTBLEND, &db);
          device_->GetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, &sep);
          device_->GetRenderState(D3DRS_COLORWRITEENABLE, &cw);
          device_->GetRenderState(D3DRS_ALPHATESTENABLE, &af);
          // Name the MATERIAL in the report. Without it the block says nothing about which of the
          // matched draws it describes, so a range covering several materials produced a texture
          // list that could not be attributed -- and the first-drawn material claims the one-shot,
          // which is not necessarily the one being hunted.
          REXGPU_INFO("nx1_d3d9: BLENDVERIFY ps={:08X} vs={:08X} device says blendenable={} "
                      "src={} dst={} separate={} colourwrite={:#x} alphatest={} || we intended "
                      "enable={} src={} dst={} mask={:#x}",
                      d.ps_object, d.vs_object, be, sb, db, sep, cw, af, blend.enabled ? 1 : 0,
                      uint32_t(HostBlendFactor(blend.color_src)),
                      uint32_t(HostBlendFactor(blend.color_dst)), d.color_write_mask);
          // Name the textures this draw samples. With the blend proven correct, an opaque result
          // under ONE->INVSRCALPHA means the shader emitted alpha 1.0, and the usual source of a
          // wrong alpha is the texture's alpha channel decoding opaque -- the same family as the
          // BC broadcast-swizzle bug that made smoke sprites invisible. The address is what the
          // existing texture tooling keys on (nx1_d3d9_dbg_track_addr, TRACK/DECODE logging), so
          // printing it here is what turns "the shader is wrong" into something inspectable.
          // Auto-arm the texture tracker on one of this draw's samplers, so no address ever has
          // to be copied by hand between runs. Guest allocations MOVE between launches, and
          // hand-carrying an address from an old log is how the muzzle-flash hunt wasted three
          // rounds -- and it just happened again (the tracked address was a leftover from that
          // very investigation). Setting it from inside the matched draw cannot go stale.
          const int32_t track_slot = int32_t(REXCVAR_GET(nx1_d3d9_dbg_blend_track_sampler));
          for (uint32_t s = 0; s < 16; ++s) {
            if (!(active_sampler_mask_ & (1u << s))) {
              continue;
            }
            const TextureFetchConstant t = DecodeTextureFetchConstant(d.texture_fetch(s));
            if (!t.valid || !t.base_address) {
              continue;
            }
            if (track_slot >= 0 && uint32_t(track_slot) == s) {
              REXCVAR_SET(nx1_d3d9_dbg_track_addr, t.base_address);
              REXGPU_INFO("nx1_d3d9: BLENDVERIFY   >>> tracking sampler{} tex={:08X} <<<", s,
                          t.base_address);
            }
            REXGPU_INFO("nx1_d3d9: BLENDVERIFY   sampler{} tex={:08X} {}x{} fmt={} swizzle={:#05X} "
                        "gamma={} miplevels={}..{}",
                        s, t.base_address, t.width, t.height, t.format, t.swizzle,
                        t.gamma ? 1 : 0, t.mip_min_level, t.mip_max_level);
          }
        }
      } else {
        // SKIP the draw rather than zeroing the colour-write mask. Zeroing left that state on the
        // device, and ImGui's D3D9 backend does not restore D3DRS_COLORWRITEENABLE -- so the F4
        // overlay itself rendered invisibly. A debug tool that hides the debug UI is not usable.
        skip_draw_ = true;
      }
    }
  }

  // Cull. D3D9 folds the front-face winding into the cull direction (it has no
  // separate "front face" state), so resolve the Xenos (cull, winding) pair here.
  const CullState& cull = d.cull;
  D3DCULL mode = D3DCULL_NONE;
  if (cull.cull_back) {
    mode = cull.front_is_cw ? D3DCULL_CCW : D3DCULL_CW;
  } else if (cull.cull_front) {
    mode = cull.front_is_cw ? D3DCULL_CW : D3DCULL_CCW;
  }
  SetRenderStateCached(D3DRS_CULLMODE, mode);
}

void Renderer::InvalidateRenderStateShadow() {
  for (uint32_t i = 0; i < kMaxRenderState; ++i) {
    render_state_[i] = kRenderStateUnset;
  }
}

void Renderer::InvalidateVertexShadow() {
  bound_decl_ = kVertexDeclUnknown;
  for (uint32_t i = 0; i < 4; ++i) {
    bound_stream_vb_[i] = kVertexBufferUnknown;
    bound_stream_stride_[i] = ~0u;
  }
}

void Renderer::InvalidateStateShadow() {
  for (uint32_t s = 0; s < 16; ++s) {
    sampler_texture_[s] = kSamplerTextureUnknown;
    for (uint32_t i = 0; i < kSamplerStates; ++i) {
      sampler_state_[s][i] = kSamplerStateUnset;
    }
  }
  bound_vs_ = kVertexShaderUnknown;
  bound_ps_ = kPixelShaderUnknown;
  bound_color_write_ = kColorWriteUnset;
  InvalidateVertexShadow();
  InvalidateRenderStateShadow();
  last_const_vs_ = nullptr;
  last_const_ps_ = nullptr;
  last_const_ring_gen_ = ~uint64_t(0);
  last_vs_uniform_shader_ = nullptr;
  last_ps_uniform_shader_ = nullptr;
  last_ps_dims_mask_ = 0;
  ShaderCache::Get().InvalidateDefShadow();
  // The draw-signature skip trusts that the device still holds the previous draw's bindings;
  // once something else has rebound them that is no longer true.
  last_sig_valid_ = false;
}

void Renderer::BindTextures(const uint8_t* base, const RecordedDraw& d,
                            uint64_t surface_key) {
  auto& tracker = ResourceTracker::Get();
  // Fetch constants come from the RECORD, not from guest memory: d.fetch_constants holds all 32
  // slots verbatim (big-endian, unswapped), so DecodeTextureFetchConstant reads them exactly as
  // it reads a live guest pointer. Everything else this function needs -- clamp modes, filters,
  // anisotropy, gamma -- is decoded from that same constant, so the whole function is now
  // deferrable. `base` survives only for the texture DATA, which is bulk memory a worker reads
  // late by design.
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
    std::memcpy(&sig[sig_n], d.texture_fetch(sampler), 6 * sizeof(uint32_t));
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

  // Aim the texture dump at ONE material. Shape filters (size/format) were tried first and matched
  // nothing: the dimensions a material binds change between runs, so a filter copied from an older
  // log silently selects no texture at all, which reads exactly like "the texture is fine".
  // ps_object names the material and is stable for the life of the loaded fastfile.
  if (const uint32_t dump_ps = REXCVAR_GET(nx1_d3d9_dbg_blend_ps)) {
    tracker.SetDumpDraw(d.ps_object == dump_ps);
  }

  for (uint32_t sampler = 0; sampler < 16; ++sampler) {
    if (!(sampler_mask & (1u << sampler))) {
      continue;
    }
    // NOTE: a fetch-constant memo was tried here and MEASURED NET-NEGATIVE (textures 8.05 ->
    // 9.27ms). Consecutive draws are usually different materials, so the constant differs, the
    // memo misses, and the compare is pure added cost. Don't re-add it without data.
    // Decoded once here and handed to GetTexture: the sampler state below needs the same
    // constant, and decoding it twice per bound slot was six byte-swapped dwords of pure waste.
    const TextureFetchConstant t = DecodeTextureFetchConstant(d.texture_fetch(sampler));
    IDirect3DBaseTexture9* tex = tracker.GetTexture(base, t, sampler);
    if (tex) {
      // For a static-geometry world draw, hold the highest-res texture this surface has shown and
      // substitute it when the engine swaps sampler to a receding (garbage) LOD -- see
      // PreferLargestForSurface. surface_key 0 (UI / inline draws) leaves the binding untouched.
      tex = tracker.PreferLargestForSurface(surface_key, sampler, t.format, tex, t.width, t.height,
                                            t.base_address);
    }
    if (tex != sampler_texture_[sampler]) {
      device_->SetTexture(sampler, tex);
      sampler_texture_[sampler] = tex;
    }
    if (!tex) {
      continue;
    }
    // Clamp modes live in dword0 of the fetch constant GetTexture already read and handed
    // back; ReadSamplerClampModes would fetch that same dword out of guest memory again,
    // once per bound slot per draw.
    const SamplerClampModes clamp{t.clamp_u, t.clamp_v, t.clamp_w};
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
  // Never let the dump flag survive this draw, or the next material's textures get attributed to
  // the one being investigated.
  tracker.SetDumpDraw(false);
}

void Renderer::CaptureDrawState(const uint8_t* base, uint32_t guest_device, RecordedDraw& d) {
  // Every fetch constant, as one memcpy of RAW guest bytes. The 32 slots are contiguous, and
  // -- exactly as with the constant blocks -- byte-swapping here is what makes recording
  // expensive: 32 slots x 6 dwords of GuestRead32 is 192 swapped reads per draw, which at
  // ~5000 draws was ~976k reads and several ms on the very thread this scheme exists to
  // unload. The executor swaps what it reads, on the worker.
  // GuestPointer, NOT base + guest_device + offset: the device is a physical-mirror EA and raw
  // pointer arithmetic on it silently reads the wrong page. This exact line rendered the whole
  // game black once already -- it was latent from the moment it was written, because nothing
  // consumed the recorded constants until the texture path started reading them.
  //
  // TWO REGIONS, not the whole array. Only the head (16 texture slots) and the tail (the vertex
  // streams, aliased as 2-dword slots 92..95) are ever read; the 368 bytes between are dead
  // weight. At ~4900 draws that is 3.75 MB/frame copied instead of 2.0 -- and this copy sits on
  // the guest thread, which PROF/bound shows is the longer pole while the worker idles ~3.4
  // ms/frame. Trimming it is the one part of that thread's time we actually control.
  const uint8_t* fetch = GuestPointer(base, guest_device + guest_device::kFetchConstants);
  static constexpr size_t kTextureBytes = 16 * guest_device::kFetchConstantStride;  // slots 0..15
  static constexpr size_t kStreamSlot0 = size_t(VertexFetchSlotForStream(kMaxHostStreams - 1)) * 8;
  static constexpr size_t kStreamBytes = sizeof(d.fetch_constants) - kStreamSlot0;
  std::memcpy(d.fetch_constants, fetch, kTextureBytes);
  std::memcpy(reinterpret_cast<uint8_t*>(d.fetch_constants) + kStreamSlot0, fetch + kStreamSlot0,
              kStreamBytes);

  d.vs_object = BoundVertexShader(base, guest_device);
  d.ps_object = BoundPixelShader(base, guest_device);
  d.vs_pass = VertexShaderPass(base, d.vs_object, /*has_pixel_shader=*/d.ps_object != 0);
  d.vertex_declaration = BoundVertexDeclaration(base, guest_device);
  for (uint32_t stream = 0; stream < kMaxHostStreams; ++stream) {
    d.stream_stride[stream] = StreamStride(base, guest_device, stream);
  }

  // Stable per-surface identity for prefer-largest LOD substitution: a static world surface draws
  // the same index-buffer range every frame (the index buffers plateau at a few hundred addresses),
  // so (ib address, draw range) keys the surface across its near/far LOD swaps. Mix in a golden-ratio
  // constant so start_index/index_count don't collide across nearby ranges.
  d.index_buffer = ReadIndexBuffer(base, guest_device);
  d.surface_key = uint64_t(d.index_buffer.base_address) ^ (uint64_t(d.start_index) * 0x9E3779B185EBCA87ull) ^
                  (uint64_t(d.index_count) << 40) ^
                  (uint64_t(d.base_vertex_index) * 0xD1B54A32D192ED03ull);

  d.viewport = ReadViewportState(base, guest_device);
  d.depth = ReadDepthState(base, guest_device);
  d.blend = ReadBlendState(base, guest_device);
  d.cull = ReadCullState(base, guest_device);
  d.alpha = ReadAlphaTestState(base, guest_device);
  d.color_write_mask = ReadColorWriteMask(base, guest_device);

  // Last, because the constant skip is keyed on the shader pair this resolves. Everything above
  // is a plain read of device state; this is the part that MUST stay on the guest thread.
  ResolveShadersAndConstants(base, guest_device, d);
}

void Renderer::RecordDraw(const uint8_t* base, uint32_t guest_device, uint32_t prim_type,
                          uint32_t base_vertex_index, uint32_t start_index,
                          uint32_t index_count) {
  RecordedDraw& d = cmdbuf_.AddDraw();
  d.prim_type = prim_type;
  d.base_vertex_index = base_vertex_index;
  d.start_index = start_index;
  d.index_count = index_count;
  d.indexed = true;
  CaptureDrawState(base, guest_device, d);

  // Only the register groups the guest actually rewrote. guest_dirty_* were captured in the
  // draw hook, before the guest's body flushed and zeroed them. Only the buffered path records
  // a delta: the UP paths build a scratch RecordedDraw on the stack and execute it immediately,
  // so appending their constants to the frame's pool would be pure waste.
  d.constant_delta =
      cmdbuf_.RecordConstantDelta(base, guest_device, guest_dirty_vs_, guest_dirty_ps_);
}

void Renderer::ExecuteCommand(const uint8_t* base, const RecordedCommand& c) {

  switch (c.kind) {
    case CommandKind::kDraw:
      ExecuteDraw(base, c.guest_device, cmdbuf_.draws()[c.draw_index]);
      break;
    case CommandKind::kClear:
      ExecuteClear(c);
      break;
    case CommandKind::kResolve:
      ExecuteResolve(base, c);
      break;
    case CommandKind::kSetRenderTarget:
      ExecuteSetRenderTarget(base, c);
      break;
    case CommandKind::kSetDepthStencil:
      ExecuteSetDepthStencil(base, c);
      break;
  }
}

void Renderer::SubmitCommand(const uint8_t* base, uint32_t guest_device, uint32_t command_index) {
  // Stamp the device onto the COMMAND, never onto a slot the worker reads later -- see
  // RecordedCommand::guest_device for what a shared slot cost. Non-draw commands pass 0 and do
  // not use it, but a draw queued behind them must still find its own.
  cmdbuf_.command(command_index).guest_device = guest_device;
  if (!worker_active_) {
    ExecuteCommand(base, cmdbuf_.commands()[command_index]);
    return;
  }
  // Async: publish the command and let the worker pick it up. The guest thread returns
  // immediately and carries on with its own between-draw logic, which is the entire point --
  // that logic was measured at 5.3-7.5 ms a frame sitting idle behind our translation.
  {
    std::lock_guard<std::mutex> lk(queue_mutex_);
    worker_base_ = base;  // genuinely constant for the process, unlike guest_device
    queue_head_ = size_t(command_index) + 1;
  }
  queue_cv_.notify_one();
}

void Renderer::DrainWorker() {
  if (!worker_active_) {
    return;
  }
  // WHO WAITS FOR WHOM is the whole bottleneck question, and it is measurable rather than
  // arguable. Guest blocked here => the worker is the limit (our translation). Worker idle in
  // WorkerMain => the guest is the limit (its game logic plus our recording). Neither => the two
  // are balanced, and anything left is the GPU.
  const auto t0 = std::chrono::steady_clock::now();
  {
    std::unique_lock<std::mutex> lk(queue_mutex_);
    queue_done_cv_.wait(lk, [this] { return queue_tail_ >= queue_head_ || !worker_running_; });
  }
  const uint64_t waited = uint64_t((std::chrono::steady_clock::now() - t0).count());
  prof_drain_wait_ns_ += waited;
  prof_drain_wait_frame_ns_ += waited;
}

void Renderer::WorkerMain() {
  for (;;) {
    size_t index = 0;
    const uint8_t* base = nullptr;
    {
      const auto t0 = std::chrono::steady_clock::now();
      std::unique_lock<std::mutex> lk(queue_mutex_);
      const bool had_work = queue_tail_ < queue_head_;
      queue_cv_.wait(lk, [this] { return queue_tail_ < queue_head_ || !worker_running_; });
      if (!had_work) {
        // Starved: the guest had nothing queued yet. Accumulated relaxed -- it is read once a
        // second by the reporter and exactness does not matter, but tearing a 64-bit counter
        // would.
        prof_worker_idle_ns_.fetch_add(uint64_t((std::chrono::steady_clock::now() - t0).count()),
                                       std::memory_order_relaxed);
      }
      if (!worker_running_ && queue_tail_ >= queue_head_) {
        return;
      }
      index = queue_tail_;
      base = worker_base_;
    }
    // Executed OUTSIDE the lock: this is the ~10 ms of translation whose whole purpose is to
    // overlap with the guest thread. Holding queue_mutex_ across it would serialise the two
    // again and buy exactly nothing. Safe because the guest only ever APPENDS to cmdbuf_, and
    // its deques and chunked constant pool never move an element that has been handed out.
    ExecuteCommand(base, cmdbuf_.commands()[index]);
    {
      std::lock_guard<std::mutex> lk(queue_mutex_);
      queue_tail_ = index + 1;
      if (queue_tail_ >= queue_head_) {
        queue_done_cv_.notify_all();
      }
    }
  }
}

void Renderer::ProbeStability(ProbeKind kind, uint32_t addr, uint32_t bytes) {
  // Locked because this is called from BOTH threads under async: the ucode probe runs on the
  // guest thread inside ResolveShadersAndConstants, while the vertex and index probes run on the
  // worker inside ExecuteDraw. Concurrent push_back on the same vector is a corruption-or-crash
  // data race -- and a diagnostic taking down the thing it is measuring is the worst kind.
  std::lock_guard<std::mutex> lk(prof_probe_mutex_);
  // STRIDE, not first-N. Taking the first 256 of each class sampled only the frame's opening
  // passes -- the same shadow-cascade draws every frame -- and reported a perfect 100% across
  // all three classes while the original first-512 probe had found 1 vertex range in 448
  // changing. A measurement that only ever looks at the most static part of the frame will
  // always say the frame is static. Sampling every kStride-th draw spreads the budget over the
  // whole frame instead; at ~5000 draws that is ~310 probes per class, still cheap.
  static constexpr size_t kStride = 16;
  static constexpr size_t kPerKind = 512;
  const uint32_t k = uint32_t(kind);
  const size_t seen = prof_probe_seen_[k]++;
  if ((seen % kStride) != 0 || prof_probe_taken_[k] >= kPerKind || !bytes ||
      bytes > (16u << 20)) {
    return;
  }
  ++prof_probe_taken_[k];
  // Microcode lives at a GPU physical address; vertex and index data at guest physical. Using
  // the wrong translation here is the mistake that has blacked the screen three times, and in a
  // diagnostic it would be worse -- it would silently report bogus instability.
  const uint8_t* p = kind == ProbeKind::kUcode ? GuestTranslateGpuPhysical(addr)
                                               : ResourceTracker::Get().PhysicalPointer(addr);
  if (!p) {
    return;
  }
  prof_stability_.push_back({addr, bytes, XXH3_64bits(p, bytes), kind});
}

void Renderer::CaptureGuestDirtyMask(const uint8_t* base, uint32_t guest_device) {
  if (!guest_device) {
    return;
  }
  guest_dirty_vs_ = GuestRead64(base, guest_device + guest_device::kAluDirtyMaskVs);
  guest_dirty_ps_ = GuestRead64(base, guest_device + guest_device::kAluDirtyMaskPs);
  if (!REXCVAR_GET(nx1_d3d9_profile)) {
    return;
  }
  for (uint32_t i = 0; i < 5; ++i) {
    const uint32_t addr = guest_device + guest_device::kPendingMask + i * 8;
    const uint64_t mask = (uint64_t(GuestRead32(base, addr)) << 32) | GuestRead32(base, addr + 4);
    prof_mask_clear_[i] += mask == 0 ? 1 : 0;
    prof_mask_same_[i] += mask == prof_last_mask_[i] ? 1 : 0;
    prof_last_mask_[i] = mask;
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

  if (REXCVAR_GET(nx1_d3d9_profile)) {
    // Charge the gap before this draw to pre-draw logic or to interleaved logic. If the
    // guest's time all lands in "pre-draw", within-frame deferral has nothing to overlap.
    const auto tl_now = std::chrono::steady_clock::now();
    if (!prof_saw_draw_) {
      prof_saw_draw_ = true;
      if (prof_frame_start_.time_since_epoch().count()) {
        prof_gap_before_first_ns_ += uint64_t((tl_now - prof_frame_start_).count());
      }
    } else if (prof_last_draw_end_.time_since_epoch().count()) {
      prof_gap_between_ns_ += uint64_t((tl_now - prof_last_draw_end_).count());
    }
  }

  // MEASUREMENT, not an optimisation yet. The guest maintains its own dirty bits
  // (m_Pending.m_Mask[5]) and zeroes them when a draw flushes, and we run inside the hook
  // BEFORE the guest's draw body -- so these are still valid here. If consecutive draws leave
  // most of them clear, we can skip re-resolving that state instead of re-reading it every
  // draw, which attacks the per-draw cost directly rather than merging draws.
  //
  // Also tracked: how often a draw is a pure continuation of the previous one (same index
  // buffer, index range picking up exactly where the last ended). That is the batchable
  // fraction -- the draws that could merge into a single DrawIndexedPrimitive.
  if (REXCVAR_GET(nx1_d3d9_profile)) {
    if (start_index == prof_last_index_end_ && prof_last_prim_type_ == prim_type) {
      ++prof_contiguous_draws_;
    }
    prof_last_index_end_ = start_index + index_count;
    prof_last_prim_type_ = prim_type;
  }

  // A frame that would exceed the command buffer's reserved capacity must not append: growing it
  // reallocates storage the worker is indexing. Drain, then run the rest of this frame
  // synchronously -- correct, and it only costs the overlap on a frame that is already an
  // outlier (the reserve is ~6x a typical 5000-draw frame). BeginFrame restores async next frame.
  if (worker_active_ && cmdbuf_.full()) {
    DrainWorker();
    worker_active_ = false;
    static bool warned = false;
    if (!warned) {
      warned = true;
      REXGPU_WARN("nx1_d3d9: command buffer full ({} draws) -- rest of frame runs synchronously",
                  cmdbuf_.draws().size());
    }
  }

  // Record, then execute from the record. STEP 2 of deferred translation: still synchronous, so
  // this buys no speed -- what it buys is that there is exactly ONE path. A "live" path reading
  // guest memory alongside a "recorded" one is how a capture gap stays invisible until the day
  // it becomes a race; here a missing field is a rendering bug immediately, on the guest thread,
  // with nothing else in the picture. Recording cost shows up as PROF/record.
  const auto t_rec = std::chrono::steady_clock::now();
  RecordDraw(base, guest_device, prim_type, base_vertex_index, start_index, index_count);
  {
    const uint64_t rec = uint64_t((std::chrono::steady_clock::now() - t_rec).count());
    prof_record_ns_ += rec;
    prof_record_frame_ns_ += rec;
  }

  SubmitCommand(base, guest_device, uint32_t(cmdbuf_.commands().size() - 1));
}

void Renderer::ExecuteDraw(const uint8_t* base, uint32_t guest_device, const RecordedDraw& d) {
  const D3DPRIMITIVETYPE host_prim = HostPrimitiveType(d.prim_type);
  const uint32_t prim_count = HostPrimitiveCount(d.prim_type, d.index_count);
  const uint32_t base_vertex_index = d.base_vertex_index;
  const uint32_t start_index = d.start_index;
  const uint32_t index_count = d.index_count;

  // TEMP PROFILING: per-phase frame cost. ~2000 draws/frame at ~19us each says the bottleneck
  // is our own per-draw work, not Xenia -- but which part is guesswork until measured.
  using PClock = std::chrono::steady_clock;
  const bool prof = REXCVAR_GET(nx1_d3d9_profile);
  auto ptick = [&] { return prof ? PClock::now() : PClock::time_point{}; };
  auto padd = [&](uint64_t& slot, PClock::time_point t0) {
    if (prof) slot += uint64_t((PClock::now() - t0).count());
  };

  auto t_vp = ptick();
  ResolveViewport(d);
  padd(prof_viewport_ns_, t_vp);

  auto t_sh = ptick();
  const bool shaders_ok = BindShadersAndConstants(d);
  padd(prof_shaders_ns_, t_sh);
  if (!shaders_ok) {
    return;
  }

  // Indices first: how far into the vertex buffer this draw actually reaches decides how
  // much of it we have to mirror. The guest's fetch constant would have us mirror the whole
  // pool the model happens to live in.
  //
  // The index-buffer DESCRIPTOR comes from the record. It is device state that changes every
  // draw, so re-reading it here meant the worker mirrored whatever the guest had bound tens of
  // draws later -- one mesh's indices through another's vertices, i.e. the whole world as
  // exploded spikes. The index DATA is still read late, which the stability probe does cover.
  auto t_ib = ptick();
  uint32_t index_size = 0;
  IDirect3DIndexBuffer9* ib =
      ResourceTracker::Get().GetIndexBuffer(base, d.index_buffer, &index_size);
  if (!ib) {
    padd(prof_indices_ns_, t_ib);
    return;
  }
  const uint32_t max_index =
      ResourceTracker::Get().GetDrawMaxIndex(d.index_buffer, start_index, index_count);
  const uint32_t needed_vertices = max_index ? base_vertex_index + max_index + 1 : 0;
  padd(prof_indices_ns_, t_ib);

  auto t_vb = ptick();
  uint32_t vertex_count = 0;
  const bool streams_ok = BindStreams(base, d, needed_vertices, &vertex_count);
  padd(prof_streams_ns_, t_vb);
  if (!streams_ok) {
    return;
  }

  if (REXCVAR_GET(nx1_d3d9_profile)) {
    // Record the ranges this draw read, to re-hash at Present. A mismatch means the engine
    // rewrote memory a deferred worker would still have been consuming.
    uint32_t probe_addr = 0, probe_bytes = 0;
    if (ResourceTracker::Get().LastStreamRange(&probe_addr, &probe_bytes) && probe_bytes) {
      ProbeStability(ProbeKind::kVertex, probe_addr, probe_bytes);
    }
    // The INDEX range this draw actually reads. Never measured before -- the executor reads it
    // late purely by analogy with the vertex result, and index buffers are written by a
    // different part of the engine on a different schedule, so the analogy is worth nothing
    // until it is a number.
    if (d.index_buffer.base_address && index_size) {
      ProbeStability(ProbeKind::kIndex, d.index_buffer.base_address + start_index * index_size,
                     index_count * index_size);
    }
  }
  device_->SetIndices(ib);
  auto t_tex = ptick();
  BindTextures(base, d, d.surface_key);
  padd(prof_textures_ns_, t_tex);

  auto t_rs = ptick();
  ApplyRenderStates(d);
  padd(prof_states_ns_, t_rs);

  if (skip_draw_) {
    skip_draw_ = false;
    return;
  }
  auto t_dr = ptick();
  const HRESULT dhr = device_->DrawIndexedPrimitive(host_prim, int(base_vertex_index), 0,
                                                    vertex_count, start_index, prim_count);
  padd(prof_draw_ns_, t_dr);
  if (prof) {
    prof_last_draw_end_ = PClock::now();
  }
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
  // Inline path: this draw is NOT in the ordered command list, so it must not run while the
  // worker is mid-stream -- it touches the device and the same shadow state (bound shaders,
  // samplers, render states) the worker is driving. Draining puts it back in order. Cheap in
  // practice: these are a handful of draws a frame in game, and in menus -- where nearly every
  // draw is a UP draw -- there is no queued world work to wait on.
  DrainWorker();
  ++draws_attempted_;
  // Non-indexed draws are a handful a frame and are not deferred, so this record is a stack
  // scratch rather than a command-buffer entry -- it exists so every path resolves its state
  // through the same CaptureDrawState.
  RecordedDraw d{};
  d.prim_type = prim_type;
  d.start_vertex = start_vertex;
  d.vertex_count = vertex_count;
  d.indexed = false;
  CaptureDrawState(base, guest_device, d);

  ResolveViewport(d);
  if (!BindShadersAndConstants(d)) {
    return;
  }
  // A non-indexed draw reads exactly [start_vertex, start_vertex + vertex_count), so its
  // reach into the (possibly pooled) buffer is known without consulting any indices.
  uint32_t stream_vertices = 0;
  if (!BindStreams(base, d, start_vertex + vertex_count, &stream_vertices)) {
    return;
  }
  BindTextures(base, d);
  ApplyRenderStates(d);
  if (skip_draw_) {  // inline path: consume it too, or it leaks into the next indexed draw
    skip_draw_ = false;
    return;
  }
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
  DrainWorker();  // inline path, not in the command list -- see Draw()
  ++draws_attempted_;
  // Stack scratch, not a command-buffer entry: a UP draw's vertex and index payloads live
  // inline in guest memory and are copied into the D3D call itself, so it can never be deferred.
  RecordedDraw d{};
  d.prim_type = prim_type;
  d.index_count = index_count;
  d.vertex_count = num_vertices;
  CaptureDrawState(base, guest_device, d);

  ResolveViewport(d);

  if (!BindShadersAndConstants(d)) {
    return;
  }

  auto& tracker = ResourceTracker::Get();
  // NX1 UP draws bind no CVertexDeclaration -- the vertex layout is baked into the
  // vertex shader's vfetch. Prefer a declaration if one is set (rare), otherwise
  // derive the layout from the bound shader, keyed on the UP stream stride.
  const VertexLayout* layout = tracker.GetVertexLayout(base, d.vertex_declaration, d.stream_stride);
  if (!layout) {
    layout = tracker.GetShaderVertexLayout(base, d.vs_object, d.vs_pass, vertex_stride,
                                           d.stream_stride);
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

  BindTextures(base, d);
  ApplyRenderStates(d);
  if (skip_draw_) {  // inline path: consume it too, or it leaks into the next indexed draw
    skip_draw_ = false;
    return;
  }
  const HRESULT hr = device_->DrawIndexedPrimitiveUP(
      host_prim, 0, num_vertices, prim_count, indices.data(),
      index_size == 4 ? D3DFMT_INDEX32 : D3DFMT_INDEX16, verts.data(), host_stride);
  if (SUCCEEDED(hr)) {
    ++draws_submitted_;
  }
  // This bound its own declaration, and the runtime sets stream 0 to NULL behind a UP draw.
  InvalidateVertexShadow();
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
  DrainWorker();  // inline path, not in the command list -- see Draw()
  RecordedDraw d{};  // stack scratch -- see DrawIndexedUP
  d.prim_type = prim_type;
  d.vertex_count = vertex_count;
  d.indexed = false;
  CaptureDrawState(base, guest_device, d);

  ResolveViewport(d);
  if (!BindShadersAndConstants(d)) {
    return;
  }

  auto& tracker = ResourceTracker::Get();
  // Same as DrawIndexedUP: NX1 binds no CVertexDeclaration, so derive the layout
  // from the bound vertex shader's vfetch when none is set.
  const VertexLayout* layout = tracker.GetVertexLayout(base, d.vertex_declaration, d.stream_stride);
  if (!layout) {
    layout = tracker.GetShaderVertexLayout(base, d.vs_object, d.vs_pass, vertex_stride,
                                           d.stream_stride);
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

  BindTextures(base, d);
  ApplyRenderStates(d);
  if (skip_draw_) {  // inline path: consume it too, or it leaks into the next indexed draw
    skip_draw_ = false;
    return;
  }
  if (SUCCEEDED(device_->DrawPrimitiveUP(host_prim, prim_count, verts.data(), host_stride))) {
    ++draws_submitted_;
  }
  InvalidateVertexShadow();
}

#endif  // _WIN32

}  // namespace nx1::d3d9
