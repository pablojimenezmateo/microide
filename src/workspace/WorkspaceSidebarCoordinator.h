#pragma once

#include <cstddef>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "workspace/WorkspaceLayout.h"
#include "workspace/WorkspaceMenuState.h"
#include "workspace/WorkspacePluginRuntime.h"
#include "workspace/WorkspaceProjectState.h"
#include "workspace/WorkspacePromptState.h"
#include "workspace/WorkspaceSidebarState.h"

namespace microide::workspace {

class SidebarCoordinator {
 public:
  struct Operations {
    std::function<void()> close_tree_context_menu;
    std::function<bool(const std::filesystem::path&, bool, bool)> open_project_tab;
    std::function<void()> stop_project_search;
    std::function<void()> request_window_redraw;
    std::function<void()> request_sidebar_redraw;
    std::function<void()> request_git_refresh;
    std::function<bool(GitSidebarState::RefreshSnapshot*)> consume_git_refresh_snapshot;
    std::function<void()> refresh_project_search;
    std::function<void(const std::filesystem::path&)> open_file;
    std::function<editor::TextViewport*()> active_editor_viewport;
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
                       std::string)>
        open_prompt_surface;
    std::function<bool(const std::filesystem::path&, std::string*)> has_dirty_editor_tabs_for_path;
    std::function<void(const std::filesystem::path&)> invalidate_editor_blame_path;
    std::function<void(const std::filesystem::path&)> reload_clean_editor_tabs_for_path;
    std::function<void(const std::filesystem::path&)> close_open_tabs_for_path;
    std::function<std::optional<WorkspaceLayout>()> current_workspace_layout;
    std::function<ScrollableListLayout(const SDL_FRect&, std::size_t)> compute_tree_sidebar_list_layout;
    std::function<std::optional<std::size_t>()> selected_git_sidebar_line_index;
    std::function<std::vector<GitSidebarLine>()> build_git_sidebar_lines;
    std::function<ScrollableListLayout(const SDL_FRect&, std::size_t)> compute_git_sidebar_list_layout;
    std::function<ScrollableListLayout(const SDL_FRect&, std::size_t)>
        compute_problems_sidebar_list_layout;
    std::function<ScrollableListLayout(const SDL_FRect&, std::size_t)> compute_tests_sidebar_list_layout;
    std::function<ScrollableListLayout(const SDL_FRect&, std::size_t)> compute_plugin_sidebar_list_layout;
    std::function<bool()> refresh_tests_sidebar_state;
    std::function<bool(const std::vector<std::string>&)> run_tests;
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
  bool ShowPlugin(std::string_view id, bool temporary = false);
  void Close();
  void Toggle();
  void RestorePrevious();
  void RefreshProjectFiles();
  void RefreshGit();
  bool RefreshProblems();
  bool RefreshTests();
  bool RefreshPlugin();
  void RevealSelectedTreeLine();
  void RevealSelectedGitLine();
  void RevealSelectedProblemsLine();
  void RevealSelectedTestsLine();
  void RevealSelectedPluginLine();
  void MoveGitSelection(int delta);
  void MoveProblemsSelection(int delta);
  void MoveTestsSelection(int delta);
  void MovePluginSelection(int delta);
  bool OpenGitEntry(std::size_t entry_index);
  bool OpenProblemItem();
  bool OpenTestItem();
  bool RunTestItem();
  bool OpenPluginItem();
  bool CanStageAllGitEntries() const;
  bool CanDiscardAllGitEntries() const;
  bool StageAllGitEntries();
  void OpenDiscardAllGitPrompt();
  bool DiscardAllGitEntries();
  bool StageGitEntry(std::size_t entry_index);
  bool UnstageGitEntry(std::size_t entry_index);
  bool DiscardGitEntry(std::size_t entry_index);
  void ReconcileOpenTabsAfterPathDiscard(const std::filesystem::path& path);

 private:
  SidebarMode SidebarModeForViewId(std::string_view view_id) const;
  SidebarMode ActiveSidebarMode() const;

  ProjectWorkspaceState& state_;
  PromptState& prompts_;
  MenuSurfaceState& menu_state_;
  std::filesystem::path& project_root_;
  WorkspacePluginRuntime& plugin_runtime_;
  Operations operations_;
};

}  // namespace microide::workspace
