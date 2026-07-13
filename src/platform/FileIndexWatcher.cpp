#include "platform/FileIndexWatcher.h"

#include "platform/HostPlatform.h"
#include "project/ProjectTraversalFilter.h"
#include "util/StringUtil.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#if defined(__linux__)
#include <fcntl.h>
#include <poll.h>
#include <sys/inotify.h>
#include <unistd.h>
#elif defined(__APPLE__)
#include <CoreServices/CoreServices.h>
#include <fcntl.h>
#include <unistd.h>
#elif defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace microide::platform {

namespace {

#if defined(__linux__) || defined(__APPLE__)
void CloseIfValid(int fd) {
  if (fd >= 0) {
    close(fd);
  }
}
#endif

bool IsGitMetadataRelativePath(const std::filesystem::path& relative_path) {
  const auto it = relative_path.begin();
  if (it == relative_path.end()) {
    return false;
  }
  const std::string first = it->string();
  if (first == ".git") {
    return true;
  }
  // Case-insensitive hosts (Windows / default macOS): `.GIT`/`.Git` name the same
  // metadata directory and must also be excluded from the watched index.
  if (HostPathsAreCaseInsensitive() && first.size() == 4) {
    return (first[0] == '.') &&
           (first[1] == 'g' || first[1] == 'G') &&
           (first[2] == 'i' || first[2] == 'I') &&
           (first[3] == 't' || first[3] == 'T');
  }
  return false;
}

// True when a computed relative path escapes the project root — either it is
// absolute or its first component is "..". `lexically_relative` happily returns
// "../outside/file" for an absolute path outside the root, and downstream code
// resolves `root / relative`, so an escaping relative would let watcher/poll
// events insert paths outside the project into the index.
bool IsEscapingRelativePath(const std::filesystem::path& relative_path) {
  if (relative_path.is_absolute()) {
    return true;
  }
  const auto it = relative_path.begin();
  return it != relative_path.end() && *it == std::filesystem::path("..");
}

bool TryComputeRelativePath(const std::filesystem::path& absolute_path,
                            const std::filesystem::path& root,
                            std::filesystem::path& relative_path) {
  const std::filesystem::path normalized_path = absolute_path.lexically_normal();
  const std::filesystem::path normalized_root = root.lexically_normal();
  std::filesystem::path relative = normalized_path.lexically_relative(normalized_root);
  if (!relative.empty()) {
    relative_path = std::move(relative);
    return true;
  }
#ifdef _WIN32
  const std::string path_text = normalized_path.generic_string();
  const std::string root_text = normalized_root.generic_string();
  if (path_text.size() <= root_text.size()) {
    return false;
  }
  const std::string lower_path = util::ToLowerAscii(path_text);
  const std::string lower_root = util::ToLowerAscii(root_text);
  if (lower_path.rfind(lower_root, 0) != 0 || path_text[root_text.size()] != '/') {
    return false;
  }
  relative_path = std::filesystem::path(path_text.substr(root_text.size() + 1)).lexically_normal();
  return !relative_path.empty();
#else
  return false;
#endif
}

// A directory is skipped (not walked, not watched, not indexed) when the shared
// traversal filter excludes it: VCS metadata, dependency/cache trees, common
// build-output dirs, user excludes, and nested .gitignore all funnel through here.
bool ShouldSkipWatchedDirectory(const std::filesystem::path& directory,
                                project::ProjectTraversalFilter* filter) {
  return filter != nullptr && filter->ShouldSkipDirectory(directory);
}

bool TryBuildTrackedRelativePath(const std::filesystem::path& absolute_path,
                                 const std::filesystem::path& root,
                                 std::filesystem::path& relative_path) {
  std::filesystem::path rel;
  if (!TryComputeRelativePath(absolute_path, root, rel) || rel.empty()) {
    return false;
  }
  rel = rel.lexically_normal();
  if (IsGitMetadataRelativePath(rel) || IsEscapingRelativePath(rel)) {
    return false;
  }
  relative_path = std::move(rel);
  return true;
}

#if defined(_WIN32)
bool ShouldIgnoreTrackedRelativePath(const std::filesystem::path& root,
                                     const std::filesystem::path& relative_path,
                                     project::ProjectTraversalFilter* filter) {
  if (relative_path.empty() || IsGitMetadataRelativePath(relative_path)) {
    return true;
  }
  if (filter == nullptr) {
    return false;
  }
  // The filter walks the full ancestor chain (nested .gitignore + defaults) itself.
  return !filter->Includes((root / relative_path).lexically_normal(),
                           platform::PathType::RegularFile);
}
#endif

// Build an initial IndexUpdateBatch by scanning root recursively, pruning excluded
// directories (VCS/build/deps/nested-.gitignore) and files, and stopping once
// `max_entries` kept files have been collected (batch.truncated is then set).
// Shared tree walk: append a CreatedOrModified change for every tracked regular file
// under `walk_root`, with paths relative to `index_root`, stopping once `changes`
// reaches `max_entries`. Returns true if that budget was hit (truncated). Both the
// initial full scan and the incremental subtree scans (moved-in dir, overflow
// recovery) go through here so the ignore/prune/relative-path rules stay identical.
bool CollectTrackedCreations(const std::filesystem::path& walk_root,
                             const std::filesystem::path& index_root,
                             project::ProjectTraversalFilter* filter,
                             std::size_t max_entries,
                             const std::atomic<bool>* stop_requested,
                             std::vector<IndexUpdateBatch::Change>* changes) {
  std::error_code error;
  constexpr auto options = std::filesystem::directory_options::skip_permission_denied;
  for (std::filesystem::recursive_directory_iterator it(walk_root, options, error), end;
       !error && it != end; it.increment(error)) {
    if (stop_requested != nullptr && stop_requested->load(std::memory_order_acquire)) {
      break;
    }
    std::error_code status_error;
    const auto status = it->status(status_error);
    if (status_error) {
      continue;
    }
    if (std::filesystem::is_directory(status)) {
      // Prune excluded subtrees before descending so they cost zero budget.
      if (ShouldSkipWatchedDirectory(it->path(), filter)) {
        it.disable_recursion_pending();
      }
      continue;
    }
    if (!std::filesystem::is_regular_file(status)) {
      continue;
    }
    const std::filesystem::path abs_path = it->path().lexically_normal();
    std::filesystem::path rel;
    if (!TryBuildTrackedRelativePath(abs_path, index_root, rel)) {
      continue;
    }
    // File-level ignore check (parity with the finder/tree): an ignored file whose
    // parent directory was not itself pruned is still excluded from the index.
    if (filter != nullptr && !filter->Includes(abs_path, platform::PathType::RegularFile)) {
      continue;
    }
    if (changes->size() >= max_entries) {
      return true;
    }
    std::error_code mtime_error;
    const auto mtime = std::filesystem::last_write_time(abs_path, mtime_error);
    std::error_code size_error;
    const auto size = std::filesystem::file_size(abs_path, size_error);
    IndexUpdateBatch::Change change;
    change.kind = IndexUpdateBatch::Kind::CreatedOrModified;
    change.entry.relative_path = rel;
    change.entry.mtime = mtime_error ? std::filesystem::file_time_type{} : mtime;
    change.entry.size = size_error ? 0 : size;
    changes->push_back(std::move(change));
  }
  return false;
}

IndexUpdateBatch BuildInitialBatch(const std::filesystem::path& root,
                                   project::ProjectTraversalFilter* filter,
                                   std::size_t max_entries,
                                   const std::atomic<bool>* stop_requested = nullptr) {
  IndexUpdateBatch batch;
  batch.is_initial = true;
  batch.truncated =
      CollectTrackedCreations(root, root, filter, max_entries, stop_requested, &batch.changes);
  return batch;
}

// Append creations for the pre-existing files inside a directory that was created or
// moved into the tree: inotify emits no per-file events for them, so without this
// bounded walk they never enter the index until a full rescan.
void AppendSubtreeCreations(const std::filesystem::path& dir,
                            const std::filesystem::path& root,
                            project::ProjectTraversalFilter* filter,
                            std::size_t max_entries,
                            std::vector<IndexUpdateBatch::Change>* changes) {
  CollectTrackedCreations(dir, root, filter, max_entries, /*stop_requested=*/nullptr, changes);
}

}  // namespace

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

// ============================================================
// Platform-specific backends
// ============================================================

#if defined(__linux__)

struct FileIndexWatcher::Impl {
  std::atomic<bool> is_native{false};
  std::filesystem::path root;
  FileIndexWatcher::Callback callback;
  // Filter inputs are set before any walk thread starts and never mutated after,
  // so worker threads read them race-free and each constructs its OWN filter (the
  // per-directory matcher cache is not thread-safe to share).
  std::vector<std::string> exclude_globs;
  std::size_t entry_budget = platform::kTreeTraversalEntryBudget;
  bool warned_fallback = false;

  // inotify backend
  int inotify_fd = -1;
  int control_pipe[2] = {-1, -1};
  std::map<int, std::filesystem::path> wd_to_path;  // watch descriptor -> abs path
  std::thread worker;
  std::thread setup_thread;
  std::atomic<bool> stop_native_setup{false};
  std::atomic<bool> setup_done{false};
  bool native_active = false;

  // poll-fallback
  bool poll_mode = false;
  std::thread poll_worker;
  std::atomic<bool> stop_poll{false};
  std::thread initial_scan_worker;
  std::atomic<bool> stop_initial_scan{false};

  ~Impl() {
    StopNative();
    StopPoll();
  }

  void StopInitialScan() {
    stop_initial_scan.store(true, std::memory_order_release);
    if (initial_scan_worker.joinable()) {
      initial_scan_worker.join();
    }
    stop_initial_scan.store(false, std::memory_order_release);
  }

  void StopNative() {
    StopInitialScan();
    // Tell the setup thread to bail and unblock the worker (if it's already running).
    stop_native_setup.store(true, std::memory_order_release);
    if (control_pipe[1] >= 0) {
      const char byte = 0;
      while (write(control_pipe[1], &byte, 1) < 0 && errno == EINTR) {
      }
    }
    if (setup_thread.joinable()) {
      setup_thread.join();
    }
    if (!native_active) {
      // Setup thread aborted (or never ran successfully); just clean up any FDs it left behind.
      for (auto& [wd, path] : wd_to_path) {
        if (inotify_fd >= 0) {
          inotify_rm_watch(inotify_fd, wd);
        }
      }
      wd_to_path.clear();
      CloseIfValid(control_pipe[0]);
      CloseIfValid(control_pipe[1]);
      control_pipe[0] = control_pipe[1] = -1;
      CloseIfValid(inotify_fd);
      inotify_fd = -1;
      stop_native_setup.store(false, std::memory_order_release);
      setup_done.store(false, std::memory_order_release);
      return;
    }
    native_active = false;
    if (worker.joinable()) {
      worker.join();
    }
    CloseIfValid(control_pipe[0]);
    CloseIfValid(control_pipe[1]);
    control_pipe[0] = control_pipe[1] = -1;
    for (auto& [wd, path] : wd_to_path) {
      if (inotify_fd >= 0) {
        inotify_rm_watch(inotify_fd, wd);
      }
    }
    wd_to_path.clear();
    CloseIfValid(inotify_fd);
    inotify_fd = -1;
    stop_native_setup.store(false, std::memory_order_release);
    setup_done.store(false, std::memory_order_release);
  }

  void StopPoll() {
    stop_poll.store(true, std::memory_order_release);
    if (poll_worker.joinable()) {
      poll_worker.join();
    }
  }

  void StartInitialScan() {
    StopInitialScan();
    stop_initial_scan.store(false, std::memory_order_release);
    initial_scan_worker = std::thread([this]() {
      project::ProjectTraversalFilter filter(root, exclude_globs);
      IndexUpdateBatch initial =
          BuildInitialBatch(root, &filter, entry_budget, &stop_initial_scan);
      if (!stop_initial_scan.load(std::memory_order_acquire) && callback) {
        callback(std::move(initial));
      }
    });
  }

  // Mask used for every inotify watch. Defined here so both the synchronous root-watch
  // bootstrap and the background recursive walk use identical event filters.
  static constexpr std::uint32_t kInotifyMask = IN_CREATE | IN_DELETE | IN_DELETE_SELF |
                                                 IN_MOVE_SELF | IN_MOVED_FROM | IN_MOVED_TO |
                                                 IN_CLOSE_WRITE | IN_ATTRIB;

  // Own watch-count ceiling, independent of the kernel's per-user inotify limit.
  // A deep/wide tree — or an attacker rapidly creating nested directories, each
  // of which re-enters AddWatchRecursive on the watcher thread — would otherwise
  // grow wd_to_path (and the kernel watch table) without bound. Past the cap we
  // degrade to tracking a partial tree, reusing the same graceful path as ENOSPC.
  static constexpr std::size_t kMaxIndexWatchEntries = 100000;

  // Add a single inotify watch for `dir` if it's not skipped. Returns false on ENOSPC
  // or when our own watch budget is exhausted. Sets `out_added` to true if a new watch
  // was registered (root-skipped or already-present entries return true with
  // out_added=false).
  bool AddSingleWatch(const std::filesystem::path& dir,
                      project::ProjectTraversalFilter* filter, bool& out_added) {
    out_added = false;
    if (ShouldSkipWatchedDirectory(dir, filter)) {
      return true;
    }
    if (wd_to_path.size() >= kMaxIndexWatchEntries) {
      return false;  // budget exhausted -> partial-tree degradation
    }
    const int wd = inotify_add_watch(inotify_fd, dir.c_str(), kInotifyMask);
    if (wd < 0) {
      if (errno == ENOSPC) {
        return false;
      }
      return true;
    }
    wd_to_path[wd] = dir;
    out_added = true;
    return true;
  }

  // Add inotify watches recursively for dir and all subdirectories. Periodically checks
  // stop_native_setup so an Unwatch() during bootstrap returns promptly. Returns false
  // only on inotify watch-limit exhaustion (ENOSPC); other per-watch errors are skipped.
  bool AddWatchRecursive(const std::filesystem::path& dir,
                         project::ProjectTraversalFilter* filter) {
    bool added = false;
    if (!AddSingleWatch(dir, filter, added)) {
      return false;
    }

    std::error_code error;
    constexpr auto opts = std::filesystem::directory_options::skip_permission_denied;
    for (std::filesystem::recursive_directory_iterator it(dir, opts, error), end;
         !error && it != end; it.increment(error)) {
      if (stop_native_setup.load(std::memory_order_acquire)) {
        return true;
      }
      std::error_code status_error;
      const auto status = it->status(status_error);
      if (status_error) {
        continue;
      }
      if (std::filesystem::is_directory(status) &&
          ShouldSkipWatchedDirectory(it->path(), filter)) {
        it.disable_recursion_pending();
        continue;
      }
      if (!std::filesystem::is_directory(status)) {
        continue;
      }
      if (wd_to_path.size() >= kMaxIndexWatchEntries) {
        return false;  // budget exhausted -> partial-tree degradation
      }
      const std::filesystem::path subdir = it->path().lexically_normal();
      const int sub_wd = inotify_add_watch(inotify_fd, subdir.c_str(), kInotifyMask);
      if (sub_wd < 0) {
        if (errno == ENOSPC) {
          return false;
        }
        continue;
      }
      wd_to_path[sub_wd] = subdir;
    }
    return true;
  }

  // StartNative opens the inotify fd + control pipe and registers a watch on the project
  // root synchronously (sub-millisecond) so top-level changes are observed even if the
  // caller mutates the tree immediately after Watch() returns. The recursive subtree walk
  // (~hundreds of ms on large trees) is handed off to a setup thread that starts the read
  // loop afterwards, or aborts cleanly if Unwatch() is called during bootstrap.
  bool StartNative() {
    inotify_fd = inotify_init1(IN_CLOEXEC | IN_NONBLOCK);
    if (inotify_fd < 0) {
      return false;
    }

    // Control pipe must be close-on-exec so an unrelated fork()+exec() on another
    // thread cannot inherit and hold these fds open for its lifetime (matches the
    // Subprocess/AsyncSubprocess inherited-pipe hardening).
    if (pipe2(control_pipe, O_CLOEXEC) != 0) {
      CloseIfValid(inotify_fd);
      inotify_fd = -1;
      return false;
    }

    bool root_added = false;
    // The project root is never itself excluded, so a null filter suffices here;
    // the setup thread builds its own filter for the recursive subtree walk.
    if (!AddSingleWatch(root, nullptr, root_added) || !root_added) {
      // Either ENOSPC on the very first watch, or root itself is somehow ignored.
      // Tear down and let the caller fall back to poll mode.
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
    // The setup thread populates wd_to_path for the rest of the tree, then starts the
    // worker. Events that arrive on the root watch during this window queue inside the
    // kernel's inotify queue and are drained when the worker starts; we don't read them
    // from a second thread, so there's no data race on wd_to_path.
    setup_thread = std::thread([this]() {
      project::ProjectTraversalFilter filter(root, exclude_globs);
      // Walk subdirectories of root and register watches for each. AddWatchRecursive
      // re-uses the existing root watch (inotify_add_watch returns the same wd).
      const bool watches_ok = AddWatchRecursive(root, &filter);
      if (stop_native_setup.load(std::memory_order_acquire)) {
        setup_done.store(true, std::memory_order_release);
        return;
      }
      if (!watches_ok && !warned_fallback) {
        warned_fallback = true;
        SDL_Log(
            "FileIndexWatcher: inotify watch limit exhausted; tracking partial tree only");
      }
      worker = std::thread([this]() { WorkerMain(); });
      setup_done.store(true, std::memory_order_release);
    });
    return true;
  }

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

      // Read all available inotify events
      std::vector<IndexUpdateBatch::Change> changes;
      bool overflow_detected = false;
      // One ignore filter per drain cycle (poll wakeup), reused for every
      // single-file create/modify event below and rebuilt next wakeup so a
      // .gitignore edited between cycles is picked up. Constructing it here (not
      // per event) amortizes the root-.gitignore read across the whole batch.
      project::ProjectTraversalFilter batch_filter(root, exclude_globs);
      {
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

            // Kernel queue overflow (wd == -1): an unknown set of events was dropped,
            // so anything read this cycle is untrustworthy. Flag it and resync fully.
            if ((ev->mask & IN_Q_OVERFLOW) != 0) {
              overflow_detected = true;
              continue;
            }

            const bool is_dir = (ev->mask & IN_ISDIR) != 0;

            // Find the directory for this watch descriptor
            const auto it = wd_to_path.find(ev->wd);
            if (it == wd_to_path.end()) {
              continue;
            }

            // IN_IGNORED: the kernel auto-removed this watch because its directory
            // was deleted or moved out. When a subtree is removed via the PARENT's
            // IN_DELETE/IN_MOVED_FROM the child descriptors are not cleaned up
            // anywhere else, so without this every removed subdirectory leaks a
            // wd_to_path entry until the watch cap (kMaxIndexWatchEntries) is hit and
            // the watcher silently degrades to partial-tree mode. The recursive
            // deletion was already emitted by the parent event, so just drop the
            // stale mapping (the watch is already gone — no inotify_rm_watch needed).
            if ((ev->mask & IN_IGNORED) != 0) {
              wd_to_path.erase(it);
              continue;
            }
            const std::filesystem::path& dir = it->second;

            if (ev->len > 0 && ev->name[0] != '\0') {
              const std::string name(ev->name, ::strnlen(ev->name, ev->len));
              const std::filesystem::path abs_path = (dir / name).lexically_normal();

              if (is_dir && (ev->mask & (IN_CREATE | IN_MOVED_TO)) != 0) {
                // New directory: add watches for it recursively. Build a fresh
                // filter per event (rare) so this single-threaded worker never
                // shares the filter's mutable cache with another walk.
                project::ProjectTraversalFilter filter(root, exclude_globs);
                AddWatchRecursive(abs_path, &filter);
                // A moved-in directory can already be populated, and inotify emits no
                // per-file events for files that existed before the move. Walk the new
                // subtree and enqueue creations so those files enter the index instead
                // of staying invisible until a full rescan.
                AppendSubtreeCreations(abs_path, root, &filter, entry_budget, &changes);
              } else if (is_dir && (ev->mask & (IN_DELETE | IN_MOVED_FROM)) != 0) {
                // A subdirectory was deleted or moved out. inotify sends no per-file
                // deletion for its contents, so emit one recursive deletion and let the
                // index drop every entry beneath it (otherwise they linger as ghosts).
                std::filesystem::path rel;
                if (TryBuildTrackedRelativePath(abs_path, root, rel)) {
                  IndexUpdateBatch::Change change;
                  change.kind = IndexUpdateBatch::Kind::Deleted;
                  change.entry.relative_path = rel;
                  change.recursive = true;
                  changes.push_back(std::move(change));
                }
              } else if (!is_dir) {
                std::filesystem::path rel;
                if (TryBuildTrackedRelativePath(abs_path, root, rel)) {
                  IndexUpdateBatch::Change change;
                  if ((ev->mask & (IN_DELETE | IN_MOVED_FROM | IN_DELETE_SELF | IN_MOVE_SELF)) != 0) {
                    change.kind = IndexUpdateBatch::Kind::Deleted;
                    change.entry.relative_path = rel;
                  } else {
                    // Apply the same regular-file + ignore-filter gate the initial
                    // scan (CollectTrackedCreations) and poll fallback enforce.
                    // Without it, a .gitignore/exclude-glob'd file (or a non-regular
                    // entry like a FIFO/socket/symlink) created in a watched dir
                    // leaks into the index via this incremental event and only
                    // vanishes on a full rescan -- an inconsistent, confusing state.
                    std::error_code type_error;
                    if (!std::filesystem::is_regular_file(abs_path, type_error) ||
                        !batch_filter.Includes(abs_path, platform::PathType::RegularFile)) {
                      continue;
                    }
                    change.kind = IndexUpdateBatch::Kind::CreatedOrModified;
                    change.entry.relative_path = rel;
                    std::error_code mtime_error;
                    const auto mtime = std::filesystem::last_write_time(abs_path, mtime_error);
                    std::error_code size_error;
                    const auto size = std::filesystem::file_size(abs_path, size_error);
                    change.entry.mtime =
                        mtime_error ? std::filesystem::file_time_type{} : mtime;
                    change.entry.size = size_error ? 0 : size;
                  }
                  changes.push_back(std::move(change));
                }
              }
            } else if ((ev->mask & (IN_DELETE_SELF | IN_MOVE_SELF)) != 0) {
              // The watched directory itself was removed/moved. Drop the watch AND
              // emit a recursive deletion for it, or every file it contained lingers
              // in the index as a ghost until a full rescan.
              std::filesystem::path rel;
              if (TryBuildTrackedRelativePath(dir, root, rel)) {
                IndexUpdateBatch::Change change;
                change.kind = IndexUpdateBatch::Kind::Deleted;
                change.entry.relative_path = rel;
                change.recursive = true;
                changes.push_back(std::move(change));
              }
              if (inotify_fd >= 0) {
                inotify_rm_watch(inotify_fd, ev->wd);
              }
              wd_to_path.erase(it);
            }
          }

          if (bytes_read < static_cast<ssize_t>(buf.size())) {
            break;
          }
        }
      }

      if (overflow_detected) {
        // Recover from a dropped-event window: re-register watches (directories may
        // have appeared during the gap) and resync the whole index with a fresh full
        // scan. is_initial=true makes the consumer replace the index wholesale, so the
        // partial `changes` gathered this cycle are intentionally discarded.
        project::ProjectTraversalFilter watch_filter(root, exclude_globs);
        AddWatchRecursive(root, &watch_filter);
        if (callback) {
          project::ProjectTraversalFilter scan_filter(root, exclude_globs);
          // Observe the WORKER's stop flag, not the initial-scan thread's:
          // StopNative() calls StopInitialScan() (which resets stop_initial_scan to
          // false) BEFORE it sets stop_native_setup and joins this worker. Polling
          // stop_initial_scan here would ignore a teardown that arrives mid-recovery
          // and block worker.join() for the full scan budget. stop_native_setup stays
          // true across the worker join, so this recovery scan bails promptly.
          callback(BuildInitialBatch(root, &scan_filter, entry_budget, &stop_native_setup));
        }
        continue;
      }

      if (!changes.empty() && callback) {
        IndexUpdateBatch batch;
        batch.is_initial = false;
        batch.changes = std::move(changes);
        callback(std::move(batch));
      }
    }
  }

  void StartPollFallback() {
    poll_mode = true;
    stop_poll.store(false, std::memory_order_release);
    // Take initial snapshot
    poll_worker = std::thread([this]() { PollWorkerMain(); });
  }

  void PollWorkerMain() {
    // Build initial snapshot: map relative_path -> mtime+size
    std::map<std::filesystem::path, std::pair<std::filesystem::file_time_type, std::uintmax_t>>
        snapshot;

    auto build_snapshot =
        [&]() -> std::map<std::filesystem::path,
                          std::pair<std::filesystem::file_time_type, std::uintmax_t>> {
      std::map<std::filesystem::path,
               std::pair<std::filesystem::file_time_type, std::uintmax_t>>
          result;
      project::ProjectTraversalFilter filter(root, exclude_globs);
      std::error_code error;
      constexpr auto opts = std::filesystem::directory_options::skip_permission_denied;
      for (std::filesystem::recursive_directory_iterator it(root, opts, error), end;
           !error && it != end; it.increment(error)) {
        std::error_code status_error;
        const auto status = it->status(status_error);
        if (status_error) {
          continue;
        }
        if (std::filesystem::is_directory(status) &&
            ShouldSkipWatchedDirectory(it->path(), &filter)) {
          it.disable_recursion_pending();
          continue;
        }
        if (!std::filesystem::is_regular_file(status)) {
          continue;
        }
        const std::filesystem::path abs_path = it->path().lexically_normal();
        std::filesystem::path rel;
        if (!TryBuildTrackedRelativePath(abs_path, root, rel)) {
          continue;
        }
        if (!filter.Includes(abs_path, platform::PathType::RegularFile)) {
          continue;
        }
        std::error_code mtime_error;
        const auto mtime = std::filesystem::last_write_time(abs_path, mtime_error);
        std::error_code size_error;
        const auto sz = std::filesystem::file_size(abs_path, size_error);
        result[rel] = {mtime_error ? std::filesystem::file_time_type{} : mtime,
                       size_error ? 0 : sz};
      }
      return result;
    };

    snapshot = build_snapshot();

    while (!stop_poll.load(std::memory_order_acquire)) {
      // Sleep 750ms in small increments to react to stop quickly
      for (int i = 0; i < 15 && !stop_poll.load(std::memory_order_acquire); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
      }
      if (stop_poll.load(std::memory_order_acquire)) {
        break;
      }

      auto current = build_snapshot();

      std::vector<IndexUpdateBatch::Change> changes =
          detail::BuildPollSnapshotDiff(snapshot, current);

      snapshot = std::move(current);

      if (!changes.empty() && callback) {
        IndexUpdateBatch batch;
        batch.is_initial = false;
        batch.changes = std::move(changes);
        callback(std::move(batch));
      }
    }
  }
};

#elif defined(__APPLE__)

struct FileIndexWatcher::Impl {
  std::atomic<bool> is_native{false};
  std::filesystem::path root;
  FileIndexWatcher::Callback callback;
  // See the __linux__ backend for the threading contract: inputs set before any
  // walk thread starts; each walk constructs its own (non-shareable) filter.
  std::vector<std::string> exclude_globs;
  std::size_t entry_budget = platform::kTreeTraversalEntryBudget;

  // FSEvents backend
  FSEventStreamRef stream = nullptr;
  CFRunLoopRef run_loop = nullptr;
  std::thread worker;
  bool native_active = false;

  // poll-fallback
  bool poll_mode = false;
  std::thread poll_worker;
  std::atomic<bool> stop_poll{false};
  std::thread initial_scan_worker;
  std::atomic<bool> stop_initial_scan{false};
  bool warned_fallback = false;

  ~Impl() {
    StopNative();
    StopPoll();
  }

  void StopInitialScan() {
    stop_initial_scan.store(true, std::memory_order_release);
    if (initial_scan_worker.joinable()) {
      initial_scan_worker.join();
    }
    stop_initial_scan.store(false, std::memory_order_release);
  }

  void StopNative() {
    StopInitialScan();
    if (!native_active) {
      return;
    }
    native_active = false;
    if (run_loop != nullptr) {
      CFRunLoopStop(run_loop);
    }
    if (worker.joinable()) {
      worker.join();
    }
    run_loop = nullptr;
    if (stream != nullptr) {
      FSEventStreamStop(stream);
      FSEventStreamInvalidate(stream);
      FSEventStreamRelease(stream);
      stream = nullptr;
    }
  }

  void StopPoll() {
    StopInitialScan();
    stop_poll.store(true, std::memory_order_release);
    if (poll_worker.joinable()) {
      poll_worker.join();
    }
  }

  void StartInitialScan() {
    StopInitialScan();
    stop_initial_scan.store(false, std::memory_order_release);
    initial_scan_worker = std::thread([this]() {
      project::ProjectTraversalFilter filter(root, exclude_globs);
      IndexUpdateBatch initial =
          BuildInitialBatch(root, &filter, entry_budget, &stop_initial_scan);
      if (!stop_initial_scan.load(std::memory_order_acquire) && callback) {
        callback(std::move(initial));
      }
    });
  }

  static void FsEventsCallback(ConstFSEventStreamRef /*stream_ref*/,
                                void* client_callback_info,
                                size_t num_events,
                                void* event_paths,
                                const FSEventStreamEventFlags* event_flags,
                                const FSEventStreamEventId* /*event_ids*/) {
    Impl* self = static_cast<Impl*>(client_callback_info);
    if (self == nullptr || !self->callback) {
      return;
    }

    char** paths = static_cast<char**>(event_paths);
    std::vector<IndexUpdateBatch::Change> changes;

    for (size_t i = 0; i < num_events; ++i) {
      const FSEventStreamEventFlags flags = event_flags[i];
      const std::filesystem::path abs_path =
          std::filesystem::path(paths[i]).lexically_normal();

      const bool is_dir =
          (flags & kFSEventStreamEventFlagItemIsDir) != 0;
      if (is_dir) {
        continue;
      }

      std::filesystem::path rel;
      if (!TryBuildTrackedRelativePath(abs_path, self->root, rel)) {
        continue;
      }

      IndexUpdateBatch::Change change;
      if ((flags & kFSEventStreamEventFlagItemRemoved) != 0) {
        change.kind = IndexUpdateBatch::Kind::Deleted;
        change.entry.relative_path = rel;
      } else {
        change.kind = IndexUpdateBatch::Kind::CreatedOrModified;
        change.entry.relative_path = rel;
        std::error_code mtime_error;
        const auto mtime = std::filesystem::last_write_time(abs_path, mtime_error);
        std::error_code size_error;
        const auto size = std::filesystem::file_size(abs_path, size_error);
        change.entry.mtime = mtime_error ? std::filesystem::file_time_type{} : mtime;
        change.entry.size = size_error ? 0 : size;
      }
      changes.push_back(std::move(change));
    }

    if (!changes.empty()) {
      IndexUpdateBatch batch;
      batch.is_initial = false;
      batch.changes = std::move(changes);
      self->callback(std::move(batch));
    }
  }

  bool StartNative() {
    CFStringRef path_str =
        CFStringCreateWithCString(nullptr, root.c_str(), kCFStringEncodingUTF8);
    if (path_str == nullptr) {
      return false;
    }
    CFArrayRef paths_to_watch =
        CFArrayCreate(nullptr, reinterpret_cast<const void**>(&path_str), 1,
                      &kCFTypeArrayCallBacks);
    CFRelease(path_str);
    if (paths_to_watch == nullptr) {
      return false;
    }

    FSEventStreamContext ctx{};
    ctx.info = this;

    stream = FSEventStreamCreate(
        nullptr, &FsEventsCallback, &ctx, paths_to_watch,
        kFSEventStreamEventIdSinceNow,
        0.05,  // 50ms latency
        kFSEventStreamCreateFlagFileEvents | kFSEventStreamCreateFlagNoDefer);
    CFRelease(paths_to_watch);

    if (stream == nullptr) {
      return false;
    }

    native_active = true;
    worker = std::thread([this]() {
      run_loop = CFRunLoopGetCurrent();
      FSEventStreamScheduleWithRunLoop(stream, run_loop, kCFRunLoopDefaultMode);
      FSEventStreamStart(stream);
      CFRunLoopRun();
      FSEventStreamStop(stream);
      FSEventStreamUnscheduleFromRunLoop(stream, run_loop, kCFRunLoopDefaultMode);
    });

    return true;
  }

  void StartPollFallback() {
    poll_mode = true;
    stop_poll.store(false, std::memory_order_release);
    poll_worker = std::thread([this]() { PollWorkerMain(); });
  }

  void PollWorkerMain() {
    std::map<std::filesystem::path, std::pair<std::filesystem::file_time_type, std::uintmax_t>>
        snapshot;

    auto build_snapshot =
        [&]() -> std::map<std::filesystem::path,
                          std::pair<std::filesystem::file_time_type, std::uintmax_t>> {
      std::map<std::filesystem::path,
               std::pair<std::filesystem::file_time_type, std::uintmax_t>>
          result;
      project::ProjectTraversalFilter filter(root, exclude_globs);
      std::error_code error;
      constexpr auto opts = std::filesystem::directory_options::skip_permission_denied;
      for (std::filesystem::recursive_directory_iterator it(root, opts, error), end;
           !error && it != end; it.increment(error)) {
        std::error_code status_error;
        const auto status = it->status(status_error);
        if (status_error) {
          continue;
        }
        if (std::filesystem::is_directory(status) &&
            ShouldSkipWatchedDirectory(it->path(), &filter)) {
          it.disable_recursion_pending();
          continue;
        }
        if (!std::filesystem::is_regular_file(status)) {
          continue;
        }
        const std::filesystem::path abs_path = it->path().lexically_normal();
        std::filesystem::path rel;
        if (!TryBuildTrackedRelativePath(abs_path, root, rel)) {
          continue;
        }
        if (!filter.Includes(abs_path, platform::PathType::RegularFile)) {
          continue;
        }
        std::error_code mtime_error;
        const auto mtime = std::filesystem::last_write_time(abs_path, mtime_error);
        std::error_code size_error;
        const auto sz = std::filesystem::file_size(abs_path, size_error);
        result[rel] = {mtime_error ? std::filesystem::file_time_type{} : mtime,
                       size_error ? 0 : sz};
      }
      return result;
    };

    snapshot = build_snapshot();

    while (!stop_poll.load(std::memory_order_acquire)) {
      for (int i = 0; i < 15 && !stop_poll.load(std::memory_order_acquire); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
      }
      if (stop_poll.load(std::memory_order_acquire)) {
        break;
      }

      auto current = build_snapshot();
      std::vector<IndexUpdateBatch::Change> changes =
          detail::BuildPollSnapshotDiff(snapshot, current);

      snapshot = std::move(current);

      if (!changes.empty() && callback) {
        IndexUpdateBatch batch;
        batch.is_initial = false;
        batch.changes = std::move(changes);
        callback(std::move(batch));
      }
    }
  }
};

#elif defined(_WIN32)

struct FileIndexWatcher::Impl {
  std::atomic<bool> is_native{false};
  std::filesystem::path root;
  FileIndexWatcher::Callback callback;
  // See the __linux__ backend for the threading contract: inputs set before any
  // walk thread starts; each walk constructs its own (non-shareable) filter.
  std::vector<std::string> exclude_globs;
  std::size_t entry_budget = platform::kTreeTraversalEntryBudget;
  bool warned_fallback = false;

  HANDLE dir_handle = INVALID_HANDLE_VALUE;
  HANDLE stop_event = nullptr;
  std::thread worker;
  bool native_active = false;

  bool poll_mode = false;
  std::thread poll_worker;
  std::atomic<bool> stop_poll{false};
  std::thread initial_scan_worker;
  std::atomic<bool> stop_initial_scan{false};

  ~Impl() {
    StopNative();
    StopPoll();
  }

  void StopInitialScan() {
    stop_initial_scan.store(true, std::memory_order_release);
    if (initial_scan_worker.joinable()) {
      initial_scan_worker.join();
    }
    stop_initial_scan.store(false, std::memory_order_release);
  }

  void StopNative() {
    StopInitialScan();
    if (!native_active) {
      return;
    }
    native_active = false;
    if (stop_event != nullptr) {
      SetEvent(stop_event);
    }
    if (worker.joinable()) {
      worker.join();
    }
    if (dir_handle != INVALID_HANDLE_VALUE) {
      CloseHandle(dir_handle);
      dir_handle = INVALID_HANDLE_VALUE;
    }
    if (stop_event != nullptr) {
      CloseHandle(stop_event);
      stop_event = nullptr;
    }
  }

  void StopPoll() {
    StopInitialScan();
    stop_poll.store(true, std::memory_order_release);
    if (poll_worker.joinable()) {
      poll_worker.join();
    }
  }

  void StartInitialScan() {
    StopInitialScan();
    stop_initial_scan.store(false, std::memory_order_release);
    initial_scan_worker = std::thread([this]() {
      project::ProjectTraversalFilter filter(root, exclude_globs);
      IndexUpdateBatch initial =
          BuildInitialBatch(root, &filter, entry_budget, &stop_initial_scan);
      if (!stop_initial_scan.load(std::memory_order_acquire) && callback) {
        callback(std::move(initial));
      }
    });
  }

  bool StartNative() {
    stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (stop_event == nullptr) {
      return false;
    }

    dir_handle = CreateFileW(
        root.wstring().c_str(),
        FILE_LIST_DIRECTORY,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
        nullptr);
    if (dir_handle == INVALID_HANDLE_VALUE) {
      CloseHandle(stop_event);
      stop_event = nullptr;
      return false;
    }

    native_active = true;
    worker = std::thread([this]() { WorkerMain(); });
    return true;
  }

  void WorkerMain() {
    // Single-threaded worker: one filter for its whole lifetime is safe.
    project::ProjectTraversalFilter filter(root, exclude_globs);
    constexpr DWORD kFilter = FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
                              FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE;
    constexpr DWORD kBufSize = 65536;
    std::vector<BYTE> buffer(kBufSize);
    OVERLAPPED overlapped{};
    overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (overlapped.hEvent == nullptr) {
      return;
    }

    auto issue_read = [&]() -> bool {
      ResetEvent(overlapped.hEvent);
      DWORD bytes = 0;
      return ReadDirectoryChangesW(dir_handle, buffer.data(), kBufSize, TRUE, kFilter, &bytes,
                                   &overlapped, nullptr) != 0 ||
             GetLastError() == ERROR_IO_PENDING;
    };

    if (!issue_read()) {
      CloseHandle(overlapped.hEvent);
      return;
    }

    HANDLE wait_handles[2] = {stop_event, overlapped.hEvent};

    while (true) {
      const DWORD wait_result =
          WaitForMultipleObjects(2, wait_handles, FALSE, INFINITE);

      if (wait_result == WAIT_OBJECT_0) {
        // Stop requested. CancelIo only *requests* cancellation of the pending
        // ReadDirectoryChangesW; the kernel may still be about to complete it into
        // `buffer`/`overlapped`, both of which are function-local and destroyed as
        // this returns. Wait for the operation to actually finish (bWait=TRUE, which
        // returns once the abort completes) before freeing them, otherwise the async
        // write lands in freed stack memory (use-after-free).
        CancelIo(dir_handle);
        DWORD drained_bytes = 0;
        GetOverlappedResult(dir_handle, &overlapped, &drained_bytes, TRUE);
        CloseHandle(overlapped.hEvent);
        return;
      }
      if (wait_result != WAIT_OBJECT_0 + 1) {
        CloseHandle(overlapped.hEvent);
        return;
      }

      DWORD bytes_transferred = 0;
      if (!GetOverlappedResult(dir_handle, &overlapped, &bytes_transferred, FALSE) ||
          bytes_transferred == 0) {
        if (!issue_read()) {
          CloseHandle(overlapped.hEvent);
          return;
        }
        continue;
      }

      std::vector<IndexUpdateBatch::Change> changes;
      const BYTE* ptr = buffer.data();
      while (true) {
        const FILE_NOTIFY_INFORMATION* info =
            reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(ptr);
        const std::wstring wname(info->FileName, info->FileNameLength / sizeof(WCHAR));
        const std::filesystem::path rel = std::filesystem::path(wname).lexically_normal();
        if (ShouldIgnoreTrackedRelativePath(root, rel, &filter)) {
          if (info->NextEntryOffset == 0) {
            break;
          }
          ptr += info->NextEntryOffset;
          continue;
        }

        IndexUpdateBatch::Change change;
        if (info->Action == FILE_ACTION_REMOVED || info->Action == FILE_ACTION_RENAMED_OLD_NAME) {
          change.kind = IndexUpdateBatch::Kind::Deleted;
          change.entry.relative_path = rel;
          changes.push_back(std::move(change));
        } else {
          const std::filesystem::path abs_path = (root / rel).lexically_normal();
          std::error_code status_error;
          const auto status = std::filesystem::status(abs_path, status_error);
          if (!status_error && std::filesystem::is_regular_file(status)) {
            change.kind = IndexUpdateBatch::Kind::CreatedOrModified;
            change.entry.relative_path = rel;
            std::error_code mtime_error;
            const auto mtime = std::filesystem::last_write_time(abs_path, mtime_error);
            std::error_code size_error;
            const auto sz = std::filesystem::file_size(abs_path, size_error);
            change.entry.mtime = mtime_error ? std::filesystem::file_time_type{} : mtime;
            change.entry.size = size_error ? 0 : sz;
            changes.push_back(std::move(change));
          }
        }

        if (info->NextEntryOffset == 0) {
          break;
        }
        ptr += info->NextEntryOffset;
      }

      if (!changes.empty() && callback) {
        IndexUpdateBatch batch;
        batch.is_initial = false;
        batch.changes = std::move(changes);
        callback(std::move(batch));
      }

      if (!issue_read()) {
        CloseHandle(overlapped.hEvent);
        return;
      }
    }
  }

  void StartPollFallback() {
    poll_mode = true;
    stop_poll.store(false, std::memory_order_release);
    poll_worker = std::thread([this]() { PollWorkerMain(); });
  }

  void PollWorkerMain() {
    std::map<std::filesystem::path, std::pair<std::filesystem::file_time_type, std::uintmax_t>>
        snapshot;

    auto build_snapshot =
        [&]() -> std::map<std::filesystem::path,
                          std::pair<std::filesystem::file_time_type, std::uintmax_t>> {
      std::map<std::filesystem::path,
               std::pair<std::filesystem::file_time_type, std::uintmax_t>>
          result;
      project::ProjectTraversalFilter filter(root, exclude_globs);
      std::error_code error;
      constexpr auto opts = std::filesystem::directory_options::skip_permission_denied;
      for (std::filesystem::recursive_directory_iterator it(root, opts, error), end;
           !error && it != end; it.increment(error)) {
        std::error_code status_error;
        const auto status = it->status(status_error);
        if (status_error) {
          continue;
        }
        if (std::filesystem::is_directory(status) &&
            ShouldSkipWatchedDirectory(it->path(), &filter)) {
          it.disable_recursion_pending();
          continue;
        }
        if (!std::filesystem::is_regular_file(status)) {
          continue;
        }
        const std::filesystem::path abs_path = it->path().lexically_normal();
        std::filesystem::path rel;
        if (!TryBuildTrackedRelativePath(abs_path, root, rel)) {
          continue;
        }
        if (!filter.Includes(abs_path, platform::PathType::RegularFile)) {
          continue;
        }
        std::error_code mtime_error;
        const auto mtime = std::filesystem::last_write_time(abs_path, mtime_error);
        std::error_code size_error;
        const auto sz = std::filesystem::file_size(abs_path, size_error);
        result[rel] = {mtime_error ? std::filesystem::file_time_type{} : mtime,
                       size_error ? 0 : sz};
      }
      return result;
    };

    snapshot = build_snapshot();

    while (!stop_poll.load(std::memory_order_acquire)) {
      for (int i = 0; i < 15 && !stop_poll.load(std::memory_order_acquire); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
      }
      if (stop_poll.load(std::memory_order_acquire)) {
        break;
      }

      auto current = build_snapshot();
      std::vector<IndexUpdateBatch::Change> changes =
          detail::BuildPollSnapshotDiff(snapshot, current);

      snapshot = std::move(current);

      if (!changes.empty() && callback) {
        IndexUpdateBatch batch;
        batch.is_initial = false;
        batch.changes = std::move(changes);
        callback(std::move(batch));
      }
    }
  }
};

#else

// Unknown platform: poll-only fallback
struct FileIndexWatcher::Impl {
  std::atomic<bool> is_native{false};
  std::filesystem::path root;
  FileIndexWatcher::Callback callback;
  // See the __linux__ backend for the threading contract: inputs set before any
  // walk thread starts; each walk constructs its own (non-shareable) filter.
  std::vector<std::string> exclude_globs;
  std::size_t entry_budget = platform::kTreeTraversalEntryBudget;
  bool warned_fallback = false;
  bool poll_mode = false;
  std::thread poll_worker;
  std::atomic<bool> stop_poll{false};

  ~Impl() { StopPoll(); }

  void StopPoll() {
    stop_poll.store(true, std::memory_order_release);
    if (poll_worker.joinable()) {
      poll_worker.join();
    }
  }

  void StartPollFallback() {
    poll_mode = true;
    stop_poll.store(false, std::memory_order_release);
    poll_worker = std::thread([this]() { PollWorkerMain(); });
  }

  void PollWorkerMain() {
    std::map<std::filesystem::path, std::pair<std::filesystem::file_time_type, std::uintmax_t>>
        snapshot;

    auto build_snapshot =
        [&]() -> std::map<std::filesystem::path,
                          std::pair<std::filesystem::file_time_type, std::uintmax_t>> {
      std::map<std::filesystem::path,
               std::pair<std::filesystem::file_time_type, std::uintmax_t>>
          result;
      project::ProjectTraversalFilter filter(root, exclude_globs);
      std::error_code error;
      constexpr auto opts = std::filesystem::directory_options::skip_permission_denied;
      for (std::filesystem::recursive_directory_iterator it(root, opts, error), end;
           !error && it != end; it.increment(error)) {
        std::error_code status_error;
        const auto status = it->status(status_error);
        if (status_error) {
          continue;
        }
        if (std::filesystem::is_directory(status) &&
            ShouldSkipWatchedDirectory(it->path(), &filter)) {
          it.disable_recursion_pending();
          continue;
        }
        if (!std::filesystem::is_regular_file(status)) {
          continue;
        }
        const std::filesystem::path abs_path = it->path().lexically_normal();
        std::filesystem::path rel;
        if (!TryBuildTrackedRelativePath(abs_path, root, rel)) {
          continue;
        }
        if (!filter.Includes(abs_path, platform::PathType::RegularFile)) {
          continue;
        }
        std::error_code mtime_error;
        const auto mtime = std::filesystem::last_write_time(abs_path, mtime_error);
        std::error_code size_error;
        const auto sz = std::filesystem::file_size(abs_path, size_error);
        result[rel] = {mtime_error ? std::filesystem::file_time_type{} : mtime,
                       size_error ? 0 : sz};
      }
      return result;
    };

    snapshot = build_snapshot();

    while (!stop_poll.load(std::memory_order_acquire)) {
      for (int i = 0; i < 15 && !stop_poll.load(std::memory_order_acquire); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
      }
      if (stop_poll.load(std::memory_order_acquire)) {
        break;
      }

      auto current = build_snapshot();
      std::vector<IndexUpdateBatch::Change> changes =
          detail::BuildPollSnapshotDiff(snapshot, current);

      snapshot = std::move(current);

      if (!changes.empty() && callback) {
        IndexUpdateBatch batch;
        batch.is_initial = false;
        batch.changes = std::move(changes);
        callback(std::move(batch));
      }
    }
  }
};

#endif

// ============================================================
// FileIndexWatcher public API
// ============================================================

FileIndexWatcher::FileIndexWatcher() : impl_(std::make_unique<Impl>()) {}

FileIndexWatcher::~FileIndexWatcher() {
  Unwatch();
}

// Buffering state shared between the wrapper (held by the workers via impl_->callback)
// and Watch()'s per-watch reset. Defined here so the header only forward-declares it.
struct FileIndexWatcher::DispatchState {
  std::mutex mutex;
  bool initial_applied = false;
  std::vector<IndexUpdateBatch> pending;
};

void FileIndexWatcher::SetCallback(Callback callback) {
  auto state = std::make_shared<DispatchState>();
  dispatch_state_ = state;
  // Wrap the client callback so the two concurrent workers dispatch in a defined
  // order: buffer incremental batches until the initial (wholesale-replace) batch
  // lands, then replay them in order on top of the baseline. The mutex serializes
  // the workers so batches apply one at a time. Reset per Watch() via dispatch_state_.
  impl_->callback = [client = std::move(callback), state](IndexUpdateBatch batch) {
    const bool is_initial = batch.is_initial;
    std::lock_guard<std::mutex> lock(state->mutex);
    if (!is_initial && !state->initial_applied) {
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
    }
  };
}

void FileIndexWatcher::SetExcludeGlobs(std::vector<std::string> globs) {
  impl_->exclude_globs = std::move(globs);
}

void FileIndexWatcher::SetEntryBudget(std::size_t max_entries) {
  impl_->entry_budget = max_entries;
}

bool FileIndexWatcher::Watch(const std::filesystem::path& root_path) {
  Unwatch();

  std::error_code error;
  const auto absolute_root = std::filesystem::absolute(root_path, error);
  if (error || !std::filesystem::exists(absolute_root) ||
      !std::filesystem::is_directory(absolute_root)) {
    return false;
  }
  impl_->root = absolute_root.lexically_normal();
  // Reset the dispatch-ordering guard for this watch: Unwatch() above joined the
  // previous watch's workers, so no worker can race this reset. The next initial
  // scan will re-arm initial_applied when its is_initial batch lands.
  if (dispatch_state_) {
    std::lock_guard<std::mutex> lock(dispatch_state_->mutex);
    dispatch_state_->initial_applied = false;
    dispatch_state_->pending.clear();
  }
  // The traversal filter (root .gitignore + built-in defaults + exclude_globs) is
  // constructed per-walk on the walk's own thread; exclude_globs / entry_budget are
  // set via the setters before this call and read race-free by those threads.

#if defined(__linux__) || defined(__APPLE__) || defined(_WIN32)
  // Try native backend
  if (impl_->StartNative()) {
    impl_->is_native.store(true, std::memory_order_release);
    impl_->StartInitialScan();
    return true;
  }

  if (!impl_->warned_fallback) {
    impl_->warned_fallback = true;
    SDL_Log("FileIndexWatcher: native file events unavailable, falling back to poll mode");
  }
#endif

  impl_->StartInitialScan();
  impl_->StartPollFallback();
  return true;
}

void FileIndexWatcher::Unwatch() {
#if defined(__linux__)
  impl_->StopNative();
  impl_->StopPoll();
#elif defined(__APPLE__)
  impl_->StopNative();
  impl_->StopPoll();
#elif defined(_WIN32)
  impl_->StopNative();
  impl_->StopPoll();
#else
  impl_->StopPoll();
#endif
  impl_->is_native.store(false, std::memory_order_release);
  impl_->root.clear();
}

bool FileIndexWatcher::IsNative() const {
  return impl_->is_native.load(std::memory_order_acquire);
}

void FileIndexWatcher::DispatchBatchForTesting(IndexUpdateBatch batch) {
  if (impl_->callback) {
    impl_->callback(std::move(batch));
  }
}

}  // namespace microide::platform
