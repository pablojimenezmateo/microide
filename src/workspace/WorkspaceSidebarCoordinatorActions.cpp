#include "workspace/WorkspaceSidebarCoordinator.h"

#include <algorithm>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include "project/GitStatusService.h"
#include "workspace/SelectionMovement.h"
#include "workspace/WorkspaceActionTypes.h"

namespace microide::workspace {

namespace {
}  // namespace

const GitSidebarEntry* SidebarCoordinator::GitEntry(const std::size_t entry_index) const {
  if (entry_index >= state_.sidebar.git.entries.size()) {
    return nullptr;
  }
  return &state_.sidebar.git.entries[entry_index];
}

void SidebarCoordinator::ReportDisabledGitAction(const GitSidebarActionId action,
                                               const std::size_t entry_index) const {
  const GitSidebarEntry* entry = GitEntry(entry_index);
  if (entry == nullptr || operations_.set_command_feedback == nullptr) {
    return;
  }
  const std::string message = GitSidebarDisabledActionMessage(
      action, *entry, state_.sidebar.git.repo_available, state_.sidebar.git.supports_mutations);
  if (!message.empty()) {
    operations_.set_command_feedback(message);
  }
}

void SidebarCoordinator::ReportGitOperationFailure(const std::string_view verb,
                                                   const GitSidebarEntry& entry) const {
  if (operations_.set_command_feedback == nullptr) {
    return;
  }
  const std::filesystem::path& shown =
      entry.relative_path.empty() ? entry.path : entry.relative_path;
  std::string name = shown.generic_string();
  if (name.empty()) {
    name = "selection";
  }
  operations_.set_command_feedback("Failed to " + std::string(verb) + " " + name +
                                   " (see git output)");
}

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

bool SidebarCoordinator::OpenGitEntry(const std::size_t entry_index) {
  return DispatchGitSidebarAction(GitSidebarActionId::DefaultView, entry_index);
}

bool SidebarCoordinator::DispatchGitSidebarAction(const GitSidebarActionId action,
                                                const std::size_t entry_index) {
  const GitSidebarEntry* entry = GitEntry(entry_index);
  if (entry == nullptr) {
    return false;
  }

  const GitSidebarActionAvailability availability = GitSidebarActionAvailabilityForEntry(
      *entry, state_.sidebar.git.repo_available, state_.sidebar.git.supports_mutations);

  switch (action) {
    case GitSidebarActionId::Refresh:
      if (operations_.execute_action != nullptr) {
        return operations_.execute_action(ActionId::GitRefresh, {}, ActionSource::Shortcut);
      }
      if (operations_.request_git_refresh != nullptr) {
        operations_.request_git_refresh();
        return true;
      }
      return false;
    case GitSidebarActionId::Commit:
      if (!availability.commit) {
        ReportDisabledGitAction(action, entry_index);
        return false;
      }
      if (operations_.open_commit_workflow != nullptr) {
        return operations_.open_commit_workflow();
      }
      return false;
    case GitSidebarActionId::OpenFile:
      if (!availability.open_file) {
        ReportDisabledGitAction(action, entry_index);
        return false;
      }
      operations_.open_file(entry->path);
      state_.surface.focus = FocusTarget::Editor;
      return true;
    case GitSidebarActionId::Stage:
      return StageGitEntry(entry_index);
    case GitSidebarActionId::Unstage:
      return UnstageGitEntry(entry_index);
    case GitSidebarActionId::Discard:
      if (!availability.discard) {
        ReportDisabledGitAction(action, entry_index);
        return false;
      }
      OpenDiscardGitEntryPrompt(entry_index);
      return true;
    case GitSidebarActionId::Merge:
      if (!availability.merge) {
        ReportDisabledGitAction(action, entry_index);
        return false;
      }
      if (operations_.open_git_conflict_merge(entry->path)) {
        state_.surface.focus = FocusTarget::Editor;
        return true;
      }
      return false;
    case GitSidebarActionId::Diff:
      if (!availability.diff) {
        ReportDisabledGitAction(action, entry_index);
        return false;
      }
      if (entry->section == GitSidebarEntry::Section::Outgoing) {
        if (state_.sidebar.git.base_ref.empty()) {
          ReportDisabledGitAction(action, entry_index);
          return false;
        }
        if (operations_.open_branch_head_comparison(
                entry->path, state_.sidebar.git.base_ref,
                state_.sidebar.git.base_label.empty() ? state_.sidebar.git.base_ref
                                                      : state_.sidebar.git.base_label,
                "HEAD", "HEAD")) {
          state_.surface.focus = FocusTarget::Editor;
          return true;
        }
        return false;
      }
      if (operations_.open_working_tree_comparison(entry->path, "HEAD", "HEAD")) {
        state_.surface.focus = FocusTarget::Editor;
        return true;
      }
      return false;
    case GitSidebarActionId::DefaultView:
      if (!availability.default_view) {
        ReportDisabledGitAction(action, entry_index);
        return false;
      }
      if (entry->section == GitSidebarEntry::Section::Conflicts ||
          entry->conflicted) {
        return DispatchGitSidebarAction(GitSidebarActionId::Merge, entry_index);
      }
      if (availability.diff) {
        return DispatchGitSidebarAction(GitSidebarActionId::Diff, entry_index);
      }
      if (availability.open_file) {
        return DispatchGitSidebarAction(GitSidebarActionId::OpenFile, entry_index);
      }
      ReportDisabledGitAction(action, entry_index);
      return false;
  }
  return false;
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

bool SidebarCoordinator::TogglePluginItem() {
  if (state_.sidebar.plugin.items.empty() ||
      state_.sidebar.plugin.selected_index >= state_.sidebar.plugin.items.size()) {
    return false;
  }
  // Snapshot the item by value: re-snapshotting below rebuilds the items vector,
  // invalidating any reference into it.
  const plugin::PluginHost::SidebarItem item =
      state_.sidebar.plugin.items[state_.sidebar.plugin.selected_index];
  if (!item.collapsible) {
    return false;
  }
  std::string error_message;
  const bool toggled =
      plugin_runtime_.Host().ToggleSidebarItem(state_.sidebar.view_id, item, &error_message);
  if (!toggled) {
    if (!error_message.empty()) {
      state_.sidebar.plugin.error = std::move(error_message);
    }
    return false;
  }
  // The plugin owns its expand/collapse state; re-snapshot to pull the reshaped
  // (visible) row set. RefreshPlugin re-clamps the selection and requests redraw.
  RefreshPlugin();
  return true;
}

bool SidebarCoordinator::CanStageAllGitEntries() const {
  return std::any_of(state_.sidebar.git.entries.begin(), state_.sidebar.git.entries.end(),
                     [](const GitSidebarEntry& entry) {
                       return entry.section == GitSidebarEntry::Section::Changed ||
                              entry.section == GitSidebarEntry::Section::Untracked;
                     });
}

bool SidebarCoordinator::CanDiscardAllGitEntries() const {
  return std::any_of(state_.sidebar.git.entries.begin(), state_.sidebar.git.entries.end(),
                     [](const GitSidebarEntry& entry) { return IsGitWorkflowSection(entry.section); });
}

bool SidebarCoordinator::StageAllGitEntries() {
  if (!CanStageAllGitEntries()) {
    return false;
  }
  std::vector<std::filesystem::path> affected_paths;
  affected_paths.reserve(state_.sidebar.git.entries.size());
  for (const auto& entry : state_.sidebar.git.entries) {
    if (entry.section != GitSidebarEntry::Section::Changed &&
        entry.section != GitSidebarEntry::Section::Untracked) {
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

void SidebarCoordinator::OpenDiscardGitEntryPrompt(const std::size_t entry_index) {
  const GitSidebarEntry* entry = GitEntry(entry_index);
  if (entry == nullptr) {
    return;
  }
  operations_.open_prompt_surface(PromptSurfaceState::Action::DiscardGitEntry,
                                  PromptSurfaceState::Kind::Confirm, entry->path,
                                  std::to_string(entry_index));
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
    if (!IsGitWorkflowSection(entry.section)) {
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

bool SidebarCoordinator::StageGitEntry(const std::size_t entry_index) {
  const GitSidebarEntry* entry = GitEntry(entry_index);
  if (entry == nullptr) {
    return false;
  }
  if (!GitSidebarActionAvailabilityForEntry(*entry, state_.sidebar.git.repo_available,
                                            state_.sidebar.git.supports_mutations)
           .stage) {
    ReportDisabledGitAction(GitSidebarActionId::Stage, entry_index);
    return false;
  }
  if (!project::GitStagePath(project_root_, entry->path)) {
    ReportGitOperationFailure("stage", *entry);
    return false;
  }
  operations_.invalidate_editor_blame_path(entry->path);
  RefreshProjectFiles();
  return true;
}

bool SidebarCoordinator::UnstageGitEntry(const std::size_t entry_index) {
  const GitSidebarEntry* entry = GitEntry(entry_index);
  if (entry == nullptr) {
    return false;
  }
  if (!GitSidebarActionAvailabilityForEntry(*entry, state_.sidebar.git.repo_available,
                                            state_.sidebar.git.supports_mutations)
           .unstage) {
    ReportDisabledGitAction(GitSidebarActionId::Unstage, entry_index);
    return false;
  }
  if (!project::GitUnstagePath(project_root_, entry->path)) {
    ReportGitOperationFailure("unstage", *entry);
    return false;
  }
  operations_.invalidate_editor_blame_path(entry->path);
  RefreshProjectFiles();
  return true;
}

bool SidebarCoordinator::DiscardGitEntry(const std::size_t entry_index) {
  const GitSidebarEntry* entry = GitEntry(entry_index);
  if (entry == nullptr) {
    return false;
  }
  if (!IsGitWorkflowSection(entry->section)) {
    return false;
  }

  std::string blocking_label;
  if (operations_.has_dirty_editor_tabs_for_path(entry->path, &blocking_label)) {
    if (operations_.set_command_feedback != nullptr && !blocking_label.empty()) {
      operations_.set_command_feedback("Save or close dirty tabs before discarding " +
                                      blocking_label);
    }
    return false;
  }
  if (!project::GitDiscardPath(project_root_, entry->path)) {
    ReportGitOperationFailure("discard", *entry);
    return false;
  }
  operations_.invalidate_editor_blame_path(entry->path);
  ReconcileOpenTabsAfterPathDiscard(entry->path);
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
