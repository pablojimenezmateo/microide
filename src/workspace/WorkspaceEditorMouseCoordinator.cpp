#include "workspace/WorkspaceEditorMouseCoordinator.h"

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

#include "editor/EditorViewRenderer.h"
#include "workspace/WorkspaceLayout.h"

namespace microide::workspace {

EditorMouseCoordinator::EditorMouseCoordinator(ProjectWorkspaceState& state,
                                               InteractionState& interaction_state,
                                               render::TextRenderer& text_renderer,
                                               Operations operations)
    : state_(state),
      interaction_state_(interaction_state),
      text_renderer_(text_renderer),
      operations_(std::move(operations)) {}

bool EditorMouseCoordinator::HandleButtonDown(const SDL_Event& event,
                                              const WorkspaceLayout& layout) {
  const auto dividers = operations_.compute_editor_split_divider_layouts(layout.editor_surface);
  const auto divider_it = std::find_if(
      dividers.begin(), dividers.end(),
      [&](const WorkspaceShell::EditorSplitDividerLayout& divider) {
        return Contains(divider.rect, event.button.x, event.button.y);
      });
  if (divider_it != dividers.end()) {
    interaction_state_.drag_target = DragTarget::EditorSplitDivider;
    interaction_state_.drag_editor_split_path = divider_it->node_path;
    interaction_state_.drag_editor_split_divider_index = divider_it->divider_index;
    state_.surface.focus = FocusTarget::Editor;
    return true;
  }

  const auto panes = operations_.compute_editor_pane_layouts(layout.editor_surface);
  const auto pane_it =
      std::find_if(panes.begin(), panes.end(),
                   [&](const WorkspaceShell::EditorPaneLayout& pane) {
                     return Contains(pane.rect, event.button.x, event.button.y);
                   });
  if (pane_it == panes.end()) {
    return false;
  }
  if (!pane_it->active) {
    operations_.set_active_editor_split(pane_it->leaf_id);
  }
  const SDL_FRect editor_rect = pane_it->rect;
  editor::TextViewport* viewport = operations_.active_editor_viewport();
  if (viewport == nullptr) {
    return false;
  }

  const editor::EditorViewMetrics metrics =
      editor::EditorViewRenderer::ComputeMetrics(text_renderer_, *viewport, editor_rect);
  viewport->SetViewportSize(metrics.visible_rows, metrics.visible_columns);

  const auto scroll_layout = operations_.compute_editor_scroll_layout(editor_rect, *viewport, metrics);
  if (scroll_layout.vertical_scrollbar.has_value() &&
      Contains(scroll_layout.vertical_scrollbar->track, event.button.x, event.button.y)) {
    interaction_state_.drag_target = DragTarget::EditorVerticalScrollbar;
    interaction_state_.drag_scrollbar_offset =
        Contains(scroll_layout.vertical_scrollbar->thumb, event.button.x, event.button.y)
            ? static_cast<float>(event.button.y) - scroll_layout.vertical_scrollbar->thumb.y
            : scroll_layout.vertical_scrollbar->thumb.h * 0.5f;
    viewport->SetScrollLine(static_cast<std::size_t>(std::max(
        0L, std::lround(ScrollUnitsForPointer(*scroll_layout.vertical_scrollbar,
                                              static_cast<float>(event.button.y),
                                              interaction_state_.drag_scrollbar_offset)))));
    state_.surface.focus = FocusTarget::Editor;
    return true;
  }
  if (scroll_layout.horizontal_scrollbar.has_value() &&
      Contains(scroll_layout.horizontal_scrollbar->track, event.button.x, event.button.y)) {
    interaction_state_.drag_target = DragTarget::EditorHorizontalScrollbar;
    interaction_state_.drag_scrollbar_offset =
        Contains(scroll_layout.horizontal_scrollbar->thumb, event.button.x, event.button.y)
            ? static_cast<float>(event.button.x) - scroll_layout.horizontal_scrollbar->thumb.x
            : scroll_layout.horizontal_scrollbar->thumb.w * 0.5f;
    viewport->SetHorizontalScroll(static_cast<std::size_t>(std::max(
        0L, std::lround(ScrollUnitsForPointer(*scroll_layout.horizontal_scrollbar,
                                              static_cast<float>(event.button.x),
                                              interaction_state_.drag_scrollbar_offset)))));
    state_.surface.focus = FocusTarget::Editor;
    return true;
  }

  const float local_y = std::max(0.0f, event.button.y - metrics.first_line_y);
  const std::size_t row = static_cast<std::size_t>(local_y / metrics.line_height);
  const std::size_t line = std::min(viewport->scroll_line() + row,
                                    viewport->line_count() == 0 ? 0 : viewport->line_count() - 1);
  const float text_offset_x = std::max(0.0f, event.button.x - metrics.text_x);
  const std::size_t visual_column = viewport->horizontal_scroll() +
      static_cast<std::size_t>(std::max(
          0L, std::lround(text_offset_x / std::max(1.0f, text_renderer_.CharWidth()))));

  viewport->MoveCursorToVisualColumn(line, visual_column,
                                     (SDL_GetModState() & SDL_KMOD_SHIFT) != 0);
  operations_.reset_caret_blink();
  state_.surface.focus = FocusTarget::Editor;
  if (event.button.button == SDL_BUTTON_MIDDLE) {
    if (const std::optional<std::string> text = operations_.read_primary_selection_text();
        text.has_value()) {
      const bool was_dirty = viewport->dirty();
      const std::size_t cursor_before_line = viewport->cursor_line();
      const std::vector<std::string> before_lines = viewport->lines();
      viewport->InsertText(*text);
      operations_.reset_caret_blink();
      operations_.request_active_editable_change_redraw(before_lines, viewport->lines());
      if (viewport->dirty() != was_dirty) {
        operations_.request_active_editable_blame_neighborhood_redraw(cursor_before_line,
                                                                      viewport->cursor_line());
        operations_.request_tab_strip_redraw();
      }
    }
    return true;
  }
  operations_.request_focused_editor_redraw();
  interaction_state_.mouse_selecting = true;
  return true;
}

bool EditorMouseCoordinator::HandleDrag(const SDL_Event& event,
                                        const WorkspaceLayout& layout) {
  if (interaction_state_.drag_target == DragTarget::EditorSplitDivider) {
    auto* editor_tab = operations_.active_editor_tab();
    if (editor_tab == nullptr || editor_tab->views.size() < 2 || editor_tab->split_root == nullptr) {
      operations_.clear_drag_state();
      return false;
    }

    operations_.normalize_editor_split_tree(*editor_tab);
    auto* split_node = operations_.find_editor_split_node(editor_tab->split_root.get(),
                                                          interaction_state_.drag_editor_split_path);
    const auto node_rect =
        operations_.compute_editor_split_node_rect(layout.editor_surface,
                                                   interaction_state_.drag_editor_split_path);
    if (split_node == nullptr || node_rect == std::nullopt || split_node->IsLeaf() ||
        split_node->orientation == EditorSplitOrientation::None ||
        interaction_state_.drag_editor_split_divider_index + 1 >= split_node->children.size()) {
      operations_.clear_drag_state();
      return false;
    }

    const bool vertical = split_node->orientation == EditorSplitOrientation::Vertical;
    std::vector<float> size_fractions(split_node->children.size(), 0.0f);
    for (std::size_t i = 0; i < split_node->children.size(); ++i) {
      size_fractions[i] = split_node->children[i]->size_fraction;
    }
    const auto split_layout = ComputeEditorSplitAxisLayout(*node_rect, vertical, size_fractions);
    if (!split_layout.has_value() || split_layout->total_extent <= 0.0f ||
        split_layout->extents.size() != split_node->children.size()) {
      return false;
    }

    float before_extent = 0.0f;
    for (std::size_t i = 0; i < interaction_state_.drag_editor_split_divider_index; ++i) {
      before_extent += split_layout->extents[i];
    }
    const float pair_extent =
        split_layout->extents[interaction_state_.drag_editor_split_divider_index] +
        split_layout->extents[interaction_state_.drag_editor_split_divider_index + 1];
    const float min_extent =
        split_layout->total_extent >
                split_layout->min_pane_extent * static_cast<float>(split_layout->extents.size())
            ? split_layout->min_pane_extent
            : 0.0f;
    float leading_extent =
        vertical ? static_cast<float>(event.motion.x) - node_rect->x - before_extent -
                       split_layout->divider_thickness *
                           static_cast<float>(interaction_state_.drag_editor_split_divider_index) -
                       split_layout->divider_thickness * 0.5f
                 : static_cast<float>(event.motion.y) - node_rect->y - before_extent -
                       split_layout->divider_thickness *
                           static_cast<float>(interaction_state_.drag_editor_split_divider_index) -
                       split_layout->divider_thickness * 0.5f;
    leading_extent =
        pair_extent <= min_extent * 2.0f
            ? std::clamp(leading_extent, 0.0f, pair_extent)
            : std::clamp(leading_extent, min_extent, pair_extent - min_extent);
    const float trailing_extent = std::max(0.0f, pair_extent - leading_extent);
    split_node->children[interaction_state_.drag_editor_split_divider_index]->size_fraction =
        leading_extent / split_layout->total_extent;
    split_node->children[interaction_state_.drag_editor_split_divider_index + 1]->size_fraction =
        trailing_extent / split_layout->total_extent;
    operations_.normalize_editor_split_node(*split_node);
    state_.surface.focus = FocusTarget::Editor;
    return true;
  }

  if (interaction_state_.drag_target != DragTarget::EditorVerticalScrollbar &&
      interaction_state_.drag_target != DragTarget::EditorHorizontalScrollbar) {
    return false;
  }

  const auto panes = operations_.compute_editor_pane_layouts(layout.editor_surface);
  const auto active_pane =
      std::find_if(panes.begin(), panes.end(),
                   [](const WorkspaceShell::EditorPaneLayout& pane) { return pane.active; });
  const SDL_FRect editor_rect =
      active_pane != panes.end() ? active_pane->rect : layout.editor_surface;
  editor::TextViewport* viewport = operations_.active_editor_viewport();
  if (viewport == nullptr) {
    return false;
  }
  const editor::EditorViewMetrics metrics =
      editor::EditorViewRenderer::ComputeMetrics(text_renderer_, *viewport, editor_rect);
  viewport->SetViewportSize(metrics.visible_rows, metrics.visible_columns);
  const auto scroll_layout = operations_.compute_editor_scroll_layout(editor_rect, *viewport, metrics);

  if (interaction_state_.drag_target == DragTarget::EditorVerticalScrollbar) {
    if (!scroll_layout.vertical_scrollbar.has_value()) {
      operations_.clear_drag_state();
      return false;
    }
    viewport->SetScrollLine(static_cast<std::size_t>(std::max(
        0L, std::lround(ScrollUnitsForPointer(*scroll_layout.vertical_scrollbar,
                                              static_cast<float>(event.motion.y),
                                              interaction_state_.drag_scrollbar_offset)))));
  } else {
    if (!scroll_layout.horizontal_scrollbar.has_value()) {
      operations_.clear_drag_state();
      return false;
    }
    viewport->SetHorizontalScroll(static_cast<std::size_t>(std::max(
        0L, std::lround(ScrollUnitsForPointer(*scroll_layout.horizontal_scrollbar,
                                              static_cast<float>(event.motion.x),
                                              interaction_state_.drag_scrollbar_offset)))));
  }
  state_.surface.focus = FocusTarget::Editor;
  return true;
}

bool EditorMouseCoordinator::HandleSelectionMotion(const SDL_Event& event,
                                                   const WorkspaceLayout& layout) {
  const auto panes = operations_.compute_editor_pane_layouts(layout.editor_surface);
  const auto active_pane =
      std::find_if(panes.begin(), panes.end(),
                   [](const WorkspaceShell::EditorPaneLayout& pane) { return pane.active; });
  const SDL_FRect editor_rect =
      active_pane != panes.end() ? active_pane->rect : layout.editor_surface;
  if (!Contains(editor_rect, event.motion.x, event.motion.y)) {
    return false;
  }
  editor::TextViewport* viewport = operations_.active_editor_viewport();
  if (viewport == nullptr) {
    return false;
  }

  const editor::EditorViewMetrics metrics =
      editor::EditorViewRenderer::ComputeMetrics(text_renderer_, *viewport, editor_rect);
  viewport->SetViewportSize(metrics.visible_rows, metrics.visible_columns);

  const float local_y = std::max(0.0f, event.motion.y - metrics.first_line_y);
  const std::size_t row = static_cast<std::size_t>(local_y / metrics.line_height);
  const std::size_t line = std::min(viewport->scroll_line() + row,
                                    viewport->line_count() == 0 ? 0 : viewport->line_count() - 1);
  const float text_offset_x = std::max(0.0f, event.motion.x - metrics.text_x);
  const std::size_t visual_column = viewport->horizontal_scroll() +
      static_cast<std::size_t>(std::max(
          0L, std::lround(text_offset_x / std::max(1.0f, text_renderer_.CharWidth()))));

  viewport->MoveCursorToVisualColumn(line, visual_column, true);
  operations_.reset_caret_blink();
  operations_.request_focused_editor_redraw();
  state_.surface.focus = FocusTarget::Editor;
  return true;
}

bool EditorMouseCoordinator::HandleWheel(const SDL_Event& event,
                                         const WorkspaceLayout& layout,
                                         int vertical_ticks) {
  if (!Contains(layout.editor_surface, event.wheel.mouse_x, event.wheel.mouse_y)) {
    return false;
  }

  const auto panes = operations_.compute_editor_pane_layouts(layout.editor_surface);
  const auto hovered_pane =
      std::find_if(panes.begin(), panes.end(),
                   [&](const WorkspaceShell::EditorPaneLayout& pane) {
                     return Contains(pane.rect, event.wheel.mouse_x, event.wheel.mouse_y);
                   });
  if (hovered_pane != panes.end() && !hovered_pane->active) {
    operations_.set_active_editor_split(hovered_pane->leaf_id);
  }
  if (editor::TextViewport* viewport = operations_.active_editor_viewport(); viewport != nullptr) {
    viewport->ScrollVertical(-vertical_ticks * 3);
  }
  state_.surface.focus = FocusTarget::Editor;
  return true;
}

EditorMouseCoordinator WorkspaceShell::MakeEditorMouseCoordinator() {
  return EditorMouseCoordinator(
      context_.current_project_state,
      context_.interaction_state,
      text_renderer_,
      EditorMouseCoordinator::Operations{
          .compute_editor_split_divider_layouts =
              [this](const SDL_FRect& rect) { return ComputeEditorSplitDividerLayouts(rect); },
          .compute_editor_pane_layouts =
              [this](const SDL_FRect& rect) { return ComputeEditorPaneLayouts(rect); },
          .set_active_editor_split = [this](std::size_t index) { SetActiveEditorSplit(index); },
          .active_editor_viewport = [this]() { return ActiveEditorViewport(); },
          .compute_editor_scroll_layout =
              [this](const SDL_FRect& rect, const editor::TextViewport& viewport,
                     const editor::EditorViewMetrics& metrics) {
                return ComputeEditorScrollLayout(rect, viewport, metrics);
              },
          .read_primary_selection_text = [this]() { return ReadPrimarySelectionText(); },
          .reset_caret_blink = [this]() { ResetCaretBlink(); },
          .request_active_editable_change_redraw =
              [this](const std::vector<std::string>& before_lines,
                     const std::vector<std::string>& after_lines) {
                RequestActiveEditableChangeRedraw(before_lines, after_lines);
              },
          .request_active_editable_blame_neighborhood_redraw =
              [this](std::size_t before_line, std::size_t after_line) {
                RequestActiveEditableBlameNeighborhoodRedraw(before_line, after_line);
              },
          .request_tab_strip_redraw = [this]() { RequestTabStripRedraw(); },
          .request_focused_editor_redraw = [this]() { RequestFocusedEditorRedraw(); },
          .active_editor_tab = [this]() { return ActiveEditorTab(); },
          .normalize_editor_split_tree =
              [this](TabEntry::EditorTabState& editor_tab) { NormalizeEditorSplitTree(editor_tab); },
          .find_editor_split_node =
              [this](TabEntry::EditorTabState::EditorSplitNode* node,
                     const std::vector<std::size_t>& path) {
                return FindEditorSplitNode(node, path);
              },
          .compute_editor_split_node_rect =
              [this](const SDL_FRect& rect, const std::vector<std::size_t>& path) {
                return ComputeEditorSplitNodeRect(rect, path);
              },
          .normalize_editor_split_node =
              [this](TabEntry::EditorTabState::EditorSplitNode& node) {
                NormalizeEditorSplitNode(node);
              },
          .clear_drag_state = [this]() { ClearDragState(); },
      });
}

}  // namespace microide::workspace
