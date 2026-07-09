#include "compare/BranchReviewStateTypes.h"

#include <chrono>
#include <sstream>

namespace microide::compare {

namespace {

std::uint64_t HashString(std::string_view text) {
  return std::hash<std::string_view>{}(text);
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
  std::ostringstream stream;
  for (int row = hunk.start_row; row <= hunk.end_row; ++row) {
    if (row < 0 || static_cast<std::size_t>(row) >= model.rows.size()) {
      continue;
    }
    const CompareRow& compare_row = model.rows[static_cast<std::size_t>(row)];
    stream << static_cast<int>(compare_row.kind) << '\n';
    // Length-prefix each field so left/right can't merge into an ambiguous byte
    // stream: ("hello","world") and ("hell","oworld") must hash differently.
    stream << compare_row.left_text.size() << ':' << compare_row.left_text << '\n';
    stream << compare_row.right_text.size() << ':' << compare_row.right_text << '\n';
  }
  return HashString(stream.str());
}

}  // namespace

bool BranchReviewHunkIdentity::operator==(const BranchReviewHunkIdentity& other) const {
  return path == other.path && old_start == other.old_start && old_count == other.old_count &&
         new_start == other.new_start && new_count == other.new_count &&
         content_hash == other.content_hash;
}

std::string BranchReviewTargetKey(const BranchReviewTargetIdentity& target) {
  std::ostringstream stream;
  stream << target.repository_root.generic_string() << '\n' << target.base_commit << '\n'
         << target.head_commit << '\n' << target.merge_base_commit << '\n'
         << target.snapshot_generation;
  return stream.str();
}

std::string BranchReviewMarkerLabel(const BranchReviewMarkerStatus status) {
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

BranchReviewHunkIdentity ComputeBranchReviewHunkIdentity(const CompareModel& model,
                                                         const CompareHunk& hunk,
                                                         const std::filesystem::path& path) {
  BranchReviewHunkIdentity identity{
      .path = path,
  };
  AccumulateHunkLineRanges(model, hunk, &identity.old_start, &identity.old_count, &identity.new_start,
                           &identity.new_count);
  identity.content_hash = HashHunkContent(model, hunk);
  return identity;
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
