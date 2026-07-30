#include "workspace/DiffDividerGeometry.h"
#include "workspace/WorkspaceShell.h"

#include "workspace/ProjectSearchPanelLayout.h"
#include "workspace/GitSidebarHeaderLayout.h"

#include <algorithm>
#include <cctype>
#include <string_view>

#include "util/PerformanceTrace.h"
#include "workspace/CompareMergeRender.h"
#include "workspace/CompareTabReview.h"
#include "workspace/PluginSurfacePreview.h"
#include "workspace/SettingsOverlayService.h"
#include "workspace/WorkspaceLayout.h"
#include "workspace/WorkspaceOutputReference.h"
#include "util/StringUtil.h"
#include "workspace/NotificationLayout.h"

namespace microide::workspace {

namespace {

constexpr float kWindowFrameHitThickness = 6.0f;

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
                    layout_mode_service_.StatusBarVisible(),
                    context_.current_project_state.debug_pane.visible,
                    context_.current_project_state.debug_pane.width,
                    ProjectTabStripVisible());
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
  // A divider drag holds its resize cursor for the whole gesture, wherever the
  // pointer travels — dragging necessarily moves off the divider, so resolving the
  // cursor from the position under it would drop the shape mid-drag. This used to
  // cover only the sidebar and bottom panel, so the other five resize drags
  // (right pane, editor split, compare, and both merge dividers) flickered back to
  // an arrow the moment the pointer left the thin divider band.
  switch (context_.interaction_state.drag_target) {
    case DragTarget::SidebarDivider:
    case DragTarget::RightPaneDivider:
    case DragTarget::EditorSplitDivider:
    case DragTarget::CompareDivider:
    case DragTarget::MergeLeftDivider:
    case DragTarget::MergeRightDivider:
      return CursorKind::EwResize;
    case DragTarget::BottomPanelDivider:
      return CursorKind::NsResize;
    default:
      break;
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

  // Toasts float above every surface and a click dismisses one, so the pointer has
  // to say so — probed ahead of the modal surfaces for the same reason it is
  // probed first on the click path.
  if (!notification_service_.Empty() &&
      NotificationToastIndexAt(notification_service_, text_renderer_, layout.status_bar, x, y)
          .has_value()) {
    return CursorKind::Pointer;
  }

  if (settings_overlay_service_.Visible()) {
    const SettingsOverlayViewModel vm =
        RenderViewModelBuilder(context_).BuildSettingsOverlay(layout, settings_overlay_service_,
                                                              text_renderer_);
    if (!Contains(vm.rect, x, y)) {
      return CursorKind::Default;
    }
    if (vm.mode != SettingsOverlayMode::Settings) {
      return CursorKind::Default;
    }
    // The font-picker dropdown floats over the value rows, so it is probed first —
    // the same order the click handler uses. Without this its rows fell through to
    // whatever row happened to be underneath, which is how a click-through
    // mismatch hides: the pointer looked right over a family but came from the
    // wrong surface, and over the pinned "Choose file…" footer it could be an
    // arrow instead.
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
    const OverlayMode overlay_mode = context_.current_project_state.overlay.mode;
    // The find / replace widget is non-modal: it only governs the cursor while the
    // pointer is over its small rect. Off the widget we fall through to the normal
    // editor cursor logic so the live editor underneath behaves as usual.
    if (overlay_mode == OverlayMode::BufferSearch || overlay_mode == OverlayMode::BufferReplace) {
      const FindWidgetLayout fw =
          ComputeBufferFindWidgetLayout(layout.editor_surface, overlay_mode == OverlayMode::BufferReplace);
      if (Contains(fw.search_field, x, y) ||
          (fw.replace_mode && Contains(fw.replace_field, x, y))) {
        return CursorKind::Text;
      }
      if (Contains(fw.widget, x, y)) {
        return CursorKind::Pointer;
      }
      // Off the widget: continue to the editor / chrome cursor logic below.
    } else {
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

      if (overlay_mode == OverlayMode::CommitPicker ||
          overlay_mode == OverlayMode::LaunchConfigPicker ||
          overlay_mode == OverlayMode::CommandPalette) {
        // Matches the picker query field y (overlay.y + 52) in WorkspaceShellRenderOverlay.cpp.
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

  // Breadcrumb band: a plugin status item with a bound command is clickable (see
  // the click handler in WorkspaceShellMouse.cpp). Match it exactly — only
  // command-bound items get the pointer; bare path text and command-less items
  // keep the arrow.
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

  // Editor tab strips are per-group. A split lays out one strip per group, either
  // side by side (within the top tab band) or stacked (the second strip is
  // synthesized inside the editor surface). Probe every group's own strip rect with
  // its *ForGroup tab geometry, mirroring the renderer (WorkspaceShellRenderChrome)
  // and the click handler (WorkspaceTabMouseCoordinator), so tabs in a non-focused
  // group resolve correctly and no phantom tab geometry appears over an empty strip.
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
      const auto tabs = tab_strip_chrome_.ComputeVisibleTabsForGroup(gi, group_tab_strip);
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
          Contains(project_search_panel::CaseButtonRect(layout.sidebar), x, y) ||
          Contains(project_search_panel::HiddenButtonRect(layout.sidebar), x, y) ||
          Contains(project_search_panel::ScopeButtonRect(layout.sidebar), x, y)) {
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
      // Top-level Commit button (shown before the commit draft opens — see
      // GitSidebarCommandCenter, show_commit_button = repo_available && !workflow.open).
      // It is clearly clickable, so it must claim the pointer like every other action.
      if (const auto& git_state = context_.current_project_state.sidebar.git;
          git_state.repo_available && !git_state.commit_workflow.open &&
          Contains(git_sidebar_header::CommitButtonRect(layout.sidebar), x, y)) {
        return CursorKind::Pointer;
      }
      // Commit workflow (open after the user begins a commit): the subject/body are
      // text inputs and the confirm button is clickable. The renderer caches these
      // rects each frame; mirror the click handlers (WorkspaceShellSingleLineInputMouse
      // / WorkspaceSidebarMouseCoordinator) so the cursor matches what is actionable.
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
      const auto& lines = BuildGitSidebarLines();
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
      if (line.kind == GitSidebarLine::Kind::Directory) {
        return CursorKind::Pointer;
      }
      if (line.kind != GitSidebarLine::Kind::Entry || line.entry_index < 0) {
        return CursorKind::Default;
      }
      // Whole entry row is clickable (select + open diff; right-click for the
      // action menu), so any hit over the row shows the pointer cursor.
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
    if (sidebar_mode == SidebarMode::Plugin || sidebar_mode == SidebarMode::Outline) {
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

  if (context_.current_project_state.debug_pane.visible && layout.right_pane.w > 0.0f &&
      Contains(layout.right_pane, x, y)) {
    // Mode-row tab buttons are always clickable.
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
    // Plugin surface preview: pointer over a published hit region (TD-2026-07-16-61).
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

  // Floating debug toolbar (continue / step / stop) renders over the top of the
  // editor and must claim the cursor before the editor surface does.
  if (DebugToolbarVisible()) {
    const DebugToolbarLayout toolbar = ComputeDebugToolbarLayout(
        layout.editor_surface, DebugToolbarAvoidBelowY(layout), DebugSupportsReverse());
    if (Contains(toolbar.widget, x, y)) {
      for (std::size_t i = 0; i < toolbar.button_count; ++i) {
        if (Contains(toolbar.buttons[i], x, y)) {
          return CursorKind::Pointer;
        }
      }
      return CursorKind::Default;  // card padding between buttons
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
    const int hovered_row =
        static_cast<int>((y - surface_layout.rows_y) / surface_layout.line_height);
    const int presentation_row = compare_tab->scroll_row + hovered_row;
    if (hovered_row >= 0 && presentation_row >= 0 &&
        static_cast<std::size_t>(presentation_row) < CompareTabPresentationRowCount(*compare_tab)) {
      if (const compare::ComparePresentationRow* row =
              CompareTabPresentationRowAt(*compare_tab, static_cast<std::size_t>(presentation_row));
          row != nullptr && row->kind == compare::ComparePresentationRowKind::CollapsedContext) {
        const SDL_FRect block_rect = CompareCollapsedContextBlockRect(
            layout.editor_surface, surface_layout.rows_y, surface_layout.line_height,
            surface_layout.show_vertical, hovered_row);
        const auto action_rects = BuildCollapsedContextActionRects(
            text_renderer_, block_rect, row->previous_hunk_index >= 0, row->next_hunk_index >= 0);
        if ((action_rects.previous_rect.has_value() && Contains(*action_rects.previous_rect, x, y)) ||
            Contains(action_rects.all_rect, x, y) ||
            (action_rects.next_rect.has_value() && Contains(*action_rects.next_rect, x, y))) {
          return CursorKind::Pointer;
        }
      }
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
    const auto divider_rects = MergeDividerHitRects(layout.editor_surface, surface_layout);
    if (Contains(divider_rects[0], x, y) || Contains(divider_rects[1], x, y)) {
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
        editor::EditorViewRenderer::ComputeMetrics(text_renderer_, merge_tab->result_viewport,
                                                   result_rect, 0, LineNumbersEnabled());
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

  // Welcome home surface: with no project open there are no editor panes, so the
  // pane-based hit-testing below returns early without ever probing the welcome regions.
  // Probe here (after compare/merge, which return inside their own branches; mirroring the
  // click handler in WorkspaceShellMouse.cpp) so the pointer shows over clickable recents /
  // the open-folder row, and the I-beam elsewhere on the surface.
  {
    editor::WelcomeViewModel welcome_model;
    editor::WelcomeLayout welcome_layout;
    if (ProbeWelcomeSurface(&welcome_model, &welcome_layout)) {
      for (const editor::WelcomeHitRegion& region : welcome_layout.hit_regions) {
        if (Contains(region.rect, x, y)) {
          return CursorKind::Pointer;
        }
      }
      return CursorKind::Text;
    }
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

  const editor::TextViewport* viewport = ViewportForPane(*pane_it);
  if (viewport == nullptr || viewport->is_placeholder()) {
    // Welcome surface is hit-tested above (before the pane logic), so any placeholder
    // pane here just shows the text caret.
    return CursorKind::Text;
  }

  const editor::EditorViewMetrics metrics = editor::EditorViewRenderer::ComputeMetrics(
      text_renderer_, *viewport, pane_it->rect, 0, LineNumbersEnabled());
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
      // `viewport` is already this pane's group viewport, which carries the
      // FoldingModel whose visual-row layout matches what's painted in the pane.
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
    case CursorKind::NResize:
      if (n_resize_cursor_ == nullptr) {
        n_resize_cursor_ = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_N_RESIZE);
      }
      return n_resize_cursor_;
    case CursorKind::EResize:
      if (e_resize_cursor_ == nullptr) {
        e_resize_cursor_ = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_E_RESIZE);
      }
      return e_resize_cursor_;
    case CursorKind::SResize:
      if (s_resize_cursor_ == nullptr) {
        s_resize_cursor_ = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_S_RESIZE);
      }
      return s_resize_cursor_;
    case CursorKind::WResize:
      if (w_resize_cursor_ == nullptr) {
        w_resize_cursor_ = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_W_RESIZE);
      }
      return w_resize_cursor_;
    case CursorKind::NeResize:
      if (ne_resize_cursor_ == nullptr) {
        ne_resize_cursor_ = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_NE_RESIZE);
      }
      return ne_resize_cursor_;
    case CursorKind::SeResize:
      if (se_resize_cursor_ == nullptr) {
        se_resize_cursor_ = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_SE_RESIZE);
      }
      return se_resize_cursor_;
    case CursorKind::SwResize:
      if (sw_resize_cursor_ == nullptr) {
        sw_resize_cursor_ = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_SW_RESIZE);
      }
      return sw_resize_cursor_;
    case CursorKind::NwResize:
      if (nw_resize_cursor_ == nullptr) {
        nw_resize_cursor_ = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_NW_RESIZE);
      }
      return nw_resize_cursor_;
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

bool WorkspaceShell::PointerOver(const SDL_FRect& rect) const {
  return last_mouse_position_valid_ && Contains(rect, last_mouse_x_, last_mouse_y_);
}

std::optional<SDL_FRect> WorkspaceShell::HoveredInteractiveRect(const WorkspaceLayout& layout,
                                                               float x, float y) const {
  // Menu bar: top-level items lift on hover (the dropdown-open case is handled by the
  // chrome coordinator's menu-motion path, so this only matters when no menu is open).
  if (layout.menu_bar.w > 0.0f && Contains(layout.menu_bar, x, y)) {
    for (const VisibleMenuBarItem& item : ComputeVisibleMenuBarItems(layout.menu_bar)) {
      if (Contains(item.rect, x, y)) {
        return item.rect;
      }
    }
    return std::nullopt;
  }

  // Project tab strip.
  if (layout.project_tab_strip.w > 0.0f && Contains(layout.project_tab_strip, x, y)) {
    for (const VisibleStripTab& tab :
         tab_strip_chrome_.ComputeVisibleProjectTabs(layout.project_tab_strip)) {
      if (Contains(tab.rect, x, y)) {
        return tab.rect;
      }
    }
    return std::nullopt;
  }

  // Per-group editor tab strips.
  {
    const EditorGroupRectsLayout group_rects = ComputeEditorGroupRectsForState(layout);
    for (std::size_t gi = 0; gi < group_rects.groups.size(); ++gi) {
      const SDL_FRect strip = group_rects.groups[gi].tab_strip;
      if (strip.w <= 0.0f || strip.h <= 0.0f || !Contains(strip, x, y)) {
        continue;
      }
      for (const VisibleStripTab& tab : tab_strip_chrome_.ComputeVisibleTabsForGroup(gi, strip)) {
        if (Contains(tab.rect, x, y)) {
          return tab.rect;
        }
      }
      return std::nullopt;
    }
  }

  // Overlay list rows (command palette / pickers / completion / code actions).
  if (context_.current_project_state.overlay.visible) {
    const OverlayMode overlay_mode = context_.current_project_state.overlay.mode;
    if (overlay_mode != OverlayMode::BufferSearch && overlay_mode != OverlayMode::BufferReplace) {
      const SDL_FRect overlay = ComputeOverlayRect(layout.editor_area);
      if (Contains(overlay, x, y)) {
        const auto list_layout = ComputeOverlayListLayout(overlay);
        if (const auto idx = ScrollableListIndexAtY(list_layout, y);
            idx.has_value() && *idx >= 0 && *idx < static_cast<int>(OverlayItemCount())) {
          const SDL_FRect row = ScrollableListRowRect(list_layout, *idx - list_layout.scroll_row);
          if (Contains(row, x, y)) {
            return row;
          }
        }
      }
    }
  }

  // Sidebar list rows for whichever mode is active.
  if (context_.current_project_state.sidebar.visible && Contains(layout.sidebar, x, y)) {
    const auto row_band = [&](const ScrollableListLayout& list_layout,
                              std::size_t count) -> std::optional<SDL_FRect> {
      const auto idx = ScrollableListIndexAtY(list_layout, y);
      if (!idx.has_value() || *idx < 0 || *idx >= static_cast<int>(count)) {
        return std::nullopt;
      }
      const SDL_FRect row = ScrollableListRowRect(list_layout, *idx - list_layout.scroll_row);
      return Contains(row, x, y) ? std::optional<SDL_FRect>(row) : std::nullopt;
    };
    switch (ActiveSidebarMode()) {
      case SidebarMode::Git: {
        const auto& lines = BuildGitSidebarLines();
        return row_band(ComputeGitSidebarListLayout(layout.sidebar, lines.size()), lines.size());
      }
      case SidebarMode::Search: {
        const auto line_map = BuildProjectSearchLineMap();
        return row_band(ComputeProjectSearchSidebarListLayout(layout.sidebar, line_map.size()),
                        line_map.size());
      }
      case SidebarMode::Plugin:
      case SidebarMode::Outline: {
        const std::size_t count = context_.current_project_state.sidebar.plugin.items.size();
        return row_band(ComputePluginSidebarListLayout(layout.sidebar, count), count);
      }
      case SidebarMode::Tree: {
        const auto& entries = context_.current_project_state.directory_tree.entries();
        return row_band(ComputeTreeSidebarListLayout(layout.sidebar, entries.size()),
                        entries.size());
      }
      default:
        return std::nullopt;
    }
  }

  // Bottom-panel header tabs.
  if (BottomPanelVisible() && Contains(layout.bottom_panel, x, y)) {
    const SDL_FRect panel_header =
        MakeRect(layout.bottom_panel.x, layout.bottom_panel.y, layout.bottom_panel.w,
                 kWorkspaceBottomPanelHeaderHeight);
    if (Contains(panel_header, x, y)) {
      for (const VisibleStripTab& tab : tab_strip_service_.ComputeVisibleBottomPanelTabs(
               context_.current_project_state, panel_header, layout_mode_service_.CurrentMode(),
               [this](std::string_view text) { return text_renderer_.MeasureWidth(text); },
               output_channels_.Channels())) {
        if (Contains(tab.rect, x, y)) {
          return tab.rect;
        }
      }
    }
    return std::nullopt;
  }

  // Debug pane rows: quantize the pointer to a fixed-height row band so the band
  // changes as the pointer crosses rows (the render lifts each row on hover).
  if (context_.current_project_state.debug_pane.visible && layout.right_pane.w > 0.0f &&
      Contains(layout.right_pane, x, y)) {
    const std::size_t row_count = DebugPaneActiveRowCount();
    const auto panel_layout = ComputeDebugPaneListLayout(layout, row_count);
    if (panel_layout.line_height > 0.0f && Contains(panel_layout.content_rect, x, y) &&
        y >= panel_layout.text_y) {
      const int visible_row = static_cast<int>((y - panel_layout.text_y) / panel_layout.line_height);
      return MakeRect(panel_layout.content_rect.x,
                      panel_layout.text_y + static_cast<float>(visible_row) * panel_layout.line_height,
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
  if (update_editor_hover &&
      (mouse_moved || editor_hover_refresh_pending_ || workspace_layout_recomputed)) {
    util::PerformanceTrace::Scope scope("WorkspaceShell::UpdateMouseCursor::UpdateEditorHover");
    UpdateEditorHover(x, y);
  } else if (!update_editor_hover) {
    active_editor_hover_target_.reset();
    editor_hover_refresh_pending_ = false;
  }

  // Fast-path: skip the hit-test entirely when neither the pointer position nor any
  // cursor-relevant state changed since the last call. Correctness rests on a single
  // invariant: every redraw that can change a cursor surface bumps one of the two
  // generations (see CursorKindFingerprint in WorkspaceShellMembers.inc for why this
  // subsumes the old hand-maintained scalar allowlist). A still pointer on an idle
  // caret-blink frame (no generation bump) short-circuits here; anything that redrew
  // a surface re-runs CursorKindForPosition.
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
    // SDL_SetCursor no-ops when the cursor handle is unchanged, but SDL's Wayland
    // hit-test path can change the *displayed* cursor (Wayland_ShowCursor) without
    // updating SDL's current-cursor handle. Setting our (unchanged) handle would
    // then no-op and leave the stale cursor on screen. Nudge through a different
    // cursor first so the platform is guaranteed to re-show ours.
    SDL_Cursor* nudge =
        cursor == SDL_GetDefaultCursor() ? CursorHandle(CursorKind::Text) : SDL_GetDefaultCursor();
    if (nudge != nullptr && nudge != cursor) {
      (void)SDL_SetCursor(nudge);
    }
  }
  (void)SDL_SetCursor(cursor);

  // A cursor shape only becomes visible once the compositor recomposites: on
  // Wayland the shape rides a hardware cursor plane that is re-latched on a frame
  // commit, not on the bare wl_pointer.set_cursor request. At event time nothing
  // else may have dirtied the scene (hovering an item with no hover visual, or an
  // idle welcome screen with no caret blink), so request a minimal present here —
  // otherwise the new shape sits queued and the stale one stays on screen until
  // some unrelated repaint. The render-path call already presents this frame, so it
  // skips this. See dev-docs/platform/wayland-stale-cursor.md.
  if (!during_frame_prepare) {
    RequestCursorPresent();
  }
}

char WorkspaceShell::KeycodeToAscii(SDL_Keycode keycode, SDL_Keymod modifiers) {
  const bool shift = (modifiers & SDL_KMOD_SHIFT) != 0;

  if (keycode >= SDLK_A && keycode <= SDLK_Z) {
    const char base = static_cast<char>('a' + (keycode - SDLK_A));
    return shift ? util::ToUpperAsciiChar(static_cast<char>(base)) : base;
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

}  // namespace microide::workspace
