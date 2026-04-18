#include "workspace/WorkspaceSidebarCoordinator.h"

#include <algorithm>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include "project/GitStatusService.h"

namespace microide::workspace {

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

void WorkspaceShell::SidebarCoordinator::MoveProblemsSelection(int delta) {
  if (shell_.problems_sidebar_.entries.empty() || delta == 0) {
    return;
  }
  const int current = static_cast<int>(shell_.problems_sidebar_.selected_index);
  const int max_index = static_cast<int>(shell_.problems_sidebar_.entries.size()) - 1;
  shell_.problems_sidebar_.selected_index =
      static_cast<std::size_t>(std::clamp(current + delta, 0, max_index));
  RevealSelectedProblemsLine();
}

void WorkspaceShell::SidebarCoordinator::MovePluginSelection(int delta) {
  if (shell_.plugin_sidebar_.items.empty() || delta == 0) {
    return;
  }
  const int current = static_cast<int>(shell_.plugin_sidebar_.selected_index);
  const int max_index = static_cast<int>(shell_.plugin_sidebar_.items.size()) - 1;
  shell_.plugin_sidebar_.selected_index =
      static_cast<std::size_t>(std::clamp(current + delta, 0, max_index));
  RevealSelectedPluginLine();
}

bool WorkspaceShell::SidebarCoordinator::OpenGitEntry(std::size_t entry_index) {
  if (entry_index >= shell_.git_sidebar_.entries.size()) {
    return false;
  }
  const auto& entry = shell_.git_sidebar_.entries[entry_index];
  bool opened = false;
  if (entry.section == GitSidebarEntry::Section::Modified) {
    if (entry.conflicted) {
      opened = shell_.OpenGitConflictMerge(entry.path);
    } else {
      opened = shell_.OpenWorkingTreeComparison(entry.path, "HEAD", "HEAD");
    }
  } else {
    if (shell_.git_sidebar_.base_ref.empty()) {
      return false;
    }
    opened = shell_.OpenBranchHeadComparison(
        entry.path, shell_.git_sidebar_.base_ref,
        shell_.git_sidebar_.base_label.empty() ? shell_.git_sidebar_.base_ref
                                               : shell_.git_sidebar_.base_label,
        "HEAD", "HEAD");
  }
  if (opened && shell_.surface_.sidebar_visible &&
      shell_.surface_.sidebar_mode == SidebarMode::Git) {
    shell_.RequestSidebarRedraw();
  }
  return opened;
}

bool WorkspaceShell::SidebarCoordinator::OpenProblemItem() {
  if (shell_.problems_sidebar_.entries.empty() ||
      shell_.problems_sidebar_.selected_index >= shell_.problems_sidebar_.entries.size()) {
    return false;
  }
  const auto& entry = shell_.problems_sidebar_.entries[shell_.problems_sidebar_.selected_index];
  if (entry.diagnostic.path.empty()) {
    return false;
  }
  shell_.OpenFile(entry.diagnostic.path);
  shell_.text_viewport_.MoveCursorTo(entry.diagnostic.range.start.line,
                                     entry.diagnostic.range.start.column);
  if (shell_.surface_.sidebar_temporary) {
    shell_.RestorePreviousSidebar();
  }
  shell_.surface_.focus = FocusTarget::Editor;
  return true;
}

bool WorkspaceShell::SidebarCoordinator::OpenPluginItem() {
  if (shell_.plugin_sidebar_.items.empty() ||
      shell_.plugin_sidebar_.selected_index >= shell_.plugin_sidebar_.items.size()) {
    return false;
  }
  const auto& item = shell_.plugin_sidebar_.items[shell_.plugin_sidebar_.selected_index];
  std::string error_message;
  const bool confirmed = shell_.plugin_host_.ConfirmSidebarItem(
      shell_.surface_.sidebar_plugin_id, item, &error_message);
  if (!confirmed && !error_message.empty()) {
    shell_.plugin_sidebar_.error = std::move(error_message);
  }
  if (confirmed && shell_.surface_.sidebar_temporary) {
    shell_.RestorePreviousSidebar();
  }
  if (confirmed && !item.path.empty()) {
    shell_.surface_.focus = FocusTarget::Editor;
  }
  return confirmed;
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

}  // namespace microide::workspace
