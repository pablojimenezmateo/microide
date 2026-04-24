#pragma once

#include <SDL3/SDL.h>

#include <cstddef>
#include <vector>

namespace microide::workspace {

enum class DragTarget {
  None,
  SidebarDivider,
  BottomPanelDivider,
  CompareDivider,
  MergeLeftDivider,
  MergeRightDivider,
  SidebarScrollbar,
  BottomPanelScrollbar,
  OverlayScrollbar,
  EditorVerticalScrollbar,
  EditorHorizontalScrollbar,
  EditorSplitDivider,
  CompareVerticalScrollbar,
  CompareHorizontalScrollbar,
};

enum class TabDragKind {
  None,
  Project,
  Editor,
  Terminal,
};

struct TabDragState {
  TabDragKind kind = TabDragKind::None;
  float press_x = 0.0f;
  float press_y = 0.0f;
  bool dragging = false;
  bool reordered = false;
};

struct InteractionState {
  bool window_has_input_focus = true;
  bool mouse_selecting = false;
  DragTarget drag_target = DragTarget::None;
  float drag_scrollbar_offset = 0.0f;
  std::vector<std::size_t> drag_editor_split_path;
  std::size_t drag_editor_split_divider_index = 0;
  TabDragState tab_drag;
  Uint64 last_title_bar_click_ms = 0;
  float last_title_bar_click_x = 0.0f;
  float last_title_bar_click_y = 0.0f;
};

}  // namespace microide::workspace
