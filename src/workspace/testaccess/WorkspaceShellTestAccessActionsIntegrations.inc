  static WorkspaceShell::ProjectWorkspaceState& CurrentProjectState(WorkspaceShell& shell) {
    return shell.context_.current_project_state;
  }
  static WorkspaceShell::ProjectWorkspaceState& ProjectState(WorkspaceShell& shell,
                                                             std::size_t index) {
    return *shell.context_.project_catalog.entries.at(index);
  }
  static void ConsumeLspCallbacks(WorkspaceShell& shell) { shell.ConsumeLspCallbacks(); }
  // TD-2026-07-17A-033: tab activation must SCHEDULE LSP hydration, not run it
  // inline. True between the activation and the post-present drain
  // (OnFramePresented -> LspService::ConsumeDeferredBufferOpen).
  static bool HasPendingLspBufferOpen(const WorkspaceShell& shell) {
    return shell.lsp_service_.HasPendingBufferOpen();
  }
  // Drive the LSP hover fallback for a cell (1-based line/column) directly, drain
  // the async result, and return the resolved hover content ("" if it failed).
  static std::string ResolveLspHoverForTesting(WorkspaceShell& shell,
                                               const std::filesystem::path& path, std::size_t line,
                                               std::size_t column) {
    shell.plugin_hover_cache_.path = path;
    shell.plugin_hover_cache_.line = line;
    shell.plugin_hover_cache_.column = column;
    shell.plugin_hover_cache_.state = WorkspaceShell::PluginHoverCache::State::Pending;
    shell.plugin_hover_cache_.result = plugin::PluginHost::HoverResult{};
    shell.KickOffLspHover(path, line, column);
    shell.ConsumeLspCallbacks();
    return shell.plugin_hover_cache_.result.content;
  }
  // Apply a settings change to live subsystem state, as the next prepared frame does.
  static void ApplyLiveSettings(WorkspaceShell& shell) { shell.ApplyLiveSettings(); }
  // Fire one code-lens pull for the active buffer, as didOpen/save/undo-to-clean do.
  // Drives the trigger directly so a test can have two responses in flight at once.
  static bool RequestCodeLensesForActiveBuffer(WorkspaceShell& shell) {
    editor::TextViewport* viewport = shell.ActiveEditableViewport();
    if (viewport == nullptr) {
      return false;
    }
    std::string language_id;
    LspClient* client = shell.lsp_service_.LspClientForViewport(*viewport, &language_id);
    if (client == nullptr) {
      return false;
    }
    shell.lsp_service_.RequestLspCodeLenses(*viewport, *client);
    return true;
  }
  // Run a published code lens's server command, exactly as a click on it would.
  static void ActivateCodeLens(WorkspaceShell& shell, std::uint64_t payload) {
    shell.lsp_service_.ActivateCodeLens(payload);
    shell.ConsumeLspCallbacks();
  }
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
  static bool ExecuteAction(WorkspaceShell& shell, WorkspaceShell::ActionId id,
                            std::vector<std::string> args,
                            WorkspaceShell::ActionSource source =
                                WorkspaceShell::ActionSource::Menu) {
    return ActionCoordinator(shell.MakeActionContext())
        .Execute(id, std::move(args), source);
  }
  static bool OpenPlainComparison(WorkspaceShell& shell, CompareInput left, CompareInput right) {
    return shell.MakeCompareMergeService().OpenPlainComparison(std::move(left), std::move(right));
  }
  static bool HasCompareSelection(const WorkspaceShell& shell) {
    return shell.context_.current_project_state.compare_selection.has_value();
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
  static TreeContextTargetKind TreeContextMenuTarget(const WorkspaceShell& shell) {
    return shell.context_.menu_state.tree_context_menu.open
               ? shell.context_.menu_state.tree_context_menu.target
               : TreeContextTargetKind::None;
  }
  static std::filesystem::path TreeContextMenuPath(const WorkspaceShell& shell) {
    return shell.context_.menu_state.tree_context_menu.path;
  }
  static int TreeContextMenuActiveItemIndex(const WorkspaceShell& shell) {
    return shell.context_.menu_state.tree_context_menu.active_item_index;
  }
  // Count of items the keyboard can land on (separators are skipped by the mover
  // but still occupy an index, so this is the raw item count).
  static std::size_t TreeContextMenuItemCount(TreeContextTargetKind target) {
    return WorkspaceTreeContextMenuItems(target).size();
  }
  static std::vector<std::string> TreeContextMenuLabels(const WorkspaceShell& shell,
                                                        TreeContextTargetKind target) {
    std::vector<std::string> labels;
    for (const auto& item : WorkspaceTreeContextMenuItems(target)) {
      if (item.separator) {
        continue;
      }
      labels.push_back(shell.MenuItemLabel(item));
    }
    return labels;
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
  // Dispatch as the editor-tab/menu-bar surfaces do (ActionSource::Menu).
  static bool ExecuteMenuAction(WorkspaceShell& shell, WorkspaceShell::ActionId id) {
    return ActionCoordinator(shell.MakeActionContext())
        .Execute(id, {}, WorkspaceShell::ActionSource::Menu);
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
  static void MaybeAutosaveDirtyTabs(WorkspaceShell& shell, bool on_focus_change) {
    shell.MaybeAutosaveDirtyTabs(on_focus_change);
  }
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
  static void ShowTreeSidebar(WorkspaceShell& shell) { shell.ShowTreeSidebar(); }
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
  static void SetProjectSearchPatternModeRegex(WorkspaceShell& shell, bool regex) {
    shell.context_.current_project_state.overlay.workflow.project_search.options.pattern_mode =
        regex ? project::ProjectSearchPatternMode::Regex
              : project::ProjectSearchPatternMode::Literal;
  }
  static void SetProjectSearchReplaceText(WorkspaceShell& shell, std::string text) {
    shell.context_.current_project_state.overlay.workflow.project_search.replace_text.SetText(
        std::move(text));
  }
  static bool ProjectSearchCanReplaceAll(const WorkspaceShell& shell) {
    return shell.ProjectSearchCanReplaceAll();
  }
  static void CycleProjectSearchCaseMode(WorkspaceShell& shell) {
    shell.CycleProjectSearchCaseMode();
  }
  static void RefreshProjectSearch(WorkspaceShell& shell) { shell.RefreshProjectSearch(); }
  static bool GitOperationBusy(const WorkspaceShell& shell) {
    return shell.git_operation_service_.busy();
  }
  static void DrainGitOperationCompletions(WorkspaceShell& shell) {
    shell.git_operation_service_.DrainCompletions();
  }
  static void ToggleProjectSearchHiddenFiles(WorkspaceShell& shell) {
    shell.ToggleProjectSearchHiddenFiles();
  }
  static void ReplaceAllProjectSearchMatches(WorkspaceShell& shell) {
    shell.ReplaceAllProjectSearchMatches();
    // Replace-all now runs the read/replace/atomic-write off the shell thread
    // (TD-2026-07-17-021). Drive it to completion deterministically: barrier the
    // background job (a trailing Post runs after it on the serial worker), then drain
    // the mailbox so the outcome applies on this (main) thread. A no-op dispatch
    // (nothing posted) simply leaves the mailbox empty.
    std::promise<void> ran;
    std::future<void> ran_future = ran.get_future();
    shell.project_background_executor_.Post([&ran]() { ran.set_value(); });
    ran_future.wait();
    shell.project_replace_mailbox_.Drain();
  }
  static void SetReplaceAllAggregateCapBytes(WorkspaceShell& shell, std::size_t cap) {
    shell.replace_all_aggregate_cap_bytes_ = cap;
  }
  static const std::vector<NotificationService::Notification>& ActiveNotifications(
      const WorkspaceShell& shell) {
    return shell.notification_service_.Active();
  }
  static void ShowNotification(WorkspaceShell& shell,
                               NotificationService::Tone tone,
                               std::string message) {
    shell.notification_service_.Show(tone, std::move(message), 0);
  }
  // Grab rect of the toast at `stack_position` (0 = newest, just above the status
  // bar), resolved through the same NotificationLayout helper the painter uses.
  static SDL_FRect NotificationToastRect(WorkspaceShell& shell, std::size_t stack_position) {
    const WorkspaceLayout layout = CurrentLayout(shell);
    const auto& active = shell.notification_service_.Active();
    const std::size_t index = active.size() - 1 - stack_position;
    return NotificationToastLayoutAt(layout.status_bar, shell.text_renderer_.LineHeight(),
                                     stack_position,
                                     shell.text_renderer_.MeasureWidth(active[index].message))
        .rect;
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
  static std::size_t TerminalTabCount(const WorkspaceShell& shell) {
    return shell.context_.current_project_state.terminal_tabs.size();
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
  static void AppendTerminalPendingInput(WorkspaceShell& shell, std::string_view input) {
    shell.AppendTerminalPendingInput(input);
  }
  static void SubmitTerminalPendingInput(WorkspaceShell& shell) {
    shell.SubmitTerminalPendingInput();
  }
  static std::size_t ActiveTerminalPendingInputSize(const WorkspaceShell& shell) {
    return shell.context_.current_project_state.terminal_tabs[shell.context_.current_project_state.active_terminal_tab_index]->pending_input.size();
  }
  static bool ActiveTerminalPendingInputTruncated(const WorkspaceShell& shell) {
    return shell.context_.current_project_state.terminal_tabs[shell.context_.current_project_state.active_terminal_tab_index]->pending_input_truncated;
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
    return shell.context_.current_project_state.sidebar.git.commit_workflow.BodyText();
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
  static TerminalFindService& TerminalFind(WorkspaceShell& shell) {
    return shell.terminal_find_service_;
  }
  static void RefreshTerminalFind(WorkspaceShell& shell) {
    shell.terminal_find_service_.Refresh(shell.ActiveTerminalTab());
  }
  static bool HandleTerminalFindKeyDown(WorkspaceShell& shell,
                                        SDL_Keycode key,
                                        SDL_Keymod modifiers) {
    SDL_KeyboardEvent event{};
    event.key = key;
    return shell.HandleTerminalFindKeyDown(event, modifiers);
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
  static SDL_FRect ProblemsSidebarRowRect(WorkspaceShell& shell, std::size_t row_index) {
    const WorkspaceLayout layout = CurrentLayout(shell);
    const auto list_layout = shell.ComputeProblemsSidebarListLayout(
        layout.sidebar, shell.context_.current_project_state.sidebar.problems.entries.size());
    return ScrollableListRowRect(list_layout,
                                 static_cast<int>(row_index) - list_layout.scroll_row);
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
