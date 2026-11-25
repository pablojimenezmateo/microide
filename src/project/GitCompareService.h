#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "platform/ProcessLauncher.h"
#include "project/DirectoryTree.h"

namespace microide::project {

struct GitCommitEntry {
  std::string hash;
  std::string short_hash;
  std::string subject;
  std::string author;
  std::string relative_date;
};

struct GitFileHistoryResult {
  std::vector<GitCommitEntry> commits;
  bool truncated = false;
};

GitFileHistoryResult CollectGitFileHistory(const std::filesystem::path& root,
                                           const platform::ProcessLauncher& launcher,
                                           const std::filesystem::path& absolute_path);

std::vector<GitCommitEntry> CollectGitRecentCommits(const std::filesystem::path& root,
                                                    const platform::ProcessLauncher& launcher,
                                                    std::size_t limit);

[[nodiscard]] bool IsFullGitObjectId(std::string_view ref);

struct GitFileContentAtCommit {
  bool exists = false;
  std::string content;
  bool truncated = false;
};

class GitRevisionBlobCache {
 public:
  void Prefetch(const std::filesystem::path& root,
                const platform::ProcessLauncher& launcher,
                const std::vector<std::string>& revisions,
                const std::vector<std::filesystem::path>& absolute_paths);

  const GitFileContentAtCommit* Find(const std::filesystem::path& root,
                                     const std::string& revision,
                                     const std::filesystem::path& absolute_path) const;

  bool empty() const { return entries_.empty(); }

 private:
  std::filesystem::path root_;
  std::unordered_map<std::string, GitFileContentAtCommit> entries_;
};

std::optional<GitFileContentAtCommit> ReadGitFileAtCommit(
    const std::filesystem::path& root,
    const platform::ProcessLauncher& launcher,
    const std::filesystem::path& absolute_path,
    const std::string& hash,
    const GitRevisionBlobCache* prefetched = nullptr);

struct GitBranchReference {
  std::string ref;
  std::string label;
  bool is_head = false;
  bool is_remote = false;
};

struct GitBranchFileEntry {
  std::filesystem::path relative_path;
  GitFileStatus status = GitFileStatus::Clean;
};

std::optional<GitBranchReference> ResolveGitBaseReference(const std::filesystem::path& root,
                                                          const platform::ProcessLauncher& launcher);

std::vector<GitBranchReference> CollectGitBranches(const std::filesystem::path& root,
                                                   const platform::ProcessLauncher& launcher);

std::vector<GitBranchFileEntry> CollectGitBranchOutgoingFiles(
    const std::filesystem::path& root, const platform::ProcessLauncher& launcher,
    std::string_view base_ref);
std::vector<GitBranchFileEntry> ParseGitBranchDiffNameStatusZ(std::string_view output);
std::vector<GitBranchFileEntry> CollectGitWorkingTreeDiffFiles(
    const std::filesystem::path& root, const platform::ProcessLauncher& launcher,
    std::string_view ref);
std::vector<std::filesystem::path> CollectGitCommitChangedFiles(
    const std::filesystem::path& root, const platform::ProcessLauncher& launcher,
    std::string_view commit_hash);

}
