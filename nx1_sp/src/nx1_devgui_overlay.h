#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <rex/ui/imgui_dialog.h>

namespace rex::ui {
class ImGuiDrawer;
}  // namespace rex::ui

namespace nx1::sp {

struct DevGuiOverlayItem {
  uint16_t handle = 0;
  uint16_t parent_handle = 0;
  uint16_t first_child_handle = 0;
  uint16_t next_handle = 0;
  uint16_t previous_handle = 0;
  uint8_t type = 0;
  uint8_t child_index = 0;
  uint32_t payload = 0;
  int depth = 0;
  std::string name;
};

struct DevGuiOverlaySnapshot {
  bool active = false;
  bool editing = false;
  uint16_t root_handle = 0;
  uint16_t selected_handle = 0;
  uint64_t generation = 0;
  std::vector<DevGuiOverlayItem> items;
};

enum class DevGuiOverlayCommandKind {
  kSelect,
  kAccept,
  kReject,
};

struct DevGuiOverlayCommand {
  DevGuiOverlayCommandKind kind = DevGuiOverlayCommandKind::kSelect;
  uint16_t handle = 0;
};

void PublishDevGuiOverlaySnapshot(DevGuiOverlaySnapshot snapshot);
DevGuiOverlaySnapshot GetDevGuiOverlaySnapshot();
void QueueDevGuiOverlaySelect(uint16_t handle);
void QueueDevGuiOverlayAccept(uint16_t handle);
void QueueDevGuiOverlayReject();
bool ConsumeDevGuiOverlayCommand(DevGuiOverlayCommand* out_command);

std::unique_ptr<rex::ui::ImGuiDialog> CreateDevGuiOverlayDialog(
    rex::ui::ImGuiDrawer* drawer);

}  // namespace nx1::sp
