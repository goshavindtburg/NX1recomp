#include "d3d9_overlay.h"

#include <windowsx.h>  // GET_X_LPARAM / GET_Y_LPARAM

#include "d3d9_renderer.h"

#ifdef _WIN32

#include <rex/cvar.h>
#include <rex/logging/macros.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <set>
#include <string_view>
#include <vector>

#include <imgui.h>

#include "backends/imgui_impl_dx9.h"
#include "backends/imgui_impl_win32.h"

#include "d3d9_resources.h"

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
REXCVAR_DECLARE(uint32_t, nx1_d3d9_dbg_texdump);
REXCVAR_DECLARE(uint32_t, nx1_d3d9_dbg_hide_matched);
REXCVAR_DECLARE(uint32_t, nx1_d3d9_dbg_highlight_ps);
REXCVAR_DECLARE(uint32_t, nx1_d3d9_dbg_solo_ps);
REXCVAR_DECLARE(uint32_t, nx1_d3d9_dbg_blend_ps);
REXCVAR_DECLARE(uint32_t, nx1_d3d9_dbg_pick_size);
REXCVAR_DECLARE(int32_t, nx1_d3d9_dbg_pick_ox);
REXCVAR_DECLARE(int32_t, nx1_d3d9_dbg_pick_oy);
REXCVAR_DECLARE(uint32_t, nx1_d3d9_dbg_track_addr);

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hwnd, UINT msg, WPARAM wparam,
                                                             LPARAM lparam);

namespace nx1::d3d9 {

namespace {
/// Toggles the overlay. F4 to match the SDK's own "bind_settings" key, so the muscle memory
/// carries over -- there is no conflict, because that overlay renders through the D3D12
/// ImmediateDrawer into a swapchain our PresentEx covers, so its F4 toggles something that
/// can never be seen anyway.
constexpr int kToggleKey = VK_F4;
// F3 matches the ReXGlue D3D12 side, where F3 is already the debug key.
constexpr int kPickerKey = VK_F3;
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
  if (msg == WM_KEYDOWN && int(wparam) == kPickerKey) {
    self.picker_visible_ = !self.picker_visible_;
    return 0;
  }

  // While the picker is open the game must not see the mouse AT ALL. It re-hides the cursor and
  // re-centres it for mouselook in response to these messages, so merely calling ShowCursor
  // alongside it just fights a battle that restarts every frame. Swallowing them stops the
  // recentring at its source, which is what actually frees the pointer.
  if (self.initialized_ && self.picker_visible_) {
    // WM_INPUT (raw mouse deltas) is NOT in the WM_MOUSEFIRST..WM_MOUSELAST range, so intercepting
    // that range alone left mouselook fully working -- the game reads its deltas from raw input
    // and never needed WM_MOUSEMOVE at all. Swallowing WM_INPUT is what actually stops the camera.
    // The OS cursor still moves normally from here, since raw input does not drive it.
    const bool mouse_msg = (msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST) || msg == WM_MOUSEHOVER ||
                           msg == WM_MOUSELEAVE || msg == WM_SETCURSOR || msg == WM_INPUT;
    if (mouse_msg) {
      if (msg == WM_INPUT) {
        return 0;  // deltas dropped: no mouselook while picking
      }
      if (msg == WM_SETCURSOR) {
        SetCursor(LoadCursor(nullptr, IDC_ARROW));
        return TRUE;  // claim it, or the game resets the cursor to its own (hidden) one
      }
      ImGui_ImplWin32_WndProcHandler(hwnd, msg, wparam, lparam);
      if (msg == WM_LBUTTONDOWN && !ImGui::GetIO().WantCaptureMouse) {
        Renderer::Get().RequestPick(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam));
        self.pick_awaiting_ = true;
      }
      return 0;  // never reaches the game: no mouselook, no recentre, no re-hide
    }
  }

  if (self.initialized_ && (self.visible_ || self.picker_visible_)) {
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
      ImGui::SetNextItemWidth(260.0f);
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

void Overlay::DrawPicker() {
  // The game re-hides and re-centres the cursor every frame, so this has to be re-asserted
  // while the picker is open rather than done once on the toggle.
  // Safe to manage the cursor now: with mouse messages swallowed the game no longer re-hides it.
  if (picker_visible_) {
    ClipCursor(nullptr);  // re-asserted per frame: the game re-clips from its own update loop
    if (!cursor_released_) {
      while (ShowCursor(TRUE) < 0) {
      }
      cursor_released_ = true;
    }
  }
  if (!picker_visible_ && cursor_released_) {
    while (ShowCursor(FALSE) >= 0) {
    }
    cursor_released_ = false;
  }
  if (!picker_visible_) {
    return;
  }

  ImGui::SetNextWindowSize(ImVec2(620, 300), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowPos(ImVec2(20, 700), ImGuiCond_FirstUseEver);
  if (!ImGui::Begin("Shader picker (F3)", &picker_visible_)) {
    ImGui::End();
    return;
  }

  ImGui::TextWrapped(
      "CLICK any surface to identify the shader that drew it. The nearest hit is highlighted in "
      "WIREFRAME automatically -- if that is not the surface you wanted, 'highlight' the other "
      "rows until the right one lights up. Nearest means nearest IN DEPTH, so a decal or overlay "
      "in front of your target will win.");
  ImGui::SetNextItemWidth(120.0f);
  int box = int(REXCVAR_GET(nx1_d3d9_dbg_pick_size));
  if (ImGui::InputInt("box size (px)", &box)) {
    REXCVAR_SET(nx1_d3d9_dbg_pick_size, uint32_t(box < 1 ? 1 : box));
  }
  ImGui::SameLine();
  ImGui::TextDisabled("(1-2 to separate touching surfaces)");

  ImGui::Separator();

  Renderer& r = Renderer::Get();
  if (pick_awaiting_ && !r.pick_pending()) {
    pick_awaiting_ = false;
    if (!r.pick_results().empty()) {
      REXCVAR_SET(nx1_d3d9_dbg_highlight_ps, r.pick_results().back().ps_object);
    }
  }
  if (pick_awaiting_) {
    ImGui::TextUnformatted("picking...");
  }

  const auto& hits = r.pick_results();
  if (hits.empty()) {
    ImGui::TextDisabled("No pick yet.");
  } else {
    // Only the frontmost matters almost always -- it is the surface you aimed at. The rest is
    // everything the pixel was drawn through (sky, terrain, decals, post) and is noise unless
    // you are specifically chasing overdraw, so it is collapsed by default.
    static bool show_all = true;
    ImGui::Text("%zu draw(s) covered the pixel", hits.size());
    ImGui::SameLine();
    ImGui::Checkbox("show all", &show_all);
    ImGui::TextDisabled("Nearest first. Deeper entries are what the surface was drawn over.");
    ImGui::Spacing();
    // FRONTMOST FIRST. It was last, which put the one entry that matters below the fold as soon
    // as the list scrolled -- so the visible rows were the skybox and whatever was drawn behind,
    // and 'hide this' on those looked like the picker returning the wrong material.
    const size_t shown = show_all ? hits.size() : 1;
    for (size_t n = 0; n < shown; ++n) {
      const size_t i = hits.size() - 1 - n;
      const auto& h = hits[i];
      const bool front = (n == 0);
      ImGui::PushStyleColor(ImGuiCol_Text, front ? ImVec4(0.4f, 1.0f, 0.4f, 1.0f)
                                                 : ImVec4(0.7f, 0.7f, 0.7f, 1.0f));
      ImGui::Text("%s draw #%u  px=%u  ps=%08X  vs=%08X", front ? "->" : "  ", h.draw_index,
                  h.pixels, h.ps_object, h.vs_object);
      ImGui::PopStyleColor();
      ImGui::SameLine();
      char buf[160];
      std::snprintf(buf, sizeof(buf), "ucode 0x%016llX",
                    static_cast<unsigned long long>(h.ps_hash));
      ImGui::TextDisabled("%s", buf);
      // Everything needed to act on the result, without hand-converting hex -- which cost a
      // round earlier: dbg_oc0_lo32 is DECIMAL and the dump file is named in hex.
      ImGui::PushID(int(i));
      std::snprintf(buf, sizeof(buf), "%u", uint32_t(h.ps_hash & 0xFFFFFFFFull));
      if (ImGui::Button("copy dbg_oc0_lo32")) {
        ImGui::SetClipboardText(buf);
      }
      ImGui::SameLine();
      ImGui::TextUnformatted(buf);
      ImGui::SameLine();
      std::snprintf(buf, sizeof(buf), "shader_%016llX.ucode.frag",
                    static_cast<unsigned long long>(h.ps_hash));
      if (ImGui::Button("copy dump name")) {
        ImGui::SetClipboardText(buf);
      }
      // VERIFY the pick before acting on it. "Frontmost" is a heuristic -- a decal, a
      // transparent overlay or a neighbouring surface inside the box can all end up last -- and
      // probing the wrong material reads as "the probe changed nothing", which is
      // indistinguishable from a real negative result.
      // Highlight is the primary confirmation: wireframe shows WHICH surface this row is
      // without removing it, so the scene stays readable while you step through the hits.
      ImGui::SameLine();
      const bool lit = REXCVAR_GET(nx1_d3d9_dbg_highlight_ps) == h.ps_object;
      if (ImGui::Button(lit ? "unhighlight" : "highlight")) {
        REXCVAR_SET(nx1_d3d9_dbg_highlight_ps, lit ? 0u : h.ps_object);
      }
      ImGui::SameLine();
      const bool solo = REXCVAR_GET(nx1_d3d9_dbg_solo_ps) == h.ps_object;
      if (ImGui::Button(solo ? "unsolo" : "SOLO")) {
        REXCVAR_SET(nx1_d3d9_dbg_solo_ps, solo ? 0u : h.ps_object);
      }
      if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Draw ONLY this material. The clearest way to see which surface a hit "
                          "is -- everything else vanishes.");
      }
      ImGui::SameLine();
      const bool hidden = REXCVAR_GET(nx1_d3d9_dbg_hide_matched) != 0 &&
                          REXCVAR_GET(nx1_d3d9_dbg_blend_ps) == h.ps_object;
      if (ImGui::Button(hidden ? "show again" : "hide this")) {
        if (hidden) {
          REXCVAR_SET(nx1_d3d9_dbg_hide_matched, 0u);
          REXCVAR_SET(nx1_d3d9_dbg_blend_ps, 0u);
        } else {
          REXCVAR_SET(nx1_d3d9_dbg_blend_ps, h.ps_object);
          REXCVAR_SET(nx1_d3d9_dbg_hide_matched, 1u);
        }
      }
      if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Hides every draw using this material. If the surface you aimed at "
                          "does not disappear, this is not its shader.");
      }
      ImGui::PopID();
      ImGui::Separator();
    }
  }
  ImGui::End();
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
    // Dedicated hex entry: the generic cvar editor parses a uint32 as DECIMAL, so typing a
    // guest address there silently tracks the wrong memory.
    ImGui::TextUnformatted("Track texture address (hex) -- traces it and paints it white");
    static char track_buf[16] = {};
    ImGui::SetNextItemWidth(140.0f);
    if (ImGui::InputText("##trackaddr", track_buf, sizeof(track_buf),
                         ImGuiInputTextFlags_CharsHexadecimal |
                             ImGuiInputTextFlags_EnterReturnsTrue)) {
      REXCVAR_SET(nx1_d3d9_dbg_track_addr, uint32_t(std::strtoul(track_buf, nullptr, 16)));
    }
    ImGui::SameLine();
    if (ImGui::Button("Set")) {
      REXCVAR_SET(nx1_d3d9_dbg_track_addr, uint32_t(std::strtoul(track_buf, nullptr, 16)));
    }
    ImGui::SameLine();
    if (ImGui::Button("Clear##track")) {
      track_buf[0] = 0;
      REXCVAR_SET(nx1_d3d9_dbg_track_addr, 0u);
    }
    // Muzzle-flash candidates: the newcomers bound exactly once per shot across 12 shots.
    ImGui::TextDisabled("flash candidates (1 frame per shot):");
    struct Candidate {
      const char* label;
      uint32_t addr;
    };
    static constexpr Candidate kCandidates[] = {
        {"0EE3E000 128x128", 0x0EE3E000u},
        {"1022E000 512x256", 0x1022E000u},
        {"1023E000 512x512", 0x1023E000u},
    };
    for (const auto& c : kCandidates) {
      if (ImGui::SmallButton(c.label)) {
        REXCVAR_SET(nx1_d3d9_dbg_track_addr, c.addr);
        std::snprintf(track_buf, sizeof(track_buf), "%08X", c.addr);
      }
      ImGui::SameLine();
    }
    ImGui::NewLine();
    ImGui::Text("now: %08X  (config overrides this at launch -- re-set after loading)",
                REXCVAR_GET(nx1_d3d9_dbg_track_addr));

    ImGui::Spacing();
    if (ImGui::Button("Learn texture baseline (3s)")) {
      ResourceTracker::Get().LearnTextureBaseline(180);
    }
    ImGui::SameLine();
    if (ImGui::Button("Report effect candidates")) {
      ResourceTracker::Get().ReportEffectCandidates();
    }
    ImGui::TextDisabled("Learn a baseline, fire ~12 SEPARATE shots, then report. The flash shows");
    ImGui::TextDisabled("up with ~12 appearances; world textures have 1.");

    ImGui::Spacing();
    if (ImGui::Button("Dump next 8 texture decodes")) {
      REXCVAR_SET(nx1_d3d9_dbg_texdump, 8u);
    }
    ImGui::TextDisabled("Logs the fetch constant + raw guest bytes and writes texdump/tex_*.bmp.");
    ImGui::TextDisabled("With mirror+freeze off, sprites re-decode as they are written -- so");
    ImGui::TextDisabled("press this, then fire, to catch the muzzle flash.");

    ImGui::Spacing();
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
  // The picker is independent of the F4 settings panel: either one being open is reason to run
  // a frame. DrawPicker also owns releasing/restoring the cursor, so it must still be called on
  // the frame the picker is turned OFF -- hence it runs whenever the cursor is still released.
  const bool want_frame = visible_ || picker_visible_ || cursor_released_;
  if (!initialized_ || !want_frame || !device_objects_valid_) {
    return;
  }
  ImGui_ImplDX9_NewFrame();
  ImGui_ImplWin32_NewFrame();
  ImGui::NewFrame();
  if (visible_) {
    DrawPanels();
  }
  DrawPicker();
  ImGui::EndFrame();
  ImGui::Render();
  // imgui_impl_dx9 brackets this in a D3D9 state block, so nothing it sets leaks into the
  // next frame's draws.
  ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
}

}  // namespace nx1::d3d9

#endif  // _WIN32
