#include "platform/FileWatcher.h"

#include "platform/DescriptorRetire.h"
#include "util/PathMatch.h"
#include "util/PerformanceTrace.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <thread>

#include <fcntl.h>
#include <poll.h>
#include <sys/inotify.h>
#include <unistd.h>

#include "util/PosixFd.h"
#include "util/PosixPipe.h"

namespace microide::platform {

namespace {

using util::CloseIfValid;

using microide::util::MakeCloexecPipe;

std::vector<std::filesystem::path> CollectRecursiveWatchPaths(
    const std::vector<std::filesystem::path>& roots,
    const TreeTraversalFilter& filter,
    std::size_t max_entries,
    bool* polling_required,
    bool* truncated) {
  std::vector<std::filesystem::path> watch_paths;
  bool requires_polling = false;
  bool budget_exhausted = false;
  std::size_t visited = 0;

  for (const auto& root : roots) {
    if (budget_exhausted) {
      break;
    }
    if (root.empty()) {
      continue;
    }

    const PathType root_type = ReadPathType(root);
    if (root_type == PathType::Missing || root_type == PathType::Other) {
      requires_polling = true;
      continue;
    }

    watch_paths.push_back(root);
    if (root_type != PathType::Directory) {
      continue;
    }

    std::error_code error;
    constexpr auto options = std::filesystem::directory_options::skip_permission_denied;
    for (std::filesystem::recursive_directory_iterator it(root, options, error), end;
         !error && it != end; it.increment(error)) {
      if (max_entries != 0 && ++visited > max_entries) {
        budget_exhausted = true;
        break;
      }
      std::error_code status_error;
      const PathType type = EntryPathType(*it, status_error);
      if (status_error) {
        continue;
      }
      // Guarded normalization: a directory iterator hands back an already-normal
      // path, and `lexically_normal()` is ~12 allocations even when it changes
      // nothing (TD-2026-08-10-174). This ran unguarded on every entry.
      std::filesystem::path path_scratch;
      const std::filesystem::path& path = util::NormalizedPathView(it->path(), path_scratch);
      if (filter && !filter(path, type)) {
        if (type == PathType::Directory) {
          it.disable_recursion_pending();
        }
        continue;
      }
      if (type == PathType::Directory) {
        watch_paths.push_back(path);
      }
    }
    if (error) {
      requires_polling = true;
    }
  }

  // A tree we could not finish enumerating cannot be watched reliably: report
  // truncation and force the polling fallback for whatever was collected.
  if (budget_exhausted) {
    requires_polling = true;
  }

  std::sort(watch_paths.begin(), watch_paths.end());
  watch_paths.erase(std::unique(watch_paths.begin(), watch_paths.end()), watch_paths.end());
  if (polling_required != nullptr) {
    *polling_required = requires_polling;
  }
  if (truncated != nullptr) {
    *truncated = budget_exhausted;
  }
  return watch_paths;
}

constexpr std::uint32_t kLinuxWatchMask = IN_ATTRIB | IN_CLOSE_WRITE | IN_CREATE | IN_DELETE |
                                          IN_DELETE_SELF | IN_MOVE_SELF | IN_MOVED_FROM |
                                          IN_MOVED_TO;

}  // namespace

struct FileTreeWatcher::NativeBackend {
  explicit NativeBackend(std::function<void()> on_change)
      : on_change_(std::move(on_change)) {}

  ~NativeBackend() {
    {
      util::PerformanceTrace::Scope scope("watch::TreeWatcher::Backend::JoinWorker");
      RequestStop();
      if (worker_.joinable()) {
        worker_.join();
      }
    }
    CloseIfValid(control_pipe_[0]);
    CloseIfValid(control_pipe_[1]);
    // Retired, not closed here: the worker is joined above, so nothing can touch
    // this descriptor again, and closing an inotify fd blocks for milliseconds at
    // random (see RetireDescriptorAsync). This destructor runs on the shell thread
    // on every project switch.
    RetireDescriptorAsync(inotify_fd_);
    inotify_fd_ = -1;
  }

  NativeBackend(const NativeBackend&) = delete;
  NativeBackend& operator=(const NativeBackend&) = delete;

  bool Start(const std::vector<std::filesystem::path>& watch_paths) {
    // IN_NONBLOCK is essential: the drain loop below re-reads whenever a read
    // exactly fills the 4096-byte buffer, and without it that trailing read blocks
    // indefinitely once the queue empties — stranding the worker in read() where
    // the control-pipe wake in RequestStop() can never reach it, hanging shutdown's
    // worker join. (Matches FileIndexWatcher, which already sets IN_NONBLOCK.)
    inotify_fd_ = inotify_init1(IN_CLOEXEC | IN_NONBLOCK);
    if (inotify_fd_ < 0) {
      return false;
    }

    if (!MakeCloexecPipe(control_pipe_)) {
      CloseIfValid(inotify_fd_);
      inotify_fd_ = -1;
      return false;
    }

    for (const auto& path : watch_paths) {
      if (inotify_add_watch(inotify_fd_, path.c_str(), kLinuxWatchMask) < 0) {
        return false;
      }
    }

    worker_ = std::thread([this]() { WorkerMain(); });
    return true;
  }

  void RequestStop() {
    if (control_pipe_[1] < 0) {
      return;
    }

    const std::array<char, 1> byte{{0}};
    while (write(control_pipe_[1], byte.data(), byte.size()) < 0) {
      if (errno != EINTR) {
        break;
      }
    }
  }

 private:
  void WorkerMain() {
    std::array<pollfd, 2> poll_fds{
        pollfd{.fd = inotify_fd_, .events = POLLIN, .revents = 0},
        pollfd{.fd = control_pipe_[0], .events = POLLIN, .revents = 0},
    };

    while (true) {
      const int poll_result = poll(poll_fds.data(), poll_fds.size(), -1);
      if (poll_result < 0) {
        if (errno == EINTR) {
          continue;
        }
        return;
      }

      if ((poll_fds[1].revents & (POLLIN | POLLHUP)) != 0) {
        return;
      }
      if ((poll_fds[0].revents & (POLLIN | POLLHUP)) == 0) {
        continue;
      }

      std::array<char, 4096> buffer{};
      bool changed = false;
      while (true) {
        const ssize_t bytes_read = read(inotify_fd_, buffer.data(), buffer.size());
        if (bytes_read > 0) {
          changed = true;
          if (bytes_read < static_cast<ssize_t>(buffer.size())) {
            break;
          }
          continue;
        }
        if (bytes_read < 0 && errno == EINTR) {
          continue;
        }
        break;
      }

      if (changed && on_change_) {
        on_change_();
      }
    }
  }

  std::function<void()> on_change_;
  int inotify_fd_ = -1;
  int control_pipe_[2] = {-1, -1};
  std::thread worker_;
};

FileTreeWatcher::FileTreeWatcher(std::chrono::milliseconds poll_interval)
    : poll_interval_(std::max(poll_interval, std::chrono::milliseconds::zero())) {}

FileTreeWatcher::~FileTreeWatcher() = default;

void FileTreeWatcher::SetPollInterval(std::chrono::milliseconds poll_interval) {
  std::scoped_lock lock(mutex_);
  poll_interval_ = std::max(poll_interval, std::chrono::milliseconds::zero());
  ResetNextPollAt();
}

void FileTreeWatcher::SetWakeCallback(WakeCallback callback) {
  std::scoped_lock lock(mutex_);
  wake_callback_ = std::move(callback);
}

void FileTreeWatcher::SetEntryBudget(std::size_t max_entries) {
  entry_budget_.store(max_entries, std::memory_order_relaxed);
}

bool FileTreeWatcher::TreeTooLarge() const {
  std::scoped_lock lock(mutex_);
  return tree_too_large_;
}

void FileTreeWatcher::SetDeferInitialSnapshot(bool defer) {
  std::scoped_lock lock(mutex_);
  defer_initial_snapshot_ = defer;
}

void FileTreeWatcher::SetEntryFilter(TreeTraversalFilter filter) {
  std::scoped_lock lock(mutex_);
  entry_filter_ = std::move(filter);
}

void FileTreeWatcher::SetRoots(std::vector<std::filesystem::path> roots) {
  std::vector<std::filesystem::path> normalized_roots;
  normalized_roots.reserve(roots.size());
  for (auto& root : roots) {
    if (!root.empty()) {
      normalized_roots.push_back(root.lexically_normal());
    }
  }
  std::sort(normalized_roots.begin(), normalized_roots.end());
  normalized_roots.erase(std::unique(normalized_roots.begin(), normalized_roots.end()),
                         normalized_roots.end());

  TreeTraversalFilter filter;
  bool defer_initial_snapshot = false;
  bool has_wake_callback = false;
  {
    std::scoped_lock lock(mutex_);
    filter = entry_filter_;
    roots_ = normalized_roots;
    pending_change_ = false;
    defer_initial_snapshot = defer_initial_snapshot_;
    has_wake_callback = static_cast<bool>(wake_callback_);
  }

  // Build the native backend and (optionally) the snapshot WITHOUT holding the
  // lock so a peer SetRoots/Clear/Poll never blocks on this tree walk.
  PreparedNativeBackend prepared = PrepareNativeBackend(normalized_roots, filter);
  bool capture_snapshot =
      !defer_initial_snapshot || prepared.polling_required || !has_wake_callback;
  std::vector<TreeSnapshotEntry> snapshot;
  if (capture_snapshot) {
    bool snapshot_truncated = false;
    snapshot = CaptureTreeSnapshot(normalized_roots, filter,
                                   entry_budget_.load(std::memory_order_relaxed),
                                   &snapshot_truncated);
    if (snapshot_truncated) {
      prepared.tree_too_large = true;
    }
  }
  if (prepared.tree_too_large) {
    // A truncated snapshot can't be diffed reliably; don't retain it.
    capture_snapshot = false;
  }

  std::unique_ptr<NativeBackend> old_backend;
  {
    std::scoped_lock lock(mutex_);
    if (roots_ != normalized_roots) {
      return;  // superseded while unlocked; discard prepared backend + snapshot
    }
    old_backend = InstallPreparedBackendLocked(std::move(prepared));
    if (capture_snapshot) {
      snapshot_ = std::make_shared<const std::vector<TreeSnapshotEntry>>(std::move(snapshot));
    } else {
      snapshot_.reset();
    }
    pending_change_ = false;
    ResetNextPollAt();
  }
}

void FileTreeWatcher::Clear() {
  std::unique_ptr<NativeBackend> old_backend;
  {
    util::PerformanceTrace::Scope scope("watch::TreeWatcher::Clear::AcquireLock");
    mutex_.lock();
  }
  {
    std::lock_guard<std::mutex> lock(mutex_, std::adopt_lock);
    roots_.clear();
    snapshot_.reset();
    pending_change_ = false;
    polling_required_ = true;
    tree_too_large_ = false;
    old_backend = std::move(native_backend_);
    next_poll_at_ = std::chrono::steady_clock::time_point::min();
  }
  // Destroyed outside the lock (it joins the backend's worker thread), and scoped
  // so a switch stall can be attributed to the join rather than to the lock.
  util::PerformanceTrace::Scope scope("watch::TreeWatcher::Clear::DestroyBackend");
  old_backend.reset();
}

std::optional<std::chrono::milliseconds> FileTreeWatcher::NextPollDelay() const {
  std::scoped_lock lock(mutex_);
  if (roots_.empty()) {
    return std::nullopt;
  }
  if (tree_too_large_) {
    // Native watching is off and the tree is unaffordable to poll: never schedule
    // periodic full-tree walks. Changes are picked up only via explicit refresh.
    return std::nullopt;
  }
  if (pending_change_) {
    if (wake_callback_ && !polling_required_) {
      return std::nullopt;
    }
    return std::chrono::milliseconds::zero();
  }
  if (poll_interval_ == std::chrono::milliseconds::zero()) {
    return std::chrono::milliseconds::zero();
  }
  if (!polling_required_) {
    return std::nullopt;
  }

  const auto now = std::chrono::steady_clock::now();
  if (now >= next_poll_at_) {
    return std::chrono::milliseconds::zero();
  }
  return std::chrono::duration_cast<std::chrono::milliseconds>(next_poll_at_ - now);
}

bool FileTreeWatcher::Poll() {
  std::vector<std::filesystem::path> roots;
  std::shared_ptr<const std::vector<TreeSnapshotEntry>> previous_snapshot;
  TreeTraversalFilter filter;
  bool polling_required = true;
  bool pending_change = false;
  {
    std::scoped_lock lock(mutex_);
    if (roots_.empty()) {
      return false;
    }
    roots = roots_;
    previous_snapshot = snapshot_;  // O(1) ref bump; the pointee is immutable
    filter = entry_filter_;
    polling_required = polling_required_;
    pending_change = pending_change_;
    pending_change_ = false;
  }
  const bool snapshot_valid = previous_snapshot != nullptr;

  const std::size_t entry_budget = entry_budget_.load(std::memory_order_relaxed);

  if (!snapshot_valid && !polling_required) {
    if (!pending_change) {
      return false;
    }
    bool snapshot_truncated = false;
    std::vector<TreeSnapshotEntry> current_snapshot =
        CaptureTreeSnapshot(roots, filter, entry_budget, &snapshot_truncated);
    PreparedNativeBackend prepared = PrepareNativeBackend(roots, filter);
    if (snapshot_truncated) {
      prepared.tree_too_large = true;
    }
    const bool too_large = prepared.tree_too_large;
    std::unique_ptr<NativeBackend> old_backend;
    std::scoped_lock lock(mutex_);
    if (roots != roots_) {
      return false;
    }
    if (too_large) {
      snapshot_.reset();
    } else {
      snapshot_ =
          std::make_shared<const std::vector<TreeSnapshotEntry>>(std::move(current_snapshot));
    }
    old_backend = InstallPreparedBackendLocked(std::move(prepared));
    ResetNextPollAt();
    return false;
  }

  bool snapshot_truncated = false;
  std::vector<TreeSnapshotEntry> current_snapshot =
      CaptureTreeSnapshot(roots, filter, entry_budget, &snapshot_truncated);

  PreparedNativeBackend prepared = PrepareNativeBackend(roots, filter);
  if (snapshot_truncated) {
    prepared.tree_too_large = true;
  }
  const bool too_large = prepared.tree_too_large;
  // A truncated snapshot is unreliable (recursive_directory_iterator order is not
  // stable), so never report a change off a partial walk. The compare runs
  // unlocked against the shared immutable previous snapshot (an invalid previous
  // snapshot compares as empty, matching the by-value member's semantics).
  static const std::vector<TreeSnapshotEntry> kNoSnapshot;
  const std::vector<TreeSnapshotEntry>& previous =
      previous_snapshot != nullptr ? *previous_snapshot : kNoSnapshot;
  const bool changed = !too_large && current_snapshot != previous;

  std::unique_ptr<NativeBackend> old_backend;
  std::scoped_lock lock(mutex_);
  if (roots != roots_) {
    // Roots were superseded while this poll scanned the filesystem unlocked. The
    // computed `changed` flag describes the *old* root set, so reporting it would
    // leak a stale change into the new root generation (a spurious project refresh
    // after a project switch). Discard it exactly like the early superseded branch.
    return false;
  }
  if (too_large) {
    snapshot_.reset();
  } else {
    snapshot_ =
        std::make_shared<const std::vector<TreeSnapshotEntry>>(std::move(current_snapshot));
  }
  old_backend = InstallPreparedBackendLocked(std::move(prepared));
  ResetNextPollAt();
  return changed;
}

FileTreeWatcher::PreparedNativeBackend FileTreeWatcher::PrepareNativeBackend(
    const std::vector<std::filesystem::path>& roots, const TreeTraversalFilter& filter) {
  // Runs WITHOUT mutex_ held: the recursive walk below dominates wall-clock on
  // large trees and must not block concurrent SetRoots/Clear/Poll callers.
  PreparedNativeBackend prepared;

  bool requires_polling = false;
  bool truncated = false;
  // The two halves of arming a native watch, separately named: the recursive
  // enumeration of directories to watch, and the inotify_add_watch storm that
  // registers them. Together they are the second full tree walk a project open
  // pays (the file index's initial scan is the first), and until these scopes
  // existed the only number for it was the caller's total.
  std::vector<std::filesystem::path> watch_paths;
  {
    util::PerformanceTrace::Scope collect_scope("watch::PrepareNativeBackend::CollectWatchPaths");
    watch_paths = CollectRecursiveWatchPaths(roots, filter,
                                             entry_budget_.load(std::memory_order_relaxed),
                                             &requires_polling, &truncated);
  }
  if (truncated) {
    // Tree too large to enumerate within budget: do not watch a partial set, and
    // do not poll-walk the whole tree on a timer. Degrade to "too large" mode.
    prepared.tree_too_large = true;
    prepared.polling_required = false;
    return prepared;
  }
  if (watch_paths.empty()) {
    prepared.polling_required = requires_polling || !roots.empty();
    return prepared;
  }

  auto backend = std::make_unique<NativeBackend>([this]() { NotifyWake(); });
  {
    util::PerformanceTrace::ScopeLabel start_label(
        "watch::PrepareNativeBackend::StartNativeBackend");
    start_label.Field("dirs", static_cast<long long>(watch_paths.size()));
    util::PerformanceTrace::Scope start_scope(start_label.View());
    if (!backend->Start(watch_paths)) {
      prepared.polling_required = true;
      return prepared;
    }
  }

  prepared.backend = std::move(backend);
  prepared.polling_required = requires_polling;
  return prepared;
}

std::unique_ptr<FileTreeWatcher::NativeBackend> FileTreeWatcher::InstallPreparedBackendLocked(
    PreparedNativeBackend prepared) {
  std::unique_ptr<NativeBackend> old_backend = std::move(native_backend_);
  native_backend_ = std::move(prepared.backend);
  polling_required_ = prepared.polling_required;
  tree_too_large_ = prepared.tree_too_large;
  return old_backend;
}

void FileTreeWatcher::ResetNextPollAt() {
  next_poll_at_ = std::chrono::steady_clock::now() + poll_interval_;
}

void FileTreeWatcher::NotifyWake() {
  WakeCallback callback;
  {
    std::scoped_lock lock(mutex_);
    if (pending_change_) {
      return;
    }
    pending_change_ = true;
    callback = wake_callback_;
  }
  if (callback) {
    callback();
  }
}

}  // namespace microide::platform
