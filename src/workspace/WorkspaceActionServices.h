#pragma once

#include <cstddef>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "workspace/WorkspaceActionRequests.h"
#include "workspace/WorkspaceActionTypes.h"
#include "workspace/WorkspaceMenuState.h"
#include "workspace/WorkspaceProjectState.h"
#include "workspace/WorkspacePromptState.h"
#include "workspace/WorkspaceSidebarRegistry.h"
#include "workspace/WorkspaceTabState.h"

namespace microide::workspace {

enum class ProjectOpenPickerResult {
  Launched,
  AlreadyOpen,
  Unavailable,
};

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
    std::function<SidebarMode()> active_sidebar_mode;
    std::function<void()> toggle_sidebar;
    std::function<void()> close_sidebar;
    std::function<void(const std::filesystem::path&)> show_tree_sidebar;
    std::function<void(std::string, bool)> show_search_sidebar;
    std::function<void()> show_problems_sidebar;
    std::function<void()> show_git_sidebar;
    std::function<void()> show_tests_sidebar;
    std::function<bool(std::string_view, bool)> show_plugin_sidebar;
    std::function<std::optional<SDL_FRect>()> current_window_rect;
    std::function<void()> refresh_project_files;
    std::function<void()> reload_clean_open_buffers_from_disk;
    std::function<std::filesystem::path(ActionSource)> tree_mutation_base_path;
    std::function<std::filesystem::path(ActionSource)> resolve_tree_action_path;
    std::function<std::filesystem::path()> active_tab_path;
    std::function<void(PromptSurfaceState::Action,
                       PromptSurfaceState::Kind,
                       const std::filesystem::path&,
                       std::string)>
        open_prompt_surface;
    std::function<bool(std::string_view)> write_clipboard_text;
    std::function<bool(std::string_view)> write_primary_selection_text;
    std::function<void(std::string)> open_terminal;
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
    std::function<bool(std::string*)> find_lsp_references;
    std::function<bool(std::string*)> discover_tests_for_active_buffer;
    std::function<bool(const std::vector<std::string>&, std::string*)> run_tests;
    std::function<bool(std::string*)> run_all_discovered_tests;
    std::function<bool(std::string*)> request_inline_completion;
    std::function<void(const std::filesystem::path&, const std::string&)> open_compare_picker_for_path;
    std::function<void(const project::GitCommitEntry&)> open_comparison;
    std::function<bool(const std::filesystem::path&,
                       const std::filesystem::path&,
                       const std::filesystem::path&,
                       const std::filesystem::path&)>
        open_merge_editor;
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
    std::function<bool(EditorSplitOrientation)> split_active_editor;
    std::function<bool()> unsplit_active_editor;
    std::function<bool(int)> cycle_editor_split;
    std::function<bool(std::size_t)> activate_ordered_editor_split;
    std::function<void(std::size_t)> request_close_tab;
    std::function<void(std::vector<std::size_t>)> request_close_tabs;
    std::function<void()> close_all_tabs;
    std::function<editor::TextViewport*()> active_navigable_viewport;
    std::function<void()> request_focused_editor_redraw;
    std::function<editor::TextViewport*()> active_editable_viewport;
    std::function<bool(std::string_view)> insert_text_into_active_text_surface;
    std::function<bool()> has_selection_at_active_single_line_text_surface;
    std::function<std::string()> selected_text_at_active_single_line_text_surface;
    std::function<bool()> select_all_at_active_single_line_text_surface;
    std::function<bool()> cut_selection_at_active_single_line_text_surface;
    std::function<CompareTabState*()> active_compare_tab;
    std::function<void()> mark_branch_file_reviewed;
    std::function<void()> mark_branch_hunk_reviewed;
    std::function<void()> clear_branch_review_state;
    std::function<void(std::string)> edit_branch_review_note;
    std::function<void(CompareTabState&)> refresh_compare_tab_derived_state;
    std::function<void(CompareTabState&, bool)> sync_compare_selection_from_viewport;
    std::function<MergeTabState*()> active_merge_tab;
    std::function<void(MergeTabState&,
                       const std::vector<std::string>&,
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
    std::function<void()> paste_clipboard_into_terminal;
    std::function<void()> refresh_available_colorscheme_names;
    std::function<void(std::string_view)> apply_colorscheme;
    std::function<void()> apply_editor_preferences_to_all_tabs;
    std::function<void()> save_config_state;
    std::function<std::optional<std::string>(std::string_view)> get_setting_value;
    std::function<bool(std::string_view, std::string)> set_setting_value;
    std::function<void()> normalize_sidebar_view_selection;
    std::function<void(float)> apply_ui_scale;
    std::function<TerminalTabState*()> active_terminal_tab;
    std::function<void()> reset_command_prompt_session;
    std::function<void(bool)> request_command_mode_transition_redraw;
    std::function<bool()> plugin_runtime_enabled;
    std::function<void()> reload_plugins_for_current_project;
    std::function<std::string()> plugin_runtime_reload_summary;
    std::function<void()> request_quit;
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
  std::filesystem::path TreeMutationBasePath(ActionSource source) const;
  std::filesystem::path ResolveTreeActionPath(ActionSource source) const;
  std::filesystem::path ActiveTabPath() const;
  void OpenCreatePathPrompt(bool directory, const std::filesystem::path& base_path);
  void OpenRenamePathPrompt(const std::filesystem::path& path);
  void OpenDeletePathPrompt(const std::filesystem::path& path);
  bool WriteClipboardText(std::string_view text) const;
  bool WritePrimarySelectionText(std::string_view text) const;

  void OpenTerminal(std::string command);
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
  bool FindLspReferences(std::string* error_message);
  bool DiscoverTestsForActiveBuffer(std::string* error_message);
  bool RunTests(const std::vector<std::string>& test_ids, std::string* error_message);
  bool RunAllDiscoveredTests(std::string* error_message);
  bool RequestInlineCompletion(std::string* error_message);
  bool ActiveTabIsCompare() const;
  bool ActiveTabIsMerge() const;
  void OpenBufferSearch(std::string query);
  void OpenBufferReplace();
  std::filesystem::path ResolveComparePath(const std::filesystem::path& requested_path,
                                           ActionSource source) const;
  void OpenComparePickerForPath(const std::filesystem::path& path,
                                const std::string& commit_spec);
  void OpenHeadComparison(const std::filesystem::path& path);
  void MarkBranchFileReviewed();
  void MarkBranchHunkReviewed();
  void ClearBranchReviewState();
  void EditBranchReviewNote(std::string note_text);
  void OpenMergeEditor(const std::filesystem::path& base_path,
                       const std::filesystem::path& incoming_path,
                       const std::filesystem::path& current_path,
                       const std::filesystem::path& output_path);

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
  bool OpenVerticalSplitPath(const std::filesystem::path& path, std::string* error_message);
  void SplitActiveEditorVertically();
  void UnsplitActiveEditor();
  void CycleEditorSplit(int delta);
  void ActivateOrderedEditorSplit(std::size_t index);
  std::size_t ActiveEditorSplitCount() const;
  void RequestCloseTab(std::size_t index);
  void RequestCloseTabs(std::vector<std::size_t> indices);
  void CloseAllTabs();

  bool ExecuteLineNavigation(const LineNavigationRequest& request, bool relative);
  void SelectAll();
  void Undo();
  void Redo();
  std::string CopySelectionText() const;
  std::optional<std::string> LastTerminalCommandText() const;
  std::optional<std::string> SelectionTextWithContext() const;
  void CutSelection();
  void PasteClipboard();

  void RefreshAvailableColorschemeNames();
  void ApplyColorscheme(std::string_view name);
  void SetTabSize(std::size_t value);
  void SetIndentWidth(std::size_t value);
  float UiScale() const;
  void ApplyUiScale(float scale);
  void SetSoftTabs(bool enabled);
  bool SoftWrapEnabled() const;
  void SetSoftWrap(bool enabled);
  bool Focus(FocusRequestTarget target);
  void OpenCommandPrompt(std::string input = {});
  bool PluginRuntimeEnabled() const;
  void ReloadPluginsWithFeedback();
  void RequestQuit();

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

 private:
  ProjectCatalogState& project_catalog_;
  ProjectWorkspaceState& state_;
  float& ui_scale_;
  Operations operations_;
};

}  // namespace microide::workspace
