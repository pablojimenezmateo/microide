#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "project/DirectoryTree.h"

namespace microide::project {

struct GitCommitEntry {
  std::string hash;
  std::string short_hash;
  std::string subject;
  std::string author;         // short author name (%an)
  std::string relative_date;  // human relative date (%ar), e.g. "2 days ago"
};

// File history plus whether it was capped. `truncated` is true when the file has
// more commits than the display cap, so callers can indicate the list is partial
// instead of silently hiding older commits.
struct GitFileHistoryResult {
  std::vector<GitCommitEntry> commits;
  bool truncated = false;
};

GitFileHistoryResult CollectGitFileHistory(const std::filesystem::path& root,
                                           const std::filesystem::path& absolute_path);

// Repo-wide commit log on HEAD, newest first, capped at `limit`. Used by the
// ref/commit picker so the outgoing-base flow can offer recent commits.
std::vector<GitCommitEntry> CollectGitRecentCommits(const std::filesystem::path& root,
                                                    std::size_t limit);

struct GitFileContentAtCommit {
  bool exists = false;
  std::string content;
};

std::optional<GitFileContentAtCommit> ReadGitFileAtCommit(const std::filesystem::path& root,
                                                          const std::filesystem::path& absolute_path,
                                                          const std::string& hash);

struct GitBranchReference {
  std::string ref;
  std::string label;
};

struct GitBranchFileEntry {
  std::filesystem::path relative_path;
  GitFileStatus status = GitFileStatus::Clean;
};

std::optional<GitBranchReference> ResolveGitBaseReference(const std::filesystem::path& root);

// Local + remote branches, most-recently-committed first, with short labels
// ("main", "origin/main"). Symbolic refs like origin/HEAD are skipped.
std::vector<GitBranchReference> CollectGitBranches(const std::filesystem::path& root);

std::vector<GitBranchFileEntry> CollectGitBranchOutgoingFiles(const std::filesystem::path& root,
                                                              std::string_view base_ref);
// Parses the NUL-delimited output of `git diff --name-status -z --find-renames`.
// Exposed for testing: handles paths containing spaces and rename/copy records
// (status NUL old NUL new), which the previous whitespace-split parser corrupted.
std::vector<GitBranchFileEntry> ParseGitBranchDiffNameStatusZ(std::string_view output);
// Files differing between `ref` and the current working tree (two-dot diff, so
// uncommitted edits are included). Backs the `review-branch` control verb.
std::vector<GitBranchFileEntry> CollectGitWorkingTreeDiffFiles(const std::filesystem::path& root,
                                                              std::string_view ref);
std::vector<std::filesystem::path> CollectGitCommitChangedFiles(const std::filesystem::path& root,
                                                                std::string_view commit_hash);

}  // namespace microide::project
