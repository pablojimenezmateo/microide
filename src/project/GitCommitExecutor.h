#pragma once

#include <filesystem>
#include <string>

#include "platform/ProcessLauncher.h"
#include "project/CommitWorkflowTypes.h"

namespace microide::project {

CommitOperationResult ExecuteGitCommit(const std::filesystem::path& repository_root,
                                       const platform::ProcessLauncher& launcher,
                                       std::string_view subject,
                                       std::string_view body,
                                       CommitOperationKind operation);

CommitOperationResultCategory ClassifyCommitFailure(int exit_code, std::string_view output);

}
