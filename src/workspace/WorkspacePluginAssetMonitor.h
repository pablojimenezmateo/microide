#pragma once

#include <chrono>
#include <filesystem>
#include <optional>

#include "platform/FileWatcher.h"

namespace microide::workspace {

class WorkspacePluginAssetMonitor {
 public:
  void SetPollInterval(std::chrono::milliseconds poll_interval);
  void SetProjectRoot(const std::filesystem::path& project_root);
  void Reset();

  std::optional<std::chrono::milliseconds> NextPollDelay() const;
  bool PollForChanges();

 private:
  platform::FileTreeWatcher watcher_;
};

}  // namespace microide::workspace
