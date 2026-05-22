#pragma once

#include <filesystem>
#include <optional>
#include <vector>

#include "project/ProjectChangeTypes.h"

namespace microide::project {

class GitRepositoryMetadataTracker {
 public:
  void Reset();
  void SetProjectRoot(const std::filesystem::path& project_root);
  std::vector<RepositoryChange> SampleChanges();

 private:
  struct MetadataTick {
    std::uint64_t head = 0;
    std::uint64_t index = 0;
  };

  std::optional<MetadataTick> ReadCurrentTicks() const;

  std::filesystem::path project_root_;
  std::optional<MetadataTick> baseline_;
};

}  // namespace microide::project
