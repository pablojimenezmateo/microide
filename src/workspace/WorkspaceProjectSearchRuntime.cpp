#include "workspace/WorkspaceProjectSearchRuntime.h"

#include <string>
#include <utility>
#include <vector>

namespace microide::workspace {

void WorkspaceProjectSearchRuntime::Initialize() {
  event_type_ = SDL_RegisterEvents(1);
  if (event_type_ != static_cast<Uint32>(-1)) {
    service_.SetWakeChannel(event_type_);
    live_hits_mailbox_.SetWakeChannel(event_type_);
  } else {
    event_type_ = 0;
  }
}

void WorkspaceProjectSearchRuntime::Shutdown() {
  Stop();
  live_hits_executor_.CancelAll();
  live_hits_executor_.WaitForIdle();
  live_hits_mailbox_.Clear();
  live_hits_.clear();
  service_.SetWakeChannel(0);
  live_hits_mailbox_.SetWakeChannel(0);
  event_type_ = 0;
}

bool WorkspaceProjectSearchRuntime::HandlesEvent(Uint32 type) const {
  return event_type_ != 0 && type == event_type_;
}

std::uint64_t WorkspaceProjectSearchRuntime::Start(const std::filesystem::path& root,
                                                   std::string query,
                                                   project::ProjectSearchOptions options,
                                                   project::SharedPathList indexed_files,
                                                   const project::ProjectSearchHost* host) {
  active_run_id_ = service_.Start(root, std::move(query), options, std::move(indexed_files), host);
  active_search_id_ = service_.active_search_id();
  return active_run_id_;
}

void WorkspaceProjectSearchRuntime::Stop() {
  service_.Stop();
  active_run_id_ = 0;
  active_search_id_ = 0;
}

std::optional<project::ProjectSearchUpdate> WorkspaceProjectSearchRuntime::ConsumeActiveUpdate() {
  project::ProjectSearchUpdate update = service_.TakePendingUpdate();
  if (update.run_id == 0 || update.run_id != active_run_id_ || update.search_id == 0 ||
      update.search_id != active_search_id_) {
    return std::nullopt;
  }
  if (update.finished) {
    active_run_id_ = 0;
    active_search_id_ = 0;
  }
  return update;
}

void WorkspaceProjectSearchRuntime::SearchDocument(LiveHitsRequest request) {
  std::string key = request.key.relative_path;
  live_hits_executor_.Submit(std::move(key), [this, request = std::move(request)](const util::CancellationToken& token) {
    if (token.IsCancellationRequested()) {
      return;
    }
    LiveHits live = SearchLiveHits(request);
    live_hits_mailbox_.Post([this, live = std::move(live)]() mutable { live_hits_.push_back(std::move(live)); });
  });
}

std::vector<LiveHits> WorkspaceProjectSearchRuntime::TakeLiveHits() {
  (void)live_hits_mailbox_.Drain();
  return std::exchange(live_hits_, {});
}

}
