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

PatchApplyResult ApplyPatchRequest(const PatchApplyRequest& request,
                                   std::string_view patch_text);

}  // namespace microide::project
