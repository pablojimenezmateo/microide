#pragma once

#include <cstddef>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace microide::platform {

struct IndexFileEntry {
  std::filesystem::path relative_path;
  std::filesystem::file_time_type mtime{};
  std::uintmax_t size = 0;
};

struct IndexUpdateBatch {
  enum class Kind { CreatedOrModified, Deleted };
  struct Change {
    Kind kind;
    IndexFileEntry entry;  // relative_path always set; mtime/size only for CreatedOrModified
    // Deleted only: relative_path is a directory and every indexed file beneath it
    // should be removed. Set when a watched/nested directory is deleted or moved out,
    // since the OS sends no per-file deletion for its contents.
    bool recursive = false;
  };
  std::vector<Change> changes;
  bool is_initial = false;  // true for first full-scan batch on Watch()
  bool truncated = false;   // true when the walk hit the entry budget and stopped early
};

// Threading contract: The callback registered via SetCallback() fires on the watcher's
// background thread. The consumer must synchronize access to any shared state it reads
// or writes inside the callback.
class FileIndexWatcher {
 public:
  using Callback = std::function<void(IndexUpdateBatch)>;

  FileIndexWatcher();
  ~FileIndexWatcher();

  FileIndexWatcher(const FileIndexWatcher&) = delete;
  FileIndexWatcher& operator=(const FileIndexWatcher&) = delete;

  // Must be called before Watch(). Replaces any previously set callback.
  void SetCallback(Callback callback);

  // User/project-configured ignore globs (gitignore syntax, root-anchored) folded
  // into the traversal filter alongside the built-in defaults. Call before Watch().
  void SetExcludeGlobs(std::vector<std::string> globs);

  // Override the per-walk entry budget for the initial scan (mainly a test seam to
  // trip truncation cheaply). Kept files past this count are dropped and the batch
  // is flagged truncated. Call before Watch(). Defaults to kTreeTraversalEntryBudget.
  void SetEntryBudget(std::size_t max_entries);

  // Start watching root_path recursively. Immediately emits an initial IndexUpdateBatch
  // (is_initial=true) with all files found in the tree. Returns true on success.
  // Subsequent changes emit incremental batches. Falls back to poll mode on failure.
  bool Watch(const std::filesystem::path& root_path);

  // Stop watching and join the background thread. Safe to call multiple times.
  void Unwatch();

  // Returns false if using poll-fallback mode (native events unavailable).
  bool IsNative() const;

  // Test seam: drive the dispatch wrapper directly, as a background worker would,
  // so the initial-vs-incremental ordering guard can be exercised deterministically
  // without racing two real threads.
  void DispatchBatchForTesting(IndexUpdateBatch batch);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  // Ordering guard shared by the (concurrent) initial-scan and native-event workers.
  // The initial-scan and native workers both drive the callback; a non-initial
  // (incremental) batch that arrives before the trailing is_initial batch — which
  // FileIndex::ApplyBatch applies as a wholesale replace — would be lost. SetCallback
  // wraps the client callback to buffer incrementals until the initial batch lands,
  // then replays them in order; Watch resets it (workers are joined by Unwatch first).
  struct DispatchState;
  std::shared_ptr<DispatchState> dispatch_state_;
};

namespace detail {

using FileIndexSnapshot =
    std::map<std::filesystem::path, std::pair<std::filesystem::file_time_type, std::uintmax_t>>;

std::vector<IndexUpdateBatch::Change> BuildPollSnapshotDiff(const FileIndexSnapshot& previous,
                                                            const FileIndexSnapshot& current);

}  // namespace detail

}  // namespace microide::platform
