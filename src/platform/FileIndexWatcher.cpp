#include "platform/FileIndexWatcher.h"

#include "util/DurableFile.h"

#include "platform/DescriptorRetire.h"
#include "platform/DirectoryWalk.h"
#include "platform/Filesystem.h"
#include "project/ProjectTraversalFilter.h"
#include "util/Log.h"
#include "util/PathMatch.h"
#include "util/PerformanceCounters.h"
#include "util/PerformanceTrace.h"
#include "util/StringUtil.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <map>
#include <set>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <sys/inotify.h>
#include <unistd.h>

#include "util/PosixFd.h"
#include "util/ThreadName.h"
#include "util/InvokeIfSet.h"

namespace microide::platform {

namespace {

void StampEntryMetadata(const std::filesystem::path& absolute_path, IndexFileEntry& entry) {
  if (const std::optional<FileMetadata> metadata = ReadFileMetadata(absolute_path)) {
    entry.mtime = metadata->mtime;
    entry.size = metadata->size;
    return;
  }
  entry.mtime = {};
  entry.size = 0;
}

using util::CloseIfValid;

bool IsGitMetadataRelativePath(const std::filesystem::path& relative_path) {
  const auto it = relative_path.begin();
  if (it == relative_path.end()) {
    return false;
  }
  const std::string_view first = it->native();
  return first == ".git";
}

bool IsEscapingRelativePath(const std::filesystem::path& relative_path) {
  if (relative_path.is_absolute()) {
    return true;
  }
  const auto it = relative_path.begin();
  return it != relative_path.end() && std::string_view(it->native()) == "..";
}

bool TryComputeRelativePath(const std::filesystem::path& absolute_path,
                            const std::filesystem::path& root,
                            std::filesystem::path& relative_path) {
  std::filesystem::path path_storage;
  const std::filesystem::path* normalized_path_ptr = &absolute_path;
  if (util::PathTextNeedsNormalizing(absolute_path.native())) {
    path_storage = absolute_path.lexically_normal();
    normalized_path_ptr = &path_storage;
  }
  std::filesystem::path root_storage;
  const std::filesystem::path* normalized_root_ptr = &root;
  if (util::PathTextNeedsNormalizing(root.native())) {
    root_storage = root.lexically_normal();
    normalized_root_ptr = &root_storage;
  }
  const std::filesystem::path& normalized_path = *normalized_path_ptr;
  const std::filesystem::path& normalized_root = *normalized_root_ptr;

  if (util::NormalizedPathEqualsOrWithin(normalized_path, normalized_root)) {
    const std::string_view relative_text =
        util::NormalizedRelativeView(normalized_path.native(), normalized_root.native());
    if (!relative_text.empty()) {
      relative_path.assign(relative_text);
      return true;
    }
  }
  std::filesystem::path relative = normalized_path.lexically_relative(normalized_root);
  if (!relative.empty()) {
    relative_path = std::move(relative);
    return true;
  }
  return false;
}

bool ShouldSkipWatchedDirectory(const std::filesystem::path& directory,
                                project::ProjectTraversalFilter* filter) {
  return filter != nullptr && filter->ShouldSkipDirectory(directory);
}

bool ShouldSkipWatchedDirectory(std::string_view directory_native,
                                project::ProjectTraversalFilter* filter) {
  return filter != nullptr && !filter->Includes(directory_native, platform::PathType::Directory);
}

bool TryBuildTrackedRelativePath(const std::filesystem::path& absolute_path,
                                 const std::filesystem::path& root,
                                 std::filesystem::path& relative_path) {
  std::filesystem::path rel;
  if (!TryComputeRelativePath(absolute_path, root, rel) || rel.empty()) {
    return false;
  }
  if (util::PathTextNeedsNormalizing(rel.native())) {
    rel = rel.lexically_normal();
  }
  if (IsGitMetadataRelativePath(rel) || IsEscapingRelativePath(rel)) {
    return false;
  }
  relative_path = std::move(rel);
  return true;
}

using DirectoryVisitor = std::function<void(std::string_view)>;

using TrackedFileVisitor =
    std::function<bool(const std::filesystem::path& absolute, std::filesystem::path relative)>;

void WalkTrackedTree(const std::filesystem::path& walk_root,
                     const std::filesystem::path& index_root,
                     project::ProjectTraversalFilter* filter,
                     const std::atomic<bool>* stop_requested,
                     const DirectoryVisitor& on_directory,
                     const TrackedFileVisitor& on_file) {
  bool files_wanted = true;
  (void) platform::WalkDirectoryTree(
      walk_root, platform::WalkOptions{}, [&](const platform::WalkEntry& entry) {
        if (stop_requested != nullptr && stop_requested->load(std::memory_order_acquire)) {
          return platform::WalkAction::Stop;
        }
        if (entry.type == platform::PathType::Directory) {
          if (entry.is_symlink) {
            return platform::WalkAction::Continue;
          }
          if (ShouldSkipWatchedDirectory(entry.path, filter)) {
            return platform::WalkAction::Prune;
          }
          if (on_directory) {
            on_directory(entry.path);
          }
          return platform::WalkAction::Continue;
        }
        if (entry.type != platform::PathType::RegularFile || !files_wanted) {
          return platform::WalkAction::Continue;
        }
        if (filter != nullptr && !filter->Includes(entry.path, platform::PathType::RegularFile)) {
          return platform::WalkAction::Continue;
        }
        const std::filesystem::path abs_path(entry.path);
        std::filesystem::path rel;
        if (!TryBuildTrackedRelativePath(abs_path, index_root, rel)) {
          return platform::WalkAction::Continue;
        }
        if (!on_file(abs_path, std::move(rel))) {
          files_wanted = false;
          if (!on_directory) {
            return platform::WalkAction::Stop;
          }
        }
        return platform::WalkAction::Continue;
      });
}

bool CollectTrackedCreations(const std::filesystem::path& walk_root,
                             const std::filesystem::path& index_root,
                             project::ProjectTraversalFilter* filter,
                             std::size_t max_entries,
                             const std::atomic<bool>* stop_requested,
                             std::vector<IndexUpdateBatch::Change>* changes,
                             const DirectoryVisitor& on_directory = {}) {
  bool truncated = false;
  WalkTrackedTree(walk_root, index_root, filter, stop_requested, on_directory,
                  [&](const std::filesystem::path& absolute, std::filesystem::path relative) {
                    if (changes->size() >= max_entries) {
                      truncated = true;
                      return false;
                    }
                    if (util::SweepAbandonedTemporaryStaging(absolute.native())) {
                      return true;
                    }
                    IndexUpdateBatch::Change change;
                    change.kind = IndexUpdateBatch::Kind::CreatedOrModified;
                    change.entry.relative_path = std::move(relative);
                    StampEntryMetadata(absolute, change.entry);
                    changes->push_back(std::move(change));
                    return true;
                  });
  return truncated;
}

class PollStopSignal {
 public:
  void Reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    stopped_ = false;
  }
  void Stop() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stopped_ = true;
    }
    cv_.notify_all();
  }
  bool stopped() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return stopped_;
  }
  bool WaitFor(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait_for(lock, timeout, [this] { return stopped_; });
    return !stopped_;
  }

 private:
  mutable std::mutex mutex_;
  std::condition_variable cv_;
  bool stopped_ = false;
};

detail::FileIndexSnapshot BuildPollSnapshot(const std::filesystem::path& root,
                                            const std::vector<std::string>& exclude_globs,
                                            std::uint64_t* out_directory_signature = nullptr) {
  util::PerformanceTrace::Scope perf_scope("watch::BuildPollSnapshot");
  util::AddPerformanceCounter(util::PerfCounterId::FileWatcherPollScans);
  detail::FileIndexSnapshot result;
  std::uint64_t directory_signature = 0;
  project::ProjectTraversalFilter filter(root, exclude_globs);
  DirectoryVisitor sign_directory;
  if (out_directory_signature != nullptr) {
    sign_directory = [&directory_signature](std::string_view directory) {
      directory_signature +=
          std::hash<std::string_view>{}(directory) * 0x9E3779B9ull + 0x632BE59Bull;
    };
  }
  WalkTrackedTree(root, root, &filter, nullptr, sign_directory,
                  [&result](const std::filesystem::path& absolute, std::filesystem::path relative) {
                    const std::optional<FileMetadata> metadata = ReadFileMetadata(absolute);
                    result[std::move(relative)] = {
                        metadata ? metadata->mtime : std::filesystem::file_time_type{},
                        metadata ? metadata->size : 0};
                    return true;
                  });
  if (out_directory_signature != nullptr) {
    *out_directory_signature = directory_signature;
  }
  return result;
}

void RunPollFallbackWorker(const std::filesystem::path& root,
                           const std::vector<std::string>& exclude_globs,
                           std::size_t entry_budget,
                           std::chrono::milliseconds poll_interval,
                           PollStopSignal& stop_poll,
                           const FileIndexWatcher::Callback& callback) {
  std::uint64_t directory_signature = 0;
  detail::FileIndexSnapshot snapshot =
      BuildPollSnapshot(root, exclude_globs, &directory_signature);
  if (stop_poll.stopped()) {
    return;
  }
  IndexUpdateBatch initial;
  initial.is_initial = true;
  initial.truncated = snapshot.size() > entry_budget;
  initial.changes.reserve(std::min(snapshot.size(), entry_budget));
  for (const auto& [relative_path, meta] : snapshot) {
    if (initial.changes.size() >= entry_budget) {
      break;
    }
    IndexUpdateBatch::Change change;
    change.kind = IndexUpdateBatch::Kind::CreatedOrModified;
    change.entry.relative_path = relative_path;
    change.entry.mtime = meta.first;
    change.entry.size = meta.second;
    initial.changes.push_back(std::move(change));
  }
  if (callback) {
    callback(std::move(initial));
  }

  while (!stop_poll.stopped()) {
    if (!stop_poll.WaitFor(poll_interval)) {
      break;
    }

    std::uint64_t current_directory_signature = 0;
    detail::FileIndexSnapshot current =
        BuildPollSnapshot(root, exclude_globs, &current_directory_signature);
    std::vector<IndexUpdateBatch::Change> changes =
        detail::BuildPollSnapshotDiff(snapshot, current);
    snapshot = std::move(current);
    const bool structure_changed = current_directory_signature != directory_signature;
    directory_signature = current_directory_signature;

    if ((!changes.empty() || structure_changed) && callback) {
      IndexUpdateBatch batch;
      batch.is_initial = false;
      batch.changes = std::move(changes);
      batch.tree_structure_changed = structure_changed;
      callback(std::move(batch));
    }
  }
}

}

std::vector<IndexUpdateBatch::Change> detail::BuildPollSnapshotDiff(
    const FileIndexSnapshot& previous,
    const FileIndexSnapshot& current) {
  std::vector<IndexUpdateBatch::Change> changes;
  auto prev_it = previous.begin();
  auto curr_it = current.begin();
  while (prev_it != previous.end() || curr_it != current.end()) {
    if (curr_it == current.end() ||
        (prev_it != previous.end() && prev_it->first < curr_it->first)) {
      IndexUpdateBatch::Change change;
      change.kind = IndexUpdateBatch::Kind::Deleted;
      change.entry.relative_path = prev_it->first;
      changes.push_back(std::move(change));
      ++prev_it;
      continue;
    }
    if (prev_it == previous.end() || curr_it->first < prev_it->first) {
      IndexUpdateBatch::Change change;
      change.kind = IndexUpdateBatch::Kind::CreatedOrModified;
      change.entry.relative_path = curr_it->first;
      change.entry.mtime = curr_it->second.first;
      change.entry.size = curr_it->second.second;
      changes.push_back(std::move(change));
      ++curr_it;
      continue;
    }

    if (prev_it->second != curr_it->second) {
      IndexUpdateBatch::Change change;
      change.kind = IndexUpdateBatch::Kind::CreatedOrModified;
      change.entry.relative_path = curr_it->first;
      change.entry.mtime = curr_it->second.first;
      change.entry.size = curr_it->second.second;
      changes.push_back(std::move(change));
    }
    ++prev_it;
    ++curr_it;
  }
  return changes;
}

struct FileIndexWatcher::Impl {
  std::atomic<bool> is_native{false};
  std::filesystem::path root;
  FileIndexWatcher::Callback callback;
  std::vector<std::string> exclude_globs;
  std::size_t entry_budget = platform::kTreeTraversalEntryBudget;
  bool warned_fallback = false;

  int inotify_fd = -1;
  int control_pipe[2] = {-1, -1};
  std::map<int, std::filesystem::path> wd_to_path;
  std::set<int> rebound_watches;
  std::thread worker;
  std::thread setup_thread;
  std::atomic<bool> stop_native_setup{false};
  std::atomic<bool> setup_done{false};
  bool native_active = false;

  bool poll_mode = false;
  bool force_poll = false;
  std::chrono::milliseconds poll_interval{750};
  std::thread poll_worker;
  PollStopSignal stop_poll;

  ~Impl() {
    StopNative();
    StopPoll();
  }

  void StopNative() {
    stop_native_setup.store(true, std::memory_order_release);
    if (control_pipe[1] >= 0) {
      const char byte = 0;
      while (write(control_pipe[1], &byte, 1) < 0 && errno == EINTR) {
      }
    }
    if (setup_thread.joinable()) {
      util::PerformanceTrace::Scope scope("watch::StopNative::JoinSetupThread");
      setup_thread.join();
    }
    if (!native_active) {
      DropAllWatchesAndCloseInotify();
      CloseIfValid(control_pipe[0]);
      CloseIfValid(control_pipe[1]);
      control_pipe[0] = control_pipe[1] = -1;
      stop_native_setup.store(false, std::memory_order_release);
      setup_done.store(false, std::memory_order_release);
      return;
    }
    native_active = false;
    if (worker.joinable()) {
      util::PerformanceTrace::Scope scope("watch::StopNative::JoinWorker");
      worker.join();
    }
    CloseIfValid(control_pipe[0]);
    CloseIfValid(control_pipe[1]);
    control_pipe[0] = control_pipe[1] = -1;
    DropAllWatchesAndCloseInotify();
    stop_native_setup.store(false, std::memory_order_release);
    setup_done.store(false, std::memory_order_release);
  }

  void DropAllWatchesAndCloseInotify() {
    RetireDescriptorAsync(inotify_fd);
    inotify_fd = -1;
    util::PerformanceTrace::Scope scope("watch::StopNative::ClearWatchMap");
    wd_to_path.clear();
  }

  void StopPoll() {
    stop_poll.Stop();
    if (poll_worker.joinable()) {
      util::PerformanceTrace::Scope scope("watch::StopPoll::Join");
      poll_worker.join();
    }
  }

  static constexpr std::uint32_t kInotifyMask = IN_CREATE | IN_DELETE | IN_DELETE_SELF |
                                                 IN_MOVE_SELF | IN_MOVED_FROM | IN_MOVED_TO |
                                                 IN_CLOSE_WRITE | IN_ATTRIB;

  static constexpr std::size_t kMaxIndexWatchEntries = 100000;

  bool AddSingleWatch(const std::filesystem::path& dir,
                      project::ProjectTraversalFilter* filter, bool& out_added) {
    out_added = false;
    if (ShouldSkipWatchedDirectory(dir, filter)) {
      return true;
    }
    if (wd_to_path.size() >= kMaxIndexWatchEntries) {
      return false;
    }
    const int wd = inotify_add_watch(inotify_fd, dir.c_str(), kInotifyMask);
    if (wd < 0) {
      if (errno == ENOSPC) {
        return false;
      }
      return true;
    }
    const auto [slot, inserted] = wd_to_path.try_emplace(wd, dir);
    if (!inserted && slot->second != dir) {
      slot->second = dir;
      rebound_watches.insert(wd);
    }
    out_added = true;
    return true;
  }

  bool WalkSubtree(const std::filesystem::path& dir,
                   project::ProjectTraversalFilter* filter,
                   std::vector<IndexUpdateBatch::Change>* changes,
                   bool* files_truncated = nullptr) {
    bool added = false;
    bool watches_ok = AddSingleWatch(dir, filter, added);

    const DirectoryVisitor register_watch = [&](std::string_view subdir_text) {
      if (!watches_ok) {
        return;
      }
      if (wd_to_path.size() >= kMaxIndexWatchEntries) {
        watches_ok = false;
        return;
      }
      std::filesystem::path subdir = util::NormalizedPath(std::filesystem::path(subdir_text));
      const int sub_wd = inotify_add_watch(inotify_fd, subdir.c_str(), kInotifyMask);
      if (sub_wd < 0) {
        if (errno == ENOSPC) {
          watches_ok = false;
        }
        return;
      }
      wd_to_path[sub_wd] = std::move(subdir);
    };

    if (changes != nullptr) {
      const bool truncated = CollectTrackedCreations(dir, root, filter, entry_budget,
                                                     &stop_native_setup, changes,
                                                     register_watch);
      if (files_truncated != nullptr) {
        *files_truncated = truncated;
      }
      return watches_ok;
    }

    std::vector<IndexUpdateBatch::Change> discarded;
    CollectTrackedCreations(dir, root, filter, 0, &stop_native_setup,
                            &discarded, register_watch);
    return watches_ok;
  }

  bool StartNative() {
    inotify_fd = inotify_init1(IN_CLOEXEC | IN_NONBLOCK);
    if (inotify_fd < 0) {
      return false;
    }

    if (pipe2(control_pipe, O_CLOEXEC) != 0) {
      CloseIfValid(inotify_fd);
      inotify_fd = -1;
      return false;
    }

    bool root_added = false;
    if (!AddSingleWatch(root, nullptr, root_added) || !root_added) {
      CloseIfValid(control_pipe[0]);
      CloseIfValid(control_pipe[1]);
      control_pipe[0] = control_pipe[1] = -1;
      CloseIfValid(inotify_fd);
      inotify_fd = -1;
      wd_to_path.clear();
      return false;
    }

    stop_native_setup.store(false, std::memory_order_release);
    setup_done.store(false, std::memory_order_release);
    native_active = true;
    setup_thread = std::thread([this]() {
      util::NameCurrentThread("index-setup");
      project::ProjectTraversalFilter filter(root, exclude_globs);
      util::PerformanceTrace::Scope perf_scope("watch::NativeSetupWalk");
      IndexUpdateBatch initial;
      initial.is_initial = true;
      const bool watches_ok =
          WalkSubtree(root, &filter, &initial.changes, &initial.truncated);
      if (stop_native_setup.load(std::memory_order_acquire)) {
        setup_done.store(true, std::memory_order_release);
        return;
      }
      if (!watches_ok && !warned_fallback) {
        warned_fallback = true;
        util::Log("FileIndexWatcher: inotify watch limit exhausted; tracking partial tree only");
      }
      worker = std::thread([this]() {
        util::NameCurrentThread("index-watch");
        WorkerMain();
      });
      if (callback) {
        callback(std::move(initial));
      }
      setup_done.store(true, std::memory_order_release);
    });
    return true;
  }

  struct DrainCycle {
    explicit DrainCycle(const std::filesystem::path& root, const std::vector<std::string>& exclude_globs)
        : filter(root, exclude_globs) {}

    std::vector<IndexUpdateBatch::Change> changes;
    bool overflow_detected = false;
    bool structure_changed = false;
    project::ProjectTraversalFilter filter;
    std::vector<std::filesystem::path> pending_files;
    std::unordered_map<std::string, bool> pending_removed;
  };

  void WorkerMain() {
    std::array<pollfd, 2> poll_fds{{
        pollfd{.fd = inotify_fd, .events = POLLIN, .revents = 0},
        pollfd{.fd = control_pipe[0], .events = POLLIN, .revents = 0},
    }};

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

      DrainCycle cycle(root, exclude_globs);
      DrainInotifyEvents(cycle);
      if (cycle.overflow_detected) {
        ResyncAfterOverflow();
        continue;
      }
      if ((!cycle.changes.empty() || cycle.structure_changed) && callback) {
        IndexUpdateBatch batch;
        batch.is_initial = false;
        batch.changes = std::move(cycle.changes);
        batch.tree_structure_changed = cycle.structure_changed;
        callback(std::move(batch));
      }
    }
  }

  void DrainInotifyEvents(DrainCycle& cycle) {
    alignas(alignof(struct inotify_event)) std::array<char, 4096> buf{};
    while (true) {
      const ssize_t bytes_read = read(inotify_fd, buf.data(), buf.size());
      if (bytes_read <= 0) {
        if (bytes_read < 0 && errno == EINTR) {
          continue;
        }
        break;
      }
      ssize_t offset = 0;
      while (offset < bytes_read) {
        const struct inotify_event* ev =
            reinterpret_cast<const struct inotify_event*>(buf.data() + offset);
        offset += static_cast<ssize_t>(sizeof(struct inotify_event) + ev->len);
        HandleInotifyEvent(*ev, cycle);
      }
      if (bytes_read < static_cast<ssize_t>(buf.size())) {
        break;
      }
    }
    FlushFileEvents(cycle);
  }

  void HandleInotifyEvent(const struct inotify_event& ev, DrainCycle& cycle) {
    if ((ev.mask & IN_Q_OVERFLOW) != 0) {
      cycle.overflow_detected = true;
      return;
    }
    const auto it = wd_to_path.find(ev.wd);
    if (it == wd_to_path.end()) {
      return;
    }
    if ((ev.mask & IN_IGNORED) != 0) {
      wd_to_path.erase(it);
      rebound_watches.erase(ev.wd);
      return;
    }
    const std::filesystem::path& dir = it->second;
    if (ev.len > 0 && ev.name[0] != '\0') {
      const std::string name(ev.name, ::strnlen(ev.name, ev.len));
      HandleEntryEvent(ev, util::NormalizedPath(dir / name), cycle);
    } else if ((ev.mask & (IN_DELETE_SELF | IN_MOVE_SELF)) != 0) {
      FlushFileEvents(cycle);
      HandleWatchedDirectoryGone(ev, dir, cycle);
    }
  }

  void HandleEntryEvent(const struct inotify_event& ev, const std::filesystem::path& abs_path,
                        DrainCycle& cycle) {
    const bool is_dir = (ev.mask & IN_ISDIR) != 0;
    if (!is_dir) {
      const bool removed = (ev.mask & (IN_DELETE | IN_MOVED_FROM | IN_DELETE_SELF | IN_MOVE_SELF)) != 0;
      const auto [slot, inserted] = cycle.pending_removed.try_emplace(abs_path.native(), removed);
      if (inserted) {
        cycle.pending_files.push_back(abs_path);
      } else {
        slot->second = removed;
      }
      return;
    }
    FlushFileEvents(cycle);
    if ((ev.mask & (IN_CREATE | IN_MOVED_TO)) != 0) {
      project::ProjectTraversalFilter filter(root, exclude_globs);
      if (!ShouldSkipWatchedDirectory(abs_path, &filter)) {
        cycle.structure_changed = true;
      }
      WalkSubtree(abs_path, &filter, &cycle.changes);
    } else if ((ev.mask & (IN_DELETE | IN_MOVED_FROM)) != 0) {
      if (!cycle.filter.ShouldSkipDirectory(abs_path)) {
        cycle.structure_changed = true;
      }
      AppendRecursiveDeletion(abs_path, cycle);
    }
  }

  void HandleWatchedDirectoryGone(const struct inotify_event& ev, const std::filesystem::path& dir,
                                  DrainCycle& cycle) {
    if ((ev.mask & IN_MOVE_SELF) != 0 && rebound_watches.erase(ev.wd) != 0) {
      return;
    }
    cycle.structure_changed = true;
    if (dir == root) {
      AppendRecursiveDeletion(dir, cycle);
    }
    if ((ev.mask & IN_MOVE_SELF) != 0 && dir != root) {
      const std::string prefix = dir.native() + '/';
      for (auto watch = wd_to_path.begin(); watch != wd_to_path.end();) {
        if (watch->second.native().starts_with(prefix)) {
          if (inotify_fd >= 0) {
            inotify_rm_watch(inotify_fd, watch->first);
          }
          rebound_watches.erase(watch->first);
          watch = wd_to_path.erase(watch);
        } else {
          ++watch;
        }
      }
    }
    if (inotify_fd >= 0) {
      inotify_rm_watch(inotify_fd, ev.wd);
    }
    wd_to_path.erase(ev.wd);
    rebound_watches.erase(ev.wd);
  }

  void AppendRecursiveDeletion(const std::filesystem::path& abs_path, DrainCycle& cycle) {
    std::filesystem::path rel;
    if (!TryBuildTrackedRelativePath(abs_path, root, rel)) {
      return;
    }
    IndexUpdateBatch::Change change;
    change.kind = IndexUpdateBatch::Kind::Deleted;
    change.entry.relative_path = rel;
    change.recursive = true;
    cycle.changes.push_back(std::move(change));
  }

  void FlushFileEvents(DrainCycle& cycle) {
    for (const std::filesystem::path& abs_path : cycle.pending_files) {
      std::filesystem::path rel;
      if (!TryBuildTrackedRelativePath(abs_path, root, rel)) {
        continue;
      }
      IndexUpdateBatch::Change change;
      if (cycle.pending_removed[abs_path.native()]) {
        change.kind = IndexUpdateBatch::Kind::Deleted;
        change.entry.relative_path = rel;
      } else {
        std::error_code type_error;
        if (!std::filesystem::is_regular_file(abs_path, type_error)) {
          std::error_code link_error;
          const bool present =
              std::filesystem::exists(std::filesystem::symlink_status(abs_path, link_error));
          if (present && cycle.filter.Includes(abs_path, platform::PathType::Other)) {
            cycle.structure_changed = true;
          }
          continue;
        }
        if (!cycle.filter.Includes(abs_path, platform::PathType::RegularFile)) {
          continue;
        }
        change.kind = IndexUpdateBatch::Kind::CreatedOrModified;
        change.entry.relative_path = rel;
        StampEntryMetadata(abs_path, change.entry);
      }
      cycle.changes.push_back(std::move(change));
    }
    cycle.pending_files.clear();
    cycle.pending_removed.clear();
  }

  void ResyncAfterOverflow() {
    project::ProjectTraversalFilter recovery_filter(root, exclude_globs);
    IndexUpdateBatch resync;
    resync.is_initial = true;
    resync.tree_structure_changed = true;
    WalkSubtree(root, &recovery_filter, &resync.changes, &resync.truncated);
    if (callback) {
      callback(std::move(resync));
    }
  }

  void StartPollFallback() {
    poll_mode = true;
    stop_poll.Reset();
    poll_worker = std::thread([this]() {
      util::NameCurrentThread("index-poll");
      RunPollFallbackWorker(root, exclude_globs, entry_budget, poll_interval, stop_poll, callback);
    });
  }

};

FileIndexWatcher::FileIndexWatcher() : impl_(std::make_unique<Impl>()) {}

FileIndexWatcher::~FileIndexWatcher() {
  Unwatch();
}

struct FileIndexWatcher::DispatchState {
  std::mutex mutex;
  bool initial_applied = false;
  std::vector<IndexUpdateBatch> pending;
  std::size_t pending_change_count = 0;
  bool pending_overflow = false;
  static constexpr std::size_t kMaxPendingChanges = 200000;
  static constexpr std::size_t kMaxPendingBatches = 4096;
};

void FileIndexWatcher::SetCallback(Callback callback) {
  auto state = std::make_shared<DispatchState>();
  dispatch_state_ = state;
  impl_->callback = [client = std::move(callback), state](IndexUpdateBatch batch) {
    const bool is_initial = batch.is_initial;
    std::lock_guard<std::mutex> lock(state->mutex);
    if (!is_initial && !state->initial_applied) {
      if (state->pending_overflow ||
          state->pending.size() >= DispatchState::kMaxPendingBatches ||
          state->pending_change_count + batch.changes.size() >
              DispatchState::kMaxPendingChanges) {
        state->pending_overflow = true;
        return;
      }
      state->pending_change_count += batch.changes.size();
      state->pending.push_back(std::move(batch));
      return;
    }
    if (is_initial) {
      state->initial_applied = true;
    }
    if (client) {
      client(std::move(batch));
    }
    if (is_initial && !state->pending.empty()) {
      if (client) {
        for (IndexUpdateBatch& pending : state->pending) {
          client(std::move(pending));
        }
      }
      state->pending.clear();
      state->pending_change_count = 0;
      state->pending_overflow = false;
    }
  };
}

void FileIndexWatcher::SetExcludeGlobs(std::vector<std::string> globs) {
  impl_->exclude_globs = std::move(globs);
}

void FileIndexWatcher::SetEntryBudget(std::size_t max_entries) {
  impl_->entry_budget = max_entries;
}

void FileIndexWatcher::SetForcePollForTesting(bool force_poll) {
  impl_->force_poll = force_poll;
}

void FileIndexWatcher::SetPollIntervalForTesting(std::chrono::milliseconds interval) {
  impl_->poll_interval = std::max(interval, std::chrono::milliseconds(1));
}

bool FileIndexWatcher::Watch(const std::filesystem::path& root_path) {
  Unwatch();

  std::error_code error;
  const auto absolute_root = std::filesystem::absolute(root_path, error);
  if (error) {
    return false;
  }
  std::error_code exists_error;
  std::error_code dir_error;
  if (!std::filesystem::exists(absolute_root, exists_error) || exists_error ||
      !std::filesystem::is_directory(absolute_root, dir_error) || dir_error) {
    return false;
  }
  impl_->root = util::NormalizedPath(absolute_root);
  if (dispatch_state_) {
    std::lock_guard<std::mutex> lock(dispatch_state_->mutex);
    dispatch_state_->initial_applied = false;
    dispatch_state_->pending.clear();
    dispatch_state_->pending_change_count = 0;
    dispatch_state_->pending_overflow = false;
  }

  if (!impl_->force_poll) {
    if (impl_->StartNative()) {
      impl_->is_native.store(true, std::memory_order_release);
      return true;
    }
    if (!impl_->warned_fallback) {
      impl_->warned_fallback = true;
      util::Log("FileIndexWatcher: native file events unavailable, falling back to poll mode");
    }
  }

  impl_->StartPollFallback();
  return true;
}

OpenFileWatcher& FileIndexWatcher::WatchOpenFiles(OpenFileWatcher::Callback callback) {
  open_files_ = std::make_unique<OpenFileWatcher>(std::move(callback));
  return *open_files_;
}

void FileIndexWatcher::Unwatch() {
  util::PerformanceTrace::Scope perf_scope("watch::Unwatch");
  open_files_.reset();
  {
    util::PerformanceTrace::Scope scope("watch::Unwatch::StopNative");
    impl_->StopNative();
  }
  {
    util::PerformanceTrace::Scope scope("watch::Unwatch::StopPoll");
    impl_->StopPoll();
  }
  impl_->is_native.store(false, std::memory_order_release);
  impl_->root.clear();
  if (dispatch_state_) {
    std::lock_guard<std::mutex> lock(dispatch_state_->mutex);
    dispatch_state_->initial_applied = true;
    dispatch_state_->pending.clear();
    dispatch_state_->pending_change_count = 0;
    dispatch_state_->pending_overflow = false;
  }
}

bool FileIndexWatcher::IsNative() const {
  return impl_->is_native.load(std::memory_order_acquire);
}

void FileIndexWatcher::DispatchBatchForTesting(IndexUpdateBatch batch) {
  util::InvokeIfSet(impl_->callback, std::move(batch));
}

}
