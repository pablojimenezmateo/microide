#include "project/ProjectChangeCoalescer.h"

#include <algorithm>

#include "util/PathMatch.h"

namespace microide::project {
namespace {


// Cap on distinct pending file changes before the coalescer collapses to a single
// full-tree rescan. A file-change flood (a build writing thousands of files, a
// `git checkout` of a huge tree, or a deliberate flood) would otherwise grow the
// pending list without bound and make MergeFileChange O(N^2) — a linear scan with
// two path normalizations per comparison — on the watcher thread. A rescan
// supersedes the individual changes and re-reads the tree, so past this cap it is
// both cheaper and bounded. The first kMaxPendingFileChanges changes keep their
// per-file handling (open-buffer reloads, diagnostics clearing).
constexpr std::size_t kMaxPendingFileChanges = 1024;

}  // namespace

void ProjectChangeCoalescer::Reset() {
  std::scoped_lock lock(mutex_);
  pending_ = ProjectChangeBatch{};
  generation_ = 0;
}

void ProjectChangeCoalescer::Ingest(ProjectChangeBatch batch) {
  if (batch.file_changes.empty() && batch.repository_changes.empty() &&
      !batch.tree_rescan_requested) {
    return;
  }

  std::scoped_lock lock(mutex_);
  pending_.tree_rescan_requested =
      pending_.tree_rescan_requested || batch.tree_rescan_requested;
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
  if (pending_.file_changes.empty() && pending_.repository_changes.empty() &&
      !pending_.tree_rescan_requested) {
    return std::nullopt;
  }

  ++generation_;
  ProjectChangeBatch ready = std::move(pending_);
  ready.generation = generation_;
  pending_ = ProjectChangeBatch{};
  if (out_generation != nullptr) {
    *out_generation = generation_;
  }
  return ready;
}


void ProjectChangeCoalescer::MergeFileChange(ProjectFileChange change) {
  // Past the cap, collapse to a full-tree rescan and stop tracking individual
  // paths. This is the O(1) short-circuit that keeps a flood from turning the
  // scan below into O(N^2) on the watcher thread and from growing pending_
  // without bound. The rescan (already handled downstream) supersedes the
  // dropped changes; the first kMaxPendingFileChanges keep per-file handling.
  if (pending_.file_changes.size() >= kMaxPendingFileChanges) {
    pending_.tree_rescan_requested = true;
    return;
  }
  // Normalized ONCE, outside the scan. This comparison used to normalize both
  // sides per candidate -- ~24 allocations to answer "different file", up to 1024
  // times per change, on the watcher thread while a build writes files. Watcher
  // paths are already normal in practice, so `SameAsNormalizedPath` answers a
  // mismatch with a string compare and normalizes nothing at all.
  const std::filesystem::path normalized_relative =
      util::PathTextNeedsNormalizing(change.relative_path.native())
          ? change.relative_path.lexically_normal()
          : change.relative_path;
  auto existing = std::find_if(pending_.file_changes.begin(), pending_.file_changes.end(),
                               [&](const ProjectFileChange& candidate) {
                                 return util::SameAsNormalizedPath(candidate.relative_path,
                                                                   normalized_relative);
                               });
  if (existing == pending_.file_changes.end()) {
    pending_.file_changes.push_back(std::move(change));
    return;
  }

  if (change.kind == ProjectFileChangeKind::Deleted) {
    existing->kind = ProjectFileChangeKind::Deleted;
    return;
  }
  if (existing->kind == ProjectFileChangeKind::Deleted &&
      change.kind == ProjectFileChangeKind::Modified) {
    existing->kind = ProjectFileChangeKind::Created;
    existing->absolute_path = change.absolute_path;
    return;
  }
  existing->kind = change.kind;
  existing->absolute_path = change.absolute_path;
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
