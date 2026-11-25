#include "workspace/coordinators/WorkspaceSidebarCoordinator.h"

#include "workspace/TypeNavigation.h"

#include <algorithm>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include <optional>

#include "editor/TextViewport.h"
#include "project/FileWriteGate.h"
#include "project/GitStatusService.h"
#include "util/PathMatch.h"
#include "workspace/SelectionMovement.h"
#include "workspace/actions/WorkspaceActionTypes.h"
#include "workspace/git/GitSidebarCommandCenter.h"

namespace microide::workspace {

namespace {

template <typename Vec>
const typename Vec::value_type* SelectedListEntry(const Vec& entries, std::size_t selected_index) {
  if (selected_index >= entries.size()) {
    return nullptr;
  }
  return &entries[selected_index];
}

void PruneEmptyDirectoriesBelow(project::FileWriteGate& gate,
                                const std::filesystem::path& root,
                                std::filesystem::path directory) {
  const std::filesystem::path normalized_root = root.lexically_normal();
  std::error_code error;
  while (!directory.empty() && directory != normalized_root &&
         util::NormalizedPathEqualsOrWithin(directory, normalized_root)) {
    if (!std::filesystem::is_directory(directory, error) || error) {
      return;
    }
    if (!gate.ApplyTreeOp({.kind = project::FileWriteGate::TreeOp::Kind::Delete,
                           .path = directory,
                           .recursive = false})
             .ok) {
      return;
    }
    directory = directory.parent_path();
  }
}

}

bool SidebarCoordinator::OpenEditorFileFromSidebar(
    const std::filesystem::path& path, std::optional<std::pair<std::size_t, std::size_t>> caret) {
  if (path.empty() || !operations_.open_file) {
    return false;
  }
  operations_.open_file(path);
  if (caret.has_value()) {
    if (editor::TextViewport* viewport =
            operations_.active_editor_viewport ? operations_.active_editor_viewport() : nullptr;
        viewport != nullptr) {
      viewport->JumpCursorTo(caret->first, caret->second);
    }
  }
  if (state_.sidebar.temporary) {
    RestorePrevious();
  }
  state_.surface.focus = FocusTarget::Editor;
  return true;
}

const GitSidebarEntry* SidebarCoordinator::GitEntry(const std::size_t entry_index) const {
  if (entry_index >= state_.sidebar.git.entries.size()) {
    return nullptr;
  }
  return &state_.sidebar.git.entries[entry_index];
}

void SidebarCoordinator::ReportGitMessage(const NotificationService::Tone tone,
                                         std::string message) const {
  if (message.empty()) {
    return;
  }
  if (operations_.set_command_feedback != nullptr) {
    operations_.set_command_feedback(message);
  }
  if (operations_.notify != nullptr) {
    operations_.notify(tone, std::move(message));
  }
}

void SidebarCoordinator::ReportDisabledGitAction(const GitSidebarActionId action,
                                               const std::size_t entry_index) const {
  const GitSidebarEntry* entry = GitEntry(entry_index);
  if (entry == nullptr) {
    return;
  }
  ReportGitMessage(NotificationService::Tone::Warning,
                   GitSidebarDisabledActionMessage(action, *entry,
                                                   state_.sidebar.git.repo_available,
                                                   state_.sidebar.git.supports_mutations));
}

std::string_view SidebarCoordinator::GitEntryLabel(const GitSidebarEntry& entry) {
  const std::string_view shown =
      entry.relative_path.empty() ? std::string_view(entry.path) : std::string_view(entry.relative_path);
  return shown.empty() ? std::string_view("selection") : shown;
}

void SidebarCoordinator::ReportGitOperationFailure(const std::string_view verb,
                                                   const GitSidebarEntry& entry) const {
  ReportGitMessage(NotificationService::Tone::Error, "Failed to " + std::string(verb) + " " +
                                                         std::string(GitEntryLabel(entry)) +
                                                         " (see git output)");
}

void SidebarCoordinator::MoveSimpleListSelection(
    std::size_t count, std::size_t* selected_index,
    const std::function<ScrollableListLayout(const SDL_FRect&, std::size_t)>& compute_layout,
    int delta) {
  if (MoveSelectionIndex(count, selected_index, delta)) {
    RevealListSelection(count, *selected_index, compute_layout);
  }
}

std::vector<std::size_t> SidebarCoordinator::VisibleGitEntries() const {
  std::vector<std::size_t> visible;
  if (!operations_.build_git_sidebar_lines) {
    return visible;
  }
  const std::vector<GitSidebarLine>& lines = operations_.build_git_sidebar_lines();
  visible.reserve(state_.sidebar.git.entries.size());
  for (const GitSidebarLine& line : lines) {
    if (line.kind == GitSidebarLine::Kind::Entry && line.entry_index >= 0) {
      visible.push_back(static_cast<std::size_t>(line.entry_index));
    }
  }
  return visible;
}

bool SidebarCoordinator::TypeNavigateGit(char typed, std::uint64_t now_ns) {
  auto& git = state_.sidebar.git;
  const std::vector<std::size_t> visible = VisibleGitEntries();
  const auto current = std::find(visible.begin(), visible.end(), git.selected_index);
  const auto file_name = [&](std::size_t row) {
    const std::string_view path = git.entries[visible[row]].relative_path;
    const std::size_t slash = path.find_last_of('/');
    return slash == std::string_view::npos ? path : path.substr(slash + 1);
  };
  const std::optional<std::size_t> row =
      TypeNavigate(git.type_navigation, typed, now_ns,
                   current == visible.end() ? 0 : static_cast<std::size_t>(current - visible.begin()),
                   visible.size(), file_name);
  if (!row.has_value()) {
    return false;
  }
  git.selected_index = visible[*row];
  RevealSelectedGitLine();
  return true;
}

void SidebarCoordinator::MoveGitSelection(int delta) {
  auto& git = state_.sidebar.git;
  if (git.entries.empty() || delta == 0) {
    return;
  }
  const std::vector<std::size_t> visible = VisibleGitEntries();
  if (visible.empty()) {
    if (MoveSelectionIndex(git.entries, &git.selected_index, delta)) {
      RevealSelectedGitLine();
    }
    return;
  }

  std::optional<std::size_t> pos;
  for (std::size_t i = 0; i < visible.size(); ++i) {
    if (visible[i] == git.selected_index) {
      pos = i;
      break;
    }
  }
  std::size_t next_pos;
  if (pos.has_value()) {
    const long long moved = static_cast<long long>(*pos) + delta;
    next_pos = static_cast<std::size_t>(
        std::clamp<long long>(moved, 0, static_cast<long long>(visible.size()) - 1));
  } else {
    next_pos = delta > 0 ? 0 : visible.size() - 1;
  }
  const bool moved = visible[next_pos] != git.selected_index;
  git.selected_index = visible[next_pos];
  if (delta < 0 && next_pos == 0) {
    state_.sidebar.scroll_row = 0;
    return;
  }
  if (moved) {
    RevealSelectedGitLine();
  }
}

void SidebarCoordinator::MoveProblemsSelection(int delta) {
  MoveSimpleListSelection(state_.sidebar.problems.entries.size(),
                          &state_.sidebar.problems.selected_index,
                          operations_.compute_problems_sidebar_list_layout, delta);
}

void SidebarCoordinator::MoveTestsSelection(int delta) {
  MoveSimpleListSelection(state_.sidebar.tests.entries.size(),
                          &state_.sidebar.tests.selected_index,
                          operations_.compute_tests_sidebar_list_layout, delta);
}

void SidebarCoordinator::MovePluginSelection(int delta) {
  MoveSimpleListSelection(state_.sidebar.plugin.items.size(),
                          &state_.sidebar.plugin.selected_index,
                          operations_.compute_plugin_sidebar_list_layout, delta);
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
      ReportGitMessage(NotificationService::Tone::Error,
                       "Could not open the merge view for " + std::string(GitEntryLabel(*entry)));
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
        ReportGitMessage(NotificationService::Tone::Error,
                         "Could not open the diff for " + std::string(GitEntryLabel(*entry)));
        return false;
      }
      if (operations_.open_working_tree_comparison(entry->path, "HEAD", "HEAD")) {
        state_.surface.focus = FocusTarget::Editor;
        return true;
      }
      ReportGitMessage(NotificationService::Tone::Error,
                       "Could not open the diff for " + std::string(GitEntryLabel(*entry)));
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
  const ProblemsSidebarEntry* entry =
      SelectedListEntry(state_.sidebar.problems.entries, state_.sidebar.problems.selected_index);
  if (entry == nullptr || entry->diagnostic.path.empty()) {
    return false;
  }
  return OpenEditorFileFromSidebar(
      entry->diagnostic.path,
      std::pair<std::size_t, std::size_t>{entry->diagnostic.range.start.line,
                                          entry->diagnostic.range.start.column});
}

bool SidebarCoordinator::OpenTestItem() {
  const TestsSidebarEntry* entry =
      SelectedListEntry(state_.sidebar.tests.entries, state_.sidebar.tests.selected_index);
  if (entry == nullptr || entry->file.empty()) {
    return false;
  }
  std::optional<std::pair<std::size_t, std::size_t>> caret;
  if (entry->line > 0) {
    caret = std::pair<std::size_t, std::size_t>{static_cast<std::size_t>(entry->line - 1), 0};
  }
  return OpenEditorFileFromSidebar(entry->file, caret);
}

bool SidebarCoordinator::RunTestItem() {
  const TestsSidebarEntry* entry =
      SelectedListEntry(state_.sidebar.tests.entries, state_.sidebar.tests.selected_index);
  if (entry == nullptr || entry->id.empty() || !operations_.run_tests) {
    return false;
  }
  return operations_.run_tests({entry->id});
}

bool SidebarCoordinator::OpenPluginItem() {
  const plugin::PluginHost::SidebarItem* selected =
      SelectedListEntry(state_.sidebar.plugin.items, state_.sidebar.plugin.selected_index);
  if (selected == nullptr) {
    return false;
  }
  const plugin::PluginHost::SidebarItem& item = *selected;

  if (const SidebarMode mode = ActiveSidebarMode(); IsHostOwnedItemView(mode)) {
    return operations_.open_host_view_row && operations_.open_host_view_row(mode, item);
  }

  if (ActiveSidebarMode() == SidebarMode::Outline) {
    if (item.path.empty() || !operations_.open_file) {
      return false;
    }
    operations_.open_file(item.path);
    if (editor::TextViewport* viewport =
            operations_.active_editor_viewport ? operations_.active_editor_viewport() : nullptr;
        viewport != nullptr) {
      viewport->JumpCursorTo(item.line > 0 ? item.line - 1 : 0,
                             item.column > 0 ? item.column - 1 : 0);
    }
    return true;
  }

  std::string error_message;
  const bool confirmed =
      plugin_runtime_.Host().ConfirmSidebarItem(state_.sidebar.view_id, item, &error_message);
  if (!confirmed && !error_message.empty()) {
    state_.sidebar.plugin.error = std::move(error_message);
    RecomputePluginSidebarPlaceholder();
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
  const plugin::PluginHost::SidebarItem* selected =
      SelectedListEntry(state_.sidebar.plugin.items, state_.sidebar.plugin.selected_index);
  if (selected == nullptr || !selected->collapsible) {
    return false;
  }
  const plugin::PluginHost::SidebarItem item = *selected;
  if (const SidebarMode mode = ActiveSidebarMode(); IsHostOwnedItemView(mode)) {
    if (!operations_.toggle_host_view_row || !operations_.toggle_host_view_row(mode, item)) {
      return false;
    }
    RefreshHostView();
    return true;
  }
  std::string error_message;
  const bool toggled =
      plugin_runtime_.Host().ToggleSidebarItem(state_.sidebar.view_id, item, &error_message);
  if (!toggled) {
    if (!error_message.empty()) {
      state_.sidebar.plugin.error = std::move(error_message);
      RecomputePluginSidebarPlaceholder();
    }
    return false;
  }
  RefreshPlugin();
  return true;
}

bool SidebarCoordinator::EndPluginItem() {
  const plugin::PluginHost::SidebarItem* selected =
      SelectedListEntry(state_.sidebar.plugin.items, state_.sidebar.plugin.selected_index);
  const SidebarMode mode = ActiveSidebarMode();
  if (selected == nullptr || !IsHostOwnedItemView(mode) || !operations_.end_host_view_row) {
    return false;
  }
  const plugin::PluginHost::SidebarItem item = *selected;
  return operations_.end_host_view_row(mode, item);
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
    affected_paths.emplace_back(entry.path);
  }
  std::sort(affected_paths.begin(), affected_paths.end());
  affected_paths.erase(std::unique(affected_paths.begin(), affected_paths.end()),
                       affected_paths.end());
  if (!project::GitStageAll(project_root_, state_.launcher())) {
    ReportGitMessage(NotificationService::Tone::Error,
                     "Failed to stage all changes (see git output)");
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
                                  PromptSurfaceState::Kind::Confirm, project_root_, {}, {});
}

void SidebarCoordinator::OpenDiscardGitEntryPrompt(const std::size_t entry_index) {
  const GitSidebarEntry* entry = GitEntry(entry_index);
  if (entry == nullptr) {
    return;
  }
  operations_.open_prompt_surface(PromptSurfaceState::Action::DiscardGitEntry,
                                  PromptSurfaceState::Kind::Confirm, entry->path,
                                  std::to_string(entry_index),
                                  BuildGitDiscardPreviewSummary(*entry, {}));
}

bool SidebarCoordinator::DiscardAllGitEntries() {
  if (!CanDiscardAllGitEntries()) {
    return false;
  }

  std::vector<std::filesystem::path> affected_paths;
  std::vector<std::filesystem::path> untracked_paths;
  affected_paths.reserve(state_.sidebar.git.entries.size());
  for (const auto& entry : state_.sidebar.git.entries) {
    if (!IsGitWorkflowSection(entry.section)) {
      continue;
    }
    affected_paths.emplace_back(entry.path);
    if (entry.section == GitSidebarEntry::Section::Untracked ||
        entry.status == project::GitFileStatus::Added) {
      untracked_paths.emplace_back(entry.path);
    }
  }
  std::sort(affected_paths.begin(), affected_paths.end());
  affected_paths.erase(std::unique(affected_paths.begin(), affected_paths.end()),
                       affected_paths.end());

  std::string blocking_label;
  for (const auto& path : affected_paths) {
    if (!operations_.has_dirty_editor_tabs_for_path(path, &blocking_label)) {
      continue;
    }
    ReportGitMessage(NotificationService::Tone::Warning,
                     blocking_label.empty()
                         ? std::string("Save or close dirty tabs before discarding")
                         : "Save or close dirty tabs before discarding " + blocking_label);
    return false;
  }

  std::size_t trash_failures = 0;
  for (const auto& untracked : untracked_paths) {
    if (!state_.write_gate()
             .ApplyTreeOp({.kind = project::FileWriteGate::TreeOp::Kind::Trash, .path = untracked})
             .ok) {
      ++trash_failures;
    }
  }

  if (!project::GitDiscardAll(project_root_, state_.launcher(), false)) {
    ReportGitMessage(NotificationService::Tone::Error,
                     "Failed to discard all changes (see git output)");
    return false;
  }

  for (const auto& untracked : untracked_paths) {
    PruneEmptyDirectoriesBelow(state_.write_gate(), project_root_, untracked.parent_path());
  }

  if (trash_failures > 0) {
    ReportGitMessage(NotificationService::Tone::Error,
                     "Discarded tracked changes; " + std::to_string(trash_failures) +
                         (trash_failures == 1 ? " untracked file could not be moved to the trash"
                                              : " untracked files could not be moved to the trash"));
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
  if (!project::GitStagePath(project_root_, state_.launcher(), entry->path)) {
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
  if (!project::GitUnstagePath(project_root_, state_.launcher(), entry->path, entry->is_staged_rename)) {
    ReportGitOperationFailure("unstage", *entry);
    return false;
  }
  operations_.invalidate_editor_blame_path(entry->path);
  RefreshProjectFiles();
  return true;
}

bool SidebarCoordinator::DiscardGitEntry(const std::size_t entry_index,
                                         const std::optional<std::filesystem::path>& expected_path) {
  const GitSidebarEntry* entry = GitEntry(entry_index);
  if (entry == nullptr) {
    return false;
  }
  if (!IsGitWorkflowSection(entry->section)) {
    return false;
  }
  if (expected_path.has_value()) {
    std::string expected = expected_path->generic_string();
    if (util::PathTextNeedsNormalizing(expected)) {
      expected = expected_path->lexically_normal().generic_string();
    }
    if (entry->path != expected) {
      return false;
    }
  }

  std::string blocking_label;
  if (operations_.has_dirty_editor_tabs_for_path(entry->path, &blocking_label)) {
    ReportGitMessage(NotificationService::Tone::Warning,
                     blocking_label.empty()
                         ? std::string("Save or close dirty tabs before discarding")
                         : "Save or close dirty tabs before discarding " + blocking_label);
    return false;
  }
  const bool staged_new_file = entry->status == project::GitFileStatus::Added &&
                               !entry->is_staged_rename;
  if (entry->section == GitSidebarEntry::Section::Untracked || staged_new_file) {
    if (staged_new_file &&
        !project::GitUnstagePath(project_root_, state_.launcher(), entry->path,
                                false)) {
      ReportGitOperationFailure("discard", *entry);
      return false;
    }
    if (!state_.write_gate()
             .ApplyTreeOp({.kind = project::FileWriteGate::TreeOp::Kind::Trash, .path = entry->path})
             .ok) {
      ReportGitOperationFailure("discard", *entry);
      return false;
    }
  } else if (!project::GitDiscardPath(project_root_, state_.launcher(), entry->path, entry->is_staged_rename)) {
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

}
