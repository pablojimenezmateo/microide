#pragma once

#include <cstddef>
#include <filesystem>
#include <functional>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "workspace/services/NotificationService.h"
#include "workspace/actions/WorkspaceActionRequests.h"
#include "workspace/CompareInput.h"
#include "workspace/actions/WorkspaceActionTypes.h"
#include "workspace/state/WorkspaceMenuState.h"
#include "workspace/state/WorkspaceProjectState.h"
#include "workspace/state/WorkspacePromptState.h"
#include "workspace/registries/WorkspaceSidebarRegistry.h"
#include "workspace/state/WorkspaceTabState.h"
#include "workspace/WorkspaceOutputChannels.h"

namespace microide::workspace {

class AssistService;
class DebugService;
class TerminalPanelService;
class EditorTabService;
class TextInputCoordinator;
class QuickPickService;
class PersistenceCoordinator;
class SidebarService;
class CompareMergeService;
class DebugPaneService;
class MenuCoordinator;
class SystemClipboard;
class WorkspacePluginRuntime;
class RemoteHostService;

// Defined in GitSidebarCommandCenter.h; only the type is needed here (function
// signatures + a parameter), so forward-declare to avoid the heavy include.
enum class GitSidebarActionId;

// What the Selected Ranges commands do to a diff's selected lines (or the change at
// the caret): stage them, unstage them, or revert them in the working tree.
enum class GitRangeVerb { Stage, Unstage, Revert };

// TD-2026-07-17A-119: shared clipboard-export byte budget. A huge explicit selection (or
// many multi-caret selections) must not allocate and duplicate a large fraction of a big
// file into both clipboards on the shell thread. Copies over this budget are truncated on
// a UTF-8 boundary with a marker; a cut over this budget is refused (never delete data
// that could not be captured).
inline constexpr std::size_t kMaxClipboardExportBytes = 64u * 1024 * 1024;  // 64 MiB

// What Save All did. An untitled buffer has no name to save under, so it is
// counted and left for its own Save As (VS Code asks for each; one prompt per
// buffer behind a single chord is what that turns into here, so it says how many).
struct SaveAllOutcome {
  std::size_t saved = 0;
  std::size_t untitled = 0;
  std::vector<std::string> failed;
};

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
    std::function<bool(ActionSource, std::string)> reject_action;
    std::function<bool(const std::filesystem::path&, bool, bool)> open_project;
    std::function<void(std::size_t)> request_close_project;
    std::function<bool(std::size_t, bool)> switch_project;
    std::function<ProjectOpenPickerResult()> open_native_project_picker;
    std::function<ProjectOpenPickerResult()> open_native_file_picker;
    std::function<void(ProjectSearchEditField)> begin_project_search_edit;
    // Move (`cut`) or copy `source` into `directory` (PathMutationCoordinator::
    // PastePath); empty on success, else why not.
    std::function<std::string(const std::filesystem::path& source, bool cut, const std::filesystem::path& directory)>
        paste_tree_path;
    std::function<void()> refresh_project_search;
    // The git view's Commit button: whether it is live, and pressing it.
    std::function<bool()> can_open_git_commit;
    std::function<bool()> open_git_commit_workflow;
    // A host-owned item view (Changed on Host, Host): IsHostOwnedItemView.
    std::function<std::optional<SDL_FRect>()> current_window_rect;
    std::function<void()> reload_clean_open_buffers_from_disk;
    // Ask git again now, whatever view is showing (Git: Refresh).
    std::function<void()> request_git_refresh;
    // Dispatch one git write action (switch/create/fetch/pull/push/publish/sync/
    // stash/stash-pop). Returns a rejection sentence on failure to start, or an
    // empty string when the operation was dispatched to the background executor.
    std::function<std::string(ActionId, const std::vector<std::string>&)> dispatch_git_operation;
    std::function<std::filesystem::path(ActionSource)> tree_mutation_base_path;
    std::function<std::filesystem::path(ActionSource)> resolve_tree_action_path;
    std::function<std::filesystem::path()> active_tab_path;
    std::function<void(PromptSurfaceState::Action,
                       PromptSurfaceState::Kind,
                       const std::filesystem::path&,
                       std::string)>
        open_prompt_surface;
    // Ask for the arguments of `command_name` (a CommandArguments prompt).
    // The input prompt for `command`'s arguments, its field holding `prefill`.
    std::function<void(std::string_view command, std::string prefill)> open_command_arguments_prompt;
    // The desktop's opener for an http(s) URL (a forwarded port's Open in Browser).
    std::function<bool(std::string_view)> open_external_url;
    std::function<bool(std::string_view)> write_primary_selection_text;
    std::function<bool(const std::filesystem::path&)> reveal_path_in_file_explorer;
    // Reveal a path in the in-app sidebar tree: force-expand ancestors, select it,
    // and scroll it into view. Returns whether a matching row was found.
    std::function<bool(const std::filesystem::path&)> reveal_path_in_tree;
    // Opens/refocuses the terminal find bar; false when no terminal is showing.
    std::function<bool(std::string)> open_terminal_find;
    std::function<void(OverlayMode)> show_overlay;
    std::function<void()> dismiss_overlay;
    std::function<void()> open_settings_overlay;
    std::function<void()> open_help_about_overlay;
    std::function<void()> toggle_status_bar;
    std::function<void()> toggle_layout_mode;
    std::function<void()> open_buffer_search;
    std::function<void()> refresh_buffer_search;
    std::function<void()> open_buffer_replace;
    std::function<bool(std::string*)> find_lsp_references;
    // `incoming` selects callers-of vs called-by; both render into the same channel.
    std::function<bool(bool, std::string*)> show_call_hierarchy;
    // Change Language Mode: the pick (empty) or a language to set ("auto" detects
    // again); false, with why, for a language no definition has.
    std::function<bool(const std::string& language, std::string* error)> change_language_mode;
    std::function<std::string()> symbol_at_cursor;
    // Refine a just-opened Rename Symbol prompt from the server's prepareRename
    // (prefill the placeholder / reject non-renameable positions). Best-effort.
    std::function<void(std::string)> refine_rename_prompt;
    std::function<bool(std::string*)> discover_tests_for_active_buffer;
    std::function<bool(const std::vector<std::string>&, std::string*)> run_tests;
    std::function<bool(std::string*)> run_all_discovered_tests;
    // Non-git ("plain") comparison of two arbitrary sides (file/buffer/clipboard).
    // Batch review sessions: switch to Source Control and open diff/merge tabs,
    // deduping + cleaning stale review tabs. Return a summary outcome.
    std::function<ReviewOpenOutcome()> open_conflict_review;
    std::function<ReviewOpenOutcome(const std::string&)> open_branch_review;
    std::function<ReviewOpenOutcome(const std::string&)> open_commit_review;
    std::function<editor::FoldingModel*()> ensure_active_folding_model_fresh;
    std::function<bool(const editor::TextViewport&)> replace_active_editor_view;
    // A view of the path for a new pane: shares the live buffer when the file is
    // open anywhere, else reads disk (TabCoordinator::OpenEditorViewForPath).
    std::function<bool(const std::filesystem::path&, editor::TextViewport&)>
        open_editor_view_for_path;
    std::function<void(const std::filesystem::path&)> open_file;
    std::function<bool(const std::filesystem::path&)> open_file_in_new_tab;
    std::function<void(std::size_t)> activate_tab;
    std::function<bool(std::size_t)> move_active_tab_to;
    std::function<bool(std::size_t)> save_tab;
    // The interactive save. Same tab, same contract, except the formatter runs off
    // the shell thread and the write lands when it returns.
    std::function<bool(std::size_t)> save_tab_deferred;
    // Revert File on the active editor's file: false when it cannot be read.
    std::function<bool()> revert_active_file;
    // Save All: every dirty editor in every group, deferred as one Ctrl+S is.
    std::function<SaveAllOutcome()> save_all_dirty_tabs;
    std::function<bool(std::size_t, const std::filesystem::path&, std::string*)> save_tab_as;
    std::function<void()> reset_caret_blink;
    // VS Code's focusLeft/Right/Above/BelowGroup and moveActiveEditorGroup*.
    // Go Back (-1) / Go Forward (+1): the place to reveal, if there is one.
    std::function<bool()> close_editor_group;
    std::function<void(std::size_t)> request_close_tab;
    std::function<void(std::vector<std::size_t>)> request_close_tabs;
    std::function<void()> close_all_tabs;
    std::function<editor::TextViewport*()> active_navigable_viewport;
    // Move tab `from_index` of group `from_group` into group `to_group` at
    // `to_slot` (EditorTabService::MoveTabToGroup).
    std::function<bool(std::size_t, std::size_t, std::size_t, std::size_t)> move_tab_to_group;
    std::function<void()> request_focused_editor_redraw;
    std::function<editor::TextViewport*()> active_editable_viewport;
    std::function<CompareTabState*()> active_compare_tab;
    std::function<void()> mark_branch_file_reviewed;
    std::function<void()> unmark_branch_file_reviewed;
    std::function<void()> mark_branch_hunk_reviewed;
    std::function<bool(ActionId, std::string*)> run_compare_command;
    std::function<void()> unmark_branch_hunk_reviewed;
    std::function<void()> clear_branch_review_state;
    std::function<void(std::string)> edit_branch_review_note;
    // Stage, unstage or revert (after a preview prompt) the active diff's selected
    // lines, else the change at the caret. False when no diff is active.
    std::function<bool(GitRangeVerb)> apply_git_selected_ranges;
    std::function<void(CompareTabState&)> refresh_compare_tab_derived_state;
    std::function<void(CompareTabState&, bool)> sync_compare_selection_from_viewport;
    std::function<MergeTabState*()> active_merge_tab;
    std::function<void(MergeTabState&)> update_merge_tracking_after_viewport_edit;
    std::function<void(bool)> request_active_tab_redraw;
    std::function<void()> request_active_editable_last_change_redraw;
    std::function<void(const std::vector<std::string>&, const std::vector<std::string>&)>
        request_active_editable_change_redraw;
    std::function<void(std::size_t, std::size_t)> request_active_editable_blame_neighborhood_redraw;
    std::function<bool()> terminal_has_selection;
    std::function<std::string()> selected_terminal_text;
    std::function<std::optional<std::string>()> last_terminal_command_text;
    std::function<std::optional<std::string>()> selection_text_with_context;
    // A key press (down then up) delivered through the window's event path, as
    // the keyboard would: shortcuts, the focused surface, the terminal's encoder.
    std::function<void(SDL_Keycode, SDL_Keymod)> press_key;
    // A mouse event (layout coordinates) delivered the same way, with `modifiers`
    // held for it: what the shell reads from InteractionState::key_modifiers for a click.
    std::function<void(const SDL_Event&, SDL_Keymod)> inject_pointer_event;
    // Show Hover: pin the editor hover to the caret. False when there is no
    // editor or the caret is off screen.
    std::function<bool()> show_hover_at_caret;
    // False when no theme of that name exists (built-in, on disk, or plugin).
    std::function<void()> apply_editor_preferences_to_all_tabs;
    std::function<std::optional<std::string>(std::string_view)> get_setting_value;
    std::function<bool(std::string_view, std::string)> set_setting_value;
    // Post a transient host notification toast (and schedule a redraw).
    std::function<void(NotificationService::Tone, std::string)> notify;
    std::function<void()> note_layout_inputs_changed;
    std::function<void()> request_window_redraw;
    // Focus the notification stack; false when it is empty.
    // Resolve and take a notification row's action (see NotificationService::
    // TakeAction); nullopt with *error naming what did not resolve.
    // Press the button labelled so on the active editor tab's banner; false with
    // *error when the tab shows no banner or the banner has no such button.
    std::function<bool(std::string_view, std::string*)> activate_editor_banner;
    // Show an Output channel by id; false when no such channel exists.
    std::function<bool(std::string_view)> show_output_channel;
    // The remote-host service (created on first use).
    std::function<RemoteHostService&()> remote_hosts;
    // Cancel a quit / close-project waiting on formatter saves; false when no
    // wait with that id exists (it already ran or was cancelled).
    std::function<bool(std::uint64_t)> cancel_save_wait;
    // Queue the host-level full-screen toggle. The window itself is owned by
    // Application, which polls WorkspaceShell::ConsumeWindowAction each frame;
    // the shell only records the request.
    std::function<void()> request_toggle_fullscreen;
    // `announce`: report the completed reload's summary (see PluginReloadRequest).
    std::function<void(bool announce)> reload_plugins_for_current_project;
    std::function<void()> request_quit;
    // Debugger (DAP). start_debugging builds a default launch config from the
    // current project's registered adapters; it returns an error string (empty
    // on success). has_debug_adapters reports whether any adapter is registered.
    std::function<std::string()> start_debugging;
    std::function<void()> stop_debugging;
    // True when the active session is paused (`stopped`); gates continue/step
    // (require stopped) vs. pause (requires running). Execution-control verbs
    // forward to the active session (no-op when none / wrong state).
    // Reverse execution. Forward to the active session (no-op when none / wrong
    // state / adapter lacks `supportsStepBack`). debug_supports_reverse reports the
    // capability so availability can hide the verbs for non-recording adapters.
    // Multi-session switcher (Phase 8). debug_session_count reports how many live
    // sessions exist (gates the switch command); debug_switch_session cycles to the
    // next session (index < 0) or selects a 1-based index.
    // Debug-console REPL (Phase 9): open the single-line prompt that evaluates an
    // expression in the active session and appends the result to the console.
    std::function<void()> open_debug_repl_prompt;
    // Launch-config picker (Phase 9): open the fuzzy picker over the project's
    // launch configs.
    // Show a pick whose rows run command lines (QuickPickKind::RunCommand).
    std::function<void(std::string query)> open_document_symbol_picker;
    // Open the command palette over all built-in + plugin commands, optionally pre-filling
    // the query (e.g. "project-open ") so the user only types the argument.
    std::function<void(std::string)> open_command_palette;
    // Breakpoint-modifier context-menu handlers (Phase 6); read the gutter
    // menu's target line on the shell side.
    std::function<void(ActionId)> edit_breakpoint_modifier_from_menu;
    std::function<void(ActionId)> breakpoint_quick_action_from_menu;
    std::function<void()> remove_breakpoint_from_menu;
    // Surface the active debug session's console output in the bottom panel.
    std::function<void()> show_debug_output;
    // Add a watch expression directly (no prompt) — the "Add to Watch" row menu
    // item, which takes the expression from the row it was invoked on.
    // Start a debug session for a named launch config (empty name → the
    // selected/default config). Returns an error string (empty on success).
    std::function<std::string(const std::string&)> start_named_debug_config;
    std::function<std::string(const std::string&, const std::vector<std::string>&,
                              const std::string&)>
        start_ad_hoc_debug;
    // Function (symbol) breakpoints + exception-filter conditions (control channel
    // + command line). Each mutates the per-project store + live re-sends.
    // Buffer-search stepping for F3 / Shift+F3 (StepBufferSearch): move the
    // selected match by a delta (wrapping) and reveal one match in the editor.
    std::function<void(int)> move_buffer_search_selection;
    std::function<void(const editor::SelectionRange&)> reveal_buffer_search_match;
  };

  // The services an action calls directly: an action whose whole job is one
  // call on one of these needs no Operations hop through the shell (TD-412).
  struct Services {
    AssistService& assist;
    DebugService& debug;
    TerminalPanelService& terminal_panel;
    EditorTabService& editor_tabs;
    TextInputCoordinator& text_input;
    QuickPickService& quick_pick;
    NotificationService& notifications;
    PersistenceCoordinator& persistence;
    const WorkspaceOutputChannels& output_channels;
    SidebarService& sidebar;
    CompareMergeService& compare_merge;
    DebugPaneService& debug_pane;
    MenuCoordinator& menus;
    SystemClipboard& clipboard;
    WorkspacePluginRuntime& plugins;
  };

  WorkspaceActionContext(ProjectCatalogState& project_catalog,
                         ProjectWorkspaceState& current_project_state,
                         float& ui_scale,
                         Services services,
                         Operations operations);

  AssistService& assist() { return services_.assist; }
  DebugService& debug() { return services_.debug; }
  TerminalPanelService& terminal_panel() { return services_.terminal_panel; }
  EditorTabService& editor_tabs() { return services_.editor_tabs; }

  void PrepareForAction(ActionSource source);
  bool RejectAction(ActionSource source, std::string feedback);
  // The command surfaces' answer for an action that ran (the control channel's
  // `feedback`), with no toast: the UI shows the outcome itself.
  void SetCommandFeedback(std::string feedback) { state_.panel.feedback.text = std::move(feedback); }

  // The receipt for the last action dispatched through ActionCoordinator: what a
  // key, menu row, palette entry or command actually ran. The `ui` control query
  // reports it, so a driver that pressed Ctrl+B can check sidebar-toggle ran --
  // and not a binding from another context, or nothing.
  struct ActionReceipt {
    std::uint64_t dispatched = 0;
    std::optional<ActionId> last;
    // A plugin command (no ActionId) when that is what ran: a surface's hit
    // region, a lens, a plugin's palette entry or key.
    std::string plugin_command;
    ActionSource source = ActionSource::Command;
    bool rejected = false;
    // How many times each action ran since startup, indexed by ActionId (grown
    // on first use; NoteActionRan), and the same per plugin command. The `action-counts`
    // control query reports them: tools/qa unions every scenario's counts into
    // the suite's command-coverage ledger, so a command no flow ever drove is a
    // listed gap rather than a guess.
    std::vector<std::uint32_t> counts;
    std::map<std::string, std::uint32_t, std::less<>> plugin_counts;
  };
  void NoteActionDispatched(ActionId id, ActionSource source) {
    ++receipt_.dispatched;
    receipt_.last = id;
    receipt_.plugin_command.clear();
    receipt_.source = source;
    receipt_.rejected = false;
  }
  // The action went on to run (past the QA dry run, which records a receipt and
  // runs nothing): `counts` measures what executed, not what was pressed.
  void NoteActionRan(ActionId id) {
    const auto index = static_cast<std::size_t>(id);
    if (receipt_.counts.size() <= index) {
      receipt_.counts.resize(index + 1, 0);
    }
    ++receipt_.counts[index];
  }
  void NotePluginCommandDispatched(std::string_view command, ActionSource source) {
    ++receipt_.dispatched;
    receipt_.last.reset();
    receipt_.plugin_command.assign(command);
    receipt_.source = source;
    receipt_.rejected = false;
    const auto found = receipt_.plugin_counts.find(command);
    if (found == receipt_.plugin_counts.end()) {
      receipt_.plugin_counts.emplace(std::string(command), 1);
    } else {
      ++found->second;
    }
  }
  void NoteActionRejected() { receipt_.rejected = true; }
  const ActionReceipt& LastActionReceipt() const { return receipt_; }

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
  void RequestGitRefresh();
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
  // The tree's Cut / Copy: holds `path` for Paste. False for no path or the root.
  bool SetTreeClipboard(const std::filesystem::path& path, bool cut);
  // The tree's Paste into `directory`: what was cut moves there, what was copied
  // is copied there. Empty on success, else why not.
  std::string PasteTreeClipboard(const std::filesystem::path& directory);
  void OpenDeletePathPrompt(const std::filesystem::path& path);
  bool WriteClipboardText(std::string_view text) const;
  bool OpenExternalUrl(std::string_view url) const;
  bool WritePrimarySelectionText(std::string_view text) const;
  bool RevealPathInFileExplorer(const std::filesystem::path& directory) const;
  bool RevealPathInTree(const std::filesystem::path& path) const;

  bool HasActiveTerminalPane() const;
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
  // Replace in Files: the search view with `query` (unless empty), the keyboard
  // in its replace field.
  void ShowProjectReplace(std::string query);
  // The keyboard into one of the search view's fields.
  void BeginProjectSearchEdit(ProjectSearchEditField field);
  // Search: Refresh / Toggle Exact Match Count / Toggle Search Details, on the
  // search view (shown first).
  void ApplyProjectSearchViewCommand(ActionId id);
  // Git: Commit…: the commit message editor, or why it cannot open.
  std::string OpenGitCommitWorkflow();
  bool FindLspReferences(std::string* error_message);
  // Search Workspace Symbols: a quick pick the language server filters as the
  // query is typed, seeded with `query`.
  void OpenWorkspaceSymbolPicker(std::string query);
  bool ChangeLanguageMode(const std::string& language, std::string* error);
  bool ShowCallHierarchy(bool incoming, std::string* error_message);
  bool DiscoverTestsForActiveBuffer(std::string* error_message);
  bool RunTests(const std::vector<std::string>& test_ids, std::string* error_message);
  bool RunAllDiscoveredTests(std::string* error_message);
  bool ActiveTabIsCompare() const;
  // A diff opened from the git view's Outgoing list: the only tabs the branch-review
  // marks and notes apply to.
  bool ActiveTabIsBranchReview() const;
  bool ActiveTabIsMerge() const;
  void OpenBufferSearch(std::string query);
  // F3 / Shift+F3: step to the next/previous match of the current find term,
  // whether or not the widget is open (VS Code). With no term yet it opens the
  // widget; with the widget closed it re-runs the search first.
  void StepBufferSearch(int delta);
  // Alt+F8 / Shift+Alt+F8: move the caret to the next (delta > 0) or previous
  // diagnostic of the active file, wrapping at either end, and toast its message.
  // Returns false (with `error_message`) when there is no file or no diagnostic.
  bool StepDiagnostic(int delta, std::string* error_message);
  // The active editor's selection when it is non-empty and on one line, else "".
  std::string SingleLineSelectionText();
  // F8 / Shift+F8 (VS Code's marker.nextInFiles): the problem after (delta > 0) or
  // before the caret in every file with problems, files in path order, wrapping
  // at the ends. nullopt when there are none; the caller reveals it.
  std::optional<editor::PublishedDiagnostic> ProblemInFiles(int delta);
  // Toasts a diagnostic's message in its severity's tone.
  void NotifyDiagnostic(const editor::PublishedDiagnostic& diagnostic);
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
  // One of the active diff's own verbs (ActionId::Compare*: whitespace toggles,
  // copy as patch / path, the review's next and previous file). False, with the
  // reason, when no diff is active or the verb has nothing to act on.
  bool RunCompareCommand(ActionId id, std::string* error);
  void UnmarkBranchHunkReviewed();
  void ClearBranchReviewState();
  void EditBranchReviewNote(std::string note_text);
  bool ApplyGitSelectedRanges(GitRangeVerb verb);
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
  // False when there is nothing to reload: no editor tab, no file on disk, or
  // unsaved changes the reload would throw away.
  bool ReopenActiveTab();
  // The newest recently closed editor whose file still exists, removed from the
  // list (entries for files gone since are dropped on the way); nullopt for none.
  std::optional<DeferredTabHandle> TakeRecentlyClosedEditor();
  bool SaveTab(std::size_t index);
  bool SaveTabDeferred(std::size_t index);
  SaveAllOutcome SaveAllDirtyTabs();
  bool RevertActiveFile();
  // Save As / naming an untitled buffer. `error` says why on failure.
  bool SaveTabAs(std::size_t index, const std::filesystem::path& path, std::string* error);
  // The Save As prompt (Ctrl+S on an untitled buffer).
  // `initial` prefills the path (a named file's own, for Save As on it).
  void OpenSaveAsPrompt(std::string initial = {});
  void ResetCaretBlink();
  // Split the focused editor group in the given orientation, optionally opening
  // `path` in the new group instead of the cloned active document.
  bool SplitEditorGroup(EditorSplitOrientation orientation,
                        const std::filesystem::path& path,
                        std::string* error_message);
  bool FocusOtherGroup();
  std::size_t EditorGroupCount() const { return state_.editor_groups.size(); }
  bool CloseEditorGroup();
  void RequestCloseTab(std::size_t index);
  void RequestCloseTabs(std::vector<std::size_t> indices);
  void CloseAllTabs();

  bool ExecuteLineNavigation(const LineNavigationRequest& request, bool relative);
  // Open `path` and put the caret on the requested line, centred. A file still
  // loading off-thread records the position so the load lands on it. False with
  // `error_message` set when the open fails.
  bool RevealPathAtLine(const std::filesystem::path& path,
                        const LineNavigationRequest& request,
                        std::string* error_message);
  // Move the focused pane's active tab into group `to_group` at `to_slot`
  // (clamped to the end). False when there is no such group or no tab.
  bool MoveActiveTabToGroup(std::size_t to_group, std::optional<std::size_t> to_slot);
  void SelectAll();
  void Undo();
  void Redo();
  // `*whole_line` (optional) is set when the text is the caret's whole line,
  // copied because nothing was selected -- the case a later paste treats as a
  // line paste (see ProjectCatalogState::line_clipboard_text).
  std::string CopySelectionText(bool* whole_line = nullptr) const;
  // Record / clear the empty-selection line copy that a paste keys on.
  void RememberLineClipboardCopy(std::string_view text, bool whole_line);
  std::optional<std::string> LastTerminalCommandText() const;
  std::optional<std::string> SelectionTextWithContext();
  void CutSelection();
  void PasteClipboard();
  // Insert literal text at the caret (replacing any selection), routed through
  // the same active-surface insertion path as PasteClipboard. Backs the
  // `type <text>` command / control-channel verb.
  void InsertText(std::string text);
  void PressKey(SDL_Keycode key, SDL_Keymod modifiers);
  void InjectPointerEvent(const SDL_Event& event, SDL_Keymod modifiers);
  bool ShowHoverAtCaret();

  void RefreshAvailableColorschemeNames();
  // Says which colour schemes `colorscheme <name>` takes (feedback and a toast).
  void ReportColorschemeNames();
  bool ApplyColorscheme(std::string_view name);
  std::string_view CurrentColorschemeName() const;
  // Records an indentation command's value as the active buffer's own (see
  // TextViewport::own_indent); the command also sets the setting.
  void SetActiveBufferOwnIndent(std::optional<bool> soft_tabs, std::optional<std::size_t> width);
  void SetTabSize(std::size_t value);
  void SetIndentWidth(std::size_t value);
  float UiScale() const;
  void ApplyUiScale(float scale);
  bool SoftTabsEnabled() const;
  void SetSoftTabs(bool enabled);
  bool SoftWrapEnabled() const;
  void SetSoftWrap(bool enabled);
  bool Focus(FocusRequestTarget target);
  // F6 / Shift+F6: the keyboard to the next (previous) part on screen.
  void CycleSurfaceFocus(bool backward);
  bool PluginRuntimeEnabled() const;
  void ReloadPluginsWithFeedback();
  void RequestQuit();
  // Debugger (DAP). DebuggerEnabled reflects the `debug.enabled` master toggle.
  bool DebuggerEnabled() const;
  // Flip the `debug.enabled` master toggle and announce the new state via toast.
  void ToggleDebuggerEnabled();
  void StartDebuggingWithFeedback();
  void StopDebuggingWithFeedback();
  // Execution control (Phase 3). No-op when no session / wrong state.
  // The active session's adapter advertises `supportsStepBack` (a recording one).
  // Stop every live debug session (Phase 10).
  void StopAllDebugSessions();
  // Debug-console REPL + launch-config picker (Phase 9).
  void OpenDebugReplPrompt();
  void OpenLaunchConfigPicker();
  // Color Theme…: a quick pick of the schemes, previewing each as it is selected.
  void OpenColorThemePicker();
  // Go to Symbol in Editor: a quick pick of the active file's symbols, seeded with
  // `query`, previewing each as it is selected.
  void OpenDocumentSymbolPicker(std::string query);
  void OpenCommandPalette(std::string seed = {});
  // Open the single-line "Go to Line" modal prompt (VSCode-style Ctrl+G).
  void OpenGoToLinePrompt();
  // Ask for the arguments a command cannot run without (its usage's `<...>`).
  void OpenCommandArgumentsPrompt(std::string_view command_name, std::string prefill = {});
  // A command that needs an argument, run bare from a menu, the palette or a key:
  // the choices it has, where it has a set of them (TD-2026-10-18-357) -- a pick
  // of the output channels, of the other panes, the file finder to compare the
  // active file with. False when it has none to offer (the input prompt asks).
  bool OpenArgumentPicker(ActionId id);
  // A command whose OPTIONAL argument it would otherwise guess, run bare from a
  // menu, the palette or a key: the choices, when there is more than one (a host
  // verb with several hosts open). False when the bare command is unambiguous and
  // runs as it is.
  bool OpenOptionalArgumentPicker(ActionId id);
  // The open remote hosts as a pick titled `title`, each row running
  // `command_line <host>`. False with fewer than two hosts (nothing to ask).
  bool OpenHostPick(std::string_view title, std::string_view command_line);
  // Browse Folder…: `folder`'s entries as a pick -- its subfolders browse on,
  // its files open (the active file's folder when empty). Empty on success,
  // else why not.
  std::string OpenFolderPick(const std::filesystem::path& folder);
  // Change End of Line Sequence…: LF / CRLF for the active file, as a pick; and
  // the change itself (an undoable edit). False without an editable file.
  bool OpenLineEndingPick();
  bool SetActiveLineEnding(util::LineEnding ending);
  // Indentation…: spaces or tabs, and the tab size, as a pick.
  bool OpenIndentationPick();
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
  // The selected git sidebar entry's absolute path when it is a conflict, else "".
  std::string SelectedConflictedGitEntryPath() const;
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

  // Editor essentials: accessors and shaping-action driver. These keep the
  // executor free of direct operations_ access while still allowing free
  // editor functions to operate on the viewport.
  editor::TextViewport* ActiveEditableViewport();
  editor::TextViewport* ActiveNavigableViewport();
  TabEntry::EditorTabState* ActiveEditorTab();
  editor::FoldingModel* EnsureActiveFoldingModelFresh();
  // Rebuild the active compare tab's diff model after its right pane was edited,
  // and re-anchor the surface's selection on the caret. Idempotent (the rebuild is
  // fingerprint-guarded) and a no-op when the active tab is not a compare tab.
  void RefreshActiveCompareAfterViewportEdit();
  // After a cut or paste in the active editable viewport: the compare and merge
  // panes that mirror it, the caret, the repaint, and the blame band when the
  // edit flipped the buffer's dirty state.
  void FinishClipboardEdit(const editor::TextViewport& viewport, bool was_dirty,
                           std::size_t cursor_before_line);
  void NotifyEditorViewportChanged(bool last_change);
  // Re-track the merge result pane's conflict spans after an edit to it. Reads the
  // viewport's own last-applied-edit span, so the caller captures nothing.
  void RetrackActiveMergeAfterViewportEdit();
  void NotifyEditorCaretMoved();
  void ToggleEditorEssentialsCapability(ActionId id);
  std::optional<std::string> GetSettingValue(std::string_view id) const;
  // Deterministic setting write through the host SetSettingValue chokepoint.
  // Returns false when no sink is wired, the id is unknown, or the value is
  // invalid for the setting's type. Backs the `set-setting` command.
  bool SetSettingValue(std::string_view id, std::string value);
  // Post a transient host notification toast. No-op when no sink is wired.
  void Notify(NotificationService::Tone tone, std::string message);
  // Focus the notification stack (see Operations::focus_notifications).
  bool FocusNotifications();
  bool ShowOutputChannel(std::string_view channel_id);
  // Null only in a context built without the shell (tests).
  RemoteHostService* RemoteHosts();
  // Remote: Open Folder on Host…'s `[user@]host[:port]:/path` prompt.
  void OpenRemoteHostPrompt();
  bool CancelSaveWait(std::uint64_t id);
  std::optional<NotificationAction> TakeNotificationAction(std::string_view row,
                                                           std::string_view action,
                                                           std::string* error);
  // Press the active tab's banner button labelled `label` (case-insensitive).
  bool ActivateEditorBanner(std::string_view label, std::string* error);
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
  Services services_;
  Operations operations_;
  ActionReceipt receipt_;
};

}  // namespace microide::workspace
