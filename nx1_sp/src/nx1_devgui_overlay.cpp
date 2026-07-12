#include "nx1_devgui_overlay.h"

#include <algorithm>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>

#include <imgui.h>
#include <rex/cvar.h>
#include <rex/ui/imgui_drawer.h>

namespace nx1::sp {
namespace {

std::mutex g_devgui_overlay_mutex;
DevGuiOverlaySnapshot g_devgui_overlay_snapshot;
std::deque<DevGuiOverlayCommand> g_devgui_overlay_commands;

const char* TypeLabel(uint8_t type) {
  switch (type) {
    case 0:
      return "menu";
    case 1:
      return "dvar";
    case 2:
      return "command";
    case 3:
      return "graph";
    default:
      return "item";
  }
}

class DevGuiOverlayDialog final : public rex::ui::ImGuiDialog {
 public:
  explicit DevGuiOverlayDialog(rex::ui::ImGuiDrawer* drawer)
      : rex::ui::ImGuiDialog(drawer) {}

 protected:
  void OnDraw(ImGuiIO& io) override {
    (void)io;
    if (!rex::cvar::Query<bool>("nx1_devgui_native_overlay")) {
      return;
    }

    DevGuiOverlaySnapshot snapshot = GetDevGuiOverlaySnapshot();
    if (!snapshot.active) {
      last_hovered_handle_ = 0;
      return;
    }

    std::unordered_map<uint16_t, size_t> item_by_handle;
    item_by_handle.reserve(snapshot.items.size());
    for (size_t i = 0; i < snapshot.items.size(); ++i) {
      item_by_handle.emplace(snapshot.items[i].handle, i);
    }

    const DevGuiOverlayItem* selected_item =
        FindItem(snapshot.selected_handle, snapshot, item_by_handle);
    const uint16_t current_parent_handle =
        selected_item != nullptr && selected_item->parent_handle != 0
            ? selected_item->parent_handle
            : snapshot.root_handle;

    std::vector<const DevGuiOverlayItem*> path;
    BuildPath(current_parent_handle, snapshot, item_by_handle, path);

    ImGui::SetNextWindowPos(ImVec2(24.0f, 56.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(430.0f, 560.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowBgAlpha(0.86f);
    if (!ImGui::Begin("NX1 DevGui##native", nullptr,
                      ImGuiWindowFlags_NoCollapse)) {
      ImGui::End();
      return;
    }
    if ((ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) &&
         ImGui::IsMouseReleased(ImGuiMouseButton_Right)) ||
        ImGui::IsKeyPressed(ImGuiKey_Escape)) {
      QueueDevGuiOverlayReject();
    }

    ImGui::TextUnformatted("NX1 DevGui");
    ImGui::SameLine();
    ImGui::TextDisabled("#%llu",
                        static_cast<unsigned long long>(snapshot.generation));

    if (ImGui::Button("Back")) {
      QueueDevGuiOverlayReject();
    }
    ImGui::SameLine();
    if (selected_item != nullptr) {
      ImGui::TextDisabled("selected: %s (%s)", selected_item->name.c_str(),
                          TypeLabel(selected_item->type));
    } else {
      ImGui::TextDisabled("selected: none");
    }
    if (snapshot.editing) {
      ImGui::SameLine();
      ImGui::TextColored(ImVec4(1.0f, 0.82f, 0.28f, 1.0f),
                         "editing dvar");
    }

    if (!path.empty()) {
      ImGui::SeparatorText("Path");
      for (size_t i = 0; i < path.size(); ++i) {
        if (i != 0) {
          ImGui::SameLine();
          ImGui::TextUnformatted("/");
          ImGui::SameLine();
        }
        ImGui::PushID(static_cast<int>(path[i]->handle));
        if (!snapshot.editing && ImGui::SmallButton(path[i]->name.c_str())) {
          QueueDevGuiOverlayAccept(path[i]->handle);
        } else if (snapshot.editing) {
          ImGui::TextDisabled("%s", path[i]->name.c_str());
        }
        ImGui::PopID();
      }
    }

    ImGui::SeparatorText("Items");
    ImGui::BeginChild("##nx1_devgui_items", ImVec2(0, 0), true);
    uint32_t rendered_children = 0;
    for (const DevGuiOverlayItem& item : snapshot.items) {
      if (item.parent_handle != current_parent_handle) {
        continue;
      }
      ++rendered_children;

      const bool is_selected = item.handle == snapshot.selected_handle;
      std::string label = item.name.empty() ? "<unnamed>" : item.name;
      if (item.first_child_handle != 0) {
        label += " >";
      }
      label += "##";
      label += std::to_string(item.handle);

      ImGui::PushID(static_cast<int>(item.handle));
      if (item.type != 0) {
        ImGui::TextColored(ImVec4(0.58f, 0.78f, 1.0f, 1.0f), "%s",
                           TypeLabel(item.type));
        ImGui::SameLine(82.0f);
      } else {
        ImGui::TextDisabled("%s", TypeLabel(item.type));
        ImGui::SameLine(82.0f);
      }

      const bool selection_locked =
          snapshot.editing && item.handle != snapshot.selected_handle;
      if (ImGui::Selectable(label.c_str(), is_selected,
                            ImGuiSelectableFlags_AllowDoubleClick,
                            ImVec2(0.0f, 0.0f))) {
        if (!selection_locked) {
          QueueDevGuiOverlayAccept(item.handle);
        }
        last_hovered_handle_ = item.handle;
      } else if (!selection_locked && !snapshot.editing &&
                 ImGui::IsItemHovered() &&
                 last_hovered_handle_ != item.handle) {
        QueueDevGuiOverlaySelect(item.handle);
        last_hovered_handle_ = item.handle;
      }
      ImGui::PopID();
    }

    if (rendered_children == 0) {
      ImGui::TextDisabled("No child items for this menu.");
    }
    ImGui::EndChild();

    if (!ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows)) {
      last_hovered_handle_ = 0;
    }
    ImGui::End();
  }

 private:
  static const DevGuiOverlayItem* FindItem(
      uint16_t handle, const DevGuiOverlaySnapshot& snapshot,
      const std::unordered_map<uint16_t, size_t>& item_by_handle) {
    auto it = item_by_handle.find(handle);
    if (it == item_by_handle.end()) {
      return nullptr;
    }
    return &snapshot.items[it->second];
  }

  static void BuildPath(
      uint16_t handle, const DevGuiOverlaySnapshot& snapshot,
      const std::unordered_map<uint16_t, size_t>& item_by_handle,
      std::vector<const DevGuiOverlayItem*>& path) {
    path.clear();
    for (uint32_t guard = 0; handle != 0 && guard < 64; ++guard) {
      const DevGuiOverlayItem* item = FindItem(handle, snapshot, item_by_handle);
      if (item == nullptr) {
        break;
      }
      path.push_back(item);
      handle = item->parent_handle;
    }
    std::reverse(path.begin(), path.end());
  }

  uint16_t last_hovered_handle_ = 0;
};

void QueueCommand(DevGuiOverlayCommand command) {
  std::lock_guard lock(g_devgui_overlay_mutex);
  if (command.kind == DevGuiOverlayCommandKind::kSelect &&
      !g_devgui_overlay_commands.empty()) {
    DevGuiOverlayCommand& last = g_devgui_overlay_commands.back();
    if (last.kind == DevGuiOverlayCommandKind::kSelect) {
      last = command;
      return;
    }
  }
  g_devgui_overlay_commands.push_back(command);
}

}  // namespace

void PublishDevGuiOverlaySnapshot(DevGuiOverlaySnapshot snapshot) {
  std::lock_guard lock(g_devgui_overlay_mutex);
  g_devgui_overlay_snapshot = std::move(snapshot);
}

DevGuiOverlaySnapshot GetDevGuiOverlaySnapshot() {
  std::lock_guard lock(g_devgui_overlay_mutex);
  return g_devgui_overlay_snapshot;
}

void QueueDevGuiOverlaySelect(uint16_t handle) {
  QueueCommand({DevGuiOverlayCommandKind::kSelect, handle});
}

void QueueDevGuiOverlayAccept(uint16_t handle) {
  QueueCommand({DevGuiOverlayCommandKind::kAccept, handle});
}

void QueueDevGuiOverlayReject() {
  QueueCommand({DevGuiOverlayCommandKind::kReject, 0});
}

bool ConsumeDevGuiOverlayCommand(DevGuiOverlayCommand* out_command) {
  std::lock_guard lock(g_devgui_overlay_mutex);
  if (g_devgui_overlay_commands.empty()) {
    return false;
  }
  if (out_command != nullptr) {
    *out_command = g_devgui_overlay_commands.front();
  }
  g_devgui_overlay_commands.pop_front();
  return true;
}

std::unique_ptr<rex::ui::ImGuiDialog> CreateDevGuiOverlayDialog(
    rex::ui::ImGuiDrawer* drawer) {
  return std::make_unique<DevGuiOverlayDialog>(drawer);
}

}  // namespace nx1::sp
