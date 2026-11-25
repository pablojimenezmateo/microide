#include <memory>

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
#include "editor/TextDragDrop.h"
#include "workspace/actions/WorkspaceActionCoordinator.h"
#include "workspace/coordinators/EditorPointerHit.h"
#include "workspace/coordinators/SelectionAutoscroll.h"
#include "workspace/coordinators/SelectionGranularity.h"
#include "workspace/ListSelection.h"
#include "workspace/render/RenderViewModelBuilder.h"
#include "workspace/SettingFlags.h"
#include "workspace/WorkspaceLayout.h"
#include "util/InvokeIfSet.h"

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
  const bool line_numbers = setting_enabled("editor.line_numbers", true);
  std::vector<std::size_t> sticky_lines;
  const editor::EditorViewMetrics metrics = editor::EditorViewRenderer::SizeViewportWithStickyBand(
      text_renderer, viewport, editor_rect, line_numbers, [&](const editor::TextViewport& view) {
        ComputeStickyScrollLinesUncached(view, folding_model, sticky_active, sticky_max_depth, sticky_lines);
        return sticky_lines.size();
      });
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

SDL_FPoint ClampPointerToVisibleTextBand(const SDL_FRect& editor_rect,
                                         const editor::EditorViewMetrics& metrics,
                                         float x, float y) {
  return selection_autoscroll::ClampPointerToBand(
      selection_autoscroll::BandFor(editor_rect, metrics), x, y);
}

bool SettingOnByDefault(
    const std::function<std::optional<std::string>(std::string_view)>& get_setting_value,
    std::string_view id) {
  if (!get_setting_value) {
    return true;
  }
  return SettingFlagEnabled(get_setting_value(id), true);
}

}

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
  const auto panes = operations_.compute_editor_pane_layouts(layout);
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
      text_renderer_, *viewport, pane_it->rect, viewport->sticky_band_rows(),
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

  if (const EditorBannerState* banner = ActiveEditorBannerForTab(state_); banner != nullptr) {
    const SDL_FRect strip =
        ActiveEditorBannerStripRect(state_, ComputeEditorGroupRects(layout, state_.editor_split));
    if (Contains(strip, event.button.x, event.button.y)) {
      const EditorBannerButtonLayout buttons =
          ComputeEditorBannerButtonRects(strip, EditorBannerButtonsFor(banner->kind));
      const std::filesystem::path path = banner->path;
      state_.surface.focus = FocusTarget::Editor;
      if (operations_.editor_banner_action) {
        if (Contains(buttons.dismiss, event.button.x, event.button.y)) {
          operations_.editor_banner_action(EditorBannerAction::Keep, path);
        } else {
          for (std::size_t i = 0; i < buttons.buttons.size(); ++i) {
            if (Contains(buttons.rects[i], event.button.x, event.button.y)) {
              operations_.editor_banner_action(buttons.buttons[i].action, path);
              break;
            }
          }
        }
      }
      return true;
    }
  }

  const auto panes = operations_.compute_editor_pane_layouts(layout);
  const auto pane_it =
      std::find_if(panes.begin(), panes.end(),
                   [&](const WorkspaceShell::EditorPaneLayout& pane) {
                     return Contains(pane.rect, event.button.x, event.button.y);
                   });
  if (pane_it == panes.end()) {
    return false;
  }
  if (pane_it->group_index != state_.focused_group_index &&
      pane_it->group_index < state_.editor_groups.size()) {
    state_.focused_group_index = pane_it->group_index;
    util::InvokeIfSet(operations_.request_tab_strip_redraw);
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
        *scroll_layout.vertical_scrollbar, static_cast<float>(event.button.y), true);
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
                            static_cast<float>(event.button.x), false);
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
        operations_.request_frame();
        state_.surface.focus = FocusTarget::Editor;
        return true;
      }
    }
  }

  if (event.button.button == SDL_BUTTON_LEFT && event.button.y >= metrics.first_line_y) {
    const auto fold_setting = operations_.get_setting_value
                                  ? operations_.get_setting_value("editor.fold.enabled")
                                  : std::nullopt;
    if (SettingFlagEnabled(fold_setting, true)) {
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
            operations_.request_frame();
            state_.surface.focus = FocusTarget::Editor;
            return true;
          }
        }
      }
    }
  }

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
            util::InvokeIfSet(operations_.on_breakpoint_toggled, path);
            operations_.request_frame();
            state_.surface.focus = FocusTarget::Editor;
            return true;
          }
        }
      }
    }
  }

  const auto [visual_row, hit_column, hit] =
      ResolveEditorPointerHit(state_, *viewport, metrics, text_renderer_, event.button.x,
                              event.button.y, operations_.get_setting_value);

  const SDL_Keymod modifiers = interaction_state_.key_modifiers;
  const bool alt_left_click =
      event.button.button == SDL_BUTTON_LEFT && (modifiers & SDL_KMOD_ALT) != 0;
  const editor::TextPosition anchor{viewport->cursor_line(), viewport->cursor_column()};
  const bool shift_alt_box =
      alt_left_click && (modifiers & SDL_KMOD_SHIFT) != 0;
  const bool plain_left_click =
      event.button.button == SDL_BUTTON_LEFT &&
      (modifiers & (SDL_KMOD_ALT | SDL_KMOD_SHIFT | SDL_KMOD_CTRL | SDL_KMOD_GUI)) == 0;
  const bool ctrl_left_click =
      event.button.button == SDL_BUTTON_LEFT && event.button.clicks == 1 &&
      (modifiers & SDL_KMOD_CTRL) != 0 &&
      (modifiers & (SDL_KMOD_ALT | SDL_KMOD_SHIFT | SDL_KMOD_GUI)) == 0;
  const bool line_number_press =
      plain_left_click && event.button.x < editor_rect.x + metrics.gutter_width;
  interaction_state_.editor_box_selecting = false;
  interaction_state_.text_drag = InteractionState::TextDragState::None;
  interaction_state_.text_drag_has_drop = false;
  interaction_state_.editor_gesture_viewport = viewport;

  if (plain_left_click && !line_number_press && event.button.clicks == 1 &&
      !viewport->has_multiple_carets() && SettingOnByDefault(operations_.get_setting_value, "editor.drag_and_drop")) {
    if (const auto selection = viewport->selection_range();
        selection.has_value() &&
        editor::text_drag_drop::PositionWithin(*selection,
                                               editor::TextPosition{hit.line, hit.column})) {
      const editor::SelectionRange source = editor::TextViewport::NormalizeRange(*selection);
      interaction_state_.text_drag = InteractionState::TextDragState::Pending;
      interaction_state_.text_drag_press_x = event.button.x;
      interaction_state_.text_drag_press_y = event.button.y;
      interaction_state_.text_drag_press_line = hit.line;
      interaction_state_.text_drag_press_column = hit.column;
      interaction_state_.text_drag_source_start_line = source.start.line;
      interaction_state_.text_drag_source_start_column = source.start.column;
      interaction_state_.text_drag_source_end_line = source.end.line;
      interaction_state_.text_drag_source_end_column = source.end.column;
      interaction_state_.text_drag_viewport = viewport;
      interaction_state_.text_drag_content_revision = viewport->content_revision();
      interaction_state_.mouse_selecting = true;
      operations_.reset_caret_blink();
      state_.surface.focus = FocusTarget::Editor;
      return true;
    }
  }

  if ((plain_left_click || ctrl_left_click) && viewport->has_multiple_carets()) {
    viewport->ClearSecondaryCarets();
  }
  if (shift_alt_box) {
    viewport->SetBoxSelection(anchor, editor::TextPosition{hit.line, hit.column});
    interaction_state_.editor_box_selecting = true;
    interaction_state_.editor_box_anchor_line = anchor.line;
    interaction_state_.editor_box_anchor_column = anchor.column;
  } else if (alt_left_click) {
    if (!viewport->RemoveSecondaryCaretAt(editor::TextPosition{hit.line, hit.column})) {
      const std::optional<editor::SelectionRange> previous = viewport->selection_range();
      viewport->MoveCursorToVisualHit(visual_row, hit_column, false);
      if (previous.has_value()) {
        viewport->AddSecondaryCaretWithRange(*previous);
      } else {
        viewport->AddSecondaryCaret(anchor.line, anchor.column);
      }
    }
  } else {
    util::PerformanceTrace::Scope move_scope(
        "EditorMouseCoordinator::HandleButtonDown::MoveCursorToVisualColumn");
    viewport->MoveCursorToVisualHit(visual_row, hit_column, (modifiers & SDL_KMOD_SHIFT) != 0);
  }
  interaction_state_.selection_granularity = InteractionState::SelectionGranularity::Character;
  if (!shift_alt_box) {
    selection_granularity::ApplyClick(interaction_state_, *viewport,
                                      line_number_press ? 3 : event.button.clicks);
  }
  operations_.reset_caret_blink();
  state_.surface.focus = FocusTarget::Editor;
  if (ctrl_left_click && operations_.go_to_definition) {
    operations_.request_frame();
    operations_.go_to_definition();
    return true;
  }
  if (event.button.button == SDL_BUTTON_MIDDLE) {
    if (const std::optional<std::string> text = operations_.read_primary_selection_text();
        text.has_value()) {
      viewport->InsertText(*text);
      operations_.reset_caret_blink();
      operations_.note_active_editable_edit();
    }
    return true;
  }
  operations_.request_frame();
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

  const auto panes = operations_.compute_editor_pane_layouts(layout);
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
  const auto panes = operations_.compute_editor_pane_layouts(layout);
  const auto active_pane =
      std::find_if(panes.begin(), panes.end(),
                   [](const WorkspaceShell::EditorPaneLayout& pane) { return pane.active; });
  const SDL_FRect editor_rect =
      active_pane != panes.end() ? active_pane->rect : layout.editor_surface;
  editor::TextViewport* viewport = operations_.active_editor_viewport();
  if (viewport == nullptr) {
    return false;
  }
  if (interaction_state_.editor_gesture_viewport != nullptr &&
      interaction_state_.editor_gesture_viewport != static_cast<const void*>(viewport)) {
    interaction_state_.mouse_selecting = false;
    interaction_state_.editor_box_selecting = false;
    interaction_state_.text_drag = InteractionState::TextDragState::None;
    interaction_state_.text_drag_has_drop = false;
    return false;
  }

  const editor::EditorViewMetrics metrics = ResolveEditorPointerMetrics(
      *viewport, editor_rect, text_renderer_, operations_.get_setting_value,
      operations_.ensure_active_folding_model_fresh,
      "EditorMouseCoordinator::HandleSelectionMotion::FallbackPointerLayout");

  const SDL_FPoint pointer = ClampPointerToVisibleTextBand(editor_rect, metrics,
                                                           static_cast<float>(event.motion.x),
                                                           static_cast<float>(event.motion.y));
  selection_autoscroll::Arm(interaction_state_, InteractionState::SelectionAutoscrollOwner::Text,
                            selection_autoscroll::BandFor(editor_rect, metrics),
                            static_cast<float>(event.motion.x),
                            static_cast<float>(event.motion.y));

  const auto [visual_row, hit_column, hit] = ResolveEditorPointerHit(
      state_, *viewport, metrics, text_renderer_, pointer.x, pointer.y,
      operations_.get_setting_value);

  if (interaction_state_.text_drag != InteractionState::TextDragState::None) {
    if (interaction_state_.text_drag == InteractionState::TextDragState::Pending) {
      const float dx = static_cast<float>(event.motion.x) - interaction_state_.text_drag_press_x;
      const float dy = static_cast<float>(event.motion.y) - interaction_state_.text_drag_press_y;
      constexpr float kTextDragThresholdPx = 4.0f;
      if (dx * dx + dy * dy < kTextDragThresholdPx * kTextDragThresholdPx) {
        return true;
      }
      interaction_state_.text_drag = InteractionState::TextDragState::Dragging;
    }
    interaction_state_.text_drag_drop_line = hit.line;
    interaction_state_.text_drag_drop_column = hit.column;
    interaction_state_.text_drag_has_drop = true;
    operations_.reset_caret_blink();
    operations_.request_frame();
    state_.surface.focus = FocusTarget::Editor;
    return true;
  }

  if (selection_granularity::DragIsGranular(interaction_state_) &&
      !interaction_state_.editor_box_selecting) {
    selection_granularity::ExtendToPointer(interaction_state_, *viewport,
                                           editor::TextPosition{hit.line, hit.column});
  } else if (interaction_state_.editor_box_selecting) {
    viewport->SetBoxSelection(
        editor::TextPosition{interaction_state_.editor_box_anchor_line,
                             interaction_state_.editor_box_anchor_column},
        editor::TextPosition{hit.line, hit.column});
  } else {
    viewport->MoveCursorToVisualHit(visual_row, hit_column, true);
  }
  operations_.reset_caret_blink();
  operations_.request_frame();
  state_.surface.focus = FocusTarget::Editor;
  return true;
}

bool EditorMouseCoordinator::FinishTextDrag(bool copy) {
  const InteractionState::TextDragState state = interaction_state_.text_drag;
  const bool has_drop = interaction_state_.text_drag_has_drop;
  interaction_state_.text_drag = InteractionState::TextDragState::None;
  interaction_state_.text_drag_has_drop = false;
  if (state == InteractionState::TextDragState::None) {
    return false;
  }

  editor::TextViewport* viewport = operations_.active_editor_viewport();
  if (viewport == nullptr) {
    return false;
  }
  const editor::SelectionRange source{
      editor::TextPosition{interaction_state_.text_drag_source_start_line,
                           interaction_state_.text_drag_source_start_column},
      editor::TextPosition{interaction_state_.text_drag_source_end_line,
                           interaction_state_.text_drag_source_end_column}};

  if (state == InteractionState::TextDragState::Pending) {
    viewport->JumpCursorTo(interaction_state_.text_drag_press_line,
                           interaction_state_.text_drag_press_column);
    operations_.reset_caret_blink();
    operations_.request_frame();
    return true;
  }

  if (!has_drop) {
    return true;
  }
  if (interaction_state_.text_drag_viewport != static_cast<const void*>(viewport) ||
      interaction_state_.text_drag_content_revision != viewport->content_revision()) {
    operations_.request_frame();
    return true;
  }
  const editor::TextPosition drop{interaction_state_.text_drag_drop_line,
                                  interaction_state_.text_drag_drop_column};
  const auto moved = editor::text_drag_drop::Apply(*viewport, source, drop, copy);
  if (!moved.has_value()) {
    operations_.request_frame();
    return true;
  }
  viewport->JumpCursorTo(moved->start.line, moved->start.column);
  viewport->MoveCursorTo(moved->end.line, moved->end.column, true);

  operations_.reset_caret_blink();
  operations_.note_active_editable_edit();
  return true;
}

bool EditorMouseCoordinator::HandleWheel(const SDL_Event& event,
                                         const WorkspaceLayout& layout,
                                         int vertical_ticks,
                                         int horizontal_ticks) {
  if (!Contains(layout.editor_surface, event.wheel.mouse_x, event.wheel.mouse_y)) {
    return false;
  }

  editor::TextViewport* viewport = nullptr;
  const auto panes = operations_.compute_editor_pane_layouts(layout);
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
  return true;
}

EditorMouseCoordinator& WorkspaceShell::MakeEditorMouseCoordinator() {
  if (glue_->editor_mouse_coordinator != nullptr) {
    return *glue_->editor_mouse_coordinator;
  }
  glue_->editor_mouse_coordinator = std::make_unique<EditorMouseCoordinator>(
      context_.current_project_state,
      context_.interaction_state,
      editor_text_renderer_,
      EditorMouseCoordinator::Operations{
          .compute_editor_pane_layouts =
              [this](const WorkspaceLayout& layout) { return ComputeEditorPaneLayouts(layout); },
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
          .note_active_editable_edit =
              [this]() { NoteActiveEditableEdit(); },
          .request_frame = [this]() { RequestPreparedFrame(); },
          .request_tab_strip_redraw = [this]() { RequestPreparedFrame(); },
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
              [this](const std::filesystem::path& path) {
                debug_service_.ResendBreakpointsForFile(path);
              },
          .open_breakpoint_context_menu =
              [this](const std::filesystem::path& path, std::size_t line,
                     const SDL_FRect& anchor) { OpenBreakpointContextMenu(path, line, anchor); },
          .go_to_definition =
              [this]() {
                if (!MakeActionAvailability().IsEnabled(ActionId::GoToDefinition)) {
                  return;
                }
                ActionCoordinator(MakeActionContext())
                    .Execute(ActionId::GoToDefinition, {}, ActionSource::Shortcut);
              },
      });
  return *glue_->editor_mouse_coordinator;
}

}
