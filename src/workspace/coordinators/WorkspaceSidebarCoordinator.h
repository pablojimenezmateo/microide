#pragma once

#include <cstddef>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "workspace/WorkspaceLayout.h"
#include "workspace/state/WorkspaceMenuState.h"
#include "workspace/WorkspacePluginRuntime.h"
#include "workspace/services/NotificationService.h"
#include "workspace/state/WorkspaceProjectState.h"
#include "workspace/state/WorkspacePromptState.h"
#include "workspace/git/GitSidebarCommandCenter.h"
#include "workspace/state/WorkspaceSidebarState.h"

namespace microide::workspace {

class SidebarCoordinator {
 public:
  struct Operations {
    std::function<void()> close_tree_context_menu;
    std::function<bool(const std::filesystem::path&, bool, bool)> open_project_tab;
    std::function<void()> stop_project_search;
    std::function<void()> request_window_redraw;
    std::function<void()> request_sidebar_redraw;
    std::function<void()> note_layout_inputs_changed;
    std::function<void()> request_git_refresh;
    std::function<bool(GitSidebarState::RefreshSnapshot*)> consume_git_refresh_snapshot;
    std::function<void()> refresh_project_search;
    std::function<void()> request_file_index_refresh;
    std::function<void(const std::filesystem::path&)> open_file;
    std::function<editor::TextViewport*()> active_editor_viewport;
    std::function<void(const editor::TextViewport&)> query_outline_symbols;
    std::function<void(std::string request_view_id, bool ok,
                       std::vector<plugin::PluginHost::SidebarItem> items,
                       std::string error_message)>
        apply_plugin_sidebar_result;
    std::function<bool(const std::filesystem::path&)> open_git_conflict_merge;
    std::function<bool(const std::filesystem::path&, const std::string&, const std::string&)>
        open_working_tree_comparison;
    std::function<bool(const std::filesystem::path&,
                       const std::string&,
                       const std::string&,
                       const std::string&,
                       const std::string&)>
        open_branch_head_comparison;
    std::function<void(PromptSurfaceState::Action,
                       PromptSurfaceState::Kind,
                       const std::filesystem::path&,
                       std::string,
                       std::string)>
        open_prompt_surface;
    std::function<bool(const std::filesystem::path&, std::string*)> has_dirty_editor_tabs_for_path;
    std::function<void(const std::filesystem::path&)> invalidate_editor_blame_path;
    std::function<void(const std::filesystem::path&)> reload_clean_editor_tabs_for_path;
    std::function<void(const std::filesystem::path&)> close_open_tabs_for_path;
    std::function<std::optional<WorkspaceLayout>()> current_workspace_layout;
    std::function<ScrollableListLayout(const SDL_FRect&, std::size_t)> compute_tree_sidebar_list_layout;
    std::function<std::optional<std::size_t>()> selected_git_sidebar_line_index;
    std::function<const std::vector<GitSidebarLine>&()> build_git_sidebar_lines;
    std::function<ScrollableListLayout(const SDL_FRect&, std::size_t)> compute_git_sidebar_list_layout;
    std::function<ScrollableListLayout(const SDL_FRect&, std::size_t)>
        compute_problems_sidebar_list_layout;
    std::function<ScrollableListLayout(const SDL_FRect&, std::size_t)> compute_tests_sidebar_list_layout;
    std::function<ScrollableListLayout(const SDL_FRect&, std::size_t)> compute_plugin_sidebar_list_layout;
    std::function<bool()> refresh_tests_sidebar_state;
    std::function<bool(const std::vector<std::string>&)> run_tests;
    std::function<void(std::string)> set_command_feedback;
    std::function<void(NotificationService::Tone, std::string)> notify;
    std::function<bool(ActionId, const std::vector<std::string>&, ActionSource)> execute_action;
    std::function<bool()> open_commit_workflow;
    std::function<std::vector<plugin::PluginHost::SidebarItem>(SidebarMode mode)> host_view_rows;
    std::function<bool(SidebarMode mode, const plugin::PluginHost::SidebarItem&)> open_host_view_row;
    std::function<bool(SidebarMode mode, const plugin::PluginHost::SidebarItem&)> end_host_view_row;
    std::function<bool(SidebarMode mode, const plugin::PluginHost::SidebarItem&)> toggle_host_view_row;
  };

  SidebarCoordinator(ProjectWorkspaceState& state,
                     PromptState& prompts,
                     MenuSurfaceState& menu_state,
                     std::filesystem::path& project_root,
                     WorkspacePluginRuntime& plugin_runtime,
                     Operations operations);

  void ShowMode(SidebarMode mode, bool temporary = false);
  void ShowTree(const std::filesystem::path& root = {});
  void ShowSearch(std::string query = {}, bool temporary = false);
  void ShowProblems();
  void ShowGit();
  void ShowTests();
  void ShowOutline();
  void ShowHostView(SidebarMode mode);
  bool ShowPlugin(std::string_view id, bool temporary = false);
  void Close();
  void Toggle();
  void RestorePrevious();
  void RefreshProjectFiles();
  void RefreshGit();
  bool RefreshProblems();
  bool CatchUpSearchResults(const editor::TextViewport& viewport);
  bool RefreshTests();
  bool RefreshPlugin();
  bool RefreshOutline();
  void RefreshHostView();
  void ApplyOutlineResult(const std::filesystem::path& request_path, const std::string& error,
                          const std::vector<plugin::PluginHost::DocumentSymbolNode>& symbols);
  void ApplyPluginSidebarResult(const std::string& request_view_id, bool ok,
                                std::vector<plugin::PluginHost::SidebarItem> items,
                                std::string error_message);
  void RevealSelectedTreeLine();
  void RevealSelectedGitLine();
  void RevealSelectedProblemsLine();
  void RevealSelectedTestsLine();
  void RevealSelectedPluginLine();
  void MoveGitSelection(int delta);
  bool TypeNavigateGit(char typed, std::uint64_t now_ns);
  void MoveProblemsSelection(int delta);
  void MoveTestsSelection(int delta);
  void MovePluginSelection(int delta);
  bool OpenProblemItem();
  bool OpenTestItem();
  bool RunTestItem();
  bool OpenPluginItem();
  bool TogglePluginItem();
  bool EndPluginItem();
  bool CanStageAllGitEntries() const;
  bool CanDiscardAllGitEntries() const;
  bool StageAllGitEntries();
  void OpenDiscardAllGitPrompt();
  bool DiscardAllGitEntries();
  bool StageGitEntry(std::size_t entry_index);
  bool UnstageGitEntry(std::size_t entry_index);
  bool DiscardGitEntry(std::size_t entry_index,
                       const std::optional<std::filesystem::path>& expected_path = {});
  void OpenDiscardGitEntryPrompt(std::size_t entry_index);
  bool DispatchGitSidebarAction(GitSidebarActionId action, std::size_t entry_index);
  void ReconcileOpenTabsAfterPathDiscard(const std::filesystem::path& path);

 private:
  std::vector<std::size_t> VisibleGitEntries() const;
  void ApplyTreeGitStatuses(project::SharedGitTreeStatusMap statuses);
  struct GitSelectionKey {
    std::string path;
    GitSidebarEntry::Section section = GitSidebarEntry::Section::Changed;
  };
  GitSelectionKey CurrentGitSelection() const;
  void ApplyGitSnapshotKeepingSelection(GitSidebarState::RefreshSnapshot snapshot,
                                        const GitSelectionKey& previous);
  const GitSidebarEntry* GitEntry(std::size_t entry_index) const;
  void ReportDisabledGitAction(GitSidebarActionId action, std::size_t entry_index) const;
  void ReportGitOperationFailure(std::string_view verb, const GitSidebarEntry& entry) const;
  static std::string_view GitEntryLabel(const GitSidebarEntry& entry);
  void ReportGitMessage(NotificationService::Tone tone, std::string message) const;
  SidebarMode ActiveSidebarMode() const;
  void RecomputePluginSidebarPlaceholder();
  void RevealListSelection(
      std::size_t count, std::size_t selected_index,
      const std::function<ScrollableListLayout(const SDL_FRect&, std::size_t)>& compute_layout);
  void MoveSimpleListSelection(
      std::size_t count, std::size_t* selected_index,
      const std::function<ScrollableListLayout(const SDL_FRect&, std::size_t)>& compute_layout,
      int delta);
  bool OpenEditorFileFromSidebar(const std::filesystem::path& path,
                                 std::optional<std::pair<std::size_t, std::size_t>> caret);

  ProjectWorkspaceState& state_;
  PromptState& prompts_;
  MenuSurfaceState& menu_state_;
  std::filesystem::path& project_root_;
  WorkspacePluginRuntime& plugin_runtime_;
  Operations operations_;
};

}
