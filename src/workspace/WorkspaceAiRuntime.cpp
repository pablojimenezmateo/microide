#include "workspace/WorkspaceAiRuntime.h"

#include <utility>

#include "platform/Subprocess.h"

namespace microide::workspace {

WorkspaceAiRuntime::WorkspaceAiRuntime() = default;

WorkspaceAiRuntime::~WorkspaceAiRuntime() {
  Shutdown();
}

void WorkspaceAiRuntime::Initialize() {
  event_type_ = SDL_RegisterEvents(1);
  if (event_type_ == static_cast<Uint32>(-1)) {
    event_type_ = 0;
  }
}

void WorkspaceAiRuntime::Shutdown() {
  CancelActive();
  std::lock_guard lock(mutex_);
  pending_update_.reset();
  active_request_id_ = 0;
  event_type_ = 0;
}

bool WorkspaceAiRuntime::HandlesEvent(Uint32 type) const {
  return event_type_ != 0 && type == event_type_;
}

std::uint64_t WorkspaceAiRuntime::Start(Request request) {
  CancelActive();

  const std::uint64_t request_id = next_request_id_++;
  {
    std::lock_guard lock(mutex_);
    active_request_id_ = request_id;
    pending_update_.reset();
  }

  executor_.Submit([this, request = std::move(request), request_id](
                       const util::CancellationToken& token) mutable {
    RunRequest(std::move(request), request_id, token);
  });
  return request_id;
}

void WorkspaceAiRuntime::CancelActive() {
  executor_.CancelAll();
}

std::optional<WorkspaceAiRuntime::Update> WorkspaceAiRuntime::ConsumeActiveUpdate() {
  std::lock_guard lock(mutex_);
  if (!pending_update_.has_value() || pending_update_->request_id != active_request_id_) {
    return std::nullopt;
  }

  Update update = std::move(*pending_update_);
  pending_update_.reset();
  if (update.finished) {
    active_request_id_ = 0;
  }
  return update;
}

void WorkspaceAiRuntime::RunRequest(Request request,
                                    std::uint64_t request_id,
                                    const util::CancellationToken& token) {
  Update update{.request_id = request_id};
  if (request.command.empty()) {
    update.finished = true;
    update.succeeded = false;
    update.status_text = "AI request has no command";
    PublishUpdate(std::move(update));
    return;
  }

  const platform::SubprocessResult result =
      platform::RunSubprocess(request.command,
                              platform::SubprocessOptions{
                                  .cwd = request.cwd,
                                  .stdin_text = request.stdin_text,
                              });
  if (token.IsCancellationRequested()) {
    update.finished = true;
    update.succeeded = false;
    update.status_text = "Cancelled";
    PublishUpdate(std::move(update));
    return;
  }

  update.chunk = result.stdout_text;
  update.finished = true;
  update.succeeded = result.success();
  if (!result.success()) {
    update.status_text = "Agent exited with code " + std::to_string(result.exit_code);
    if (!result.stderr_text.empty()) {
      if (!update.chunk.empty()) {
        update.chunk.push_back('\n');
      }
      update.chunk += result.stderr_text;
    }
  } else {
    update.status_text = "Finished";
  }
  PublishUpdate(std::move(update));
}

void WorkspaceAiRuntime::PublishUpdate(Update update) {
  {
    std::lock_guard lock(mutex_);
    if (!pending_update_.has_value() || pending_update_->request_id != update.request_id) {
      pending_update_ = std::move(update);
    } else {
      pending_update_->chunk += update.chunk;
      if (update.finished) {
        pending_update_->finished = true;
        pending_update_->succeeded = update.succeeded;
        pending_update_->status_text = std::move(update.status_text);
      }
    }
  }
  PushWakeEvent();
}

void WorkspaceAiRuntime::PushWakeEvent() const {
  if (event_type_ == 0) {
    return;
  }
  SDL_Event event{};
  event.type = event_type_;
  SDL_PushEvent(&event);
}

}  // namespace microide::workspace
