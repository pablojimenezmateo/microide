#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "project/GitCommandUtil.h"
#include "platform/ProcessLauncher.h"
#include "project/GitCompareService.h"
#include "project/GitStatusService.h"

namespace microide::project {

class GitRepository {
 public:
  GitRepository(std::filesystem::path root, const platform::ProcessLauncher& launcher);

  const std::filesystem::path& root() const { return root_; }
  const platform::ProcessLauncher& launcher() const { return *launcher_; }
  bool IsValid() const;

  std::optional<std::filesystem::path> ToRelative(const std::filesystem::path& absolute_path) const;
  std::filesystem::path ToAbsolute(const std::filesystem::path& relative_path) const;

  using CommandResult = internal::CommandResult;

  CommandResult Execute(std::initializer_list<std::string_view> arguments,
                        bool silence_stderr = true,
                        int timeout_ms = internal::kGitReadTimeoutMs) const;
  CommandResult Execute(const std::vector<std::string>& arguments,
                        bool silence_stderr = true,
                        int timeout_ms = internal::kGitReadTimeoutMs) const;
  CommandResult ExecuteWithStdin(const std::vector<std::string>& arguments,
                                 std::string stdin_text,
                                 bool silence_stderr = true,
                                 int timeout_ms = internal::kGitReadTimeoutMs) const;
  CommandResult ExecuteWithEnvironment(const std::vector<std::string>& arguments,
                                       std::vector<platform::SubprocessEnvironmentOverride> environment,
                                       int timeout_ms = internal::kGitReadTimeoutMs) const;
  bool ExecuteSucceeds(std::initializer_list<std::string_view> arguments,
                       bool silence_stderr = true) const;
  bool ExecuteSucceeds(const std::vector<std::string>& arguments,
                       bool silence_stderr = true) const;

  std::optional<std::vector<GitWorkingTreeEntry>> GetWorkingTreeEntries() const;
  GitFileHistoryResult GetFileHistory(const std::filesystem::path& relative_path) const;
  bool FileExistsAtRevision(const std::filesystem::path& relative_path,
                            std::string_view revision = "HEAD") const;

  struct BlobAtRevision {
    std::string content;
    bool truncated = false;
  };

  static std::optional<BlobAtRevision> InterpretBlobResult(const CommandResult& result);

  using BlobLookup = GitFileContentAtCommit;

  std::optional<BlobLookup> LookupBlobAtRevision(const std::filesystem::path& relative_path,
                                                 std::string_view revision = "HEAD") const;

  static std::optional<BlobLookup> InterpretBatchBlobResult(const CommandResult& result);

  static bool ReadNextBatchBlob(std::string_view output, std::size_t& offset, BlobLookup& out);

  struct BlobRequest {
    std::string revision;
    std::filesystem::path relative_path;
  };

  std::vector<std::optional<BlobLookup>> LookupBlobsAtRevisions(
      const std::vector<BlobRequest>& requests) const;

  std::optional<BlobAtRevision> ReadBlobAtRevision(const std::filesystem::path& relative_path,
                                                   std::string_view revision = "HEAD") const;
  std::optional<std::string> ReadFileAtRevision(const std::filesystem::path& relative_path,
                                                std::string_view revision = "HEAD") const;
  bool HasHeadCommit() const;
  bool FileIsTracked(const std::filesystem::path& relative_path) const;
  bool FileIsWorkingTreeClean(const std::filesystem::path& relative_path) const;

  bool Stage(const std::filesystem::path& relative_path) const;
  bool Unstage(const std::filesystem::path& relative_path,
               bool may_be_staged_rename = true) const;
  bool Discard(const std::filesystem::path& relative_path,
               bool may_be_staged_rename = true) const;
  bool StageAll() const;
  bool DiscardAll(bool remove_untracked = true) const;

 private:
  std::optional<std::filesystem::path> StagedRenameSource(
      const std::filesystem::path& dest_relative) const;

  std::filesystem::path root_;
  const platform::ProcessLauncher* launcher_ = nullptr;
};

}
