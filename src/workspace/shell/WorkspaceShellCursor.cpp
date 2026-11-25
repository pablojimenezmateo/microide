#include "workspace/render/CompareReviewControls.h"
#include "workspace/render/GitSidebarPaint.h"
#include "workspace/render/DiffDividerGeometry.h"
#include "workspace/shell/WorkspaceShell.h"

#include "workspace/ProjectSearchPanelLayout.h"
#include "workspace/git/GitSidebarHeaderLayout.h"

#include <algorithm>
#include <array>
#include <initializer_list>
#include <span>
#include <cctype>
#include <string_view>

#include "util/PerformanceTrace.h"
#include "workspace/render/CompareMergeRender.h"
#include "workspace/git/CompareTabReview.h"
#include "workspace/PluginSurfacePreview.h"
#include "workspace/services/SettingsOverlayService.h"
#include "workspace/WorkspaceLayout.h"
#include "workspace/WorkspaceOutputReference.h"
#include "util/StringUtil.h"
#include "workspace/render/NotificationLayout.h"
#include "workspace/debug/DebugPaneRegistry.h"

namespace microide::workspace {

namespace {

constexpr float kWindowFrameHitThickness = 6.0f;

std::optional<SDL_FRect> OverflowChipAt(const TabStripOverflowControls& overflow, float x,
                                        float y) {
  if (overflow.hidden_left > 0 && Contains(overflow.left_button, x, y)) {
    return overflow.left_button;
  }
  if (overflow.hidden_right > 0 && Contains(overflow.right_button, x, y)) {
    return overflow.right_button;
  }
  return std::nullopt;
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

}

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

  const WorkspaceLayout layout = ComputeLayout(CurrentLayoutInputs(window_width, window_height));
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
  if (const DragTarget target = context_.interaction_state.drag_target; IsDividerDrag(target)) {
    return target == DragTarget::BottomPanelDivider ? CursorKind::NsResize : CursorKind::EwResize;
  }

  const auto window_rect = CurrentWindowRect();
  if (!window_rect.has_value()) {
    return CursorKind::Default;
  }

  switch (WindowHitTest(x, y)) {
    case SDL_HITTEST_RESIZE_TOPLEFT:
      return CursorKind::NwResize;
    case SDL_HITTEST_RESIZE_TOP:
      return CursorKind::NResize;
    case SDL_HITTEST_RESIZE_TOPRIGHT:
      return CursorKind::NeResize;
    case SDL_HITTEST_RESIZE_RIGHT:
      return CursorKind::EResize;
    case SDL_HITTEST_RESIZE_BOTTOMRIGHT:
      return CursorKind::SeResize;
    case SDL_HITTEST_RESIZE_BOTTOM:
      return CursorKind::SResize;
    case SDL_HITTEST_RESIZE_BOTTOMLEFT:
      return CursorKind::SwResize;
    case SDL_HITTEST_RESIZE_LEFT:
      return CursorKind::WResize;
    default:
      break;
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

  if (const auto kind = CursorKindOverModalsAndMenus(x, y, layout)) {
    return *kind;
  }
  if (const auto kind = CursorKindOverChrome(x, y, layout)) {
    return *kind;
  }
  if (const auto kind = CursorKindOverSidebar(x, y, layout)) {
    return *kind;
  }
  if (const auto kind = CursorKindOverPanels(x, y, layout)) {
    return *kind;
  }
  return CursorKindOverEditorArea(x, y, layout);
}

std::optional<WorkspaceShell::CursorKind> WorkspaceShell::CursorKindOverModalsAndMenus(
    float x, float y, const WorkspaceLayout& layout) const {
  if (!notification_service_.Empty() &&
      NotificationHitAt(RenderViewModelBuilder(context_).BuildNotifications(
                            notification_service_, layout.status_bar, text_renderer_),
                        x, y)
          .has_value()) {
    return CursorKind::Pointer;
  }

  if (settings_overlay_service_.Visible()) {
    const SettingsOverlayViewModel vm =
        RenderViewModelBuilder(context_).BuildSettingsOverlay(layout, settings_overlay_service_,
                                                              text_renderer_);
    if (!SettingsOverlayContains(vm, x, y)) {
      return CursorKind::Default;
    }
    if (vm.mode != SettingsOverlayMode::Settings) {
      return CursorKind::Default;
    }
    if (vm.value_picker.visible) {
      for (const SettingsPickerItemViewModel& item : vm.value_picker.items) {
        if (Contains(item.rect, x, y)) {
          return CursorKind::Pointer;
        }
      }
      if (Contains(vm.value_picker.rect, x, y)) {
        return CursorKind::Default;
      }
    }
    if (Contains(vm.filter_rect, x, y)) {
      return CursorKind::Text;
    }
    for (const SettingsCategoryViewModel& cat : vm.categories) {
      if (Contains(cat.rect, x, y)) {
        return CursorKind::Pointer;
      }
    }
    for (const SettingsRowViewModel& row : vm.rows) {
      if (Contains(row.row_rect, x, y)) {
        return CursorKind::Pointer;
      }
    }
    return CursorKind::Default;
  }

  const auto row_cursor = [this](std::span<const MenuItemSpec> items,
                                 const PopupRowGeometry& hit) {
    return hit.separator || !IsMenuItemEnabled(items[hit.index]) ? CursorKind::Default
                                                                 : CursorKind::Pointer;
  };
  if (context_.menu_state.tree_context_menu.open) {
    if (const auto popup_rect = ComputeTreeContextMenuRect(); popup_rect.has_value()) {
      const auto items = WorkspaceTreeContextMenuItems(context_.menu_state.tree_context_menu.target);
      if (const auto hit = HitTestPopupRow(items, *popup_rect, x, y); hit.has_value()) {
        return row_cursor(items, *hit);
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
        return row_cursor(items, *hit);
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
        return row_cursor(items, *hit);
      }
      if (Contains(*popup_rect, x, y)) {
        return CursorKind::Default;
      }
    }
  }

  if (const auto chevron = MenuOverflowChevronRect(layout.menu_bar);
      context_.menu_state.overflow_popup_open && chevron.has_value()) {
    const auto overflow = ComputeOverflowMenuBarItems(layout.menu_bar);
    const SDL_FRect popup = ComputeMenuOverflowPopupRect(*chevron, overflow.size());
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

  return std::nullopt;
}

std::optional<WorkspaceShell::CursorKind> WorkspaceShell::CursorKindOverChrome(float x, float y,
                                                                               const WorkspaceLayout& layout) const {
  if (context_.current_project_state.overlay.visible) {
    const OverlayMode overlay_mode = context_.current_project_state.overlay.mode;
    if (overlay_mode == OverlayMode::BufferSearch || overlay_mode == OverlayMode::BufferReplace) {
      const FindWidgetLayout fw =
          ComputeBufferFindWidgetLayout(ActiveTabSurface(layout), overlay_mode == OverlayMode::BufferReplace);
      if (Contains(fw.search_field, x, y) ||
          (fw.replace_mode && Contains(fw.replace_field, x, y))) {
        return CursorKind::Text;
      }
      if (Contains(fw.widget, x, y)) {
        return CursorKind::Pointer;
      }
    } else {
      const SDL_FRect overlay = ComputeOverlayRect(layout.overlay_anchor);
      if (!Contains(overlay, x, y)) {
        return CursorKind::Default;
      }

      const auto overlay_list_layout = ComputeOverlayListLayout(overlay);
      if (overlay_list_layout.scrollbar.has_value() &&
          Contains(overlay_list_layout.scrollbar->track, x, y)) {
        return CursorKind::Default;
      }
      if (ScrollableListItemAtY(overlay_list_layout, y, OverlayItemCount()).has_value()) {
        return CursorKind::Pointer;
      }

      if (overlay_mode == OverlayMode::CommitPicker ||
          overlay_mode == OverlayMode::QuickPick ||
          overlay_mode == OverlayMode::CommandPalette) {
        return y >= overlay.y + 48.0f && y < overlay.y + 68.0f ? CursorKind::Text
                                                                : CursorKind::Default;
      }
      if (overlay_mode == OverlayMode::Completion || overlay_mode == OverlayMode::CodeActions) {
        return CursorKind::Default;
      }
      return y >= overlay.y + 40.0f && y < overlay.y + 60.0f ? CursorKind::Text
                                                              : CursorKind::Default;
    }
  }

  if (context_.current_project_state.sidebar.visible &&
      Contains(SidebarResizeCursorRect(layout), x, y)) {
    return CursorKind::EwResize;
  }
  if (context_.current_project_state.debug_pane.visible &&
      Contains(RightPaneResizeCursorRect(layout), x, y)) {
    return CursorKind::EwResize;
  }
  if (BottomPanelVisible() && Contains(BottomPanelResizeCursorRect(layout), x, y)) {
    return CursorKind::NsResize;
  }

  if (layout.status_bar.w > 0.0f && layout.status_bar.h > 0.0f &&
      Contains(layout.status_bar, x, y)) {
    return HoveredStatusBarSegmentRect(layout, x, y).has_value() ? CursorKind::Pointer
                                                                 : CursorKind::Default;
  }

  if (layout.breadcrumb.w > 0.0f && layout.breadcrumb.h > 0.0f &&
      Contains(layout.breadcrumb, x, y)) {
    for (const VisibleStatusItem& status_item : ComputeVisibleStatusItems(layout.breadcrumb)) {
      if (!status_item.item.command.empty() && Contains(status_item.rect, x, y)) {
        return CursorKind::Pointer;
      }
    }
    return CursorKind::Default;
  }

  if (Contains(layout.project_tab_strip, x, y)) {
    const auto& project_tabs =
        tab_strip_chrome_.ComputeVisibleProjectTabs(layout.project_tab_strip);
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

  {
    const EditorGroupRectsLayout group_rects = ComputeEditorGroupRectsForState(layout);
    const std::vector<EditorGroup>& editor_groups = context_.current_project_state.editor_groups;
    for (std::size_t gi = 0; gi < group_rects.groups.size(); ++gi) {
      const SDL_FRect group_tab_strip = group_rects.groups[gi].tab_strip;
      if (group_tab_strip.w <= 0.0f || group_tab_strip.h <= 0.0f ||
          !Contains(group_tab_strip, x, y)) {
        continue;
      }
      if (context_.current_project_state.root.empty()) {
        return CursorKind::Default;
      }
      if (gi >= editor_groups.size() || editor_groups[gi].open_tabs.empty()) {
        return Contains(EmptyTabStripPlaceholderRect(group_tab_strip), x, y)
                   ? CursorKind::Pointer
                   : CursorKind::Default;
      }
      const auto& tabs = tab_strip_chrome_.ComputeVisibleTabsForGroup(gi, group_tab_strip);
      const auto tab_overflow =
          tab_strip_chrome_.ComputeTabOverflowControlsForGroup(gi, group_tab_strip, tabs);
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
  }

  return std::nullopt;
}

std::optional<WorkspaceShell::CursorKind> WorkspaceShell::CursorKindOverSidebar(float x, float y,
                                                                                const WorkspaceLayout& layout) const {
  if (context_.current_project_state.sidebar.visible && Contains(layout.sidebar, x, y)) {
    const SidebarMode sidebar_mode = ActiveSidebarMode();
    const SidebarModeRowLayout mode_row = SidebarModeRow(layout.sidebar);
    for (int i = 0; i < mode_row.tab_count; ++i) {
      if (Contains(mode_row.tabs[static_cast<std::size_t>(i)].rect, x, y)) {
        return CursorKind::Pointer;
      }
    }
    if (mode_row.has_overflow && Contains(mode_row.overflow_rect, x, y)) {
      return CursorKind::Pointer;
    }
    if (sidebar_mode == SidebarMode::Search) {
      for (const auto& field : project_search_panel::SidebarSearchFieldRects(
               layout.sidebar,
               context_.current_project_state.overlay.workflow.project_search.scope_expanded)) {
        if (field.rect.w > 0.0f && Contains(field.rect, x, y)) {
          return CursorKind::Text;
        }
      }
      if (Contains(project_search_panel::ModeButtonRect(layout.sidebar), x, y) ||
          Contains(project_search_panel::ReplaceAllButtonRect(layout.sidebar), x, y) ||
          Contains(project_search_panel::CaseButtonRect(layout.sidebar), x, y) ||
          Contains(project_search_panel::HiddenButtonRect(layout.sidebar), x, y) ||
          Contains(project_search_panel::ScopeButtonRect(layout.sidebar), x, y)) {
        return CursorKind::Pointer;
      }

      const auto line_map = BuildProjectSearchLineMap();
      const auto list_layout =
          ComputeProjectSearchSidebarListLayout(layout.sidebar, line_map.size());
      const auto line_index = ScrollableListItemAt(list_layout, x, y, line_map.size());
      return line_index.has_value() && line_map[*line_index] >= 0 ? CursorKind::Pointer
                                                                   : CursorKind::Default;
    }
    if (sidebar_mode == SidebarMode::Git) {
      if (Contains(git_sidebar_header::StageAllButtonRect(layout.sidebar), x, y) &&
          CanStageAllGitSidebarEntries()) {
        return CursorKind::Pointer;
      }
      if (Contains(git_sidebar_header::DiscardAllButtonRect(layout.sidebar), x, y) &&
          CanDiscardAllGitSidebarEntries()) {
        return CursorKind::Pointer;
      }
      if (Contains(git_sidebar_header::BranchButtonRect(layout.sidebar), x, y) ||
          Contains(git_sidebar_header::SyncButtonRect(layout.sidebar), x, y)) {
        return CursorKind::Pointer;
      }
      if (Contains(git_sidebar_header::RefreshButtonRect(layout.sidebar), x, y)) {
        return CursorKind::Pointer;
      }
      if (const auto button_rect = GitSidebarOutgoingBaseButtonRect(layout.sidebar);
          button_rect.has_value() && Contains(*button_rect, x, y)) {
        return CursorKind::Pointer;
      }
      if (CanOpenGitCommitButton() &&
          Contains(git_sidebar_header::CommitButtonRect(layout.sidebar), x, y)) {
        return CursorKind::Pointer;
      }
      if (const auto& workflow = context_.current_project_state.sidebar.git.commit_workflow;
          workflow.open) {
        if ((workflow.subject_field_rect.w > 0.0f && Contains(workflow.subject_field_rect, x, y)) ||
            (workflow.body_field_rect.w > 0.0f && Contains(workflow.body_field_rect, x, y))) {
          return CursorKind::Text;
        }
        if (workflow.commit_button_rect.w > 0.0f && Contains(workflow.commit_button_rect, x, y)) {
          return CursorKind::Pointer;
        }
      }
      const PaintedGitSidebarList painted = PaintedGitSidebarListOf(frame_state_);
      const auto& lines = painted.lines != nullptr ? *painted.lines : BuildGitSidebarLines();
      const auto list_layout =
          painted.lines != nullptr ? *painted.layout : ComputeGitSidebarListLayout(layout.sidebar, lines.size());
      const auto line_index = ScrollableListItemAt(list_layout, x, y, lines.size());
      if (!line_index.has_value()) {
        return CursorKind::Default;
      }
      const auto& line = lines[*line_index];
      if (line.kind == GitSidebarLine::Kind::Directory) {
        return CursorKind::Pointer;
      }
      if (line.kind != GitSidebarLine::Kind::Entry || line.entry_index < 0) {
        return CursorKind::Default;
      }
      return CursorKind::Pointer;
    }
    if (sidebar_mode == SidebarMode::Problems) {
      const std::size_t count = context_.current_project_state.sidebar.problems.entries.size();
      return ScrollableListItemAt(ComputeProblemsSidebarListLayout(layout.sidebar, count), x, y, count).has_value()
                 ? CursorKind::Pointer
                 : CursorKind::Default;
    }
    if (sidebar_mode == SidebarMode::Tests) {
      const std::size_t count = context_.current_project_state.sidebar.tests.entries.size();
      return ScrollableListItemAt(ComputeTestsSidebarListLayout(layout.sidebar, count), x, y, count).has_value()
                 ? CursorKind::Pointer
                 : CursorKind::Default;
    }
    if (UsesPluginItemSurface(sidebar_mode)) {
      const std::size_t count = context_.current_project_state.sidebar.plugin.items.size();
      return ScrollableListItemAt(ComputePluginSidebarListLayout(layout.sidebar, count), x, y, count).has_value()
                 ? CursorKind::Pointer
                 : CursorKind::Default;
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
    return ScrollableListItemAt(list_layout, x, y, entries.size()).has_value()
               ? CursorKind::Pointer
               : CursorKind::Default;
  }

  return std::nullopt;
}

std::optional<WorkspaceShell::CursorKind> WorkspaceShell::CursorKindOverPanels(float x, float y,
                                                                               const WorkspaceLayout& layout) const {
  if (context_.current_project_state.debug_pane.visible && layout.right_pane.w > 0.0f &&
      Contains(layout.right_pane, x, y)) {
    const DebugPaneModeRowLayout mode_row = DebugPaneModeRow(layout.right_pane);
    for (int i = 0; i < mode_row.tab_count; ++i) {
      if (Contains(mode_row.tabs[static_cast<std::size_t>(i)].rect, x, y)) {
        return CursorKind::Pointer;
      }
    }
    const std::size_t row_count = DebugPaneActiveRowCount();
    const auto panel_layout = ComputeDebugPaneListLayout(layout, row_count);
    if (panel_layout.scroll.vertical_scrollbar.has_value() &&
        Contains(panel_layout.scroll.vertical_scrollbar->track, x, y)) {
      return CursorKind::Default;
    }
    const DebugPaneRowHit hit = DebugPaneRowAtPoint(
        panel_layout.content_rect, panel_layout.text_y, panel_layout.line_height,
        panel_layout.scroll.visible_rows, panel_layout.scroll.vertical_scroll, row_count, x, y);
    if (hit.row_index >= 0 && DebugPaneRowIsActionable(static_cast<std::size_t>(hit.row_index))) {
      return CursorKind::Pointer;
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
                   x, y) ||
          Contains(tab_strip_service_.BottomPanelMaximizeButtonRect(
                       layout_mode_service_.CurrentMode(), panel_header),
                   x, y)) {
        return CursorKind::Pointer;
      }
      const std::vector<VisibleStripTab>& visible_panel_tabs =
          tab_strip_service_.ComputeVisibleBottomPanelTabs(
              context_.current_project_state, panel_header, layout_mode_service_.CurrentMode(),
              [this](std::string_view text) { return text_renderer_.MeasureWidth(text); },
              output_channels_.Channels());
      for (const VisibleStripTab& tab : visible_panel_tabs) {
        if (Contains(tab.rect, x, y)) {
          return CursorKind::Pointer;
        }
      }
      if (OverflowChipAt(tab_strip_service_.ComputeBottomPanelTabOverflowControls(
                             context_.current_project_state, panel_header,
                             layout_mode_service_.CurrentMode(), visible_panel_tabs,
                             output_channels_.Channels()),
                         x, y)
              .has_value()) {
        return CursorKind::Pointer;
      }
      return CursorKind::Default;
    }
    if (terminal_find_service_.visible() && prepare_cached_bottom_panel_vm_.has_value() &&
        prepare_cached_bottom_panel_vm_->find_visible) {
      const FindWidgetLayout& fw = prepare_cached_bottom_panel_vm_->find.fw;
      if (Contains(fw.widget, x, y)) {
        if (Contains(fw.search_field, x, y)) {
          return CursorKind::Text;
        }
        for (const SDL_FRect& button : {fw.toggle_buttons[0], fw.toggle_buttons[1], fw.prev_button,
                                        fw.next_button, fw.close_button}) {
          if (Contains(button, x, y)) {
            return CursorKind::Pointer;
          }
        }
        return CursorKind::Default;
      }
    }
    if (context_.current_project_state.panel.content == PanelContentKind::PluginSurface) {
      const auto& panel_state = context_.current_project_state.panel;
      const auto* pres = context_.current_project_state.plugin_presentation_if_present();
      const editor::SurfaceContent* content =
          pres != nullptr ? pres->surfaces.Find(panel_state.surface_owner, panel_state.surface_id)
                          : nullptr;
      const SDL_FRect body = BottomPanelContentRect(layout);
      if (content != nullptr && content->has_body() && Contains(body, x, y) &&
          FindPluginSurfacePreviewHitRegion(*content, body,
                                            static_cast<float>(panel_state.surface_scroll_y), x,
                                            y) != nullptr) {
        return CursorKind::Pointer;
      }
      return CursorKind::Default;
    }
    const std::size_t line_count =
        BottomPanelShowsTerminal() && ActiveTerminalPane() != nullptr
            ? ActiveTerminalPane()->session.LineCount()
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
      if (BottomPanelShowsOutput() && Contains(panel_layout.content_rect, x, y)) {
        if (const auto* entries =
                OutputChannelEntries(context_.current_project_state.panel.output.channel_id);
            entries != nullptr) {
          const auto line_index = BottomPanelLineIndexAtY(
              panel_layout.text_y, panel_layout.line_height, panel_layout.scroll.visible_rows,
              panel_layout.scroll.vertical_scroll, y, entries->size());
          if (line_index.has_value() &&
              ParseOutputReference((*entries)[*line_index]).has_value()) {
            return CursorKind::Pointer;
          }
        }
      }
    }
    if (BottomPanelShowsTerminal() && ActiveTerminalPane() != nullptr &&
        y >= layout.bottom_panel.y + kWorkspaceBottomPanelHeaderHeight) {
      if (const TerminalTabState* tab = context_.current_project_state.active_terminal_tab();
          tab != nullptr && tab->pane_count() > 1) {
        for (const TerminalPaneDividerRect& divider :
             tab->PaneRects(BottomPanelContentRect(layout)).dividers) {
          if (Contains(divider.rect, x, y)) {
            return CursorKind::EwResize;
          }
        }
      }
      const TerminalPaneState* pane = ActiveTerminalPane();
      if ((context_.interaction_state.key_modifiers & SDL_KMOD_SHIFT) == 0) {
        if (const auto cell = TerminalViewportPositionForPoint(static_cast<int>(x), static_cast<int>(y));
            cell.has_value()) {
          if (const terminal::TerminalPointerShape shape = pane->session.PointerShapeAt(cell->row, cell->column);
              shape != terminal::TerminalPointerShape::None) {
            return CursorKindFor(shape);
          }
        }
      }
      if (TerminalLinkAtPoint(x, y).has_value()) {
        return CursorKind::Pointer;
      }
      return CursorKind::Text;
    }
    return CursorKind::Default;
  }

  return std::nullopt;
}

WorkspaceShell::CursorKind WorkspaceShell::CursorKindOverEditorArea(float x, float y,
                                                                    const WorkspaceLayout& layout) const {
  if (const auto popup = ActiveEditorHoverPopupLayout(); popup.has_value()) {
    if (popup->primary_action_rect.has_value() &&
        Contains(EditorHoverPopupPrimaryActionHitRect(*popup), x, y)) {
      return CursorKind::Pointer;
    }
    if (Contains(popup->rect, x, y)) {
      return CursorKind::Default;
    }
  }

  if (DebugToolbarVisible()) {
    const DebugToolbarLayout toolbar = ComputeDebugToolbarLayout(
        ActiveTabSurface(layout), DebugToolbarAvoidBelowY(layout), debug_service_.SupportsStepBack());
    if (Contains(toolbar.widget, x, y)) {
      for (std::size_t i = 0; i < toolbar.button_count; ++i) {
        if (Contains(toolbar.buttons[i], x, y)) {
          return CursorKind::Pointer;
        }
      }
      return CursorKind::Default;
    }
  }

  if (!Contains(layout.editor_surface, x, y)) {
    return CursorKind::Default;
  }

  const SDL_FRect view_surface = ActiveTabSurface(layout);
  const bool over_view_surface = Contains(view_surface, x, y);
  if (ActiveTabIsCompare() && over_view_surface) {
    return CursorKindOverCompare(x, y, view_surface);
  }
  if (ActiveTabIsMerge() && over_view_surface) {
    return CursorKindOverMerge(x, y, view_surface);
  }

  {
    const editor::WelcomeViewModel* welcome_model = nullptr;
    const editor::WelcomeLayout* welcome_layout = nullptr;
    if (ProbeWelcomeSurface(&welcome_model, &welcome_layout)) {
      for (const editor::WelcomeHitRegion& region : welcome_layout->hit_regions) {
        if (Contains(region.rect, x, y)) {
          return CursorKind::Pointer;
        }
      }
      return CursorKind::Text;
    }
  }

  for (const EditorSplitDividerRect& divider : ComputeEditorGroupRectsForState(layout).dividers) {
    if (Contains(divider.rect, x, y)) {
      return divider.vertical ? CursorKind::EwResize : CursorKind::NsResize;
    }
  }

  const auto panes = ComputeEditorPaneLayouts(layout);
  const auto pane_it =
      std::find_if(panes.begin(), panes.end(),
                   [&](const EditorPaneLayout& pane) { return Contains(pane.rect, x, y); });
  if (pane_it == panes.end()) {
    return CursorKind::Default;
  }

  const editor::TextViewport* viewport = ViewportForPane(*pane_it);
  if (viewport == nullptr || viewport->is_placeholder()) {
    return CursorKind::Text;
  }

  const editor::EditorViewMetrics metrics = editor::EditorViewRenderer::ComputeMetrics(
      editor_text_renderer_, *viewport, pane_it->rect, viewport->sticky_band_rows(), LineNumbersEnabled());
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
  if (x < metrics.text_x) {
    const float gutter_right = pane_it->rect.x + metrics.gutter_width;
    const float fold_hit_left = gutter_right - 18.0f;
    if (x >= fold_hit_left && x < gutter_right && y >= metrics.first_line_y) {
      const std::size_t visual_row =
          viewport->scroll_line() +
          static_cast<std::size_t>((y - metrics.first_line_y) / metrics.line_height);
      if (visual_row < viewport->visual_line_count()) {
        const std::size_t opener_line = viewport->VisualRowLineIndex(visual_row);
        const std::vector<EditorGroup>& groups = context_.current_project_state.editor_groups;
        const TabEntry::EditorTabState* editor_tab =
            pane_it->group_index < groups.size()
                ? GroupActiveEditorTab(groups[pane_it->group_index])
                : nullptr;
        if (editor_tab != nullptr &&
            editor_tab->folding_model->FoldStartingAt(opener_line).has_value()) {
          return CursorKind::Pointer;
        }
      }
    }
    return CursorKind::Default;
  }
  return CursorKind::Text;
}

WorkspaceShell::CursorKind WorkspaceShell::CursorKindOverCompare(float x, float y,
                                                                 const SDL_FRect& view_surface) const {
  const CompareTabState* compare_tab = ActiveCompareTab();
  if (compare_tab == nullptr) {
    return CursorKind::Default;
  }
  const CompareSurfaceLayout surface_layout = ComputeCompareSurfaceLayout(view_surface, *compare_tab);
  const auto scroll_layout = ComputeCompareScrollLayout(view_surface, surface_layout, *compare_tab);
  if (scroll_layout.vertical_scrollbar.has_value() && Contains(scroll_layout.vertical_scrollbar->track, x, y)) {
    return CursorKind::Default;
  }
  if (scroll_layout.horizontal_scrollbar.has_value() && Contains(scroll_layout.horizontal_scrollbar->track, x, y)) {
    return CursorKind::Default;
  }
  const SDL_FRect divider_rect = CompareDividerHitRect(view_surface, surface_layout);
  if (Contains(divider_rect, x, y)) {
    return CursorKind::EwResize;
  }
  if (CompareControlAt(editor_text_renderer_, MakeCompareControlGeometry(view_surface, surface_layout), *compare_tab, x,
                       y)
          .has_value()) {
    return CursorKind::Pointer;
  }
  if (const auto hit = CompareCollapsedContextRowAt(*compare_tab, view_surface, surface_layout.rows_y,
                                                    surface_layout.line_height, surface_layout.show_vertical, y);
      hit.has_value()) {
    const auto action_rects = BuildCollapsedContextActionRects(
        editor_text_renderer_, hit->block_rect, hit->row->previous_hunk_index >= 0, hit->row->next_hunk_index >= 0);
    if (CompareCollapsedContextActionAt(action_rects, x, y).has_value()) {
      return CursorKind::Pointer;
    }
  }
  if (compare_tab->right_editable && x >= surface_layout.right_x) {
    return CursorKind::Text;
  }
  return CursorKind::Pointer;
}

WorkspaceShell::CursorKind WorkspaceShell::CursorKindOverMerge(float x, float y, const SDL_FRect& view_surface) const {
  const MergeTabState* merge_tab = ActiveMergeTab();
  if (merge_tab == nullptr) {
    return CursorKind::Default;
  }
  const MergeSurfaceLayout surface_layout = ComputeMergeSurfaceLayout(view_surface, *merge_tab);
  const auto scroll_layout = ComputeMergeScrollLayout(view_surface, surface_layout, *merge_tab);
  const auto divider_rects = MergeDividerHitRects(view_surface, surface_layout);
  if (Contains(divider_rects[0], x, y) || Contains(divider_rects[1], x, y)) {
    return CursorKind::EwResize;
  }
  const MergeToolbarLayout toolbar = ComputeMergeToolbarLayout(view_surface, surface_layout);
  if (Contains(toolbar.prev_rect, x, y) || Contains(toolbar.next_rect, x, y) || Contains(toolbar.save_rect, x, y) ||
      Contains(toolbar.open_rect, x, y)) {
    return CursorKind::Pointer;
  }
  if (scroll_layout.vertical_scrollbar.has_value() && Contains(scroll_layout.vertical_scrollbar->track, x, y)) {
    return CursorKind::Default;
  }
  if (scroll_layout.horizontal_scrollbar.has_value() && Contains(scroll_layout.horizontal_scrollbar->track, x, y)) {
    return CursorKind::Default;
  }
  const MergeInteractionLayout interaction = {
      .content_bottom = scroll_layout.content_rect.y + scroll_layout.content_rect.h,
      .result = BuildMergeResultInteractionLayout(view_surface, surface_layout, *merge_tab),
      .incoming_accept_button_width = ComputeChromeButtonWidth(editor_text_renderer_.MeasureWidth("Accept Remote")),
      .current_accept_button_width = ComputeChromeButtonWidth(editor_text_renderer_.MeasureWidth("Accept Local")),
      .result_action_widths =
          {
              ComputeChromeButtonWidth(editor_text_renderer_.MeasureWidth("Base")),
              ComputeChromeButtonWidth(editor_text_renderer_.MeasureWidth("Remote")),
              ComputeChromeButtonWidth(editor_text_renderer_.MeasureWidth("Local")),
              ComputeChromeButtonWidth(editor_text_renderer_.MeasureWidth("Both")),
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

void WorkspaceShell::SdlCursorDeleter::operator()(SDL_Cursor* cursor) const {
  SDL_DestroyCursor(cursor);
}

SDL_Cursor* WorkspaceShell::CursorHandle(CursorKind kind) {
  if (kind == CursorKind::Default) {
    return SDL_GetDefaultCursor();
  }
  const SDL_SystemCursor system_cursor = [kind]() {
    switch (kind) {
      case CursorKind::Text: return SDL_SYSTEM_CURSOR_TEXT;
      case CursorKind::Pointer: return SDL_SYSTEM_CURSOR_POINTER;
      case CursorKind::EwResize: return SDL_SYSTEM_CURSOR_EW_RESIZE;
      case CursorKind::NsResize: return SDL_SYSTEM_CURSOR_NS_RESIZE;
      case CursorKind::NResize: return SDL_SYSTEM_CURSOR_N_RESIZE;
      case CursorKind::EResize: return SDL_SYSTEM_CURSOR_E_RESIZE;
      case CursorKind::SResize: return SDL_SYSTEM_CURSOR_S_RESIZE;
      case CursorKind::WResize: return SDL_SYSTEM_CURSOR_W_RESIZE;
      case CursorKind::NeResize: return SDL_SYSTEM_CURSOR_NE_RESIZE;
      case CursorKind::SeResize: return SDL_SYSTEM_CURSOR_SE_RESIZE;
      case CursorKind::SwResize: return SDL_SYSTEM_CURSOR_SW_RESIZE;
      case CursorKind::NwResize: return SDL_SYSTEM_CURSOR_NW_RESIZE;
      case CursorKind::Wait: return SDL_SYSTEM_CURSOR_WAIT;
      case CursorKind::Progress: return SDL_SYSTEM_CURSOR_PROGRESS;
      case CursorKind::Crosshair: return SDL_SYSTEM_CURSOR_CROSSHAIR;
      case CursorKind::Move: return SDL_SYSTEM_CURSOR_MOVE;
      case CursorKind::NotAllowed: return SDL_SYSTEM_CURSOR_NOT_ALLOWED;
      case CursorKind::NwseResize: return SDL_SYSTEM_CURSOR_NWSE_RESIZE;
      case CursorKind::NeswResize: return SDL_SYSTEM_CURSOR_NESW_RESIZE;
      case CursorKind::Default: break;
    }
    return SDL_SYSTEM_CURSOR_DEFAULT;
  }();
  auto& slot = system_cursors_[static_cast<std::size_t>(kind)];
  if (slot == nullptr) {
    slot.reset(SDL_CreateSystemCursor(system_cursor));
  }
  return slot.get();
}

void WorkspaceShell::ClearMouseHoverState() {
  last_mouse_position_valid_ = false;
  active_editor_hover_target_.reset();
  editor_hover_refresh_pending_ = false;
  cursor_kind_fingerprint_.valid = false;
  ++editor_hover_target_generation_;
  notification_service_.SetHovered(std::nullopt, std::nullopt, SDL_GetTicks());
  if (CompareTabState* compare = ActiveCompareTab();
      compare != nullptr && (compare->hover_state.has_value() || compare->review_hover_visible_row >= 0)) {
    compare->hover_state.reset();
    compare->review_hover_visible_row = -1;
    RequestPreparedFrame();
  }

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

bool WorkspaceShell::PointerOver(const SDL_FRect& rect) const {
  if (!last_mouse_position_valid_ || !Contains(rect, last_mouse_x_, last_mouse_y_)) {
    return false;
  }
  if (!frame_state_.pointer_occluder.has_value()) {
    return true;
  }
  const SDL_FRect& occluder = *frame_state_.pointer_occluder;
  return occluder.w > 0.0f && occluder.h > 0.0f && rect.x >= occluder.x && rect.y >= occluder.y &&
         rect.x + rect.w <= occluder.x + occluder.w && rect.y + rect.h <= occluder.y + occluder.h;
}

namespace {

constexpr std::optional<SDL_FRect> kNothingHovered{};

}

std::optional<SDL_FRect> WorkspaceShell::HoveredInteractiveRect(const WorkspaceLayout& layout,
                                                               float x, float y) const {
  if (const auto hovered = HoverOverOverlayAndWidgets(layout, x, y)) {
    return *hovered;
  }
  if (const auto hovered = HoverOverChrome(layout, x, y)) {
    return *hovered;
  }
  if (const auto hovered = HoverOverSidebar(layout, x, y)) {
    return *hovered;
  }
  return HoverOverPanels(layout, x, y);
}

std::optional<std::optional<SDL_FRect>> WorkspaceShell::HoverOverOverlayAndWidgets(const WorkspaceLayout& layout,
                                                                                   float x, float y) const {
  if (context_.current_project_state.overlay.visible) {
    const OverlayMode overlay_mode = context_.current_project_state.overlay.mode;
    if (overlay_mode != OverlayMode::BufferSearch && overlay_mode != OverlayMode::BufferReplace) {
      const SDL_FRect overlay = ComputeOverlayRect(layout.overlay_anchor);
      if (Contains(overlay, x, y)) {
        const auto list_layout = ComputeOverlayListLayout(overlay);
        if (const auto idx = ScrollableListItemAt(list_layout, x, y, OverlayItemCount());
            idx.has_value()) {
          return ScrollableListItemRect(list_layout, *idx);
        }
        return kNothingHovered;
      }
    }
  }

  {
    std::optional<std::optional<SDL_FRect>> floating;
    const auto probe = [&](const SDL_FRect& card, std::span<const SDL_FRect> buttons) {
      if (floating.has_value() || card.w <= 0.0f || card.h <= 0.0f || !Contains(card, x, y)) {
        return;
      }
      floating.emplace();
      for (const SDL_FRect& button : buttons) {
        if (Contains(button, x, y)) {
          *floating = button;
          return;
        }
      }
    };
    const auto probe_find_widget = [&](const FindWidgetLayout& fw) {
      std::array<SDL_FRect, kFindWidgetMaxToggles + 5> buttons{};
      std::size_t count = 0;
      for (std::size_t i = 0; i < fw.toggle_count; ++i) {
        buttons[count++] = fw.toggle_buttons[i];
      }
      buttons[count++] = fw.prev_button;
      buttons[count++] = fw.next_button;
      buttons[count++] = fw.close_button;
      if (fw.replace_mode) {
        buttons[count++] = fw.replace_button;
        buttons[count++] = fw.replace_all_button;
      }
      probe(fw.widget, std::span<const SDL_FRect>(buttons.data(), count));
    };
    const OverlayState& overlay = context_.current_project_state.overlay;
    if (overlay.visible &&
        (overlay.mode == OverlayMode::BufferSearch || overlay.mode == OverlayMode::BufferReplace)) {
      probe_find_widget(ComputeBufferFindWidgetLayout(
          ActiveTabSurface(layout), overlay.mode == OverlayMode::BufferReplace));
    }
    if (BottomPanelVisible() && terminal_find_service_.visible()) {
      probe_find_widget(ComputeTerminalFindWidgetLayout(ActiveTerminalBodyRect(layout), layout));
    }
    if (DebugToolbarVisible()) {
      const DebugToolbarLayout tb = ComputeDebugToolbarLayout(
          ActiveTabSurface(layout), DebugToolbarAvoidBelowY(layout), debug_service_.SupportsStepBack());
      probe(tb.widget, std::span<const SDL_FRect>(tb.buttons.data(), tb.button_count));
    }
    if (floating.has_value()) {
      return *floating;
    }
  }

  return std::nullopt;
}

std::optional<std::optional<SDL_FRect>> WorkspaceShell::HoverOverChrome(const WorkspaceLayout& layout, float x,
                                                                        float y) const {
  if (layout.menu_bar.w > 0.0f && Contains(layout.menu_bar, x, y)) {
    for (const VisibleMenuBarItem& item : ComputeVisibleMenuBarItems(layout.menu_bar)) {
      if (Contains(item.rect, x, y)) {
        return item.rect;
      }
    }
    for (const VisibleWindowControlButton& button : ComputeVisibleWindowControlButtons(layout.menu_bar)) {
      if (Contains(button.rect, x, y)) {
        return button.rect;
      }
    }
    if (const auto chevron = MenuOverflowChevronRect(layout.menu_bar); chevron.has_value() && Contains(*chevron, x, y)) {
      return *chevron;
    }
    return kNothingHovered;
  }

  if (layout.breadcrumb.w > 0.0f && Contains(layout.breadcrumb, x, y)) {
    for (const VisibleStatusItem& item : ComputeVisibleStatusItems(layout.breadcrumb)) {
      if (Contains(item.rect, x, y)) {
        return item.rect;
      }
    }
  }

  if (layout.project_tab_strip.w > 0.0f && Contains(layout.project_tab_strip, x, y)) {
    const auto& project_tabs = tab_strip_chrome_.ComputeVisibleProjectTabs(layout.project_tab_strip);
    for (const VisibleStripTab& tab : project_tabs) {
      if (Contains(tab.rect, x, y)) {
        return tab.rect;
      }
    }
    return OverflowChipAt(
        tab_strip_chrome_.ComputeProjectTabOverflowControls(layout.project_tab_strip, project_tabs),
        x, y);
  }

  {
    const EditorGroupRectsLayout group_rects = ComputeEditorGroupRectsForState(layout);
    for (std::size_t gi = 0; gi < group_rects.groups.size(); ++gi) {
      const SDL_FRect strip = group_rects.groups[gi].tab_strip;
      if (strip.w <= 0.0f || strip.h <= 0.0f || !Contains(strip, x, y)) {
        continue;
      }
      const auto& tabs = tab_strip_chrome_.ComputeVisibleTabsForGroup(gi, strip);
      for (const VisibleStripTab& tab : tabs) {
        if (Contains(tab.rect, x, y)) {
          return tab.rect;
        }
      }
      return OverflowChipAt(tab_strip_chrome_.ComputeTabOverflowControlsForGroup(gi, strip, tabs),
                            x, y);
    }
  }

  if (const EditorBannerState* banner = ActiveEditorBannerForTab(context_.current_project_state);
      banner != nullptr) {
    const SDL_FRect strip = ActiveEditorBannerStripRect(context_.current_project_state,
                                                        ComputeEditorGroupRectsForState(layout));
    if (Contains(strip, x, y)) {
      const EditorBannerButtonLayout buttons =
          ComputeEditorBannerButtonRects(strip, EditorBannerButtonsFor(banner->kind));
      for (std::size_t i = 0; i < buttons.buttons.size(); ++i) {
        if (Contains(buttons.rects[i], x, y)) {
          return buttons.rects[i];
        }
      }
      if (Contains(buttons.dismiss, x, y)) {
        return buttons.dismiss;
      }
      return kNothingHovered;
    }
  }

  {
    const ProjectWorkspaceState& state = context_.current_project_state;
    const EditorGroupRectsLayout group_rects = ComputeEditorGroupRectsForState(layout);
    for (std::size_t gi = 0; gi < group_rects.groups.size() && gi < state.editor_groups.size(); ++gi) {
      const SDL_FRect& surface = group_rects.groups[gi].editor_surface;
      const EditorGroup& group = state.editor_groups[gi];
      if (!Contains(surface, x, y) || !group.has_active_tab() ||
          group.active_tab().kind != TabEntry::Kind::Merge || !group.active_tab().merge.has_value()) {
        continue;
      }
      const MergeTabState& merge_tab = *group.active_tab().merge;
      const MergeSurfaceLayout merge_surface = ComputeMergeSurfaceLayout(surface, merge_tab);
      const MergeToolbarLayout toolbar = ComputeMergeToolbarLayout(surface, merge_surface);
      for (const SDL_FRect& rect : {toolbar.prev_rect, toolbar.next_rect, toolbar.open_rect, toolbar.save_rect}) {
        if (Contains(rect, x, y)) {
          return rect;
        }
      }
      for (const std::string_view label : {"Unresolved", "Toggle Base", "Mark Resolved"}) {
        if (const auto rect = ComputeMergeSecondaryToolbarButtonRect(surface, merge_surface, label);
            rect.has_value() && Contains(*rect, x, y)) {
          return rect;
        }
      }
    }
  }

  return std::nullopt;
}

std::optional<std::optional<SDL_FRect>> WorkspaceShell::HoverOverSidebar(const WorkspaceLayout& layout, float x,
                                                                         float y) const {
  if (context_.current_project_state.sidebar.visible && Contains(layout.sidebar, x, y)) {
    if (!MenuSurfaceCapturingMouse()) {
      const auto first_containing = [x, y](std::initializer_list<std::optional<SDL_FRect>> rects)
          -> std::optional<SDL_FRect> {
        for (const std::optional<SDL_FRect>& rect : rects) {
          if (rect.has_value() && Contains(*rect, x, y)) {
            return rect;
          }
        }
        return kNothingHovered;
      };
      const SidebarModeRowLayout mode_row = SidebarModeRow(layout.sidebar);
      for (int i = 0; i < mode_row.tab_count; ++i) {
        if (Contains(mode_row.tabs[static_cast<std::size_t>(i)].rect, x, y)) {
          return mode_row.tabs[static_cast<std::size_t>(i)].rect;
        }
      }
      if (mode_row.has_overflow && Contains(mode_row.overflow_rect, x, y)) {
        return mode_row.overflow_rect;
      }
      const SDL_FRect sidebar = layout.sidebar;
      std::optional<SDL_FRect> button;
      switch (ActiveSidebarMode()) {
        case SidebarMode::Search:
          button = first_containing({project_search_panel::ModeButtonRect(sidebar),
                                     project_search_panel::CaseButtonRect(sidebar),
                                     project_search_panel::HiddenButtonRect(sidebar),
                                     project_search_panel::ScopeButtonRect(sidebar),
                                     project_search_panel::ReplaceAllButtonRect(sidebar)});
          break;
        case SidebarMode::Git: {
          const auto& workflow = context_.current_project_state.sidebar.git.commit_workflow;
          const auto if_enabled = [](bool enabled, SDL_FRect rect) {
            return enabled ? std::optional<SDL_FRect>(rect) : std::nullopt;
          };
          button = first_containing(
              {if_enabled(CanStageAllGitSidebarEntries(), git_sidebar_header::StageAllButtonRect(sidebar)),
               if_enabled(CanDiscardAllGitSidebarEntries(),
                          git_sidebar_header::DiscardAllButtonRect(sidebar)),
               git_sidebar_header::BranchButtonRect(sidebar), git_sidebar_header::SyncButtonRect(sidebar),
               git_sidebar_header::RefreshButtonRect(sidebar),
               if_enabled(CanOpenGitCommitButton(), git_sidebar_header::CommitButtonRect(sidebar)),
               if_enabled(workflow.open, workflow.commit_button_rect),
               GitSidebarOutgoingBaseButtonRect(sidebar)});
          break;
        }
        case SidebarMode::Tree:
          button = first_containing(
              {context_.current_project_state.directory_tree.CanCollapseAll()
                   ? std::optional<SDL_FRect>(TreeSidebarCollapseButtonRect(sidebar))
                   : std::nullopt,
               TreeSidebarRefreshButtonRect(sidebar)});
          break;
        default:
          break;
      }
      if (button.has_value()) {
        return button;
      }
    }
    const auto row_band = [&](const ScrollableListLayout& list_layout,
                              std::size_t count) -> std::optional<SDL_FRect> {
      const auto idx = ScrollableListItemAt(list_layout, x, y, count);
      return idx.has_value() ? std::optional<SDL_FRect>(ScrollableListItemRect(list_layout, *idx))
                             : std::nullopt;
    };
    switch (ActiveSidebarMode()) {
      case SidebarMode::Git: {
        const PaintedGitSidebarList painted = PaintedGitSidebarListOf(frame_state_);
        if (painted.lines != nullptr) {
          return row_band(*painted.layout, painted.lines->size());
        }
        const auto& lines = BuildGitSidebarLines();
        return row_band(ComputeGitSidebarListLayout(layout.sidebar, lines.size()), lines.size());
      }
      case SidebarMode::Search: {
        const auto line_map = BuildProjectSearchLineMap();
        return row_band(ComputeProjectSearchSidebarListLayout(layout.sidebar, line_map.size()),
                        line_map.size());
      }
      case SidebarMode::Plugin:
      case SidebarMode::Outline:
      case SidebarMode::HostChanges:
      case SidebarMode::Host:
      case SidebarMode::Conflicts: {
        const std::size_t count = context_.current_project_state.sidebar.plugin.items.size();
        return row_band(ComputePluginSidebarListLayout(layout.sidebar, count), count);
      }
      case SidebarMode::Tree: {
        const auto& entries = context_.current_project_state.directory_tree.entries();
        return row_band(ComputeTreeSidebarListLayout(layout.sidebar, entries.size()),
                        entries.size());
      }
      case SidebarMode::Problems: {
        const std::size_t count = context_.current_project_state.sidebar.problems.entries.size();
        return row_band(ComputeProblemsSidebarListLayout(layout.sidebar, count), count);
      }
      case SidebarMode::Tests: {
        const std::size_t count = context_.current_project_state.sidebar.tests.entries.size();
        return row_band(ComputeTestsSidebarListLayout(layout.sidebar, count), count);
      }
      case SidebarMode::None:
        break;
    }
    return kNothingHovered;
  }

  return std::nullopt;
}

std::optional<SDL_FRect> WorkspaceShell::HoverOverPanels(const WorkspaceLayout& layout, float x, float y) const {
  if (BottomPanelVisible() && Contains(layout.bottom_panel, x, y)) {
    const SDL_FRect panel_header =
        MakeRect(layout.bottom_panel.x, layout.bottom_panel.y, layout.bottom_panel.w,
                 kWorkspaceBottomPanelHeaderHeight);
    if (Contains(panel_header, x, y)) {
      const auto& panel_tabs = tab_strip_service_.ComputeVisibleBottomPanelTabs(
          context_.current_project_state, panel_header, layout_mode_service_.CurrentMode(),
          [this](std::string_view text) { return text_renderer_.MeasureWidth(text); },
          output_channels_.Channels());
      for (const VisibleStripTab& tab : panel_tabs) {
        if (Contains(tab.rect, x, y)) {
          return tab.rect;
        }
      }
      if (const auto chip = OverflowChipAt(
              tab_strip_service_.ComputeBottomPanelTabOverflowControls(
                  context_.current_project_state, panel_header, layout_mode_service_.CurrentMode(),
                  panel_tabs, output_channels_.Channels()),
              x, y)) {
        return chip;
      }
      const LayoutMode mode = layout_mode_service_.CurrentMode();
      for (const SDL_FRect& button :
           {tab_strip_service_.BottomPanelTerminalNewTabRect(mode, panel_header),
            tab_strip_service_.BottomPanelMaximizeButtonRect(mode, panel_header)}) {
        if (Contains(button, x, y)) {
          return button;
        }
      }
    }
    return std::nullopt;
  }

  if (context_.current_project_state.debug_pane.visible && layout.right_pane.w > 0.0f &&
      Contains(layout.right_pane, x, y)) {
    const DebugPaneModeRowLayout modes = DebugPaneModeRow(layout.right_pane);
    for (int i = 0; i < modes.tab_count; ++i) {
      if (Contains(modes.tabs[static_cast<std::size_t>(i)].rect, x, y)) {
        return modes.tabs[static_cast<std::size_t>(i)].rect;
      }
    }
    const std::size_t row_count = DebugPaneActiveRowCount();
    const auto panel_layout = ComputeDebugPaneListLayout(layout, row_count);
    const float band_top = DebugPaneRowBandTop(panel_layout.text_y, panel_layout.line_height, 0);
    if (panel_layout.line_height > 0.0f && Contains(panel_layout.content_rect, x, y) && y >= band_top) {
      const int visible_row = static_cast<int>((y - band_top) / panel_layout.line_height);
      return MakeRect(panel_layout.content_rect.x,
                      DebugPaneRowBandTop(panel_layout.text_y, panel_layout.line_height, visible_row),
                      panel_layout.content_rect.w, panel_layout.line_height);
    }
  }

  return std::nullopt;
}

bool WorkspaceShell::MenuSurfaceCapturingMouse() const {
  return context_.menu_state.menu_bar_open || context_.menu_state.overflow_popup_open ||
         context_.menu_state.tree_context_menu.open;
}

void WorkspaceShell::UpdateMouseCursor(float x, float y, bool update_editor_hover,
                                       bool workspace_layout_recomputed,
                                       bool during_frame_prepare) {
  util::PerformanceTrace::Scope perf_scope("WorkspaceShell::UpdateMouseCursor");
  const bool mouse_moved =
      !last_mouse_position_valid_ || x != last_mouse_x_ || y != last_mouse_y_;
  last_mouse_x_ = x;
  last_mouse_y_ = y;
  last_mouse_position_valid_ = true;
  if (!during_frame_prepare) {
    if (update_editor_hover && (mouse_moved || editor_hover_refresh_pending_ || workspace_layout_recomputed)) {
      util::PerformanceTrace::Scope scope("WorkspaceShell::UpdateMouseCursor::UpdateEditorHover");
      UpdateEditorHover(x, y);
    } else if (!update_editor_hover) {
      active_editor_hover_target_.reset();
      editor_hover_refresh_pending_ = false;
    }
  }

  const CursorKindFingerprint next_fp{
      .x = x,
      .y = y,
      .valid = true,
      .hit_generation = cursor_hit_generation_,
      .hover_generation = editor_hover_target_generation_,
  };
  if (!force_cursor_reassert_ &&
      cursor_kind_fingerprint_.valid &&
      cursor_kind_fingerprint_.x == next_fp.x &&
      cursor_kind_fingerprint_.y == next_fp.y &&
      cursor_kind_fingerprint_.hit_generation == next_fp.hit_generation &&
      cursor_kind_fingerprint_.hover_generation == next_fp.hover_generation &&
      !workspace_layout_recomputed) {
    return;
  }
  cursor_kind_fingerprint_ = next_fp;

  const CursorKind next_kind = CursorKindForPosition(x, y);
  const bool force = force_cursor_reassert_;
  force_cursor_reassert_ = false;
  if (next_kind == cursor_kind_ && !force) {
    return;
  }

  cursor_kind_ = next_kind;
  SDL_Cursor* cursor = CursorHandle(next_kind);
  if (cursor == nullptr) {
    cursor = CursorHandle(CursorKind::Default);
  }
  if (cursor == nullptr) {
    return;
  }

  if (force) {
    SDL_Cursor* nudge =
        cursor == SDL_GetDefaultCursor() ? CursorHandle(CursorKind::Text) : SDL_GetDefaultCursor();
    if (nudge != nullptr && nudge != cursor) {
      (void)SDL_SetCursor(nudge);
    }
  }
  (void)SDL_SetCursor(cursor);

  if (!during_frame_prepare) {
    RequestCursorPresent();
  }
}

std::optional<SDL_FRect> WorkspaceShell::FocusSurfaceRect(const WorkspaceLayout& layout,
                                                          FocusTarget focus) const {
  switch (focus) {
    case FocusTarget::Sidebar:
      return context_.current_project_state.sidebar.visible
                 ? std::optional<SDL_FRect>(layout.sidebar)
                 : std::nullopt;
    case FocusTarget::Editor:
      return layout.editor_surface;
    case FocusTarget::Panel:
      return BottomPanelVisible() ? std::optional<SDL_FRect>(layout.bottom_panel) : std::nullopt;
    case FocusTarget::DebugPane:
      return context_.current_project_state.debug_pane.visible && layout.right_pane.w > 0.0f
                 ? std::optional<SDL_FRect>(layout.right_pane)
                 : std::nullopt;
    case FocusTarget::Overlay:
    default:
      return std::nullopt;
  }
}

std::optional<SDL_FRect> WorkspaceShell::HoveredStatusBarSegmentRect(
    const WorkspaceLayout& layout, float x, float y) const {
  if (layout.status_bar.w <= 0.0f || layout.status_bar.h <= 0.0f ||
      !Contains(layout.status_bar, x, y)) {
    return std::nullopt;
  }
  const StatusBarViewModel vm =
      RenderViewModelBuilder(context_).BuildStatusBar(layout, status_bar_service_);
  std::optional<SDL_FRect> hovered;
  ForEachStatusBarSegmentRect(
      vm, text_renderer_, [&](const StatusBarSegmentViewModel& segment, const SDL_FRect& row) {
        if (!hovered.has_value() && segment.clickable && Contains(row, x, y)) {
          hovered = row;
        }
      });
  return hovered;
}

}
