#include "project/GitPatchApply.h"

#include <vector>

#include "project/GitCommandUtil.h"
#include "project/GitRepository.h"

namespace microide::project {

namespace {

namespace gitutil = microide::project::internal;

GitPatchApplyOutcome RunGitApply(const std::filesystem::path& repository_root,
                                 std::string_view patch_text,
                                 const GitPatchApplyOptions& options,
                                 bool check_only) {
  GitRepository repo(repository_root);
  if (!repo.IsValid() || patch_text.empty()) {
    return GitPatchApplyOutcome{
        .ok = false,
        .exit_code = -1,
        .output = "repository is not valid or patch is empty",
    };
  }

  std::vector<std::string> arguments = {"apply"};
  if (check_only) {
    arguments.push_back("--check");
  }
  if (options.apply_to_index) {
    arguments.push_back("--cached");
  }
  if (options.reverse) {
    arguments.push_back("--reverse");
  }
  arguments.emplace_back("-");

  const auto result = gitutil::ReadGitCommandOutputWithStdin(
      repository_root, std::move(arguments), std::string(patch_text),
      /*silence_stderr=*/false);
  if (!result.success() && result.output.empty()) {
    return GitPatchApplyOutcome{
        .ok = false,
        .exit_code = result.exit_code,
        .output = "git apply failed",
    };
  }
  return GitPatchApplyOutcome{
      .ok = result.success(),
      .exit_code = result.exit_code,
      .output = result.output,
  };
}

}  // namespace

GitPatchApplyOutcome PreflightGitPatch(const std::filesystem::path& repository_root,
                                       std::string_view patch_text,
                                       const GitPatchApplyOptions& options) {
  return RunGitApply(repository_root, patch_text, options, true);
}

GitPatchApplyOutcome ApplyGitPatch(const std::filesystem::path& repository_root,
                                   std::string_view patch_text,
                                   const GitPatchApplyOptions& options) {
  GitPatchApplyOutcome preflight = PreflightGitPatch(repository_root, patch_text, options);
  if (!preflight.ok) {
    return preflight;
  }
  return RunGitApply(repository_root, patch_text, options, false);
}

PatchApplyResult ApplyPatchRequest(const PatchApplyRequest& request, std::string_view patch_text) {
  if (patch_text.empty()) {
    return PatchApplyResult{
        .category = PatchApplyResultCategory::UnsupportedTarget,
        .detail = "no patch content was generated for the selection",
    };
  }

  GitPatchApplyOptions git_options{
      .apply_to_index = PatchOperationAppliesToIndex(request.operation),
      .reverse = PatchOperationReversesPatch(request.operation),
  };
  const GitPatchApplyOutcome outcome =
      ApplyGitPatch(request.target.repository_root, patch_text, git_options);
  if (outcome.ok) {
    return PatchApplyResult{
        .category = PatchApplyResultCategory::Success,
        .completed_repository_generation = request.repository_snapshot_generation,
    };
  }
  return PatchApplyResult{
      .category = PatchApplyResultCategory::PatchDidNotApply,
      .detail = outcome.output.empty() ? "git apply failed" : outcome.output,
  };
}

}  // namespace microide::project
