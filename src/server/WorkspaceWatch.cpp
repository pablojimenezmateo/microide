#include "server/WorkspaceWatch.h"

#include <utility>

#if defined(__unix__) || defined(__APPLE__)
#include <sys/stat.h>
#endif

namespace microide::server {

WorkspaceWatch::WorkspaceWatch(std::filesystem::path root, Options options,
                               std::function<void(WorkspaceTree::Changes)> changed)
    : options_(options), changed_(std::move(changed)), root_(root) {
  thread_ = std::thread([this]() { Run(); });
  watcher_.SetCallback([this](platform::IndexUpdateBatch batch) { OnBatch(std::move(batch)); });
  (void)watcher_.Watch(root);  // false: it polls, which native() reports
}

WorkspaceWatch::~WorkspaceWatch() {
  watcher_.Unwatch();
  {
    std::lock_guard lock(mutex_);
    stop_ = true;
  }
  cv_.notify_all();
  if (thread_.joinable()) {
    thread_.join();
  }
}

void WorkspaceWatch::OnBatch(platform::IndexUpdateBatch batch) {
  if (batch.is_initial) {
    return;  // the workspace already has a manifest; this is the watcher's baseline
  }
  {
    std::lock_guard lock(mutex_);
    for (const platform::IndexUpdateBatch::Change& change : batch.changes) {
      std::string path = change.entry.relative_path.generic_string();
      if (change.kind == platform::IndexUpdateBatch::Kind::Deleted && change.recursive) {
        pending_.deleted_directories.push_back(path);
      }
      pending_.touched.push_back(std::move(path));
    }
    // A shape change (a directory created or moved, a link, dropped events) or a
    // walk that hit its budget: paths cannot describe it.
    pending_.full = pending_.full || batch.tree_structure_changed || batch.truncated;
    has_pending_ = true;
  }
  cv_.notify_all();
}

WorkspaceWatch::Polled WorkspaceWatch::StatPolled(const std::filesystem::path& root, std::string path) {
  Polled polled;
#if defined(__unix__) || defined(__APPLE__)
  struct stat info {};
  if (::lstat((root / path).c_str(), &info) == 0) {
    polled.size = static_cast<std::int64_t>(info.st_size);
    polled.mtime_ns = static_cast<std::int64_t>(info.st_mtim.tv_sec) * 1'000'000'000 + info.st_mtim.tv_nsec;
    polled.ctime_ns = static_cast<std::int64_t>(info.st_ctim.tv_sec) * 1'000'000'000 + info.st_ctim.tv_nsec;
  }
#endif
  polled.path = std::move(path);
  return polled;
}

void WorkspaceWatch::SetPolledPaths(std::vector<std::string> relative_paths) {
  if (relative_paths.size() > kMaxPolledPaths) {
    relative_paths.resize(kMaxPolledPaths);
  }
  std::vector<Polled> polled;
  polled.reserve(relative_paths.size());
  for (std::string& path : relative_paths) {
    polled.push_back(StatPolled(root_, std::move(path)));
  }
  {
    std::lock_guard lock(mutex_);
    polled_ = std::move(polled);
  }
  cv_.notify_all();  // an idle Run starts (or stops) its poll timer
}

void WorkspaceWatch::PollLocked() {
  bool any = false;
  for (Polled& polled : polled_) {
    Polled now = StatPolled(root_, polled.path);
    if (!(now == polled)) {
      pending_.touched.push_back(polled.path);
      polled = std::move(now);
      any = true;
    }
  }
  has_pending_ = has_pending_ || any;
}

void WorkspaceWatch::Run() {
  std::unique_lock lock(mutex_);
  while (!stop_) {
    // Idle: wait for the watcher, and with paths to poll, for the poll tick too.
    while (!stop_ && !has_pending_) {
      if (polled_.empty()) {
        cv_.wait(lock, [this] { return stop_ || has_pending_ || !polled_.empty(); });
      } else if (!cv_.wait_for(lock, options_.poll_interval, [this] { return stop_ || has_pending_; })) {
        PollLocked();
      }
    }
    if (stop_) {
      return;
    }
    // Let the burst settle: every further event restarts the quiet period, up to
    // max_delay after the first.
    const auto first = std::chrono::steady_clock::now();
    for (;;) {
      has_pending_ = false;
      const auto deadline =
          std::min(std::chrono::steady_clock::now() + options_.quiet, first + options_.max_delay);
      if (!cv_.wait_until(lock, deadline, [this] { return stop_ || has_pending_; }) || stop_ ||
          std::chrono::steady_clock::now() >= first + options_.max_delay) {
        break;
      }
    }
    if (stop_) {
      return;
    }
    WorkspaceTree::Changes changes = std::exchange(pending_, {});
    has_pending_ = false;
    lock.unlock();
    if (changed_) {
      changed_(std::move(changes));
    }
    lock.lock();
  }
}

}  // namespace microide::server
