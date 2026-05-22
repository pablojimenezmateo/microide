#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "compare/CompareModel.h"
#include "compare/CompareReviewTypes.h"

namespace microide::compare {

enum class BranchReviewMarkerStatus {
  Unreviewed,
  Reviewed,
  ChangedSinceReviewed,
};

enum class BranchReviewNoteScope {
  File,
  Hunk,
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

std::string BranchReviewTargetKey(const BranchReviewTargetIdentity& target);
std::string BranchReviewMarkerLabel(BranchReviewMarkerStatus status);

BranchReviewHunkIdentity ComputeBranchReviewHunkIdentity(const CompareModel& model,
                                                         int hunk_index,
                                                         const std::filesystem::path& path);
BranchReviewHunkIdentity ComputeBranchReviewHunkIdentity(const CompareModel& model,
                                                         const CompareHunk& hunk,
                                                         const std::filesystem::path& path);

}  // namespace microide::compare
