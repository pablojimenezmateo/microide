// DebugSession execution-control commands (resume / step / pause / restart),
// split out of DebugSession.cpp to keep that translation unit within the debug
// subsystem's per-TU code-line budget. These methods drive state transitions on
// the shared DebugSession and are a cohesive cluster.
#include "workspace/DebugSession.h"

#include <utility>
#include <vector>

#include "util/JsonValue.h"

namespace microide::workspace {

namespace {

// Local copy of DebugSession.cpp's helper: DAP thread-scoped request arguments.
util::JsonValue ThreadIdArgs(int thread_id) {
  util::JsonObject args;
  args["threadId"] = util::JsonValue(static_cast<std::int64_t>(thread_id));
  return util::JsonValue(std::move(args));
}

}  // namespace

void DebugSession::SendResumeRequest(const char* command) {
  if (state_ != State::Stopped) {
    return;
  }
  if (stopped_thread_id_.has_value()) {
    client_->SendRequestAsync(command, ThreadIdArgs(*stopped_thread_id_), {});
  } else {
    // A `stopped` with no threadId whose threads query has not landed yet: resolve
    // a thread and resume it rather than sending threadId:0 (which DAP rejects /
    // misroutes). `command` is a string literal, safe to capture by value.
    RequestThreads([this, command](std::vector<dap_protocol::DapThread> threads) {
      if (!threads.empty()) {
        client_->SendRequestAsync(command, ThreadIdArgs(threads.front().id), {});
      }
    });
  }
  // Optimistically resume so the UI clears immediately; the next `stopped`
  // (breakpoint/step end) repopulates the execution state.
  if (callbacks_.on_resumed) {
    callbacks_.on_resumed();
  }
  SetState(State::Running);
}

void DebugSession::Continue() { SendResumeRequest("continue"); }
void DebugSession::StepOver() { SendResumeRequest("next"); }
void DebugSession::StepIn() { SendResumeRequest("stepIn"); }
void DebugSession::StepOut() { SendResumeRequest("stepOut"); }

void DebugSession::ReverseContinue() {
  if (!client_->Capabilities().supports_step_back) {
    return;
  }
  SendResumeRequest("reverseContinue");
}

void DebugSession::StepBack() {
  if (!client_->Capabilities().supports_step_back) {
    return;
  }
  SendResumeRequest("stepBack");
}

void DebugSession::Pause() {
  if (state_ != State::Running || !client_->IsInitialized()) {
    return;
  }
  // `pause` requires a threadId. When running we have no stopped thread, so ask
  // the adapter for its threads and pause the first one.
  RequestThreads([this](std::vector<dap_protocol::DapThread> threads) {
    // The target may have stopped, terminated, or another action may have run
    // before this threads response landed — only send `pause` if still Running.
    if (state_ != State::Running || threads.empty()) {
      return;
    }
    client_->SendRequestAsync("pause", ThreadIdArgs(threads.front().id), {});
  });
}

void DebugSession::Restart() {
  if (!client_->IsInitialized() || !client_->Capabilities().supports_restart_request) {
    return;
  }
  // A re-emitted `initialized` (some adapters send one on restart) must re-install
  // breakpoints and re-finalize, so re-arm the configurationDone guard.
  configuration_done_sent_ = false;
  // The DAP `restart` request optionally carries the original launch/attach
  // arguments under `arguments`; pass them through when we have them.
  util::JsonObject args;
  if (!config_.arguments.IsNull()) {
    args["arguments"] = config_.arguments;
  }
  client_->SendRequestAsync("restart", util::JsonValue(std::move(args)), {});
  // Optimistically clear the current stop; the adapter re-runs and stops again.
  if (state_ == State::Stopped) {
    if (callbacks_.on_resumed) {
      callbacks_.on_resumed();
    }
    SetState(State::Running);
  }
}

}  // namespace microide::workspace
