#include "project/GitCommitExecutor.h"

#include <algorithm>

#include "project/GitRepository.h"
#include "util/StringUtil.h"

namespace microide::project {
namespace {
}  // namespace

CommitOperationResultCategory ClassifyCommitFailure(const int exit_code,
                                                    const std::string_view output) {
  if (exit_code == 0) {
    return CommitOperationResultCategory::Success;
  }

  const std::string lowered = util::ToLowerAscii(std::string(output));
  if (lowered.find("pre-commit") != std::string::npos ||
      lowered.find("commit-msg") != std::string::npos ||
      lowered.find("prepare-commit-msg") != std::string::npos ||
      lowered.find("post-commit") != std::string::npos ||
      lowered.find("hook") != std::string::npos) {
    return CommitOperationResultCategory::HookFailed;
  }
  if (lowered.find("user.name") != std::string::npos ||
      lowered.find("user.email") != std::string::npos ||
      lowered.find("author identity unknown") != std::string::npos ||
      lowered.find("please tell me who you are") != std::string::npos) {
    return CommitOperationResultCategory::AuthFailed;
  }
  if (lowered.find("gpg") != std::string::npos || lowered.find("signing") != std::string::npos ||
      lowered.find("sign failed") != std::string::npos) {
    return CommitOperationResultCategory::AuthFailed;
  }
  if (lowered.find("index.lock") != std::string::npos ||
      lowered.find("cannot lock ref") != std::string::npos) {
    return CommitOperationResultCategory::RepoLocked;
  }
  if (lowered.find("conflict") != std::string::npos) {
    return CommitOperationResultCategory::Conflict;
  }
  if (lowered.find("nothing to commit") != std::string::npos ||
      lowered.find("no changes added to commit") != std::string::npos) {
    return CommitOperationResultCategory::DirtyWorktree;
  }
  (void)exit_code;
  return CommitOperationResultCategory::UnknownError;
}

CommitOperationResult ExecuteGitCommit(const std::filesystem::path& repository_root,
                                       const std::string_view subject,
                                       const std::string_view body,
                                       const CommitOperationKind operation) {
  CommitOperationResult result;
  if (repository_root.empty()) {
    result.category = CommitOperationResultCategory::UnknownError;
    result.detail = "Repository root is unavailable";
    return result;
  }

  GitRepository repo(repository_root);
  std::vector<std::string> arguments;
  arguments.emplace_back("commit");
  if (operation == CommitOperationKind::Amend) {
    arguments.emplace_back("--amend");
  }
  if (operation == CommitOperationKind::NoVerify) {
    arguments.emplace_back("--no-verify");
  }
  arguments.emplace_back("-m");
  arguments.emplace_back(std::string(subject));
  if (!body.empty()) {
    arguments.emplace_back("-m");
    arguments.emplace_back(std::string(body));
  }

  const GitRepository::CommandResult command = repo.Execute(arguments, false);
  result.hook_output = command.output;
  util::TrimTrailingLineEndings(&result.hook_output);
  result.category = ClassifyCommitFailure(command.exit_code, result.hook_output);
  if (result.category == CommitOperationResultCategory::Success) {
    result.detail.clear();
    return result;
  }

  switch (result.category) {
    case CommitOperationResultCategory::HookFailed:
      result.detail = "Commit hook failed; no commit was created";
      break;
    case CommitOperationResultCategory::AuthFailed:
      result.detail = "Git author or signing configuration is incomplete";
      break;
    case CommitOperationResultCategory::RepoLocked:
      result.detail = "Git repository is locked";
      break;
    case CommitOperationResultCategory::Conflict:
      result.detail = "Commit failed due to repository conflicts";
      break;
    case CommitOperationResultCategory::DirtyWorktree:
      result.detail = "Nothing to commit";
      break;
    default:
      result.detail = result.hook_output.empty() ? "Commit failed" : result.hook_output;
      break;
  }
  return result;
}

}  // namespace microide::project
