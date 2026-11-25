#include "workspace/debug/DebugSession.h"

#include "platform/HostPlatform.h"

#include <utility>
#include <vector>

#include "util/JsonValue.h"
#include "util/InvokeIfSet.h"

namespace microide::workspace {

void DebugSession::SendResumeRequest(const char* command) {
  if (state_ != State::Stopped) {
    return;
  }
  auto on_response = [this](const dap_protocol::DapResponse& response) {
    if (response.success || state_ != State::Running) {
      return;
    }
    SetState(State::Stopped);
    RequestStackTrace(last_stop_);
  };
  if (stopped_thread_id_.has_value()) {
    client_->SendRequestAsync(command, dap_protocol::MakeThreadIdArguments(*stopped_thread_id_), on_response);
  } else {
    RequestThreads([this, command, on_response](std::vector<dap_protocol::DapThread> threads) {
      if (!threads.empty()) {
        client_->SendRequestAsync(command, dap_protocol::MakeThreadIdArguments(threads.front().id), on_response);
      }
    });
  }
  util::InvokeIfSet(callbacks_.on_resumed);
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

bool DebugSession::InterruptGdbStillStartingProgram() {
  return is_gdb_adapter_ && configuration_done_sent_ && !configuration_done_answered_ &&
         debuggee_pid_ > 0 && platform::InterruptProcess(debuggee_pid_);
}

void DebugSession::Pause() {
  if (state_ != State::Running || !client_->IsInitialized()) {
    return;
  }
  if (InterruptGdbStillStartingProgram()) {
    return;
  }
  if (!known_thread_ids_.empty()) {
    client_->SendRequestAsync("pause", dap_protocol::MakeThreadIdArguments(known_thread_ids_.front()), {});
    return;
  }
  pause_pending_ = true;
  RequestThreads([this](std::vector<dap_protocol::DapThread> threads) {
    if (state_ != State::Running || threads.empty() || !pause_pending_) {
      return;
    }
    pause_pending_ = false;
    client_->SendRequestAsync("pause", dap_protocol::MakeThreadIdArguments(threads.front().id), {});
  });
}

void DebugSession::Restart() {
  if (!client_->IsInitialized() || !client_->Capabilities().supports_restart_request) {
    return;
  }
  configuration_done_sent_ = false;
  configuration_done_answered_ = false;
  known_thread_ids_.clear();
  util::JsonObject args;
  if (!config_.arguments.IsNull()) {
    args["arguments"] = config_.arguments;
  }
  client_->SendRequestAsync("restart", util::JsonValue(std::move(args)), {});
  if (state_ == State::Stopped) {
    util::InvokeIfSet(callbacks_.on_resumed);
    SetState(State::Running);
  }
}

}
