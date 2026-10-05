#pragma once

#include <filesystem>
#include <string>

#include "platform/ProcessLauncher.h"
#include "project/CommitWorkflowTypes.h"

namespace microide::project {

// `launcher` is required, with no default (TD-2026-09-22-301): a commit is the
// least recoverable thing to run on the wrong machine.
CommitOperationResult ExecuteGitCommit(const std::filesystem::path& repository_root,
                                       const platform::ProcessLauncher& launcher,
                                       std::string_view subject,
                                       std::string_view body,
                                       CommitOperationKind operation);

CommitOperationResultCategory ClassifyCommitFailure(int exit_code, std::string_view output);

}  // namespace microide::project
