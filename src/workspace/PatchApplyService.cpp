#include "workspace/PatchApplyService.h"

#include <algorithm>
#include <utility>

#include "compare/CompareReviewTypes.h"
#include "compare/CompareSemanticMetadata.h"
#include "project/GitPatchApply.h"
#include "project/GitRepository.h"
#include "project/PatchGenerator.h"
#include "workspace/CompareTabReview.h"
#include "workspace/GitRepositoryService.h"

namespace microide::workspace {

namespace {

using compare::CompareSemanticFileKind;
using project::PatchApplyResult;
using project::PatchApplyResultCategory;
using project::PatchOperationKind;

bool IsWorkingTreePatchTarget(const CompareTabState& compare_tab) {
  return compare_tab.review_mode == compare::CompareReviewMode::WorkingTree &&
         compare_tab.right_ref == "WORKTREE";
}

std::size_t ModelRowFromSelectionLine(const CompareTabState& compare_tab, std::size_t line) {
  return CompareTabModelRowForRightLine(compare_tab, line);
}

}  // namespace

PatchApplyService::PatchApplyService(project::ProjectBackgroundExecutor& background_executor,
                                     GitRepositoryService& git_repository_service)
    : background_executor_(background_executor),
      git_repository_service_(git_repository_service) {}

void PatchApplyService::SetCallbacks(Callbacks callbacks) {
  std::lock_guard lock(mutex_);
  callbacks_ = std::move(callbacks);
}

bool PatchApplyService::CanApplyPatchToCompareTab(const CompareTabState& compare_tab,
                                                  const PatchOperationKind operation) const {
  if (!IsWorkingTreePatchTarget(compare_tab)) {
    return false;
  }
  if (compare_tab.semantic_file.file_kind != CompareSemanticFileKind::Text) {
    return false;
  }
  if (compare_tab.semantic_file.submodule_pointer_changed) {
    return false;
  }
  switch (operation) {
    case PatchOperationKind::StageHunk:
    case PatchOperationKind::StageSelectedLines:
      return compare_tab.staging_view != compare::WorkingTreeStagingView::Staged;
    case PatchOperationKind::UnstageHunk:
    case PatchOperationKind::UnstageSelectedLines:
      return compare_tab.staging_view != compare::WorkingTreeStagingView::Unstaged;
    case PatchOperationKind::DiscardHunk:
    case PatchOperationKind::DiscardSelectedLines:
      return true;
    default:
      return false;
  }
}

std::optional<project::PatchApplyRequest> PatchApplyService::BuildRequest(
    CompareTabState& compare_tab,
    const PatchOperationKind operation,
    const bool line_scope) const {
  if (!CanApplyPatchToCompareTab(compare_tab, operation)) {
    return std::nullopt;
  }

  const project::GitRepositoryState repository_state = callbacks_.current_repository_state();
  if (repository_state.repository_root.empty() || !repository_state.repo_available) {
    return std::nullopt;
  }

  project::GitRepository repo(repository_state.repository_root);
  const std::optional<std::filesystem::path> relative_path = repo.ToRelative(compare_tab.path);
  if (!relative_path.has_value()) {
    return std::nullopt;
  }

  project::PatchApplyTarget target{
      .repository_root = repository_state.repository_root,
      .relative_path = *relative_path,
      .review_mode = compare_tab.review_mode,
      .staging_view = compare_tab.staging_view,
      .semantic_file = compare_tab.semantic_file,
      .hunk = std::nullopt,
      .line_selection = std::nullopt,
  };

  if (line_scope) {
    if (!compare_tab.right_view_active) {
      return std::nullopt;
    }
    const std::optional<editor::SelectionRange> selection =
        compare_tab.right_viewport.selection_range();
    if (!selection.has_value()) {
      return std::nullopt;
    }
    const std::size_t first_row =
        ModelRowFromSelectionLine(compare_tab, selection->start.line);
    // selection_range() reports an EXCLUSIVE end. A whole-line selection made with
    // the standard gesture (anchor at line N's start, cursor dragged to line N+1's
    // start) yields end.column == 0 and end.line == N+1, but line N+1 is not part
    // of the selection. Without this correction the patch spans one extra line —
    // and for Discard Selected Lines that silently destroys an unselected
    // working-tree change (irreversible). Mirror the end.column==0 adjustment used
    // by every other line-oriented consumer (WorkspaceShellInteraction line-copy,
    // ShapingActions, DiagnosticsRender, redraw).
    std::size_t end_line = selection->end.line;
    if (selection->end.column == 0 && selection->end.line > selection->start.line) {
      --end_line;
    }
    const std::size_t last_row = ModelRowFromSelectionLine(compare_tab, end_line);
    const auto line_selection =
        project::PatchLineSelectionFromModelRows(compare_tab.model, first_row, last_row);
    if (!line_selection.has_value() ||
        !project::PatchLineSelectionHasChanges(compare_tab.model, *line_selection)) {
      return std::nullopt;
    }
    target.line_selection = *line_selection;
  } else {
    const int hunk_index = CompareTabSelectedHunkIndex(compare_tab);
    if (hunk_index < 0) {
      return std::nullopt;
    }
    target.hunk_scope = true;
    target.hunk = project::PatchHunkTarget{.hunk_index = hunk_index};
  }

  // Deliberately do NOT copy compare_tab.model into the request. The patch text
  // is generated synchronously (BuildPatchForRequest) from the live model before
  // dispatch, and the background apply path (project::ApplyPatchRequest) never
  // reads request.model. Copying a whole CompareModel here — every row and hunk
  // of a potentially huge diff — just to stage one small hunk was pure UI-thread
  // overhead. The .model field is left value-initialized (empty).
  return project::PatchApplyRequest{
      .operation = operation,
      .target = std::move(target),
      .repository_snapshot_generation = repository_state.generation,
      .diff_model_generation = compare_tab.model_revision,
  };
}

std::optional<std::string> PatchApplyService::BuildPatchForRequest(
    const project::PatchApplyRequest& request,
    const compare::CompareModel& model) const {
  if (request.target.line_selection.has_value()) {
    const auto& selection = *request.target.line_selection;
    return project::GenerateComparePatchForRows(
        model, request.target.relative_path, selection.first_model_row,
        selection.last_model_row);
  }
  if (request.target.hunk.has_value()) {
    return project::GenerateComparePatch(model, request.target.relative_path,
                                         request.target.hunk->hunk_index);
  }
  return std::nullopt;
}

void PatchApplyService::DispatchApply(project::PatchApplyRequest request, std::string patch_text) {
  const std::uint64_t diff_generation = request.diff_model_generation;
  background_executor_.Post([this, request = std::move(request), patch_text = std::move(patch_text),
                             diff_generation]() mutable {
    const project::GitRepositoryState current_state = callbacks_.current_repository_state();
    if (request.repository_snapshot_generation != current_state.generation) {
      PublishResult(
          PatchApplyResult{
              .category = PatchApplyResultCategory::StaleGeneration,
              .detail = "repository state changed before the patch could be applied",
          },
          request);
      return;
    }

    PatchApplyResult result = project::ApplyPatchRequest(request, patch_text);

    const project::GitRepositoryState completion_state = callbacks_.current_repository_state();
    if (result.category == PatchApplyResultCategory::Success &&
        request.repository_snapshot_generation != completion_state.generation) {
      result = PatchApplyResult{
          .category = PatchApplyResultCategory::StaleGeneration,
          .detail = "repository state changed while the patch was applying",
      };
    }

    if (result.category == PatchApplyResultCategory::Success) {
      result.completed_repository_generation = completion_state.generation;
      git_repository_service_.MarkStale();
      if (callbacks_.request_git_refresh != nullptr) {
        callbacks_.request_git_refresh();
      }
      const std::filesystem::path absolute_path =
          request.target.repository_root / request.target.relative_path;
      if (callbacks_.invalidate_editor_blame_path != nullptr) {
        callbacks_.invalidate_editor_blame_path(absolute_path);
      }
      if (callbacks_.reload_clean_editor_tabs_for_path != nullptr) {
        callbacks_.reload_clean_editor_tabs_for_path(absolute_path);
      }
      if (callbacks_.refresh_compare_tab_for_path != nullptr) {
        callbacks_.refresh_compare_tab_for_path(absolute_path);
      }
      (void)diff_generation;
    }

    PublishResult(std::move(result), request);
  });
}

void PatchApplyService::PublishResult(project::PatchApplyResult result,
                                      const project::PatchApplyRequest& request) {
  (void)request;
  ReportResult(result);
}

void PatchApplyService::ReportResult(const project::PatchApplyResult& result) {
  if (callbacks_.set_command_feedback == nullptr) {
    return;
  }
  switch (result.category) {
    case PatchApplyResultCategory::Success:
      callbacks_.set_command_feedback("Patch applied");
      break;
    case PatchApplyResultCategory::StaleDiff:
    case PatchApplyResultCategory::StaleGeneration:
      callbacks_.set_command_feedback(
          "Diff is stale; refresh the compare tab and try again. " + result.detail);
      break;
    case PatchApplyResultCategory::PatchDidNotApply:
      callbacks_.set_command_feedback("Patch did not apply: " + result.detail);
      break;
    case PatchApplyResultCategory::UnsupportedTarget:
      callbacks_.set_command_feedback(result.detail);
      break;
    case PatchApplyResultCategory::Cancelled:
      callbacks_.set_command_feedback("Patch operation cancelled");
      break;
    case PatchApplyResultCategory::UnknownError:
      callbacks_.set_command_feedback(result.detail.empty() ? "Patch operation failed"
                                                            : result.detail);
      break;
  }
}

bool PatchApplyService::RequestStageHunk(CompareTabState& compare_tab) {
  const auto request = BuildRequest(compare_tab, PatchOperationKind::StageHunk, false);
  if (!request.has_value()) {
    ReportResult({.category = PatchApplyResultCategory::UnsupportedTarget,
                  .detail = "Stage hunk is not available for this compare target"});
    return false;
  }
  auto patch = BuildPatchForRequest(*request, compare_tab.model);
  if (!patch.has_value()) {
    ReportResult({.category = PatchApplyResultCategory::UnsupportedTarget,
                  .detail = "Selected hunk has no stageable changes"});
    return false;
  }
  DispatchApply(*request, std::move(*patch));
  return true;
}

bool PatchApplyService::RequestStageSelectedLines(CompareTabState& compare_tab) {
  const auto request = BuildRequest(compare_tab, PatchOperationKind::StageSelectedLines, true);
  if (!request.has_value()) {
    ReportResult({.category = PatchApplyResultCategory::UnsupportedTarget,
                  .detail = "Stage selected lines is not available for this selection"});
    return false;
  }
  auto patch = BuildPatchForRequest(*request, compare_tab.model);
  if (!patch.has_value()) {
    ReportResult({.category = PatchApplyResultCategory::UnsupportedTarget,
                  .detail = "Selected lines have no stageable changes"});
    return false;
  }
  DispatchApply(*request, std::move(*patch));
  return true;
}

bool PatchApplyService::RequestUnstageHunk(CompareTabState& compare_tab) {
  const auto request = BuildRequest(compare_tab, PatchOperationKind::UnstageHunk, false);
  if (!request.has_value()) {
    ReportResult({.category = PatchApplyResultCategory::UnsupportedTarget,
                  .detail = "Unstage hunk is not available for this compare target"});
    return false;
  }
  auto patch = BuildPatchForRequest(*request, compare_tab.model);
  if (!patch.has_value()) {
    ReportResult({.category = PatchApplyResultCategory::UnsupportedTarget,
                  .detail = "Selected hunk has no unstaged changes"});
    return false;
  }
  DispatchApply(*request, std::move(*patch));
  return true;
}

bool PatchApplyService::RequestUnstageSelectedLines(CompareTabState& compare_tab) {
  const auto request = BuildRequest(compare_tab, PatchOperationKind::UnstageSelectedLines, true);
  if (!request.has_value()) {
    ReportResult({.category = PatchApplyResultCategory::UnsupportedTarget,
                  .detail = "Unstage selected lines is not available for this selection"});
    return false;
  }
  auto patch = BuildPatchForRequest(*request, compare_tab.model);
  if (!patch.has_value()) {
    ReportResult({.category = PatchApplyResultCategory::UnsupportedTarget,
                  .detail = "Selected lines have no unstaged changes"});
    return false;
  }
  DispatchApply(*request, std::move(*patch));
  return true;
}

bool PatchApplyService::RequestDiscardHunkPreview(CompareTabState& compare_tab) {
  const auto request = BuildRequest(compare_tab, PatchOperationKind::DiscardHunk, false);
  if (!request.has_value()) {
    ReportResult({.category = PatchApplyResultCategory::UnsupportedTarget,
                  .detail = "Discard hunk is not available for this compare target"});
    return false;
  }
  auto patch = BuildPatchForRequest(*request, compare_tab.model);
  if (!patch.has_value()) {
    ReportResult({.category = PatchApplyResultCategory::UnsupportedTarget,
                  .detail = "Selected hunk has no discardable changes"});
    return false;
  }
  std::lock_guard lock(mutex_);
  pending_discard_ = PendingDiscard{
      .request = *request,
      .patch_text = *patch,
  };
  if (callbacks_.open_discard_preview_prompt != nullptr) {
    callbacks_.open_discard_preview_prompt(project::PatchApplyPreview{
        .patch_text = pending_discard_->patch_text,
        .summary = "Discard the selected hunk from the working tree?",
    });
  }
  return true;
}

bool PatchApplyService::RequestDiscardSelectedLinesPreview(CompareTabState& compare_tab) {
  const auto request = BuildRequest(compare_tab, PatchOperationKind::DiscardSelectedLines, true);
  if (!request.has_value()) {
    ReportResult({.category = PatchApplyResultCategory::UnsupportedTarget,
                  .detail = "Discard selected lines is not available for this selection"});
    return false;
  }
  auto patch = BuildPatchForRequest(*request, compare_tab.model);
  if (!patch.has_value()) {
    ReportResult({.category = PatchApplyResultCategory::UnsupportedTarget,
                  .detail = "Selected lines have no discardable changes"});
    return false;
  }
  std::lock_guard lock(mutex_);
  pending_discard_ = PendingDiscard{
      .request = *request,
      .patch_text = *patch,
  };
  if (callbacks_.open_discard_preview_prompt != nullptr) {
    callbacks_.open_discard_preview_prompt(project::PatchApplyPreview{
        .patch_text = pending_discard_->patch_text,
        .summary = "Discard the selected lines from the working tree?",
    });
  }
  return true;
}

bool PatchApplyService::ConfirmPendingDiscard() {
  std::optional<PendingDiscard> pending;
  {
    std::lock_guard lock(mutex_);
    pending = std::move(pending_discard_);
  }
  if (!pending.has_value()) {
    return false;
  }
  DispatchApply(pending->request, std::move(pending->patch_text));
  return true;
}

void PatchApplyService::CancelPendingDiscard() {
  std::lock_guard lock(mutex_);
  pending_discard_.reset();
}

project::PatchApplyResult PatchApplyService::ApplyPatchSynchronouslyForTesting(
    project::PatchApplyRequest request,
    std::string patch_text) {
  return project::ApplyPatchRequest(request, patch_text);
}

}  // namespace microide::workspace
