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
  static constexpr std::size_t kApplyBurst = 4;
  static constexpr std::uint64_t kApplyIntervalMs = 250;

  void Reset();
  void Ingest(ProjectChangeBatch batch);
  std::optional<ProjectChangeBatch> ConsumeReady(std::uint64_t* out_generation = nullptr);
  std::optional<ProjectChangeBatch> ConsumeReadyAt(std::uint64_t now_ms);
  std::optional<std::uint64_t> NextReadyDelayMs(std::uint64_t now_ms) const;

 private:
  bool HasPendingLocked() const;
  bool MustReleaseLocked() const;
  std::optional<ProjectChangeBatch> TakePendingLocked(std::uint64_t* out_generation);
  std::size_t BudgetAtLocked(std::uint64_t now_ms, std::uint64_t* epoch_ms) const;
  void MergeFileChange(ProjectFileChange change);
  void MergeRepositoryChange(RepositoryChange change);

  mutable std::mutex mutex_;
  ProjectChangeBatch pending_;
  std::unordered_map<std::string, std::size_t> pending_index_;
  std::uint64_t generation_ = 0;
  std::size_t apply_budget_ = kApplyBurst;
  std::uint64_t budget_epoch_ms_ = 0;
};

}
