#include "project/GitRepository.h"

#include "project/GitCommandUtil.h"
#include "project/GitPorcelainParser.h"
#include "util/StringUtil.h"

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
    bool silence_stderr,
    int timeout_ms) const {
  return Execute(OwnArguments(arguments), silence_stderr, timeout_ms);
}

GitRepository::CommandResult GitRepository::Execute(
    const std::vector<std::string>& arguments,
    bool silence_stderr,
    int timeout_ms) const {
  const auto result = gitutil::ReadGitCommandOutput(
      root_, std::vector<std::string>(arguments), silence_stderr, timeout_ms);
  return CommandResult{
      .exit_code = result.exit_code,
      .output = result.output,
      .timed_out = result.timed_out,
      .truncated = result.truncated,
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

GitFileHistoryResult GitRepository::GetFileHistory(
    const std::filesystem::path& relative_path) const {
  // Cap the walk: a file with an enormous history would otherwise stream every
  // commit into one entry-per-line vector, an unbounded memory/parse cost. We ask
  // for one past the cap so we can tell whether the history was actually longer
  // and report `truncated`, rather than silently hiding older commits.
  constexpr std::size_t kMaxFileHistoryCommits = 5000;
  const auto result = Execute({"log", "--follow", "--no-color", "-n", "5001",
                               "--pretty=format:%H%x09%h%x09%an%x09%ar%x09%s", "--",
                               relative_path.generic_string()});
  if (!result.success()) {
    return {};
  }
  GitFileHistoryResult history;
  history.commits = GitPorcelainParser::ParseLog(result.output);
  if (history.commits.size() > kMaxFileHistoryCommits) {
    history.commits.resize(kMaxFileHistoryCommits);
    history.truncated = true;
  }
  return history;
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
  // A `truncated` result means the blob exceeded the subprocess capture ceiling
  // and git was killed (non-zero exit). Return the partial content so an enormous
  // blob still shows (clipped) in compare/blob views rather than collapsing to a
  // generic error the way a real failure (missing revision) does.
  if (!result.success() && !result.truncated) {
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

std::optional<std::filesystem::path> GitRepository::StagedRenameSource(
    const std::filesystem::path& dest_relative) const {
  const std::string dest = dest_relative.generic_string();
  // `-z` name-status: non-rename records are <status>\0<path>\0; rename/copy
  // records are R<score>\0<source>\0<dest>\0. -M asks git to detect renames.
  const auto result = Execute({"diff", "--cached", "--name-status", "-M", "-z"});
  if (!result.success()) {
    return std::nullopt;
  }
  const std::vector<std::string_view> records = util::SplitNulDelimited(result.output);
  for (std::size_t i = 0; i + 1 < records.size();) {
    const std::string_view status = records[i];
    if (status.empty()) {
      ++i;
      continue;
    }
    if ((status[0] == 'R' || status[0] == 'C') && i + 2 < records.size()) {
      if (records[i + 2] == dest) {
        return std::filesystem::path(records[i + 1]);
      }
      i += 3;
    } else {
      i += 2;
    }
  }
  return std::nullopt;
}

bool GitRepository::Unstage(const std::filesystem::path& relative_path) const {
  if (const auto rename_source = StagedRenameSource(relative_path); rename_source.has_value()) {
    // Unstaging a staged rename must reset BOTH sides to HEAD, else the source's
    // staged deletion is orphaned (left staged with the file already gone).
    return ExecuteSucceeds(std::vector<std::string>{
        "restore", "--staged", "--", rename_source->generic_string(),
        relative_path.generic_string()});
  }
  if (FileExistsAtRevision(relative_path)) {
    return ExecuteSucceeds({"restore", "--staged", "--", relative_path.generic_string()});
  }
  return ExecuteSucceeds({"rm", "--cached", "--", relative_path.generic_string()});
}

bool GitRepository::Discard(const std::filesystem::path& relative_path) const {
  if (const auto rename_source = StagedRenameSource(relative_path); rename_source.has_value()) {
    // Discarding a staged rename restores the source (content + tracking) and
    // removes the destination in one operation, fully undoing the rename. Without
    // this the source's staged deletion is left behind and its content is lost —
    // the destination-only path below would delete `new` and orphan `old`.
    return ExecuteSucceeds(std::vector<std::string>{
        "restore", "--source=HEAD", "--staged", "--worktree", "--",
        rename_source->generic_string(), relative_path.generic_string()});
  }
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
