#include "workspace/coordinators/WorkspaceSidebarMouseCoordinator.h"

#include <cmath>
#include <string>

#include "workspace/EditorBodyDrop.h"
#include "workspace/SidebarMetrics.h"
#include "util/PathMatch.h"
#include "util/InvokeIfSet.h"
#include "workspace/services/SidebarService.h"

namespace microide::workspace {

namespace {

constexpr float kPathDragThresholdPx = 4.0f;

}

bool SidebarMouseCoordinator::HandlePathDragMotion(float x, float y, const WorkspaceLayout& layout) {
  PathDragState& drag = interaction_state_.path_drag;
  if (!drag.armed) {
    return false;
  }
  drag.pointer_x = x;
  drag.pointer_y = y;
  if (!drag.dragging) {
    if (std::hypot(x - drag.press_x, y - drag.press_y) < kPathDragThresholdPx) {
      return false;
    }
    drag.dragging = true;
  }
  ResolvePathDropTarget(layout);
  return true;
}

void SidebarMouseCoordinator::ResolvePathDropTarget(const WorkspaceLayout& layout) {
  PathDragState& drag = interaction_state_.path_drag;
  drag.target = PathDragState::Target::None;
  drag.zone = EditorBodyDropZone::None;
  drag.folder.clear();
  drag.drop_rect = SDL_FRect{0.0f, 0.0f, 0.0f, 0.0f};
  const float x = drag.pointer_x;
  const float y = drag.pointer_y;

  if (!drag.is_directory && operations_.compute_editor_group_rects) {
    const EditorGroupRectsLayout rects = operations_.compute_editor_group_rects(layout);
    for (std::size_t gi = 0; gi < rects.groups.size(); ++gi) {
      const EditorGroupRects& group = rects.groups[gi];
      const bool on_strip = Contains(group.tab_strip, x, y);
      if (!on_strip && !Contains(group.editor_surface, x, y)) {
        continue;
      }
      const bool can_split = !on_strip && !(operations_.editor_area_full && operations_.editor_area_full());
      const EditorBodyDrop drop = EditorBodyDropAt(group.editor_surface, x, y, can_split);
      drag.target = PathDragState::Target::EditorGroup;
      drag.group_index = gi;
      drag.zone = drop.zone;
      drag.drop_rect = drop.region;
      return;
    }
  }

  if (operations_.terminal_drop_rect) {
    if (const auto terminal = operations_.terminal_drop_rect(layout); terminal.has_value() && Contains(*terminal, x, y)) {
      drag.target = PathDragState::Target::Terminal;
      drag.drop_rect = *terminal;
      return;
    }
  }

  if (operations_.active_sidebar_mode() != SidebarMode::Tree || !Contains(layout.sidebar, x, y)) {
    return;
  }
  const auto& entries = state_.directory_tree.entries();
  const auto list = operations_.compute_tree_sidebar_list_layout(layout.sidebar, entries.size());
  const auto row = ScrollableListItemAtY(list, y, entries.size());
  std::filesystem::path folder = state_.root;
  int folder_row = -1;
  if (row.has_value()) {
    const auto& entry = entries[*row];
    folder = entry.is_directory ? entry.path : entry.path.parent_path();
    for (int i = static_cast<int>(*row); i >= 0; --i) {
      if (entries[static_cast<std::size_t>(i)].path == folder) {
        folder_row = i;
        break;
      }
    }
  }
  const std::filesystem::path source = drag.path.lexically_normal();
  folder = folder.lexically_normal();
  const std::filesystem::path inside = folder.lexically_relative(source);
  const bool into_itself = drag.is_directory && !inside.empty() && !util::RelativePathClimbsOut(inside);
  if (folder == source.parent_path() || into_itself) {
    return;
  }
  drag.target = PathDragState::Target::TreeFolder;
  drag.folder = folder;
  drag.drop_rect = folder_row >= 0 ? ScrollableListRowRect(list, folder_row - list.scroll_row) : list.list_rect;
}

bool SidebarMouseCoordinator::FinishPathDrag(bool copy) {
  PathDragState drag = interaction_state_.path_drag;
  interaction_state_.path_drag = PathDragState{};
  if (!drag.armed) {
    return false;
  }
  if (!drag.dragging) {
    ActivateTreeSelection();
    return true;
  }
  switch (drag.target) {
    case PathDragState::Target::EditorGroup:
      if (drag.zone == EditorBodyDropZone::Center || drag.zone == EditorBodyDropZone::None) {
        if (operations_.open_path_in_group) {
          (void)operations_.open_path_in_group(drag.path, drag.group_index);
        }
      } else if (operations_.open_path_in_new_group) {
        const bool side_by_side = drag.zone == EditorBodyDropZone::Left || drag.zone == EditorBodyDropZone::Right;
        const bool before = drag.zone == EditorBodyDropZone::Left || drag.zone == EditorBodyDropZone::Top;
        (void)operations_.open_path_in_new_group(
            drag.path, drag.group_index,
            side_by_side ? EditorSplitOrientation::Vertical : EditorSplitOrientation::Horizontal, before);
      }
      break;
    case PathDragState::Target::TreeFolder:
      if (operations_.paste_tree_path) {
        (void)operations_.paste_tree_path(drag.path, !copy, drag.folder);
      }
      break;
    case PathDragState::Target::Terminal:
      util::InvokeIfSet(operations_.type_path_into_terminal, drag.path);
      break;
    case PathDragState::Target::None:
      break;
  }
  return true;
}

bool SidebarMouseCoordinator::CancelPathDrag() {
  const bool was_dragging = interaction_state_.path_drag.dragging;
  interaction_state_.path_drag = PathDragState{};
  return was_dragging;
}

void SidebarMouseCoordinator::ActivateTreeSelection() {
  const auto opened = state_.directory_tree.ActivateSelection();
  services_.sidebar.RevealSelectedTreeLine();
  if (opened.has_value()) {
    operations_.open_file(*opened);
    state_.surface.focus = FocusTarget::Sidebar;
  }
}

}
