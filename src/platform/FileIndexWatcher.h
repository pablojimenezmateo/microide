#pragma once

#include <filesystem>
#include <functional>
#include <memory>
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
  };
  std::vector<Change> changes;
  bool is_initial = false;  // true for first full-scan batch on Watch()
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

  // Start watching root_path recursively. Immediately emits an initial IndexUpdateBatch
  // (is_initial=true) with all files found in the tree. Returns true on success.
  // Subsequent changes emit incremental batches. Falls back to poll mode on failure.
  bool Watch(const std::filesystem::path& root_path);

  // Stop watching and join the background thread. Safe to call multiple times.
  void Unwatch();

  // Returns false if using poll-fallback mode (native events unavailable).
  bool IsNative() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace microide::platform
