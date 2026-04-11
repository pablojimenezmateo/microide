#include "workspace/WorkspaceProjectSearchRuntime.h"

namespace microide::workspace {

void WorkspaceProjectSearchRuntime::Initialize() {
  event_type_ = SDL_RegisterEvents(1);
  if (event_type_ != static_cast<Uint32>(-1)) {
    service_.SetWakeEventType(event_type_);
  } else {
    event_type_ = 0;
  }
}

void WorkspaceProjectSearchRuntime::Shutdown() {
  Stop();
  service_.SetWakeEventType(0);
  event_type_ = 0;
}

bool WorkspaceProjectSearchRuntime::HandlesEvent(Uint32 type) const {
  return event_type_ != 0 && type == event_type_;
}

std::uint64_t WorkspaceProjectSearchRuntime::Start(const std::filesystem::path& root,
                                                   std::string query,
                                                   project::ProjectSearchOptions options) {
  active_run_id_ = service_.Start(root, std::move(query), options);
  return active_run_id_;
}

void WorkspaceProjectSearchRuntime::Stop() {
  service_.Stop();
  active_run_id_ = 0;
}

std::optional<project::ProjectSearchUpdate> WorkspaceProjectSearchRuntime::ConsumeActiveUpdate() {
  project::ProjectSearchUpdate update = service_.TakePendingUpdate();
  if (update.run_id == 0 || update.run_id != active_run_id_) {
    return std::nullopt;
  }
  if (update.finished) {
    active_run_id_ = 0;
  }
  return update;
}

}  // namespace microide::workspace
