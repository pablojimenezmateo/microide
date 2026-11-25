#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "platform/FileIndexWatcher.h"
#include "server/WorkspaceTree.h"

namespace microide::server {

class WorkspaceWatch {
 public:
  struct Options {
    std::chrono::milliseconds quiet{250};
    std::chrono::milliseconds max_delay{1000};
    std::chrono::milliseconds poll_interval{2000};
  };
  static constexpr std::size_t kMaxPolledPaths = 4096;

  WorkspaceWatch(std::filesystem::path root, Options options,
                 std::function<void(WorkspaceTree::Changes)> changed);
  ~WorkspaceWatch();
  WorkspaceWatch(const WorkspaceWatch&) = delete;
  WorkspaceWatch& operator=(const WorkspaceWatch&) = delete;

  bool native() const { return watcher_.IsNative(); }

  void SetPolledPaths(std::vector<std::string> relative_paths);

 private:
  void OnBatch(platform::IndexUpdateBatch batch);
  void Run();
  void PollLocked();

  struct Polled {
    std::string path;
    std::int64_t size = -1;
    std::int64_t mtime_ns = 0;
    std::int64_t ctime_ns = 0;
    friend bool operator==(const Polled&, const Polled&) = default;
  };
  static Polled StatPolled(const std::filesystem::path& root, std::string path);

  Options options_;
  std::function<void(WorkspaceTree::Changes)> changed_;
  mutable std::mutex mutex_;
  std::condition_variable cv_;
  std::filesystem::path root_;
  std::vector<Polled> polled_;
  WorkspaceTree::Changes pending_;
  bool has_pending_ = false;
  bool stop_ = false;
  std::thread thread_;
  platform::FileIndexWatcher watcher_;
};

}
