#include "server/WorkspaceWatch.h"

#include <utility>

namespace microide::server {

WorkspaceWatch::WorkspaceWatch(std::filesystem::path root, Options options,
                               std::function<void(WorkspaceTree::Changes)> changed)
    : options_(options), changed_(std::move(changed)) {
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

void WorkspaceWatch::Run() {
  std::unique_lock lock(mutex_);
  while (!stop_) {
    cv_.wait(lock, [this] { return stop_ || has_pending_; });
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
