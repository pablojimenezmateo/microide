#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

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
    bool success() const { return exit_code == 0; }
  };

  CommandResult Execute(std::initializer_list<std::string_view> arguments,
                        bool silence_stderr = true) const;
  CommandResult Execute(const std::vector<std::string>& arguments,
                        bool silence_stderr = true) const;
  bool ExecuteSucceeds(std::initializer_list<std::string_view> arguments,
                       bool silence_stderr = true) const;
  bool ExecuteSucceeds(const std::vector<std::string>& arguments,
                       bool silence_stderr = true) const;

  std::unordered_map<std::string, GitFileStatus> GetStatuses() const;
  std::vector<GitWorkingTreeEntry> GetWorkingTreeEntries() const;
  std::vector<GitCommitEntry> GetFileHistory(const std::filesystem::path& relative_path) const;
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
