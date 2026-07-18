// nx1_mp - ReXGlue Recompiled Project
//
// Customize your app by overriding virtual hooks from rex::ReXApp.

#pragma once

#include "generated/5-nx1mp_demo/nx1_mp_init.h"

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/rex_app.h>

#include <algorithm>
#include <string>
#include <string_view>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

class Nx1MpApp : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<Nx1MpApp>(new Nx1MpApp(ctx, "nx1_mp",
        PPCImageConfig));
  }

  // Override virtual hooks for customization:
  void OnPostInitLogging() override {
    const bool use_unorm16 = rex::cvar::Query<bool>("nx1_gamma_render_target_as_unorm16");
    if (!rex::cvar::SetFlagByName("gamma_render_target_as_unorm16",
                                  use_unorm16 ? "true" : "false")) {
      REXLOG_WARN("NX1: failed to set gamma_render_target_as_unorm16");
    } else {
      REXLOG_INFO("NX1: gamma render targets use {}", use_unorm16 ? "UNORM16" : "sRGB");
    }
    ConfigureNx1VideoMode();
    ConfigureNx1Performance();
    ConfigureNx1Input();
  }
  void OnPreSetup(rex::RuntimeConfig& config) override {
    // The native D3D9 renderer (nx1_d3d9) renders the guest frame directly into
    // the host window. Suppress the Xenia host presenter/swapchain so the two
    // never fight for the same HWND: the command processor still executes the
    // ring (fences, swap interrupt), only the host present is dropped. Leaving
    // this off keeps the default Xenia presentation path untouched.
    if (rex::cvar::Query<bool>("nx1_d3d9")) {
      config.suppress_host_presentation = true;
      REXLOG_INFO("NX1 MP: nx1_d3d9 set -- host presenter suppressed, D3D9 owns the window");
    }
  }
  void OnLoadXexImage(std::string& xex_image) override {
    xex_image = "game:\\5-nx1mp_demo.xex";
  }
  bool SetupPresentation() override {
    if (!rex::ReXApp::SetupPresentation()) {
      return false;
    }
    window()->SetTitle("Call Of Duty: Future Warfare");
    return true;
  }
  // void OnPostSetup() override {}
  // void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {}
  void OnShutdown() override {
    RestoreNx1Performance();
  }
  // void OnConfigurePaths(rex::PathConfig& paths) override {}

 private:
  static void SetNx1ConfigFlag(std::string_view name, std::string_view value) {
    if (!rex::cvar::SetFlagByName(name, value)) {
      REXLOG_WARN("NX1: failed to set config flag '{}'", name);
    }
  }

  static void SetNx1InputFlag(std::string_view name, std::string_view value) {
    if (!rex::cvar::SetFlagByName(name, value)) {
      REXLOG_WARN("NX1: failed to set input flag '{}'", name);
    }
  }

  static uint32_t Nx1InternalResolutionWidth() {
    return std::clamp(rex::cvar::Query<uint32_t>("nx1_internal_resolution_width"), 640u,
                      0x0FFFu);
  }

  static uint32_t Nx1InternalResolutionHeight() {
    return std::clamp(rex::cvar::Query<uint32_t>("nx1_internal_resolution_height"), 480u,
                      0x0FFFu);
  }

  struct Nx1RenderResolution {
    uint32_t requested_width;
    uint32_t requested_height;
    uint32_t guest_width;
    uint32_t guest_height;
    uint32_t draw_scale_x;
    uint32_t draw_scale_y;
  };

  static Nx1RenderResolution ResolveNx1RenderResolution() {
    constexpr uint32_t kScaledBaseWidth = 1280;
    constexpr uint32_t kScaledBaseHeight = 720;
    constexpr uint32_t kMaxDrawScale = 8;

    Nx1RenderResolution resolution{};
    resolution.requested_width = Nx1InternalResolutionWidth();
    resolution.requested_height = Nx1InternalResolutionHeight();
    resolution.guest_width = resolution.requested_width;
    resolution.guest_height = resolution.requested_height;
    resolution.draw_scale_x = 1;
    resolution.draw_scale_y = 1;

    if (!rex::cvar::Query<bool>("nx1_internal_resolution_auto_draw_scale")) {
      return resolution;
    }

    if (resolution.requested_width % kScaledBaseWidth != 0 ||
        resolution.requested_height % kScaledBaseHeight != 0) {
      return resolution;
    }

    const uint32_t scale_x = resolution.requested_width / kScaledBaseWidth;
    const uint32_t scale_y = resolution.requested_height / kScaledBaseHeight;
    if (scale_x <= 1 || scale_y <= 1 || scale_x > kMaxDrawScale ||
        scale_y > kMaxDrawScale) {
      return resolution;
    }

    resolution.guest_width = kScaledBaseWidth;
    resolution.guest_height = kScaledBaseHeight;
    resolution.draw_scale_x = scale_x;
    resolution.draw_scale_y = scale_y;
    return resolution;
  }

  static void ConfigureNx1VideoMode() {
    if (!rex::cvar::Query<bool>("nx1_internal_resolution_patch")) {
      return;
    }

    const Nx1RenderResolution resolution = ResolveNx1RenderResolution();
    if (resolution.guest_width != 1280 || resolution.guest_height != 720) {
      SetNx1ConfigFlag("nx1_force_screen_filter_quads_off", "true");
      if (!rex::cvar::HasNonDefaultValue("nx1_force_glow_off")) {
        SetNx1ConfigFlag("nx1_force_glow_off", "true");
      }
      REXLOG_INFO(
          "NX1 MP: disabling 720p glow/filter quads for high internal resolution post effects");
    }

    SetNx1ConfigFlag("draw_resolution_scale_x", std::to_string(resolution.draw_scale_x));
    SetNx1ConfigFlag("draw_resolution_scale_y", std::to_string(resolution.draw_scale_y));

    if (!rex::cvar::HasNonDefaultValue("video_mode_width") &&
        !rex::cvar::HasNonDefaultValue("window_width") &&
        !rex::cvar::HasNonDefaultValue("resolution")) {
      SetNx1ConfigFlag("video_mode_width", std::to_string(resolution.requested_width));
    }

    if (!rex::cvar::HasNonDefaultValue("video_mode_height") &&
        !rex::cvar::HasNonDefaultValue("window_height") &&
        !rex::cvar::HasNonDefaultValue("resolution")) {
      SetNx1ConfigFlag("video_mode_height", std::to_string(resolution.requested_height));
    }

    REXLOG_INFO(
        "NX1 MP: internal resolution patch configured for {}x{} guest, {}x{} output, {}x{} draw "
        "scale",
        resolution.guest_width, resolution.guest_height, resolution.requested_width,
        resolution.requested_height, resolution.draw_scale_x, resolution.draw_scale_y);
  }

  static void ConfigureNx1Input() {
    SetNx1InputFlag("mnk_mode", "true");
    SetNx1InputFlag("mnk_raw_mouse", "true");
    SetNx1InputFlag("mnk_auto_capture_gameplay", "true");
    SetNx1InputFlag("mnk_auto_release_on_key_catcher", "true");
    SetNx1InputFlag("mnk_sensitivity", "1.0");
    SetNx1InputFlag("mnk_mouse_stick_deadzone", "8500");
    SetNx1InputFlag("mnk_mouse_stick_scale", "2800");
    SetNx1InputFlag("mnk_mouse_stick_hold_ms", "18");
    SetNx1InputFlag("keyboard_passthrough", "true");
    SetNx1InputFlag("keybind_b", "Control");
    SetNx1InputFlag("keybind_lstick_press", "Shift");
    SetNx1InputFlag("keybind_rstick_press", "V");
    SetNx1InputFlag("mnk_menu_virtual_cursor", "false");
    SetNx1InputFlag("mnk_menu_virtual_cursor_warp_os", "false");
    SetNx1InputFlag("mnk_menu_direct_hover", "true");
    SetNx1InputFlag("mnk_menu_direct_hover_cursor_only", "true");
    SetNx1InputFlag("mnk_menu_direct_hover_padding", "-2");
    SetNx1InputFlag("mnk_menu_direct_hover_suppress_fallback", "true");
    SetNx1InputFlag("mnk_menu_direct_hover_block_fallback_on_miss", "true");
    SetNx1InputFlag("mnk_menu_direct_hover_fallback_miss_frames", "5");
    SetNx1InputFlag("mnk_menu_direct_hover_stable_frames", "1");
    SetNx1InputFlag("mnk_menu_hover_y_offset", "-0.0425");
    SetNx1InputFlag("mnk_menu_hover_nav", "true");
    SetNx1InputFlag("mnk_menu_click_sync", "true");
    SetNx1InputFlag("mnk_menu_hover_clamp_edges", "false");
    SetNx1InputFlag("mnk_menu_hover_x_min", "0.20");
    SetNx1InputFlag("mnk_menu_hover_x_max", "0.36");
    SetNx1InputFlag("mnk_menu_hover_row_deadzone", "0.016");
    SetNx1InputFlag("mnk_menu_hover_row_count", "6");
    SetNx1InputFlag("mnk_menu_carousel_hover_nav", "false");
  }

  static void ConfigureNx1Performance() {
#if defined(_WIN32)
    RequestHighResolutionTimer();
    if (!SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS)) {
      REXLOG_WARN("NX1: failed to raise process priority");
    }
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
#endif
  }

#if defined(_WIN32)
  using TimePeriodFn = UINT(WINAPI*)(UINT);

  static HMODULE& WinmmModule() {
    static HMODULE module = nullptr;
    return module;
  }

  static bool& HighResolutionTimerActive() {
    static bool active = false;
    return active;
  }

  static void RequestHighResolutionTimer() {
    if (HighResolutionTimerActive()) {
      return;
    }

    HMODULE& module = WinmmModule();
    if (!module) {
      module = LoadLibraryW(L"winmm.dll");
    }
    if (!module) {
      REXLOG_WARN("NX1: failed to load winmm.dll for high resolution timer");
      return;
    }

    auto time_begin_period =
        reinterpret_cast<TimePeriodFn>(GetProcAddress(module, "timeBeginPeriod"));
    if (time_begin_period && time_begin_period(1) == 0) {
      HighResolutionTimerActive() = true;
      REXLOG_INFO("NX1: requested 1 ms Windows timer resolution");
    } else {
      REXLOG_WARN("NX1: failed to request 1 ms Windows timer resolution");
    }
  }

  static void RestoreNx1Performance() {
    if (!HighResolutionTimerActive()) {
      return;
    }

    auto time_end_period =
        reinterpret_cast<TimePeriodFn>(GetProcAddress(WinmmModule(), "timeEndPeriod"));
    if (time_end_period) {
      time_end_period(1);
    }
    HighResolutionTimerActive() = false;
  }
#else
  static void RestoreNx1Performance() {}
#endif
};
