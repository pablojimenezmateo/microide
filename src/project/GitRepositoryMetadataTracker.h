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
    // Tick of the branch ref HEAD points at (loose ref under the common gitdir). Catches
    // the common "same branch, new commit" case where `.git/HEAD` text is unchanged but
    // `refs/heads/<branch>` advances. (TD-2026-07-16-63.)
    std::uint64_t branch_ref = 0;
    // Tick of `packed-refs`: a fallback for branch refs stored packed rather than loose.
    std::uint64_t packed_refs = 0;
  };

  std::optional<MetadataTick> ReadCurrentTicks() const;

  std::filesystem::path project_root_;
  std::optional<MetadataTick> baseline_;
};

}  // namespace microide::project
