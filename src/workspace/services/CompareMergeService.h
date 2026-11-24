#pragma once

#include <utility>

#include "workspace/coordinators/WorkspaceCompareInteractionCoordinator.h"
#include "workspace/coordinators/WorkspaceDiffTabCoordinator.h"

namespace microide::workspace {

// The compare/merge surface's two halves, owned together: opening diff and merge
// tabs (DiffTabCoordinator) and everything done inside one (the commit picker,
// selection, hunks, staging, merge choices: CompareInteractionCoordinator).
// Callers reach the half they use; this class adds no behaviour of its own.
class CompareMergeService {
 public:
  CompareMergeService(DiffTabCoordinator diff_tabs, CompareInteractionCoordinator interactions)
      : diff_tabs_(std::move(diff_tabs)), interactions_(std::move(interactions)) {}

  DiffTabCoordinator& diff_tabs() { return diff_tabs_; }
  const DiffTabCoordinator& diff_tabs() const { return diff_tabs_; }
  CompareInteractionCoordinator& interactions() { return interactions_; }
  const CompareInteractionCoordinator& interactions() const { return interactions_; }

 private:
  DiffTabCoordinator diff_tabs_;
  CompareInteractionCoordinator interactions_;
};

}  // namespace microide::workspace
