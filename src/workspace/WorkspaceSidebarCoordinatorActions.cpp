#include "workspace/WorkspaceSidebarCoordinator.h"

#include <algorithm>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include "project/GitStatusService.h"

namespace microide::workspace {

namespace {

template <typename Entries>
bool MoveSelectionIndex(const Entries& entries, std::size_t* selected_index, int delta) {
  if (entries.empty() || delta == 0 || selected_index == nullptr) {
    return false;
  }
  const int current = static_cast<int>(*selected_index);
  const int max_index = static_cast<int>(entries.size()) - 1;
  *selected_index = static_cast<std::size_t>(std::clamp(current + delta, 0, max_index));
  return true;
}

}  // namespace

void SidebarCoordinator::MoveGitSelection(int delta) {
  if (MoveSelectionIndex(state_.sidebar.git.entries, &state_.sidebar.git.selected_index, delta)) {
    RevealSelectedGitLine();
  }
}

void SidebarCoordinator::MoveProblemsSelection(int delta) {
  if (MoveSelectionIndex(state_.sidebar.problems.entries, &state_.sidebar.problems.selected_index,
                         delta)) {
    RevealSelectedProblemsLine();
  }
}

void SidebarCoordinator::MoveTestsSelection(int delta) {
  if (MoveSelectionIndex(state_.sidebar.tests.entries, &state_.sidebar.tests.selected_index,
                         delta)) {
    RevealSelectedTestsLine();
  }
}

void SidebarCoordinator::MovePluginSelection(int delta) {
  if (MoveSelectionIndex(state_.sidebar.plugin.items, &state_.sidebar.plugin.selected_index,
                         delta)) {
    RevealSelectedPluginLine();
  }
}

bool SidebarCoordinator::OpenGitEntry(std::size_t entry_index) {
  if (entry_index >= state_.sidebar.git.entries.size()) {
    return false;
  }
  const auto& entry = state_.sidebar.git.entries[entry_index];
  bool opened = false;
  if (entry.section == GitSidebarEntry::Section::Modified) {
    if (entry.conflicted) {
      opened = operations_.open_git_conflict_merge(entry.path);
    } else {
      opened = operations_.open_working_tree_comparison(entry.path, "HEAD", "HEAD");
    }
  } else {
    if (state_.sidebar.git.base_ref.empty()) {
      return false;
    }
    opened = operations_.open_branch_head_comparison(
        entry.path, state_.sidebar.git.base_ref,
        state_.sidebar.git.base_label.empty() ? state_.sidebar.git.base_ref
                                              : state_.sidebar.git.base_label,
        "HEAD", "HEAD");
  }
  if (opened && state_.sidebar.visible && ActiveSidebarMode() == SidebarMode::Git) {
    operations_.request_sidebar_redraw();
  }
  return opened;
}

bool SidebarCoordinator::OpenProblemItem() {
  if (state_.sidebar.problems.entries.empty() ||
      state_.sidebar.problems.selected_index >= state_.sidebar.problems.entries.size()) {
    return false;
  }
  const auto& entry = state_.sidebar.problems.entries[state_.sidebar.problems.selected_index];
  if (entry.diagnostic.path.empty()) {
    return false;
  }
  operations_.open_file(entry.diagnostic.path);
  if (editor::TextViewport* viewport = operations_.active_editor_viewport(); viewport != nullptr) {
    viewport->MoveCursorTo(entry.diagnostic.range.start.line,
                           entry.diagnostic.range.start.column);
  }
  if (state_.sidebar.temporary) {
    RestorePrevious();
  }
  state_.surface.focus = FocusTarget::Editor;
  return true;
}

bool SidebarCoordinator::OpenTestItem() {
  if (state_.sidebar.tests.entries.empty() ||
      state_.sidebar.tests.selected_index >= state_.sidebar.tests.entries.size()) {
    return false;
  }
  const auto& entry = state_.sidebar.tests.entries[state_.sidebar.tests.selected_index];
  if (entry.file.empty()) {
    return false;
  }
  operations_.open_file(entry.file);
  if (editor::TextViewport* viewport = operations_.active_editor_viewport(); viewport != nullptr &&
      entry.line > 0) {
    viewport->MoveCursorTo(static_cast<std::size_t>(entry.line - 1), 0);
  }
  if (state_.sidebar.temporary) {
    RestorePrevious();
  }
  state_.surface.focus = FocusTarget::Editor;
  return true;
}

bool SidebarCoordinator::RunTestItem() {
  if (state_.sidebar.tests.entries.empty() ||
      state_.sidebar.tests.selected_index >= state_.sidebar.tests.entries.size() ||
      !operations_.run_tests) {
    return false;
  }
  const auto& entry = state_.sidebar.tests.entries[state_.sidebar.tests.selected_index];
  if (entry.id.empty()) {
    return false;
  }
  return operations_.run_tests({entry.id});
}

bool SidebarCoordinator::OpenPluginItem() {
  if (state_.sidebar.plugin.items.empty() ||
      state_.sidebar.plugin.selected_index >= state_.sidebar.plugin.items.size()) {
    return false;
  }
  const auto& item = state_.sidebar.plugin.items[state_.sidebar.plugin.selected_index];
  std::string error_message;
  const bool confirmed =
      plugin_runtime_.Host().ConfirmSidebarItem(state_.sidebar.view_id, item, &error_message);
  if (!confirmed && !error_message.empty()) {
    state_.sidebar.plugin.error = std::move(error_message);
  }
  if (confirmed && state_.sidebar.temporary) {
    RestorePrevious();
  }
  if (confirmed && !item.path.empty()) {
    state_.surface.focus = FocusTarget::Editor;
  }
  return confirmed;
}

bool SidebarCoordinator::CanStageAllGitEntries() const {
  return std::any_of(state_.sidebar.git.entries.begin(), state_.sidebar.git.entries.end(),
                     [](const auto& entry) {
                       return entry.section == GitSidebarEntry::Section::Modified && !entry.staged;
                     });
}

bool SidebarCoordinator::CanDiscardAllGitEntries() const {
  return std::any_of(state_.sidebar.git.entries.begin(), state_.sidebar.git.entries.end(),
                     [](const auto& entry) {
                       return entry.section == GitSidebarEntry::Section::Modified;
                     });
}

bool SidebarCoordinator::StageAllGitEntries() {
  if (!CanStageAllGitEntries()) {
    return false;
  }
  std::vector<std::filesystem::path> affected_paths;
  affected_paths.reserve(state_.sidebar.git.entries.size());
  for (const auto& entry : state_.sidebar.git.entries) {
    if (entry.section != GitSidebarEntry::Section::Modified || entry.staged) {
      continue;
    }
    affected_paths.push_back(entry.path.lexically_normal());
  }
  std::sort(affected_paths.begin(), affected_paths.end());
  affected_paths.erase(std::unique(affected_paths.begin(), affected_paths.end()),
                       affected_paths.end());
  if (!project::GitStageAll(project_root_)) {
    return false;
  }
  for (const auto& path : affected_paths) {
    operations_.invalidate_editor_blame_path(path);
  }
  RefreshProjectFiles();
  return true;
}

void SidebarCoordinator::OpenDiscardAllGitPrompt() {
  if (!CanDiscardAllGitEntries()) {
    return;
  }
  operations_.open_prompt_surface(PromptSurfaceState::Action::DiscardGitChanges,
                                  PromptSurfaceState::Kind::Confirm, project_root_, {});
}

bool SidebarCoordinator::DiscardAllGitEntries() {
  if (!CanDiscardAllGitEntries()) {
    return false;
  }

  std::string blocking_label;
  if (operations_.has_dirty_editor_tabs_for_path(project_root_, &blocking_label)) {
    return false;
  }

  std::vector<std::filesystem::path> affected_paths;
  affected_paths.reserve(state_.sidebar.git.entries.size());
  for (const auto& entry : state_.sidebar.git.entries) {
    if (entry.section != GitSidebarEntry::Section::Modified) {
      continue;
    }
    affected_paths.push_back(entry.path.lexically_normal());
  }
  std::sort(affected_paths.begin(), affected_paths.end());
  affected_paths.erase(std::unique(affected_paths.begin(), affected_paths.end()),
                       affected_paths.end());

  if (!project::GitDiscardAll(project_root_)) {
    return false;
  }

  for (const auto& path : affected_paths) {
    operations_.invalidate_editor_blame_path(path);
    ReconcileOpenTabsAfterPathDiscard(path);
  }
  RefreshProjectFiles();
  return true;
}

bool SidebarCoordinator::StageGitEntry(std::size_t entry_index) {
  if (entry_index >= state_.sidebar.git.entries.size()) {
    return false;
  }
  const auto& entry = state_.sidebar.git.entries[entry_index];
  if (entry.section != GitSidebarEntry::Section::Modified || entry.staged) {
    return false;
  }
  if (!project::GitStagePath(project_root_, entry.path)) {
    return false;
  }
  operations_.invalidate_editor_blame_path(entry.path);
  RefreshProjectFiles();
  return true;
}

bool SidebarCoordinator::UnstageGitEntry(std::size_t entry_index) {
  if (entry_index >= state_.sidebar.git.entries.size()) {
    return false;
  }
  const auto& entry = state_.sidebar.git.entries[entry_index];
  if (entry.section != GitSidebarEntry::Section::Modified || !entry.staged) {
    return false;
  }
  if (!project::GitUnstagePath(project_root_, entry.path)) {
    return false;
  }
  operations_.invalidate_editor_blame_path(entry.path);
  RefreshProjectFiles();
  return true;
}

bool SidebarCoordinator::DiscardGitEntry(std::size_t entry_index) {
  if (entry_index >= state_.sidebar.git.entries.size()) {
    return false;
  }
  const auto& entry = state_.sidebar.git.entries[entry_index];
  if (entry.section != GitSidebarEntry::Section::Modified) {
    return false;
  }

  std::string blocking_label;
  if (operations_.has_dirty_editor_tabs_for_path(entry.path, &blocking_label)) {
    return false;
  }
  if (!project::GitDiscardPath(project_root_, entry.path)) {
    return false;
  }
  operations_.invalidate_editor_blame_path(entry.path);
  ReconcileOpenTabsAfterPathDiscard(entry.path);
  RefreshProjectFiles();
  return true;
}

void SidebarCoordinator::ReconcileOpenTabsAfterPathDiscard(const std::filesystem::path& path) {
  const std::filesystem::path normalized_path = path.lexically_normal();
  std::error_code error;
  if (std::filesystem::exists(normalized_path, error) && !error) {
    operations_.reload_clean_editor_tabs_for_path(normalized_path);
    return;
  }
  operations_.close_open_tabs_for_path(normalized_path);
}

}  // namespace microide::workspace
