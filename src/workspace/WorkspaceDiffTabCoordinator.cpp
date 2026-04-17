#include "workspace/WorkspaceDiffTabCoordinator.h"

#include <algorithm>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include "project/GitCompareService.h"
#include "util/TextFileIO.h"

namespace microide::workspace {

WorkspaceShell::DiffTabCoordinator::DiffTabCoordinator(WorkspaceShell& shell) : shell_(shell) {}

std::optional<std::size_t> WorkspaceShell::DiffTabCoordinator::FindOpenCompareTabIndex(
    const std::filesystem::path& path,
    std::string_view left_ref,
    std::string_view right_ref) const {
  const std::filesystem::path normalized_path = path.lexically_normal();
  for (std::size_t i = 0; i < shell_.open_tabs_.size(); ++i) {
    const auto& tab = shell_.open_tabs_[i];
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

std::optional<std::size_t> WorkspaceShell::DiffTabCoordinator::FindOpenMergeTabIndex(
    const std::filesystem::path& path) const {
  const std::filesystem::path normalized_path = path.lexically_normal();
  for (std::size_t i = 0; i < shell_.open_tabs_.size(); ++i) {
    const auto& tab = shell_.open_tabs_[i];
    if (tab.kind != TabEntry::Kind::Merge || !tab.merge.has_value()) {
      continue;
    }
    if (tab.merge->output_path == normalized_path) {
      return i;
    }
  }
  return std::nullopt;
}

void WorkspaceShell::DiffTabCoordinator::ActivateCompareTab(std::size_t index,
                                                            bool dismiss_overlay) {
  shell_.active_tab_index_ = index;
  shell_.RevealActiveCompareSelection();
  shell_.EnsureActiveTabVisible();
  if (dismiss_overlay) {
    shell_.DismissOverlay(true);
  } else {
    shell_.surface_.focus = FocusTarget::Editor;
  }
  shell_.RequestActiveTabRedraw(false);
}

void WorkspaceShell::DiffTabCoordinator::ActivateMergeTab(std::size_t index) {
  shell_.active_tab_index_ = index;
  shell_.RevealActiveMergeSelection();
  shell_.EnsureActiveTabVisible();
  shell_.surface_.focus = FocusTarget::Editor;
  shell_.RequestActiveTabRedraw(false);
}

void WorkspaceShell::DiffTabCoordinator::RefreshExistingCompareTab(
    std::size_t index,
    const std::filesystem::path& normalized_path,
    bool only_when_clean) {
  if (!shell_.open_tabs_[index].compare.has_value()) {
    return;
  }
  if (only_when_clean && shell_.open_tabs_[index].compare->right_viewport.dirty()) {
    return;
  }
  auto rebuilt =
      shell_.BuildCompareTabEntry(normalized_path, shell_.open_tabs_[index].compare.value());
  if (rebuilt.has_value() && rebuilt->compare.has_value()) {
    shell_.open_tabs_[index] = std::move(*rebuilt);
  }
}

void WorkspaceShell::DiffTabCoordinator::RestoreMergeViewState(MergeTabState& rebuilt_merge,
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

void WorkspaceShell::DiffTabCoordinator::OpenComparison(const project::GitCommitEntry& commit) {
  if (const auto existing_index =
          FindOpenCompareTabIndex(shell_.overlay_workflow_.compare_picker.path, commit.hash, "WORKTREE");
      existing_index.has_value()) {
    shell_.SyncActiveEditorTab();
    RefreshExistingCompareTab(*existing_index, shell_.overlay_workflow_.compare_picker.path, false);
    ActivateCompareTab(*existing_index, true);
    return;
  }

  auto compare_tab = shell_.BuildCompareTabEntry(shell_.overlay_workflow_.compare_picker.path, commit);
  if (!compare_tab.has_value()) {
    return;
  }

  shell_.SyncActiveEditorTab();
  shell_.open_tabs_.push_back(std::move(*compare_tab));
  ActivateCompareTab(shell_.open_tabs_.size() - 1, true);
}

bool WorkspaceShell::DiffTabCoordinator::OpenMergeEditor(
    const std::filesystem::path& base_path,
    const std::filesystem::path& incoming_path,
    const std::filesystem::path& current_path,
    const std::filesystem::path& output_path) {
  const std::filesystem::path normalized_base = base_path.lexically_normal();
  const std::filesystem::path normalized_incoming = incoming_path.lexically_normal();
  const std::filesystem::path normalized_current = current_path.lexically_normal();
  const std::filesystem::path normalized_output = output_path.lexically_normal();

  if (const auto existing_index = FindOpenMergeTabIndex(normalized_output); existing_index.has_value()) {
    shell_.SyncActiveEditorTab();
    if (shell_.open_tabs_[*existing_index].merge.has_value() &&
        !shell_.open_tabs_[*existing_index].merge->result_viewport.dirty()) {
      auto rebuilt = shell_.BuildMergeTabEntry(normalized_base, normalized_incoming,
                                               normalized_current, normalized_output);
      if (rebuilt.has_value() && rebuilt->merge.has_value()) {
        RestoreMergeViewState(rebuilt->merge.value(), shell_.open_tabs_[*existing_index].merge.value());
        shell_.open_tabs_[*existing_index] = std::move(*rebuilt);
      }
    }
    ActivateMergeTab(*existing_index);
    return true;
  }

  auto merge_tab = shell_.BuildMergeTabEntry(normalized_base, normalized_incoming,
                                             normalized_current, normalized_output);
  if (!merge_tab.has_value()) {
    return false;
  }

  shell_.SyncActiveEditorTab();
  shell_.open_tabs_.push_back(std::move(*merge_tab));
  ActivateMergeTab(shell_.open_tabs_.size() - 1);
  return true;
}

bool WorkspaceShell::DiffTabCoordinator::OpenWorkingTreeComparison(
    const std::filesystem::path& path,
    const std::string& left_ref,
    const std::string& left_label) {
  const std::filesystem::path normalized_path = path.lexically_normal();
  if (const auto existing_index = FindOpenCompareTabIndex(normalized_path, left_ref, "WORKTREE");
      existing_index.has_value()) {
    shell_.SyncActiveEditorTab();
    RefreshExistingCompareTab(*existing_index, normalized_path, true);
    ActivateCompareTab(*existing_index, false);
    return true;
  }

  const auto left_content = project::ReadGitFileAtCommit(shell_.project_root_, normalized_path, left_ref);
  if (!left_content.has_value()) {
    return false;
  }
  const std::optional<std::string> working_content = util::ReadTextFile(normalized_path);
  auto compare_tab = shell_.BuildCompareTabFromBuffers(
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

  shell_.SyncActiveEditorTab();
  shell_.open_tabs_.push_back(std::move(*compare_tab));
  ActivateCompareTab(shell_.open_tabs_.size() - 1, false);
  return true;
}

bool WorkspaceShell::DiffTabCoordinator::OpenBranchHeadComparison(
    const std::filesystem::path& path,
    const std::string& left_ref,
    const std::string& left_label,
    const std::string& right_ref,
    const std::string& right_label) {
  const std::filesystem::path normalized_path = path.lexically_normal();
  if (const auto existing_index = FindOpenCompareTabIndex(normalized_path, left_ref, right_ref);
      existing_index.has_value()) {
    shell_.SyncActiveEditorTab();
    RefreshExistingCompareTab(*existing_index, normalized_path, true);
    ActivateCompareTab(*existing_index, false);
    return true;
  }

  const auto left_content = project::ReadGitFileAtCommit(shell_.project_root_, normalized_path, left_ref);
  const auto right_content = project::ReadGitFileAtCommit(shell_.project_root_, normalized_path, right_ref);
  if (!left_content.has_value() || !right_content.has_value()) {
    return false;
  }
  auto compare_tab = shell_.BuildCompareTabFromBuffers(
      normalized_path, left_content->exists ? left_content->content : "",
      right_content->exists ? right_content->content : "", left_label, right_label, 0, true);
  if (!compare_tab.has_value() || !compare_tab->compare.has_value()) {
    return false;
  }
  compare_tab->compare->commit_hash = left_ref;
  compare_tab->compare->right_ref = right_ref;
  compare_tab->compare->left_path = normalized_path;
  compare_tab->compare->right_path = normalized_path;

  shell_.SyncActiveEditorTab();
  shell_.open_tabs_.push_back(std::move(*compare_tab));
  ActivateCompareTab(shell_.open_tabs_.size() - 1, false);
  return true;
}

bool WorkspaceShell::DiffTabCoordinator::OpenGitConflictMerge(const std::filesystem::path& path) {
  const std::filesystem::path normalized_path = path.lexically_normal();
  if (const auto existing_index = FindOpenMergeTabIndex(normalized_path);
      existing_index.has_value() && shell_.open_tabs_[*existing_index].merge.has_value() &&
      shell_.open_tabs_[*existing_index].merge->result_viewport.dirty()) {
    shell_.SyncActiveEditorTab();
    ActivateMergeTab(*existing_index);
    return true;
  }

  const auto base_content = project::ReadGitFileAtCommit(shell_.project_root_, normalized_path, ":1");
  const auto current_content = project::ReadGitFileAtCommit(shell_.project_root_, normalized_path, ":2");
  const auto incoming_content = project::ReadGitFileAtCommit(shell_.project_root_, normalized_path, ":3");
  if (!current_content.has_value() || !incoming_content.has_value()) {
    return false;
  }

  auto merge_tab = shell_.BuildMergeTabFromBuffers(
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

  shell_.SyncActiveEditorTab();
  if (const auto existing_index = FindOpenMergeTabIndex(normalized_path); existing_index.has_value()) {
    if (shell_.open_tabs_[*existing_index].merge.has_value()) {
      RestoreMergeViewState(merge_tab->merge.value(), shell_.open_tabs_[*existing_index].merge.value());
    }
    shell_.open_tabs_[*existing_index] = std::move(*merge_tab);
    ActivateMergeTab(*existing_index);
    return true;
  }

  shell_.open_tabs_.push_back(std::move(*merge_tab));
  ActivateMergeTab(shell_.open_tabs_.size() - 1);
  return true;
}

std::optional<std::size_t> WorkspaceShell::FindOpenCompareTabIndex(
    const std::filesystem::path& path,
    std::string_view left_ref,
    std::string_view right_ref) const {
  return DiffTabCoordinator(*const_cast<WorkspaceShell*>(this))
      .FindOpenCompareTabIndex(path, left_ref, right_ref);
}

std::optional<std::size_t> WorkspaceShell::FindOpenMergeTabIndex(
    const std::filesystem::path& path) const {
  return DiffTabCoordinator(*const_cast<WorkspaceShell*>(this)).FindOpenMergeTabIndex(path);
}

void WorkspaceShell::OpenComparison(const project::GitCommitEntry& commit) {
  DiffTabCoordinator(*this).OpenComparison(commit);
}

bool WorkspaceShell::OpenMergeEditor(const std::filesystem::path& base_path,
                                     const std::filesystem::path& incoming_path,
                                     const std::filesystem::path& current_path,
                                     const std::filesystem::path& output_path) {
  return DiffTabCoordinator(*this).OpenMergeEditor(base_path, incoming_path, current_path,
                                                   output_path);
}

bool WorkspaceShell::OpenWorkingTreeComparison(const std::filesystem::path& path,
                                               const std::string& left_ref,
                                               const std::string& left_label) {
  return DiffTabCoordinator(*this).OpenWorkingTreeComparison(path, left_ref, left_label);
}

bool WorkspaceShell::OpenBranchHeadComparison(const std::filesystem::path& path,
                                              const std::string& left_ref,
                                              const std::string& left_label,
                                              const std::string& right_ref,
                                              const std::string& right_label) {
  return DiffTabCoordinator(*this).OpenBranchHeadComparison(path, left_ref, left_label, right_ref,
                                                            right_label);
}

bool WorkspaceShell::OpenGitConflictMerge(const std::filesystem::path& path) {
  return DiffTabCoordinator(*this).OpenGitConflictMerge(path);
}

}  // namespace microide::workspace
