#include "d3d9_overlay.h"

#ifdef _WIN32

#include <rex/cvar.h>
#include <rex/logging/macros.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <set>
#include <string_view>
#include <vector>

#include <imgui.h>

#include "backends/imgui_impl_dx9.h"
#include "backends/imgui_impl_win32.h"

// Defined by the renderer / resource tracker. REXCVAR_GET yields a reference to the storage,
// so the widgets below can bind straight to it.
REXCVAR_DECLARE(bool, nx1_d3d9_mips);
REXCVAR_DECLARE(bool, nx1_d3d9_bc_mips);
REXCVAR_DECLARE(bool, nx1_d3d9_profile);
REXCVAR_DECLARE(uint32_t, nx1_d3d9_dbg_mipsrc);
REXCVAR_DECLARE(uint32_t, nx1_d3d9_dbg_lod);
REXCVAR_DECLARE(uint32_t, nx1_d3d9_dbg_mipfill);
REXCVAR_DECLARE(bool, nx1_d3d9_fast_detile);
REXCVAR_DECLARE(bool, nx1_d3d9_commit_textures);
REXCVAR_DECLARE(bool, nx1_d3d9_texture_mirror);

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hwnd, UINT msg, WPARAM wparam,
                                                             LPARAM lparam);

namespace nx1::d3d9 {

namespace {
/// Toggles the overlay. F4 to match the SDK's own "bind_settings" key, so the muscle memory
/// carries over -- there is no conflict, because that overlay renders through the D3D12
/// ImmediateDrawer into a swapchain our PresentEx covers, so its F4 toggles something that
/// can never be seen anyway.
constexpr int kToggleKey = VK_F4;
}  // namespace

Overlay& Overlay::Get() {
  static Overlay instance;
  return instance;
}

bool Overlay::Initialize(IDirect3DDevice9Ex* device, HWND hwnd) {
  if (initialized_) {
    return true;
  }
  if (!device || !hwnd) {
    return false;
  }
  device_ = device;
  hwnd_ = hwnd;

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  // No ini/log files: this is a debug overlay inside someone else's process, and silently
  // dropping an imgui.ini next to the exe is rude.
  io.IniFilename = nullptr;
  io.LogFilename = nullptr;
  ImGui::StyleColorsDark();

  if (!ImGui_ImplWin32_Init(hwnd)) {
    REXGPU_ERROR("nx1_d3d9: overlay ImGui_ImplWin32_Init failed");
    ImGui::DestroyContext();
    return false;
  }
  if (!ImGui_ImplDX9_Init(device)) {
    REXGPU_ERROR("nx1_d3d9: overlay ImGui_ImplDX9_Init failed");
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    return false;
  }
  device_objects_valid_ = true;

  // Subclass rather than replace: the window belongs to the SDK, and its handler still has to
  // see everything the overlay does not consume.
  prev_wndproc_ = reinterpret_cast<WNDPROC>(
      SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&Overlay::WndProcThunk)));
  if (!prev_wndproc_) {
    REXGPU_WARN("nx1_d3d9: overlay could not subclass the window; input will not reach it");
  }

  initialized_ = true;
  REXGPU_INFO("nx1_d3d9: overlay ready (F4 toggles)");
  return true;
}

void Overlay::Shutdown() {
  if (!initialized_) {
    return;
  }
  if (prev_wndproc_ && hwnd_) {
    SetWindowLongPtrW(hwnd_, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(prev_wndproc_));
    prev_wndproc_ = nullptr;
  }
  if (device_objects_valid_) {
    ImGui_ImplDX9_Shutdown();
    device_objects_valid_ = false;
  }
  ImGui_ImplWin32_Shutdown();
  ImGui::DestroyContext();
  device_ = nullptr;
  hwnd_ = nullptr;
  initialized_ = false;
}

void Overlay::OnDeviceLost() {
  if (initialized_ && device_objects_valid_) {
    ImGui_ImplDX9_InvalidateDeviceObjects();
    device_objects_valid_ = false;
  }
}

void Overlay::OnDeviceReset() {
  if (initialized_ && !device_objects_valid_) {
    ImGui_ImplDX9_CreateDeviceObjects();
    device_objects_valid_ = true;
  }
}

LRESULT CALLBACK Overlay::WndProcThunk(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
  Overlay& self = Get();

  if (msg == WM_KEYDOWN && int(wparam) == kToggleKey) {
    self.visible_ = !self.visible_;
    return 0;  // never let the toggle itself reach the game
  }

  if (self.initialized_ && self.visible_) {
    ImGui_ImplWin32_WndProcHandler(hwnd, msg, wparam, lparam);
    // Swallow only what ImGui actually wants. Anything else still reaches the game, so the
    // overlay being open does not freeze the rest of the input.
    const ImGuiIO& io = ImGui::GetIO();
    const bool mouse_msg = (msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST) || msg == WM_MOUSEHOVER;
    const bool key_msg = (msg >= WM_KEYFIRST && msg <= WM_KEYLAST);
    if ((mouse_msg && io.WantCaptureMouse) || (key_msg && io.WantCaptureKeyboard)) {
      return 0;
    }
  }

  if (self.prev_wndproc_) {
    return CallWindowProcW(self.prev_wndproc_, hwnd, msg, wparam, lparam);
  }
  return DefWindowProcW(hwnd, msg, wparam, lparam);
}

namespace {

const char* LifecycleBadge(rex::cvar::Lifecycle lc) {
  switch (lc) {
    case rex::cvar::Lifecycle::kHotReload:
      return "live";
    case rex::cvar::Lifecycle::kRequiresRestart:
      return "restart";
    case rex::cvar::Lifecycle::kInitOnly:
      return "init-only";
  }
  return "?";
}

ImVec4 LifecycleColor(rex::cvar::Lifecycle lc) {
  switch (lc) {
    case rex::cvar::Lifecycle::kHotReload:
      return ImVec4(0.45f, 0.85f, 0.45f, 1.0f);
    case rex::cvar::Lifecycle::kRequiresRestart:
      return ImVec4(0.95f, 0.80f, 0.35f, 1.0f);
    case rex::cvar::Lifecycle::kInitOnly:
      return ImVec4(0.70f, 0.70f, 0.70f, 1.0f);
  }
  return ImVec4(1, 1, 1, 1);
}

bool ContainsNoCase(std::string_view haystack, std::string_view needle) {
  if (needle.empty()) {
    return true;
  }
  const auto it =
      std::search(haystack.begin(), haystack.end(), needle.begin(), needle.end(),
                  [](char a, char b) {
                    return std::tolower(static_cast<unsigned char>(a)) ==
                           std::tolower(static_cast<unsigned char>(b));
                  });
  return it != haystack.end();
}

/// Where "Save to config" writes. The SDK's dialog is handed this path by the app; deriving
/// the same one from the executable keeps both writing the same file.
std::filesystem::path ConfigPath() {
  wchar_t buf[MAX_PATH] = {};
  const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
  if (!n) {
    return std::filesystem::path("nx1_mp.toml");
  }
  const std::filesystem::path exe(buf, buf + n);
  return exe.parent_path() / (exe.stem().wstring() + L".toml");
}

}  // namespace

void Overlay::DrawSettings() {
  auto& registry = rex::cvar::GetRegistry();

  ImGui::SetNextItemWidth(220.0f);
  ImGui::InputTextWithHint("##search", "search cvars", search_, sizeof(search_));
  ImGui::SameLine();
  if (ImGui::Button("Clear")) {
    search_[0] = '\0';
  }

  std::set<std::string> categories;
  for (const auto& e : registry) {
    categories.insert(e.category.empty() ? std::string("(uncategorised)") : e.category);
  }

  ImGui::BeginChild("##cats", ImVec2(170, 340), true);
  if (ImGui::Selectable("All", selected_category_.empty())) {
    selected_category_.clear();
  }
  for (const auto& c : categories) {
    if (ImGui::Selectable(c.c_str(), selected_category_ == c)) {
      selected_category_ = c;
    }
  }
  ImGui::EndChild();

  ImGui::SameLine();
  ImGui::BeginChild("##entries", ImVec2(0, 340), true);
  for (auto& e : registry) {
    const std::string cat = e.category.empty() ? std::string("(uncategorised)") : e.category;
    if (!selected_category_.empty() && cat != selected_category_) {
      continue;
    }
    if (!ContainsNoCase(e.name, search_) && !ContainsNoCase(e.description, search_)) {
      continue;
    }

    ImGui::PushID(e.name.c_str());
    // Init-only flags are shown but not editable: changing one at runtime is a silent no-op,
    // which is worse to discover than a greyed-out widget.
    const bool read_only = e.lifecycle == rex::cvar::Lifecycle::kInitOnly;
    ImGui::BeginDisabled(read_only);

    const std::string value = e.getter ? e.getter() : std::string();
    if (e.type == rex::cvar::FlagType::Command) {
      if (ImGui::Button("Run") && e.command_callback) {
        e.command_callback();
      }
      ImGui::SameLine();
      ImGui::TextUnformatted(e.name.c_str());
    } else if (e.type == rex::cvar::FlagType::Boolean) {
      bool v = value == "true" || value == "1";
      if (ImGui::Checkbox(e.name.c_str(), &v)) {
        rex::cvar::SetFlagByName(e.name, v ? "true" : "false");
      }
    } else if (e.constraints.HasAllowedValues()) {
      int current = 0;
      std::vector<const char*> items;
      items.reserve(e.constraints.allowed_values.size());
      for (size_t i = 0; i < e.constraints.allowed_values.size(); ++i) {
        items.push_back(e.constraints.allowed_values[i].c_str());
        if (e.constraints.allowed_values[i] == value) {
          current = static_cast<int>(i);
        }
      }
      ImGui::SetNextItemWidth(200.0f);
      if (ImGui::Combo(e.name.c_str(), &current, items.data(), static_cast<int>(items.size()))) {
        rex::cvar::SetFlagByName(e.name, e.constraints.allowed_values[size_t(current)]);
      }
    } else {
      char buf[256] = {};
      std::snprintf(buf, sizeof(buf), "%s", value.c_str());
      ImGui::SetNextItemWidth(200.0f);
      if (ImGui::InputText(e.name.c_str(), buf, sizeof(buf),
                           ImGuiInputTextFlags_EnterReturnsTrue)) {
        // Goes through SetFlagByName so the registry's own validation and range constraints
        // apply, exactly as they do for a value read from the config file.
        if (!rex::cvar::SetFlagByName(e.name, buf)) {
          REXGPU_WARN("nx1_d3d9: overlay rejected '{}' for cvar {}", buf, e.name);
        }
      }
    }

    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextColored(LifecycleColor(e.lifecycle), "[%s]", LifecycleBadge(e.lifecycle));
    if (ImGui::IsItemHovered() && !e.description.empty()) {
      ImGui::SetTooltip("%s\n\ndefault: %s", e.description.c_str(), e.default_value.c_str());
    }
    ImGui::PopID();
  }
  ImGui::EndChild();

  const std::filesystem::path cfg = ConfigPath();
  if (ImGui::Button("Save to config")) {
    rex::cvar::SaveConfig(cfg);
    REXGPU_INFO("nx1_d3d9: overlay wrote {}", cfg.string());
  }
  ImGui::SameLine();
  ImGui::TextDisabled("(%s)", cfg.filename().string().c_str());
}

void Overlay::DrawPanels() {
  ImGui::SetNextWindowSize(ImVec2(780, 660), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowPos(ImVec2(20, 20), ImGuiCond_FirstUseEver);
  if (!ImGui::Begin("NX1 D3D9 renderer")) {
    ImGui::End();
    return;
  }

  const ImGuiIO& io = ImGui::GetIO();
  ImGui::Text("%.1f FPS  (%.2f ms/frame)", io.Framerate, 1000.0f / io.Framerate);
  ImGui::Separator();

  if (ImGui::CollapsingHeader("Diagnostics", ImGuiTreeNodeFlags_DefaultOpen)) {
    ImGui::Checkbox("Frame profiler (writes PROF/ lines to the log)",
                    &REXCVAR_GET(nx1_d3d9_profile));

    ImGui::Spacing();
    ImGui::TextUnformatted("Mip chains");
    ImGui::Checkbox("Generate mips at all", &REXCVAR_GET(nx1_d3d9_mips));
    ImGui::Checkbox("CPU-build block-compressed mips", &REXCVAR_GET(nx1_d3d9_bc_mips));
    ImGui::Checkbox("Read textures from the CPU mirror snapshot",
                    &REXCVAR_GET(nx1_d3d9_texture_mirror));
    ImGui::TextDisabled("Off = read live guest memory like the reference backend does.");
    ImGui::TextDisabled("The mirror holds a page from first touch, so anything the guest");
    ImGui::TextDisabled("writes afterwards never reaches us.");

    ImGui::Spacing();
    ImGui::Checkbox("Freeze texture decodes after 32 clean frames",
                    &REXCVAR_GET(nx1_d3d9_commit_textures));
    ImGui::TextDisabled("Off = keep honouring guest writes, so a texture decoded before its");
    ImGui::TextDisabled("data finished streaming can still correct itself.");

    ImGui::Spacing();
    ImGui::Checkbox("Fast (table) detile path", &REXCVAR_GET(nx1_d3d9_fast_detile));
    ImGui::TextDisabled("Off = per-block reference addressing. Costs nothing measurable;");
    ImGui::TextDisabled("turn off first if anything tile-shaped looks wrong.");

    ImGui::Spacing();
    ImGui::TextUnformatted("Mip chain test");
    int mipfill = int(REXCVAR_GET(nx1_d3d9_dbg_mipfill));
    ImGui::RadioButton("off##mipfill", &mipfill, 0);
    ImGui::SameLine();
    ImGui::RadioButton("flat colours", &mipfill, 1);
    ImGui::SameLine();
    ImGui::RadioButton("gradient source", &mipfill, 2);
    REXCVAR_SET(nx1_d3d9_dbg_mipfill, uint32_t(mipfill));
    ImGui::TextDisabled("flat: tests the plumbing (proven OK).");
    ImGui::TextDisabled("gradient: tests filter+encoder on varying data, bypassing our decoder.");
    ImGui::TextDisabled("Unticking the second leaves BC textures unmipped while keeping the");
    ImGui::TextDisabled("driver's chains -- it isolates our encoder from the driver's.");

    ImGui::Spacing();
    ImGui::TextUnformatted("Paint textures white by mip source");
    int mipsrc = int(REXCVAR_GET(nx1_d3d9_dbg_mipsrc));
    ImGui::RadioButton("off##mipsrc", &mipsrc, 0);
    ImGui::SameLine();
    ImGui::RadioButton("cpu-built", &mipsrc, 1);
    ImGui::SameLine();
    ImGui::RadioButton("driver", &mipsrc, 2);
    ImGui::SameLine();
    ImGui::RadioButton("none", &mipsrc, 3);
    REXCVAR_SET(nx1_d3d9_dbg_mipsrc, uint32_t(mipsrc));

    ImGui::Spacing();
    ImGui::TextUnformatted("Paint textures white by LOD-substitution branch");
    int dbglod = int(REXCVAR_GET(nx1_d3d9_dbg_lod));
    ImGui::RadioButton("off##lod", &dbglod, 0);
    ImGui::SameLine();
    ImGui::RadioButton("substitute", &dbglod, 1);
    ImGui::SameLine();
    ImGui::RadioButton("equal", &dbglod, 2);
    ImGui::SameLine();
    ImGui::RadioButton("adopt", &dbglod, 3);
    ImGui::SameLine();
    ImGui::RadioButton("equal-diff", &dbglod, 4);
    REXCVAR_SET(nx1_d3d9_dbg_lod, uint32_t(dbglod));
  }

  if (ImGui::CollapsingHeader("Settings (all cvars, save to TOML)")) {
    DrawSettings();
  }

  ImGui::Separator();
  ImGui::TextDisabled("F4 closes this.");
  ImGui::End();
}

void Overlay::Render() {
  if (!initialized_ || !visible_ || !device_objects_valid_) {
    return;
  }
  ImGui_ImplDX9_NewFrame();
  ImGui_ImplWin32_NewFrame();
  ImGui::NewFrame();
  DrawPanels();
  ImGui::EndFrame();
  ImGui::Render();
  // imgui_impl_dx9 brackets this in a D3D9 state block, so nothing it sets leaks into the
  // next frame's draws.
  ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
}

}  // namespace nx1::d3d9

#endif  // _WIN32
