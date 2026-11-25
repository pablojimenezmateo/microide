#include <memory>

#include "workspace/shell/WorkspaceShell.h"
#include "workspace/services/RemoteHostService.h"

#include <algorithm>

#include "compare/BranchReviewStateTypes.h"
#include "compare/CompareReviewTypes.h"
#include "editor/RuntimeSyntaxRegistry.h"
#include "workspace/git/BranchReviewStateBridge.h"
#include "workspace/services/CompareMergeService.h"
#include "workspace/git/CompareTabLoad.h"
#include "workspace/git/CompareTabReview.h"
#include "workspace/WorkspaceLayout.h"
#include "workspace/persistence/WorkspacePersistenceCoordinator.h"

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
          .request_frame = [this]() { RequestPreparedFrame(); },
          .build_compare_tab_entry =
              [this](const std::filesystem::path& path,
                     const project::GitCommitEntry& commit,
                     std::size_t selected_row) {
                return BuildCompareTabEntry(path, commit, selected_row);
              },
          .rebuild_compare_tab_entry =
              [this](const std::filesystem::path& path, const CompareTabState& compare_state,
                     const CompareRevisionSides* sides) {
                return BuildCompareTabEntry(path, compare_state, sides);
              },
          .build_compare_tab_from_buffers =
              [this](const std::filesystem::path& path,
                     const std::string& left_content,
                     const std::string& right_content,
                     const std::string& left_label,
                     const std::string& right_label,
                     std::size_t selected_row,
                     bool persistable,
                     bool left_exists,
                     bool right_exists) {
                return BuildCompareTabFromBuffers(path, left_content, right_content, left_label,
                                                  right_label, selected_row, persistable,
                                                  left_exists, right_exists);
              },
          .build_plain_compare_tab =
              [this](CompareInput left, CompareInput right) -> std::optional<TabEntry> {
                const std::filesystem::path left_path = left.path.lexically_normal();
                const std::filesystem::path right_path = right.path.lexically_normal();
                const std::filesystem::path tab_path =
                    !right_path.empty() ? right_path : left_path;

                CompareTabState compare_tab;
                compare_tab.path = tab_path;
                compare_tab.left_path = left_path;
                compare_tab.right_path = right_path;
                compare_tab.title = "compare: " + left.label + "  ↔  " + right.label;
                compare_tab.plain_compare = true;
                compare_tab.review_mode = compare::CompareReviewMode::Plain;
                compare_tab.commit_hash = left.label;
                compare_tab.right_ref = "PLAIN";
                compare_tab.left_label = std::move(left.label);
                compare_tab.right_label = std::move(right.label);
                compare_tab.left_content = compare::MakeCompareText(std::move(left.content));
                compare_tab.persistable = false;
                compare_tab.right_editable = right.editable && !right_path.empty();
                compare_tab.right_viewport.LoadContent(right.content, tab_path);
                ApplyEditorPreferences(compare_tab.right_viewport);
                AdoptLiveBufferForCompareRightSide(context_.current_project_state.editor_groups,
                                                   compare_tab);
                RefreshCompareTabDerivedState(compare_tab);
                compare_tab.selected_row = 0;
                compare_tab.right_view_active = true;
                return TabEntry{
                    .kind = TabEntry::Kind::Compare,
                    .path = tab_path,
                    .title = compare_tab.title,
                    .editor_state = std::nullopt,
                    .deferred_handle = std::nullopt,
                    .compare = std::move(compare_tab),
                    .merge = std::nullopt,
                };
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
          .refresh_compare_tab_derived_state =
              [this](CompareTabState& compare_tab) { RefreshCompareTabDerivedState(compare_tab); },
          .begin_compare_tab_load =
              [this](CompareTabLoadRequest request) -> std::uint64_t {
                request.build_options.syntax = language_contract_.ResolveDiffSyntax(
                    editor::runtime_syntax::DetectFiletype(request.path));
                auto shared_request =
                    std::make_shared<const CompareTabLoadRequest>(std::move(request));
                auto result = std::make_shared<CompareTabLoadResult>();
                return file_read_service_.Begin({
                    .path = shared_request->path,
                    .on_worker =
                        [shared_request, result](std::string& bytes) {
                          RunCompareTabLoad(*shared_request, std::move(bytes), *result);
                        },
                    .on_complete =
                        [this, result](project::FileReadService::Completion completion) {
                          MakeDiffTabCoordinator().ApplyCompareTabLoad(
                              completion.id,
                              completion.status == project::FileReadService::Status::Cancelled,
                              completion.ok(), *result);
                        },
                    .read_path = shared_request->right_is_working_tree(),
                });
              },
          .begin_git_conflict_prefetch =
              [this](const std::filesystem::path& path) {
                const std::filesystem::path root = context_.current_project_state.root;
                const platform::ProcessLauncher* launcher =
                    &context_.current_project_state.launcher();
                auto cache = std::make_shared<project::GitRevisionBlobCache>();
                (void)file_read_service_.Begin({
                    .path = path,
                    .on_worker =
                        [root, launcher, path, cache](std::string&) {
                          cache->Prefetch(root, *launcher, {":1", ":2", ":3"}, {path});
                        },
                    .on_complete =
                        [this, root, path, cache](
                            project::FileReadService::Completion completion) {
                          if (completion.status ==
                                  project::FileReadService::Status::Cancelled ||
                              context_.current_project_state.root != root) {
                            return;
                          }
                          (void)MakeDiffTabCoordinator().OpenGitConflictMerge(path, cache.get());
                        },
                    .read_path = false,
                });
              },
          .apply_editor_preferences =
              [this](editor::TextViewport& view) { ApplyEditorPreferences(view); },
          .report_compare_load_failure =
              [this](std::string message) {
                Notify(NotificationService::Tone::Error, std::move(message));
              },
          .begin_compare_tab_refresh =
              [this](CompareTabRefreshRequest request) -> std::uint64_t {
                auto shared_request = std::make_shared<const CompareTabRefreshRequest>(std::move(request));
                auto sides = std::make_shared<CompareRevisionSides>();
                return file_read_service_.Begin({
                    .path = shared_request->path,
                    .on_worker = [shared_request, sides](std::string&) { *sides = ReadCompareRevisionSides(*shared_request); },
                    .on_complete =
                        [this, sides](project::FileReadService::Completion completion) {
                          if (MakeDiffTabCoordinator().ApplyCompareTabRefresh(
                                  completion.id,
                                  completion.status == project::FileReadService::Status::Cancelled, *sides)) {
                            return;
                          }
                          for (const auto& project : context_.project_catalog.entries) {
                            if (project == nullptr) {
                              continue;
                            }
                            for (EditorGroup& group : project->editor_groups) {
                              for (TabEntry& tab : group.open_tabs) {
                                if (tab.compare.has_value() && tab.compare->pending_refresh.Holds(completion.id)) {
                                  tab.compare->pending_refresh.Disarm();
                                }
                              }
                            }
                          }
                        },
                    .read_path = false,
                });
              },
          .cancel_file_read = [this](std::uint64_t id) { file_read_service_.Cancel(id); },
      });
}

CompareInteractionCoordinator WorkspaceShell::MakeCompareInteractionCoordinator() {
  return CompareInteractionCoordinator(
      context_.current_project_state,
      CompareInteractionCoordinator::Operations{
          .show_compare_picker_overlay = [this]() { ShowOverlay(OverlayMode::CommitPicker); },
          .reset_overlay_scroll = [this]() { ResetOverlayScroll(); },
          .reveal_compare_picker_selection =
              [this]() {
                if (const auto layout = CurrentWorkspaceLayout(); layout.has_value()) {
                  RevealOverlaySelection(ComputeOverlayRect(layout->overlay_anchor));
                }
              },
          .open_comparison =
              [this](const project::GitCommitEntry& commit) { MakeCompareMergeService().diff_tabs().OpenComparison(commit); },
          .set_outgoing_base_ref =
              [this](const std::string& ref, const std::string& ) {
                SetGitOutgoingBaseChoice(OutgoingBaseChoice{
                    .kind = OutgoingBaseChoice::Kind::SpecificRef,
                    .custom_ref = ref,
                });
              },
          .switch_to_branch =
              [this](const std::string& branch) {
                DismissOverlay();
                const std::string rejection =
                    DispatchGitOperationAction(ActionId::GitSwitchBranch, {branch});
                if (!rejection.empty()) {
                  Notify(NotificationService::Tone::Warning, rejection);
                }
              },
          .active_compare_tab = [this]() { return ActiveCompareTab(); },
          .active_merge_tab = [this]() { return ActiveMergeTab(); },
          .open_file = [this](const std::filesystem::path& path) { OpenFile(path); },
          .active_editor_viewport = [this]() { return ActiveEditorViewport(); },
          .reveal_active_compare_selection = [this]() { RevealActiveCompareSelection(); },
          .request_frame = [this]() { RequestPreparedFrame(); },
          .sync_compare_viewport_scroll =
              [this](CompareTabState& compare_tab) { SyncCompareViewportScroll(compare_tab); },
          .scroll_compare_rows =
              [this](CompareTabState& compare_tab, int delta) {
                if (const auto layout_state = CurrentWorkspaceLayout(); layout_state.has_value()) {
                  const SDL_FRect surface = ActiveTabSurface(*layout_state, &compare_tab);
                  const auto surface_layout =
                      ComputeCompareSurfaceLayout(surface, compare_tab);
                  const auto scroll_layout =
                      ComputeCompareScrollLayout(surface, surface_layout, compare_tab);
                  compare_tab.scroll_row = std::clamp(scroll_layout.vertical_scroll + delta, 0,
                                                      scroll_layout.max_vertical_scroll);
                  SyncCompareViewportScroll(compare_tab);
                }
              },
          .scroll_compare_columns =
              [this](CompareTabState& compare_tab, int delta) {
                if (const auto layout_state = CurrentWorkspaceLayout(); layout_state.has_value()) {
                  const SDL_FRect surface = ActiveTabSurface(*layout_state, &compare_tab);
                  const auto surface_layout =
                      ComputeCompareSurfaceLayout(surface, compare_tab);
                  const auto scroll_layout =
                      ComputeCompareScrollLayout(surface, surface_layout, compare_tab);
                  const long long target_scroll =
                      static_cast<long long>(scroll_layout.horizontal_scroll) +
                      static_cast<long long>(delta);
                  compare_tab.horizontal_scroll = static_cast<std::size_t>(std::clamp(
                      target_scroll, 0LL,
                      static_cast<long long>(scroll_layout.max_horizontal_scroll)));
                  SyncCompareViewportScroll(compare_tab);
                }
              },
          .reveal_active_merge_selection = [this]() { RevealActiveMergeSelection(); },
          .update_merge_max_visual_columns =
              [this](MergeTabState& merge_tab, const std::vector<std::string>& replacement_lines) {
                UpdateMergeMaxVisualColumns(merge_tab, replacement_lines);
              },
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
                     const std::string& right_label,
                     const std::vector<std::filesystem::path>* review_files) {
                return OpenBranchHeadComparison(path, left_ref, left_label, right_ref,
                                                right_label, nullptr, review_files);
              },
          .refresh_compare_tab_derived_state =
              [this](CompareTabState& compare_tab) { RefreshCompareTabDerivedState(compare_tab); },
          .save_active_merge_tab =
              [this]() {
                MergeTabState* merge_tab = ActiveMergeTab();
                if (merge_tab == nullptr) {
                  return false;
                }
                if (!PrepareEditorViewportForSave(merge_tab->result_viewport.path(),
                                                  merge_tab->result_viewport, nullptr,
                                                  SaveMode::Blocking)
                         .ok()) {
                  return false;
                }
                return merge_tab->result_viewport.Save(context_.current_project_state.write_gate());
              },
          .stage_merge_result_path =
              [this](const std::filesystem::path& path) {
                if (glue_->remote_host_service != nullptr &&
                    glue_->remote_host_service->ResolveMerged(path)) {
                  return true;
                }
                return project::GitStagePath(context_.current_project_state.root,
                                             context_.current_project_state.launcher(), path);
              },
          .refresh_git_sidebar = [this]() { RefreshGitSidebar(); },
          .request_compare_file_history =
              [this](const std::filesystem::path& path) {
                RequestComparePickerFileHistory(path);
              },
          .request_ref_list =
              [this](bool include_commits) { RequestComparePickerRefs(include_commits); },
      });
}

CompareMergeService& WorkspaceShell::MakeCompareMergeService() {
  if (glue_->compare_merge_service != nullptr) {
    return *glue_->compare_merge_service;
  }
  glue_->compare_merge_service = std::make_unique<CompareMergeService>(MakeDiffTabCoordinator(), MakeCompareInteractionCoordinator());
  return *glue_->compare_merge_service;
}

std::optional<std::size_t> WorkspaceShell::FindOpenCompareTabIndex(
    const std::filesystem::path& path,
    std::string_view left_ref,
    std::string_view right_ref) const {
  return const_cast<WorkspaceShell*>(this)->MakeCompareMergeService().diff_tabs().FindOpenCompareTabIndex(
      path, left_ref, right_ref);
}

std::optional<std::size_t> WorkspaceShell::FindOpenMergeTabIndex(
    const std::filesystem::path& path) const {
  return const_cast<WorkspaceShell*>(this)->MakeCompareMergeService().diff_tabs().FindOpenMergeTabIndex(path);
}

bool WorkspaceShell::OpenMergeEditor(const std::filesystem::path& base_path,
                                     const std::filesystem::path& incoming_path,
                                     const std::filesystem::path& current_path,
                                     const std::filesystem::path& output_path) {
  return MakeCompareMergeService().diff_tabs().OpenMergeEditor(base_path, incoming_path, current_path,
                                                   output_path);
}

bool WorkspaceShell::OpenWorkingTreeComparison(const std::filesystem::path& path,
                                               const std::string& left_ref,
                                               const std::string& left_label) {
  return MakeCompareMergeService().diff_tabs().OpenWorkingTreeComparison(path, left_ref, left_label);
}

bool WorkspaceShell::OpenBranchHeadComparison(
    const std::filesystem::path& path,
    const std::string& left_ref,
    const std::string& left_label,
    const std::string& right_ref,
    const std::string& right_label,
    const project::GitRevisionBlobCache* prefetched,
    const std::vector<std::filesystem::path>* review_files) {
  return MakeCompareMergeService().diff_tabs().OpenBranchHeadComparison(path, left_ref, left_label, right_ref,
                                                            right_label, prefetched, review_files);
}

bool WorkspaceShell::OpenGitConflictMerge(const std::filesystem::path& path) {
  return MakeCompareMergeService().diff_tabs().OpenGitConflictMerge(path);
}

bool WorkspaceShell::OpenComparePickerForPath(const std::filesystem::path& path,
                                              std::string_view commit_spec) {
  return MakeCompareMergeService().interactions().OpenPickerForPath(path, commit_spec);
}

void WorkspaceShell::RequestComparePickerFileHistory(const std::filesystem::path& path) {
  auto& picker = context_.current_project_state.overlay.workflow.compare_picker;
  const std::uint64_t generation = ++compare_picker_generation_;
  picker.active_request_generation = generation;

  const std::filesystem::path root = context_.current_project_state.root;
  const platform::ProcessLauncher* launcher = &context_.current_project_state.launcher();
  auto provider = compare_picker_file_history_provider_
                      ? compare_picker_file_history_provider_
                      : std::function<project::GitFileHistoryResult(
                            const std::filesystem::path&, const std::filesystem::path&)>(
                            [launcher](const std::filesystem::path& repo_root,
                                       const std::filesystem::path& file_path) {
                              return project::CollectGitFileHistory(repo_root, *launcher,
                                                                    file_path);
                            });
  interactive_background_executor_.PostLatest(
      "compare-picker",
      [this, root, path, generation, provider = std::move(provider)]() {
        project::GitFileHistoryResult history = provider(root, path);
        compare_picker_mailbox_.Post(
            [this, generation, history = std::move(history)]() mutable {
              ApplyComparePickerFileHistory(generation, history);
            });
      });
}

void WorkspaceShell::RequestComparePickerRefs(const bool include_commits) {
  auto& picker = context_.current_project_state.overlay.workflow.compare_picker;
  const std::uint64_t generation = ++compare_picker_generation_;
  picker.active_request_generation = generation;

  const std::filesystem::path root = context_.current_project_state.root;
  const platform::ProcessLauncher* launcher = &context_.current_project_state.launcher();
  auto branches_provider =
      compare_picker_branches_provider_
          ? compare_picker_branches_provider_
          : std::function<std::vector<project::GitBranchReference>(const std::filesystem::path&)>(
                [launcher](const std::filesystem::path& repo_root) {
                  return project::CollectGitBranches(repo_root, *launcher);
                });
  auto commits_provider =
      compare_picker_recent_commits_provider_
          ? compare_picker_recent_commits_provider_
          : std::function<std::vector<project::GitCommitEntry>(const std::filesystem::path&,
                                                               std::size_t)>(
                [launcher](const std::filesystem::path& repo_root, std::size_t limit) {
                  return project::CollectGitRecentCommits(repo_root, *launcher, limit);
                });
  interactive_background_executor_.PostLatest(
      "compare-picker",
      [this, root, generation, include_commits, branches_provider = std::move(branches_provider),
       commits_provider = std::move(commits_provider)]() {
        std::vector<project::GitBranchReference> branches = branches_provider(root);
        std::vector<project::GitCommitEntry> commits;
        if (include_commits) {
          commits = commits_provider(root, 50);
        }
        compare_picker_mailbox_.Post([this, generation, branches = std::move(branches),
                                      commits = std::move(commits)]() mutable {
          ApplyComparePickerRefs(generation, branches, commits);
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
    return;
  }
  MakeCompareMergeService().interactions().ApplyFileHistoryResult(history);
}

void WorkspaceShell::ApplyComparePickerRefs(
    std::uint64_t generation, const std::vector<project::GitBranchReference>& branches,
    const std::vector<project::GitCommitEntry>& commits) {
  if (!ComparePickerRequestCurrent(generation)) {
    return;
  }
  MakeCompareMergeService().interactions().ApplyRefsResult(branches, commits);
}

void WorkspaceShell::JumpCompareHunk(int delta) {
  MakeCompareMergeService().interactions().JumpCompareHunk(delta);
}

void WorkspaceShell::RefreshOpenCompareTabsForPath(const std::filesystem::path& path) {
  MakeDiffTabCoordinator().RefreshCompareTabsForPath(path.lexically_normal());
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

void WorkspaceShell::MoveMergeSelection(int delta) {
  MakeCompareMergeService().interactions().MoveMergeSelection(delta);
}

void WorkspaceShell::ApplyMergeChoice(compare::MergeChoice choice) {
  MakeCompareMergeService().interactions().ApplyMergeChoice(choice);
}

void WorkspaceShell::ResetMergeHunk() {
  MakeCompareMergeService().interactions().ResetMergeHunk();
}

void WorkspaceShell::ToggleMergeBasePane() {
  MakeCompareMergeService().interactions().ToggleMergeBasePane();
}

void WorkspaceShell::MarkMergeResolved() {
  MakeCompareMergeService().interactions().MarkMergeResolved();
}

}
