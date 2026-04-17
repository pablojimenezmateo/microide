#pragma once

#include <chrono>
#include <filesystem>
#include <optional>
#include <vector>

#include "platform/Filesystem.h"

namespace microide::platform {

class FileTreeWatcher {
 public:
  explicit FileTreeWatcher(
      std::chrono::milliseconds poll_interval = std::chrono::milliseconds(750));

  void SetPollInterval(std::chrono::milliseconds poll_interval);
  void SetRoots(std::vector<std::filesystem::path> roots);
  void Clear();

  std::optional<std::chrono::milliseconds> NextPollDelay() const;
  bool Poll();

  const std::vector<std::filesystem::path>& roots() const { return roots_; }

 private:
  void ResetNextPollAt();

  std::chrono::milliseconds poll_interval_;
  std::vector<std::filesystem::path> roots_;
  std::vector<TreeSnapshotEntry> snapshot_;
  std::chrono::steady_clock::time_point next_poll_at_ = std::chrono::steady_clock::time_point::min();
};

}  // namespace microide::platform
