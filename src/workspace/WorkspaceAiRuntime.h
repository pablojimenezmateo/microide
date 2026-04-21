#pragma once

#include <SDL3/SDL.h>

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "util/TaskExecutor.h"

namespace microide::workspace {

class WorkspaceAiRuntime {
 public:
  struct Request {
    std::string capability;
    std::vector<std::string> command;
    std::filesystem::path cwd;
    std::string stdin_text;
  };

  struct Update {
    std::uint64_t request_id = 0;
    std::string chunk;
    bool finished = false;
    bool succeeded = false;
    std::string status_text;
  };

  WorkspaceAiRuntime();
  ~WorkspaceAiRuntime();

  void Initialize();
  void Shutdown();

  bool HandlesEvent(Uint32 type) const;
  std::uint64_t active_request_id() const { return active_request_id_; }

  std::uint64_t Start(Request request);
  void CancelActive();
  std::optional<Update> ConsumeActiveUpdate();

 private:
  void RunRequest(Request request,
                  std::uint64_t request_id,
                  const util::CancellationToken& token);
  void PublishUpdate(Update update);
  void PushWakeEvent() const;

  mutable std::mutex mutex_;
  util::TaskExecutor executor_;
  Uint32 event_type_ = 0;
  std::uint64_t next_request_id_ = 1;
  std::uint64_t active_request_id_ = 0;
  std::optional<Update> pending_update_;
};

}  // namespace microide::workspace
