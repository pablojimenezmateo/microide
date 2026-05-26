#include "workspace/CompareTabReview.h"
#include "workspace/CompareMergeRender.h"
#include "workspace/WorkspaceCompareMouseCoordinator.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include "workspace/CompareMergeService.h"
#include "workspace/WorkspaceLayout.h"

namespace microide::workspace {

namespace {

constexpr float kCompareScrollbarReserve = 12.0f;

}  // namespace

CompareMouseCoordinator::CompareMouseCoordinator(ProjectWorkspaceState& state,
                                                 InteractionState& interaction_state,
                                                 Operations operations)
    : state_(state),
      interaction_state_(interaction_state),
      operations_(std::move(operations)) {}

bool CompareMouseCoordinator::ActiveTabIsCompare() const {
  return state_.active_tab_index < state_.open_tabs.size() &&
         state_.open_tabs[state_.active_tab_index].kind == TabEntry::Kind::Compare;
}

CompareTabState* CompareMouseCoordinator::ActiveCompareTab() const {
  if (!ActiveTabIsCompare()) {
    return nullptr;
  }
  auto& tab = state_.open_tabs[state_.active_tab_index];
  return tab.compare.has_value() ? &tab.compare.value() : nullptr;
}

bool CompareMouseCoordinator::HandleButtonDown(const SDL_Event& event,
                                               const WorkspaceLayout& layout) {
  if (!ActiveTabIsCompare()) {
    return false;
  }

  CompareTabState* compare_tab = ActiveCompareTab();
  if (compare_tab == nullptr) {
    return false;
  }

  const auto surface_layout =
      operations_.compute_compare_surface_layout(layout.editor_surface, *compare_tab);
  const auto scroll_layout =
      operations_.compute_compare_scroll_layout(layout.editor_surface, surface_layout, *compare_tab);
  compare_tab->scroll_row = scroll_layout.vertical_scroll;
  compare_tab->horizontal_scroll = scroll_layout.horizontal_scroll;
  operations_.sync_compare_viewport_scroll(*compare_tab);
  const SDL_FRect divider_rect =
      operations_.compare_divider_hit_rect(layout.editor_surface, surface_layout);

  if (Contains(divider_rect, event.button.x, event.button.y)) {
    interaction_state_.drag_target = DragTarget::CompareDivider;
    state_.surface.focus = FocusTarget::Editor;
    return true;
  }

  if (scroll_layout.vertical_scrollbar.has_value() &&
      Contains(scroll_layout.vertical_scrollbar->track, event.button.x, event.button.y)) {
    interaction_state_.drag_target = DragTarget::CompareVerticalScrollbar;
    interaction_state_.drag_scrollbar_offset =
        Contains(scroll_layout.vertical_scrollbar->thumb, event.button.x, event.button.y)
            ? static_cast<float>(event.button.y) - scroll_layout.vertical_scrollbar->thumb.y
            : scroll_layout.vertical_scrollbar->thumb.h * 0.5f;
    const int target_scroll = std::clamp(
        static_cast<int>(std::lround(ScrollUnitsForPointer(
            *scroll_layout.vertical_scrollbar, static_cast<float>(event.button.y),
            interaction_state_.drag_scrollbar_offset))),
        0, scroll_layout.max_vertical_scroll);
    compare_tab->scroll_row = target_scroll;
    operations_.sync_compare_viewport_scroll(*compare_tab);
    state_.surface.focus = FocusTarget::Editor;
    return true;
  }

  if (scroll_layout.horizontal_scrollbar.has_value() &&
      Contains(scroll_layout.horizontal_scrollbar->track, event.button.x, event.button.y)) {
    interaction_state_.drag_target = DragTarget::CompareHorizontalScrollbar;
    interaction_state_.drag_scrollbar_offset =
        Contains(scroll_layout.horizontal_scrollbar->thumb, event.button.x, event.button.y)
            ? static_cast<float>(event.button.x) - scroll_layout.horizontal_scrollbar->thumb.x
            : scroll_layout.horizontal_scrollbar->thumb.w * 0.5f;
    compare_tab->horizontal_scroll = static_cast<std::size_t>(std::max(
        0L, std::lround(ScrollUnitsForPointer(*scroll_layout.horizontal_scrollbar,
                                              static_cast<float>(event.button.x),
                                              interaction_state_.drag_scrollbar_offset))));
    operations_.sync_compare_viewport_scroll(*compare_tab);
    state_.surface.focus = FocusTarget::Editor;
    return true;
  }

  const int clicked_row =
      static_cast<int>((event.button.y - surface_layout.rows_y) / surface_layout.line_height);
  const int presentation_row = compare_tab->scroll_row + clicked_row;
  if (clicked_row < 0 || presentation_row < 0 ||
      static_cast<std::size_t>(presentation_row) >= CompareTabPresentationRowCount(*compare_tab)) {
    compare_tab->right_view_active = false;
    return false;
  }

  const std::size_t previous_selected_row = compare_tab->selected_row;
  const bool previous_right_view_active = compare_tab->right_view_active;
  compare_tab->selected_row = static_cast<std::size_t>(presentation_row);
  if (const compare::ComparePresentationRow* row =
          CompareTabPresentationRowAt(*compare_tab, compare_tab->selected_row);
      row != nullptr && row->kind == compare::ComparePresentationRowKind::CollapsedContext) {
    const SDL_FRect row_rect = MakeRect(layout.editor_surface.x, surface_layout.rows_y +
                                                             static_cast<float>(clicked_row) *
                                                                 surface_layout.line_height -
                                                             1.0f,
                                        layout.editor_surface.w,
                                        surface_layout.line_height);
    const auto action_rects =
        operations_.build_compare_collapsed_context_action_rects != nullptr
            ? operations_.build_compare_collapsed_context_action_rects(row_rect, *row)
            : CollapsedContextActionRects{};
    const float mouse_x = static_cast<float>(event.button.x);
    const float mouse_y = static_cast<float>(event.button.y);
    const auto handle_expand = [&](const std::optional<SDL_FRect>& rect,
                                   CompareCollapsedContextAction action) {
      return rect.has_value() && Contains(*rect, mouse_x, mouse_y) &&
             operations_.expand_compare_collapsed_context &&
             operations_.expand_compare_collapsed_context(*compare_tab, compare_tab->selected_row,
                                                          action);
    };
    if (handle_expand(action_rects.previous_rect, CompareCollapsedContextAction::ShowPrevious) ||
        handle_expand(action_rects.next_rect, CompareCollapsedContextAction::ShowNext) ||
        (Contains(action_rects.all_rect, mouse_x, mouse_y) &&
         operations_.expand_compare_collapsed_context &&
         operations_.expand_compare_collapsed_context(*compare_tab, compare_tab->selected_row,
                                                      CompareCollapsedContextAction::ShowAll))) {
      state_.surface.focus = FocusTarget::Editor;
      return true;
    }
    compare_tab->right_view_active = false;
    operations_.request_compare_row_range_redraw(previous_selected_row, previous_selected_row + 1);
    operations_.request_compare_row_range_redraw(compare_tab->selected_row,
                                                 compare_tab->selected_row + 1);
    state_.surface.focus = FocusTarget::Editor;
    return true;
  }
  if (event.button.x >= surface_layout.right_x) {
    compare_tab->right_view_active = true;
    const TextGridInteractionLayout right_interaction =
        operations_.build_compare_right_interaction_layout(surface_layout, *compare_tab);
    const std::size_t line = operations_.compare_right_line_for_row(
        *compare_tab, CompareTabSelectedModelRow(*compare_tab));
    const std::size_t visual_column = TextGridVisualColumnAtX(right_interaction, event.button.x);
    compare_tab->right_viewport.MoveCursorToVisualColumn(
        line, visual_column, (SDL_GetModState() & SDL_KMOD_SHIFT) != 0);
    operations_.sync_compare_selection_from_viewport(*compare_tab, false);
    operations_.reset_caret_blink();
    if (event.button.button == SDL_BUTTON_MIDDLE && compare_tab->right_editable) {
      if (const std::optional<std::string> text = operations_.read_primary_selection_text();
          text.has_value()) {
        const bool was_dirty = compare_tab->right_viewport.dirty();
        const std::size_t cursor_before_line = compare_tab->right_viewport.cursor_line();
        compare_tab->right_viewport.InsertText(*text);
        operations_.refresh_compare_tab_derived_state(*compare_tab);
        operations_.sync_compare_selection_from_viewport(*compare_tab, true);
        operations_.request_active_editable_last_change_redraw();
        if (compare_tab->right_viewport.dirty() != was_dirty) {
          operations_.request_active_editable_blame_neighborhood_redraw(
              cursor_before_line, compare_tab->right_viewport.cursor_line());
          operations_.request_tab_strip_redraw();
        }
      }
      state_.surface.focus = FocusTarget::Editor;
      return true;
    }
    interaction_state_.mouse_selecting = true;
  } else {
    compare_tab->right_view_active = false;
  }
  if (compare_tab->selected_row != previous_selected_row) {
    operations_.request_compare_row_range_redraw(previous_selected_row, previous_selected_row + 1);
    operations_.request_compare_row_range_redraw(compare_tab->selected_row,
                                                 compare_tab->selected_row + 1);
  } else if (compare_tab->right_view_active != previous_right_view_active) {
    operations_.request_compare_row_range_redraw(compare_tab->selected_row,
                                                 compare_tab->selected_row + 1);
  }
  state_.surface.focus = FocusTarget::Editor;
  return true;
}

bool CompareMouseCoordinator::HandleDrag(const SDL_Event& event,
                                         const WorkspaceLayout& layout) {
  if (!ActiveTabIsCompare() ||
      (interaction_state_.drag_target != DragTarget::CompareDivider &&
       interaction_state_.drag_target != DragTarget::CompareVerticalScrollbar &&
       interaction_state_.drag_target != DragTarget::CompareHorizontalScrollbar)) {
    return false;
  }

  CompareTabState* compare_tab = ActiveCompareTab();
  if (compare_tab == nullptr) {
    operations_.clear_drag_state();
    return false;
  }

  const auto surface_layout =
      operations_.compute_compare_surface_layout(layout.editor_surface, *compare_tab);
  const auto scroll_layout =
      operations_.compute_compare_scroll_layout(layout.editor_surface, surface_layout, *compare_tab);
  compare_tab->scroll_row = scroll_layout.vertical_scroll;
  compare_tab->horizontal_scroll = scroll_layout.horizontal_scroll;
  if (interaction_state_.drag_target == DragTarget::CompareDivider) {
    const float content_width = std::max(
        1.0f, layout.editor_surface.w -
                   (surface_layout.show_vertical ? kCompareScrollbarReserve : 0.0f) -
                   surface_layout.gutter_width * 2.0f - surface_layout.divider_width - 16.0f);
    const float desired_left_width =
        static_cast<float>(event.motion.x) - surface_layout.left_x - surface_layout.gutter_width -
        surface_layout.divider_width * 0.5f;
    compare_tab->divider_fraction =
        std::clamp(desired_left_width / content_width, surface_layout.min_divider_fraction,
                   1.0f - surface_layout.min_divider_fraction);
    state_.surface.focus = FocusTarget::Editor;
    return true;
  }
  if (interaction_state_.drag_target == DragTarget::CompareVerticalScrollbar) {
    if (!scroll_layout.vertical_scrollbar.has_value()) {
      operations_.clear_drag_state();
      return false;
    }
    const int target_scroll = std::clamp(
        static_cast<int>(std::lround(ScrollUnitsForPointer(
            *scroll_layout.vertical_scrollbar, static_cast<float>(event.motion.y),
            interaction_state_.drag_scrollbar_offset))),
        0, scroll_layout.max_vertical_scroll);
    compare_tab->scroll_row = target_scroll;
    operations_.sync_compare_viewport_scroll(*compare_tab);
    state_.surface.focus = FocusTarget::Editor;
    return true;
  }

  if (!scroll_layout.horizontal_scrollbar.has_value()) {
    operations_.clear_drag_state();
    return false;
  }
  compare_tab->horizontal_scroll = static_cast<std::size_t>(std::max(
      0L, std::lround(ScrollUnitsForPointer(*scroll_layout.horizontal_scrollbar,
                                            static_cast<float>(event.motion.x),
                                            interaction_state_.drag_scrollbar_offset))));
  operations_.sync_compare_viewport_scroll(*compare_tab);
  state_.surface.focus = FocusTarget::Editor;
  return true;
}

bool CompareMouseCoordinator::HandleSelectionMotion(const SDL_Event& event,
                                                    const WorkspaceLayout& layout) {
  if (!ActiveTabIsCompare()) {
    return false;
  }

  CompareTabState* compare_tab = ActiveCompareTab();
  if (compare_tab == nullptr || !compare_tab->right_view_active) {
    return false;
  }

  const auto surface_layout =
      operations_.compute_compare_surface_layout(layout.editor_surface, *compare_tab);
  if (event.motion.x < surface_layout.right_x) {
    return false;
  }
  const TextGridInteractionLayout right_interaction =
      operations_.build_compare_right_interaction_layout(surface_layout, *compare_tab);
  const auto hovered_row = VisibleTextGridLineAtY(right_interaction, event.motion.y);
  if (!hovered_row.has_value()) {
    return false;
  }

  const std::size_t line = operations_.compare_right_line_for_row(*compare_tab, *hovered_row);
  const std::size_t previous_selected_row = compare_tab->selected_row;
  const std::size_t visual_column = TextGridVisualColumnAtX(right_interaction, event.motion.x);
  compare_tab->right_viewport.MoveCursorToVisualColumn(line, visual_column, true);
  operations_.sync_compare_selection_from_viewport(*compare_tab, false);
  operations_.reset_caret_blink();
  if (compare_tab->selected_row != previous_selected_row) {
    operations_.request_compare_row_range_redraw(previous_selected_row, previous_selected_row + 1);
    operations_.request_compare_row_range_redraw(compare_tab->selected_row,
                                                 compare_tab->selected_row + 1);
  } else {
    operations_.request_focused_editor_redraw();
  }
  state_.surface.focus = FocusTarget::Editor;
  return true;
}

bool CompareMouseCoordinator::HandleWheel(const SDL_Event& event,
                                          const WorkspaceLayout& layout,
                                          int vertical_ticks,
                                          int horizontal_ticks) {
  if (!ActiveTabIsCompare() ||
      !Contains(layout.editor_surface, event.wheel.mouse_x, event.wheel.mouse_y)) {
    return false;
  }

  if (horizontal_ticks != 0) {
    operations_.scroll_compare_columns(-horizontal_ticks * 3);
  } else {
    operations_.scroll_compare_rows(-vertical_ticks * 3);
  }
  if (auto* compare_tab = ActiveCompareTab(); compare_tab != nullptr) {
    operations_.sync_compare_viewport_scroll(*compare_tab);
  }
  state_.surface.focus = FocusTarget::Editor;
  return true;
}

CompareMouseCoordinator WorkspaceShell::MakeCompareMouseCoordinator() {
  return CompareMouseCoordinator(
      context_.current_project_state,
      context_.interaction_state,
      CompareMouseCoordinator::Operations{
          .compute_compare_surface_layout =
              [this](const SDL_FRect& rect, const CompareTabState& compare_tab) {
                return ComputeCompareSurfaceLayout(rect, compare_tab);
              },
          .compute_compare_scroll_layout =
              [this](const SDL_FRect& rect, const CompareSurfaceLayout& surface,
                     const CompareTabState& compare_tab) {
                return ComputeCompareScrollLayout(rect, surface, compare_tab);
              },
          .sync_compare_viewport_scroll =
              [this](CompareTabState& compare_tab) { SyncCompareViewportScroll(compare_tab); },
          .compare_divider_hit_rect =
              [this](const SDL_FRect& rect, const CompareSurfaceLayout& surface) {
                return CompareDividerHitRect(rect, surface);
              },
          .build_compare_right_interaction_layout =
              [this](const CompareSurfaceLayout& surface, CompareTabState& compare_tab) {
                return BuildCompareRightInteractionLayout(surface, compare_tab);
              },
          .compare_right_line_for_row =
              [this](const CompareTabState& compare_tab, std::size_t row_index) {
                return CompareRightLineForRow(compare_tab, row_index);
              },
          .sync_compare_selection_from_viewport =
              [this](CompareTabState& compare_tab, bool reveal_selection) {
                SyncCompareSelectionFromViewport(compare_tab, reveal_selection);
              },
          .reset_caret_blink = [this]() { ResetCaretBlink(); },
          .read_primary_selection_text = [this]() { return ReadPrimarySelectionText(); },
          .refresh_compare_tab_derived_state =
              [this](CompareTabState& compare_tab) { RefreshCompareTabDerivedState(compare_tab); },
          .request_active_editable_last_change_redraw =
              [this]() { RequestActiveEditableLastChangeRedraw(); },
          .request_active_editable_blame_neighborhood_redraw =
              [this](std::size_t before_line, std::size_t after_line) {
                RequestActiveEditableBlameNeighborhoodRedraw(before_line, after_line);
              },
          .request_tab_strip_redraw = [this]() { RequestTabStripRedraw(); },
          .request_compare_row_range_redraw =
              [this](std::size_t start_row, std::size_t end_row) {
                RequestCompareRowRangeRedraw(start_row, end_row);
              },
          .request_focused_editor_redraw = [this]() { RequestFocusedEditorRedraw(); },
          .build_compare_collapsed_context_action_rects =
              [this](const SDL_FRect& row_rect, const compare::ComparePresentationRow& row) {
                return BuildCollapsedContextActionRects(
                    text_renderer_, row_rect, row.previous_hunk_index >= 0,
                    row.next_hunk_index >= 0);
              },
          .expand_compare_collapsed_context =
              [this](CompareTabState& compare_tab,
                     std::size_t presentation_row,
                     CompareCollapsedContextAction action) {
                const bool expanded =
                    ExpandCompareCollapsedContext(compare_tab, presentation_row, action);
                if (expanded) {
                  RequestEditorSurfaceRedraw();
                }
                return expanded;
              },
          .scroll_compare_rows =
              [this](int delta) { MakeCompareMergeService().ScrollCompareRows(delta); },
          .scroll_compare_columns =
              [this](int delta) { MakeCompareMergeService().ScrollCompareColumns(delta); },
          .clear_drag_state = [this]() { ClearDragState(); },
      });
}

}  // namespace microide::workspace
