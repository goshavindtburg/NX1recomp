/**
 * @file        ui/rex_app.cpp
 * @brief       ReXApp implementation — compiled as part of the consumer executable
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/rex_app.h>

#include <rex/cvar.h>
#include <rex/ui/flags.h>
#include <rex/kernel/crt/heap.h>
#include <rex/filesystem.h>
#include <rex/logging/sink.h>
#include <rex/logging.h>
#include <rex/ui/overlay/console_overlay.h>
#include <rex/ui/overlay/debug_overlay.h>
#include <rex/ui/overlay/settings_overlay.h>
#include <rex/graphics/graphics_system.h>
#if REX_HAS_VULKAN
#include <rex/graphics/vulkan/graphics_system.h>
#endif
#if REX_HAS_D3D12
#include <rex/graphics/d3d12/graphics_system.h>
#endif
#include <rex/audio/audio_system.h>
#include <rex/audio/sdl/sdl_audio_system.h>
#include <rex/input/input_system.h>
#include <rex/kernel/init.h>
#include <rex/system.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xlive_web_client.h>
#include <rex/system/xthread.h>
#include <rex/ui/graphics_provider.h>
#include <rex/ui/keybinds.h>
#include <rex/version.h>

#include <fmt/format.h>
#include <imgui.h>

#include <algorithm>
#include <filesystem>
#include <optional>
#include <string_view>
#include <system_error>
#include <vector>

namespace rex {

namespace {

std::filesystem::path XexImageToHostRelativePath(std::string_view xex_image) {
  constexpr std::string_view kGameDevice = "game:\\";
  constexpr std::string_view kDDevice = "d:\\";

  if (xex_image.starts_with(kGameDevice)) {
    xex_image.remove_prefix(kGameDevice.size());
  } else if (xex_image.starts_with(kDDevice)) {
    xex_image.remove_prefix(kDDevice.size());
  }

  std::string host_tail{xex_image};
  std::replace(host_tail.begin(), host_tail.end(), '\\', '/');
  return std::filesystem::path(host_tail);
}

void AddSearchBaseWithParents(std::vector<std::filesystem::path>& search_bases,
                              std::filesystem::path base) {
  if (base.empty()) {
    return;
  }

  constexpr size_t kMaxParentDepth = 8;
  for (size_t i = 0; i < kMaxParentDepth && !base.empty(); ++i) {
    std::error_code ec;
    auto absolute_base = std::filesystem::weakly_canonical(base, ec);
    if (ec) {
      absolute_base = std::filesystem::absolute(base, ec);
    }
    if (!ec) {
      bool already_added = false;
      for (const auto& existing : search_bases) {
        if (existing == absolute_base) {
          already_added = true;
          break;
        }
      }
      if (!already_added) {
        search_bases.push_back(std::move(absolute_base));
      }
    }

    auto parent = base.parent_path();
    if (parent == base) {
      break;
    }
    base = std::move(parent);
  }
}

std::optional<std::filesystem::path> FindGameDataRootInBase(
    const std::filesystem::path& base, const std::filesystem::path& xex_host_tail) {
  std::error_code ec;
  if (!std::filesystem::is_directory(base, ec)) {
    return std::nullopt;
  }

  auto direct_candidate = base / xex_host_tail;
  if (std::filesystem::is_regular_file(direct_candidate, ec)) {
    return base;
  }

  constexpr auto kDirectoryOptions = std::filesystem::directory_options::skip_permission_denied;
  for (std::filesystem::directory_iterator it(base, kDirectoryOptions, ec), end; !ec && it != end;
       it.increment(ec)) {
    std::error_code entry_ec;
    if (!it->is_directory(entry_ec)) {
      continue;
    }

    auto child_candidate = it->path() / xex_host_tail;
    if (std::filesystem::is_regular_file(child_candidate, entry_ec)) {
      return it->path();
    }
  }

  return std::nullopt;
}

std::optional<std::filesystem::path> AutoDetectGameDataRoot(std::string_view xex_image) {
  const auto xex_host_tail = XexImageToHostRelativePath(xex_image);
  if (xex_host_tail.empty()) {
    return std::nullopt;
  }

  std::vector<std::filesystem::path> search_bases;
  std::error_code ec;
  AddSearchBaseWithParents(search_bases, std::filesystem::current_path(ec));
  AddSearchBaseWithParents(search_bases, rex::filesystem::GetExecutableFolder());

  for (const auto& base : search_bases) {
    if (auto detected = FindGameDataRootInBase(base, xex_host_tail)) {
      return detected;
    }
  }

  return std::nullopt;
}

void ApplyPathCvarOverrides(PathConfig& paths) {
  std::string game_data_cvar = REXCVAR_GET(game_data_root);
  if (!game_data_cvar.empty()) {
    paths.game_data_root = game_data_cvar;
  }

  bool user_data_changed = false;
  std::string user_data_cvar = REXCVAR_GET(user_data_root);
  if (!user_data_cvar.empty()) {
    paths.user_data_root = user_data_cvar;
    user_data_changed = true;
  }

  std::string update_data_cvar = REXCVAR_GET(update_data_root);
  if (!update_data_cvar.empty()) {
    paths.update_data_root = update_data_cvar;
  }

  std::string cache_path_cvar = REXCVAR_GET(cache_path);
  if (!cache_path_cvar.empty()) {
    paths.cache_root = cache_path_cvar;
  } else if (user_data_changed) {
    paths.cache_root = paths.user_data_root / "cache";
  }
}

}  // namespace

// --- ReXApp ---

ReXApp::~ReXApp() = default;

ReXApp::ReXApp(ui::WindowedAppContext& ctx, std::string_view name, PPCImageInfo ppc_info,
               std::string_view usage)
    : WindowedApp(ctx, name, usage), ppc_info_(ppc_info) {}

bool ReXApp::OnInitialize() {
  if (!SetupEnvironment())
    return false;
  if (!SetupPresentation())
    return false;

  auto paths = OnFinalizePaths(resolved_defaults_, MakeResumeCallback());
  if (!paths) {
    // Async: consumer will invoke resume when ready. OnInitialize returns
    // true so the event loop keeps pumping (wizard dialogs render).
    return true;
  }

  if (!ConstructRuntime(*paths))
    return false;
  LaunchModule();
  return true;
}

bool ReXApp::SetupEnvironment() {
  auto exe_dir = rex::filesystem::GetExecutableFolder();

  std::filesystem::path game_dir;
  std::string game_data_cvar = REXCVAR_GET(game_data_root);
  if (!game_data_cvar.empty()) {
    game_dir = game_data_cvar;
  }

  // User data: cvar override, or platform user directory
  std::filesystem::path user_dir;
  std::string user_data_cvar = REXCVAR_GET(user_data_root);
  if (!user_data_cvar.empty()) {
    user_dir = user_data_cvar;
  } else {
    user_dir = rex::filesystem::GetUserFolder() / GetName();
  }

  // Update data: cvar override, or empty (opt-in)
  std::filesystem::path update_dir;
  std::string update_data_cvar = REXCVAR_GET(update_data_root);
  if (!update_data_cvar.empty()) {
    update_dir = update_data_cvar;
  }

  // Cache: cvar override, or user_dir/cache
  std::filesystem::path cache_dir;
  std::string cache_path_cvar = REXCVAR_GET(cache_path);
  if (!cache_path_cvar.empty()) {
    cache_dir = cache_path_cvar;
  } else {
    cache_dir = user_dir / "cache";
  }

  PathConfig path_config{game_dir, user_dir, update_dir, cache_dir,
                         exe_dir / (std::string(GetName()) + ".toml")};
  OnConfigurePaths(path_config);
  config_path_ = path_config.config_path;

  // Load config FIRST so log cvars have final values
  const bool had_config_file = std::filesystem::exists(config_path_);
  if (had_config_file) {
    rex::cvar::LoadConfig(config_path_);
    ApplyPathCvarOverrides(path_config);
  }

  game_data_root_ = path_config.game_data_root;
  user_data_root_ = path_config.user_data_root;
  update_data_root_ = path_config.update_data_root;
  cache_root_ = path_config.cache_root;
  resolved_defaults_ = path_config;

  // Late-phase logging
  std::string log_file_cvar = REXCVAR_GET(log_file);
  std::string log_level_str = REXCVAR_GET(log_level);
  if (REXCVAR_GET(log_verbose) && log_level_str == "info")
    log_level_str = "trace";

  auto category_levels = rex::ParseCategoryLevelsFromConfig(config_path_);
  auto log_config = rex::BuildLogConfig(log_file_cvar.empty() ? nullptr : log_file_cvar.c_str(),
                                        log_level_str, category_levels);
  if (log_file_cvar.empty()) {
    log_config.app_name = std::string(GetName());
    log_config.log_dir = (exe_dir / "logs").string();
  }

  rex::InitLogging(log_config);
  rex::RegisterLogLevelCallback();

  log_sink_ = std::make_shared<rex::LogCaptureSink>();
  rex::AddSink(log_sink_);

  OnPostInitLogging();

  if (had_config_file) {
    REXLOG_INFO("Loaded config: {}", config_path_.filename().string());
  } else {
    rex::cvar::SaveConfig(config_path_);
    if (std::filesystem::exists(config_path_)) {
      REXLOG_INFO("Created editable config: {}", config_path_.filename().string());
    }
  }

  REXLOG_INFO("{} starting", GetName());
  if (!game_data_root_.empty()) {
    REXLOG_INFO("  Game directory: {}", game_data_root_.string());
  }
  if (!user_data_root_.empty()) {
    REXLOG_INFO("  User data:      {}", user_data_root_.string());
  }
  if (!update_data_root_.empty()) {
    REXLOG_INFO("  Update data:    {}", update_data_root_.string());
  }
  REXLOG_INFO("  Cache root:     {}", cache_root_.string());

  return true;
}

bool ReXApp::ConstructRuntime(const PathConfig& paths) {
  PathConfig resolved_paths = paths;

  std::string xex_image = "game:\\default.xex";
  OnLoadXexImage(xex_image);

  if (resolved_paths.game_data_root.empty()) {
    if (auto detected_root = AutoDetectGameDataRoot(xex_image)) {
      resolved_paths.game_data_root = *detected_root;
      game_data_root_ = resolved_paths.game_data_root;
      REXLOG_INFO("Auto-detected game directory: {}", resolved_paths.game_data_root.string());
    }
  }

  if (resolved_paths.game_data_root.empty()) {
    auto msg = fmt::format(
        "Game data root was not provided and could not be auto-detected from the current "
        "directory. Run from a directory containing {} or a child folder containing it.",
        XexImageToHostRelativePath(xex_image).string());
    REXLOG_ERROR("{}", msg);
    rex::ShowSimpleMessageBox(rex::SimpleMessageBoxType::Error, msg);
    return false;
  }
  if (!std::filesystem::is_directory(resolved_paths.game_data_root)) {
    auto msg =
        fmt::format("--game_data_root does not exist: {}", resolved_paths.game_data_root.string());
    REXLOG_ERROR("{}", msg);
    rex::ShowSimpleMessageBox(rex::SimpleMessageBoxType::Error, msg);
    return false;
  }

  runtime_ =
      std::make_unique<rex::Runtime>(resolved_paths.game_data_root, resolved_paths.user_data_root,
                                     resolved_paths.update_data_root, resolved_paths.cache_root);
  runtime_->set_app_context(&app_context());

  // Window and ImGui drawer already exist from SetupPresentation; publish them
  // to the runtime before Setup so hooks and native rendering see them.
  if (window_) {
    runtime_->set_display_window(window_.get());
  }
  if (imgui_drawer_) {
    runtime_->set_imgui_drawer(imgui_drawer_.get());
  }

  auto status = runtime_->Setup(ppc_info_, std::move(config_));
  if (XFAILED(status)) {
    REXLOG_ERROR("Runtime setup failed: {:08X}", status);
    return false;
  }

  if (window_ && runtime_->input_system()) {
    static_cast<rex::input::InputSystem*>(runtime_->input_system())->AttachWindow(window_.get());
  }

  if (ppc_info_.register_modules) {
    ppc_info_.register_modules(runtime_->kernel_state());
  }

  if (imgui_drawer_) {
    auto* input_sys = static_cast<rex::input::InputSystem*>(runtime_->input_system());
    if (input_sys) {
      input_sys->SetActiveCallback([this]() {
        if (!debug_overlay_ && !console_overlay_ && !settings_overlay_)
          return true;
        return !imgui_drawer_->GetIO().WantCaptureMouse;
      });
    }
  }

  // Mirrors the game:\ / d:\ -> game_data_root mapping in Runtime::SetupVfs.
  {
    auto xex_host = resolved_paths.game_data_root / XexImageToHostRelativePath(xex_image);
    if (!std::filesystem::is_regular_file(xex_host)) {
      auto msg = fmt::format("Entrypoint XEX not found: {}", xex_host.string());
      REXLOG_ERROR("{}", msg);
      rex::ShowSimpleMessageBox(rex::SimpleMessageBoxType::Error, msg);
      return false;
    }
  }

  status = runtime_->LoadXexImage(xex_image);
  if (XFAILED(status)) {
    auto msg = fmt::format("Failed to load XEX ({}): {:08X}", xex_image, status);
    REXLOG_ERROR("{}", msg);
    rex::ShowSimpleMessageBox(rex::SimpleMessageBoxType::Error, msg);
    return false;
  }

  OnPostLoadXexImage();

  if (rex::cvar::Query<bool>("xlive_web_probe_on_startup")) {
    rex::system::XLiveWebClient::Get().LogStatus(runtime_->kernel_state(), true);
  }

  if (ppc_info_.rexcrt_heap) {
    if (!rex::kernel::crt::InitHeap(REXCVAR_GET(rexcrt_heap_size_mb), runtime_->memory())) {
      REXLOG_ERROR("Failed to initialize rexcrt heap");
      return false;
    }
  }

  OnPostSetup();

  return true;
}

bool ReXApp::SetupPresentation() {
#if REX_HAS_D3D12
  config_.graphics = REX_GRAPHICS_BACKEND(rex::graphics::d3d12::D3D12GraphicsSystem);
#elif REX_HAS_VULKAN
  config_.graphics = REX_GRAPHICS_BACKEND(rex::graphics::vulkan::VulkanGraphicsSystem);
#endif
  config_.audio_factory = REX_AUDIO_BACKEND(rex::audio::sdl::SDLAudioSystem);
  config_.input_factory = REX_INPUT_BACKEND(rex::input::CreateDefaultInputSystem);
  config_.kernel_init = rex::kernel::InitializeKernel;

  OnPreSetup(config_);

  if (config_.graphics) {
    X_STATUS status = config_.graphics->SetupPresentation(&app_context());
    if (XFAILED(status)) {
      REXLOG_ERROR("Graphics presentation setup failed: {:08X}", status);
      return false;
    }
  }

  // Create window
  window_ = rex::ui::Window::Create(app_context(), GetName(), 1280, 720);
  if (!window_) {
    REXLOG_ERROR("Failed to create window");
    return false;
  }

  // Set window title with SDK build stamp
  std::string title = std::string(GetName()) + " " + REXGLUE_BUILD_TITLE;
  window_->SetTitle(title);

  window_->AddListener(this);
  window_->AddInputListener(this, 0);

  if (REXCVAR_GET(fullscreen)) {
    window_->SetFullscreen(true);
  }
  window_->Open();

  auto* graphics_system = static_cast<rex::graphics::GraphicsSystem*>(config_.graphics.get());
  if (graphics_system && graphics_system->presenter()) {
    auto* presenter = graphics_system->presenter();
    auto* provider = graphics_system->provider();
    if (provider) {
      immediate_drawer_ = provider->CreateImmediateDrawer();
      if (immediate_drawer_) {
        immediate_drawer_->SetPresenter(presenter);
        imgui_drawer_ = std::make_unique<rex::ui::ImGuiDrawer>(
            window_.get(), 64, [this](ImFontAtlas* atlas) { OnConfigureFonts(atlas); });
        imgui_drawer_->SetPresenterAndImmediateDrawer(presenter, immediate_drawer_.get());
        rex::ui::RegisterBind("bind_debug_overlay", "F3", "Toggle debug overlay", [this] {
          if (debug_overlay_) {
            debug_overlay_.reset();
          } else {
            debug_overlay_ = std::make_unique<ui::DebugOverlayDialog>(imgui_drawer_.get(),
                                                                      frame_stats_provider_);
          }
        });
        rex::ui::RegisterBind("bind_console", "F10", "Toggle console overlay",
                              [this] { ToggleConsoleOverlay(); });
        rex::ui::RegisterBind("bind_settings", "F4", "Toggle settings overlay", [this] {
          if (settings_overlay_) {
            settings_overlay_.reset();
          } else {
            settings_overlay_ = std::make_unique<ui::SettingsDialog>(
                imgui_drawer_.get(), config_path_,
                [this](std::string_view name, std::string_view value) {
                  if (name == "fullscreen" && window_) {
                    bool fullscreen = value == "true";
                    REXLOG_INFO("Scheduling fullscreen {}", fullscreen ? "on" : "off");
                    app_context().CallInUIThreadDeferred([this, fullscreen]() {
                      if (!window_) {
                        return;
                      }
                      if (window_->IsFullscreen() == fullscreen) {
                        return;
                      }
                      REXLOG_INFO("Applying fullscreen {}", fullscreen ? "on" : "off");
                      window_->SetFullscreen(fullscreen);
                    });
                  }
                });
          }
        });

        OnCreateDialogs(imgui_drawer_.get());
      }
    }
    window_->SetPresenter(presenter);
  }

  return true;
}

void ReXApp::LaunchModule() {
  app_context().CallInUIThreadDeferred([this]() {
    REXLOG_INFO("LaunchModule: deferred launch begin");
    OnPreLaunchModule();

    auto main_thread = runtime_->PrepareModuleLaunch();
    if (!main_thread) {
      REXLOG_ERROR("Failed to launch module");
      app_context().QuitFromUIThread();
      return;
    }

    auto* graphics_system =
        static_cast<rex::graphics::GraphicsSystem*>(runtime_->graphics_system());
    if (graphics_system && !runtime_->cache_root().empty()) {
      uint32_t title_id = runtime_->kernel_state()->title_id();
      if (title_id != 0) {
        REXLOG_INFO("Initializing shader storage for title {:08X}...", title_id);
        graphics_system->InitializeShaderStorage(runtime_->cache_root(), title_id, true);
        REXLOG_INFO("LaunchModule: shader storage initialized");
      }
    }

    OnPostLaunchModule(main_thread.get());
    REXLOG_INFO("LaunchModule: resuming main guest thread");
    main_thread->Resume();
    REXLOG_INFO("LaunchModule: main guest thread resumed");

    module_thread_ = std::thread([this, main_thread = std::move(main_thread)]() mutable {
      REXLOG_INFO("LaunchModule: wait thread started");
      main_thread->Wait(0, 0, 0, nullptr);
      OnGuestThreadExit(main_thread.get());
      REXLOG_INFO("Execution complete");
      if (!shutting_down_.load(std::memory_order_acquire)) {
        app_context().CallInUIThread([this]() { app_context().QuitFromUIThread(); });
      }
    });
  });
}

std::function<void(PathConfig)> ReXApp::MakeResumeCallback() {
  return [this](PathConfig paths) {
    if (shutting_down_.load(std::memory_order_acquire))
      return;
    if (!ConstructRuntime(std::move(paths))) {
      app_context().QuitFromUIThread();
      return;
    }
    LaunchModule();
  };
}

void ReXApp::ToggleConsoleOverlay() {
  if (!imgui_drawer_) {
    return;
  }

  if (console_overlay_) {
    console_overlay_.reset();
  } else {
    console_overlay_ = std::make_unique<ui::ConsoleDialog>(imgui_drawer_.get(), log_sink_);
  }
}

void ReXApp::OnKeyDown(ui::KeyEvent& e) {
  if (rex::ui::ProcessKeyEvent(e)) {
    return;
  }

  if (e.virtual_key() == ui::VirtualKey::kF10) {
    ToggleConsoleOverlay();
    e.set_handled(true);
  }
}

void ReXApp::OnClosing(ui::UIEvent& e) {
  (void)e;
  REXLOG_INFO("Window closing, shutting down...");
  shutting_down_.store(true, std::memory_order_release);
  if (runtime_ && runtime_->kernel_state()) {
    runtime_->kernel_state()->TerminateTitle();
  }
  app_context().QuitFromUIThread();
}

void ReXApp::OnDestroy() {
  // Notify subclass before cleanup
  OnShutdown();

  // Unregister overlay keybinds before destroying dialogs
  rex::ui::UnregisterBind("bind_debug_overlay");
  rex::ui::UnregisterBind("bind_console");
  rex::ui::UnregisterBind("bind_settings");

  // ImGui cleanup (reverse of setup)
  settings_overlay_.reset();
  console_overlay_.reset();
  debug_overlay_.reset();
  if (imgui_drawer_) {
    imgui_drawer_->SetPresenterAndImmediateDrawer(nullptr, nullptr);
    imgui_drawer_.reset();
  }
  if (immediate_drawer_) {
    immediate_drawer_->SetPresenter(nullptr);
    immediate_drawer_.reset();
  }
  if (runtime_) {
    runtime_->set_display_window(nullptr);
    runtime_->set_imgui_drawer(nullptr);
  }
  // Window/runtime cleanup
  if (window_) {
    window_->SetPresenter(nullptr);
  }
  if (module_thread_.joinable()) {
    module_thread_.join();
  }
  if (window_) {
    window_->RemoveInputListener(this);
    window_->RemoveListener(this);
  }
  // Input drivers unregister from the window in their destructors.
  runtime_.reset();
  window_.reset();
}

void ReXApp::SetGuestFrameStats(ui::DebugOverlayDialog::FrameStatsProvider provider) {
  frame_stats_provider_ = provider;
  if (debug_overlay_) {
    debug_overlay_->SetStatsProvider(provider);
  }
}

}  // namespace rex
