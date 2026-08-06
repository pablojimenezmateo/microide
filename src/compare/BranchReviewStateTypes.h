#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "compare/CompareModel.h"
#include "compare/CompareReviewTypes.h"

namespace microide::compare {

// Every path stored inside a BranchReviewTargetState — file-entry paths, hunk
// identity paths, note paths — is normalized through this on the way in, by the
// service's mutators and by the persistence bridge. That invariant is what lets
// the query scans compare paths as plain strings instead of re-normalizing both
// sides of every comparison (~12 allocations a call, in loops that run per
// reviewed-hunk entry per hunk per row). A new ingress point must normalize here.
std::filesystem::path NormalizeReviewPath(const std::filesystem::path& path);

// Debug-only guard for the invariant above: a path that reaches a comparison
// un-normalized would silently compare unequal to its own stored form, which
// reads as "this hunk was never reviewed" rather than as a crash.
#ifdef NDEBUG
#define MICROIDE_ASSERT_NORMALIZED_REVIEW_PATH(path) ((void)0)
#else
#define MICROIDE_ASSERT_NORMALIZED_REVIEW_PATH(path) \
  ::microide::compare::AssertNormalizedReviewPath(path)
void AssertNormalizedReviewPath(const std::filesystem::path& path);
#endif

enum class BranchReviewMarkerStatus {
  Unreviewed,
  Reviewed,
  ChangedSinceReviewed,
};

enum class BranchReviewNoteScope {
  File,
  Hunk,
};

// One hunk's resolved review presentation, as produced for a whole file at once
// by BranchReviewStateService::ResolveHunkMarkers.
struct BranchReviewHunkMarker {
  BranchReviewMarkerStatus status = BranchReviewMarkerStatus::Unreviewed;
  bool has_note = false;
};

struct BranchReviewHunkIdentity {
  std::filesystem::path path;
  int old_start = 0;
  int old_count = 0;
  int new_start = 0;
  int new_count = 0;
  std::uint64_t content_hash = 0;

  bool operator==(const BranchReviewHunkIdentity& other) const;
};

struct BranchReviewFileReviewEntry {
  std::filesystem::path path;
  std::uint64_t reviewed_snapshot_generation = 0;
  std::uint64_t reviewed_at_unix_ms = 0;
};

struct BranchReviewHunkReviewEntry {
  BranchReviewHunkIdentity identity;
  std::uint64_t reviewed_at_unix_ms = 0;
};

struct BranchReviewNote {
  BranchReviewNoteScope scope = BranchReviewNoteScope::File;
  std::filesystem::path path;
  std::optional<BranchReviewHunkIdentity> hunk_identity;
  std::string text;
  std::uint64_t updated_at_unix_ms = 0;
};

struct BranchReviewTargetState {
  BranchReviewTargetIdentity target;
  std::vector<BranchReviewFileReviewEntry> reviewed_files;
  std::vector<BranchReviewHunkReviewEntry> reviewed_hunks;
  std::vector<BranchReviewNote> notes;
  std::uint64_t last_accessed_unix_ms = 0;
};

// Static label text; a view because it used to return a fresh std::string per
// hunk per marker pass.
std::string_view BranchReviewMarkerLabel(BranchReviewMarkerStatus status);

// The path-free half of a hunk identity — everything that is a function of the
// compare model alone. A whole-file resolve computes one per hunk and matches it
// against the stored entries whose path it has already checked once, instead of
// building a full identity (which copies the path) per hunk per entry.
struct BranchReviewHunkContentKey {
  int old_start = 0;
  int old_count = 0;
  int new_start = 0;
  int new_count = 0;
  std::uint64_t content_hash = 0;
};

BranchReviewHunkContentKey ComputeBranchReviewHunkContentKey(const CompareModel& model,
                                                             const CompareHunk& hunk);
BranchReviewHunkContentKey ComputeBranchReviewHunkContentKey(const CompareModel& model,
                                                             int hunk_index);

BranchReviewHunkIdentity ComputeBranchReviewHunkIdentity(const CompareModel& model,
                                                         int hunk_index,
                                                         const std::filesystem::path& path);
BranchReviewHunkIdentity ComputeBranchReviewHunkIdentity(const CompareModel& model,
                                                         const CompareHunk& hunk,
                                                         const std::filesystem::path& path);

}  // namespace microide::compare
