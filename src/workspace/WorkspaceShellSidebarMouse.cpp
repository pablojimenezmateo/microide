#include "workspace/WorkspaceShell.h"

#include "workspace/WorkspaceActionCoordinator.h"
#include "workspace/WorkspaceMenuCoordinator.h"
#include "workspace/WorkspaceSidebarMouseCoordinator.h"

namespace microide::workspace {

SidebarMouseCoordinator WorkspaceShell::MakeSidebarMouseCoordinator() {
  return SidebarMouseCoordinator(
      context_.current_project_state, context_.interaction_state,
      SidebarMouseCoordinator::Operations{
          .active_sidebar_mode = [this]() { return ActiveSidebarMode(); },
          .begin_project_search_edit =
              [this](ProjectSearchEditField field) { BeginProjectSearchEdit(field); },
          .project_search_query_rect =
              [this](const SDL_FRect& rect) { return ProjectSearchQueryRect(rect); },
          .project_search_replace_rect =
              [this](const SDL_FRect& rect) { return ProjectSearchReplaceRect(rect); },
          .project_search_mode_button_rect =
              [this](const SDL_FRect& rect) { return ProjectSearchModeButtonRect(rect); },
          .commit_project_search_edit = [this]() { CommitProjectSearchEdit(); },
          .toggle_project_search_pattern_mode = [this]() { ToggleProjectSearchPatternMode(); },
          .project_search_case_button_rect =
              [this](const SDL_FRect& rect) { return ProjectSearchCaseButtonRect(rect); },
          .cycle_project_search_case_mode = [this]() { CycleProjectSearchCaseMode(); },
          .project_search_hidden_button_rect =
              [this](const SDL_FRect& rect) { return ProjectSearchHiddenButtonRect(rect); },
          .toggle_project_search_hidden_files = [this]() { ToggleProjectSearchHiddenFiles(); },
          .build_project_search_line_map = [this]() { return BuildProjectSearchLineMap(); },
          .compute_project_search_sidebar_list_layout =
              [this](const SDL_FRect& rect, std::size_t count) {
                return ComputeProjectSearchSidebarListLayout(rect, count);
              },
          .open_file = [this](const std::filesystem::path& path) { OpenFile(path); },
          .open_file_at_location =
              [this](const std::filesystem::path& path, std::size_t line, std::size_t column) {
                OpenFileAtLocation(path, line, column);
              },
          .restore_previous_sidebar = [this]() { RestorePreviousSidebar(); },
          .can_stage_all_git_sidebar_entries = [this]() { return CanStageAllGitSidebarEntries(); },
          .git_sidebar_stage_all_button_rect =
              [this](const SDL_FRect& rect) { return GitSidebarStageAllButtonRect(rect); },
          .stage_all_git_sidebar_entries = [this]() { return StageAllGitSidebarEntries(); },
          .can_discard_all_git_sidebar_entries =
              [this]() { return CanDiscardAllGitSidebarEntries(); },
          .git_sidebar_discard_all_button_rect =
              [this](const SDL_FRect& rect) { return GitSidebarDiscardAllButtonRect(rect); },
          .open_discard_all_git_sidebar_prompt = [this]() { OpenDiscardAllGitSidebarPrompt(); },
          .git_sidebar_refresh_button_rect =
              [this](const SDL_FRect& rect) { return GitSidebarRefreshButtonRect(rect); },
          .git_sidebar_outgoing_base_button_rect =
              [this](const SDL_FRect& rect) { return GitSidebarOutgoingBaseButtonRect(rect); },
          .open_anchored_menu =
              [this](MenuId id, const SDL_FRect& rect) {
                MakeMenuCoordinator().OpenAnchoredMenu(id, rect);
              },
          .execute_action =
              [this](ActionId id, const std::vector<std::string>& args, ActionSource source) {
                return ActionCoordinator(MakeActionContext()).Execute(id, args, source);
              },
          .git_sidebar_list_top = [this](const SDL_FRect& rect) { return GitSidebarListTop(rect); },
          .build_git_sidebar_lines = [this]() { return BuildGitSidebarLines(); },
          .compute_git_sidebar_list_layout =
              [this](const SDL_FRect& rect, std::size_t count) {
                return ComputeGitSidebarListLayout(rect, count);
              },
          .compute_git_sidebar_entry_action_layout =
              [this](const SDL_FRect& rect, const GitSidebarEntry& entry) {
                return ComputeGitSidebarEntryActionLayout(rect, entry);
              },
          .unstage_git_sidebar_entry =
              [this](std::size_t index) { return UnstageGitSidebarEntry(index); },
          .stage_git_sidebar_entry = [this](std::size_t index) { return StageGitSidebarEntry(index); },
          .discard_git_sidebar_entry =
              [this](std::size_t index) { return DiscardGitSidebarEntry(index); },
          .open_git_sidebar_entry = [this](std::size_t index) { return OpenGitSidebarEntry(index); },
          .compute_problems_sidebar_list_layout =
              [this](const SDL_FRect& rect, std::size_t count) {
                return ComputeProblemsSidebarListLayout(rect, count);
              },
          .reveal_selected_problems_sidebar_line =
              [this]() { RevealSelectedProblemsSidebarLine(); },
          .open_selected_problem_sidebar_item = [this]() { return OpenSelectedProblemSidebarItem(); },
          .compute_tests_sidebar_list_layout =
              [this](const SDL_FRect& rect, std::size_t count) {
                return ComputeTestsSidebarListLayout(rect, count);
              },
          .reveal_selected_tests_sidebar_line =
              [this]() { RevealSelectedTestsSidebarLine(); },
          .open_selected_test_sidebar_item = [this]() { return OpenSelectedTestSidebarItem(); },
          .run_selected_test_sidebar_item = [this]() { return RunSelectedTestSidebarItem(); },
          .compute_plugin_sidebar_list_layout =
              [this](const SDL_FRect& rect, std::size_t count) {
                return ComputePluginSidebarListLayout(rect, count);
              },
          .reveal_selected_plugin_sidebar_line = [this]() { RevealSelectedPluginSidebarLine(); },
          .open_selected_plugin_sidebar_item = [this]() { return OpenSelectedPluginSidebarItem(); },
          .handle_outline_sidebar_pointer_down =
              [this](const SDL_Event& event, const WorkspaceLayout& layout) {
                HandleOutlineSidebarPointerDown(event, layout);
              },
          .can_collapse_tree =
              [this]() { return context_.current_project_state.directory_tree.CanCollapseAll(); },
          .tree_sidebar_collapse_button_rect =
              [this](const SDL_FRect& rect) { return TreeSidebarCollapseButtonRect(rect); },
          .collapse_all_tree = [this]() { context_.current_project_state.directory_tree.CollapseAll(); },
          .reveal_selected_tree_sidebar_line = [this]() { RevealSelectedTreeSidebarLine(); },
          .tree_sidebar_refresh_button_rect =
              [this](const SDL_FRect& rect) { return TreeSidebarRefreshButtonRect(rect); },
          .open_tree_context_menu =
              [this](TreeContextTargetKind target,
                     const std::filesystem::path& path,
                     const SDL_FRect& rect) {
                MakeMenuCoordinator().OpenTreeContextMenu(target, path, rect);
              },
          .compute_tree_sidebar_list_layout =
              [this](const SDL_FRect& rect, std::size_t count) {
                return ComputeTreeSidebarListLayout(rect, count);
              },
      });
}

}  // namespace microide::workspace
