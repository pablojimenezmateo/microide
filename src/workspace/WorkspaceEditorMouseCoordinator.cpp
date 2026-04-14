#include "workspace/WorkspaceEditorMouseCoordinator.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "editor/EditorViewRenderer.h"
#include "workspace/WorkspaceShellShared.h"

namespace microide::workspace {

WorkspaceShell::EditorMouseCoordinator::EditorMouseCoordinator(WorkspaceShell& shell)
    : shell_(shell) {}

bool WorkspaceShell::EditorMouseCoordinator::HandleButtonDown(
    const SDL_Event& event,
    const WorkspaceLayout& layout) {
  const auto dividers = shell_.ComputeEditorSplitDividerLayouts(layout.editor_surface);
  const auto divider_it = std::find_if(
      dividers.begin(), dividers.end(), [&](const EditorSplitDividerLayout& divider) {
        return Contains(divider.rect, event.button.x, event.button.y);
      });
  if (divider_it != dividers.end()) {
    shell_.surface_.drag_target = DragTarget::EditorSplitDivider;
    shell_.surface_.drag_editor_split_path = divider_it->node_path;
    shell_.surface_.drag_editor_split_divider_index = divider_it->divider_index;
    shell_.surface_.focus = FocusTarget::Editor;
    return true;
  }

  const auto panes = shell_.ComputeEditorPaneLayouts(layout.editor_surface);
  const auto pane_it =
      std::find_if(panes.begin(), panes.end(), [&](const EditorPaneLayout& pane) {
        return Contains(pane.rect, event.button.x, event.button.y);
      });
  if (pane_it == panes.end()) {
    return false;
  }
  if (!pane_it->active) {
    shell_.SetActiveEditorSplit(pane_it->leaf_id);
  }
  const SDL_FRect editor_rect = pane_it->rect;

  const editor::EditorViewMetrics metrics =
      editor::EditorViewRenderer::ComputeMetrics(shell_.text_renderer_,
                                                 shell_.text_viewport_, editor_rect);
  shell_.text_viewport_.SetViewportSize(metrics.visible_rows, metrics.visible_columns);

  const auto scroll_layout =
      shell_.ComputeEditorScrollLayout(editor_rect, shell_.text_viewport_, metrics);
  if (scroll_layout.vertical_scrollbar.has_value() &&
      Contains(scroll_layout.vertical_scrollbar->track, event.button.x, event.button.y)) {
    shell_.surface_.drag_target = DragTarget::EditorVerticalScrollbar;
    shell_.surface_.drag_scrollbar_offset =
        Contains(scroll_layout.vertical_scrollbar->thumb, event.button.x, event.button.y)
            ? static_cast<float>(event.button.y) -
                  scroll_layout.vertical_scrollbar->thumb.y
            : scroll_layout.vertical_scrollbar->thumb.h * 0.5f;
    shell_.text_viewport_.SetScrollLine(static_cast<std::size_t>(std::max(
        0L, std::lround(ScrollUnitsForPointer(*scroll_layout.vertical_scrollbar,
                                              static_cast<float>(event.button.y),
                                              shell_.surface_.drag_scrollbar_offset)))));
    shell_.surface_.focus = FocusTarget::Editor;
    return true;
  }
  if (scroll_layout.horizontal_scrollbar.has_value() &&
      Contains(scroll_layout.horizontal_scrollbar->track, event.button.x, event.button.y)) {
    shell_.surface_.drag_target = DragTarget::EditorHorizontalScrollbar;
    shell_.surface_.drag_scrollbar_offset =
        Contains(scroll_layout.horizontal_scrollbar->thumb, event.button.x, event.button.y)
            ? static_cast<float>(event.button.x) -
                  scroll_layout.horizontal_scrollbar->thumb.x
            : scroll_layout.horizontal_scrollbar->thumb.w * 0.5f;
    shell_.text_viewport_.SetHorizontalScroll(static_cast<std::size_t>(std::max(
        0L, std::lround(ScrollUnitsForPointer(*scroll_layout.horizontal_scrollbar,
                                              static_cast<float>(event.button.x),
                                              shell_.surface_.drag_scrollbar_offset)))));
    shell_.surface_.focus = FocusTarget::Editor;
    return true;
  }

  const float local_y = std::max(0.0f, event.button.y - metrics.first_line_y);
  const std::size_t row = static_cast<std::size_t>(local_y / metrics.line_height);
  const std::size_t line =
      std::min(shell_.text_viewport_.scroll_line() + row,
               shell_.text_viewport_.line_count() == 0
                   ? 0
                   : shell_.text_viewport_.line_count() - 1);
  const float text_offset_x = std::max(0.0f, event.button.x - metrics.text_x);
  const std::size_t visual_column =
      shell_.text_viewport_.horizontal_scroll() +
      static_cast<std::size_t>(std::max(
          0L, std::lround(text_offset_x /
                          std::max(1.0f, shell_.text_renderer_.CharWidth()))));

  shell_.text_viewport_.MoveCursorToVisualColumn(
      line, visual_column, (SDL_GetModState() & SDL_KMOD_SHIFT) != 0);
  shell_.ResetCaretBlink();
  shell_.surface_.focus = FocusTarget::Editor;
  shell_.surface_.mouse_selecting = true;
  return true;
}

bool WorkspaceShell::EditorMouseCoordinator::HandleDrag(const SDL_Event& event,
                                                        const WorkspaceLayout& layout) {
  if (shell_.surface_.drag_target == DragTarget::EditorSplitDivider) {
    auto* editor_tab = shell_.ActiveEditorTab();
    if (editor_tab == nullptr || editor_tab->views.size() < 2 ||
        editor_tab->split_root == nullptr) {
      shell_.surface_.drag_target = DragTarget::None;
      return false;
    }

    shell_.NormalizeEditorSplitTree(*editor_tab);
    auto* split_node = shell_.FindEditorSplitNode(
        editor_tab->split_root.get(), shell_.surface_.drag_editor_split_path);
    const auto node_rect = shell_.ComputeEditorSplitNodeRect(
        layout.editor_surface, shell_.surface_.drag_editor_split_path);
    if (split_node == nullptr || node_rect == std::nullopt || split_node->IsLeaf() ||
        split_node->orientation == EditorSplitOrientation::None ||
        shell_.surface_.drag_editor_split_divider_index + 1 >=
            split_node->children.size()) {
      shell_.surface_.drag_target = DragTarget::None;
      return false;
    }

    const bool vertical =
        split_node->orientation == EditorSplitOrientation::Vertical;
    std::vector<float> size_fractions(split_node->children.size(), 0.0f);
    for (std::size_t i = 0; i < split_node->children.size(); ++i) {
      size_fractions[i] = split_node->children[i]->size_fraction;
    }
    const auto split_layout =
        ComputeEditorSplitAxisLayout(*node_rect, vertical, size_fractions);
    if (!split_layout.has_value() || split_layout->total_extent <= 0.0f ||
        split_layout->extents.size() != split_node->children.size()) {
      return false;
    }

    float before_extent = 0.0f;
    for (std::size_t i = 0; i < shell_.surface_.drag_editor_split_divider_index; ++i) {
      before_extent += split_layout->extents[i];
    }
    const float pair_extent =
        split_layout->extents[shell_.surface_.drag_editor_split_divider_index] +
        split_layout
            ->extents[shell_.surface_.drag_editor_split_divider_index + 1];
    const float min_extent =
        split_layout->total_extent >
                split_layout->min_pane_extent *
                    static_cast<float>(split_layout->extents.size())
            ? split_layout->min_pane_extent
            : 0.0f;
    float leading_extent =
        vertical
            ? static_cast<float>(event.motion.x) - node_rect->x - before_extent -
                  split_layout->divider_thickness *
                      static_cast<float>(
                          shell_.surface_.drag_editor_split_divider_index) -
                  split_layout->divider_thickness * 0.5f
            : static_cast<float>(event.motion.y) - node_rect->y - before_extent -
                  split_layout->divider_thickness *
                      static_cast<float>(
                          shell_.surface_.drag_editor_split_divider_index) -
                  split_layout->divider_thickness * 0.5f;
    leading_extent =
        pair_extent <= min_extent * 2.0f
            ? std::clamp(leading_extent, 0.0f, pair_extent)
            : std::clamp(leading_extent, min_extent, pair_extent - min_extent);
    const float trailing_extent = std::max(0.0f, pair_extent - leading_extent);
    split_node->children[shell_.surface_.drag_editor_split_divider_index]
        ->size_fraction = leading_extent / split_layout->total_extent;
    split_node->children[shell_.surface_.drag_editor_split_divider_index + 1]
        ->size_fraction = trailing_extent / split_layout->total_extent;
    shell_.NormalizeEditorSplitNode(*split_node);
    shell_.surface_.focus = FocusTarget::Editor;
    return true;
  }

  if (shell_.surface_.drag_target != DragTarget::EditorVerticalScrollbar &&
      shell_.surface_.drag_target != DragTarget::EditorHorizontalScrollbar) {
    return false;
  }

  const auto panes = shell_.ComputeEditorPaneLayouts(layout.editor_surface);
  const auto active_pane = std::find_if(
      panes.begin(), panes.end(), [](const EditorPaneLayout& pane) { return pane.active; });
  const SDL_FRect editor_rect =
      active_pane != panes.end() ? active_pane->rect : layout.editor_surface;
  const editor::EditorViewMetrics metrics =
      editor::EditorViewRenderer::ComputeMetrics(shell_.text_renderer_,
                                                 shell_.text_viewport_, editor_rect);
  shell_.text_viewport_.SetViewportSize(metrics.visible_rows, metrics.visible_columns);
  const auto scroll_layout =
      shell_.ComputeEditorScrollLayout(editor_rect, shell_.text_viewport_, metrics);

  if (shell_.surface_.drag_target == DragTarget::EditorVerticalScrollbar) {
    if (!scroll_layout.vertical_scrollbar.has_value()) {
      shell_.surface_.drag_target = DragTarget::None;
      shell_.surface_.drag_scrollbar_offset = 0.0f;
      return false;
    }
    shell_.text_viewport_.SetScrollLine(static_cast<std::size_t>(std::max(
        0L, std::lround(ScrollUnitsForPointer(*scroll_layout.vertical_scrollbar,
                                              static_cast<float>(event.motion.y),
                                              shell_.surface_.drag_scrollbar_offset)))));
  } else {
    if (!scroll_layout.horizontal_scrollbar.has_value()) {
      shell_.surface_.drag_target = DragTarget::None;
      shell_.surface_.drag_scrollbar_offset = 0.0f;
      return false;
    }
    shell_.text_viewport_.SetHorizontalScroll(static_cast<std::size_t>(std::max(
        0L, std::lround(ScrollUnitsForPointer(*scroll_layout.horizontal_scrollbar,
                                              static_cast<float>(event.motion.x),
                                              shell_.surface_.drag_scrollbar_offset)))));
  }
  shell_.surface_.focus = FocusTarget::Editor;
  return true;
}

bool WorkspaceShell::EditorMouseCoordinator::HandleSelectionMotion(
    const SDL_Event& event,
    const WorkspaceLayout& layout) {
  const auto panes = shell_.ComputeEditorPaneLayouts(layout.editor_surface);
  const auto active_pane = std::find_if(
      panes.begin(), panes.end(), [](const EditorPaneLayout& pane) { return pane.active; });
  const SDL_FRect editor_rect =
      active_pane != panes.end() ? active_pane->rect : layout.editor_surface;
  if (!Contains(editor_rect, event.motion.x, event.motion.y)) {
    return false;
  }

  const editor::EditorViewMetrics metrics =
      editor::EditorViewRenderer::ComputeMetrics(shell_.text_renderer_,
                                                 shell_.text_viewport_, editor_rect);
  shell_.text_viewport_.SetViewportSize(metrics.visible_rows, metrics.visible_columns);

  const float local_y = std::max(0.0f, event.motion.y - metrics.first_line_y);
  const std::size_t row = static_cast<std::size_t>(local_y / metrics.line_height);
  const std::size_t line =
      std::min(shell_.text_viewport_.scroll_line() + row,
               shell_.text_viewport_.line_count() == 0
                   ? 0
                   : shell_.text_viewport_.line_count() - 1);
  const float text_offset_x = std::max(0.0f, event.motion.x - metrics.text_x);
  const std::size_t visual_column =
      shell_.text_viewport_.horizontal_scroll() +
      static_cast<std::size_t>(std::max(
          0L, std::lround(text_offset_x /
                          std::max(1.0f, shell_.text_renderer_.CharWidth()))));

  shell_.text_viewport_.MoveCursorToVisualColumn(line, visual_column, true);
  shell_.ResetCaretBlink();
  shell_.surface_.focus = FocusTarget::Editor;
  return true;
}

bool WorkspaceShell::EditorMouseCoordinator::HandleWheel(const SDL_Event& event,
                                                         const WorkspaceLayout& layout,
                                                         int vertical_ticks) {
  if (!Contains(layout.editor_surface, event.wheel.mouse_x, event.wheel.mouse_y)) {
    return false;
  }

  const auto panes = shell_.ComputeEditorPaneLayouts(layout.editor_surface);
  const auto hovered_pane =
      std::find_if(panes.begin(), panes.end(), [&](const EditorPaneLayout& pane) {
        return Contains(pane.rect, event.wheel.mouse_x, event.wheel.mouse_y);
      });
  if (hovered_pane != panes.end() && !hovered_pane->active) {
    shell_.SetActiveEditorSplit(hovered_pane->leaf_id);
  }
  shell_.text_viewport_.ScrollVertical(-vertical_ticks * 3);
  shell_.surface_.focus = FocusTarget::Editor;
  return true;
}

}  // namespace microide::workspace
