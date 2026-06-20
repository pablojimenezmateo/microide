#pragma once

#include <cstddef>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "workspace/CompareMergeService.h"
#include "workspace/WorkspaceActionRequests.h"
#include "workspace/WorkspaceProjectState.h"
#include "workspace/WorkspaceTabState.h"

namespace microide::workspace {

// Drives the batch review verbs (review-conflicts / review-branch / review-commit):
// enumerate the relevant git files, switch to Source Control, and reconcile the
// open diff/merge tabs against that target set (dedup + clean stale, never closing
// dirty tabs). Host-owned rendering and tab lifecycle stay behind the injected
// callbacks; this coordinator owns no shell access.
class ReviewSessionCoordinator {
 public:
  struct Operations {
    std::function<void()> show_git_sidebar;
    std::function<void(std::vector<std::size_t>)> request_close_tabs;
    std::function<bool(std::size_t)> tab_is_dirty;
  };

  ReviewSessionCoordinator(ProjectWorkspaceState& state,
                           CompareMergeService compare_merge,
                           Operations operations);

  ReviewOpenOutcome OpenConflictReview();
  ReviewOpenOutcome OpenBranchReview(const std::string& ref);
  ReviewOpenOutcome OpenCommitReview(const std::string& ref);

 private:
  ReviewOpenOutcome RunReviewSession(
      std::string_view verb,
      const std::vector<std::filesystem::path>& targets,
      const std::function<std::optional<std::filesystem::path>(const TabEntry&)>& scoped_path_of,
      const std::function<bool(const std::filesystem::path&)>& open_one,
      std::string_view empty_message);

  ProjectWorkspaceState& state_;
  CompareMergeService compare_merge_;
  Operations operations_;
};

}  // namespace microide::workspace
