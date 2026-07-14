#include "workspace/WorkspaceShell.h"

#include <algorithm>

#include "compare/BranchReviewStateTypes.h"
#include "compare/CompareReviewTypes.h"
#include "workspace/BranchReviewStateBridge.h"
#include "workspace/CompareMergeService.h"
#include "workspace/CompareTabReview.h"
#include "workspace/WorkspaceLayout.h"
#include "workspace/WorkspacePersistenceCoordinator.h"

namespace microide::workspace {

DiffTabCoordinator WorkspaceShell::MakeDiffTabCoordinator() {
  return DiffTabCoordinator(
      context_.current_project_state,
      DiffTabCoordinator::Operations{
          .sync_active_editor_tab = [this]() { SyncActiveEditorTab(); },
          .notify_plugin_buffer_open =
              [this](const std::filesystem::path& path) { NotifyPluginBufferOpen(path); },
          .reveal_active_compare_selection = [this]() { RevealActiveCompareSelection(); },
          .reveal_active_merge_selection = [this]() { RevealActiveMergeSelection(); },
          .ensure_active_tab_visible = [this]() { tab_strip_chrome_.EnsureActiveTabVisible(); },
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
          .finalize_git_merge_tab =
              [this](MergeTabState& merge_tab, const std::filesystem::path& path) {
                FinalizeGitMergeTab(merge_tab, path);
              },
      });
}

CompareInteractionCoordinator WorkspaceShell::MakeCompareInteractionCoordinator() {
  return CompareInteractionCoordinator(
      context_.current_project_state,
      CompareInteractionCoordinator::Operations{
          .active_sidebar_mode = [this]() { return ActiveSidebarMode(); },
          .show_compare_picker_overlay = [this]() { ShowOverlay(OverlayMode::CommitPicker); },
          .reset_overlay_scroll = [this]() { ResetOverlayScroll(); },
          .request_overlay_redraw = [this]() { RequestOverlayRedraw(); },
          .reveal_compare_picker_selection =
              [this]() {
                if (const auto layout = CurrentWorkspaceLayout(); layout.has_value()) {
                  RevealOverlaySelection(ComputeOverlayRect(layout->editor_area));
                }
              },
          .open_comparison =
              [this](const project::GitCommitEntry& commit) { OpenComparison(commit); },
          .set_outgoing_base_ref =
              [this](const std::string& ref, const std::string& /*label*/) {
                SetGitOutgoingBaseChoice(OutgoingBaseChoice{
                    .kind = OutgoingBaseChoice::Kind::SpecificRef,
                    .custom_ref = ref,
                });
              },
          .active_compare_tab = [this]() { return ActiveCompareTab(); },
          .active_merge_tab = [this]() { return ActiveMergeTab(); },
          .open_file = [this](const std::filesystem::path& path) { OpenFile(path); },
          .active_editor_viewport = [this]() { return ActiveEditorViewport(); },
          .reveal_active_compare_selection = [this]() { RevealActiveCompareSelection(); },
          .request_compare_row_range_redraw =
              [this](std::size_t start, std::size_t end) {
                RequestCompareRowRangeRedraw(start, end);
              },
          .sync_compare_viewport_scroll =
              [this](CompareTabState& compare_tab) { SyncCompareViewportScroll(compare_tab); },
          .scroll_compare_rows =
              [this](CompareTabState& compare_tab, int delta) {
                if (const auto layout_state = CurrentWorkspaceLayout(); layout_state.has_value()) {
                  const WorkspaceLayout layout = *layout_state;
                  const auto surface_layout =
                      ComputeCompareSurfaceLayout(layout.editor_surface, compare_tab);
                  const auto scroll_layout =
                      ComputeCompareScrollLayout(layout.editor_surface, surface_layout, compare_tab);
                  compare_tab.scroll_row = std::clamp(scroll_layout.vertical_scroll + delta, 0,
                                                      scroll_layout.max_vertical_scroll);
                  SyncCompareViewportScroll(compare_tab);
                }
              },
          .scroll_compare_columns =
              [this](CompareTabState& compare_tab, int delta) {
                if (const auto layout_state = CurrentWorkspaceLayout(); layout_state.has_value()) {
                  const WorkspaceLayout layout = *layout_state;
                  const auto surface_layout =
                      ComputeCompareSurfaceLayout(layout.editor_surface, compare_tab);
                  const auto scroll_layout =
                      ComputeCompareScrollLayout(layout.editor_surface, surface_layout, compare_tab);
                  const long long target_scroll =
                      static_cast<long long>(scroll_layout.horizontal_scroll) +
                      static_cast<long long>(delta);
                  compare_tab.horizontal_scroll = static_cast<std::size_t>(std::clamp(
                      target_scroll, 0LL,
                      static_cast<long long>(scroll_layout.max_horizontal_scroll)));
                  SyncCompareViewportScroll(compare_tab);
                }
              },
          .request_editor_surface_redraw = [this]() { RequestEditorSurfaceRedraw(); },
          .reveal_active_merge_selection = [this]() { RevealActiveMergeSelection(); },
          .request_merge_conflict_redraw =
              [this](std::size_t conflict_index) { RequestMergeConflictRedraw(conflict_index); },
          .scroll_merge_columns =
              [this](MergeTabState& merge_tab, int delta) {
                if (const auto layout_state = CurrentWorkspaceLayout(); layout_state.has_value()) {
                  const WorkspaceLayout layout = *layout_state;
                  const auto surface_layout =
                      ComputeMergeSurfaceLayout(layout.editor_surface, merge_tab);
                  const auto scroll_layout =
                      ComputeMergeScrollLayout(layout.editor_surface, surface_layout, merge_tab);
                  const long long target_scroll =
                      static_cast<long long>(scroll_layout.horizontal_scroll) +
                      static_cast<long long>(delta);
                  merge_tab.horizontal_scroll = static_cast<std::size_t>(std::clamp(
                      target_scroll, 0LL,
                      static_cast<long long>(scroll_layout.max_horizontal_scroll)));
                  merge_tab.result_viewport.SetHorizontalScroll(merge_tab.horizontal_scroll);
                  merge_tab.horizontal_scroll = merge_tab.result_viewport.horizontal_scroll();
                }
              },
          .update_merge_max_visual_columns =
              [this](MergeTabState& merge_tab, const std::vector<std::string>& replacement_lines) {
                UpdateMergeMaxVisualColumns(merge_tab, replacement_lines);
              },
          .request_merge_result_line_to_bottom_redraw =
              [this](std::size_t start_line) { RequestMergeResultLineToBottomRedraw(start_line); },
          .request_active_editable_blame_neighborhood_redraw =
              [this](std::size_t before, std::size_t after) {
                RequestActiveEditableBlameNeighborhoodRedraw(before, after);
              },
          .request_tab_strip_redraw = [this]() { RequestTabStripRedraw(); },
          .write_clipboard_text =
              [this](std::string_view text) { return WriteClipboardText(text); },
          .open_working_tree_comparison =
              [this](const std::filesystem::path& path, const std::string& left_ref,
                     const std::string& left_label) {
                return OpenWorkingTreeComparison(path, left_ref, left_label);
              },
          .open_branch_head_comparison =
              [this](const std::filesystem::path& path, const std::string& left_ref,
                     const std::string& left_label, const std::string& right_ref,
                     const std::string& right_label) {
                return OpenBranchHeadComparison(path, left_ref, left_label, right_ref, right_label);
              },
          .refresh_compare_tab_derived_state =
              [this](CompareTabState& compare_tab) { RefreshCompareTabDerivedState(compare_tab); },
          .stage_compare_hunk = [this]() { StageCompareHunk(); },
          .stage_compare_selected_lines = [this]() { StageCompareSelectedLines(); },
          .unstage_compare_hunk = [this]() { UnstageCompareHunk(); },
          .unstage_compare_selected_lines = [this]() { UnstageCompareSelectedLines(); },
          .open_discard_compare_hunk_prompt = [this]() { OpenDiscardCompareHunkPrompt(); },
          .open_discard_compare_selected_lines_prompt =
              [this]() { OpenDiscardCompareSelectedLinesPrompt(); },
          .save_active_merge_tab =
              [this]() {
                MergeTabState* merge_tab = ActiveMergeTab();
                return merge_tab != nullptr && merge_tab->result_viewport.Save();
              },
          .stage_merge_result_path =
              [this](const std::filesystem::path& path) {
                return project::GitStagePath(context_.current_project_state.root, path);
              },
          .refresh_git_sidebar = [this]() { RefreshGitSidebar(); },
          .request_compare_file_history =
              [this](const std::filesystem::path& path) {
                RequestComparePickerFileHistory(path);
              },
          .request_outgoing_base_refs = [this]() { RequestComparePickerOutgoingBase(); },
      });
}

CompareMergeService WorkspaceShell::MakeCompareMergeService() {
  return CompareMergeService(MakeDiffTabCoordinator(), MakeCompareInteractionCoordinator());
}

std::optional<std::size_t> WorkspaceShell::FindOpenCompareTabIndex(
    const std::filesystem::path& path,
    std::string_view left_ref,
    std::string_view right_ref) const {
  return const_cast<WorkspaceShell*>(this)->MakeCompareMergeService().FindOpenCompareTabIndex(
      path, left_ref, right_ref);
}

std::optional<std::size_t> WorkspaceShell::FindOpenMergeTabIndex(
    const std::filesystem::path& path) const {
  return const_cast<WorkspaceShell*>(this)->MakeCompareMergeService().FindOpenMergeTabIndex(path);
}

void WorkspaceShell::OpenComparison(const project::GitCommitEntry& commit) {
  MakeCompareMergeService().OpenComparison(commit);
}

bool WorkspaceShell::OpenMergeEditor(const std::filesystem::path& base_path,
                                     const std::filesystem::path& incoming_path,
                                     const std::filesystem::path& current_path,
                                     const std::filesystem::path& output_path) {
  return MakeCompareMergeService().OpenMergeEditor(base_path, incoming_path, current_path,
                                                   output_path);
}

bool WorkspaceShell::OpenWorkingTreeComparison(const std::filesystem::path& path,
                                               const std::string& left_ref,
                                               const std::string& left_label) {
  return MakeCompareMergeService().OpenWorkingTreeComparison(path, left_ref, left_label);
}

bool WorkspaceShell::OpenBranchHeadComparison(const std::filesystem::path& path,
                                              const std::string& left_ref,
                                              const std::string& left_label,
                                              const std::string& right_ref,
                                              const std::string& right_label) {
  return MakeCompareMergeService().OpenBranchHeadComparison(path, left_ref, left_label, right_ref,
                                                            right_label);
}

bool WorkspaceShell::OpenGitConflictMerge(const std::filesystem::path& path) {
  return MakeCompareMergeService().OpenGitConflictMerge(path);
}

void WorkspaceShell::OpenComparePicker() {
  MakeCompareMergeService().OpenPicker();
}

bool WorkspaceShell::OpenComparePickerForPath(const std::filesystem::path& path,
                                              std::string_view commit_spec) {
  return MakeCompareMergeService().OpenPickerForPath(path, commit_spec);
}

void WorkspaceShell::OpenOutgoingBaseRefPicker() {
  MakeCompareMergeService().OpenOutgoingBasePicker();
}

void WorkspaceShell::RequestComparePickerFileHistory(const std::filesystem::path& path) {
  auto& picker = context_.current_project_state.overlay.workflow.compare_picker;
  const std::uint64_t generation = ++compare_picker_generation_;
  picker.active_request_generation = generation;

  const std::filesystem::path root = context_.current_project_state.root;
  // Copy the provider (fall back to the real free function) so the worker never
  // touches shell state. The captured closure produces raw commits; the mailbox
  // marshals a completion back to the render thread (see ApplyComparePickerFileHistory).
  auto provider = compare_picker_file_history_provider_
                      ? compare_picker_file_history_provider_
                      : std::function<project::GitFileHistoryResult(
                            const std::filesystem::path&, const std::filesystem::path&)>(
                            &project::CollectGitFileHistory);
  project_background_executor_.PostLatest(
      "compare-picker",
      [this, root, path, generation, provider = std::move(provider)]() {
        project::GitFileHistoryResult history = provider(root, path);
        compare_picker_mailbox_.Post(
            [this, generation, history = std::move(history)]() mutable {
              ApplyComparePickerFileHistory(generation, history);
            });
      });
}

void WorkspaceShell::RequestComparePickerOutgoingBase() {
  auto& picker = context_.current_project_state.overlay.workflow.compare_picker;
  const std::uint64_t generation = ++compare_picker_generation_;
  picker.active_request_generation = generation;

  const std::filesystem::path root = context_.current_project_state.root;
  auto branches_provider =
      compare_picker_branches_provider_
          ? compare_picker_branches_provider_
          : std::function<std::vector<project::GitBranchReference>(const std::filesystem::path&)>(
                &project::CollectGitBranches);
  auto commits_provider =
      compare_picker_recent_commits_provider_
          ? compare_picker_recent_commits_provider_
          : std::function<std::vector<project::GitCommitEntry>(const std::filesystem::path&,
                                                               std::size_t)>(
                &project::CollectGitRecentCommits);
  project_background_executor_.PostLatest(
      "compare-picker",
      [this, root, generation, branches_provider = std::move(branches_provider),
       commits_provider = std::move(commits_provider)]() {
        std::vector<project::GitBranchReference> branches = branches_provider(root);
        std::vector<project::GitCommitEntry> commits = commits_provider(root, 50);
        compare_picker_mailbox_.Post([this, generation, branches = std::move(branches),
                                      commits = std::move(commits)]() mutable {
          ApplyComparePickerOutgoingBase(generation, branches, commits);
        });
      });
}

bool WorkspaceShell::ComparePickerRequestCurrent(std::uint64_t generation) const {
  const auto& overlay = context_.current_project_state.overlay;
  return overlay.visible && overlay.mode == OverlayMode::CommitPicker &&
         overlay.workflow.compare_picker.active_request_generation == generation;
}

void WorkspaceShell::ApplyComparePickerFileHistory(
    std::uint64_t generation, const project::GitFileHistoryResult& history) {
  if (!ComparePickerRequestCurrent(generation)) {
    return;  // Overlay closed, project switched, or a newer picker superseded this.
  }
  MakeCompareMergeService().ApplyFileHistoryResult(history);
}

void WorkspaceShell::ApplyComparePickerOutgoingBase(
    std::uint64_t generation, const std::vector<project::GitBranchReference>& branches,
    const std::vector<project::GitCommitEntry>& commits) {
  if (!ComparePickerRequestCurrent(generation)) {
    return;
  }
  MakeCompareMergeService().ApplyOutgoingBaseResult(branches, commits);
}

void WorkspaceShell::RefreshComparePicker() {
  MakeCompareMergeService().RefreshPicker();
}

void WorkspaceShell::MoveComparePickerSelection(int delta) {
  MakeCompareMergeService().MovePickerSelection(delta);
}

void WorkspaceShell::OpenSelectedCompareCommit() {
  MakeCompareMergeService().OpenSelectedCommit();
}

void WorkspaceShell::OpenWorkingFileFromCompare() {
  MakeCompareMergeService().OpenWorkingFileFromCompare();
}

void WorkspaceShell::OpenMergeResultFile() {
  MakeCompareMergeService().OpenMergeResultFile();
}

void WorkspaceShell::MoveCompareSelection(int delta) {
  MakeCompareMergeService().MoveCompareSelection(delta);
}

void WorkspaceShell::JumpCompareHunk(int delta) {
  MakeCompareMergeService().JumpCompareHunk(delta);
}

void WorkspaceShell::RefreshOpenCompareTabsForPath(const std::filesystem::path& path) {
  const std::filesystem::path normalized_path = path.lexically_normal();
  for (std::size_t index = 0; index < context_.current_project_state.focused_group().open_tabs.size(); ++index) {
    const auto& tab = context_.current_project_state.focused_group().open_tabs[index];
    if (tab.kind != TabEntry::Kind::Compare || !tab.compare.has_value() ||
        tab.compare->path != normalized_path) {
      continue;
    }
    auto rebuilt = BuildCompareTabEntry(normalized_path, tab.compare.value());
    if (!rebuilt.has_value() || !rebuilt->compare.has_value()) {
      continue;
    }
    context_.current_project_state.focused_group().open_tabs[index] = std::move(*rebuilt);
    if (index == context_.current_project_state.focused_group().active_tab_index) {
      RevealActiveCompareSelection();
      RequestActiveTabRedraw(false);
    }
  }
}

void WorkspaceShell::StageCompareHunk() {
  CompareTabState* compare_tab = ActiveCompareTab();
  if (compare_tab != nullptr) {
    patch_apply_service_.RequestStageHunk(*compare_tab);
  }
}

void WorkspaceShell::StageCompareSelectedLines() {
  CompareTabState* compare_tab = ActiveCompareTab();
  if (compare_tab != nullptr) {
    patch_apply_service_.RequestStageSelectedLines(*compare_tab);
  }
}

void WorkspaceShell::UnstageCompareHunk() {
  CompareTabState* compare_tab = ActiveCompareTab();
  if (compare_tab != nullptr) {
    patch_apply_service_.RequestUnstageHunk(*compare_tab);
  }
}

void WorkspaceShell::UnstageCompareSelectedLines() {
  CompareTabState* compare_tab = ActiveCompareTab();
  if (compare_tab != nullptr) {
    patch_apply_service_.RequestUnstageSelectedLines(*compare_tab);
  }
}

void WorkspaceShell::OpenDiscardCompareHunkPrompt() {
  CompareTabState* compare_tab = ActiveCompareTab();
  if (compare_tab != nullptr) {
    patch_apply_service_.RequestDiscardHunkPreview(*compare_tab);
  }
}

void WorkspaceShell::OpenDiscardCompareSelectedLinesPrompt() {
  CompareTabState* compare_tab = ActiveCompareTab();
  if (compare_tab != nullptr) {
    patch_apply_service_.RequestDiscardSelectedLinesPreview(*compare_tab);
  }
}

void WorkspaceShell::ScrollCompareRows(int delta) {
  MakeCompareMergeService().ScrollCompareRows(delta);
}

void WorkspaceShell::ScrollCompareColumns(int delta) {
  MakeCompareMergeService().ScrollCompareColumns(delta);
}

void WorkspaceShell::MoveMergeSelection(int delta) {
  MakeCompareMergeService().MoveMergeSelection(delta);
}

void WorkspaceShell::ScrollMergeColumns(int delta) {
  MakeCompareMergeService().ScrollMergeColumns(delta);
}

void WorkspaceShell::ApplyMergeChoice(compare::MergeChoice choice) {
  MakeCompareMergeService().ApplyMergeChoice(choice);
}

void WorkspaceShell::ResetMergeHunk() {
  MakeCompareMergeService().ResetMergeHunk();
}

void WorkspaceShell::JumpNextUnresolvedMergeConflict() {
  MakeCompareMergeService().JumpNextUnresolvedMergeConflict();
}

void WorkspaceShell::ToggleMergeBasePane() {
  MakeCompareMergeService().ToggleMergeBasePane();
}

void WorkspaceShell::CopyMergeIncomingSnippet() {
  MakeCompareMergeService().CopyMergeSideSnippet(true);
}

void WorkspaceShell::CopyMergeCurrentSnippet() {
  MakeCompareMergeService().CopyMergeSideSnippet(false);
}

void WorkspaceShell::MarkMergeResolved() {
  MakeCompareMergeService().MarkMergeResolved();
}

void WorkspaceShell::PersistBranchReviewState() {
  MakePersistenceCoordinator().SaveConfigState();
}

void WorkspaceShell::MarkActiveBranchFileReviewed() {
  CompareTabState* compare_tab = ActiveCompareTab();
  if (compare_tab == nullptr || compare_tab->review_mode != compare::CompareReviewMode::Branch) {
    return;
  }
  context_.current_project_state.branch_review.MarkFileReviewed(compare_tab->branch_target,
                                                                compare_tab->path);
  ApplyBranchReviewPresentationMarkers(*compare_tab, context_.current_project_state.branch_review);
  PersistBranchReviewState();
  RequestActiveTabRedraw(true);
  RequestSidebarRedraw();
}

void WorkspaceShell::MarkActiveBranchHunkReviewed() {
  CompareTabState* compare_tab = ActiveCompareTab();
  if (compare_tab == nullptr || compare_tab->review_mode != compare::CompareReviewMode::Branch) {
    return;
  }
  const int hunk_index = CompareTabSelectedHunkIndex(*compare_tab);
  if (hunk_index < 0) {
    return;
  }
  const compare::BranchReviewHunkIdentity identity = compare::ComputeBranchReviewHunkIdentity(
      compare_tab->model, hunk_index, compare_tab->path);
  context_.current_project_state.branch_review.MarkHunkReviewed(compare_tab->branch_target, identity);
  ApplyBranchReviewPresentationMarkers(*compare_tab, context_.current_project_state.branch_review);
  PersistBranchReviewState();
  RequestActiveTabRedraw(true);
}

void WorkspaceShell::ClearActiveBranchReviewState() {
  CompareTabState* compare_tab = ActiveCompareTab();
  if (compare_tab != nullptr && compare_tab->review_mode == compare::CompareReviewMode::Branch) {
    context_.current_project_state.branch_review.ClearTarget(compare_tab->branch_target);
    ApplyBranchReviewPresentationMarkers(*compare_tab, context_.current_project_state.branch_review);
    RequestActiveTabRedraw(true);
  } else if (const std::optional<compare::BranchReviewTargetIdentity> target =
                 OutgoingBranchReviewTarget(context_.current_project_state.sidebar.git,
                                            context_.current_project_state.root);
             target.has_value()) {
    context_.current_project_state.branch_review.ClearTarget(*target);
  }
  PersistBranchReviewState();
  RequestSidebarRedraw();
}

void WorkspaceShell::EditActiveBranchReviewNote(const std::string& note_text) {
  CompareTabState* compare_tab = ActiveCompareTab();
  if (compare_tab == nullptr || compare_tab->review_mode != compare::CompareReviewMode::Branch) {
    return;
  }
  const int hunk_index = CompareTabSelectedHunkIndex(*compare_tab);
  std::optional<compare::BranchReviewHunkIdentity> hunk_identity;
  compare::BranchReviewNoteScope scope = compare::BranchReviewNoteScope::File;
  if (hunk_index >= 0) {
    hunk_identity =
        compare::ComputeBranchReviewHunkIdentity(compare_tab->model, hunk_index, compare_tab->path);
    scope = compare::BranchReviewNoteScope::Hunk;
  }
  if (note_text.empty()) {
    context_.current_project_state.branch_review.DeleteNote(compare_tab->branch_target, scope,
                                                            compare_tab->path, hunk_identity);
  } else {
    context_.current_project_state.branch_review.SetNote(compare_tab->branch_target, scope,
                                                         compare_tab->path, hunk_identity, note_text);
  }
  ApplyBranchReviewPresentationMarkers(*compare_tab, context_.current_project_state.branch_review);
  PersistBranchReviewState();
  RequestActiveTabRedraw(true);
}

}  // namespace microide::workspace
