#include "workspace/WorkspaceShell.h"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "workspace/GitSidebarCommandCenter.h"
#include "workspace/WorkspaceActionServices.h"
#include "workspace/WorkspaceCommandParsing.h"
#include "workspace/WorkspaceCommandLineCoordinator.h"
#include "workspace/WorkspaceMenuCoordinator.h"
#include "workspace/WorkspacePersistenceCoordinator.h"
#include "workspace/ReviewSessionCoordinator.h"
#include "workspace/WorkspaceTextInputCoordinator.h"

namespace microide::workspace {

bool WorkspaceActionContext::PluginRuntimeEnabled() const {
  return operations_.plugin_runtime_enabled();
}

void WorkspaceActionContext::ReloadPluginsWithFeedback() {
  operations_.reload_plugins_for_current_project();
  std::string summary = operations_.plugin_runtime_reload_summary();
  state_.panel.feedback.text = summary;
  if (operations_.notify && !summary.empty()) {
    // Heuristic: surface failures/warnings as Warning, otherwise Info.
    const bool looks_problematic =
        summary.find("error") != std::string::npos || summary.find("fail") != std::string::npos ||
        summary.find("warn") != std::string::npos;
    operations_.notify(looks_problematic ? NotificationService::Tone::Warning
                                         : NotificationService::Tone::Info,
                       std::move(summary));
  }
}

void WorkspaceActionContext::RequestQuit() {
  operations_.request_quit();
}

bool WorkspaceActionContext::DebuggerEnabled() const {
  if (!operations_.get_setting_value) {
    return false;
  }
  const auto value = operations_.get_setting_value("debug.enabled");
  if (!value.has_value()) {
    return false;
  }
  return !(*value == "false" || *value == "0" || *value == "off" || value->empty());
}

void WorkspaceActionContext::ToggleDebuggerEnabled() {
  const bool was_enabled = DebuggerEnabled();
  if (operations_.set_setting_value) {
    operations_.set_setting_value("debug.enabled", was_enabled ? "false" : "true");
  }
  if (operations_.notify) {
    operations_.notify(NotificationService::Tone::Info,
                       was_enabled ? "Debugger disabled" : "Debugger enabled");
  }
}

void WorkspaceActionContext::StartDebuggingWithFeedback() {
  if (!operations_.start_debugging) {
    return;
  }
  const std::string error = operations_.start_debugging();
  state_.panel.feedback.text =
      error.empty() ? std::string("Debugging started") : ("Debug: " + error);
  if (operations_.notify) {
    operations_.notify(error.empty() ? NotificationService::Tone::Info
                                     : NotificationService::Tone::Error,
                       error.empty() ? std::string("Debugging started") : ("Debug: " + error));
  }
}

void WorkspaceActionContext::StopDebuggingWithFeedback() {
  if (operations_.stop_debugging) {
    operations_.stop_debugging();
  }
  state_.panel.feedback.text = "Debugging stopped";
  if (operations_.notify) {
    operations_.notify(NotificationService::Tone::Info, "Debugging stopped");
  }
}

bool WorkspaceActionContext::DebugSessionActive() const {
  return operations_.debug_session_active && operations_.debug_session_active();
}

bool WorkspaceActionContext::DebugSessionStopped() const {
  return operations_.debug_session_stopped && operations_.debug_session_stopped();
}

void WorkspaceActionContext::DebugContinue() {
  if (operations_.debug_continue) {
    operations_.debug_continue();
  }
}

void WorkspaceActionContext::DebugStepOver() {
  if (operations_.debug_step_over) {
    operations_.debug_step_over();
  }
}

void WorkspaceActionContext::DebugStepIn() {
  if (operations_.debug_step_in) {
    operations_.debug_step_in();
  }
}

void WorkspaceActionContext::DebugStepOut() {
  if (operations_.debug_step_out) {
    operations_.debug_step_out();
  }
}

void WorkspaceActionContext::DebugPause() {
  if (operations_.debug_pause) {
    operations_.debug_pause();
  }
}

void WorkspaceActionContext::DebugReverseContinue() {
  if (operations_.debug_reverse_continue) {
    operations_.debug_reverse_continue();
  }
}

void WorkspaceActionContext::DebugStepBack() {
  if (operations_.debug_step_back) {
    operations_.debug_step_back();
  }
}

void WorkspaceActionContext::DebugRestart() {
  if (operations_.debug_restart) {
    operations_.debug_restart();
  }
}

std::size_t WorkspaceActionContext::DebugSessionCount() const {
  return operations_.debug_session_count ? operations_.debug_session_count() : 0;
}

void WorkspaceActionContext::DebugSwitchSession(int index) {
  if (operations_.debug_switch_session) {
    operations_.debug_switch_session(index);
  }
}

void WorkspaceActionContext::ToggleDebugPane() {
  if (operations_.toggle_debug_pane) {
    operations_.toggle_debug_pane();
  }
}

void WorkspaceActionContext::ShowDebugPaneSurface(DebugPaneMode mode) {
  if (operations_.show_debug_pane_mode) {
    operations_.show_debug_pane_mode(mode);
  }
}

void WorkspaceActionContext::ShowDebugOutput() {
  if (operations_.show_debug_output) {
    operations_.show_debug_output();
  }
}

WorkspaceActionContext::DebugValueRowSelection WorkspaceActionContext::SelectedDebugValueRow()
    const {
  // Reads the pane's own models rather than routing through an Operations hook:
  // both live on project state, which this context already owns.
  const auto row_from = [](const auto& model) {
    DebugValueRowSelection selection;
    const std::vector<DebugVariableRowView>& rows = model.Rows();
    const std::size_t index = model.SelectedRow();
    if (index >= rows.size()) {
      return selection;
    }
    const DebugVariableRowView& row = rows[index];
    // Synthetic rows ("loading…", "show more…") name nothing and hold no value.
    if (row.is_placeholder || row.is_show_more) {
      return selection;
    }
    selection.valid = true;
    selection.name = row.display_name;
    selection.value = row.display_value;
    return selection;
  };

  switch (state_.debug_pane.mode) {
    case DebugPaneMode::Variables:
      return row_from(state_.debug_variables);
    case DebugPaneMode::Watch:
      return row_from(state_.debug_watch);
    case DebugPaneMode::CallStack:
    case DebugPaneMode::Breakpoints:
      break;
  }
  return {};
}

void WorkspaceActionContext::AddDebugWatchExpression(std::string expression) {
  if (operations_.add_debug_watch_expression) {
    operations_.add_debug_watch_expression(std::move(expression));
  }
}

void WorkspaceActionContext::StopAllDebugSessions() {
  if (operations_.stop_all_debug_sessions) {
    operations_.stop_all_debug_sessions();
  }
}

void WorkspaceActionContext::OpenDebugReplPrompt() {
  if (operations_.open_debug_repl_prompt) {
    operations_.open_debug_repl_prompt();
  }
}

void WorkspaceActionContext::OpenLaunchConfigPicker() {
  if (operations_.open_launch_config_picker) {
    operations_.open_launch_config_picker();
  }
}

void WorkspaceActionContext::OpenCommandPalette(std::string seed) {
  if (operations_.open_command_palette) {
    operations_.open_command_palette(std::move(seed));
  }
}

void WorkspaceActionContext::OpenGoToLinePrompt() {
  if (operations_.open_prompt_surface) {
    operations_.open_prompt_surface(PromptSurfaceState::Action::GoToLine,
                                    PromptSurfaceState::Kind::TextInput,
                                    std::filesystem::path{}, std::string{});
  }
}

void WorkspaceActionContext::OpenRenameSymbolPrompt() {
  if (!operations_.open_prompt_surface) {
    return;
  }
  // Prefill with the identifier under the cursor so a rename usually starts from the
  // current name — this opens instantly. The language server's prepareRename then
  // refines the seed / rejects invalid positions asynchronously (best-effort).
  std::string seed = operations_.symbol_at_cursor ? operations_.symbol_at_cursor() : std::string{};
  operations_.open_prompt_surface(PromptSurfaceState::Action::RenameSymbol,
                                  PromptSurfaceState::Kind::TextInput, std::filesystem::path{}, seed);
  if (operations_.refine_rename_prompt) {
    operations_.refine_rename_prompt(std::move(seed));
  }
}

void WorkspaceActionContext::EditBreakpointModifierFromMenu(ActionId id) {
  if (operations_.edit_breakpoint_modifier_from_menu) {
    operations_.edit_breakpoint_modifier_from_menu(id);
  }
}

void WorkspaceActionContext::BreakpointQuickActionFromMenu(ActionId id) {
  if (operations_.breakpoint_quick_action_from_menu) {
    operations_.breakpoint_quick_action_from_menu(id);
  }
}

void WorkspaceActionContext::RemoveBreakpointFromMenu() {
  if (operations_.remove_breakpoint_from_menu) {
    operations_.remove_breakpoint_from_menu();
  }
}

bool WorkspaceActionContext::DispatchSelectedGitSidebarAction(GitSidebarActionId action) {
  const auto& git = state_.sidebar.git;
  if (git.selected_index >= git.entries.size() || !operations_.dispatch_git_sidebar_action) {
    return false;
  }
  return operations_.dispatch_git_sidebar_action(action, git.selected_index);
}

bool WorkspaceActionContext::ToggleStageSelectedGitEntry() {
  const auto& git = state_.sidebar.git;
  if (git.selected_index >= git.entries.size()) {
    return false;
  }
  return DispatchSelectedGitSidebarAction(git.entries[git.selected_index].staged
                                              ? GitSidebarActionId::Unstage
                                              : GitSidebarActionId::Stage);
}

editor::BreakpointStore& WorkspaceActionContext::MutableBreakpointStore() {
  return state_.breakpoint_store;
}

void WorkspaceActionContext::ResendBreakpoints(const std::filesystem::path& path) {
  if (operations_.resend_breakpoints_for_file) {
    operations_.resend_breakpoints_for_file(path);
  }
}

std::string WorkspaceActionContext::StartNamedDebugConfig(const std::string& name) {
  if (operations_.start_named_debug_config) {
    return operations_.start_named_debug_config(name);
  }
  return "debug launch unavailable";
}

std::string WorkspaceActionContext::StartAdHocDebug(const std::string& program,
                                                    const std::vector<std::string>& args,
                                                    const std::string& type) {
  if (operations_.start_ad_hoc_debug) {
    return operations_.start_ad_hoc_debug(program, args, type);
  }
  return "debug launch unavailable";
}

void WorkspaceActionContext::AddFunctionBreakpoint(const std::string& name) {
  if (operations_.add_function_breakpoint) {
    operations_.add_function_breakpoint(name);
  }
}

void WorkspaceActionContext::RemoveFunctionBreakpoint(const std::string& name) {
  if (operations_.remove_function_breakpoint) {
    operations_.remove_function_breakpoint(name);
  }
}

void WorkspaceActionContext::ToggleFunctionBreakpoint(const std::string& name) {
  if (operations_.toggle_function_breakpoint) {
    operations_.toggle_function_breakpoint(name);
  }
}

void WorkspaceActionContext::SetFunctionBreakpointCondition(const std::string& name,
                                                            std::optional<std::string> condition) {
  if (operations_.set_function_breakpoint_condition) {
    operations_.set_function_breakpoint_condition(name, std::move(condition));
  }
}

void WorkspaceActionContext::SetExceptionFilterCondition(const std::string& filter_id,
                                                         std::optional<std::string> condition) {
  if (operations_.set_exception_filter_condition) {
    operations_.set_exception_filter_condition(filter_id, std::move(condition));
  }
}

WorkspaceActionContext WorkspaceShell::MakeActionContext() {
  // Builds a fresh review coordinator per verb invocation; it owns no shell
  // access beyond these narrow callbacks (host-owned sidebar + tab lifecycle).
  const auto make_review = [this]() {
    return ReviewSessionCoordinator(
        context_.current_project_state, MakeCompareMergeService(),
        ReviewSessionCoordinator::Operations{
            .show_git_sidebar = [this]() { ShowGitSidebar(); },
            .request_close_tabs =
                [this](std::vector<std::size_t> indices) {
                  RequestCloseTabs(std::move(indices));
                },
            .tab_is_dirty = [this](std::size_t index) { return TabIsDirty(index); },
        });
  };
  return WorkspaceActionContext(
      context_.project_catalog,
      context_.current_project_state,
      ui_scale_,
      WorkspaceActionContext::Operations{
          .close_tree_context_menu = [this]() { MakeMenuCoordinator().CloseTreeContextMenu(); },
          .reject_action =
              [this](ActionSource source, std::string feedback) {
                const std::string feedback_copy = feedback;
                const bool accepted =
                    MakeCommandLineCoordinator().RejectAction(source, std::move(feedback));
                if (source != ActionSource::Command && !feedback_copy.empty()) {
                  output_channels_.AppendLine("actions.log", "Actions", feedback_copy);
                }
                return accepted;
              },
          .parse_sidebar_view_request =
              [this](const std::vector<std::string>& args) {
                return workspace::ParseSidebarViewRequest(args, plugin_runtime_.Host());
              },
          .open_project =
              [this](const std::filesystem::path& root, bool restore, bool log_feedback) {
                return OpenProjectTab(root, restore, log_feedback);
              },
          .request_close_project = [this](std::size_t index) { RequestCloseProject(index); },
          .switch_project =
              [this](std::size_t index, bool log_feedback) {
                return SwitchProject(index, log_feedback);
              },
          .open_native_project_picker =
              [this]() {
                switch (OpenNativeProjectPicker(nullptr)) {
                  case ProjectOpenDialogLaunchResult::Launched:
                    return ProjectOpenPickerResult::Launched;
                  case ProjectOpenDialogLaunchResult::AlreadyOpen:
                    return ProjectOpenPickerResult::AlreadyOpen;
                  case ProjectOpenDialogLaunchResult::Unavailable:
                    return ProjectOpenPickerResult::Unavailable;
                }
                return ProjectOpenPickerResult::Unavailable;
              },
          .open_native_file_picker =
              [this]() {
                switch (OpenNativeFilePicker(nullptr)) {
                  case ProjectOpenDialogLaunchResult::Launched:
                    return ProjectOpenPickerResult::Launched;
                  case ProjectOpenDialogLaunchResult::AlreadyOpen:
                    return ProjectOpenPickerResult::AlreadyOpen;
                  case ProjectOpenDialogLaunchResult::Unavailable:
                    return ProjectOpenPickerResult::Unavailable;
                }
                return ProjectOpenPickerResult::Unavailable;
              },
          .active_sidebar_mode = [this]() { return ActiveSidebarMode(); },
          .toggle_sidebar = [this]() { ToggleSidebar(); },
          .close_sidebar = [this]() { CloseSidebar(); },
          .show_tree_sidebar = [this](const std::filesystem::path& root) { ShowTreeSidebar(root); },
          .show_search_sidebar =
              [this](std::string query, bool temporary) {
                ShowSearchSidebar(std::move(query), temporary);
              },
          .show_problems_sidebar = [this]() { ShowProblemsSidebar(); },
          .show_git_sidebar = [this]() { ShowGitSidebar(); },
          .show_tests_sidebar = [this]() { ShowTestsSidebar(); },
          .show_outline_sidebar = [this]() { ShowOutlineSidebar(); },
          .show_plugin_sidebar =
              [this](std::string_view id, bool temporary) {
                return ShowPluginSidebar(id, temporary);
              },
          .current_window_rect = [this]() { return CurrentWindowRect(); },
          .refresh_project_files = [this]() { RefreshProjectFiles(); },
          .reload_clean_open_buffers_from_disk = [this]() { ReloadCleanOpenBuffersFromDisk(); },
          .dispatch_git_operation =
              [this](ActionId id, const std::vector<std::string>& args) {
                return DispatchGitOperationAction(id, args);
              },
          .tree_mutation_base_path =
              [this](ActionSource source) { return TreeMutationBasePath(source); },
          .resolve_tree_action_path =
              [this](ActionSource source) { return ResolveTreeActionPath(source); },
          .row_context_menu_path =
              [this]() {
                const auto& menu = context_.menu_state.tree_context_menu;
                return menu.open ? menu.path.lexically_normal() : std::filesystem::path{};
              },
          .active_tab_path = [this]() { return ActiveTabPath(); },
          .open_prompt_surface =
              [this](PromptSurfaceState::Action action, PromptSurfaceState::Kind kind,
                     const std::filesystem::path& path, std::string input) {
                OpenPromptSurface(action, kind, path, std::move(input));
              },
          .write_clipboard_text = [this](std::string_view text) { return WriteClipboardText(text); },
          .write_primary_selection_text =
              [this](std::string_view text) { return WritePrimarySelectionText(text); },
          .reveal_path_in_file_explorer =
              [this](const std::filesystem::path& dir) { return RevealPathInFileExplorer(dir); },
          .reveal_path_in_tree =
              [this](const std::filesystem::path& path) {
                // Force-expand ancestors and select the row (SelectPath), then scroll
                // it into view. Unconditional reveal — ignores the manual-collapse guard.
                const bool found =
                    context_.current_project_state.directory_tree.SelectPath(path);
                if (found) {
                  RevealSelectedTreeSidebarLine();
                }
                return found;
              },
          .open_terminal = [this](std::string command) { OpenTerminal(std::move(command)); },
          .close_active_terminal =
              [this]() {
                auto& state = context_.current_project_state;
                if (state.active_terminal_tab() == nullptr) {
                  return false;
                }
                CloseTerminalTab(state.active_terminal_tab_index);
                return true;
              },
          .open_terminal_find =
              [this](std::string query) {
                // A seedless invocation preloads the terminal selection, matching
                // "Ctrl+F with something selected" everywhere else in the shell.
                if (!BottomPanelShowsTerminal() || ActiveTerminalTab() == nullptr) {
                  return false;
                }
                if (query.empty() && TerminalHasSelection()) {
                  query = SelectedTerminalText();
                  // A multi-line selection is not a useful seed for a per-row scan.
                  if (const std::size_t newline = query.find('\n');
                      newline != std::string::npos) {
                    query.resize(newline);
                  }
                }
                context_.current_project_state.surface.focus = FocusTarget::Panel;
                terminal_find_service_.Open(ActiveTerminalTab(), query);
                RequestBottomPanelContentRedraw();
                return true;
              },
          .show_overlay = [this](OverlayMode mode) { ShowOverlay(mode); },
          .dismiss_overlay = [this]() { DismissOverlay(); },
          .open_settings_overlay = [this]() { OpenSettingsOverlay(); },
          .open_help_about_overlay = [this]() { OpenHelpAboutOverlay(); },
          .toggle_status_bar =
              [this]() {
                SetSettingValue("ui.show_status_bar",
                                layout_mode_service_.StatusBarVisible() ? "false" : "true");
              },
          .toggle_layout_mode =
              [this]() {
                const bool compact = layout_mode_service_.CurrentMode() != LayoutMode::Compact;
                SetSettingValue("ui.layout_mode", compact ? "compact" : "regular");
              },
          .active_editor_viewport = [this]() { return ActiveEditorViewport(); },
          .open_buffer_search = [this]() { OpenBufferSearch(); },
          .refresh_buffer_search = [this]() { RefreshBufferSearch(); },
          .open_buffer_replace = [this]() { OpenBufferReplace(); },
          .show_completion_overlay =
              [this](std::string* error_message) {
                return assist_service_.ShowCompletionOverlay(error_message);
              },
          .show_insert_snippet_overlay =
              [this](std::string* error_message) {
                return assist_service_.ShowInsertSnippetOverlay(error_message);
              },
          .show_code_actions_overlay =
              [this](std::string* error_message) {
                return assist_service_.ShowCodeActionsOverlay(error_message);
              },
          .go_to_lsp_definition =
              [this](std::string* error_message) {
                return assist_service_.GoToLspDefinition(error_message);
              },
          .go_to_lsp_type_definition =
              [this](std::string* error_message) {
                return assist_service_.GoToLspTypeDefinition(error_message);
              },
          .go_to_lsp_implementation =
              [this](std::string* error_message) {
                return assist_service_.GoToLspImplementation(error_message);
              },
          .go_to_lsp_declaration =
              [this](std::string* error_message) {
                return assist_service_.GoToLspDeclaration(error_message);
              },
          .find_lsp_references =
              [this](std::string* error_message) {
                return assist_service_.FindLspReferences(error_message);
              },
          .show_call_hierarchy =
              [this](bool incoming, std::string* error_message) {
                const bool ok = assist_service_.ShowCallHierarchy(incoming, error_message);
                if (ok) {
                  // Surface the results channel in the bottom panel (it fills async).
                  EnsureOutputChannelTabOpen("lsp.callHierarchy");
                  context_.current_project_state.panel.content = PanelContentKind::Output;
                  context_.current_project_state.panel.output.channel_id = "lsp.callHierarchy";
                  RequestBottomPanelRedraw();
                }
                return ok;
              },
          .show_workspace_symbols =
              [this](const std::string& query, std::string* error_message) {
                const bool ok = assist_service_.ShowWorkspaceSymbols(query, error_message);
                if (ok) {
                  // Surface the results channel in the bottom panel (it fills async).
                  EnsureOutputChannelTabOpen("lsp.workspaceSymbols");
                  context_.current_project_state.panel.content = PanelContentKind::Output;
                  context_.current_project_state.panel.output.channel_id = "lsp.workspaceSymbols";
                  RequestBottomPanelRedraw();
                }
                return ok;
              },
          .format_active_document =
              [this](std::string* error_message) {
                return assist_service_.FormatActiveDocument(error_message);
              },
          .symbol_at_cursor = [this]() { return assist_service_.SymbolAtCursor(); },
          .rename_symbol =
              [this](const std::string& new_name, std::string* error_message) {
                return assist_service_.RenameSymbol(new_name, error_message);
              },
          // Refine a just-opened Rename Symbol prompt from the server's prepareRename:
          // prefill the exact placeholder (only while the user has not typed over the
          // heuristic seed) or dismiss with a hint for a non-renameable position.
          // Best-effort and async; a no-op when no server serves the buffer.
          .refine_rename_prompt =
              [this](std::string original_seed) {
                assist_service_.PrepareRenameForCursor(
                    [this, original_seed = std::move(original_seed)](
                        bool can_rename, std::string placeholder) {
                      if (!context_.prompts.surface_visible ||
                          context_.prompts.surface.action !=
                              PromptSurfaceState::Action::RenameSymbol) {
                        return;
                      }
                      if (!can_rename) {
                        DismissPromptSurface(true);
                        context_.current_project_state.panel.feedback.text =
                            "You cannot rename this element";
                        RequestChromeRedraw();
                        return;
                      }
                      if (!placeholder.empty() && placeholder != original_seed &&
                          context_.prompts.surface.input.text() == original_seed) {
                        context_.prompts.surface.input.SetText(placeholder);
                        context_.prompts.surface.input.SelectAll();
                        RequestPromptRedraw();
                      }
                    });
              },
          .show_signature_help =
              [this](std::string* error_message) {
                return assist_service_.ShowSignatureHelp(error_message);
              },
          .discover_tests_for_active_buffer =
              [this](std::string* error_message) {
                return DiscoverTestsForActiveBuffer(error_message);
              },
          .run_tests =
              [this](const std::vector<std::string>& test_ids, std::string* error_message) {
                return RunTests(test_ids, error_message);
              },
          .run_all_discovered_tests =
              [this](std::string* error_message) {
                return RunAllDiscoveredTests(error_message);
              },
          .open_compare_picker_for_path =
              [this](const std::filesystem::path& path, const std::string& commit_spec) {
                OpenComparePickerForPath(path, commit_spec);
              },
          .open_comparison = [this](const project::GitCommitEntry& commit) { OpenComparison(commit); },
          .open_plain_comparison =
              [this](CompareInput left, CompareInput right) {
                return MakeCompareMergeService().OpenPlainComparison(std::move(left),
                                                                     std::move(right));
              },
          .open_merge_editor =
              [this](const std::filesystem::path& base_path,
                     const std::filesystem::path& incoming_path,
                     const std::filesystem::path& current_path,
                     const std::filesystem::path& output_path) {
                return OpenMergeEditor(base_path, incoming_path, current_path, output_path);
              },
          .open_conflict_review = [make_review]() { return make_review().OpenConflictReview(); },
          .open_branch_review =
              [make_review](const std::string& ref) { return make_review().OpenBranchReview(ref); },
          .open_commit_review =
              [make_review](const std::string& ref) { return make_review().OpenCommitReview(ref); },
          .active_editor_tab = [this]() { return ActiveEditorTab(); },
          .ensure_active_folding_model_fresh =
              [this]() { return EnsureActiveFoldingModelFresh(); },
          .replace_active_editor_view =
              [this](const editor::TextViewport& viewport) {
                return ReplaceActiveEditorView(viewport);
              },
          .open_file = [this](const std::filesystem::path& path) { OpenFile(path); },
          .open_file_in_new_tab =
              [this](const std::filesystem::path& path) { return OpenFileInNewTab(path); },
          .open_untitled_tab = [this]() { return OpenUntitledTab(); },
          .find_tab_index_by_specifier =
              [this](std::string_view specifier, std::string* error_message) {
                return FindTabIndexBySpecifier(specifier, error_message);
              },
          .activate_tab = [this](std::size_t index) { ActivateTab(index); },
          .move_active_tab_to = [this](std::size_t index) { return MoveActiveTabTo(index); },
          .reopen_active_tab = [this]() { return ReopenActiveTab(); },
          .save_tab = [this](std::size_t index) { return SaveTab(index); },
          .reset_caret_blink = [this]() { ResetCaretBlink(); },
          .notify_snippet_session_caret_moved =
              [this]() { assist_service_.NotifySnippetSessionCaretMoved(); },
          .clear_active_snippet_session_after_undo =
              [this]() { assist_service_.ClearActiveSnippetSessionAfterUndo(); },
          .split_editor_group =
              [this](EditorSplitOrientation orientation) {
                return SplitEditorGroup(orientation);
              },
          .editor_group_count = [this]() { return EditorGroupCount(); },
          .focus_other_group = [this]() { return FocusOtherEditorGroup(); },
          .close_editor_group = [this]() { return CloseEditorGroup(); },
          .request_close_tab = [this](std::size_t index) { RequestCloseTab(index); },
          .request_close_tabs =
              [this](std::vector<std::size_t> indices) { RequestCloseTabs(std::move(indices)); },
          .close_all_tabs = [this]() { CloseAllTabs(); },
          .active_navigable_viewport = [this]() { return ActiveNavigableViewport(); },
          .request_focused_editor_redraw = [this]() { RequestFocusedEditorRedraw(); },
          .active_editable_viewport = [this]() { return ActiveEditableViewport(); },
          .insert_text_into_active_text_surface =
              [this](std::string_view text) {
                return MakeTextInputCoordinator().InsertTextAtActiveSurface(text);
              },
          .has_active_single_line_text_surface =
              [this]() {
                return MakeTextInputCoordinator().HasActiveSingleLineTextSurface();
              },
          .selected_text_at_active_single_line_text_surface =
              [this]() {
                return MakeTextInputCoordinator().SelectedTextAtActiveSingleLineSurface();
              },
          .select_all_at_active_single_line_text_surface =
              [this]() {
                return MakeTextInputCoordinator().SelectAllAtActiveSingleLineSurface();
              },
          .cut_selection_at_active_single_line_text_surface =
              [this]() {
                return MakeTextInputCoordinator().CutSelectionAtActiveSingleLineSurface();
              },
          .active_compare_tab = [this]() { return ActiveCompareTab(); },
          .mark_branch_file_reviewed = [this]() { SetActiveBranchFileReviewed(true); },
          .unmark_branch_file_reviewed = [this]() { SetActiveBranchFileReviewed(false); },
          .mark_branch_hunk_reviewed = [this]() { SetActiveBranchHunkReviewed(true); },
          .unmark_branch_hunk_reviewed = [this]() { SetActiveBranchHunkReviewed(false); },
          .clear_branch_review_state = [this]() { ClearActiveBranchReviewState(); },
          .edit_branch_review_note =
              [this](std::string note_text) { EditActiveBranchReviewNote(note_text); },
          .refresh_compare_tab_derived_state =
              [this](CompareTabState& compare_tab) { RefreshCompareTabDerivedState(compare_tab); },
          .sync_compare_selection_from_viewport =
              [this](CompareTabState& compare_tab, bool reveal_selection) {
                SyncCompareSelectionFromViewport(compare_tab, reveal_selection);
              },
          .active_merge_tab = [this]() { return ActiveMergeTab(); },
          .update_merge_tracking_after_viewport_edit =
              [this](MergeTabState& merge_tab,
                     std::optional<editor::SelectionRange> selection_before,
                     editor::TextPosition cursor_before) {
                UpdateMergeTrackingAfterViewportEdit(merge_tab, selection_before, cursor_before);
              },
          .request_active_tab_redraw =
              [this](bool include_tree_sidebar) { RequestActiveTabRedraw(include_tree_sidebar); },
          .request_active_editable_last_change_redraw =
              [this]() { RequestActiveEditableLastChangeRedraw(); },
          .request_active_editable_change_redraw =
              [this](const std::vector<std::string>& before_lines,
                     const std::vector<std::string>& after_lines) {
                RequestActiveEditableChangeRedraw(before_lines, after_lines);
              },
          .request_active_editable_blame_neighborhood_redraw =
              [this](std::size_t before_line, std::size_t after_line) {
                RequestActiveEditableBlameNeighborhoodRedraw(before_line, after_line);
              },
          .request_tab_strip_redraw = [this]() { RequestTabStripRedraw(); },
          .terminal_has_selection = [this]() { return TerminalHasSelection(); },
          .selected_terminal_text = [this]() { return SelectedTerminalText(); },
          .last_terminal_command_text = [this]() { return LastTerminalCommandText(); },
          .selection_text_with_context = [this]() { return SelectionTextWithContext(); },
          .read_clipboard_text = [this]() { return ReadClipboardText(); },
          .paste_text_into_terminal =
              [this](std::string text) {
                MakeTextInputCoordinator().PasteTextIntoTerminal(std::move(text));
              },
          .refresh_available_colorscheme_names =
              [this]() { MakePersistenceCoordinator().RefreshAvailableColorschemeNames(); },
          .apply_colorscheme =
              [this](std::string_view name) {
                MakePersistenceCoordinator().ApplyColorscheme(name, true, true);
              },
          .apply_editor_preferences_to_all_tabs = [this]() { ApplyEditorPreferencesToAllTabs(); },
          .save_config_state = [this]() { MakePersistenceCoordinator().SaveConfigState(); },
          .get_setting_value =
              [this](std::string_view id) { return GetSettingValue(id); },
          .set_setting_value =
              [this](std::string_view id, std::string value) {
                return SetSettingValue(id, std::move(value));
              },
          .notify =
              [this](NotificationService::Tone tone, std::string message) {
                Notify(tone, std::move(message));
              },
          .apply_ui_scale =
              [this](float scale) { MakePersistenceCoordinator().ApplyUiScale(scale, true, true); },
          .mark_layout_dirty = [this]() { MarkLayoutDirty(); },
          .request_window_redraw = [this]() { RequestWindowRedraw(); },
          .request_toggle_fullscreen =
              [this]() { pending_window_action_ = WindowAction::ToggleFullscreen; },
          .active_terminal_tab = [this]() { return ActiveTerminalTab(); },
          .plugin_runtime_enabled = [this]() { return plugin_runtime_.enabled(); },
          .reload_plugins_for_current_project = [this]() { ReloadPluginsForCurrentProject(); },
          .plugin_runtime_reload_summary = [this]() { return PluginRuntimeReloadSummary(); },
          .request_quit = [this]() { RequestQuit(); },
          .start_debugging = [this]() { return StartDebuggingWithDefaultConfig(); },
          .stop_debugging = [this]() { StopDebugging(); },
          .debug_session_active = [this]() { return IsDebugSessionActive(); },
          .debug_session_stopped = [this]() { return IsDebugSessionStopped(); },
          .debug_continue = [this]() { DebugContinue(); },
          .debug_step_over = [this]() { DebugStepOver(); },
          .debug_step_in = [this]() { DebugStepIn(); },
          .debug_step_out = [this]() { DebugStepOut(); },
          .debug_pause = [this]() { DebugPause(); },
          .debug_restart = [this]() { DebugRestart(); },
          .debug_reverse_continue = [this]() { DebugReverseContinue(); },
          .debug_step_back = [this]() { DebugStepBack(); },
          .debug_session_count = [this]() { return CurrentDapManager().SessionCount(); },
          .debug_switch_session = [this](int index) { DebugSwitchSession(index); },
          .stop_all_debug_sessions = [this]() { StopAllDebugSessions(); },
          .open_debug_repl_prompt = [this]() { OpenDebugReplPrompt(); },
          .open_launch_config_picker = [this]() { OpenLaunchConfigPicker(); },
          .open_command_palette = [this](std::string seed) { OpenCommandPalette(std::move(seed)); },
          .edit_breakpoint_modifier_from_menu =
              [this](ActionId id) { EditBreakpointModifierFromMenu(id); },
          .breakpoint_quick_action_from_menu =
              [this](ActionId id) { BreakpointQuickActionFromMenu(id); },
          .remove_breakpoint_from_menu = [this]() { RemoveBreakpointFromMenu(); },
          .dispatch_git_sidebar_action =
              [this](GitSidebarActionId action, std::size_t index) {
                return DispatchGitSidebarAction(action, index);
              },
          .toggle_debug_pane = [this]() { ToggleDebugPane(); },
          .show_debug_pane_mode = [this](DebugPaneMode mode) { ShowDebugPaneMode(mode); },
          .show_debug_output = [this]() { ShowDebugOutput(); },
          .add_debug_watch_expression =
              [this](std::string expression) {
                debug_service_.AddWatch(std::move(expression));
                // Show the surface the expression just landed on, so the item has
                // visible feedback when invoked from the Variables list.
                ShowDebugPaneMode(DebugPaneMode::Watch);
              },
          .resend_breakpoints_for_file =
              [this](const std::filesystem::path& path) { ResendBreakpointsForFile(path); },
          .start_named_debug_config =
              [this](const std::string& name) { return StartNamedDebugConfig(name); },
          .start_ad_hoc_debug =
              [this](const std::string& program, const std::vector<std::string>& args,
                     const std::string& type) { return StartAdHocDebug(program, args, type); },
          .add_function_breakpoint =
              [this](const std::string& name) { debug_service_.AddFunctionBreakpoint(name); },
          .remove_function_breakpoint =
              [this](const std::string& name) {
                debug_service_.RemoveFunctionBreakpointByName(name);
              },
          .toggle_function_breakpoint =
              [this](const std::string& name) {
                debug_service_.ToggleFunctionBreakpointByName(name);
              },
          .set_function_breakpoint_condition =
              [this](const std::string& name, std::optional<std::string> condition) {
                debug_service_.SetFunctionBreakpointConditionByName(name, std::move(condition));
              },
          .set_exception_filter_condition =
              [this](const std::string& filter_id, std::optional<std::string> condition) {
                debug_service_.SetExceptionFilterCondition(filter_id, std::move(condition));
              },
      });
}

}  // namespace microide::workspace
