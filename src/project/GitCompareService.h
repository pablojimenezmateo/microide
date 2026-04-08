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
};

std::vector<GitCommitEntry> CollectGitFileHistory(const std::filesystem::path& root,
                                                  const std::filesystem::path& absolute_path);

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
std::vector<GitBranchFileEntry> CollectGitBranchOutgoingFiles(const std::filesystem::path& root,
                                                              std::string_view base_ref);

}  // namespace microide::project
