#include "workspace/coordinators/WorkspaceEditorMouseCoordinator.h"

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
#include "workspace/ListSelection.h"
#include "workspace/render/RenderViewModelBuilder.h"
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

// Resolve the pointer metrics for a viewport, preferring the allocation-free
// fast path and falling back to the full layout bundle when the viewport's
// cached row/column counts do not agree with a recomputed layout (a resize or
// a folding change between frames).
//
// The drag and the selection-motion handlers each spelled this out: the same
// fast call with the same `editor.line_numbers` default-true lookup, the same
// has_value test, the same fallback that builds the bundle and discards its
// sticky scratch. Only the trace label differed, so it is a parameter.
editor::EditorViewMetrics ResolveEditorPointerMetrics(
    editor::TextViewport& viewport,
    const SDL_FRect& editor_rect,
    render::TextRenderer& text_renderer,
    const std::function<std::optional<std::string>(std::string_view)>& get_setting_value,
    const std::function<editor::FoldingModel*()>& ensure_active_folding_model_fresh,
    const char* fallback_trace_label) {
  if (const auto fast_metrics = FastEditorPointerMetricsFromViewportState(
          viewport, editor_rect, text_renderer,
          SettingFlagEnabled(
              get_setting_value ? get_setting_value("editor.line_numbers") : std::nullopt, true));
      fast_metrics.has_value()) {
    return *fast_metrics;
  }
  util::PerformanceTrace::Scope fallback_scope(fallback_trace_label);
  const auto [metrics, sticky_scratch] = EditorPointerLayoutBundle(
      viewport, editor_rect, text_renderer, get_setting_value, ensure_active_folding_model_fresh);
  (void)sticky_scratch;
  return metrics;
}

// Project a pointer onto the nearest cell of the pane's visible text band.
//
// The band is the rows the renderer actually painted -- `first_line_y` through
// `visible_rows` -- not the pane rect, which also covers the sticky-scroll band
// and any bottom slack. Clamping to the rect instead would map a pointer dragged
// above the text onto a sticky header row rather than onto the first document
// row.
//
// The half-cell inset on each edge keeps the clamped point strictly inside the
// band, so the row/column arithmetic that follows lands on the edge cell rather
// than one past it.
SDL_FPoint ClampPointerToVisibleTextBand(const SDL_FRect& editor_rect,
                                         const editor::EditorViewMetrics& metrics,
                                         float x, float y) {
  const float line_height = std::max(1.0f, metrics.line_height);
  const float band_top = metrics.first_line_y;
  const float band_bottom =
      metrics.first_line_y +
      static_cast<float>(std::max<std::size_t>(1, metrics.visible_rows)) * line_height;
  return SDL_FPoint{
      .x = std::clamp(x, editor_rect.x, editor_rect.x + std::max(0.0f, editor_rect.w - 1.0f)),
      .y = std::clamp(y, band_top, std::max(band_top, band_bottom - line_height * 0.5f)),
  };
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
// Map a click's DISPLAY visual column (which counts on-screen mid-line inlay-hint
// cells) back to the real visual column, so clicks land on the intended glyph when
// inlay hints have shifted the line's text rightward. Identity when the row has no
// mid-line hints; suppressed on soft-wrapped lines (hints are not rendered there
// in v1). Cold click path only.
int CorrectClickVisualColumnForInlay(const editor::TextViewport& viewport,
                                     const ProjectWorkspaceState& state,
                                     const render::TextRenderer& text_renderer,
                                     std::size_t visual_row, int display_visual_column) {
  if (display_visual_column < 0 || viewport.soft_wrap() ||
      visual_row >= viewport.visual_line_count()) {
    return display_visual_column;
  }
  const auto* pres = state.plugin_presentation_if_present();
  if (pres == nullptr) {
    return display_visual_column;
  }
  const editor::FileDecorations* file_dec = pres->decorations.FindByPath(viewport.path());
  if (file_dec == nullptr) {
    return display_visual_column;
  }
  const std::size_t line_index = viewport.VisualRowLineIndex(visual_row);
  const auto inline_texts = file_dec->InlineTextsForLine(static_cast<std::uint32_t>(line_index));
  if (inline_texts.empty()) {
    return display_visual_column;
  }
  const editor::LayoutLine& layout = viewport.VisibleLineLayoutRef(line_index);
  const std::size_t row_start = viewport.horizontal_scroll();
  const std::size_t row_end = row_start + viewport.visible_columns();
  return static_cast<int>(editor::RealVisualColumnForDisplayColumn(
      inline_texts, &layout, nullptr, row_start, row_end, text_renderer,
      text_renderer.CharWidth(), static_cast<std::size_t>(display_visual_column)));
}

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
    interaction_state_.drag_scrollbar_offset = ScrollbarGrabOffset(
        *scroll_layout.vertical_scrollbar, static_cast<float>(event.button.y), /*vertical=*/true);
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
        ScrollbarGrabOffset(*scroll_layout.horizontal_scrollbar,
                            static_cast<float>(event.button.x), /*vertical=*/false);
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
  const int display_visual_column = static_cast<int>(viewport->horizontal_scroll() +
      static_cast<std::size_t>(std::max(
          0L, std::lround(text_offset_x / std::max(1.0f, text_renderer_.CharWidth())))));
  const int visual_row = static_cast<int>(viewport->scroll_line() + row);
  const int visual_column = CorrectClickVisualColumnForInlay(
      *viewport, state_, text_renderer_, static_cast<std::size_t>(visual_row),
      display_visual_column);
  // LogicalPositionForVisualHit expects a SCREEN-RELATIVE column: it re-adds the
  // row's visual_start, which equals horizontal_scroll in the non-wrap layout. The
  // column computed above (and the inlay correction) is ABSOLUTE, so strip the
  // scroll offset here or it is counted twice and the caret snaps to the right
  // edge on every click once the line is horizontally scrolled. No-op under soft
  // wrap, where horizontal_scroll is pinned to 0.
  const int hit_column =
      std::max(0, visual_column - static_cast<int>(viewport->horizontal_scroll()));
  const editor::LogicalPosition hit = viewport->LogicalPositionForVisualHit(visual_row, hit_column);

  const SDL_Keymod modifiers = SDL_GetModState();
  const bool alt_left_click =
      event.button.button == SDL_BUTTON_LEFT && (modifiers & SDL_KMOD_ALT) != 0;
  const editor::TextPosition anchor{viewport->cursor_line(), viewport->cursor_column()};
  // Shift+Alt+drag is a rectangular (column/box) selection: the anchor corner is
  // the current primary caret, and the pointer extends a per-line selection across
  // the row span. When the two corners share a column this degenerates to plain
  // zero-width column carets, matching the prior Shift+Alt+click behavior.
  const bool shift_alt_box =
      alt_left_click && (modifiers & SDL_KMOD_SHIFT) != 0;
  const bool plain_left_click =
      event.button.button == SDL_BUTTON_LEFT &&
      (modifiers & (SDL_KMOD_ALT | SDL_KMOD_SHIFT | SDL_KMOD_CTRL | SDL_KMOD_GUI)) == 0;
  interaction_state_.editor_box_selecting = false;
  if (plain_left_click && viewport->has_multiple_carets()) {
    viewport->ClearSecondaryCarets();
  }
  if (shift_alt_box) {
    viewport->SetBoxSelection(anchor, editor::TextPosition{hit.line, hit.column});
    interaction_state_.editor_box_selecting = true;
    interaction_state_.editor_box_anchor_line = anchor.line;
    interaction_state_.editor_box_anchor_column = anchor.column;
  } else {
    {
      util::PerformanceTrace::Scope move_scope(
          "EditorMouseCoordinator::HandleButtonDown::MoveCursorToVisualColumn");
      // Hit-test and place in one call: under soft wrap a click past a wrapped
      // row's last glyph resolves to the wrap point, which the plain
      // MoveCursorTo(line, column) form renders at the start of the row BELOW the
      // one that was clicked. Same result everywhere else.
      viewport->MoveCursorToVisualHit(visual_row, hit_column,
                                      !alt_left_click && (modifiers & SDL_KMOD_SHIFT) != 0);
    }
    if (alt_left_click) {
      viewport->AddSecondaryCaret(anchor.line, anchor.column);
      viewport->ClearSelection();
    }
  }
  // Word/line expansion never applies to a box gesture (it would collapse the
  // rectangular selection back to a single row).
  if (!shift_alt_box) {
    if (event.button.clicks == 2) {
      viewport->SelectWordAtCursor();
    } else if (event.button.clicks >= 3) {
      viewport->SelectLineAtCursor();
    }
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
  // A plain Alt+click just drops a caret and does not drag-select. A Shift+Alt box
  // gesture keeps selecting on drag, so it falls through to arm mouse_selecting.
  if (alt_left_click && !shift_alt_box) {
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
  const editor::EditorViewMetrics metrics = ResolveEditorPointerMetrics(
      *viewport, editor_rect, text_renderer_, operations_.get_setting_value,
      operations_.ensure_active_folding_model_fresh,
      "EditorMouseCoordinator::HandleDrag::FallbackPointerLayout");
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
  editor::TextViewport* viewport = operations_.active_editor_viewport();
  if (viewport == nullptr) {
    return false;
  }

  const editor::EditorViewMetrics metrics = ResolveEditorPointerMetrics(
      *viewport, editor_rect, text_renderer_, operations_.get_setting_value,
      operations_.ensure_active_folding_model_fresh,
      "EditorMouseCoordinator::HandleSelectionMotion::FallbackPointerLayout");

  // A selection drag does not stop at the pane edge. This used to refuse any
  // pointer outside `editor_rect`, so dragging up into the tab strip, down into
  // the panel, sideways into a split sibling, or off the window froze the
  // selection at wherever it last crossed the boundary -- and it stayed frozen
  // while the button was still held. Every other editor (VS Code included)
  // extends toward the pointer wherever the pointer goes.
  //
  // Clamping into the visible text band is what that means geometrically: the
  // row/column math below is unchanged and simply reads a pointer that has been
  // projected back onto the nearest visible cell. Autoscroll then walks that
  // clamped edge through the document while the pointer sits still outside it,
  // which is the half a clamp alone cannot do.
  const SDL_FPoint pointer = ClampPointerToVisibleTextBand(editor_rect, metrics,
                                                           static_cast<float>(event.motion.x),
                                                           static_cast<float>(event.motion.y));

  const std::size_t row =
      ResolveGapAwareRow(state_, *viewport, metrics, pointer.y, operations_.get_setting_value).row;
  const float text_offset_x = std::max(0.0f, pointer.x - metrics.text_x);
  const int display_visual_column = static_cast<int>(viewport->horizontal_scroll() +
      static_cast<std::size_t>(std::max(
          0L, std::lround(text_offset_x / std::max(1.0f, text_renderer_.CharWidth())))));
  const int visual_row = static_cast<int>(viewport->scroll_line() + row);
  const int visual_column = CorrectClickVisualColumnForInlay(
      *viewport, state_, text_renderer_, static_cast<std::size_t>(visual_row),
      display_visual_column);
  // Screen-relative column: see the button-down handler above — strip the scroll
  // offset that LogicalPositionForVisualHit re-adds so it is not double-counted.
  const int hit_column =
      std::max(0, visual_column - static_cast<int>(viewport->horizontal_scroll()));
  const editor::LogicalPosition hit = viewport->LogicalPositionForVisualHit(visual_row, hit_column);

  if (interaction_state_.editor_box_selecting) {
    // Rebuild the rectangular selection from the fixed anchor corner to the live
    // pointer. The anchor column is virtual: SetBoxSelection clamps it per line, so
    // dragging past a short line and back preserves the full-width box.
    viewport->SetBoxSelection(
        editor::TextPosition{interaction_state_.editor_box_anchor_line,
                             interaction_state_.editor_box_anchor_column},
        editor::TextPosition{hit.line, hit.column});
  } else {
    viewport->MoveCursorToVisualHit(visual_row, hit_column, true);
  }
  operations_.reset_caret_blink();
  operations_.request_focused_editor_redraw();
  state_.surface.focus = FocusTarget::Editor;
  return true;
}

bool EditorMouseCoordinator::HandleWheel(const SDL_Event& event,
                                         const WorkspaceLayout& layout,
                                         int vertical_ticks,
                                         int horizontal_ticks) {
  if (!Contains(layout.editor_surface, event.wheel.mouse_x, event.wheel.mouse_y)) {
    return false;
  }

  // Scroll the split pane under the pointer, not merely the focused pane, so a
  // wheel over an inactive split moves that split (matches VS Code and most
  // editors). Fall back to the active viewport when the pointer is not over any
  // pane rect (e.g. in a divider gap). Focus is intentionally left unchanged.
  editor::TextViewport* viewport = nullptr;
  const auto panes = operations_.compute_editor_pane_layouts(layout.editor_surface);
  const auto pane_it =
      std::find_if(panes.begin(), panes.end(),
                   [&](const WorkspaceShell::EditorPaneLayout& pane) {
                     return Contains(pane.rect, event.wheel.mouse_x, event.wheel.mouse_y);
                   });
  if (pane_it != panes.end() && operations_.viewport_for_pane) {
    viewport = operations_.viewport_for_pane(*pane_it);
  }
  if (viewport == nullptr) {
    viewport = operations_.active_editor_viewport();
  }
  if (viewport != nullptr) {
    // Shift+wheel (and a real horizontal wheel) scrolls sideways, as it does in the
    // compare and merge surfaces one tab over and in VS Code. The shell has been
    // resolving horizontal ticks all along — synthesizing them from Shift+vertical —
    // and handing them to compare, merge and the chrome; the editor was the one
    // surface that ignored them and scrolled down instead.
    if (horizontal_ticks != 0) {
      const auto columns = static_cast<std::size_t>(kWheelScrollRows);
      const std::size_t current = viewport->horizontal_scroll();
      viewport->SetHorizontalScroll(horizontal_ticks > 0
                                        ? (current < columns ? 0 : current - columns)
                                        : current + columns);
    } else {
      viewport->ScrollVertical(-vertical_ticks * kWheelScrollRows);
    }
  }
  // Scrolling is not focusing — see SidebarMouseCoordinator::HandleWheel.
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
          .viewport_for_pane =
              [this](const EditorPaneLayout& pane) { return ViewportForPane(pane); },
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
