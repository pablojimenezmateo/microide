#include "workspace/WorkspaceEditorMouseCoordinator.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <utility>
#include <vector>

#include "editor/EditorViewRenderer.h"
#include "util/PerformanceTrace.h"
#include "workspace/RenderViewModelBuilder.h"
#include "workspace/WorkspaceLayout.h"

namespace microide::workspace {
namespace {

std::pair<editor::EditorViewMetrics, std::vector<std::size_t>> EditorPointerLayoutBundle(
    editor::TextViewport& viewport,
    const SDL_FRect& editor_rect,
    render::TextRenderer& text_renderer,
    const std::function<std::optional<std::string>(std::string_view id)>& get_setting_value,
    const std::function<editor::FoldingModel*()>& ensure_active_folding_model_fresh) {
  util::PerformanceTrace::Scope perf_scope("EditorMouseCoordinator::ResolvePointerLayout");
  auto setting_enabled = [&](std::string_view id, bool default_value) -> bool {
    const auto value = get_setting_value(id);
    if (!value.has_value()) {
      return default_value;
    }
    return *value != "false" && *value != "0" && *value != "off";
  };

  const bool fold_enabled = setting_enabled("editor.fold.enabled", true);
  const bool sticky_setting = setting_enabled("editor.fold.sticky_scroll.enabled", true);
  const int sticky_max_depth =
      ParseStickyScrollMaxDepthSetting(get_setting_value("editor.fold.sticky_scroll.max_depth"));
  editor::FoldingModel* folding_model =
      fold_enabled ? ensure_active_folding_model_fresh() : nullptr;
  const bool sticky_active = fold_enabled && sticky_setting && folding_model != nullptr;
  std::vector<std::size_t> sticky_lines;
  ComputeStickyScrollLinesUncached(viewport, folding_model, sticky_active, sticky_max_depth,
                                   sticky_lines);

  editor::EditorViewMetrics metrics =
      editor::EditorViewRenderer::ComputeMetrics(text_renderer, viewport, editor_rect, 0);
  viewport.SetViewportSize(metrics.visible_rows, metrics.visible_columns);
  if (!sticky_lines.empty()) {
    metrics = editor::EditorViewRenderer::ComputeMetrics(text_renderer, viewport, editor_rect,
                                                         sticky_lines.size());
    viewport.SetViewportSize(metrics.visible_rows, metrics.visible_columns);
  }
  return {metrics, sticky_lines};
}

std::optional<editor::EditorViewMetrics> FastEditorPointerMetricsFromViewportState(
    const editor::TextViewport& viewport,
    const SDL_FRect& editor_rect,
    render::TextRenderer& text_renderer) {
  util::PerformanceTrace::Scope perf_scope("EditorMouseCoordinator::FastPointerMetrics");
  const float line_height = std::max(1.0f, text_renderer.LineHeight());
  const int max_total_rows =
      std::max(1, static_cast<int>(std::floor((editor_rect.h - 12.0f) / line_height)));
  const std::size_t visible_rows = viewport.visible_lines();
  if (visible_rows == 0 || visible_rows > static_cast<std::size_t>(max_total_rows)) {
    return std::nullopt;
  }

  const std::size_t sticky_budget = static_cast<std::size_t>(std::max(0, max_total_rows - 1));
  const std::size_t sticky_rows =
      std::min(sticky_budget, static_cast<std::size_t>(max_total_rows) - visible_rows);
  const editor::EditorViewMetrics metrics =
      editor::EditorViewRenderer::ComputeMetrics(text_renderer, viewport, editor_rect, sticky_rows);
  if (metrics.visible_rows != viewport.visible_lines() ||
      metrics.visible_columns != viewport.visible_columns()) {
    return std::nullopt;
  }
  return metrics;
}

}  // namespace

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
  util::PerformanceTrace::Scope perf_scope("EditorMouseCoordinator::HandleButtonDown");
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

  const auto [metrics, sticky_lines] = EditorPointerLayoutBundle(
      *viewport, editor_rect, text_renderer_, operations_.get_setting_value,
      operations_.ensure_active_folding_model_fresh);

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

  if (!sticky_lines.empty() && metrics.sticky_scroll_rows > 0 &&
      event.button.button == SDL_BUTTON_LEFT) {
    const float sticky_top = metrics.sticky_band_top_y;
    const float sticky_bottom =
        sticky_top + static_cast<float>(metrics.sticky_scroll_rows) * metrics.line_height;
    if (event.button.y >= sticky_top && event.button.y < sticky_bottom) {
      const std::size_t band_row = static_cast<std::size_t>(
          (event.button.y - sticky_top) / metrics.line_height);
      if (band_row < sticky_lines.size()) {
        const std::size_t opener_line = sticky_lines[band_row];
        viewport->SetScrollLine(viewport->VisualRowForLine(opener_line));
        operations_.request_focused_editor_redraw();
        state_.surface.focus = FocusTarget::Editor;
        return true;
      }
    }
  }

  // Fold gutter click toggles the fold on the opener row. The painted marker
  // (see FoldGutterMarkerRect) sits in the rightmost few px of the gutter; we
  // widen the hit zone here so users don't need pixel precision.
  if (event.button.button == SDL_BUTTON_LEFT && event.button.y >= metrics.first_line_y) {
    const auto fold_setting = operations_.get_setting_value
                                  ? operations_.get_setting_value("editor.fold.enabled")
                                  : std::nullopt;
    const bool fold_enabled = !fold_setting.has_value() ||
                              (*fold_setting != "false" && *fold_setting != "0" &&
                               *fold_setting != "off");
    if (fold_enabled) {
      const float gutter_right = editor_rect.x + metrics.gutter_width;
      const float fold_hit_left = gutter_right - 18.0f;
      if (event.button.x >= fold_hit_left && event.button.x < gutter_right) {
        const std::size_t visual_row =
            viewport->scroll_line() +
            static_cast<std::size_t>((event.button.y - metrics.first_line_y) /
                                     metrics.line_height);
        if (visual_row < viewport->visual_line_count()) {
          const std::size_t opener_line = viewport->VisualRowLineIndex(visual_row);
          editor::FoldingModel* fold_model =
              operations_.ensure_active_folding_model_fresh
                  ? operations_.ensure_active_folding_model_fresh()
                  : nullptr;
          if (fold_model != nullptr && fold_model->FoldStartingAt(opener_line).has_value()) {
            fold_model->ToggleFold(opener_line);
            operations_.request_focused_editor_redraw();
            state_.surface.focus = FocusTarget::Editor;
            return true;
          }
        }
      }
    }
  }

  const float local_y = std::max(0.0f, event.button.y - metrics.first_line_y);
  const std::size_t row = static_cast<std::size_t>(local_y / metrics.line_height);
  const float text_offset_x = std::max(0.0f, event.button.x - metrics.text_x);
  const int visual_column = static_cast<int>(viewport->horizontal_scroll() +
      static_cast<std::size_t>(std::max(
          0L, std::lround(text_offset_x / std::max(1.0f, text_renderer_.CharWidth())))));
  const int visual_row = static_cast<int>(viewport->scroll_line() + row);
  const editor::LogicalPosition hit = viewport->LogicalPositionForVisualHit(visual_row, visual_column);

  const SDL_Keymod modifiers = SDL_GetModState();
  const bool alt_left_click =
      event.button.button == SDL_BUTTON_LEFT && (modifiers & SDL_KMOD_ALT) != 0;
  const editor::TextPosition anchor{viewport->cursor_line(), viewport->cursor_column()};
  const bool shift_alt_column_click =
      alt_left_click && (modifiers & SDL_KMOD_SHIFT) != 0 && hit.column == anchor.column;
  const bool plain_left_click =
      event.button.button == SDL_BUTTON_LEFT &&
      (modifiers & (SDL_KMOD_ALT | SDL_KMOD_SHIFT | SDL_KMOD_CTRL | SDL_KMOD_GUI)) == 0;
  if (plain_left_click && viewport->has_multiple_carets()) {
    viewport->ClearSecondaryCarets();
  }
  if (shift_alt_column_click) {
    viewport->PlaceColumnCaretsBetweenLines(anchor.line, hit.line, anchor.column);
  } else {
    {
      util::PerformanceTrace::Scope move_scope(
          "EditorMouseCoordinator::HandleButtonDown::MoveCursorToVisualColumn");
      viewport->MoveCursorTo(hit.line, hit.column,
                             !alt_left_click && (modifiers & SDL_KMOD_SHIFT) != 0);
    }
    if (alt_left_click) {
      viewport->AddSecondaryCaret(anchor.line, anchor.column);
      viewport->ClearSelection();
    }
  }
  if (event.button.clicks == 2) {
    viewport->SelectWordAtCursor();
  } else if (event.button.clicks >= 3) {
    viewport->SelectLineAtCursor();
  }
  operations_.reset_caret_blink();
  state_.surface.focus = FocusTarget::Editor;
  if (event.button.button == SDL_BUTTON_MIDDLE) {
    if (const std::optional<std::string> text = operations_.read_primary_selection_text();
        text.has_value()) {
      const bool was_dirty = viewport->dirty();
      const std::size_t cursor_before_line = viewport->cursor_line();
      viewport->InsertText(*text);
      operations_.reset_caret_blink();
      operations_.request_active_editable_last_change_redraw();
      if (viewport->dirty() != was_dirty) {
        operations_.request_active_editable_blame_neighborhood_redraw(cursor_before_line,
                                                                      viewport->cursor_line());
        operations_.request_tab_strip_redraw();
      }
    }
    return true;
  }
  operations_.request_focused_editor_redraw();
  if (alt_left_click) {
    return true;
  }
  interaction_state_.mouse_selecting = true;
  return true;
}

bool EditorMouseCoordinator::HandleDrag(const SDL_Event& event,
                                        const WorkspaceLayout& layout) {
  util::PerformanceTrace::Scope perf_scope("EditorMouseCoordinator::HandleDrag");
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
  editor::EditorViewMetrics metrics{};
  if (const auto fast_metrics =
          FastEditorPointerMetricsFromViewportState(*viewport, editor_rect, text_renderer_);
      fast_metrics.has_value()) {
    metrics = *fast_metrics;
  } else {
    util::PerformanceTrace::Scope fallback_scope(
        "EditorMouseCoordinator::HandleDrag::FallbackPointerLayout");
    const auto [resolved_metrics, metrics_sticky_scratch] =
        EditorPointerLayoutBundle(*viewport, editor_rect, text_renderer_,
                                  operations_.get_setting_value,
                                  operations_.ensure_active_folding_model_fresh);
    (void)metrics_sticky_scratch;
    metrics = resolved_metrics;
  }
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
  util::PerformanceTrace::Scope perf_scope("EditorMouseCoordinator::HandleSelectionMotion");
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

  editor::EditorViewMetrics metrics{};
  if (const auto fast_metrics =
          FastEditorPointerMetricsFromViewportState(*viewport, editor_rect, text_renderer_);
      fast_metrics.has_value()) {
    metrics = *fast_metrics;
  } else {
    util::PerformanceTrace::Scope fallback_scope(
        "EditorMouseCoordinator::HandleSelectionMotion::FallbackPointerLayout");
    const auto [resolved_metrics, selection_sticky_scratch] =
        EditorPointerLayoutBundle(*viewport, editor_rect, text_renderer_,
                                  operations_.get_setting_value,
                                  operations_.ensure_active_folding_model_fresh);
    (void)selection_sticky_scratch;
    metrics = resolved_metrics;
  }

  const float local_y = std::max(0.0f, event.motion.y - metrics.first_line_y);
  const std::size_t row = static_cast<std::size_t>(local_y / metrics.line_height);
  const float text_offset_x = std::max(0.0f, event.motion.x - metrics.text_x);
  const int visual_column = static_cast<int>(viewport->horizontal_scroll() +
      static_cast<std::size_t>(std::max(
          0L, std::lround(text_offset_x / std::max(1.0f, text_renderer_.CharWidth())))));
  const int visual_row = static_cast<int>(viewport->scroll_line() + row);
  const editor::LogicalPosition hit = viewport->LogicalPositionForVisualHit(visual_row, visual_column);

  viewport->MoveCursorTo(hit.line, hit.column, true);
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
          .request_active_editable_last_change_redraw =
              [this]() { RequestActiveEditableLastChangeRedraw(); },
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
          .get_setting_value =
              [this](std::string_view id) { return GetSettingValue(id); },
          .ensure_active_folding_model_fresh =
              [this]() { return EnsureActiveFoldingModelFresh(); },
      });
}

}  // namespace microide::workspace
