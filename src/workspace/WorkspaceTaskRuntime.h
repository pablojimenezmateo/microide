#pragma once

#include <SDL3/SDL.h>

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "util/TaskExecutor.h"
#include "workspace/WorkspaceTaskRegistry.h"

namespace microide::workspace {

class WorkspaceTaskRuntime {
 public:
  struct TaskUpdate {
    std::uint64_t run_id = 0;
    std::string task_id;
    std::string channel_id;
    std::string channel_label;
    std::vector<std::string> appended_lines;
    bool finished = false;
    bool succeeded = false;
    std::string status_text;
  };

  WorkspaceTaskRuntime();
  ~WorkspaceTaskRuntime();

  void Initialize();
  void Shutdown();

  bool HandlesEvent(Uint32 type) const;
  std::uint64_t active_run_id() const { return active_run_id_; }

  std::uint64_t Start(const TaskSpec& spec, const std::filesystem::path& project_root);
  void CancelActive();
  std::optional<TaskUpdate> ConsumeActiveUpdate();

 private:
  void RunTask(TaskSpec spec,
               std::filesystem::path project_root,
               std::uint64_t run_id,
               const util::CancellationToken& token);
  void PublishUpdate(TaskUpdate update);
  void PushWakeEvent() const;

  mutable std::mutex mutex_;
  util::TaskExecutor executor_;
  Uint32 event_type_ = 0;
  std::uint64_t next_run_id_ = 1;
  std::uint64_t active_run_id_ = 0;
  std::optional<TaskUpdate> pending_update_;
};

}  // namespace microide::workspace
