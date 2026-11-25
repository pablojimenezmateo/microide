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

class ReviewSessionCoordinator {
 public:
  struct Operations {
    std::function<void()> show_git_sidebar;
    std::function<void(std::vector<std::size_t>)> request_close_tabs;
    std::function<bool(std::size_t)> tab_is_dirty;
    std::function<void(std::size_t)> activate_tab;
    std::function<void(std::function<void()> gather, std::function<void()> apply)>
        run_off_shell_thread;
    std::function<void(ReviewOpenOutcome)> report_outcome;
  };

  ReviewSessionCoordinator(ProjectWorkspaceState& state,
                           CompareMergeService compare_merge,
                           Operations operations);

  static constexpr std::size_t kMaxReviewSessionOpenTabs = 200;

  ReviewOpenOutcome OpenConflictReview();
  ReviewOpenOutcome OpenBranchReview(const std::string& ref);
  ReviewOpenOutcome OpenCommitReview(const std::string& ref);
  ReviewOpenOutcome OpenCheckpointReview();
  ReviewOpenOutcome MarkCheckpoint();

 private:
  ReviewOpenOutcome RunReviewSession(
      std::string_view verb,
      const std::vector<std::filesystem::path>& targets,
      const std::function<std::optional<std::filesystem::path>(const TabEntry&)>& scoped_path_of,
      const std::function<bool(const std::filesystem::path&)>& open_one,
      std::string_view empty_message);

  ReviewOpenOutcome Dispatch(std::function<void()> gather,
                             std::function<ReviewOpenOutcome(ReviewSessionCoordinator&)> apply);

  ProjectWorkspaceState& state_;
  CompareMergeService compare_merge_;
  Operations operations_;
};

}
