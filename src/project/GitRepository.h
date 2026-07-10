#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "project/GitCommandUtil.h"
#include "project/GitCompareService.h"
#include "project/GitStatusService.h"

namespace microide::project {

class GitRepository {
 public:
  explicit GitRepository(std::filesystem::path root);

  const std::filesystem::path& root() const { return root_; }
  bool IsValid() const;

  std::optional<std::filesystem::path> ToRelative(const std::filesystem::path& absolute_path) const;
  std::filesystem::path ToAbsolute(const std::filesystem::path& relative_path) const;

  struct CommandResult {
    int exit_code = -1;
    std::string output;
    bool timed_out = false;
    bool truncated = false;
    bool success() const { return exit_code == 0; }
  };

  // `timeout_ms` bounds the invocation (see internal::kGitReadTimeoutMs /
  // kGitWriteTimeoutMs). Read commands use the default; write/long ops (commit,
  // apply) pass the generous write cap so a slow pre-commit hook is not killed.
  CommandResult Execute(std::initializer_list<std::string_view> arguments,
                        bool silence_stderr = true,
                        int timeout_ms = internal::kGitReadTimeoutMs) const;
  CommandResult Execute(const std::vector<std::string>& arguments,
                        bool silence_stderr = true,
                        int timeout_ms = internal::kGitReadTimeoutMs) const;
  bool ExecuteSucceeds(std::initializer_list<std::string_view> arguments,
                       bool silence_stderr = true) const;
  bool ExecuteSucceeds(const std::vector<std::string>& arguments,
                       bool silence_stderr = true) const;

  std::unordered_map<std::string, GitFileStatus> GetStatuses() const;
  std::vector<GitWorkingTreeEntry> GetWorkingTreeEntries() const;
  GitFileHistoryResult GetFileHistory(const std::filesystem::path& relative_path) const;
  bool FileExistsAtRevision(const std::filesystem::path& relative_path,
                            std::string_view revision = "HEAD") const;

  // A blob read at a revision plus whether it was clipped. `truncated` is true when
  // the blob exceeded the subprocess capture ceiling and git was killed mid-read, so
  // `content` holds only a partial prefix that callers must NOT diff/save as truth.
  struct BlobAtRevision {
    std::string content;
    bool truncated = false;
  };

  // Interprets a `git show <rev>:<path>` result into a blob outcome. A real failure
  // (missing revision, non-zero exit that was NOT a capture-ceiling kill) is nullopt;
  // a success or a capture-ceiling truncation returns the (possibly partial) bytes
  // with `truncated` set accordingly. Pure and static so the truncation contract can
  // be unit-tested without spawning git.
  static std::optional<BlobAtRevision> InterpretBlobResult(const CommandResult& result);

  std::optional<BlobAtRevision> ReadBlobAtRevision(const std::filesystem::path& relative_path,
                                                   std::string_view revision = "HEAD") const;
  // Content-only convenience wrapper over ReadBlobAtRevision; drops the truncation
  // flag, so callers that must reject partial blobs use ReadBlobAtRevision instead.
  std::optional<std::string> ReadFileAtRevision(const std::filesystem::path& relative_path,
                                                std::string_view revision = "HEAD") const;
  std::optional<std::string> ResolveHeadId() const;
  bool HasHeadCommit() const;
  bool FileIsTracked(const std::filesystem::path& relative_path) const;
  bool FileIsWorkingTreeClean(const std::filesystem::path& relative_path) const;

  bool Stage(const std::filesystem::path& relative_path) const;
  bool Unstage(const std::filesystem::path& relative_path) const;
  bool Discard(const std::filesystem::path& relative_path) const;
  bool StageAll() const;
  bool DiscardAll() const;

 private:
  // If `dest_relative` is the destination of a currently-staged rename/copy,
  // returns the source path; otherwise nullopt. Lets Unstage/Discard operate on
  // both sides of a rename instead of orphaning the source's staged deletion.
  std::optional<std::filesystem::path> StagedRenameSource(
      const std::filesystem::path& dest_relative) const;

  std::filesystem::path root_;
};

}  // namespace microide::project
