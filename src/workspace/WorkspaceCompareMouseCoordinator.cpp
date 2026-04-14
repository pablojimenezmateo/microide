#include "workspace/WorkspaceCompareMouseCoordinator.h"

#include <algorithm>
#include <cmath>

#include "workspace/WorkspaceShellShared.h"

namespace microide::workspace {

WorkspaceShell::CompareMouseCoordinator::CompareMouseCoordinator(WorkspaceShell& shell)
    : shell_(shell) {}

bool WorkspaceShell::CompareMouseCoordinator::HandleButtonDown(
    const SDL_Event& event,
    const WorkspaceLayout& layout) {
  if (!shell_.ActiveTabIsCompare()) {
    return false;
  }

  CompareTabState* compare_tab = shell_.ActiveCompareTab();
  if (compare_tab == nullptr) {
    return false;
  }

  const CompareSurfaceLayout surface_layout =
      shell_.ComputeCompareSurfaceLayout(layout.editor_surface, *compare_tab);
  const auto scroll_layout =
      shell_.ComputeCompareScrollLayout(layout.editor_surface, surface_layout, *compare_tab);
  compare_tab->scroll_row = scroll_layout.vertical_scroll;
  compare_tab->horizontal_scroll = scroll_layout.horizontal_scroll;
  shell_.SyncCompareViewportScroll(*compare_tab);

  if (scroll_layout.vertical_scrollbar.has_value() &&
      Contains(scroll_layout.vertical_scrollbar->track, event.button.x, event.button.y)) {
    shell_.surface_.drag_target = DragTarget::CompareVerticalScrollbar;
    shell_.surface_.drag_scrollbar_offset =
        Contains(scroll_layout.vertical_scrollbar->thumb, event.button.x, event.button.y)
            ? static_cast<float>(event.button.y) -
                  scroll_layout.vertical_scrollbar->thumb.y
            : scroll_layout.vertical_scrollbar->thumb.h * 0.5f;
    const int target_scroll = std::clamp(
        static_cast<int>(std::lround(ScrollUnitsForPointer(
            *scroll_layout.vertical_scrollbar, static_cast<float>(event.button.y),
            shell_.surface_.drag_scrollbar_offset))),
        0, scroll_layout.max_vertical_scroll);
    compare_tab->scroll_row = target_scroll;
    shell_.SyncCompareViewportScroll(*compare_tab);
    shell_.surface_.focus = FocusTarget::Editor;
    return true;
  }

  if (scroll_layout.horizontal_scrollbar.has_value() &&
      Contains(scroll_layout.horizontal_scrollbar->track, event.button.x, event.button.y)) {
    shell_.surface_.drag_target = DragTarget::CompareHorizontalScrollbar;
    shell_.surface_.drag_scrollbar_offset =
        Contains(scroll_layout.horizontal_scrollbar->thumb, event.button.x, event.button.y)
            ? static_cast<float>(event.button.x) -
                  scroll_layout.horizontal_scrollbar->thumb.x
            : scroll_layout.horizontal_scrollbar->thumb.w * 0.5f;
    compare_tab->horizontal_scroll = static_cast<std::size_t>(std::max(
        0L, std::lround(ScrollUnitsForPointer(*scroll_layout.horizontal_scrollbar,
                                              static_cast<float>(event.button.x),
                                              shell_.surface_.drag_scrollbar_offset))));
    shell_.SyncCompareViewportScroll(*compare_tab);
    shell_.surface_.focus = FocusTarget::Editor;
    return true;
  }

  const int clicked_row =
      static_cast<int>((event.button.y - surface_layout.rows_y) /
                       surface_layout.line_height);
  const int model_row = compare_tab->scroll_row + clicked_row;
  if (clicked_row < 0 || model_row < 0 ||
      model_row >= static_cast<int>(compare_tab->model.rows.size())) {
    compare_tab->right_view_active = false;
    return false;
  }

  compare_tab->selected_row = static_cast<std::size_t>(model_row);
  if (compare_tab->right_editable && event.button.x >= surface_layout.right_x) {
    compare_tab->right_view_active = true;
    const TextGridInteractionLayout right_interaction =
        shell_.BuildCompareRightInteractionLayout(surface_layout, *compare_tab);
    const std::size_t line =
        shell_.CompareRightLineForRow(*compare_tab, compare_tab->selected_row);
    const std::size_t visual_column =
        TextGridVisualColumnAtX(right_interaction, event.button.x);
    compare_tab->right_viewport.MoveCursorToVisualColumn(
        line, visual_column, (SDL_GetModState() & SDL_KMOD_SHIFT) != 0);
    shell_.SyncCompareSelectionFromViewport(*compare_tab, false);
    shell_.ResetCaretBlink();
    shell_.surface_.mouse_selecting = true;
  } else {
    compare_tab->right_view_active = false;
  }
  shell_.surface_.focus = FocusTarget::Editor;
  return true;
}

bool WorkspaceShell::CompareMouseCoordinator::HandleDrag(
    const SDL_Event& event,
    const WorkspaceLayout& layout) {
  if (!shell_.ActiveTabIsCompare() ||
      (shell_.surface_.drag_target != DragTarget::CompareVerticalScrollbar &&
       shell_.surface_.drag_target != DragTarget::CompareHorizontalScrollbar)) {
    return false;
  }

  CompareTabState* compare_tab = shell_.ActiveCompareTab();
  if (compare_tab == nullptr) {
    shell_.surface_.drag_target = DragTarget::None;
    shell_.surface_.drag_scrollbar_offset = 0.0f;
    return false;
  }

  const CompareSurfaceLayout surface_layout =
      shell_.ComputeCompareSurfaceLayout(layout.editor_surface, *compare_tab);
  const auto scroll_layout =
      shell_.ComputeCompareScrollLayout(layout.editor_surface, surface_layout, *compare_tab);
  compare_tab->scroll_row = scroll_layout.vertical_scroll;
  compare_tab->horizontal_scroll = scroll_layout.horizontal_scroll;
  if (shell_.surface_.drag_target == DragTarget::CompareVerticalScrollbar) {
    if (!scroll_layout.vertical_scrollbar.has_value()) {
      shell_.surface_.drag_target = DragTarget::None;
      shell_.surface_.drag_scrollbar_offset = 0.0f;
      return false;
    }
    const int target_scroll = std::clamp(
        static_cast<int>(std::lround(ScrollUnitsForPointer(
            *scroll_layout.vertical_scrollbar, static_cast<float>(event.motion.y),
            shell_.surface_.drag_scrollbar_offset))),
        0, scroll_layout.max_vertical_scroll);
    compare_tab->scroll_row = target_scroll;
    shell_.SyncCompareViewportScroll(*compare_tab);
    shell_.surface_.focus = FocusTarget::Editor;
    return true;
  }

  if (!scroll_layout.horizontal_scrollbar.has_value()) {
    shell_.surface_.drag_target = DragTarget::None;
    shell_.surface_.drag_scrollbar_offset = 0.0f;
    return false;
  }
  compare_tab->horizontal_scroll = static_cast<std::size_t>(std::max(
      0L, std::lround(ScrollUnitsForPointer(*scroll_layout.horizontal_scrollbar,
                                            static_cast<float>(event.motion.x),
                                            shell_.surface_.drag_scrollbar_offset))));
  shell_.SyncCompareViewportScroll(*compare_tab);
  shell_.surface_.focus = FocusTarget::Editor;
  return true;
}

bool WorkspaceShell::CompareMouseCoordinator::HandleSelectionMotion(
    const SDL_Event& event,
    const WorkspaceLayout& layout) {
  if (!shell_.ActiveTabIsCompare()) {
    return false;
  }

  CompareTabState* compare_tab = shell_.ActiveCompareTab();
  if (compare_tab == nullptr || !compare_tab->right_editable ||
      !compare_tab->right_view_active) {
    return false;
  }

  const CompareSurfaceLayout surface_layout =
      shell_.ComputeCompareSurfaceLayout(layout.editor_surface, *compare_tab);
  if (event.motion.x < surface_layout.right_x) {
    return false;
  }
  const TextGridInteractionLayout right_interaction =
      shell_.BuildCompareRightInteractionLayout(surface_layout, *compare_tab);
  const auto hovered_row = VisibleTextGridLineAtY(right_interaction, event.motion.y);
  if (!hovered_row.has_value()) {
    return false;
  }

  const std::size_t line =
      shell_.CompareRightLineForRow(*compare_tab, *hovered_row);
  const std::size_t visual_column =
      TextGridVisualColumnAtX(right_interaction, event.motion.x);
  compare_tab->right_viewport.MoveCursorToVisualColumn(line, visual_column, true);
  shell_.SyncCompareSelectionFromViewport(*compare_tab, false);
  shell_.ResetCaretBlink();
  shell_.surface_.focus = FocusTarget::Editor;
  return true;
}

bool WorkspaceShell::CompareMouseCoordinator::HandleWheel(const SDL_Event& event,
                                                          const WorkspaceLayout& layout,
                                                          int vertical_ticks,
                                                          int horizontal_ticks) {
  if (!shell_.ActiveTabIsCompare() ||
      !Contains(layout.editor_surface, event.wheel.mouse_x, event.wheel.mouse_y)) {
    return false;
  }

  if (horizontal_ticks != 0) {
    shell_.ScrollCompareColumns(-horizontal_ticks * 3);
  } else {
    shell_.ScrollCompareRows(-vertical_ticks * 3);
  }
  if (auto* compare_tab = shell_.ActiveCompareTab(); compare_tab != nullptr) {
    shell_.SyncCompareViewportScroll(*compare_tab);
  }
  shell_.surface_.focus = FocusTarget::Editor;
  return true;
}

}  // namespace microide::workspace
