#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "project/ProjectChangeTypes.h"

namespace microide::project {

class GitMetadataSource;

std::optional<std::string> ReadHeadBranchName(const std::filesystem::path& project_root,
                                              const GitMetadataSource& metadata);

class GitRepositoryMetadataTracker {
 public:
  void Reset();
  void SetProjectRoot(const std::filesystem::path& project_root);
  std::vector<RepositoryChange> SampleChanges();
  std::vector<std::filesystem::path> WatchedDirectories() const;

 private:
  struct MetadataFingerprint {
    std::string head_text;
    std::string branch_ref_text;
    std::uint64_t index = 0;
    std::uint64_t packed_refs = 0;
    std::uint64_t worktrees = 0;
  };

  std::optional<MetadataFingerprint> ReadCurrentFingerprint() const;

  std::filesystem::path project_root_;
  std::optional<MetadataFingerprint> baseline_;
};

}
