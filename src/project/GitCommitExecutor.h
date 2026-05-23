#pragma once

#include <filesystem>
#include <string>

#include "project/CommitWorkflowTypes.h"

namespace microide::project {

CommitOperationResult ExecuteGitCommit(const std::filesystem::path& repository_root,
                                       std::string_view subject,
                                       std::string_view body,
                                       CommitOperationKind operation);

CommitOperationResultCategory ClassifyCommitFailure(int exit_code, std::string_view output);

}  // namespace microide::project
