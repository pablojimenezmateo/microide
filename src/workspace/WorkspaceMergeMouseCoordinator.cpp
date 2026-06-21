#include "workspace/WorkspaceMergeMouseCoordinator.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <utility>

#include "workspace/CompareMergeService.h"
#include "workspace/WorkspaceActionCoordinator.h"
#include "workspace/WorkspaceLayout.h"

namespace microide::workspace {

namespace {

bool MergeHoverStatesEqual(const std::optional<MergeHoverState>& lhs,
                           const std::optional<MergeHoverState>& rhs) {
  return lhs.has_value() == rhs.has_value() &&
         (!lhs.has_value() ||
          (lhs->kind == rhs->kind && lhs->conflict_index == rhs->conflict_index &&
           lhs->preview_choice == rhs->preview_choice));
}

}  // namespace

MergeMouseCoordinator::MergeMouseCoordinator(ProjectWorkspaceState& state,
                                             InteractionState& interaction_state,
                                             Operations operations)
    : state_(state),
      interaction_state_(interaction_state),
      operations_(std::move(operations)) {}

bool MergeMouseCoordinator::ActiveTabIsMerge() const {
  return state_.focused_group().active_tab_index < state_.focused_group().open_tabs.size() &&
         state_.focused_group().open_tabs[state_.focused_group().active_tab_index].kind == TabEntry::Kind::Merge;
}

MergeTabState* MergeMouseCoordinator::ActiveMergeTab() const {
  if (!ActiveTabIsMerge()) {
    return nullptr;
  }
  auto& tab = state_.focused_group().open_tabs[state_.focused_group().active_tab_index];
  return tab.merge.has_value() ? &tab.merge.value() : nullptr;
}

bool MergeMouseCoordinator::HandleButtonDown(const SDL_Event& event,
                                             const WorkspaceLayout& layout) {
  if (!ActiveTabIsMerge()) {
    return false;
  }

  MergeTabState* merge_tab = ActiveMergeTab();
  if (merge_tab == nullptr) {
    return false;
  }

  const auto surface_layout =
      operations_.compute_merge_surface_layout(layout.editor_surface, *merge_tab);
  const auto scroll_layout =
      operations_.compute_merge_scroll_layout(layout.editor_surface, surface_layout, *merge_tab);
  merge_tab->scroll_row = scroll_layout.vertical_scroll;
  merge_tab->horizontal_scroll = scroll_layout.horizontal_scroll;
  merge_tab->result_viewport.SetScrollLine(static_cast<std::size_t>(std::max(0, merge_tab->scroll_row)));
  merge_tab->result_viewport.SetHorizontalScroll(merge_tab->horizontal_scroll);
  merge_tab->scroll_row = static_cast<int>(merge_tab->result_viewport.scroll_line());
  merge_tab->horizontal_scroll = merge_tab->result_viewport.horizontal_scroll();
  const auto interaction =
      operations_.build_merge_interaction_layout(layout.editor_surface, surface_layout, *merge_tab);
  const SDL_FRect left_divider_rect =
      MakeRect(surface_layout.center_x - surface_layout.divider_width, layout.editor_surface.y,
               surface_layout.divider_width, layout.editor_surface.h);
  const SDL_FRect right_divider_rect =
      MakeRect(surface_layout.right_x - surface_layout.divider_width, layout.editor_surface.y,
               surface_layout.divider_width, layout.editor_surface.h);
  if (Contains(left_divider_rect, event.button.x, event.button.y)) {
    interaction_state_.drag_target = DragTarget::MergeLeftDivider;
    state_.surface.focus = FocusTarget::Editor;
    return true;
  }
  if (Contains(right_divider_rect, event.button.x, event.button.y)) {
    interaction_state_.drag_target = DragTarget::MergeRightDivider;
    state_.surface.focus = FocusTarget::Editor;
    return true;
  }

  const auto toolbar = operations_.compute_merge_toolbar_layout(layout.editor_surface, surface_layout);
  if (Contains(toolbar.prev_rect, event.button.x, event.button.y)) {
    operations_.move_merge_selection(-1);
    return true;
  }
  if (Contains(toolbar.next_rect, event.button.x, event.button.y)) {
    operations_.move_merge_selection(1);
    return true;
  }
  if (Contains(toolbar.save_rect, event.button.x, event.button.y)) {
    operations_.execute_action(ActionId::Save, {}, ActionSource::Menu);
    return true;
  }
  if (Contains(toolbar.open_rect, event.button.x, event.button.y)) {
    operations_.open_merge_result_file();
    return true;
  }
  if (operations_.merge_secondary_toolbar_button_rect) {
    const auto hit = [&](std::string_view label) {
      const auto rect = operations_.merge_secondary_toolbar_button_rect(
          layout.editor_surface, surface_layout, label);
      return rect.has_value() && Contains(*rect, event.button.x, event.button.y);
    };
    if (hit("Unresolved") && operations_.jump_next_unresolved_merge_conflict) {
      operations_.jump_next_unresolved_merge_conflict();
      return true;
    }
    if (hit("Toggle Base") && operations_.toggle_merge_base_pane) {
      operations_.toggle_merge_base_pane();
      return true;
    }
    if (hit("Mark Resolved") && operations_.mark_merge_resolved) {
      operations_.mark_merge_resolved();
      return true;
    }
  }

  const auto source_button_rect = [&](const MergeTrackedConflict& conflict, bool incoming) {
    return operations_.build_merge_source_action_button_rect(surface_layout, interaction, conflict,
                                                            incoming);
  };
  const auto result_action_rects = [&](const MergeTrackedConflict& conflict) {
    return operations_.build_merge_result_action_button_rects(surface_layout, interaction, conflict);
  };

  if (merge_tab->hover_state.has_value() &&
      merge_tab->hover_state->conflict_index < merge_tab->conflicts.size()) {
    const auto& hovered_conflict = merge_tab->conflicts[merge_tab->hover_state->conflict_index];
    if (merge_tab->hover_state->kind == MergeHoverState::Kind::IncomingConflict ||
        merge_tab->hover_state->kind == MergeHoverState::Kind::IncomingAccept) {
      if (Contains(source_button_rect(hovered_conflict, true), event.button.x, event.button.y)) {
        merge_tab->selected_hunk = merge_tab->hover_state->conflict_index;
        operations_.apply_merge_choice(compare::MergeChoice::Incoming);
        return true;
      }
    }
    if (merge_tab->hover_state->kind == MergeHoverState::Kind::CurrentConflict ||
        merge_tab->hover_state->kind == MergeHoverState::Kind::CurrentAccept) {
      if (Contains(source_button_rect(hovered_conflict, false), event.button.x, event.button.y)) {
        merge_tab->selected_hunk = merge_tab->hover_state->conflict_index;
        operations_.apply_merge_choice(compare::MergeChoice::Current);
        return true;
      }
    }
    if ((merge_tab->hover_state->kind == MergeHoverState::Kind::ResultConflict ||
         merge_tab->hover_state->kind == MergeHoverState::Kind::ResultAction) &&
        hovered_conflict.valid) {
      const auto action_rects = result_action_rects(hovered_conflict);
      if (Contains(action_rects[0], event.button.x, event.button.y)) {
        merge_tab->selected_hunk = merge_tab->hover_state->conflict_index;
        operations_.apply_merge_choice(compare::MergeChoice::Base);
        return true;
      }
      if (Contains(action_rects[1], event.button.x, event.button.y)) {
        merge_tab->selected_hunk = merge_tab->hover_state->conflict_index;
        operations_.apply_merge_choice(compare::MergeChoice::Incoming);
        return true;
      }
      if (Contains(action_rects[2], event.button.x, event.button.y)) {
        merge_tab->selected_hunk = merge_tab->hover_state->conflict_index;
        operations_.apply_merge_choice(compare::MergeChoice::Current);
        return true;
      }
      if (Contains(action_rects[3], event.button.x, event.button.y)) {
        merge_tab->selected_hunk = merge_tab->hover_state->conflict_index;
        operations_.apply_merge_choice(compare::MergeChoice::Both);
        return true;
      }
    }
  }

  if (scroll_layout.vertical_scrollbar.has_value() &&
      Contains(scroll_layout.vertical_scrollbar->track, event.button.x, event.button.y)) {
    interaction_state_.drag_target = DragTarget::CompareVerticalScrollbar;
    interaction_state_.drag_scrollbar_offset =
        Contains(scroll_layout.vertical_scrollbar->thumb, event.button.x, event.button.y)
            ? static_cast<float>(event.button.y) - scroll_layout.vertical_scrollbar->thumb.y
            : scroll_layout.vertical_scrollbar->thumb.h * 0.5f;
    merge_tab->scroll_row = std::clamp(
        static_cast<int>(std::lround(ScrollUnitsForPointer(
            *scroll_layout.vertical_scrollbar, static_cast<float>(event.button.y),
            interaction_state_.drag_scrollbar_offset))),
        0, scroll_layout.max_vertical_scroll);
    merge_tab->result_viewport.SetScrollLine(static_cast<std::size_t>(std::max(0, merge_tab->scroll_row)));
    merge_tab->scroll_row = static_cast<int>(merge_tab->result_viewport.scroll_line());
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
    merge_tab->horizontal_scroll = static_cast<std::size_t>(std::max(
        0L, std::lround(ScrollUnitsForPointer(*scroll_layout.horizontal_scrollbar,
                                              static_cast<float>(event.button.x),
                                              interaction_state_.drag_scrollbar_offset))));
    merge_tab->result_viewport.SetHorizontalScroll(merge_tab->horizontal_scroll);
    merge_tab->horizontal_scroll = merge_tab->result_viewport.horizontal_scroll();
    state_.surface.focus = FocusTarget::Editor;
    return true;
  }

  if (Contains(interaction.result.rect, event.button.x, event.button.y)) {
    const std::size_t previous_selected_hunk = merge_tab->selected_hunk;
    const std::size_t line = ClampTextGridLineAtY(interaction.result.text, event.button.y);
    const std::size_t visual_column = TextGridVisualColumnAtX(interaction.result.text, event.button.x);
    merge_tab->result_viewport.MoveCursorToVisualColumn(
        line, visual_column, (SDL_GetModState() & SDL_KMOD_SHIFT) != 0);
    if (const auto conflict_index =
            operations_.find_merge_tracked_conflict_at_result_line(*merge_tab, line);
        conflict_index.has_value()) {
      merge_tab->selected_hunk = *conflict_index;
    }
    merge_tab->scroll_row = static_cast<int>(merge_tab->result_viewport.scroll_line());
    merge_tab->horizontal_scroll = merge_tab->result_viewport.horizontal_scroll();
    operations_.reset_caret_blink();
    state_.surface.focus = FocusTarget::Editor;
    if (event.button.button == SDL_BUTTON_MIDDLE) {
      if (const std::optional<std::string> text = operations_.read_primary_selection_text();
          text.has_value()) {
        const bool was_dirty = merge_tab->result_viewport.dirty();
        const std::size_t cursor_before_line = merge_tab->result_viewport.cursor_line();
        const std::vector<std::string> before_lines = merge_tab->result_viewport.lines();
        const std::optional<editor::SelectionRange> selection_before =
            merge_tab->result_viewport.selection_range();
        const editor::TextPosition cursor_before{merge_tab->result_viewport.cursor_line(),
                                                 merge_tab->result_viewport.cursor_column()};
        merge_tab->result_viewport.InsertText(*text);
        operations_.update_merge_tracking_after_viewport_edit(*merge_tab, before_lines,
                                                              selection_before, cursor_before);
        operations_.request_active_editable_last_change_redraw();
        if (merge_tab->result_viewport.dirty() != was_dirty) {
          operations_.request_active_editable_blame_neighborhood_redraw(
              cursor_before_line, merge_tab->result_viewport.cursor_line());
          operations_.request_tab_strip_redraw();
        }
      }
      return true;
    }
    if (merge_tab->selected_hunk != previous_selected_hunk) {
      operations_.request_merge_conflict_redraw(previous_selected_hunk);
      operations_.request_merge_conflict_redraw(merge_tab->selected_hunk);
    } else {
      operations_.request_focused_editor_redraw();
    }
    interaction_state_.mouse_selecting = true;
    return true;
  }

  const int clicked_row = static_cast<int>((event.button.y - surface_layout.rows_y) /
                                           std::max(1.0f, surface_layout.line_height));
  if (clicked_row < 0) {
    return false;
  }

  const std::size_t line = static_cast<std::size_t>(std::max(0, merge_tab->scroll_row + clicked_row));
  const std::size_t previous_selected_hunk = merge_tab->selected_hunk;
  if (event.button.x < surface_layout.center_x) {
    if (const auto conflict_index =
            operations_.find_merge_tracked_conflict_at_source_line(*merge_tab, line, true);
        conflict_index.has_value()) {
      merge_tab->selected_hunk = *conflict_index;
      operations_.reveal_active_merge_selection();
    }
  } else if (event.button.x >= surface_layout.right_x) {
    if (const auto conflict_index =
            operations_.find_merge_tracked_conflict_at_source_line(*merge_tab, line, false);
        conflict_index.has_value()) {
      merge_tab->selected_hunk = *conflict_index;
      operations_.reveal_active_merge_selection();
    }
  }
  if (merge_tab->selected_hunk != previous_selected_hunk) {
    operations_.request_merge_conflict_redraw(previous_selected_hunk);
    operations_.request_merge_conflict_redraw(merge_tab->selected_hunk);
  }
  state_.surface.focus = FocusTarget::Editor;
  return true;
}

bool MergeMouseCoordinator::HandleDrag(const SDL_Event& event,
                                       const WorkspaceLayout& layout) {
  if (!ActiveTabIsMerge()) {
    return false;
  }

  if ((interaction_state_.drag_target != DragTarget::MergeLeftDivider &&
       interaction_state_.drag_target != DragTarget::MergeRightDivider &&
       interaction_state_.drag_target != DragTarget::CompareVerticalScrollbar &&
       interaction_state_.drag_target != DragTarget::CompareHorizontalScrollbar)) {
    return false;
  }

  MergeTabState* merge_tab = ActiveMergeTab();
  if (merge_tab == nullptr) {
    operations_.clear_drag_state();
    return false;
  }

  if (interaction_state_.drag_target == DragTarget::MergeLeftDivider ||
      interaction_state_.drag_target == DragTarget::MergeRightDivider) {
    const auto surface_layout =
        operations_.compute_merge_surface_layout(layout.editor_surface, *merge_tab);
    const float content_width =
        std::max(1.0f, surface_layout.left_width + surface_layout.center_width +
                           surface_layout.right_width);
    const float current_left_fraction = surface_layout.left_width / content_width;
    const float current_right_fraction =
        (surface_layout.left_width + surface_layout.center_width) / content_width;
    if (interaction_state_.drag_target == DragTarget::MergeLeftDivider) {
      const float divider_center_x =
          layout.editor_surface.x + 8.0f + surface_layout.gutter_width +
          surface_layout.divider_width * 0.5f;
      const float raw_fraction =
          (static_cast<float>(event.motion.x) - divider_center_x) / content_width;
      merge_tab->left_divider_fraction =
          std::clamp(raw_fraction, surface_layout.min_divider_fraction,
                     current_right_fraction - surface_layout.min_divider_fraction);
    } else {
      const float divider_center_x =
          layout.editor_surface.x + 8.0f + surface_layout.gutter_width * 2.0f +
          surface_layout.divider_width * 1.5f;
      const float raw_fraction =
          (static_cast<float>(event.motion.x) - divider_center_x) / content_width;
      merge_tab->right_divider_fraction =
          std::clamp(raw_fraction,
                     current_left_fraction + surface_layout.min_divider_fraction,
                     1.0f - surface_layout.min_divider_fraction);
    }
    state_.surface.focus = FocusTarget::Editor;
    return true;
  }

  const auto surface_layout =
      operations_.compute_merge_surface_layout(layout.editor_surface, *merge_tab);
  const auto scroll_layout =
      operations_.compute_merge_scroll_layout(layout.editor_surface, surface_layout, *merge_tab);
  merge_tab->scroll_row = scroll_layout.vertical_scroll;
  merge_tab->horizontal_scroll = scroll_layout.horizontal_scroll;
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
    merge_tab->scroll_row = target_scroll;
    merge_tab->result_viewport.SetScrollLine(static_cast<std::size_t>(std::max(0, merge_tab->scroll_row)));
    merge_tab->scroll_row = static_cast<int>(merge_tab->result_viewport.scroll_line());
    state_.surface.focus = FocusTarget::Editor;
    return true;
  }

  if (!scroll_layout.horizontal_scrollbar.has_value()) {
    operations_.clear_drag_state();
    return false;
  }
  merge_tab->horizontal_scroll = static_cast<std::size_t>(std::max(
      0L, std::lround(ScrollUnitsForPointer(*scroll_layout.horizontal_scrollbar,
                                            static_cast<float>(event.motion.x),
                                            interaction_state_.drag_scrollbar_offset))));
  merge_tab->result_viewport.SetHorizontalScroll(merge_tab->horizontal_scroll);
  merge_tab->horizontal_scroll = merge_tab->result_viewport.horizontal_scroll();
  state_.surface.focus = FocusTarget::Editor;
  return true;
}

bool MergeMouseCoordinator::HandleHoverMotion(const SDL_Event& event,
                                              const WorkspaceLayout& layout) {
  if (!ActiveTabIsMerge()) {
    return false;
  }

  MergeTabState* merge_tab = ActiveMergeTab();
  if (merge_tab == nullptr) {
    return false;
  }

  const std::optional<MergeHoverState> previous_hover = merge_tab->hover_state;
  std::optional<MergeHoverState> next_hover;
  if (Contains(layout.editor_surface, event.motion.x, event.motion.y) &&
      !(interaction_state_.mouse_selecting && (event.motion.state & SDL_BUTTON_LMASK) != 0)) {
    const auto surface_layout =
        operations_.compute_merge_surface_layout(layout.editor_surface, *merge_tab);
    const auto scroll_layout =
        operations_.compute_merge_scroll_layout(layout.editor_surface, surface_layout, *merge_tab);
    merge_tab->result_viewport.SetScrollLine(static_cast<std::size_t>(std::max(0, scroll_layout.vertical_scroll)));
    merge_tab->result_viewport.SetHorizontalScroll(scroll_layout.horizontal_scroll);
    merge_tab->scroll_row = static_cast<int>(merge_tab->result_viewport.scroll_line());
    merge_tab->horizontal_scroll = merge_tab->result_viewport.horizontal_scroll();
    const auto interaction =
        operations_.build_merge_interaction_layout(layout.editor_surface, surface_layout, *merge_tab);
    next_hover = operations_.classify_merge_hover_state(
        surface_layout, interaction, *merge_tab, static_cast<float>(event.motion.x),
        static_cast<float>(event.motion.y));
  }

  merge_tab->hover_state = next_hover;
  if (MergeHoverStatesEqual(previous_hover, merge_tab->hover_state)) {
    return false;
  }
  if (previous_hover.has_value()) {
    operations_.request_merge_conflict_redraw(previous_hover->conflict_index);
  }
  if (merge_tab->hover_state.has_value()) {
    operations_.request_merge_conflict_redraw(merge_tab->hover_state->conflict_index);
  }
  return true;
}

bool MergeMouseCoordinator::HandleSelectionMotion(const SDL_Event& event,
                                                  const WorkspaceLayout& layout) {
  if (!ActiveTabIsMerge()) {
    return false;
  }

  MergeTabState* merge_tab = ActiveMergeTab();
  if (merge_tab == nullptr) {
    return false;
  }
  if (merge_tab->hover_state.has_value()) {
    merge_tab->hover_state.reset();
  }

  const auto surface_layout =
      operations_.compute_merge_surface_layout(layout.editor_surface, *merge_tab);
  const auto interaction =
      operations_.build_merge_interaction_layout(layout.editor_surface, surface_layout, *merge_tab);
  if (!Contains(interaction.result.rect, event.motion.x, event.motion.y)) {
    return false;
  }

  const std::size_t line = ClampTextGridLineAtY(interaction.result.text, event.motion.y);
  const std::size_t previous_selected_hunk = merge_tab->selected_hunk;
  const std::size_t visual_column = TextGridVisualColumnAtX(interaction.result.text, event.motion.x);
  merge_tab->result_viewport.MoveCursorToVisualColumn(line, visual_column, true);
  if (const auto conflict_index = operations_.find_merge_tracked_conflict_at_result_line(*merge_tab, line);
      conflict_index.has_value()) {
    merge_tab->selected_hunk = *conflict_index;
  }
  merge_tab->scroll_row = static_cast<int>(merge_tab->result_viewport.scroll_line());
  merge_tab->horizontal_scroll = merge_tab->result_viewport.horizontal_scroll();
  operations_.reset_caret_blink();
  if (merge_tab->selected_hunk != previous_selected_hunk) {
    operations_.request_merge_conflict_redraw(previous_selected_hunk);
    operations_.request_merge_conflict_redraw(merge_tab->selected_hunk);
  } else {
    operations_.request_focused_editor_redraw();
  }
  state_.surface.focus = FocusTarget::Editor;
  return true;
}

bool MergeMouseCoordinator::HandleWheel(const SDL_Event& event,
                                        const WorkspaceLayout& layout,
                                        int vertical_ticks,
                                        int horizontal_ticks) {
  if (!ActiveTabIsMerge() ||
      !Contains(layout.editor_surface, event.wheel.mouse_x, event.wheel.mouse_y)) {
    return false;
  }

  if (horizontal_ticks != 0) {
    operations_.scroll_merge_columns(-horizontal_ticks * 3);
  } else {
    MergeTabState* merge_tab = ActiveMergeTab();
    if (merge_tab != nullptr) {
      const auto surface_layout =
          operations_.compute_merge_surface_layout(layout.editor_surface, *merge_tab);
      const auto scroll_layout =
          operations_.compute_merge_scroll_layout(layout.editor_surface, surface_layout, *merge_tab);
      merge_tab->scroll_row =
          std::clamp(scroll_layout.vertical_scroll - vertical_ticks * 3, 0,
                     scroll_layout.max_vertical_scroll);
      merge_tab->result_viewport.SetScrollLine(static_cast<std::size_t>(std::max(0, merge_tab->scroll_row)));
      merge_tab->scroll_row = static_cast<int>(merge_tab->result_viewport.scroll_line());
    }
  }
  state_.surface.focus = FocusTarget::Editor;
  return true;
}

MergeMouseCoordinator WorkspaceShell::MakeMergeMouseCoordinator() {
  return MergeMouseCoordinator(
      context_.current_project_state,
      context_.interaction_state,
      MergeMouseCoordinator::Operations{
          .compute_merge_surface_layout =
              [this](const SDL_FRect& rect, const MergeTabState& merge_tab) {
                return ComputeMergeSurfaceLayout(rect, merge_tab);
              },
          .compute_merge_scroll_layout =
              [this](const SDL_FRect& rect, const MergeSurfaceLayout& surface,
                     const MergeTabState& merge_tab) {
                return ComputeMergeScrollLayout(rect, surface, merge_tab);
              },
          .build_merge_interaction_layout =
              [this](const SDL_FRect& rect, const MergeSurfaceLayout& surface,
                     MergeTabState& merge_tab) {
                return BuildMergeInteractionLayout(rect, surface, merge_tab);
              },
          .compute_merge_toolbar_layout =
              [this](const SDL_FRect& rect, const MergeSurfaceLayout& surface) {
                return ComputeMergeToolbarLayout(rect, surface);
              },
          .execute_action =
              [this](ActionId id, const std::vector<std::string>& args, ActionSource source) {
                return ActionCoordinator(MakeActionContext()).Execute(id, args, source);
              },
          .open_merge_result_file =
              [this]() { MakeCompareMergeService().OpenMergeResultFile(); },
          .build_merge_source_action_button_rect =
              [this](const MergeSurfaceLayout& surface, const MergeInteractionLayout& interaction,
                     const MergeTrackedConflict& conflict, bool incoming) {
                return BuildMergeSourceActionButtonRect(surface, interaction, conflict, incoming);
              },
          .build_merge_result_action_button_rects =
              [this](const MergeSurfaceLayout& surface, const MergeInteractionLayout& interaction,
                     const MergeTrackedConflict& conflict) {
                return BuildMergeResultActionButtonRects(surface, interaction, conflict);
              },
          .apply_merge_choice =
              [this](compare::MergeChoice choice) {
                MakeCompareMergeService().ApplyMergeChoice(choice);
              },
          .read_primary_selection_text = [this]() { return ReadPrimarySelectionText(); },
          .update_merge_tracking_after_viewport_edit =
              [this](MergeTabState& merge_tab, const std::vector<std::string>& before_lines,
                     std::optional<editor::SelectionRange> selection_before,
                     editor::TextPosition cursor_before) {
                UpdateMergeTrackingAfterViewportEdit(merge_tab, before_lines, selection_before,
                                                     cursor_before);
              },
          .request_active_editable_last_change_redraw =
              [this]() { RequestActiveEditableLastChangeRedraw(); },
          .request_active_editable_blame_neighborhood_redraw =
              [this](std::size_t before_line, std::size_t after_line) {
                RequestActiveEditableBlameNeighborhoodRedraw(before_line, after_line);
              },
          .request_tab_strip_redraw = [this]() { RequestTabStripRedraw(); },
          .request_merge_conflict_redraw =
              [this](std::size_t conflict_index) { RequestMergeConflictRedraw(conflict_index); },
          .request_focused_editor_redraw = [this]() { RequestFocusedEditorRedraw(); },
          .clear_drag_state = [this]() { ClearDragState(); },
          .find_merge_tracked_conflict_at_source_line =
              [this](const MergeTabState& merge_tab, std::size_t line, bool incoming) {
                return FindMergeTrackedConflictAtSourceLine(merge_tab, line, incoming);
              },
          .find_merge_tracked_conflict_at_result_line =
              [this](const MergeTabState& merge_tab, std::size_t line) {
                return FindMergeTrackedConflictAtResultLine(merge_tab, line);
              },
          .reveal_active_merge_selection = [this]() { RevealActiveMergeSelection(); },
          .classify_merge_hover_state =
              [this](const MergeSurfaceLayout& surface, const MergeInteractionLayout& interaction,
                     const MergeTabState& merge_tab, float x, float y) {
                return ClassifyMergeHoverState(surface, interaction, merge_tab, x, y);
              },
          .reset_caret_blink = [this]() { ResetCaretBlink(); },
          .move_merge_selection =
              [this](int delta) { MakeCompareMergeService().MoveMergeSelection(delta); },
          .scroll_merge_columns =
              [this](int delta) { MakeCompareMergeService().ScrollMergeColumns(delta); },
          .mark_merge_resolved = [this]() { MarkMergeResolved(); },
          .toggle_merge_base_pane = [this]() { ToggleMergeBasePane(); },
          .jump_next_unresolved_merge_conflict =
              [this]() { JumpNextUnresolvedMergeConflict(); },
          .merge_secondary_toolbar_button_rect =
              [this](const SDL_FRect& rect, const MergeSurfaceLayout& surface,
                     std::string_view label) {
                return ComputeMergeSecondaryToolbarButtonRect(rect, surface, label);
              },
      });
}

}  // namespace microide::workspace
