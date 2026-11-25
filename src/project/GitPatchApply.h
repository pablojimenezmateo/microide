#pragma once

#include <filesystem>
#include <string>

#include "project/PatchApplyTypes.h"
#include "platform/ProcessLauncher.h"

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

GitPatchApplyOutcome PreflightGitPatch(const platform::ProcessLauncher& launcher,
                                       const std::filesystem::path& repository_root,
                                       std::string_view patch_text,
                                       const GitPatchApplyOptions& options = {});

GitPatchApplyOutcome ApplyGitPatch(const platform::ProcessLauncher& launcher,
                                   const std::filesystem::path& repository_root,
                                   std::string_view patch_text,
                                   const GitPatchApplyOptions& options = {});

PatchApplyResultCategory ClassifyGitApplyFailure(std::string_view git_output);

PatchApplyResult ApplyPatchRequest(const platform::ProcessLauncher& launcher,
                                   const PatchApplyRequest& request,
                                   std::string_view patch_text);

}
