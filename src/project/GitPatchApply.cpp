#include "project/GitPatchApply.h"

#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "compare/CompareModel.h"
#include "project/PatchGenerator.h"
#include "util/TextFileIO.h"

#include "project/GitCommandUtil.h"
#include "project/GitRepository.h"

namespace microide::project {

namespace {

namespace gitutil = microide::project::internal;

GitPatchApplyOutcome RunGitApply(const platform::ProcessLauncher& launcher,
                                 const std::filesystem::path& repository_root,
                                 std::string_view patch_text,
                                 const GitPatchApplyOptions& options,
                                 bool check_only) {
  GitRepository repo(repository_root, launcher);
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
      launcher, repository_root, std::move(arguments), std::string(patch_text),
      false, gitutil::kGitWriteTimeoutMs);
  if (result.timed_out) {
    return GitPatchApplyOutcome{
        .ok = false,
        .exit_code = result.exit_code,
        .output = "git apply timed out and was aborted",
    };
  }
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

}

PatchApplyResultCategory ClassifyGitApplyFailure(std::string_view git_output) {
  const auto mentions = [git_output](std::string_view needle) {
    return git_output.find(needle) != std::string_view::npos;
  };
  if (mentions("corrupt patch") || mentions("unrecognized input") ||
      mentions("No valid patches in input") || mentions("patch fragment without header") ||
      mentions("new file with no content")) {
    return PatchApplyResultCategory::PatchDidNotApply;
  }
  if (mentions("patch does not apply") || mentions("does not match index") ||
      mentions("patch failed:") || mentions("already exists in working directory") ||
      mentions("No such file or directory") || mentions("does not exist in index")) {
    return PatchApplyResultCategory::StaleDiff;
  }
  return PatchApplyResultCategory::PatchDidNotApply;
}

GitPatchApplyOutcome PreflightGitPatch(const platform::ProcessLauncher& launcher,
                                       const std::filesystem::path& repository_root,
                                       std::string_view patch_text,
                                       const GitPatchApplyOptions& options) {
  return RunGitApply(launcher, repository_root, patch_text, options, true);
}

GitPatchApplyOutcome ApplyGitPatch(const platform::ProcessLauncher& launcher,
                                   const std::filesystem::path& repository_root,
                                   std::string_view patch_text,
                                   const GitPatchApplyOptions& options) {
  GitPatchApplyOutcome preflight =
      PreflightGitPatch(launcher, repository_root, patch_text, options);
  if (!preflight.ok) {
    return preflight;
  }
  return RunGitApply(launcher, repository_root, patch_text, options, false);
}

namespace {

std::variant<std::string, PatchApplyResult> RegenerateStagingPatch(
    const platform::ProcessLauncher& launcher,
    const PatchApplyRequest& request) {
  const PatchApplyTarget& target = request.target;
  const bool stage = request.operation == PatchOperationKind::StageHunk ||
                     request.operation == PatchOperationKind::StageSelectedLines;
  const GitRepository repo(target.repository_root, launcher);
  const auto head_blob = repo.ReadBlobAtRevision(target.relative_path, "HEAD");
  const auto index_blob = repo.ReadBlobAtRevision(target.relative_path, ":0");
  const std::string head = head_blob.has_value() ? head_blob->content : std::string();
  const std::string index = index_blob.has_value() ? index_blob->content : std::string();
  std::string worktree;
  bool worktree_exists = target.working_tree_exists;
  if (target.working_tree_source != nullptr) {
    worktree = *target.working_tree_source;
  } else {
    const util::TextFileReadResult working =
        util::ReadTextFileClassified(target.repository_root / target.relative_path);
    worktree = working.ok() ? working.content : std::string();
    worktree_exists = working.status != util::TextFileReadStatus::Missing;
  }
  StagingPatchOutcome outcome = BuildStagingPatch(
      head, index, worktree, *target.change_span, stage, target.relative_path,
      head_blob.has_value(), index_blob.has_value(), worktree_exists);
  if (outcome.already_applied) {
    return PatchApplyResult{
        .category = PatchApplyResultCategory::UnsupportedTarget,
        .detail = stage ? "Nothing to stage: this change is already in the index"
                        : "Nothing to unstage: this change is not in the index",
    };
  }
  if (outcome.span_not_intact || !outcome.patch.has_value()) {
    return PatchApplyResult{
        .category = PatchApplyResultCategory::StaleDiff,
        .detail = stage ? "This change is partly staged already; refresh the compare tab"
                        : "This change is partly unstaged already; refresh the compare tab",
    };
  }
  return std::move(*outcome.patch);
}

}

PatchApplyResult ApplyPatchRequest(const platform::ProcessLauncher& launcher,
                                   const PatchApplyRequest& request,
                                   std::string_view patch_text) {
  std::string regenerated;
  bool forward = false;
  if (request.target.change_span.has_value() &&
      request.operation != PatchOperationKind::DiscardHunk &&
      request.operation != PatchOperationKind::DiscardSelectedLines) {
    auto outcome = RegenerateStagingPatch(launcher, request);
    if (auto* failure = std::get_if<PatchApplyResult>(&outcome)) {
      return std::move(*failure);
    }
    regenerated = std::move(std::get<std::string>(outcome));
    patch_text = regenerated;
    forward = true;
  }
  if (patch_text.empty()) {
    return PatchApplyResult{
        .category = PatchApplyResultCategory::UnsupportedTarget,
        .detail = "no patch content was generated for the selection",
    };
  }

  GitPatchApplyOptions git_options{
      .apply_to_index = PatchOperationAppliesToIndex(request.operation),
      .reverse = !forward && PatchOperationReversesPatch(request.operation),
  };
  const GitPatchApplyOutcome outcome =
      ApplyGitPatch(launcher, request.target.repository_root, patch_text, git_options);
  if (outcome.ok) {
    return PatchApplyResult{
        .category = PatchApplyResultCategory::Success,
        .detail = {},
    };
  }
  return PatchApplyResult{
      .category = ClassifyGitApplyFailure(outcome.output),
      .detail = outcome.output.empty() ? "git apply failed" : outcome.output,
  };
}

}
