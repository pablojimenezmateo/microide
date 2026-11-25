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

#include "project/remote/FileVersionHistory.h"
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
struct TerminalAlertTarget;
class TabCoordinator;
using EditorTabService = TabCoordinator;
class TextInputCoordinator;
class QuickPickService;
class PersistenceCoordinator;
class SidebarCoordinator;
using SidebarService = SidebarCoordinator;
class CompareMergeService;
class CompareReviewService;
class DebugPaneService;
class MenuCoordinator;
class SystemClipboard;
class WorkspacePluginRuntime;
class RemoteHostService;

enum class GitSidebarActionId;

enum class GitRangeVerb { Stage, Unstage, Revert };

inline constexpr std::size_t kMaxClipboardExportBytes = 64u * 1024 * 1024;

struct SaveAllOutcome {
  std::size_t saved = 0;
  std::size_t untitled = 0;
  std::vector<std::string> failed;
};

struct ClipboardExportResult {
  std::string text;
  bool truncated = false;
};

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
    std::function<std::string(const std::filesystem::path& source, bool cut, const std::filesystem::path& directory)>
        paste_tree_path;
    std::function<void()> refresh_project_search;
    std::function<bool()> can_open_git_commit;
    std::function<bool()> open_git_commit_workflow;
    std::function<std::optional<SDL_FRect>()> current_window_rect;
    std::function<void()> reload_clean_open_buffers_from_disk;
    std::function<void()> request_git_refresh;
    std::function<std::string(ActionId, const std::vector<std::string>&)> dispatch_git_operation;
    std::function<std::filesystem::path(ActionSource)> tree_mutation_base_path;
    std::function<std::filesystem::path(ActionSource)> resolve_tree_action_path;
    std::function<std::filesystem::path()> active_tab_path;
    std::function<void(PromptSurfaceState::Action,
                       PromptSurfaceState::Kind,
                       const std::filesystem::path&,
                       std::string)>
        open_prompt_surface;
    std::function<void(std::string_view command, std::string prefill)> open_command_arguments_prompt;
    std::function<bool(std::string_view)> open_external_url;
    std::function<bool(std::string_view)> write_primary_selection_text;
    std::function<bool(const std::filesystem::path&)> reveal_path_in_file_explorer;
    std::function<bool(const std::filesystem::path&)> reveal_path_in_tree;
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
    std::function<bool(bool, std::string*)> show_call_hierarchy;
    std::function<bool(const std::string& language, std::string* error)> change_language_mode;
    std::function<void(std::string)> refine_rename_prompt;
    std::function<bool(std::string*)> discover_tests_for_active_buffer;
    std::function<bool(const std::vector<std::string>&, std::string*)> run_tests;
    std::function<bool(std::string*)> run_all_discovered_tests;
    std::function<void(std::function<void()> gather, std::function<void()> apply)> run_off_shell_thread;
    std::function<ReviewOpenOutcome()> open_conflict_review;
    std::function<ReviewOpenOutcome(const std::string&)> open_branch_review;
    std::function<ReviewOpenOutcome(const std::string&)> open_commit_review;
    std::function<ReviewOpenOutcome()> open_checkpoint_review;
    std::function<ReviewOpenOutcome()> mark_review_checkpoint;
    std::function<editor::FoldingModel*()> ensure_active_folding_model_fresh;
    std::function<bool(const editor::TextViewport&)> replace_active_editor_view;
    std::function<bool(const std::filesystem::path&, editor::TextViewport&)>
        open_editor_view_for_path;
    std::function<void(const std::filesystem::path&)> open_file;
    std::function<bool(const std::filesystem::path&)> open_file_in_new_tab;
    std::function<void(std::size_t)> activate_tab;
    std::function<bool(std::size_t)> move_active_tab_to;
    std::function<bool(std::size_t)> save_tab;
    std::function<bool(std::size_t)> save_tab_deferred;
    std::function<bool()> revert_active_file;
    std::function<SaveAllOutcome()> save_all_dirty_tabs;
    std::function<bool(std::size_t, const std::filesystem::path&, std::string*)> save_tab_as;
    std::function<void()> reset_caret_blink;
    std::function<bool()> close_editor_group;
    std::function<void(std::size_t)> request_close_tab;
    std::function<void(std::vector<std::size_t>)> request_close_tabs;
    std::function<void()> close_all_tabs;
    std::function<editor::TextViewport*()> active_navigable_viewport;
    std::function<bool(std::size_t, std::size_t, std::size_t, std::size_t)> move_tab_to_group;
    std::function<void()> request_frame;
    std::function<editor::TextViewport*()> active_editable_viewport;
    std::function<CompareTabState*()> active_compare_tab;
    std::function<bool(ActionId, std::string*)> run_compare_command;
    std::function<bool(GitRangeVerb)> apply_git_selected_ranges;
    std::function<void(CompareTabState&)> refresh_compare_tab_derived_state;
    std::function<void(CompareTabState&, bool)> sync_compare_selection_from_viewport;
    std::function<MergeTabState*()> active_merge_tab;
    std::function<void(MergeTabState&)> update_merge_tracking_after_viewport_edit;
    std::function<void()> note_active_editable_edit;
    std::function<void(const std::vector<std::string>&, const std::vector<std::string>&)>
        request_active_editable_change_redraw;
    std::function<std::optional<std::string>()> selection_text_with_context;
    std::function<void(SDL_Keycode, SDL_Keymod)> press_key;
    std::function<void(const SDL_Event&, SDL_Keymod)> inject_pointer_event;
    std::function<bool()> show_hover_at_caret;
    std::function<void()> apply_editor_preferences_to_all_tabs;
    std::function<std::optional<std::string>(std::string_view)> get_setting_value;
    std::function<bool(std::string_view, std::string)> set_setting_value;
    std::function<void(NotificationService::Tone, std::string)> notify;
    std::function<void()> note_layout_inputs_changed;
    std::function<void()> request_window_redraw;
    std::function<bool(std::string_view, std::string*)> activate_editor_banner;
    std::function<bool(std::string_view)> show_output_channel;
    std::function<RemoteHostService&()> remote_hosts;
    std::function<bool(std::uint64_t)> cancel_save_wait;
    std::function<void()> request_toggle_fullscreen;
    std::function<void(bool announce)> reload_plugins_for_current_project;
    std::function<void()> request_quit;
    std::function<std::string()> start_debugging;
    std::function<void()> stop_debugging;
    std::function<void()> open_debug_repl_prompt;
    std::function<void(std::string query)> open_document_symbol_picker;
    std::function<void(std::string)> open_command_palette;
    std::function<void(ActionId)> edit_breakpoint_modifier_from_menu;
    std::function<void(ActionId)> breakpoint_quick_action_from_menu;
    std::function<void()> remove_breakpoint_from_menu;
    std::function<void()> show_debug_output;
    std::function<std::string(const std::string&)> start_named_debug_config;
    std::function<std::string(const std::string&, const std::vector<std::string>&,
                              const std::string&)>
        start_ad_hoc_debug;
    std::function<void(int)> move_buffer_search_selection;
    std::function<void(const editor::SelectionRange&)> reveal_buffer_search_match;
  };

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
    CompareReviewService& compare_review;
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
  void SetCommandFeedback(std::string feedback) { state_.panel.feedback.text = std::move(feedback); }

  struct ActionReceipt {
    std::uint64_t dispatched = 0;
    std::optional<ActionId> last;
    std::string plugin_command;
    ActionSource source = ActionSource::Command;
    bool rejected = false;
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
  std::optional<TerminalAlertTarget> NextTerminalAlert() const;
  std::filesystem::path ProjectRoot() const;
  bool OpenProject(const std::filesystem::path& project_root,
                   bool restore_persistence,
                   bool log_feedback);
  void RequestCloseProject(std::size_t index);
  bool SwitchProject(std::size_t index, bool log_feedback);
  ProjectOpenPickerResult OpenNativeProjectPicker();
  ProjectOpenPickerResult OpenNativeFilePicker();

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
  bool SetTreeClipboard(const std::filesystem::path& path, bool cut);
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
  void ShowProjectReplace(std::string query);
  void BeginProjectSearchEdit(ProjectSearchEditField field);
  void ApplyProjectSearchViewCommand(ActionId id);
  std::string OpenGitCommitWorkflow();
  bool FindLspReferences(std::string* error_message);
  void OpenWorkspaceSymbolPicker(std::string query);
  bool ChangeLanguageMode(const std::string& language, std::string* error);
  bool ShowCallHierarchy(bool incoming, std::string* error_message);
  bool DiscoverTestsForActiveBuffer(std::string* error_message);
  bool RunTests(const std::vector<std::string>& test_ids, std::string* error_message);
  bool RunAllDiscoveredTests(std::string* error_message);
  bool ActiveTabIsCompare() const;
  bool ActiveTabIsBranchReview() const;
  std::string ReviewComments(const std::filesystem::path& scope) const;
  std::filesystem::path ReviewCommentMenuScope() const;
  bool ActiveTabIsMerge() const;
  void OpenBufferSearch(std::string query);
  void StepBufferSearch(int delta);
  bool StepDiagnostic(int delta, std::string* error_message);
  std::string SingleLineSelectionText();
  std::optional<editor::PublishedDiagnostic> ProblemInFiles(int delta);
  void NotifyDiagnostic(const editor::PublishedDiagnostic& diagnostic);
  void OpenBufferReplace();
  std::filesystem::path ResolveComparePath(const std::filesystem::path& requested_path,
                                           ActionSource source) const;
  void OpenComparePickerForPath(const std::filesystem::path& path,
                                const std::string& commit_spec);
  void OpenHeadComparison(const std::filesystem::path& path);
  std::string CompareFiles(const std::filesystem::path& left_path,
                           const std::filesystem::path& right_path);
  std::string SelectForCompare(ActionSource source);
  std::string CompareWithSelected(ActionSource source);
  std::string CompareWithClipboard(ActionSource source);
  void MarkBranchFileReviewed();
  void UnmarkBranchFileReviewed();
  void MarkBranchHunkReviewed();
  bool RunCompareCommand(ActionId id, std::string* error);
  void UnmarkBranchHunkReviewed();
  void ClearBranchReviewState();
  void EditBranchReviewNote(std::string note_text);
  bool ApplyGitSelectedRanges(GitRangeVerb verb);
  void OpenMergeEditor(const std::filesystem::path& base_path,
                       const std::filesystem::path& incoming_path,
                       const std::filesystem::path& current_path,
                       const std::filesystem::path& output_path);
  ReviewOpenOutcome ReviewConflicts();
  ReviewOpenOutcome ReviewBranch(const std::string& ref);
  ReviewOpenOutcome ReviewCommit(const std::string& ref);
  ReviewOpenOutcome ReviewCheckpoint();
  ReviewOpenOutcome MarkReviewCheckpoint();

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
  bool ReopenActiveTab();
  std::optional<DeferredTabHandle> TakeRecentlyClosedEditor();
  bool SaveTabDeferred(std::size_t index);
  SaveAllOutcome SaveAllDirtyTabs();
  bool RevertActiveFile();
  bool SaveTabAs(std::size_t index, const std::filesystem::path& path, std::string* error);
  void OpenSaveAsPrompt(std::string initial = {});
  void ResetCaretBlink();
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
  bool RevealPathAtLine(const std::filesystem::path& path,
                        const LineNavigationRequest& request,
                        std::string* error_message);
  bool MoveActiveTabToGroup(std::size_t to_group, std::optional<std::size_t> to_slot);
  void SelectAll();
  void Undo();
  void Redo();
  std::string CopySelectionText(bool* whole_line = nullptr) const;
  void RememberLineClipboardCopy(std::string_view text, bool whole_line);
  std::optional<std::string> LastTerminalCommandText() const;
  std::optional<std::string> SelectionTextWithContext();
  std::optional<std::string> ActiveEditorReference(bool for_terminal);
  bool TypeIntoActiveTerminal(std::string text);
  void CutSelection();
  void PasteClipboard();
  void InsertText(std::string text);
  void PressKey(SDL_Keycode key, SDL_Keymod modifiers);
  void InjectPointerEvent(const SDL_Event& event, SDL_Keymod modifiers);
  bool ShowHoverAtCaret();

  void RefreshAvailableColorschemeNames();
  void ReportColorschemeNames();
  bool ApplyColorscheme(std::string_view name);
  std::string_view CurrentColorschemeName() const;
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
  void CycleSurfaceFocus(bool backward);
  bool PluginRuntimeEnabled() const;
  void ReloadPluginsWithFeedback();
  void RequestQuit();
  bool DebuggerEnabled() const;
  void ToggleDebuggerEnabled();
  void StartDebuggingWithFeedback();
  void StopDebuggingWithFeedback();
  void StopAllDebugSessions();
  void OpenDebugReplPrompt();
  void OpenLaunchConfigPicker();
  void OpenColorThemePicker();
  void OpenDocumentSymbolPicker(std::string query);
  void OpenCommandPalette(std::string seed = {});
  void OpenGoToLinePrompt();
  void OpenCommandArgumentsPrompt(std::string_view command_name, std::string prefill = {});
  bool OpenArgumentPicker(ActionId id);
  bool OpenOptionalArgumentPicker(ActionId id);
  bool OpenHostPick(std::string_view title, std::string_view command_line);
  bool OpenFileHistoryPick(const std::vector<project::remote::FileVersionHistory::Version>& versions,
                           std::string_view file_name);
  bool OpenWorktreePick();
  bool OpenCommitPick();
  std::string OpenFolderPick(const std::filesystem::path& folder);
  bool OpenLineEndingPick();
  bool SetActiveLineEnding(util::LineEnding ending);
  bool OpenIndentationPick();
  void OpenRenameSymbolPrompt();
  void ToggleDebugPane();
  void ShowDebugPaneSurface(DebugPaneMode mode);
  void ShowDebugOutput();
  void EditBreakpointModifierFromMenu(ActionId id);
  void BreakpointQuickActionFromMenu(ActionId id);
  void RemoveBreakpointFromMenu();
  bool DispatchSelectedGitSidebarAction(GitSidebarActionId action);
  bool ToggleStageSelectedGitEntry();
  std::string SelectedConflictedGitEntryPath() const;
  editor::BreakpointStore& MutableBreakpointStore();
  void ResendBreakpoints(const std::filesystem::path& path);
  std::string StartNamedDebugConfig(const std::string& name);
  std::string StartAdHocDebug(const std::string& program, const std::vector<std::string>& args,
                              const std::string& type);

  editor::TextViewport* ActiveEditableViewport();
  editor::TextViewport* ActiveNavigableViewport();
  TabEntry::EditorTabState* ActiveEditorTab();
  editor::FoldingModel* EnsureActiveFoldingModelFresh();
  void RefreshActiveCompareAfterViewportEdit();
  void FinishClipboardEdit();
  void NotifyEditorViewportChanged(bool last_change);
  void RetrackActiveMergeAfterViewportEdit();
  void NotifyEditorCaretMoved();
  void ToggleEditorEssentialsCapability(ActionId id);
  std::optional<std::string> GetSettingValue(std::string_view id) const;
  bool SetSettingValue(std::string_view id, std::string value);
  void Notify(NotificationService::Tone tone, std::string message);
  bool FocusNotifications();
  bool ShowOutputChannel(std::string_view channel_id);
  RemoteHostService* RemoteHosts();
  void OpenRemoteHostPrompt();
  bool CancelSaveWait(std::uint64_t id);
  std::optional<NotificationAction> TakeNotificationAction(std::string_view row,
                                                           std::string_view action,
                                                           std::string* error);
  bool ActivateEditorBanner(std::string_view label, std::string* error);
  void ToggleWindowFullscreen();

 private:
  void ApplyUndoRedo(bool redo);

  void RequestLiveConfigRedraw();

  void InsertTextIntoActiveSurface(std::string text, bool distribute_across_carets = false);

  std::optional<CompareInput> ResolveCurrentCompareInput(ActionSource source, bool* from_file,
                                                         std::string* error) const;

  ProjectCatalogState& project_catalog_;
  ProjectWorkspaceState& state_;
  float& ui_scale_;
  Services services_;
  Operations operations_;
  ActionReceipt receipt_;
};

}
