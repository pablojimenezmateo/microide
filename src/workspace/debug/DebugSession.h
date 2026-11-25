#pragma once

#include <SDL3/SDL.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <filesystem>

#include "editor/BreakpointStore.h"
#include "editor/FunctionBreakpointStore.h"
#include "platform/SubprocessSandbox.h"
#include "util/JsonValue.h"
#include "workspace/debug/DapProtocol.h"
#include "workspace/debug/LaunchConfig.h"
#include "workspace/debug/WorkspaceDapClient.h"

namespace microide::workspace {

struct ExceptionFilterRequest {
  std::string id;
  std::string condition;
};

class DebugSession {
 public:
  enum class State {
    Inactive,
    Initializing,
    Configuring,
    Running,
    Stopped,
    Terminated,
    Failed,
  };

  struct Callbacks {
    std::function<void(const dap_protocol::DapOutputEvent& output)> on_output;
    std::function<void(State state)> on_state_changed;
    std::function<void(State terminal_state, const std::string& reason)> on_terminated;
    std::function<void(const dap_protocol::DapStoppedEvent& stop)> on_stop_began;
    std::function<void(const dap_protocol::DapStoppedEvent& stop,
                       const std::vector<dap_protocol::DapStackFrame>& frames)>
        on_stopped;
    std::function<void()> on_resumed;
    std::function<void(const std::vector<dap_protocol::DapThread>& threads)> on_threads;
    std::function<std::vector<editor::BreakpointStore::FileBreakpoints>()> breakpoint_provider;
    std::function<void(const std::filesystem::path& path,
                       const std::vector<int>& requested_lines,
                       const std::vector<dap_protocol::DapBreakpoint>& breakpoints)>
        on_breakpoints_verified;
    std::function<void(const std::filesystem::path& path,
                       const dap_protocol::DapBreakpoint& breakpoint)>
        on_breakpoint_changed;
    std::function<std::vector<editor::FunctionBreakpoint>()> function_breakpoint_provider;
    std::function<void(const std::vector<std::string>& requested_names,
                       const std::vector<dap_protocol::DapBreakpoint>& breakpoints)>
        on_function_breakpoints_verified;
    std::function<void(const std::vector<dap_protocol::DapExceptionFilter>& filters)>
        on_exception_filters_available;
    std::function<std::vector<ExceptionFilterRequest>()> exception_filter_provider;
    std::function<void()> on_capabilities_changed;
  };

  DebugSession();
  explicit DebugSession(std::unique_ptr<DapClient> client);
  ~DebugSession();
  DebugSession(const DebugSession&) = delete;
  DebugSession& operator=(const DebugSession&) = delete;

  void SetWakeChannel(Uint32 event_type);
  void SetCallbacks(Callbacks callbacks);

  bool Start(const platform::ProcessLauncher& launcher, const std::vector<std::string>& command,
             const LaunchConfig& config,
             const std::string& cwd = {}, const platform::SubprocessSandbox& sandbox = {});

  State CurrentState() const { return state_; }
  int debuggee_pid() const { return debuggee_pid_; }
  bool IsActive() const;
  const std::string& LastError() const { return last_error_; }
  const LaunchConfig& Config() const { return config_; }

  void DrainCallbacks();

  void RequestStop();

  void NotifyProcessExited();

  void Continue();
  void StepOver();
  void StepIn();
  void StepOut();
  void Pause();
  void ReverseContinue();
  void StepBack();

  void Restart();

  void RequestThreads(std::function<void(std::vector<dap_protocol::DapThread>)> callback);
  void SwitchThread(int thread_id);

  void Reactivate();

  void RequestScopes(int frame_id,
                     std::function<void(std::vector<dap_protocol::DapScope>)> callback);
  void RequestVariables(int variables_reference, int start, int count,
                        std::function<void(bool ok, std::vector<dap_protocol::DapVariable>)> callback);
  void SetVariable(int variables_reference, const std::string& name, const std::string& value,
                   std::function<void(bool ok, dap_protocol::DapSetVariableResult)> callback);

  void RequestEvaluate(const std::string& expression, int frame_id, const std::string& context,
                       std::function<void(bool ok, dap_protocol::DapEvaluateResult)> callback);

  void ResendBreakpointsForFile(const std::filesystem::path& path);

  void ResendFunctionBreakpoints();

  void ResendExceptionFilters();

  DapClient& Client() { return *client_; }
  const DapClient& Client() const { return *client_; }

 private:
  void HandleEvent(const std::string& event, const util::JsonValue& body);
  void RequestStackTrace(const dap_protocol::DapStoppedEvent& stop);
  void ResolveFocusThreadForStop(std::uint64_t epoch);
  void RefreshThreadList();
  void SendResumeRequest(const char* command);
  void SendLaunchRequest();
  void SendAllBreakpoints();
  void SendBreakpointsForFile(const editor::BreakpointStore::FileBreakpoints& file);
  void SendFunctionBreakpoints();
  void SendExceptionFilters();
  void SendConfigurationDone();
  void TransitionToTerminal(State terminal_state, std::string reason);
  void SendDebuggerValueLimits();
  void SetState(State state);
  bool InterruptGdbStillStartingProgram();

  std::unique_ptr<DapClient> client_;
  Callbacks callbacks_{};
  LaunchConfig config_{};
  State state_ = State::Inactive;
  std::string last_error_;
  bool launch_sent_ = false;
  bool configuration_done_sent_ = false;
  bool configuration_done_answered_ = false;
  bool local_adapter_ = false;
  int debuggee_pid_ = 0;
  std::vector<int> known_thread_ids_;
  bool pause_pending_ = false;
  bool is_gdb_adapter_ = false;
  std::optional<int> stopped_thread_id_;
  dap_protocol::DapStoppedEvent last_stop_{};
  std::uint64_t stop_epoch_ = 0;
  bool stop_requested_ = false;
};
}
