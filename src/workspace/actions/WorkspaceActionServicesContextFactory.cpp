#include "workspace/shell/WorkspaceShell.h"

#include "workspace/services/TerminalPanelService.h"

#include <filesystem>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "util/StringUtil.h"
#include "workspace/git/CompareTabReview.h"
#include "workspace/git/GitSidebarCommandCenter.h"
#include "workspace/actions/WorkspaceActionServices.h"
#include "workspace/WorkspaceCommandParsing.h"
#include "workspace/coordinators/WorkspaceCommandLineCoordinator.h"
#include "workspace/coordinators/WorkspaceDirtyPromptCoordinator.h"
#include "workspace/coordinators/WorkspaceMenuCoordinator.h"
#include "workspace/coordinators/WorkspacePathMutationCoordinator.h"
#include "workspace/services/PromptSurfaceService.h"
#include "workspace/persistence/WorkspacePersistenceCoordinator.h"
#include "workspace/git/ReviewSessionCoordinator.h"
#include "workspace/coordinators/WorkspaceTextInputCoordinator.h"
#include "workspace/services/EditorTabService.h"
#include "workspace/services/SidebarService.h"

namespace microide::workspace {

bool WorkspaceActionContext::PluginRuntimeEnabled() const {
  return operations_.plugin_runtime_enabled();
}

void WorkspaceActionContext::ReloadPluginsWithFeedback() {
  // The summary lands with the reload's completion (inline when no worker is
  // wired, so this placeholder is overwritten before the command returns there).
  state_.panel.feedback.text = "Reloading plugins\u2026";
  operations_.reload_plugins_for_current_project(/*announce=*/true);
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

bool WorkspaceActionContext::DebugSupportsReverse() const {
  return operations_.debug_supports_reverse && operations_.debug_supports_reverse();
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

void WorkspaceActionContext::CycleSurfaceFocus(bool backward) {
  if (state_.overlay.visible) {
    state_.surface.focus = FocusTarget::Overlay;
    return;
  }
  state_.surface.focus = NextSurfaceInFocusRing(state_.surface.focus, state_.sidebar.visible,
                                                state_.debug_pane.visible,
                                                state_.panel.content != PanelContentKind::None, backward);
  RequestLiveConfigRedraw();
}

void WorkspaceActionContext::OpenColorThemePicker() {
  if (operations_.open_color_theme_picker) {
    operations_.open_color_theme_picker();
  }
}

void WorkspaceActionContext::OpenDocumentSymbolPicker(std::string query) {
  if (operations_.open_document_symbol_picker) {
    operations_.open_document_symbol_picker(std::move(query));
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

void WorkspaceActionContext::OpenCommandArgumentsPrompt(std::string_view command_name, std::string prefill) {
  if (operations_.open_command_arguments_prompt) {
    operations_.open_command_arguments_prompt(command_name, std::move(prefill));
  }
}

void WorkspaceActionContext::OpenRemoteHostPrompt(bool folder) {
  if (operations_.open_prompt_surface) {
    operations_.open_prompt_surface(folder ? PromptSurfaceState::Action::RemoteFolder
                                           : PromptSurfaceState::Action::RemoteHost,
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

std::string WorkspaceActionContext::SelectedConflictedGitEntryPath() const {
  const auto& git = state_.sidebar.git;
  if (git.selected_index >= git.entries.size() || !git.entries[git.selected_index].conflicted) {
    return {};
  }
  return git.entries[git.selected_index].path;
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

std::string WorkspaceActionContext::SetExceptionFilterCondition(const std::string& filter_id,
                                                                std::optional<std::string> condition) {
  return operations_.set_exception_filter_condition
             ? operations_.set_exception_filter_condition(filter_id, std::move(condition))
             : std::string("debugging is unavailable");
}

WorkspaceActionContext& WorkspaceShell::MakeActionContext() {
  if (glue_->action_context != nullptr) {
    return *glue_->action_context;
  }
  // The LSP list results (references, call hierarchy, workspace symbols) fill an
  // output channel asynchronously; the request shows that channel in the bottom
  // panel so the answer lands where the user is looking. References forgot to,
  // so Shift+F12 wrote its results into a channel nobody had open (QA flow
  // lsp-navigation).
  // `key` and `pointer` hand the shell an event from inside a control command. The
  // app loop paints what an event returns; this one returns into the command, so
  // its damage goes back on the pending list the command's own event returns --
  // dropped, the frame after a control click repainted only what was still
  // pending (QA on Wayland: a newly active tab kept its old look).
  const auto dispatch_injected = [this](const SDL_Event& event) {
    pending_render_invalidation_.Merge(HandleEvent(event).redraw);
  };
  const auto show_results_channel = [this](const char* channel_id, const char* label) {
    // With its label from the start: the answer comes later, and the tab showed the
    // channel's id until then.
    output_channels_.EnsureChannel(channel_id, label);
    EnsureOutputChannelTabOpen(channel_id);
    context_.current_project_state.panel.content = PanelContentKind::Output;
    context_.current_project_state.panel.output.channel_id = channel_id;
    RequestBottomPanelRedraw();
  };
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
            .activate_tab = [this](std::size_t index) { ActivateTab(index); },
            // The file reader's thread, with "read nothing": the shape a branch
            // compare's blob load and a conflict merge's stage read already use.
            .run_off_shell_thread =
                [this](std::function<void()> gather, std::function<void()> apply) {
                  const std::filesystem::path root = context_.current_project_state.root;
                  (void)file_read_service_.Begin({
                      .path = root,
                      .on_worker = [gather = std::move(gather)](std::string&) { gather(); },
                      .on_complete =
                          [this, root, apply = std::move(apply)](
                              project::FileReadService::Completion completion) {
                            if (completion.status !=
                                    project::FileReadService::Status::Cancelled &&
                                context_.current_project_state.root == root) {
                              apply();
                            }
                          },
                      .read_path = false,
                  });
                },
            .report_outcome =
                [this](ReviewOpenOutcome outcome) {
                  // As FinishReviewOutcome does for a synchronous one: command
                  // feedback for the control channel, and a notification.
                  context_.current_project_state.panel.feedback.text = outcome.message;
                  if (!outcome.message.empty()) {
                    Notify(outcome.ok ? NotificationService::Tone::Info
                                      : NotificationService::Tone::Warning,
                           outcome.message);
                  }
                },
        });
  };
  glue_->action_context = std::make_unique<WorkspaceActionContext>(
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
                  // A rejection sentence exists to be read. From a button, menu, or
                  // shortcut the command feedback line is never painted and the log
                  // channel is one nobody has open, so the refusal (a git op already
                  // in flight, no repo, an action the selected row does not support)
                  // looked exactly like the click doing nothing. Identical toasts
                  // collapse, so a held shortcut cannot stack them.
                  Notify(NotificationService::Tone::Warning, feedback_copy);
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
              [this]() { return OpenNativeProjectPicker(nullptr); },
          .open_native_file_picker =
              [this]() { return OpenNativeFilePicker(nullptr); },
          .active_sidebar_mode = [this]() { return ActiveSidebarMode(); },
          .toggle_sidebar = [this]() { ToggleSidebar(); },
          .close_sidebar = [this]() { CloseSidebar(); },
          .show_tree_sidebar = [this](const std::filesystem::path& root) { ShowTreeSidebar(root); },
          .show_search_sidebar =
              [this](std::string query, bool temporary) {
                ShowSearchSidebar(std::move(query), temporary);
              },
          .begin_project_search_edit = [this](ProjectSearchEditField field) { BeginProjectSearchEdit(field); },
          .paste_tree_path =
              [this](const std::filesystem::path& source, bool cut, const std::filesystem::path& directory) {
                EditorTabService& editor_tabs = MakeEditorTabService();
                PromptSurfaceService& prompt_surfaces = MakePromptSurfaceService();
                return MakePathMutationCoordinator(editor_tabs, prompt_surfaces).PastePath(source, cut, directory);
              },
          .refresh_project_search = [this]() { RefreshProjectSearch(); },
          .can_open_git_commit = [this]() { return CanOpenGitCommitButton(); },
          .open_git_commit_workflow = [this]() { return OpenCommitWorkflow(); },
          .show_problems_sidebar = [this]() { ShowProblemsSidebar(); },
          .show_git_sidebar = [this]() { ShowGitSidebar(); },
          .show_tests_sidebar = [this]() { ShowTestsSidebar(); },
          .show_outline_sidebar = [this]() { ShowOutlineSidebar(); },
          .show_host_changes_sidebar = [this]() { MakeSidebarService().ShowHostChanges(); },
          .show_plugin_sidebar =
              [this](std::string_view id, bool temporary) {
                return ShowPluginSidebar(id, temporary);
              },
          .current_window_rect = [this]() { return CurrentWindowRect(); },
          .refresh_project_files = [this]() { RefreshProjectFiles(); },
          .reload_clean_open_buffers_from_disk = [this]() { ReloadCleanOpenBuffersFromDisk(); },
          .request_git_refresh = [this]() { RequestGitSidebarRefresh(); },
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
          .open_command_arguments_prompt =
              [this](std::string_view command_name, std::string prefill) {
                OpenPromptSurface(PromptSurfaceState::Action::CommandArguments,
                                  PromptSurfaceState::Kind::TextInput, std::filesystem::path{},
                                  std::move(prefill));
                context_.prompts.surface.command_name.assign(command_name);
              },
          .write_clipboard_text = [this](std::string_view text) { return WriteClipboardText(text); },
          .open_external_url = [this](std::string_view url) { return OpenExternalUrl(url); },
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
          .open_terminal =
              [this](std::string command) {
                MakeTerminalPanelService().OpenTerminal(std::move(command));
              },
          // "Close Terminal" closes the active PANE (VS Code's kill terminal kills
          // the active instance); the tab goes with its last pane.
          .close_active_terminal = [this]() { return MakeTerminalPanelService().CloseActivePane(); },
          .split_terminal = [this]() { return MakeTerminalPanelService().SplitActivePane(); },
          .focus_terminal_pane =
              [this](int delta) { return MakeTerminalPanelService().FocusPaneInDirection(delta); },
          .cycle_terminal =
              [this](int delta) { return MakeTerminalPanelService().CycleTerminalTab(delta); },
          .relaunch_terminal = [this]() { return MakeTerminalPanelService().RelaunchActivePane(); },
          .toggle_terminal = [this]() { MakeTerminalPanelService().ToggleTerminal(); },
          .toggle_panel = [this]() { MakeTerminalPanelService().TogglePanel(); },
          .toggle_panel_maximized =
              [this]() { MakeTerminalPanelService().TogglePanelMaximized(); },
          .open_terminal_find =
              [this](std::string query) {
                // A terminal behind a hidden panel or another panel tab is still
                // one to search: bring it up (it answered "No terminal to search").
                if (ActiveTerminalPane() == nullptr) {
                  return false;
                }
                if (!BottomPanelShowsTerminal() && !MakeTerminalPanelService().RevealTerminal()) {
                  return false;
                }
                // A seedless invocation preloads the terminal selection, matching
                // "Ctrl+F with something selected" everywhere else in the shell.
                if (query.empty() && TerminalHasSelection()) {
                  query = SelectedTerminalText();
                  // A multi-line selection is not a useful seed for a per-row scan.
                  if (const std::size_t newline = query.find('\n');
                      newline != std::string::npos) {
                    query.resize(newline);
                  }
                }
                context_.current_project_state.surface.focus = FocusTarget::Panel;
                terminal_find_service_.Open(ActiveTerminalPane(), query);
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
              [this, show_results_channel](std::string* error_message) {
                const bool ok = assist_service_.FindLspReferences(error_message);
                if (ok) {
                  show_results_channel("lsp.references", "LSP References");
                }
                return ok;
              },
          .show_call_hierarchy =
              [this, show_results_channel](bool incoming, std::string* error_message) {
                const bool ok = assist_service_.ShowCallHierarchy(incoming, error_message);
                if (ok) {
                  show_results_channel("lsp.callHierarchy", incoming ? "Incoming Calls" : "Outgoing Calls");
                }
                return ok;
              },
          .open_workspace_symbol_picker =
              [this](std::string query) {
                quick_pick_service_.Open(QuickPickService::WorkspaceSymbols(std::move(query)));
              },
          .change_language_mode =
              [this](const std::string& language, std::string* error) {
                editor::TextViewport* viewport = ActiveEditorViewport();
                if (viewport == nullptr) {
                  return false;
                }
                std::vector<std::string> languages = editor::runtime_syntax::FiletypeNames();
                if (language.empty()) {
                  quick_pick_service_.Open(QuickPickService::LanguageModes(
                      languages, viewport->language_override(),
                      editor::runtime_syntax::DetectFiletype(viewport->path())));
                  return true;
                }
                const std::string value = language == "auto" ? std::string() : language;
                if (!value.empty() && value != editor::runtime_syntax::kPlainTextFiletype &&
                    std::find(languages.begin(), languages.end(), value) == languages.end()) {
                  *error = "No such language: " + language;
                  return false;
                }
                ApplyLanguageModeToActiveEditor(value);
                return true;
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
          .open_editor_view_for_path =
              [this](const std::filesystem::path& path, editor::TextViewport& view) {
                return MakeTabCoordinator().OpenEditorViewForPath(path, view);
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
          .reopen_editor =
              [this](DeferredTabHandle view) {
                return MakeEditorTabService().ReopenEditor(std::move(view));
              },
          .save_tab = [this](std::size_t index) { return SaveTab(index, SaveMode::Blocking); },
          .save_tab_deferred =
              [this](std::size_t index) { return SaveTab(index, SaveMode::Deferred); },
          .revert_active_file =
              [this]() {
                const editor::TextViewport* viewport = ActiveEditableViewport();
                if (viewport == nullptr || viewport->path().empty()) {
                  return false;
                }
                const std::filesystem::path path = viewport->path();
                if (!MakeEditorTabService().RevertEditorTabsForPath(path)) {
                  return false;
                }
                // A changed-on-disk banner over this file is answered: the buffer
                // now holds what the disk holds.
                RefreshOpenCompareTabsForPath(path);
                DismissEditorBannerForPath(context_.current_project_state, path);
                RequestEditorSurfaceRedraw();
                RequestTabStripRedraw();
                return true;
              },
          .save_all_dirty_tabs =
              [this]() {
                SaveAllOutcome outcome;
                std::lock_guard<std::mutex> lock(save_tab_mutex_);
                auto& tabs = MakeEditorTabService();
                // One file open in two groups is one document: its second tab is
                // saved by the first one's write, and saving it again would run
                // the formatter twice.
                std::vector<const editor::TextViewport*> saved_views;
                for (const GroupTabRef& ref : tabs.DirtyGroupTabs()) {
                  const TabEntry& tab =
                      context_.current_project_state.editor_groups[ref.group_index].open_tabs[ref.tab_index];
                  const editor::TextViewport* view =
                      tab.kind == TabEntry::Kind::Editor && tab.editor_state.has_value() &&
                              !tab.editor_state->content_pending()
                          ? &tab.editor_state->viewport
                          : nullptr;
                  if (view != nullptr && view->path().empty()) {
                    ++outcome.untitled;
                    continue;
                  }
                  if (view != nullptr &&
                      std::any_of(saved_views.begin(), saved_views.end(),
                                  [view](const editor::TextViewport* saved) {
                                    return saved->SharesDocumentWith(*view);
                                  })) {
                    continue;
                  }
                  const std::string title = tab.title;
                  if (tabs.SaveGroupTab(ref.group_index, ref.tab_index, SaveMode::Deferred)) {
                    ++outcome.saved;
                    if (view != nullptr) {
                      saved_views.push_back(view);
                    }
                  } else {
                    outcome.failed.push_back(title);
                  }
                }
                return outcome;
              },
          .save_tab_as =
              [this](std::size_t index, const std::filesystem::path& path, std::string* error) {
                std::lock_guard<std::mutex> lock(save_tab_mutex_);
                return MakeEditorTabService().SaveGroupTabAs(
                    context_.current_project_state.clamped_focused_group_index(), index, path,
                    error);
              },
          .open_new_buffer_in_new_tab =
              [this](const std::filesystem::path& path) {
                return MakeEditorTabService().OpenNewBufferInNewTab(path);
              },
          .reset_caret_blink = [this]() { ResetCaretBlink(); },
          .notify_snippet_session_caret_moved =
              [this]() { assist_service_.NotifySnippetSessionCaretMoved(); },
          .clear_active_snippet_session_after_undo =
              [this]() { assist_service_.ClearActiveSnippetSessionAfterUndo(); },
          .split_editor_group =
              [this](EditorSplitOrientation orientation) {
                return SplitEditorGroup(orientation);
              },
          .focus_other_group = [this]() { return FocusOtherEditorGroup(); },
          .focus_editor_group_in_direction =
              [this](EditorGroupDirection direction) {
                return MakeEditorTabService().FocusEditorGroupInDirection(direction);
              },
          .focus_editor_group_at =
              [this](std::size_t index) { return MakeEditorTabService().FocusEditorGroupAt(index); },
          .step_navigation =
              [this](int direction) { return MakeEditorTabService().StepNavigation(direction); },
          .move_editor_group_in_direction =
              [this](EditorGroupDirection direction) {
                return MakeEditorTabService().MoveEditorGroupInDirection(direction);
              },
          .close_editor_group = [this]() { return CloseEditorGroup(); },
          .request_close_tab = [this](std::size_t index) { RequestCloseTab(index); },
          .request_close_tabs =
              [this](std::vector<std::size_t> indices) { RequestCloseTabs(std::move(indices)); },
          .close_all_tabs = [this]() { CloseAllTabs(); },
          .active_navigable_viewport = [this]() { return ActiveNavigableViewport(); },
          .move_tab_to_group =
              [this](std::size_t from_group, std::size_t from_index, std::size_t to_group,
                     std::size_t to_slot) {
                // Same geometry drop the drag path does: both strips change length.
                tab_strip_service_.InvalidateTabStripGeometry();
                return MakeEditorTabService().MoveTabToGroup(from_group, from_index, to_group,
                                                             to_slot);
              },
          .request_focused_editor_redraw = [this]() { RequestFocusedEditorRedraw(); },
          .active_editable_viewport = [this]() { return ActiveEditableViewport(); },
          .insert_text_into_active_text_surface =
              [this](std::string_view text, bool distribute_across_carets) {
                return MakeTextInputCoordinator().InsertTextAtActiveSurface(
                    text, distribute_across_carets);
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
          .run_compare_command =
              [this](ActionId id, std::string* error) {
                if (id == ActionId::MergeCopyIncoming || id == ActionId::MergeCopyCurrent ||
                    id == ActionId::MergeResetConflict) {
                  const MergeTabState* merge = ActiveMergeTab();
                  if (merge == nullptr) {
                    *error = "Not a merge: open one from the git view's conflicts";
                    return false;
                  }
                  if (merge->conflicts.empty()) {
                    *error = id == ActionId::MergeResetConflict ? "No conflict to reset" : "No conflict to copy";
                    return false;
                  }
                  if (id == ActionId::MergeResetConflict) {
                    MakeCompareMergeService().ResetMergeHunk();
                  } else {
                    MakeCompareMergeService().CopyMergeSideSnippet(id == ActionId::MergeCopyIncoming);
                  }
                  return true;
                }
                const CompareTabState* tab = ActiveCompareTab();
                if (tab == nullptr) {
                  *error = "Not a diff: open one from the git view, a commit or Compare";
                  return false;
                }
                CompareMergeService& compare = MakeCompareMergeService();
                switch (id) {
                  case ActionId::CompareToggleIgnoreWhitespace:
                    compare.ToggleCompareIgnoreWhitespace();
                    return true;
                  case ActionId::CompareToggleShowWhitespace:
                    compare.ToggleCompareShowWhitespace();
                    return true;
                  case ActionId::CompareCopyHunkPatch:
                    if (CompareTabSelectedHunkIndex(*tab) < 0) {
                      *error = "No change at the selection to copy";
                      return false;
                    }
                    compare.CopyCompareHunkPatch();
                    return true;
                  case ActionId::CompareCopyFilePatch:
                    if (tab->model.rows.empty()) {
                      *error = "The diff is empty";
                      return false;
                    }
                    compare.CopyCompareFilePatch();
                    return true;
                  case ActionId::CompareCopyPath:
                    compare.CopyComparePath();
                    return true;
                  case ActionId::CompareNextReviewFile:
                  case ActionId::ComparePreviousReviewFile:
                    if (tab->review_files.size() < 2) {
                      *error = "This diff is not part of a review of several files";
                      return false;
                    }
                    compare.JumpCompareReviewFile(id == ActionId::CompareNextReviewFile ? 1 : -1);
                    return true;
                  default:
                    *error = "Not a diff command";
                    return false;
                }
              },
          .unmark_branch_hunk_reviewed = [this]() { SetActiveBranchHunkReviewed(false); },
          .clear_branch_review_state = [this]() { ClearActiveBranchReviewState(); },
          .edit_branch_review_note =
              [this](std::string note_text) { EditActiveBranchReviewNote(note_text); },
          .apply_git_selected_ranges =
              [this](GitRangeVerb verb) {
                const CompareTabState* compare_tab = ActiveCompareTab();
                if (compare_tab == nullptr) {
                  return false;
                }
                // VS Code's rule: the selected lines when there are any, else the
                // change the caret is in.
                const bool lines = compare_tab->right_view_active &&
                                   compare_tab->right_viewport.selection_range().has_value();
                switch (verb) {
                  case GitRangeVerb::Stage:
                    lines ? StageCompareSelectedLines() : StageCompareHunk();
                    break;
                  case GitRangeVerb::Unstage:
                    lines ? UnstageCompareSelectedLines() : UnstageCompareHunk();
                    break;
                  case GitRangeVerb::Revert:
                    lines ? OpenDiscardCompareSelectedLinesPrompt() : OpenDiscardCompareHunkPrompt();
                    break;
                }
                return true;
              },
          .refresh_compare_tab_derived_state =
              [this](CompareTabState& compare_tab) { RefreshCompareTabDerivedState(compare_tab); },
          .sync_compare_selection_from_viewport =
              [this](CompareTabState& compare_tab, bool reveal_selection) {
                SyncCompareSelectionFromViewport(compare_tab, reveal_selection);
              },
          .active_merge_tab = [this]() { return ActiveMergeTab(); },
          .update_merge_tracking_after_viewport_edit =
              [this](MergeTabState& merge_tab) {
                UpdateMergeTrackingAfterViewportEdit(merge_tab);
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
          .terminal_has_selection = [this]() { return TerminalHasSelection(); },
          .selected_terminal_text = [this]() { return SelectedTerminalText(); },
          .last_terminal_command_text = [this]() { return LastTerminalCommandText(); },
          .selection_text_with_context = [this]() { return SelectionTextWithContext(); },
          .read_clipboard_text = [this]() { return ReadClipboardText(); },
          .paste_text_into_terminal =
              [this](std::string text) {
                MakeTextInputCoordinator().PasteTextIntoTerminal(std::move(text));
              },
          .press_key =
              [dispatch_injected](SDL_Keycode key, SDL_Keymod modifiers) {
                // A chord is pressed as a keyboard presses it: its modifiers go
                // down first and come up last, each as its own key event. What
                // reacts to a modifier's release (letting go of Ctrl opens the
                // editor Ctrl+Tab selected) saw only the chord's own release, so
                // `key ctrl+Tab` left the recent-editors pick up forever.
                struct HeldModifier {
                  SDL_Keymod mask;
                  SDL_Keycode key;
                };
                static constexpr HeldModifier kHeldModifiers[] = {
                    {SDL_KMOD_CTRL, SDLK_LCTRL},
                    {SDL_KMOD_SHIFT, SDLK_LSHIFT},
                    {SDL_KMOD_ALT, SDLK_LALT},
                    {SDL_KMOD_GUI, SDLK_LGUI},
                };
                const auto modifier_event = [&](const HeldModifier& held, bool down, SDL_Keymod held_mods) {
                  SDL_Event modifier{};
                  modifier.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
                  modifier.key.key = held.key;
                  modifier.key.mod = held_mods;
                  modifier.key.scancode = SDL_GetScancodeFromKey(held.key, nullptr);
                  modifier.key.down = down;
                  dispatch_injected(modifier);
                };
                SDL_Keymod held_mods = SDL_KMOD_NONE;
                for (const HeldModifier& held : kHeldModifiers) {
                  if ((modifiers & held.mask) != 0) {
                    held_mods = static_cast<SDL_Keymod>(held_mods | (modifiers & held.mask));
                    modifier_event(held, true, held_mods);
                  }
                }
                SDL_Event event{};
                event.type = SDL_EVENT_KEY_DOWN;
                event.key.key = key;
                event.key.mod = modifiers;
                event.key.scancode = SDL_GetScancodeFromKey(key, nullptr);
                event.key.down = true;
                dispatch_injected(event);
                // A printable key also types its character: SDL sends the text
                // as its own event between the press and the release, and every
                // text surface (the editor, the terminal, a query) reads only
                // that. Without it `key y` answered no TUI prompt at all. Where
                // no surface takes text, the text handler drops it.
                const bool chorded = (modifiers & (SDL_KMOD_CTRL | SDL_KMOD_ALT | SDL_KMOD_GUI)) != 0;
                if (!chorded) {
                  const SDL_Keycode typed =
                      (modifiers & SDL_KMOD_SHIFT) != 0
                          ? SDL_GetKeyFromScancode(event.key.scancode, SDL_KMOD_SHIFT, false)
                          : key;
                  if (typed >= 0x20 && typed < 0x7F) {
                    const char text[2] = {static_cast<char>(typed), '\0'};
                    SDL_Event text_event{};
                    text_event.type = SDL_EVENT_TEXT_INPUT;
                    text_event.text.text = text;
                    dispatch_injected(text_event);
                  }
                }
                event.type = SDL_EVENT_KEY_UP;
                event.key.down = false;
                dispatch_injected(event);
                for (auto held = std::rbegin(kHeldModifiers); held != std::rend(kHeldModifiers); ++held) {
                  if ((modifiers & held->mask) != 0) {
                    held_mods = static_cast<SDL_Keymod>(held_mods & ~(modifiers & held->mask));
                    modifier_event(*held, false, held_mods);
                  }
                }
              },
          .inject_pointer_event =
              [dispatch_injected](const SDL_Event& event, SDL_Keymod modifiers) {
                // Click handlers read the held modifiers from SDL, not the event.
                const SDL_Keymod previous = SDL_GetModState();
                SDL_SetModState(modifiers);
                dispatch_injected(event);
                SDL_SetModState(previous);
              },
          .show_hover_at_caret =
              [this]() {
                const editor::TextViewport* viewport = ActiveEditableViewport();
                if (viewport == nullptr) {
                  return false;
                }
                context_.interaction_state.caret_hover = InteractionState::CaretHover{
                    .active = true,
                    .viewport = viewport,
                    .line = viewport->cursor_line(),
                    .column = viewport->cursor_column(),
                    .pointer_x = last_mouse_x_,
                    .pointer_y = last_mouse_y_,
                    .pointer_valid = last_mouse_position_valid_,
                };
                // Asked for: a fresh answer, not one kept from an earlier hover.
                if (plugin_hover_cache_.state == PluginHoverCache::State::Resolved ||
                    plugin_hover_cache_.state == PluginHoverCache::State::Failed) {
                  plugin_hover_cache_.state = PluginHoverCache::State::Empty;
                  ++plugin_hover_cache_.generation;
                }
                UpdateEditorHover(last_mouse_x_, last_mouse_y_);
                RequestWindowRedraw();
                // Resolved at the caret, or being fetched (a language server's
                // answer arrives later and re-resolves into the card).
                return context_.interaction_state.caret_hover.active;
              },
          .refresh_available_colorscheme_names =
              [this]() { MakePersistenceCoordinator().RefreshAvailableColorschemeNames(); },
          .available_colorscheme_names = [this]() { return available_colorscheme_names_; },
          .apply_colorscheme =
              [this](std::string_view name) {
                return MakePersistenceCoordinator().ApplyColorscheme(name, true, true);
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
          .note_layout_inputs_changed = [this]() { NoteLayoutInputsChanged(); },
          .request_window_redraw = [this]() { RequestWindowRedraw(); },
          .focus_notifications =
              [this]() {
                if (!notification_service_.Focus()) {
                  return false;
                }
                RequestWindowRedraw();
                return true;
              },
          .take_notification_action =
              [this](std::string_view row, std::string_view action,
                     std::string* error) -> std::optional<NotificationAction> {
                const std::optional<std::size_t> row_index = notification_service_.FindRow(row);
                if (!row_index.has_value()) {
                  *error = "no notification \"" + std::string(row) + "\"";
                  return std::nullopt;
                }
                const std::optional<std::size_t> action_index =
                    notification_service_.FindAction(*row_index, action);
                if (!action_index.has_value()) {
                  *error = "notification \"" + std::string(row) + "\" has no action \"" +
                           std::string(action) + "\"";
                  return std::nullopt;
                }
                RequestWindowRedraw();
                return notification_service_.TakeAction(*row_index, *action_index);
              },
          .activate_editor_banner =
              [this](std::string_view label, std::string* error) {
                const EditorBannerState* banner =
                    ActiveEditorBannerForTab(context_.current_project_state);
                if (banner == nullptr) {
                  *error = "the active tab shows no banner";
                  return false;
                }
                // By label, ignoring case and a trailing ellipsis: `banner-action Save
                // As` presses "Save As…" (nobody types the ellipsis).
                const auto bare = [](std::string_view text) {
                  for (const std::string_view ellipsis : {std::string_view("\u2026"),
                                                          std::string_view("...")}) {
                    if (text.ends_with(ellipsis)) {
                      text.remove_suffix(ellipsis.size());
                    }
                  }
                  return text;
                };
                const std::string wanted = util::ToLowerAscii(bare(label));
                for (const EditorBannerButton& button : EditorBannerButtonsFor(banner->kind)) {
                  if (util::EqualsAsciiCaseInsensitive(bare(button.label), wanted)) {
                    // The path is copied: the action may erase the banner it came from.
                    ActivateEditorBannerAction(button.action, std::filesystem::path(banner->path));
                    RequestWindowRedraw();
                    return true;
                  }
                }
                *error = "the banner has no \"" + std::string(label) + "\" button";
                return false;
              },
          .show_output_channel =
              [this](std::string_view channel_id) {
                const auto& channels = output_channels_.Channels();
                if (std::none_of(channels.begin(), channels.end(), [&](const auto& channel) {
                      return channel.id == channel_id;
                    })) {
                  return false;
                }
                // The debug console's recipe: open the tab, select it, follow the tail.
                EnsureOutputChannelTabOpen(channel_id);
                auto& panel = context_.current_project_state.panel;
                panel.content = PanelContentKind::Output;
                panel.output.channel_id = std::string(channel_id);
                panel.output.follow_tail = true;
                NoteLayoutInputsChanged();
                RequestWindowRedraw();
                return true;
              },
          .remote_hosts = [this]() -> RemoteHostService& { return MakeRemoteHostService(); },
          .cancel_save_wait =
              [this](std::uint64_t id) {
                EditorTabService& editor_tabs = MakeEditorTabService();
                PromptSurfaceService& prompt_surfaces = MakePromptSurfaceService();
                return MakeDirtyPromptCoordinator(editor_tabs, prompt_surfaces)
                    .CancelSaveContinuation(id);
              },
          .request_toggle_fullscreen =
              [this]() { pending_window_action_ = WindowAction::ToggleFullscreen; },
          .active_terminal_pane = [this]() { return ActiveTerminalPane(); },
          .plugin_runtime_enabled = [this]() { return plugin_runtime_.enabled(); },
          .reload_plugins_for_current_project =
              [this](bool announce) {
                ReloadPluginsForCurrentProject(PluginReloadRequest{.announce = announce});
              },
          .request_quit = [this]() { RequestQuit(); },
          .start_debugging = [this]() { return StartDebuggingWithDefaultConfig(); },
          .stop_debugging = [this]() { StopDebugging(); },
          .debug_session_active = [this]() { return IsDebugSessionActive(); },
          .debug_continue = [this]() { DebugContinue(); },
          .debug_step_over = [this]() { DebugStepOver(); },
          .debug_step_in = [this]() { DebugStepIn(); },
          .debug_step_out = [this]() { DebugStepOut(); },
          .debug_pause = [this]() { DebugPause(); },
          .debug_restart = [this]() { DebugRestart(); },
          .debug_reverse_continue = [this]() { DebugReverseContinue(); },
          .debug_step_back = [this]() { DebugStepBack(); },
          .debug_supports_reverse = [this]() { return DebugSupportsReverse(); },
          .debug_session_count = [this]() { return CurrentDapManager().SessionCount(); },
          .debug_switch_session = [this](int index) { DebugSwitchSession(index); },
          .stop_all_debug_sessions = [this]() { StopAllDebugSessions(); },
          .open_debug_repl_prompt = [this]() { OpenDebugReplPrompt(); },
          .open_launch_config_picker =
              [this]() {
                quick_pick_service_.Open(
                    QuickPickService::LaunchConfigs(context_.current_project_state.launch_configs));
              },
          .open_color_theme_picker =
              [this]() {
                MakePersistenceCoordinator().RefreshAvailableColorschemeNames();
                quick_pick_service_.Open(QuickPickService::ColorThemes(
                    available_colorscheme_names_, context_.current_project_state.active_colorscheme_name));
              },
          .open_command_choices = [this](QuickPickState pick) { quick_pick_service_.Open(std::move(pick)); },
          .output_channels = [this]() -> const std::vector<WorkspaceOutputChannels::ChannelInfo>& {
            return output_channels_.Channels();
          },
          .open_document_symbol_picker =
              [this](std::string query) {
                const editor::TextViewport* viewport = ActiveEditorViewport();
                if (viewport == nullptr || viewport->path().empty()) {
                  return;
                }
                QuickPickState picker = QuickPickService::DocumentSymbolsLoading(QuickPickState::View{
                    .path = viewport->path(),
                    .line = viewport->cursor_line(),
                    .column = viewport->cursor_column(),
                    .scroll = viewport->scroll_line(),
                });
                picker.query.SetText(std::move(query));
                quick_pick_service_.Open(std::move(picker));
                QueryDocumentSymbols(*viewport, [this](const std::filesystem::path& path, const std::string&,
                                                       std::vector<plugin::PluginHost::DocumentSymbolNode> symbols) {
                  const QuickPickState& pick = context_.current_project_state.overlay.workflow.quick_pick;
                  if (!pick.revert_view.has_value() || pick.revert_view->path != path) {
                    return;  // another file's pick is up by now
                  }
                  (void)quick_pick_service_.DeliverItems(QuickPickKind::DocumentSymbol,
                                                         QuickPickService::DocumentSymbolItems(symbols));
                });
              },
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
                return debug_service_.SetExceptionFilterCondition(filter_id, std::move(condition));
              },
          .move_buffer_search_selection = [this](int delta) { MoveBufferSearchSelection(delta); },
          .reveal_buffer_search_match =
              [this](const editor::SelectionRange& match) {
                RevealBufferSearchMatch(match);
                RequestEditorSurfaceRedraw();
              },
      });
  return *glue_->action_context;
}

}  // namespace microide::workspace
