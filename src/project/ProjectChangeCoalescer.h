#pragma once

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

#include "project/ProjectChangeTypes.h"

namespace microide::project {

class ProjectChangeCoalescer {
 public:
  void Reset();
  void Ingest(ProjectChangeBatch batch);
  std::optional<ProjectChangeBatch> ConsumeReady(std::uint64_t* out_generation = nullptr);

 private:
  void MergeFileChange(ProjectFileChange change);
  void MergeRepositoryChange(RepositoryChange change);

  mutable std::mutex mutex_;
  ProjectChangeBatch pending_;
  // pending_.file_changes by normalized path text: the merge is a lookup, not a
  // scan of every pending change (TD-2026-10-28-418).
  std::unordered_map<std::string, std::size_t> pending_index_;
  std::uint64_t generation_ = 0;
};

}  // namespace microide::project
