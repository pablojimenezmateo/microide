#include "workspace/WorkspaceEditorMouseCoordinator.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <utility>
#include <vector>

#include "editor/EditorInsetLayout.h"
#include "editor/EditorRowYLayout.h"
#include "editor/EditorViewRenderer.h"
#include "editor/GutterMetrics.h"
#include "editor/PluginSurfaceStore.h"
#include "util/PathMatch.h"
#include "util/PerformanceTrace.h"
#include "workspace/RenderViewModelBuilder.h"
#include "workspace/SettingFlags.h"
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
    return SettingFlagEnabled(get_setting_value(id), default_value);
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

  const bool line_numbers = setting_enabled("editor.line_numbers", true);
  editor::EditorViewMetrics metrics = editor::EditorViewRenderer::ComputeMetrics(
      text_renderer, viewport, editor_rect, 0, line_numbers);
  viewport.SetViewportSize(metrics.visible_rows, metrics.visible_columns);
  if (!sticky_lines.empty()) {
    metrics = editor::EditorViewRenderer::ComputeMetrics(text_renderer, viewport, editor_rect,
                                                         sticky_lines.size(), line_numbers);
    viewport.SetViewportSize(metrics.visible_rows, metrics.visible_columns);
  }
  return {metrics, sticky_lines};
}

std::optional<editor::EditorViewMetrics> FastEditorPointerMetricsFromViewportState(
    const editor::TextViewport& viewport,
    const SDL_FRect& editor_rect,
    render::TextRenderer& text_renderer,
    bool show_line_numbers) {
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
  const editor::EditorViewMetrics metrics = editor::EditorViewRenderer::ComputeMetrics(
      text_renderer, viewport, editor_rect, sticky_rows, show_line_numbers);
  if (metrics.visible_rows != viewport.visible_lines() ||
      metrics.visible_columns != viewport.visible_columns()) {
    return std::nullopt;
  }
  return metrics;
}

bool SettingOn(const std::function<std::optional<std::string>(std::string_view)>& get_setting_value,
               std::string_view id) {
  if (!get_setting_value) {
    return false;
  }
  return SettingFlagEnabled(get_setting_value(id));
}

// Resolve the gap-aware visible-row offset for a pointer at screen-y `y`. When an
// inline-inset setting (`plugins.inline_surfaces` / `plugins.code_lens_above`) is
// on, the insets are resolved into the same row gaps the renderer drew (via the
// single EditorRowYLayout mapping) so a click below a gap lands on the correct
// logical line. With both off — or nothing visible — the gap list is empty and
// this collapses to the legacy `(y - first_line_y) / line_height` result.
editor::EditorRowYLayout::HitResult ResolveGapAwareRow(
    const ProjectWorkspaceState& state, const editor::TextViewport& viewport,
    const editor::EditorViewMetrics& metrics, float y,
    const std::function<std::optional<std::string>(std::string_view)>& get_setting_value) {
  // No plugin/LSP contribution: no insets can exist, so the gap list is empty and
  // the mapping collapses to the legacy formula. Skip the store probing entirely.
  const auto* pres = state.plugin_presentation_if_present();
  if (pres == nullptr) {
    return editor::EditorRowYLayout(metrics.first_line_y, metrics.line_height,
                                    static_cast<std::uint32_t>(viewport.scroll_line()))
        .HitTest(y, metrics.visible_rows);
  }
  editor::InsetGapOptions options{
      .inline_surfaces = SettingOn(get_setting_value, "plugins.inline_surfaces"),
      .code_lens_above = SettingOn(get_setting_value, "plugins.code_lens_above"),
      .code_lens_height = metrics.line_height};
  // Mirror the render path's ghost-text Below gap (geometry only) so a click below
  // a multi-line suggestion lands on the line the user actually sees.
  if (const auto* ghost = state.ghost_text_if_present();
      ghost != nullptr && ghost->lines.size() > 1 &&
      SettingOn(get_setting_value, "plugins.ghost_text") &&
      util::SamePathNormalized(viewport.path(), ghost->path)) {
    options.ghost_anchor_line = ghost->anchor_line;
    options.ghost_height = static_cast<float>(ghost->lines.size() - 1) * metrics.line_height;
  }
  return editor::ResolveInsetClick(pres->surfaces, pres->decorations, viewport,
                                   metrics.first_line_y, metrics.line_height, metrics.visible_rows,
                                   y, options)
      .hit;
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

bool EditorMouseCoordinator::HandleGutterContextMenu(const SDL_Event& event,
                                                     const WorkspaceLayout& layout) {
  if (event.button.button != SDL_BUTTON_RIGHT || !operations_.open_breakpoint_context_menu) {
    return false;
  }
  const auto debug_setting = operations_.get_setting_value
                                 ? operations_.get_setting_value("debug.enabled")
                                 : std::nullopt;
  if (!SettingFlagEnabled(debug_setting)) {
    return false;
  }
  const auto panes = operations_.compute_editor_pane_layouts(layout.editor_surface);
  const auto pane_it = std::find_if(panes.begin(), panes.end(),
                                    [&](const WorkspaceShell::EditorPaneLayout& pane) {
                                      return Contains(pane.rect, event.button.x, event.button.y);
                                    });
  if (pane_it == panes.end()) {
    return false;
  }
  editor::TextViewport* viewport = operations_.active_editor_viewport();
  if (viewport == nullptr || viewport->path().empty()) {
    return false;
  }
  const editor::EditorViewMetrics metrics = editor::EditorViewRenderer::ComputeMetrics(
      text_renderer_, *viewport, pane_it->rect, 0,
      SettingFlagEnabled(operations_.get_setting_value
                             ? operations_.get_setting_value("editor.line_numbers")
                             : std::nullopt,
                         true));
  const float gutter_left = pane_it->rect.x;
  const float fold_hit_left = editor::kGutterFoldHitLeft(pane_it->rect.x, metrics.gutter_width);
  if (event.button.y < metrics.first_line_y || event.button.x < gutter_left ||
      event.button.x >= fold_hit_left) {
    return false;
  }
  const std::size_t visual_row =
      viewport->scroll_line() +
      ResolveGapAwareRow(state_, *viewport, metrics, event.button.y,
                         operations_.get_setting_value)
          .row;
  if (visual_row >= viewport->visual_line_count()) {
    return false;
  }
  const std::size_t line_index = viewport->VisualRowLineIndex(visual_row);
  const SDL_FRect anchor{static_cast<float>(event.button.x), static_cast<float>(event.button.y),
                         1.0f, 1.0f};
  operations_.open_breakpoint_context_menu(viewport->path(), line_index, anchor);
  state_.surface.focus = FocusTarget::Editor;
  return true;
}

bool EditorMouseCoordinator::HandleButtonDown(const SDL_Event& event,
                                              const WorkspaceLayout& layout) {
  util::PerformanceTrace::Scope perf_scope("EditorMouseCoordinator::HandleButtonDown");

  // Non-blocking external-change banner occupies the top strip of the editor
  // surface, above the panes. Intercept its clicks before pane dispatch.
  if (const EditorBannerState* banner = ActiveEditorBannerForTab(state_); banner != nullptr) {
    const SDL_FRect strip = ComputeEditorBannerStripRect(layout.editor_surface);
    if (Contains(strip, event.button.x, event.button.y)) {
      const bool has_actions = banner->kind == EditorBannerState::Kind::ExternalChange;
      const EditorBannerButtonLayout buttons = ComputeEditorBannerButtonRects(strip, has_actions);
      const std::filesystem::path path = banner->path;
      state_.surface.focus = FocusTarget::Editor;
      if (operations_.editor_banner_action) {
        if (has_actions && Contains(buttons.reload, event.button.x, event.button.y)) {
          operations_.editor_banner_action(EditorBannerAction::Reload, path);
        } else if (has_actions && Contains(buttons.overwrite, event.button.x, event.button.y)) {
          operations_.editor_banner_action(EditorBannerAction::Overwrite, path);
        } else if (Contains(buttons.dismiss, event.button.x, event.button.y) ||
                   (has_actions && Contains(buttons.keep, event.button.x, event.button.y))) {
          operations_.editor_banner_action(EditorBannerAction::Keep, path);
        }
      }
      // Consume any click on the strip so it does not fall through to content.
      return true;
    }
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
  // Clicking into a split group focuses it first, so the cursor positioning and
  // viewport below resolve against that group's active editor.
  if (pane_it->group_index != state_.focused_group_index &&
      pane_it->group_index < state_.editor_groups.size()) {
    state_.focused_group_index = pane_it->group_index;
    if (operations_.request_tab_strip_redraw) {
      operations_.request_tab_strip_redraw();
    }
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
    if (SettingFlagEnabled(fold_setting, /*default_value=*/true)) {
      const float gutter_right = editor_rect.x + metrics.gutter_width;
      const float fold_hit_left = editor::kGutterFoldHitLeft(editor_rect.x, metrics.gutter_width);
      if (event.button.x >= fold_hit_left && event.button.x < gutter_right) {
        const std::size_t visual_row =
            viewport->scroll_line() +
            ResolveGapAwareRow(state_, *viewport, metrics, event.button.y,
                               operations_.get_setting_value)
                .row;
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

  // Breakpoint gutter click. When the debugger is enabled, the gutter area to
  // the left of the fold hit zone toggles a breakpoint on the clicked line.
  // Gated on `debug.enabled` so the editor is unchanged when debugging is off.
  // (Right-click opens the breakpoint context menu via HandleGutterContextMenu,
  // dispatched ahead of the editor context menu in WorkspaceShellMouse.)
  if (event.button.button == SDL_BUTTON_LEFT && event.button.y >= metrics.first_line_y) {
    const auto debug_setting = operations_.get_setting_value
                                   ? operations_.get_setting_value("debug.enabled")
                                   : std::nullopt;
    if (SettingFlagEnabled(debug_setting)) {
      const float gutter_left = editor_rect.x;
      const float fold_hit_left = editor::kGutterFoldHitLeft(editor_rect.x, metrics.gutter_width);
      if (event.button.x >= gutter_left && event.button.x < fold_hit_left) {
        const std::size_t visual_row =
            viewport->scroll_line() +
            ResolveGapAwareRow(state_, *viewport, metrics, event.button.y,
                               operations_.get_setting_value)
                .row;
        if (visual_row < viewport->visual_line_count()) {
          const std::size_t line_index = viewport->VisualRowLineIndex(visual_row);
          const std::filesystem::path& path = viewport->path();
          if (!path.empty()) {
            state_.breakpoint_store.Toggle(path, line_index);
            if (operations_.on_breakpoint_toggled) {
              operations_.on_breakpoint_toggled(path);
            }
            operations_.request_focused_editor_redraw();
            state_.surface.focus = FocusTarget::Editor;
            return true;
          }
        }
      }
    }
  }

  const std::size_t row = ResolveGapAwareRow(state_, *viewport, metrics,
                                             event.button.y, operations_.get_setting_value)
                              .row;
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
          FastEditorPointerMetricsFromViewportState(
              *viewport, editor_rect, text_renderer_,
              SettingFlagEnabled(operations_.get_setting_value
                                     ? operations_.get_setting_value("editor.line_numbers")
                                     : std::nullopt,
                                 true));
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
          FastEditorPointerMetricsFromViewportState(
              *viewport, editor_rect, text_renderer_,
              SettingFlagEnabled(operations_.get_setting_value
                                     ? operations_.get_setting_value("editor.line_numbers")
                                     : std::nullopt,
                                 true));
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

  const std::size_t row = ResolveGapAwareRow(state_, *viewport, metrics,
                                             event.motion.y, operations_.get_setting_value)
                              .row;
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
          .compute_editor_pane_layouts =
              [this](const SDL_FRect& rect) { return ComputeEditorPaneLayouts(rect); },
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
          .clear_drag_state = [this]() { ClearDragState(); },
          .get_setting_value =
              [this](std::string_view id) { return GetSettingValue(id); },
          .ensure_active_folding_model_fresh =
              [this]() { return EnsureActiveFoldingModelFresh(); },
          .editor_banner_action =
              [this](EditorBannerAction action, const std::filesystem::path& path) {
                ActivateEditorBannerAction(action, path);
              },
          .on_breakpoint_toggled =
              [this](const std::filesystem::path& path) { ResendBreakpointsForFile(path); },
          .open_breakpoint_context_menu =
              [this](const std::filesystem::path& path, std::size_t line,
                     const SDL_FRect& anchor) { OpenBreakpointContextMenu(path, line, anchor); },
      });
}

}  // namespace microide::workspace
