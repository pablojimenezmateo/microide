#include "project/ProjectChangeCoalescer.h"

#include <algorithm>

#include "util/PathMatch.h"

namespace microide::project {
namespace {


constexpr std::size_t kMaxPendingFileChanges = 1024;
constexpr std::size_t kReleaseHeldAtFileChanges = kMaxPendingFileChanges / 2;
constexpr std::size_t kMaxUnitemizedFileChanges = 15 * 1024;

void MergeIntoPendingChange(ProjectFileChange& existing, ProjectFileChange change) {
  if (change.kind == ProjectFileChangeKind::Deleted) {
    existing.kind = ProjectFileChangeKind::Deleted;
    return;
  }
  if (existing.kind == ProjectFileChangeKind::Deleted && change.kind == ProjectFileChangeKind::Modified) {
    existing.kind = ProjectFileChangeKind::Created;
    existing.absolute_path = std::move(change.absolute_path);
    return;
  }
  existing.kind = change.kind;
  existing.absolute_path = std::move(change.absolute_path);
}

}

void ProjectChangeCoalescer::Reset() {
  std::scoped_lock lock(mutex_);
  pending_ = ProjectChangeBatch{};
  pending_index_.clear();
  generation_ = 0;
  apply_budget_ = kApplyBurst;
  budget_epoch_ms_ = 0;
}

void ProjectChangeCoalescer::Ingest(ProjectChangeBatch batch) {
  if (batch.file_changes.empty() && batch.repository_changes.empty() &&
      !batch.tree_rescan_requested) {
    return;
  }

  std::scoped_lock lock(mutex_);
  pending_.tree_rescan_requested =
      pending_.tree_rescan_requested || batch.tree_rescan_requested;
  pending_.file_changes_truncated =
      pending_.file_changes_truncated || batch.file_changes_truncated;
  for (ProjectFileChange& change : batch.file_changes) {
    MergeFileChange(std::move(change));
  }
  for (RepositoryChange& change : batch.repository_changes) {
    MergeRepositoryChange(change);
  }
}

std::optional<ProjectChangeBatch> ProjectChangeCoalescer::ConsumeReady(
    std::uint64_t* out_generation) {
  std::scoped_lock lock(mutex_);
  return TakePendingLocked(out_generation);
}

std::optional<ProjectChangeBatch> ProjectChangeCoalescer::ConsumeReadyAt(std::uint64_t now_ms) {
  std::scoped_lock lock(mutex_);
  if (!HasPendingLocked()) {
    return std::nullopt;
  }
  std::uint64_t epoch_ms = 0;
  const std::size_t budget = BudgetAtLocked(now_ms, &epoch_ms);
  if (budget == 0) {
    if (!MustReleaseLocked()) {
      return std::nullopt;
    }
    return TakePendingLocked(nullptr);
  }
  apply_budget_ = budget - 1;
  budget_epoch_ms_ = epoch_ms;
  return TakePendingLocked(nullptr);
}

std::optional<std::uint64_t> ProjectChangeCoalescer::NextReadyDelayMs(std::uint64_t now_ms) const {
  std::scoped_lock lock(mutex_);
  if (!HasPendingLocked()) {
    return std::nullopt;
  }
  std::uint64_t epoch_ms = 0;
  if (BudgetAtLocked(now_ms, &epoch_ms) > 0 || MustReleaseLocked()) {
    return 0;
  }
  return kApplyIntervalMs - (now_ms - epoch_ms);
}

bool ProjectChangeCoalescer::MustReleaseLocked() const {
  return pending_.file_changes.size() >= kReleaseHeldAtFileChanges || pending_.file_changes_truncated;
}

std::size_t ProjectChangeCoalescer::BudgetAtLocked(std::uint64_t now_ms,
                                                   std::uint64_t* epoch_ms) const {
  const std::uint64_t elapsed = now_ms > budget_epoch_ms_ ? now_ms - budget_epoch_ms_ : 0;
  const std::uint64_t refilled = elapsed / kApplyIntervalMs;
  if (apply_budget_ + refilled >= kApplyBurst) {
    *epoch_ms = now_ms;
    return kApplyBurst;
  }
  *epoch_ms = budget_epoch_ms_ + refilled * kApplyIntervalMs;
  return apply_budget_ + static_cast<std::size_t>(refilled);
}

bool ProjectChangeCoalescer::HasPendingLocked() const {
  return !pending_.file_changes.empty() || !pending_.repository_changes.empty() ||
         pending_.tree_rescan_requested;
}

std::optional<ProjectChangeBatch> ProjectChangeCoalescer::TakePendingLocked(
    std::uint64_t* out_generation) {
  if (!HasPendingLocked()) {
    return std::nullopt;
  }

  ++generation_;
  ProjectChangeBatch ready = std::move(pending_);
  ready.generation = generation_;
  pending_ = ProjectChangeBatch{};
  pending_index_.clear();
  if (out_generation != nullptr) {
    *out_generation = generation_;
  }
  return ready;
}


void ProjectChangeCoalescer::MergeFileChange(ProjectFileChange change) {
  std::string key = util::PathTextNeedsNormalizing(change.relative_path.native())
                        ? change.relative_path.lexically_normal().native()
                        : change.relative_path.native();
  if (const auto found = pending_index_.find(key); found != pending_index_.end()) {
    MergeIntoPendingChange(found->second < kMaxPendingFileChanges
                               ? pending_.file_changes[found->second]
                               : pending_.unitemized_file_changes[found->second - kMaxPendingFileChanges],
                           std::move(change));
    return;
  }
  if (pending_.file_changes.size() < kMaxPendingFileChanges) {
    pending_index_.emplace(std::move(key), pending_.file_changes.size());
    pending_.file_changes.push_back(std::move(change));
    return;
  }
  pending_.tree_rescan_requested = true;
  pending_.file_changes_truncated = true;
  if (pending_.unitemized_file_changes.size() < kMaxUnitemizedFileChanges) {
    pending_index_.emplace(std::move(key),
                           kMaxPendingFileChanges + pending_.unitemized_file_changes.size());
    pending_.unitemized_file_changes.push_back(std::move(change));
  }
}

void ProjectChangeCoalescer::MergeRepositoryChange(RepositoryChange change) {
  auto existing = std::find_if(pending_.repository_changes.begin(),
                               pending_.repository_changes.end(),
                               [&](const RepositoryChange& candidate) {
                                 return candidate.kind == change.kind;
                               });
  if (existing == pending_.repository_changes.end()) {
    pending_.repository_changes.push_back(change);
  }
}

}
