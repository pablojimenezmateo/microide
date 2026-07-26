#pragma once

#include <filesystem>
#include <string>

#include "project/PatchApplyTypes.h"

namespace microide::project {

struct GitPatchApplyOptions {
  bool apply_to_index = true;
  bool reverse = false;
};

struct GitPatchApplyOutcome {
  bool ok = false;
  int exit_code = -1;
  std::string output;
};

GitPatchApplyOutcome PreflightGitPatch(const std::filesystem::path& repository_root,
                                       std::string_view patch_text,
                                       const GitPatchApplyOptions& options = {});

GitPatchApplyOutcome ApplyGitPatch(const std::filesystem::path& repository_root,
                                   std::string_view patch_text,
                                   const GitPatchApplyOptions& options = {});

// Classify a failed `git apply` from its stderr. The distinction that matters to
// the user is whether the PATCH is bad or the FILE MOVED under the diff they are
// looking at: the second is the common case (edit or pull between opening the
// compare tab and clicking the hunk) and has an actionable message — "refresh the
// compare tab and try again" — while the first does not. Both collapsed into
// PatchDidNotApply, so a stale diff surfaced a raw `error: patch does not apply`
// with no guidance.
//
// Conservative by construction: only git's content/index-mismatch phrasings map
// to StaleDiff. A corrupt or unrecognized patch is OUR bug, not staleness, and
// stays PatchDidNotApply so it is not disguised as a refresh-and-retry.
PatchApplyResultCategory ClassifyGitApplyFailure(std::string_view git_output);

PatchApplyResult ApplyPatchRequest(const PatchApplyRequest& request,
                                   std::string_view patch_text);

}  // namespace microide::project
