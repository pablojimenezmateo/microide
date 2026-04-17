#pragma once

#include <SDL3/SDL.h>

#include <chrono>
#include <filesystem>
#include <mutex>
#include <optional>

#include "platform/FileWatcher.h"

namespace microide::workspace {

class WorkspacePluginAssetMonitor {
 public:
  void SetPollInterval(std::chrono::milliseconds poll_interval);
  void SetWakeEventType(Uint32 event_type);
  bool ConsumeWakeEvent(Uint32 type);
  void SetProjectRoot(const std::filesystem::path& project_root);
  void Reset();

  std::optional<std::chrono::milliseconds> NextPollDelay() const;
  bool PollForChanges();
  bool ConsumePendingChanges();

 private:
  bool ReserveWakeEvent(Uint32* event_type) const;
  void PushWakeEvent() const;

  mutable std::mutex wake_mutex_;
  Uint32 wake_event_type_ = 0;
  mutable bool wake_event_pending_ = false;
  platform::FileTreeWatcher watcher_;
};

}  // namespace microide::workspace
