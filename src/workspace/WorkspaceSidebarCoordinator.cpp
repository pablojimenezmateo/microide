#include "workspace/WorkspaceSidebarCoordinator.h"

#include <utility>

#include "workspace/WorkspaceMenuCoordinator.h"
#include "workspace/WorkspacePathMutationCoordinator.h"
#include "workspace/WorkspaceShell.h"
#include "workspace/WorkspaceSidebarRegistry.h"

namespace microide::workspace {

SidebarCoordinator::SidebarCoordinator(ProjectWorkspaceState& state,
                                       PromptState& prompts,
                                       MenuSurfaceState& menu_state,
                                       std::filesystem::path& project_root,
                                       WorkspacePluginRuntime& plugin_runtime,
                                       Operations operations)
    : state_(state),
      prompts_(prompts),
      menu_state_(menu_state),
      project_root_(project_root),
      plugin_runtime_(plugin_runtime),
      operations_(std::move(operations)) {}

SidebarMode SidebarCoordinator::SidebarModeForViewId(std::string_view view_id) const {
  if (view_id.empty()) {
    return SidebarMode::None;
  }
  const auto view = FindSidebarView(view_id, plugin_runtime_.Host());
  return view.has_value() ? view->mode : SidebarMode::None;
}

SidebarMode SidebarCoordinator::ActiveSidebarMode() const {
  return SidebarModeForViewId(state_.sidebar.view_id);
}

void SidebarCoordinator::ShowMode(SidebarMode mode, bool temporary) {
  if (mode == SidebarMode::None) {
    Close();
    return;
  }
  if (mode != SidebarMode::Tree) {
    operations_.close_tree_context_menu();
  }

  if (ActiveSidebarMode() == SidebarMode::Search && mode != SidebarMode::Search) {
    operations_.stop_project_search();
  }

  if (temporary) {
    if (!state_.sidebar.temporary && state_.sidebar.visible) {
      state_.sidebar.prev_view_id = state_.sidebar.view_id;
    }
  } else {
    state_.sidebar.prev_view_id.clear();
  }

  if (mode != SidebarMode::Plugin) {
    if (const SidebarViewSpec* view = FindBuiltinSidebarView(mode); view != nullptr) {
      state_.sidebar.view_id = std::string(view->id);
    }
  }
  state_.sidebar.temporary = temporary;
  state_.sidebar.visible = true;
  state_.surface.focus = FocusTarget::Sidebar;
  state_.sidebar.scroll_row = 0;
  operations_.request_window_redraw();
}

void SidebarCoordinator::ShowTree(const std::filesystem::path& root) {
  if (!root.empty() && !operations_.open_project_tab(root, true, true)) {
    return;
  }

  ShowMode(SidebarMode::Tree, false);
}

void SidebarCoordinator::ShowSearch(std::string query, bool temporary) {
  if (!query.empty() || state_.overlay.workflow.project_search.query.empty()) {
    state_.overlay.workflow.project_search.query = std::move(query);
  }
  state_.overlay.workflow.project_search.edit_buffer = state_.overlay.workflow.project_search.query;
  state_.overlay.workflow.project_search.editing =
      state_.overlay.workflow.project_search.query.empty();
  state_.overlay.workflow.project_search.edit_field = ProjectSearchEditField::Query;
  state_.overlay.workflow.project_search.selected_index = 0;
  operations_.refresh_project_search();
  ShowMode(SidebarMode::Search, temporary);
}

void SidebarCoordinator::ShowProblems() {
  RefreshProblems();
  ShowMode(SidebarMode::Problems, false);
  RevealSelectedProblemsLine();
}

void SidebarCoordinator::ShowGit() {
  RefreshGit();
  ShowMode(SidebarMode::Git, false);
  RevealSelectedGitLine();
}

bool SidebarCoordinator::ShowPlugin(std::string_view id, bool temporary) {
  const auto* provider = plugin_runtime_.Host().FindSidebarProvider(id);
  if (provider == nullptr) {
    return false;
  }

  state_.sidebar.view_id = provider->id;
  ShowMode(SidebarMode::Plugin, temporary);
  return RefreshPlugin();
}

void SidebarCoordinator::Close() {
  const bool was_visible = state_.sidebar.visible;
  if (ActiveSidebarMode() == SidebarMode::Search) {
    operations_.stop_project_search();
  }
  operations_.close_tree_context_menu();

  if (state_.sidebar.temporary && !state_.sidebar.prev_view_id.empty()) {
    RestorePrevious();
    return;
  }

  state_.sidebar.visible = false;
  state_.sidebar.temporary = false;
  state_.sidebar.prev_view_id.clear();
  if (state_.surface.focus == FocusTarget::Sidebar) {
    state_.surface.focus = FocusTarget::Editor;
  }
  if (was_visible) {
    operations_.request_window_redraw();
  }
}

void SidebarCoordinator::Toggle() {
  const bool was_visible = state_.sidebar.visible;
  if (state_.sidebar.visible) {
    Close();
    return;
  }

  if (ActiveSidebarMode() == SidebarMode::None) {
    state_.sidebar.view_id = "tree";
  }
  state_.sidebar.visible = true;
  state_.sidebar.temporary = false;
  state_.sidebar.prev_view_id.clear();
  state_.surface.focus = FocusTarget::Sidebar;
  if (!was_visible) {
    operations_.request_window_redraw();
  }
}

void SidebarCoordinator::RestorePrevious() {
  if (ActiveSidebarMode() == SidebarMode::Search &&
      SidebarModeForViewId(state_.sidebar.prev_view_id) != SidebarMode::Search) {
    operations_.stop_project_search();
  }

  if (state_.sidebar.prev_view_id.empty()) {
    state_.sidebar.temporary = false;
    return;
  }

  state_.sidebar.view_id = state_.sidebar.prev_view_id;
  if (SidebarModeForViewId(state_.sidebar.view_id) == SidebarMode::None) {
    state_.sidebar.view_id = "tree";
  }
  state_.sidebar.prev_view_id.clear();
  state_.sidebar.temporary = false;
  state_.sidebar.visible = true;
  state_.surface.focus = FocusTarget::Sidebar;
  state_.sidebar.scroll_row = 0;
  if (ActiveSidebarMode() == SidebarMode::Plugin) {
    RefreshPlugin();
  } else if (ActiveSidebarMode() == SidebarMode::Problems) {
    RefreshProblems();
  }
  operations_.request_sidebar_redraw();
}

void SidebarCoordinator::RefreshProjectFiles() {
  state_.directory_tree.Refresh();
  RevealSelectedTreeLine();
  state_.file_index.Refresh();
  state_.file_finder.SetIndex(&state_.file_index);
  RefreshGit();
  RefreshProblems();
  RefreshPlugin();
  if (state_.sidebar.visible) {
    operations_.request_sidebar_redraw();
  }
}

SidebarCoordinator WorkspaceShell::MakeSidebarCoordinator() {
  return SidebarCoordinator(
      context_.current_project_state, context_.prompts, context_.menu_state,
      context_.current_project_state.root, plugin_runtime_,
      SidebarCoordinator::Operations{
          .close_tree_context_menu = [this]() { MakeMenuCoordinator().CloseTreeContextMenu(); },
          .open_project_tab =
              [this](const std::filesystem::path& root, bool restore_persistence, bool log_feedback) {
                return OpenProjectTab(root, restore_persistence, log_feedback);
              },
          .stop_project_search = [this]() { StopProjectSearch(); },
          .request_window_redraw = [this]() { RequestWindowRedraw(); },
          .request_sidebar_redraw = [this]() { RequestSidebarRedraw(); },
          .refresh_project_search = [this]() { RefreshProjectSearch(); },
          .open_file = [this](const std::filesystem::path& path) { OpenFile(path); },
          .active_editor_viewport = [this]() { return ActiveEditorViewport(); },
          .open_git_conflict_merge =
              [this](const std::filesystem::path& path) { return OpenGitConflictMerge(path); },
          .open_working_tree_comparison =
              [this](const std::filesystem::path& path,
                     const std::string& left_ref,
                     const std::string& left_label) {
                return OpenWorkingTreeComparison(path, left_ref, left_label);
              },
          .open_branch_head_comparison =
              [this](const std::filesystem::path& path,
                     const std::string& left_ref,
                     const std::string& left_label,
                     const std::string& right_ref,
                     const std::string& right_label) {
                return OpenBranchHeadComparison(path, left_ref, left_label, right_ref, right_label);
              },
          .open_prompt_surface =
              [this](PromptSurfaceState::Action action,
                     PromptSurfaceState::Kind kind,
                     const std::filesystem::path& path,
                     std::string input) {
                OpenPromptSurface(action, kind, path, std::move(input));
              },
          .has_dirty_editor_tabs_for_path =
              [this](const std::filesystem::path& path, std::string* blocking_label) {
                return MakePathMutationCoordinator().HasDirtyEditorTabsForPath(path, blocking_label);
              },
          .invalidate_editor_blame_path =
              [this](const std::filesystem::path& path) { InvalidateEditorBlamePath(path); },
          .reload_clean_editor_tabs_for_path =
              [this](const std::filesystem::path& path) { ReloadCleanEditorTabsForPath(path); },
          .close_open_tabs_for_path =
              [this](const std::filesystem::path& path) {
                MakePathMutationCoordinator().CloseOpenTabsForPath(path);
              },
          .current_workspace_layout = [this]() { return CurrentWorkspaceLayout(); },
          .compute_tree_sidebar_list_layout =
              [this](const SDL_FRect& rect, std::size_t count) {
                return ComputeTreeSidebarListLayout(rect, count);
              },
          .selected_git_sidebar_line_index = [this]() { return SelectedGitSidebarLineIndex(); },
          .build_git_sidebar_lines = [this]() { return BuildGitSidebarLines(); },
          .compute_git_sidebar_list_layout =
              [this](const SDL_FRect& rect, std::size_t count) {
                return ComputeGitSidebarListLayout(rect, count);
              },
          .compute_problems_sidebar_list_layout =
              [this](const SDL_FRect& rect, std::size_t count) {
                return ComputeProblemsSidebarListLayout(rect, count);
              },
          .compute_plugin_sidebar_list_layout =
              [this](const SDL_FRect& rect, std::size_t count) {
                return ComputePluginSidebarListLayout(rect, count);
              },
      });
}

void WorkspaceShell::ShowSidebarMode(SidebarMode mode, bool temporary) {
  MakeSidebarCoordinator().ShowMode(mode, temporary);
}

void WorkspaceShell::ShowTreeSidebar(const std::filesystem::path& root) {
  MakeSidebarCoordinator().ShowTree(root);
}

void WorkspaceShell::ShowSearchSidebar(std::string query, bool temporary) {
  MakeSidebarCoordinator().ShowSearch(std::move(query), temporary);
}

void WorkspaceShell::ShowProblemsSidebar() {
  MakeSidebarCoordinator().ShowProblems();
}

void WorkspaceShell::ShowGitSidebar() {
  MakeSidebarCoordinator().ShowGit();
}

bool WorkspaceShell::ShowPluginSidebar(std::string_view id, bool temporary) {
  return MakeSidebarCoordinator().ShowPlugin(id, temporary);
}

void WorkspaceShell::CloseSidebar() {
  MakeSidebarCoordinator().Close();
}

void WorkspaceShell::ToggleSidebar() {
  MakeSidebarCoordinator().Toggle();
}

void WorkspaceShell::RestorePreviousSidebar() {
  MakeSidebarCoordinator().RestorePrevious();
}

void WorkspaceShell::RefreshProjectFiles() {
  MakeSidebarCoordinator().RefreshProjectFiles();
}

void WorkspaceShell::RefreshGitSidebar() {
  MakeSidebarCoordinator().RefreshGit();
}

bool WorkspaceShell::RefreshProblemsSidebar() {
  return MakeSidebarCoordinator().RefreshProblems();
}

bool WorkspaceShell::RefreshPluginSidebar() {
  return MakeSidebarCoordinator().RefreshPlugin();
}

void WorkspaceShell::RevealSelectedGitSidebarLine() {
  MakeSidebarCoordinator().RevealSelectedGitLine();
}

void WorkspaceShell::RevealSelectedProblemsSidebarLine() {
  MakeSidebarCoordinator().RevealSelectedProblemsLine();
}

void WorkspaceShell::RevealSelectedTreeSidebarLine() {
  MakeSidebarCoordinator().RevealSelectedTreeLine();
}

void WorkspaceShell::RevealSelectedPluginSidebarLine() {
  MakeSidebarCoordinator().RevealSelectedPluginLine();
}

void WorkspaceShell::MoveGitSidebarSelection(int delta) {
  MakeSidebarCoordinator().MoveGitSelection(delta);
}

void WorkspaceShell::MoveProblemsSidebarSelection(int delta) {
  MakeSidebarCoordinator().MoveProblemsSelection(delta);
}

void WorkspaceShell::MovePluginSidebarSelection(int delta) {
  MakeSidebarCoordinator().MovePluginSelection(delta);
}

bool WorkspaceShell::OpenGitSidebarEntry(std::size_t entry_index) {
  return MakeSidebarCoordinator().OpenGitEntry(entry_index);
}

bool WorkspaceShell::OpenSelectedProblemSidebarItem() {
  return MakeSidebarCoordinator().OpenProblemItem();
}

bool WorkspaceShell::OpenSelectedPluginSidebarItem() {
  return MakeSidebarCoordinator().OpenPluginItem();
}

bool WorkspaceShell::CanStageAllGitSidebarEntries() const {
  return const_cast<WorkspaceShell*>(this)->MakeSidebarCoordinator().CanStageAllGitEntries();
}

bool WorkspaceShell::CanDiscardAllGitSidebarEntries() const {
  return const_cast<WorkspaceShell*>(this)->MakeSidebarCoordinator().CanDiscardAllGitEntries();
}

bool WorkspaceShell::StageAllGitSidebarEntries() {
  return MakeSidebarCoordinator().StageAllGitEntries();
}

void WorkspaceShell::OpenDiscardAllGitSidebarPrompt() {
  MakeSidebarCoordinator().OpenDiscardAllGitPrompt();
}

bool WorkspaceShell::DiscardAllGitSidebarEntries() {
  return MakeSidebarCoordinator().DiscardAllGitEntries();
}

bool WorkspaceShell::StageGitSidebarEntry(std::size_t entry_index) {
  return MakeSidebarCoordinator().StageGitEntry(entry_index);
}

bool WorkspaceShell::UnstageGitSidebarEntry(std::size_t entry_index) {
  return MakeSidebarCoordinator().UnstageGitEntry(entry_index);
}

bool WorkspaceShell::DiscardGitSidebarEntry(std::size_t entry_index) {
  return MakeSidebarCoordinator().DiscardGitEntry(entry_index);
}

void WorkspaceShell::ReconcileOpenTabsAfterPathDiscard(const std::filesystem::path& path) {
  MakeSidebarCoordinator().ReconcileOpenTabsAfterPathDiscard(path);
}

}  // namespace microide::workspace
