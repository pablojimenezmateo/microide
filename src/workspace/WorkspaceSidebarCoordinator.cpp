#include "workspace/WorkspaceSidebarCoordinator.h"

#include <utility>

#include "workspace/WorkspaceMenuCoordinator.h"

namespace microide::workspace {

WorkspaceShell::SidebarCoordinator::SidebarCoordinator(WorkspaceShell& shell) : shell_(shell) {}

void WorkspaceShell::SidebarCoordinator::ShowMode(SidebarMode mode, bool temporary) {
  if (mode == SidebarMode::None) {
    Close();
    return;
  }
  if (mode != SidebarMode::Tree) {
    MenuCoordinator(shell_).CloseTreeContextMenu();
  }

  if (shell_.surface_.sidebar_mode == SidebarMode::Search && mode != SidebarMode::Search) {
    shell_.StopProjectSearch();
  }

  if (temporary) {
    if (!shell_.surface_.sidebar_temporary && shell_.surface_.sidebar_visible) {
      shell_.surface_.sidebar_prev_mode = shell_.surface_.sidebar_mode;
      shell_.surface_.sidebar_prev_plugin_id = shell_.surface_.sidebar_plugin_id;
    }
  } else {
    shell_.surface_.sidebar_prev_mode = SidebarMode::None;
    shell_.surface_.sidebar_prev_plugin_id.clear();
  }

  if (mode != SidebarMode::Plugin) {
    shell_.surface_.sidebar_plugin_id.clear();
  }
  shell_.surface_.sidebar_mode = mode;
  shell_.surface_.sidebar_temporary = temporary;
  shell_.surface_.sidebar_visible = true;
  shell_.surface_.focus = FocusTarget::Sidebar;
  shell_.surface_.sidebar_scroll_row = 0;
  shell_.RequestWindowRedraw();
}

void WorkspaceShell::SidebarCoordinator::ShowTree(const std::filesystem::path& root) {
  if (!root.empty()) {
    if (!shell_.OpenProjectTab(root, true, true)) {
      return;
    }
  }

  ShowMode(SidebarMode::Tree, false);
}

void WorkspaceShell::SidebarCoordinator::ShowSearch(std::string query, bool temporary) {
  if (!query.empty() || shell_.overlay_workflow_.project_search.query.empty()) {
    shell_.overlay_workflow_.project_search.query = std::move(query);
  }
  shell_.overlay_workflow_.project_search.edit_buffer = shell_.overlay_workflow_.project_search.query;
  shell_.overlay_workflow_.project_search.editing =
      shell_.overlay_workflow_.project_search.query.empty();
  shell_.overlay_workflow_.project_search.edit_field = ProjectSearchEditField::Query;
  shell_.overlay_workflow_.project_search.selected_index = 0;
  shell_.RefreshProjectSearch();
  ShowMode(SidebarMode::Search, temporary);
}

void WorkspaceShell::SidebarCoordinator::ShowProblems() {
  RefreshProblems();
  ShowMode(SidebarMode::Problems, false);
  RevealSelectedProblemsLine();
}

void WorkspaceShell::SidebarCoordinator::ShowGit() {
  RefreshGit();
  ShowMode(SidebarMode::Git, false);
  RevealSelectedGitLine();
}

bool WorkspaceShell::SidebarCoordinator::ShowPlugin(std::string_view id, bool temporary) {
  const auto* provider = shell_.plugin_runtime_.Host().FindSidebarProvider(id);
  if (provider == nullptr) {
    return false;
  }

  shell_.surface_.sidebar_plugin_id = provider->id;
  ShowMode(SidebarMode::Plugin, temporary);
  return RefreshPlugin();
}

void WorkspaceShell::SidebarCoordinator::Close() {
  const bool was_visible = shell_.surface_.sidebar_visible;
  if (shell_.surface_.sidebar_mode == SidebarMode::Search) {
    shell_.StopProjectSearch();
  }
  MenuCoordinator(shell_).CloseTreeContextMenu();

  if (shell_.surface_.sidebar_temporary && shell_.surface_.sidebar_prev_mode != SidebarMode::None) {
    RestorePrevious();
    return;
  }

  shell_.surface_.sidebar_visible = false;
  shell_.surface_.sidebar_temporary = false;
  shell_.surface_.sidebar_prev_mode = SidebarMode::None;
  shell_.surface_.sidebar_prev_plugin_id.clear();
  if (shell_.surface_.focus == FocusTarget::Sidebar) {
    shell_.surface_.focus = FocusTarget::Editor;
  }
  if (was_visible) {
    shell_.RequestWindowRedraw();
  }
}

void WorkspaceShell::SidebarCoordinator::Toggle() {
  const bool was_visible = shell_.surface_.sidebar_visible;
  if (shell_.surface_.sidebar_visible) {
    Close();
    return;
  }

  if (shell_.surface_.sidebar_mode == SidebarMode::None) {
    shell_.surface_.sidebar_mode = SidebarMode::Tree;
  }
  shell_.surface_.sidebar_visible = true;
  shell_.surface_.sidebar_temporary = false;
  shell_.surface_.sidebar_prev_plugin_id.clear();
  shell_.surface_.focus = FocusTarget::Sidebar;
  if (!was_visible) {
    shell_.RequestWindowRedraw();
  }
}

void WorkspaceShell::SidebarCoordinator::RestorePrevious() {
  if (shell_.surface_.sidebar_mode == SidebarMode::Search &&
      shell_.surface_.sidebar_prev_mode != SidebarMode::Search) {
    shell_.StopProjectSearch();
  }

  if (shell_.surface_.sidebar_prev_mode == SidebarMode::None) {
    shell_.surface_.sidebar_temporary = false;
    return;
  }

  shell_.surface_.sidebar_mode = shell_.surface_.sidebar_prev_mode;
  shell_.surface_.sidebar_plugin_id = shell_.surface_.sidebar_prev_plugin_id;
  shell_.surface_.sidebar_prev_mode = SidebarMode::None;
  shell_.surface_.sidebar_prev_plugin_id.clear();
  shell_.surface_.sidebar_temporary = false;
  shell_.surface_.sidebar_visible = true;
  shell_.surface_.focus = FocusTarget::Sidebar;
  shell_.surface_.sidebar_scroll_row = 0;
  if (shell_.surface_.sidebar_mode == SidebarMode::Plugin) {
    RefreshPlugin();
  } else if (shell_.surface_.sidebar_mode == SidebarMode::Problems) {
    RefreshProblems();
  }
  shell_.RequestSidebarRedraw();
}

void WorkspaceShell::SidebarCoordinator::RefreshProjectFiles() {
  shell_.directory_tree_.Refresh();
  RevealSelectedTreeLine();
  shell_.file_index_.Refresh();
  shell_.file_finder_.SetIndex(&shell_.file_index_);
  RefreshGit();
  RefreshProblems();
  RefreshPlugin();
  if (shell_.surface_.sidebar_visible) {
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
