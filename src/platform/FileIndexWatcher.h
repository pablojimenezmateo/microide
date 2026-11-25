#pragma once

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "platform/OpenFileWatcher.h"

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
    IndexFileEntry entry;
    bool recursive = false;
  };
  std::vector<Change> changes;
  bool is_initial = false;
  bool truncated = false;
  bool tree_structure_changed = false;
};

class FileIndexWatcher {
 public:
  using Callback = std::function<void(IndexUpdateBatch)>;

  FileIndexWatcher();
  ~FileIndexWatcher();

  FileIndexWatcher(const FileIndexWatcher&) = delete;
  FileIndexWatcher& operator=(const FileIndexWatcher&) = delete;

  void SetCallback(Callback callback);

  void SetExcludeGlobs(std::vector<std::string> globs);

  void SetEntryBudget(std::size_t max_entries);

  void SetForcePollForTesting(bool force_poll);
  void SetPollIntervalForTesting(std::chrono::milliseconds interval);

  bool Watch(const std::filesystem::path& root_path);

  void Unwatch();

  bool IsNative() const;

  OpenFileWatcher& WatchOpenFiles(OpenFileWatcher::Callback callback);
  OpenFileWatcher* open_files() { return open_files_.get(); }

  void DispatchBatchForTesting(IndexUpdateBatch batch);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  struct DispatchState;
  std::shared_ptr<DispatchState> dispatch_state_;
  std::unique_ptr<OpenFileWatcher> open_files_;
};

namespace detail {

using FileIndexSnapshot =
    std::map<std::filesystem::path, std::pair<std::filesystem::file_time_type, std::uintmax_t>>;

std::vector<IndexUpdateBatch::Change> BuildPollSnapshotDiff(const FileIndexSnapshot& previous,
                                                            const FileIndexSnapshot& current);

}

}
