#include "workspace/WorkspaceSidebarCoordinator.h"

#include <utility>

#include "workspace/WorkspaceMenuCoordinator.h"
#include "workspace/WorkspaceSidebarRegistry.h"

namespace microide::workspace {

SidebarCoordinator::SidebarCoordinator(WorkspaceShell& shell) : shell_(shell) {}

void SidebarCoordinator::ShowMode(WorkspaceShell::SidebarMode mode, bool temporary) {
  if (mode == WorkspaceShell::SidebarMode::None) {
    Close();
    return;
  }
  if (mode != WorkspaceShell::SidebarMode::Tree) {
    MenuCoordinator(shell_).CloseTreeContextMenu();
  }

  if (shell_.ActiveSidebarMode() == WorkspaceShell::SidebarMode::Search &&
      mode != WorkspaceShell::SidebarMode::Search) {
    shell_.StopProjectSearch();
  }

  if (temporary) {
    if (!shell_.sidebar_state_.temporary && shell_.sidebar_state_.visible) {
      shell_.sidebar_state_.prev_view_id = shell_.sidebar_state_.view_id;
    }
  } else {
    shell_.sidebar_state_.prev_view_id.clear();
  }

  if (mode != WorkspaceShell::SidebarMode::Plugin) {
    if (const SidebarViewSpec* view = FindBuiltinSidebarView(mode); view != nullptr) {
      shell_.sidebar_state_.view_id = std::string(view->id);
    }
  }
  shell_.sidebar_state_.temporary = temporary;
  shell_.sidebar_state_.visible = true;
  shell_.surface_.focus = WorkspaceShell::FocusTarget::Sidebar;
  shell_.sidebar_state_.scroll_row = 0;
  shell_.RequestWindowRedraw();
}

void SidebarCoordinator::ShowTree(const std::filesystem::path& root) {
  if (!root.empty()) {
    if (!shell_.OpenProjectTab(root, true, true)) {
      return;
    }
  }

  ShowMode(WorkspaceShell::SidebarMode::Tree, false);
}

void SidebarCoordinator::ShowSearch(std::string query, bool temporary) {
  if (!query.empty() || shell_.overlay_workflow_.project_search.query.empty()) {
    shell_.overlay_workflow_.project_search.query = std::move(query);
  }
  shell_.overlay_workflow_.project_search.edit_buffer = shell_.overlay_workflow_.project_search.query;
  shell_.overlay_workflow_.project_search.editing =
      shell_.overlay_workflow_.project_search.query.empty();
  shell_.overlay_workflow_.project_search.edit_field =
      WorkspaceShell::ProjectSearchEditField::Query;
  shell_.overlay_workflow_.project_search.selected_index = 0;
  shell_.RefreshProjectSearch();
  ShowMode(WorkspaceShell::SidebarMode::Search, temporary);
}

void SidebarCoordinator::ShowProblems() {
  RefreshProblems();
  ShowMode(WorkspaceShell::SidebarMode::Problems, false);
  RevealSelectedProblemsLine();
}

void SidebarCoordinator::ShowGit() {
  RefreshGit();
  ShowMode(WorkspaceShell::SidebarMode::Git, false);
  RevealSelectedGitLine();
}

bool SidebarCoordinator::ShowPlugin(std::string_view id, bool temporary) {
  const auto* provider = shell_.plugin_runtime_.Host().FindSidebarProvider(id);
  if (provider == nullptr) {
    return false;
  }

  shell_.sidebar_state_.view_id = provider->id;
  ShowMode(WorkspaceShell::SidebarMode::Plugin, temporary);
  return RefreshPlugin();
}

void SidebarCoordinator::Close() {
  const bool was_visible = shell_.sidebar_state_.visible;
  if (shell_.ActiveSidebarMode() == WorkspaceShell::SidebarMode::Search) {
    shell_.StopProjectSearch();
  }
  MenuCoordinator(shell_).CloseTreeContextMenu();

  if (shell_.sidebar_state_.temporary && !shell_.sidebar_state_.prev_view_id.empty()) {
    RestorePrevious();
    return;
  }

  shell_.sidebar_state_.visible = false;
  shell_.sidebar_state_.temporary = false;
  shell_.sidebar_state_.prev_view_id.clear();
  if (shell_.surface_.focus == WorkspaceShell::FocusTarget::Sidebar) {
    shell_.surface_.focus = WorkspaceShell::FocusTarget::Editor;
  }
  if (was_visible) {
    shell_.RequestWindowRedraw();
  }
}

void SidebarCoordinator::Toggle() {
  const bool was_visible = shell_.sidebar_state_.visible;
  if (shell_.sidebar_state_.visible) {
    Close();
    return;
  }

  if (shell_.ActiveSidebarMode() == WorkspaceShell::SidebarMode::None) {
    shell_.sidebar_state_.view_id = "tree";
  }
  shell_.sidebar_state_.visible = true;
  shell_.sidebar_state_.temporary = false;
  shell_.sidebar_state_.prev_view_id.clear();
  shell_.surface_.focus = WorkspaceShell::FocusTarget::Sidebar;
  if (!was_visible) {
    shell_.RequestWindowRedraw();
  }
}

void SidebarCoordinator::RestorePrevious() {
  if (shell_.ActiveSidebarMode() == WorkspaceShell::SidebarMode::Search &&
      shell_.SidebarModeForViewId(shell_.sidebar_state_.prev_view_id) !=
          WorkspaceShell::SidebarMode::Search) {
    shell_.StopProjectSearch();
  }

  if (shell_.sidebar_state_.prev_view_id.empty()) {
    shell_.sidebar_state_.temporary = false;
    return;
  }

  shell_.sidebar_state_.view_id = shell_.sidebar_state_.prev_view_id;
  if (shell_.SidebarModeForViewId(shell_.sidebar_state_.view_id) ==
      WorkspaceShell::SidebarMode::None) {
    shell_.sidebar_state_.view_id = "tree";
  }
  shell_.sidebar_state_.prev_view_id.clear();
  shell_.sidebar_state_.temporary = false;
  shell_.sidebar_state_.visible = true;
  shell_.surface_.focus = WorkspaceShell::FocusTarget::Sidebar;
  shell_.sidebar_state_.scroll_row = 0;
  if (shell_.ActiveSidebarMode() == WorkspaceShell::SidebarMode::Plugin) {
    RefreshPlugin();
  } else if (shell_.ActiveSidebarMode() == WorkspaceShell::SidebarMode::Problems) {
    RefreshProblems();
  }
  shell_.RequestSidebarRedraw();
}

void SidebarCoordinator::RefreshProjectFiles() {
  shell_.directory_tree_.Refresh();
  RevealSelectedTreeLine();
  shell_.file_index_.Refresh();
  shell_.file_finder_.SetIndex(&shell_.file_index_);
  RefreshGit();
  RefreshProblems();
  RefreshPlugin();
  if (shell_.sidebar_state_.visible) {
    shell_.RequestSidebarRedraw();
  }
}

void WorkspaceShell::ShowSidebarMode(SidebarMode mode, bool temporary) {
  SidebarCoordinator(*this).ShowMode(mode, temporary);
}

void WorkspaceShell::ShowTreeSidebar(const std::filesystem::path& root) {
  SidebarCoordinator(*this).ShowTree(root);
}

void WorkspaceShell::ShowSearchSidebar(std::string query, bool temporary) {
  SidebarCoordinator(*this).ShowSearch(std::move(query), temporary);
}

void WorkspaceShell::ShowProblemsSidebar() {
  SidebarCoordinator(*this).ShowProblems();
}

void WorkspaceShell::ShowGitSidebar() {
  SidebarCoordinator(*this).ShowGit();
}

bool WorkspaceShell::ShowPluginSidebar(std::string_view id, bool temporary) {
  return SidebarCoordinator(*this).ShowPlugin(id, temporary);
}

void WorkspaceShell::CloseSidebar() {
  SidebarCoordinator(*this).Close();
}

void WorkspaceShell::ToggleSidebar() {
  SidebarCoordinator(*this).Toggle();
}

void WorkspaceShell::RestorePreviousSidebar() {
  SidebarCoordinator(*this).RestorePrevious();
}

void WorkspaceShell::RefreshProjectFiles() {
  SidebarCoordinator(*this).RefreshProjectFiles();
}

void WorkspaceShell::RefreshGitSidebar() {
  SidebarCoordinator(*this).RefreshGit();
}

bool WorkspaceShell::RefreshProblemsSidebar() {
  return SidebarCoordinator(*this).RefreshProblems();
}

bool WorkspaceShell::RefreshPluginSidebar() {
  return SidebarCoordinator(*this).RefreshPlugin();
}

void WorkspaceShell::RevealSelectedGitSidebarLine() {
  SidebarCoordinator(*this).RevealSelectedGitLine();
}

void WorkspaceShell::RevealSelectedProblemsSidebarLine() {
  SidebarCoordinator(*this).RevealSelectedProblemsLine();
}

void WorkspaceShell::RevealSelectedTreeSidebarLine() {
  SidebarCoordinator(*this).RevealSelectedTreeLine();
}

void WorkspaceShell::RevealSelectedPluginSidebarLine() {
  SidebarCoordinator(*this).RevealSelectedPluginLine();
}

void WorkspaceShell::MoveGitSidebarSelection(int delta) {
  SidebarCoordinator(*this).MoveGitSelection(delta);
}

void WorkspaceShell::MoveProblemsSidebarSelection(int delta) {
  SidebarCoordinator(*this).MoveProblemsSelection(delta);
}

void WorkspaceShell::MovePluginSidebarSelection(int delta) {
  SidebarCoordinator(*this).MovePluginSelection(delta);
}

bool WorkspaceShell::OpenGitSidebarEntry(std::size_t entry_index) {
  return SidebarCoordinator(*this).OpenGitEntry(entry_index);
}

bool WorkspaceShell::OpenSelectedProblemSidebarItem() {
  return SidebarCoordinator(*this).OpenProblemItem();
}

bool WorkspaceShell::OpenSelectedPluginSidebarItem() {
  return SidebarCoordinator(*this).OpenPluginItem();
}

bool WorkspaceShell::CanStageAllGitSidebarEntries() const {
  return SidebarCoordinator(*const_cast<WorkspaceShell*>(this)).CanStageAllGitEntries();
}

bool WorkspaceShell::CanDiscardAllGitSidebarEntries() const {
  return SidebarCoordinator(*const_cast<WorkspaceShell*>(this)).CanDiscardAllGitEntries();
}

bool WorkspaceShell::StageAllGitSidebarEntries() {
  return SidebarCoordinator(*this).StageAllGitEntries();
}

void WorkspaceShell::OpenDiscardAllGitSidebarPrompt() {
  SidebarCoordinator(*this).OpenDiscardAllGitPrompt();
}

bool WorkspaceShell::DiscardAllGitSidebarEntries() {
  return SidebarCoordinator(*this).DiscardAllGitEntries();
}

bool WorkspaceShell::StageGitSidebarEntry(std::size_t entry_index) {
  return SidebarCoordinator(*this).StageGitEntry(entry_index);
}

bool WorkspaceShell::UnstageGitSidebarEntry(std::size_t entry_index) {
  return SidebarCoordinator(*this).UnstageGitEntry(entry_index);
}

bool WorkspaceShell::DiscardGitSidebarEntry(std::size_t entry_index) {
  return SidebarCoordinator(*this).DiscardGitEntry(entry_index);
}

void WorkspaceShell::ReconcileOpenTabsAfterPathDiscard(const std::filesystem::path& path) {
  SidebarCoordinator(*this).ReconcileOpenTabsAfterPathDiscard(path);
}

}  // namespace microide::workspace
