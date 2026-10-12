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
    // How often the paths the watcher cannot see (SetPolledPaths) are stat'ed.
    std::chrono::milliseconds poll_interval{2000};
  };
  // At most this many polled paths: past it the rest wait for a full sync.
  static constexpr std::size_t kMaxPolledPaths = 4096;

  // `changed` runs on the watch's own thread, once per batch.
  WorkspaceWatch(std::filesystem::path root, Options options,
                 std::function<void(WorkspaceTree::Changes)> changed);
  ~WorkspaceWatch();
  WorkspaceWatch(const WorkspaceWatch&) = delete;
  WorkspaceWatch& operator=(const WorkspaceWatch&) = delete;

  // Whether the host's notification is in use (false: polling, e.g. inotify watches
  // exhausted — reported to the user, never presented as normal freshness).
  bool native() const { return watcher_.IsNative(); }

  // Content-set members (relative paths) under a directory the watcher prunes — a
  // TRACKED file in build/ or out/, which the scanner's skip list keeps out of the
  // native watch and the poll walk alike. They are stat'ed every poll_interval and a
  // change reports like any other touched path. Replaces the previous set; its
  // baseline is the files as they are now.
  void SetPolledPaths(std::vector<std::string> relative_paths);

 private:
  void OnBatch(platform::IndexUpdateBatch batch);
  void Run();
  // Stat every polled path; changed ones join `pending_`. mutex_ held.
  void PollLocked();

  struct Polled {
    std::string path;
    std::int64_t size = -1;  // -1: absent
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
  std::vector<Polled> polled_;  // guarded by mutex_
  WorkspaceTree::Changes pending_;
  bool has_pending_ = false;
  bool stop_ = false;
  std::thread thread_;
  platform::FileIndexWatcher watcher_;  // last: its thread calls OnBatch
};

}  // namespace microide::server
