#include "workspace/WorkspaceMergeMouseCoordinator.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>

#include "workspace/WorkspaceActionCoordinator.h"
#include "workspace/WorkspaceLayout.h"

namespace microide::workspace {

namespace {

constexpr float kMinMergePaneWidth = 140.0f;

bool MergeHoverStatesEqual(const std::optional<MergeHoverState>& lhs,
                           const std::optional<MergeHoverState>& rhs) {
  return lhs.has_value() == rhs.has_value() &&
         (!lhs.has_value() ||
          (lhs->kind == rhs->kind && lhs->conflict_index == rhs->conflict_index &&
           lhs->preview_choice == rhs->preview_choice));
}

}  // namespace

WorkspaceShell::MergeMouseCoordinator::MergeMouseCoordinator(WorkspaceShell& shell)
    : shell_(shell) {}

bool WorkspaceShell::MergeMouseCoordinator::HandleButtonDown(
    const SDL_Event& event,
    const WorkspaceLayout& layout) {
  if (!shell_.ActiveTabIsMerge()) {
    return false;
  }

  MergeTabState* merge_tab = shell_.ActiveMergeTab();
  if (merge_tab == nullptr) {
    return false;
  }

  const MergeSurfaceLayout surface_layout =
      shell_.ComputeMergeSurfaceLayout(layout.editor_surface, *merge_tab);
  const auto scroll_layout =
      shell_.ComputeMergeScrollLayout(layout.editor_surface, surface_layout, *merge_tab);
  merge_tab->scroll_row = scroll_layout.vertical_scroll;
  merge_tab->horizontal_scroll = scroll_layout.horizontal_scroll;
  merge_tab->result_viewport.SetScrollLine(
      static_cast<std::size_t>(std::max(0, merge_tab->scroll_row)));
  merge_tab->result_viewport.SetHorizontalScroll(merge_tab->horizontal_scroll);
  merge_tab->scroll_row = static_cast<int>(merge_tab->result_viewport.scroll_line());
  merge_tab->horizontal_scroll = merge_tab->result_viewport.horizontal_scroll();
  const MergeInteractionLayout interaction =
      shell_.BuildMergeInteractionLayout(layout.editor_surface, surface_layout, *merge_tab);
  const SDL_FRect left_divider_rect =
      MakeRect(surface_layout.center_x - surface_layout.divider_width,
               layout.editor_surface.y, surface_layout.divider_width,
               layout.editor_surface.h);
  const SDL_FRect right_divider_rect =
      MakeRect(surface_layout.right_x - surface_layout.divider_width,
               layout.editor_surface.y, surface_layout.divider_width,
               layout.editor_surface.h);
  if (Contains(left_divider_rect, event.button.x, event.button.y)) {
    shell_.surface_.drag_target = DragTarget::MergeLeftDivider;
    shell_.surface_.focus = FocusTarget::Editor;
    return true;
  }
  if (Contains(right_divider_rect, event.button.x, event.button.y)) {
    shell_.surface_.drag_target = DragTarget::MergeRightDivider;
    shell_.surface_.focus = FocusTarget::Editor;
    return true;
  }

  const MergeToolbarLayout toolbar =
      shell_.ComputeMergeToolbarLayout(layout.editor_surface, surface_layout);
  if (Contains(toolbar.prev_rect, event.button.x, event.button.y)) {
    shell_.MoveMergeSelection(-1);
    return true;
  }
  if (Contains(toolbar.next_rect, event.button.x, event.button.y)) {
    shell_.MoveMergeSelection(1);
    return true;
  }
  if (Contains(toolbar.save_rect, event.button.x, event.button.y)) {
    ActionCoordinator(shell_).Execute(ActionId::Save, {}, ActionSource::Menu);
    return true;
  }
  if (Contains(toolbar.open_rect, event.button.x, event.button.y)) {
    shell_.OpenMergeResultFile();
    return true;
  }

  const auto source_button_rect =
      [&](const MergeTrackedConflict& conflict, bool incoming) {
        return shell_.BuildMergeSourceActionButtonRect(surface_layout, interaction,
                                                       conflict, incoming);
      };
  const auto result_action_rects =
      [&](const MergeTrackedConflict& conflict) -> std::array<SDL_FRect, 4> {
        return shell_.BuildMergeResultActionButtonRects(surface_layout, interaction,
                                                        conflict);
      };

  if (merge_tab->hover_state.has_value() &&
      merge_tab->hover_state->conflict_index < merge_tab->conflicts.size()) {
    const auto& hovered_conflict =
        merge_tab->conflicts[merge_tab->hover_state->conflict_index];
    if (merge_tab->hover_state->kind == MergeHoverState::Kind::IncomingConflict ||
        merge_tab->hover_state->kind == MergeHoverState::Kind::IncomingAccept) {
      if (Contains(source_button_rect(hovered_conflict, true), event.button.x,
                   event.button.y)) {
        merge_tab->selected_hunk = merge_tab->hover_state->conflict_index;
        shell_.ApplyMergeChoice(compare::MergeChoice::Incoming);
        return true;
      }
    }
    if (merge_tab->hover_state->kind == MergeHoverState::Kind::CurrentConflict ||
        merge_tab->hover_state->kind == MergeHoverState::Kind::CurrentAccept) {
      if (Contains(source_button_rect(hovered_conflict, false), event.button.x,
                   event.button.y)) {
        merge_tab->selected_hunk = merge_tab->hover_state->conflict_index;
        shell_.ApplyMergeChoice(compare::MergeChoice::Current);
        return true;
      }
    }
    if ((merge_tab->hover_state->kind == MergeHoverState::Kind::ResultConflict ||
         merge_tab->hover_state->kind == MergeHoverState::Kind::ResultAction) &&
        hovered_conflict.valid) {
      const auto action_rects = result_action_rects(hovered_conflict);
      if (Contains(action_rects[0], event.button.x, event.button.y)) {
        merge_tab->selected_hunk = merge_tab->hover_state->conflict_index;
        shell_.ApplyMergeChoice(compare::MergeChoice::Base);
        return true;
      }
      if (Contains(action_rects[1], event.button.x, event.button.y)) {
        merge_tab->selected_hunk = merge_tab->hover_state->conflict_index;
        shell_.ApplyMergeChoice(compare::MergeChoice::Incoming);
        return true;
      }
      if (Contains(action_rects[2], event.button.x, event.button.y)) {
        merge_tab->selected_hunk = merge_tab->hover_state->conflict_index;
        shell_.ApplyMergeChoice(compare::MergeChoice::Current);
        return true;
      }
      if (Contains(action_rects[3], event.button.x, event.button.y)) {
        merge_tab->selected_hunk = merge_tab->hover_state->conflict_index;
        shell_.ApplyMergeChoice(compare::MergeChoice::Both);
        return true;
      }
    }
  }

  if (scroll_layout.vertical_scrollbar.has_value() &&
      Contains(scroll_layout.vertical_scrollbar->track, event.button.x,
               event.button.y)) {
    shell_.surface_.drag_target = DragTarget::CompareVerticalScrollbar;
    shell_.surface_.drag_scrollbar_offset =
        Contains(scroll_layout.vertical_scrollbar->thumb, event.button.x,
                 event.button.y)
            ? static_cast<float>(event.button.y) -
                  scroll_layout.vertical_scrollbar->thumb.y
            : scroll_layout.vertical_scrollbar->thumb.h * 0.5f;
    merge_tab->scroll_row = std::clamp(
        static_cast<int>(std::lround(ScrollUnitsForPointer(
            *scroll_layout.vertical_scrollbar, static_cast<float>(event.button.y),
            shell_.surface_.drag_scrollbar_offset))),
        0, scroll_layout.max_vertical_scroll);
    merge_tab->result_viewport.SetScrollLine(
        static_cast<std::size_t>(std::max(0, merge_tab->scroll_row)));
    merge_tab->scroll_row = static_cast<int>(merge_tab->result_viewport.scroll_line());
    shell_.surface_.focus = FocusTarget::Editor;
    return true;
  }

  if (scroll_layout.horizontal_scrollbar.has_value() &&
      Contains(scroll_layout.horizontal_scrollbar->track, event.button.x,
               event.button.y)) {
    shell_.surface_.drag_target = DragTarget::CompareHorizontalScrollbar;
    shell_.surface_.drag_scrollbar_offset =
        Contains(scroll_layout.horizontal_scrollbar->thumb, event.button.x,
                 event.button.y)
            ? static_cast<float>(event.button.x) -
                  scroll_layout.horizontal_scrollbar->thumb.x
            : scroll_layout.horizontal_scrollbar->thumb.w * 0.5f;
    merge_tab->horizontal_scroll = static_cast<std::size_t>(std::max(
        0L, std::lround(ScrollUnitsForPointer(
                *scroll_layout.horizontal_scrollbar,
                static_cast<float>(event.button.x),
                shell_.surface_.drag_scrollbar_offset))));
    merge_tab->result_viewport.SetHorizontalScroll(merge_tab->horizontal_scroll);
    merge_tab->horizontal_scroll = merge_tab->result_viewport.horizontal_scroll();
    shell_.surface_.focus = FocusTarget::Editor;
    return true;
  }

  if (Contains(interaction.result.rect, event.button.x, event.button.y)) {
    const std::size_t previous_selected_hunk = merge_tab->selected_hunk;
    const std::size_t line =
        ClampTextGridLineAtY(interaction.result.text, event.button.y);
    const std::size_t visual_column =
        TextGridVisualColumnAtX(interaction.result.text, event.button.x);
    merge_tab->result_viewport.MoveCursorToVisualColumn(
        line, visual_column, (SDL_GetModState() & SDL_KMOD_SHIFT) != 0);
    if (const auto conflict_index =
            shell_.FindMergeTrackedConflictAtResultLine(*merge_tab, line);
        conflict_index.has_value()) {
      merge_tab->selected_hunk = *conflict_index;
    }
    merge_tab->scroll_row =
        static_cast<int>(merge_tab->result_viewport.scroll_line());
    merge_tab->horizontal_scroll = merge_tab->result_viewport.horizontal_scroll();
    shell_.ResetCaretBlink();
    shell_.surface_.focus = FocusTarget::Editor;
    if (event.button.button == SDL_BUTTON_MIDDLE) {
      if (const std::optional<std::string> text = shell_.ReadPrimarySelectionText();
          text.has_value()) {
        const bool was_dirty = merge_tab->result_viewport.dirty();
        const std::size_t cursor_before_line = merge_tab->result_viewport.cursor_line();
        const std::vector<std::string> before_lines = merge_tab->result_viewport.lines();
        const std::optional<editor::SelectionRange> selection_before =
            merge_tab->result_viewport.selection_range();
        const editor::TextPosition cursor_before{merge_tab->result_viewport.cursor_line(),
                                                 merge_tab->result_viewport.cursor_column()};
        merge_tab->result_viewport.InsertText(*text);
        shell_.UpdateMergeTrackingAfterViewportEdit(*merge_tab, before_lines, selection_before,
                                                    cursor_before);
        shell_.RequestActiveEditableChangeRedraw(before_lines, merge_tab->result_viewport.lines());
        if (merge_tab->result_viewport.dirty() != was_dirty) {
          shell_.RequestActiveEditableBlameNeighborhoodRedraw(
              cursor_before_line, merge_tab->result_viewport.cursor_line());
          shell_.RequestTabStripRedraw();
        }
      }
      return true;
    }
    if (merge_tab->selected_hunk != previous_selected_hunk) {
      shell_.RequestMergeConflictRedraw(previous_selected_hunk);
      shell_.RequestMergeConflictRedraw(merge_tab->selected_hunk);
    } else {
      shell_.RequestFocusedEditorRedraw();
    }
    shell_.surface_.mouse_selecting = true;
    return true;
  }

  const int clicked_row = static_cast<int>(
      (event.button.y - surface_layout.rows_y) /
      std::max(1.0f, surface_layout.line_height));
  if (clicked_row < 0) {
    return false;
  }

  const std::size_t line = static_cast<std::size_t>(
      std::max(0, merge_tab->scroll_row + clicked_row));
  const std::size_t previous_selected_hunk = merge_tab->selected_hunk;
  if (event.button.x < surface_layout.center_x) {
    if (const auto conflict_index =
            shell_.FindMergeTrackedConflictAtSourceLine(*merge_tab, line, true);
        conflict_index.has_value()) {
      merge_tab->selected_hunk = *conflict_index;
      shell_.RevealActiveMergeSelection();
    }
  } else if (event.button.x >= surface_layout.right_x) {
    if (const auto conflict_index =
            shell_.FindMergeTrackedConflictAtSourceLine(*merge_tab, line, false);
        conflict_index.has_value()) {
      merge_tab->selected_hunk = *conflict_index;
      shell_.RevealActiveMergeSelection();
    }
  }
  if (merge_tab->selected_hunk != previous_selected_hunk) {
    shell_.RequestMergeConflictRedraw(previous_selected_hunk);
    shell_.RequestMergeConflictRedraw(merge_tab->selected_hunk);
  }
  shell_.surface_.focus = FocusTarget::Editor;
  return true;
}

bool WorkspaceShell::MergeMouseCoordinator::HandleDrag(
    const SDL_Event& event,
    const WorkspaceLayout& layout) {
  if (!shell_.ActiveTabIsMerge()) {
    return false;
  }

  if ((shell_.surface_.drag_target != DragTarget::MergeLeftDivider &&
       shell_.surface_.drag_target != DragTarget::MergeRightDivider &&
       shell_.surface_.drag_target != DragTarget::CompareVerticalScrollbar &&
       shell_.surface_.drag_target != DragTarget::CompareHorizontalScrollbar)) {
    return false;
  }

  MergeTabState* merge_tab = shell_.ActiveMergeTab();
  if (merge_tab == nullptr) {
    shell_.surface_.drag_target = DragTarget::None;
    shell_.surface_.drag_scrollbar_offset = 0.0f;
    return false;
  }

  if (shell_.surface_.drag_target == DragTarget::MergeLeftDivider ||
      shell_.surface_.drag_target == DragTarget::MergeRightDivider) {
    const MergeSurfaceLayout surface_layout =
        shell_.ComputeMergeSurfaceLayout(layout.editor_surface, *merge_tab);
    const float content_width =
        std::max(1.0f, surface_layout.left_width + surface_layout.center_width +
                           surface_layout.right_width);
    const float min_fraction =
        std::min(1.0f / 3.0f,
                 kMinMergePaneWidth / std::max(content_width, 1.0f));
    const float current_left_fraction = surface_layout.left_width / content_width;
    const float current_right_fraction =
        (surface_layout.left_width + surface_layout.center_width) / content_width;
    if (shell_.surface_.drag_target == DragTarget::MergeLeftDivider) {
      const float divider_center_x =
          layout.editor_surface.x + 8.0f + surface_layout.gutter_width +
          surface_layout.divider_width * 0.5f;
      const float raw_fraction =
          (static_cast<float>(event.motion.x) - divider_center_x) / content_width;
      merge_tab->left_divider_fraction =
          std::clamp(raw_fraction, min_fraction,
                     current_right_fraction - min_fraction);
    } else {
      const float divider_center_x =
          layout.editor_surface.x + 8.0f + surface_layout.gutter_width * 2.0f +
          surface_layout.divider_width * 1.5f;
      const float raw_fraction =
          (static_cast<float>(event.motion.x) - divider_center_x) / content_width;
      merge_tab->right_divider_fraction =
          std::clamp(raw_fraction, current_left_fraction + min_fraction,
                     1.0f - min_fraction);
    }
    shell_.surface_.focus = FocusTarget::Editor;
    return true;
  }

  const MergeSurfaceLayout surface_layout =
      shell_.ComputeMergeSurfaceLayout(layout.editor_surface, *merge_tab);
  const auto scroll_layout =
      shell_.ComputeMergeScrollLayout(layout.editor_surface, surface_layout, *merge_tab);
  merge_tab->scroll_row = scroll_layout.vertical_scroll;
  merge_tab->horizontal_scroll = scroll_layout.horizontal_scroll;
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
    merge_tab->scroll_row = target_scroll;
    merge_tab->result_viewport.SetScrollLine(
        static_cast<std::size_t>(std::max(0, merge_tab->scroll_row)));
    merge_tab->scroll_row = static_cast<int>(merge_tab->result_viewport.scroll_line());
    shell_.surface_.focus = FocusTarget::Editor;
    return true;
  }

  if (!scroll_layout.horizontal_scrollbar.has_value()) {
    shell_.surface_.drag_target = DragTarget::None;
    shell_.surface_.drag_scrollbar_offset = 0.0f;
    return false;
  }
  merge_tab->horizontal_scroll = static_cast<std::size_t>(std::max(
      0L, std::lround(ScrollUnitsForPointer(*scroll_layout.horizontal_scrollbar,
                                            static_cast<float>(event.motion.x),
                                            shell_.surface_.drag_scrollbar_offset))));
  merge_tab->result_viewport.SetHorizontalScroll(merge_tab->horizontal_scroll);
  merge_tab->horizontal_scroll = merge_tab->result_viewport.horizontal_scroll();
  shell_.surface_.focus = FocusTarget::Editor;
  return true;
}

bool WorkspaceShell::MergeMouseCoordinator::HandleHoverMotion(
    const SDL_Event& event,
    const WorkspaceLayout& layout) {
  if (!shell_.ActiveTabIsMerge()) {
    return false;
  }

  MergeTabState* merge_tab = shell_.ActiveMergeTab();
  if (merge_tab == nullptr) {
    return false;
  }

  const std::optional<MergeHoverState> previous_hover = merge_tab->hover_state;
  std::optional<MergeHoverState> next_hover;
  if (Contains(layout.editor_surface, event.motion.x, event.motion.y) &&
      !(shell_.surface_.mouse_selecting &&
        (event.motion.state & SDL_BUTTON_LMASK) != 0)) {
    const MergeSurfaceLayout surface_layout =
        shell_.ComputeMergeSurfaceLayout(layout.editor_surface, *merge_tab);
    const auto scroll_layout =
        shell_.ComputeMergeScrollLayout(layout.editor_surface, surface_layout, *merge_tab);
    merge_tab->result_viewport.SetScrollLine(
        static_cast<std::size_t>(std::max(0, scroll_layout.vertical_scroll)));
    merge_tab->result_viewport.SetHorizontalScroll(scroll_layout.horizontal_scroll);
    merge_tab->scroll_row = static_cast<int>(merge_tab->result_viewport.scroll_line());
    merge_tab->horizontal_scroll = merge_tab->result_viewport.horizontal_scroll();
    const MergeInteractionLayout interaction =
        shell_.BuildMergeInteractionLayout(layout.editor_surface, surface_layout,
                                           *merge_tab);
    next_hover = shell_.ClassifyMergeHoverState(
        surface_layout, interaction, *merge_tab,
        static_cast<float>(event.motion.x), static_cast<float>(event.motion.y));
  }

  merge_tab->hover_state = next_hover;
  if (MergeHoverStatesEqual(previous_hover, merge_tab->hover_state)) {
    return false;
  }
  if (previous_hover.has_value()) {
    shell_.RequestMergeConflictRedraw(previous_hover->conflict_index);
  }
  if (merge_tab->hover_state.has_value()) {
    shell_.RequestMergeConflictRedraw(merge_tab->hover_state->conflict_index);
  }
  return true;
}

bool WorkspaceShell::MergeMouseCoordinator::HandleSelectionMotion(
    const SDL_Event& event,
    const WorkspaceLayout& layout) {
  if (!shell_.ActiveTabIsMerge()) {
    return false;
  }

  MergeTabState* merge_tab = shell_.ActiveMergeTab();
  if (merge_tab == nullptr) {
    return false;
  }
  if (merge_tab->hover_state.has_value()) {
    merge_tab->hover_state.reset();
  }

  const MergeSurfaceLayout surface_layout =
      shell_.ComputeMergeSurfaceLayout(layout.editor_surface, *merge_tab);
  const MergeInteractionLayout interaction =
      shell_.BuildMergeInteractionLayout(layout.editor_surface, surface_layout, *merge_tab);
  if (!Contains(interaction.result.rect, event.motion.x, event.motion.y)) {
    return false;
  }

  const std::size_t line =
      ClampTextGridLineAtY(interaction.result.text, event.motion.y);
  const std::size_t previous_selected_hunk = merge_tab->selected_hunk;
  const std::size_t visual_column =
      TextGridVisualColumnAtX(interaction.result.text, event.motion.x);
  merge_tab->result_viewport.MoveCursorToVisualColumn(line, visual_column, true);
  if (const auto conflict_index = shell_.FindMergeTrackedConflictAtResultLine(*merge_tab, line);
      conflict_index.has_value()) {
    merge_tab->selected_hunk = *conflict_index;
  }
  merge_tab->scroll_row = static_cast<int>(merge_tab->result_viewport.scroll_line());
  merge_tab->horizontal_scroll = merge_tab->result_viewport.horizontal_scroll();
  shell_.ResetCaretBlink();
  if (merge_tab->selected_hunk != previous_selected_hunk) {
    shell_.RequestMergeConflictRedraw(previous_selected_hunk);
    shell_.RequestMergeConflictRedraw(merge_tab->selected_hunk);
  } else {
    shell_.RequestFocusedEditorRedraw();
  }
  shell_.surface_.focus = FocusTarget::Editor;
  return true;
}

bool WorkspaceShell::MergeMouseCoordinator::HandleWheel(const SDL_Event& event,
                                                        const WorkspaceLayout& layout,
                                                        int vertical_ticks,
                                                        int horizontal_ticks) {
  if (!shell_.ActiveTabIsMerge() ||
      !Contains(layout.editor_surface, event.wheel.mouse_x, event.wheel.mouse_y)) {
    return false;
  }

  if (horizontal_ticks != 0) {
    shell_.ScrollMergeColumns(-horizontal_ticks * 3);
  } else {
    MergeTabState* merge_tab = shell_.ActiveMergeTab();
    if (merge_tab != nullptr) {
      const MergeSurfaceLayout surface_layout =
          shell_.ComputeMergeSurfaceLayout(layout.editor_surface, *merge_tab);
      const auto scroll_layout =
          shell_.ComputeMergeScrollLayout(layout.editor_surface, surface_layout,
                                          *merge_tab);
      merge_tab->scroll_row =
          std::clamp(scroll_layout.vertical_scroll - vertical_ticks * 3, 0,
                     scroll_layout.max_vertical_scroll);
      merge_tab->result_viewport.SetScrollLine(
          static_cast<std::size_t>(std::max(0, merge_tab->scroll_row)));
      merge_tab->scroll_row = static_cast<int>(merge_tab->result_viewport.scroll_line());
    }
  }
  shell_.surface_.focus = FocusTarget::Editor;
  return true;
}

}  // namespace microide::workspace
