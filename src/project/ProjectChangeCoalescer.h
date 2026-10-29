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
  // The apply rate while batches keep arriving: a token bucket of kApplyBurst
  // applies, refilled one per kApplyIntervalMs. A lone save, and the first few
  // batches of anything, apply at once; a first remote sync or a build writing for
  // seconds applies (and so repaints the tree and the status bar) at most four
  // times a second instead of once per watcher batch -- 176 frames over a 16 s
  // sync on a slow link, before (TD-2026-10-28-419).
  static constexpr std::size_t kApplyBurst = 4;
  static constexpr std::uint64_t kApplyIntervalMs = 250;

  void Reset();
  void Ingest(ProjectChangeBatch batch);
  // Everything pending, now, whatever the rate: the "answer synchronously" path
  // (an explicit refresh, force_check).
  std::optional<ProjectChangeBatch> ConsumeReady(std::uint64_t* out_generation = nullptr);
  // Everything pending if the apply budget allows one at `now_ms`, else nullopt
  // and the batch keeps merging; NextReadyDelayMs says when to ask again.
  std::optional<ProjectChangeBatch> ConsumeReadyAt(std::uint64_t now_ms);
  // nullopt when nothing is pending; 0 when a batch is ready now; else the wait
  // until the budget allows the pending one.
  std::optional<std::uint64_t> NextReadyDelayMs(std::uint64_t now_ms) const;

 private:
  bool HasPendingLocked() const;
  std::optional<ProjectChangeBatch> TakePendingLocked(std::uint64_t* out_generation);
  // The budget at `now_ms` without spending it, and the epoch it refills from.
  std::size_t BudgetAtLocked(std::uint64_t now_ms, std::uint64_t* epoch_ms) const;
  void MergeFileChange(ProjectFileChange change);
  void MergeRepositoryChange(RepositoryChange change);

  mutable std::mutex mutex_;
  ProjectChangeBatch pending_;
  // pending_.file_changes by normalized path text: the merge is a lookup, not a
  // scan of every pending change (TD-2026-10-28-418).
  std::unordered_map<std::string, std::size_t> pending_index_;
  std::uint64_t generation_ = 0;
  // The token bucket: `apply_budget_` tokens as of `budget_epoch_ms_`, plus one per
  // kApplyIntervalMs since. Starts full, so the epoch's value is moot until spent.
  std::size_t apply_budget_ = kApplyBurst;
  std::uint64_t budget_epoch_ms_ = 0;
};

}  // namespace microide::project
