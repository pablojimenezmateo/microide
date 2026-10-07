#pragma once

#include <cstddef>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "workspace/services/CompareMergeService.h"
#include "workspace/actions/WorkspaceActionRequests.h"
#include "workspace/state/WorkspaceProjectState.h"
#include "workspace/state/WorkspaceTabState.h"

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
    // Run `gather` off the shell thread, then `apply` on it -- dropped if the
    // project changed meanwhile (TD-2026-10-07-323). A review's git work (the file
    // list, then a blob prefetch for every file it opens) is one or two spawns
    // locally and as many round trips for a remote project, and it ran on the
    // shell thread before the first tab appeared. Unset: both run inline.
    std::function<void(std::function<void()> gather, std::function<void()> apply)>
        run_off_shell_thread;
    // Report an outcome that lands after the command returned.
    std::function<void(ReviewOpenOutcome)> report_outcome;
  };

  ReviewSessionCoordinator(ProjectWorkspaceState& state,
                           CompareMergeService compare_merge,
                           Operations operations);

  // A batch review verb opens one compare/merge tab (with a full diff/model build)
  // per newly-actionable file. The git collection helpers only cap at
  // kMaxGitCollectionEntries (50,000) as a defensive parse ceiling, which is far too
  // large to treat as a UI workload — opening thousands of tabs synchronously would
  // stall the shell. Cap the number of tabs a single review session opens and report
  // the remainder as truncated. TD-2026-07-17A-041.
  static constexpr std::size_t kMaxReviewSessionOpenTabs = 200;

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

  // `gather` does the git work and touches nothing shell-owned; `apply` runs on
  // the shell thread through a coordinator rebuilt from this one's handles (this
  // one is a per-use value, long gone by then) and returns the outcome.
  ReviewOpenOutcome Dispatch(std::function<void()> gather,
                             std::function<ReviewOpenOutcome(ReviewSessionCoordinator&)> apply);

  ProjectWorkspaceState& state_;
  CompareMergeService compare_merge_;
  Operations operations_;
};

}  // namespace microide::workspace
