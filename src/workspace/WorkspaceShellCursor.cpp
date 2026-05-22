#include "workspace/WorkspaceShell.h"

#include <algorithm>
#include <cctype>
#include <string_view>

#include "util/PerformanceTrace.h"
#include "util/Parse.h"
#include "workspace/SettingsOverlayService.h"
#include "workspace/WorkspaceLayout.h"

namespace microide::workspace {

namespace {

constexpr float kWindowFrameHitThickness = 6.0f;

bool IsNavigableOutputLine(std::string_view text) {
  const std::size_t column_delimiter = text.rfind(':');
  if (column_delimiter == std::string_view::npos || column_delimiter == 0) {
    return false;
  }
  const std::size_t line_delimiter = text.rfind(':', column_delimiter - 1);
  if (line_delimiter == std::string_view::npos || line_delimiter == 0) {
    return false;
  }

  const std::string_view path_text = text.substr(0, line_delimiter);
  const std::string_view line_text =
      text.substr(line_delimiter + 1, column_delimiter - line_delimiter - 1);
  const std::string_view column_text = text.substr(column_delimiter + 1);
  const auto line = util::ParseSize(line_text);
  const auto column = util::ParseSize(column_text);
  return !path_text.empty() && line.has_value() && *line > 0 && column.has_value();
}

SDL_HitTestResult ResizeHitTestResult(bool left, bool right, bool top, bool bottom) {
  if (top && left) {
    return SDL_HITTEST_RESIZE_TOPLEFT;
  }
  if (top && right) {
    return SDL_HITTEST_RESIZE_TOPRIGHT;
  }
  if (bottom && left) {
    return SDL_HITTEST_RESIZE_BOTTOMLEFT;
  }
  if (bottom && right) {
    return SDL_HITTEST_RESIZE_BOTTOMRIGHT;
  }
  if (top) {
    return SDL_HITTEST_RESIZE_TOP;
  }
  if (bottom) {
    return SDL_HITTEST_RESIZE_BOTTOM;
  }
  if (left) {
    return SDL_HITTEST_RESIZE_LEFT;
  }
  if (right) {
    return SDL_HITTEST_RESIZE_RIGHT;
  }
  return SDL_HITTEST_NORMAL;
}

}  // namespace

SDL_HitTestResult WorkspaceShell::WindowHitTest(float x, float y) const {
  if (!CurrentWindowChromeState().custom_enabled) {
    return SDL_HITTEST_NORMAL;
  }

  const auto window_rect = CurrentWindowRect();
  if (!window_rect.has_value()) {
    return SDL_HITTEST_NORMAL;
  }
  const float window_width = window_rect->w;
  const float window_height = window_rect->h;
  if (x < 0.0f || y < 0.0f || x >= window_width || y >= window_height) {
    return SDL_HITTEST_NORMAL;
  }

  if (CurrentWindowChromeState().ResizableFrameEnabled()) {
    const bool left = x < kWindowFrameHitThickness;
    const bool right = x >= window_width - kWindowFrameHitThickness;
    const bool top = y < kWindowFrameHitThickness;
    const bool bottom = y >= window_height - kWindowFrameHitThickness;
    if (left || right || top || bottom) {
      return ResizeHitTestResult(left, right, top, bottom);
    }
  }

  if (WindowDragRegionContains(x, y)) {
    return SDL_HITTEST_DRAGGABLE;
  }

  return SDL_HITTEST_NORMAL;
}

bool WorkspaceShell::WindowDragRegionContains(float x, float y) const {
  if (!CurrentWindowChromeState().custom_enabled || CurrentWindowChromeState().fullscreen) {
    return false;
  }

  const auto window_rect = CurrentWindowRect();
  if (!window_rect.has_value()) {
    return false;
  }
  const float window_width = window_rect->w;
  const float window_height = window_rect->h;
  if (x < 0.0f || y < 0.0f || x >= window_width || y >= window_height) {
    return false;
  }

  if (MenuSurfaceCapturingMouse()) {
    return false;
  }

  const WorkspaceLayout layout =
      ComputeLayout(window_width, window_height, context_.current_project_state.sidebar.visible, BottomPanelVisible(),
                    context_.current_project_state.sidebar.width, context_.current_project_state.panel.height,
                    layout_mode_service_.SnapshotInputs(),
                    layout_mode_service_.StatusBarVisible());
  if (!Contains(layout.menu_bar, x, y)) {
    return false;
  }

  for (const VisibleMenuBarItem& item : ComputeVisibleMenuBarItems(layout.menu_bar)) {
    if (Contains(item.rect, x, y)) {
      return false;
    }
  }
  if (const auto chevron_rect = MenuOverflowChevronRect(layout.menu_bar);
      chevron_rect.has_value() && Contains(*chevron_rect, x, y)) {
    return false;
  }
  for (const VisibleWindowControlButton& button :
       ComputeVisibleWindowControlButtons(layout.menu_bar)) {
    if (Contains(button.rect, x, y)) {
      return false;
    }
  }

  return true;
}

WorkspaceShell::WindowAction WorkspaceShell::ConsumeWindowAction() {
  const WindowAction action = pending_window_action_;
  pending_window_action_ = WindowAction::None;
  return action;
}

WorkspaceShell::CursorKind WorkspaceShell::CursorKindForPosition(float x, float y) const {
  switch (context_.interaction_state.drag_target) {
    case DragTarget::SidebarDivider:
      return CursorKind::EwResize;
    case DragTarget::BottomPanelDivider:
      return CursorKind::NsResize;
    case DragTarget::EditorSplitDivider: {
      const auto* editor_tab = ActiveEditorTab();
      const auto* split_node = editor_tab != nullptr
                                   ? FindEditorSplitNode(editor_tab->split_root.get(),
                                                         context_.interaction_state.drag_editor_split_path)
                                   : nullptr;
      return split_node != nullptr &&
                     split_node->orientation == EditorSplitOrientation::Horizontal
                 ? CursorKind::NsResize
                 : CursorKind::EwResize;
    }
    default:
      break;
  }

  const auto window_rect = CurrentWindowRect();
  if (!window_rect.has_value()) {
    return CursorKind::Default;
  }

  if (context_.prompts.dirty_visible) {
    const auto buttons = ComputeDirtyPromptButtonRects(ComputeDirtyPromptRect(*window_rect));
    for (const SDL_FRect& button : buttons) {
      if (Contains(button, x, y)) {
        return CursorKind::Pointer;
      }
    }
    return CursorKind::Default;
  }

  if (context_.prompts.surface_visible) {
    const SDL_FRect dialog = ComputePromptSurfaceRect(*window_rect);
    for (const SDL_FRect& button :
         ComputePromptSurfaceButtonRects(dialog, context_.prompts.surface.button_count)) {
      if (Contains(button, x, y)) {
        return CursorKind::Pointer;
      }
    }
    if (context_.prompts.surface.kind == PromptSurfaceState::Kind::TextInput &&
        Contains(ComputePromptSurfaceInputRect(dialog), x, y)) {
      return CursorKind::Text;
    }
    return CursorKind::Default;
  }

  const auto layout_state = CurrentWorkspaceLayout();
  if (!layout_state.has_value()) {
    return CursorKind::Default;
  }
  const WorkspaceLayout layout = *layout_state;

  if (settings_overlay_service_.Visible()) {
    const SDL_FRect settings_rect = ComputeOverlaySurfaceRect(layout.editor_area);
    if (!Contains(settings_rect, x, y)) {
      return CursorKind::Default;
    }
    constexpr float kRowHeight = 24.0f;
    float list_top = settings_rect.y + 42.0f;
    if (settings_overlay_service_.Mode() == SettingsOverlayMode::Settings) {
      list_top += 16.0f;
    }
    const float list_bottom = settings_rect.y + settings_rect.h - 10.0f;
    if (y >= list_top && y <= list_bottom) {
      const std::size_t row =
          static_cast<std::size_t>(std::max(0.0f, y - list_top) / kRowHeight) +
          static_cast<std::size_t>(settings_overlay_service_.ScrollRow());
      if (settings_overlay_service_.Mode() == SettingsOverlayMode::Settings &&
          row < settings_overlay_service_.SettingsRows().size()) {
        return CursorKind::Pointer;
      }
      if (settings_overlay_service_.Mode() == SettingsOverlayMode::HelpAbout &&
          row < settings_overlay_service_.HelpRows().size()) {
        return CursorKind::Pointer;
      }
    }
    return CursorKind::Default;
  }

  // Hit-test the popup using geometry only. The full ComputeVisiblePopupMenuItems
  // path here was calling IsMenuItemEnabled/IsMenuItemChecked for every row, and
  // CursorKindForPosition runs on every mouse motion — that cost roughly half of
  // the per-motion handler budget while a menu was open.
  if (context_.menu_state.tree_context_menu.open) {
    if (const auto popup_rect = ComputeTreeContextMenuRect(); popup_rect.has_value()) {
      const auto items = TreeContextMenuItems(context_.menu_state.tree_context_menu.target);
      if (const auto hit = HitTestPopupRow(items, *popup_rect, x, y); hit.has_value()) {
        return hit->separator ? CursorKind::Default : CursorKind::Pointer;
      }
      if (Contains(*popup_rect, x, y)) {
        return CursorKind::Default;
      }
    }
  }

  if (context_.menu_state.menu_bar_open) {
    if (const auto popup_rect = ActiveSubmenuRect(layout.menu_bar); popup_rect.has_value()) {
      const auto items = MenuItems(context_.menu_state.active_submenu_id);
      if (const auto hit = HitTestPopupRow(items, *popup_rect, x, y); hit.has_value()) {
        return hit->separator ? CursorKind::Default : CursorKind::Pointer;
      }
      if (Contains(*popup_rect, x, y)) {
        return CursorKind::Default;
      }
    }
    if (const auto popup_rect =
            ComputePopupMenuRect(layout.menu_bar, context_.menu_state.active_menu_id);
        popup_rect.has_value()) {
      const auto items = MenuItems(context_.menu_state.active_menu_id);
      if (const auto hit = HitTestPopupRow(items, *popup_rect, x, y); hit.has_value()) {
        return hit->separator ? CursorKind::Default : CursorKind::Pointer;
      }
      if (Contains(*popup_rect, x, y)) {
        return CursorKind::Default;
      }
    }
  }

  if (context_.menu_state.overflow_popup_open &&
      context_.menu_state.overflow_popup_anchor_rect.has_value()) {
    const auto overflow = ComputeOverflowMenuBarItems(layout.menu_bar);
    const SDL_FRect popup = ComputeMenuOverflowPopupRect(
        *context_.menu_state.overflow_popup_anchor_rect, overflow.size());
    if (Contains(popup, x, y)) {
      return CursorKind::Pointer;
    }
  }

  if (Contains(layout.menu_bar, x, y)) {
    for (const VisibleMenuBarItem& item : ComputeVisibleMenuBarItems(layout.menu_bar)) {
      if (Contains(item.rect, x, y)) {
        return CursorKind::Pointer;
      }
    }
    if (const auto chevron_rect = MenuOverflowChevronRect(layout.menu_bar);
        chevron_rect.has_value() && Contains(*chevron_rect, x, y)) {
      return CursorKind::Pointer;
    }
    for (const VisibleWindowControlButton& button :
         ComputeVisibleWindowControlButtons(layout.menu_bar)) {
      if (Contains(WindowControlButtonHitRect(button.rect), x, y)) {
        return CursorKind::Pointer;
      }
    }
    return CursorKind::Default;
  }

  if (context_.current_project_state.overlay.visible) {
    const SDL_FRect overlay = ComputeOverlayRect(layout.editor_area);
    if (!Contains(overlay, x, y)) {
      return CursorKind::Default;
    }

    const auto overlay_list_layout = ComputeOverlayListLayout(overlay);
    if (overlay_list_layout.scrollbar.has_value() &&
        Contains(overlay_list_layout.scrollbar->track, x, y)) {
      return CursorKind::Default;
    }
    if (const auto item_index = ScrollableListIndexAtY(overlay_list_layout, y);
        item_index.has_value() && *item_index >= 0 &&
        *item_index < static_cast<int>(OverlayItemCount())) {
      return CursorKind::Pointer;
    }

    if (context_.current_project_state.overlay.mode == OverlayMode::BufferReplace) {
      return y >= overlay.y + 40.0f && y < overlay.y + 82.0f ? CursorKind::Text
                                                              : CursorKind::Default;
    }
    if (context_.current_project_state.overlay.mode == OverlayMode::CommitPicker) {
      return y >= overlay.y + 58.0f && y < overlay.y + 78.0f ? CursorKind::Text
                                                              : CursorKind::Default;
    }
    if (context_.current_project_state.overlay.mode == OverlayMode::Completion ||
        context_.current_project_state.overlay.mode == OverlayMode::CodeActions) {
      return CursorKind::Default;
    }
    return y >= overlay.y + 40.0f && y < overlay.y + 60.0f ? CursorKind::Text
                                                            : CursorKind::Default;
  }

  if (context_.current_project_state.sidebar.visible &&
      Contains(SidebarResizeCursorRect(layout), x, y)) {
    return CursorKind::EwResize;
  }
  if (BottomPanelVisible() && Contains(BottomPanelResizeCursorRect(layout), x, y)) {
    return CursorKind::NsResize;
  }

  if (Contains(layout.project_tab_strip, x, y)) {
    const auto project_tabs = tab_strip_chrome_.ComputeVisibleProjectTabs(layout.project_tab_strip);
    const auto project_overflow =
        tab_strip_chrome_.ComputeProjectTabOverflowControls(layout.project_tab_strip, project_tabs);
    if ((project_overflow.hidden_left > 0 &&
         Contains(project_overflow.left_button, x, y)) ||
        (project_overflow.hidden_right > 0 &&
         Contains(project_overflow.right_button, x, y))) {
      return CursorKind::Pointer;
    }
    for (const VisibleStripTab& tab : project_tabs) {
      if (Contains(tab.rect, x, y)) {
        return CursorKind::Pointer;
      }
    }
    return CursorKind::Default;
  }

  if (Contains(layout.tab_strip, x, y)) {
    if (context_.current_project_state.root.empty()) {
      return CursorKind::Default;
    }
    if (context_.current_project_state.open_tabs.empty()) {
      return Contains(MakeRect(layout.tab_strip.x, layout.tab_strip.y + 2.0f, 220.0f,
                               std::max(22.0f, layout.tab_strip.h - 2.0f)),
                      x, y)
                 ? CursorKind::Pointer
                 : CursorKind::Default;
    }
    const auto tabs = tab_strip_chrome_.ComputeVisibleTabs(layout.tab_strip);
    const auto tab_overflow = tab_strip_chrome_.ComputeTabOverflowControls(layout.tab_strip, tabs);
    if ((tab_overflow.hidden_left > 0 && Contains(tab_overflow.left_button, x, y)) ||
        (tab_overflow.hidden_right > 0 && Contains(tab_overflow.right_button, x, y))) {
      return CursorKind::Pointer;
    }
    for (const VisibleStripTab& tab : tabs) {
      if (Contains(tab.rect, x, y)) {
        return CursorKind::Pointer;
      }
    }
    return CursorKind::Default;
  }

  if (context_.current_project_state.sidebar.visible && Contains(layout.sidebar, x, y)) {
    const SidebarMode sidebar_mode = ActiveSidebarMode();
    if (Contains(SidebarModeControlRect(layout.sidebar), x, y)) {
      return CursorKind::Pointer;
    }
    if (sidebar_mode == SidebarMode::Search) {
      if (Contains(ProjectSearchQueryRect(layout.sidebar), x, y) ||
          Contains(ProjectSearchReplaceRect(layout.sidebar), x, y)) {
        return CursorKind::Text;
      }
      if (Contains(ProjectSearchModeButtonRect(layout.sidebar), x, y) ||
          Contains(ProjectSearchCaseButtonRect(layout.sidebar), x, y) ||
          Contains(ProjectSearchHiddenButtonRect(layout.sidebar), x, y)) {
        return CursorKind::Pointer;
      }

      const auto line_map = BuildProjectSearchLineMap();
      const auto list_layout =
          ComputeProjectSearchSidebarListLayout(layout.sidebar, line_map.size());
      if (const auto line_index = ScrollableListIndexAtY(list_layout, y);
          line_index.has_value() && *line_index >= 0 &&
          *line_index < static_cast<int>(line_map.size()) &&
          line_map[static_cast<std::size_t>(*line_index)] >= 0) {
        const SDL_FRect row_rect =
            ScrollableListRowRect(list_layout, *line_index - list_layout.scroll_row);
        return Contains(row_rect, x, y) ? CursorKind::Pointer : CursorKind::Default;
      }
      return CursorKind::Default;
    }
    if (sidebar_mode == SidebarMode::Git) {
      if (Contains(GitSidebarStageAllButtonRect(layout.sidebar), x, y) &&
          CanStageAllGitSidebarEntries()) {
        return CursorKind::Pointer;
      }
      if (Contains(GitSidebarDiscardAllButtonRect(layout.sidebar), x, y) &&
          CanDiscardAllGitSidebarEntries()) {
        return CursorKind::Pointer;
      }
      if (Contains(GitSidebarRefreshButtonRect(layout.sidebar), x, y)) {
        return CursorKind::Pointer;
      }
      if (const auto button_rect = GitSidebarOutgoingBaseButtonRect(layout.sidebar);
          button_rect.has_value() && Contains(*button_rect, x, y)) {
        return CursorKind::Pointer;
      }
      const auto lines = BuildGitSidebarLines();
      const auto list_layout = ComputeGitSidebarListLayout(layout.sidebar, lines.size());
      const auto line_index = ScrollableListIndexAtY(list_layout, y);
      if (!line_index.has_value() || *line_index < 0 ||
          *line_index >= static_cast<int>(lines.size())) {
        return CursorKind::Default;
      }
      const SDL_FRect row_rect =
          ScrollableListRowRect(list_layout, *line_index - list_layout.scroll_row);
      if (!Contains(row_rect, x, y)) {
        return CursorKind::Default;
      }
      const auto& line = lines[static_cast<std::size_t>(*line_index)];
      if (line.kind != GitSidebarLine::Kind::Entry || line.entry_index < 0) {
        return CursorKind::Default;
      }
      const auto& entry = context_.current_project_state.sidebar.git.entries[static_cast<std::size_t>(line.entry_index)];
      const GitSidebarEntryActionLayout actions =
          ComputeGitSidebarEntryActionLayout(row_rect, entry);
      if ((actions.primary_rect.has_value() && Contains(*actions.primary_rect, x, y)) ||
          (actions.discard_rect.has_value() && Contains(*actions.discard_rect, x, y))) {
        return CursorKind::Pointer;
      }
      return CursorKind::Pointer;
    }
    if (sidebar_mode == SidebarMode::Problems) {
      const auto list_layout =
          ComputeProblemsSidebarListLayout(layout.sidebar, context_.current_project_state.sidebar.problems.entries.size());
      const auto line_index = ScrollableListIndexAtY(list_layout, y);
      if (!line_index.has_value() || *line_index < 0 ||
          *line_index >= static_cast<int>(context_.current_project_state.sidebar.problems.entries.size())) {
        return CursorKind::Default;
      }
      const SDL_FRect row_rect =
          ScrollableListRowRect(list_layout, *line_index - list_layout.scroll_row);
      return Contains(row_rect, x, y) ? CursorKind::Pointer : CursorKind::Default;
    }
    if (sidebar_mode == SidebarMode::Tests) {
      const auto list_layout =
          ComputeTestsSidebarListLayout(layout.sidebar, context_.current_project_state.sidebar.tests.entries.size());
      const auto line_index = ScrollableListIndexAtY(list_layout, y);
      if (!line_index.has_value() || *line_index < 0 ||
          *line_index >= static_cast<int>(context_.current_project_state.sidebar.tests.entries.size())) {
        return CursorKind::Default;
      }
      const SDL_FRect row_rect =
          ScrollableListRowRect(list_layout, *line_index - list_layout.scroll_row);
      return Contains(row_rect, x, y) ? CursorKind::Pointer : CursorKind::Default;
    }
    if (sidebar_mode == SidebarMode::Plugin) {
      const auto list_layout =
          ComputePluginSidebarListLayout(layout.sidebar, context_.current_project_state.sidebar.plugin.items.size());
      const auto line_index = ScrollableListIndexAtY(list_layout, y);
      if (!line_index.has_value() || *line_index < 0 ||
          *line_index >= static_cast<int>(context_.current_project_state.sidebar.plugin.items.size())) {
        return CursorKind::Default;
      }
      const SDL_FRect row_rect =
          ScrollableListRowRect(list_layout, *line_index - list_layout.scroll_row);
      return Contains(row_rect, x, y) ? CursorKind::Pointer : CursorKind::Default;
    }

    if (Contains(TreeSidebarCollapseButtonRect(layout.sidebar), x, y) &&
        context_.current_project_state.directory_tree.CanCollapseAll()) {
      return CursorKind::Pointer;
    }

    if (Contains(TreeSidebarRefreshButtonRect(layout.sidebar), x, y)) {
      return CursorKind::Pointer;
    }

    const auto& entries = context_.current_project_state.directory_tree.entries();
    const auto list_layout = ComputeTreeSidebarListLayout(layout.sidebar, entries.size());
    const auto entry_index = ScrollableListIndexAtY(list_layout, y);
    if (entry_index.has_value() && *entry_index >= 0 &&
        *entry_index < static_cast<int>(entries.size())) {
      const SDL_FRect row_rect =
          ScrollableListRowRect(list_layout, *entry_index - list_layout.scroll_row);
      return Contains(row_rect, x, y) ? CursorKind::Pointer : CursorKind::Default;
    }
    return CursorKind::Default;
  }

  if (BottomPanelVisible() && Contains(layout.bottom_panel, x, y)) {
    const SDL_FRect panel_header =
        MakeRect(layout.bottom_panel.x, layout.bottom_panel.y, layout.bottom_panel.w,
                 kWorkspaceBottomPanelHeaderHeight);
    if (Contains(panel_header, x, y)) {
      if (Contains(tab_strip_service_.BottomPanelTerminalNewTabRect(
                       layout_mode_service_.CurrentMode(), panel_header),
                   x, y)) {
        return CursorKind::Pointer;
      }
      const std::vector<VisibleStripTab> visible_panel_tabs =
          tab_strip_service_.ComputeVisibleBottomPanelTabs(
              context_.current_project_state, panel_header, layout_mode_service_.CurrentMode(),
              [this](std::string_view text) { return text_renderer_.MeasureWidth(text); },
              output_channels_.Channels());
      for (const VisibleStripTab& tab : visible_panel_tabs) {
        if (Contains(tab.rect, x, y)) {
          return CursorKind::Pointer;
        }
      }
      return CursorKind::Default;
    }
    const std::size_t line_count =
        BottomPanelShowsTerminal() && ActiveTerminalTab() != nullptr
            ? ActiveTerminalTab()->session.LineCount()
            : BottomPanelShowsOutput()
                ? (OutputChannelEntries(context_.current_project_state.panel.output.channel_id) != nullptr
                       ? OutputChannelEntries(context_.current_project_state.panel.output.channel_id)->size()
                       : 0)
                : 0;
    if (line_count > 0) {
      const auto panel_layout = ComputeBottomPanelLogLayout(layout, line_count);
      if (panel_layout.scroll.vertical_scrollbar.has_value() &&
          Contains(panel_layout.scroll.vertical_scrollbar->track, x, y)) {
        return CursorKind::Default;
      }
      if (BottomPanelShowsOutput() && Contains(panel_layout.content_rect, x, y) &&
          y >= panel_layout.text_y && panel_layout.line_height > 0.0f) {
        const int row = static_cast<int>((y - panel_layout.text_y) / panel_layout.line_height);
        if (row >= 0 && row < panel_layout.scroll.visible_rows) {
          const int absolute_index = panel_layout.scroll.vertical_scroll + row;
          if (absolute_index >= 0) {
            if (const auto* entries =
                    OutputChannelEntries(context_.current_project_state.panel.output.channel_id);
                entries != nullptr &&
                static_cast<std::size_t>(absolute_index) < entries->size() &&
                IsNavigableOutputLine((*entries)[static_cast<std::size_t>(absolute_index)])) {
              return CursorKind::Pointer;
            }
          }
        }
      }
    }
    if (context_.current_project_state.panel.command_mode &&
        Contains(BottomPanelCommandPromptRect(layout), x, y)) {
      return CursorKind::Text;
    }
    if (BottomPanelShowsTerminal() && ActiveTerminalTab() != nullptr &&
        y >= layout.bottom_panel.y + kWorkspaceBottomPanelHeaderHeight) {
      if (TerminalUrlAtPoint(x, y).has_value()) {
        return CursorKind::Pointer;
      }
      return CursorKind::Text;
    }
    return CursorKind::Default;
  }

  if (const auto popup = ActiveEditorHoverPopupLayout(); popup.has_value()) {
    if (popup->primary_action_rect.has_value() &&
        Contains(EditorHoverPopupPrimaryActionHitRect(*popup), x, y)) {
      return CursorKind::Pointer;
    }
    if (Contains(popup->rect, x, y)) {
      return CursorKind::Default;
    }
  }

  if (!Contains(layout.editor_surface, x, y)) {
    return CursorKind::Default;
  }

  if (ActiveTabIsCompare()) {
    const CompareTabState* compare_tab = ActiveCompareTab();
    if (compare_tab == nullptr) {
      return CursorKind::Default;
    }
    const CompareSurfaceLayout surface_layout =
        ComputeCompareSurfaceLayout(layout.editor_surface, *compare_tab);
    const auto scroll_layout =
        ComputeCompareScrollLayout(layout.editor_surface, surface_layout, *compare_tab);
    if (scroll_layout.vertical_scrollbar.has_value() &&
        Contains(scroll_layout.vertical_scrollbar->track, x, y)) {
      return CursorKind::Default;
    }
    if (scroll_layout.horizontal_scrollbar.has_value() &&
        Contains(scroll_layout.horizontal_scrollbar->track, x, y)) {
      return CursorKind::Default;
    }
    const SDL_FRect divider_rect =
        CompareDividerHitRect(layout.editor_surface, surface_layout);
    if (Contains(divider_rect, x, y)) {
      return CursorKind::EwResize;
    }
    if (compare_tab->right_editable && x >= surface_layout.right_x) {
      return CursorKind::Text;
    }
    return CursorKind::Pointer;
  }
  if (ActiveTabIsMerge()) {
    const MergeTabState* merge_tab = ActiveMergeTab();
    if (merge_tab == nullptr) {
      return CursorKind::Default;
    }
    const MergeSurfaceLayout surface_layout =
        ComputeMergeSurfaceLayout(layout.editor_surface, *merge_tab);
    const auto scroll_layout =
        ComputeMergeScrollLayout(layout.editor_surface, surface_layout, *merge_tab);
    const SDL_FRect left_divider_rect =
        MakeRect(surface_layout.center_x - surface_layout.divider_width, layout.editor_surface.y,
                 surface_layout.divider_width, layout.editor_surface.h);
    const SDL_FRect right_divider_rect =
        MakeRect(surface_layout.right_x - surface_layout.divider_width, layout.editor_surface.y,
                 surface_layout.divider_width, layout.editor_surface.h);
    if (Contains(left_divider_rect, x, y) || Contains(right_divider_rect, x, y)) {
      return CursorKind::EwResize;
    }
    const MergeToolbarLayout toolbar = ComputeMergeToolbarLayout(layout.editor_surface, surface_layout);
    if (Contains(toolbar.prev_rect, x, y) || Contains(toolbar.next_rect, x, y) ||
        Contains(toolbar.save_rect, x, y) || Contains(toolbar.open_rect, x, y)) {
      return CursorKind::Pointer;
    }
    if (scroll_layout.vertical_scrollbar.has_value() &&
        Contains(scroll_layout.vertical_scrollbar->track, x, y)) {
      return CursorKind::Default;
    }
    if (scroll_layout.horizontal_scrollbar.has_value() &&
        Contains(scroll_layout.horizontal_scrollbar->track, x, y)) {
      return CursorKind::Default;
    }
    const SDL_FRect result_rect = ComputeMergeResultViewportRect(
        layout.editor_surface, surface_layout.center_x, surface_layout.rows_y,
        surface_layout.gutter_width, surface_layout.center_width, surface_layout.show_horizontal);
    const editor::EditorViewMetrics result_metrics =
        editor::EditorViewRenderer::ComputeMetrics(text_renderer_, merge_tab->result_viewport, result_rect);
    const MergeInteractionLayout interaction = {
        .content_bottom = scroll_layout.content_rect.y + scroll_layout.content_rect.h,
        .result =
            MergeResultInteractionLayout{
                .rect = result_rect,
                .metrics = result_metrics,
                .lines =
                    VisibleLineRangeLayout{
                        .first_line_y = result_metrics.first_line_y,
                        .line_height = result_metrics.line_height,
                        .scroll_line = merge_tab->result_viewport.scroll_line(),
                        .visible_rows = result_metrics.visible_rows,
                    },
                .text = ComputeTextGridInteractionLayout(
                    result_rect, result_metrics.text_x, result_metrics.first_line_y,
                    result_metrics.line_height, text_renderer_.CharWidth(),
                    merge_tab->result_viewport.scroll_line(), merge_tab->result_viewport.line_count(),
                    merge_tab->result_viewport.horizontal_scroll(), result_metrics.visible_rows,
                    result_metrics.visible_columns),
            },
        .incoming_accept_button_width =
            ComputeChromeButtonWidth(text_renderer_.MeasureWidth("Accept Theirs")),
        .current_accept_button_width =
            ComputeChromeButtonWidth(text_renderer_.MeasureWidth("Accept Ours")),
        .result_action_widths =
            {
                ComputeChromeButtonWidth(text_renderer_.MeasureWidth("Base")),
                ComputeChromeButtonWidth(text_renderer_.MeasureWidth("Theirs")),
                ComputeChromeButtonWidth(text_renderer_.MeasureWidth("Ours")),
                ComputeChromeButtonWidth(text_renderer_.MeasureWidth("Both")),
            },
    };
    if (const auto hover = ClassifyMergeHoverState(surface_layout, interaction, *merge_tab, x, y);
        hover.has_value() && hover->kind == MergeHoverState::Kind::ResultAction) {
      return CursorKind::Pointer;
    }
    if (Contains(interaction.result.rect, x, y) ||
        (x >= surface_layout.center_x && x < surface_layout.right_x && y >= surface_layout.rows_y)) {
      return CursorKind::Text;
    }
    return CursorKind::Pointer;
  }

  for (const EditorSplitDividerLayout& divider :
       ComputeEditorSplitDividerLayouts(layout.editor_surface)) {
    if (Contains(divider.rect, x, y)) {
      return divider.rect.h > divider.rect.w ? CursorKind::EwResize : CursorKind::NsResize;
    }
  }

  const auto panes = ComputeEditorPaneLayouts(layout.editor_surface);
  const auto pane_it =
      std::find_if(panes.begin(), panes.end(),
                   [&](const EditorPaneLayout& pane) { return Contains(pane.rect, x, y); });
  if (pane_it == panes.end()) {
    return CursorKind::Default;
  }

  const TabEntry::EditorTabState* editor_tab = ActiveEditorTab();
  const editor::TextViewport* viewport =
      pane_it->active ? &context_.current_project_state.welcome_surface.viewport
                      : (editor_tab != nullptr ? FindEditorView(*editor_tab, pane_it->leaf_id)
                                               : nullptr);
  if (viewport == nullptr || viewport->is_placeholder()) {
    return CursorKind::Text;
  }

  const editor::EditorViewMetrics metrics =
      editor::EditorViewRenderer::ComputeMetrics(text_renderer_, *viewport, pane_it->rect);
  const auto scroll_layout = ComputeEditorScrollLayout(pane_it->rect, *viewport, metrics);
  if (scroll_layout.vertical_scrollbar.has_value() &&
      Contains(scroll_layout.vertical_scrollbar->track, x, y)) {
    return CursorKind::Default;
  }
  if (scroll_layout.horizontal_scrollbar.has_value() &&
      Contains(scroll_layout.horizontal_scrollbar->track, x, y)) {
    return CursorKind::Default;
  }
  if (const editor::EditorBlameLine* blame_line = editor_blame_overlay_service_.LineAtPosition(x, y);
      blame_line != nullptr) {
    return blame_line->interactive ? CursorKind::Pointer : CursorKind::Default;
  }
  // Gutter region: line-number column shows the default arrow; the fold
  // marker hit zone (rightmost ~18px of the gutter, matching the
  // EditorMouseCoordinator hit-test) shows the pointer when over a fold
  // opener so it reads as a clickable affordance. Use the active editor
  // viewport (not the welcome_surface snapshot used above) because only the
  // active viewport has the FoldingModel attached, so its visual-row layout
  // matches what's painted.
  if (x < metrics.text_x) {
    const float gutter_right = pane_it->rect.x + metrics.gutter_width;
    const float fold_hit_left = gutter_right - 18.0f;
    if (x >= fold_hit_left && x < gutter_right && y >= metrics.first_line_y) {
      const editor::TextViewport* fold_viewport =
          pane_it->active ? ActiveEditorViewport() : viewport;
      if (fold_viewport != nullptr) {
        const std::size_t visual_row =
            fold_viewport->scroll_line() +
            static_cast<std::size_t>((y - metrics.first_line_y) / metrics.line_height);
        if (visual_row < fold_viewport->visual_line_count()) {
          const std::size_t opener_line = fold_viewport->VisualRowLineIndex(visual_row);
          if (editor_tab != nullptr &&
              editor_tab->folding_model->FoldStartingAt(opener_line).has_value()) {
            return CursorKind::Pointer;
          }
        }
      }
    }
    return CursorKind::Default;
  }
  return CursorKind::Text;
}

SDL_Cursor* WorkspaceShell::CursorHandle(CursorKind kind) {
  switch (kind) {
    case CursorKind::Default:
      return SDL_GetDefaultCursor();
    case CursorKind::Text:
      if (text_cursor_ == nullptr) {
        text_cursor_ = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_TEXT);
      }
      return text_cursor_;
    case CursorKind::Pointer:
      if (pointer_cursor_ == nullptr) {
        pointer_cursor_ = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_POINTER);
      }
      return pointer_cursor_;
    case CursorKind::EwResize:
      if (ew_resize_cursor_ == nullptr) {
        ew_resize_cursor_ = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_EW_RESIZE);
      }
      return ew_resize_cursor_;
    case CursorKind::NsResize:
      if (ns_resize_cursor_ == nullptr) {
        ns_resize_cursor_ = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_NS_RESIZE);
      }
      return ns_resize_cursor_;
  }

  return SDL_GetDefaultCursor();
}

void WorkspaceShell::ClearMouseHoverState() {
  last_mouse_position_valid_ = false;
  active_editor_hover_target_.reset();
  editor_hover_refresh_pending_ = false;
  cursor_kind_fingerprint_.valid = false;
  ++editor_hover_target_generation_;

  if (cursor_kind_ == CursorKind::Default) {
    return;
  }

  if (SDL_Cursor* default_cursor = CursorHandle(CursorKind::Default);
      default_cursor != nullptr && SDL_SetCursor(default_cursor)) {
    cursor_kind_ = CursorKind::Default;
    return;
  }

  cursor_kind_ = CursorKind::Default;
}

bool WorkspaceShell::MenuSurfaceCapturingMouse() const {
  return context_.menu_state.menu_bar_open || context_.menu_state.overflow_popup_open ||
         context_.menu_state.tree_context_menu.open;
}

void WorkspaceShell::UpdateMouseCursor(float x, float y, bool update_editor_hover,
                                       bool workspace_layout_recomputed) {
  util::PerformanceTrace::Scope perf_scope("WorkspaceShell::UpdateMouseCursor");
  const bool mouse_moved =
      !last_mouse_position_valid_ || x != last_mouse_x_ || y != last_mouse_y_;
  last_mouse_x_ = x;
  last_mouse_y_ = y;
  last_mouse_position_valid_ = true;
  if (update_editor_hover &&
      (mouse_moved || editor_hover_refresh_pending_ || workspace_layout_recomputed)) {
    util::PerformanceTrace::Scope scope("WorkspaceShell::UpdateMouseCursor::UpdateEditorHover");
    UpdateEditorHover(x, y);
  } else if (!update_editor_hover) {
    active_editor_hover_target_.reset();
    editor_hover_refresh_pending_ = false;
  }

  // Fast-path: if the inputs CursorKindForPosition reads haven't changed since the
  // last call (typical PrepareFrameOnce frame where the mouse is still and no menu
  // / prompt / drag state changed), skip the hit-testing work entirely.
  const CursorKindFingerprint next_fp{
      .x = x,
      .y = y,
      .drag_target = static_cast<int>(context_.interaction_state.drag_target),
      .valid = true,
      .dirty_prompt = context_.prompts.dirty_visible,
      .prompt_surface = context_.prompts.surface_visible,
      .menu_bar_open = context_.menu_state.menu_bar_open,
      .overflow_popup_open = context_.menu_state.overflow_popup_open,
      .tree_context_menu_open = context_.menu_state.tree_context_menu.open,
      .active_menu_id = static_cast<int>(context_.menu_state.active_menu_id),
      .hovered_popup_row_index = context_.menu_state.hovered_popup_row_index,
      .hovered_submenu_row_index = context_.menu_state.hovered_submenu_row_index,
      .active_submenu_id = static_cast<int>(context_.menu_state.active_submenu_id),
      .overlay_visible = context_.current_project_state.overlay.visible,
      .settings_overlay_visible = settings_overlay_service_.Visible(),
      .bottom_panel_visible = BottomPanelVisible(),
      .active_tab_index =
          static_cast<std::uint32_t>(context_.current_project_state.active_tab_index),
      .cursor_hit_generation = cursor_hit_generation_,
      .editor_hover_target_generation = editor_hover_target_generation_,
  };
  if (cursor_kind_fingerprint_.valid &&
      cursor_kind_fingerprint_.x == next_fp.x &&
      cursor_kind_fingerprint_.y == next_fp.y &&
      cursor_kind_fingerprint_.drag_target == next_fp.drag_target &&
      cursor_kind_fingerprint_.dirty_prompt == next_fp.dirty_prompt &&
      cursor_kind_fingerprint_.prompt_surface == next_fp.prompt_surface &&
      cursor_kind_fingerprint_.menu_bar_open == next_fp.menu_bar_open &&
      cursor_kind_fingerprint_.overflow_popup_open == next_fp.overflow_popup_open &&
      cursor_kind_fingerprint_.tree_context_menu_open == next_fp.tree_context_menu_open &&
      cursor_kind_fingerprint_.active_menu_id == next_fp.active_menu_id &&
      cursor_kind_fingerprint_.hovered_popup_row_index == next_fp.hovered_popup_row_index &&
      cursor_kind_fingerprint_.hovered_submenu_row_index == next_fp.hovered_submenu_row_index &&
      cursor_kind_fingerprint_.active_submenu_id == next_fp.active_submenu_id &&
      cursor_kind_fingerprint_.overlay_visible == next_fp.overlay_visible &&
      cursor_kind_fingerprint_.settings_overlay_visible == next_fp.settings_overlay_visible &&
      cursor_kind_fingerprint_.bottom_panel_visible == next_fp.bottom_panel_visible &&
      cursor_kind_fingerprint_.active_tab_index == next_fp.active_tab_index &&
      cursor_kind_fingerprint_.cursor_hit_generation == next_fp.cursor_hit_generation &&
      cursor_kind_fingerprint_.editor_hover_target_generation ==
          next_fp.editor_hover_target_generation &&
      !workspace_layout_recomputed) {
    return;
  }
  cursor_kind_fingerprint_ = next_fp;

  const CursorKind next_kind = CursorKindForPosition(x, y);
  if (next_kind == cursor_kind_) {
    return;
  }

  cursor_kind_ = next_kind;
  if (SDL_Cursor* cursor = CursorHandle(next_kind); cursor != nullptr) {
    (void)SDL_SetCursor(cursor);
    return;
  }

  if (SDL_Cursor* default_cursor = CursorHandle(CursorKind::Default); default_cursor != nullptr) {
    (void)SDL_SetCursor(default_cursor);
  }
}

char WorkspaceShell::KeycodeToAscii(SDL_Keycode keycode, SDL_Keymod modifiers) {
  const bool shift = (modifiers & SDL_KMOD_SHIFT) != 0;

  if (keycode >= SDLK_A && keycode <= SDLK_Z) {
    const char base = static_cast<char>('a' + (keycode - SDLK_A));
    return shift ? static_cast<char>(std::toupper(static_cast<unsigned char>(base))) : base;
  }

  if (keycode >= SDLK_0 && keycode <= SDLK_9) {
    static constexpr char shifted_digits[] = {')', '!', '@', '#', '$', '%', '^', '&', '*', '('};
    const int index = keycode - SDLK_0;
    return shift ? shifted_digits[index] : static_cast<char>('0' + index);
  }

  switch (keycode) {
    case SDLK_SPACE:
      return ' ';
    case SDLK_SLASH:
      return shift ? '?' : '/';
    case SDLK_BACKSLASH:
      return shift ? '|' : '\\';
    case SDLK_PERIOD:
      return shift ? '>' : '.';
    case SDLK_COMMA:
      return shift ? '<' : ',';
    case SDLK_MINUS:
      return shift ? '_' : '-';
    case SDLK_EQUALS:
      return shift ? '+' : '=';
    case SDLK_SEMICOLON:
      return shift ? ':' : ';';
    case SDLK_APOSTROPHE:
      return shift ? '"' : '\'';
    case SDLK_LEFTBRACKET:
      return shift ? '{' : '[';
    case SDLK_RIGHTBRACKET:
      return shift ? '}' : ']';
    default:
      return '\0';
  }
}

}  // namespace microide::workspace
