  static WorkspaceShell::ProjectWorkspaceState& CurrentProjectState(WorkspaceShell& shell) {
    return shell.context_.current_project_state;
  }
  static WorkspaceShell::ProjectWorkspaceState& ProjectState(WorkspaceShell& shell,
                                                             std::size_t index) {
    return *shell.context_.project_catalog.entries.at(index);
  }
  static void ConsumeLspCallbacks(WorkspaceShell& shell) { shell.ConsumeLspCallbacks(); }
  static void PublishLspDiagnostics(WorkspaceShell& shell, const std::filesystem::path& path,
                                    std::vector<LspClient::Diagnostic> diagnostics) {
    shell.lsp_service_.PublishLspDiagnostics(shell.context_.current_project_state, FileUriForPath(path),
                                             lsp_encoding::PositionEncoding::Utf16, std::move(diagnostics));
  }
  static bool HasPendingLspBufferOpen(const WorkspaceShell& shell) {
    return shell.lsp_service_.HasPendingBufferOpen();
  }
  static void BeginLspHoverForTesting(WorkspaceShell& shell, const std::filesystem::path& path,
                                      std::size_t line, std::size_t column) {
    shell.plugin_hover_cache_.path = path;
    shell.plugin_hover_cache_.line = line;
    shell.plugin_hover_cache_.column = column;
    shell.plugin_hover_cache_.state = WorkspaceShell::PluginHoverCache::State::Pending;
    shell.plugin_hover_cache_.result.title.clear();
    shell.plugin_hover_cache_.result.content.clear();
    ++shell.plugin_hover_cache_.generation;
    shell.KickOffLspHover(path, line, column);
  }
  static std::size_t HoverCacheLineForTesting(const WorkspaceShell& shell) {
    return shell.plugin_hover_cache_.line;
  }
  static std::string HoverCacheContentForTesting(const WorkspaceShell& shell) {
    return shell.plugin_hover_cache_.result.content;
  }
  static std::string ResolveLspHoverForTesting(WorkspaceShell& shell,
                                               const std::filesystem::path& path, std::size_t line,
                                               std::size_t column) {
    BeginLspHoverForTesting(shell, path, line, column);
    shell.ConsumeLspCallbacks();
    return HoverCacheContentForTesting(shell);
  }
  static void ApplyLiveSettings(WorkspaceShell& shell) { shell.ApplyLiveSettings(); }
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
  static void ActivateCodeLens(WorkspaceShell& shell, std::uint64_t payload) {
    shell.lsp_service_.ActivateCodeLens(payload);
    shell.ConsumeLspCallbacks();
  }
  static bool IsControlChannelRunning(const WorkspaceShell& shell) {
    return shell.control_channel_service_.IsRunning();
  }
  static void ConsumeControlCallbacks(WorkspaceShell& shell) { shell.ConsumeControlCallbacks(); }
  static ControlChannelService::CommandOutcome ExecuteControlCommand(
      WorkspaceShell& shell, const std::string& command_line) {
    return shell.ExecuteControlCommand(command_line);
  }
  static const std::vector<microide::project::ProjectId>& RecentProjects(const WorkspaceShell& shell) {
    return shell.recents_service_.RecentProjects();
  }
  static util::JsonValue ExecuteControlQuery(const WorkspaceShell& shell, const std::string& verb,
                                             std::string* error = nullptr) {
    bool ok = false;
    std::string refusal;
    util::JsonValue result = shell.control_channel_service_.AnswerQuery(verb, &ok, &refusal);
    if (error != nullptr) {
      *error = refusal;
    }
    return ok ? result : util::JsonValue();
  }
  static void SeedLaunchConfigs(WorkspaceShell& shell,
                                std::vector<microide::workspace::LaunchConfig> configs) {
    shell.context_.current_project_state.launch_configs = std::move(configs);
  }
  static void OpenLaunchConfigPicker(WorkspaceShell& shell) {
    shell.quick_pick_service_.Open(
        QuickPickService::LaunchConfigs(shell.context_.current_project_state.launch_configs));
  }
  static void SetLaunchConfigPickerQuery(WorkspaceShell& shell, const std::string& query) {
    shell.context_.current_project_state.overlay.workflow.quick_pick.query.SetText(query);
    shell.quick_pick_service_.Refilter();
  }
  static std::vector<std::string> QuickPickMatchLabels(const WorkspaceShell& shell) {
    std::vector<std::string> labels;
    for (const auto& match :
         shell.context_.current_project_state.overlay.workflow.quick_pick.matches) {
      labels.push_back(match.primary_label);
    }
    return labels;
  }
  static void SelectAndConfirmLaunchConfig(WorkspaceShell& shell, std::size_t match_index) {
    shell.context_.current_project_state.overlay.workflow.quick_pick.selected_index =
        match_index;
    shell.quick_pick_service_.Accept();
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
    return shell.MakeCompareMergeService().diff_tabs().OpenPlainComparison(std::move(left), std::move(right));
  }
  static const CompareInput& CompareSelection(const WorkspaceShell& shell) {
    return *shell.context_.current_project_state.compare_selection;
  }
  static bool CompareSelectionCameFromFile(const WorkspaceShell& shell) {
    return shell.context_.current_project_state.compare_selection_from_file;
  }
  static bool HasCompareSelection(const WorkspaceShell& shell) {
    return shell.context_.current_project_state.compare_selection.has_value();
  }
  static std::optional<EditorHoverTarget> EditorHoverTargetAtPosition(
      const WorkspaceShell& shell, float x, float y) {
    return shell.EditorHoverTargetAtPosition(x, y);
  }
  static std::optional<editor::CodeLensDecoration> AboveLensAtPosition(const WorkspaceShell& shell,
                                                                       float x, float y) {
    return shell.AboveLensAtPosition(x, y);
  }
  static std::optional<editor::CodeLensDecoration> CodeLensAtPosition(const WorkspaceShell& shell,
                                                                      float x, float y) {
    return shell.CodeLensAtPosition(x, y);
  }
  static bool SetDiagnosticsForTest(WorkspaceShell& shell, const std::filesystem::path& path,
                                    std::vector<editor::Diagnostic> diagnostics) {
    return shell.context_.current_project_state.diagnostics_store.ReplaceForOwnerFile(
        "test", path, std::move(diagnostics));
  }
  static void OpenMenuBarMenuForTest(WorkspaceShell& shell, WorkspaceShell::MenuId id) {
    shell.MakeMenuCoordinator().OpenMenuBarMenu(id);
  }
  static std::optional<SDL_FRect> PopupMenuRectForTest(WorkspaceShell& shell,
                                                       WorkspaceShell::MenuId id) {
    const auto layout = shell.CurrentWorkspaceLayout();
    return layout.has_value() ? shell.ComputePopupMenuRect(layout->menu_bar, id) : std::nullopt;
  }
  static void OpenSubmenuForTest(WorkspaceShell& shell, WorkspaceShell::MenuId id,
                                 const SDL_FRect& anchor_rect) {
    shell.MakeMenuCoordinator().OpenSubmenu(id, anchor_rect);
  }
  static void SetActiveMenuItemIndexForTest(WorkspaceShell& shell, int index) {
    shell.context_.menu_state.active_menu_item_index = index;
  }
  static void SetTreeContextMenuActiveItemIndexForTest(WorkspaceShell& shell, int index) {
    shell.context_.menu_state.tree_context_menu.active_item_index = index;
  }
  static void OpenMenuOverflowPopupForTest(WorkspaceShell& shell) {
    OpenMenuOverflowPopup(shell.context_.menu_state);
  }
  static bool AnyMenuPopupOpen(const WorkspaceShell& shell) {
    return shell.context_.menu_state.menu_bar_open ||
           shell.context_.menu_state.overflow_popup_open ||
           shell.context_.menu_state.tree_context_menu.open;
  }
  static std::size_t LastPaintedPopupRowCount(const WorkspaceShell& shell) {
    return shell.menu_popup_rows_scratch_.size();
  }
  static void ClearPaintedPopupRowsForTest(WorkspaceShell& shell) {
    shell.menu_popup_rows_scratch_.clear();
  }
  static void BeginTrackedLspRequest(WorkspaceShell& shell) { shell.lsp_service_.BeginTrackedLspRequest(); }
  static void FinishTrackedLspRequest(WorkspaceShell& shell) { shell.lsp_service_.FinishTrackedLspRequest(); }
  static void OpenTreeContextMenuForPath(WorkspaceShell& shell,
                                         TreeContextTargetKind target,
                                         const std::filesystem::path& path) {
    shell.MakeMenuCoordinator().OpenTreeContextMenu(target, path, SDL_FRect{0, 0, 0, 0});
  }
  static void SetTreeContextMenuTargetForTest(WorkspaceShell& shell, TreeContextTargetKind target) {
    shell.context_.menu_state.tree_context_menu.target = target;
  }
  static void SetProjectStripScrollForTest(WorkspaceShell& shell, int first_visible) {
    shell.context_.project_catalog.tab_scroll_index = first_visible;
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
  static std::size_t TreeContextMenuLine(const WorkspaceShell& shell) {
    return shell.context_.menu_state.tree_context_menu.line;
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
      labels.emplace_back(shell.MenuItemLabel(item, false));
    }
    return labels;
  }
  static std::vector<WorkspaceShell::VisiblePopupMenuItem> TreeContextMenuVisibleItems(
      const WorkspaceShell& shell) {
    const auto popup_rect = shell.ComputeTreeContextMenuRect();
    if (!popup_rect.has_value()) {
      return {};
    }
    return shell.ComputeVisiblePopupMenuItems(
        WorkspaceTreeContextMenuItems(shell.context_.menu_state.tree_context_menu.target),
        shell.context_.menu_state.tree_context_menu.active_item_index, *popup_rect);
  }
  static std::optional<SDL_FRect> TreeContextMenuRect(const WorkspaceShell& shell) {
    return shell.ComputeTreeContextMenuRect();
  }
  static bool GitEntryContextMenuOpen(const WorkspaceShell& shell) {
    return shell.context_.menu_state.tree_context_menu.open &&
           shell.context_.menu_state.tree_context_menu.target == TreeContextTargetKind::GitEntry;
  }
  static std::string TreeContextMenuActiveLabel(const WorkspaceShell& shell) {
    const auto& menu = shell.context_.menu_state.tree_context_menu;
    const auto items = WorkspaceTreeContextMenuItems(menu.target);
    if (!menu.open || menu.active_item_index < 0 ||
        static_cast<std::size_t>(menu.active_item_index) >= items.size()) {
      return {};
    }
    return std::string(shell.MenuItemLabel(items[static_cast<std::size_t>(menu.active_item_index)], false));
  }
  static std::vector<std::string> GitEntryContextMenuLabels(const WorkspaceShell& shell) {
    std::vector<std::string> labels;
    for (const auto& item : WorkspaceTreeContextMenuItems(TreeContextTargetKind::GitEntry)) {
      if (item.separator) {
        continue;
      }
      labels.emplace_back(shell.MenuItemLabel(item, false));
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
  static bool ExecuteBareMenuAction(WorkspaceShell& shell, WorkspaceShell::ActionId id) {
    return ActionCoordinator(shell.MakeActionContext()).Execute(id, {}, WorkspaceShell::ActionSource::Menu);
  }
  static const QuickPickState& QuickPick(const WorkspaceShell& shell) {
    return shell.context_.current_project_state.overlay.workflow.quick_pick;
  }
  static const std::string& FileFinderCompareTitle(const WorkspaceShell& shell) {
    return shell.context_.current_project_state.overlay.workflow.file_finder_compare_title;
  }
  static bool ExecuteContextMenuAction(WorkspaceShell& shell, WorkspaceShell::ActionId id) {
    return ActionCoordinator(shell.MakeActionContext())
        .Execute(id, {}, WorkspaceShell::ActionSource::ContextMenu);
  }
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
  static bool SaveTab(WorkspaceShell& shell, std::size_t index) {
    return shell.SaveTab(index, SaveMode::Blocking);
  }
  static bool SaveTabAs(WorkspaceShell& shell, std::size_t index, const std::filesystem::path& path,
                        std::string* error) {
    return shell.MakeEditorTabService().SaveGroupTabAs(
        shell.context_.current_project_state.clamped_focused_group_index(), index, path, error);
  }
  static void SyncLspForActiveEditableLastChange(WorkspaceShell& shell) {
    shell.lsp_service_.SyncLspForActiveEditableLastChange();
  }
  static void SetProjectWriteGate(WorkspaceShell& shell, microide::project::FileWriteGate& gate) {
    shell.context_.current_project_state.locality.write_gate = &gate;
  }
  static const microide::platform::ProcessLauncher& ProjectLauncher(const WorkspaceShell& shell) {
    return shell.context_.current_project_state.launcher();
  }
  static bool OpenProjectTabWithLocality(WorkspaceShell& shell,
                                         const std::filesystem::path& project_root,
                                         const microide::project::ProjectLocality& locality) {
    return shell.OpenProjectTab(project_root, false, false, locality);
  }
  static void ReloadCleanEditorTabsForPath(WorkspaceShell& shell,
                                           const std::filesystem::path& path) {
    shell.ReloadCleanEditorTabsForPath(path,
                                       microide::workspace::EditorReloadEchoGuard::AlreadyResolved);
  }
  static AssistService& Assist(WorkspaceShell& shell) { return shell.assist_service_; }
  static bool SaveTabDeferred(WorkspaceShell& shell, std::size_t index) {
    return shell.SaveTab(index, SaveMode::Deferred);
  }
  static bool SaveThenCloseTab(WorkspaceShell& shell, std::size_t index) {
    EditorTabService& editor_tabs = shell.MakeEditorTabService();
    PromptSurfaceService& prompt_surfaces = shell.MakePromptSurfaceService();
    return shell.MakeDirtyPromptCoordinator(editor_tabs, prompt_surfaces).SaveThenClose(index);
  }
  static int DrainSaveFormatterCompletions(WorkspaceShell& shell) {
    return shell.save_formatter_service_.DrainCompletions();
  }
  static void FlushPendingFileReads(WorkspaceShell& shell) {
    shell.file_read_service_.FlushPendingReads();
  }
  static void SettleCompareLoads(WorkspaceShell& shell) {
    for (int round = 0; round < 4; ++round) {
      bool posted = false;
      for (EditorGroup& group : shell.context_.current_project_state.editor_groups) {
        if (!group.has_active_tab() || !group.active_tab().compare.has_value()) {
          continue;
        }
        CompareTabState& compare = *group.active_tab().compare;
        posted = (compare.load_pending && !compare.pending_load.armed() &&
                  shell.MakeDiffTabCoordinator().StartPendingCompareLoad(compare)) ||
                 (compare.refresh_owed && !compare.pending_refresh.armed() &&
                  shell.MakeDiffTabCoordinator().StartCompareTabRefresh(compare)) ||
                 compare.pending_load.armed() || compare.pending_refresh.armed() || posted;
      }
      if (!posted) {
        return;
      }
      shell.file_read_service_.FlushPendingReads();
    }
  }
  static void DeliverCancelledCompareLoad(WorkspaceShell& shell, std::uint64_t id) {
    microide::workspace::CompareTabLoadResult empty;
    shell.MakeDiffTabCoordinator().ApplyCompareTabLoad(id, true,
                                                        false, empty);
  }
  static void DeliverCancelledCompareRefresh(WorkspaceShell& shell, std::uint64_t id) {
    (void)shell.MakeDiffTabCoordinator().ApplyCompareTabRefresh(id, true,
                                                                 microide::workspace::CompareRevisionSides{});
  }
  static bool OpenGitConflictMerge(WorkspaceShell& shell, const std::filesystem::path& path) {
    return shell.OpenGitConflictMerge(path);
  }
  static int PendingFileReadCount(const WorkspaceShell& shell) {
    return shell.file_read_service_.PendingCount();
  }
  static std::uint64_t PostedFileReadCount(const WorkspaceShell& shell) {
    return shell.file_read_service_.PostedCount();
  }
  static TabEntry::EditorTabState::Content GroupTabContentState(const WorkspaceShell& shell,
                                                                std::size_t group_index,
                                                                std::size_t tab_index) {
    const auto& groups = shell.context_.current_project_state.editor_groups;
    if (group_index >= groups.size() || tab_index >= groups[group_index].open_tabs.size()) {
      throw std::runtime_error("no such tab");
    }
    const TabEntry& tab = groups[group_index].open_tabs[tab_index];
    if (!tab.editor_state.has_value()) {
      throw std::runtime_error("tab has no editor state");
    }
    return tab.editor_state->content;
  }
  static TabEntry::EditorTabState::Content ActiveTabContentState(const WorkspaceShell& shell) {
    const TabEntry::EditorTabState* editor_tab = shell.ActiveEditorTab();
    return editor_tab != nullptr ? editor_tab->content
                                 : TabEntry::EditorTabState::Content::Ready;
  }
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
  static std::size_t DerivedUnitCount(const WorkspaceShell& shell, DerivedSurface surface) {
    return shell.frame_state_.derived_damage.LaidOutUnitCount(surface);
  }
  static void ShowProblemsSidebar(WorkspaceShell& shell) { shell.ShowProblemsSidebar(); }
  static void ShowTestsSidebar(WorkspaceShell& shell) { shell.ShowTestsSidebar(); }
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
    shell.MakeSidebarService().RevealSelectedGitLine();
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
    return shell.assist_service_.signature_help().Show(nullptr);
  }
  static std::optional<WorkspaceShell::SignatureHelpPopup> SignatureHelpPopup(
      const WorkspaceShell& shell) {
    return shell.active_signature_help_;
  }
  static void ExpireSignatureHelp(WorkspaceShell& shell) { shell.MaybeExpireSignatureHelp(); }
  static bool ApplySelectedCompletion(WorkspaceShell& shell) {
    return shell.assist_service_.completion().ApplySelectedCompletion();
  }
  static bool ExecuteSelectedCodeAction(WorkspaceShell& shell) {
    return shell.assist_service_.code_actions().ExecuteSelected();
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
    shell.patch_apply_service_.DrainCompletions();
  }
  static void ToggleProjectSearchHiddenFiles(WorkspaceShell& shell) {
    shell.ToggleProjectSearchHiddenFiles();
  }
  static void RequestProjectSearchReplaceAll(WorkspaceShell& shell) { shell.RequestProjectSearchReplaceAll(); }
  static void ReplaceAllProjectSearchMatches(WorkspaceShell& shell) {
    shell.ReplaceAllProjectSearchMatches();
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
  static bool ExpireNotificationsAt(WorkspaceShell& shell, std::uint64_t now_ms) {
    return shell.notification_service_.ExpireDue(now_ms);
  }
  static void Notify(WorkspaceShell& shell, NotificationService::Tone tone, std::string message) {
    shell.Notify(tone, std::move(message));
  }
  static void ShowNotification(WorkspaceShell& shell,
                               NotificationService::Tone tone,
                               std::string message) {
    shell.notification_service_.Show(tone, std::move(message), 0);
  }
  static SDL_FRect NotificationToastRect(WorkspaceShell& shell, std::size_t stack_position) {
    return NotificationsVm(shell).entries[stack_position].layout.rect;
  }
  static SDL_FRect NotificationButtonRect(WorkspaceShell& shell, std::size_t stack_position,
                                          std::size_t action) {
    return NotificationsVm(shell).entries[stack_position].buttons[action].rect;
  }
  static NotificationsViewModel NotificationsVm(WorkspaceShell& shell) {
    const WorkspaceLayout layout = CurrentLayout(shell);
    return RenderViewModelBuilder(shell.context_)
        .BuildNotifications(shell.notification_service_, layout.status_bar, shell.text_renderer_);
  }
  static void ShowNotificationRequest(WorkspaceShell& shell, NotificationService::Request request,
                                      std::uint64_t now_ms = 0) {
    shell.notification_service_.Show(std::move(request), now_ms);
  }
  static void EnsureTerminalTab(WorkspaceShell& shell) {
    if (shell.context_.current_project_state.terminal_tabs.empty()) {
      shell.context_.current_project_state.terminal_tabs.push_back(
          MakeTerminalTab(std::make_unique<WorkspaceShell::TerminalPaneState>()));
    }
    shell.context_.current_project_state.active_terminal_tab_index = shell.context_.current_project_state.terminal_tabs.size() - 1;
    shell.context_.current_project_state.panel.content = WorkspaceShell::PanelContentKind::Terminal;
    shell.context_.current_project_state.surface.focus = WorkspaceShell::FocusTarget::Panel;
  }
  static void AddTerminalTab(WorkspaceShell& shell) {
    shell.context_.current_project_state.terminal_tabs.push_back(
        MakeTerminalTab(std::make_unique<WorkspaceShell::TerminalPaneState>()));
    shell.context_.current_project_state.active_terminal_tab_index = shell.context_.current_project_state.terminal_tabs.size() - 1;
    shell.context_.current_project_state.panel.content = WorkspaceShell::PanelContentKind::Terminal;
    shell.context_.current_project_state.surface.focus = WorkspaceShell::FocusTarget::Panel;
  }
  static void ConsumeTerminalSessionUpdates(WorkspaceShell& shell) {
    shell.ConsumeTerminalSessionUpdates();
  }
  static std::size_t TerminalPaneCount(const WorkspaceShell& shell) {
    const auto* tab = shell.context_.current_project_state.active_terminal_tab();
    return tab == nullptr ? 0 : tab->pane_count();
  }
  static std::size_t ActiveTerminalPaneIndex(const WorkspaceShell& shell) {
    const auto* tab = shell.context_.current_project_state.active_terminal_tab();
    return tab == nullptr ? 0 : tab->active_pane;
  }
  static std::vector<float> TerminalPaneWeights(const WorkspaceShell& shell) {
    const auto* tab = shell.context_.current_project_state.active_terminal_tab();
    return tab == nullptr ? std::vector<float>{}
                          : std::vector<float>(tab->weights.begin(), tab->weights.end());
  }
  static microide::workspace::TerminalPaneRectsLayout TerminalPaneRects(WorkspaceShell& shell) {
    return shell.MakeTerminalPanelService().PaneRects(CurrentLayout(shell));
  }
  static microide::terminal::TerminalSession& TerminalPaneSession(WorkspaceShell& shell,
                                                                  std::size_t pane_index) {
    return shell.context_.current_project_state.active_terminal_tab()->panes[pane_index]->session;
  }
  static microide::terminal::TerminalSession& TerminalTabSession(WorkspaceShell& shell,
                                                                 std::size_t tab_index) {
    return shell.context_.current_project_state.terminal_tabs[tab_index]->active()->session;
  }
  static bool TerminalTabHasUnseenBell(const WorkspaceShell& shell, std::size_t tab_index) {
    return shell.context_.current_project_state.terminal_tabs[tab_index]->has_unseen_bell;
  }
  static bool TerminalTabHasUnseenOutput(const WorkspaceShell& shell, std::size_t tab_index) {
    return shell.context_.current_project_state.terminal_tabs[tab_index]->has_unseen_output;
  }
  static void TogglePanelMaximizedForTest(WorkspaceShell& shell) {
    shell.MakeTerminalPanelService().TogglePanelMaximized();
  }
  static bool PanelMaximized(const WorkspaceShell& shell) {
    return shell.context_.current_project_state.panel.maximized;
  }
  static std::filesystem::path ActiveTerminalPaneLaunchDirectory(const WorkspaceShell& shell) {
    const auto* pane = shell.context_.current_project_state.active_terminal_pane();
    return pane == nullptr ? std::filesystem::path{} : pane->launch_working_directory;
  }
  static std::string TerminalTabLabelPrefix(const WorkspaceShell& shell, std::size_t tab_index) {
    return shell.context_.current_project_state.terminal_tabs[tab_index]->active()->label_prefix;
  }
  static const platform::ProcessLauncher& TerminalTabLauncher(WorkspaceShell& shell, std::size_t tab_index) {
    const auto* pane = shell.context_.current_project_state.terminal_tabs[tab_index]->active();
    return pane->launcher != nullptr ? *pane->launcher : shell.context_.current_project_state.launcher();
  }
  static RemoteHostService& RemoteHosts(WorkspaceShell& shell) { return shell.MakeRemoteHostService(); }
  static void OpenTerminalViaService(WorkspaceShell& shell, std::string command = {}) {
    shell.MakeTerminalPanelService().OpenTerminal(std::move(command));
  }
  static bool ActivateTerminalTab(WorkspaceShell& shell, std::size_t index) {
    return shell.MakeTerminalPanelService().ActivateTerminalTab(index);
  }
  static bool TerminalPaneFollowTail(const WorkspaceShell& shell, std::size_t pane_index) {
    return shell.context_.current_project_state.active_terminal_tab()->panes[pane_index]->follow_tail;
  }
  static SDL_FRect BottomPanelMaximizeButtonRect(WorkspaceShell& shell) {
    const WorkspaceLayout layout = CurrentLayout(shell);
    const SDL_FRect panel_header =
        MakeRect(layout.bottom_panel.x, layout.bottom_panel.y, layout.bottom_panel.w,
                 kWorkspaceBottomPanelHeaderHeight);
    return shell.tab_strip_service_.BottomPanelMaximizeButtonRect(
        shell.layout_mode_service_.CurrentMode(), panel_header);
  }
  static SDL_FRect BottomPanelTerminalNewTabRect(WorkspaceShell& shell) {
    const WorkspaceLayout layout = CurrentLayout(shell);
    const SDL_FRect panel_header =
        MakeRect(layout.bottom_panel.x, layout.bottom_panel.y, layout.bottom_panel.w,
                 kWorkspaceBottomPanelHeaderHeight);
    return shell.tab_strip_service_.BottomPanelTerminalNewTabRect(
        shell.layout_mode_service_.CurrentMode(), panel_header);
  }
  static std::size_t TerminalTabCount(const WorkspaceShell& shell) {
    return shell.context_.current_project_state.terminal_tabs.size();
  }
  static microide::terminal::TerminalSession& ActiveTerminalSession(WorkspaceShell& shell) {
    return shell.context_.current_project_state.active_terminal_pane()->session;
  }
  static void SetActiveTerminalFollowTail(WorkspaceShell& shell, bool follow_tail) {
    shell.context_.current_project_state.active_terminal_pane()->follow_tail = follow_tail;
  }
  static bool ActiveTerminalFollowTail(const WorkspaceShell& shell) {
    return shell.context_.current_project_state.active_terminal_pane()->follow_tail;
  }
  static void SetActiveTerminalScrollRow(WorkspaceShell& shell, int scroll_row) {
    shell.context_.current_project_state.active_terminal_pane()->scroll_row = scroll_row;
  }
  static int ActiveTerminalScrollRow(const WorkspaceShell& shell) {
    return shell.context_.current_project_state.active_terminal_pane()->scroll_row;
  }
  static void AppendTerminalPendingInput(WorkspaceShell& shell, std::string_view input) {
    shell.AppendTerminalPendingInput(input);
  }
  static void SubmitTerminalPendingInput(WorkspaceShell& shell) {
    shell.SubmitTerminalPendingInput();
  }
  static std::size_t ActiveTerminalPendingInputSize(const WorkspaceShell& shell) {
    return shell.context_.current_project_state.active_terminal_pane()->pending_input.size();
  }
  static bool ActiveTerminalPendingInputTruncated(const WorkspaceShell& shell) {
    return shell.context_.current_project_state.active_terminal_pane()->pending_input_truncated;
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
  static bool RequestCommitWorkflowCommit(WorkspaceShell& shell) {
    return shell.RequestCommitWorkflowCommit();
  }
  static void FailGitRefreshesForTesting(WorkspaceShell& shell, std::string error) {
    shell.git_repository_service_.SetRepositoryStateProviderForTesting(
        [state = shell.git_repository_service_.CurrentState(), error](
            const std::filesystem::path&, std::uint64_t) {
          project::GitRepositoryState failed = state;
          failed.refresh_error = {
              .category = project::GitRefreshErrorCategory::UnknownError,
              .detail = error,
          };
          failed.stale = true;
          return failed;
        });
  }
  static CommitWorkflowState& CommitWorkflow(WorkspaceShell& shell) {
    return shell.context_.current_project_state.sidebar.git.commit_workflow;
  }
  static std::string CommitSubjectText(const WorkspaceShell& shell) {
    return shell.context_.current_project_state.sidebar.git.commit_workflow.subject.text();
  }
  static std::string CommitBodyText(const WorkspaceShell& shell) {
    return shell.context_.current_project_state.sidebar.git.commit_workflow.BodyText();
  }
  static const editor::TextViewport& CommitBodyViewport(const WorkspaceShell& shell) {
    return shell.context_.current_project_state.sidebar.git.commit_workflow.body;
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
  static std::optional<TerminalLinkSpan> TerminalHoveredLink(const WorkspaceShell& shell) {
    const auto* pane = shell.ActiveTerminalPane();
    return pane != nullptr ? pane->hovered_link : std::nullopt;
  }
  static std::optional<TerminalLinkSpan> TerminalPaneHoveredLink(const WorkspaceShell& shell,
                                                                 std::size_t index) {
    const auto* tab = shell.context_.current_project_state.active_terminal_tab();
    return tab != nullptr && index < tab->pane_count() ? tab->panes[index]->hovered_link
                                                       : std::nullopt;
  }
  static SDL_FPoint TerminalPaneCellPoint(WorkspaceShell& shell, std::size_t index,
                                          std::size_t row, std::size_t column) {
    const auto& tab = *shell.context_.current_project_state.active_terminal_tab();
    const WorkspaceLayout layout = CurrentLayout(shell);
    const auto rects = tab.PaneRects(microide::workspace::BottomPanelContentRect(layout));
    const auto panel_layout = shell.ComputeTerminalPaneLogLayout(
        rects.panes[index], layout.bottom_panel.h, *tab.panes[index],
        tab.panes[index]->session.LineCount());
    return SDL_FPoint{
        .x = panel_layout.text_x +
             static_cast<float>(column) * std::max(1.0f, shell.terminal_text_renderer_.CharWidth()) +
             1.0f,
        .y = panel_layout.text_y + static_cast<float>(row) * panel_layout.line_height +
             panel_layout.line_height * 0.5f,
    };
  }
  static std::optional<TerminalLinkHit> TerminalLinkAtPoint(const WorkspaceShell& shell, float x,
                                                       float y) {
    return shell.TerminalLinkAtPoint(x, y);
  }
  static SDL_FPoint TerminalCellPoint(WorkspaceShell& shell,
                                      std::size_t row,
                                      std::size_t column) {
    const auto* terminal_tab = shell.ActiveTerminalPane();
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
  static std::size_t TerminalVisibleRowCount(WorkspaceShell& shell) {
    const auto* terminal_tab = shell.ActiveTerminalPane();
    const std::size_t line_count =
        terminal_tab != nullptr ? terminal_tab->session.LineCount() : 0;
    const WorkspaceLayout layout = CurrentLayout(shell);
    return static_cast<std::size_t>(
        std::max(0, shell.ComputeBottomPanelLogLayout(layout, line_count).scroll.visible_rows));
  }
  static SDL_FPoint BottomPanelTextOrigin(WorkspaceShell& shell) {
    const auto* terminal_tab = shell.ActiveTerminalPane();
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
    shell.terminal_find_service_.Refresh(shell.ActiveTerminalPane());
  }
  static bool HandleTerminalFindKeyDown(WorkspaceShell& shell,
                                        SDL_Keycode key,
                                        SDL_Keymod modifiers) {
    SDL_KeyboardEvent event{};
    event.key = key;
    return shell.HandleTerminalFindKeyDown(event, modifiers);
  }
  static bool TerminalHasSelection(const WorkspaceShell& shell) {
    return TerminalPanelService::HasSelection(shell.context_.current_project_state);
  }
  static void SetActiveTerminalSelectionForTest(WorkspaceShell& shell,
                                                std::optional<TerminalSelectionPoint> anchor,
                                                std::optional<TerminalSelectionPoint> head) {
    if (TerminalPaneState* pane = shell.ActiveTerminalPane(); pane != nullptr) {
      pane->selection_anchor = anchor;
      pane->selection_head = head;
    }
  }
  static std::string ActiveTerminalSelectedText(WorkspaceShell& shell) {
    return TerminalPanelService::SelectedText(shell.context_.current_project_state);
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
  static std::optional<WorkspaceShell::VisibleStripTab> VisibleProjectTab(WorkspaceShell& shell,
                                                                         std::size_t index) {
    const WorkspaceLayout layout = CurrentLayout(shell);
    for (const WorkspaceShell::VisibleStripTab& tab :
         shell.tab_strip_chrome_.ComputeVisibleProjectTabs(layout.project_tab_strip)) {
      if (tab.index == index) {
        return tab;
      }
    }
    return std::nullopt;
  }
  static std::string ProjectTabTooltipLabel(WorkspaceShell& shell, std::size_t index) {
    return shell.ProjectTabTooltipLabel(index);
  }
  static std::string ProjectTabDisplayTitle(WorkspaceShell& shell, std::size_t index) {
    return shell.ProjectTabDisplayTitle(index);
  }
  static std::string ProjectLabel(const WorkspaceShell& shell) { return shell.ProjectLabel(); }
  static const microide::project::ProjectId& ProjectCatalogId(const WorkspaceShell& shell, std::size_t index) {
    return shell.context_.ProjectCatalogId(index);
  }
  static std::string TerminalPanesSummary(WorkspaceShell& shell) {
    std::string out;
    const auto describe = [&out](const ProjectWorkspaceState& state, bool active) {
      for (const auto& tab : state.terminal_tabs) {
        for (const auto& pane : tab->panes) {
          if (pane == nullptr) {
            continue;
          }
          std::string first;
          for (const auto& line : pane->session.SnapshotLines()) {
            for (const auto& cell : line.cells) {
              first += cell.DisplayText().empty() ? std::string(" ") : std::string(cell.DisplayText());
            }
            if (first.find_first_not_of(' ') != std::string::npos) {
              break;
            }
            first.clear();
          }
          out += "\n  " + std::string(active ? "[active] " : "[background] ") + state.root.generic_string() +
                 ": pending=" + (pane->session.host_pending() ? "yes" : "no") +
                 " running=" + (pane->session.running() ? "yes" : "no") +
                 " handle=" + std::to_string(pane->session.host_identity().handle) + " screen=\"" +
                 first.substr(0, first.find_last_not_of(' ') + 1) + "\"";
        }
      }
    };
    describe(shell.context_.current_project_state, true);
    for (std::size_t i = 0; i < shell.context_.project_catalog.entries.size(); ++i) {
      const ProjectWorkspaceState* state = shell.context_.project_catalog.entries[i].get();
      if (state != nullptr && i != shell.context_.project_catalog.active_index) {
        describe(*state, false);
      }
    }
    return out.empty() ? std::string(" (no terminal panes)") : out;
  }
  static bool ProjectCatalogEntryInitialized(const WorkspaceShell& shell, std::size_t index) {
    const auto* entry = shell.context_.ProjectCatalogEntry(index);
    return entry != nullptr && entry->initialized;
  }
