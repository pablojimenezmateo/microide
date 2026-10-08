#pragma once

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <mutex>
#include <thread>

#include "platform/FileIndexWatcher.h"
#include "server/WorkspaceTree.h"

namespace microide::server {

// Tells a workspace what in its tree changed (dev-docs/design/remote-projects.md
// § 6.3): native notification where the host allows it, polling where it does not.
// It reports the paths a batch touched — the workspace re-stats and re-hashes only
// those — and asks for a full rebuild when the tree's shape changed in a way paths
// cannot describe.
//
// Agent bursts are the normal traffic: a batch closes `quiet` after the last
// event, or `max_delay` after the first, whichever comes first.
class WorkspaceWatch {
 public:
  struct Options {
    std::chrono::milliseconds quiet{250};
    std::chrono::milliseconds max_delay{1000};
  };

  // `changed` runs on the watch's own thread, once per batch.
  WorkspaceWatch(std::filesystem::path root, Options options,
                 std::function<void(WorkspaceTree::Changes)> changed);
  ~WorkspaceWatch();
  WorkspaceWatch(const WorkspaceWatch&) = delete;
  WorkspaceWatch& operator=(const WorkspaceWatch&) = delete;

  // Whether the host's notification is in use (false: polling, e.g. inotify watches
  // exhausted — reported to the user, never presented as normal freshness).
  bool native() const { return watcher_.IsNative(); }

 private:
  void OnBatch(platform::IndexUpdateBatch batch);
  void Run();

  Options options_;
  std::function<void(WorkspaceTree::Changes)> changed_;
  mutable std::mutex mutex_;
  std::condition_variable cv_;
  WorkspaceTree::Changes pending_;
  bool has_pending_ = false;
  bool stop_ = false;
  std::thread thread_;
  platform::FileIndexWatcher watcher_;  // last: its thread calls OnBatch
};

}  // namespace microide::server
