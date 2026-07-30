#include "project/PatchApplyTypes.h"

namespace microide::project {

bool PatchOperationAppliesToIndex(const PatchOperationKind operation) {
  switch (operation) {
    case PatchOperationKind::StageHunk:
    case PatchOperationKind::StageSelectedLines:
    case PatchOperationKind::UnstageHunk:
    case PatchOperationKind::UnstageSelectedLines:
      return true;
    case PatchOperationKind::DiscardHunk:
    case PatchOperationKind::DiscardSelectedLines:
      return false;
  }
  return false;
}

bool PatchOperationReversesPatch(const PatchOperationKind operation) {
  switch (operation) {
    case PatchOperationKind::UnstageHunk:
    case PatchOperationKind::UnstageSelectedLines:
    case PatchOperationKind::DiscardHunk:
    case PatchOperationKind::DiscardSelectedLines:
      return true;
    default:
      return false;
  }
}

}  // namespace microide::project
