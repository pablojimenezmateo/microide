#include "workspace/WorkspaceShell.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>

#include "editor/EditorViewRenderer.h"
#include "workspace/WorkspaceShellShared.h"

namespace microide::workspace {

namespace {

constexpr float kSidebarHeaderHeight = 26.0f;
constexpr float kBottomPanelHeaderHeight = 28.0f;
constexpr float kSidebarInset = 10.0f;
constexpr float kSidebarRowHeight = 20.0f;
constexpr float kDirtyPromptWidth = 460.0f;
constexpr float kDirtyPromptHeight = 176.0f;
constexpr float kDirtyPromptButtonWidth = 96.0f;
constexpr float kDirtyPromptButtonHeight = 28.0f;
constexpr float kDirtyPromptButtonGap = 10.0f;
constexpr float kPromptSurfaceWidth = 520.0f;
constexpr float kPromptSurfaceHeight = 188.0f;
constexpr float kPromptSurfaceButtonWidth = 108.0f;
constexpr float kPromptSurfaceButtonHeight = 28.0f;
constexpr float kPromptSurfaceButtonGap = 10.0f;
constexpr float kScrollbarThickness = 10.0f;
constexpr float kEditorSplitDividerThickness = 6.0f;
constexpr float kMinSplitPaneExtent = 180.0f;
constexpr float kMergeToolbarHeight = 54.0f;
constexpr float kMergeToolbarButtonHeight = 22.0f;
constexpr float kMergeToolbarButtonGap = 8.0f;

SDL_FRect ComputeDirtyPromptRect(const SDL_FRect& full) {
  const float width = std::min(kDirtyPromptWidth, full.w - 32.0f);
  const float height = std::min(kDirtyPromptHeight, full.h - 32.0f);
  return MakeRect(full.x + std::floor((full.w - width) * 0.5f),
                  full.y + std::floor((full.h - height) * 0.5f), width, height);
}

std::array<SDL_FRect, 3> ComputeDirtyPromptButtonRects(const SDL_FRect& dialog) {
  const float total_width =
      kDirtyPromptButtonWidth * 3.0f + kDirtyPromptButtonGap * 2.0f;
  const float start_x = dialog.x + dialog.w - total_width - 16.0f;
  const float y = dialog.y + dialog.h - kDirtyPromptButtonHeight - 16.0f;
  return {
      MakeRect(start_x, y, kDirtyPromptButtonWidth, kDirtyPromptButtonHeight),
      MakeRect(start_x + kDirtyPromptButtonWidth + kDirtyPromptButtonGap, y,
               kDirtyPromptButtonWidth, kDirtyPromptButtonHeight),
      MakeRect(start_x + (kDirtyPromptButtonWidth + kDirtyPromptButtonGap) * 2.0f, y,
               kDirtyPromptButtonWidth, kDirtyPromptButtonHeight),
  };
}

SDL_FRect ComputePromptSurfaceRect(const SDL_FRect& full) {
  const float width = std::min(kPromptSurfaceWidth, full.w - 32.0f);
  const float height = std::min(kPromptSurfaceHeight, full.h - 32.0f);
  return MakeRect(full.x + std::floor((full.w - width) * 0.5f),
                  full.y + std::floor((full.h - height) * 0.5f), width, height);
}

std::array<SDL_FRect, 2> ComputePromptSurfaceButtonRects(const SDL_FRect& dialog) {
  const float total_width =
      kPromptSurfaceButtonWidth * 2.0f + kPromptSurfaceButtonGap;
  const float start_x = dialog.x + dialog.w - total_width - 16.0f;
  const float y = dialog.y + dialog.h - kPromptSurfaceButtonHeight - 16.0f;
  return {
      MakeRect(start_x, y, kPromptSurfaceButtonWidth, kPromptSurfaceButtonHeight),
      MakeRect(start_x + kPromptSurfaceButtonWidth + kPromptSurfaceButtonGap, y,
               kPromptSurfaceButtonWidth, kPromptSurfaceButtonHeight),
  };
}

std::size_t MaxVisualColumns(const editor::TextViewport& viewport) {
  return viewport.max_visual_columns();
}

}  // namespace

bool WorkspaceShell::HandleMouseButtonDown(const SDL_Event& event) {
  if ((event.button.button != SDL_BUTTON_LEFT && event.button.button != SDL_BUTTON_MIDDLE &&
       event.button.button != SDL_BUTTON_RIGHT) ||
      last_window_width_ <= 0 || last_window_height_ <= 0) {
    return false;
  }

  UpdateMouseCursor(static_cast<float>(event.button.x), static_cast<float>(event.button.y));

  if (dirty_prompt_visible_) {
    const SDL_FRect full = MakeRect(0.0f, 0.0f, static_cast<float>(last_window_width_),
                                    static_cast<float>(last_window_height_));
    const SDL_FRect dialog = ComputeDirtyPromptRect(full);
    const auto buttons = ComputeDirtyPromptButtonRects(dialog);
    for (std::size_t i = 0; i < buttons.size(); ++i) {
      if (Contains(buttons[i], event.button.x, event.button.y)) {
        dirty_prompt_state_.selected_action = static_cast<int>(i);
        ConfirmDirtyPrompt();
        return true;
      }
    }
    return true;
  }

  if (prompt_surface_visible_) {
    const SDL_FRect full = MakeRect(0.0f, 0.0f, static_cast<float>(last_window_width_),
                                    static_cast<float>(last_window_height_));
    const SDL_FRect dialog = ComputePromptSurfaceRect(full);
    const auto buttons = ComputePromptSurfaceButtonRects(dialog);
    for (std::size_t i = 0; i < buttons.size(); ++i) {
      if (Contains(buttons[i], event.button.x, event.button.y)) {
        prompt_surface_state_.selected_button = static_cast<int>(i);
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
                    sidebar_visible_, BottomPanelVisible(), sidebar_width_, bottom_panel_height_);
  mouse_selecting_ = false;

  if (tree_context_menu_.open) {
    if (const auto popup_rect = ComputeTreeContextMenuRect();
        popup_rect.has_value() && Contains(*popup_rect, event.button.x, event.button.y)) {
      for (const VisiblePopupMenuItem& item : ComputeVisiblePopupMenuItems(
               TreeContextMenuItems(tree_context_menu_.target),
               tree_context_menu_.active_item_index, *popup_rect)) {
        if (!Contains(item.rect, event.button.x, event.button.y)) {
          continue;
        }
        tree_context_menu_.active_item_index = item.enabled ? static_cast<int>(item.index) : -1;
        if (event.button.button == SDL_BUTTON_LEFT && !item.separator && item.enabled) {
          ExecuteTreeContextMenuItem(item.index);
        }
        return true;
      }
      return true;
    }
    CloseTreeContextMenu();
  }

  if (menu_bar_open_ && event.button.button != SDL_BUTTON_LEFT) {
    CloseMenuBar();
  }

  if (event.button.button == SDL_BUTTON_LEFT) {
    if (sidebar_visible_) {
      const SDL_FRect sidebar_mode_rect = SidebarModeControlRect(layout.sidebar);
      if (Contains(sidebar_mode_rect, event.button.x, event.button.y)) {
        if (menu_bar_open_ && active_menu_id_ == MenuId::SidebarMode &&
            active_menu_anchor_rect_.has_value()) {
          CloseMenuBar();
        } else {
          OpenAnchoredMenu(MenuId::SidebarMode, sidebar_mode_rect);
        }
        focus_ = FocusTarget::Sidebar;
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
    if (menu_bar_open_) {
      for (const VisibleMenuBarItem& item : menu_bar_items) {
        if (!Contains(item.rect, event.button.x, event.button.y)) {
          continue;
        }
        if (item.id == active_menu_id_) {
          CloseMenuBar();
        } else {
          OpenMenuBarMenu(item.id);
        }
        return true;
      }

      if (const auto submenu_rect = ActiveSubmenuRect(layout.menu_bar);
          submenu_rect.has_value() && Contains(*submenu_rect, event.button.x, event.button.y)) {
        for (const VisiblePopupMenuItem& item :
             ComputeVisiblePopupMenuItems(active_submenu_id_, *submenu_rect)) {
          if (!Contains(item.rect, event.button.x, event.button.y)) {
            continue;
          }
          active_submenu_item_index_ = item.enabled ? static_cast<int>(item.index) : -1;
          if (!item.separator && item.enabled) {
            ExecuteMenuItem(active_submenu_id_, item.index);
          }
          return true;
        }
        return true;
      }

      if (const auto popup_rect = ComputePopupMenuRect(layout.menu_bar, active_menu_id_);
          popup_rect.has_value() && Contains(*popup_rect, event.button.x, event.button.y)) {
        for (const VisiblePopupMenuItem& item :
             ComputeVisiblePopupMenuItems(active_menu_id_, *popup_rect)) {
          if (!Contains(item.rect, event.button.x, event.button.y)) {
            continue;
          }
          active_menu_item_index_ = item.enabled ? static_cast<int>(item.index) : -1;
          if (!item.separator && item.enabled) {
            ExecuteMenuItem(active_menu_id_, item.index);
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

  if (event.button.button == SDL_BUTTON_LEFT && sidebar_visible_ &&
      Contains(SidebarResizeHandleRect(layout), event.button.x, event.button.y)) {
    drag_target_ = DragTarget::SidebarDivider;
    return true;
  }

  if (event.button.button == SDL_BUTTON_LEFT && BottomPanelVisible() &&
      Contains(BottomPanelResizeHandleRect(layout), event.button.x, event.button.y)) {
    drag_target_ = DragTarget::BottomPanelDivider;
    return true;
  }

  if (overlay_visible_ && event.button.button == SDL_BUTTON_LEFT) {
    const SDL_FRect overlay = ComputeOverlayRect(layout.editor_area);

    if (!Contains(overlay, event.button.x, event.button.y)) {
      overlay_visible_ = false;
      focus_ = sidebar_visible_ ? FocusTarget::Sidebar : FocusTarget::Editor;
      LogMessage("Overlay closed");
      return true;
    }

    ClampOverlayScrollRow(overlay);
    constexpr float kOverlayRowHeight = 22.0f;
    const float list_y = overlay.y + OverlayListStartOffset();
    const int visible_rows = OverlayVisibleRows(overlay);
    const int max_scroll =
        std::max(0, static_cast<int>(OverlayItemCount()) - visible_rows);
    const auto scrollbar = MakeVerticalScrollbarGeometry(
        MakeRect(overlay.x, list_y, overlay.w,
                 std::max(0.0f, overlay.y + overlay.h - list_y - 8.0f)),
        static_cast<float>(OverlayItemCount()), static_cast<float>(visible_rows),
        static_cast<float>(overlay_scroll_row_));
    if (scrollbar.has_value() && Contains(scrollbar->track, event.button.x, event.button.y)) {
      drag_target_ = DragTarget::OverlayScrollbar;
      drag_scrollbar_offset_ =
          Contains(scrollbar->thumb, event.button.x, event.button.y)
              ? static_cast<float>(event.button.y) - scrollbar->thumb.y
              : scrollbar->thumb.h * 0.5f;
      overlay_scroll_row_ =
          std::clamp(static_cast<int>(std::lround(
                         ScrollUnitsForPointer(*scrollbar, static_cast<float>(event.button.y),
                                              drag_scrollbar_offset_))),
                     0, max_scroll);
      focus_ = FocusTarget::Overlay;
      return true;
    }

    const int row = static_cast<int>((event.button.y - list_y) / kOverlayRowHeight);
    if (row >= 0 && row < visible_rows) {
      const int item_index = overlay_scroll_row_ + row;
      if (item_index >= 0 && item_index < static_cast<int>(OverlayItemCount())) {
        SetOverlaySelectedIndex(static_cast<std::size_t>(item_index));
        RevealOverlaySelection(overlay);
        if (overlay_mode_ == OverlayMode::CommitPicker) {
          ActivateOverlaySelection();
        }
      }
    }
    focus_ = FocusTarget::Overlay;
    return true;
  }

  if (event.button.button == SDL_BUTTON_LEFT && sidebar_visible_) {
    if (sidebar_mode_ == SidebarMode::Search) {
      const float list_y = layout.sidebar.y + kProjectSearchResultsTop;
      const auto line_map = BuildProjectSearchLineMap();
      const int visible_rows = std::max(
          1, static_cast<int>((layout.sidebar.h - kProjectSearchResultsTop) / kSidebarRowHeight));
      const int max_scroll = std::max(0, static_cast<int>(line_map.size()) - visible_rows);
      const int scroll_row = std::clamp(sidebar_scroll_row_, 0, max_scroll);
      const auto scrollbar = MakeVerticalScrollbarGeometry(
          MakeRect(layout.sidebar.x, list_y, layout.sidebar.w,
                   std::max(0.0f, layout.sidebar.y + layout.sidebar.h - list_y)),
          static_cast<float>(line_map.size()), static_cast<float>(visible_rows),
          static_cast<float>(scroll_row));
      if (scrollbar.has_value() && Contains(scrollbar->track, event.button.x, event.button.y)) {
        drag_target_ = DragTarget::SidebarScrollbar;
        drag_scrollbar_offset_ =
            Contains(scrollbar->thumb, event.button.x, event.button.y)
                ? static_cast<float>(event.button.y) - scrollbar->thumb.y
                : scrollbar->thumb.h * 0.5f;
        sidebar_scroll_row_ =
            std::clamp(static_cast<int>(std::lround(
                           ScrollUnitsForPointer(*scrollbar, static_cast<float>(event.button.y),
                                                drag_scrollbar_offset_))),
                       0, max_scroll);
        focus_ = FocusTarget::Sidebar;
        return true;
      }
    } else if (sidebar_mode_ == SidebarMode::Git) {
      const auto lines = BuildGitSidebarLines();
      const float list_y = GitSidebarListTop(layout.sidebar);
      const float visible_units = GitSidebarVisibleUnits(layout.sidebar);
      const int max_scroll = std::max(
          0, static_cast<int>(std::ceil(static_cast<float>(lines.size()) - visible_units)));
      const int scroll_row = std::clamp(sidebar_scroll_row_, 0, max_scroll);
      const auto scrollbar = MakeVerticalScrollbarGeometry(
          MakeRect(layout.sidebar.x, list_y, layout.sidebar.w,
                   std::max(0.0f, layout.sidebar.y + layout.sidebar.h - list_y)),
          static_cast<float>(lines.size()), visible_units,
          static_cast<float>(scroll_row));
      if (scrollbar.has_value() && Contains(scrollbar->track, event.button.x, event.button.y)) {
        drag_target_ = DragTarget::SidebarScrollbar;
        drag_scrollbar_offset_ =
            Contains(scrollbar->thumb, event.button.x, event.button.y)
                ? static_cast<float>(event.button.y) - scrollbar->thumb.y
                : scrollbar->thumb.h * 0.5f;
        sidebar_scroll_row_ =
            std::clamp(static_cast<int>(std::lround(
                           ScrollUnitsForPointer(*scrollbar, static_cast<float>(event.button.y),
                                                drag_scrollbar_offset_))),
                       0, max_scroll);
        focus_ = FocusTarget::Sidebar;
        return true;
      }
    } else {
      const auto& entries = directory_tree_.entries();
      const float list_y = layout.sidebar.y + kSidebarHeaderHeight + 6.0f;
      const int visible_rows =
          std::max(1, static_cast<int>((layout.sidebar.h - 36.0f) / kSidebarRowHeight));
      const int max_scroll = std::max(0, static_cast<int>(entries.size()) - visible_rows);
      const int scroll_row = std::clamp(sidebar_scroll_row_, 0, max_scroll);
      const auto scrollbar = MakeVerticalScrollbarGeometry(
          MakeRect(layout.sidebar.x, list_y, layout.sidebar.w,
                   std::max(0.0f, layout.sidebar.y + layout.sidebar.h - list_y)),
          static_cast<float>(entries.size()), static_cast<float>(visible_rows),
          static_cast<float>(scroll_row));
      if (scrollbar.has_value() && Contains(scrollbar->track, event.button.x, event.button.y)) {
        drag_target_ = DragTarget::SidebarScrollbar;
        drag_scrollbar_offset_ =
            Contains(scrollbar->thumb, event.button.x, event.button.y)
                ? static_cast<float>(event.button.y) - scrollbar->thumb.y
                : scrollbar->thumb.h * 0.5f;
        sidebar_scroll_row_ =
            std::clamp(static_cast<int>(std::lround(
                           ScrollUnitsForPointer(*scrollbar, static_cast<float>(event.button.y),
                                                drag_scrollbar_offset_))),
                       0, max_scroll);
        focus_ = FocusTarget::Sidebar;
        return true;
      }
    }
  }

  if (Contains(layout.project_tab_strip, event.button.x, event.button.y)) {
    for (const VisibleProjectTab& tab : ComputeVisibleProjectTabs(layout.project_tab_strip)) {
      if (!Contains(tab.rect, event.button.x, event.button.y)) {
        continue;
      }
      if (event.button.button == SDL_BUTTON_MIDDLE ||
          (event.button.button == SDL_BUTTON_LEFT &&
           Contains(tab.close_rect, event.button.x, event.button.y))) {
        RequestCloseProject(tab.index);
      } else if (event.button.button == SDL_BUTTON_LEFT) {
        SwitchProject(tab.index, true);
      }
      return true;
    }
  }

  if (Contains(layout.tab_strip, event.button.x, event.button.y)) {
    if (project_root_.empty()) {
      return false;
    }
    if (open_tabs_.empty()) {
      const SDL_FRect placeholder_tab =
          MakeRect(layout.tab_strip.x + 12.0f, layout.tab_strip.y + 2.0f, 220.0f,
                   std::max(22.0f, layout.tab_strip.h - 2.0f));
      if (event.button.button == SDL_BUTTON_LEFT &&
          Contains(placeholder_tab, event.button.x, event.button.y)) {
        focus_ = FocusTarget::Editor;
        return true;
      }
      return false;
    }

    for (const VisibleTab& tab : ComputeVisibleTabs(layout.tab_strip)) {
      if (Contains(tab.rect, event.button.x, event.button.y)) {
        if (event.button.button == SDL_BUTTON_MIDDLE ||
            (event.button.button == SDL_BUTTON_LEFT &&
             Contains(tab.close_rect, event.button.x, event.button.y))) {
          RequestCloseTab(tab.index);
        } else if (event.button.button == SDL_BUTTON_LEFT) {
          ActivateTab(tab.index);
        }
        return true;
      }
    }
  }

  if (BottomPanelVisible()) {
    const SDL_FRect panel_header =
        MakeRect(layout.bottom_panel.x, layout.bottom_panel.y, layout.bottom_panel.w,
                 kBottomPanelHeaderHeight);
    if (ActiveTerminalTab() != nullptr && Contains(panel_header, event.button.x, event.button.y)) {
      if (event.button.button == SDL_BUTTON_LEFT &&
          Contains(BottomPanelTerminalNewTabRect(panel_header), event.button.x, event.button.y)) {
        OpenTerminal({});
        return true;
      }

      for (const VisibleTerminalTab& tab : ComputeVisibleTerminalTabs(panel_header)) {
        if (!Contains(tab.rect, event.button.x, event.button.y)) {
          continue;
        }

        if (event.button.button == SDL_BUTTON_MIDDLE ||
            (event.button.button == SDL_BUTTON_LEFT &&
             Contains(tab.close_rect, event.button.x, event.button.y))) {
          CloseTerminalTab(tab.index);
        } else if (event.button.button == SDL_BUTTON_LEFT) {
          active_terminal_tab_index_ = tab.index;
          focus_ = FocusTarget::Panel;
        }
        return true;
      }
    }
  }

  if (sidebar_visible_ && Contains(layout.sidebar, event.button.x, event.button.y)) {
    focus_ = FocusTarget::Sidebar;
    const float header_height = kSidebarHeaderHeight + 6.0f;
    const float inset = kSidebarInset;
    const float row_height = kSidebarRowHeight;
    const float list_top = layout.sidebar.y + header_height;
    const float local_y = event.button.y - list_top;

    if (sidebar_mode_ == SidebarMode::Search) {
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
        if (project_search_editing_) {
          CommitProjectSearchEdit();
        }
        ToggleProjectSearchPatternMode();
        return true;
      }
      if (Contains(ProjectSearchCaseButtonRect(layout.sidebar), event.button.x, event.button.y)) {
        if (project_search_editing_) {
          CommitProjectSearchEdit();
        }
        CycleProjectSearchCaseMode();
        return true;
      }
      if (Contains(ProjectSearchHiddenButtonRect(layout.sidebar), event.button.x, event.button.y)) {
        if (project_search_editing_) {
          CommitProjectSearchEdit();
        }
        ToggleProjectSearchHiddenFiles();
        return true;
      }
      if (local_y < 0.0f) {
        return true;
      }

      const auto line_map = BuildProjectSearchLineMap();
      const int visible_rows =
          std::max(1, static_cast<int>((layout.sidebar.h - kProjectSearchResultsTop) / row_height));
      const int max_scroll = std::max(0, static_cast<int>(line_map.size()) - visible_rows);
      const int scroll_row = std::clamp(sidebar_scroll_row_, 0, max_scroll);
      const int clicked_row =
          static_cast<int>((local_y - (kProjectSearchResultsTop - header_height)) / row_height);
      if (clicked_row >= 0) {
        const int line_index = scroll_row + clicked_row;
        if (line_index >= 0 && line_index < static_cast<int>(line_map.size()) &&
            line_map[static_cast<std::size_t>(line_index)] >= 0) {
          project_search_selected_index_ =
              static_cast<std::size_t>(line_map[static_cast<std::size_t>(line_index)]);
          const auto& result = project_search_results_[project_search_selected_index_];
          OpenFile(project_root_ / result.relative_path);
          text_viewport_.MoveCursorTo(result.line, result.column);
          if (sidebar_temporary_) {
            RestorePreviousSidebar();
          }
          focus_ = FocusTarget::Editor;
          LogMessage("Project search result opened");
        }
      }
      return true;
    }

    if (sidebar_mode_ == SidebarMode::Git) {
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
      const float git_list_top = GitSidebarListTop(layout.sidebar);
      const float local_git_y = event.button.y - git_list_top;
      if (local_git_y < 0.0f) {
        return true;
      }

      const auto lines = BuildGitSidebarLines();
      const float visible_units = GitSidebarVisibleUnits(layout.sidebar);
      const int max_scroll = std::max(
          0, static_cast<int>(std::ceil(static_cast<float>(lines.size()) - visible_units)));
      const int scroll_row = std::clamp(sidebar_scroll_row_, 0, max_scroll);
      const float row_width =
          std::max(0.0f, layout.sidebar.w - inset * 2.0f -
                             (max_scroll > 0 ? kScrollbarThickness + 6.0f : 0.0f));
      const int clicked_row = static_cast<int>(local_git_y / row_height);
      const int line_index = scroll_row + clicked_row;
      if (line_index < 0 || line_index >= static_cast<int>(lines.size())) {
        return true;
      }

      const auto& line = lines[static_cast<std::size_t>(line_index)];
      if (line.kind != GitSidebarLine::Kind::Entry || line.entry_index < 0) {
        return true;
      }

      git_sidebar_selected_index_ = static_cast<std::size_t>(line.entry_index);
      const auto& entry = git_sidebar_entries_[git_sidebar_selected_index_];
      const SDL_FRect row_rect = MakeRect(layout.sidebar.x + inset,
                                          list_top + static_cast<float>(clicked_row) * row_height,
                                          row_width, row_height - 2.0f);
      float right_edge = row_rect.x + row_rect.w - 8.0f;
      if (entry.section == GitSidebarEntry::Section::Modified) {
        const std::string_view stage_label = entry.staged ? "Unstage" : "Stage";
        const float stage_width =
            std::max(entry.staged ? 68.0f : 48.0f, text_renderer_.MeasureWidth(stage_label) + 16.0f);
        const SDL_FRect stage_rect =
            MakeRect(right_edge - stage_width, row_rect.y + 1.0f, stage_width, row_rect.h - 2.0f);
        if (Contains(stage_rect, event.button.x, event.button.y)) {
          if (entry.staged) {
            UnstageGitSidebarEntry(git_sidebar_selected_index_);
          } else {
            StageGitSidebarEntry(git_sidebar_selected_index_);
          }
          return true;
        }
        right_edge = stage_rect.x - 6.0f;
        const float discard_width =
            std::max(62.0f, text_renderer_.MeasureWidth("Discard") + 16.0f);
        const SDL_FRect discard_rect =
            MakeRect(right_edge - discard_width, row_rect.y + 1.0f, discard_width, row_rect.h - 2.0f);
        if (Contains(discard_rect, event.button.x, event.button.y)) {
          DiscardGitSidebarEntry(git_sidebar_selected_index_);
          return true;
        }
      }
      OpenGitSidebarEntry(git_sidebar_selected_index_);
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
    const int visible_rows = std::max(1, static_cast<int>((layout.sidebar.h - 36.0f) / row_height));
    const int max_scroll = std::max(0, static_cast<int>(entries.size()) - visible_rows);
    const int scroll_row = std::clamp(sidebar_scroll_row_, 0, max_scroll);
    const float row_width =
        std::max(0.0f, layout.sidebar.w - inset * 2.0f -
                           (max_scroll > 0 ? kScrollbarThickness + 6.0f : 0.0f));

    const int clicked_row = static_cast<int>(local_y / row_height);
    const int entry_index = scroll_row + clicked_row;
    if (entry_index >= 0 && entry_index < static_cast<int>(entries.size())) {
      directory_tree_.SetSelectedIndex(static_cast<std::size_t>(entry_index));
      const SDL_FRect row_rect = MakeRect(
          layout.sidebar.x + inset,
          list_top + static_cast<float>(clicked_row) * row_height,
          row_width,
          row_height - 2.0f);
      if (Contains(row_rect, event.button.x, event.button.y) &&
          event.button.button == SDL_BUTTON_RIGHT) {
        const auto& entry = entries[static_cast<std::size_t>(entry_index)];
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
        } else {
          LogMessage("Tree selection toggled");
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
    const int visible_rows = BottomPanelVisibleRows(layout.bottom_panel.h);
    const int max_scroll = std::max(0, static_cast<int>(line_count) - visible_rows);
    const int scroll_row = BottomPanelScrollRow(line_count, visible_rows);
    const auto scrollbar =
        MakeVerticalScrollbarGeometry(BottomPanelContentRect(layout, command_mode_),
                                      static_cast<float>(line_count),
                                      static_cast<float>(visible_rows),
                                      static_cast<float>(scroll_row));
    if (scrollbar.has_value() && Contains(scrollbar->track, event.button.x, event.button.y)) {
      drag_target_ = DragTarget::BottomPanelScrollbar;
      drag_scrollbar_offset_ =
          Contains(scrollbar->thumb, event.button.x, event.button.y)
              ? static_cast<float>(event.button.y) - scrollbar->thumb.y
              : scrollbar->thumb.h * 0.5f;
      SetBottomPanelScrollRow(
          std::clamp(static_cast<int>(std::lround(
                         ScrollUnitsForPointer(*scrollbar, static_cast<float>(event.button.y),
                                              drag_scrollbar_offset_))),
                     0, max_scroll),
          line_count, visible_rows);
      if (ActiveTerminalTab() != nullptr) {
        focus_ = FocusTarget::Panel;
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
        focus_ = FocusTarget::Panel;
        return true;
      }
    }
  }

  if (event.button.button == SDL_BUTTON_LEFT && BottomPanelVisible() &&
      Contains(layout.bottom_panel, event.button.x, event.button.y)) {
    if (ActiveTerminalTab() != nullptr) {
      const SDL_FRect panel_content = BottomPanelContentRect(layout, command_mode_);
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
      focus_ = FocusTarget::Panel;
    }
    if (command_mode_) {
      focus_ = FocusTarget::Panel;
    }
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
    ClampCompareScrollRow(*compare_tab, surface_layout.visible_rows);
    ClampCompareHorizontalScroll(*compare_tab, surface_layout.visible_columns);

    const auto vertical_scrollbar = MakeVerticalScrollbarGeometry(
        layout.editor_surface, static_cast<float>(compare_tab->model.rows.size()),
        static_cast<float>(surface_layout.visible_rows), static_cast<float>(compare_tab->scroll_row),
        surface_layout.show_horizontal);
    if (vertical_scrollbar.has_value() &&
        Contains(vertical_scrollbar->track, event.button.x, event.button.y)) {
      drag_target_ = DragTarget::CompareVerticalScrollbar;
      drag_scrollbar_offset_ =
          Contains(vertical_scrollbar->thumb, event.button.x, event.button.y)
              ? static_cast<float>(event.button.y) - vertical_scrollbar->thumb.y
              : vertical_scrollbar->thumb.h * 0.5f;
      const int target_scroll = std::clamp(
          static_cast<int>(std::lround(ScrollUnitsForPointer(
              *vertical_scrollbar, static_cast<float>(event.button.y), drag_scrollbar_offset_))),
          0, CompareMaxScrollRow(*compare_tab, surface_layout.visible_rows));
      compare_tab->scroll_row = target_scroll;
      focus_ = FocusTarget::Editor;
      return true;
    }

    if (surface_layout.show_horizontal) {
      const auto horizontal_scrollbar = MakeHorizontalScrollbarGeometry(
          layout.editor_surface, static_cast<float>(compare_tab->max_visual_columns),
          static_cast<float>(surface_layout.visible_columns),
          static_cast<float>(compare_tab->horizontal_scroll), surface_layout.show_vertical);
      if (horizontal_scrollbar.has_value() &&
          Contains(horizontal_scrollbar->track, event.button.x, event.button.y)) {
        drag_target_ = DragTarget::CompareHorizontalScrollbar;
        drag_scrollbar_offset_ =
            Contains(horizontal_scrollbar->thumb, event.button.x, event.button.y)
                ? static_cast<float>(event.button.x) - horizontal_scrollbar->thumb.x
                : horizontal_scrollbar->thumb.w * 0.5f;
        compare_tab->horizontal_scroll = static_cast<std::size_t>(std::max(
            0L, std::lround(ScrollUnitsForPointer(*horizontal_scrollbar,
                                                  static_cast<float>(event.button.x),
                                                  drag_scrollbar_offset_))));
        focus_ = FocusTarget::Editor;
        return true;
      }
    }

    const int clicked_row =
        static_cast<int>((event.button.y - surface_layout.rows_y) / surface_layout.line_height);
    const int model_row = compare_tab->scroll_row + clicked_row;
    if (clicked_row >= 0 && model_row >= 0 &&
        model_row < static_cast<int>(compare_tab->model.rows.size())) {
      compare_tab->selected_row = static_cast<std::size_t>(model_row);
      focus_ = FocusTarget::Editor;
      return true;
    }
    return false;
  }

  if (ActiveTabIsMerge()) {
    MergeTabState* merge_tab = ActiveMergeTab();
    if (merge_tab == nullptr) {
      return false;
    }

    const MergeSurfaceLayout surface_layout =
        ComputeMergeSurfaceLayout(layout.editor_surface, *merge_tab);
    ClampMergeScrollRow(*merge_tab, surface_layout.visible_rows);
    ClampMergeHorizontalScroll(*merge_tab, surface_layout.visible_columns);

    const auto make_button_rect = [&](float x, float y, std::string_view label) {
      const float width =
          std::clamp(text_renderer_.MeasureWidth(label) + 18.0f, 64.0f, 160.0f);
      return MakeRect(x, y, width, kMergeToolbarButtonHeight);
    };
    float button_x = layout.editor_surface.x + 8.0f;
    const float button_y = surface_layout.button_y;
    const SDL_FRect incoming_all_rect = make_button_rect(button_x, button_y, "All Incoming");
    button_x += incoming_all_rect.w + kMergeToolbarButtonGap;
    const SDL_FRect auto_rect = make_button_rect(button_x, button_y, "All Auto");
    button_x += auto_rect.w + kMergeToolbarButtonGap;
    const SDL_FRect current_all_rect = make_button_rect(button_x, button_y, "All Current");
    button_x += current_all_rect.w + kMergeToolbarButtonGap;
    const SDL_FRect base_all_rect = make_button_rect(button_x, button_y, "All Base");
    const SDL_FRect save_rect =
        make_button_rect(layout.editor_surface.x + layout.editor_surface.w - 92.0f, button_y, "Save");

    if (Contains(incoming_all_rect, event.button.x, event.button.y)) {
      ApplyMergeChoiceToAll(compare::MergeChoice::Incoming);
      return true;
    }
    if (Contains(auto_rect, event.button.x, event.button.y)) {
      ApplyMergeChoiceToAll(compare::MergeChoice::Auto);
      return true;
    }
    if (Contains(current_all_rect, event.button.x, event.button.y)) {
      ApplyMergeChoiceToAll(compare::MergeChoice::Current);
      return true;
    }
    if (Contains(base_all_rect, event.button.x, event.button.y)) {
      ApplyMergeChoiceToAll(compare::MergeChoice::Base);
      return true;
    }
    if (Contains(save_rect, event.button.x, event.button.y)) {
      ExecuteAction(ActionId::Save, {}, ActionSource::Menu);
      return true;
    }

    if (!merge_tab->model.hunks.empty()) {
      button_x = layout.editor_surface.x + 8.0f;
      const float selected_y = surface_layout.secondary_button_y;
      const SDL_FRect incoming_rect = make_button_rect(button_x, selected_y, "Incoming");
      button_x += incoming_rect.w + kMergeToolbarButtonGap;
      const SDL_FRect base_rect = make_button_rect(button_x, selected_y, "Base");
      button_x += base_rect.w + kMergeToolbarButtonGap;
      const SDL_FRect current_rect = make_button_rect(button_x, selected_y, "Current");
      button_x += current_rect.w + kMergeToolbarButtonGap;
      const SDL_FRect both_rect = make_button_rect(button_x, selected_y, "Both");
      button_x += both_rect.w + kMergeToolbarButtonGap;
      const SDL_FRect open_rect = make_button_rect(button_x, selected_y, "Open Result");

      if (Contains(incoming_rect, event.button.x, event.button.y)) {
        ApplyMergeChoice(compare::MergeChoice::Incoming);
        return true;
      }
      if (Contains(base_rect, event.button.x, event.button.y)) {
        ApplyMergeChoice(compare::MergeChoice::Base);
        return true;
      }
      if (Contains(current_rect, event.button.x, event.button.y)) {
        ApplyMergeChoice(compare::MergeChoice::Current);
        return true;
      }
      if (Contains(both_rect, event.button.x, event.button.y)) {
        ApplyMergeChoice(compare::MergeChoice::Both);
        return true;
      }
      if (Contains(open_rect, event.button.x, event.button.y)) {
        OpenMergeResultFile();
        return true;
      }
    }

    const auto vertical_scrollbar = MakeVerticalScrollbarGeometry(
        layout.editor_surface, static_cast<float>(merge_tab->display_model.rows.size()),
        static_cast<float>(surface_layout.visible_rows), static_cast<float>(merge_tab->scroll_row),
        surface_layout.show_horizontal);
    if (vertical_scrollbar.has_value() &&
        Contains(vertical_scrollbar->track, event.button.x, event.button.y)) {
      drag_target_ = DragTarget::CompareVerticalScrollbar;
      drag_scrollbar_offset_ =
          Contains(vertical_scrollbar->thumb, event.button.x, event.button.y)
              ? static_cast<float>(event.button.y) - vertical_scrollbar->thumb.y
              : vertical_scrollbar->thumb.h * 0.5f;
      merge_tab->scroll_row = std::clamp(
          static_cast<int>(std::lround(ScrollUnitsForPointer(
              *vertical_scrollbar, static_cast<float>(event.button.y), drag_scrollbar_offset_))),
          0, MergeMaxScrollRow(*merge_tab, surface_layout.visible_rows));
      focus_ = FocusTarget::Editor;
      return true;
    }

    if (surface_layout.show_horizontal) {
      const auto horizontal_scrollbar = MakeHorizontalScrollbarGeometry(
          layout.editor_surface, static_cast<float>(merge_tab->max_visual_columns),
          static_cast<float>(surface_layout.visible_columns),
          static_cast<float>(merge_tab->horizontal_scroll), surface_layout.show_vertical);
      if (horizontal_scrollbar.has_value() &&
          Contains(horizontal_scrollbar->track, event.button.x, event.button.y)) {
        drag_target_ = DragTarget::CompareHorizontalScrollbar;
        drag_scrollbar_offset_ =
            Contains(horizontal_scrollbar->thumb, event.button.x, event.button.y)
                ? static_cast<float>(event.button.x) - horizontal_scrollbar->thumb.x
                : horizontal_scrollbar->thumb.w * 0.5f;
        merge_tab->horizontal_scroll = static_cast<std::size_t>(std::max(
            0L, std::lround(ScrollUnitsForPointer(*horizontal_scrollbar,
                                                  static_cast<float>(event.button.x),
                                                  drag_scrollbar_offset_))));
        focus_ = FocusTarget::Editor;
        return true;
      }
    }

    const int clicked_row =
        static_cast<int>((event.button.y - surface_layout.rows_y) /
                         std::max(1.0f, surface_layout.line_height));
    const int model_row = merge_tab->scroll_row + clicked_row;
    if (clicked_row >= 0 && model_row >= 0 &&
        model_row < static_cast<int>(merge_tab->display_model.rows.size())) {
      const int hunk_index = merge_tab->display_model.rows[static_cast<std::size_t>(model_row)].hunk;
      if (hunk_index >= 0) {
        merge_tab->selected_hunk = static_cast<std::size_t>(hunk_index);
        RevealActiveMergeSelection();
      }
      focus_ = FocusTarget::Editor;
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
    drag_target_ = DragTarget::EditorSplitDivider;
    drag_editor_split_path_ = divider_it->node_path;
    drag_editor_split_divider_index_ = divider_it->divider_index;
    focus_ = FocusTarget::Editor;
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

  const std::size_t total_columns =
      std::max<std::size_t>(text_viewport_.visible_columns(), MaxVisualColumns(text_viewport_));
  const bool show_vertical = text_viewport_.line_count() > text_viewport_.visible_lines();
  const bool show_horizontal = total_columns > text_viewport_.visible_columns();
  if (show_vertical) {
    const auto scrollbar = MakeVerticalScrollbarGeometry(
        editor_rect, static_cast<float>(text_viewport_.line_count()),
        static_cast<float>(text_viewport_.visible_lines()),
        static_cast<float>(text_viewport_.scroll_line()), show_horizontal);
    if (scrollbar.has_value() && Contains(scrollbar->track, event.button.x, event.button.y)) {
      drag_target_ = DragTarget::EditorVerticalScrollbar;
      drag_scrollbar_offset_ =
          Contains(scrollbar->thumb, event.button.x, event.button.y)
              ? static_cast<float>(event.button.y) - scrollbar->thumb.y
              : scrollbar->thumb.h * 0.5f;
      text_viewport_.SetScrollLine(static_cast<std::size_t>(std::max(
          0L, std::lround(ScrollUnitsForPointer(*scrollbar, static_cast<float>(event.button.y),
                                                drag_scrollbar_offset_)))));
      focus_ = FocusTarget::Editor;
      return true;
    }
  }
  if (show_horizontal) {
    const auto scrollbar = MakeHorizontalScrollbarGeometry(
        editor_rect, static_cast<float>(total_columns),
        static_cast<float>(text_viewport_.visible_columns()),
        static_cast<float>(text_viewport_.horizontal_scroll()), show_vertical);
    if (scrollbar.has_value() && Contains(scrollbar->track, event.button.x, event.button.y)) {
      drag_target_ = DragTarget::EditorHorizontalScrollbar;
      drag_scrollbar_offset_ =
          Contains(scrollbar->thumb, event.button.x, event.button.y)
              ? static_cast<float>(event.button.x) - scrollbar->thumb.x
              : scrollbar->thumb.w * 0.5f;
      text_viewport_.SetHorizontalScroll(static_cast<std::size_t>(std::max(
          0L, std::lround(ScrollUnitsForPointer(*scrollbar, static_cast<float>(event.button.x),
                                                drag_scrollbar_offset_)))));
      focus_ = FocusTarget::Editor;
      return true;
    }
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
  focus_ = FocusTarget::Editor;
  mouse_selecting_ = true;
  return true;
}

bool WorkspaceShell::HandleMouseButtonUp(const SDL_Event& event) {
  UpdateMouseCursor(static_cast<float>(event.button.x), static_cast<float>(event.button.y));

  if (dirty_prompt_visible_) {
    return true;
  }
  if (prompt_surface_visible_) {
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
      focus_ = FocusTarget::Panel;
      return true;
    }
  }

  if (event.button.button != SDL_BUTTON_LEFT) {
    return false;
  }
  if (drag_target_ != DragTarget::None) {
    drag_target_ = DragTarget::None;
    drag_scrollbar_offset_ = 0.0f;
    drag_editor_split_path_.clear();
    drag_editor_split_divider_index_ = 0;
    mouse_selecting_ = false;
    UpdateMouseCursor(static_cast<float>(event.button.x), static_cast<float>(event.button.y));
    return true;
  }
  if (auto* terminal_tab = ActiveTerminalTab();
      terminal_tab != nullptr && terminal_tab->mouse_selecting) {
    terminal_tab->mouse_selecting = false;
    return true;
  }
  const bool was_selecting = mouse_selecting_;
  mouse_selecting_ = false;
  return was_selecting;
}

bool WorkspaceShell::HandleMouseMotion(const SDL_Event& event) {
  if (last_window_width_ > 0 && last_window_height_ > 0) {
    UpdateMouseCursor(static_cast<float>(event.motion.x), static_cast<float>(event.motion.y));
  }

  if (dirty_prompt_visible_) {
    return true;
  }
  if (prompt_surface_visible_) {
    return true;
  }

  if (last_window_width_ <= 0 || last_window_height_ <= 0) {
    return false;
  }

  if (tree_context_menu_.open) {
    if (const auto popup_rect = ComputeTreeContextMenuRect();
        popup_rect.has_value() && Contains(*popup_rect, event.motion.x, event.motion.y)) {
      tree_context_menu_.active_item_index = -1;
      for (const VisiblePopupMenuItem& item :
           ComputeVisiblePopupMenuItems(TreeContextMenuItems(tree_context_menu_.target),
                                        tree_context_menu_.active_item_index, *popup_rect)) {
        if (Contains(item.rect, event.motion.x, event.motion.y)) {
          tree_context_menu_.active_item_index = item.enabled ? static_cast<int>(item.index) : -1;
          break;
        }
      }
      return true;
    }
    tree_context_menu_.active_item_index = -1;
    return true;
  }

  if (menu_bar_open_) {
    const WorkspaceLayout layout =
        ComputeLayout(static_cast<float>(last_window_width_), static_cast<float>(last_window_height_),
                      sidebar_visible_, BottomPanelVisible(), sidebar_width_, bottom_panel_height_);
    for (const VisibleMenuBarItem& item : ComputeVisibleMenuBarItems(layout.menu_bar)) {
      if (!Contains(item.rect, event.motion.x, event.motion.y)) {
        continue;
      }
      if (item.id != active_menu_id_) {
        OpenMenuBarMenu(item.id);
      }
      return true;
    }
    if (const auto submenu_rect = ActiveSubmenuRect(layout.menu_bar);
        submenu_rect.has_value() && Contains(*submenu_rect, event.motion.x, event.motion.y)) {
      active_submenu_item_index_ = -1;
      for (const VisiblePopupMenuItem& item :
           ComputeVisiblePopupMenuItems(active_submenu_id_, *submenu_rect)) {
        if (Contains(item.rect, event.motion.x, event.motion.y)) {
          active_submenu_item_index_ = item.enabled ? static_cast<int>(item.index) : -1;
          break;
        }
      }
      return true;
    }
    if (const auto popup_rect = ComputePopupMenuRect(layout.menu_bar, active_menu_id_);
        popup_rect.has_value() && Contains(*popup_rect, event.motion.x, event.motion.y)) {
      active_menu_item_index_ = -1;
      for (const VisiblePopupMenuItem& item :
           ComputeVisiblePopupMenuItems(active_menu_id_, *popup_rect)) {
        if (Contains(item.rect, event.motion.x, event.motion.y)) {
          active_menu_item_index_ = item.enabled ? static_cast<int>(item.index) : -1;
          const MenuSpec* menu = FindMenuSpec(active_menu_id_);
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
    active_menu_item_index_ = -1;
    return true;
  }

  if (drag_target_ != DragTarget::None) {
    if ((event.motion.state & SDL_BUTTON_LMASK) == 0) {
      drag_target_ = DragTarget::None;
      drag_scrollbar_offset_ = 0.0f;
      drag_editor_split_path_.clear();
      drag_editor_split_divider_index_ = 0;
      UpdateMouseCursor(static_cast<float>(event.motion.x), static_cast<float>(event.motion.y));
      return false;
    }

    if (drag_target_ == DragTarget::SidebarDivider) {
      sidebar_width_ =
          ClampSidebarWidth(static_cast<float>(event.motion.x), static_cast<float>(last_window_width_));
      return true;
    }

    if (drag_target_ == DragTarget::BottomPanelDivider) {
      const float desired_height =
          static_cast<float>(last_window_height_) - static_cast<float>(event.motion.y);
      bottom_panel_height_ =
          ClampBottomPanelHeight(desired_height, static_cast<float>(last_window_height_));
      return true;
    }

    const WorkspaceLayout drag_layout =
        ComputeLayout(static_cast<float>(last_window_width_), static_cast<float>(last_window_height_),
                      sidebar_visible_, BottomPanelVisible(), sidebar_width_, bottom_panel_height_);

    if (drag_target_ == DragTarget::EditorSplitDivider) {
      auto* editor_tab = ActiveEditorTab();
      if (editor_tab == nullptr || editor_tab->views.size() < 2 || editor_tab->split_root == nullptr) {
        drag_target_ = DragTarget::None;
        return false;
      }

      NormalizeEditorSplitTree(*editor_tab);
      auto* split_node = FindEditorSplitNode(editor_tab->split_root.get(), drag_editor_split_path_);
      const auto node_rect =
          ComputeEditorSplitNodeRect(drag_layout.editor_surface, drag_editor_split_path_);
      if (split_node == nullptr || node_rect == std::nullopt || split_node->IsLeaf() ||
          split_node->orientation == EditorSplitOrientation::None ||
          drag_editor_split_divider_index_ + 1 >= split_node->children.size()) {
        drag_target_ = DragTarget::None;
        return false;
      }

      const bool vertical = split_node->orientation == EditorSplitOrientation::Vertical;
      const std::size_t child_count = split_node->children.size();
      const float total_extent =
          std::max(0.0f,
                   (vertical ? node_rect->w : node_rect->h) -
                       kEditorSplitDividerThickness * static_cast<float>(child_count - 1));
      if (total_extent <= 0.0f) {
        return false;
      }

      std::vector<float> weights(child_count, 0.0f);
      float total_weight = 0.0f;
      for (std::size_t i = 0; i < child_count; ++i) {
        weights[i] = std::max(0.0f, split_node->children[i]->size_fraction);
        total_weight += weights[i];
      }
      if (total_weight <= 0.0f) {
        std::fill(weights.begin(), weights.end(), 1.0f);
        total_weight = static_cast<float>(child_count);
      }

      std::vector<float> extents(child_count, 0.0f);
      float remaining_extent = total_extent;
      float remaining_weight = total_weight;
      for (std::size_t i = 0; i < child_count; ++i) {
        const std::size_t remaining_children = child_count - i;
        extents[i] = remaining_children == 1
                         ? remaining_extent
                         : std::floor(remaining_weight > 0.0f
                                          ? remaining_extent * (weights[i] / remaining_weight)
                                          : remaining_extent /
                                                static_cast<float>(remaining_children));
        if (remaining_extent > kMinSplitPaneExtent * static_cast<float>(remaining_children)) {
          extents[i] = std::clamp(
              extents[i], kMinSplitPaneExtent,
              remaining_extent -
                  kMinSplitPaneExtent * static_cast<float>(remaining_children - 1));
        }
        remaining_extent = std::max(0.0f, remaining_extent - extents[i]);
        remaining_weight = std::max(0.0f, remaining_weight - weights[i]);
      }

      float before_extent = 0.0f;
      for (std::size_t i = 0; i < drag_editor_split_divider_index_; ++i) {
        before_extent += extents[i];
      }
      const float pair_extent =
          extents[drag_editor_split_divider_index_] + extents[drag_editor_split_divider_index_ + 1];
      const float min_extent =
          total_extent > kMinSplitPaneExtent * static_cast<float>(child_count) ? kMinSplitPaneExtent
                                                                                : 0.0f;
      float leading_extent =
          vertical ? static_cast<float>(event.motion.x) - node_rect->x - before_extent -
                         kEditorSplitDividerThickness *
                             static_cast<float>(drag_editor_split_divider_index_) -
                         kEditorSplitDividerThickness * 0.5f
                   : static_cast<float>(event.motion.y) - node_rect->y - before_extent -
                         kEditorSplitDividerThickness *
                             static_cast<float>(drag_editor_split_divider_index_) -
                         kEditorSplitDividerThickness * 0.5f;
      leading_extent =
          pair_extent <= min_extent * 2.0f
              ? std::clamp(leading_extent, 0.0f, pair_extent)
              : std::clamp(leading_extent, min_extent, pair_extent - min_extent);
      const float trailing_extent = std::max(0.0f, pair_extent - leading_extent);
      split_node->children[drag_editor_split_divider_index_]->size_fraction =
          leading_extent / total_extent;
      split_node->children[drag_editor_split_divider_index_ + 1]->size_fraction =
          trailing_extent / total_extent;
      NormalizeEditorSplitNode(*split_node);
      focus_ = FocusTarget::Editor;
      return true;
    }

    if (drag_target_ == DragTarget::SidebarScrollbar && sidebar_visible_) {
      if (sidebar_mode_ == SidebarMode::Search) {
        const float list_y = drag_layout.sidebar.y + kProjectSearchResultsTop;
        const auto line_map = BuildProjectSearchLineMap();
        const int visible_rows = std::max(
            1, static_cast<int>((drag_layout.sidebar.h - kProjectSearchResultsTop) / kSidebarRowHeight));
        const int max_scroll = std::max(0, static_cast<int>(line_map.size()) - visible_rows);
        const int scroll_row = std::clamp(sidebar_scroll_row_, 0, max_scroll);
        const auto scrollbar = MakeVerticalScrollbarGeometry(
            MakeRect(drag_layout.sidebar.x, list_y, drag_layout.sidebar.w,
                     std::max(0.0f, drag_layout.sidebar.y + drag_layout.sidebar.h - list_y)),
            static_cast<float>(line_map.size()), static_cast<float>(visible_rows),
            static_cast<float>(scroll_row));
        if (!scrollbar.has_value()) {
          drag_target_ = DragTarget::None;
          drag_scrollbar_offset_ = 0.0f;
          return false;
        }
        sidebar_scroll_row_ =
            std::clamp(static_cast<int>(std::lround(
                           ScrollUnitsForPointer(*scrollbar, static_cast<float>(event.motion.y),
                                                drag_scrollbar_offset_))),
                       0, max_scroll);
      } else if (sidebar_mode_ == SidebarMode::Git) {
        const auto lines = BuildGitSidebarLines();
        const float list_y = GitSidebarListTop(drag_layout.sidebar);
        const float visible_units = GitSidebarVisibleUnits(drag_layout.sidebar);
        const int max_scroll = std::max(
            0, static_cast<int>(std::ceil(static_cast<float>(lines.size()) - visible_units)));
        const int scroll_row = std::clamp(sidebar_scroll_row_, 0, max_scroll);
        const auto scrollbar = MakeVerticalScrollbarGeometry(
            MakeRect(drag_layout.sidebar.x, list_y, drag_layout.sidebar.w,
                     std::max(0.0f, drag_layout.sidebar.y + drag_layout.sidebar.h - list_y)),
            static_cast<float>(lines.size()), visible_units,
            static_cast<float>(scroll_row));
        if (!scrollbar.has_value()) {
          drag_target_ = DragTarget::None;
          drag_scrollbar_offset_ = 0.0f;
          return false;
        }
        sidebar_scroll_row_ =
            std::clamp(static_cast<int>(std::lround(
                           ScrollUnitsForPointer(*scrollbar, static_cast<float>(event.motion.y),
                                                drag_scrollbar_offset_))),
                       0, max_scroll);
      } else {
        const auto& entries = directory_tree_.entries();
        const float list_y = drag_layout.sidebar.y + kSidebarHeaderHeight + 6.0f;
        const int visible_rows =
            std::max(1, static_cast<int>((drag_layout.sidebar.h - 36.0f) / kSidebarRowHeight));
        const int max_scroll = std::max(0, static_cast<int>(entries.size()) - visible_rows);
        const int scroll_row = std::clamp(sidebar_scroll_row_, 0, max_scroll);
        const auto scrollbar = MakeVerticalScrollbarGeometry(
            MakeRect(drag_layout.sidebar.x, list_y, drag_layout.sidebar.w,
                     std::max(0.0f, drag_layout.sidebar.y + drag_layout.sidebar.h - list_y)),
            static_cast<float>(entries.size()), static_cast<float>(visible_rows),
            static_cast<float>(scroll_row));
        if (!scrollbar.has_value()) {
          drag_target_ = DragTarget::None;
          drag_scrollbar_offset_ = 0.0f;
          return false;
        }
        sidebar_scroll_row_ =
            std::clamp(static_cast<int>(std::lround(
                           ScrollUnitsForPointer(*scrollbar, static_cast<float>(event.motion.y),
                                                drag_scrollbar_offset_))),
                       0, max_scroll);
      }
      focus_ = FocusTarget::Sidebar;
      return true;
    }

    if (drag_target_ == DragTarget::BottomPanelScrollbar && BottomPanelVisible()) {
      const std::size_t line_count =
          ActiveTerminalTab() != nullptr ? ActiveTerminalTab()->session.SnapshotLines().size() : 0;
      const int visible_rows = BottomPanelVisibleRows(drag_layout.bottom_panel.h);
      const int max_scroll = std::max(0, static_cast<int>(line_count) - visible_rows);
      const int scroll_row = BottomPanelScrollRow(line_count, visible_rows);
      const auto scrollbar =
          MakeVerticalScrollbarGeometry(BottomPanelContentRect(drag_layout, command_mode_),
                                        static_cast<float>(line_count),
                                        static_cast<float>(visible_rows),
                                        static_cast<float>(scroll_row));
      if (!scrollbar.has_value()) {
        drag_target_ = DragTarget::None;
        drag_scrollbar_offset_ = 0.0f;
        return false;
      }
      SetBottomPanelScrollRow(
          std::clamp(static_cast<int>(std::lround(
                         ScrollUnitsForPointer(*scrollbar, static_cast<float>(event.motion.y),
                                              drag_scrollbar_offset_))),
                     0, max_scroll),
          line_count, visible_rows);
      if (ActiveTerminalTab() != nullptr) {
        focus_ = FocusTarget::Panel;
      }
      return true;
    }

    if (drag_target_ == DragTarget::OverlayScrollbar && overlay_visible_) {
      const SDL_FRect overlay = ComputeOverlayRect(drag_layout.editor_area);
      const float list_y = overlay.y + OverlayListStartOffset();
      const int visible_rows = OverlayVisibleRows(overlay);
      const int max_scroll =
          std::max(0, static_cast<int>(OverlayItemCount()) - visible_rows);
      const auto scrollbar = MakeVerticalScrollbarGeometry(
          MakeRect(overlay.x, list_y, overlay.w,
                   std::max(0.0f, overlay.y + overlay.h - list_y - 8.0f)),
          static_cast<float>(OverlayItemCount()), static_cast<float>(visible_rows),
          static_cast<float>(overlay_scroll_row_));
      if (!scrollbar.has_value()) {
        drag_target_ = DragTarget::None;
        drag_scrollbar_offset_ = 0.0f;
        return false;
      }
      overlay_scroll_row_ =
          std::clamp(static_cast<int>(std::lround(
                         ScrollUnitsForPointer(*scrollbar, static_cast<float>(event.motion.y),
                                              drag_scrollbar_offset_))),
                     0, max_scroll);
      focus_ = FocusTarget::Overlay;
      return true;
    }

    if ((drag_target_ == DragTarget::CompareVerticalScrollbar ||
         drag_target_ == DragTarget::CompareHorizontalScrollbar) &&
        (ActiveTabIsCompare() || ActiveTabIsMerge())) {
      if (ActiveTabIsCompare()) {
        CompareTabState* compare_tab = ActiveCompareTab();
        if (compare_tab == nullptr) {
          drag_target_ = DragTarget::None;
          drag_scrollbar_offset_ = 0.0f;
          return false;
        }
        const CompareSurfaceLayout surface_layout =
            ComputeCompareSurfaceLayout(drag_layout.editor_surface, *compare_tab);
        ClampCompareScrollRow(*compare_tab, surface_layout.visible_rows);
        ClampCompareHorizontalScroll(*compare_tab, surface_layout.visible_columns);
        if (drag_target_ == DragTarget::CompareVerticalScrollbar) {
          const auto scrollbar = MakeVerticalScrollbarGeometry(
              drag_layout.editor_surface, static_cast<float>(compare_tab->model.rows.size()),
              static_cast<float>(surface_layout.visible_rows),
              static_cast<float>(compare_tab->scroll_row), surface_layout.show_horizontal);
          if (!scrollbar.has_value()) {
            drag_target_ = DragTarget::None;
            drag_scrollbar_offset_ = 0.0f;
            return false;
          }
          const int target_scroll = std::clamp(
              static_cast<int>(std::lround(ScrollUnitsForPointer(
                  *scrollbar, static_cast<float>(event.motion.y), drag_scrollbar_offset_))),
              0, CompareMaxScrollRow(*compare_tab, surface_layout.visible_rows));
          compare_tab->scroll_row = target_scroll;
          focus_ = FocusTarget::Editor;
          return true;
        }
        const auto scrollbar = MakeHorizontalScrollbarGeometry(
            drag_layout.editor_surface, static_cast<float>(compare_tab->max_visual_columns),
            static_cast<float>(surface_layout.visible_columns),
            static_cast<float>(compare_tab->horizontal_scroll), surface_layout.show_vertical);
        if (!scrollbar.has_value()) {
          drag_target_ = DragTarget::None;
          drag_scrollbar_offset_ = 0.0f;
          return false;
        }
        compare_tab->horizontal_scroll = static_cast<std::size_t>(std::max(
            0L, std::lround(ScrollUnitsForPointer(*scrollbar, static_cast<float>(event.motion.x),
                                                  drag_scrollbar_offset_))));
        focus_ = FocusTarget::Editor;
        return true;
      }

      MergeTabState* merge_tab = ActiveMergeTab();
      if (merge_tab == nullptr) {
        drag_target_ = DragTarget::None;
        drag_scrollbar_offset_ = 0.0f;
        return false;
      }
      const MergeSurfaceLayout surface_layout =
          ComputeMergeSurfaceLayout(drag_layout.editor_surface, *merge_tab);
      ClampMergeScrollRow(*merge_tab, surface_layout.visible_rows);
      ClampMergeHorizontalScroll(*merge_tab, surface_layout.visible_columns);
      if (drag_target_ == DragTarget::CompareVerticalScrollbar) {
        const auto scrollbar = MakeVerticalScrollbarGeometry(
            drag_layout.editor_surface, static_cast<float>(merge_tab->display_model.rows.size()),
            static_cast<float>(surface_layout.visible_rows), static_cast<float>(merge_tab->scroll_row),
            surface_layout.show_horizontal);
        if (!scrollbar.has_value()) {
          drag_target_ = DragTarget::None;
          drag_scrollbar_offset_ = 0.0f;
          return false;
        }
        const int target_scroll = std::clamp(
            static_cast<int>(std::lround(ScrollUnitsForPointer(
                *scrollbar, static_cast<float>(event.motion.y), drag_scrollbar_offset_))),
            0, MergeMaxScrollRow(*merge_tab, surface_layout.visible_rows));
        merge_tab->scroll_row = target_scroll;
        focus_ = FocusTarget::Editor;
        return true;
      }
      const auto scrollbar = MakeHorizontalScrollbarGeometry(
          drag_layout.editor_surface, static_cast<float>(merge_tab->max_visual_columns),
          static_cast<float>(surface_layout.visible_columns),
          static_cast<float>(merge_tab->horizontal_scroll), surface_layout.show_vertical);
      if (!scrollbar.has_value()) {
        drag_target_ = DragTarget::None;
        drag_scrollbar_offset_ = 0.0f;
        return false;
      }
      merge_tab->horizontal_scroll = static_cast<std::size_t>(std::max(
          0L, std::lround(ScrollUnitsForPointer(*scrollbar, static_cast<float>(event.motion.x),
                                                drag_scrollbar_offset_))));
      focus_ = FocusTarget::Editor;
      return true;
    }

    if (drag_target_ == DragTarget::EditorVerticalScrollbar ||
        drag_target_ == DragTarget::EditorHorizontalScrollbar) {
      const auto panes = ComputeEditorPaneLayouts(drag_layout.editor_surface);
      const auto active_pane = std::find_if(
          panes.begin(), panes.end(), [](const EditorPaneLayout& pane) { return pane.active; });
      const SDL_FRect editor_rect =
          active_pane != panes.end() ? active_pane->rect : drag_layout.editor_surface;
      const editor::EditorViewMetrics metrics = editor::EditorViewRenderer::ComputeMetrics(
          text_renderer_, text_viewport_, editor_rect);
      text_viewport_.SetViewportSize(metrics.visible_rows, metrics.visible_columns);
      const std::size_t total_columns =
          std::max<std::size_t>(text_viewport_.visible_columns(), MaxVisualColumns(text_viewport_));
      const bool show_vertical = text_viewport_.line_count() > text_viewport_.visible_lines();
      const bool show_horizontal = total_columns > text_viewport_.visible_columns();

      if (drag_target_ == DragTarget::EditorVerticalScrollbar) {
        const auto scrollbar = MakeVerticalScrollbarGeometry(
            editor_rect, static_cast<float>(text_viewport_.line_count()),
            static_cast<float>(text_viewport_.visible_lines()),
            static_cast<float>(text_viewport_.scroll_line()), show_horizontal);
        if (!scrollbar.has_value()) {
          drag_target_ = DragTarget::None;
          drag_scrollbar_offset_ = 0.0f;
          return false;
        }
        text_viewport_.SetScrollLine(static_cast<std::size_t>(std::max(
            0L, std::lround(ScrollUnitsForPointer(*scrollbar, static_cast<float>(event.motion.y),
                                                  drag_scrollbar_offset_)))));
      } else {
        const auto scrollbar = MakeHorizontalScrollbarGeometry(
            editor_rect, static_cast<float>(total_columns),
            static_cast<float>(text_viewport_.visible_columns()),
            static_cast<float>(text_viewport_.horizontal_scroll()), show_vertical);
        if (!scrollbar.has_value()) {
          drag_target_ = DragTarget::None;
          drag_scrollbar_offset_ = 0.0f;
          return false;
        }
        text_viewport_.SetHorizontalScroll(static_cast<std::size_t>(std::max(
            0L, std::lround(ScrollUnitsForPointer(*scrollbar, static_cast<float>(event.motion.x),
                                                  drag_scrollbar_offset_)))));
      }
      focus_ = FocusTarget::Editor;
      return true;
    }

    drag_target_ = DragTarget::None;
    drag_scrollbar_offset_ = 0.0f;
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
          focus_ = FocusTarget::Panel;
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
                                                 sidebar_visible_, BottomPanelVisible(),
                                                 sidebar_width_, bottom_panel_height_);
    if (!Contains(layout.bottom_panel, event.motion.x, event.motion.y)) {
      return false;
    }

    const auto terminal_lines = terminal_tab->session.SnapshotLines();
    if (const auto position =
            TerminalSelectionPositionForPoint(event.motion.x, event.motion.y, terminal_lines);
        position.has_value()) {
      terminal_tab->selection_head = *position;
      focus_ = FocusTarget::Panel;
      return true;
    }
  }

  if (!mouse_selecting_ || (event.motion.state & SDL_BUTTON_LMASK) == 0) {
    return false;
  }

  const WorkspaceLayout layout =
      ComputeLayout(static_cast<float>(last_window_width_), static_cast<float>(last_window_height_),
                    sidebar_visible_, BottomPanelVisible(), sidebar_width_, bottom_panel_height_);
  if (!Contains(layout.editor_surface, event.motion.x, event.motion.y)) {
    return false;
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
  focus_ = FocusTarget::Editor;
  return true;
}

bool WorkspaceShell::HandleMouseWheel(const SDL_Event& event) {
  if (dirty_prompt_visible_) {
    return true;
  }
  if (prompt_surface_visible_) {
    return true;
  }

  if (menu_bar_open_ || tree_context_menu_.open) {
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
                    sidebar_visible_, BottomPanelVisible(), sidebar_width_, bottom_panel_height_);

  if (overlay_visible_) {
    const int overlay_ticks = vertical_ticks != 0 ? vertical_ticks : horizontal_ticks;
    if (overlay_mode_ == OverlayMode::CommitPicker) {
      MoveComparePickerSelection(-overlay_ticks);
    } else if (overlay_mode_ == OverlayMode::BufferSearch || overlay_mode_ == OverlayMode::BufferReplace) {
      MoveBufferSearchSelection(-overlay_ticks);
    } else if (overlay_mode_ == OverlayMode::ProjectSearch) {
      MoveProjectSearchSelection(-overlay_ticks);
    } else {
      MoveFileFinderSelection(-overlay_ticks);
    }
    return true;
  }

  if (Contains(layout.project_tab_strip, event.wheel.mouse_x, event.wheel.mouse_y) &&
      !projects_.empty()) {
    const int max_scroll = std::max(0, static_cast<int>(projects_.size()) - 1);
    project_tab_scroll_index_ =
        std::clamp(project_tab_scroll_index_ - vertical_ticks, 0, max_scroll);
    return true;
  }

  if (Contains(layout.tab_strip, event.wheel.mouse_x, event.wheel.mouse_y) && !open_tabs_.empty()) {
    const int max_scroll = std::max(0, static_cast<int>(open_tabs_.size()) - 1);
    tab_scroll_index_ = std::clamp(tab_scroll_index_ - vertical_ticks, 0, max_scroll);
    return true;
  }

  if (sidebar_visible_ && Contains(layout.sidebar, event.wheel.mouse_x, event.wheel.mouse_y)) {
    int visible_rows = 1;
    int max_scroll = 0;
    if (sidebar_mode_ == SidebarMode::Search) {
      const auto line_map = BuildProjectSearchLineMap();
      visible_rows =
          std::max(1, static_cast<int>((layout.sidebar.h - kProjectSearchResultsTop) / 20.0f));
      max_scroll = std::max(0, static_cast<int>(line_map.size()) - visible_rows);
    } else if (sidebar_mode_ == SidebarMode::Git) {
      const auto lines = BuildGitSidebarLines();
      const float visible_units = GitSidebarVisibleUnits(layout.sidebar);
      visible_rows = std::max(1, static_cast<int>(std::floor(visible_units)));
      max_scroll = std::max(
          0, static_cast<int>(std::ceil(static_cast<float>(lines.size()) - visible_units)));
    } else {
      const auto& entries = directory_tree_.entries();
      visible_rows = std::max(1, static_cast<int>((layout.sidebar.h - 36.0f) / 20.0f));
      max_scroll = std::max(0, static_cast<int>(entries.size()) - visible_rows);
    }
    sidebar_scroll_row_ = std::clamp(sidebar_scroll_row_ - vertical_ticks, 0, max_scroll);
    focus_ = FocusTarget::Sidebar;
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
        focus_ = FocusTarget::Panel;
        return true;
      }
    }
  }

  if (BottomPanelVisible() && Contains(layout.bottom_panel, event.wheel.mouse_x, event.wheel.mouse_y)) {
    const std::size_t line_count =
        ActiveTerminalTab() != nullptr ? ActiveTerminalTab()->session.SnapshotLines().size() : 0;
    const int visible_rows = BottomPanelVisibleRows(layout.bottom_panel.h);
    const int max_scroll = std::max(0, static_cast<int>(line_count) - visible_rows);
    const int scroll_row = BottomPanelScrollRow(line_count, visible_rows);
    SetBottomPanelScrollRow(std::clamp(scroll_row - vertical_ticks, 0, max_scroll), line_count,
                            visible_rows);
    if (ActiveTerminalTab() != nullptr) {
      focus_ = FocusTarget::Panel;
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
      focus_ = FocusTarget::Editor;
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
          merge_tab->scroll_row =
              std::clamp(merge_tab->scroll_row - vertical_ticks * 3, 0,
                         MergeMaxScrollRow(*merge_tab, surface_layout.visible_rows));
        }
      }
      focus_ = FocusTarget::Editor;
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
    focus_ = FocusTarget::Editor;
    return true;
  }

  return false;
}


}  // namespace microide::workspace
