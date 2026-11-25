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

std::filesystem::path NormalizeReviewPath(const std::filesystem::path& repository_root,
                                          const std::filesystem::path& path);

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

struct BranchReviewHunkMarker {
  BranchReviewMarkerStatus status = BranchReviewMarkerStatus::Unreviewed;
  bool has_note = false;
  std::string note_text;
  bool note_outdated = false;
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
  std::uint64_t reviewed_diff_fingerprint = 0;
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

std::string_view BranchReviewMarkerLabel(BranchReviewMarkerStatus status);

struct BranchReviewHunkContentKey {
  int old_start = 0;
  int old_count = 0;
  int new_start = 0;
  int new_count = 0;
  std::uint64_t content_hash = 0;
};

BranchReviewHunkContentKey ComputeBranchReviewHunkContentKey(const CompareModel& model,
                                                             const CompareHunk& hunk);

std::uint64_t DiffContentFingerprint(const CompareModel& model);

BranchReviewHunkIdentity ComputeBranchReviewHunkIdentity(const CompareModel& model,
                                                         int hunk_index,
                                                         const std::filesystem::path& path);
BranchReviewHunkIdentity ComputeBranchReviewHunkIdentity(const CompareModel& model,
                                                         const CompareHunk& hunk,
                                                         const std::filesystem::path& path);

}
