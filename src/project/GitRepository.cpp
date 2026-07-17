#include "project/GitRepository.h"

#include "project/GitCommandUtil.h"
#include "project/GitPorcelainParser.h"
#include "util/StringUtil.h"

#include <filesystem>
#include <system_error>

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

GitRepository::CommandResult GitRepository::ExecuteWithStdin(
    const std::vector<std::string>& arguments,
    std::string stdin_text,
    bool silence_stderr,
    int timeout_ms) const {
  const auto result = gitutil::ReadGitCommandOutputWithStdin(
      root_, std::vector<std::string>(arguments), std::move(stdin_text), silence_stderr,
      timeout_ms);
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
                               "--pretty=format:%H%x1f%h%x1f%an%x1f%ar%x1f%s", "--",
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
  return ExecuteSucceeds({"cat-file", "-e", "--end-of-options",
                          std::string(revision) + ":" + relative_path.generic_string()});
}

std::optional<GitRepository::BlobAtRevision> GitRepository::InterpretBlobResult(
    const CommandResult& result) {
  // A `truncated` result means the blob exceeded the subprocess capture ceiling and
  // git was killed (non-zero exit). Surface the partial content WITH the truncated
  // flag so a caller can render a "content truncated" state instead of mistaking the
  // clipped prefix for the whole file. A genuine failure (missing revision, other
  // non-zero exit) stays nullopt so it is distinguishable from a real empty blob.
  if (!result.success() && !result.truncated) {
    return std::nullopt;
  }
  return BlobAtRevision{.content = result.output, .truncated = result.truncated};
}

std::optional<GitRepository::BlobAtRevision> GitRepository::ReadBlobAtRevision(
    const std::filesystem::path& relative_path,
    std::string_view revision) const {
  const auto result = Execute(
      {"show", "--end-of-options", std::string(revision) + ":" + relative_path.generic_string()});
  return InterpretBlobResult(result);
}

std::optional<std::string> GitRepository::ReadFileAtRevision(
    const std::filesystem::path& relative_path,
    std::string_view revision) const {
  auto blob = ReadBlobAtRevision(relative_path, revision);
  if (!blob.has_value()) {
    return std::nullopt;
  }
  return std::move(blob->content);
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
  // Bound token materialization: this only needs one matching rename source, and a
  // rename record spans 3 NUL fields. Split at most 3x a generous file cap so a hostile
  // staged diff cannot build millions of record views; a legit staged rename set is far
  // smaller, so the match (if any) is well within the bound. (TD-2026-07-16-30.)
  constexpr std::size_t kMaxStagedRenameRecords = 50000 * 3 + 2;
  const std::vector<std::string_view> records =
      util::SplitNulDelimited(result.output, kMaxStagedRenameRecords);
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

bool GitRepository::Unstage(const std::filesystem::path& relative_path,
                            bool may_be_staged_rename) const {
  if (may_be_staged_rename) {
    if (const auto rename_source = StagedRenameSource(relative_path); rename_source.has_value()) {
      // Unstaging a staged rename must reset BOTH sides to HEAD, else the source's
      // staged deletion is orphaned (left staged with the file already gone).
      return ExecuteSucceeds(std::vector<std::string>{
          "restore", "--staged", "--", rename_source->generic_string(),
          relative_path.generic_string()});
    }
  }
  if (FileExistsAtRevision(relative_path)) {
    return ExecuteSucceeds({"restore", "--staged", "--", relative_path.generic_string()});
  }
  return ExecuteSucceeds({"rm", "--cached", "--", relative_path.generic_string()});
}

bool GitRepository::Discard(const std::filesystem::path& relative_path,
                            bool may_be_staged_rename) const {
  if (may_be_staged_rename) {
    if (const auto rename_source = StagedRenameSource(relative_path); rename_source.has_value()) {
      // Discarding a staged rename restores the source (content + tracking) and
      // removes the destination in one operation, fully undoing the rename. Without
      // this the source's staged deletion is left behind and its content is lost —
      // the destination-only path below would delete `new` and orphan `old`.
      return ExecuteSucceeds(std::vector<std::string>{
          "restore", "--source=HEAD", "--staged", "--worktree", "--",
          rename_source->generic_string(), relative_path.generic_string()});
    }
  }
  if (FileExistsAtRevision(relative_path)) {
    return ExecuteSucceeds(
        {"restore", "--source=HEAD", "--staged", "--worktree", "--",
         relative_path.generic_string()});
  }

  // The remaining case is an untracked file row. `git status --untracked-files=all`
  // lists individual files (never bare directories), so a single-row discard must
  // never recurse: `clean -fd <path>` on a path that is (or became, via a stale
  // row) a directory would delete the entire subtree — silent data loss. Refuse a
  // directory target here (a recursive clean must go through an explicit,
  // separately-confirmed directory flow) and drop `-d` so only the file is removed.
  std::error_code dir_error;
  // Classify the row node itself (symlink_status does not follow the link). An
  // untracked symlink whose target happens to be a directory is a single entry that
  // `git clean -f -- <link>` removes on its own; is_directory() would follow it and
  // refuse the discard as if it were a real subtree (TD-2026-07-17A-124). Only a
  // genuine directory node must be refused here.
  const std::filesystem::file_status node_status =
      std::filesystem::symlink_status(root_ / relative_path, dir_error);
  if (std::filesystem::is_directory(node_status)) {
    return false;
  }
  return ExecuteSucceeds(
             {"rm", "-f", "--cached", "--ignore-unmatch", "--",
              relative_path.generic_string()}) &&
         ExecuteSucceeds({"clean", "-f", "--", relative_path.generic_string()});
}

bool GitRepository::StageAll() const {
  return ExecuteSucceeds({"add", "-A", "--", "."});
}

bool GitRepository::DiscardAll(bool remove_untracked) const {
  if (HasHeadCommit()) {
    const bool reset_and_restore =
        ExecuteSucceeds({"reset", "--quiet", "HEAD", "--", "."}) &&
        ExecuteSucceeds({"restore", "--source=HEAD", "--worktree", "--", "."});
    if (!reset_and_restore) {
      return false;
    }
    return !remove_untracked || ExecuteSucceeds({"clean", "-fd", "--", "."});
  }

  if (!ExecuteSucceeds({"rm", "-r", "-f", "--cached", "--ignore-unmatch", "--", "."})) {
    return false;
  }
  return !remove_untracked || ExecuteSucceeds({"clean", "-fd", "--", "."});
}

}  // namespace microide::project
