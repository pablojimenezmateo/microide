#include "workspace/WorkspaceSidebarCoordinator.h"

#include <algorithm>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include <optional>

#include "editor/TextViewport.h"
#include "project/FileOperationService.h"
#include "project/GitStatusService.h"
#include "workspace/SelectionMovement.h"
#include "workspace/WorkspaceActionTypes.h"

namespace microide::workspace {

namespace {

// Returns the selected entry of a flat sidebar list, or nullptr when the index
// is out of range (which also covers an empty list). Collapses the repeated
// `entries.empty() || selected_index >= entries.size()` guard at the open sites.
template <typename Vec>
const typename Vec::value_type* SelectedListEntry(const Vec& entries, std::size_t selected_index) {
  if (selected_index >= entries.size()) {
    return nullptr;
  }
  return &entries[selected_index];
}

}  // namespace

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
      viewport->MoveCursorTo(caret->first, caret->second);
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

void SidebarCoordinator::MoveSimpleListSelection(
    std::size_t count, std::size_t* selected_index,
    const std::function<ScrollableListLayout(const SDL_FRect&, std::size_t)>& compute_layout,
    int delta) {
  if (MoveSelectionIndex(count, selected_index, delta)) {
    RevealListSelection(count, *selected_index, compute_layout);
  }
}

void SidebarCoordinator::MoveGitSelection(int delta) {
  // Selection is a flat index into git.entries, but the rendered list is a
  // collapsible tree: entries hidden under a collapsed directory must be skipped,
  // or arrow keys walk invisible rows and the highlight appears to vanish. Step
  // through the VISIBLE entry rows of the current line model instead.
  auto& git = state_.sidebar.git;
  if (git.entries.empty() || delta == 0) {
    return;
  }
  static const std::vector<GitSidebarLine> kEmptyLines;
  const std::vector<GitSidebarLine>& lines =
      operations_.build_git_sidebar_lines ? operations_.build_git_sidebar_lines() : kEmptyLines;
  std::vector<std::size_t> visible;
  visible.reserve(git.entries.size());
  for (const GitSidebarLine& line : lines) {
    if (line.kind == GitSidebarLine::Kind::Entry && line.entry_index >= 0) {
      visible.push_back(static_cast<std::size_t>(line.entry_index));
    }
  }
  if (visible.empty()) {
    // No tree model available (or nothing visible): fall back to the flat clamp.
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
    // Selected entry is currently hidden: land on the first visible entry in the
    // direction of travel.
    next_pos = delta > 0 ? 0 : visible.size() - 1;
  }
  if (visible[next_pos] != git.selected_index) {
    git.selected_index = visible[next_pos];
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
  // Tests only carry a line when discovered with one; jump only then.
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

  // Outline rows reuse this storage but the host owns navigation: jump the active
  // editor to the symbol's location instead of invoking a plugin confirm callback.
  // Keep sidebar focus so arrow keys keep browsing the outline (VS Code-style).
  if (ActiveSidebarMode() == SidebarMode::Outline) {
    if (item.path.empty() || !operations_.open_file) {
      return false;
    }
    operations_.open_file(item.path);
    if (editor::TextViewport* viewport =
            operations_.active_editor_viewport ? operations_.active_editor_viewport() : nullptr;
        viewport != nullptr) {
      viewport->MoveCursorTo(item.line > 0 ? item.line - 1 : 0,
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
  // Snapshot the item by value: re-snapshotting below rebuilds the items vector,
  // invalidating any reference into it.
  const plugin::PluginHost::SidebarItem item = *selected;
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
  std::vector<std::filesystem::path> untracked_paths;
  affected_paths.reserve(state_.sidebar.git.entries.size());
  for (const auto& entry : state_.sidebar.git.entries) {
    if (!IsGitWorkflowSection(entry.section)) {
      continue;
    }
    affected_paths.push_back(entry.path.lexically_normal());
    // Files with no committed content to restore are the ones DiscardAll would
    // permanently delete: untracked files (removed by `git clean`) and staged NEW
    // files (status Added — after `reset HEAD` they become untracked and are
    // clean-deleted too). Trash them instead (recoverable), mirroring the
    // single-file untracked-discard policy, and tell DiscardAll to skip its clean.
    // Modified/Deleted tracked files keep their HEAD content and are restored, not
    // trashed.
    if (entry.section == GitSidebarEntry::Section::Untracked ||
        entry.status == project::GitFileStatus::Added) {
      untracked_paths.push_back(entry.path.lexically_normal());
    }
  }
  std::sort(affected_paths.begin(), affected_paths.end());
  affected_paths.erase(std::unique(affected_paths.begin(), affected_paths.end()),
                       affected_paths.end());

  for (const auto& untracked : untracked_paths) {
    // Best-effort: a failed trash should not abort the tracked discard, but it must
    // not be silently `git clean`-deleted either — it simply stays as an untracked
    // file the user can retry on.
    (void)project::FileOperationService::TrashPath(untracked);
  }

  if (!project::GitDiscardAll(project_root_, /*remove_untracked=*/false)) {
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
  if (!project::GitUnstagePath(project_root_, entry->path, entry->is_staged_rename)) {
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
  // A confirm prompt captures the entry index, but an async git-status refresh can
  // reorder/shrink the entries before the user confirms. Re-validate that the index
  // still points at the exact path they confirmed, so a discard (which destroys
  // working-tree changes) never lands on a different file that slid into the slot.
  if (expected_path.has_value() &&
      entry->path.lexically_normal() != expected_path->lexically_normal()) {
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
  // An untracked file has no committed content to restore — "discarding" it deletes a
  // file the user created. The confirm prompt promises "Existing file-operation policy
  // applies (trash when configured)", and the file tree's Delete honors exactly that, so
  // route untracked discard through the same TrashPath (recoverable when trash is on).
  // GitDiscardPath's untracked branch runs `git clean -fd`, which permanently destroys
  // the file despite the prompt — a silent data-loss contract violation. Tracked-path
  // discard stays a git restore.
  if (entry->section == GitSidebarEntry::Section::Untracked) {
    if (!project::FileOperationService::TrashPath(entry->path).ok) {
      ReportGitOperationFailure("discard", *entry);
      return false;
    }
  } else if (!project::GitDiscardPath(project_root_, entry->path, entry->is_staged_rename)) {
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
