#include "workspace/WorkspaceShell.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>

#include "editor/EditorViewRenderer.h"
#include "workspace/WorkspaceShellShared.h"
#include "workspace/WorkspaceTabMouseCoordinator.h"

namespace microide::workspace {

namespace {

constexpr float kSidebarHeaderHeight = 26.0f;
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

  if (surface_.tree_context_menu.open) {
    if (const auto popup_rect = ComputeTreeContextMenuRect();
        popup_rect.has_value() && Contains(*popup_rect, event.button.x, event.button.y)) {
      for (const VisiblePopupMenuItem& item : ComputeVisiblePopupMenuItems(
               TreeContextMenuItems(surface_.tree_context_menu.target),
               surface_.tree_context_menu.active_item_index, *popup_rect)) {
        if (!Contains(item.rect, event.button.x, event.button.y)) {
          continue;
        }
        surface_.tree_context_menu.active_item_index = item.enabled ? static_cast<int>(item.index) : -1;
        if (event.button.button == SDL_BUTTON_LEFT && !item.separator && item.enabled) {
          ExecuteTreeContextMenuItem(item.index);
        }
        return true;
      }
      return true;
    }
    CloseTreeContextMenu();
  }

  if (surface_.menu_bar_open && event.button.button != SDL_BUTTON_LEFT) {
    CloseMenuBar();
  }

  if (event.button.button == SDL_BUTTON_LEFT) {
    if (EditorBlameLineAtPosition(static_cast<float>(event.button.x),
                                  static_cast<float>(event.button.y)) != nullptr) {
      surface_.focus = FocusTarget::Editor;
      return true;
    }
  }

  if (event.button.button == SDL_BUTTON_LEFT) {
    if (surface_.sidebar_visible) {
      const SDL_FRect sidebar_mode_rect = SidebarModeControlRect(layout.sidebar);
      if (Contains(sidebar_mode_rect, event.button.x, event.button.y)) {
        if (surface_.menu_bar_open && surface_.active_menu_id == MenuId::SidebarMode &&
            surface_.active_menu_anchor_rect.has_value()) {
          CloseMenuBar();
        } else {
          OpenAnchoredMenu(MenuId::SidebarMode, sidebar_mode_rect);
        }
        surface_.focus = FocusTarget::Sidebar;
        return true;
      }
    }

    const auto menu_bar_items = ComputeVisibleMenuBarItems(layout.menu_bar);
    const auto window_buttons = ComputeVisibleWindowControlButtons(layout.menu_bar);
    for (const VisibleWindowControlButton& button : window_buttons) {
      if (!Contains(button.rect, event.button.x, event.button.y)) {
        continue;
      }
      CloseMenuBar();
      switch (button.id) {
        case WindowControlButtonId::Minimize:
          pending_window_action_ = WindowAction::Minimize;
          break;
        case WindowControlButtonId::Maximize:
          pending_window_action_ = WindowAction::ToggleMaximize;
          break;
        case WindowControlButtonId::Close:
          RequestQuit();
          break;
      }
      return true;
    }
    if (surface_.menu_bar_open) {
      for (const VisibleMenuBarItem& item : menu_bar_items) {
        if (!Contains(item.rect, event.button.x, event.button.y)) {
          continue;
        }
        if (item.id == surface_.active_menu_id) {
          CloseMenuBar();
        } else {
          OpenMenuBarMenu(item.id);
        }
        return true;
      }

      if (const auto submenu_rect = ActiveSubmenuRect(layout.menu_bar);
          submenu_rect.has_value() && Contains(*submenu_rect, event.button.x, event.button.y)) {
        for (const VisiblePopupMenuItem& item :
             ComputeVisiblePopupMenuItems(surface_.active_submenu_id, *submenu_rect)) {
          if (!Contains(item.rect, event.button.x, event.button.y)) {
            continue;
          }
          surface_.active_submenu_item_index = item.enabled ? static_cast<int>(item.index) : -1;
          if (!item.separator && item.enabled) {
            ExecuteMenuItem(surface_.active_submenu_id, item.index);
          }
          return true;
        }
        return true;
      }

      if (const auto popup_rect = ComputePopupMenuRect(layout.menu_bar, surface_.active_menu_id);
          popup_rect.has_value() && Contains(*popup_rect, event.button.x, event.button.y)) {
        for (const VisiblePopupMenuItem& item :
             ComputeVisiblePopupMenuItems(surface_.active_menu_id, *popup_rect)) {
          if (!Contains(item.rect, event.button.x, event.button.y)) {
            continue;
          }
          surface_.active_menu_item_index = item.enabled ? static_cast<int>(item.index) : -1;
          if (!item.separator && item.enabled) {
            ExecuteMenuItem(surface_.active_menu_id, item.index);
          }
          return true;
        }
        return true;
      }

      CloseMenuBar();
      return true;
    }

    if (Contains(layout.menu_bar, event.button.x, event.button.y)) {
      for (const VisibleMenuBarItem& item : menu_bar_items) {
        if (Contains(item.rect, event.button.x, event.button.y)) {
          OpenMenuBarMenu(item.id);
          return true;
        }
      }
      return true;
    }
  }

  if (event.button.button == SDL_BUTTON_LEFT && surface_.sidebar_visible &&
      Contains(SidebarResizeHandleRect(layout), event.button.x, event.button.y)) {
    surface_.drag_target = DragTarget::SidebarDivider;
    return true;
  }

  if (event.button.button == SDL_BUTTON_LEFT && BottomPanelVisible() &&
      Contains(BottomPanelResizeHandleRect(layout), event.button.x, event.button.y)) {
    surface_.drag_target = DragTarget::BottomPanelDivider;
    return true;
  }

  if (surface_.overlay_visible && event.button.button == SDL_BUTTON_LEFT) {
    const SDL_FRect overlay = ComputeOverlayRect(layout.editor_area);

    if (!Contains(overlay, event.button.x, event.button.y)) {
      DismissOverlay();
      return true;
    }

    ClampOverlayScrollRow(overlay);
    const auto list_layout = ComputeOverlayListLayout(overlay);
    if (list_layout.scrollbar.has_value() &&
        Contains(list_layout.scrollbar->track, event.button.x, event.button.y)) {
      surface_.drag_target = DragTarget::OverlayScrollbar;
      surface_.drag_scrollbar_offset =
          Contains(list_layout.scrollbar->thumb, event.button.x, event.button.y)
              ? static_cast<float>(event.button.y) - list_layout.scrollbar->thumb.y
              : list_layout.scrollbar->thumb.h * 0.5f;
      surface_.overlay_scroll_row =
          std::clamp(static_cast<int>(std::lround(
                         ScrollUnitsForPointer(*list_layout.scrollbar,
                                              static_cast<float>(event.button.y),
                                              surface_.drag_scrollbar_offset))),
                     0, list_layout.max_scroll);
      surface_.focus = FocusTarget::Overlay;
      return true;
    }

    if (const auto item_index =
            ScrollableListIndexAtY(list_layout, static_cast<float>(event.button.y));
        item_index.has_value() && *item_index >= 0 &&
        *item_index < static_cast<int>(OverlayItemCount())) {
      SetOverlaySelectedIndex(static_cast<std::size_t>(*item_index));
      RevealOverlaySelection(overlay);
      if (surface_.overlay_mode == OverlayMode::CommitPicker) {
        ActivateOverlaySelection();
      }
    }
    surface_.focus = FocusTarget::Overlay;
    return true;
  }

  if (event.button.button == SDL_BUTTON_LEFT && surface_.sidebar_visible) {
    if (surface_.sidebar_mode == SidebarMode::Search) {
      const auto line_map = BuildProjectSearchLineMap();
      const auto list_layout =
          ComputeProjectSearchSidebarListLayout(layout.sidebar, line_map.size());
      if (list_layout.scrollbar.has_value() &&
          Contains(list_layout.scrollbar->track, event.button.x, event.button.y)) {
        surface_.drag_target = DragTarget::SidebarScrollbar;
        surface_.drag_scrollbar_offset =
            Contains(list_layout.scrollbar->thumb, event.button.x, event.button.y)
                ? static_cast<float>(event.button.y) - list_layout.scrollbar->thumb.y
                : list_layout.scrollbar->thumb.h * 0.5f;
        surface_.sidebar_scroll_row =
            std::clamp(static_cast<int>(std::lround(
                           ScrollUnitsForPointer(*list_layout.scrollbar,
                                                static_cast<float>(event.button.y),
                                                surface_.drag_scrollbar_offset))),
                       0, list_layout.max_scroll);
        surface_.focus = FocusTarget::Sidebar;
        return true;
      }
    } else if (surface_.sidebar_mode == SidebarMode::Git) {
      const auto lines = BuildGitSidebarLines();
      const auto list_layout = ComputeGitSidebarListLayout(layout.sidebar, lines.size());
      if (list_layout.scrollbar.has_value() &&
          Contains(list_layout.scrollbar->track, event.button.x, event.button.y)) {
        surface_.drag_target = DragTarget::SidebarScrollbar;
        surface_.drag_scrollbar_offset =
            Contains(list_layout.scrollbar->thumb, event.button.x, event.button.y)
                ? static_cast<float>(event.button.y) - list_layout.scrollbar->thumb.y
                : list_layout.scrollbar->thumb.h * 0.5f;
        surface_.sidebar_scroll_row =
            std::clamp(static_cast<int>(std::lround(
                           ScrollUnitsForPointer(*list_layout.scrollbar,
                                                static_cast<float>(event.button.y),
                                                surface_.drag_scrollbar_offset))),
                       0, list_layout.max_scroll);
        surface_.focus = FocusTarget::Sidebar;
        return true;
      }
    } else {
      const auto& entries = directory_tree_.entries();
      const auto list_layout =
          ComputeTreeSidebarListLayout(layout.sidebar, entries.size());
      if (list_layout.scrollbar.has_value() &&
          Contains(list_layout.scrollbar->track, event.button.x, event.button.y)) {
        surface_.drag_target = DragTarget::SidebarScrollbar;
        surface_.drag_scrollbar_offset =
            Contains(list_layout.scrollbar->thumb, event.button.x, event.button.y)
                ? static_cast<float>(event.button.y) - list_layout.scrollbar->thumb.y
                : list_layout.scrollbar->thumb.h * 0.5f;
        surface_.sidebar_scroll_row =
            std::clamp(static_cast<int>(std::lround(
                           ScrollUnitsForPointer(*list_layout.scrollbar,
                                                static_cast<float>(event.button.y),
                                                surface_.drag_scrollbar_offset))),
                       0, list_layout.max_scroll);
        surface_.focus = FocusTarget::Sidebar;
        return true;
      }
    }
  }

  if (TabMouseCoordinator(*this).HandleButtonDown(event, layout)) {
    return true;
  }

  if (surface_.sidebar_visible && Contains(layout.sidebar, event.button.x, event.button.y)) {
    surface_.focus = FocusTarget::Sidebar;
    const float local_y =
        event.button.y - (layout.sidebar.y + kSidebarHeaderHeight + 6.0f);

    if (surface_.sidebar_mode == SidebarMode::Search) {
      if (event.button.button != SDL_BUTTON_LEFT) {
        return true;
      }
      if (Contains(ProjectSearchQueryRect(layout.sidebar), event.button.x, event.button.y)) {
        BeginProjectSearchEdit(ProjectSearchEditField::Query);
        return true;
      }
      if (Contains(ProjectSearchReplaceRect(layout.sidebar), event.button.x, event.button.y)) {
        BeginProjectSearchEdit(ProjectSearchEditField::Replace);
        return true;
      }
      if (Contains(ProjectSearchModeButtonRect(layout.sidebar), event.button.x, event.button.y)) {
        if (overlay_workflow_.project_search.editing) {
          CommitProjectSearchEdit();
        }
        ToggleProjectSearchPatternMode();
        return true;
      }
      if (Contains(ProjectSearchCaseButtonRect(layout.sidebar), event.button.x, event.button.y)) {
        if (overlay_workflow_.project_search.editing) {
          CommitProjectSearchEdit();
        }
        CycleProjectSearchCaseMode();
        return true;
      }
      if (Contains(ProjectSearchHiddenButtonRect(layout.sidebar), event.button.x, event.button.y)) {
        if (overlay_workflow_.project_search.editing) {
          CommitProjectSearchEdit();
        }
        ToggleProjectSearchHiddenFiles();
        return true;
      }
      if (local_y < 0.0f) {
        return true;
      }

      const auto line_map = BuildProjectSearchLineMap();
      const auto list_layout =
          ComputeProjectSearchSidebarListLayout(layout.sidebar, line_map.size());
      if (const auto line_index =
              ScrollableListIndexAtY(list_layout, static_cast<float>(event.button.y));
          line_index.has_value() && *line_index >= 0 &&
          *line_index < static_cast<int>(line_map.size()) &&
          line_map[static_cast<std::size_t>(*line_index)] >= 0) {
        overlay_workflow_.project_search.selected_index =
            static_cast<std::size_t>(line_map[static_cast<std::size_t>(*line_index)]);
        const auto& result =
            overlay_workflow_.project_search
                .results[overlay_workflow_.project_search.selected_index];
        OpenFile(project_root_ / result.relative_path);
        text_viewport_.MoveCursorTo(result.line, result.column);
        if (surface_.sidebar_temporary) {
          RestorePreviousSidebar();
        }
        surface_.focus = FocusTarget::Editor;
      }
      return true;
    }

    if (surface_.sidebar_mode == SidebarMode::Git) {
      if (event.button.button != SDL_BUTTON_LEFT) {
        return true;
      }
      if (CanStageAllGitSidebarEntries() &&
          Contains(GitSidebarStageAllButtonRect(layout.sidebar), event.button.x, event.button.y)) {
        return StageAllGitSidebarEntries();
      }
      if (CanDiscardAllGitSidebarEntries() &&
          Contains(GitSidebarDiscardAllButtonRect(layout.sidebar), event.button.x, event.button.y)) {
        OpenDiscardAllGitSidebarPrompt();
        return true;
      }
      if (Contains(GitSidebarRefreshButtonRect(layout.sidebar), event.button.x, event.button.y)) {
        return ExecuteAction(ActionId::GitRefresh, {}, ActionSource::Shortcut);
      }
      if (event.button.y < GitSidebarListTop(layout.sidebar)) {
        return true;
      }

      const auto lines = BuildGitSidebarLines();
      const auto list_layout = ComputeGitSidebarListLayout(layout.sidebar, lines.size());
      const auto line_index =
          ScrollableListIndexAtY(list_layout, static_cast<float>(event.button.y));
      if (!line_index.has_value() || *line_index < 0 ||
          *line_index >= static_cast<int>(lines.size())) {
        return true;
      }

      const auto& line = lines[static_cast<std::size_t>(*line_index)];
      if (line.kind != GitSidebarLine::Kind::Entry || line.entry_index < 0) {
        return true;
      }

      git_sidebar_.selected_index = static_cast<std::size_t>(line.entry_index);
      const auto& entry = git_sidebar_.entries[git_sidebar_.selected_index];
      const SDL_FRect row_rect =
          ScrollableListRowRect(list_layout, *line_index - list_layout.scroll_row);
      const GitSidebarEntryActionLayout actions =
          ComputeGitSidebarEntryActionLayout(row_rect, entry);
      if (actions.primary_rect.has_value() &&
          Contains(*actions.primary_rect, event.button.x, event.button.y)) {
        if (entry.staged) {
          UnstageGitSidebarEntry(git_sidebar_.selected_index);
        } else {
          StageGitSidebarEntry(git_sidebar_.selected_index);
        }
        return true;
      }
      if (actions.discard_rect.has_value() &&
          Contains(*actions.discard_rect, event.button.x, event.button.y)) {
        DiscardGitSidebarEntry(git_sidebar_.selected_index);
        return true;
      }
      OpenGitSidebarEntry(git_sidebar_.selected_index);
      return true;
    }

    if (event.button.button == SDL_BUTTON_LEFT &&
        Contains(TreeSidebarRefreshButtonRect(layout.sidebar), event.button.x, event.button.y)) {
      return ExecuteAction(ActionId::TreeRefresh, {}, ActionSource::Shortcut);
    }

    if (local_y < 0.0f) {
      if (event.button.button == SDL_BUTTON_RIGHT) {
        OpenTreeContextMenu(TreeContextTargetKind::Background, {},
                            MakeRect(static_cast<float>(event.button.x),
                                     static_cast<float>(event.button.y), 1.0f, 1.0f));
      }
      return true;
    }

    const auto& entries = directory_tree_.entries();
    const auto list_layout = ComputeTreeSidebarListLayout(layout.sidebar, entries.size());
    const auto entry_index =
        ScrollableListIndexAtY(list_layout, static_cast<float>(event.button.y));
    if (entry_index.has_value() && *entry_index >= 0 &&
        *entry_index < static_cast<int>(entries.size())) {
      directory_tree_.SetSelectedIndex(static_cast<std::size_t>(*entry_index));
      const SDL_FRect row_rect =
          ScrollableListRowRect(list_layout, *entry_index - list_layout.scroll_row);
      if (Contains(row_rect, event.button.x, event.button.y) &&
          event.button.button == SDL_BUTTON_RIGHT) {
        const auto& entry = entries[static_cast<std::size_t>(*entry_index)];
        const TreeContextTargetKind target =
            !entry.is_directory ? TreeContextTargetKind::File
            : entry.path == project_root_ ? TreeContextTargetKind::Root
                                          : TreeContextTargetKind::Directory;
        OpenTreeContextMenu(target, entry.path,
                            MakeRect(static_cast<float>(event.button.x),
                                     static_cast<float>(event.button.y), 1.0f, 1.0f));
        return true;
      }
      if (Contains(row_rect, event.button.x, event.button.y) &&
          event.button.button != SDL_BUTTON_RIGHT) {
        const auto opened = directory_tree_.ActivateSelection();
        if (opened.has_value()) {
          OpenFile(*opened);
        }
      }
      return true;
    }
    if (event.button.button == SDL_BUTTON_RIGHT) {
      OpenTreeContextMenu(TreeContextTargetKind::Background, {},
                          MakeRect(static_cast<float>(event.button.x),
                                   static_cast<float>(event.button.y), 1.0f, 1.0f));
    }
    return true;
  }

  if (event.button.button == SDL_BUTTON_LEFT && BottomPanelVisible()) {
    const std::size_t line_count =
        ActiveTerminalTab() != nullptr ? ActiveTerminalTab()->session.SnapshotLines().size() : 0;
    const BottomPanelLogLayout panel_layout = ComputeBottomPanelLogLayout(layout, line_count);
    if (panel_layout.scroll.vertical_scrollbar.has_value() &&
        Contains(panel_layout.scroll.vertical_scrollbar->track, event.button.x, event.button.y)) {
      surface_.drag_target = DragTarget::BottomPanelScrollbar;
      surface_.drag_scrollbar_offset =
          Contains(panel_layout.scroll.vertical_scrollbar->thumb, event.button.x, event.button.y)
              ? static_cast<float>(event.button.y) -
                    panel_layout.scroll.vertical_scrollbar->thumb.y
              : panel_layout.scroll.vertical_scrollbar->thumb.h * 0.5f;
      SetBottomPanelScrollRow(
          std::clamp(static_cast<int>(std::lround(
                         ScrollUnitsForPointer(*panel_layout.scroll.vertical_scrollbar,
                                              static_cast<float>(event.button.y),
                                              surface_.drag_scrollbar_offset))),
                     0, panel_layout.scroll.max_vertical_scroll),
          line_count, panel_layout.scroll.visible_rows);
      if (ActiveTerminalTab() != nullptr) {
        surface_.focus = FocusTarget::Panel;
      }
      return true;
    }
  }

  if (ActiveTerminalTab() != nullptr) {
    auto* terminal_tab = ActiveTerminalTab();
    if (terminal_tab != nullptr) {
      const auto viewport_position =
          TerminalViewportPositionForPoint(event.button.x, event.button.y);
      const auto mouse_button = TerminalMouseButtonForSdl(event.button.button);
      if (viewport_position.has_value() &&
          mouse_button != terminal::TerminalSession::MouseButton::None &&
          terminal_tab->session.WantsMouseCapture()) {
        ClearTerminalSelection();
        terminal_tab->session.SendMouseButton(mouse_button, true, viewport_position->row,
                                              viewport_position->column, SDL_GetModState());
        surface_.focus = FocusTarget::Panel;
        return true;
      }
    }
  }

  if (event.button.button == SDL_BUTTON_LEFT && BottomPanelVisible() &&
      Contains(layout.bottom_panel, event.button.x, event.button.y)) {
    if (ActiveTerminalTab() != nullptr) {
      const SDL_FRect panel_content = BottomPanelContentRect(layout, surface_.command_mode);
      if (Contains(panel_content, event.button.x, event.button.y)) {
        const auto terminal_lines =
            ActiveTerminalTab() != nullptr ? ActiveTerminalTab()->session.SnapshotLines()
                                           : std::vector<terminal::TerminalLine>{};
        if (const auto position =
                TerminalSelectionPositionForPoint(event.button.x, event.button.y, terminal_lines);
            position.has_value()) {
          if (auto* terminal_tab = ActiveTerminalTab(); terminal_tab != nullptr) {
            terminal_tab->selection_anchor = *position;
            terminal_tab->selection_head = *position;
            terminal_tab->mouse_selecting = true;
            terminal_tab->follow_tail = false;
          }
        } else {
          ClearTerminalSelection();
        }
      } else {
        ClearTerminalSelection();
      }
      surface_.focus = FocusTarget::Panel;
    }
    if (surface_.command_mode) {
      surface_.focus = FocusTarget::Panel;
    }
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
    CompareTabState* compare_tab = ActiveCompareTab();
    if (compare_tab == nullptr) {
      return false;
    }

    const CompareSurfaceLayout surface_layout =
        ComputeCompareSurfaceLayout(layout.editor_surface, *compare_tab);
    const auto scroll_layout =
        ComputeCompareScrollLayout(layout.editor_surface, surface_layout, *compare_tab);
    compare_tab->scroll_row = scroll_layout.vertical_scroll;
    compare_tab->horizontal_scroll = scroll_layout.horizontal_scroll;
    SyncCompareViewportScroll(*compare_tab);

    if (scroll_layout.vertical_scrollbar.has_value() &&
        Contains(scroll_layout.vertical_scrollbar->track, event.button.x, event.button.y)) {
      surface_.drag_target = DragTarget::CompareVerticalScrollbar;
      surface_.drag_scrollbar_offset =
          Contains(scroll_layout.vertical_scrollbar->thumb, event.button.x, event.button.y)
              ? static_cast<float>(event.button.y) - scroll_layout.vertical_scrollbar->thumb.y
              : scroll_layout.vertical_scrollbar->thumb.h * 0.5f;
      const int target_scroll = std::clamp(
          static_cast<int>(std::lround(ScrollUnitsForPointer(
              *scroll_layout.vertical_scrollbar, static_cast<float>(event.button.y),
              surface_.drag_scrollbar_offset))),
          0, scroll_layout.max_vertical_scroll);
      compare_tab->scroll_row = target_scroll;
      SyncCompareViewportScroll(*compare_tab);
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
      compare_tab->horizontal_scroll = static_cast<std::size_t>(std::max(
          0L, std::lround(ScrollUnitsForPointer(*scroll_layout.horizontal_scrollbar,
                                                static_cast<float>(event.button.x),
                                                surface_.drag_scrollbar_offset))));
      SyncCompareViewportScroll(*compare_tab);
      surface_.focus = FocusTarget::Editor;
      return true;
    }

    const int clicked_row =
        static_cast<int>((event.button.y - surface_layout.rows_y) / surface_layout.line_height);
    const int model_row = compare_tab->scroll_row + clicked_row;
    if (clicked_row >= 0 && model_row >= 0 &&
        model_row < static_cast<int>(compare_tab->model.rows.size())) {
      compare_tab->selected_row = static_cast<std::size_t>(model_row);
      if (compare_tab->right_editable && event.button.button == SDL_BUTTON_LEFT &&
          event.button.x >= surface_layout.right_x) {
        compare_tab->right_view_active = true;
        const TextGridInteractionLayout right_interaction =
            BuildCompareRightInteractionLayout(surface_layout, *compare_tab);
        const std::size_t line = CompareRightLineForRow(*compare_tab, compare_tab->selected_row);
        const std::size_t visual_column = TextGridVisualColumnAtX(right_interaction, event.button.x);
        compare_tab->right_viewport.MoveCursorToVisualColumn(
            line, visual_column, (SDL_GetModState() & SDL_KMOD_SHIFT) != 0);
        SyncCompareSelectionFromViewport(*compare_tab, false);
        ResetCaretBlink();
        surface_.mouse_selecting = true;
      } else {
        compare_tab->right_view_active = false;
      }
      surface_.focus = FocusTarget::Editor;
      return true;
    }
    compare_tab->right_view_active = false;
    return false;
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

  const auto dividers = ComputeEditorSplitDividerLayouts(layout.editor_surface);
  const auto divider_it = std::find_if(
      dividers.begin(), dividers.end(), [&](const EditorSplitDividerLayout& divider) {
        return Contains(divider.rect, event.button.x, event.button.y);
      });
  if (divider_it != dividers.end()) {
    surface_.drag_target = DragTarget::EditorSplitDivider;
    surface_.drag_editor_split_path = divider_it->node_path;
    surface_.drag_editor_split_divider_index = divider_it->divider_index;
    surface_.focus = FocusTarget::Editor;
    return true;
  }

  const auto panes = ComputeEditorPaneLayouts(layout.editor_surface);
  const auto pane_it = std::find_if(panes.begin(), panes.end(), [&](const EditorPaneLayout& pane) {
    return Contains(pane.rect, event.button.x, event.button.y);
  });
  if (pane_it == panes.end()) {
    return false;
  }
  if (!pane_it->active) {
    SetActiveEditorSplit(pane_it->leaf_id);
  }
  const SDL_FRect editor_rect = pane_it->rect;

  const editor::EditorViewMetrics metrics =
      editor::EditorViewRenderer::ComputeMetrics(text_renderer_, text_viewport_, editor_rect);
  text_viewport_.SetViewportSize(metrics.visible_rows, metrics.visible_columns);

  const auto scroll_layout = ComputeEditorScrollLayout(editor_rect, text_viewport_, metrics);
  if (scroll_layout.vertical_scrollbar.has_value() &&
      Contains(scroll_layout.vertical_scrollbar->track, event.button.x, event.button.y)) {
    surface_.drag_target = DragTarget::EditorVerticalScrollbar;
    surface_.drag_scrollbar_offset =
        Contains(scroll_layout.vertical_scrollbar->thumb, event.button.x, event.button.y)
            ? static_cast<float>(event.button.y) - scroll_layout.vertical_scrollbar->thumb.y
            : scroll_layout.vertical_scrollbar->thumb.h * 0.5f;
    text_viewport_.SetScrollLine(static_cast<std::size_t>(std::max(
        0L, std::lround(ScrollUnitsForPointer(*scroll_layout.vertical_scrollbar,
                                              static_cast<float>(event.button.y),
                                              surface_.drag_scrollbar_offset)))));
    surface_.focus = FocusTarget::Editor;
    return true;
  }
  if (scroll_layout.horizontal_scrollbar.has_value() &&
      Contains(scroll_layout.horizontal_scrollbar->track, event.button.x, event.button.y)) {
    surface_.drag_target = DragTarget::EditorHorizontalScrollbar;
    surface_.drag_scrollbar_offset =
        Contains(scroll_layout.horizontal_scrollbar->thumb, event.button.x, event.button.y)
            ? static_cast<float>(event.button.x) - scroll_layout.horizontal_scrollbar->thumb.x
            : scroll_layout.horizontal_scrollbar->thumb.w * 0.5f;
    text_viewport_.SetHorizontalScroll(static_cast<std::size_t>(std::max(
        0L, std::lround(ScrollUnitsForPointer(*scroll_layout.horizontal_scrollbar,
                                              static_cast<float>(event.button.x),
                                              surface_.drag_scrollbar_offset)))));
    surface_.focus = FocusTarget::Editor;
    return true;
  }

  const float local_y = std::max(0.0f, event.button.y - metrics.first_line_y);
  const std::size_t row = static_cast<std::size_t>(local_y / metrics.line_height);
  const std::size_t line = std::min(text_viewport_.scroll_line() + row,
                                    text_viewport_.line_count() == 0 ? 0 : text_viewport_.line_count() - 1);
  const float text_offset_x = std::max(0.0f, event.button.x - metrics.text_x);
  const std::size_t visual_column =
      text_viewport_.horizontal_scroll() +
      static_cast<std::size_t>(
          std::max(0L, std::lround(text_offset_x / std::max(1.0f, text_renderer_.CharWidth()))));

  text_viewport_.MoveCursorToVisualColumn(line, visual_column, (SDL_GetModState() & SDL_KMOD_SHIFT) != 0);
  ResetCaretBlink();
  surface_.focus = FocusTarget::Editor;
  surface_.mouse_selecting = true;
  return true;
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

  if (ActiveTerminalTab() != nullptr) {
    auto* terminal_tab = ActiveTerminalTab();
    const auto viewport_position =
        TerminalViewportPositionForPoint(event.button.x, event.button.y);
    const auto mouse_button = TerminalMouseButtonForSdl(event.button.button);
    if (terminal_tab != nullptr && viewport_position.has_value() &&
        mouse_button != terminal::TerminalSession::MouseButton::None &&
        terminal_tab->session.WantsMouseCapture()) {
      ClearTerminalSelection();
      terminal_tab->session.SendMouseButton(mouse_button, false, viewport_position->row,
                                            viewport_position->column, SDL_GetModState());
      surface_.focus = FocusTarget::Panel;
      return true;
    }
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
  if (auto* terminal_tab = ActiveTerminalTab();
      terminal_tab != nullptr && terminal_tab->mouse_selecting) {
    terminal_tab->mouse_selecting = false;
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

  if (surface_.tree_context_menu.open) {
    if (const auto popup_rect = ComputeTreeContextMenuRect();
        popup_rect.has_value() && Contains(*popup_rect, event.motion.x, event.motion.y)) {
      surface_.tree_context_menu.active_item_index = -1;
      for (const VisiblePopupMenuItem& item :
           ComputeVisiblePopupMenuItems(TreeContextMenuItems(surface_.tree_context_menu.target),
                                        surface_.tree_context_menu.active_item_index, *popup_rect)) {
        if (Contains(item.rect, event.motion.x, event.motion.y)) {
          surface_.tree_context_menu.active_item_index = item.enabled ? static_cast<int>(item.index) : -1;
          break;
        }
      }
      return true;
    }
    surface_.tree_context_menu.active_item_index = -1;
    return true;
  }

  if (surface_.menu_bar_open) {
    const WorkspaceLayout layout =
        ComputeLayout(static_cast<float>(last_window_width_), static_cast<float>(last_window_height_),
                      surface_.sidebar_visible, BottomPanelVisible(), surface_.sidebar_width, surface_.bottom_panel_height);
    for (const VisibleMenuBarItem& item : ComputeVisibleMenuBarItems(layout.menu_bar)) {
      if (!Contains(item.rect, event.motion.x, event.motion.y)) {
        continue;
      }
      if (item.id != surface_.active_menu_id) {
        OpenMenuBarMenu(item.id);
      }
      return true;
    }
    if (const auto submenu_rect = ActiveSubmenuRect(layout.menu_bar);
        submenu_rect.has_value() && Contains(*submenu_rect, event.motion.x, event.motion.y)) {
      surface_.active_submenu_item_index = -1;
      for (const VisiblePopupMenuItem& item :
           ComputeVisiblePopupMenuItems(surface_.active_submenu_id, *submenu_rect)) {
        if (Contains(item.rect, event.motion.x, event.motion.y)) {
          surface_.active_submenu_item_index = item.enabled ? static_cast<int>(item.index) : -1;
          break;
        }
      }
      return true;
    }
    if (const auto popup_rect = ComputePopupMenuRect(layout.menu_bar, surface_.active_menu_id);
        popup_rect.has_value() && Contains(*popup_rect, event.motion.x, event.motion.y)) {
      surface_.active_menu_item_index = -1;
      for (const VisiblePopupMenuItem& item :
           ComputeVisiblePopupMenuItems(surface_.active_menu_id, *popup_rect)) {
        if (Contains(item.rect, event.motion.x, event.motion.y)) {
          surface_.active_menu_item_index = item.enabled ? static_cast<int>(item.index) : -1;
          const MenuSpec* menu = FindMenuSpec(surface_.active_menu_id);
          if (menu != nullptr && item.enabled) {
            const MenuItemSpec& spec = menu->items[item.index];
            if (spec.submenu != MenuId::None) {
              OpenSubmenu(spec.submenu, item.rect);
            } else {
              CloseSubmenu();
            }
          } else {
            CloseSubmenu();
          }
          break;
        }
      }
      return true;
    }
    surface_.active_menu_item_index = -1;
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

    if (surface_.drag_target == DragTarget::BottomPanelDivider) {
      const float desired_height =
          static_cast<float>(last_window_height_) - static_cast<float>(event.motion.y);
      surface_.bottom_panel_height =
          ClampBottomPanelHeight(desired_height, static_cast<float>(last_window_height_));
      return true;
    }

    const WorkspaceLayout drag_layout =
        ComputeLayout(static_cast<float>(last_window_width_), static_cast<float>(last_window_height_),
                      surface_.sidebar_visible, BottomPanelVisible(), surface_.sidebar_width, surface_.bottom_panel_height);

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

    if (surface_.drag_target == DragTarget::EditorSplitDivider) {
      auto* editor_tab = ActiveEditorTab();
      if (editor_tab == nullptr || editor_tab->views.size() < 2 || editor_tab->split_root == nullptr) {
        surface_.drag_target = DragTarget::None;
        return false;
      }

      NormalizeEditorSplitTree(*editor_tab);
      auto* split_node = FindEditorSplitNode(editor_tab->split_root.get(), surface_.drag_editor_split_path);
      const auto node_rect =
          ComputeEditorSplitNodeRect(drag_layout.editor_surface, surface_.drag_editor_split_path);
      if (split_node == nullptr || node_rect == std::nullopt || split_node->IsLeaf() ||
          split_node->orientation == EditorSplitOrientation::None ||
          surface_.drag_editor_split_divider_index + 1 >= split_node->children.size()) {
        surface_.drag_target = DragTarget::None;
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
      for (std::size_t i = 0; i < surface_.drag_editor_split_divider_index; ++i) {
        before_extent += split_layout->extents[i];
      }
      const float pair_extent = split_layout->extents[surface_.drag_editor_split_divider_index] +
                                split_layout->extents[surface_.drag_editor_split_divider_index +
                                                      1];
      const float min_extent =
          split_layout->total_extent >
                  split_layout->min_pane_extent *
                      static_cast<float>(split_layout->extents.size())
              ? split_layout->min_pane_extent
              : 0.0f;
      float leading_extent =
          vertical ? static_cast<float>(event.motion.x) - node_rect->x - before_extent -
                         split_layout->divider_thickness *
                             static_cast<float>(surface_.drag_editor_split_divider_index) -
                         split_layout->divider_thickness * 0.5f
                   : static_cast<float>(event.motion.y) - node_rect->y - before_extent -
                         split_layout->divider_thickness *
                             static_cast<float>(surface_.drag_editor_split_divider_index) -
                         split_layout->divider_thickness * 0.5f;
      leading_extent =
          pair_extent <= min_extent * 2.0f
              ? std::clamp(leading_extent, 0.0f, pair_extent)
              : std::clamp(leading_extent, min_extent, pair_extent - min_extent);
      const float trailing_extent = std::max(0.0f, pair_extent - leading_extent);
      split_node->children[surface_.drag_editor_split_divider_index]->size_fraction =
          leading_extent / split_layout->total_extent;
      split_node->children[surface_.drag_editor_split_divider_index + 1]->size_fraction =
          trailing_extent / split_layout->total_extent;
      NormalizeEditorSplitNode(*split_node);
      surface_.focus = FocusTarget::Editor;
      return true;
    }

    if (surface_.drag_target == DragTarget::SidebarScrollbar && surface_.sidebar_visible) {
      if (surface_.sidebar_mode == SidebarMode::Search) {
        const auto line_map = BuildProjectSearchLineMap();
        const auto list_layout =
            ComputeProjectSearchSidebarListLayout(drag_layout.sidebar, line_map.size());
        if (!list_layout.scrollbar.has_value()) {
          surface_.drag_target = DragTarget::None;
          surface_.drag_scrollbar_offset = 0.0f;
          return false;
        }
        surface_.sidebar_scroll_row =
            std::clamp(static_cast<int>(std::lround(
                           ScrollUnitsForPointer(*list_layout.scrollbar,
                                                static_cast<float>(event.motion.y),
                                                surface_.drag_scrollbar_offset))),
                       0, list_layout.max_scroll);
      } else if (surface_.sidebar_mode == SidebarMode::Git) {
        const auto lines = BuildGitSidebarLines();
        const auto list_layout = ComputeGitSidebarListLayout(drag_layout.sidebar, lines.size());
        if (!list_layout.scrollbar.has_value()) {
          surface_.drag_target = DragTarget::None;
          surface_.drag_scrollbar_offset = 0.0f;
          return false;
        }
        surface_.sidebar_scroll_row =
            std::clamp(static_cast<int>(std::lround(
                           ScrollUnitsForPointer(*list_layout.scrollbar,
                                                static_cast<float>(event.motion.y),
                                                surface_.drag_scrollbar_offset))),
                       0, list_layout.max_scroll);
      } else {
        const auto& entries = directory_tree_.entries();
        const auto list_layout =
            ComputeTreeSidebarListLayout(drag_layout.sidebar, entries.size());
        if (!list_layout.scrollbar.has_value()) {
          surface_.drag_target = DragTarget::None;
          surface_.drag_scrollbar_offset = 0.0f;
          return false;
        }
        surface_.sidebar_scroll_row =
            std::clamp(static_cast<int>(std::lround(
                           ScrollUnitsForPointer(*list_layout.scrollbar,
                                                static_cast<float>(event.motion.y),
                                                surface_.drag_scrollbar_offset))),
                       0, list_layout.max_scroll);
      }
      surface_.focus = FocusTarget::Sidebar;
      return true;
    }

    if (surface_.drag_target == DragTarget::BottomPanelScrollbar && BottomPanelVisible()) {
      const std::size_t line_count =
          ActiveTerminalTab() != nullptr ? ActiveTerminalTab()->session.SnapshotLines().size() : 0;
      const BottomPanelLogLayout panel_layout =
          ComputeBottomPanelLogLayout(drag_layout, line_count);
      if (!panel_layout.scroll.vertical_scrollbar.has_value()) {
        surface_.drag_target = DragTarget::None;
        surface_.drag_scrollbar_offset = 0.0f;
        return false;
      }
      SetBottomPanelScrollRow(
          std::clamp(static_cast<int>(std::lround(
                         ScrollUnitsForPointer(*panel_layout.scroll.vertical_scrollbar,
                                              static_cast<float>(event.motion.y),
                                              surface_.drag_scrollbar_offset))),
                     0, panel_layout.scroll.max_vertical_scroll),
          line_count, panel_layout.scroll.visible_rows);
      if (ActiveTerminalTab() != nullptr) {
        surface_.focus = FocusTarget::Panel;
      }
      return true;
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

    if ((surface_.drag_target == DragTarget::CompareVerticalScrollbar ||
         surface_.drag_target == DragTarget::CompareHorizontalScrollbar) &&
        (ActiveTabIsCompare() || ActiveTabIsMerge())) {
      if (ActiveTabIsCompare()) {
        CompareTabState* compare_tab = ActiveCompareTab();
        if (compare_tab == nullptr) {
          surface_.drag_target = DragTarget::None;
          surface_.drag_scrollbar_offset = 0.0f;
          return false;
        }
        const CompareSurfaceLayout surface_layout =
            ComputeCompareSurfaceLayout(drag_layout.editor_surface, *compare_tab);
        const auto scroll_layout =
            ComputeCompareScrollLayout(drag_layout.editor_surface, surface_layout, *compare_tab);
        compare_tab->scroll_row = scroll_layout.vertical_scroll;
        compare_tab->horizontal_scroll = scroll_layout.horizontal_scroll;
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
          compare_tab->scroll_row = target_scroll;
          SyncCompareViewportScroll(*compare_tab);
          surface_.focus = FocusTarget::Editor;
          return true;
        }
        if (!scroll_layout.horizontal_scrollbar.has_value()) {
          surface_.drag_target = DragTarget::None;
          surface_.drag_scrollbar_offset = 0.0f;
          return false;
        }
        compare_tab->horizontal_scroll = static_cast<std::size_t>(std::max(
            0L, std::lround(ScrollUnitsForPointer(*scroll_layout.horizontal_scrollbar,
                                                  static_cast<float>(event.motion.x),
                                                  surface_.drag_scrollbar_offset))));
        SyncCompareViewportScroll(*compare_tab);
        surface_.focus = FocusTarget::Editor;
        return true;
      }

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

    if (surface_.drag_target == DragTarget::EditorVerticalScrollbar ||
        surface_.drag_target == DragTarget::EditorHorizontalScrollbar) {
      const auto panes = ComputeEditorPaneLayouts(drag_layout.editor_surface);
      const auto active_pane = std::find_if(
          panes.begin(), panes.end(), [](const EditorPaneLayout& pane) { return pane.active; });
      const SDL_FRect editor_rect =
          active_pane != panes.end() ? active_pane->rect : drag_layout.editor_surface;
      const editor::EditorViewMetrics metrics = editor::EditorViewRenderer::ComputeMetrics(
          text_renderer_, text_viewport_, editor_rect);
      text_viewport_.SetViewportSize(metrics.visible_rows, metrics.visible_columns);
      const auto scroll_layout = ComputeEditorScrollLayout(editor_rect, text_viewport_, metrics);

      if (surface_.drag_target == DragTarget::EditorVerticalScrollbar) {
        if (!scroll_layout.vertical_scrollbar.has_value()) {
          surface_.drag_target = DragTarget::None;
          surface_.drag_scrollbar_offset = 0.0f;
          return false;
        }
        text_viewport_.SetScrollLine(static_cast<std::size_t>(std::max(
            0L, std::lround(ScrollUnitsForPointer(*scroll_layout.vertical_scrollbar,
                                                  static_cast<float>(event.motion.y),
                                                  surface_.drag_scrollbar_offset)))));
      } else {
        if (!scroll_layout.horizontal_scrollbar.has_value()) {
          surface_.drag_target = DragTarget::None;
          surface_.drag_scrollbar_offset = 0.0f;
          return false;
        }
        text_viewport_.SetHorizontalScroll(static_cast<std::size_t>(std::max(
            0L, std::lround(ScrollUnitsForPointer(*scroll_layout.horizontal_scrollbar,
                                                  static_cast<float>(event.motion.x),
                                                  surface_.drag_scrollbar_offset)))));
      }
      surface_.focus = FocusTarget::Editor;
      return true;
    }

    surface_.drag_target = DragTarget::None;
    surface_.drag_scrollbar_offset = 0.0f;
    return false;
  }

  if (ActiveTerminalTab() != nullptr) {
    if (auto* terminal_tab = ActiveTerminalTab(); terminal_tab != nullptr) {
      const bool buttons_down =
          (event.motion.state & (SDL_BUTTON_LMASK | SDL_BUTTON_MMASK | SDL_BUTTON_RMASK)) != 0;
      if (terminal_tab->session.WantsMouseMotionCapture(buttons_down)) {
        if (const auto viewport_position =
                TerminalViewportPositionForPoint(event.motion.x, event.motion.y);
            viewport_position.has_value()) {
          terminal::TerminalSession::MouseButton button =
              terminal::TerminalSession::MouseButton::None;
          if ((event.motion.state & SDL_BUTTON_LMASK) != 0) {
            button = terminal::TerminalSession::MouseButton::Left;
          } else if ((event.motion.state & SDL_BUTTON_MMASK) != 0) {
            button = terminal::TerminalSession::MouseButton::Middle;
          } else if ((event.motion.state & SDL_BUTTON_RMASK) != 0) {
            button = terminal::TerminalSession::MouseButton::Right;
          }
          ClearTerminalSelection();
          terminal_tab->session.SendMouseMotion(button, viewport_position->row,
                                                viewport_position->column, SDL_GetModState());
          surface_.focus = FocusTarget::Panel;
          return true;
        }
      }
    }
  }

  if (auto* terminal_tab = ActiveTerminalTab();
      terminal_tab != nullptr && terminal_tab->mouse_selecting &&
      (event.motion.state & SDL_BUTTON_LMASK) != 0 && BottomPanelVisible() &&
      ActiveTerminalTab() != nullptr) {
    const WorkspaceLayout layout = ComputeLayout(static_cast<float>(last_window_width_),
                                                 static_cast<float>(last_window_height_),
                                                 surface_.sidebar_visible, BottomPanelVisible(),
                                                 surface_.sidebar_width, surface_.bottom_panel_height);
    if (!Contains(layout.bottom_panel, event.motion.x, event.motion.y)) {
      return false;
    }

    const auto terminal_lines = terminal_tab->session.SnapshotLines();
    if (const auto position =
            TerminalSelectionPositionForPoint(event.motion.x, event.motion.y, terminal_lines);
        position.has_value()) {
      terminal_tab->selection_head = *position;
      surface_.focus = FocusTarget::Panel;
      return true;
    }
  }

  if (ActiveTabIsMerge()) {
    MergeTabState* merge_tab = ActiveMergeTab();
    if (merge_tab != nullptr) {
      const std::optional<MergeHoverState> previous_hover = merge_tab->hover_state;
      std::optional<MergeHoverState> next_hover;
      const WorkspaceLayout layout =
          ComputeLayout(static_cast<float>(last_window_width_), static_cast<float>(last_window_height_),
                        surface_.sidebar_visible, BottomPanelVisible(), surface_.sidebar_width, surface_.bottom_panel_height);
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

  const WorkspaceLayout layout =
      ComputeLayout(static_cast<float>(last_window_width_), static_cast<float>(last_window_height_),
                    surface_.sidebar_visible, BottomPanelVisible(), surface_.sidebar_width, surface_.bottom_panel_height);
  if (!Contains(layout.editor_surface, event.motion.x, event.motion.y)) {
    return false;
  }

  if (ActiveTabIsCompare()) {
    CompareTabState* compare_tab = ActiveCompareTab();
    if (compare_tab == nullptr || !compare_tab->right_editable || !compare_tab->right_view_active) {
      return false;
    }

    const CompareSurfaceLayout surface_layout =
        ComputeCompareSurfaceLayout(layout.editor_surface, *compare_tab);
    if (event.motion.x < surface_layout.right_x) {
      return false;
    }
    const TextGridInteractionLayout right_interaction =
        BuildCompareRightInteractionLayout(surface_layout, *compare_tab);
    const auto hovered_row = VisibleTextGridLineAtY(right_interaction, event.motion.y);
    if (!hovered_row.has_value()) {
      return false;
    }
    const std::size_t line = CompareRightLineForRow(*compare_tab, *hovered_row);
    const std::size_t visual_column = TextGridVisualColumnAtX(right_interaction, event.motion.x);

    compare_tab->right_viewport.MoveCursorToVisualColumn(line, visual_column, true);
    SyncCompareSelectionFromViewport(*compare_tab, false);
    ResetCaretBlink();
    surface_.focus = FocusTarget::Editor;
    return true;
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

  const auto panes = ComputeEditorPaneLayouts(layout.editor_surface);
  const auto active_pane = std::find_if(
      panes.begin(), panes.end(), [](const EditorPaneLayout& pane) { return pane.active; });
  const SDL_FRect editor_rect =
      active_pane != panes.end() ? active_pane->rect : layout.editor_surface;
  if (!Contains(editor_rect, event.motion.x, event.motion.y)) {
    return false;
  }

  const editor::EditorViewMetrics metrics =
      editor::EditorViewRenderer::ComputeMetrics(text_renderer_, text_viewport_, editor_rect);
  text_viewport_.SetViewportSize(metrics.visible_rows, metrics.visible_columns);

  const float local_y = std::max(0.0f, event.motion.y - metrics.first_line_y);
  const std::size_t row = static_cast<std::size_t>(local_y / metrics.line_height);
  const std::size_t line = std::min(text_viewport_.scroll_line() + row,
                                    text_viewport_.line_count() == 0 ? 0 : text_viewport_.line_count() - 1);
  const float text_offset_x = std::max(0.0f, event.motion.x - metrics.text_x);
  const std::size_t visual_column =
      text_viewport_.horizontal_scroll() +
      static_cast<std::size_t>(
          std::max(0L, std::lround(text_offset_x / std::max(1.0f, text_renderer_.CharWidth()))));

  text_viewport_.MoveCursorToVisualColumn(line, visual_column, true);
  ResetCaretBlink();
  surface_.focus = FocusTarget::Editor;
  return true;
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

  if (surface_.overlay_visible) {
    const int overlay_ticks = vertical_ticks != 0 ? vertical_ticks : horizontal_ticks;
    if (surface_.overlay_mode == OverlayMode::CommitPicker) {
      MoveComparePickerSelection(-overlay_ticks);
    } else if (surface_.overlay_mode == OverlayMode::BufferSearch || surface_.overlay_mode == OverlayMode::BufferReplace) {
      MoveBufferSearchSelection(-overlay_ticks);
    } else if (surface_.overlay_mode == OverlayMode::ProjectSearch) {
      MoveProjectSearchSelection(-overlay_ticks);
    } else {
      MoveFileFinderSelection(-overlay_ticks);
    }
    return true;
  }

  if (TabMouseCoordinator(*this).HandleWheel(event, layout, vertical_ticks)) {
    return true;
  }

  if (surface_.sidebar_visible && Contains(layout.sidebar, event.wheel.mouse_x, event.wheel.mouse_y)) {
    int max_scroll = 0;
    if (surface_.sidebar_mode == SidebarMode::Search) {
      const auto line_map = BuildProjectSearchLineMap();
      max_scroll =
          ComputeProjectSearchSidebarListLayout(layout.sidebar, line_map.size()).max_scroll;
    } else if (surface_.sidebar_mode == SidebarMode::Git) {
      const auto lines = BuildGitSidebarLines();
      max_scroll = ComputeGitSidebarListLayout(layout.sidebar, lines.size()).max_scroll;
    } else {
      const auto& entries = directory_tree_.entries();
      max_scroll = ComputeTreeSidebarListLayout(layout.sidebar, entries.size()).max_scroll;
    }
    surface_.sidebar_scroll_row = std::clamp(surface_.sidebar_scroll_row - vertical_ticks, 0, max_scroll);
    surface_.focus = FocusTarget::Sidebar;
    return true;
  }

  if (ActiveTerminalTab() != nullptr) {
    if (auto* terminal_tab = ActiveTerminalTab(); terminal_tab != nullptr &&
                                                terminal_tab->session.WantsMouseCapture()) {
      if (const auto viewport_position =
              TerminalViewportPositionForPoint(event.wheel.mouse_x, event.wheel.mouse_y);
          viewport_position.has_value()) {
        const terminal::TerminalSession::MouseButton button =
            vertical_ticks > 0 ? terminal::TerminalSession::MouseButton::WheelUp
                      : terminal::TerminalSession::MouseButton::WheelDown;
        const int step_count = std::abs(vertical_ticks);
        ClearTerminalSelection();
        for (int i = 0; i < step_count; ++i) {
          terminal_tab->session.SendMouseButton(button, true, viewport_position->row,
                                                viewport_position->column, SDL_GetModState());
        }
        surface_.focus = FocusTarget::Panel;
        return true;
      }
    }
  }

  if (BottomPanelVisible() && Contains(layout.bottom_panel, event.wheel.mouse_x, event.wheel.mouse_y)) {
    const std::size_t line_count =
        ActiveTerminalTab() != nullptr ? ActiveTerminalTab()->session.SnapshotLines().size() : 0;
    const BottomPanelLogLayout panel_layout = ComputeBottomPanelLogLayout(layout, line_count);
    SetBottomPanelScrollRow(
        std::clamp(panel_layout.scroll.vertical_scroll - vertical_ticks, 0,
                   panel_layout.scroll.max_vertical_scroll),
        line_count, panel_layout.scroll.visible_rows);
    if (ActiveTerminalTab() != nullptr) {
      surface_.focus = FocusTarget::Panel;
    }
    return true;
  }

  if (Contains(layout.editor_surface, event.wheel.mouse_x, event.wheel.mouse_y)) {
    if (ActiveTabIsCompare()) {
      if (horizontal_ticks != 0) {
        ScrollCompareColumns(-horizontal_ticks * 3);
      } else {
        ScrollCompareRows(-vertical_ticks * 3);
      }
      if (auto* compare_tab = ActiveCompareTab(); compare_tab != nullptr) {
        SyncCompareViewportScroll(*compare_tab);
      }
      surface_.focus = FocusTarget::Editor;
      return true;
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
    const auto panes = ComputeEditorPaneLayouts(layout.editor_surface);
    const auto hovered_pane = std::find_if(panes.begin(), panes.end(), [&](const EditorPaneLayout& pane) {
      return Contains(pane.rect, event.wheel.mouse_x, event.wheel.mouse_y);
    });
    if (hovered_pane != panes.end() && !hovered_pane->active) {
      SetActiveEditorSplit(hovered_pane->leaf_id);
    }
    text_viewport_.ScrollVertical(-vertical_ticks * 3);
    surface_.focus = FocusTarget::Editor;
    return true;
  }

  return false;
}


}  // namespace microide::workspace
