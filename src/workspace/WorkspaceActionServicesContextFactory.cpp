#include "workspace/WorkspaceShell.h"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "workspace/WorkspaceActionServices.h"
#include "workspace/WorkspaceCommandParsing.h"
#include "workspace/WorkspaceCommandPromptCoordinator.h"
#include "workspace/WorkspaceMenuCoordinator.h"
#include "workspace/WorkspacePersistenceCoordinator.h"
#include "workspace/WorkspaceTextInputCoordinator.h"

namespace microide::workspace {

void WorkspaceActionContext::OpenCommandPrompt(std::string input) {
  const bool bottom_panel_was_visible =
      state_.panel.command_mode || operations_.active_terminal_tab() != nullptr;
  state_.panel.command_mode = true;
  state_.surface.focus = FocusTarget::Panel;
  state_.panel.command.input.SetText(std::move(input));
  operations_.reset_command_prompt_session();
  operations_.request_command_mode_transition_redraw(bottom_panel_was_visible);
}

bool WorkspaceActionContext::PluginRuntimeEnabled() const {
  return operations_.plugin_runtime_enabled();
}

void WorkspaceActionContext::ReloadPluginsWithFeedback() {
  operations_.reload_plugins_for_current_project();
  state_.panel.command.feedback_text = operations_.plugin_runtime_reload_summary();
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

void WorkspaceActionContext::StartDebuggingWithFeedback() {
  if (!operations_.start_debugging) {
    return;
  }
  const std::string error = operations_.start_debugging();
  state_.panel.command.feedback_text =
      error.empty() ? std::string("Debugging started") : ("Debug: " + error);
}

void WorkspaceActionContext::StopDebuggingWithFeedback() {
  if (operations_.stop_debugging) {
    operations_.stop_debugging();
  }
  state_.panel.command.feedback_text = "Debugging stopped";
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

void WorkspaceActionContext::EditBreakpointModifierFromMenu(ActionId id) {
  if (operations_.edit_breakpoint_modifier_from_menu) {
    operations_.edit_breakpoint_modifier_from_menu(id);
  }
}

void WorkspaceActionContext::RemoveBreakpointFromMenu() {
  if (operations_.remove_breakpoint_from_menu) {
    operations_.remove_breakpoint_from_menu();
  }
}

WorkspaceActionContext WorkspaceShell::MakeActionContext() {
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
                    MakeCommandPromptCoordinator().RejectAction(source, std::move(feedback));
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
          .show_plugin_sidebar =
              [this](std::string_view id, bool temporary) {
                return ShowPluginSidebar(id, temporary);
              },
          .current_window_rect = [this]() { return CurrentWindowRect(); },
          .refresh_project_files = [this]() { RefreshProjectFiles(); },
          .reload_clean_open_buffers_from_disk = [this]() { ReloadCleanOpenBuffersFromDisk(); },
          .tree_mutation_base_path =
              [this](ActionSource source) { return TreeMutationBasePath(source); },
          .resolve_tree_action_path =
              [this](ActionSource source) { return ResolveTreeActionPath(source); },
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
          .open_terminal = [this](std::string command) { OpenTerminal(std::move(command)); },
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
          .find_lsp_references =
              [this](std::string* error_message) {
                return assist_service_.FindLspReferences(error_message);
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
          .request_inline_completion =
              [this](std::string* error_message) {
                if (error_message != nullptr) {
                  *error_message = "Inline completion is retired";
                }
                return false;
              },
          .open_compare_picker_for_path =
              [this](const std::filesystem::path& path, const std::string& commit_spec) {
                OpenComparePickerForPath(path, commit_spec);
              },
          .open_comparison = [this](const project::GitCommitEntry& commit) { OpenComparison(commit); },
          .open_merge_editor =
              [this](const std::filesystem::path& base_path,
                     const std::filesystem::path& incoming_path,
                     const std::filesystem::path& current_path,
                     const std::filesystem::path& output_path) {
                return OpenMergeEditor(base_path, incoming_path, current_path, output_path);
              },
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
          .split_active_editor =
              [this](EditorSplitOrientation orientation) { return SplitActiveEditor(orientation); },
          .unsplit_active_editor = [this]() { return UnsplitActiveEditor(); },
          .cycle_editor_split = [this](int delta) { return CycleEditorSplit(delta); },
          .activate_ordered_editor_split =
              [this](std::size_t index) { return ActivateOrderedEditorSplit(index); },
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
          .has_selection_at_active_single_line_text_surface =
              [this]() {
                return MakeTextInputCoordinator().HasSelectionAtActiveSingleLineSurface();
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
          .mark_branch_file_reviewed = [this]() { MarkActiveBranchFileReviewed(); },
          .mark_branch_hunk_reviewed = [this]() { MarkActiveBranchHunkReviewed(); },
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
              [this](MergeTabState& merge_tab, const std::vector<std::string>& before_lines,
                     std::optional<editor::SelectionRange> selection_before,
                     editor::TextPosition cursor_before) {
                UpdateMergeTrackingAfterViewportEdit(merge_tab, before_lines, selection_before,
                                                     cursor_before);
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
          .paste_clipboard_into_terminal =
              [this]() { MakeTextInputCoordinator().PasteClipboardIntoTerminal(); },
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
          .normalize_sidebar_view_selection = [this]() { NormalizeSidebarViewSelection(); },
          .apply_ui_scale =
              [this](float scale) { MakePersistenceCoordinator().ApplyUiScale(scale, true, true); },
          .mark_layout_dirty = [this]() { MarkLayoutDirty(); },
          .request_window_redraw = [this]() { RequestWindowRedraw(); },
          .active_terminal_tab = [this]() { return ActiveTerminalTab(); },
          .reset_command_prompt_session =
              [this]() { MakeCommandPromptCoordinator().ResetSessionState(); },
          .request_command_mode_transition_redraw =
              [this](bool bottom_panel_was_visible) {
                RequestCommandModeTransitionRedraw(bottom_panel_was_visible);
              },
          .plugin_runtime_enabled = [this]() { return plugin_runtime_.enabled(); },
          .reload_plugins_for_current_project = [this]() { ReloadPluginsForCurrentProject(); },
          .plugin_runtime_reload_summary = [this]() { return PluginRuntimeReloadSummary(); },
          .request_quit = [this]() { RequestQuit(); },
          .start_debugging = [this]() { return StartDebuggingWithDefaultConfig(); },
          .stop_debugging = [this]() { StopDebugging(); },
          .has_debug_adapters = [this]() { return CurrentDapManager().HasRegisteredAdapters(); },
          .debug_session_active = [this]() { return IsDebugSessionActive(); },
          .debug_session_stopped = [this]() { return IsDebugSessionStopped(); },
          .debug_continue = [this]() { DebugContinue(); },
          .debug_step_over = [this]() { DebugStepOver(); },
          .debug_step_in = [this]() { DebugStepIn(); },
          .debug_step_out = [this]() { DebugStepOut(); },
          .debug_pause = [this]() { DebugPause(); },
          .debug_restart = [this]() { DebugRestart(); },
          .debug_session_count = [this]() { return CurrentDapManager().SessionCount(); },
          .debug_switch_session = [this](int index) { DebugSwitchSession(index); },
          .stop_all_debug_sessions = [this]() { StopAllDebugSessions(); },
          .open_debug_repl_prompt = [this]() { OpenDebugReplPrompt(); },
          .open_launch_config_picker = [this]() { OpenLaunchConfigPicker(); },
          .edit_breakpoint_modifier_from_menu =
              [this](ActionId id) { EditBreakpointModifierFromMenu(id); },
          .remove_breakpoint_from_menu = [this]() { RemoveBreakpointFromMenu(); },
          .toggle_debug_pane = [this]() { ToggleDebugPane(); },
          .show_debug_pane_mode = [this](DebugPaneMode mode) { ShowDebugPaneMode(mode); },
      });
}

}  // namespace microide::workspace
