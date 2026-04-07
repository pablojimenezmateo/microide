#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

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

}  // namespace microide::project
