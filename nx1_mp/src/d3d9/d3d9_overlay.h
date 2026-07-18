/**
 * @file    d3d9_overlay.h
 * @brief   Dear ImGui overlay rendered on the native D3D9 device.
 *
 * The SDK already has an ImGui stack (ui::ImGuiDrawer + dialogs), but it renders through an
 * ImmediateDrawer implemented only for D3D12, into a swapchain the D3D12 presenter owns. Once
 * the native renderer creates its own D3D9Ex device on the same HWND and calls PresentEx, that
 * swapchain never reaches the screen -- so the overlay is still being drawn every frame, just
 * underneath us. Owning the PM4 ring would not change that: the ring carries guest GPU
 * commands, while the overlay is host-side UI.
 *
 * So this is a self-contained context on our own device, drawn at the end of our frame right
 * before PresentEx. It uses the stock imgui_impl_dx9 backend, which brackets its work in a
 * D3D9 state block, so it cannot leak state into the next frame's draws.
 *
 * Input arrives through a WndProc subclass. Messages are only consumed while the overlay is
 * visible AND ImGui wants them; otherwise they fall through to the window's own handler so the
 * game keeps its input.
 */

#pragma once

#include <cstdint>
#include <string>

#ifdef _WIN32
#include <d3d9.h>
#include <windows.h>

namespace nx1::d3d9 {

class Overlay {
 public:
  static Overlay& Get();

  /// Create the ImGui context and hook the window. Safe to call repeatedly.
  bool Initialize(IDirect3DDevice9Ex* device, HWND hwnd);
  void Shutdown();

  /// Build and draw the UI. Call from Present, after the scene, before PresentEx.
  void Render();

  bool visible() const { return visible_; }
  void SetVisible(bool visible) { visible_ = visible; }

  /// Released/recreated around a device reset (imgui holds device objects).
  void OnDeviceLost();
  void OnDeviceReset();

 private:
  Overlay() = default;
  ~Overlay() { Shutdown(); }
  Overlay(const Overlay&) = delete;
  Overlay& operator=(const Overlay&) = delete;

  void DrawPanels();
  /// The cvar/TOML editor, rebuilt against our own ImGui context. The SDK's SettingsDialog
  /// cannot be reused directly: it is an ImGuiDialog, and ImGuiDialog::GetIO switches the
  /// current ImGui context to the drawer's, which would fight ours mid-frame. Only the
  /// presentation is duplicated -- the data, validation and TOML writing all still come from
  /// rex::cvar, so this stays in step with whatever cvars the rest of the project defines.
  void DrawSettings();

  char search_[128] = {};
  std::string selected_category_;
  bool show_settings_ = true;
  bool show_diagnostics_ = true;

  static LRESULT CALLBACK WndProcThunk(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);

  IDirect3DDevice9Ex* device_ = nullptr;
  HWND hwnd_ = nullptr;
  WNDPROC prev_wndproc_ = nullptr;
  bool initialized_ = false;
  bool visible_ = false;
  bool device_objects_valid_ = false;
};

}  // namespace nx1::d3d9

#endif  // _WIN32
