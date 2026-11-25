#include "workspace/git/PatchApplyService.h"

#include <algorithm>
#include <utility>

#include "compare/CompareReviewTypes.h"
#include "compare/CompareSemanticMetadata.h"
#include "project/GitPatchApply.h"
#include "project/GitCommandUtil.h"
#include "project/PatchGenerator.h"
#include "workspace/git/CompareTabReview.h"
#include "workspace/git/GitRepositoryService.h"
#include "util/InvokeIfSet.h"

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

}

PatchApplyService::PatchApplyService(project::ProjectBackgroundExecutor& background_executor,
                                     GitRepositoryService& git_repository_service)
    : background_executor_(background_executor),
      git_repository_service_(git_repository_service) {}

void PatchApplyService::SetCallbacks(Callbacks callbacks) {
  std::lock_guard lock(mutex_);
  callbacks_ = std::move(callbacks);
}

bool PatchApplyService::CanApplyPatchToCompareTab(const CompareTabState& compare_tab,
                                                  const PatchOperationKind operation) {
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

  if (!callbacks_.current_repository_state) {
    return std::nullopt;
  }
  const project::GitRepositoryState repository_state = callbacks_.current_repository_state();
  if (repository_state.repository_root.empty() || !repository_state.repo_available) {
    return std::nullopt;
  }

  const std::optional<std::filesystem::path> relative_path =
      project::internal::AbsoluteToRelativePath(repository_state.repository_root,
                                                compare_tab.path);
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

  const bool stage = operation == PatchOperationKind::StageHunk ||
                     operation == PatchOperationKind::StageSelectedLines;
  const bool unstage = operation == PatchOperationKind::UnstageHunk ||
                       operation == PatchOperationKind::UnstageSelectedLines;
  if ((stage || unstage) &&
      compare_tab.staging_view == compare::WorkingTreeStagingView::Combined) {
    std::size_t first_row = 0;
    std::size_t last_row = 0;
    if (target.line_selection.has_value()) {
      first_row = target.line_selection->first_model_row;
      last_row = target.line_selection->last_model_row;
    } else if (target.hunk.has_value() && target.hunk->hunk_index >= 0 &&
               static_cast<std::size_t>(target.hunk->hunk_index) < compare_tab.model.hunks.size()) {
      const compare::CompareHunk& hunk =
          compare_tab.model.hunks[static_cast<std::size_t>(target.hunk->hunk_index)];
      first_row = static_cast<std::size_t>(std::max(0, hunk.start_row));
      last_row = static_cast<std::size_t>(std::max(0, hunk.end_row));
    }
    target.change_span = project::ChangeSpanForRows(compare_tab.model, first_row, last_row);
    target.working_tree_source = compare_tab.model.right_source;
    target.working_tree_exists = compare_tab.build_options.right_exists.value_or(true);
  }

  return project::PatchApplyRequest{
      .operation = operation,
      .target = std::move(target),
      .launcher = &platform::LauncherOrLocal(
          callbacks_.project_launcher ? callbacks_.project_launcher() : nullptr),
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
  std::filesystem::path project_root =
      callbacks_.front_project_root ? callbacks_.front_project_root() : std::filesystem::path{};
  background_executor_.PostDurable([this, request = std::move(request), patch_text = std::move(patch_text),
                                    project_root = std::move(project_root)]() mutable {
    PatchApplyResult result = project::ApplyPatchRequest(*request.launcher, request, patch_text);
    const bool patch_applied = result.category == PatchApplyResultCategory::Success;
    if (patch_applied) {
      git_repository_service_.MarkStale();
    }

    completion_mailbox_.Post([this, request = std::move(request), result = std::move(result), patch_applied,
                              project_root = std::move(project_root)]() mutable {
      const bool in_front =
          project_root.empty() || !callbacks_.is_in_front || callbacks_.is_in_front(project_root);
      if (patch_applied) {
        const std::filesystem::path absolute_path =
            request.target.repository_root / request.target.relative_path;
        if (callbacks_.invalidate_editor_blame_path != nullptr) {
          callbacks_.invalidate_editor_blame_path(absolute_path);
        }
        if (!in_front) {
          util::InvokeIfSet(callbacks_.defer_to_project, project_root);
        } else {
          if (callbacks_.request_git_refresh != nullptr) {
            callbacks_.request_git_refresh();
          }
          if (callbacks_.reload_clean_editor_tabs_for_path != nullptr) {
            callbacks_.reload_clean_editor_tabs_for_path(absolute_path);
          }
          if (callbacks_.refresh_compare_tab_for_path != nullptr) {
            callbacks_.refresh_compare_tab_for_path(absolute_path);
          }
        }
      }
      ReportResult(result, in_front || !callbacks_.project_label ? std::string()
                                                                  : " in " + callbacks_.project_label(project_root));
    });
  });
}

void PatchApplyService::SetCompletionWakeEvent(std::uint32_t event_type) {
  completion_mailbox_.SetWakeChannel(event_type);
}

void PatchApplyService::DrainCompletions() { completion_mailbox_.Drain(); }

void PatchApplyService::ReportResult(const project::PatchApplyResult& result, std::string_view project_suffix) {
  if (callbacks_.set_command_feedback == nullptr) {
    return;
  }
  switch (result.category) {
    case PatchApplyResultCategory::Success:
      callbacks_.set_command_feedback("Patch applied" + std::string(project_suffix));
      break;
    case PatchApplyResultCategory::StaleDiff:
      callbacks_.set_command_feedback(
          "Diff is stale; refresh the compare tab and try again. " + result.detail);
      break;
    case PatchApplyResultCategory::PatchDidNotApply:
      callbacks_.set_command_feedback("Patch did not apply" + std::string(project_suffix) + ": " + result.detail);
      break;
    case PatchApplyResultCategory::UnsupportedTarget:
      callbacks_.set_command_feedback(result.detail);
      break;
    case PatchApplyResultCategory::UnknownError:
      callbacks_.set_command_feedback(result.detail.empty() ? "Patch operation failed"
                                                            : result.detail);
      break;
  }
}

bool PatchApplyService::RejectIgnoreWhitespaceApply(const CompareTabState& compare_tab) {
  if (!compare_tab.build_options.ignore_whitespace) {
    return false;
  }
  ReportResult({.category = PatchApplyResultCategory::UnsupportedTarget,
                .detail = "Turn off Ignore Whitespace to stage, unstage, or discard changes — "
                          "whitespace-only differences are hidden, so the patch would not match "
                          "the working tree."});
  return true;
}

std::optional<PatchApplyService::ResolvedPatch> PatchApplyService::ResolvePatch(
    CompareTabState& compare_tab,
    PatchOperationKind operation,
    bool line_scope,
    std::string_view unavailable_detail,
    std::string_view empty_patch_detail) {
  if (RejectIgnoreWhitespaceApply(compare_tab)) {
    return std::nullopt;
  }
  auto request = BuildRequest(compare_tab, operation, line_scope);
  if (!request.has_value()) {
    ReportResult({.category = PatchApplyResultCategory::UnsupportedTarget,
                  .detail = std::string(unavailable_detail)});
    return std::nullopt;
  }
  auto patch = BuildPatchForRequest(*request, compare_tab.model);
  if (!patch.has_value()) {
    ReportResult({.category = PatchApplyResultCategory::UnsupportedTarget,
                  .detail = std::string(empty_patch_detail)});
    return std::nullopt;
  }
  return ResolvedPatch{.request = std::move(*request), .patch_text = std::move(*patch)};
}

bool PatchApplyService::ResolveAndDispatch(CompareTabState& compare_tab, PatchOperationKind kind,
                                           bool selected_lines,
                                           std::string_view unavailable_detail,
                                           std::string_view empty_patch_detail) {
  auto resolved =
      ResolvePatch(compare_tab, kind, selected_lines, unavailable_detail, empty_patch_detail);
  if (!resolved.has_value()) {
    return false;
  }
  DispatchApply(std::move(resolved->request), std::move(resolved->patch_text));
  return true;
}

bool PatchApplyService::RequestStageHunk(CompareTabState& compare_tab) {
  return ResolveAndDispatch(compare_tab, PatchOperationKind::StageHunk, false,
                            "Stage hunk is not available for this compare target",
                            "Selected hunk has no stageable changes");
}

bool PatchApplyService::RequestStageSelectedLines(CompareTabState& compare_tab) {
  return ResolveAndDispatch(compare_tab, PatchOperationKind::StageSelectedLines, true,
                            "Stage selected lines is not available for this selection",
                            "Selected lines have no stageable changes");
}

bool PatchApplyService::RequestUnstageHunk(CompareTabState& compare_tab) {
  return ResolveAndDispatch(compare_tab, PatchOperationKind::UnstageHunk, false,
                            "Unstage hunk is not available for this compare target",
                            "Selected hunk has no unstaged changes");
}

bool PatchApplyService::RequestUnstageSelectedLines(CompareTabState& compare_tab) {
  return ResolveAndDispatch(compare_tab, PatchOperationKind::UnstageSelectedLines, true,
                            "Unstage selected lines is not available for this selection",
                            "Selected lines have no unstaged changes");
}

bool PatchApplyService::RequestDiscardHunkPreview(CompareTabState& compare_tab) {
  auto resolved = ResolvePatch(compare_tab, PatchOperationKind::DiscardHunk, false,
                               "Discard hunk is not available for this compare target",
                               "Selected hunk has no discardable changes");
  if (!resolved.has_value()) {
    return false;
  }
  std::lock_guard lock(mutex_);
  pending_discard_ = PendingDiscard{
      .request = std::move(resolved->request),
      .patch_text = std::move(resolved->patch_text),
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
  auto resolved = ResolvePatch(compare_tab, PatchOperationKind::DiscardSelectedLines, true,
                               "Discard selected lines is not available for this selection",
                               "Selected lines have no discardable changes");
  if (!resolved.has_value()) {
    return false;
  }
  std::lock_guard lock(mutex_);
  pending_discard_ = PendingDiscard{
      .request = std::move(resolved->request),
      .patch_text = std::move(resolved->patch_text),
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


}
