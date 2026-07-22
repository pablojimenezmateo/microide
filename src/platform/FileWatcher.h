#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

#include "platform/Filesystem.h"

namespace microide::platform {

class FileTreeWatcher {
 public:
  using WakeCallback = std::function<void()>;

  explicit FileTreeWatcher(
      std::chrono::milliseconds poll_interval = std::chrono::milliseconds(750));
  ~FileTreeWatcher();

  FileTreeWatcher(const FileTreeWatcher&) = delete;
  FileTreeWatcher& operator=(const FileTreeWatcher&) = delete;

  void SetPollInterval(std::chrono::milliseconds poll_interval);
  void SetWakeCallback(WakeCallback callback);
  void SetDeferInitialSnapshot(bool defer);
  void SetEntryFilter(TreeTraversalFilter filter);
  void SetRoots(std::vector<std::filesystem::path> roots);
  void Clear();

  // Override the per-walk entry budget (defaults to kTreeTraversalEntryBudget).
  // Primarily a testing seam so fixtures can trip the "too large" path cheaply.
  void SetEntryBudget(std::size_t max_entries);

  std::optional<std::chrono::milliseconds> NextPollDelay() const;
  bool Poll();

  // True once a tree exceeded the entry budget: native watching is disabled and
  // periodic polling is suppressed (NextPollDelay returns nullopt). Reset by
  // SetRoots/Clear.
  bool TreeTooLarge() const;

  const std::vector<std::filesystem::path>& roots() const { return roots_; }

 private:
  struct NativeBackend;

  // Result of building a native backend for a root set. The recursive directory
  // walk required to enumerate watch targets is expensive on large trees, so it
  // is computed WITHOUT holding mutex_ (see PrepareNativeBackend) and only swapped
  // in under the lock (InstallPreparedBackendLocked). This keeps concurrent
  // SetRoots/Clear/Poll callers from blocking on a peer's tree walk.
  struct PreparedNativeBackend {
    std::unique_ptr<NativeBackend> backend;
    bool polling_required = true;
    bool tree_too_large = false;
  };

  PreparedNativeBackend PrepareNativeBackend(const std::vector<std::filesystem::path>& roots,
                                             const TreeTraversalFilter& filter);
  std::unique_ptr<NativeBackend> InstallPreparedBackendLocked(PreparedNativeBackend prepared);
  void ResetNextPollAt();
  void NotifyWake();

  mutable std::mutex mutex_;
  std::chrono::milliseconds poll_interval_;
  std::vector<std::filesystem::path> roots_;
  // Immutable shared snapshot (nullptr = no valid snapshot). Poll() grabs a
  // reference under the lock (O(1) ref bump), walks and compares unlocked, then
  // swaps the freshly captured snapshot in under the lock (O(1) pointer swap).
  // The previous by-value member forced two O(tree) deep copies per poll tick
  // (copy-out for the compare, copy-in to store) on top of the walk itself.
  std::shared_ptr<const std::vector<TreeSnapshotEntry>> snapshot_;
  TreeTraversalFilter entry_filter_;
  WakeCallback wake_callback_;
  std::unique_ptr<NativeBackend> native_backend_;
  bool defer_initial_snapshot_ = false;
  bool pending_change_ = false;
  bool polling_required_ = true;
  bool tree_too_large_ = false;
  std::atomic<std::size_t> entry_budget_{kTreeTraversalEntryBudget};
  std::chrono::steady_clock::time_point next_poll_at_ = std::chrono::steady_clock::time_point::min();
};

}  // namespace microide::platform
