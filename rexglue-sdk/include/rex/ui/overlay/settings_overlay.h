/**
 * @file        rex/ui/overlay/settings_overlay.h
 *
 * @brief       ImGui settings overlay dialog for cvar editing with save-to-config.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#pragma once
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <rex/ui/imgui_dialog.h>

namespace rex::ui {

class SettingsDialog : public ImGuiDialog {
 public:
  using ValueChangedCallback =
      std::function<void(std::string_view name, std::string_view value)>;

  // config_path: where "Save to config" writes (e.g. exe_dir / "app.toml")
  SettingsDialog(ImGuiDrawer* imgui_drawer, std::filesystem::path config_path,
                 ValueChangedCallback value_changed_callback = {});
  ~SettingsDialog();

 protected:
  void OnDraw(ImGuiIO& io) override;

 private:
  void NotifyValueChanged(std::string_view name, std::string_view value);

  std::filesystem::path config_path_;
  ValueChangedCallback value_changed_callback_;
  char search_buf_[128] = {};
  std::string selected_category_;
  std::string capturing_bind_name_;
};

}  // namespace rex::ui
