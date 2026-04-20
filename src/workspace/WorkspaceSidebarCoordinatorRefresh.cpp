#include "workspace/WorkspaceSidebarCoordinator.h"

#include <algorithm>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>

#include "project/GitStatusService.h"
#include "workspace/WorkspaceLayout.h"
#include "workspace/WorkspacePathUtils.h"
#include "workspace/WorkspaceTextSearch.h"

namespace microide::workspace {

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
      if (shell_.sidebar_state_.visible && shell_.sidebar_state_.mode == SidebarMode::Git) {
        shell_.RequestSidebarRedraw();
      }
      return;
    }
  }

  RevealSelectedGitLine();
  if (shell_.sidebar_state_.visible && shell_.sidebar_state_.mode == SidebarMode::Git) {
    shell_.RequestSidebarRedraw();
  }
}

bool WorkspaceShell::SidebarCoordinator::RefreshProblems() {
  std::optional<editor::PublishedDiagnostic> previous_diagnostic;
  if (shell_.problems_sidebar_.selected_index < shell_.problems_sidebar_.entries.size()) {
    previous_diagnostic =
        shell_.problems_sidebar_.entries[shell_.problems_sidebar_.selected_index].diagnostic;
  }

  shell_.problems_sidebar_.entries.clear();
  shell_.problems_sidebar_.selected_index = 0;
  for (const auto& diagnostic : shell_.diagnostics_store_.SnapshotAll()) {
    ProblemsSidebarEntry entry;
    const std::string collapsed_message = CollapseWhitespace(diagnostic.message);
    entry.diagnostic = diagnostic;
    entry.primary_label = collapsed_message.empty() ? "Diagnostic" : collapsed_message;
    entry.detail_label =
        RelativePathLabel(shell_.project_root_, diagnostic.path) + ":" +
        std::to_string(diagnostic.range.start.line + 1) + ":" +
        std::to_string(diagnostic.range.start.column + 1);
    if (!diagnostic.owner.empty()) {
      entry.detail_label += " | " + diagnostic.owner;
    }
    shell_.problems_sidebar_.entries.push_back(std::move(entry));
  }

  if (previous_diagnostic.has_value()) {
    for (std::size_t i = 0; i < shell_.problems_sidebar_.entries.size(); ++i) {
      if (shell_.problems_sidebar_.entries[i].diagnostic == *previous_diagnostic) {
        shell_.problems_sidebar_.selected_index = i;
        break;
      }
    }
  }

  RevealSelectedProblemsLine();
  if (shell_.sidebar_state_.visible && shell_.sidebar_state_.mode == SidebarMode::Problems) {
    shell_.RequestSidebarRedraw();
  }
  return !shell_.problems_sidebar_.entries.empty();
}

bool WorkspaceShell::SidebarCoordinator::RefreshPlugin() {
  shell_.plugin_sidebar_.items.clear();
  shell_.plugin_sidebar_.error.clear();
  shell_.plugin_sidebar_.selected_index = 0;
  if (shell_.sidebar_state_.mode != SidebarMode::Plugin) {
    return false;
  }
  if (shell_.sidebar_state_.view_id.empty() ||
      shell_.plugin_runtime_.Host().FindSidebarProvider(shell_.sidebar_state_.view_id) ==
          nullptr) {
    shell_.sidebar_state_.mode = SidebarMode::Tree;
    shell_.sidebar_state_.view_id = "tree";
    if (shell_.sidebar_state_.visible) {
      shell_.RequestSidebarRedraw();
    }
    return false;
  }

  std::string error_message;
  if (!shell_.plugin_runtime_.Host().SnapshotSidebar(shell_.sidebar_state_.view_id,
                                                     &shell_.plugin_sidebar_.items,
                                                     &error_message)) {
    shell_.plugin_sidebar_.error = std::move(error_message);
    if (shell_.sidebar_state_.visible && shell_.sidebar_state_.mode == SidebarMode::Plugin) {
      shell_.RequestSidebarRedraw();
    }
    return false;
  }
  if (!shell_.plugin_sidebar_.items.empty()) {
    shell_.plugin_sidebar_.selected_index = std::min(
        shell_.plugin_sidebar_.selected_index, shell_.plugin_sidebar_.items.size() - 1);
  }
  RevealSelectedPluginLine();
  if (shell_.sidebar_state_.visible && shell_.sidebar_state_.mode == SidebarMode::Plugin) {
    shell_.RequestSidebarRedraw();
  }
  return true;
}

void WorkspaceShell::SidebarCoordinator::RevealSelectedTreeLine() {
  const auto& entries = shell_.directory_tree_.entries();
  if (shell_.directory_tree_.selected_index() >= entries.size()) {
    return;
  }

  const auto layout_state = shell_.CurrentWorkspaceLayout();
  if (!layout_state.has_value()) {
    return;
  }
  const WorkspaceLayout layout = *layout_state;
  if (layout.sidebar.h <= 0.0f) {
    return;
  }

  const auto list_layout = shell_.ComputeTreeSidebarListLayout(layout.sidebar, entries.size());
  shell_.sidebar_state_.scroll_row = RevealScrollableListIndex(
      list_layout, static_cast<int>(shell_.directory_tree_.selected_index()));
}

void WorkspaceShell::SidebarCoordinator::RevealSelectedGitLine() {
  const auto selected_line = shell_.SelectedGitSidebarLineIndex();
  if (!selected_line.has_value()) {
    return;
  }

  const auto layout_state = shell_.CurrentWorkspaceLayout();
  if (!layout_state.has_value()) {
    return;
  }
  const WorkspaceLayout layout = *layout_state;
  if (layout.sidebar.h <= 0.0f) {
    return;
  }
  const auto lines = shell_.BuildGitSidebarLines();
  const auto list_layout = shell_.ComputeGitSidebarListLayout(layout.sidebar, lines.size());
  shell_.sidebar_state_.scroll_row =
      RevealScrollableListIndex(list_layout, static_cast<int>(*selected_line));
}

void WorkspaceShell::SidebarCoordinator::RevealSelectedProblemsLine() {
  if (shell_.problems_sidebar_.entries.empty() ||
      shell_.problems_sidebar_.selected_index >= shell_.problems_sidebar_.entries.size()) {
    return;
  }
  const auto layout_state = shell_.CurrentWorkspaceLayout();
  if (!layout_state.has_value()) {
    return;
  }
  const WorkspaceLayout layout = *layout_state;
  if (layout.sidebar.h <= 0.0f) {
    return;
  }
  const auto list_layout = shell_.ComputeProblemsSidebarListLayout(
      layout.sidebar, shell_.problems_sidebar_.entries.size());
  shell_.sidebar_state_.scroll_row = RevealScrollableListIndex(
      list_layout, static_cast<int>(shell_.problems_sidebar_.selected_index));
}

void WorkspaceShell::SidebarCoordinator::RevealSelectedPluginLine() {
  if (shell_.plugin_sidebar_.items.empty() ||
      shell_.plugin_sidebar_.selected_index >= shell_.plugin_sidebar_.items.size()) {
    return;
  }
  const auto layout_state = shell_.CurrentWorkspaceLayout();
  if (!layout_state.has_value()) {
    return;
  }
  const WorkspaceLayout layout = *layout_state;
  if (layout.sidebar.h <= 0.0f) {
    return;
  }
  const auto list_layout =
      shell_.ComputePluginSidebarListLayout(layout.sidebar, shell_.plugin_sidebar_.items.size());
  shell_.sidebar_state_.scroll_row = RevealScrollableListIndex(
      list_layout, static_cast<int>(shell_.plugin_sidebar_.selected_index));
}

}  // namespace microide::workspace
