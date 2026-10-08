#pragma once

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <mutex>
#include <thread>

#include "platform/FileWatcher.h"

namespace microide::server {

// Tells a workspace that its tree changed (dev-docs/design/remote-projects.md
// § 6.3): native notification where the host allows it, polling where it does not.
// It reports THAT something changed, after a quiet period, never WHAT — the
// workspace answers "what" by rebuilding its manifest through the hash cache,
// which costs a stat per file and covers what a per-path event cannot (a
// .gitignore edit changing the content set, a branch switch changing thousands).
//
// Agent bursts are the normal traffic: a batch closes `quiet` after the last
// event, or `max_delay` after the first, whichever comes first.
class WorkspaceWatch {
 public:
  struct Options {
    std::chrono::milliseconds quiet{250};
    std::chrono::milliseconds max_delay{1000};
    std::chrono::milliseconds poll_interval{2000};
  };

  // `changed` runs on the watch's own thread, once per batch.
  WorkspaceWatch(std::filesystem::path root, Options options, std::function<void()> changed);
  ~WorkspaceWatch();
  WorkspaceWatch(const WorkspaceWatch&) = delete;
  WorkspaceWatch& operator=(const WorkspaceWatch&) = delete;

  // Whether the host's notification is in use (false: polling, e.g. inotify watches
  // exhausted — reported to the user, never presented as normal freshness).
  bool native() const;

 private:
  void Run();

  Options options_;
  std::function<void()> changed_;
  platform::FileTreeWatcher watcher_;
  mutable std::mutex mutex_;
  std::condition_variable cv_;
  bool woken_ = false;
  bool stop_ = false;
  std::thread thread_;  // last: it uses everything above
};

}  // namespace microide::server
