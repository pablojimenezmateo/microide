#pragma once

#include <chrono>
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

  std::optional<std::chrono::milliseconds> NextPollDelay() const;
  bool Poll();

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
  };

  PreparedNativeBackend PrepareNativeBackend(const std::vector<std::filesystem::path>& roots,
                                             const TreeTraversalFilter& filter);
  std::unique_ptr<NativeBackend> InstallPreparedBackendLocked(PreparedNativeBackend prepared);
  void ResetNextPollAt();
  void NotifyWake();

  mutable std::mutex mutex_;
  std::chrono::milliseconds poll_interval_;
  std::vector<std::filesystem::path> roots_;
  std::vector<TreeSnapshotEntry> snapshot_;
  TreeTraversalFilter entry_filter_;
  WakeCallback wake_callback_;
  std::unique_ptr<NativeBackend> native_backend_;
  bool defer_initial_snapshot_ = false;
  bool snapshot_valid_ = false;
  bool pending_change_ = false;
  bool polling_required_ = true;
  std::chrono::steady_clock::time_point next_poll_at_ = std::chrono::steady_clock::time_point::min();
};

}  // namespace microide::platform
