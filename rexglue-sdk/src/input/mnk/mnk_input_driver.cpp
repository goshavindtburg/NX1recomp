/**
 * @file        input/mnk/mnk_input_driver.cpp
 * @brief       Keyboard/mouse input driver implementation.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#include <rex/input/mnk/mnk_input_driver.h>

#include <rex/cvar.h>
#include <rex/input/input.h>
#include <rex/logging.h>
#include <rex/input/mnk/mouse_look.h>
#include <rex/ui/keybinds.h>
#include <rex/ui/virtual_key.h>
#include <rex/ui/window.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>

#if REX_PLATFORM_WIN32
#include <rex/ui/window_win.h>
#include <Windows.h>
#endif

REXCVAR_DEFINE_BOOL(mnk_mode, false, "Input", "Enable keyboard/mouse controller emulation");
REXCVAR_DEFINE_BOOL(keyboard_passthrough, true, "Input",
                    "Pass keyboard events through to XamInputGetKeystroke");
REXCVAR_DEFINE_INT32(mnk_user_index, 0, "Input", "Controller slot (0-3) for MnK").range(0, 3);
REXCVAR_DEFINE_DOUBLE(mnk_sensitivity, 1.0, "Input", "Mouse sensitivity for right stick")
    .range(0.01, 10.0);
REXCVAR_DEFINE_BOOL(mnk_raw_mouse, true, "Input",
                    "Use raw mouse deltas for captured keyboard/mouse camera look");
REXCVAR_DEFINE_BOOL(mnk_auto_capture_gameplay, true, "Input",
                    "Automatically capture the mouse when the game leaves UI input catchers");
REXCVAR_DEFINE_BOOL(mnk_auto_release_on_key_catcher, true, "Input",
                    "Automatically release captured mouse when the game opens UI input catchers");
REXCVAR_DEFINE_BOOL(mnk_native_mouse_look, true, "Input",
                    "Feed raw captured mouse deltas into the game's native mouse-look path");
REXCVAR_DEFINE_DOUBLE(mnk_native_mouse_sensitivity, 1.0, "Input",
                      "Sensitivity multiplier for native mouse-look deltas")
    .range(0.01, 20.0);
REXCVAR_DEFINE_DOUBLE(mnk_native_mouse_max_delta, 250.0, "Input",
                      "Maximum native mouse delta consumed by one game command")
    .range(1.0, 10000.0);
REXCVAR_DEFINE_DOUBLE(mnk_mouse_stick_deadzone, 8500.0, "Input",
                      "Base right-stick magnitude applied when raw mouse moves")
    .range(0.0, 32767.0);
REXCVAR_DEFINE_DOUBLE(mnk_mouse_stick_scale, 2800.0, "Input",
                      "Raw mouse delta to virtual right-stick scale")
    .range(1.0, 32767.0);
REXCVAR_DEFINE_DOUBLE(mnk_mouse_stick_hold_ms, 18.0, "Input",
                      "Virtual right-stick fallback release smoothing in milliseconds")
    .range(1.0, 100.0);
REXCVAR_DEFINE_DOUBLE(mnk_mouse_stick_smoothing_ms, 6.0, "Input",
                      "Virtual right-stick fallback attack smoothing in milliseconds")
    .range(0.0, 100.0);
REXCVAR_DEFINE_BOOL(mnk_menu_direct_hover, true, "Input",
                    "Set the game menu cursor directly from item hitboxes under the mouse");
REXCVAR_DEFINE_BOOL(mnk_menu_direct_hover_cursor_only, true, "Input",
                    "Direct menu hover should only update the game's selected item index");
REXCVAR_DEFINE_BOOL(mnk_menu_direct_hover_carousel, true, "Input",
                    "Drive horizontal menu carousels from direct mouse hover hit bands");
REXCVAR_DEFINE_DOUBLE(mnk_menu_direct_hover_padding, 4.0, "Input",
                      "Virtual UI pixels added around menu item hitboxes for direct mouse hover")
    .range(-16.0, 32.0);
REXCVAR_DEFINE_BOOL(mnk_menu_direct_hover_suppress_fallback, true, "Input",
                    "When direct menu hover is enabled, ignore legacy D-pad hover emulation unless "
                    "direct hover is disabled");
REXCVAR_DEFINE_BOOL(mnk_menu_direct_hover_block_fallback_on_miss, true, "Input",
                    "When direct menu hover is enabled, do not fall back to D-pad hover if the "
                    "mouse is not currently over a menu item");
REXCVAR_DEFINE_INT32(mnk_menu_direct_hover_fallback_miss_frames, 5, "Input",
                     "Direct-hover misses before legacy absolute menu hover is allowed again")
    .range(0, 120);
REXCVAR_DEFINE_INT32(mnk_menu_direct_hover_stable_frames, 2, "Input",
                     "Frames a direct menu hover candidate must remain stable before focus changes")
    .range(0, 30);
REXCVAR_DEFINE_BOOL(mnk_menu_horizontal_motion_nav, false, "Input",
                    "Allow horizontal mouse movement to emulate D-pad left/right in menus");
REXCVAR_DEFINE_BOOL(mnk_menu_hover_nav, true, "Input",
                    "Use absolute mouse position to steer menu selection vertically");
REXCVAR_DEFINE_BOOL(mnk_menu_virtual_cursor, false, "Input",
                    "Use an internal virtual cursor for menu hover instead of OS absolute position");
REXCVAR_DEFINE_DOUBLE(mnk_menu_virtual_cursor_sensitivity, 1.0, "Input",
                      "Mouse delta scale for the menu virtual cursor")
    .range(0.05, 20.0);
REXCVAR_DEFINE_BOOL(mnk_menu_virtual_cursor_warp_os, true, "Input",
                    "Keep the OS cursor visually pinned to the menu virtual cursor");
REXCVAR_DEFINE_BOOL(mnk_menu_hover_game_viewport, true, "Input",
                    "Map menu hover against the centered game viewport instead of the full window");
REXCVAR_DEFINE_DOUBLE(mnk_menu_hover_viewport_aspect, 16.0 / 9.0, "Input",
                      "Aspect ratio used for game-viewport menu hover mapping")
    .range(0.1, 10.0);
REXCVAR_DEFINE_DOUBLE(mnk_menu_hover_y_offset, 0.0, "Input",
                      "Normalized vertical calibration offset applied to menu mouse hover")
    .range(-0.25, 0.25);
REXCVAR_DEFINE_DOUBLE(mnk_menu_hover_x_min, 0.20, "Input",
                      "Normalized left edge of the menu option hitbox lane")
    .range(0.0, 1.0);
REXCVAR_DEFINE_DOUBLE(mnk_menu_hover_x_max, 0.36, "Input",
                      "Normalized right edge of the menu option hitbox lane")
    .range(0.0, 1.0);
REXCVAR_DEFINE_DOUBLE(mnk_menu_hover_first_y, 0.232, "Input",
                      "Normalized Y coordinate for the first menu row")
    .range(0.0, 1.0);
REXCVAR_DEFINE_DOUBLE(mnk_menu_hover_row_step, 0.0425, "Input",
                      "Normalized vertical spacing between menu rows")
    .range(0.001, 0.25);
REXCVAR_DEFINE_DOUBLE(mnk_menu_hover_row_deadzone, 0.016, "Input",
                      "Normalized half-height of a selectable menu option hitbox")
    .range(0.001, 0.25);
REXCVAR_DEFINE_BOOL(mnk_menu_hover_clamp_edges, false, "Input",
                    "Treat pointer movement beyond menu ends as hovering the nearest end item");
REXCVAR_DEFINE_INT32(mnk_menu_hover_row_count, 7, "Input",
                     "Number of vertical menu rows covered by absolute mouse hover")
    .range(1, 32);
REXCVAR_DEFINE_INT32(mnk_menu_hover_initial_row, 0, "Input",
                     "Assumed selected row when entering menu mouse mode")
    .range(0, 31);
REXCVAR_DEFINE_BOOL(mnk_menu_hover_reanchor, true, "Input",
                    "Re-anchor absolute menu hover after clicks, focus changes, and menu transitions");
REXCVAR_DEFINE_INT32(mnk_menu_hover_reanchor_steps, 7, "Input",
                     "D-pad steps used to clamp vertical hover navigation to an edge")
    .range(1, 64);
REXCVAR_DEFINE_BOOL(mnk_menu_click_sync, true, "Input",
                    "Move menu selection to the hovered item before sending click/select");
REXCVAR_DEFINE_INT32(mnk_menu_transition_cooldown_frames, 12, "Input",
                     "Frames to wait after menu-changing inputs before syncing hover again")
    .range(0, 120);
REXCVAR_DEFINE_BOOL(mnk_menu_carousel_hover_nav, true, "Input",
                    "Use absolute mouse position to steer horizontal menu carousels");
REXCVAR_DEFINE_DOUBLE(mnk_menu_carousel_y_min, 0.53, "Input",
                      "Normalized top edge of the horizontal carousel hover zone")
    .range(0.0, 1.0);
REXCVAR_DEFINE_DOUBLE(mnk_menu_carousel_y_max, 0.72, "Input",
                      "Normalized bottom edge of the horizontal carousel hover zone")
    .range(0.0, 1.0);
REXCVAR_DEFINE_DOUBLE(mnk_menu_carousel_first_x, 0.28, "Input",
                      "Normalized X coordinate for the first horizontal carousel item")
    .range(0.0, 1.0);
REXCVAR_DEFINE_DOUBLE(mnk_menu_carousel_column_step, 0.22, "Input",
                      "Normalized horizontal spacing between carousel items")
    .range(0.001, 0.5);
REXCVAR_DEFINE_DOUBLE(mnk_menu_carousel_column_deadzone, 0.105, "Input",
                      "Normalized distance from a carousel item center that still counts as hover")
    .range(0.001, 0.5);
REXCVAR_DEFINE_INT32(mnk_menu_carousel_column_count, 3, "Input",
                     "Number of horizontal carousel items covered by absolute mouse hover")
    .range(1, 16);
REXCVAR_DEFINE_INT32(mnk_menu_carousel_initial_column, 1, "Input",
                     "Assumed selected column when entering carousel menu mouse mode")
    .range(0, 15);
REXCVAR_DEFINE_INT32(mnk_menu_carousel_reanchor_steps, 8, "Input",
                     "D-pad steps used to clamp horizontal carousel hover navigation to an edge")
    .range(1, 64);

REXCVAR_DEFINE_STRING(keybind_a, "Space", "Input/Keybinds/Controller", "A button");
REXCVAR_DEFINE_STRING(keybind_b, "Shift", "Input/Keybinds/Controller", "B button");
REXCVAR_DEFINE_STRING(keybind_x, "R", "Input/Keybinds/Controller", "X button");
REXCVAR_DEFINE_STRING(keybind_y, "E", "Input/Keybinds/Controller", "Y button");
REXCVAR_DEFINE_STRING(keybind_left_trigger, "RMB", "Input/Keybinds/Controller", "Left trigger");
REXCVAR_DEFINE_STRING(keybind_right_trigger, "LMB", "Input/Keybinds/Controller", "Right trigger");
REXCVAR_DEFINE_STRING(keybind_left_shoulder, "Q", "Input/Keybinds/Controller", "Left shoulder");
REXCVAR_DEFINE_STRING(keybind_right_shoulder, "F", "Input/Keybinds/Controller", "Right shoulder");
REXCVAR_DEFINE_STRING(keybind_lstick_up, "W", "Input/Keybinds/Controller", "Left stick up");
REXCVAR_DEFINE_STRING(keybind_lstick_down, "S", "Input/Keybinds/Controller", "Left stick down");
REXCVAR_DEFINE_STRING(keybind_lstick_left, "A", "Input/Keybinds/Controller", "Left stick left");
REXCVAR_DEFINE_STRING(keybind_lstick_right, "D", "Input/Keybinds/Controller", "Left stick right");
REXCVAR_DEFINE_STRING(keybind_lstick_press, "C", "Input/Keybinds/Controller", "Left stick press");
REXCVAR_DEFINE_STRING(keybind_rstick_press, "MMB", "Input/Keybinds/Controller",
                      "Right stick press");
REXCVAR_DEFINE_STRING(keybind_dpad_up, "Up", "Input/Keybinds/Controller", "D-pad up");
REXCVAR_DEFINE_STRING(keybind_dpad_down, "Down", "Input/Keybinds/Controller", "D-pad down");
REXCVAR_DEFINE_STRING(keybind_dpad_left, "Left", "Input/Keybinds/Controller", "D-pad left");
REXCVAR_DEFINE_STRING(keybind_dpad_right, "Right", "Input/Keybinds/Controller", "D-pad right");
REXCVAR_DEFINE_STRING(keybind_back, "Tab", "Input/Keybinds/Controller", "Back button");
REXCVAR_DEFINE_STRING(keybind_start, "Escape", "Input/Keybinds/Controller", "Start button");
REXCVAR_DEFINE_STRING(keybind_guide, "", "Input/Keybinds/Controller", "Guide button");

namespace rex::input::mnk {

using rex::ui::VirtualKey;

namespace {

constexpr int32_t kMenuVerticalNavThreshold = 28;
constexpr int32_t kMenuHorizontalNavThreshold = 96;
constexpr uint32_t kMenuNavPulseFrames = 3;
constexpr uint32_t kMenuHoverReleaseFrames = 2;
constexpr uint32_t kEscapeStartPulseFrames = 1;
constexpr double kDefaultMousePollMs = 1000.0 / 60.0;
constexpr double kStickIdleSnap = 128.0;

std::mutex g_native_mouse_mutex;
double g_native_mouse_dx = 0.0;
double g_native_mouse_dy = 0.0;
std::mutex g_menu_pointer_mutex;
float g_menu_pointer_x = 0.0f;
float g_menu_pointer_y = 0.0f;
uint32_t g_menu_pointer_generation = 0;
bool g_menu_pointer_valid = false;
uint32_t g_menu_direct_hover_applied_generation = 0;
bool g_menu_direct_hover_applied_known = false;
uint32_t g_menu_mouse_buttons_down = 0;
uint32_t g_menu_mouse_buttons_pressed = 0;
uint32_t g_menu_mouse_buttons_released = 0;
uint32_t g_menu_mouse_buttons_generation = 0;
bool g_force_menu_mouse_mode = false;
std::mutex g_game_key_catcher_mutex;
uint32_t g_game_key_catcher_mask = 0;
uint32_t g_game_key_catcher_generation = 0;
bool g_game_key_catcher_known = false;

double ClampFinite(double value, double min_value, double max_value) {
  if (!std::isfinite(value)) {
    return 0.0;
  }
  return std::clamp(value, min_value, max_value);
}

double SmoothAxis(double current, double target, double dt_ms, double time_constant_ms) {
  if (time_constant_ms <= 0.0) {
    return target;
  }

  const double alpha = 1.0 - std::exp(-std::max(dt_ms, 0.0) / time_constant_ms);
  return current + ((target - current) * std::clamp(alpha, 0.0, 1.0));
}

bool GetMenuHoverViewport(const rex::ui::Window* window, double& x_origin_out,
                          double& y_origin_out, double& width_out, double& height_out) {
  if (!window) {
    return false;
  }

  const uint32_t width = window->GetActualPhysicalWidth();
  const uint32_t height = window->GetActualPhysicalHeight();
  if (width == 0 || height == 0) {
    return false;
  }

  double x_origin = 0.0;
  double y_origin = 0.0;
  double mapped_width = static_cast<double>(width);
  double mapped_height = static_cast<double>(height);
  if (REXCVAR_GET(mnk_menu_hover_game_viewport)) {
    const double viewport_aspect = std::max(REXCVAR_GET(mnk_menu_hover_viewport_aspect), 0.1);
    const double window_aspect = mapped_width / mapped_height;
    if (window_aspect > viewport_aspect) {
      mapped_width = mapped_height * viewport_aspect;
      x_origin = (static_cast<double>(width) - mapped_width) * 0.5;
    } else if (window_aspect < viewport_aspect) {
      mapped_height = mapped_width / viewport_aspect;
      y_origin = (static_cast<double>(height) - mapped_height) * 0.5;
    }
  }

  x_origin_out = x_origin;
  y_origin_out = y_origin;
  width_out = mapped_width;
  height_out = mapped_height;
  return mapped_width > 0.0 && mapped_height > 0.0;
}

bool NormalizeMenuHoverPoint(const rex::ui::Window* window, int32_t x, int32_t y,
                             double& normalized_x_out, double& normalized_y_out) {
  double x_origin = 0.0;
  double y_origin = 0.0;
  double mapped_width = 0.0;
  double mapped_height = 0.0;
  if (!GetMenuHoverViewport(window, x_origin, y_origin, mapped_width, mapped_height)) {
    return false;
  }

  const double mapped_x = static_cast<double>(x) - x_origin;
  const double mapped_y = static_cast<double>(y) - y_origin;
  if (mapped_x < 0.0 || mapped_x >= mapped_width || mapped_y < 0.0 ||
      mapped_y >= mapped_height) {
    return false;
  }

  normalized_x_out = std::clamp(mapped_x / mapped_width, 0.0, 1.0);
  normalized_y_out =
      std::clamp((mapped_y / mapped_height) + REXCVAR_GET(mnk_menu_hover_y_offset),
                 0.0, 1.0);
  return true;
}

void PublishMenuMousePointer(const rex::ui::Window* window, bool has_position, int32_t x,
                             int32_t y) {
  if (!has_position) {
    ClearMenuPointerVirtualPosition();
    return;
  }

  double normalized_x = 0.0;
  double normalized_y = 0.0;
  if (!NormalizeMenuHoverPoint(window, x, y, normalized_x, normalized_y)) {
    ClearMenuPointerVirtualPosition();
    return;
  }

  SetMenuPointerVirtualPosition(static_cast<float>(normalized_x * 640.0),
                                static_cast<float>(normalized_y * 480.0), true);
}

int32_t ClassifyMenuHoverRow(const rex::ui::Window* window, int32_t x, int32_t y) {
  double normalized_x = 0.0;
  double normalized_y = 0.0;
  if (!NormalizeMenuHoverPoint(window, x, y, normalized_x, normalized_y)) {
    return -1;
  }

  const double hover_x_min = std::clamp(REXCVAR_GET(mnk_menu_hover_x_min), 0.0, 1.0);
  const double hover_x_max = std::clamp(REXCVAR_GET(mnk_menu_hover_x_max), hover_x_min, 1.0);
  if (normalized_x < hover_x_min || normalized_x > hover_x_max) {
    return -1;
  }

  const double first_y = std::clamp(REXCVAR_GET(mnk_menu_hover_first_y), 0.0, 1.0);
  const double row_step = std::max(REXCVAR_GET(mnk_menu_hover_row_step), 0.001);
  const double row_deadzone =
      std::min(std::max(REXCVAR_GET(mnk_menu_hover_row_deadzone), 0.001), row_step * 0.49);
  const int32_t row_count = std::max(REXCVAR_GET(mnk_menu_hover_row_count), 1);
  if (REXCVAR_GET(mnk_menu_hover_clamp_edges)) {
    const double last_y = first_y + (static_cast<double>(row_count - 1) * row_step);
    if (normalized_y <= first_y) {
      return 0;
    }
    if (normalized_y >= last_y) {
      return row_count - 1;
    }
  }

  const int32_t row =
      static_cast<int32_t>(std::floor(((normalized_y - first_y) / row_step) + 0.5));
  if (row < 0 || row >= row_count) {
    return -1;
  }

  const double row_y = first_y + (static_cast<double>(row) * row_step);
  if (std::abs(normalized_y - row_y) > row_deadzone) {
    return -1;
  }

  return row;
}

int32_t ClassifyMenuHoverColumn(const rex::ui::Window* window, int32_t x, int32_t y) {
  if (!window || !REXCVAR_GET(mnk_menu_carousel_hover_nav)) {
    return -1;
  }

  double normalized_x = 0.0;
  double normalized_y = 0.0;
  if (!NormalizeMenuHoverPoint(window, x, y, normalized_x, normalized_y)) {
    return -1;
  }

  const double y_min = std::clamp(REXCVAR_GET(mnk_menu_carousel_y_min), 0.0, 1.0);
  const double y_max = std::clamp(REXCVAR_GET(mnk_menu_carousel_y_max), y_min, 1.0);
  if (normalized_y < y_min || normalized_y > y_max) {
    return -1;
  }

  const double first_x = std::clamp(REXCVAR_GET(mnk_menu_carousel_first_x), 0.0, 1.0);
  const double column_step = std::max(REXCVAR_GET(mnk_menu_carousel_column_step), 0.001);
  const double column_deadzone =
      std::max(REXCVAR_GET(mnk_menu_carousel_column_deadzone), 0.001);
  const int32_t column_count = std::max(REXCVAR_GET(mnk_menu_carousel_column_count), 1);
  if (REXCVAR_GET(mnk_menu_hover_clamp_edges)) {
    const double last_x = first_x + (static_cast<double>(column_count - 1) * column_step);
    if (normalized_x <= first_x) {
      return 0;
    }
    if (normalized_x >= last_x) {
      return column_count - 1;
    }
  }

  const int32_t column =
      static_cast<int32_t>(std::floor(((normalized_x - first_x) / column_step) + 0.5));
  if (column < 0 || column >= column_count) {
    return -1;
  }

  const double column_x = first_x + (static_cast<double>(column) * column_step);
  if (std::abs(normalized_x - column_x) > column_deadzone) {
    return -1;
  }

  return column;
}

double MouseDeltaToVirtualStick(double delta) {
  if (delta == 0.0) {
    return 0;
  }

  const double sign = delta < 0.0 ? -1.0 : 1.0;
  const double magnitude =
      std::clamp(REXCVAR_GET(mnk_mouse_stick_deadzone) +
                     (std::abs(delta) * REXCVAR_GET(mnk_sensitivity) *
                      REXCVAR_GET(mnk_mouse_stick_scale)),
                 0.0, 32767.0);
  return sign * magnitude;
}

}  // namespace

void AccumulateNativeMouseMovement(int32_t delta_x, int32_t delta_y) {
  if (!REXCVAR_GET(mnk_native_mouse_look) || (delta_x == 0 && delta_y == 0)) {
    return;
  }

  const double sensitivity = REXCVAR_GET(mnk_native_mouse_sensitivity);
  const double max_delta = std::max(REXCVAR_GET(mnk_native_mouse_max_delta), 1.0);
  std::lock_guard lock(g_native_mouse_mutex);
  g_native_mouse_dx =
      ClampFinite(g_native_mouse_dx + (static_cast<double>(delta_x) * sensitivity), -max_delta,
                  max_delta);
  g_native_mouse_dy =
      ClampFinite(g_native_mouse_dy + (static_cast<double>(delta_y) * sensitivity), -max_delta,
                  max_delta);
}

bool ConsumeNativeMouseMovement(float* out_x, float* out_y) {
  if (!REXCVAR_GET(mnk_native_mouse_look)) {
    return false;
  }

  std::lock_guard lock(g_native_mouse_mutex);
  if (g_native_mouse_dx == 0.0 && g_native_mouse_dy == 0.0) {
    return false;
  }

  if (out_x) {
    *out_x = static_cast<float>(g_native_mouse_dx);
  }
  if (out_y) {
    *out_y = static_cast<float>(g_native_mouse_dy);
  }
  g_native_mouse_dx = 0.0;
  g_native_mouse_dy = 0.0;
  return true;
}

void ClearNativeMouseMovement() {
  std::lock_guard lock(g_native_mouse_mutex);
  g_native_mouse_dx = 0.0;
  g_native_mouse_dy = 0.0;
}

void SetMenuPointerVirtualPosition(float x, float y, bool valid) {
  std::lock_guard lock(g_menu_pointer_mutex);
  if (g_menu_pointer_valid == valid && g_menu_pointer_x == x &&
      g_menu_pointer_y == y) {
    return;
  }

  g_menu_pointer_valid = valid;
  g_menu_pointer_x = x;
  g_menu_pointer_y = y;
  ++g_menu_pointer_generation;
}

bool QueryMenuPointerVirtualPosition(float* out_x, float* out_y,
                                     uint32_t* out_generation) {
  std::lock_guard lock(g_menu_pointer_mutex);
  if (!g_menu_pointer_valid) {
    return false;
  }

  if (out_x) {
    *out_x = g_menu_pointer_x;
  }
  if (out_y) {
    *out_y = g_menu_pointer_y;
  }
  if (out_generation) {
    *out_generation = g_menu_pointer_generation;
  }
  return true;
}

void ClearMenuPointerVirtualPosition() {
  std::lock_guard lock(g_menu_pointer_mutex);
  if (!g_menu_pointer_valid) {
    return;
  }

  g_menu_pointer_valid = false;
  ++g_menu_pointer_generation;
}

void ReportMenuDirectHoverApplied(uint32_t pointer_generation) {
  std::lock_guard lock(g_menu_pointer_mutex);
  g_menu_direct_hover_applied_generation = pointer_generation;
  g_menu_direct_hover_applied_known = true;
}

void ClearMenuDirectHoverApplied() {
  std::lock_guard lock(g_menu_pointer_mutex);
  g_menu_direct_hover_applied_known = false;
}

bool QueryMenuDirectHoverApplied(uint32_t pointer_generation) {
  std::lock_guard lock(g_menu_pointer_mutex);
  return g_menu_direct_hover_applied_known &&
         g_menu_direct_hover_applied_generation == pointer_generation;
}

void SetMenuMouseButtonState(uint32_t button_mask, bool down) {
  std::lock_guard lock(g_menu_pointer_mutex);
  if (button_mask == 0) {
    return;
  }

  const uint32_t changed = down ? (button_mask & ~g_menu_mouse_buttons_down)
                                : (button_mask & g_menu_mouse_buttons_down);
  if (changed == 0) {
    return;
  }

  if (down) {
    g_menu_mouse_buttons_down |= changed;
    g_menu_mouse_buttons_pressed |= changed;
  } else {
    g_menu_mouse_buttons_down &= ~changed;
    g_menu_mouse_buttons_released |= changed;
  }
  ++g_menu_mouse_buttons_generation;
}

void ClearMenuMouseButtons() {
  std::lock_guard lock(g_menu_pointer_mutex);
  if (g_menu_mouse_buttons_down == 0 && g_menu_mouse_buttons_pressed == 0 &&
      g_menu_mouse_buttons_released == 0) {
    return;
  }

  g_menu_mouse_buttons_down = 0;
  g_menu_mouse_buttons_pressed = 0;
  g_menu_mouse_buttons_released = 0;
  ++g_menu_mouse_buttons_generation;
}

bool ConsumeMenuMouseButtonSnapshot(uint32_t* out_down, uint32_t* out_pressed,
                                    uint32_t* out_released,
                                    uint32_t* out_generation) {
  std::lock_guard lock(g_menu_pointer_mutex);
  if (out_down) {
    *out_down = g_menu_mouse_buttons_down;
  }
  if (out_pressed) {
    *out_pressed = g_menu_mouse_buttons_pressed;
  }
  if (out_released) {
    *out_released = g_menu_mouse_buttons_released;
  }
  if (out_generation) {
    *out_generation = g_menu_mouse_buttons_generation;
  }

  const bool had_input = g_menu_mouse_buttons_down != 0 ||
                         g_menu_mouse_buttons_pressed != 0 ||
                         g_menu_mouse_buttons_released != 0;
  g_menu_mouse_buttons_pressed = 0;
  g_menu_mouse_buttons_released = 0;
  return had_input;
}

void SetForceMenuMouseMode(bool active) {
  std::lock_guard lock(g_menu_pointer_mutex);
  g_force_menu_mouse_mode = active;
}

bool QueryForceMenuMouseMode() {
  std::lock_guard lock(g_menu_pointer_mutex);
  return g_force_menu_mouse_mode;
}

void SetGameKeyCatcherMask(int32_t local_client_num, uint32_t catcher_mask) {
  if (local_client_num != 0) {
    return;
  }

  std::lock_guard lock(g_game_key_catcher_mutex);
  if (!g_game_key_catcher_known || g_game_key_catcher_mask != catcher_mask) {
    g_game_key_catcher_mask = catcher_mask;
    ++g_game_key_catcher_generation;
  }
  g_game_key_catcher_known = true;
}

void AddGameKeyCatcherMask(int32_t local_client_num, uint32_t catcher_mask) {
  if (local_client_num != 0) {
    return;
  }

  std::lock_guard lock(g_game_key_catcher_mutex);
  const uint32_t new_mask = g_game_key_catcher_mask | catcher_mask;
  if (!g_game_key_catcher_known || g_game_key_catcher_mask != new_mask) {
    g_game_key_catcher_mask = new_mask;
    ++g_game_key_catcher_generation;
  }
  g_game_key_catcher_known = true;
}

void RemoveGameKeyCatcherMask(int32_t local_client_num, uint32_t catcher_mask) {
  if (local_client_num != 0) {
    return;
  }

  std::lock_guard lock(g_game_key_catcher_mutex);
  const uint32_t new_mask = g_game_key_catcher_mask & ~catcher_mask;
  if (!g_game_key_catcher_known || g_game_key_catcher_mask != new_mask) {
    g_game_key_catcher_mask = new_mask;
    ++g_game_key_catcher_generation;
  }
  g_game_key_catcher_known = true;
}

void ClearGameKeyCatcherState() {
  std::lock_guard lock(g_game_key_catcher_mutex);
  g_game_key_catcher_mask = 0;
  ++g_game_key_catcher_generation;
  g_game_key_catcher_known = false;
}

bool QueryGameKeyCatcherMask(uint32_t* out_catcher_mask, uint32_t* out_generation) {
  std::lock_guard lock(g_game_key_catcher_mutex);
  if (!g_game_key_catcher_known) {
    return false;
  }
  if (out_catcher_mask) {
    *out_catcher_mask = g_game_key_catcher_mask;
  }
  if (out_generation) {
    *out_generation = g_game_key_catcher_generation;
  }
  return true;
}

MnkInputDriver::MnkInputDriver(rex::ui::Window* window, size_t window_z_order)
    : InputDriver(window, window_z_order) {}

MnkInputDriver::~MnkInputDriver() {
  // Detach handled by OnClosing; if window outlives the driver, clean up here.
  if (attached_window_) {
    SetForceMenuMouseMode(false);
    ClearMenuMouseButtons();
    ReleaseMouseClip();
    if (mouse_captured_) {
      mouse_captured_ = false;
      attached_window_->SetCursorVisibility(rex::ui::Window::CursorVisibility::kVisible);
      attached_window_->ReleaseMouse();
    }
    attached_window_->RemoveInputListener(this);
    attached_window_->RemoveListener(this);
    attached_window_ = nullptr;
  }
}

X_STATUS MnkInputDriver::Setup() {
  REXLOG_INFO("MnK input driver initialized");
  return X_STATUS_SUCCESS;
}

void MnkInputDriver::OnWindowAvailable(rex::ui::Window* window) {
  if (window) {
    attached_window_ = window;
    window->AddInputListener(this, window_z_order());
    window->AddListener(this);
  }
}

void MnkInputDriver::OnClosing(rex::ui::UIEvent&) {
  if (attached_window_) {
    mouse_capture_requested_ = false;
    restore_mouse_capture_on_focus_ = false;
    SetForceMenuMouseMode(false);
    ClearMenuMouseButtons();
    ClearGameKeyCatcherState();
    ReleaseMouseClip();
    if (mouse_captured_) {
      mouse_captured_ = false;
      attached_window_->SetCursorVisibility(rex::ui::Window::CursorVisibility::kVisible);
      attached_window_->ReleaseMouse();
    }
    attached_window_->RemoveInputListener(this);
    attached_window_->RemoveListener(this);
    attached_window_ = nullptr;
  }
}

uint32_t MnkInputDriver::UserIndex() const {
  return static_cast<uint32_t>(REXCVAR_GET(mnk_user_index));
}

bool MnkInputDriver::IsEnabled() const {
  return REXCVAR_GET(mnk_mode);
}

bool MnkInputDriver::IsKeyboardPassthroughEnabled() const {
  return REXCVAR_GET(keyboard_passthrough);
}

static bool IsBindPressed(const bool (&key_down)[256], const std::string& cvar_val) {
  VirtualKey vk = rex::ui::ParseVirtualKey(cvar_val);
  if (vk == VirtualKey::kNone)
    return false;
  uint16_t idx = static_cast<uint16_t>(vk);
  return idx < 256 && key_down[idx];
}

static bool IsBindKey(VirtualKey actual, const std::string& cvar_val) {
  const VirtualKey expected = rex::ui::ParseVirtualKey(cvar_val);
  return expected != VirtualKey::kNone && actual == expected;
}

static bool IsCaptureActivationKey(VirtualKey vk) {
  return IsBindKey(vk, REXCVAR_GET(keybind_lstick_press)) ||
         IsBindKey(vk, REXCVAR_GET(keybind_rstick_press));
}

static bool IsMenuControlKey(VirtualKey vk) {
  return IsBindKey(vk, REXCVAR_GET(keybind_a)) || IsBindKey(vk, REXCVAR_GET(keybind_b)) ||
         IsBindKey(vk, REXCVAR_GET(keybind_start)) ||
         IsBindKey(vk, REXCVAR_GET(keybind_back)) ||
         IsBindKey(vk, REXCVAR_GET(keybind_dpad_up)) ||
         IsBindKey(vk, REXCVAR_GET(keybind_dpad_down)) ||
         IsBindKey(vk, REXCVAR_GET(keybind_dpad_left)) ||
         IsBindKey(vk, REXCVAR_GET(keybind_dpad_right));
}

static bool IsMenuTransitionKey(VirtualKey vk) {
  return IsBindKey(vk, REXCVAR_GET(keybind_a)) || IsBindKey(vk, REXCVAR_GET(keybind_b)) ||
         IsBindKey(vk, REXCVAR_GET(keybind_start)) ||
         IsBindKey(vk, REXCVAR_GET(keybind_back));
}

static bool IsKeyboardVirtualKey(VirtualKey vk) {
  switch (vk) {
    case VirtualKey::kNone:
    case VirtualKey::kLButton:
    case VirtualKey::kRButton:
    case VirtualKey::kMButton:
    case VirtualKey::kXButton1:
    case VirtualKey::kXButton2:
      return false;
    default:
      break;
  }

  return static_cast<uint16_t>(vk) <= static_cast<uint16_t>(VirtualKey::kOemClear);
}

static uint16_t TranslateUnicodeForKeyEvent(const rex::ui::KeyEvent& e) {
#if REX_PLATFORM_WIN32
  const uint16_t vk = static_cast<uint16_t>(e.virtual_key());
  if (vk == 0 || vk > 0xFE) {
    return 0;
  }

  BYTE key_state[256] = {};
  if (!GetKeyboardState(key_state)) {
    return 0;
  }

  WCHAR chars[4] = {};
  const UINT scan_code = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
  const int count = ToUnicode(vk, scan_code, key_state, chars, 4, 0);
  return count == 1 ? static_cast<uint16_t>(chars[0]) : 0;
#else
  if (e.virtual_key() >= VirtualKey::kA && e.virtual_key() <= VirtualKey::kZ) {
    uint16_t ch = static_cast<uint16_t>(
        'a' + (static_cast<uint16_t>(e.virtual_key()) - static_cast<uint16_t>(VirtualKey::kA)));
    return e.is_shift_pressed() ? static_cast<uint16_t>(ch - 32) : ch;
  }
  if (e.virtual_key() >= VirtualKey::k0 && e.virtual_key() <= VirtualKey::k9) {
    return static_cast<uint16_t>(e.virtual_key());
  }
  if (e.virtual_key() == VirtualKey::kSpace) {
    return ' ';
  }
  return 0;
#endif
}

X_RESULT MnkInputDriver::GetCapabilities(uint32_t user_index, uint32_t flags,
                                         X_INPUT_CAPABILITIES* out_caps) {
  const bool keyboard_query = (flags & XINPUT_DEVTYPE_KEYBOARD) != 0 &&
                              (flags & XINPUT_DEVTYPE_GAMEPAD) == 0;
  if (keyboard_query) {
    if (!IsKeyboardPassthroughEnabled() || user_index != UserIndex()) {
      return X_ERROR_DEVICE_NOT_CONNECTED;
    }
    if (out_caps) {
      std::memset(out_caps, 0, sizeof(*out_caps));
      out_caps->type = XINPUT_DEVTYPE_KEYBOARD;
    }
    return X_ERROR_SUCCESS;
  }

  if (!IsEnabled() || user_index != UserIndex()) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  if (out_caps) {
    std::memset(out_caps, 0, sizeof(*out_caps));
    out_caps->type = XINPUT_DEVTYPE_GAMEPAD;
    out_caps->sub_type = 0x01;
    out_caps->flags = 0;
    out_caps->gamepad.buttons = 0xFFFF;
    out_caps->gamepad.left_trigger = 0xFF;
    out_caps->gamepad.right_trigger = 0xFF;
    out_caps->gamepad.thumb_lx = static_cast<int16_t>(0x7FFF);
    out_caps->gamepad.thumb_ly = static_cast<int16_t>(0x7FFF);
    out_caps->gamepad.thumb_rx = static_cast<int16_t>(0x7FFF);
    out_caps->gamepad.thumb_ry = static_cast<int16_t>(0x7FFF);
    out_caps->vibration.left_motor_speed = 0xFFFF;
    out_caps->vibration.right_motor_speed = 0xFFFF;
  }
  return X_ERROR_SUCCESS;
}

X_RESULT MnkInputDriver::GetState(uint32_t user_index, X_INPUT_STATE* out_state) {
  if (!IsEnabled() || user_index != UserIndex()) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  UpdateAutoMouseCaptureFromGameInput();
  UpdateMouseCapture();

  if (!is_active() || !has_focus_) {
    ClearMenuPointerVirtualPosition();
    ClearMenuMouseButtons();
    if (out_state) {
      std::memset(out_state, 0, sizeof(*out_state));
      out_state->packet_number = packet_number_;
    }
    return X_ERROR_SUCCESS;
  }

  std::lock_guard lock(state_mutex_);
  const bool force_menu_mouse_mode = QueryForceMenuMouseMode();
  if (force_menu_mouse_mode && (mouse_capture_requested_ || mouse_captured_)) {
    mouse_capture_requested_ = false;
    escape_menu_release_active_ = true;
    ReleaseMouseCaptureNow();
    ResetMenuHoverState();
  }
  const bool menu_mouse_mode = force_menu_mouse_mode || !mouse_capture_requested_;
  if (menu_mouse_mode) {
    if (REXCVAR_GET(mnk_menu_virtual_cursor)) {
      EnsureMenuVirtualCursorInitialized();
      SetMenuMousePositionFromVirtualCursor();
      WarpOSCursorToVirtualCursor();
    } else {
      RefreshMenuMousePositionFromOS();
    }
    PublishMenuMousePointer(attached_window_, menu_mouse_has_position_, menu_mouse_x_,
                            menu_mouse_y_);

    uint32_t menu_pointer_generation = 0;
    const bool menu_pointer_valid =
        QueryMenuPointerVirtualPosition(nullptr, nullptr, &menu_pointer_generation);
    const bool direct_menu_hover_pending =
        REXCVAR_GET(mnk_menu_direct_hover) && menu_pointer_valid;
    const bool direct_menu_hover_active =
        direct_menu_hover_pending && QueryMenuDirectHoverApplied(menu_pointer_generation);
    const bool direct_menu_hover_suppresses_fallback =
        direct_menu_hover_pending && REXCVAR_GET(mnk_menu_direct_hover_suppress_fallback);
    if (direct_menu_hover_pending) {
      if (menu_pointer_generation != menu_direct_hover_pointer_generation_) {
        menu_direct_hover_pointer_generation_ = menu_pointer_generation;
      }
    }
    if (!direct_menu_hover_pending || direct_menu_hover_active) {
      menu_direct_hover_miss_frames_ = 0;
    } else if (menu_direct_hover_miss_frames_ < UINT32_MAX) {
      ++menu_direct_hover_miss_frames_;
    }
    const bool direct_menu_hover_fallback_allowed =
        direct_menu_hover_suppresses_fallback &&
        !REXCVAR_GET(mnk_menu_direct_hover_block_fallback_on_miss) &&
        menu_direct_hover_miss_frames_ >=
            static_cast<uint32_t>(std::max(REXCVAR_GET(
                                      mnk_menu_direct_hover_fallback_miss_frames),
                                      0));
    const bool direct_menu_hover_blocks_fallback =
        direct_menu_hover_suppresses_fallback && !direct_menu_hover_fallback_allowed;

    if (direct_menu_hover_active || direct_menu_hover_blocks_fallback) {
      ClearMenuNavigation();
      menu_mouse_dx_ = 0;
      menu_mouse_dy_ = 0;
      menu_hover_target_row_ = -1;
      menu_hover_target_column_ = -1;
      menu_select_pending_after_nav_ = false;
    } else if (REXCVAR_GET(mnk_menu_horizontal_motion_nav)) {
      if (!HasPendingMenuNavigation() && menu_mouse_dx_ <= -kMenuHorizontalNavThreshold) {
        menu_nav_left_frames_ = kMenuNavPulseFrames;
        menu_mouse_dx_ = 0;
        MarkMenuHoverDirty();
      } else if (!HasPendingMenuNavigation() && menu_mouse_dx_ >= kMenuHorizontalNavThreshold) {
        menu_nav_right_frames_ = kMenuNavPulseFrames;
        menu_mouse_dx_ = 0;
        MarkMenuHoverDirty();
      }
    } else {
      menu_mouse_dx_ = 0;
    }

    if (direct_menu_hover_active || direct_menu_hover_blocks_fallback) {
      if (menu_transition_cooldown_frames_ > 0) {
        --menu_transition_cooldown_frames_;
      }
    } else if (menu_transition_cooldown_frames_ > 0) {
      --menu_transition_cooldown_frames_;
      menu_mouse_dx_ = 0;
      menu_mouse_dy_ = 0;
    } else {
      bool hover_target_found = false;
      if (menu_mouse_has_position_) {
        const int32_t target_column =
            ClassifyMenuHoverColumn(attached_window_, menu_mouse_x_, menu_mouse_y_);
        if (target_column >= 0) {
          ScheduleMenuHoverColumn(target_column, false);
          menu_mouse_dy_ = 0;
          hover_target_found = true;
        } else if (REXCVAR_GET(mnk_menu_hover_nav)) {
          const int32_t target_row =
              ClassifyMenuHoverRow(attached_window_, menu_mouse_x_, menu_mouse_y_);
          if (target_row >= 0) {
            ScheduleMenuHoverRow(target_row, false);
            hover_target_found = true;
          }
          menu_mouse_dy_ = 0;
        }
      }

      if (!hover_target_found) {
        menu_hover_target_row_ = -1;
        menu_hover_target_column_ = -1;
      }

      if (!hover_target_found && !REXCVAR_GET(mnk_menu_hover_nav) &&
          !HasPendingMenuNavigation()) {
        if (menu_mouse_dy_ <= -kMenuVerticalNavThreshold) {
          menu_nav_up_frames_ = kMenuNavPulseFrames;
          --menu_hover_selected_row_;
          menu_mouse_dy_ += kMenuVerticalNavThreshold;
          MarkMenuHoverDirty();
        } else if (menu_mouse_dy_ >= kMenuVerticalNavThreshold) {
          menu_nav_down_frames_ = kMenuNavPulseFrames;
          ++menu_hover_selected_row_;
          menu_mouse_dy_ -= kMenuVerticalNavThreshold;
          MarkMenuHoverDirty();
        }
      }

      PumpMenuNavigation();
      if (menu_select_pending_after_nav_ && !HasPendingMenuNavigation()) {
        menu_select_pending_after_nav_ = false;
        menu_select_pulse_frames_ = kMenuNavPulseFrames;
        InvalidateMenuHoverForTransition();
      }
    }
  } else {
    ClearMenuPointerVirtualPosition();
    ResetMenuHoverState();
    right_stick_x_ = 0.0;
    right_stick_y_ = 0.0;
    right_stick_last_poll_ = {};
  }

  uint16_t buttons = 0;
  if (menu_select_down_ || menu_select_pulse_frames_ > 0)
    buttons |= X_INPUT_GAMEPAD_A;
  if (menu_select_pulse_frames_ > 0) {
    --menu_select_pulse_frames_;
  }
  if (IsBindPressed(key_down_, REXCVAR_GET(keybind_a)))
    buttons |= X_INPUT_GAMEPAD_A;
  if (IsBindPressed(key_down_, REXCVAR_GET(keybind_b)))
    buttons |= X_INPUT_GAMEPAD_B;
  if (IsBindPressed(key_down_, REXCVAR_GET(keybind_x)))
    buttons |= X_INPUT_GAMEPAD_X;
  if (IsBindPressed(key_down_, REXCVAR_GET(keybind_y)))
    buttons |= X_INPUT_GAMEPAD_Y;
  if (IsBindPressed(key_down_, REXCVAR_GET(keybind_left_shoulder)))
    buttons |= X_INPUT_GAMEPAD_LEFT_SHOULDER;
  if (IsBindPressed(key_down_, REXCVAR_GET(keybind_right_shoulder)))
    buttons |= X_INPUT_GAMEPAD_RIGHT_SHOULDER;
  if (IsBindPressed(key_down_, REXCVAR_GET(keybind_lstick_press)))
    buttons |= X_INPUT_GAMEPAD_LEFT_THUMB;
  if (IsBindPressed(key_down_, REXCVAR_GET(keybind_rstick_press)))
    buttons |= X_INPUT_GAMEPAD_RIGHT_THUMB;
  if (IsBindPressed(key_down_, REXCVAR_GET(keybind_back)))
    buttons |= X_INPUT_GAMEPAD_BACK;
  if (start_pulse_frames_ > 0) {
    buttons |= X_INPUT_GAMEPAD_START;
    --start_pulse_frames_;
  }
  if (IsBindPressed(key_down_, REXCVAR_GET(keybind_start)))
    buttons |= X_INPUT_GAMEPAD_START;
  if (IsBindPressed(key_down_, REXCVAR_GET(keybind_guide)))
    buttons |= X_INPUT_GAMEPAD_GUIDE;
  if (IsBindPressed(key_down_, REXCVAR_GET(keybind_dpad_up)))
    buttons |= X_INPUT_GAMEPAD_DPAD_UP;
  if (IsBindPressed(key_down_, REXCVAR_GET(keybind_dpad_down)))
    buttons |= X_INPUT_GAMEPAD_DPAD_DOWN;
  if (IsBindPressed(key_down_, REXCVAR_GET(keybind_dpad_left)))
    buttons |= X_INPUT_GAMEPAD_DPAD_LEFT;
  if (IsBindPressed(key_down_, REXCVAR_GET(keybind_dpad_right)))
    buttons |= X_INPUT_GAMEPAD_DPAD_RIGHT;
  bool completed_menu_nav_pulse = false;
  if (menu_nav_up_frames_ > 0) {
    buttons |= X_INPUT_GAMEPAD_DPAD_UP;
    --menu_nav_up_frames_;
    completed_menu_nav_pulse = completed_menu_nav_pulse || menu_nav_up_frames_ == 0;
  }
  if (menu_nav_down_frames_ > 0) {
    buttons |= X_INPUT_GAMEPAD_DPAD_DOWN;
    --menu_nav_down_frames_;
    completed_menu_nav_pulse = completed_menu_nav_pulse || menu_nav_down_frames_ == 0;
  }
  if (menu_nav_left_frames_ > 0) {
    buttons |= X_INPUT_GAMEPAD_DPAD_LEFT;
    --menu_nav_left_frames_;
    completed_menu_nav_pulse = completed_menu_nav_pulse || menu_nav_left_frames_ == 0;
  }
  if (menu_nav_right_frames_ > 0) {
    buttons |= X_INPUT_GAMEPAD_DPAD_RIGHT;
    --menu_nav_right_frames_;
    completed_menu_nav_pulse = completed_menu_nav_pulse || menu_nav_right_frames_ == 0;
  }
  if (completed_menu_nav_pulse && menu_hover_nav_cooldown_frames_ == 0) {
    menu_hover_nav_cooldown_frames_ = kMenuHoverReleaseFrames;
  }

  uint8_t lt =
      !menu_mouse_mode && IsBindPressed(key_down_, REXCVAR_GET(keybind_left_trigger)) ? 0xFF : 0;
  uint8_t rt =
      !menu_mouse_mode && IsBindPressed(key_down_, REXCVAR_GET(keybind_right_trigger)) ? 0xFF : 0;

  int32_t lx = 0;
  int32_t ly = 0;
  if (!menu_mouse_mode) {
    if (IsBindPressed(key_down_, REXCVAR_GET(keybind_lstick_left)))
      lx -= INT16_MAX;
    if (IsBindPressed(key_down_, REXCVAR_GET(keybind_lstick_right)))
      lx += INT16_MAX;
    if (IsBindPressed(key_down_, REXCVAR_GET(keybind_lstick_up)))
      ly += INT16_MAX;
    if (IsBindPressed(key_down_, REXCVAR_GET(keybind_lstick_down)))
      ly -= INT16_MAX;
  }

  int32_t rx = 0;
  int32_t ry = 0;
  if (!menu_mouse_mode && REXCVAR_GET(mnk_raw_mouse) &&
      !REXCVAR_GET(mnk_native_mouse_look)) {
    const auto now = std::chrono::steady_clock::now();
    double dt_ms = kDefaultMousePollMs;
    if (right_stick_last_poll_ != std::chrono::steady_clock::time_point{}) {
      dt_ms = std::chrono::duration<double, std::milli>(now - right_stick_last_poll_).count();
    }
    right_stick_last_poll_ = now;
    dt_ms = std::clamp(dt_ms, 1.0, 50.0);

    const int32_t dx = mouse_dx_;
    const int32_t dy = mouse_dy_;
    mouse_dx_ = 0;
    mouse_dy_ = 0;

    const double target_x = MouseDeltaToVirtualStick(static_cast<double>(dx));
    const double target_y = MouseDeltaToVirtualStick(static_cast<double>(-dy));
    const bool has_mouse_input = dx != 0 || dy != 0;
    const double time_constant_ms =
        has_mouse_input ? REXCVAR_GET(mnk_mouse_stick_smoothing_ms)
                        : REXCVAR_GET(mnk_mouse_stick_hold_ms);

    right_stick_x_ = SmoothAxis(right_stick_x_, target_x, dt_ms, time_constant_ms);
    right_stick_y_ = SmoothAxis(right_stick_y_, target_y, dt_ms, time_constant_ms);
    if (std::abs(right_stick_x_) < kStickIdleSnap) {
      right_stick_x_ = 0.0;
    }
    if (std::abs(right_stick_y_) < kStickIdleSnap) {
      right_stick_y_ = 0.0;
    }

    rx = static_cast<int32_t>(std::lround(right_stick_x_));
    ry = static_cast<int32_t>(std::lround(right_stick_y_));
  } else if (!menu_mouse_mode && REXCVAR_GET(mnk_raw_mouse)) {
    mouse_dx_ = 0;
    mouse_dy_ = 0;
    right_stick_x_ = 0.0;
    right_stick_y_ = 0.0;
    right_stick_last_poll_ = {};
  } else {
    double sensitivity = REXCVAR_GET(mnk_sensitivity);
    constexpr double kBaseScale = 200.0;
    rx = static_cast<int32_t>(mouse_dx_ * sensitivity * kBaseScale);
    ry = static_cast<int32_t>(-mouse_dy_ * sensitivity * kBaseScale);
    mouse_dx_ = 0;
    mouse_dy_ = 0;
  }

  auto clamp16 = [](int32_t v) -> int16_t {
    return static_cast<int16_t>(std::clamp(v, (int32_t)INT16_MIN, (int32_t)INT16_MAX));
  };

  packet_number_++;

  if (out_state) {
    out_state->packet_number = packet_number_;
    out_state->gamepad.buttons = buttons;
    out_state->gamepad.left_trigger = lt;
    out_state->gamepad.right_trigger = rt;
    out_state->gamepad.thumb_lx = clamp16(lx);
    out_state->gamepad.thumb_ly = clamp16(ly);
    out_state->gamepad.thumb_rx = clamp16(rx);
    out_state->gamepad.thumb_ry = clamp16(ry);
  }
  return X_ERROR_SUCCESS;
}

X_RESULT MnkInputDriver::SetState(uint32_t user_index, X_INPUT_VIBRATION* vibration) {
  (void)vibration;
  if (!IsEnabled() || user_index != UserIndex()) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  return X_ERROR_SUCCESS;
}

X_RESULT MnkInputDriver::GetKeystroke(uint32_t user_index, uint32_t flags,
                                      X_INPUT_KEYSTROKE* out_keystroke) {
  (void)flags;
  if ((!IsKeyboardPassthroughEnabled() && !IsEnabled()) || user_index != UserIndex()) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  std::lock_guard lock(state_mutex_);
  FlushQueuedKeystrokes();
  if (keystroke_queue_.empty()) {
    return X_ERROR_EMPTY;
  }
  if (out_keystroke) {
    *out_keystroke = keystroke_queue_.front();
  }
  keystroke_queue_.pop();
  return X_ERROR_SUCCESS;
}

void MnkInputDriver::EnqueueKeystroke(uint16_t vk, uint16_t unicode, uint16_t flags) {
  X_INPUT_KEYSTROKE ks = {};
  ks.virtual_key = vk;
  ks.unicode = unicode;
  ks.flags = flags;
  ks.user_index = static_cast<uint8_t>(UserIndex());
  ks.hid_code = 0;
  pending_keystroke_queue_.push(ks);
}

void MnkInputDriver::FlushQueuedKeystrokes() {
  while (!pending_keystroke_queue_.empty()) {
    keystroke_queue_.push(pending_keystroke_queue_.front());
    pending_keystroke_queue_.pop();
  }
}

void MnkInputDriver::CenterCursor() {
  if (!attached_window_)
    return;
  int32_t cx = static_cast<int32_t>(attached_window_->GetActualLogicalWidth() / 2);
  int32_t cy = static_cast<int32_t>(attached_window_->GetActualLogicalHeight() / 2);
  prev_mouse_x_ = cx;
  prev_mouse_y_ = cy;
#if REX_PLATFORM_WIN32
  auto* win32_window = dynamic_cast<rex::ui::Win32Window*>(attached_window_);
  if (win32_window && win32_window->hwnd()) {
    POINT pt = {static_cast<LONG>(cx), static_cast<LONG>(cy)};
    ClientToScreen(win32_window->hwnd(), &pt);
    SetCursorPos(pt.x, pt.y);
  }
#endif
}

void MnkInputDriver::RefreshMenuMousePositionFromOS() {
#if REX_PLATFORM_WIN32
  if (!attached_window_ || mouse_capture_requested_ || mouse_captured_) {
    return;
  }

  auto* win32_window = dynamic_cast<rex::ui::Win32Window*>(attached_window_);
  if (!win32_window || !win32_window->hwnd()) {
    return;
  }

  POINT cursor_pos = {};
  if (!GetCursorPos(&cursor_pos) || !ScreenToClient(win32_window->hwnd(), &cursor_pos)) {
    return;
  }

  RECT client_rect = {};
  if (!GetClientRect(win32_window->hwnd(), &client_rect)) {
    return;
  }

  if (cursor_pos.x < client_rect.left || cursor_pos.x >= client_rect.right ||
      cursor_pos.y < client_rect.top || cursor_pos.y >= client_rect.bottom) {
    menu_mouse_has_position_ = false;
    menu_mouse_dx_ = 0;
    menu_mouse_dy_ = 0;
    MarkMenuHoverDirty();
    return;
  }

  if (menu_mouse_has_position_) {
    menu_mouse_dx_ += cursor_pos.x - menu_mouse_x_;
    if (!REXCVAR_GET(mnk_menu_hover_nav)) {
      menu_mouse_dy_ += cursor_pos.y - menu_mouse_y_;
    }
  }
  menu_mouse_has_position_ = true;
  menu_mouse_x_ = cursor_pos.x;
  menu_mouse_y_ = cursor_pos.y;
#endif
}

void MnkInputDriver::EnsureMenuVirtualCursorInitialized() {
  if (menu_virtual_cursor_initialized_ || !attached_window_) {
    return;
  }

  double viewport_x = 0.0;
  double viewport_y = 0.0;
  double viewport_width = 0.0;
  double viewport_height = 0.0;
  if (!GetMenuHoverViewport(attached_window_, viewport_x, viewport_y, viewport_width,
                            viewport_height)) {
    return;
  }

  double initial_x = viewport_x + (viewport_width * 0.5);
  double initial_y = viewport_y + (viewport_height * 0.5);
#if REX_PLATFORM_WIN32
  auto* win32_window = dynamic_cast<rex::ui::Win32Window*>(attached_window_);
  if (win32_window && win32_window->hwnd()) {
    POINT cursor_pos = {};
    if (GetCursorPos(&cursor_pos) && ScreenToClient(win32_window->hwnd(), &cursor_pos)) {
      initial_x = static_cast<double>(cursor_pos.x);
      initial_y = static_cast<double>(cursor_pos.y);
    }
  }
#endif

  menu_virtual_cursor_x_ =
      std::clamp(initial_x, viewport_x, viewport_x + std::max(viewport_width - 1.0, 0.0));
  menu_virtual_cursor_y_ =
      std::clamp(initial_y, viewport_y, viewport_y + std::max(viewport_height - 1.0, 0.0));
  menu_virtual_cursor_initialized_ = true;
  SetMenuMousePositionFromVirtualCursor();
}

void MnkInputDriver::MoveMenuVirtualCursor(double delta_x, double delta_y) {
  if (!REXCVAR_GET(mnk_menu_virtual_cursor)) {
    return;
  }

  EnsureMenuVirtualCursorInitialized();
  if (!menu_virtual_cursor_initialized_ || !attached_window_) {
    return;
  }

  double viewport_x = 0.0;
  double viewport_y = 0.0;
  double viewport_width = 0.0;
  double viewport_height = 0.0;
  if (!GetMenuHoverViewport(attached_window_, viewport_x, viewport_y, viewport_width,
                            viewport_height)) {
    return;
  }

  const int32_t old_x = static_cast<int32_t>(std::lround(menu_virtual_cursor_x_));
  const int32_t old_y = static_cast<int32_t>(std::lround(menu_virtual_cursor_y_));
  const double sensitivity = REXCVAR_GET(mnk_menu_virtual_cursor_sensitivity);
  menu_virtual_cursor_x_ =
      std::clamp(menu_virtual_cursor_x_ + (delta_x * sensitivity), viewport_x,
                 viewport_x + std::max(viewport_width - 1.0, 0.0));
  menu_virtual_cursor_y_ =
      std::clamp(menu_virtual_cursor_y_ + (delta_y * sensitivity), viewport_y,
                 viewport_y + std::max(viewport_height - 1.0, 0.0));

  const int32_t new_x = static_cast<int32_t>(std::lround(menu_virtual_cursor_x_));
  const int32_t new_y = static_cast<int32_t>(std::lround(menu_virtual_cursor_y_));
  menu_mouse_dx_ += new_x - old_x;
  if (!REXCVAR_GET(mnk_menu_hover_nav)) {
    menu_mouse_dy_ += new_y - old_y;
  }
  SetMenuMousePositionFromVirtualCursor();
}

void MnkInputDriver::SetMenuMousePositionFromVirtualCursor() {
  menu_mouse_has_position_ = menu_virtual_cursor_initialized_;
  if (!menu_virtual_cursor_initialized_) {
    return;
  }
  menu_mouse_x_ = static_cast<int32_t>(std::lround(menu_virtual_cursor_x_));
  menu_mouse_y_ = static_cast<int32_t>(std::lround(menu_virtual_cursor_y_));
}

void MnkInputDriver::WarpOSCursorToVirtualCursor() {
#if REX_PLATFORM_WIN32
  if (!REXCVAR_GET(mnk_menu_virtual_cursor) ||
      !REXCVAR_GET(mnk_menu_virtual_cursor_warp_os) || !menu_virtual_cursor_initialized_ ||
      !attached_window_ || mouse_capture_requested_ || mouse_captured_) {
    return;
  }

  auto* win32_window = dynamic_cast<rex::ui::Win32Window*>(attached_window_);
  if (!win32_window || !win32_window->hwnd()) {
    return;
  }

  POINT pt = {static_cast<LONG>(std::lround(menu_virtual_cursor_x_)),
              static_cast<LONG>(std::lround(menu_virtual_cursor_y_))};
  ClientToScreen(win32_window->hwnd(), &pt);
  SetCursorPos(pt.x, pt.y);
#endif
}

void MnkInputDriver::UpdateMouseClip() {
#if REX_PLATFORM_WIN32
  if (!attached_window_ || !has_focus_) {
    ReleaseMouseClip();
    return;
  }

  auto* win32_window = dynamic_cast<rex::ui::Win32Window*>(attached_window_);
  if (!win32_window || !win32_window->hwnd()) {
    ReleaseMouseClip();
    return;
  }

  RECT client_rect = {};
  if (!GetClientRect(win32_window->hwnd(), &client_rect) ||
      client_rect.right <= client_rect.left || client_rect.bottom <= client_rect.top) {
    ReleaseMouseClip();
    return;
  }

  POINT top_left = {client_rect.left, client_rect.top};
  POINT bottom_right = {client_rect.right, client_rect.bottom};
  if (!ClientToScreen(win32_window->hwnd(), &top_left) ||
      !ClientToScreen(win32_window->hwnd(), &bottom_right)) {
    ReleaseMouseClip();
    return;
  }

  RECT screen_rect = {top_left.x, top_left.y, bottom_right.x, bottom_right.y};
  if (mouse_clip_active_ && mouse_clip_left_ == screen_rect.left &&
      mouse_clip_top_ == screen_rect.top && mouse_clip_right_ == screen_rect.right &&
      mouse_clip_bottom_ == screen_rect.bottom) {
    return;
  }

  if (ClipCursor(&screen_rect)) {
    mouse_clip_active_ = true;
    mouse_clip_left_ = screen_rect.left;
    mouse_clip_top_ = screen_rect.top;
    mouse_clip_right_ = screen_rect.right;
    mouse_clip_bottom_ = screen_rect.bottom;
  } else {
    mouse_clip_active_ = false;
  }
#endif
}

void MnkInputDriver::ReleaseMouseClip() {
#if REX_PLATFORM_WIN32
  if (mouse_clip_active_) {
    ClipCursor(nullptr);
    mouse_clip_active_ = false;
  }
#else
  mouse_clip_active_ = false;
#endif
}

void MnkInputDriver::UpdateAutoMouseCaptureFromGameInput() {
  if (!IsEnabled() || !has_focus_ || !is_active() || !attached_window_) {
    return;
  }

  uint32_t catcher_mask = 0;
  uint32_t generation = 0;
  if (!QueryGameKeyCatcherMask(&catcher_mask, &generation)) {
    return;
  }

  std::lock_guard lock(state_mutex_);
  if (generation == game_key_catcher_generation_) {
    return;
  }
  game_key_catcher_generation_ = generation;

  if (catcher_mask == 0) {
    if (escape_menu_release_active_) {
      return;
    }
    if (REXCVAR_GET(mnk_auto_capture_gameplay) && !mouse_capture_requested_) {
      mouse_capture_requested_ = true;
      mouse_dx_ = 0;
      mouse_dy_ = 0;
      right_stick_x_ = 0.0;
      right_stick_y_ = 0.0;
      right_stick_last_poll_ = {};
      ClearNativeMouseMovement();
      ResetMenuHoverState();
    }
    return;
  }

  escape_menu_release_active_ = false;
  if (REXCVAR_GET(mnk_auto_release_on_key_catcher) && mouse_capture_requested_) {
    mouse_capture_requested_ = false;
    mouse_dx_ = 0;
    mouse_dy_ = 0;
    right_stick_x_ = 0.0;
    right_stick_y_ = 0.0;
    right_stick_last_poll_ = {};
    ClearNativeMouseMovement();
    ResetMenuHoverState();
  }
}

void MnkInputDriver::ClearMenuNavigation() {
  while (!menu_nav_queue_.empty()) {
    menu_nav_queue_.pop();
  }
  menu_hover_nav_cooldown_frames_ = 0;
  menu_nav_left_frames_ = 0;
  menu_nav_right_frames_ = 0;
  menu_nav_up_frames_ = 0;
  menu_nav_down_frames_ = 0;
}

bool MnkInputDriver::HasPendingMenuNavigation() const {
  return !menu_nav_queue_.empty() || menu_hover_nav_cooldown_frames_ != 0 ||
         menu_nav_left_frames_ != 0 || menu_nav_right_frames_ != 0 ||
         menu_nav_up_frames_ != 0 || menu_nav_down_frames_ != 0;
}

void MnkInputDriver::QueueMenuNavigation(MenuNavDirection direction, int32_t steps) {
  for (int32_t i = 0; i < steps; ++i) {
    menu_nav_queue_.push(direction);
  }
}

void MnkInputDriver::PumpMenuNavigation() {
  if (menu_hover_nav_cooldown_frames_ > 0) {
    --menu_hover_nav_cooldown_frames_;
  }

  if (menu_hover_nav_cooldown_frames_ != 0 || menu_nav_left_frames_ != 0 ||
      menu_nav_right_frames_ != 0 || menu_nav_up_frames_ != 0 || menu_nav_down_frames_ != 0 ||
      menu_nav_queue_.empty()) {
    return;
  }

  const MenuNavDirection direction = menu_nav_queue_.front();
  menu_nav_queue_.pop();
  switch (direction) {
    case MenuNavDirection::kUp:
      menu_nav_up_frames_ = kMenuNavPulseFrames;
      break;
    case MenuNavDirection::kDown:
      menu_nav_down_frames_ = kMenuNavPulseFrames;
      break;
    case MenuNavDirection::kLeft:
      menu_nav_left_frames_ = kMenuNavPulseFrames;
      break;
    case MenuNavDirection::kRight:
      menu_nav_right_frames_ = kMenuNavPulseFrames;
      break;
  }
}

void MnkInputDriver::ScheduleMenuHoverRow(int32_t target_row, bool force_reanchor) {
  const int32_t row_count = std::max(REXCVAR_GET(mnk_menu_hover_row_count), 1);
  target_row = std::clamp(target_row, 0, row_count - 1);
  const int32_t current_row = std::clamp(menu_hover_selected_row_, 0, row_count - 1);
  if (!force_reanchor && !menu_hover_dirty_ && menu_hover_target_column_ < 0 &&
      menu_hover_target_row_ == target_row) {
    return;
  }

  ClearMenuNavigation();
  menu_hover_target_row_ = target_row;
  menu_hover_target_column_ = -1;

  const int32_t delta = target_row - current_row;
  if (delta != 0) {
    QueueMenuNavigation(delta < 0 ? MenuNavDirection::kUp : MenuNavDirection::kDown,
                        std::abs(delta));
    menu_hover_selected_row_ = target_row;
  } else {
    menu_hover_selected_row_ = target_row;
  }

  menu_hover_dirty_ = false;
}

void MnkInputDriver::ScheduleMenuHoverColumn(int32_t target_column, bool force_reanchor) {
  const int32_t column_count = std::max(REXCVAR_GET(mnk_menu_carousel_column_count), 1);
  target_column = std::clamp(target_column, 0, column_count - 1);
  const int32_t current_column =
      std::clamp(menu_hover_selected_column_, 0, column_count - 1);
  if (!force_reanchor && !menu_hover_dirty_ && menu_hover_target_row_ < 0 &&
      menu_hover_target_column_ == target_column) {
    return;
  }

  ClearMenuNavigation();
  menu_hover_target_row_ = -1;
  menu_hover_target_column_ = target_column;

  const int32_t delta = target_column - current_column;
  if (delta != 0) {
    QueueMenuNavigation(delta < 0 ? MenuNavDirection::kLeft : MenuNavDirection::kRight,
                        std::abs(delta));
    menu_hover_selected_column_ = target_column;
  } else {
    menu_hover_selected_column_ = target_column;
  }

  menu_hover_dirty_ = false;
}

void MnkInputDriver::MarkMenuHoverDirty() {
  menu_hover_dirty_ = true;
  menu_hover_target_row_ = -1;
  menu_hover_target_column_ = -1;
}

void MnkInputDriver::ResetMenuHoverSelectionAnchor() {
  const int32_t row_count = std::max(REXCVAR_GET(mnk_menu_hover_row_count), 1);
  const int32_t column_count = std::max(REXCVAR_GET(mnk_menu_carousel_column_count), 1);
  menu_hover_selected_row_ =
      std::clamp(REXCVAR_GET(mnk_menu_hover_initial_row), 0, row_count - 1);
  menu_hover_selected_column_ =
      std::clamp(REXCVAR_GET(mnk_menu_carousel_initial_column), 0, column_count - 1);
}

void MnkInputDriver::InvalidateMenuHoverForTransition() {
  ClearMenuNavigation();
  menu_select_pending_after_nav_ = false;
  ResetMenuHoverSelectionAnchor();
  MarkMenuHoverDirty();
  menu_mouse_dx_ = 0;
  menu_mouse_dy_ = 0;
  menu_direct_hover_miss_frames_ = 0;
  menu_transition_cooldown_frames_ =
      static_cast<uint32_t>(std::max(REXCVAR_GET(mnk_menu_transition_cooldown_frames), 0));
}

void MnkInputDriver::ResetMenuHoverState() {
  ClearMenuPointerVirtualPosition();
  ClearMenuMouseButtons();
  menu_select_down_ = false;
  menu_mouse_has_position_ = false;
  menu_mouse_dx_ = 0;
  menu_mouse_dy_ = 0;
  menu_direct_hover_miss_frames_ = 0;
  menu_virtual_cursor_initialized_ = false;
  menu_virtual_cursor_x_ = 0.0;
  menu_virtual_cursor_y_ = 0.0;
  ResetMenuHoverSelectionAnchor();
  menu_transition_cooldown_frames_ = 0;
  menu_select_pending_after_nav_ = false;
  menu_select_pulse_frames_ = 0;
  ClearMenuNavigation();
  MarkMenuHoverDirty();
}

void MnkInputDriver::UpdateMouseCapture() {
  if (!attached_window_)
    return;

  bool should_capture = IsEnabled() && has_focus_ && is_active() && mouse_capture_requested_;

  if (should_capture && !mouse_captured_) {
    mouse_captured_ = true;
    attached_window_->SetCursorVisibility(rex::ui::Window::CursorVisibility::kHidden);
    attached_window_->CaptureMouse();
    UpdateMouseClip();
    CenterCursor();
    mouse_dx_ = 0;
    mouse_dy_ = 0;
    right_stick_x_ = 0.0;
    right_stick_y_ = 0.0;
    right_stick_last_poll_ = {};
    ClearNativeMouseMovement();
  } else if (!should_capture && mouse_captured_) {
    ReleaseMouseCaptureNow();
  }

  if (mouse_captured_) {
    UpdateMouseClip();
  }

  // Re-center cursor each frame while captured to prevent edge clamping
  if (mouse_captured_ && !REXCVAR_GET(mnk_raw_mouse)) {
    CenterCursor();
  }
}

void MnkInputDriver::ReleaseMouseCaptureNow() {
  ReleaseMouseClip();
  if (mouse_captured_ && attached_window_) {
    mouse_captured_ = false;
    attached_window_->SetCursorVisibility(rex::ui::Window::CursorVisibility::kVisible);
    attached_window_->ReleaseMouse();
  } else if (attached_window_) {
    attached_window_->SetCursorVisibility(rex::ui::Window::CursorVisibility::kVisible);
  } else {
    mouse_captured_ = false;
  }
  mouse_dx_ = 0;
  mouse_dy_ = 0;
  right_stick_x_ = 0.0;
  right_stick_y_ = 0.0;
  right_stick_last_poll_ = {};
  ClearNativeMouseMovement();
}

void MnkInputDriver::SetKeyState(uint16_t vk, bool down) {
  if (vk < 256) {
    key_down_[vk] = down;
  }
}

void MnkInputDriver::OnKeyDown(rex::ui::KeyEvent& e) {
  if (!has_focus_)
    return;
  std::lock_guard lock(state_mutex_);
  uint16_t vk = static_cast<uint16_t>(e.virtual_key());
  const bool is_escape = e.virtual_key() == VirtualKey::kEscape;
  const bool escape_is_start_bind = is_escape && IsBindKey(e.virtual_key(), REXCVAR_GET(keybind_start));
  const bool suppress_keyboard_passthrough = IsEnabled() && escape_is_start_bind;

  if (!suppress_keyboard_passthrough && IsKeyboardPassthroughEnabled() &&
      IsKeyboardVirtualKey(e.virtual_key())) {
    uint16_t key_flags = X_INPUT_KEYSTROKE_KEYDOWN;
    if (e.prev_state() || e.repeat_count() > 1) {
      key_flags |= X_INPUT_KEYSTROKE_REPEAT;
    }
    const uint16_t unicode = TranslateUnicodeForKeyEvent(e);
    if (unicode >= 0x20 && unicode < 0x7F && e.virtual_key() != VirtualKey::kOem3) {
      key_flags |= X_INPUT_KEYSTROKE_VALID_UNICODE;
    }
    EnqueueKeystroke(vk, unicode, key_flags);
  }

  if (IsEnabled()) {
    const bool was_menu_mouse_mode = !mouse_capture_requested_;
    if (is_escape) {
      const bool was_capture_active = mouse_capture_requested_ || mouse_captured_;
      SetKeyState(vk, false);
      mouse_capture_requested_ = false;
      escape_menu_release_active_ = was_capture_active;
      ResetMenuHoverState();
      ReleaseMouseCaptureNow();
      if (escape_is_start_bind && !escape_start_suppressed_until_key_up_ &&
          !e.prev_state()) {
        start_pulse_frames_ = kEscapeStartPulseFrames;
        escape_start_suppressed_until_key_up_ = true;
      }
    } else if (IsCaptureActivationKey(e.virtual_key())) {
      SetKeyState(vk, true);
      mouse_capture_requested_ = true;
      escape_menu_release_active_ = false;
      ResetMenuHoverState();
    } else if (was_menu_mouse_mode && !e.prev_state() &&
               IsMenuTransitionKey(e.virtual_key())) {
      SetKeyState(vk, true);
      InvalidateMenuHoverForTransition();
    } else if (was_menu_mouse_mode && IsMenuControlKey(e.virtual_key())) {
      SetKeyState(vk, true);
      ClearMenuNavigation();
      menu_select_pending_after_nav_ = false;
      menu_select_pulse_frames_ = 0;
      MarkMenuHoverDirty();
    } else {
      SetKeyState(vk, true);
    }
  }
}

void MnkInputDriver::OnKeyUp(rex::ui::KeyEvent& e) {
  std::lock_guard lock(state_mutex_);
  uint16_t vk = static_cast<uint16_t>(e.virtual_key());
  const bool suppress_keyboard_passthrough =
      IsEnabled() && e.virtual_key() == VirtualKey::kEscape &&
      escape_start_suppressed_until_key_up_;

  if (!suppress_keyboard_passthrough && IsKeyboardPassthroughEnabled() && has_focus_ &&
      IsKeyboardVirtualKey(e.virtual_key())) {
    EnqueueKeystroke(vk, 0, X_INPUT_KEYSTROKE_KEYUP);
  }

  if (IsEnabled()) {
    SetKeyState(vk, false);
    if (e.virtual_key() == VirtualKey::kEscape) {
      escape_start_suppressed_until_key_up_ = false;
    }
  }
}

void MnkInputDriver::OnKeyChar(rex::ui::KeyEvent& e) {
  (void)e;
}

void MnkInputDriver::OnMouseDown(rex::ui::MouseEvent& e) {
  if (!IsEnabled() || !has_focus_)
    return;
  std::lock_guard lock(state_mutex_);
  if (!mouse_capture_requested_) {
    if (REXCVAR_GET(mnk_menu_virtual_cursor)) {
      EnsureMenuVirtualCursorInitialized();
      SetMenuMousePositionFromVirtualCursor();
    } else {
      menu_mouse_has_position_ = true;
      menu_mouse_x_ = e.x();
      menu_mouse_y_ = e.y();
    }
  }
  switch (e.button()) {
    case rex::ui::MouseEvent::Button::kLeft: {
      SetKeyState(static_cast<uint16_t>(VirtualKey::kLButton), true);
      SetMenuMouseButtonState(kMenuMouseButtonLeft, true);
      if (!mouse_capture_requested_) {
        menu_mouse_dx_ = 0;
        menu_mouse_dy_ = 0;
        menu_select_down_ = false;

        bool select_after_sync = false;
        uint32_t menu_pointer_generation = 0;
        const bool direct_hover_pointer =
            REXCVAR_GET(mnk_menu_direct_hover) &&
            QueryMenuPointerVirtualPosition(nullptr, nullptr,
                                            &menu_pointer_generation);
        const bool direct_hover_click =
            direct_hover_pointer &&
            (REXCVAR_GET(mnk_menu_direct_hover_cursor_only) ||
             !REXCVAR_GET(mnk_menu_direct_hover_suppress_fallback) ||
             QueryMenuDirectHoverApplied(menu_pointer_generation));
        const bool direct_hover_fallback_click_sync =
            direct_hover_pointer &&
            REXCVAR_GET(mnk_menu_direct_hover_suppress_fallback) &&
            !REXCVAR_GET(mnk_menu_direct_hover_block_fallback_on_miss) &&
            menu_direct_hover_miss_frames_ >=
                static_cast<uint32_t>(std::max(REXCVAR_GET(
                                          mnk_menu_direct_hover_fallback_miss_frames),
                                          0));
        const bool direct_hover_miss_blocks_click =
            direct_hover_pointer &&
            REXCVAR_GET(mnk_menu_direct_hover_suppress_fallback) &&
            REXCVAR_GET(mnk_menu_direct_hover_block_fallback_on_miss) &&
            !direct_hover_click;
        if (direct_hover_click) {
          ClearMenuNavigation();
          menu_hover_target_row_ = -1;
          menu_hover_target_column_ = -1;
        } else if ((!direct_hover_pointer || direct_hover_fallback_click_sync) &&
                   REXCVAR_GET(mnk_menu_click_sync)) {
          const int32_t target_column =
              ClassifyMenuHoverColumn(attached_window_, menu_mouse_x_, menu_mouse_y_);
          if (target_column >= 0) {
            ScheduleMenuHoverColumn(target_column, true);
            select_after_sync = true;
          } else if (REXCVAR_GET(mnk_menu_hover_nav)) {
            const int32_t target_row =
                ClassifyMenuHoverRow(attached_window_, menu_mouse_x_, menu_mouse_y_);
            if (target_row >= 0) {
              ScheduleMenuHoverRow(target_row, true);
              select_after_sync = true;
            }
          }
        }

        if (select_after_sync) {
          menu_select_pending_after_nav_ = true;
          menu_select_pulse_frames_ = 0;
        } else if (direct_hover_miss_blocks_click) {
          ClearMenuNavigation();
          menu_hover_target_row_ = -1;
          menu_hover_target_column_ = -1;
          menu_select_pending_after_nav_ = false;
          menu_select_down_ = false;
        } else {
          menu_select_down_ = true;
          InvalidateMenuHoverForTransition();
        }
      }
      break;
    }
    case rex::ui::MouseEvent::Button::kRight:
      SetKeyState(static_cast<uint16_t>(VirtualKey::kRButton), true);
      SetMenuMouseButtonState(kMenuMouseButtonRight, true);
      if (!QueryForceMenuMouseMode()) {
        mouse_capture_requested_ = true;
        escape_menu_release_active_ = false;
        ResetMenuHoverState();
      }
      break;
    case rex::ui::MouseEvent::Button::kMiddle:
      SetKeyState(static_cast<uint16_t>(VirtualKey::kMButton), true);
      SetMenuMouseButtonState(kMenuMouseButtonMiddle, true);
      if (!QueryForceMenuMouseMode()) {
        mouse_capture_requested_ = true;
        escape_menu_release_active_ = false;
        ResetMenuHoverState();
      }
      break;
    default:
      break;
  }
}

void MnkInputDriver::OnMouseUp(rex::ui::MouseEvent& e) {
  if (!IsEnabled())
    return;
  std::lock_guard lock(state_mutex_);
  switch (e.button()) {
    case rex::ui::MouseEvent::Button::kLeft:
      SetKeyState(static_cast<uint16_t>(VirtualKey::kLButton), false);
      SetMenuMouseButtonState(kMenuMouseButtonLeft, false);
      menu_select_down_ = false;
      break;
    case rex::ui::MouseEvent::Button::kRight:
      SetKeyState(static_cast<uint16_t>(VirtualKey::kRButton), false);
      SetMenuMouseButtonState(kMenuMouseButtonRight, false);
      break;
    case rex::ui::MouseEvent::Button::kMiddle:
      SetKeyState(static_cast<uint16_t>(VirtualKey::kMButton), false);
      SetMenuMouseButtonState(kMenuMouseButtonMiddle, false);
      break;
    default:
      break;
  }
}

void MnkInputDriver::OnMouseMove(rex::ui::MouseEvent& e) {
  if (!IsEnabled() || !has_focus_)
    return;
  std::lock_guard lock(state_mutex_);
  int32_t x = e.x();
  int32_t y = e.y();
  if (!mouse_captured_) {
    if (!mouse_capture_requested_) {
      if (REXCVAR_GET(mnk_menu_virtual_cursor)) {
        EnsureMenuVirtualCursorInitialized();
        SetMenuMousePositionFromVirtualCursor();
      } else {
        menu_mouse_has_position_ = true;
        menu_mouse_x_ = x;
        menu_mouse_y_ = y;
        menu_mouse_dx_ += x - prev_mouse_x_;
        if (!REXCVAR_GET(mnk_menu_hover_nav)) {
          menu_mouse_dy_ += y - prev_mouse_y_;
        }
      }
    }
    prev_mouse_x_ = x;
    prev_mouse_y_ = y;
    return;
  }
  if (!REXCVAR_GET(mnk_raw_mouse)) {
    mouse_dx_ += x - prev_mouse_x_;
    mouse_dy_ += y - prev_mouse_y_;
  }
  prev_mouse_x_ = x;
  prev_mouse_y_ = y;
}

void MnkInputDriver::OnRawMouseMove(rex::ui::RawMouseEvent& e) {
  if (!IsEnabled() || !has_focus_ || !is_active() || !REXCVAR_GET(mnk_raw_mouse))
    return;
  std::lock_guard lock(state_mutex_);
  if (!mouse_capture_requested_) {
    if (REXCVAR_GET(mnk_menu_virtual_cursor)) {
      MoveMenuVirtualCursor(e.delta_x(), e.delta_y());
      WarpOSCursorToVirtualCursor();
      e.set_handled(true);
    }
    return;
  }
  if (REXCVAR_GET(mnk_native_mouse_look)) {
    AccumulateNativeMouseMovement(e.delta_x(), e.delta_y());
  } else {
    mouse_dx_ += e.delta_x();
    mouse_dy_ += e.delta_y();
  }
  e.set_handled(true);
}

void MnkInputDriver::OnLostFocus(rex::ui::UISetupEvent&) {
  std::lock_guard lock(state_mutex_);
  restore_mouse_capture_on_focus_ = mouse_capture_requested_ || mouse_captured_;
  has_focus_ = false;
  mouse_capture_requested_ = false;
  escape_menu_release_active_ = false;
  ResetMenuHoverState();
  std::memset(key_down_, 0, sizeof(key_down_));
  mouse_dx_ = 0;
  mouse_dy_ = 0;
  start_pulse_frames_ = 0;
  escape_start_suppressed_until_key_up_ = false;
  right_stick_x_ = 0.0;
  right_stick_y_ = 0.0;
  right_stick_last_poll_ = {};
  ClearNativeMouseMovement();
  while (!keystroke_queue_.empty()) {
    keystroke_queue_.pop();
  }
  while (!pending_keystroke_queue_.empty()) {
    pending_keystroke_queue_.pop();
  }
  ReleaseMouseClip();
  if (mouse_captured_ && attached_window_) {
    mouse_captured_ = false;
    attached_window_->SetCursorVisibility(rex::ui::Window::CursorVisibility::kVisible);
    attached_window_->ReleaseMouse();
  }
}

void MnkInputDriver::OnGotFocus(rex::ui::UISetupEvent&) {
  bool restore_capture = false;
  {
    std::lock_guard lock(state_mutex_);
    has_focus_ = true;
    restore_capture = restore_mouse_capture_on_focus_;
    restore_mouse_capture_on_focus_ = false;
    if (restore_capture && IsEnabled()) {
      mouse_capture_requested_ = true;
      escape_menu_release_active_ = false;
      mouse_dx_ = 0;
      mouse_dy_ = 0;
      right_stick_x_ = 0.0;
      right_stick_y_ = 0.0;
      right_stick_last_poll_ = {};
      ClearNativeMouseMovement();
    } else {
      ResetMenuHoverState();
    }
  }

  if (restore_capture) {
    UpdateMouseCapture();
  }
}

}  // namespace rex::input::mnk
