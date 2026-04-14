#include "workspace/WorkspaceShell.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>

#include "workspace/WorkspaceCompareMouseCoordinator.h"
#include "workspace/WorkspaceChromeMouseCoordinator.h"
#include "workspace/WorkspaceEditorMouseCoordinator.h"
#include "workspace/WorkspacePanelMouseCoordinator.h"
#include "workspace/WorkspaceSidebarMouseCoordinator.h"
#include "workspace/WorkspaceShellShared.h"
#include "workspace/WorkspaceTabMouseCoordinator.h"

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

bool WorkspaceShell::HandleMouseButtonDown(const SDL_Event& event) {
  if ((event.button.button != SDL_BUTTON_LEFT && event.button.button != SDL_BUTTON_MIDDLE &&
       event.button.button != SDL_BUTTON_RIGHT) ||
      last_window_width_ <= 0 || last_window_height_ <= 0) {
    return false;
  }

  const auto visible_blame_popup = ActiveEditorBlamePopupLayout();
  if (event.button.button == SDL_BUTTON_LEFT && visible_blame_popup.has_value() &&
      Contains(visible_blame_popup->rect, event.button.x, event.button.y)) {
    if (Contains(EditorBlamePopupCopyShaHitRect(*visible_blame_popup), event.button.x,
                 event.button.y)) {
      if (const editor::EditorBlameLine* blame_line =
              VisibleEditorBlameLine(visible_blame_popup->line_index);
          blame_line != nullptr && !blame_line->commit_id.empty() &&
          WriteClipboardText(blame_line->commit_id)) {
      }
    }
    surface_.focus = FocusTarget::Editor;
    return true;
  }
  UpdateMouseCursor(static_cast<float>(event.button.x), static_cast<float>(event.button.y));

  if (prompts_.dirty_visible) {
    const SDL_FRect full = MakeRect(0.0f, 0.0f, static_cast<float>(last_window_width_),
                                    static_cast<float>(last_window_height_));
    const SDL_FRect dialog = ComputeDirtyPromptRect(full);
    const auto buttons = ComputeDirtyPromptButtonRects(dialog);
    for (std::size_t i = 0; i < buttons.size(); ++i) {
      if (Contains(buttons[i], event.button.x, event.button.y)) {
        prompts_.dirty.selected_action = static_cast<int>(i);
        ConfirmDirtyPrompt();
        return true;
      }
    }
    return true;
  }

  if (prompts_.surface_visible) {
    const SDL_FRect full = MakeRect(0.0f, 0.0f, static_cast<float>(last_window_width_),
                                    static_cast<float>(last_window_height_));
    const SDL_FRect dialog = ComputePromptSurfaceRect(full);
    const auto buttons = ComputePromptSurfaceButtonRects(dialog);
    for (std::size_t i = 0; i < buttons.size(); ++i) {
      if (Contains(buttons[i], event.button.x, event.button.y)) {
        prompts_.surface.selected_button = static_cast<int>(i);
        if (event.button.button == SDL_BUTTON_LEFT) {
          ConfirmPromptSurface();
        }
        return true;
      }
    }
    return true;
  }

  if (!text_composition_.text.empty()) {
    text_composition_ = TextCompositionState{};
    if (SDL_Window* window = SDL_GetKeyboardFocus(); window != nullptr) {
      SDL_ClearComposition(window);
    }
  }

  const WorkspaceLayout layout =
      ComputeLayout(static_cast<float>(last_window_width_), static_cast<float>(last_window_height_),
                    surface_.sidebar_visible, BottomPanelVisible(), surface_.sidebar_width, surface_.bottom_panel_height);
  surface_.mouse_selecting = false;

  if (ChromeMouseCoordinator(*this).HandleButtonDown(event, layout)) {
    return true;
  }

  if (event.button.button == SDL_BUTTON_LEFT) {
    if (EditorBlameLineAtPosition(static_cast<float>(event.button.x),
                                  static_cast<float>(event.button.y)) != nullptr) {
      surface_.focus = FocusTarget::Editor;
      return true;
    }
  }

  if (event.button.button == SDL_BUTTON_LEFT && surface_.sidebar_visible &&
      Contains(SidebarResizeHandleRect(layout), event.button.x, event.button.y)) {
    surface_.drag_target = DragTarget::SidebarDivider;
    return true;
  }

  if (PanelMouseCoordinator(*this).HandleResizeButtonDown(event, layout)) {
    return true;
  }

  if (SidebarMouseCoordinator(*this).HandleButtonDown(event, layout)) {
    return true;
  }

  if (TabMouseCoordinator(*this).HandleButtonDown(event, layout)) {
    return true;
  }

  if (PanelMouseCoordinator(*this).HandleButtonDown(event, layout)) {
    return true;
  }

  if (event.button.button == SDL_BUTTON_RIGHT &&
      Contains(layout.editor_surface, event.button.x, event.button.y) &&
      ActiveEditableViewport() != nullptr) {
    OpenAnchoredMenu(MenuId::Edit,
                     MakeRect(static_cast<float>(event.button.x),
                              static_cast<float>(event.button.y), 1.0f, 1.0f));
    surface_.focus = FocusTarget::Editor;
    return true;
  }

  if (event.button.button != SDL_BUTTON_LEFT ||
      !Contains(layout.editor_surface, event.button.x, event.button.y)) {
    return false;
  }

  if (ActiveTabIsCompare()) {
    return CompareMouseCoordinator(*this).HandleButtonDown(event, layout);
  }

  if (ActiveTabIsMerge()) {
    MergeTabState* merge_tab = ActiveMergeTab();
    if (merge_tab == nullptr) {
      return false;
    }

    const MergeSurfaceLayout surface_layout =
        ComputeMergeSurfaceLayout(layout.editor_surface, *merge_tab);
    const auto scroll_layout =
        ComputeMergeScrollLayout(layout.editor_surface, surface_layout, *merge_tab);
    merge_tab->scroll_row = scroll_layout.vertical_scroll;
    merge_tab->horizontal_scroll = scroll_layout.horizontal_scroll;
    merge_tab->result_viewport.SetScrollLine(
        static_cast<std::size_t>(std::max(0, merge_tab->scroll_row)));
    merge_tab->result_viewport.SetHorizontalScroll(merge_tab->horizontal_scroll);
    merge_tab->scroll_row = static_cast<int>(merge_tab->result_viewport.scroll_line());
    merge_tab->horizontal_scroll = merge_tab->result_viewport.horizontal_scroll();
    const MergeInteractionLayout interaction =
        BuildMergeInteractionLayout(layout.editor_surface, surface_layout, *merge_tab);
    const SDL_FRect left_divider_rect =
        MakeRect(surface_layout.center_x - surface_layout.divider_width, layout.editor_surface.y,
                 surface_layout.divider_width, layout.editor_surface.h);
    const SDL_FRect right_divider_rect =
        MakeRect(surface_layout.right_x - surface_layout.divider_width, layout.editor_surface.y,
                 surface_layout.divider_width, layout.editor_surface.h);
    if (Contains(left_divider_rect, event.button.x, event.button.y)) {
      surface_.drag_target = DragTarget::MergeLeftDivider;
      surface_.focus = FocusTarget::Editor;
      return true;
    }
    if (Contains(right_divider_rect, event.button.x, event.button.y)) {
      surface_.drag_target = DragTarget::MergeRightDivider;
      surface_.focus = FocusTarget::Editor;
      return true;
    }

    const MergeToolbarLayout toolbar = ComputeMergeToolbarLayout(layout.editor_surface, surface_layout);
    if (Contains(toolbar.prev_rect, event.button.x, event.button.y)) {
      MoveMergeSelection(-1);
      return true;
    }
    if (Contains(toolbar.next_rect, event.button.x, event.button.y)) {
      MoveMergeSelection(1);
      return true;
    }
    if (Contains(toolbar.save_rect, event.button.x, event.button.y)) {
      ExecuteAction(ActionId::Save, {}, ActionSource::Menu);
      return true;
    }
    if (Contains(toolbar.open_rect, event.button.x, event.button.y)) {
      OpenMergeResultFile();
      return true;
    }

    const auto source_button_rect = [&](const MergeTrackedConflict& conflict, bool incoming) {
      return BuildMergeSourceActionButtonRect(surface_layout, interaction, conflict, incoming);
    };
    const auto result_action_rects =
        [&](const MergeTrackedConflict& conflict) -> std::array<SDL_FRect, 4> {
      return BuildMergeResultActionButtonRects(surface_layout, interaction, conflict);
    };

    if (merge_tab->hover_state.has_value() &&
        merge_tab->hover_state->conflict_index < merge_tab->conflicts.size()) {
      const auto& hovered_conflict = merge_tab->conflicts[merge_tab->hover_state->conflict_index];
      if (merge_tab->hover_state->kind == MergeHoverState::Kind::IncomingConflict ||
          merge_tab->hover_state->kind == MergeHoverState::Kind::IncomingAccept) {
        if (Contains(source_button_rect(hovered_conflict, true), event.button.x, event.button.y)) {
          merge_tab->selected_hunk = merge_tab->hover_state->conflict_index;
          ApplyMergeChoice(compare::MergeChoice::Incoming);
          return true;
        }
      }
      if (merge_tab->hover_state->kind == MergeHoverState::Kind::CurrentConflict ||
          merge_tab->hover_state->kind == MergeHoverState::Kind::CurrentAccept) {
        if (Contains(source_button_rect(hovered_conflict, false), event.button.x, event.button.y)) {
          merge_tab->selected_hunk = merge_tab->hover_state->conflict_index;
          ApplyMergeChoice(compare::MergeChoice::Current);
          return true;
        }
      }
      if ((merge_tab->hover_state->kind == MergeHoverState::Kind::ResultConflict ||
           merge_tab->hover_state->kind == MergeHoverState::Kind::ResultAction) &&
          hovered_conflict.valid) {
        const auto action_rects = result_action_rects(hovered_conflict);
        if (Contains(action_rects[0], event.button.x, event.button.y)) {
          merge_tab->selected_hunk = merge_tab->hover_state->conflict_index;
          ApplyMergeChoice(compare::MergeChoice::Base);
          return true;
        }
        if (Contains(action_rects[1], event.button.x, event.button.y)) {
          merge_tab->selected_hunk = merge_tab->hover_state->conflict_index;
          ApplyMergeChoice(compare::MergeChoice::Incoming);
          return true;
        }
        if (Contains(action_rects[2], event.button.x, event.button.y)) {
          merge_tab->selected_hunk = merge_tab->hover_state->conflict_index;
          ApplyMergeChoice(compare::MergeChoice::Current);
          return true;
        }
        if (Contains(action_rects[3], event.button.x, event.button.y)) {
          merge_tab->selected_hunk = merge_tab->hover_state->conflict_index;
          ApplyMergeChoice(compare::MergeChoice::Both);
          return true;
        }
      }
    }

    if (scroll_layout.vertical_scrollbar.has_value() &&
        Contains(scroll_layout.vertical_scrollbar->track, event.button.x, event.button.y)) {
      surface_.drag_target = DragTarget::CompareVerticalScrollbar;
      surface_.drag_scrollbar_offset =
          Contains(scroll_layout.vertical_scrollbar->thumb, event.button.x, event.button.y)
              ? static_cast<float>(event.button.y) - scroll_layout.vertical_scrollbar->thumb.y
              : scroll_layout.vertical_scrollbar->thumb.h * 0.5f;
      merge_tab->scroll_row = std::clamp(
          static_cast<int>(std::lround(ScrollUnitsForPointer(
              *scroll_layout.vertical_scrollbar, static_cast<float>(event.button.y),
              surface_.drag_scrollbar_offset))),
          0, scroll_layout.max_vertical_scroll);
      merge_tab->result_viewport.SetScrollLine(static_cast<std::size_t>(std::max(0, merge_tab->scroll_row)));
      merge_tab->scroll_row = static_cast<int>(merge_tab->result_viewport.scroll_line());
      surface_.focus = FocusTarget::Editor;
      return true;
    }

    if (scroll_layout.horizontal_scrollbar.has_value() &&
        Contains(scroll_layout.horizontal_scrollbar->track, event.button.x, event.button.y)) {
      surface_.drag_target = DragTarget::CompareHorizontalScrollbar;
      surface_.drag_scrollbar_offset =
          Contains(scroll_layout.horizontal_scrollbar->thumb, event.button.x, event.button.y)
              ? static_cast<float>(event.button.x) - scroll_layout.horizontal_scrollbar->thumb.x
              : scroll_layout.horizontal_scrollbar->thumb.w * 0.5f;
      merge_tab->horizontal_scroll = static_cast<std::size_t>(std::max(
          0L, std::lround(ScrollUnitsForPointer(*scroll_layout.horizontal_scrollbar,
                                                static_cast<float>(event.button.x),
                                                surface_.drag_scrollbar_offset))));
      merge_tab->result_viewport.SetHorizontalScroll(merge_tab->horizontal_scroll);
      merge_tab->horizontal_scroll = merge_tab->result_viewport.horizontal_scroll();
      surface_.focus = FocusTarget::Editor;
      return true;
    }

    if (Contains(interaction.result.rect, event.button.x, event.button.y)) {
      const std::size_t line = ClampTextGridLineAtY(interaction.result.text, event.button.y);
      const std::size_t visual_column =
          TextGridVisualColumnAtX(interaction.result.text, event.button.x);
      merge_tab->result_viewport.MoveCursorToVisualColumn(
          line, visual_column, (SDL_GetModState() & SDL_KMOD_SHIFT) != 0);
      if (const auto conflict_index = FindMergeTrackedConflictAtResultLine(*merge_tab, line);
          conflict_index.has_value()) {
        merge_tab->selected_hunk = *conflict_index;
      }
      merge_tab->scroll_row = static_cast<int>(merge_tab->result_viewport.scroll_line());
      merge_tab->horizontal_scroll = merge_tab->result_viewport.horizontal_scroll();
      ResetCaretBlink();
      surface_.focus = FocusTarget::Editor;
      surface_.mouse_selecting = true;
      return true;
    }

    const int clicked_row =
        static_cast<int>((event.button.y - surface_layout.rows_y) / std::max(1.0f, surface_layout.line_height));
    if (clicked_row >= 0) {
      const std::size_t line = static_cast<std::size_t>(std::max(0, merge_tab->scroll_row + clicked_row));
      if (event.button.x < surface_layout.center_x) {
        if (const auto conflict_index = FindMergeTrackedConflictAtSourceLine(*merge_tab, line, true);
            conflict_index.has_value()) {
          merge_tab->selected_hunk = *conflict_index;
          RevealActiveMergeSelection();
        }
      } else if (event.button.x >= surface_layout.right_x) {
        if (const auto conflict_index = FindMergeTrackedConflictAtSourceLine(*merge_tab, line, false);
            conflict_index.has_value()) {
          merge_tab->selected_hunk = *conflict_index;
          RevealActiveMergeSelection();
        }
      }
      surface_.focus = FocusTarget::Editor;
      return true;
    }
    return false;
  }

  return EditorMouseCoordinator(*this).HandleButtonDown(event, layout);
}

bool WorkspaceShell::HandleMouseButtonUp(const SDL_Event& event) {
  UpdateMouseCursor(static_cast<float>(event.button.x), static_cast<float>(event.button.y));

  if (prompts_.dirty_visible) {
    return true;
  }
  if (prompts_.surface_visible) {
    return true;
  }

  if (event.button.button == SDL_BUTTON_LEFT && tab_drag_state_.kind != TabDragKind::None) {
    if (TabMouseCoordinator(*this).HandleButtonUp(event)) {
      return true;
    }
    return true;
  }

  if (PanelMouseCoordinator(*this).HandleButtonUp(event)) {
    return true;
  }

  if (event.button.button != SDL_BUTTON_LEFT) {
    return false;
  }
  if (surface_.drag_target != DragTarget::None) {
    surface_.drag_target = DragTarget::None;
    surface_.drag_scrollbar_offset = 0.0f;
    surface_.drag_editor_split_path.clear();
    surface_.drag_editor_split_divider_index = 0;
    surface_.mouse_selecting = false;
    UpdateMouseCursor(static_cast<float>(event.button.x), static_cast<float>(event.button.y));
    return true;
  }
  const bool was_selecting = surface_.mouse_selecting;
  surface_.mouse_selecting = false;
  return was_selecting;
}

bool WorkspaceShell::HandleMouseMotion(const SDL_Event& event) {
  bool hover_visual_changed = false;
  if (last_window_width_ > 0 && last_window_height_ > 0) {
    const std::optional<std::size_t> previous_popup_line = active_editor_blame_popup_line_;
    const bool previous_copy_hovered =
        last_mouse_position_valid_ && EditorBlamePopupCopyShaHovered(last_mouse_x_, last_mouse_y_);
    UpdateMouseCursor(static_cast<float>(event.motion.x), static_cast<float>(event.motion.y));
    const bool current_copy_hovered =
        EditorBlamePopupCopyShaHovered(static_cast<float>(event.motion.x),
                                       static_cast<float>(event.motion.y));
    hover_visual_changed = previous_popup_line != active_editor_blame_popup_line_ ||
                           previous_copy_hovered != current_copy_hovered;
  }

  if (prompts_.dirty_visible) {
    return true;
  }
  if (prompts_.surface_visible) {
    return true;
  }

  if (last_window_width_ <= 0 || last_window_height_ <= 0) {
    return false;
  }

  const WorkspaceLayout layout =
      ComputeLayout(static_cast<float>(last_window_width_), static_cast<float>(last_window_height_),
                    surface_.sidebar_visible, BottomPanelVisible(), surface_.sidebar_width, surface_.bottom_panel_height);
  if (ChromeMouseCoordinator(*this).HandleMotion(event, layout)) {
    return true;
  }

  if (tab_drag_state_.kind != TabDragKind::None) {
    return TabMouseCoordinator(*this).HandleMotion(event);
  }

  if (surface_.drag_target != DragTarget::None) {
    if ((event.motion.state & SDL_BUTTON_LMASK) == 0) {
      surface_.drag_target = DragTarget::None;
      surface_.drag_scrollbar_offset = 0.0f;
      surface_.drag_editor_split_path.clear();
      surface_.drag_editor_split_divider_index = 0;
      UpdateMouseCursor(static_cast<float>(event.motion.x), static_cast<float>(event.motion.y));
      return false;
    }

    if (surface_.drag_target == DragTarget::SidebarDivider) {
      surface_.sidebar_width =
          ClampSidebarWidth(static_cast<float>(event.motion.x), static_cast<float>(last_window_width_));
      return true;
    }

    const WorkspaceLayout drag_layout = layout;

    if (PanelMouseCoordinator(*this).HandleDrag(event, drag_layout)) {
      return true;
    }

    if ((surface_.drag_target == DragTarget::MergeLeftDivider ||
         surface_.drag_target == DragTarget::MergeRightDivider) &&
        ActiveTabIsMerge()) {
      MergeTabState* merge_tab = ActiveMergeTab();
      if (merge_tab == nullptr) {
        surface_.drag_target = DragTarget::None;
        return false;
      }

      const MergeSurfaceLayout surface_layout =
          ComputeMergeSurfaceLayout(drag_layout.editor_surface, *merge_tab);
      const float content_width = std::max(
          1.0f, surface_layout.left_width + surface_layout.center_width + surface_layout.right_width);
      const float min_fraction =
          std::min(1.0f / 3.0f, kMinMergePaneWidth / std::max(content_width, 1.0f));
      const float current_left_fraction = surface_layout.left_width / content_width;
      const float current_right_fraction =
          (surface_layout.left_width + surface_layout.center_width) / content_width;
      if (surface_.drag_target == DragTarget::MergeLeftDivider) {
        const float divider_center_x =
            drag_layout.editor_surface.x + 8.0f + surface_layout.gutter_width +
            surface_layout.divider_width * 0.5f;
        const float raw_fraction =
            (static_cast<float>(event.motion.x) - divider_center_x) / content_width;
        merge_tab->left_divider_fraction =
            std::clamp(raw_fraction, min_fraction, current_right_fraction - min_fraction);
      } else {
        const float divider_center_x =
            drag_layout.editor_surface.x + 8.0f + surface_layout.gutter_width * 2.0f +
            surface_layout.divider_width * 1.5f;
        const float raw_fraction =
            (static_cast<float>(event.motion.x) - divider_center_x) / content_width;
        merge_tab->right_divider_fraction =
            std::clamp(raw_fraction, current_left_fraction + min_fraction, 1.0f - min_fraction);
      }
      surface_.focus = FocusTarget::Editor;
      return true;
    }

    if (surface_.drag_target == DragTarget::SidebarScrollbar) {
      return SidebarMouseCoordinator(*this).HandleDrag(event, drag_layout);
    }

    if (surface_.drag_target == DragTarget::OverlayScrollbar && surface_.overlay_visible) {
      const SDL_FRect overlay = ComputeOverlayRect(drag_layout.editor_area);
      const auto list_layout = ComputeOverlayListLayout(overlay);
      if (!list_layout.scrollbar.has_value()) {
        surface_.drag_target = DragTarget::None;
        surface_.drag_scrollbar_offset = 0.0f;
        return false;
      }
      surface_.overlay_scroll_row =
          std::clamp(static_cast<int>(std::lround(
                         ScrollUnitsForPointer(*list_layout.scrollbar,
                                              static_cast<float>(event.motion.y),
                                              surface_.drag_scrollbar_offset))),
                     0, list_layout.max_scroll);
      surface_.focus = FocusTarget::Overlay;
      return true;
    }

    if (CompareMouseCoordinator(*this).HandleDrag(event, drag_layout)) {
      return true;
    }

    if ((surface_.drag_target == DragTarget::CompareVerticalScrollbar ||
         surface_.drag_target == DragTarget::CompareHorizontalScrollbar) &&
        ActiveTabIsMerge()) {
      MergeTabState* merge_tab = ActiveMergeTab();
      if (merge_tab == nullptr) {
        surface_.drag_target = DragTarget::None;
        surface_.drag_scrollbar_offset = 0.0f;
        return false;
      }
      const MergeSurfaceLayout surface_layout =
          ComputeMergeSurfaceLayout(drag_layout.editor_surface, *merge_tab);
      const auto scroll_layout =
          ComputeMergeScrollLayout(drag_layout.editor_surface, surface_layout, *merge_tab);
      merge_tab->scroll_row = scroll_layout.vertical_scroll;
      merge_tab->horizontal_scroll = scroll_layout.horizontal_scroll;
      if (surface_.drag_target == DragTarget::CompareVerticalScrollbar) {
        if (!scroll_layout.vertical_scrollbar.has_value()) {
          surface_.drag_target = DragTarget::None;
          surface_.drag_scrollbar_offset = 0.0f;
          return false;
        }
        const int target_scroll = std::clamp(
            static_cast<int>(std::lround(ScrollUnitsForPointer(
                *scroll_layout.vertical_scrollbar, static_cast<float>(event.motion.y),
                surface_.drag_scrollbar_offset))),
            0, scroll_layout.max_vertical_scroll);
        merge_tab->scroll_row = target_scroll;
        merge_tab->result_viewport.SetScrollLine(
            static_cast<std::size_t>(std::max(0, merge_tab->scroll_row)));
        merge_tab->scroll_row = static_cast<int>(merge_tab->result_viewport.scroll_line());
        surface_.focus = FocusTarget::Editor;
        return true;
      }
      if (!scroll_layout.horizontal_scrollbar.has_value()) {
        surface_.drag_target = DragTarget::None;
        surface_.drag_scrollbar_offset = 0.0f;
        return false;
      }
      merge_tab->horizontal_scroll = static_cast<std::size_t>(std::max(
          0L, std::lround(ScrollUnitsForPointer(*scroll_layout.horizontal_scrollbar,
                                                static_cast<float>(event.motion.x),
                                                surface_.drag_scrollbar_offset))));
      merge_tab->result_viewport.SetHorizontalScroll(merge_tab->horizontal_scroll);
      merge_tab->horizontal_scroll = merge_tab->result_viewport.horizontal_scroll();
      surface_.focus = FocusTarget::Editor;
      return true;
    }

    if (EditorMouseCoordinator(*this).HandleDrag(event, drag_layout)) {
      return true;
    }

    surface_.drag_target = DragTarget::None;
    surface_.drag_scrollbar_offset = 0.0f;
    return false;
  }

  if (PanelMouseCoordinator(*this).HandleMotion(event)) {
    return true;
  }

  if (ActiveTabIsMerge()) {
    MergeTabState* merge_tab = ActiveMergeTab();
    if (merge_tab != nullptr) {
      const std::optional<MergeHoverState> previous_hover = merge_tab->hover_state;
      std::optional<MergeHoverState> next_hover;
      if (Contains(layout.editor_surface, event.motion.x, event.motion.y) &&
          !(surface_.mouse_selecting && (event.motion.state & SDL_BUTTON_LMASK) != 0)) {
        const MergeSurfaceLayout surface_layout =
            ComputeMergeSurfaceLayout(layout.editor_surface, *merge_tab);
        const auto scroll_layout =
            ComputeMergeScrollLayout(layout.editor_surface, surface_layout, *merge_tab);
        merge_tab->result_viewport.SetScrollLine(
            static_cast<std::size_t>(std::max(0, scroll_layout.vertical_scroll)));
        merge_tab->result_viewport.SetHorizontalScroll(scroll_layout.horizontal_scroll);
        merge_tab->scroll_row = static_cast<int>(merge_tab->result_viewport.scroll_line());
        merge_tab->horizontal_scroll = merge_tab->result_viewport.horizontal_scroll();
        const MergeInteractionLayout interaction =
            BuildMergeInteractionLayout(layout.editor_surface, surface_layout, *merge_tab);
        next_hover = ClassifyMergeHoverState(surface_layout, interaction, *merge_tab,
                                             static_cast<float>(event.motion.x),
                                             static_cast<float>(event.motion.y));
      }

      merge_tab->hover_state = next_hover;
      const bool hover_changed = !MergeHoverStatesEqual(previous_hover, merge_tab->hover_state);
      hover_visual_changed = hover_visual_changed || hover_changed;
    }
  }

  if (!surface_.mouse_selecting || (event.motion.state & SDL_BUTTON_LMASK) == 0) {
    return hover_visual_changed;
  }

  if (!Contains(layout.editor_surface, event.motion.x, event.motion.y)) {
    return false;
  }

  if (ActiveTabIsCompare()) {
    return CompareMouseCoordinator(*this).HandleSelectionMotion(event, layout);
  }

  if (ActiveTabIsMerge()) {
    MergeTabState* merge_tab = ActiveMergeTab();
    if (merge_tab == nullptr) {
      return false;
    }
    if (merge_tab->hover_state.has_value()) {
      merge_tab->hover_state.reset();
    }
    const MergeSurfaceLayout surface_layout =
        ComputeMergeSurfaceLayout(layout.editor_surface, *merge_tab);
    const MergeInteractionLayout interaction =
        BuildMergeInteractionLayout(layout.editor_surface, surface_layout, *merge_tab);
    if (!Contains(interaction.result.rect, event.motion.x, event.motion.y)) {
      return false;
    }
    const std::size_t line = ClampTextGridLineAtY(interaction.result.text, event.motion.y);
    const std::size_t visual_column =
        TextGridVisualColumnAtX(interaction.result.text, event.motion.x);

    merge_tab->result_viewport.MoveCursorToVisualColumn(line, visual_column, true);
    merge_tab->scroll_row = static_cast<int>(merge_tab->result_viewport.scroll_line());
    merge_tab->horizontal_scroll = merge_tab->result_viewport.horizontal_scroll();
    ResetCaretBlink();
    surface_.focus = FocusTarget::Editor;
    return true;
  }

  return EditorMouseCoordinator(*this).HandleSelectionMotion(event, layout);
}

bool WorkspaceShell::HandleMouseWheel(const SDL_Event& event) {
  if (prompts_.dirty_visible) {
    return true;
  }
  if (prompts_.surface_visible) {
    return true;
  }

  if (surface_.menu_bar_open || surface_.tree_context_menu.open) {
    return true;
  }

  if (last_window_width_ <= 0 || last_window_height_ <= 0) {
    return false;
  }

  const int vertical_ticks = event.wheel.integer_y != 0
                                 ? event.wheel.integer_y
                                 : static_cast<int>(std::lround(event.wheel.y));
  const int axis_horizontal_ticks = event.wheel.integer_x != 0
                                        ? event.wheel.integer_x
                                        : static_cast<int>(std::lround(event.wheel.x));
  const int horizontal_ticks =
      axis_horizontal_ticks != 0
          ? axis_horizontal_ticks
          : ((SDL_GetModState() & SDL_KMOD_SHIFT) != 0 ? -vertical_ticks : 0);
  if (vertical_ticks == 0 && horizontal_ticks == 0) {
    return false;
  }

  const WorkspaceLayout layout =
      ComputeLayout(static_cast<float>(last_window_width_), static_cast<float>(last_window_height_),
                    surface_.sidebar_visible, BottomPanelVisible(), surface_.sidebar_width, surface_.bottom_panel_height);

  if (ChromeMouseCoordinator(*this).HandleWheel(event, layout, vertical_ticks, horizontal_ticks)) {
    return true;
  }

  if (TabMouseCoordinator(*this).HandleWheel(event, layout, vertical_ticks)) {
    return true;
  }

  if (SidebarMouseCoordinator(*this).HandleWheel(event, layout, vertical_ticks)) {
    return true;
  }

  if (PanelMouseCoordinator(*this).HandleWheel(event, layout, vertical_ticks)) {
    return true;
  }

  if (Contains(layout.editor_surface, event.wheel.mouse_x, event.wheel.mouse_y)) {
    if (ActiveTabIsCompare()) {
      return CompareMouseCoordinator(*this).HandleWheel(event, layout, vertical_ticks,
                                                        horizontal_ticks);
    }
    if (ActiveTabIsMerge()) {
      if (horizontal_ticks != 0) {
        ScrollMergeColumns(-horizontal_ticks * 3);
      } else {
        MergeTabState* merge_tab = ActiveMergeTab();
        if (merge_tab != nullptr) {
          const MergeSurfaceLayout surface_layout =
              ComputeMergeSurfaceLayout(layout.editor_surface, *merge_tab);
          const auto scroll_layout =
              ComputeMergeScrollLayout(layout.editor_surface, surface_layout, *merge_tab);
          merge_tab->scroll_row =
              std::clamp(scroll_layout.vertical_scroll - vertical_ticks * 3, 0,
                         scroll_layout.max_vertical_scroll);
          merge_tab->result_viewport.SetScrollLine(
              static_cast<std::size_t>(std::max(0, merge_tab->scroll_row)));
          merge_tab->scroll_row = static_cast<int>(merge_tab->result_viewport.scroll_line());
        }
      }
      surface_.focus = FocusTarget::Editor;
      return true;
    }
    return EditorMouseCoordinator(*this).HandleWheel(event, layout, vertical_ticks);
  }

  return false;
}


}  // namespace microide::workspace
