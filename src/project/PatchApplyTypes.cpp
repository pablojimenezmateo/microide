#include "project/PatchApplyTypes.h"

namespace microide::project {

bool PatchOperationAppliesToIndex(const PatchOperationKind operation) {
  switch (operation) {
    case PatchOperationKind::StageFile:
    case PatchOperationKind::StageHunk:
    case PatchOperationKind::StageSelectedLines:
    case PatchOperationKind::UnstageFile:
    case PatchOperationKind::UnstageHunk:
    case PatchOperationKind::UnstageSelectedLines:
      return true;
    case PatchOperationKind::DiscardHunk:
    case PatchOperationKind::DiscardSelectedLines:
      return false;
  }
  return false;
}

bool PatchOperationAppliesToWorktree(const PatchOperationKind operation) {
  switch (operation) {
    case PatchOperationKind::DiscardHunk:
    case PatchOperationKind::DiscardSelectedLines:
      return true;
    default:
      return false;
  }
}

bool PatchOperationReversesPatch(const PatchOperationKind operation) {
  switch (operation) {
    case PatchOperationKind::UnstageFile:
    case PatchOperationKind::UnstageHunk:
    case PatchOperationKind::UnstageSelectedLines:
    case PatchOperationKind::DiscardHunk:
    case PatchOperationKind::DiscardSelectedLines:
      return true;
    default:
      return false;
  }
}

PatchApplySurface PatchOperationApplySurface(const PatchOperationKind operation) {
  return PatchOperationAppliesToWorktree(operation) ? PatchApplySurface::Worktree
                                                    : PatchApplySurface::Index;
}

GitOperationResultCategory MapPatchResultToGitCategory(const PatchApplyResultCategory category) {
  switch (category) {
    case PatchApplyResultCategory::Success:
      return GitOperationResultCategory::Success;
    case PatchApplyResultCategory::Cancelled:
      return GitOperationResultCategory::Cancelled;
    case PatchApplyResultCategory::StaleDiff:
    case PatchApplyResultCategory::StaleGeneration:
      return GitOperationResultCategory::StaleGeneration;
    case PatchApplyResultCategory::PatchDidNotApply:
    case PatchApplyResultCategory::UnsupportedTarget:
    case PatchApplyResultCategory::UnknownError:
      return GitOperationResultCategory::UnknownError;
  }
  return GitOperationResultCategory::UnknownError;
}

const char* PatchApplyResultCategoryLabel(const PatchApplyResultCategory category) {
  switch (category) {
    case PatchApplyResultCategory::Success:
      return "success";
    case PatchApplyResultCategory::Cancelled:
      return "cancelled";
    case PatchApplyResultCategory::StaleDiff:
      return "stale_diff";
    case PatchApplyResultCategory::StaleGeneration:
      return "stale_generation";
    case PatchApplyResultCategory::PatchDidNotApply:
      return "patch_did_not_apply";
    case PatchApplyResultCategory::UnsupportedTarget:
      return "unsupported_target";
    case PatchApplyResultCategory::UnknownError:
      return "unknown_error";
  }
  return "unknown_error";
}

}  // namespace microide::project
