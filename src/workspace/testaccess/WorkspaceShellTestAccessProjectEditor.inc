  static void SetProjectBaseColor(WorkspaceShell& shell, SDL_Color color) {
    shell.context_.current_project_state.project_base_color = color;
  }
  static void SetProjectRoot(WorkspaceShell& shell, const std::filesystem::path& root) {
    shell.context_.current_project_state.root = root.lexically_normal();
    shell.context_.current_project_state.directory_tree.SetRoot(shell.context_.current_project_state.root);
    shell.context_.current_project_state.file_index.SetRoot(shell.context_.current_project_state.root);
    shell.context_.current_project_state.file_finder.SetIndex(&shell.context_.current_project_state.file_index);
    shell.context_.current_project_state.sidebar.visible = true;
    shell.context_.current_project_state.sidebar.view_id = "tree";
    shell.context_.current_project_state.surface.focus = WorkspaceShell::FocusTarget::Sidebar;
  }
  static void PerfPrimeGitRepository(WorkspaceShell& shell, const std::filesystem::path& root) {
    shell.project_background_executor_.Cancel();
    shell.git_repository_service_.Reset();
    shell.context_.current_project_state.root = root.lexically_normal();
    shell.context_.current_project_state.sidebar.git.refreshing = false;
    shell.context_.current_project_state.sidebar.git.entries.clear();
    shell.context_.current_project_state.sidebar.visible = true;
    shell.context_.current_project_state.sidebar.view_id = "git";
    shell.context_.current_project_state.surface.focus = WorkspaceShell::FocusTarget::Sidebar;
  }
  static void PerfRunGitSidebarRefreshSync(WorkspaceShell& shell) {
    shell.context_.current_project_state.sidebar.git.refreshing = true;
    shell.git_repository_service_.RunRefreshSynchronouslyForTesting(
        shell.context_.current_project_state.root, GitSidebarRefreshScope::Full,
        shell.context_.current_project_state.sidebar.git.outgoing_base_choice,
        shell.context_.current_project_state.sidebar.git.tree_git_badges_materialized);
    shell.ConsumeGitSidebarRefresh();
  }

  static void OpenSingleEditorTab(WorkspaceShell& shell, const std::filesystem::path& path) {
    editor::TextViewport opened_view;
    if (!opened_view.OpenFile(path)) {
      throw std::runtime_error("failed to open editor fixture: " + path.string());
    }
    shell.ApplyEditorPreferences(opened_view);
    shell.ApplyDetectedIndentOnOpen(opened_view);
    shell.context_.current_project_state.focused_group().welcome_surface.viewport = opened_view;
    shell.context_.current_project_state.focused_group().open_tabs.push_back(WorkspaceShell::TabEntry{
        .kind = WorkspaceShell::TabEntry::Kind::Editor,
        .path = path.lexically_normal(),
        .title = path.filename().string(),
        .editor_state = WorkspaceShell::MakeEditorTabState(opened_view),
        .deferred_handle = std::nullopt,
        .compare = std::nullopt,
        .merge = std::nullopt,
    });
    shell.context_.current_project_state.focused_group().active_tab_index = 0;
    shell.context_.current_project_state.surface.focus = WorkspaceShell::FocusTarget::Editor;
  }

  static WorkspaceShell::TabEntry::EditorTabState MakeEditorTabStateForTesting(
      const editor::TextViewport& view) {
    return WorkspaceShell::MakeEditorTabState(view);
  }

  static editor::TextViewport& ActiveEditor(WorkspaceShell& shell) {
    return *shell.ActiveEditorViewport();
  }

  static void ApplyEditorPreferencesToAllTabs(WorkspaceShell& shell) {
    shell.ApplyEditorPreferencesToAllTabs();
  }

  static editor::FoldingModel* EnsureActiveFoldingModelFresh(WorkspaceShell& shell) {
    return shell.EnsureActiveFoldingModelFresh();
  }
  static editor::FoldingModel* ActiveTabFoldingModelForTesting(WorkspaceShell& shell) {
    WorkspaceShell::TabEntry::EditorTabState* editor_tab = shell.ActiveEditorTab();
    return editor_tab != nullptr ? editor_tab->folding_model.get() : nullptr;
  }
  static workspace::LspManager& LspManagerForTesting(WorkspaceShell& shell) {
    return shell.CurrentLspManager();
  }
  static bool ActiveEditorHasSelection(const WorkspaceShell& shell) {
    const editor::TextViewport* viewport = shell.ActiveEditorViewport();
    return viewport != nullptr && viewport->selection_range().has_value();
  }
  static std::string ActiveEditorSelectedText(WorkspaceShell& shell) {
    return shell.ActiveEditorViewport()->SelectedText();
  }
  static WorkspaceShell::CompareTabState& ActiveCompare(WorkspaceShell& shell) {
    return shell.context_.current_project_state.focused_group().open_tabs[shell.context_.current_project_state.focused_group().active_tab_index].compare.value();
  }
  static WorkspaceShell::MergeTabState& ActiveMerge(WorkspaceShell& shell) {
    return shell.context_.current_project_state.focused_group().open_tabs[shell.context_.current_project_state.focused_group().active_tab_index].merge.value();
  }
  static bool ActiveMergeHasSelection(const WorkspaceShell& shell) {
    return shell.context_.current_project_state.focused_group().open_tabs[shell.context_.current_project_state.focused_group().active_tab_index]
        .merge->result_viewport.selection_range()
        .has_value();
  }
  static std::string ActiveMergeSelectedText(WorkspaceShell& shell) {
    return shell.context_.current_project_state.focused_group().open_tabs[shell.context_.current_project_state.focused_group().active_tab_index].merge->result_viewport.SelectedText();
  }
  static void ApplyMergeChoice(WorkspaceShell& shell, microide::compare::MergeChoice choice) {
    shell.ApplyMergeChoice(choice);
  }
  static void ResetMergeHunk(WorkspaceShell& shell) { shell.ResetMergeHunk(); }
  static void ToggleMergeBasePane(WorkspaceShell& shell) { shell.ToggleMergeBasePane(); }
  static void MarkMergeResolved(WorkspaceShell& shell) { shell.MarkMergeResolved(); }
  static void RefreshMergeTabDerivedState(WorkspaceShell& shell) {
    shell.RefreshMergeTabDerivedState(ActiveMerge(shell));
  }
  static const std::optional<WorkspaceShell::MergeHoverState>& ActiveMergeHoverState(
      const WorkspaceShell& shell) {
    return shell.context_.current_project_state.focused_group().open_tabs[shell.context_.current_project_state.focused_group().active_tab_index].merge->hover_state;
  }
  static bool ActiveMergeHoverIsIncomingConflict(const WorkspaceShell& shell) {
    return shell.context_.current_project_state.focused_group().open_tabs[shell.context_.current_project_state.focused_group().active_tab_index].merge->hover_state.has_value() &&
           shell.context_.current_project_state.focused_group().open_tabs[shell.context_.current_project_state.focused_group().active_tab_index].merge->hover_state->kind ==
               WorkspaceShell::MergeHoverState::Kind::IncomingConflict;
  }
  static microide::compare::MergeChoice ActiveMergeHoverPreviewChoice(const WorkspaceShell& shell) {
    const auto& hover = shell.context_.current_project_state.focused_group().open_tabs[shell.context_.current_project_state.focused_group().active_tab_index].merge->hover_state;
    return hover.has_value() ? hover->preview_choice : microide::compare::MergeChoice::Base;
  }
  static WorkspaceLayout CurrentLayout(WorkspaceShell& shell) {
    return shell.CurrentWorkspaceLayout().value();
  }
  static WorkspaceShell::MergeSurfaceLayout ActiveMergeSurfaceLayout(WorkspaceShell& shell) {
    const WorkspaceLayout layout = CurrentLayout(shell);
    return shell.ComputeMergeSurfaceLayout(layout.editor_surface, ActiveMerge(shell));
  }
  static WorkspaceShell::MergeInteractionLayout ActiveMergeInteractionLayout(WorkspaceShell& shell) {
    auto& merge = ActiveMerge(shell);
    const WorkspaceLayout layout = CurrentLayout(shell);
    const auto surface = shell.ComputeMergeSurfaceLayout(layout.editor_surface, merge);
    const auto scroll =
        shell.ComputeMergeScrollLayout(layout.editor_surface, surface, merge);
    merge.scroll_row = scroll.vertical_scroll;
    merge.horizontal_scroll = scroll.horizontal_scroll;
    merge.result_viewport.SetScrollLine(
        static_cast<std::size_t>(std::max(0, merge.scroll_row)));
    merge.result_viewport.SetHorizontalScroll(merge.horizontal_scroll);
    merge.scroll_row = static_cast<int>(merge.result_viewport.scroll_line());
    merge.horizontal_scroll = merge.result_viewport.horizontal_scroll();
    return shell.BuildMergeInteractionLayout(layout.editor_surface, surface, merge);
  }
  static WorkspaceShell::CompareSurfaceLayout ActiveCompareSurfaceLayout(WorkspaceShell& shell) {
    const WorkspaceLayout layout = CurrentLayout(shell);
    return shell.ComputeCompareSurfaceLayout(layout.editor_surface, ActiveCompare(shell));
  }
  static std::size_t ActiveComparePresentationRowCount(WorkspaceShell& shell) {
    return CompareTabPresentationRowCount(ActiveCompare(shell));
  }
  static compare::ComparePresentationRowKind ActiveComparePresentationRowKind(
      WorkspaceShell& shell,
      std::size_t presentation_row) {
    const compare::ComparePresentationRow* row =
        CompareTabPresentationRowAt(ActiveCompare(shell), presentation_row);
    return row != nullptr ? row->kind : compare::ComparePresentationRowKind::Metadata;
  }
  static int ActiveCompareCollapsedLineCount(WorkspaceShell& shell,
                                             std::size_t presentation_row) {
    const compare::ComparePresentationRow* row =
        CompareTabPresentationRowAt(ActiveCompare(shell), presentation_row);
    return row != nullptr ? row->collapsed_line_count : 0;
  }
  static std::pair<std::size_t, std::size_t> ActiveCompareCollapsedRunIdentity(
      WorkspaceShell& shell,
      std::size_t presentation_row) {
    const compare::ComparePresentationRow* row =
        CompareTabPresentationRowAt(ActiveCompare(shell), presentation_row);
    if (row == nullptr) {
      return {0, 0};
    }
    return {row->collapsed_run_start_model_row, row->collapsed_run_length};
  }
  static std::optional<CompareHoverKind> ActiveCompareHoverKind(const WorkspaceShell& shell) {
    const auto& compare = shell.context_.current_project_state
                              .focused_group().open_tabs[shell.context_.current_project_state.focused_group().active_tab_index]
                              .compare;
    return compare.has_value() && compare->hover_state.has_value()
               ? std::optional<CompareHoverKind>(compare->hover_state->kind)
               : std::nullopt;
  }
  static std::optional<std::size_t> ActiveCompareHoverPresentationRow(const WorkspaceShell& shell) {
    const auto& compare = shell.context_.current_project_state
                              .focused_group().open_tabs[shell.context_.current_project_state.focused_group().active_tab_index]
                              .compare;
    return compare.has_value() && compare->hover_state.has_value()
               ? std::optional<std::size_t>(compare->hover_state->presentation_row)
               : std::nullopt;
  }
  static CollapsedContextActionRects ActiveCompareCollapsedContextActionRects(
      WorkspaceShell& shell,
      std::size_t presentation_row) {
    const WorkspaceLayout layout = CurrentLayout(shell);
    auto& compare = ActiveCompare(shell);
    const auto surface = shell.ComputeCompareSurfaceLayout(layout.editor_surface, compare);
    const int visible_row = static_cast<int>(presentation_row) - compare.scroll_row;
    if (visible_row < 0 || visible_row >= surface.visible_rows) {
      return {};
    }
    const compare::ComparePresentationRow* row =
        CompareTabPresentationRowAt(compare, presentation_row);
    if (row == nullptr || row->kind != compare::ComparePresentationRowKind::CollapsedContext) {
      return {};
    }
    const SDL_FRect row_rect = MakeRect(layout.editor_surface.x,
                                        surface.rows_y +
                                            static_cast<float>(visible_row) * surface.line_height - 1.0f,
                                        layout.editor_surface.w, surface.line_height);
    return BuildCollapsedContextActionRects(shell.text_renderer_, row_rect,
                                            row->previous_hunk_index >= 0,
                                            row->next_hunk_index >= 0);
  }
  static SDL_FRect ActiveCompareEditableRect(WorkspaceShell& shell) {
    auto& compare = ActiveCompare(shell);
    const auto surface = ActiveCompareSurfaceLayout(shell);
    return shell.BuildCompareRightInteractionLayout(surface, compare).rect;
  }
  static SDL_FRect ActiveMergeResultRect(WorkspaceShell& shell) {
    const WorkspaceLayout layout = CurrentLayout(shell);
    const auto surface = ActiveMergeSurfaceLayout(shell);
    return ComputeMergeResultViewportRect(layout.editor_surface, surface.center_x, surface.rows_y,
                                          surface.gutter_width, surface.center_width,
                                          surface.show_horizontal);
  }
  static SDL_FRect MergeSourceAcceptRect(WorkspaceShell& shell,
                                         std::size_t conflict_index,
                                         bool incoming) {
    auto& merge = ActiveMerge(shell);
    const auto surface = ActiveMergeSurfaceLayout(shell);
    const auto interaction = ActiveMergeInteractionLayout(shell);
    const auto& conflict = merge.conflicts[conflict_index];
    return shell.BuildMergeSourceActionButtonRect(surface, interaction, conflict, incoming);
  }
  static std::array<SDL_FRect, 4> MergeResultActionRects(WorkspaceShell& shell,
                                                         std::size_t conflict_index) {
    auto& merge = ActiveMerge(shell);
    const auto surface = ActiveMergeSurfaceLayout(shell);
    const auto interaction = ActiveMergeInteractionLayout(shell);
    const auto& conflict = merge.conflicts[conflict_index];
    return shell.BuildMergeResultActionButtonRects(surface, interaction, conflict);
  }
  static std::array<SDL_FRect, 2> MergeToolbarNavigationRects(WorkspaceShell& shell) {
    const WorkspaceLayout layout = CurrentLayout(shell);
    const auto surface = ActiveMergeSurfaceLayout(shell);
    const auto toolbar = shell.ComputeMergeToolbarLayout(layout.editor_surface, surface);
    return {toolbar.prev_rect, toolbar.next_rect};
  }
  static std::array<SDL_FRect, 2> MergeDividerRects(WorkspaceShell& shell) {
    const WorkspaceLayout layout = CurrentLayout(shell);
    const auto surface = ActiveMergeSurfaceLayout(shell);
    return {MakeRect(surface.center_x - surface.divider_width, layout.editor_surface.y,
                     surface.divider_width, layout.editor_surface.h),
            MakeRect(surface.right_x - surface.divider_width, layout.editor_surface.y,
                     surface.divider_width, layout.editor_surface.h)};
  }

  static void PrepareRenamePrompt(WorkspaceShell& shell,
                                  const std::filesystem::path& path,
                                  std::string input) {
    shell.OpenPromptSurface(WorkspaceShell::PromptSurfaceState::Action::RenamePath,
                            WorkspaceShell::PromptSurfaceState::Kind::TextInput, path,
                            std::move(input));
  }

  static void PrepareDeletePrompt(WorkspaceShell& shell, const std::filesystem::path& path) {
    shell.OpenPromptSurface(WorkspaceShell::PromptSurfaceState::Action::DeletePath,
                            WorkspaceShell::PromptSurfaceState::Kind::Confirm, path);
  }
  static void PrepareDiscardAllGitPrompt(WorkspaceShell& shell) {
    shell.OpenDiscardAllGitSidebarPrompt();
  }

  static void ConfirmPromptSurface(WorkspaceShell& shell) { shell.ConfirmPromptSurface(); }
  static void ConfirmPromptSurface(WorkspaceShell& shell, int selected_button) {
    shell.context_.prompts.surface.selected_button = selected_button;
    shell.ConfirmPromptSurface();
  }
  static bool ReplaceActiveEditorWithFile(WorkspaceShell& shell, const std::filesystem::path& path) {
    editor::TextViewport opened_view;
    if (!opened_view.OpenFile(path)) {
      return false;
    }
    return shell.ReplaceActiveEditorView(opened_view);
  }
  static bool OpenWorkingTreeComparison(WorkspaceShell& shell,
                                        const std::filesystem::path& path,
                                        const std::string& left_ref,
                                        const std::string& left_label) {
    return shell.OpenWorkingTreeComparison(path, left_ref, left_label);
  }
  static bool OpenComparePickerForPath(WorkspaceShell& shell,
                                       const std::filesystem::path& path) {
    return shell.OpenComparePickerForPath(path, {});
  }
  static void StageCompareHunk(WorkspaceShell& shell) { shell.StageCompareHunk(); }
  static void StageCompareSelectedLines(WorkspaceShell& shell) {
    shell.StageCompareSelectedLines();
  }
  static bool OpenBranchHeadComparison(WorkspaceShell& shell,
                                       const std::filesystem::path& path,
                                       const std::string& left_ref,
                                       const std::string& left_label,
                                       const std::string& right_ref,
                                       const std::string& right_label) {
    return shell.OpenBranchHeadComparison(path, left_ref, left_label, right_ref, right_label);
  }
  static bool OpenMergeEditor(WorkspaceShell& shell,
                              const std::filesystem::path& base_path,
                              const std::filesystem::path& incoming_path,
                              const std::filesystem::path& current_path,
                              const std::filesystem::path& output_path) {
    return shell.OpenMergeEditor(base_path, incoming_path, current_path, output_path);
  }
  static bool OpenProjectTab(WorkspaceShell& shell,
                             const std::filesystem::path& project_root,
                             bool restore_persistence = false,
                             bool log_feedback = false) {
    return shell.OpenProjectTab(project_root, restore_persistence, log_feedback);
  }
  static void SetWindowSize(WorkspaceShell& shell, int width, int height) {
    shell.window_presentation_.logical_width = width;
    shell.window_presentation_.logical_height = height;
    shell.MarkLayoutDirty();
  }
  static std::size_t PrepareFrameLayoutComputeCount(const WorkspaceShell& shell) {
    return shell.prepare_frame_layout_compute_count_;
  }
  static void ResetPrepareFrameLayoutComputeCount(WorkspaceShell& shell) {
    shell.prepare_frame_layout_compute_count_ = 0;
  }
  static void SetWindowChromeEnabled(WorkspaceShell& shell,
                                     bool enabled,
                                     bool maximized = false,
                                     bool fullscreen = false) {
    shell.window_presentation_.chrome = WorkspaceShell::WindowChromeState{
        .custom_enabled = enabled,
        .maximized = maximized,
        .fullscreen = fullscreen,
    };
  }
  static void SetLayoutMode(WorkspaceShell& shell, microide::workspace::LayoutMode mode) {
    shell.layout_mode_service_.SetCurrentMode(mode);
  }
  static void RenderFrame(WorkspaceShell& shell) {
    shell.Render(nullptr, shell.window_presentation_.logical_width,
                 shell.window_presentation_.logical_height);
  }
  static bool ExecuteProjectOpenFromMenu(WorkspaceShell& shell) {
    return ActionCoordinator(shell.MakeActionContext()).Execute(
        WorkspaceShell::ActionId::ProjectOpen, {}, WorkspaceShell::ActionSource::Menu);
  }
  static bool ExecuteProjectOpenFromCommand(WorkspaceShell& shell) {
    return ActionCoordinator(shell.MakeActionContext()).Execute(
        WorkspaceShell::ActionId::ProjectOpen, {}, WorkspaceShell::ActionSource::Command);
  }
  static void ResetProjectScopedState(WorkspaceShell& shell, bool show_welcome) {
    shell.ResetProjectScopedState(show_welcome);
  }
  static bool ExecuteFilesFromShortcut(WorkspaceShell& shell) {
    return ActionCoordinator(shell.MakeActionContext()).Execute(WorkspaceShell::ActionId::Files, {},
                                            WorkspaceShell::ActionSource::Shortcut);
  }
  static void SetFileFinderQuery(WorkspaceShell& shell, std::string query) {
    shell.context_.current_project_state.file_finder.SetQuery(std::move(query));
  }
  static void SetClipboardTextReader(
      WorkspaceShell& shell,
      std::function<std::optional<std::string>()> reader) {
    shell.clipboard_text_reader_ = std::move(reader);
  }
  static void SetClipboardTextWriter(WorkspaceShell& shell,
                                     std::function<bool(std::string_view)> writer) {
    shell.clipboard_text_writer_ = std::move(writer);
  }
  static void SetPrimarySelectionTextReader(
      WorkspaceShell& shell,
      std::function<std::optional<std::string>()> reader) {
    shell.primary_selection_text_reader_ = std::move(reader);
  }
  static void SetPrimarySelectionTextWriter(
      WorkspaceShell& shell,
      std::function<bool(std::string_view)> writer) {
    shell.primary_selection_text_writer_ = std::move(writer);
  }
  static void SetExternalUrlOpener(WorkspaceShell& shell,
                                   std::function<bool(std::string_view)> opener) {
    shell.external_url_opener_ = std::move(opener);
  }
  static void SetFileManagerOpener(
      WorkspaceShell& shell,
      std::function<bool(const std::filesystem::path&)> opener) {
    shell.file_manager_opener_ = std::move(opener);
  }
  static void SetProjectOpenDialogLauncher(
      WorkspaceShell& shell,
      std::function<bool(WorkspaceShell&, const std::filesystem::path&)> launcher) {
    shell.project_dialog_state_.launcher = std::move(launcher);
  }
  static void QueueProjectOpenDialogSelection(WorkspaceShell& shell,
                                              const std::filesystem::path& path) {
    std::lock_guard<std::mutex> lock(shell.project_dialog_state_.mutex);
    shell.project_dialog_state_.pending_result = microide::workspace::PendingProjectOpenDialogResult{
        .ready = true,
        .cancelled = false,
        .selected_path = path.lexically_normal(),
        .error_message = {},
    };
  }
  static void QueueProjectOpenDialogCancel(WorkspaceShell& shell) {
    std::lock_guard<std::mutex> lock(shell.project_dialog_state_.mutex);
    shell.project_dialog_state_.pending_result = microide::workspace::PendingProjectOpenDialogResult{
        .ready = true,
        .cancelled = true,
        .selected_path = {},
        .error_message = {},
    };
  }
  static void ConsumePendingProjectOpenDialogResult(WorkspaceShell& shell) {
    shell.ConsumePendingProjectOpenDialogResult();
  }
  static bool SwitchProject(WorkspaceShell& shell,
                            std::size_t index,
                            bool log_feedback = false) {
    return shell.SwitchProject(index, log_feedback);
  }
  static void RequestCloseProject(WorkspaceShell& shell, std::size_t index) {
    shell.RequestCloseProject(index);
  }
  static void CloseProject(WorkspaceShell& shell, std::size_t index) { shell.CloseProject(index); }
  static void OpenFile(WorkspaceShell& shell, const std::filesystem::path& path) {
    shell.OpenFile(path);
  }
  static bool OpenFileInNewTab(WorkspaceShell& shell, const std::filesystem::path& path) {
    return shell.OpenFileInNewTab(path);
  }
  static bool SplitEditorGroup(WorkspaceShell& shell,
                               microide::workspace::EditorSplitOrientation orientation) {
    return shell.SplitEditorGroup(orientation);
  }
  static bool FocusOtherEditorGroup(WorkspaceShell& shell) {
    return shell.FocusOtherEditorGroup();
  }
  static bool CloseEditorGroup(WorkspaceShell& shell) { return shell.CloseEditorGroup(); }
  static std::size_t EditorGroupCount(WorkspaceShell& shell) { return shell.EditorGroupCount(); }
  static std::size_t CountOpenBufferViews(WorkspaceShell& shell,
                                          const std::filesystem::path& path) {
    return shell.CountOpenBufferViews(path);
  }
  static std::unordered_map<std::string, std::size_t> OpenBufferViewCounts(WorkspaceShell& shell) {
    return shell.OpenBufferViewCounts();
  }
  static std::size_t FocusedGroupIndex(WorkspaceShell& shell) {
    return shell.context_.current_project_state.focused_group_index;
  }
  static microide::workspace::EditorSplitOrientation GroupSplitOrientation(WorkspaceShell& shell) {
    return shell.context_.current_project_state.group_split_orientation;
  }
  // The active editor viewport of group `gi` (path + scroll line live here).
  static const editor::TextViewport& GroupActiveViewport(WorkspaceShell& shell, std::size_t gi) {
    auto& group = shell.context_.current_project_state.editor_groups[gi];
    return *shell.GroupActiveViewport(group);
  }
  static void SetGroupScrollLine(WorkspaceShell& shell, std::size_t gi, std::size_t line) {
    auto& group = shell.context_.current_project_state.editor_groups[gi];
    shell.GroupActiveViewport(group)->SetScrollLine(line);
  }
  static void RegisterVirtualDocument(WorkspaceShell& shell,
                                      const microide::workspace::VirtualDocumentSpec& spec) {
    shell.virtual_document_registry_.Register(spec);
  }
  static void UpdateVirtualDocumentContent(WorkspaceShell& shell,
                                           std::string_view uri,
                                           std::string content) {
    shell.virtual_document_registry_.UpdateContent(std::string(uri), content);
  }
  static bool OpenVirtualDocument(WorkspaceShell& shell, std::string_view uri) {
    return shell.OpenVirtualDocumentInNewTab(uri);
  }
  static bool ExecuteCommandLine(WorkspaceShell& shell, const std::string& command_line) {
    return shell.MakeCommandLineCoordinator().ExecuteCommandLine(command_line);
  }
  static const std::vector<std::string>& PluginMessages(const WorkspaceShell& shell) {
    return shell.plugin_runtime_.Host().Messages();
  }
  static int PluginPendingAsyncProcessCount(const WorkspaceShell& shell) {
    return shell.plugin_runtime_.Host().PendingAsyncProcessCount();
  }
  static const std::vector<std::string>& PluginErrors(const WorkspaceShell& shell) {
    return shell.plugin_runtime_.Host().Errors();
  }
  static std::size_t SaveParticipantCount(const WorkspaceShell& shell) {
    return shell.save_participant_registry_.Specs().size();
  }
  static bool HasFormatterForLanguage(WorkspaceShell& shell, std::string_view language_id) {
    return shell.formatter_registry_.FindFormatter(std::string(language_id)) != nullptr;
  }
  static bool RunSaveParticipantsForTesting(WorkspaceShell& shell,
                                            const std::filesystem::path& path,
                                            std::string* text,
                                            std::string* error_message) {
    return shell.plugin_runtime_.Host().RunSaveParticipants(path, text, error_message);
  }
  static bool PrepareEditorViewportForSaveForTesting(WorkspaceShell& shell,
                                                     const std::filesystem::path& path,
                                                     editor::TextViewport& viewport,
                                                     std::string* error_message) {
    return shell.PrepareEditorViewportForSave(path, viewport, error_message);
  }
  static const std::vector<plugin::PluginHost::ContributedAuthProvider>& ContributedAuthProviders(
      const WorkspaceShell& shell) {
    return shell.plugin_runtime_.Host().ContributedAuthProviders();
  }
  static void ClearPluginMessages(WorkspaceShell& shell) {
    shell.plugin_runtime_.Host().ClearMessages();
  }
  static void SetPluginAssetPollInterval(WorkspaceShell& shell,
                                         std::chrono::milliseconds poll_interval) {
    shell.plugin_runtime_.SetPollInterval(poll_interval);
  }
  static void RegisterLifecycleWakeEvents(WorkspaceShell& shell) {
    shell.RegisterLifecycleWakeEvents();
  }
  static bool ReloadProjectIfFilesChanged(WorkspaceShell& shell, bool force_check) {
    return shell.ReloadProjectIfFilesChanged(force_check);
  }
  static void SetFileIndexHasPendingChanges(WorkspaceShell& shell, bool pending) {
    shell.file_index_has_pending_changes_.store(pending, std::memory_order_release);
  }
  static bool ApplyFileIndexBatchForTesting(WorkspaceShell& shell,
                                            const platform::IndexUpdateBatch& batch) {
    const bool changed = shell.context_.current_project_state.file_index.ApplyBatch(batch);
    if (changed) {
      shell.file_index_has_pending_changes_.store(true, std::memory_order_release);
    }
    return changed;
  }
  static bool PluginHostEnabled(const WorkspaceShell& shell) {
    return shell.plugin_runtime_.Host().enabled();
  }
  static std::size_t LoadedPluginCount(const WorkspaceShell& shell) {
    return shell.plugin_runtime_.Host().LoadedPluginCount();
  }
  static std::string PluginRuntimeReloadSummary(const WorkspaceShell& shell) {
    return shell.PluginRuntimeReloadSummary();
  }
  static const WorkspaceStartupOptions& StartupOptions(const WorkspaceShell& shell) {
    return shell.startup_options_;
  }
  static std::size_t ReloadPluginsInvocationCount(const WorkspaceShell& shell) {
    return shell.reload_plugins_invocation_count_;
  }
  static void ResetReloadPluginsInvocationCount(WorkspaceShell& shell) {
    shell.reload_plugins_invocation_count_ = 0;
  }
  static std::size_t ReloadCleanOpenBuffersFromDiskInvocationCount(const WorkspaceShell& shell) {
    return shell.reload_clean_open_buffers_from_disk_invocation_count_;
  }
  static void ResetReloadCleanOpenBuffersFromDiskInvocationCount(WorkspaceShell& shell) {
    shell.reload_clean_open_buffers_from_disk_invocation_count_ = 0;
  }
  static std::size_t RenderCompareSurfaceInvocationCount(const WorkspaceShell& shell) {
    return shell.render_compare_surface_invocation_count_;
  }
  static void ResetRenderCompareSurfaceInvocationCount(WorkspaceShell& shell) {
    shell.render_compare_surface_invocation_count_ = 0;
  }
  static WorkspaceShell::EventResult HandleScheduledWake(WorkspaceShell& shell) {
    return shell.HandleScheduledWake();
  }
  static bool ReloadPluginsIfPluginAssetsChanged(WorkspaceShell& shell, bool force_check) {
    return shell.ReloadPluginsIfPluginAssetsChanged(force_check);
  }
  static const std::vector<std::string>* OutputChannelEntries(const WorkspaceShell& shell,
                                                              std::string_view id) {
    return shell.OutputChannelEntries(id);
  }
  static void SetOutputChannelTabs(WorkspaceShell& shell,
                                   std::vector<std::string> open_channel_ids,
                                   std::string active_channel_id) {
    auto& panel = shell.context_.current_project_state.panel;
    panel.content = WorkspaceShell::PanelContentKind::Output;
    panel.output.open_channel_ids = std::move(open_channel_ids);
    panel.output.channel_id = std::move(active_channel_id);
  }
  static const std::vector<std::string>& OutputChannelTabOrder(const WorkspaceShell& shell) {
    return shell.context_.current_project_state.panel.output.open_channel_ids;
  }
  static bool MoveActiveOutputTabTo(WorkspaceShell& shell, std::size_t index) {
    return shell.MoveActiveOutputTabTo(index);
  }
  static WorkspaceShell::PanelContentKind PanelContent(const WorkspaceShell& shell) {
    return shell.context_.current_project_state.panel.content;
  }
  static WorkspaceShell::OverlayMode ActiveOverlayMode(const WorkspaceShell& shell) {
    return shell.context_.current_project_state.overlay.mode;
  }
  static const WorkspaceShell::CompletionSessionState& CompletionSession(
      const WorkspaceShell& shell) {
    return shell.context_.current_project_state.overlay.workflow.completion;
  }
  static const WorkspaceShell::CodeActionSessionState& CodeActionSession(
      const WorkspaceShell& shell) {
    return shell.context_.current_project_state.overlay.workflow.code_actions;
  }
  static bool PerfExpandSnippetAtCaret(WorkspaceShell& shell, std::string_view snippet_body) {
    editor::TextViewport* viewport = shell.ActiveEditorViewport();
    if (viewport == nullptr) {
      return false;
    }
    auto& project = shell.context_.current_project_state;
    if (project.focused_group().active_tab_index >= project.focused_group().open_tabs.size()) {
      return false;
    }
    TabEntry& tab = project.focused_group().open_tabs[project.focused_group().active_tab_index];
    if (tab.kind != TabEntry::Kind::Editor) {
      return false;
    }
    if (!tab.editor_state.has_value()) {
      return false;
    }
    viewport->BeginUndoGroup();
    const editor::SelectionRange trigger{
        editor::TextPosition{viewport->cursor_line(), viewport->cursor_column()},
        editor::TextPosition{viewport->cursor_line(), viewport->cursor_column()}};
    return editor::ExpandSnippetAtSelection(*viewport, tab.editor_state->snippet_session, trigger,
                                            snippet_body);
  }

  /// Perf harness only: expand a minimal placeholder snippet on the active editor so an idle soak can
  /// assert no periodic wakes while `SnippetSessionState::active` remains true (outline debounce is
  /// primed separately in `ScenarioContext::PrimeEditorEssentialsIdleSoakSurface`).
  static void PerfHarnessPrimeSnippetPlaceholderSession(WorkspaceShell& shell) {
    WorkspaceShell::TabEntry::EditorTabState* editor_tab = shell.ActiveEditorTab();
    editor::TextViewport* viewport = shell.ActiveEditorViewport();
    if (editor_tab == nullptr || viewport == nullptr || viewport->is_placeholder()) {
      return;
    }
    editor_tab->snippet_session.Reset(nullptr);
    (void)PerfExpandSnippetAtCaret(shell, "${1:idle}$0");
  }
