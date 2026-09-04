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

  // Applying a large patch can be slow, so use the generous write timeout rather
  // than the short read cap.
  const auto result = gitutil::ReadGitCommandOutputWithStdin(
      repository_root, std::move(arguments), std::string(patch_text),
      /*silence_stderr=*/false, gitutil::kGitWriteTimeoutMs);
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

}  // namespace

PatchApplyResultCategory ClassifyGitApplyFailure(std::string_view git_output) {
  const auto mentions = [git_output](std::string_view needle) {
    return git_output.find(needle) != std::string_view::npos;
  };
  // A corrupt / unparseable patch is checked FIRST: git reports the content
  // mismatch phrasings alongside it in some modes, and misreading our own bad
  // patch as "your diff is stale" would send the user to refresh forever.
  // These phrasings were captured from real `git apply` runs (see the unit test),
  // not guessed. "No valid patches in input" is what git says for wholly
  // unparseable input; it previously matched nothing here and only landed on
  // PatchDidNotApply via the fallback, which would have silently become
  // "your diff is stale" if the fallback were ever changed.
  if (mentions("corrupt patch") || mentions("unrecognized input") ||
      mentions("No valid patches in input") || mentions("patch fragment without header") ||
      mentions("new file with no content")) {
    return PatchApplyResultCategory::PatchDidNotApply;
  }
  // git apply's content/index mismatch vocabulary. `does not match index` is the
  // --cached form; the rest appear for worktree applies and for reverse applies
  // whose target line no longer holds the expected text.
  if (mentions("patch does not apply") || mentions("does not match index") ||
      mentions("patch failed:") || mentions("already exists in working directory") ||
      mentions("No such file or directory") || mentions("does not exist in index")) {
    return PatchApplyResultCategory::StaleDiff;
  }
  return PatchApplyResultCategory::PatchDidNotApply;
}

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

namespace {

// A Combined-view stage or unstage: build the patch against the index as it is
// now (see PatchChangeSpan). Returns the patch, or the result to report.
std::variant<std::string, PatchApplyResult> RegenerateStagingPatch(
    const PatchApplyRequest& request) {
  const PatchApplyTarget& target = request.target;
  const bool stage = request.operation == PatchOperationKind::StageHunk ||
                     request.operation == PatchOperationKind::StageSelectedLines;
  const GitRepository repo(target.repository_root);
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

}  // namespace

PatchApplyResult ApplyPatchRequest(const PatchApplyRequest& request, std::string_view patch_text) {
  // A regenerated patch already goes from the index to the desired state, for a
  // stage and an unstage alike, so it is applied forward either way.
  std::string regenerated;
  bool forward = false;
  if (request.target.change_span.has_value() &&
      request.operation != PatchOperationKind::DiscardHunk &&
      request.operation != PatchOperationKind::DiscardSelectedLines) {
    auto outcome = RegenerateStagingPatch(request);
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
      ApplyGitPatch(request.target.repository_root, patch_text, git_options);
  if (outcome.ok) {
    return PatchApplyResult{
        .category = PatchApplyResultCategory::Success,
        .detail = {},
        .completed_repository_generation = request.repository_snapshot_generation,
    };
  }
  return PatchApplyResult{
      .category = ClassifyGitApplyFailure(outcome.output),
      .detail = outcome.output.empty() ? "git apply failed" : outcome.output,
  };
}

}  // namespace microide::project
