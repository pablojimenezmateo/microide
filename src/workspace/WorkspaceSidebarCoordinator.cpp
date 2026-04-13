#include "workspace/WorkspaceSidebarCoordinator.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <string>
#include <vector>

#include "project/GitStatusService.h"

namespace microide::workspace {

WorkspaceShell::SidebarCoordinator::SidebarCoordinator(WorkspaceShell& shell) : shell_(shell) {}

void WorkspaceShell::SidebarCoordinator::ShowMode(SidebarMode mode, bool temporary) {
  if (mode == SidebarMode::None) {
    Close();
    return;
  }
  if (mode != SidebarMode::Tree) {
    shell_.CloseTreeContextMenu();
  }

  if (shell_.surface_.sidebar_mode == SidebarMode::Search && mode != SidebarMode::Search) {
    shell_.StopProjectSearch();
  }

  if (temporary) {
    if (!shell_.surface_.sidebar_temporary && shell_.surface_.sidebar_visible) {
      shell_.surface_.sidebar_prev_mode = shell_.surface_.sidebar_mode;
    }
  } else {
    shell_.surface_.sidebar_prev_mode = SidebarMode::None;
  }

  shell_.surface_.sidebar_mode = mode;
  shell_.surface_.sidebar_temporary = temporary;
  shell_.surface_.sidebar_visible = true;
  shell_.surface_.focus = FocusTarget::Sidebar;
  shell_.surface_.sidebar_scroll_row = 0;
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

void WorkspaceShell::SidebarCoordinator::ShowGit() {
  RefreshGit();
  ShowMode(SidebarMode::Git, false);
  RevealSelectedGitLine();
}

void WorkspaceShell::SidebarCoordinator::Close() {
  if (shell_.surface_.sidebar_mode == SidebarMode::Search) {
    shell_.StopProjectSearch();
  }
  shell_.CloseTreeContextMenu();

  if (shell_.surface_.sidebar_temporary && shell_.surface_.sidebar_prev_mode != SidebarMode::None) {
    RestorePrevious();
    return;
  }

  shell_.surface_.sidebar_visible = false;
  shell_.surface_.sidebar_temporary = false;
  shell_.surface_.sidebar_prev_mode = SidebarMode::None;
  if (shell_.surface_.focus == FocusTarget::Sidebar) {
    shell_.surface_.focus = FocusTarget::Editor;
  }
}

void WorkspaceShell::SidebarCoordinator::Toggle() {
  if (shell_.surface_.sidebar_visible) {
    Close();
    return;
  }

  if (shell_.surface_.sidebar_mode == SidebarMode::None) {
    shell_.surface_.sidebar_mode = SidebarMode::Tree;
  }
  shell_.surface_.sidebar_visible = true;
  shell_.surface_.sidebar_temporary = false;
  shell_.surface_.focus = FocusTarget::Sidebar;
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
  shell_.surface_.sidebar_prev_mode = SidebarMode::None;
  shell_.surface_.sidebar_temporary = false;
  shell_.surface_.sidebar_visible = true;
  shell_.surface_.focus = FocusTarget::Sidebar;
  shell_.surface_.sidebar_scroll_row = 0;
}

void WorkspaceShell::SidebarCoordinator::RefreshProjectFiles() {
  shell_.directory_tree_.Refresh();
  shell_.file_index_.Refresh();
  shell_.file_finder_.SetIndex(&shell_.file_index_);
  RefreshGit();
}

void WorkspaceShell::SidebarCoordinator::RefreshGit() {
  const std::filesystem::path previous_path =
      shell_.git_sidebar_.selected_index < shell_.git_sidebar_.entries.size()
          ? shell_.git_sidebar_.entries[shell_.git_sidebar_.selected_index].path
          : std::filesystem::path{};
  const GitSidebarEntry::Section previous_section =
      shell_.git_sidebar_.selected_index < shell_.git_sidebar_.entries.size()
          ? shell_.git_sidebar_.entries[shell_.git_sidebar_.selected_index].section
          : GitSidebarEntry::Section::Modified;

  shell_.git_sidebar_.entries.clear();
  shell_.git_sidebar_.base_ref.clear();
  shell_.git_sidebar_.base_label.clear();
  shell_.git_sidebar_.repo_available = false;
  shell_.git_sidebar_.selected_index = 0;
  if (shell_.project_root_.empty()) {
    return;
  }

  const auto working_entries = project::CollectGitWorkingTreeEntries(shell_.project_root_);
  for (const auto& entry : working_entries) {
    shell_.git_sidebar_.entries.push_back(GitSidebarEntry{
        .section = GitSidebarEntry::Section::Modified,
        .path = (shell_.project_root_ / entry.relative_path).lexically_normal(),
        .relative_path = entry.relative_path,
        .status = entry.conflicted ? project::GitFileStatus::Conflicted : entry.status,
        .conflicted = entry.conflicted,
        .staged = entry.staged,
    });
  }

  const auto base_ref = project::ResolveGitBaseReference(shell_.project_root_);
  if (base_ref.has_value()) {
    shell_.git_sidebar_.repo_available = true;
    shell_.git_sidebar_.base_ref = base_ref->ref;
    shell_.git_sidebar_.base_label = base_ref->label;
    const auto outgoing_entries =
        project::CollectGitBranchOutgoingFiles(shell_.project_root_, shell_.git_sidebar_.base_ref);
    for (const auto& entry : outgoing_entries) {
      shell_.git_sidebar_.entries.push_back(GitSidebarEntry{
          .section = GitSidebarEntry::Section::Outgoing,
          .path = (shell_.project_root_ / entry.relative_path).lexically_normal(),
          .relative_path = entry.relative_path,
          .status = entry.status,
      });
    }
  } else {
    shell_.git_sidebar_.repo_available = std::filesystem::exists(shell_.project_root_ / ".git");
  }

  for (std::size_t i = 0; i < shell_.git_sidebar_.entries.size(); ++i) {
    if (shell_.git_sidebar_.entries[i].path == previous_path &&
        shell_.git_sidebar_.entries[i].section == previous_section) {
      shell_.git_sidebar_.selected_index = i;
      RevealSelectedGitLine();
      return;
    }
  }

  RevealSelectedGitLine();
}

void WorkspaceShell::SidebarCoordinator::RevealSelectedGitLine() {
  if (shell_.last_window_width_ <= 0 || shell_.last_window_height_ <= 0) {
    return;
  }

  const auto selected_line = shell_.SelectedGitSidebarLineIndex();
  if (!selected_line.has_value()) {
    return;
  }

  const WorkspaceLayout layout =
      ComputeLayout(static_cast<float>(shell_.last_window_width_),
                    static_cast<float>(shell_.last_window_height_),
                    shell_.surface_.sidebar_visible, shell_.BottomPanelVisible(),
                    shell_.surface_.sidebar_width, shell_.surface_.bottom_panel_height);
  if (layout.sidebar.h <= 0.0f) {
    return;
  }
  const auto lines = shell_.BuildGitSidebarLines();
  const auto list_layout = shell_.ComputeGitSidebarListLayout(layout.sidebar, lines.size());
  shell_.surface_.sidebar_scroll_row =
      RevealScrollableListIndex(list_layout, static_cast<int>(*selected_line));
}

void WorkspaceShell::SidebarCoordinator::MoveGitSelection(int delta) {
  if (shell_.git_sidebar_.entries.empty() || delta == 0) {
    return;
  }
  const int current = static_cast<int>(shell_.git_sidebar_.selected_index);
  const int max_index = static_cast<int>(shell_.git_sidebar_.entries.size()) - 1;
  shell_.git_sidebar_.selected_index =
      static_cast<std::size_t>(std::clamp(current + delta, 0, max_index));
  RevealSelectedGitLine();
}

bool WorkspaceShell::SidebarCoordinator::OpenGitEntry(std::size_t entry_index) {
  if (entry_index >= shell_.git_sidebar_.entries.size()) {
    return false;
  }
  const auto& entry = shell_.git_sidebar_.entries[entry_index];
  if (entry.section == GitSidebarEntry::Section::Modified) {
    if (entry.conflicted) {
      return shell_.OpenGitConflictMerge(entry.path);
    }
    return shell_.OpenWorkingTreeComparison(entry.path, "HEAD", "HEAD");
  }
  if (shell_.git_sidebar_.base_ref.empty()) {
    return false;
  }
  return shell_.OpenBranchHeadComparison(
      entry.path, shell_.git_sidebar_.base_ref,
      shell_.git_sidebar_.base_label.empty() ? shell_.git_sidebar_.base_ref
                                             : shell_.git_sidebar_.base_label,
      "HEAD", "HEAD");
}

bool WorkspaceShell::SidebarCoordinator::CanStageAllGitEntries() const {
  return std::any_of(shell_.git_sidebar_.entries.begin(), shell_.git_sidebar_.entries.end(),
                     [](const auto& entry) {
                       return entry.section == GitSidebarEntry::Section::Modified && !entry.staged;
                     });
}

bool WorkspaceShell::SidebarCoordinator::CanDiscardAllGitEntries() const {
  return std::any_of(shell_.git_sidebar_.entries.begin(), shell_.git_sidebar_.entries.end(),
                     [](const auto& entry) {
                       return entry.section == GitSidebarEntry::Section::Modified;
                     });
}

bool WorkspaceShell::SidebarCoordinator::StageAllGitEntries() {
  if (!CanStageAllGitEntries()) {
    return false;
  }
  std::vector<std::filesystem::path> affected_paths;
  affected_paths.reserve(shell_.git_sidebar_.entries.size());
  for (const auto& entry : shell_.git_sidebar_.entries) {
    if (entry.section != GitSidebarEntry::Section::Modified || entry.staged) {
      continue;
    }
    affected_paths.push_back(entry.path.lexically_normal());
  }
  std::sort(affected_paths.begin(), affected_paths.end());
  affected_paths.erase(std::unique(affected_paths.begin(), affected_paths.end()),
                       affected_paths.end());
  if (!project::GitStageAll(shell_.project_root_)) {
    return false;
  }
  for (const auto& path : affected_paths) {
    shell_.InvalidateEditorBlamePath(path);
  }
  RefreshProjectFiles();
  return true;
}

void WorkspaceShell::SidebarCoordinator::OpenDiscardAllGitPrompt() {
  if (!CanDiscardAllGitEntries()) {
    return;
  }
  shell_.OpenPromptSurface(PromptSurfaceState::Action::DiscardGitChanges,
                           PromptSurfaceState::Kind::Confirm, shell_.project_root_);
}

bool WorkspaceShell::SidebarCoordinator::DiscardAllGitEntries() {
  if (!CanDiscardAllGitEntries()) {
    return false;
  }

  std::string blocking_label;
  if (shell_.HasDirtyEditorTabsForPath(shell_.project_root_, &blocking_label)) {
    return false;
  }

  std::vector<std::filesystem::path> affected_paths;
  affected_paths.reserve(shell_.git_sidebar_.entries.size());
  for (const auto& entry : shell_.git_sidebar_.entries) {
    if (entry.section != GitSidebarEntry::Section::Modified) {
      continue;
    }
    affected_paths.push_back(entry.path.lexically_normal());
  }
  std::sort(affected_paths.begin(), affected_paths.end());
  affected_paths.erase(std::unique(affected_paths.begin(), affected_paths.end()),
                       affected_paths.end());

  if (!project::GitDiscardAll(shell_.project_root_)) {
    return false;
  }

  for (const auto& path : affected_paths) {
    shell_.InvalidateEditorBlamePath(path);
    ReconcileOpenTabsAfterPathDiscard(path);
  }
  RefreshProjectFiles();
  return true;
}

bool WorkspaceShell::SidebarCoordinator::StageGitEntry(std::size_t entry_index) {
  if (entry_index >= shell_.git_sidebar_.entries.size()) {
    return false;
  }
  const auto& entry = shell_.git_sidebar_.entries[entry_index];
  if (entry.section != GitSidebarEntry::Section::Modified || entry.staged) {
    return false;
  }
  if (!project::GitStagePath(shell_.project_root_, entry.path)) {
    return false;
  }
  shell_.InvalidateEditorBlamePath(entry.path);
  RefreshProjectFiles();
  return true;
}

bool WorkspaceShell::SidebarCoordinator::UnstageGitEntry(std::size_t entry_index) {
  if (entry_index >= shell_.git_sidebar_.entries.size()) {
    return false;
  }
  const auto& entry = shell_.git_sidebar_.entries[entry_index];
  if (entry.section != GitSidebarEntry::Section::Modified || !entry.staged) {
    return false;
  }
  if (!project::GitUnstagePath(shell_.project_root_, entry.path)) {
    return false;
  }
  shell_.InvalidateEditorBlamePath(entry.path);
  RefreshProjectFiles();
  return true;
}

bool WorkspaceShell::SidebarCoordinator::DiscardGitEntry(std::size_t entry_index) {
  if (entry_index >= shell_.git_sidebar_.entries.size()) {
    return false;
  }
  const auto& entry = shell_.git_sidebar_.entries[entry_index];
  if (entry.section != GitSidebarEntry::Section::Modified) {
    return false;
  }

  std::string blocking_label;
  if (shell_.HasDirtyEditorTabsForPath(entry.path, &blocking_label)) {
    return false;
  }
  if (!project::GitDiscardPath(shell_.project_root_, entry.path)) {
    return false;
  }
  shell_.InvalidateEditorBlamePath(entry.path);
  ReconcileOpenTabsAfterPathDiscard(entry.path);
  RefreshProjectFiles();
  return true;
}

void WorkspaceShell::SidebarCoordinator::ReconcileOpenTabsAfterPathDiscard(
    const std::filesystem::path& path) {
  const std::filesystem::path normalized_path = path.lexically_normal();
  std::error_code error;
  if (std::filesystem::exists(normalized_path, error) && !error) {
    shell_.ReloadCleanEditorTabsForPath(normalized_path);
    return;
  }
  shell_.CloseOpenTabsForPath(normalized_path);
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

void WorkspaceShell::ShowGitSidebar() {
  SidebarCoordinator(*this).ShowGit();
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

void WorkspaceShell::RevealSelectedGitSidebarLine() {
  SidebarCoordinator(*this).RevealSelectedGitLine();
}

void WorkspaceShell::MoveGitSidebarSelection(int delta) {
  SidebarCoordinator(*this).MoveGitSelection(delta);
}

bool WorkspaceShell::OpenGitSidebarEntry(std::size_t entry_index) {
  return SidebarCoordinator(*this).OpenGitEntry(entry_index);
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
