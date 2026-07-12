// nx1_sp - ReXGlue Recompiled Project
//
// Customize your app by overriding virtual hooks from rex::ReXApp.

#pragma once

#include "generated/1-nx1sp/nx1_sp_init.h"
#include "nx1_devgui_overlay.h"

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/kernel/xam/module.h>
#include <rex/logging.h>
#include <rex/platform.h>
#include <rex/rex_app.h>
#include <rex/system/debug_hud.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xthread.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#if REX_PLATFORM_WIN32
#include <Windows.h>
#endif

class Nx1SpApp : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<Nx1SpApp>(new Nx1SpApp(ctx, "nx1_sp",
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
    ConfigureNx1Input();
  }
  // void OnPreSetup(rex::RuntimeConfig& config) override {}
  void OnLoadXexImage(std::string& xex_image) override {
    xex_image = "game:\\1-nx1sp.xex";
  }
  bool SetupPresentation() override {
    if (!rex::ReXApp::SetupPresentation()) {
      return false;
    }
    window()->SetTitle("Call Of Duty: Future Warfare");
    return true;
  }
  void OnGuestThreadExit(rex::system::XThread* thread) override {
    (void)thread;
    if (multiplayer_launch_started_) {
      return;
    }

    const std::string launch_path = PendingLaunchPath();
    if (!IsMultiplayerLaunch(launch_path)) {
      return;
    }

    multiplayer_launch_started_ = true;
    if (!LaunchMultiplayerExecutable()) {
      REXLOG_ERROR("NX1: failed to launch nx1_mp.exe for guest title '{}'", launch_path);
    }
  }
  void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {
    devgui_overlay_ = nx1::sp::CreateDevGuiOverlayDialog(drawer);
  }
  // void OnShutdown() override {}
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

  static void EnsureNx1DebugHudCvarsLinked() {
    volatile bool cvar_link_guard =
        rex::system::IsNx1DebugHudEnabled() ||
        rex::system::IsNx1ForceScreenFilterQuadsOffEnabled() ||
        rex::system::IsNx1ForceGlowOffEnabled();
    (void)cvar_link_guard;
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
    EnsureNx1DebugHudCvarsLinked();

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
          "NX1 SP: disabling 720p glow/filter quads for high internal resolution post effects");
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
        "NX1 SP: internal resolution patch configured for {}x{} guest, {}x{} output, {}x{} draw "
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
    SetNx1InputFlag("mnk_menu_direct_hover_carousel", "true");
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
    SetNx1InputFlag("mnk_menu_carousel_hover_nav", "false");
    SetNx1InputFlag("mnk_menu_carousel_y_min", "0.53");
    SetNx1InputFlag("mnk_menu_carousel_y_max", "0.72");
    SetNx1InputFlag("mnk_menu_carousel_first_x", "0.28");
    SetNx1InputFlag("mnk_menu_carousel_column_step", "0.22");
    SetNx1InputFlag("mnk_menu_carousel_column_deadzone", "0.105");
    SetNx1InputFlag("mnk_menu_carousel_column_count", "3");
    SetNx1InputFlag("mnk_menu_carousel_initial_column", "1");
  }

  std::string PendingLaunchPath() const {
    if (!runtime() || !runtime()->kernel_state()) {
      return {};
    }
    auto xam = runtime()->kernel_state()->GetKernelModule<rex::kernel::xam::XamModule>("xam.xex");
    if (!xam) {
      return {};
    }
    return xam->loader_data().launch_path;
  }

  static std::string LowerAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
      return static_cast<char>(std::tolower(ch));
    });
    return value;
  }

  static bool IsMultiplayerLaunch(const std::string& launch_path) {
    const std::string lower_path = LowerAscii(launch_path);
    return lower_path.find("default_mp.xex") != std::string::npos ||
           lower_path.find("nx1mp") != std::string::npos;
  }

  static bool IsExistingFile(const std::filesystem::path& path) {
    std::error_code ec;
    return !path.empty() && std::filesystem::is_regular_file(path, ec);
  }

  static void AddBaseAndParents(std::vector<std::filesystem::path>& bases,
                                std::filesystem::path base) {
    base = base.lexically_normal();
    while (!base.empty()) {
      if (std::find(bases.begin(), bases.end(), base) == bases.end()) {
        bases.push_back(base);
      }

      const auto parent = base.parent_path();
      if (parent.empty() || parent == base) {
        break;
      }
      base = parent;
    }
  }

  static std::filesystem::path FindMultiplayerExecutable() {
    std::vector<std::filesystem::path> bases;
    AddBaseAndParents(bases, rex::filesystem::GetExecutableFolder());

    std::error_code ec;
    const auto cwd = std::filesystem::current_path(ec);
    if (!ec) {
      AddBaseAndParents(bases, cwd);
    }

    for (const auto& base : bases) {
      const std::filesystem::path direct = base / "nx1_mp.exe";
      if (IsExistingFile(direct)) {
        return direct;
      }

      const std::filesystem::path build_tree =
          base / "nx1_mp" / "out" / "build" / "win-amd64-release" / "nx1_mp.exe";
      if (IsExistingFile(build_tree)) {
        return build_tree;
      }
    }

    return {};
  }

  bool LaunchMultiplayerExecutable() const {
    const std::filesystem::path mp_exe = FindMultiplayerExecutable();
    if (mp_exe.empty()) {
      REXLOG_ERROR("NX1: could not find nx1_mp.exe near the SP executable or workspace root");
      return false;
    }

    const std::filesystem::path cwd = game_data_root().empty() ? mp_exe.parent_path()
                                                              : game_data_root();
    REXLOG_INFO("NX1: launching multiplayer executable: {}", mp_exe.string());
    return LaunchHostProcess(mp_exe, cwd);
  }

  static bool LaunchHostProcess(const std::filesystem::path& exe,
                                const std::filesystem::path& cwd) {
#if REX_PLATFORM_WIN32
    std::wstring command_line = L"\"" + exe.wstring() + L"\" --allow_game_relative_writes";
    STARTUPINFOW startup_info = {};
    startup_info.cb = sizeof(startup_info);

    PROCESS_INFORMATION process_info = {};
    const BOOL ok = CreateProcessW(exe.c_str(), command_line.data(), nullptr, nullptr, FALSE, 0,
                                   nullptr, cwd.empty() ? nullptr : cwd.c_str(), &startup_info,
                                   &process_info);
    if (!ok) {
      REXLOG_ERROR("NX1: CreateProcessW failed with error {} for {}", GetLastError(),
                   exe.string());
      return false;
    }

    CloseHandle(process_info.hThread);
    CloseHandle(process_info.hProcess);
    return true;
#else
    (void)exe;
    (void)cwd;
    REXLOG_ERROR("NX1: multiplayer title switching is only implemented on Windows");
    return false;
#endif
  }

  bool multiplayer_launch_started_ = false;
  std::unique_ptr<rex::ui::ImGuiDialog> devgui_overlay_;
};
