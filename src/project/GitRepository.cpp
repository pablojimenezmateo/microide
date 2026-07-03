#include "project/GitRepository.h"

#include "project/GitCommandUtil.h"
#include "project/GitPorcelainParser.h"

namespace microide::project {

namespace gitutil = microide::project::internal;

namespace {

std::vector<std::string> OwnArguments(std::initializer_list<std::string_view> arguments) {
  std::vector<std::string> owned;
  owned.reserve(arguments.size());
  for (std::string_view argument : arguments) {
    owned.emplace_back(argument);
  }
  return owned;
}

}  // namespace

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

GitRepository::CommandResult GitRepository::Execute(
    std::initializer_list<std::string_view> arguments,
    bool silence_stderr) const {
  return Execute(OwnArguments(arguments), silence_stderr);
}

GitRepository::CommandResult GitRepository::Execute(
    const std::vector<std::string>& arguments,
    bool silence_stderr) const {
  const auto result =
      gitutil::ReadGitCommandOutput(root_, std::vector<std::string>(arguments), silence_stderr);
  return CommandResult{
      .exit_code = result.exit_code,
      .output = result.output,
  };
}

bool GitRepository::ExecuteSucceeds(std::initializer_list<std::string_view> arguments,
                                    bool silence_stderr) const {
  return Execute(arguments, silence_stderr).success();
}

bool GitRepository::ExecuteSucceeds(const std::vector<std::string>& arguments,
                                    bool silence_stderr) const {
  return Execute(arguments, silence_stderr).success();
}

std::unordered_map<std::string, GitFileStatus> GitRepository::GetStatuses() const {
  const auto result = Execute({"status", "--porcelain=v1", "-z", "--untracked-files=all"});
  if (!result.success() || result.output.empty()) {
    return {};
  }
  return GitPorcelainParser::ParseStatusV1(result.output);
}

std::vector<GitWorkingTreeEntry> GitRepository::GetWorkingTreeEntries() const {
  const auto result = Execute({"status", "--porcelain=v1", "-z", "--untracked-files=all"});
  if (!result.success() || result.output.empty()) {
    return {};
  }
  return GitPorcelainParser::ParseWorkingTreeEntries(result.output);
}

std::vector<GitCommitEntry> GitRepository::GetFileHistory(
    const std::filesystem::path& relative_path) const {
  // Cap the walk: a file with an enormous history would otherwise stream every
  // commit into one entry-per-line vector. 5000 is far more than any history view
  // displays, so this bounds memory/parse without losing anything the UI shows.
  const auto result = Execute({"log", "--follow", "--no-color", "-n", "5000",
                               "--pretty=format:%H%x09%h%x09%an%x09%ar%x09%s", "--",
                               relative_path.generic_string()});
  if (!result.success()) {
    return {};
  }
  return GitPorcelainParser::ParseLog(result.output);
}

bool GitRepository::FileExistsAtRevision(const std::filesystem::path& relative_path,
                                         std::string_view revision) const {
  return ExecuteSucceeds(
      {"cat-file", "-e", std::string(revision) + ":" + relative_path.generic_string()});
}

std::optional<std::string> GitRepository::ReadFileAtRevision(
    const std::filesystem::path& relative_path,
    std::string_view revision) const {
  const auto result =
      Execute({"show", std::string(revision) + ":" + relative_path.generic_string()});
  if (!result.success()) {
    return std::nullopt;
  }
  return result.output;
}

std::optional<std::string> GitRepository::ResolveHeadId() const {
  return gitutil::ResolveHeadId(root_);
}

bool GitRepository::HasHeadCommit() const {
  return ExecuteSucceeds({"rev-parse", "--verify", "HEAD"});
}

bool GitRepository::FileIsTracked(const std::filesystem::path& relative_path) const {
  return ExecuteSucceeds({"ls-files", "--error-unmatch", "--", relative_path.generic_string()});
}

bool GitRepository::FileIsWorkingTreeClean(const std::filesystem::path& relative_path) const {
  const auto result = Execute(
      {"status", "--porcelain=v1", "-z", "--untracked-files=all", "--",
       relative_path.generic_string()});
  return result.success() && result.output.empty();
}

bool GitRepository::Stage(const std::filesystem::path& relative_path) const {
  return ExecuteSucceeds({"add", "--", relative_path.generic_string()});
}

bool GitRepository::Unstage(const std::filesystem::path& relative_path) const {
  if (FileExistsAtRevision(relative_path)) {
    return ExecuteSucceeds({"restore", "--staged", "--", relative_path.generic_string()});
  }
  return ExecuteSucceeds({"rm", "--cached", "--", relative_path.generic_string()});
}

bool GitRepository::Discard(const std::filesystem::path& relative_path) const {
  if (FileExistsAtRevision(relative_path)) {
    return ExecuteSucceeds(
        {"restore", "--source=HEAD", "--staged", "--worktree", "--",
         relative_path.generic_string()});
  }

  return ExecuteSucceeds(
             {"rm", "-f", "--cached", "--ignore-unmatch", "--",
              relative_path.generic_string()}) &&
         ExecuteSucceeds({"clean", "-fd", "--", relative_path.generic_string()});
}

bool GitRepository::StageAll() const {
  return ExecuteSucceeds({"add", "-A", "--", "."});
}

bool GitRepository::DiscardAll() const {
  if (HasHeadCommit()) {
    return ExecuteSucceeds({"reset", "--quiet", "HEAD", "--", "."}) &&
           ExecuteSucceeds({"restore", "--source=HEAD", "--worktree", "--", "."}) &&
           ExecuteSucceeds({"clean", "-fd", "--", "."});
  }

  return ExecuteSucceeds({"rm", "-r", "-f", "--cached", "--ignore-unmatch", "--", "."}) &&
         ExecuteSucceeds({"clean", "-fd", "--", "."});
}

}  // namespace microide::project
