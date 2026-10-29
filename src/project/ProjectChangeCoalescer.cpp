#include "project/ProjectChangeCoalescer.h"

#include <algorithm>

#include "util/PathMatch.h"

namespace microide::project {
namespace {


// Cap on distinct pending file changes before the coalescer collapses to a single
// full-tree rescan. A file-change flood (a build writing thousands of files, a
// `git checkout` of a huge tree, or a deliberate flood) would otherwise grow the
// pending list without bound. A rescan supersedes the individual changes and
// re-reads the tree, so past this cap it is both cheaper and bounded. The first
// kMaxPendingFileChanges changes keep their per-file handling (open-buffer
// reloads, diagnostics clearing).
constexpr std::size_t kMaxPendingFileChanges = 1024;
// A batch the apply rate holds is released anyway once it holds this many changes:
// holding may delay a change, never cost it its itemization. Past
// kMaxPendingFileChanges a flood collapses to a rescan, and the language servers
// hear only the itemized changes (LspService::NotifyWatchedFileChanges).
constexpr std::size_t kReleaseHeldAtFileChanges = kMaxPendingFileChanges / 2;
// Past kMaxPendingFileChanges a change is still kept, for the language servers
// alone (ProjectChangeBatch::unitemized_file_changes), up to this many. What it
// costs is the servers' notification on the shell thread -- a path conversion
// and a glob match per change per watching server -- once per flood.
constexpr std::size_t kMaxUnitemizedFileChanges = 15 * 1024;

// One more event for a path already pending: a delete wins, a modify of a
// deleted path is a re-creation, and anything else takes the new kind.
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

}  // namespace

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
    return TakePendingLocked(nullptr);  // released, not paid for: the budget stays spent
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
  // A clock that went backwards (or a first call) refills nothing and loses nothing.
  const std::uint64_t elapsed = now_ms > budget_epoch_ms_ ? now_ms - budget_epoch_ms_ : 0;
  const std::uint64_t refilled = elapsed / kApplyIntervalMs;
  if (apply_budget_ + refilled >= kApplyBurst) {
    // Full: the refill clock restarts when the next token is spent.
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
  // Looked up by its normalized text. This was a scan of every pending change
  // with a path comparison each -- up to 1024 per incoming change, under the lock
  // the shell thread's ConsumeReady waits on: a first remote sync's batches kept
  // the watcher thread busy 0.4 s and stalled a frame for 138 ms
  // (TD-2026-10-28-418). Watcher paths are already normal in practice, so the key
  // is usually the path's own text. An index past kMaxPendingFileChanges names a
  // slot in `unitemized_file_changes`.
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
  // Past the cap the flood collapses to a full-tree rescan, which bounds what is
  // acted on per path: the first kMaxPendingFileChanges keep per-file handling,
  // and `file_changes_truncated` makes the apply re-check every open buffer for
  // the rest. A rescan alone did not cover them: the apply re-checked buffers
  // only for a rescan with NO itemized change, which a flood never is, so an open
  // file past the cap kept its old text. The language servers still hear each
  // change, up to kMaxUnitemizedFileChanges more; past that, nothing is kept.
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

}  // namespace microide::project
