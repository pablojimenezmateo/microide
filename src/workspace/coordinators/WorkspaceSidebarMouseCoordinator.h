#pragma once

#include <functional>
#include <string>
#include <vector>

#include "workspace/git/GitSidebarCommandCenter.h"
#include "workspace/WorkspaceLayout.h"
#include "workspace/actions/WorkspaceActionTypes.h"
#include "workspace/state/WorkspaceMenuState.h"
#include "workspace/state/WorkspaceProjectState.h"
#include "workspace/state/WorkspaceInteractionState.h"

namespace microide::workspace {

class SidebarMouseCoordinator {
 public:
  struct Operations {
    std::function<SidebarMode()> active_sidebar_mode;
    std::function<void(ProjectSearchEditField)> begin_project_search_edit;
    // Search-panel geometry is not routed through here: the coordinator already
    // holds the project state the layout depends on, so it calls the free
    // functions in ProjectSearchPanelLayout.h directly instead of five
    // shell-bound rect callbacks.
    std::function<void()> commit_project_search_edit;
    std::function<void()> toggle_project_search_pattern_mode;
    std::function<void()> cycle_project_search_case_mode;
    std::function<void()> toggle_project_search_hidden_files;
    std::function<void()> toggle_project_search_scope_expanded;
    std::function<void()> request_project_search_replace_all;
    std::function<std::vector<int>()> build_project_search_line_map;
    std::function<ScrollableListLayout(const SDL_FRect&, std::size_t)>
        compute_project_search_sidebar_list_layout;
    std::function<void(const std::filesystem::path&)> open_file;
    std::function<void(const std::filesystem::path&, std::size_t, std::size_t)> open_file_at_location;
    std::function<void()> restore_previous_sidebar;
    std::function<void()> seed_buffer_search_from_project_search;
    std::function<bool()> can_stage_all_git_sidebar_entries;
    std::function<bool()> stage_all_git_sidebar_entries;
    std::function<bool()> can_open_git_commit_button;
    std::function<bool()> open_git_commit_workflow;
    std::function<bool()> confirm_commit_workflow;
    std::function<bool()> can_discard_all_git_sidebar_entries;
    std::function<void()> open_discard_all_git_sidebar_prompt;
    std::function<std::optional<SDL_FRect>(const SDL_FRect&)> git_sidebar_outgoing_base_button_rect;
    std::function<void(MenuId, const SDL_FRect&)> open_anchored_menu;
    std::function<bool(ActionId, const std::vector<std::string>&, ActionSource)> execute_action;
    std::function<float(const SDL_FRect&)> git_sidebar_list_top;
    // Reference to the shared cached line model (valid until the next cache call on
    // the same thread); mouse hit-testing reads it without copying the tree.
    std::function<const std::vector<GitSidebarLine>&()> build_git_sidebar_lines;
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
    // The tree drag's drop targets (PathDragState) and what each drop does.
    std::function<EditorGroupRectsLayout(const WorkspaceLayout&)> compute_editor_group_rects;
    std::function<bool()> editor_area_full;
    std::function<std::optional<SDL_FRect>(const WorkspaceLayout&)> terminal_drop_rect;
    std::function<bool(const std::filesystem::path&, std::size_t group)> open_path_in_group;
    std::function<bool(const std::filesystem::path&, std::size_t group, EditorSplitOrientation, bool insert_before)>
        open_path_in_new_group;
    std::function<std::string(const std::filesystem::path& source, bool cut, const std::filesystem::path& folder)>
        paste_tree_path;
    std::function<void(std::string)> type_into_terminal;
  };

  SidebarMouseCoordinator(ProjectWorkspaceState& state,
                          InteractionState& interaction_state,
                          Operations operations);

  bool HandleButtonDown(const SDL_Event& event, const WorkspaceLayout& layout);
  // Shift+F10 / the Menu key: the context menu a right-click on the selected row
  // opens, anchored under that row. False when the view has no selected row with
  // a menu (the plugin views, an empty list).
  bool OpenSelectionContextMenu(const WorkspaceLayout& layout);
  bool HandleDrag(const SDL_Event& event, const WorkspaceLayout& layout);
  // The tree drag (PathDragState). Motion past the threshold starts it and
  // resolves the drop target under the pointer; the release drops it (`copy`:
  // Ctrl held, a folder drop copies) or, if it never moved, opens the row.
  // True when the gesture's state or target changed (the preview repaints).
  bool HandlePathDragMotion(float x, float y, const WorkspaceLayout& layout);
  bool FinishPathDrag(bool copy);
  bool CancelPathDrag();
  bool HandleWheel(const SDL_Event& event,
                   const WorkspaceLayout& layout,
                   int vertical_ticks);

 private:
  bool HandleSearchButtonDown(const SDL_Event& event,
                              const WorkspaceLayout& layout,
                              float local_y);
  bool HandleSearchResultContextMenu(const SDL_Event& event,
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
  void ResolvePathDropTarget(const WorkspaceLayout& layout);
  // The click a press on a tree row defers to its release: a file opens, a
  // folder toggles.
  void ActivateTreeSelection();
  ScrollableListLayout CurrentListLayout(const WorkspaceLayout& layout) const;

  ProjectWorkspaceState& state_;
  InteractionState& interaction_state_;
  Operations operations_;
};

}  // namespace microide::workspace
