#include "workspace/WorkspaceDiffTabCoordinator.h"

#include <algorithm>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "project/GitCompareService.h"
#include "util/TextFileIO.h"
#include "workspace/WorkspaceShell.h"

namespace microide::workspace {

DiffTabCoordinator::DiffTabCoordinator(ProjectWorkspaceState& state, Operations operations)
    : state_(state), operations_(std::move(operations)) {}

std::optional<std::size_t> DiffTabCoordinator::FindOpenCompareTabIndex(
    const std::filesystem::path& path, std::string_view left_ref, std::string_view right_ref) const {
  const std::filesystem::path normalized_path = path.lexically_normal();
  for (std::size_t i = 0; i < state_.open_tabs.size(); ++i) {
    const auto& tab = state_.open_tabs[i];
    if (tab.kind != TabEntry::Kind::Compare || !tab.compare.has_value()) {
      continue;
    }
    if (tab.compare->path == normalized_path && tab.compare->commit_hash == left_ref &&
        tab.compare->right_ref == right_ref) {
      return i;
    }
  }
  return std::nullopt;
}

std::optional<std::size_t> DiffTabCoordinator::FindOpenMergeTabIndex(
    const std::filesystem::path& path) const {
  const std::filesystem::path normalized_path = path.lexically_normal();
  for (std::size_t i = 0; i < state_.open_tabs.size(); ++i) {
    const auto& tab = state_.open_tabs[i];
    if (tab.kind != TabEntry::Kind::Merge || !tab.merge.has_value()) {
      continue;
    }
    if (tab.merge->output_path == normalized_path) {
      return i;
    }
  }
  return std::nullopt;
}

void DiffTabCoordinator::ActivateCompareTab(std::size_t index, bool dismiss_overlay) {
  state_.active_tab_index = index;
  operations_.reveal_active_compare_selection();
  operations_.ensure_active_tab_visible();
  if (dismiss_overlay) {
    operations_.dismiss_overlay(true);
  } else {
    state_.surface.focus = FocusTarget::Editor;
  }
  operations_.request_active_tab_redraw(false);
}

void DiffTabCoordinator::ActivateMergeTab(std::size_t index) {
  state_.active_tab_index = index;
  operations_.reveal_active_merge_selection();
  operations_.ensure_active_tab_visible();
  state_.surface.focus = FocusTarget::Editor;
  operations_.request_active_tab_redraw(false);
}

void DiffTabCoordinator::RefreshExistingCompareTab(std::size_t index,
                                                   const std::filesystem::path& normalized_path,
                                                   bool only_when_clean) {
  if (!state_.open_tabs[index].compare.has_value()) {
    return;
  }
  if (only_when_clean && state_.open_tabs[index].compare->right_viewport.dirty()) {
    return;
  }
  auto rebuilt = operations_.rebuild_compare_tab_entry(normalized_path,
                                                       state_.open_tabs[index].compare.value());
  if (rebuilt.has_value() && rebuilt->compare.has_value()) {
    state_.open_tabs[index] = std::move(*rebuilt);
  }
}

void DiffTabCoordinator::RestoreMergeViewState(MergeTabState& rebuilt_merge,
                                               const MergeTabState& previous_merge) {
  rebuilt_merge.selected_hunk =
      rebuilt_merge.conflicts.empty()
          ? 0
          : std::min(previous_merge.selected_hunk, rebuilt_merge.conflicts.size() - 1);
  rebuilt_merge.scroll_row = previous_merge.scroll_row;
  rebuilt_merge.horizontal_scroll = previous_merge.horizontal_scroll;
  rebuilt_merge.left_divider_fraction = previous_merge.left_divider_fraction;
  rebuilt_merge.right_divider_fraction = previous_merge.right_divider_fraction;
  rebuilt_merge.persistable = previous_merge.persistable;
  rebuilt_merge.result_viewport.SetViewportSize(previous_merge.result_viewport.visible_lines(),
                                                previous_merge.result_viewport.visible_columns());
  rebuilt_merge.result_viewport.SetScrollLine(
      static_cast<std::size_t>(std::max(0, rebuilt_merge.scroll_row)));
  rebuilt_merge.result_viewport.SetHorizontalScroll(rebuilt_merge.horizontal_scroll);
  rebuilt_merge.scroll_row = static_cast<int>(rebuilt_merge.result_viewport.scroll_line());
  rebuilt_merge.horizontal_scroll = rebuilt_merge.result_viewport.horizontal_scroll();
}

void DiffTabCoordinator::OpenComparison(const project::GitCommitEntry& commit) {
  if (const auto existing_index =
          FindOpenCompareTabIndex(state_.overlay.workflow.compare_picker.path, commit.hash, "WORKTREE");
      existing_index.has_value()) {
    operations_.sync_active_editor_tab();
    RefreshExistingCompareTab(*existing_index, state_.overlay.workflow.compare_picker.path, false);
    ActivateCompareTab(*existing_index, true);
    return;
  }

  auto compare_tab = operations_.build_compare_tab_entry(state_.overlay.workflow.compare_picker.path,
                                                         commit, 0);
  if (!compare_tab.has_value()) {
    return;
  }

  operations_.sync_active_editor_tab();
  state_.open_tabs.push_back(std::move(*compare_tab));
  ActivateCompareTab(state_.open_tabs.size() - 1, true);
}

bool DiffTabCoordinator::OpenMergeEditor(const std::filesystem::path& base_path,
                                         const std::filesystem::path& incoming_path,
                                         const std::filesystem::path& current_path,
                                         const std::filesystem::path& output_path) {
  const std::filesystem::path normalized_base = base_path.lexically_normal();
  const std::filesystem::path normalized_incoming = incoming_path.lexically_normal();
  const std::filesystem::path normalized_current = current_path.lexically_normal();
  const std::filesystem::path normalized_output = output_path.lexically_normal();

  if (const auto existing_index = FindOpenMergeTabIndex(normalized_output); existing_index.has_value()) {
    operations_.sync_active_editor_tab();
    if (state_.open_tabs[*existing_index].merge.has_value() &&
        !state_.open_tabs[*existing_index].merge->result_viewport.dirty()) {
      auto rebuilt = operations_.build_merge_tab_entry(normalized_base, normalized_incoming,
                                                       normalized_current, normalized_output);
      if (rebuilt.has_value() && rebuilt->merge.has_value()) {
        RestoreMergeViewState(rebuilt->merge.value(),
                              state_.open_tabs[*existing_index].merge.value());
        state_.open_tabs[*existing_index] = std::move(*rebuilt);
      }
    }
    ActivateMergeTab(*existing_index);
    return true;
  }

  auto merge_tab = operations_.build_merge_tab_entry(normalized_base, normalized_incoming,
                                                     normalized_current, normalized_output);
  if (!merge_tab.has_value()) {
    return false;
  }

  operations_.sync_active_editor_tab();
  state_.open_tabs.push_back(std::move(*merge_tab));
  ActivateMergeTab(state_.open_tabs.size() - 1);
  return true;
}

bool DiffTabCoordinator::OpenWorkingTreeComparison(const std::filesystem::path& path,
                                                   const std::string& left_ref,
                                                   const std::string& left_label) {
  const std::filesystem::path normalized_path = path.lexically_normal();
  if (const auto existing_index = FindOpenCompareTabIndex(normalized_path, left_ref, "WORKTREE");
      existing_index.has_value()) {
    operations_.sync_active_editor_tab();
    RefreshExistingCompareTab(*existing_index, normalized_path, true);
    ActivateCompareTab(*existing_index, false);
    return true;
  }

  const auto left_content = project::ReadGitFileAtCommit(state_.root, normalized_path, left_ref);
  if (!left_content.has_value()) {
    return false;
  }
  const std::optional<std::string> working_content = util::ReadTextFile(normalized_path);
  auto compare_tab = operations_.build_compare_tab_from_buffers(
      normalized_path, left_content->exists ? left_content->content : "",
      working_content.value_or(""), left_label, "Working tree", 0, true);
  if (!compare_tab.has_value() || !compare_tab->compare.has_value()) {
    return false;
  }
  compare_tab->compare->commit_hash = left_ref;
  compare_tab->compare->right_ref = "WORKTREE";
  compare_tab->compare->left_path = normalized_path;
  compare_tab->compare->right_path = normalized_path;
  compare_tab->compare->right_editable = true;
  compare_tab->compare->right_view_active = true;

  operations_.sync_active_editor_tab();
  state_.open_tabs.push_back(std::move(*compare_tab));
  ActivateCompareTab(state_.open_tabs.size() - 1, false);
  return true;
}

bool DiffTabCoordinator::OpenBranchHeadComparison(const std::filesystem::path& path,
                                                  const std::string& left_ref,
                                                  const std::string& left_label,
                                                  const std::string& right_ref,
                                                  const std::string& right_label) {
  const std::filesystem::path normalized_path = path.lexically_normal();
  if (const auto existing_index = FindOpenCompareTabIndex(normalized_path, left_ref, right_ref);
      existing_index.has_value()) {
    operations_.sync_active_editor_tab();
    RefreshExistingCompareTab(*existing_index, normalized_path, true);
    ActivateCompareTab(*existing_index, false);
    return true;
  }

  const auto left_content = project::ReadGitFileAtCommit(state_.root, normalized_path, left_ref);
  const auto right_content = project::ReadGitFileAtCommit(state_.root, normalized_path, right_ref);
  if (!left_content.has_value() || !right_content.has_value()) {
    return false;
  }
  auto compare_tab = operations_.build_compare_tab_from_buffers(
      normalized_path, left_content->exists ? left_content->content : "",
      right_content->exists ? right_content->content : "", left_label, right_label, 0, true);
  if (!compare_tab.has_value() || !compare_tab->compare.has_value()) {
    return false;
  }
  compare_tab->compare->commit_hash = left_ref;
  compare_tab->compare->right_ref = right_ref;
  compare_tab->compare->left_path = normalized_path;
  compare_tab->compare->right_path = normalized_path;

  operations_.sync_active_editor_tab();
  state_.open_tabs.push_back(std::move(*compare_tab));
  ActivateCompareTab(state_.open_tabs.size() - 1, false);
  return true;
}

bool DiffTabCoordinator::OpenGitConflictMerge(const std::filesystem::path& path) {
  const std::filesystem::path normalized_path = path.lexically_normal();
  if (const auto existing_index = FindOpenMergeTabIndex(normalized_path);
      existing_index.has_value() && state_.open_tabs[*existing_index].merge.has_value() &&
      state_.open_tabs[*existing_index].merge->result_viewport.dirty()) {
    operations_.sync_active_editor_tab();
    ActivateMergeTab(*existing_index);
    return true;
  }

  const auto base_content = project::ReadGitFileAtCommit(state_.root, normalized_path, ":1");
  const auto current_content = project::ReadGitFileAtCommit(state_.root, normalized_path, ":2");
  const auto incoming_content = project::ReadGitFileAtCommit(state_.root, normalized_path, ":3");
  if (!current_content.has_value() || !incoming_content.has_value()) {
    return false;
  }

  auto merge_tab = operations_.build_merge_tab_from_buffers(
      normalized_path, base_content.has_value() && base_content->exists ? base_content->content : "",
      incoming_content->exists ? incoming_content->content : "",
      current_content->exists ? current_content->content : "", "Theirs", "Result", "Ours", 0,
      false);
  if (!merge_tab.has_value() || !merge_tab->merge.has_value()) {
    return false;
  }
  merge_tab->merge->base_path = normalized_path;
  merge_tab->merge->incoming_path = normalized_path;
  merge_tab->merge->current_path = normalized_path;

  operations_.sync_active_editor_tab();
  if (const auto existing_index = FindOpenMergeTabIndex(normalized_path); existing_index.has_value()) {
    if (state_.open_tabs[*existing_index].merge.has_value()) {
      RestoreMergeViewState(merge_tab->merge.value(),
                            state_.open_tabs[*existing_index].merge.value());
    }
    state_.open_tabs[*existing_index] = std::move(*merge_tab);
    ActivateMergeTab(*existing_index);
    return true;
  }

  state_.open_tabs.push_back(std::move(*merge_tab));
  ActivateMergeTab(state_.open_tabs.size() - 1);
  return true;
}

DiffTabCoordinator WorkspaceShell::MakeDiffTabCoordinator() {
  return DiffTabCoordinator(
      current_project_state_,
      DiffTabCoordinator::Operations{
          .sync_active_editor_tab = [this]() { SyncActiveEditorTab(); },
          .reveal_active_compare_selection = [this]() { RevealActiveCompareSelection(); },
          .reveal_active_merge_selection = [this]() { RevealActiveMergeSelection(); },
          .ensure_active_tab_visible = [this]() { EnsureActiveTabVisible(); },
          .dismiss_overlay = [this](bool restore_focus) { DismissOverlay(restore_focus); },
          .request_active_tab_redraw =
              [this](bool include_layout) { RequestActiveTabRedraw(include_layout); },
          .build_compare_tab_entry =
              [this](const std::filesystem::path& path,
                     const project::GitCommitEntry& commit,
                     std::size_t selected_row) {
                return BuildCompareTabEntry(path, commit, selected_row);
              },
          .rebuild_compare_tab_entry =
              [this](const std::filesystem::path& path, const CompareTabState& compare_state) {
                return BuildCompareTabEntry(path, compare_state);
              },
          .build_compare_tab_from_buffers =
              [this](const std::filesystem::path& path,
                     const std::string& left_content,
                     const std::string& right_content,
                     const std::string& left_label,
                     const std::string& right_label,
                     std::size_t selected_row,
                     bool persistable) {
                return BuildCompareTabFromBuffers(path, left_content, right_content, left_label,
                                                  right_label, selected_row, persistable);
              },
          .build_merge_tab_entry =
              [this](const std::filesystem::path& base_path,
                     const std::filesystem::path& incoming_path,
                     const std::filesystem::path& current_path,
                     const std::filesystem::path& output_path) {
                return BuildMergeTabEntry(base_path, incoming_path, current_path, output_path);
              },
          .build_merge_tab_from_buffers =
              [this](const std::filesystem::path& output_path,
                     const std::string& base_content,
                     const std::string& incoming_content,
                     const std::string& current_content,
                     const std::string& incoming_label,
                     const std::string& result_label,
                     const std::string& current_label,
                     std::size_t selected_hunk,
                     bool persistable) {
                return BuildMergeTabFromBuffers(output_path, base_content, incoming_content,
                                                current_content, incoming_label, result_label,
                                                current_label, selected_hunk, persistable);
              },
      });
}

std::optional<std::size_t> WorkspaceShell::FindOpenCompareTabIndex(
    const std::filesystem::path& path,
    std::string_view left_ref,
    std::string_view right_ref) const {
  return const_cast<WorkspaceShell*>(this)->MakeDiffTabCoordinator()
      .FindOpenCompareTabIndex(path, left_ref, right_ref);
}

std::optional<std::size_t> WorkspaceShell::FindOpenMergeTabIndex(
    const std::filesystem::path& path) const {
  return const_cast<WorkspaceShell*>(this)->MakeDiffTabCoordinator().FindOpenMergeTabIndex(path);
}

void WorkspaceShell::OpenComparison(const project::GitCommitEntry& commit) {
  MakeDiffTabCoordinator().OpenComparison(commit);
}

bool WorkspaceShell::OpenMergeEditor(const std::filesystem::path& base_path,
                                     const std::filesystem::path& incoming_path,
                                     const std::filesystem::path& current_path,
                                     const std::filesystem::path& output_path) {
  return MakeDiffTabCoordinator().OpenMergeEditor(base_path, incoming_path, current_path,
                                                  output_path);
}

bool WorkspaceShell::OpenWorkingTreeComparison(const std::filesystem::path& path,
                                               const std::string& left_ref,
                                               const std::string& left_label) {
  return MakeDiffTabCoordinator().OpenWorkingTreeComparison(path, left_ref, left_label);
}

bool WorkspaceShell::OpenBranchHeadComparison(const std::filesystem::path& path,
                                              const std::string& left_ref,
                                              const std::string& left_label,
                                              const std::string& right_ref,
                                              const std::string& right_label) {
  return MakeDiffTabCoordinator().OpenBranchHeadComparison(path, left_ref, left_label, right_ref,
                                                           right_label);
}

bool WorkspaceShell::OpenGitConflictMerge(const std::filesystem::path& path) {
  return MakeDiffTabCoordinator().OpenGitConflictMerge(path);
}

}  // namespace microide::workspace
