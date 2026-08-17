#include "platform/FileIndexWatcher.h"

#include "platform/DescriptorRetire.h"
#include "platform/Filesystem.h"
#include "platform/HostPlatform.h"
#include "project/ProjectTraversalFilter.h"
#include "util/PathMatch.h"
#include "util/PerformanceCounters.h"
#include "util/PerformanceTrace.h"
#include "util/StringUtil.h"

#include <SDL3/SDL.h>

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
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
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

// Stamp an index entry's (mtime, size) from ONE stat.
//
// Every site below used to call `last_write_time` and then `file_size` on the
// same path -- two syscalls for one inode, per file, on walks that visit tens of
// thousands. `ReadFileMetadata` answers both from a single stat and reports the
// same values (Filesystem/ReadFileMetadataMatchesStdFilesystem pins that), which
// matters here because a poll snapshot taken by one path is diffed against an
// initial batch taken by another.
//
// An unreadable path (deleted between readdir and here, permission revoked)
// leaves the zero stamp the callers already used for a failed stat.
void StampEntryMetadata(const std::filesystem::path& absolute_path, IndexFileEntry& entry) {
  if (const std::optional<FileMetadata> metadata = ReadFileMetadata(absolute_path)) {
    entry.mtime = metadata->mtime;
    entry.size = metadata->size;
    return;
  }
  entry.mtime = {};
  entry.size = 0;
}

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
  // A view, not a copy: this runs once per filesystem entry of the initial walk
  // and of every poll re-walk, and `it->string()` is an allocation per entry.
  const std::string_view first = it->native();
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
  // `it->native()` rather than a `path("..")` temporary: constructing that path
  // per filesystem entry is an allocation for a two-character comparison.
  return it != relative_path.end() && std::string_view(it->native()) == "..";
}


bool TryComputeRelativePath(const std::filesystem::path& absolute_path,
                            const std::filesystem::path& root,
                            std::filesystem::path& relative_path) {
  // Per filesystem entry of the initial walk and of every poll re-walk. Both
  // inputs are normalized by their producers (the walk normalizes each entry, the
  // impl normalizes the root once at watch start), and `lexically_normal()` is
  // ~12 allocations even when it changes nothing — so confirm with the
  // allocation-free scan and only pay for an unusually spelled input
  // (TD-2026-08-10-174).
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

  // The overwhelmingly common case: the entry sits under the root, so the
  // relative part is the root prefix removed — one string instead of the
  // `lexically_relative()` component walk and its path temporaries.
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
  // `rel` comes out of TryComputeRelativePath already normal in every case but an
  // oddly spelled input; the guard keeps the ~12-allocation no-op off the walk.
  if (util::PathTextNeedsNormalizing(rel.native())) {
    rel = rel.lexically_normal();
  }
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
  return !filter->Includes(util::NormalizedPath(root / relative_path),
                           platform::PathType::RegularFile);
}
#endif

// Called once per KEPT directory (post-prune, pre-descend) by the walk below.
// The native backend registers an inotify watch here, which is what lets the
// watch registration and the file scan share one traversal instead of running
// two identical ones over the same tree.
using DirectoryVisitor = std::function<void(const std::filesystem::path&)>;

// Build an initial IndexUpdateBatch by scanning root recursively, pruning excluded
// directories (VCS/build/deps/nested-.gitignore) and files, and stopping once
// `max_entries` kept files have been collected (batch.truncated is then set).
// Shared tree walk: append a CreatedOrModified change for every tracked regular file
// under `walk_root`, with paths relative to `index_root`, stopping once `changes`
// reaches `max_entries`. Returns true if that budget was hit (truncated). Both the
// initial full scan and the incremental subtree scans (moved-in dir, overflow
// recovery) go through here so the ignore/prune/relative-path rules stay identical.
//
// `on_directory`, when set, is invoked for every directory the filter keeps. Every
// caller that wanted the directory set wanted the file set from the same tree at
// the same moment (project open, a moved-in subtree, overflow recovery), and each
// was walking twice to get them.
bool CollectTrackedCreations(const std::filesystem::path& walk_root,
                             const std::filesystem::path& index_root,
                             project::ProjectTraversalFilter* filter,
                             std::size_t max_entries,
                             const std::atomic<bool>* stop_requested,
                             std::vector<IndexUpdateBatch::Change>* changes,
                             const DirectoryVisitor& on_directory = {}) {
  bool truncated = false;
  std::error_code error;
  constexpr auto options = std::filesystem::directory_options::skip_permission_denied;
  // Reused across the whole walk: `NormalizedPathView` only writes into it for an
  // oddly-spelled entry, which a directory iterator does not produce. The owning
  // `NormalizedPath` form here was a path COPY -- two allocations per file of the
  // tree -- to reproduce its own input.
  std::filesystem::path normalize_scratch;
  for (std::filesystem::recursive_directory_iterator it(walk_root, options, error), end;
       !error && it != end; it.increment(error)) {
    if (stop_requested != nullptr && stop_requested->load(std::memory_order_acquire)) {
      break;
    }
    std::error_code status_error;
    // EntryPathType, not status(): the type readdir already reported, instead of
    // one newfstatat per entry of the whole tree (see platform/Filesystem.h).
    const platform::PathType type = platform::EntryPathType(*it, status_error);
    if (status_error) {
      continue;
    }
    if (type == platform::PathType::Directory) {
      // Prune excluded subtrees before descending so they cost zero budget.
      if (ShouldSkipWatchedDirectory(it->path(), filter)) {
        it.disable_recursion_pending();
        continue;
      }
      if (on_directory) {
        on_directory(it->path());
      }
      continue;
    }
    if (type != platform::PathType::RegularFile) {
      continue;
    }
    if (truncated) {
      continue;  // file budget spent; still walking so watches keep being registered
    }
    const std::filesystem::path& abs_path = util::NormalizedPathView(it->path(), normalize_scratch);
    // File-level ignore check (parity with the finder/tree) FIRST: an ignored file
    // whose parent directory was not itself pruned is still excluded from the
    // index, and most of what a walk sees is that case. On this repo the walk
    // visits ~52,000 entries and keeps 8,000 — the other 44,000 are individually
    // ignored files (fuzz corpora, generated fixtures) sitting in directories that
    // are not themselves ignored, so they cannot be pruned.
    //
    // `Includes` derives its own relative view and allocates nothing.
    // `TryBuildTrackedRelativePath` materializes a `std::filesystem::path`, which
    // libstdc++ builds with an eager component list — two allocations. Running it
    // before the rejection meant paying both for every ignored file. Both are pure
    // predicates, so the order is free to change; every rejection either one made
    // before, it still makes.
    if (filter != nullptr && !filter->Includes(abs_path, platform::PathType::RegularFile)) {
      continue;
    }
    std::filesystem::path rel;
    if (!TryBuildTrackedRelativePath(abs_path, index_root, rel)) {
      continue;
    }
    if (changes->size() >= max_entries) {
      truncated = true;
      // Without a directory visitor there is nothing left to collect, so stop.
      // With one, the walk continues for watches only: a tree whose file count
      // exceeds the budget still has to be watched in full, exactly as the
      // separate registration walk used to do.
      if (!on_directory) {
        return true;
      }
      continue;
    }
    IndexUpdateBatch::Change change;
    change.kind = IndexUpdateBatch::Kind::CreatedOrModified;
    change.entry.relative_path = std::move(rel);
    StampEntryMetadata(abs_path, change.entry);
    changes->push_back(std::move(change));
  }
  return truncated;
}

#if defined(__APPLE__) || defined(_WIN32)
// Standalone initial scan, for the backends whose watch registration is not a
// tree walk and so has nothing to share one with. On Linux the registration walk
// IS the initial scan (see Impl::WalkSubtree), which is why this is guarded
// rather than merely unused there.
IndexUpdateBatch BuildInitialBatch(const std::filesystem::path& root,
                                   project::ProjectTraversalFilter* filter,
                                   std::size_t max_entries,
                                   const std::atomic<bool>* stop_requested = nullptr) {
  // Full recursive walk of the project tree. Runs once per watch on a background
  // thread, and it is the dominant single cost of opening a project.
  util::PerformanceTrace::Scope perf_scope("watch::BuildInitialBatch");
  IndexUpdateBatch batch;
  batch.is_initial = true;
  batch.truncated =
      CollectTrackedCreations(root, root, filter, max_entries, stop_requested, &batch.changes);
  return batch;
}
#endif

// Stop flag plus wakeup channel for the poll-fallback worker. The worker used to
// sleep its interval in fixed slices purely so a stop request was noticed
// promptly, which burned ~15 pointless wakeups per idle 750ms interval; waiting
// on the condition variable costs ONE, and a stop is observed immediately rather
// than up to a slice late.
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
  // Blocks up to `timeout`, returning false as soon as a stop is requested.
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

// One tree walk producing the poll backend's snapshot (relative path -> mtime+size).
// Filtering is deliberately identical to CollectTrackedCreations (directory prune,
// tracked-relative check, file-level ignore), so a baseline seeded from an initial
// batch and a snapshot taken here describe the same file set.
//
// `out_directory_signature` (optional) accumulates a hash of every KEPT directory
// the walk visited. The snapshot itself holds regular files only, so an empty
// directory created or removed between two polls is invisible in the diff; the
// signature is what lets the poll backend still report `tree_structure_changed`.
// It is combined commutatively (recursive_directory_iterator order is not
// contractual) and costs one hash of an existing string per directory.
detail::FileIndexSnapshot BuildPollSnapshot(const std::filesystem::path& root,
                                            const std::vector<std::string>& exclude_globs,
                                            std::uint64_t* out_directory_signature = nullptr) {
  // The poll fallback re-walks the whole tree every interval, forever, on any
  // host without native watch events. That is a standing CPU cost with no user
  // action behind it, so it needs to be visible in an idle-soak summary -- the
  // call count alone tells you whether the native backend is actually in use.
  util::PerformanceTrace::Scope perf_scope("watch::BuildPollSnapshot");
  util::AddPerformanceCounter(util::PerfCounterId::FileWatcherPollScans);
  detail::FileIndexSnapshot result;
  std::uint64_t directory_signature = 0;
  project::ProjectTraversalFilter filter(root, exclude_globs);
  std::error_code error;
  constexpr auto opts = std::filesystem::directory_options::skip_permission_denied;
  std::filesystem::path normalize_scratch;
  for (std::filesystem::recursive_directory_iterator it(root, opts, error), end;
       !error && it != end; it.increment(error)) {
    std::error_code status_error;
    const platform::PathType type = platform::EntryPathType(*it, status_error);
    if (status_error) {
      continue;
    }
    if (type == platform::PathType::Directory) {
      if (ShouldSkipWatchedDirectory(it->path(), &filter)) {
        it.disable_recursion_pending();
        continue;
      }
      if (out_directory_signature != nullptr) {
        // Commutative combine (sum), so the walk order never changes the answer.
        // The odd multiplier keeps a rename that permutes names from cancelling.
        directory_signature +=
            std::hash<std::string_view>{}(std::string_view(it->path().native())) * 0x9E3779B9ull +
            0x632BE59Bull;
      }
      continue;
    }
    if (type != platform::PathType::RegularFile) {
      continue;
    }
    // Same shape as CollectTrackedCreations above, for the same reason and with
    // more force: this loop runs every poll interval, forever, so an ignored file
    // costs its two relative-path allocations again on every scan. Reject first,
    // materialize after. The path view avoids the owning copy of an input that is
    // already normal.
    const std::filesystem::path& abs_path = util::NormalizedPathView(it->path(), normalize_scratch);
    if (!filter.Includes(abs_path, platform::PathType::RegularFile)) {
      continue;
    }
    std::filesystem::path rel;
    if (!TryBuildTrackedRelativePath(abs_path, root, rel)) {
      continue;
    }
    const std::optional<FileMetadata> metadata = ReadFileMetadata(abs_path);
    result[std::move(rel)] = {metadata ? metadata->mtime : std::filesystem::file_time_type{},
                            metadata ? metadata->size : 0};
  }
  if (out_directory_signature != nullptr) {
    *out_directory_signature = directory_signature;
  }
  return result;
}

// The poll fallback, shared by every backend (the four per-platform Impls carried
// byte-identical copies of this loop).
//
// It emits the INITIAL batch itself and seeds its diff baseline from that same
// walk. That single-walk ownership is load-bearing, not a tidy-up: the initial
// scan used to run on its own thread while the poll worker took a second,
// independent baseline walk, so a file created between the two walks was absent
// from the initial batch AND already present in the baseline — no diff would ever
// report it and it stayed invisible to the index for the life of the watch (a
// file deleted in that window stayed in the index forever, symmetrically). The
// window is normally sub-millisecond, which is why it only ever surfaced as a
// rare flake in the poll-backend contract test under load.
void RunPollFallbackWorker(const std::filesystem::path& root,
                           const std::vector<std::string>& exclude_globs,
                           std::size_t entry_budget,
                           std::chrono::milliseconds poll_interval,
                           PollStopSignal& stop_poll,
                           const FileIndexWatcher::Callback& callback) {
  // ONE walk feeds both outputs. The baseline is the full snapshot (budget-free,
  // as the poll baseline has always been) while the initial batch it derives is
  // entry-budgeted, so a tree past the budget still reports only `entry_budget`
  // files and the later diffs do not smuggle the remainder in one interval later.
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
    // One blocking wait per interval: an idle watch parks the thread instead of
    // ticking, and Unwatch() wakes it at once rather than after a sleep slice.
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
  bool force_poll = false;
  std::chrono::milliseconds poll_interval{750};
  std::thread poll_worker;
  PollStopSignal stop_poll;

  ~Impl() {
    StopNative();
    StopPoll();
  }

  void StopNative() {
    // No initial-scan thread to stop: the setup thread's single walk produces the
    // initial batch, and stop_native_setup below is what aborts it.
    // Tell the setup thread to bail and unblock the worker (if it's already running).
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
      // Setup thread aborted (or never ran successfully); just clean up any FDs it left behind.
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

  // Release every inotify watch this instance holds.
  //
  // Closing the inotify descriptor is all it takes: inotify(7) guarantees that
  // once the last descriptor referring to an instance is closed, the kernel frees
  // the object AND every watch associated with it. Both teardown paths used to
  // walk `wd_to_path` calling inotify_rm_watch per entry first -- one syscall per
  // watched directory, on the shell thread (Unwatch runs there on every project
  // switch and close), immediately before the close that would have done it
  // anyway. This tree keeps one watch per directory, so on a real source
  // checkout that is thousands of redundant syscalls per switch.
  //
  // The watch count goes in the label because it is the one number that says
  // whether a teardown stall is the kernel freeing a big watch table or (as it
  // turned out to be here, on a fixture holding ~20 watches) the shell thread
  // being descheduled while the background index scans run.
  void DropAllWatchesAndCloseInotify() {
    // Retired rather than closed inline: every thread that could touch this
    // descriptor has been joined by the time we get here, and closing an inotify
    // fd blocks for milliseconds at random (see RetireDescriptorAsync). Unwatch()
    // runs on the shell thread on every project switch and close.
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

  // Walk `dir` once, registering an inotify watch for every kept directory and —
  // when `changes` is non-null — collecting the tracked files in the SAME pass.
  //
  // Every caller here needs both halves of that tree, and each used to get them
  // from two identical traversals: project open ran the registration walk on the
  // setup thread while the initial-scan thread ran BuildInitialBatch over the same
  // entries; a moved-in directory registered watches and then re-walked for
  // AppendSubtreeCreations; overflow recovery registered and then re-walked for
  // BuildInitialBatch. The traversal is the expensive part (~55k entries, ~130 ms
  // on this repo) and the prune rules were already required to be identical, so
  // they are now literally one walk.
  //
  // Returns false only on inotify watch-limit exhaustion (ENOSPC) or our own watch
  // budget; other per-watch errors are skipped. Neither stops the file half: the
  // walk keeps collecting after a watch failure, and keeps registering after the
  // file budget is spent, because a partial index and a partial watch set are
  // independent degradations and merging the walks must not couple them.
  //
  // Periodically checks stop_native_setup so an Unwatch() during bootstrap returns
  // promptly.
  bool WalkSubtree(const std::filesystem::path& dir,
                   project::ProjectTraversalFilter* filter,
                   std::vector<IndexUpdateBatch::Change>* changes,
                   bool* files_truncated = nullptr) {
    // Best-effort, and deliberately NOT an early return: when the two walks were
    // separate, a watch budget exhausted at the subtree root still left the file
    // scan to run, so a directory moved in at the inotify limit still entered the
    // index. Bailing here instead would have made the merged walk lose those files.
    bool added = false;
    bool watches_ok = AddSingleWatch(dir, filter, added);

    const DirectoryVisitor register_watch = [&](const std::filesystem::path& subdir_path) {
      if (!watches_ok) {
        return;
      }
      if (wd_to_path.size() >= kMaxIndexWatchEntries) {
        watches_ok = false;  // budget exhausted -> partial-tree degradation
        return;
      }
      std::filesystem::path subdir = util::NormalizedPath(subdir_path);
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

    // Watches only: max_entries = 0 trips the file budget on the first regular
    // file, after which the walk skips every file and keeps registering watches.
    std::vector<IndexUpdateBatch::Change> discarded;
    CollectTrackedCreations(dir, root, filter, /*max_entries=*/0, &stop_native_setup,
                            &discarded, register_watch);
    return watches_ok;
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
      // ONE walk of the tree, producing both halves of what a project open needs:
      // an inotify watch per kept directory, and the initial file batch. This used
      // to be two threads walking the same ~55k entries with the same filter (this
      // one, and StartInitialScan's) — the same directories read, classified and
      // ignore-matched twice. WalkSubtree re-uses the existing root watch
      // (inotify_add_watch returns the same wd for it).
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
        SDL_Log(
            "FileIndexWatcher: inotify watch limit exhausted; tracking partial tree only");
      }
      // Start the read loop BEFORE dispatching, so events that arrive while the
      // consumer applies the batch are queued rather than dropped. Kernel-queued
      // events from the root watch are drained by this worker either way.
      worker = std::thread([this]() { WorkerMain(); });
      if (callback) {
        callback(std::move(initial));
      }
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
      // Set by any event that moves the tree's SHAPE rather than a tracked file's
      // content: a directory appearing/vanishing, or a non-regular entry the index
      // cannot represent. See IndexUpdateBatch::tree_structure_changed.
      bool structure_changed = false;
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
              const std::filesystem::path abs_path = util::NormalizedPath(dir / name);

              if (is_dir && (ev->mask & (IN_CREATE | IN_MOVED_TO)) != 0) {
                // New directory: add watches for it recursively. Build a fresh
                // filter per event (rare) so this single-threaded worker never
                // shares the filter's mutable cache with another walk.
                project::ProjectTraversalFilter filter(root, exclude_globs);
                // A moved-in directory can already be populated, and inotify emits no
                // per-file events for files that existed before the move. One walk
                // registers the new subtree's watches AND enqueues creations for the
                // files that were already in it.
                // An EMPTY new directory adds no file change at all, so without this
                // the sidebar tree would not learn about `mkdir` until something else
                // happened to wake it. Gated on the same prune the walk applies, so
                // a build creating `node_modules/` does not request a refresh of a
                // tree that will not show it.
                if (!ShouldSkipWatchedDirectory(abs_path, &filter)) {
                  structure_changed = true;
                }
                WalkSubtree(abs_path, &filter, &changes);
              } else if (is_dir && (ev->mask & (IN_DELETE | IN_MOVED_FROM)) != 0) {
                // A subdirectory was deleted or moved out. inotify sends no per-file
                // deletion for its contents, so emit one recursive deletion and let the
                // index drop every entry beneath it (otherwise they linger as ghosts).
                if (!batch_filter.ShouldSkipDirectory(abs_path)) {
                  structure_changed = true;
                }
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
                    if (!std::filesystem::is_regular_file(abs_path, type_error)) {
                      // Not a regular file, but it IS a new entry in a watched
                      // directory: a symlink to a directory, a fifo, a socket. The
                      // index cannot hold it, the sidebar tree shows it, so report
                      // it as a shape change instead of dropping the event. Gated on
                      // the ignore filter so a build writing ignored artifacts does
                      // not turn into a refresh storm.
                      if (!type_error &&
                          batch_filter.Includes(abs_path, platform::PathType::Other)) {
                        structure_changed = true;
                      }
                      continue;
                    }
                    if (!batch_filter.Includes(abs_path, platform::PathType::RegularFile)) {
                      continue;
                    }
                    change.kind = IndexUpdateBatch::Kind::CreatedOrModified;
                    change.entry.relative_path = rel;
                    StampEntryMetadata(abs_path, change.entry);
                  }
                  changes.push_back(std::move(change));
                }
              }
            } else if ((ev->mask & (IN_DELETE_SELF | IN_MOVE_SELF)) != 0) {
              // The watched directory itself was removed/moved. Drop the watch AND
              // emit a recursive deletion for it, or every file it contained lingers
              // in the index as a ghost until a full rescan.
              structure_changed = true;
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
        //
        // One walk for both, as at project open. WalkSubtree observes
        // stop_native_setup, which stays true across the worker join, so a
        // teardown arriving mid-recovery bails promptly instead of blocking
        // worker.join() for the whole scan budget.
        project::ProjectTraversalFilter recovery_filter(root, exclude_globs);
        IndexUpdateBatch resync;
        resync.is_initial = true;
        // The dropped window may have contained directory creations/removals, and a
        // wholesale index replace says nothing about them: flag the resync so the
        // coarse consumers (sidebar tree, finder, search) rebuild too.
        resync.tree_structure_changed = true;
        WalkSubtree(root, &recovery_filter, &resync.changes, &resync.truncated);
        if (callback) {
          callback(std::move(resync));
        }
        continue;
      }

      if ((!changes.empty() || structure_changed) && callback) {
        IndexUpdateBatch batch;
        batch.is_initial = false;
        batch.changes = std::move(changes);
        batch.tree_structure_changed = structure_changed;
        callback(std::move(batch));
      }
    }
  }

  void StartPollFallback() {
    poll_mode = true;
    stop_poll.Reset();
    // Take initial snapshot
    poll_worker = std::thread([this]() {
      RunPollFallbackWorker(root, exclude_globs, entry_budget, poll_interval, stop_poll, callback);
    });
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
  bool force_poll = false;
  std::chrono::milliseconds poll_interval{750};
  std::thread poll_worker;
  PollStopSignal stop_poll;
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
      util::PerformanceTrace::Scope scope("watch::StopInitialScan::Join");
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
    stop_poll.Stop();
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
    bool structure_changed = false;

    for (size_t i = 0; i < num_events; ++i) {
      const FSEventStreamEventFlags flags = event_flags[i];
      const std::filesystem::path abs_path =
          std::filesystem::path(paths[i]).lexically_normal();

      const bool is_dir =
          (flags & kFSEventStreamEventFlagItemIsDir) != 0;
      if (is_dir) {
        // The index holds regular files only, so a directory event carries no
        // change — but it does move the tree's shape, which the coarse consumers
        // (sidebar tree, finder, search) subscribe to. Only creates/removes/renames
        // qualify; a plain content event on a directory is noise.
        constexpr FSEventStreamEventFlags kShapeFlags =
            kFSEventStreamEventFlagItemCreated | kFSEventStreamEventFlagItemRemoved |
            kFSEventStreamEventFlagItemRenamed;
        if ((flags & kShapeFlags) != 0) {
          structure_changed = true;
        }
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
        StampEntryMetadata(abs_path, change.entry);
      }
      changes.push_back(std::move(change));
    }

    if (!changes.empty() || structure_changed) {
      IndexUpdateBatch batch;
      batch.is_initial = false;
      batch.changes = std::move(changes);
      batch.tree_structure_changed = structure_changed;
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
    stop_poll.Reset();
    poll_worker = std::thread([this]() {
      RunPollFallbackWorker(root, exclude_globs, entry_budget, poll_interval, stop_poll, callback);
    });
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
  bool force_poll = false;
  std::chrono::milliseconds poll_interval{750};
  std::thread poll_worker;
  PollStopSignal stop_poll;
  std::thread initial_scan_worker;
  std::atomic<bool> stop_initial_scan{false};

  ~Impl() {
    StopNative();
    StopPoll();
  }

  void StopInitialScan() {
    stop_initial_scan.store(true, std::memory_order_release);
    if (initial_scan_worker.joinable()) {
      util::PerformanceTrace::Scope scope("watch::StopInitialScan::Join");
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
    stop_poll.Stop();
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
      bool structure_changed = false;
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
          if (!status_error && !std::filesystem::is_regular_file(status)) {
            // A directory (or other non-regular entry) appearing/renamed: no index
            // change, but the tree's shape moved. See tree_structure_changed.
            structure_changed = true;
          }
          if (!status_error && std::filesystem::is_regular_file(status)) {
            change.kind = IndexUpdateBatch::Kind::CreatedOrModified;
            change.entry.relative_path = rel;
            StampEntryMetadata(abs_path, change.entry);
            changes.push_back(std::move(change));
          }
        }

        if (info->NextEntryOffset == 0) {
          break;
        }
        ptr += info->NextEntryOffset;
      }

      if ((!changes.empty() || structure_changed) && callback) {
        IndexUpdateBatch batch;
        batch.is_initial = false;
        batch.changes = std::move(changes);
        batch.tree_structure_changed = structure_changed;
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
    stop_poll.Reset();
    poll_worker = std::thread([this]() {
      RunPollFallbackWorker(root, exclude_globs, entry_budget, poll_interval, stop_poll, callback);
    });
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
  bool force_poll = false;
  std::chrono::milliseconds poll_interval{750};
  std::thread poll_worker;
  PollStopSignal stop_poll;

  ~Impl() { StopPoll(); }

  void StopPoll() {
    stop_poll.Stop();
    if (poll_worker.joinable()) {
      util::PerformanceTrace::Scope scope("watch::StopPoll::Join");
      poll_worker.join();
    }
  }

  void StartPollFallback() {
    poll_mode = true;
    stop_poll.Reset();
    poll_worker = std::thread([this]() {
      RunPollFallbackWorker(root, exclude_globs, entry_budget, poll_interval, stop_poll, callback);
    });
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
  // Aggregate change/batch counters for the pre-initial buffer so a slow initial
  // scan plus rapid create/delete churn cannot retain unbounded path batches before
  // the wholesale-replace baseline lands. Once either budget is hit, further
  // pre-initial batches are dropped (newest-first) and `pending_overflow` records
  // it; the earlier buffered batches (which carry the oldest deltas) still replay.
  // TD-2026-07-17A-107.
  std::size_t pending_change_count = 0;
  bool pending_overflow = false;
  static constexpr std::size_t kMaxPendingChanges = 200000;
  static constexpr std::size_t kMaxPendingBatches = 4096;
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
      // Bound the pre-initial buffer: drop the batch (newest-first) once either the
      // aggregate change budget or the batch-count budget is exceeded, so churn
      // during a slow initial scan cannot retain unbounded path batches.
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
  // Probe with the non-throwing overloads: the root can be on a disconnected mount,
  // behind a permission-denied parent, or contain a symlink-loop/status edge that
  // would make the throwing exists()/is_directory raise straight out of watcher setup
  // instead of degrading to "cannot watch". Any probe error is a graceful false.
  std::error_code exists_error;
  std::error_code dir_error;
  if (!std::filesystem::exists(absolute_root, exists_error) || exists_error ||
      !std::filesystem::is_directory(absolute_root, dir_error) || dir_error) {
    return false;
  }
  impl_->root = util::NormalizedPath(absolute_root);
  // Reset the dispatch-ordering guard for this watch: Unwatch() above joined the
  // previous watch's workers, so no worker can race this reset. The next initial
  // scan will re-arm initial_applied when its is_initial batch lands.
  if (dispatch_state_) {
    std::lock_guard<std::mutex> lock(dispatch_state_->mutex);
    dispatch_state_->initial_applied = false;
    dispatch_state_->pending.clear();
    dispatch_state_->pending_change_count = 0;
    dispatch_state_->pending_overflow = false;
  }
  // The traversal filter (root .gitignore + built-in defaults + exclude_globs) is
  // constructed per-walk on the walk's own thread; exclude_globs / entry_budget are
  // set via the setters before this call and read race-free by those threads.

#if defined(__linux__) || defined(__APPLE__) || defined(_WIN32)
  // Try native backend (unless a test forced the poll fallback so the
  // backend-independent contract suite can exercise both paths on one host).
  if (!impl_->force_poll) {
    if (impl_->StartNative()) {
      impl_->is_native.store(true, std::memory_order_release);
      // No StartInitialScan() here: the native setup thread's single walk produces
      // the initial batch as well as the watch registrations (see StartNative).
      return true;
    }
    if (!impl_->warned_fallback) {
      impl_->warned_fallback = true;
      SDL_Log("FileIndexWatcher: native file events unavailable, falling back to poll mode");
    }
  }
#endif

  // Poll fallback: the poll worker owns the initial batch too. Starting a separate
  // initial-scan thread here would race it — a file created between the two walks
  // would be missing from the initial batch and already present in the poll
  // baseline, so no diff would ever surface it (see RunPollFallbackWorker).
  impl_->StartPollFallback();
  return true;
}

void FileIndexWatcher::Unwatch() {
  // Every branch below joins background threads, and this runs on the shell
  // thread from StopFileIndexWatcher (project switch / close). Scope the halves
  // so a switch stall can be attributed to the backend that actually blocked.
  util::PerformanceTrace::Scope perf_scope("watch::Unwatch");
#if defined(__linux__) || defined(__APPLE__) || defined(_WIN32)
  {
    util::PerformanceTrace::Scope scope("watch::Unwatch::StopNative");
    impl_->StopNative();
  }
  {
    util::PerformanceTrace::Scope scope("watch::Unwatch::StopPoll");
    impl_->StopPoll();
  }
#else
  {
    util::PerformanceTrace::Scope scope("watch::Unwatch::StopPoll");
    impl_->StopPoll();
  }
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
