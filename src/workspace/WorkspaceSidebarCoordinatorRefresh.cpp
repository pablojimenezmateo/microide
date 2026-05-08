#include "workspace/WorkspaceSidebarCoordinator.h"

#include <algorithm>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>

#include "project/GitRepository.h"
#include "project/GitStatusService.h"
#include "workspace/WorkspaceGitOutgoingBase.h"
#include "workspace/WorkspaceLayout.h"
#include "workspace/WorkspacePathUtils.h"
#include "workspace/WorkspaceSidebarRegistry.h"
#include "workspace/WorkspaceTextSearch.h"

namespace microide::workspace {

namespace {

std::string ResolveGitBranchLabelFallback(const std::filesystem::path& project_root) {
  const project::GitRepository repo(project_root);
  if (!repo.IsValid()) {
    return {};
  }
  if (const auto symbolic_ref = repo.Execute({"symbolic-ref", "--short", "HEAD"});
      symbolic_ref.success()) {
    std::string label = symbolic_ref.output;
    while (!label.empty() && (label.back() == '\n' || label.back() == '\r')) {
      label.pop_back();
    }
    if (!label.empty()) {
      return label;
    }
  }
  return {};
}

}  // namespace

void SidebarCoordinator::RefreshGit() {
  const std::filesystem::path previous_path =
      state_.sidebar.git.selected_index < state_.sidebar.git.entries.size()
          ? state_.sidebar.git.entries[state_.sidebar.git.selected_index].path
          : std::filesystem::path{};
  const GitSidebarEntry::Section previous_section =
      state_.sidebar.git.selected_index < state_.sidebar.git.entries.size()
          ? state_.sidebar.git.entries[state_.sidebar.git.selected_index].section
          : GitSidebarEntry::Section::Modified;

  state_.sidebar.git.entries.clear();
  state_.sidebar.git.branch_label.clear();
  state_.sidebar.git.base_ref.clear();
  state_.sidebar.git.base_label.clear();
  state_.sidebar.git.repo_available = false;
  state_.sidebar.git.selected_index = 0;
  if (project_root_.empty()) {
    return;
  }

  GitSidebarState::RefreshSnapshot snapshot;
  bool has_snapshot =
      operations_.consume_git_refresh_snapshot != nullptr &&
      operations_.consume_git_refresh_snapshot(&snapshot);
  if (!has_snapshot) {
    const auto working_entries = project::CollectGitWorkingTreeEntries(project_root_);
    for (const auto& entry : working_entries) {
      snapshot.entries.push_back(GitSidebarState::RefreshSnapshotEntry{
          .section = GitSidebarEntry::Section::Modified,
          .relative_path = entry.relative_path,
          .status = entry.status,
          .conflicted = entry.conflicted,
          .staged = entry.staged,
      });
    }
    const ResolvedGitOutgoingBase resolved_base = ResolveGitOutgoingBase(
        project_root_, state_.sidebar.git.outgoing_base_choice);
    snapshot.repo_available = resolved_base.repo_available;
    snapshot.branch_label = ResolveGitBranchLabelFallback(project_root_);
    snapshot.base_ref = resolved_base.base_ref;
    snapshot.base_label = resolved_base.base_label;
    if (!snapshot.base_ref.empty()) {
      const auto outgoing_entries =
          project::CollectGitBranchOutgoingFiles(project_root_, snapshot.base_ref);
      for (const auto& entry : outgoing_entries) {
        snapshot.entries.push_back(GitSidebarState::RefreshSnapshotEntry{
            .section = GitSidebarEntry::Section::Outgoing,
            .relative_path = entry.relative_path,
            .status = entry.status,
            .conflicted = false,
            .staged = false,
        });
      }
    }
  }

  for (const auto& entry : snapshot.entries) {
    state_.sidebar.git.entries.push_back(GitSidebarEntry{
        .section = entry.section,
        .path = (project_root_ / entry.relative_path).lexically_normal(),
        .relative_path = entry.relative_path,
        .status = entry.conflicted ? project::GitFileStatus::Conflicted : entry.status,
        .conflicted = entry.conflicted,
        .staged = entry.staged,
        .provider_id = {},
        .provider_label = {},
        .supports_stage = true,
        .supports_discard = true,
    });
  }
  state_.sidebar.git.repo_available = snapshot.repo_available;
  state_.sidebar.git.branch_label = snapshot.branch_label;
  state_.sidebar.git.base_ref = snapshot.base_ref;
  state_.sidebar.git.base_label = snapshot.base_label;
  // Preserve refresh-in-flight state when data was rendered from a synchronous
  // fallback while an async refresh request is still pending.
  if (has_snapshot) {
    state_.sidebar.git.refreshing = false;
  }

  for (std::size_t i = 0; i < state_.sidebar.git.entries.size(); ++i) {
    if (state_.sidebar.git.entries[i].path == previous_path &&
        state_.sidebar.git.entries[i].section == previous_section) {
      state_.sidebar.git.selected_index = i;
      RevealSelectedGitLine();
      if (state_.sidebar.visible && ActiveSidebarMode() == SidebarMode::Git) {
        operations_.request_sidebar_redraw();
      }
      return;
    }
  }

  RevealSelectedGitLine();
  operations_.request_window_redraw();
  if (state_.sidebar.visible && ActiveSidebarMode() == SidebarMode::Git) {
    operations_.request_sidebar_redraw();
  }
}

bool SidebarCoordinator::RefreshProblems() {
  std::optional<editor::PublishedDiagnostic> previous_diagnostic;
  if (state_.sidebar.problems.selected_index < state_.sidebar.problems.entries.size()) {
    previous_diagnostic =
        state_.sidebar.problems.entries[state_.sidebar.problems.selected_index].diagnostic;
  }

  state_.sidebar.problems.entries.clear();
  state_.sidebar.problems.selected_index = 0;
  for (const auto& diagnostic : state_.diagnostics_store.SnapshotAll()) {
    ProblemsSidebarEntry entry;
    const std::string collapsed_message = CollapseWhitespace(diagnostic.message);
    entry.diagnostic = diagnostic;
    entry.primary_label = collapsed_message.empty() ? "Diagnostic" : collapsed_message;
    entry.detail_label = RelativePathLabel(project_root_, diagnostic.path) + ":" +
                         std::to_string(diagnostic.range.start.line + 1) + ":" +
                         std::to_string(diagnostic.range.start.column + 1);
    if (!diagnostic.owner.empty()) {
      entry.detail_label += " | " + diagnostic.owner;
    }
    state_.sidebar.problems.entries.push_back(std::move(entry));
  }

  if (previous_diagnostic.has_value()) {
    for (std::size_t i = 0; i < state_.sidebar.problems.entries.size(); ++i) {
      if (state_.sidebar.problems.entries[i].diagnostic == *previous_diagnostic) {
        state_.sidebar.problems.selected_index = i;
        break;
      }
    }
  }

  RevealSelectedProblemsLine();
  if (state_.sidebar.visible && ActiveSidebarMode() == SidebarMode::Problems) {
    operations_.request_sidebar_redraw();
  }
  return !state_.sidebar.problems.entries.empty();
}

bool SidebarCoordinator::RefreshTests() {
  const bool populated =
      operations_.refresh_tests_sidebar_state ? operations_.refresh_tests_sidebar_state() : false;
  if (!state_.sidebar.tests.entries.empty()) {
    state_.sidebar.tests.selected_index =
        std::min(state_.sidebar.tests.selected_index, state_.sidebar.tests.entries.size() - 1);
  } else {
    state_.sidebar.tests.selected_index = 0;
  }
  RevealSelectedTestsLine();
  if (state_.sidebar.visible && ActiveSidebarMode() == SidebarMode::Tests) {
    operations_.request_sidebar_redraw();
  }
  return populated;
}

bool SidebarCoordinator::RefreshPlugin() {
  state_.sidebar.plugin.items.clear();
  state_.sidebar.plugin.error.clear();
  state_.sidebar.plugin.selected_index = 0;
  if (state_.sidebar.view_id.empty() || FindBuiltinSidebarView(state_.sidebar.view_id) != nullptr) {
    return false;
  }
  if (plugin_runtime_.Host().FindSidebarProvider(state_.sidebar.view_id) == nullptr) {
    state_.sidebar.view_id = "tree";
    if (state_.sidebar.visible) {
      operations_.request_sidebar_redraw();
    }
    return false;
  }

  std::string error_message;
  if (!plugin_runtime_.Host().SnapshotSidebar(state_.sidebar.view_id, &state_.sidebar.plugin.items,
                                              &error_message)) {
    state_.sidebar.plugin.error = std::move(error_message);
    if (state_.sidebar.visible && ActiveSidebarMode() == SidebarMode::Plugin) {
      operations_.request_sidebar_redraw();
    }
    return false;
  }
  if (!state_.sidebar.plugin.items.empty()) {
    state_.sidebar.plugin.selected_index =
        std::min(state_.sidebar.plugin.selected_index, state_.sidebar.plugin.items.size() - 1);
  }
  RevealSelectedPluginLine();
  if (state_.sidebar.visible && ActiveSidebarMode() == SidebarMode::Plugin) {
    operations_.request_sidebar_redraw();
  }
  return true;
}

void SidebarCoordinator::RevealSelectedTreeLine() {
  const auto& entries = state_.directory_tree.entries();
  if (state_.directory_tree.selected_index() >= entries.size()) {
    return;
  }

  const auto layout_state = operations_.current_workspace_layout();
  if (!layout_state.has_value()) {
    return;
  }
  const WorkspaceLayout layout = *layout_state;
  if (layout.sidebar.h <= 0.0f) {
    return;
  }

  const auto list_layout = operations_.compute_tree_sidebar_list_layout(layout.sidebar, entries.size());
  state_.sidebar.scroll_row =
      RevealScrollableListIndex(list_layout, static_cast<int>(state_.directory_tree.selected_index()));
}

void SidebarCoordinator::RevealSelectedGitLine() {
  const auto selected_line = operations_.selected_git_sidebar_line_index();
  if (!selected_line.has_value()) {
    return;
  }

  const auto layout_state = operations_.current_workspace_layout();
  if (!layout_state.has_value()) {
    return;
  }
  const WorkspaceLayout layout = *layout_state;
  if (layout.sidebar.h <= 0.0f) {
    return;
  }
  const auto lines = operations_.build_git_sidebar_lines();
  const auto list_layout = operations_.compute_git_sidebar_list_layout(layout.sidebar, lines.size());
  state_.sidebar.scroll_row = RevealScrollableListIndex(list_layout, static_cast<int>(*selected_line));
}

void SidebarCoordinator::RevealSelectedProblemsLine() {
  if (state_.sidebar.problems.entries.empty() ||
      state_.sidebar.problems.selected_index >= state_.sidebar.problems.entries.size()) {
    return;
  }
  const auto layout_state = operations_.current_workspace_layout();
  if (!layout_state.has_value()) {
    return;
  }
  const WorkspaceLayout layout = *layout_state;
  if (layout.sidebar.h <= 0.0f) {
    return;
  }
  const auto list_layout =
      operations_.compute_problems_sidebar_list_layout(layout.sidebar, state_.sidebar.problems.entries.size());
  state_.sidebar.scroll_row =
      RevealScrollableListIndex(list_layout, static_cast<int>(state_.sidebar.problems.selected_index));
}

void SidebarCoordinator::RevealSelectedTestsLine() {
  if (state_.sidebar.tests.entries.empty() ||
      state_.sidebar.tests.selected_index >= state_.sidebar.tests.entries.size()) {
    return;
  }
  const auto layout_state = operations_.current_workspace_layout();
  if (!layout_state.has_value()) {
    return;
  }
  const WorkspaceLayout layout = *layout_state;
  if (layout.sidebar.h <= 0.0f) {
    return;
  }
  const auto list_layout =
      operations_.compute_tests_sidebar_list_layout(layout.sidebar, state_.sidebar.tests.entries.size());
  state_.sidebar.scroll_row =
      RevealScrollableListIndex(list_layout, static_cast<int>(state_.sidebar.tests.selected_index));
}

void SidebarCoordinator::RevealSelectedPluginLine() {
  if (state_.sidebar.plugin.items.empty() ||
      state_.sidebar.plugin.selected_index >= state_.sidebar.plugin.items.size()) {
    return;
  }
  const auto layout_state = operations_.current_workspace_layout();
  if (!layout_state.has_value()) {
    return;
  }
  const WorkspaceLayout layout = *layout_state;
  if (layout.sidebar.h <= 0.0f) {
    return;
  }
  const auto list_layout =
      operations_.compute_plugin_sidebar_list_layout(layout.sidebar, state_.sidebar.plugin.items.size());
  state_.sidebar.scroll_row =
      RevealScrollableListIndex(list_layout, static_cast<int>(state_.sidebar.plugin.selected_index));
}

}  // namespace microide::workspace
