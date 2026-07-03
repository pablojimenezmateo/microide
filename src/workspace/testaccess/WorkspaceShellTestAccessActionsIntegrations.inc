  static WorkspaceShell::ProjectWorkspaceState& CurrentProjectState(WorkspaceShell& shell) {
    return shell.context_.current_project_state;
  }
  static WorkspaceShell::ProjectWorkspaceState& ProjectState(WorkspaceShell& shell,
                                                             std::size_t index) {
    return *shell.context_.project_catalog.entries.at(index);
  }
  static void ConsumeLspCallbacks(WorkspaceShell& shell) { shell.ConsumeLspCallbacks(); }
  static bool IsControlChannelRunning(const WorkspaceShell& shell) {
    return shell.control_channel_service_.IsRunning();
  }
  // Launch-config picker (Phase 9): seed configs, drive the picker, and read back
  // its filtered matches / the persisted selection on confirm.
  static void SeedLaunchConfigs(WorkspaceShell& shell,
                                std::vector<microide::workspace::LaunchConfig> configs) {
    shell.context_.current_project_state.launch_configs = std::move(configs);
  }
  static void OpenLaunchConfigPicker(WorkspaceShell& shell) { shell.OpenLaunchConfigPicker(); }
  static void SetLaunchConfigPickerQuery(WorkspaceShell& shell, const std::string& query) {
    shell.context_.current_project_state.overlay.workflow.launch_config_picker.query.SetText(query);
    shell.RefreshLaunchConfigPicker();
  }
  static std::vector<std::string> LaunchConfigPickerMatchLabels(const WorkspaceShell& shell) {
    std::vector<std::string> labels;
    for (const auto& match :
         shell.context_.current_project_state.overlay.workflow.launch_config_picker.matches) {
      labels.push_back(match.primary_label);
    }
    return labels;
  }
  static void SelectAndConfirmLaunchConfig(WorkspaceShell& shell, std::size_t match_index) {
    shell.context_.current_project_state.overlay.workflow.launch_config_picker.selected_index =
        match_index;
    shell.ConfirmLaunchConfigSelection();
  }
  static std::size_t SelectedLaunchConfigIndex(const WorkspaceShell& shell) {
    return shell.context_.current_project_state.selected_launch_config_index;
  }
  static const std::vector<editor::PublishedDiagnostic>* DiagnosticsForPath(
      const WorkspaceShell& shell,
      const std::filesystem::path& path) {
    return shell.context_.current_project_state.diagnostics_store.FindByPath(path);
  }
  static const std::vector<microide::workspace::ScmProviderSpec>& ScmProviders(
      const WorkspaceShell& shell) {
    return shell.scm_registry_.Specs();
  }
  static const std::vector<microide::workspace::AnnotationProviderSpec>& AnnotationProviders(
      const WorkspaceShell& shell) {
    return shell.annotation_registry_.Specs();
  }
  static bool PublishDiagnostics(WorkspaceShell& shell,
                                 std::string_view owner,
                                 const std::filesystem::path& path,
                                 std::vector<editor::Diagnostic> diagnostics) {
    return shell.context_.current_project_state.diagnostics_store.ReplaceForOwnerFile(owner, path, std::move(diagnostics));
  }
  static bool ExecuteCopySelectionWithContext(WorkspaceShell& shell) {
    return ActionCoordinator(shell.MakeActionContext()).Execute(
        WorkspaceShell::ActionId::CopySelectionWithContext, {},
        WorkspaceShell::ActionSource::Menu);
  }
  static bool ExecuteCopySelection(WorkspaceShell& shell) {
    return ActionCoordinator(shell.MakeActionContext()).Execute(
        WorkspaceShell::ActionId::CopySelection, {}, WorkspaceShell::ActionSource::Menu);
  }
  static bool ExecuteSelectAll(WorkspaceShell& shell) {
    return ActionCoordinator(shell.MakeActionContext()).Execute(
        WorkspaceShell::ActionId::SelectAll, {}, WorkspaceShell::ActionSource::Menu);
  }
  static bool ExecuteCopyRelativePath(WorkspaceShell& shell) {
    return ActionCoordinator(shell.MakeActionContext()).Execute(
        WorkspaceShell::ActionId::CopyRelativePath, {}, WorkspaceShell::ActionSource::Menu);
  }
  static bool ExecuteCopyAbsolutePath(WorkspaceShell& shell) {
    return ActionCoordinator(shell.MakeActionContext()).Execute(
        WorkspaceShell::ActionId::CopyAbsolutePath, {}, WorkspaceShell::ActionSource::Menu);
  }
  static void OpenTreeContextMenuForPath(WorkspaceShell& shell,
                                         TreeContextTargetKind target,
                                         const std::filesystem::path& path) {
    shell.MakeMenuCoordinator().OpenTreeContextMenu(target, path, SDL_FRect{0, 0, 0, 0});
  }
  static editor::BreakpointStore& BreakpointStore(WorkspaceShell& shell) {
    return shell.context_.current_project_state.breakpoint_store;
  }
  static void OpenBreakpointContextMenu(WorkspaceShell& shell, const std::filesystem::path& path,
                                        std::size_t line) {
    shell.OpenBreakpointContextMenu(path, line, SDL_FRect{0, 0, 0, 0});
  }
  static bool BreakpointContextMenuOpen(const WorkspaceShell& shell) {
    return shell.context_.menu_state.tree_context_menu.open &&
           shell.context_.menu_state.tree_context_menu.target ==
               TreeContextTargetKind::BreakpointLine;
  }
  static bool BreakpointContextMenuEnabledFlag(const WorkspaceShell& shell) {
    return shell.context_.menu_state.tree_context_menu.breakpoint_enabled;
  }
  static bool GitEntryContextMenuOpen(const WorkspaceShell& shell) {
    return shell.context_.menu_state.tree_context_menu.open &&
           shell.context_.menu_state.tree_context_menu.target == TreeContextTargetKind::GitEntry;
  }
  static std::vector<std::string> GitEntryContextMenuLabels(const WorkspaceShell& shell) {
    std::vector<std::string> labels;
    for (const auto& item : WorkspaceTreeContextMenuItems(TreeContextTargetKind::GitEntry)) {
      if (item.separator) {
        continue;
      }
      labels.push_back(shell.MenuItemLabel(item));
    }
    return labels;
  }
  static bool ExecuteBreakpointMenuAction(WorkspaceShell& shell, WorkspaceShell::ActionId id) {
    return ActionCoordinator(shell.MakeActionContext())
        .Execute(id, {}, WorkspaceShell::ActionSource::ContextMenu);
  }
  static bool ExecuteShowInFileExplorer(WorkspaceShell& shell) {
    return ActionCoordinator(shell.MakeActionContext()).Execute(
        WorkspaceShell::ActionId::ShowInFileExplorer, {},
        WorkspaceShell::ActionSource::ContextMenu);
  }
  static bool ExecuteContextMenuAction(WorkspaceShell& shell, WorkspaceShell::ActionId id) {
    return ActionCoordinator(shell.MakeActionContext())
        .Execute(id, {}, WorkspaceShell::ActionSource::ContextMenu);
  }
  static bool IsActionEnabled(WorkspaceShell& shell, WorkspaceShell::ActionId id) {
    return shell.MakeActionAvailability().IsEnabled(id);
  }
  static bool ExecutePasteClipboard(WorkspaceShell& shell) {
    return ActionCoordinator(shell.MakeActionContext()).Execute(
        WorkspaceShell::ActionId::PasteClipboard, {}, WorkspaceShell::ActionSource::Menu);
  }
  static bool ExecuteCopyLastTerminalCommand(WorkspaceShell& shell) {
    return ActionCoordinator(shell.MakeActionContext()).Execute(
        WorkspaceShell::ActionId::CopyLastTerminalCommand, {},
        WorkspaceShell::ActionSource::Menu);
  }
  static bool ExecuteCloseAllTabs(WorkspaceShell& shell) {
    return ActionCoordinator(shell.MakeActionContext()).Execute(
        WorkspaceShell::ActionId::CloseAllTabs, {}, WorkspaceShell::ActionSource::Menu);
  }
  static bool ExecuteCloseOtherTabs(WorkspaceShell& shell) {
    return ActionCoordinator(shell.MakeActionContext()).Execute(
        WorkspaceShell::ActionId::CloseOtherTabs, {}, WorkspaceShell::ActionSource::Menu);
  }
  static bool ExecuteCloseTabsToRight(WorkspaceShell& shell) {
    return ActionCoordinator(shell.MakeActionContext()).Execute(
        WorkspaceShell::ActionId::CloseTabsToRight, {}, WorkspaceShell::ActionSource::Menu);
  }
  static bool ExecuteCloseTabsToLeft(WorkspaceShell& shell) {
    return ActionCoordinator(shell.MakeActionContext()).Execute(
        WorkspaceShell::ActionId::CloseTabsToLeft, {}, WorkspaceShell::ActionSource::Menu);
  }
  static bool ExecuteTreeRefresh(WorkspaceShell& shell) {
    return ActionCoordinator(shell.MakeActionContext()).Execute(
        WorkspaceShell::ActionId::TreeRefresh, {}, WorkspaceShell::ActionSource::Menu);
  }
  static bool ExecuteShowGitSidebar(WorkspaceShell& shell) {
    return ActionCoordinator(shell.MakeActionContext()).Execute(
        WorkspaceShell::ActionId::SidebarShow, {"git"}, WorkspaceShell::ActionSource::Menu);
  }
  static void OpenCommandPalette(WorkspaceShell& shell) { shell.OpenCommandPalette(); }
  static bool SaveTab(WorkspaceShell& shell, std::size_t index) { return shell.SaveTab(index); }
  static void ActivateTab(WorkspaceShell& shell, std::size_t index) { shell.ActivateTab(index); }
  static void MaybeArmAutosaveTimer(WorkspaceShell& shell) { shell.MaybeArmAutosaveTimer(); }
  static bool AutosaveArmed(const WorkspaceShell& shell) { return shell.autosave_armed_; }
  static bool SelectTreePath(WorkspaceShell& shell, const std::filesystem::path& path) {
    return shell.context_.current_project_state.directory_tree.SelectPath(path);
  }
  static void ExpandTreeSelection(WorkspaceShell& shell) {
    shell.context_.current_project_state.directory_tree.ExpandSelection();
  }
  static std::optional<std::filesystem::path> ActivateTreeSelection(WorkspaceShell& shell) {
    return shell.context_.current_project_state.directory_tree.ActivateSelection();
  }
  static void CollapseTreeSelection(WorkspaceShell& shell) { shell.context_.current_project_state.directory_tree.CollapseSelection(); }
  static void ShowSearchSidebar(WorkspaceShell& shell,
                                std::string query,
                                bool temporary = false) {
    shell.ShowSearchSidebar(std::move(query), temporary);
  }
  static void ShowProblemsSidebar(WorkspaceShell& shell) { shell.ShowProblemsSidebar(); }
  static void ShowGitSidebar(WorkspaceShell& shell) { shell.ShowGitSidebar(); }
  static void MaybeRequestTreeGitBadgesAfterFirstPaint(WorkspaceShell& shell) {
    shell.MaybeRequestTreeGitBadgesAfterFirstPaint();
  }
  static void OnFramePresented(WorkspaceShell& shell) { shell.OnFramePresented(); }
  static void RefreshProjectFiles(WorkspaceShell& shell) { shell.RefreshProjectFiles(); }
  static bool RefreshProblemsSidebar(WorkspaceShell& shell) {
    return shell.RefreshProblemsSidebar();
  }
  static void RefreshGitSidebar(WorkspaceShell& shell) { shell.RefreshGitSidebar(); }
  static void RevealGitSidebarEntry(WorkspaceShell& shell, std::size_t entry_index) {
    shell.context_.current_project_state.sidebar.git.selected_index = entry_index;
    shell.RevealSelectedGitSidebarLine();
  }
  static void RequestAutomaticGitSidebarRefresh(WorkspaceShell& shell) {
    shell.RequestAutomaticGitSidebarRefresh();
  }
  static void ConsumeGitSidebarRefresh(WorkspaceShell& shell) {
    shell.ConsumeGitSidebarRefresh();
  }
  static std::vector<std::string> GitSidebarSummaryLines(const WorkspaceShell& shell) {
    return shell.GitSidebarSummaryLines();
  }
  static const std::vector<WorkspaceShell::ProblemsSidebarEntry>& ProblemsSidebarEntries(
      const WorkspaceShell& shell) {
    return shell.context_.current_project_state.sidebar.problems.entries;
  }
  static const std::vector<WorkspaceShell::TestsSidebarEntry>& TestsSidebarEntries(
      const WorkspaceShell& shell) {
    return shell.context_.current_project_state.sidebar.tests.entries;
  }
  static const std::vector<WorkspaceShell::GitSidebarEntry>& GitSidebarEntries(
      const WorkspaceShell& shell) {
    return shell.context_.current_project_state.sidebar.git.entries;
  }
  static std::size_t GitSidebarSelectedIndex(const WorkspaceShell& shell) {
    return shell.context_.current_project_state.sidebar.git.selected_index;
  }
  static void SetPluginSidebarSelectedIndex(WorkspaceShell& shell, std::size_t index) {
    shell.context_.current_project_state.sidebar.plugin.selected_index = index;
  }
  static bool OpenSelectedPluginSidebarItem(WorkspaceShell& shell) {
    return shell.OpenSelectedPluginSidebarItem();
  }
  static bool ShowSignatureHelp(WorkspaceShell& shell) {
    return shell.assist_service_.ShowSignatureHelp(nullptr);
  }
  static std::optional<WorkspaceShell::SignatureHelpPopup> SignatureHelpPopup(
      const WorkspaceShell& shell) {
    return shell.active_signature_help_;
  }
  static void ExpireSignatureHelp(WorkspaceShell& shell) { shell.MaybeExpireSignatureHelp(); }
  static bool ApplySelectedCompletion(WorkspaceShell& shell) {
    return shell.assist_service_.ApplySelectedCompletion();
  }
  static bool ExecuteSelectedCodeAction(WorkspaceShell& shell) {
    return shell.assist_service_.ExecuteSelectedCodeAction();
  }
  static void ConsumeProjectSearchUpdates(WorkspaceShell& shell) {
    shell.ConsumeProjectSearchUpdates();
  }
  static void ToggleProjectSearchPatternMode(WorkspaceShell& shell) {
    shell.ToggleProjectSearchPatternMode();
  }
  static void CycleProjectSearchCaseMode(WorkspaceShell& shell) {
    shell.CycleProjectSearchCaseMode();
  }
  static void ToggleProjectSearchHiddenFiles(WorkspaceShell& shell) {
    shell.ToggleProjectSearchHiddenFiles();
  }
  static void ReplaceAllProjectSearchMatches(WorkspaceShell& shell) {
    shell.ReplaceAllProjectSearchMatches();
  }
  static void SetReplaceAllAggregateCapBytes(WorkspaceShell& shell, std::size_t cap) {
    shell.replace_all_aggregate_cap_bytes_ = cap;
  }
  static const std::vector<NotificationService::Notification>& ActiveNotifications(
      const WorkspaceShell& shell) {
    return shell.notification_service_.Active();
  }
  static void EnsureTerminalTab(WorkspaceShell& shell) {
    if (shell.context_.current_project_state.terminal_tabs.empty()) {
      shell.context_.current_project_state.terminal_tabs.push_back(std::make_unique<WorkspaceShell::TerminalTabState>());
    }
    shell.context_.current_project_state.active_terminal_tab_index = shell.context_.current_project_state.terminal_tabs.size() - 1;
    shell.context_.current_project_state.panel.content = WorkspaceShell::PanelContentKind::Terminal;
    shell.context_.current_project_state.surface.focus = WorkspaceShell::FocusTarget::Panel;
  }
  static void AddTerminalTab(WorkspaceShell& shell) {
    shell.context_.current_project_state.terminal_tabs.push_back(std::make_unique<WorkspaceShell::TerminalTabState>());
    shell.context_.current_project_state.active_terminal_tab_index = shell.context_.current_project_state.terminal_tabs.size() - 1;
    shell.context_.current_project_state.panel.content = WorkspaceShell::PanelContentKind::Terminal;
    shell.context_.current_project_state.surface.focus = WorkspaceShell::FocusTarget::Panel;
  }
  static void ConsumeTerminalSessionUpdates(WorkspaceShell& shell) {
    shell.ConsumeTerminalSessionUpdates();
  }
  static microide::terminal::TerminalSession& ActiveTerminalSession(WorkspaceShell& shell) {
    return shell.context_.current_project_state.terminal_tabs[shell.context_.current_project_state.active_terminal_tab_index]->session;
  }
  static void SetActiveTerminalFollowTail(WorkspaceShell& shell, bool follow_tail) {
    shell.context_.current_project_state.terminal_tabs[shell.context_.current_project_state.active_terminal_tab_index]->follow_tail = follow_tail;
  }
  static bool ActiveTerminalFollowTail(const WorkspaceShell& shell) {
    return shell.context_.current_project_state.terminal_tabs[shell.context_.current_project_state.active_terminal_tab_index]->follow_tail;
  }
  static void SetActiveTerminalScrollRow(WorkspaceShell& shell, int scroll_row) {
    shell.context_.current_project_state.terminal_tabs[shell.context_.current_project_state.active_terminal_tab_index]->scroll_row = scroll_row;
  }
  static int ActiveTerminalScrollRow(const WorkspaceShell& shell) {
    return shell.context_.current_project_state.terminal_tabs[shell.context_.current_project_state.active_terminal_tab_index]->scroll_row;
  }
  static bool HandleTerminalKeyDown(WorkspaceShell& shell,
                                    SDL_Keycode key,
                                    SDL_Keymod modifiers) {
    SDL_KeyboardEvent event{};
    event.key = key;
    return shell.MakeTextInputCoordinator().HandleTerminalKeyDown(event, modifiers);
  }
  static bool HandleTextInput(WorkspaceShell& shell, std::string_view text) {
    SDL_TextInputEvent event{};
    const std::string storage(text);
    event.text = storage.c_str();
    return shell.MakeTextInputCoordinator().HandleTextInput(event);
  }
  static bool HandleKeyDown(WorkspaceShell& shell, SDL_Keycode key, SDL_Keymod modifiers) {
    SDL_KeyboardEvent event{};
    event.key = key;
    event.mod = modifiers;
    return shell.MakeKeyInputCoordinator().HandleKeyDown(event);
  }
  static bool OpenCommitWorkflow(WorkspaceShell& shell) { return shell.OpenCommitWorkflow(); }
  static CommitWorkflowState& CommitWorkflow(WorkspaceShell& shell) {
    return shell.context_.current_project_state.sidebar.git.commit_workflow;
  }
  static std::string CommitSubjectText(const WorkspaceShell& shell) {
    return shell.context_.current_project_state.sidebar.git.commit_workflow.subject.text();
  }
  static std::string CommitBodyText(const WorkspaceShell& shell) {
    return CommitWorkflowBodyText(
        shell.context_.current_project_state.sidebar.git.commit_workflow.body);
  }
  static TextInputSurface CurrentTextInputSurface(const WorkspaceShell& shell) {
    return shell.CurrentTextInputSurface();
  }
  static int ProjectTabScrollIndex(const WorkspaceShell& shell) {
    return shell.context_.project_catalog.tab_scroll_index;
  }
  static int EditorTabScrollIndex(const WorkspaceShell& shell) { return shell.context_.current_project_state.focused_group().tab_scroll_index; }
  static SDL_FRect BottomPanelContentRect(WorkspaceShell& shell) {
    const WorkspaceLayout layout = CurrentLayout(shell);
    return microide::workspace::BottomPanelContentRect(layout);
  }
  static SDL_FPoint TerminalCellPoint(WorkspaceShell& shell,
                                      std::size_t row,
                                      std::size_t column) {
    const auto* terminal_tab = shell.ActiveTerminalTab();
    const auto lines = terminal_tab != nullptr ? terminal_tab->session.SnapshotLines()
                                               : std::vector<microide::terminal::TerminalLine>{};
    const WorkspaceLayout layout = CurrentLayout(shell);
    const auto panel_layout = shell.ComputeBottomPanelLogLayout(layout, lines.size());
    return SDL_FPoint{
        .x = panel_layout.text_x +
             static_cast<float>(column) * std::max(1.0f, shell.text_renderer_.CharWidth()) + 1.0f,
        .y = panel_layout.text_y + static_cast<float>(row) * panel_layout.line_height +
             panel_layout.line_height * 0.5f,
    };
  }
  static SDL_FPoint BottomPanelTextOrigin(WorkspaceShell& shell) {
    const auto* terminal_tab = shell.ActiveTerminalTab();
    const std::size_t line_count =
        terminal_tab != nullptr ? terminal_tab->session.LineCount() : 0;
    const WorkspaceLayout layout = CurrentLayout(shell);
    const auto panel_layout = shell.ComputeBottomPanelLogLayout(layout, line_count);
    return SDL_FPoint{.x = panel_layout.text_x, .y = panel_layout.text_y};
  }
  static bool TerminalHasSelection(const WorkspaceShell& shell) {
    return shell.TerminalHasSelection();
  }
  static std::string ActiveTerminalSelectedText(WorkspaceShell& shell) {
    return shell.SelectedTerminalText();
  }
  static SDL_FRect ProjectSearchResultRect(WorkspaceShell& shell, std::size_t result_index) {
    const WorkspaceLayout layout = CurrentLayout(shell);
    const auto line_map = shell.BuildProjectSearchLineMap();
    const auto list_layout =
        shell.ComputeProjectSearchSidebarListLayout(layout.sidebar, line_map.size());
    const int line_index = shell.ProjectSearchLineForResult(result_index);
    return ScrollableListRowRect(list_layout, line_index - list_layout.scroll_row);
  }
  static SDL_FRect ProjectTabRect(WorkspaceShell& shell, std::size_t index) {
    const WorkspaceLayout layout = CurrentLayout(shell);
    for (const WorkspaceShell::VisibleStripTab& tab :
         shell.tab_strip_chrome_.ComputeVisibleProjectTabs(layout.project_tab_strip)) {
      if (tab.index == index) {
        return tab.rect;
      }
    }
    return {};
  }
  static std::string ProjectTabTooltipLabel(WorkspaceShell& shell, std::size_t index) {
    return shell.ProjectTabTooltipLabel(index);
  }
  static std::string HoveredProjectTabTooltipLabel(WorkspaceShell& shell) {
    const WorkspaceLayout layout = CurrentLayout(shell);
    return shell.HoveredProjectTabTooltipLabel(layout.project_tab_strip);
  }
