#include "workspace/DebugSession.h"

#include <utility>

namespace microide::workspace {

namespace {

bool IsTerminalState(DebugSession::State state) {
  return state == DebugSession::State::Terminated || state == DebugSession::State::Failed;
}

}  // namespace

const char* DebugSessionStateName(DebugSession::State state) {
  switch (state) {
    case DebugSession::State::Inactive:
      return "inactive";
    case DebugSession::State::Initializing:
      return "initializing";
    case DebugSession::State::Configuring:
      return "configuring";
    case DebugSession::State::Running:
      return "running";
    case DebugSession::State::Stopped:
      return "stopped";
    case DebugSession::State::Terminated:
      return "terminated";
    case DebugSession::State::Failed:
      return "failed";
  }
  return "unknown";
}

DebugSession::DebugSession() : client_(std::make_unique<DapClient>()) {}

DebugSession::DebugSession(std::unique_ptr<DapClient> client) : client_(std::move(client)) {
  if (client_ == nullptr) {
    client_ = std::make_unique<DapClient>();
  }
}

DebugSession::~DebugSession() = default;

void DebugSession::SetWakeEventType(Uint32 event_type) { client_->SetWakeEventType(event_type); }

void DebugSession::SetCallbacks(Callbacks callbacks) { callbacks_ = std::move(callbacks); }

bool DebugSession::IsActive() const {
  switch (state_) {
    case State::Initializing:
    case State::Configuring:
    case State::Running:
    case State::Stopped:
      return true;
    case State::Inactive:
    case State::Terminated:
    case State::Failed:
      return false;
  }
  return false;
}

bool DebugSession::Start(const std::vector<std::string>& command, const LaunchConfig& config,
                         const std::string& cwd, const platform::SubprocessSandbox& sandbox) {
  config_ = config;
  launch_sent_ = false;
  configuration_done_sent_ = false;
  last_error_.clear();

  client_->SetEventCallback(
      [this](const std::string& event, const util::JsonValue& body) { HandleEvent(event, body); });

  if (!client_->Start(command, config_.type, cwd, sandbox)) {
    last_error_ = client_->LastError();
    if (last_error_.empty()) {
      last_error_ = "debug adapter failed to start";
    }
    SetState(State::Failed);
    return false;
  }

  SetState(State::Initializing);
  // The launch/attach request is queued now; the client flushes it once the
  // `initialize` response arrives (DapClient defers pre-initialize requests).
  SendLaunchRequest();
  return true;
}

void DebugSession::SendLaunchRequest() {
  if (launch_sent_) {
    return;
  }
  launch_sent_ = true;
  const std::string verb = config_.IsAttach() ? "attach" : "launch";
  client_->SendRequestAsync(verb, config_.arguments,
                            [this, verb](const dap_protocol::DapResponse& response) {
                              if (!response.success) {
                                last_error_ = response.message.empty()
                                                  ? (verb + " request was rejected")
                                                  : response.message;
                                SetState(State::Failed);
                                client_->BeginShutdown();
                                return;
                              }
                              // Per the DAP handshake the launch response usually
                              // arrives after configurationDone, but some adapters
                              // need no configuration phase — converge to Running.
                              if (state_ == State::Initializing || state_ == State::Configuring) {
                                SetState(State::Running);
                              }
                            });
}

void DebugSession::SendConfigurationDone() {
  if (configuration_done_sent_) {
    return;
  }
  configuration_done_sent_ = true;

  if (!client_->Capabilities().supports_configuration_done_request) {
    // No configuration phase: the adapter is ready to run once launched.
    if (state_ == State::Initializing) {
      SetState(State::Running);
    }
    return;
  }

  if (state_ == State::Initializing) {
    SetState(State::Configuring);
  }
  client_->SendRequestAsync("configurationDone", util::JsonValue(nullptr),
                            [this](const dap_protocol::DapResponse& response) {
                              if (!response.success && last_error_.empty()) {
                                last_error_ = response.message.empty()
                                                  ? "configurationDone was rejected"
                                                  : response.message;
                              }
                              if (state_ == State::Configuring || state_ == State::Initializing) {
                                SetState(State::Running);
                              }
                            });
}

void DebugSession::HandleEvent(const std::string& event, const util::JsonValue& body) {
  if (event == "initialized") {
    // Adapter is ready for configuration (breakpoints in Phase 2). Phase 1 has
    // nothing to configure, so finalize with configurationDone immediately.
    SendConfigurationDone();
  } else if (event == "output") {
    if (callbacks_.on_output) {
      callbacks_.on_output(dap_protocol::ParseOutputEvent(body));
    }
  } else if (event == "terminated" || event == "exited") {
    if (state_ != State::Terminated) {
      SetState(State::Terminated);
      client_->BeginShutdown();
    }
  } else if (event == "stopped") {
    // Execution control is Phase 3; record the coarse state so the UI can react.
    SetState(State::Stopped);
  } else if (event == "continued") {
    if (state_ == State::Stopped) {
      SetState(State::Running);
    }
  }

  if (callbacks_.on_event) {
    callbacks_.on_event(event, body);
  }
}

void DebugSession::RequestStop() {
  if (state_ == State::Inactive || IsTerminalState(state_)) {
    if (client_->IsRunning() && !client_->IsShuttingDown()) {
      client_->BeginShutdown();
    }
    return;
  }
  // Prefer a graceful terminate so a launched debuggee can shut down cleanly;
  // the adapter then emits `terminated`, which drives the disconnect. Adapters
  // without terminate support are disconnected directly.
  if (client_->Capabilities().supports_terminate_request) {
    client_->SendRequestAsync("terminate", util::JsonValue(nullptr), {});
  } else {
    client_->BeginShutdown();
    SetState(State::Terminated);
  }
}

void DebugSession::DrainCallbacks() { client_->DrainCallbacks(); }

void DebugSession::SetState(State state) {
  if (state_ == state) {
    return;
  }
  // Terminal states are absorbing: once failed/terminated, stay there.
  if (IsTerminalState(state_)) {
    return;
  }
  state_ = state;
  if (callbacks_.on_state_changed) {
    callbacks_.on_state_changed(state_);
  }
}

}  // namespace microide::workspace
