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
  std::filesystem::path root_;
};

}  // namespace microide::project
