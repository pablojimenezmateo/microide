    return conversation != nullptr ? conversation->provider_id : std::string{};
  }
  static bool ActivateChatConversation(WorkspaceShell& shell, std::string_view conversation_id) {
    return shell.ActivateChatConversation(conversation_id);
  }
  static bool CreateChatConversation(WorkspaceShell& shell) {
    return shell.CreateChatConversation();
  }
  static std::string ActiveConversationId(const WorkspaceShell& shell) {
    return shell.context_.current_project_state.panel.chat.conversation_id;
  }
static WorkspaceShell::ProjectWorkspaceState& CurrentProjectState(WorkspaceShell& shell) {
    return shell.context_.current_project_state;
  }
  static WorkspaceShell::ProjectWorkspaceState& ProjectState(WorkspaceShell& shell,
                                                             std::size_t index) {
    return *shell.context_.project_catalog.entries.at(index);
  }
  static const std::vector<AiProviderSpec>& AiProviders(const WorkspaceShell& shell) {
    return shell.ai_provider_registry_.Specs();
  }
  static bool SetProviderApiKey(WorkspaceShell& shell,
                                std::string_view provider_id,
                                std::string_view api_key,
                                std::string* error_message = nullptr) {
    return shell.SetProviderApiKey(provider_id, api_key, error_message);
  }
  static ProviderAuthStatus GetProviderAuthStatus(const WorkspaceShell& shell,
                                                  std::string_view provider_id) {
    return shell.GetProviderAuthStatus(provider_id);
  }
  static void RequestProviderAuthCheck(WorkspaceShell& shell, std::string_view provider_id) {
    shell.provider_bridge_manager_.RequestAuthCheck(std::string(provider_id));
  }
  static void RequestProviderModelList(WorkspaceShell& shell, std::string_view provider_id) {
    shell.provider_bridge_manager_.RequestModelList(std::string(provider_id));
  }
  static std::vector<std::string> ProviderModels(const WorkspaceShell& shell,
                                                 std::string_view provider_id) {
    return shell.provider_bridge_manager_.GetModels(std::string(provider_id));
  }
  static ProviderCapabilities GetProviderCapabilities(const WorkspaceShell& shell,
                                                      std::string_view provider_id) {
    return shell.provider_bridge_manager_.GetCapabilities(std::string(provider_id));
  }
  static void ConsumeProviderBridgeUpdates(WorkspaceShell& shell) {
    shell.ConsumeProviderBridgeUpdates();
  }
  static void ConsumeLspCallbacks(WorkspaceShell& shell) { shell.ConsumeLspCallbacks(); }
  static void ConsumePluginAsyncProcessCallbacks(WorkspaceShell& shell) {
    shell.ConsumePluginAsyncProcessCallbacks();
  }  static bool WaitForPluginAsyncProcessCallbacks(WorkspaceShell& shell, int timeout_ms = 5000) {
    const Uint64 deadline = SDL_GetTicks() + static_cast<Uint64>(std::max(timeout_ms, 0));
    while (shell.plugin_runtime_.Host().PendingAsyncProcessCount() > 0 &&
           SDL_GetTicks() <= deadline) {
      shell.ConsumePluginAsyncProcessCallbacks();
      SDL_Delay(5);
    }
    shell.ConsumePluginAsyncProcessCallbacks();
    return shell.plugin_runtime_.Host().PendingAsyncProcessCount() == 0;
  }
  static bool WaitForProviderBridgeIdle(WorkspaceShell& shell, int timeout_ms = 2000) {
    const auto any_in_flight = [&]() {
      const auto project_busy = [](const WorkspaceShell::ProjectWorkspaceState& project) {
        return project.panel.chat.request_in_flight || project.inline_completion.request_in_flight;
      };
      if (project_busy(shell.context_.current_project_state)) {
        return true;
      }
      return std::any_of(shell.context_.project_catalog.entries.begin(),
                         shell.context_.project_catalog.entries.end(),
                         [&](const auto& entry) {
                           return entry != nullptr && project_busy(*entry);
                         });
    };
    const Uint64 deadline = SDL_GetTicks() + static_cast<Uint64>(std::max(timeout_ms, 0));
    while (any_in_flight() && SDL_GetTicks() <= deadline) {
      shell.ConsumeProviderBridgeUpdates();
      shell.HandleScheduledWake();
      SDL_Delay(5);
    }
    shell.ConsumeProviderBridgeUpdates();
    return !any_in_flight();
  }
  static bool WaitForTaskRuntimeIdle(WorkspaceShell& shell, int timeout_ms = 2000) {
    const Uint64 deadline = SDL_GetTicks() + static_cast<Uint64>(std::max(timeout_ms, 0));
    while (shell.task_runtime_.active_run_id() != 0 && SDL_GetTicks() <= deadline) {
      shell.ConsumeTaskRuntimeUpdates();
      SDL_Delay(5);
    }
    shell.ConsumeTaskRuntimeUpdates();
    return shell.task_runtime_.active_run_id() == 0;
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
  static std::vector<microide::workspace::AuthSession> AuthSessions(
      const WorkspaceShell& shell,
      std::string_view provider_id) {
    return shell.auth_provider_registry_.GetSessions(std::string(provider_id));
  }
  static const microide::workspace::AuthProviderSpec* AuthProvider(
      const WorkspaceShell& shell,
      std::string_view provider_id) {
    return shell.auth_provider_registry_.GetProvider(std::string(provider_id));
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
  static bool SaveTab(WorkspaceShell& shell, std::size_t index) { return shell.SaveTab(index); }
  static void ActivateTab(WorkspaceShell& shell, std::size_t index) { shell.ActivateTab(index); }
  static bool SelectTreePath(WorkspaceShell& shell, const std::filesystem::path& path) {
    return shell.context_.current_project_state.directory_tree.SelectPath(path);
  }
  static void CollapseTreeSelection(WorkspaceShell& shell) { shell.context_.current_project_state.directory_tree.CollapseSelection(); }
  static void ShowSearchSidebar(WorkspaceShell& shell,
                                std::string query,
                                bool temporary = false) {
    shell.ShowSearchSidebar(std::move(query), temporary);
  }
  static void ShowProblemsSidebar(WorkspaceShell& shell) { shell.ShowProblemsSidebar(); }
  static void ShowGitSidebar(WorkspaceShell& shell) { shell.ShowGitSidebar(); }  static bool RefreshProblemsSidebar(WorkspaceShell& shell) {
    return shell.RefreshProblemsSidebar();
  }
  static void RefreshGitSidebar(WorkspaceShell& shell) { shell.RefreshGitSidebar(); }
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
  static bool ApplySelectedCompletion(WorkspaceShell& shell) {
    return shell.ApplySelectedCompletion();
  }
  static bool ExecuteSelectedCodeAction(WorkspaceShell& shell) {
    return shell.ExecuteSelectedCodeAction();
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
  static void EnsureTerminalTab(WorkspaceShell& shell) {
    if (shell.context_.current_project_state.terminal_tabs.empty()) {
      shell.context_.current_project_state.terminal_tabs.push_back(std::make_unique<WorkspaceShell::TerminalTabState>());
    }
    shell.context_.current_project_state.active_terminal_tab_index = shell.context_.current_project_state.terminal_tabs.size() - 1;
    shell.context_.current_project_state.panel.content = WorkspaceShell::PanelContentKind::Terminal;
    shell.context_.current_project_state.surface.focus = WorkspaceShell::FocusTarget::Panel;
  }
  static void ShowChatPanel(WorkspaceShell& shell) { shell.ShowChatPanel(); }
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
  static int ProjectTabScrollIndex(const WorkspaceShell& shell) {
    return shell.context_.project_catalog.tab_scroll_index;
  }
  static int EditorTabScrollIndex(const WorkspaceShell& shell) { return shell.context_.current_project_state.tab_scroll_index; }
  static SDL_FRect BottomPanelContentRect(WorkspaceShell& shell) {
    const WorkspaceLayout layout = CurrentLayout(shell);
    return microide::workspace::BottomPanelContentRect(layout, shell.context_.current_project_state.panel.command_mode);
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
         shell.ComputeVisibleProjectTabs(layout.project_tab_strip)) {
      if (tab.index == index) {
        return tab.rect;
      }
    }
    return {};
  }
  static std::string ProjectTabTooltipLabel(WorkspaceShell& shell, std::size_t index) {
    return shell.ProjectTabTooltipLabel(index);
  }
  static WorkspaceShell::VisibleStripTab::ChatStatus ProjectTabChatStatus(WorkspaceShell& shell,
                                                                          std::size_t index) {
