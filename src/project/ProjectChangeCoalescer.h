#pragma once

#include <cstdint>
#include <mutex>
#include <optional>

#include "project/ProjectChangeTypes.h"

namespace microide::project {

class ProjectChangeCoalescer {
 public:
  void Reset();
  void Ingest(ProjectChangeBatch batch);
  std::optional<ProjectChangeBatch> ConsumeReady(std::uint64_t* out_generation = nullptr);
  std::uint64_t CurrentGeneration() const;

 private:
  void MergeFileChange(ProjectFileChange change);
  void MergeRepositoryChange(RepositoryChange change);

  mutable std::mutex mutex_;
  ProjectChangeBatch pending_;
  std::uint64_t generation_ = 0;
};

}  // namespace microide::project
