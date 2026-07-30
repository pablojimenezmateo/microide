#pragma once

#include <cstddef>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "workspace/NotificationService.h"
#include "workspace/WorkspaceActionRequests.h"
#include "workspace/CompareInput.h"
#include "workspace/WorkspaceActionTypes.h"
#include "workspace/WorkspaceMenuState.h"
#include "workspace/WorkspaceProjectState.h"
#include "workspace/WorkspacePromptState.h"
#include "workspace/WorkspaceSidebarRegistry.h"
#include "workspace/WorkspaceTabState.h"

namespace microide::workspace {

// Defined in GitSidebarCommandCenter.h; only the type is needed here (function
// signatures + a parameter), so forward-declare to avoid the heavy include.
enum class GitSidebarActionId;

enum class ProjectOpenPickerResult {
  Launched,
  AlreadyOpen,
  Unavailable,
};

// TD-2026-07-17A-119: shared clipboard-export byte budget. A huge explicit selection (or
// many multi-caret selections) must not allocate and duplicate a large fraction of a big
// file into both clipboards on the shell thread. Copies over this budget are truncated on
// a UTF-8 boundary with a marker; a cut over this budget is refused (never delete data
// that could not be captured).
inline constexpr std::size_t kMaxClipboardExportBytes = 64u * 1024 * 1024;  // 64 MiB

struct ClipboardExportResult {
  std::string text;
  bool truncated = false;
};

// Clamp `text` to `budget` bytes on a UTF-8 codepoint boundary, appending a truncation
// marker when it does not fit. Exposed (with an injectable budget) so the truncation
// logic is unit-testable without materializing a 64 MiB buffer.
[[nodiscard]] ClipboardExportResult ClampClipboardExport(
    std::string_view text, std::size_t budget = kMaxClipboardExportBytes);

class WorkspaceActionContext {
 public:
  struct Operations {
    std::function<void()> close_tree_context_menu;
    std::function<bool(ActionSource, std::string)> reject_action;
    std::function<SidebarViewRequest(const std::vector<std::string>&)> parse_sidebar_view_request;
    std::function<bool(const std::filesystem::path&, bool, bool)> open_project;
    std::function<void(std::size_t)> request_close_project;
    std::function<bool(std::size_t, bool)> switch_project;
    std::function<ProjectOpenPickerResult()> open_native_project_picker;
    std::function<ProjectOpenPickerResult()> open_native_file_picker;
    std::function<SidebarMode()> active_sidebar_mode;
    std::function<void()> toggle_sidebar;
    std::function<void()> close_sidebar;
    std::function<void(const std::filesystem::path&)> show_tree_sidebar;
    std::function<void(std::string, bool)> show_search_sidebar;
    std::function<void()> show_problems_sidebar;
    std::function<void()> show_git_sidebar;
    std::function<void()> show_tests_sidebar;
    std::function<void()> show_outline_sidebar;
    std::function<bool(std::string_view, bool)> show_plugin_sidebar;
    std::function<std::optional<SDL_FRect>()> current_window_rect;
    std::function<void()> refresh_project_files;
    std::function<void()> reload_clean_open_buffers_from_disk;
    // Dispatch one git write action (switch/create/fetch/pull/push/publish/sync/
    // stash/stash-pop). Returns a rejection sentence on failure to start, or an
    // empty string when the operation was dispatched to the background executor.
    std::function<std::string(ActionId, const std::vector<std::string>&)> dispatch_git_operation;
    std::function<std::filesystem::path(ActionSource)> tree_mutation_base_path;
    std::function<std::filesystem::path(ActionSource)> resolve_tree_action_path;
    // The path the *open* row context menu targets, or empty when no such menu is
    // open. Unlike resolve_tree_action_path this never falls back to the tree
    // selection, so an action shared between the row menus and the editor-tab menu
    // can tell "this menu named a row" from "ask the tree what is selected".
    std::function<std::filesystem::path()> row_context_menu_path;
    std::function<std::filesystem::path()> active_tab_path;
    std::function<void(PromptSurfaceState::Action,
                       PromptSurfaceState::Kind,
                       const std::filesystem::path&,
                       std::string)>
        open_prompt_surface;
    std::function<bool(std::string_view)> write_clipboard_text;
    std::function<bool(std::string_view)> write_primary_selection_text;
    std::function<bool(const std::filesystem::path&)> reveal_path_in_file_explorer;
    // Reveal a path in the in-app sidebar tree: force-expand ancestors, select it,
    // and scroll it into view. Returns whether a matching row was found.
    std::function<bool(const std::filesystem::path&)> reveal_path_in_tree;
    std::function<void(std::string)> open_terminal;
    std::function<bool()> close_active_terminal;
    // Opens/refocuses the terminal find bar; false when no terminal is showing.
    std::function<bool(std::string)> open_terminal_find;
    std::function<void(OverlayMode)> show_overlay;
    std::function<void()> dismiss_overlay;
    std::function<void()> open_settings_overlay;
    std::function<void()> open_help_about_overlay;
    std::function<void()> toggle_status_bar;
    std::function<void()> toggle_layout_mode;
    std::function<editor::TextViewport*()> active_editor_viewport;
    std::function<void()> open_buffer_search;
    std::function<void()> refresh_buffer_search;
    std::function<void()> open_buffer_replace;
    std::function<bool(std::string*)> show_completion_overlay;
    std::function<bool(std::string*)> show_insert_snippet_overlay;
    std::function<bool(std::string*)> show_code_actions_overlay;
    std::function<bool(std::string*)> go_to_lsp_definition;
    std::function<bool(std::string*)> go_to_lsp_type_definition;
    std::function<bool(std::string*)> go_to_lsp_implementation;
    std::function<bool(std::string*)> go_to_lsp_declaration;
    std::function<bool(std::string*)> find_lsp_references;
    // `incoming` selects callers-of vs called-by; both render into the same channel.
    std::function<bool(bool, std::string*)> show_call_hierarchy;
    std::function<bool(const std::string&, std::string*)> show_workspace_symbols;
    std::function<bool(std::string*)> format_active_document;
    std::function<std::string()> symbol_at_cursor;
    std::function<bool(const std::string&, std::string*)> rename_symbol;
    // Refine a just-opened Rename Symbol prompt from the server's prepareRename
    // (prefill the placeholder / reject non-renameable positions). Best-effort.
    std::function<void(std::string)> refine_rename_prompt;
    std::function<bool(std::string*)> show_signature_help;
    std::function<bool(std::string*)> discover_tests_for_active_buffer;
    std::function<bool(const std::vector<std::string>&, std::string*)> run_tests;
    std::function<bool(std::string*)> run_all_discovered_tests;
    std::function<void(const std::filesystem::path&, const std::string&)> open_compare_picker_for_path;
    std::function<void(const project::GitCommitEntry&)> open_comparison;
    // Non-git ("plain") comparison of two arbitrary sides (file/buffer/clipboard).
    std::function<bool(CompareInput, CompareInput)> open_plain_comparison;
    std::function<bool(const std::filesystem::path&,
                       const std::filesystem::path&,
                       const std::filesystem::path&,
                       const std::filesystem::path&)>
        open_merge_editor;
    // Batch review sessions: switch to Source Control and open diff/merge tabs,
    // deduping + cleaning stale review tabs. Return a summary outcome.
    std::function<ReviewOpenOutcome()> open_conflict_review;
    std::function<ReviewOpenOutcome(const std::string&)> open_branch_review;
    std::function<ReviewOpenOutcome(const std::string&)> open_commit_review;
    std::function<TabEntry::EditorTabState*()> active_editor_tab;
    std::function<editor::FoldingModel*()> ensure_active_folding_model_fresh;
    std::function<bool(const editor::TextViewport&)> replace_active_editor_view;
    std::function<void(const std::filesystem::path&)> open_file;
    std::function<bool(const std::filesystem::path&)> open_file_in_new_tab;
    std::function<bool()> open_untitled_tab;
    std::function<std::optional<std::size_t>(std::string_view, std::string*)>
        find_tab_index_by_specifier;
    std::function<void(std::size_t)> activate_tab;
    std::function<bool(std::size_t)> move_active_tab_to;
    std::function<bool()> reopen_active_tab;
    std::function<bool(std::size_t)> save_tab;
    std::function<void()> reset_caret_blink;
    std::function<void()> notify_snippet_session_caret_moved;
    std::function<void()> clear_active_snippet_session_after_undo;
    std::function<bool(EditorSplitOrientation)> split_editor_group;
    std::function<std::size_t()> editor_group_count;
    std::function<bool()> focus_other_group;
    std::function<bool()> close_editor_group;
    std::function<void(std::size_t)> request_close_tab;
    std::function<void(std::vector<std::size_t>)> request_close_tabs;
    std::function<void()> close_all_tabs;
    std::function<editor::TextViewport*()> active_navigable_viewport;
    std::function<void()> request_focused_editor_redraw;
    std::function<editor::TextViewport*()> active_editable_viewport;
    std::function<bool(std::string_view)> insert_text_into_active_text_surface;
    std::function<bool()> has_active_single_line_text_surface;
    std::function<std::string()> selected_text_at_active_single_line_text_surface;
    std::function<bool()> select_all_at_active_single_line_text_surface;
    std::function<bool()> cut_selection_at_active_single_line_text_surface;
    std::function<CompareTabState*()> active_compare_tab;
    std::function<void()> mark_branch_file_reviewed;
    std::function<void()> unmark_branch_file_reviewed;
    std::function<void()> mark_branch_hunk_reviewed;
    std::function<void()> unmark_branch_hunk_reviewed;
    std::function<void()> clear_branch_review_state;
    std::function<void(std::string)> edit_branch_review_note;
    std::function<void(CompareTabState&)> refresh_compare_tab_derived_state;
    std::function<void(CompareTabState&, bool)> sync_compare_selection_from_viewport;
    std::function<MergeTabState*()> active_merge_tab;
    std::function<void(MergeTabState&,
                       std::optional<editor::SelectionRange>,
                       editor::TextPosition)>
        update_merge_tracking_after_viewport_edit;
    std::function<void(bool)> request_active_tab_redraw;
    std::function<void()> request_active_editable_last_change_redraw;
    std::function<void(const std::vector<std::string>&, const std::vector<std::string>&)>
        request_active_editable_change_redraw;
    std::function<void(std::size_t, std::size_t)> request_active_editable_blame_neighborhood_redraw;
    std::function<void()> request_tab_strip_redraw;
    std::function<bool()> terminal_has_selection;
    std::function<std::string()> selected_terminal_text;
    std::function<std::optional<std::string>()> last_terminal_command_text;
    std::function<std::optional<std::string>()> selection_text_with_context;
    std::function<std::optional<std::string>()> read_clipboard_text;
    std::function<void(std::string)> paste_text_into_terminal;
    std::function<void()> refresh_available_colorscheme_names;
    std::function<void(std::string_view)> apply_colorscheme;
    std::function<void()> apply_editor_preferences_to_all_tabs;
    std::function<void()> save_config_state;
    std::function<std::optional<std::string>(std::string_view)> get_setting_value;
    std::function<bool(std::string_view, std::string)> set_setting_value;
    // Post a transient host notification toast (and schedule a redraw).
    std::function<void(NotificationService::Tone, std::string)> notify;
    std::function<void(float)> apply_ui_scale;
    std::function<void()> mark_layout_dirty;
    std::function<void()> request_window_redraw;
    // Queue the host-level full-screen toggle. The window itself is owned by
    // Application, which polls WorkspaceShell::ConsumeWindowAction each frame;
    // the shell only records the request.
    std::function<void()> request_toggle_fullscreen;
    std::function<TerminalTabState*()> active_terminal_tab;
    std::function<bool()> plugin_runtime_enabled;
    std::function<void()> reload_plugins_for_current_project;
    std::function<std::string()> plugin_runtime_reload_summary;
    std::function<void()> request_quit;
    // Debugger (DAP). start_debugging builds a default launch config from the
    // current project's registered adapters; it returns an error string (empty
    // on success). has_debug_adapters reports whether any adapter is registered.
    std::function<std::string()> start_debugging;
    std::function<void()> stop_debugging;
    std::function<bool()> debug_session_active;
    // True when the active session is paused (`stopped`); gates continue/step
    // (require stopped) vs. pause (requires running). Execution-control verbs
    // forward to the active session (no-op when none / wrong state).
    std::function<bool()> debug_session_stopped;
    std::function<void()> debug_continue;
    std::function<void()> debug_step_over;
    std::function<void()> debug_step_in;
    std::function<void()> debug_step_out;
    std::function<void()> debug_pause;
    std::function<void()> debug_restart;
    // Reverse execution. Forward to the active session (no-op when none / wrong
    // state / adapter lacks `supportsStepBack`). debug_supports_reverse reports the
    // capability so availability can hide the verbs for non-recording adapters.
    std::function<void()> debug_reverse_continue;
    std::function<void()> debug_step_back;
    std::function<bool()> debug_supports_reverse;
    // Multi-session switcher (Phase 8). debug_session_count reports how many live
    // sessions exist (gates the switch command); debug_switch_session cycles to the
    // next session (index < 0) or selects a 1-based index.
    std::function<std::size_t()> debug_session_count;
    std::function<void(int index)> debug_switch_session;
    // Stop every live debug session (Phase 10).
    std::function<void()> stop_all_debug_sessions;
    // Debug-console REPL (Phase 9): open the single-line prompt that evaluates an
    // expression in the active session and appends the result to the console.
    std::function<void()> open_debug_repl_prompt;
    // Launch-config picker (Phase 9): open the fuzzy picker over the project's
    // launch configs.
    std::function<void()> open_launch_config_picker;
    // Open the command palette over all built-in + plugin commands, optionally pre-filling
    // the query (e.g. "project-open ") so the user only types the argument.
    std::function<void(std::string)> open_command_palette;
    // Breakpoint-modifier context-menu handlers (Phase 6); read the gutter
    // menu's target line on the shell side.
    std::function<void(ActionId)> edit_breakpoint_modifier_from_menu;
    std::function<void(ActionId)> breakpoint_quick_action_from_menu;
    std::function<void()> remove_breakpoint_from_menu;
    // Git sidebar entry context-menu dispatch: forwards to the shell's
    // DispatchGitSidebarAction for the given entry index.
    std::function<bool(GitSidebarActionId, std::size_t)> dispatch_git_sidebar_action;
    // Right-side debug pane: toggle visibility, or show a specific surface.
    std::function<void()> toggle_debug_pane;
    std::function<void(DebugPaneMode)> show_debug_pane_mode;
    // Surface the active debug session's console output in the bottom panel.
    std::function<void()> show_debug_output;
    // Add a watch expression directly (no prompt) — the "Add to Watch" row menu
    // item, which takes the expression from the row it was invoked on.
    std::function<void(std::string)> add_debug_watch_expression;
    // Headless breakpoint control (control channel + cold-start spec): re-send a
    // file's breakpoints to the active session after the store is mutated.
    std::function<void(const std::filesystem::path&)> resend_breakpoints_for_file;
    // Start a debug session for a named launch config (empty name → the
    // selected/default config). Returns an error string (empty on success).
    std::function<std::string(const std::string&)> start_named_debug_config;
    std::function<std::string(const std::string&, const std::vector<std::string>&,
                              const std::string&)>
        start_ad_hoc_debug;
    // Function (symbol) breakpoints + exception-filter conditions (control channel
    // + command line). Each mutates the per-project store + live re-sends.
    std::function<void(const std::string&)> add_function_breakpoint;
    std::function<void(const std::string&)> remove_function_breakpoint;
    std::function<void(const std::string&)> toggle_function_breakpoint;
    std::function<void(const std::string&, std::optional<std::string>)>
        set_function_breakpoint_condition;
    std::function<void(const std::string&, std::optional<std::string>)>
        set_exception_filter_condition;
  };

  WorkspaceActionContext(ProjectCatalogState& project_catalog,
                         ProjectWorkspaceState& current_project_state,
                         float& ui_scale,
                         Operations operations);

  void PrepareForAction(ActionSource source);
  bool RejectAction(ActionSource source, std::string feedback);

  SidebarViewRequest ParseSidebarViewRequest(const std::vector<std::string>& args) const;

  bool HasProjectRoot() const;
  bool HasActiveProject() const;
  std::size_t ProjectCount() const;
  std::size_t ActiveProjectIndex() const;
  std::filesystem::path ProjectRoot() const;
  bool OpenProject(const std::filesystem::path& project_root,
                   bool restore_persistence,
                   bool log_feedback);
  void RequestCloseProject(std::size_t index);
  bool SwitchProject(std::size_t index, bool log_feedback);
  ProjectOpenPickerResult OpenNativeProjectPicker();
  ProjectOpenPickerResult OpenNativeFilePicker();

  bool SidebarVisible() const;
  bool SidebarTemporary() const;
  std::string_view SidebarViewId() const;
  SidebarMode ActiveSidebarMode() const;
  void ShowSidebarSurface();
  void ToggleSidebar();
  void CloseSidebar();
  bool ShowSidebarView(const SidebarViewInfo& view,
                       const std::filesystem::path& root,
                       const std::string& query);
  bool ToggleSidebarView(const SidebarViewInfo& view,
                         const std::filesystem::path& root,
                         const std::string& query);
  float CurrentWindowWidth() const;
  void SetSidebarWidth(float width);
  void RefreshProjectFiles();
  void ReloadCleanOpenBuffersFromDisk();
  std::string DispatchGitOperation(ActionId id, const std::vector<std::string>& args);
  std::filesystem::path TreeMutationBasePath(ActionSource source) const;
  // The selected row of whichever debug value surface (Variables / Watch) the pane
  // is showing. `valid` is false in Call Stack / Breakpoints mode, with no rows, or
  // when the selection points at a synthetic placeholder / "show more…" row.
  struct DebugValueRowSelection {
    bool valid = false;
    std::string name;
    std::string value;
  };
  DebugValueRowSelection SelectedDebugValueRow() const;
  void AddDebugWatchExpression(std::string expression);

  std::filesystem::path ResolveTreeActionPath(ActionSource source) const;
  std::filesystem::path RowContextMenuPath() const;
  std::filesystem::path ActiveTabPath() const;
  void OpenCreatePathPrompt(bool directory, const std::filesystem::path& base_path);
  void OpenRenamePathPrompt(const std::filesystem::path& path);
  void OpenDeletePathPrompt(const std::filesystem::path& path);
  bool WriteClipboardText(std::string_view text) const;
  bool WritePrimarySelectionText(std::string_view text) const;
  bool RevealPathInFileExplorer(const std::filesystem::path& directory) const;
  bool RevealPathInTree(const std::filesystem::path& path) const;

  void OpenTerminal(std::string command);
  // Closes the active terminal tab. False when there is none — the terminal
  // strip was the one tab strip whose tabs could only be closed by mouse.
  bool CloseActiveTerminal();
  bool OpenTerminalFind(std::string query);
  void ShowFileFinderWithQuery(std::string query);
  void ShowFileFinder();
  bool OverlayVisible() const;
  void DismissOverlay();
  void OpenSettingsOverlay();
  void OpenHelpAboutOverlay();
  void ToggleStatusBar();
  void ToggleLayoutMode();
  void ShowProjectSearchSidebar(std::string query);
  bool ShowCompletionOverlay(std::string* error_message);
  bool ShowInsertSnippetOverlay(std::string* error_message);
  bool ShowCodeActionsOverlay(std::string* error_message);
  bool GoToLspDefinition(std::string* error_message);
  bool GoToLspTypeDefinition(std::string* error_message);
  bool GoToLspImplementation(std::string* error_message);
  bool GoToLspDeclaration(std::string* error_message);
  bool ShowSignatureHelp(std::string* error_message);
  bool FindLspReferences(std::string* error_message);
  bool ShowWorkspaceSymbols(const std::string& query, std::string* error_message);
  bool ShowCallHierarchy(bool incoming, std::string* error_message);
  bool FormatActiveDocument(std::string* error_message);
  bool RenameSymbol(const std::string& new_name, std::string* error_message);
  bool DiscoverTestsForActiveBuffer(std::string* error_message);
  bool RunTests(const std::vector<std::string>& test_ids, std::string* error_message);
  bool RunAllDiscoveredTests(std::string* error_message);
  bool ActiveTabIsCompare() const;
  bool ActiveTabIsMerge() const;
  void OpenBufferSearch(std::string query);
  void OpenBufferReplace();
  std::filesystem::path ResolveComparePath(const std::filesystem::path& requested_path,
                                           ActionSource source) const;
  void OpenComparePickerForPath(const std::filesystem::path& path,
                                const std::string& commit_spec);
  void OpenHeadComparison(const std::filesystem::path& path);
  // Non-git compare verbs. `CompareFiles` diffs two arbitrary paths;
  // `SelectForCompare` stashes the current side; `CompareWithSelected` diffs the
  // current side against the stash; `CompareWithClipboard` against clipboard text.
  // Return an empty string on success, or an error message to surface.
  std::string CompareFiles(const std::filesystem::path& left_path,
                           const std::filesystem::path& right_path);
  std::string SelectForCompare(ActionSource source);
  std::string CompareWithSelected(ActionSource source);
  std::string CompareWithClipboard(ActionSource source);
  void MarkBranchFileReviewed();
  void UnmarkBranchFileReviewed();
  void MarkBranchHunkReviewed();
  void UnmarkBranchHunkReviewed();
  void ClearBranchReviewState();
  void EditBranchReviewNote(std::string note_text);
  void OpenMergeEditor(const std::filesystem::path& base_path,
                       const std::filesystem::path& incoming_path,
                       const std::filesystem::path& current_path,
                       const std::filesystem::path& output_path);
  // Batch review verbs. Each switches to Source Control, opens the relevant
  // diff/merge tabs (dedup + clean stale), sets the command feedback to the
  // summary, and returns it. `ref` is any commit-ish (empty = sensible default).
  ReviewOpenOutcome ReviewConflicts();
  ReviewOpenOutcome ReviewBranch(const std::string& ref);
  ReviewOpenOutcome ReviewCommit(const std::string& ref);

  bool OpenPath(const std::filesystem::path& path, std::string* error_message);
  bool OpenPathInNewTab(const std::filesystem::path& path);
  bool OpenUntitledTab();
  std::optional<std::size_t> FindTabIndexBySpecifier(std::string_view specifier,
                                                     std::string* error_message) const;
  void ActivateTab(std::size_t index);
  bool HasOpenTabs() const;
  std::size_t OpenTabCount() const;
  std::size_t ActiveTabIndex() const;
  void MoveActiveTabTo(std::size_t index);
  void ReopenActiveTab();
  bool SaveTab(std::size_t index);
  void ResetCaretBlink();
  // Split the focused editor group in the given orientation, optionally opening
  // `path` in the new group instead of the cloned active document.
  bool SplitEditorGroup(EditorSplitOrientation orientation,
                        const std::filesystem::path& path,
                        std::string* error_message);
  bool FocusOtherGroup();
  bool CloseEditorGroup();
  void RequestCloseTab(std::size_t index);
  void RequestCloseTabs(std::vector<std::size_t> indices);
  void CloseAllTabs();

  bool ExecuteLineNavigation(const LineNavigationRequest& request, bool relative);
  void SelectAll();
  void Undo();
  void Redo();
  std::string CopySelectionText() const;
  std::optional<std::string> LastTerminalCommandText() const;
  std::optional<std::string> SelectionTextWithContext();
  void CutSelection();
  void PasteClipboard();
  // Insert literal text at the caret (replacing any selection), routed through
  // the same active-surface insertion path as PasteClipboard. Backs the
  // `type <text>` command / control-channel verb.
  void InsertText(std::string text);

  void RefreshAvailableColorschemeNames();
  void ApplyColorscheme(std::string_view name);
  std::string_view CurrentColorschemeName() const;
  void SetTabSize(std::size_t value);
  void SetIndentWidth(std::size_t value);
  float UiScale() const;
  void ApplyUiScale(float scale);
  void SetSoftTabs(bool enabled);
  bool SoftWrapEnabled() const;
  void SetSoftWrap(bool enabled);
  bool Focus(FocusRequestTarget target);
  bool PluginRuntimeEnabled() const;
  void ReloadPluginsWithFeedback();
  void RequestQuit();
  // Debugger (DAP). DebuggerEnabled reflects the `debug.enabled` master toggle.
  bool DebuggerEnabled() const;
  // Flip the `debug.enabled` master toggle and announce the new state via toast.
  void ToggleDebuggerEnabled();
  bool DebugSessionActive() const;
  bool DebugSessionStopped() const;
  void StartDebuggingWithFeedback();
  void StopDebuggingWithFeedback();
  // Execution control (Phase 3). No-op when no session / wrong state.
  void DebugContinue();
  void DebugStepOver();
  void DebugStepIn();
  void DebugStepOut();
  void DebugPause();
  void DebugReverseContinue();
  void DebugStepBack();
  void DebugRestart();
  // Multi-session switcher (Phase 8). Count of live sessions; switch to the next
  // (index < 0) or a 1-based session index.
  std::size_t DebugSessionCount() const;
  void DebugSwitchSession(int index);
  // Stop every live debug session (Phase 10).
  void StopAllDebugSessions();
  // Debug-console REPL + launch-config picker (Phase 9).
  void OpenDebugReplPrompt();
  void OpenLaunchConfigPicker();
  void OpenCommandPalette(std::string seed = {});
  // Open the single-line "Go to Line" modal prompt (VSCode-style Ctrl+G).
  void OpenGoToLinePrompt();
  void OpenRenameSymbolPrompt();
  // Right-side debug pane (toggle / surface switch).
  void ToggleDebugPane();
  void ShowDebugPaneSurface(DebugPaneMode mode);
  // Surface the active debug session's console output in the bottom panel.
  void ShowDebugOutput();
  // Breakpoint modifiers (Phase 6). Read the breakpoint-gutter context menu's
  // target line; open a prompt seeded with the current field / remove the bp.
  void EditBreakpointModifierFromMenu(ActionId id);
  void BreakpointQuickActionFromMenu(ActionId id);
  void RemoveBreakpointFromMenu();
  // Git sidebar entry context-menu handlers. Act on the selected git entry;
  // return false when there is no valid selection. The toggle picks Stage vs
  // Unstage from the selected entry's staged flag.
  bool DispatchSelectedGitSidebarAction(GitSidebarActionId action);
  bool ToggleStageSelectedGitEntry();
  // Headless breakpoint control: the project breakpoint store, a resend hook,
  // and named-launch start. Used by the breakpoint-* / debug-launch commands.
  editor::BreakpointStore& MutableBreakpointStore();
  void ResendBreakpoints(const std::filesystem::path& path);
  std::string StartNamedDebugConfig(const std::string& name);
  // Ad-hoc launch by program path (debug-run). Synthesizes a transient launch
  // config from `program` (resolved against the project root) + `args`, using
  // `type` or the sole registered adapter. Returns an error string (empty on
  // success).
  std::string StartAdHocDebug(const std::string& program, const std::vector<std::string>& args,
                              const std::string& type);
  // Function (symbol) breakpoints + exception-filter conditions (breakpoint-function-*
  // / breakpoint-exception-condition commands). Name/id-keyed; no-op when not found.
  void AddFunctionBreakpoint(const std::string& name);
  void RemoveFunctionBreakpoint(const std::string& name);
  void ToggleFunctionBreakpoint(const std::string& name);
  void SetFunctionBreakpointCondition(const std::string& name,
                                      std::optional<std::string> condition);
  void SetExceptionFilterCondition(const std::string& filter_id,
                                   std::optional<std::string> condition);

  // Editor essentials: accessors and shaping-action driver. These keep the
  // executor free of direct operations_ access while still allowing free
  // editor functions to operate on the viewport.
  editor::TextViewport* ActiveEditableViewport();
  editor::TextViewport* ActiveNavigableViewport();
  TabEntry::EditorTabState* ActiveEditorTab();
  editor::FoldingModel* EnsureActiveFoldingModelFresh();
  void NotifyEditorViewportChanged(bool last_change);
  void NotifyEditorCaretMoved();
  void ToggleEditorEssentialsCapability(ActionId id);
  std::optional<std::string> GetSettingValue(std::string_view id) const;
  // Deterministic setting write through the host SetSettingValue chokepoint.
  // Returns false when no sink is wired, the id is unknown, or the value is
  // invalid for the setting's type. Backs the `set-setting` command.
  bool SetSettingValue(std::string_view id, std::string value);
  // Post a transient host notification toast. No-op when no sink is wired.
  void Notify(NotificationService::Tone tone, std::string message);
  // Queue the host full-screen toggle (see Operations::request_toggle_fullscreen).
  // The shell only records it; Application owns the SDL window and applies the
  // request on its next ConsumeWindowAction poll.
  void ToggleWindowFullscreen();

 private:
  // Undo and Redo differ only in which viewport call they make; everything around
  // it (merge/compare bookkeeping, fold-model rescan, the redraw requests) has to
  // stay identical, so it lives here once instead of as two 55-line copies.
  void ApplyUndoRedo(bool redo);

  // Mark layout dirty and request a full-window repaint after a live config
  // change (theme, UI scale). Shared so the redraw idiom is not duplicated.
  void RequestLiveConfigRedraw();

  // Insert `text` into the currently focused surface (terminal, single-line
  // text input, or the active editable viewport) with the matching redraw
  // requests. Shared by PasteClipboard and InsertText so the insertion +
  // merge/compare-tracking + redraw idiom lives in exactly one place. Caps
  // pathologically large payloads before line-splitting/undo storage. When
  // `distribute_across_carets` is set (paste), a multi-line payload split N ways
  // is distributed one line per caret if the counts match (VSCode paste).
  void InsertTextIntoActiveSurface(std::string text, bool distribute_across_carets = false);

  // Resolves the compare side the user is acting on for a plain compare: a file
  // targeted in the tree (context menu) or the active editor buffer (its live,
  // possibly-unsaved content). Sets `*from_file` true for a file source. Returns
  // nullopt with `*error` populated when nothing resolves or the file is
  // unreadable/binary.
  std::optional<CompareInput> ResolveCurrentCompareInput(ActionSource source, bool* from_file,
                                                         std::string* error) const;

  ProjectCatalogState& project_catalog_;
  ProjectWorkspaceState& state_;
  float& ui_scale_;
  Operations operations_;
};

}  // namespace microide::workspace
