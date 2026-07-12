/**
 * @file        rex/input/mnk/mnk_input_driver.h
 * @brief       Keyboard/mouse input driver - maps MnK to Xbox 360 controller.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#pragma once

#include <rex/input/input_driver.h>
#include <rex/ui/window_listener.h>

#include <chrono>
#include <cstdint>
#include <mutex>
#include <queue>
#include <string>

namespace rex::input::mnk {

class MnkInputDriver final : public InputDriver,
                             public rex::ui::WindowInputListener,
                             public rex::ui::WindowListener {
 public:
  explicit MnkInputDriver(rex::ui::Window* window, size_t window_z_order);
  ~MnkInputDriver() override;

  X_STATUS Setup() override;

  X_RESULT GetCapabilities(uint32_t user_index, uint32_t flags,
                           X_INPUT_CAPABILITIES* out_caps) override;
  X_RESULT GetState(uint32_t user_index, X_INPUT_STATE* out_state) override;
  X_RESULT SetState(uint32_t user_index, X_INPUT_VIBRATION* vibration) override;
  X_RESULT GetKeystroke(uint32_t user_index, uint32_t flags,
                        X_INPUT_KEYSTROKE* out_keystroke) override;

  void OnWindowAvailable(rex::ui::Window* window) override;

  // WindowInputListener
  void OnKeyDown(rex::ui::KeyEvent& e) override;
  void OnKeyUp(rex::ui::KeyEvent& e) override;
  void OnKeyChar(rex::ui::KeyEvent& e) override;
  void OnMouseDown(rex::ui::MouseEvent& e) override;
  void OnMouseUp(rex::ui::MouseEvent& e) override;
  void OnMouseMove(rex::ui::MouseEvent& e) override;
  void OnRawMouseMove(rex::ui::RawMouseEvent& e) override;

  // WindowListener
  void OnClosing(rex::ui::UIEvent& e) override;
  void OnLostFocus(rex::ui::UISetupEvent& e) override;
  void OnGotFocus(rex::ui::UISetupEvent& e) override;

 private:
  enum class MenuNavDirection { kUp, kDown, kLeft, kRight };

  uint32_t UserIndex() const;
  bool IsEnabled() const;
  bool IsKeyboardPassthroughEnabled() const;
  void CenterCursor();
  void RefreshMenuMousePositionFromOS();
  void EnsureMenuVirtualCursorInitialized();
  void MoveMenuVirtualCursor(double delta_x, double delta_y);
  void SetMenuMousePositionFromVirtualCursor();
  void WarpOSCursorToVirtualCursor();
  void UpdateAutoMouseCaptureFromGameInput();
  void UpdateMouseCapture();
  void UpdateMouseClip();
  void ReleaseMouseClip();
  void ReleaseMouseCaptureNow();
  void ClearMenuNavigation();
  bool HasPendingMenuNavigation() const;
  void QueueMenuNavigation(MenuNavDirection direction, int32_t steps);
  void PumpMenuNavigation();
  void ScheduleMenuHoverRow(int32_t target_row, bool force_reanchor);
  void ScheduleMenuHoverColumn(int32_t target_column, bool force_reanchor);
  void MarkMenuHoverDirty();
  void ResetMenuHoverSelectionAnchor();
  void InvalidateMenuHoverForTransition();
  void ResetMenuHoverState();
  void SetKeyState(uint16_t vk, bool down);
  void EnqueueKeystroke(uint16_t vk, uint16_t unicode, uint16_t flags);
  void FlushQueuedKeystrokes();

  rex::ui::Window* attached_window_ = nullptr;

  std::mutex state_mutex_;
  bool key_down_[256] = {};

  // Mouse delta tracking
  int32_t mouse_dx_ = 0;
  int32_t mouse_dy_ = 0;
  double right_stick_x_ = 0.0;
  double right_stick_y_ = 0.0;
  std::chrono::steady_clock::time_point right_stick_last_poll_ = {};
  int32_t prev_mouse_x_ = 0;
  int32_t prev_mouse_y_ = 0;
  bool mouse_captured_ = false;
  bool mouse_capture_requested_ = false;
  bool escape_menu_release_active_ = false;
  bool restore_mouse_capture_on_focus_ = false;
  bool mouse_clip_active_ = false;
  int32_t mouse_clip_left_ = 0;
  int32_t mouse_clip_top_ = 0;
  int32_t mouse_clip_right_ = 0;
  int32_t mouse_clip_bottom_ = 0;
  bool menu_select_down_ = false;
  bool menu_mouse_has_position_ = false;
  int32_t menu_mouse_x_ = 0;
  int32_t menu_mouse_y_ = 0;
  int32_t menu_mouse_dx_ = 0;
  int32_t menu_mouse_dy_ = 0;
  bool menu_virtual_cursor_initialized_ = false;
  double menu_virtual_cursor_x_ = 0.0;
  double menu_virtual_cursor_y_ = 0.0;
  int32_t menu_hover_target_row_ = -1;
  int32_t menu_hover_target_column_ = -1;
  int32_t menu_hover_selected_row_ = 0;
  int32_t menu_hover_selected_column_ = 1;
  uint32_t menu_direct_hover_pointer_generation_ = 0;
  uint32_t menu_direct_hover_miss_frames_ = 0;
  uint32_t menu_hover_nav_cooldown_frames_ = 0;
  uint32_t menu_nav_left_frames_ = 0;
  uint32_t menu_nav_right_frames_ = 0;
  uint32_t menu_nav_up_frames_ = 0;
  uint32_t menu_nav_down_frames_ = 0;
  bool menu_hover_dirty_ = true;
  uint32_t menu_transition_cooldown_frames_ = 0;
  bool menu_select_pending_after_nav_ = false;
  uint32_t menu_select_pulse_frames_ = 0;
  uint32_t start_pulse_frames_ = 0;
  bool escape_start_suppressed_until_key_up_ = false;
  uint32_t game_key_catcher_generation_ = 0;
  std::queue<MenuNavDirection> menu_nav_queue_;
  bool has_focus_ = true;

  // Keystroke queue
  std::queue<X_INPUT_KEYSTROKE> keystroke_queue_;
  std::queue<X_INPUT_KEYSTROKE> pending_keystroke_queue_;

  // Packet number incremented on state change
  uint32_t packet_number_ = 0;
};

}  // namespace rex::input::mnk
