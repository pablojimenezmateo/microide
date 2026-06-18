#pragma once

#include <SDL3/SDL.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <memory>
#include <string>

#include "platform/FileWatcher.h"

namespace microide::workspace {

class WorkspaceProjectFileMonitor {
 public:
  WorkspaceProjectFileMonitor();
  ~WorkspaceProjectFileMonitor();

  void SetDeferredArming(bool deferred);
  void SetPollInterval(std::chrono::milliseconds poll_interval);
  // Override the watcher's per-walk entry budget (mainly a testing seam to trip the
  // "too large" degradation cheaply without building a 50k-entry tree).
  void SetEntryBudget(std::size_t max_entries);
  void SetWakeEventType(Uint32 event_type);
  // Inject a "post a deduped background task" capability (typically
  // ProjectBackgroundExecutor::PostLatest). When set, periodic polling runs the
  // recursive tree walk off the shell thread; when unset (tests/headless), polls
  // run synchronously.
  void SetBackgroundPoster(std::function<void(std::string, std::function<void()>)> poster);
  bool ConsumeWakeEvent(Uint32 type);
  void SetProjectRoot(const std::filesystem::path& project_root);
  void ArmPendingWatch();
  void Reset();

  std::optional<std::chrono::milliseconds> NextPollDelay() const;
  bool PollForChanges();
  bool ConsumePendingChanges();
  // True exactly once after the watched tree was found too large to live-watch, so
  // the shell can surface a one-time notification. Consumed on the shell thread.
  bool ConsumeTreeTooLargeNotice();

 private:
  class ProjectTraversalFilter;

  bool EnsureWatching();
  bool HasVisibleChangesSinceDeferredArming() const;
  bool ReserveWakeEvent(Uint32* event_type) const;
  void PushWakeEvent() const;
  void ScheduleBackgroundPoll();
  void RunPollNow();
  void SignalTreeTooLargeIfNeeded();
  void ResetChangeSignals();

  mutable std::mutex wake_mutex_;
  mutable std::mutex state_mutex_;
  Uint32 wake_event_type_ = 0;
  mutable bool wake_event_pending_ = false;
  bool deferred_arming_ = false;
  std::uint64_t arm_generation_ = 0;
  std::filesystem::path pending_project_root_;
  std::filesystem::path watched_project_root_;
  std::optional<std::filesystem::file_time_type> deferred_arm_baseline_;
  platform::FileTreeWatcher watcher_;
  std::shared_ptr<ProjectTraversalFilter> traversal_filter_;

  // Off-shell-thread polling. background_poster_ is set once at startup and only
  // touched on the shell thread; the flags below cross to the executor thread.
  std::function<void(std::string, std::function<void()>)> background_poster_;
  std::chrono::milliseconds poll_interval_{0};
  std::atomic<bool> tree_change_pending_{false};
  std::atomic<bool> background_poll_scheduled_{false};
  std::atomic<bool> tree_too_large_pending_{false};
  std::atomic<bool> tree_too_large_notified_{false};
};

}  // namespace microide::workspace
