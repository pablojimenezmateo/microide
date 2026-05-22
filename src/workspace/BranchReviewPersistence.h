#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace microide::workspace {

struct PersistedBranchReviewHunkIdentity {
  std::filesystem::path path;
  int old_start = 0;
  int old_count = 0;
  int new_start = 0;
  int new_count = 0;
  std::uint64_t content_hash = 0;
};

struct PersistedBranchReviewFileEntry {
  std::filesystem::path path;
  std::uint64_t reviewed_snapshot_generation = 0;
  std::uint64_t reviewed_at_unix_ms = 0;
};

struct PersistedBranchReviewHunkEntry {
  PersistedBranchReviewHunkIdentity identity;
  std::uint64_t reviewed_at_unix_ms = 0;
};

struct PersistedBranchReviewNote {
  std::string scope;
  std::filesystem::path path;
  std::optional<PersistedBranchReviewHunkIdentity> hunk_identity;
  std::string text;
  std::uint64_t updated_at_unix_ms = 0;
};

struct PersistedBranchReviewTarget {
  std::filesystem::path repository_root;
  std::string base_commit;
  std::string head_commit;
  std::string merge_base_commit;
  std::uint64_t snapshot_generation = 0;
  std::uint64_t last_accessed_unix_ms = 0;
  std::vector<PersistedBranchReviewFileEntry> reviewed_files;
  std::vector<PersistedBranchReviewHunkEntry> reviewed_hunks;
  std::vector<PersistedBranchReviewNote> notes;
};

struct PersistedBranchReviewState {
  std::vector<PersistedBranchReviewTarget> targets;
};

}  // namespace microide::workspace
