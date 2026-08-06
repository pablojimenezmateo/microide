#include "compare/BranchReviewStateTypes.h"

#include <cassert>
#include <chrono>

namespace microide::compare {

namespace {

// FNV-1a, accumulated in place. This used to serialize the whole hunk into a
// std::ostringstream and hash the resulting string — several allocations plus a
// full copy of the hunk's text, per hunk, on a path that resolves every hunk of a
// file on the compare tab's derived-state refresh.
constexpr std::uint64_t kFnvOffsetBasis = 1469598103934665603ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

void HashBytes(std::uint64_t* hash, std::string_view bytes) {
  for (const char byte : bytes) {
    *hash ^= static_cast<std::uint64_t>(static_cast<unsigned char>(byte));
    *hash *= kFnvPrime;
  }
}

void HashInteger(std::uint64_t* hash, std::uint64_t value) {
  for (int shift = 0; shift < 64; shift += 8) {
    *hash ^= (value >> shift) & 0xFFULL;
    *hash *= kFnvPrime;
  }
}

void AccumulateHunkLineRanges(const CompareModel& model,
                              const CompareHunk& hunk,
                              int* old_start,
                              int* old_count,
                              int* new_start,
                              int* new_count) {
  int min_old = 0;
  int max_old = 0;
  int min_new = 0;
  int max_new = 0;
  bool have_old = false;
  bool have_new = false;
  for (int row = hunk.start_row; row <= hunk.end_row; ++row) {
    if (row < 0 || static_cast<std::size_t>(row) >= model.rows.size()) {
      continue;
    }
    const CompareRow& compare_row = model.rows[static_cast<std::size_t>(row)];
    if (compare_row.left_line > 0) {
      if (!have_old) {
        min_old = max_old = compare_row.left_line;
        have_old = true;
      } else {
        min_old = std::min(min_old, compare_row.left_line);
        max_old = std::max(max_old, compare_row.left_line);
      }
    }
    if (compare_row.right_line > 0) {
      if (!have_new) {
        min_new = max_new = compare_row.right_line;
        have_new = true;
      } else {
        min_new = std::min(min_new, compare_row.right_line);
        max_new = std::max(max_new, compare_row.right_line);
      }
    }
  }
  if (have_old) {
    *old_start = min_old;
    *old_count = max_old - min_old + 1;
  }
  if (have_new) {
    *new_start = min_new;
    *new_count = max_new - min_new + 1;
  }
}

std::uint64_t HashHunkContent(const CompareModel& model, const CompareHunk& hunk) {
  std::uint64_t hash = kFnvOffsetBasis;
  for (int row = hunk.start_row; row <= hunk.end_row; ++row) {
    if (row < 0 || static_cast<std::size_t>(row) >= model.rows.size()) {
      continue;
    }
    const CompareRow& compare_row = model.rows[static_cast<std::size_t>(row)];
    HashInteger(&hash, static_cast<std::uint64_t>(static_cast<int>(compare_row.kind)));
    // Length-prefix each field so left/right can't merge into an ambiguous byte
    // stream: ("hello","world") and ("hell","oworld") must hash differently.
    HashInteger(&hash, compare_row.left_text.size());
    HashBytes(&hash, compare_row.left_text);
    HashInteger(&hash, compare_row.right_text.size());
    HashBytes(&hash, compare_row.right_text);
  }
  return hash;
}

}  // namespace

std::filesystem::path NormalizeReviewPath(const std::filesystem::path& path) {
  return path.lexically_normal();
}

#ifndef NDEBUG
void AssertNormalizedReviewPath(const std::filesystem::path& path) {
  assert(path.native() == path.lexically_normal().native() &&
         "branch-review paths must be normalized on ingress");
  (void)path;
}
#endif

bool BranchReviewHunkIdentity::operator==(const BranchReviewHunkIdentity& other) const {
  return path == other.path && old_start == other.old_start && old_count == other.old_count &&
         new_start == other.new_start && new_count == other.new_count &&
         content_hash == other.content_hash;
}

std::string_view BranchReviewMarkerLabel(const BranchReviewMarkerStatus status) {
  switch (status) {
    case BranchReviewMarkerStatus::Unreviewed:
      return {};
    case BranchReviewMarkerStatus::Reviewed:
      return "reviewed";
    case BranchReviewMarkerStatus::ChangedSinceReviewed:
      return "changed";
  }
  return {};
}

BranchReviewHunkContentKey ComputeBranchReviewHunkContentKey(const CompareModel& model,
                                                             const CompareHunk& hunk) {
  BranchReviewHunkContentKey key;
  AccumulateHunkLineRanges(model, hunk, &key.old_start, &key.old_count, &key.new_start,
                           &key.new_count);
  key.content_hash = HashHunkContent(model, hunk);
  return key;
}

BranchReviewHunkContentKey ComputeBranchReviewHunkContentKey(const CompareModel& model,
                                                             const int hunk_index) {
  if (hunk_index < 0 || static_cast<std::size_t>(hunk_index) >= model.hunks.size()) {
    return BranchReviewHunkContentKey{};
  }
  return ComputeBranchReviewHunkContentKey(model,
                                           model.hunks[static_cast<std::size_t>(hunk_index)]);
}

BranchReviewHunkIdentity ComputeBranchReviewHunkIdentity(const CompareModel& model,
                                                         const CompareHunk& hunk,
                                                         const std::filesystem::path& path) {
  const BranchReviewHunkContentKey key = ComputeBranchReviewHunkContentKey(model, hunk);
  return BranchReviewHunkIdentity{
      .path = path,
      .old_start = key.old_start,
      .old_count = key.old_count,
      .new_start = key.new_start,
      .new_count = key.new_count,
      .content_hash = key.content_hash,
  };
}

BranchReviewHunkIdentity ComputeBranchReviewHunkIdentity(const CompareModel& model,
                                                         const int hunk_index,
                                                         const std::filesystem::path& path) {
  if (hunk_index < 0 || static_cast<std::size_t>(hunk_index) >= model.hunks.size()) {
    return BranchReviewHunkIdentity{.path = path};
  }
  return ComputeBranchReviewHunkIdentity(model, model.hunks[static_cast<std::size_t>(hunk_index)],
                                         path);
}

}  // namespace microide::compare
