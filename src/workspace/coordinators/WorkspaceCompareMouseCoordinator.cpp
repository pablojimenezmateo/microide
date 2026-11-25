#include <memory>

#include "workspace/coordinators/WorkspaceCompareMouseCoordinator.h"

#include "workspace/coordinators/SelectionAutoscroll.h"
#include "workspace/coordinators/SelectionGranularity.h"

#include "workspace/render/CompareMergeRender.h"
#include "workspace/git/CompareTabReview.h"
#include "workspace/render/DiffDividerGeometry.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include "workspace/services/CompareMergeService.h"
#include "workspace/services/CompareReviewService.h"
#include "workspace/ListSelection.h"
#include "workspace/WorkspaceLayout.h"

namespace microide::workspace {

namespace {

constexpr float kCompareScrollbarReserve = 12.0f;

}

CompareMouseCoordinator::CompareMouseCoordinator(ProjectWorkspaceState& state,
                                                 InteractionState& interaction_state,
                                                 Operations operations)
    : state_(state),
      interaction_state_(interaction_state),
      operations_(std::move(operations)) {}

CompareTabState* CompareMouseCoordinator::ActiveCompareTab() const {
  return state_.focused_group().active_compare();
}

CompareMouseCoordinator::ClampedLayout CompareMouseCoordinator::ClampScrollToLayout(
    const SDL_FRect& surface, CompareTabState& compare_tab) const {
  ClampedLayout layout{.surface = operations_.compute_compare_surface_layout(surface, compare_tab)};
  layout.scroll = operations_.compute_compare_scroll_layout(surface, layout.surface, compare_tab);
  compare_tab.scroll_row = layout.scroll.vertical_scroll;
  compare_tab.horizontal_scroll = layout.scroll.horizontal_scroll;
  return layout;
}

bool CompareMouseCoordinator::HandleButtonDown(const SDL_Event& event,
                                               const SDL_FRect& surface) {
  CompareTabState* compare_tab = ActiveCompareTab();
  if (compare_tab == nullptr) {
    return false;
  }

  const auto [surface_layout, scroll_layout] = ClampScrollToLayout(surface, *compare_tab);
  operations_.sync_compare_viewport_scroll(*compare_tab);
  const SDL_FRect divider_rect = CompareDividerHitRect(surface, surface_layout);

  if (Contains(divider_rect, event.button.x, event.button.y)) {
    if (event.button.clicks >= 2) {
      compare_tab->divider_fraction = kWorkspaceDefaultCompareDividerFraction;
      operations_.clear_drag_state();
      state_.surface.focus = FocusTarget::Editor;
      return true;
    }
    interaction_state_.drag_target = DragTarget::CompareDivider;
    state_.surface.focus = FocusTarget::Editor;
    return true;
  }

  if (scroll_layout.vertical_scrollbar.has_value() &&
      Contains(scroll_layout.vertical_scrollbar->track, event.button.x, event.button.y)) {
    interaction_state_.drag_target = DragTarget::CompareVerticalScrollbar;
    interaction_state_.drag_scrollbar_offset = ScrollbarGrabOffset(
        *scroll_layout.vertical_scrollbar, static_cast<float>(event.button.y), true);
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
        ScrollbarGrabOffset(*scroll_layout.horizontal_scrollbar,
                            static_cast<float>(event.button.x), false);
    compare_tab->horizontal_scroll = static_cast<std::size_t>(std::max(
        0L, std::lround(ScrollUnitsForPointer(*scroll_layout.horizontal_scrollbar,
                                              static_cast<float>(event.button.x),
                                              interaction_state_.drag_scrollbar_offset))));
    operations_.sync_compare_viewport_scroll(*compare_tab);
    state_.surface.focus = FocusTarget::Editor;
    return true;
  }

  if (event.button.button == SDL_BUTTON_LEFT && operations_.compare_control_at && operations_.run_compare_control) {
    if (const std::optional<CompareControlHit> hit = operations_.compare_control_at(
            surface, surface_layout, *compare_tab, static_cast<float>(event.button.x),
            static_cast<float>(event.button.y));
        hit.has_value() && operations_.run_compare_control(*compare_tab, *hit)) {
      state_.surface.focus = FocusTarget::Editor;
      return true;
    }
  }

  const auto deactivate_right_side = [&]() {
    if (compare_tab->right_view_active) {
      compare_tab->right_view_active = false;
      operations_.request_frame();
    }
  };
  const float row_offset_px = static_cast<float>(event.button.y) - surface_layout.rows_y;
  if (row_offset_px < 0.0f) {
    deactivate_right_side();
    return false;
  }
  const int clicked_row = static_cast<int>(row_offset_px / surface_layout.line_height);
  const int visual_row = compare_tab->scroll_row + clicked_row;
  if (clicked_row < 0 || visual_row < 0 ||
      static_cast<std::size_t>(visual_row) >= CompareTabVisualRowCount(*compare_tab)) {
    deactivate_right_side();
    return false;
  }

  const std::size_t previous_selected_row = compare_tab->selected_row;
  const bool previous_right_view_active = compare_tab->right_view_active;
  compare_tab->selected_row =
      CompareVisualRowToPresentationRow(*compare_tab, static_cast<std::size_t>(visual_row));
  if (const compare::ComparePresentationRow* row =
          CompareTabPresentationRowAt(*compare_tab, compare_tab->selected_row);
      row != nullptr && row->kind == compare::ComparePresentationRowKind::CollapsedContext) {
    const SDL_FRect block_rect = CompareCollapsedContextBlockRect(
        surface, surface_layout.rows_y, surface_layout.line_height,
        surface_layout.show_vertical, clicked_row);
    const auto action_rects =
        operations_.build_compare_collapsed_context_action_rects != nullptr
            ? operations_.build_compare_collapsed_context_action_rects(block_rect, *row)
            : CollapsedContextActionRects{};
    if (const auto kind = CompareCollapsedContextActionAt(
            action_rects, static_cast<float>(event.button.x), static_cast<float>(event.button.y));
        kind.has_value()) {
      const CompareCollapsedContextAction action =
          *kind == CompareHoverKind::CollapsedContextPreviousAction
              ? CompareCollapsedContextAction::ShowPrevious
          : *kind == CompareHoverKind::CollapsedContextNextAction
              ? CompareCollapsedContextAction::ShowNext
              : CompareCollapsedContextAction::ShowAll;
      if (operations_.expand_compare_collapsed_context &&
          operations_.expand_compare_collapsed_context(*compare_tab, compare_tab->selected_row,
                                                       action)) {
        state_.surface.focus = FocusTarget::Editor;
        return true;
      }
    }
    compare_tab->right_view_active = false;
    operations_.request_frame();
    state_.surface.focus = FocusTarget::Editor;
    return true;
  }
  if (event.button.x >= surface_layout.right_x) {
    compare_tab->right_view_active = true;
    const TextGridInteractionLayout right_interaction =
        operations_.build_compare_right_interaction_layout(surface_layout, *compare_tab);
    const CompareRightPaneHit hit = CompareRightPaneHitAt(
        *compare_tab, static_cast<std::size_t>(visual_row),
        TextGridVisualColumnAtX(right_interaction, event.button.x));
    compare_tab->right_viewport.MoveCursorToVisualColumn(
        hit.line, hit.visual_column, (interaction_state_.key_modifiers & SDL_KMOD_SHIFT) != 0);
    operations_.sync_compare_selection_from_viewport(*compare_tab, false);
    operations_.reset_caret_blink();
    if (event.button.button == SDL_BUTTON_MIDDLE && compare_tab->right_editable) {
      if (const std::optional<std::string> text = operations_.read_primary_selection_text();
          text.has_value()) {
        compare_tab->right_viewport.InsertText(*text);
        operations_.refresh_compare_tab_derived_state(*compare_tab);
        operations_.sync_compare_selection_from_viewport(*compare_tab, true);
        operations_.note_active_editable_edit();
      }
      state_.surface.focus = FocusTarget::Editor;
      return true;
    }
    selection_granularity::ApplyClick(interaction_state_, compare_tab->right_viewport,
                                      event.button.clicks);
    operations_.sync_compare_selection_from_viewport(*compare_tab, false);
    interaction_state_.mouse_selecting = true;
  } else {
    compare_tab->right_view_active = false;
  }
  if (compare_tab->selected_row != previous_selected_row) {
    operations_.request_frame();
  } else if (compare_tab->right_view_active != previous_right_view_active) {
    operations_.request_frame();
  }
  state_.surface.focus = FocusTarget::Editor;
  return true;
}

bool CompareMouseCoordinator::HandleDrag(const SDL_Event& event,
                                         const SDL_FRect& surface) {
  if ((interaction_state_.drag_target != DragTarget::CompareDivider &&
       interaction_state_.drag_target != DragTarget::CompareVerticalScrollbar &&
       interaction_state_.drag_target != DragTarget::CompareHorizontalScrollbar)) {
    return false;
  }

  CompareTabState* compare_tab = ActiveCompareTab();
  if (compare_tab == nullptr) {
    operations_.clear_drag_state();
    return false;
  }

  const auto [surface_layout, scroll_layout] = ClampScrollToLayout(surface, *compare_tab);
  if (interaction_state_.drag_target == DragTarget::CompareDivider) {
    const float content_width = std::max(
        1.0f, surface.w -
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
                                                    const SDL_FRect& surface) {
  CompareTabState* compare_tab = ActiveCompareTab();
  if (compare_tab == nullptr || !compare_tab->right_view_active) {
    return false;
  }
  if (compare_tab->hover_state.has_value()) {
    operations_.request_frame();
    compare_tab->hover_state.reset();
  }

  const auto surface_layout =
      operations_.compute_compare_surface_layout(surface, *compare_tab);
  const TextGridInteractionLayout right_interaction =
      operations_.build_compare_right_interaction_layout(surface_layout, *compare_tab);
  const float pointer_x = std::max(static_cast<float>(event.motion.x), surface_layout.right_x);
  selection_autoscroll::Arm(interaction_state_, InteractionState::SelectionAutoscrollOwner::Text,
                            selection_autoscroll::BandFor(right_interaction),
                            static_cast<float>(event.motion.x),
                            static_cast<float>(event.motion.y));
  const std::size_t row = ClampTextGridLineAtY(right_interaction, static_cast<float>(event.motion.y));

  const CompareRightPaneHit hit =
      CompareRightPaneHitAt(*compare_tab, row, TextGridVisualColumnAtX(right_interaction, pointer_x));
  const std::size_t line = hit.line;
  const std::size_t visual_column = hit.visual_column;
  if (selection_granularity::DragIsGranular(interaction_state_)) {
    compare_tab->right_viewport.MoveCursorToVisualColumn(line, visual_column, false);
    selection_granularity::ExtendToPointer(
        interaction_state_, compare_tab->right_viewport,
        editor::TextPosition{compare_tab->right_viewport.cursor_line(),
                             compare_tab->right_viewport.cursor_column()});
  } else {
    compare_tab->right_viewport.MoveCursorToVisualColumn(line, visual_column, true);
  }
  operations_.sync_compare_selection_from_viewport(*compare_tab, false);
  operations_.reset_caret_blink();
  operations_.request_frame();
  state_.surface.focus = FocusTarget::Editor;
  return true;
}

bool CompareMouseCoordinator::HandleHoverMotion(const SDL_Event& event,
                                                const SDL_FRect& surface) {
  CompareTabState* compare_tab = ActiveCompareTab();
  if (compare_tab == nullptr) {
    return false;
  }

  const std::optional<CompareHoverState> previous_hover = compare_tab->hover_state;
  const int previous_hover_hunk = CompareHoveredHunk(*compare_tab);
  std::optional<CompareHoverState> next_hover;
  int next_hover_row = -1;
  if (Contains(surface, event.motion.x, event.motion.y) &&
      !(interaction_state_.mouse_selecting && (event.motion.state & SDL_BUTTON_LMASK) != 0)) {
    const auto [surface_layout, scroll_layout] = ClampScrollToLayout(surface, *compare_tab);
    operations_.sync_compare_viewport_scroll(*compare_tab);

    next_hover_row = CompareHoverRowAtY(MakeCompareControlGeometry(surface, surface_layout), *compare_tab,
                                        static_cast<float>(event.motion.y));
    if (operations_.compare_control_at) {
      if (const std::optional<CompareControlHit> control =
              operations_.compare_control_at(surface, surface_layout, *compare_tab,
                                             static_cast<float>(event.motion.x), static_cast<float>(event.motion.y));
          control.has_value()) {
        next_hover = CompareHoverState{
            .kind = control->kind,
            .presentation_row = control->presentation_row,
            .hunk = control->hunk,
        };
      }
    }

    if (const auto hit = CompareCollapsedContextRowAt(
            *compare_tab, surface, surface_layout.rows_y,
            surface_layout.line_height, surface_layout.show_vertical,
            static_cast<float>(event.motion.y));
        hit.has_value()) {
      {
        const auto action_rects =
            operations_.build_compare_collapsed_context_action_rects != nullptr
                ? operations_.build_compare_collapsed_context_action_rects(hit->block_rect,
                                                                           *hit->row)
                : CollapsedContextActionRects{};
        if (const std::optional<CompareHoverKind> kind = CompareCollapsedContextActionAt(
                action_rects, static_cast<float>(event.motion.x),
                static_cast<float>(event.motion.y));
            kind.has_value()) {
          next_hover = CompareHoverState{
              .kind = *kind,
              .presentation_row = hit->presentation_row,
              .collapsed_run_start_model_row = hit->row->collapsed_run_start_model_row,
              .collapsed_run_length = hit->row->collapsed_run_length,
          };
        }
      }
    }
  }

  compare_tab->hover_state = next_hover;
  compare_tab->review_hover_visible_row = next_hover_row;
  const int next_hover_hunk = CompareHoveredHunk(*compare_tab);
  if (previous_hover_hunk != next_hover_hunk) {
    operations_.request_frame();
  }
  if (previous_hover == compare_tab->hover_state) {
    return previous_hover_hunk != next_hover_hunk;
  }
  if (previous_hover.has_value()) {
    operations_.request_frame();
  }
  if (compare_tab->hover_state.has_value()) {
    operations_.request_frame();
  }
  return true;
}

bool CompareMouseCoordinator::HandleWheel(const SDL_Event& event,
                                          const SDL_FRect& surface,
                                          CompareTabState& compare_tab,
                                          int vertical_ticks,
                                          int horizontal_ticks) {
  if (!Contains(surface, event.wheel.mouse_x, event.wheel.mouse_y)) {
    return false;
  }
  const auto surface_layout = operations_.compute_compare_surface_layout(surface, compare_tab);
  const auto scroll_layout =
      operations_.compute_compare_scroll_layout(surface, surface_layout, compare_tab);
  if (horizontal_ticks != 0) {
    const long long target = static_cast<long long>(scroll_layout.horizontal_scroll) -
                             static_cast<long long>(horizontal_ticks) * kWheelScrollRows;
    compare_tab.horizontal_scroll = static_cast<std::size_t>(
        std::clamp(target, 0LL, static_cast<long long>(scroll_layout.max_horizontal_scroll)));
  } else {
    compare_tab.scroll_row =
        std::clamp(scroll_layout.vertical_scroll - vertical_ticks * kWheelScrollRows, 0,
                   scroll_layout.max_vertical_scroll);
  }
  operations_.sync_compare_viewport_scroll(compare_tab);
  return true;
}

CompareMouseCoordinator& WorkspaceShell::MakeCompareMouseCoordinator() {
  if (glue_->compare_mouse_coordinator != nullptr) {
    return *glue_->compare_mouse_coordinator;
  }
  glue_->compare_mouse_coordinator = std::make_unique<CompareMouseCoordinator>(
      context_.current_project_state,
      context_.interaction_state,
      CompareMouseCoordinator::Operations{
          .compute_compare_surface_layout =
              [this](const SDL_FRect& rect, const CompareTabState& compare_tab) {
                return ComputeCompareSurfaceLayout(rect, compare_tab);
              },
          .compute_compare_scroll_layout =
              [](const SDL_FRect& rect, const CompareSurfaceLayout& surface,
                     const CompareTabState& compare_tab) {
                return ComputeCompareScrollLayout(rect, surface, compare_tab);
              },
          .sync_compare_viewport_scroll =
              [this](CompareTabState& compare_tab) { SyncCompareViewportScroll(compare_tab); },
          .build_compare_right_interaction_layout =
              [this](const CompareSurfaceLayout& surface, CompareTabState& compare_tab) {
                return BuildCompareRightInteractionLayout(surface, compare_tab);
              },
          .sync_compare_selection_from_viewport =
              [this](CompareTabState& compare_tab, bool reveal_selection) {
                SyncCompareSelectionFromViewport(compare_tab, reveal_selection);
              },
          .reset_caret_blink = [this]() { ResetCaretBlink(); },
          .read_primary_selection_text = [this]() { return ReadPrimarySelectionText(); },
          .refresh_compare_tab_derived_state =
              [this](CompareTabState& compare_tab) { RefreshCompareTabDerivedState(compare_tab); },
          .note_active_editable_edit =
              [this]() { NoteActiveEditableEdit(); },
          .request_frame = [this]() { RequestPreparedFrame(); },
          .build_compare_collapsed_context_action_rects =
              [this](const SDL_FRect& row_rect, const compare::ComparePresentationRow& row) {
                return BuildCollapsedContextActionRects(
                    editor_text_renderer_, row_rect, row.previous_hunk_index >= 0,
                    row.next_hunk_index >= 0);
              },
          .expand_compare_collapsed_context =
              [this](CompareTabState& compare_tab,
                     std::size_t presentation_row,
                     CompareCollapsedContextAction action) {
                const bool expanded =
                    ExpandCompareCollapsedContext(compare_tab, presentation_row, action);
                if (expanded) {
                  RequestPreparedFrame();
                }
                return expanded;
              },
          .clear_drag_state = [this]() { ClearDragState(); },
          .compare_control_at =
              [this](const SDL_FRect& rect, const CompareSurfaceLayout& surface, const CompareTabState& compare_tab,
                     float x, float y) {
                return CompareControlAt(editor_text_renderer_, MakeCompareControlGeometry(rect, surface), compare_tab,
                                        x, y);
              },
          .run_compare_control =
              [this](CompareTabState& compare_tab, const CompareControlHit& hit) {
                return MakeCompareReviewService().RunControl(compare_tab, hit);
              },
      });
  return *glue_->compare_mouse_coordinator;
}

}
