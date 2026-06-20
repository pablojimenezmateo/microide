#pragma once

#include <cstddef>
#include <filesystem>
#include <span>
#include <vector>

namespace microide::workspace {

// One already-open review tab (compare or merge) that belongs to the current
// review session's ref-scope. `index` is its position in `open_tabs`; `dirty`
// marks unsaved edits (merge result / editable working-tree side) that must
// never be auto-closed.
struct ReviewTabRef {
  std::filesystem::path path;  // normalized absolute path
  std::size_t index = 0;
  bool dirty = false;
};

// The result of reconciling the desired target file set against the review tabs
// already open for this session.
struct ReviewTabPlan {
  std::vector<std::filesystem::path> to_open;     // targets not already open
  std::vector<std::size_t> to_close;              // stale clean tabs, descending index
  std::vector<std::filesystem::path> reused;      // targets already open, kept
  std::vector<std::filesystem::path> kept_dirty;  // stale-but-dirty, kept (not closed)
};

// Pure reconciliation: no IO.
//
// `existing` must already be scoped to THIS review session (matching kind +
// refs); the caller is responsible for that filtering so the planner stays a
// trivial set-difference. `targets` is the desired set of file paths (order
// preserved in `to_open`/`reused`).
//
// - A target already present in `existing` -> `reused`; otherwise -> `to_open`.
// - An existing tab whose path is not a target is stale: closed (`to_close`)
//   unless dirty (`kept_dirty`).
// - `to_close` is emitted in descending index order so the caller can close
//   without invalidating earlier indices.
ReviewTabPlan ComputeReviewTabPlan(std::span<const ReviewTabRef> existing,
                                   std::span<const std::filesystem::path> targets);

}  // namespace microide::workspace
