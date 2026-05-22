#include "workspace/WorkspaceSidebarCoordinator.h"

#include <algorithm>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>

#include "workspace/GitSidebarCommandCenter.h"
#include "workspace/WorkspaceLayout.h"
#include "workspace/WorkspacePathUtils.h"
#include "workspace/WorkspaceSidebarRegistry.h"
#include "workspace/WorkspaceTextSearch.h"

namespace microide::workspace {

namespace {

void ClampSelectionToItemCount(std::size_t item_count, std::size_t* selected_index) {
  if (selected_index == nullptr) {
    return;
  }
  *selected_index = item_count == 0 ? 0 : std::min(*selected_index, item_count - 1);
}

void ApplyGitRefreshSnapshot(GitSidebarState& git_state,
                             const GitSidebarState::RefreshSnapshot& snapshot,
                             const std::filesystem::path& project_root) {
  git_state.entries.clear();
  git_state.entries.reserve(snapshot.entries.size());
  for (const auto& entry : snapshot.entries) {
    GitSidebarEntry git_entry{
        .section = entry.section,
        .path = (project_root / entry.relative_path).lexically_normal(),
        .relative_path = entry.relative_path,
        .status = entry.conflicted ? project::GitFileStatus::Conflicted : entry.status,
        .conflicted = entry.conflicted,
        .staged = entry.staged,
        .provider_id = {},
        .provider_label = {},
        .supports_stage = false,
        .supports_discard = false,
    };
    const GitSidebarActionAvailability availability = GitSidebarActionAvailabilityForEntry(
        git_entry, snapshot.repo_available, git_state.supports_mutations);
    git_entry.supports_stage = availability.stage;
    git_entry.supports_discard = availability.discard;
    git_state.entries.push_back(std::move(git_entry));
  }
  git_state.repo_available = snapshot.repo_available;
  git_state.branch_label = snapshot.branch_label;
  git_state.upstream_label = snapshot.upstream_label;
  git_state.ahead = snapshot.ahead;
  git_state.behind = snapshot.behind;
  git_state.base_ref = snapshot.base_ref;
  git_state.base_label = snapshot.base_label;
  git_state.snapshot_stale = snapshot.snapshot_stale;
  git_state.refresh_error = snapshot.refresh_error;
  git_state.snapshot_generation = snapshot.generation;
}

}  // namespace

void SidebarCoordinator::RefreshGit() {
  if (state_.sidebar.git.refreshing &&
      operations_.consume_git_refresh_snapshot != nullptr) {
    GitSidebarState::RefreshSnapshot pending_snapshot;
    if (!operations_.consume_git_refresh_snapshot(&pending_snapshot)) {
      if (state_.sidebar.visible && ActiveSidebarMode() == SidebarMode::Git) {
        operations_.request_sidebar_redraw();
      }
      return;
    }
    if (pending_snapshot.includes_tree_git_statuses) {
      state_.directory_tree.ApplyGitStatuses(std::move(pending_snapshot.tree_git_statuses));
    }

    const std::filesystem::path previous_path =
        state_.sidebar.git.selected_index < state_.sidebar.git.entries.size()
            ? state_.sidebar.git.entries[state_.sidebar.git.selected_index].path
            : std::filesystem::path{};
    const GitSidebarEntry::Section previous_section =
        state_.sidebar.git.selected_index < state_.sidebar.git.entries.size()
            ? state_.sidebar.git.entries[state_.sidebar.git.selected_index].section
            : GitSidebarEntry::Section::Changed;

    state_.sidebar.git.selected_index = 0;
    ApplyGitRefreshSnapshot(state_.sidebar.git, pending_snapshot, project_root_);
    state_.sidebar.git.refreshing = false;

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
    return;
  }

  const std::filesystem::path previous_path =
      state_.sidebar.git.selected_index < state_.sidebar.git.entries.size()
          ? state_.sidebar.git.entries[state_.sidebar.git.selected_index].path
          : std::filesystem::path{};
  const GitSidebarEntry::Section previous_section =
      state_.sidebar.git.selected_index < state_.sidebar.git.entries.size()
          ? state_.sidebar.git.entries[state_.sidebar.git.selected_index].section
          : GitSidebarEntry::Section::Changed;

  state_.sidebar.git.selected_index = 0;
  if (project_root_.empty()) {
    state_.sidebar.git.entries.clear();
    return;
  }

  GitSidebarState::RefreshSnapshot snapshot;
  bool has_snapshot =
      operations_.consume_git_refresh_snapshot != nullptr &&
      operations_.consume_git_refresh_snapshot(&snapshot);
  if (!has_snapshot) {
    if (!state_.sidebar.git.refreshing && state_.sidebar.visible &&
        ActiveSidebarMode() == SidebarMode::Git &&
        operations_.request_git_refresh != nullptr) {
      operations_.request_git_refresh();
    }
    operations_.request_sidebar_redraw();
    return;
  }
  if (snapshot.includes_tree_git_statuses) {
    state_.directory_tree.ApplyGitStatuses(std::move(snapshot.tree_git_statuses));
  }

  ApplyGitRefreshSnapshot(state_.sidebar.git, snapshot, project_root_);
  // Preserve refresh-in-flight state when data was rendered from a synchronous
  // fallback while an async refresh request is still pending.
  state_.sidebar.git.refreshing = false;

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
  ClampSelectionToItemCount(state_.sidebar.tests.entries.size(),
                            &state_.sidebar.tests.selected_index);
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
  ClampSelectionToItemCount(state_.sidebar.plugin.items.size(),
                            &state_.sidebar.plugin.selected_index);
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
