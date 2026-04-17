#include "platform/FileWatcher.h"

#include <algorithm>

namespace microide::platform {

FileTreeWatcher::FileTreeWatcher(std::chrono::milliseconds poll_interval)
    : poll_interval_(std::max(poll_interval, std::chrono::milliseconds::zero())) {}

void FileTreeWatcher::SetPollInterval(std::chrono::milliseconds poll_interval) {
  poll_interval_ = std::max(poll_interval, std::chrono::milliseconds::zero());
  ResetNextPollAt();
}

void FileTreeWatcher::SetRoots(std::vector<std::filesystem::path> roots) {
  roots_.clear();
  roots_.reserve(roots.size());
  for (auto& root : roots) {
    if (!root.empty()) {
      roots_.push_back(root.lexically_normal());
    }
  }
  std::sort(roots_.begin(), roots_.end());
  roots_.erase(std::unique(roots_.begin(), roots_.end()), roots_.end());
  snapshot_ = CaptureTreeSnapshot(roots_);
  ResetNextPollAt();
}

void FileTreeWatcher::Clear() {
  roots_.clear();
  snapshot_.clear();
  next_poll_at_ = std::chrono::steady_clock::time_point::min();
}

std::optional<std::chrono::milliseconds> FileTreeWatcher::NextPollDelay() const {
  if (roots_.empty()) {
    return std::nullopt;
  }

  const auto now = std::chrono::steady_clock::now();
  if (now >= next_poll_at_) {
    return std::chrono::milliseconds::zero();
  }
  return std::chrono::duration_cast<std::chrono::milliseconds>(next_poll_at_ - now);
}

bool FileTreeWatcher::Poll() {
  if (roots_.empty()) {
    return false;
  }

  const std::vector<TreeSnapshotEntry> current_snapshot = CaptureTreeSnapshot(roots_);
  const bool changed = current_snapshot != snapshot_;
  snapshot_ = current_snapshot;
  ResetNextPollAt();
  return changed;
}

void FileTreeWatcher::ResetNextPollAt() {
  next_poll_at_ = std::chrono::steady_clock::now() + poll_interval_;
}

}  // namespace microide::platform
