  static bool RestoreSessionState(WorkspaceShell& shell) {
    return shell.MakePersistenceCoordinator().RestoreSessionState();
  }
  static bool RestoreUserConfig(WorkspaceShell& shell) {
    return shell.MakePersistenceCoordinator().RestoreUserConfig();
  }
  static bool RestoreConfigState(WorkspaceShell& shell) {
    return shell.MakePersistenceCoordinator().RestoreConfigState();
  }
  static void SaveSessionState(WorkspaceShell& shell) {
    shell.MakePersistenceCoordinator().SaveSessionState();
  }
  static bool RestoreWorkspaceSession(WorkspaceShell& shell) {
    return shell.MakePersistenceCoordinator().RestoreWorkspaceSession();
  }
  static void SaveWorkspaceSession(WorkspaceShell& shell) {
    shell.MakePersistenceCoordinator().SaveWorkspaceSession();
  }
  static void SaveConfigState(WorkspaceShell& shell) {
    shell.MakePersistenceCoordinator().SaveConfigState();
  }
  static void ConfirmDirtyPrompt(WorkspaceShell& shell, int selected_action) {
    shell.context_.prompts.dirty.selected_action = selected_action;
    shell.ConfirmDirtyPrompt();
  }
  static bool StageAllGitSidebarEntries(WorkspaceShell& shell) {
    return shell.StageAllGitSidebarEntries();
  }
  static bool DirtyPromptVisible(const WorkspaceShell& shell) { return shell.context_.prompts.dirty_visible; }
  static std::string DirtyPromptMessage(const WorkspaceShell& shell) {
    return shell.DirtyPromptMessage();
  }
  static bool PromptSurfaceVisible(const WorkspaceShell& shell) {
    return shell.context_.prompts.surface_visible;
  }
  static SDL_FRect PromptSurfaceInputRect(WorkspaceShell& shell) {
    const WorkspaceLayout layout = CurrentLayout(shell);
    return ComputePromptSurfaceInputRect(ComputePromptSurfaceRect(layout.full));
  }
  static const std::string& PromptSurfaceInput(const WorkspaceShell& shell) {
    return shell.context_.prompts.surface.input.text();
  }
  static std::size_t PromptSurfaceInputCaret(const WorkspaceShell& shell) {
    return shell.context_.prompts.surface.input.caret();
  }
  static std::optional<std::pair<std::size_t, std::size_t>> PromptSurfaceInputSelection(
      const WorkspaceShell& shell) {
    const auto selection = shell.context_.prompts.surface.input.Selection();
    if (!selection.has_value()) {
      return std::nullopt;
    }
    return std::make_pair(selection->start, selection->end);
  }
  static std::string PromptSurfaceTitle(const WorkspaceShell& shell) {
    return shell.PromptSurfaceTitle();
  }
  static std::string PromptSurfaceMessage(const WorkspaceShell& shell) {
    return shell.PromptSurfaceMessage();
  }
  static std::string PromptSurfaceDetail(const WorkspaceShell& shell) {
    return shell.PromptSurfaceDetail();
  }
  static int PromptSurfaceButtonCount(const WorkspaceShell& shell) {
    return shell.context_.prompts.surface.button_count;
  }
  static const std::vector<WorkspaceShell::TabEntry>& OpenTabs(const WorkspaceShell& shell) {
    return shell.context_.current_project_state.open_tabs;
  }
  static const std::vector<ResolvedKeybinding>& ResolvedKeybindings(WorkspaceShell& shell) {
    return shell.ResolvedKeybindings();
  }
  static void SetDisabledKeybindingIds(WorkspaceShell& shell, std::vector<std::string> ids) {
    shell.context_.disabled_keybinding_ids = std::move(ids);
  }
  static std::size_t ActiveTabIndex(const WorkspaceShell& shell) { return shell.context_.current_project_state.active_tab_index; }
  static std::size_t ActiveTerminalTabIndex(const WorkspaceShell& shell) {
    return shell.context_.current_project_state.active_terminal_tab_index;
  }
  static std::vector<std::filesystem::path> ProjectRoots(const WorkspaceShell& shell) {
    std::vector<std::filesystem::path> roots;
    roots.reserve(shell.context_.project_catalog.entries.size());
    for (std::size_t i = 0; i < shell.context_.project_catalog.entries.size(); ++i) {
      roots.push_back(shell.ProjectCatalogRoot(i));
    }
    return roots;
  }
  static std::vector<std::string> TerminalLaunchLabels(WorkspaceShell& shell) {
    std::vector<std::string> labels;
    labels.reserve(shell.context_.current_project_state.terminal_tabs.size());
    for (const auto& terminal_tab : shell.context_.current_project_state.terminal_tabs) {
      labels.push_back(terminal_tab == nullptr ? std::string{} : terminal_tab->session.LaunchLabel());
    }
    return labels;
  }
  static std::vector<std::string> VisibleMenuBarLabels(WorkspaceShell& shell) {
    const WorkspaceLayout layout = CurrentLayout(shell);
    std::vector<std::string> labels;
    for (const auto& item : shell.ComputeVisibleMenuBarItems(layout.menu_bar)) {
      if (const auto* menu = shell.FindMenuSpec(item.id); menu != nullptr) {
        labels.emplace_back(menu->label);
      }
    }
    return labels;
  }
  static std::vector<std::string> OverflowMenuBarLabels(WorkspaceShell& shell) {
    const WorkspaceLayout layout = CurrentLayout(shell);
    std::vector<std::string> labels;
    for (const MenuId id : shell.ComputeOverflowMenuBarItems(layout.menu_bar)) {
      if (const auto* menu = shell.FindMenuSpec(id); menu != nullptr) {
        labels.emplace_back(menu->label);
      }
    }
    return labels;
  }
  static std::optional<SDL_FRect> MenuOverflowChevronRect(WorkspaceShell& shell) {
    const WorkspaceLayout layout = CurrentLayout(shell);
    return shell.MenuOverflowChevronRect(layout.menu_bar);
  }
  static bool MenuOverflowPopupOpen(const WorkspaceShell& shell) {
    return shell.context_.menu_state.overflow_popup_open;
  }
  static std::optional<SDL_FRect> MenuOverflowPopupRect(WorkspaceShell& shell) {
    const WorkspaceLayout layout = CurrentLayout(shell);
    if (!shell.context_.menu_state.overflow_popup_anchor_rect.has_value()) {
      return std::nullopt;
    }
    return ComputeMenuOverflowPopupRect(*shell.context_.menu_state.overflow_popup_anchor_rect,
                                        shell.ComputeOverflowMenuBarItems(layout.menu_bar).size());
  }
  static std::optional<SDL_FRect> MenuBarItemRect(WorkspaceShell& shell, std::string_view label) {
    const WorkspaceLayout layout = CurrentLayout(shell);
    for (const auto& item : shell.ComputeVisibleMenuBarItems(layout.menu_bar)) {
      if (const auto* menu = shell.FindMenuSpec(item.id);
          menu != nullptr && menu->label == label) {
        return item.rect;
      }
    }
    return std::nullopt;
  }
  static std::vector<std::string> VisiblePopupMenuLabels(WorkspaceShell& shell,
                                                         WorkspaceShell::MenuId id) {
    std::vector<std::string> labels;
    const WorkspaceLayout layout = CurrentLayout(shell);
    const auto popup_rect = shell.ComputePopupMenuRect(layout.menu_bar, id);
    if (!popup_rect.has_value()) {
      return labels;
    }
    const auto items = shell.MenuItems(id);
    for (const auto& visible_item : shell.ComputeVisiblePopupMenuItems(id, *popup_rect)) {
      if (visible_item.separator) {
        continue;
      }
      labels.push_back(shell.MenuItemLabel(items[visible_item.index]));
    }
    return labels;
  }
  static std::optional<SDL_FRect> PopupMenuItemRect(WorkspaceShell& shell,
                                                    WorkspaceShell::MenuId id,
                                                    std::string_view label) {
    const WorkspaceLayout layout = CurrentLayout(shell);
    const auto popup_rect = shell.ComputePopupMenuRect(layout.menu_bar, id);
    if (!popup_rect.has_value()) {
      return std::nullopt;
    }
    const auto items = shell.MenuItems(id);
    for (const auto& visible_item : shell.ComputeVisiblePopupMenuItems(id, *popup_rect)) {
      if (visible_item.separator) {
        continue;
      }
      if (shell.MenuItemLabel(items[visible_item.index]) == label) {
        return visible_item.rect;
      }
    }
    return std::nullopt;
  }
  static std::optional<bool> PopupMenuItemChecked(WorkspaceShell& shell,
                                                  WorkspaceShell::MenuId id,
                                                  std::string_view label) {
    const WorkspaceLayout layout = CurrentLayout(shell);
    const auto popup_rect = shell.ComputePopupMenuRect(layout.menu_bar, id);
    if (!popup_rect.has_value()) {
      return std::nullopt;
    }
    const auto items = shell.MenuItems(id);
    for (const auto& visible_item : shell.ComputeVisiblePopupMenuItems(id, *popup_rect)) {
      if (visible_item.separator) {
        continue;
      }
      if (shell.MenuItemLabel(items[visible_item.index]) == label) {
        return visible_item.checked;
      }
    }
    return std::nullopt;
  }
  // Returns true if any menu item with the given label resolves to a
  // checked state on the menu's item table (does NOT require the menu to
  // be open — useful for unit-style checks against IsMenuItemChecked).
  static std::optional<bool> MenuItemCheckedByLabel(const WorkspaceShell& shell,
                                                    WorkspaceShell::MenuId id,
                                                    std::string_view label) {
    const auto items = shell.MenuItems(id);
    for (const auto& item : items) {
      if (item.separator) {
        continue;
      }
      if (shell.MenuItemLabel(item) == label) {
        return shell.IsMenuItemChecked(item);
      }
    }
    return std::nullopt;
  }
  static std::vector<std::string> SidebarModeMenuLabels(WorkspaceShell& shell) {
    return VisiblePopupMenuLabels(shell, WorkspaceShell::MenuId::SidebarMode);
  }
  static std::optional<SDL_FRect> SidebarModeMenuItemRect(WorkspaceShell& shell,
                                                          std::string_view label) {
    return PopupMenuItemRect(shell, WorkspaceShell::MenuId::SidebarMode, label);
  }
  static SDL_FRect SidebarModeButtonRect(WorkspaceShell& shell) {
    const WorkspaceLayout layout = CurrentLayout(shell);
    return shell.SidebarModeControlRect(layout.sidebar);
  }
  static SDL_FRect SearchSidebarModeButtonRect(WorkspaceShell& shell) {
    const WorkspaceLayout layout = CurrentLayout(shell);
    return shell.ProjectSearchModeButtonRect(layout.sidebar);
  }
  static SDL_FRect SearchSidebarCaseButtonRect(WorkspaceShell& shell) {
    const WorkspaceLayout layout = CurrentLayout(shell);
    return shell.ProjectSearchCaseButtonRect(layout.sidebar);
  }
  static SDL_FRect SearchSidebarHiddenButtonRect(WorkspaceShell& shell) {
    const WorkspaceLayout layout = CurrentLayout(shell);
    return shell.ProjectSearchHiddenButtonRect(layout.sidebar);
  }
  static SDL_FRect TreeSidebarCollapseButtonRect(WorkspaceShell& shell) {
    const WorkspaceLayout layout = CurrentLayout(shell);
    return shell.TreeSidebarCollapseButtonRect(layout.sidebar);
  }
  static SDL_FRect TreeSidebarRefreshButtonRect(WorkspaceShell& shell) {
    const WorkspaceLayout layout = CurrentLayout(shell);
    return shell.TreeSidebarRefreshButtonRect(layout.sidebar);
  }
  static std::string StatusBarSegmentText(const WorkspaceShell& shell,
                                          StatusBarSegmentId id) {
    return shell.status_bar_service_.Segment(id).text;
  }
  static std::string StatusBarSegmentTooltip(const WorkspaceShell& shell,
                                             StatusBarSegmentId id) {
    return shell.status_bar_service_.Segment(id).tooltip;
  }
  static bool StatusBarSegmentVisible(const WorkspaceShell& shell,
                                      StatusBarSegmentId id) {
    return shell.status_bar_service_.Segment(id).visible;
  }
  static std::optional<SDL_FRect> StatusBarSegmentRect(WorkspaceShell& shell,
                                                       StatusBarSegmentId id) {
    const WorkspaceLayout layout = CurrentLayout(shell);
    const StatusBarViewModel vm =
        RenderViewModelBuilder(shell.context_).BuildStatusBar(layout, shell.status_bar_service_);
    constexpr float kStatusPadding = 12.0f;
    constexpr float kStatusGap = 14.0f;
    float left_x = vm.rect.x + kStatusPadding;
    for (const StatusBarSegmentViewModel& segment : vm.left_segments) {
      const float width = shell.text_renderer_.MeasureWidth(segment.text);
      const SDL_FRect row = MakeRect(left_x, vm.rect.y, width, vm.rect.h);
      if (segment.id == id) {
        return row;
      }
      left_x += width + kStatusGap;
    }

    float right_x = vm.rect.x + vm.rect.w - kStatusPadding;
    for (auto it = vm.right_segments.rbegin(); it != vm.right_segments.rend(); ++it) {
      const float width = shell.text_renderer_.MeasureWidth(it->text);
      right_x -= width;
      const SDL_FRect row = MakeRect(right_x, vm.rect.y, width, vm.rect.h);
      if (it->id == id) {
        return row;
      }
      right_x -= kStatusGap;
    }
    return std::nullopt;
  }
  static std::string SettingsOverlayInputHintLabel(const WorkspaceShell& shell) {
    return shell.SettingsOverlayInputHintLabel();
  }
  static void RefreshStatusBar(WorkspaceShell& shell) { shell.RefreshStatusBar(); }
  static void SetStatusBarGitSnapshot(WorkspaceShell& shell,
                                      bool repo_available,
                                      std::string base_label,
                                      bool dirty_worktree) {
    auto& git = shell.context_.current_project_state.sidebar.git;
    git.repo_available = repo_available;
    git.branch_label = base_label;
    git.base_label = std::move(base_label);
    git.base_ref = git.base_label;
    git.entries.clear();
    if (repo_available && dirty_worktree) {
      GitSidebarEntry entry;
      entry.section = GitSidebarEntry::Section::Changed;
      entry.path = shell.context_.current_project_state.root / "status_bar_fixture.cpp";
      entry.relative_path = std::filesystem::path("status_bar_fixture.cpp");
      entry.status = project::GitFileStatus::Modified;
      git.entries.push_back(std::move(entry));
    }
  }
  static std::string BreadcrumbLabel(WorkspaceShell& shell) { return shell.BreadcrumbLabel(); }
  static const std::vector<project::TreeEntry>& TreeEntries(const WorkspaceShell& shell) {
    return shell.context_.current_project_state.directory_tree.entries();
  }
  static std::filesystem::path SelectedTreePath(const WorkspaceShell& shell) {
    return shell.SelectedTreePath();
  }
  static int SidebarScrollRow(const WorkspaceShell& shell) {
    return shell.context_.current_project_state.sidebar.scroll_row;
  }
  static std::size_t ProjectCount(const WorkspaceShell& shell) { return shell.context_.project_catalog.entries.size(); }
  static std::size_t ActiveProjectIndex(const WorkspaceShell& shell) {
    return shell.context_.project_catalog.active_index;
  }
  static const std::filesystem::path& ProjectRoot(const WorkspaceShell& shell) {
    return shell.context_.current_project_state.root;
  }
  static const std::vector<project::ProjectSearchResult>& ProjectSearchResults(
      const WorkspaceShell& shell) {
    return shell.context_.current_project_state.overlay.workflow.project_search.results;
  }
  static bool ProjectSearchRunning(const WorkspaceShell& shell) {
    return shell.context_.current_project_state.overlay.workflow.project_search.running;
  }
  static bool GitSidebarRefreshing(const WorkspaceShell& shell) {
    return shell.context_.current_project_state.sidebar.git.refreshing;
  }
  static bool GitSidebarSnapshotStale(const WorkspaceShell& shell) {
    return shell.context_.current_project_state.sidebar.git.snapshot_stale;
  }
  static bool ProjectSearchTruncated(const WorkspaceShell& shell) {
    return shell.context_.current_project_state.overlay.workflow.project_search.truncated;
  }
  static const std::string& ProjectSearchError(const WorkspaceShell& shell) {
    return shell.context_.current_project_state.overlay.workflow.project_search.error;
  }
  static bool ProjectOpenDialogActive(const WorkspaceShell& shell) {
    return shell.project_dialog_state_.active;
  }
  static bool CommandMode(const WorkspaceShell& shell) { return shell.context_.current_project_state.panel.command_mode; }
  static const std::string& CommandInput(const WorkspaceShell& shell) {
    return shell.context_.current_project_state.panel.command.input.text();
  }
  static std::string CommandPromptStatusText(const WorkspaceShell& shell) {
    return CommandPromptCoordinator::PromptStatusText(shell.context_.current_project_state.panel.command);
  }
  static const std::string& ProjectSearchQuery(const WorkspaceShell& shell) {
    return shell.context_.current_project_state.overlay.workflow.project_search.query.text();
  }
  static const std::string& BufferSearchQuery(const WorkspaceShell& shell) {
    return shell.context_.current_project_state.overlay.workflow.buffer_search.query.text();
  }
  static std::size_t BufferSearchMatchCount(const WorkspaceShell& shell) {
    return shell.context_.current_project_state.overlay.workflow.buffer_search.matches.size();
  }
  static bool OverlayVisible(const WorkspaceShell& shell) { return shell.context_.current_project_state.overlay.visible; }
  static bool OverlayModeIsFileFinder(const WorkspaceShell& shell) {
    return shell.context_.current_project_state.overlay.mode == WorkspaceShell::OverlayMode::FileFinder;
  }
  static bool TextInputSurfaceIsEditor(const WorkspaceShell& shell) {
    return shell.CurrentTextInputSurface() == WorkspaceShell::TextInputSurface::Editor;
  }
  static bool TextInputSurfaceIsFileFinder(const WorkspaceShell& shell) {
    return shell.CurrentTextInputSurface() == WorkspaceShell::TextInputSurface::FileFinder;
  }
  static std::size_t FileFinderResultCount(const WorkspaceShell& shell) {
    return shell.context_.current_project_state.file_finder.results().size();
  }
  static std::optional<std::filesystem::path> FileFinderSelectedPath(const WorkspaceShell& shell) {
    return shell.context_.current_project_state.file_finder.SelectedPath();
  }
  static bool FileIndexContainsPath(const WorkspaceShell& shell,
                                    const std::filesystem::path& relative_path) {
    const auto files = shell.context_.current_project_state.file_index.Snapshot();
    const std::filesystem::path normalized = relative_path.lexically_normal();
    for (const auto& file : files) {
      if (file.relative_path == normalized) {
        return true;
      }
    }
    return false;
  }
  static bool TextInputSurfaceIsPromptInput(const WorkspaceShell& shell) {
    return shell.CurrentTextInputSurface() == WorkspaceShell::TextInputSurface::PromptInput;
  }
  static bool SidebarVisible(const WorkspaceShell& shell) { return shell.context_.current_project_state.sidebar.visible; }
  static WorkspaceShell::SidebarMode SidebarMode(const WorkspaceShell& shell) {
    return shell.ActiveSidebarMode();
  }
  static const std::string& SidebarViewId(const WorkspaceShell& shell) {
    return shell.context_.current_project_state.sidebar.view_id;
  }  static const std::vector<plugin::PluginHost::SidebarItem>& PluginSidebarItems(
      const WorkspaceShell& shell) {
    return shell.context_.current_project_state.sidebar.plugin.items;
  }
  static const std::string& PluginSidebarError(const WorkspaceShell& shell) {
    return shell.context_.current_project_state.sidebar.plugin.error;
  }
  static float SidebarWidth(const WorkspaceShell& shell) { return shell.context_.current_project_state.sidebar.width; }
  static bool SoftTabsEnabled(const WorkspaceShell& shell) {
    return shell.context_.current_project_state.editor_preferences.soft_tabs;
  }
  static std::size_t EditorTabSize(const WorkspaceShell& shell) {
    return shell.context_.current_project_state.editor_preferences.tab_size;
  }
  static std::size_t EditorIndentWidth(const WorkspaceShell& shell) {
    return shell.context_.current_project_state.editor_preferences.indent_width;
  }
  static bool SoftWrapEnabled(const WorkspaceShell& shell) {
    return shell.context_.current_project_state.editor_preferences.soft_wrap;
  }
  static bool SettingsOverlayVisible(const WorkspaceShell& shell) {
    return shell.settings_overlay_service_.Visible();
  }
  static bool SetSettingValue(WorkspaceShell& shell, std::string_view id, std::string value) {
    return shell.SetSettingValue(id, std::move(value));
  }
  static std::optional<std::string> ProjectStoredSettingValue(const WorkspaceShell& shell,
                                                              std::string_view id) {
    const auto& settings = shell.context_.current_project_state.settings;
    const auto it = std::find_if(settings.begin(), settings.end(),
                                 [id](const auto& entry) { return entry.first == id; });
    if (it == settings.end()) {
      return std::nullopt;
    }
    return it->second;
  }
  static void OpenSettingsOverlay(WorkspaceShell& shell) { shell.OpenSettingsOverlay(); }
  static void OpenHelpAboutOverlay(WorkspaceShell& shell) { shell.OpenHelpAboutOverlay(); }
  static std::vector<HelpAboutRow> HelpAboutRows(const WorkspaceShell& shell) {
    return shell.settings_overlay_service_.HelpRows();
  }
  static SDL_FRect SettingsOverlayRect(WorkspaceShell& shell) {
    const WorkspaceLayout layout = CurrentLayout(shell);
    return ComputeOverlaySurfaceRect(layout.editor_area);
  }
  static void SetTransientDragTargetSidebarDivider(WorkspaceShell& shell) {
    shell.context_.interaction_state.drag_target = WorkspaceShell::DragTarget::SidebarDivider;
  }
  static void SetTransientDragTargetBottomPanelScrollbar(WorkspaceShell& shell) {
    shell.context_.interaction_state.drag_target = WorkspaceShell::DragTarget::BottomPanelScrollbar;
  }
  static bool TransientDragTargetIsNone(const WorkspaceShell& shell) {
    return shell.context_.interaction_state.drag_target == WorkspaceShell::DragTarget::None;
  }
  static void SetTransientMouseSelecting(WorkspaceShell& shell, bool selecting) {
    shell.context_.interaction_state.mouse_selecting = selecting;
  }
  static bool TransientMouseSelecting(const WorkspaceShell& shell) {
    return shell.context_.interaction_state.mouse_selecting;
  }
  static bool FocusIsEditor(const WorkspaceShell& shell) {
    return shell.context_.current_project_state.surface.focus == WorkspaceShell::FocusTarget::Editor;
  }
  static bool FocusIsSidebar(const WorkspaceShell& shell) {
    return shell.context_.current_project_state.surface.focus == WorkspaceShell::FocusTarget::Sidebar;
  }
  static bool FocusIsPanel(const WorkspaceShell& shell) {
    return shell.context_.current_project_state.surface.focus == WorkspaceShell::FocusTarget::Panel;
  }
  static void SetFocusSidebar(WorkspaceShell& shell) {
    shell.context_.current_project_state.surface.focus = WorkspaceShell::FocusTarget::Sidebar;
  }
  static void ResetCaretBlink(WorkspaceShell& shell) { shell.ResetCaretBlink(); }
  static void SetCaretBlinkEpochMs(WorkspaceShell& shell, Uint64 epoch_ms) {
    shell.caret_blink_epoch_ms_ = epoch_ms;
  }
  static bool CaretVisibleNow(const WorkspaceShell& shell) { return shell.CaretVisibleNow(); }
  static bool CaretBlinkAnimating(const WorkspaceShell& shell) {
    return shell.CaretBlinkAnimating();
  }
  static std::optional<Uint32> NextCaretBlinkDelayMs(const WorkspaceShell& shell) {
    return shell.NextCaretBlinkDelayMs();
  }
  static std::optional<std::chrono::milliseconds> ProjectFileMonitorNextPollDelay(
      const WorkspaceShell& shell) {
    return shell.project_file_monitor_.NextPollDelay();
  }
  static void MoveMergeSelection(WorkspaceShell& shell, int delta) { shell.MoveMergeSelection(delta); }
  static bool ShouldBlinkCaret(const WorkspaceShell& shell) { return shell.ShouldBlinkCaret(); }
  static bool FocusIsOverlay(const WorkspaceShell& shell) {
    return shell.context_.current_project_state.surface.focus == WorkspaceShell::FocusTarget::Overlay;
  }
  static bool MenuBarOpen(const WorkspaceShell& shell) { return shell.context_.menu_state.menu_bar_open; }
  static bool EditMenuOpen(const WorkspaceShell& shell) {
    return shell.context_.menu_state.menu_bar_open &&
           shell.context_.menu_state.active_menu_id == WorkspaceShell::MenuId::Edit;
  }
  static bool EditorContextMenuOpen(const WorkspaceShell& shell) {
    return shell.context_.menu_state.menu_bar_open &&
           shell.context_.menu_state.active_menu_id == WorkspaceShell::MenuId::EditorContext;
  }
  static bool FileMenuOpen(const WorkspaceShell& shell) {
    return shell.context_.menu_state.menu_bar_open &&
           shell.context_.menu_state.active_menu_id == WorkspaceShell::MenuId::File;
  }
  static bool EditorTabContextMenuOpen(const WorkspaceShell& shell) {
    return shell.context_.menu_state.menu_bar_open &&
           shell.context_.menu_state.active_menu_id == WorkspaceShell::MenuId::EditorTabContext;
  }
  static bool SidebarModeMenuOpen(const WorkspaceShell& shell) {
    return shell.context_.menu_state.menu_bar_open &&
           shell.context_.menu_state.active_menu_id == WorkspaceShell::MenuId::SidebarMode;
  }
  static bool TerminalTabContextMenuOpen(const WorkspaceShell& shell) {
    return shell.context_.menu_state.menu_bar_open &&
           shell.context_.menu_state.active_menu_id == WorkspaceShell::MenuId::TerminalTabContext;
  }
  static bool TerminalContextMenuOpen(const WorkspaceShell& shell) {
    return shell.context_.menu_state.menu_bar_open &&
           shell.context_.menu_state.active_menu_id == WorkspaceShell::MenuId::TerminalContext;
  }
