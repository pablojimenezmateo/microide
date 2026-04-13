#include "project/GitRepository.h"

#include "project/GitCommandUtil.h"
#include "project/GitPorcelainParser.h"

namespace microide::project {

namespace gitutil = microide::project::internal;

GitRepository::GitRepository(std::filesystem::path root) : root_(std::move(root)) {
  if (!root_.empty()) {
    root_ = root_.lexically_normal();
  }
}

bool GitRepository::IsValid() const {
  return gitutil::HasGitMarker(root_);
}

std::optional<std::filesystem::path> GitRepository::ToRelative(
    const std::filesystem::path& absolute_path) const {
  return gitutil::AbsoluteToRelativePath(root_, absolute_path);
}

std::filesystem::path GitRepository::ToAbsolute(
    const std::filesystem::path& relative_path) const {
  return (root_ / relative_path).lexically_normal();
}

GitRepository::CommandResult GitRepository::Execute(std::string_view arguments,
                                                    bool silence_stderr) const {
  const std::string command = gitutil::BuildGitCommand(root_, arguments, silence_stderr);
  const auto result = gitutil::ReadCommandOutput(command);
  return CommandResult{
      .exit_code = result.exit_code,
      .output = result.output,
  };
}

bool GitRepository::ExecuteSucceeds(std::string_view arguments, bool silence_stderr) const {
  return Execute(arguments, silence_stderr).success();
}

std::unordered_map<std::string, GitFileStatus> GitRepository::GetStatuses() const {
  const auto result = Execute("status --porcelain=v1 -z --untracked-files=all");
  if (!result.success() || result.output.empty()) {
    return {};
  }
  return GitPorcelainParser::ParseStatusV1(result.output);
}

std::vector<GitWorkingTreeEntry> GitRepository::GetWorkingTreeEntries() const {
  const auto result = Execute("status --porcelain=v1 -z --untracked-files=all");
  if (!result.success() || result.output.empty()) {
    return {};
  }
  return GitPorcelainParser::ParseWorkingTreeEntries(result.output);
}

std::vector<GitCommitEntry> GitRepository::GetFileHistory(
    const std::filesystem::path& relative_path) const {
  const std::string arguments =
      "log --follow --no-color --pretty=format:%H%x09%h%x09%s -- '" +
      gitutil::EscapeShellArg(relative_path.generic_string()) + "'";
  const auto result = Execute(arguments);
  if (!result.success()) {
    return {};
  }
  return GitPorcelainParser::ParseLog(result.output);
}

bool GitRepository::FileExistsAtRevision(const std::filesystem::path& relative_path,
                                         std::string_view revision) const {
  const std::string spec = std::string(revision) + ":" + relative_path.generic_string();
  return ExecuteSucceeds("cat-file -e '" + gitutil::EscapeShellArg(spec) + "'");
}

std::optional<std::string> GitRepository::ReadFileAtRevision(
    const std::filesystem::path& relative_path,
    std::string_view revision) const {
  const std::string spec = std::string(revision) + ":" + relative_path.generic_string();
  const std::string arguments = "show '" + gitutil::EscapeShellArg(spec) + "'";
  const auto result = Execute(arguments);
  if (!result.success()) {
    return std::nullopt;
  }
  return result.output;
}

std::optional<std::string> GitRepository::ResolveHeadId() const {
  const auto result = Execute("rev-parse --verify HEAD");
  if (!result.success() || result.output.empty()) {
    return std::nullopt;
  }
  std::string id = result.output;
  while (!id.empty() && (id.back() == '\n' || id.back() == '\r')) {
    id.pop_back();
  }
  return id;
}

bool GitRepository::HasHeadCommit() const {
  return ExecuteSucceeds("rev-parse --verify HEAD >/dev/null");
}

bool GitRepository::FileIsTracked(const std::filesystem::path& relative_path) const {
  return ExecuteSucceeds("ls-files --error-unmatch -- '" +
                         gitutil::EscapeShellArg(relative_path.generic_string()) + "' >/dev/null");
}

bool GitRepository::FileIsWorkingTreeClean(const std::filesystem::path& relative_path) const {
  const auto result = Execute("status --porcelain=v1 -z --untracked-files=all -- '" +
                              gitutil::EscapeShellArg(relative_path.generic_string()) + "'");
  return result.success() && result.output.empty();
}

bool GitRepository::Stage(const std::filesystem::path& relative_path) const {
  return ExecuteSucceeds("add -- '" + gitutil::EscapeShellArg(relative_path.generic_string()) + "'");
}

bool GitRepository::Unstage(const std::filesystem::path& relative_path) const {
  const std::string escaped_relative = gitutil::EscapeShellArg(relative_path.generic_string());
  if (FileExistsAtRevision(relative_path)) {
    return ExecuteSucceeds("restore --staged -- '" + escaped_relative + "' >/dev/null");
  }
  return ExecuteSucceeds("rm --cached -- '" + escaped_relative + "' >/dev/null");
}

bool GitRepository::Discard(const std::filesystem::path& relative_path) const {
  const std::string escaped_relative = gitutil::EscapeShellArg(relative_path.generic_string());
  if (FileExistsAtRevision(relative_path)) {
    return ExecuteSucceeds("restore --source=HEAD --staged --worktree -- '" + escaped_relative +
                           "' >/dev/null");
  }

  return ExecuteSucceeds("rm -f --cached --ignore-unmatch -- '" + escaped_relative +
                             "' >/dev/null") &&
         ExecuteSucceeds("clean -fd -- '" + escaped_relative + "' >/dev/null");
}

bool GitRepository::StageAll() const {
  return ExecuteSucceeds("add -A -- . >/dev/null");
}

bool GitRepository::DiscardAll() const {
  if (HasHeadCommit()) {
    return ExecuteSucceeds("reset --quiet HEAD -- . >/dev/null") &&
           ExecuteSucceeds("restore --source=HEAD --worktree -- . >/dev/null") &&
           ExecuteSucceeds("clean -fd -- . >/dev/null");
  }

  return ExecuteSucceeds("rm -r -f --cached --ignore-unmatch -- . >/dev/null") &&
         ExecuteSucceeds("clean -fd -- . >/dev/null");
}

}  // namespace microide::project
