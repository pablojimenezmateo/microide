#pragma once

#include <functional>
#include <string>
#include <vector>

#include "workspace/GitSidebarCommandCenter.h"
#include "workspace/WorkspaceLayout.h"
#include "workspace/WorkspaceActionTypes.h"
#include "workspace/WorkspaceMenuState.h"
#include "workspace/WorkspaceProjectState.h"
#include "workspace/WorkspaceInteractionState.h"

namespace microide::workspace {

class SidebarMouseCoordinator {
 public:
  struct Operations {
    std::function<SidebarMode()> active_sidebar_mode;
    std::function<void(ProjectSearchEditField)> begin_project_search_edit;
    std::function<SDL_FRect(const SDL_FRect&)> project_search_query_rect;
    std::function<SDL_FRect(const SDL_FRect&)> project_search_replace_rect;
    std::function<SDL_FRect(const SDL_FRect&)> project_search_mode_button_rect;
    std::function<void()> commit_project_search_edit;
    std::function<void()> toggle_project_search_pattern_mode;
    std::function<SDL_FRect(const SDL_FRect&)> project_search_case_button_rect;
    std::function<void()> cycle_project_search_case_mode;
    std::function<SDL_FRect(const SDL_FRect&)> project_search_hidden_button_rect;
    std::function<void()> toggle_project_search_hidden_files;
    std::function<std::vector<int>()> build_project_search_line_map;
    std::function<ScrollableListLayout(const SDL_FRect&, std::size_t)>
        compute_project_search_sidebar_list_layout;
    std::function<void(const std::filesystem::path&)> open_file;
    std::function<void(const std::filesystem::path&, std::size_t, std::size_t)> open_file_at_location;
    std::function<void()> restore_previous_sidebar;
    std::function<void()> seed_buffer_search_from_project_search;
    std::function<bool()> can_stage_all_git_sidebar_entries;
    std::function<SDL_FRect(const SDL_FRect&)> git_sidebar_stage_all_button_rect;
    std::function<bool()> stage_all_git_sidebar_entries;
    std::function<bool()> can_open_git_commit_button;
    std::function<SDL_FRect(const SDL_FRect&)> git_sidebar_commit_button_rect;
    std::function<bool()> open_git_commit_workflow;
    std::function<bool()> confirm_commit_workflow;
    std::function<bool()> can_discard_all_git_sidebar_entries;
    std::function<SDL_FRect(const SDL_FRect&)> git_sidebar_discard_all_button_rect;
    std::function<void()> open_discard_all_git_sidebar_prompt;
    std::function<SDL_FRect(const SDL_FRect&)> git_sidebar_refresh_button_rect;
    std::function<std::optional<SDL_FRect>(const SDL_FRect&)> git_sidebar_outgoing_base_button_rect;
    std::function<void(MenuId, const SDL_FRect&)> open_anchored_menu;
    std::function<bool(ActionId, const std::vector<std::string>&, ActionSource)> execute_action;
    std::function<float(const SDL_FRect&)> git_sidebar_list_top;
    std::function<std::vector<GitSidebarLine>()> build_git_sidebar_lines;
    std::function<ScrollableListLayout(const SDL_FRect&, std::size_t)> compute_git_sidebar_list_layout;
    std::function<bool(const std::string&)> toggle_git_sidebar_directory_collapsed;
    std::function<bool(GitSidebarActionId, std::size_t)> dispatch_git_sidebar_action;
    std::function<ScrollableListLayout(const SDL_FRect&, std::size_t)>
        compute_problems_sidebar_list_layout;
    std::function<void()> reveal_selected_problems_sidebar_line;
    std::function<bool()> open_selected_problem_sidebar_item;
    std::function<ScrollableListLayout(const SDL_FRect&, std::size_t)>
        compute_tests_sidebar_list_layout;
    std::function<void()> reveal_selected_tests_sidebar_line;
    std::function<bool()> open_selected_test_sidebar_item;
    std::function<bool()> run_selected_test_sidebar_item;
    std::function<ScrollableListLayout(const SDL_FRect&, std::size_t)>
        compute_plugin_sidebar_list_layout;
    std::function<void()> reveal_selected_plugin_sidebar_line;
    std::function<bool()> open_selected_plugin_sidebar_item;
    std::function<bool()> toggle_plugin_sidebar_item;
    std::function<bool()> can_collapse_tree;
    std::function<SDL_FRect(const SDL_FRect&)> tree_sidebar_collapse_button_rect;
    std::function<void()> collapse_all_tree;
    std::function<void()> reveal_selected_tree_sidebar_line;
    std::function<SDL_FRect(const SDL_FRect&)> tree_sidebar_refresh_button_rect;
    std::function<void(TreeContextTargetKind, const std::filesystem::path&, const SDL_FRect&)>
        open_tree_context_menu;
    std::function<ScrollableListLayout(const SDL_FRect&, std::size_t)> compute_tree_sidebar_list_layout;
  };

  SidebarMouseCoordinator(ProjectWorkspaceState& state,
                          InteractionState& interaction_state,
                          Operations operations);

  bool HandleButtonDown(const SDL_Event& event, const WorkspaceLayout& layout);
  bool HandleDrag(const SDL_Event& event, const WorkspaceLayout& layout);
  bool HandleWheel(const SDL_Event& event,
                   const WorkspaceLayout& layout,
                   int vertical_ticks);

 private:
  bool HandleSearchButtonDown(const SDL_Event& event,
                              const WorkspaceLayout& layout,
                              float local_y);
  bool HandleGitButtonDown(const SDL_Event& event,
                           const WorkspaceLayout& layout,
                           float local_y);
  bool HandleProblemsButtonDown(const SDL_Event& event,
                                const WorkspaceLayout& layout,
                                float local_y);
  bool HandleTestsButtonDown(const SDL_Event& event,
                             const WorkspaceLayout& layout,
                             float local_y);
  bool HandlePluginButtonDown(const SDL_Event& event,
                              const WorkspaceLayout& layout,
                              float local_y);
  bool HandleTreeButtonDown(const SDL_Event& event,
                            const WorkspaceLayout& layout,
                            float local_y);
  bool BeginScrollbarDrag(const SDL_Event& event, const WorkspaceLayout& layout);
  ScrollableListLayout CurrentListLayout(const WorkspaceLayout& layout) const;

  ProjectWorkspaceState& state_;
  InteractionState& interaction_state_;
  Operations operations_;
};

}  // namespace microide::workspace
