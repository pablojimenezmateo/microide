#include "TestSupport.h"
#include "support/MockDapAdapter.h"

#include "editor/BreakpointStore.h"
#include "editor/FunctionBreakpointStore.h"
#include "util/JsonValue.h"
#include "workspace/debug/DapProtocol.h"
#include "workspace/debug/DebugBreakpointsModel.h"
#include "workspace/debug/DebugFrameFocus.h"
#include "workspace/debug/DebugService.h"
#include "workspace/CommandSummary.h"
#include "workspace/debug/DebugSession.h"
#include "workspace/debug/DebugVariablesModel.h"
#include "workspace/debug/DebugViewModel.h"
#include "workspace/debug/DebugWatchModel.h"
#include "workspace/debug/LaunchConfig.h"
#include "workspace/WorkspaceContext.h"
#include "workspace/debug/WorkspaceDapManager.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <signal.h>
#include <string>
#include <thread>
#include <vector>

namespace microide::tests {
namespace {

using microide::util::JsonValue;
using microide::workspace::DapManager;
using microide::workspace::DebugSession;
using microide::workspace::DebugHoverModel;
using microide::workspace::DebugValueKind;
using microide::workspace::DebugValueTree;
using microide::workspace::DebugVariablesModel;
using microide::workspace::DebugWatchModel;
using microide::workspace::LaunchConfig;
namespace codec = microide::workspace::dap_protocol;

std::vector<std::string> GdbFlavoredAdapterCommand(const std::filesystem::path& server_path,
                                                   const std::string& mode) {
  return {"python3", server_path.string(), mode, "gdb"};
}

constexpr std::chrono::milliseconds kAdapterHangCeiling{60000};

bool PollUntil(DapManager& manager, const std::function<bool()>& predicate) {
  return WaitUntil(predicate, kAdapterHangCeiling, std::chrono::milliseconds(5),
                   [&manager]() { manager.DrainCallbacks(); });
}

bool PollStaysFalse(DapManager& manager,
                    const std::function<bool()>& predicate,
                    std::chrono::milliseconds settle_window = std::chrono::milliseconds(2000)) {
  return WaitUntil(predicate, settle_window, std::chrono::milliseconds(5),
                   [&manager]() { manager.DrainCallbacks(); });
}

struct CapturedSession {
  std::vector<DebugSession::State> states;
  std::string output;
  bool got_terminated_event = false;
  bool got_terminated_callback = false;
  bool terminated_failed = false;
  std::string terminated_reason;
  std::vector<codec::DapStackFrame> last_frames;
  std::string last_stop_reason;
  int stop_count = 0;
  int resume_count = 0;
  std::vector<codec::DapThread> last_threads;
  std::vector<codec::DapExceptionFilter> advertised_filters;
  int capabilities_changed_count = 0;
};

DebugSession::Callbacks MakeCallbacks(CapturedSession& captured) {
  DebugSession::Callbacks callbacks;
  callbacks.on_output = [&captured](const codec::DapOutputEvent& output) {
    captured.output += output.output;
  };
  callbacks.on_state_changed = [&captured](DebugSession::State state) {
    captured.states.push_back(state);
    if (state == DebugSession::State::Terminated) {
      captured.got_terminated_event = true;
    }
  };
  callbacks.on_terminated = [&captured](DebugSession::State terminal_state,
                                        const std::string& reason) {
    captured.got_terminated_callback = true;
    captured.terminated_failed = terminal_state == DebugSession::State::Failed;
    captured.terminated_reason = reason;
  };
  callbacks.on_stopped = [&captured](const codec::DapStoppedEvent& stop,
                                     const std::vector<codec::DapStackFrame>& frames) {
    captured.last_stop_reason = stop.reason;
    captured.last_frames = frames;
    ++captured.stop_count;
  };
  callbacks.on_resumed = [&captured]() { ++captured.resume_count; };
  callbacks.on_threads = [&captured](const std::vector<codec::DapThread>& threads) {
    captured.last_threads = threads;
  };
  callbacks.on_exception_filters_available =
      [&captured](const std::vector<codec::DapExceptionFilter>& filters) {
        captured.advertised_filters = filters;
      };
  callbacks.on_capabilities_changed = [&captured]() { ++captured.capabilities_changed_count; };
  return callbacks;
}

bool SawState(const CapturedSession& captured, DebugSession::State state) {
  return std::find(captured.states.begin(), captured.states.end(), state) != captured.states.end();
}

void TestDebugSessionDrivesLaunchLifecycleWithConfigurationDone() {
  TemporaryDirectory temp_dir;
  const auto server_path = temp_dir.path() / "adapter.py";
  WriteFile(server_path, std::string(MockAdapterSource()));

  DapManager manager;
  manager.RegisterAdapter("mock", platform::LocalProcessLauncher(), MockAdapterCommand(server_path, "config_done"));
  Expect(manager.HasAdapter("mock"), "adapter should be registered");

  CapturedSession captured;
  LaunchConfig config;
  config.name = "Launch";
  config.type = "mock";
  config.request = "launch";
  config.arguments = *util::ParseJson(R"({"program":"main.py"})");

  Expect(manager.StartSession(config, MakeCallbacks(captured)),
         "session should start against the registered adapter");

  Expect(PollUntil(manager,
                   [&]() {
                     const auto* session = manager.ActiveSession();
                     return session != nullptr &&
                            session->CurrentState() == DebugSession::State::Terminated;
                   }),
         "session should reach Terminated after the adapter emits terminated");

  Expect(SawState(captured, DebugSession::State::Initializing), "session should pass Initializing");
  Expect(SawState(captured, DebugSession::State::Running),
         "configurationDone should drive the session to Running");
  Expect(SawState(captured, DebugSession::State::Terminated), "session should end Terminated");
  Expect(captured.output.find("hello from adapter") != std::string::npos,
         "adapter output event should reach the console callback");
  Expect(captured.got_terminated_event, "terminated should drive the session to Terminated");
  Expect(captured.got_terminated_callback, "on_terminated fires on a clean terminated event");
  Expect(!captured.terminated_failed, "a clean exit reports failed=false");
  Expect(captured.terminated_reason.empty(), "a clean exit carries no reason");
  manager.ShutdownAll();
}

void TestDebugSessionRunsWithoutConfigurationDoneSupport() {
  TemporaryDirectory temp_dir;
  const auto server_path = temp_dir.path() / "adapter.py";
  WriteFile(server_path, std::string(MockAdapterSource()));

  DapManager manager;
  manager.RegisterAdapter("mock", platform::LocalProcessLauncher(), MockAdapterCommand(server_path, "no_config_done"));

  CapturedSession captured;
  LaunchConfig config;
  config.type = "mock";
  config.request = "launch";

  Expect(manager.StartSession(config, MakeCallbacks(captured)), "session should start");
  Expect(PollUntil(manager,
                   [&]() {
                     const auto* session = manager.ActiveSession();
                     return session != nullptr &&
                            session->CurrentState() == DebugSession::State::Terminated;
                   }),
         "session should terminate even without a configurationDone phase");
  Expect(SawState(captured, DebugSession::State::Running),
         "adapter without configurationDone should still reach Running");
  Expect(captured.output.find("hello from adapter") != std::string::npos,
         "output should stream without a configuration phase");
  manager.ShutdownAll();
}

void TestDebugSessionLaunchHandshakeOrder() {
  TemporaryDirectory temp_dir;
  const auto server_path = temp_dir.path() / "adapter.py";
  WriteFile(server_path, std::string(MockAdapterSource()));

  DapManager manager;
  manager.RegisterAdapter("mock", platform::LocalProcessLauncher(), MockAdapterCommand(server_path, "config_done"));

  CapturedSession captured;
  DebugSession::Callbacks callbacks = MakeCallbacks(captured);

  const std::filesystem::path source_path = "/proj/main.py";
  callbacks.breakpoint_provider = [&]() {
    editor::BreakpointStore::FileBreakpoints file;
    file.path = source_path;
    file.breakpoints.push_back(editor::Breakpoint{.line = 4});
    file.breakpoints.push_back(editor::Breakpoint{.line = 9});
    return std::vector<editor::BreakpointStore::FileBreakpoints>{file};
  };
  std::vector<codec::DapBreakpoint> verified;
  std::vector<int> verified_requested_lines;
  std::filesystem::path verified_path;
  callbacks.on_breakpoints_verified =
      [&](const std::filesystem::path& path, const std::vector<int>& requested_lines,
          const std::vector<codec::DapBreakpoint>& breakpoints) {
        verified_path = path;
        verified_requested_lines = requested_lines;
        verified = breakpoints;
      };

  LaunchConfig config;
  config.type = "mock";
  config.request = "launch";
  Expect(manager.StartSession(config, std::move(callbacks)), "session should start");

  Expect(PollUntil(manager,
                   [&]() {
                     const auto* session = manager.ActiveSession();
                     return session != nullptr &&
                            session->CurrentState() == DebugSession::State::Terminated;
                   }),
         "session should reach Terminated");

  Expect(verified.size() == 2, "adapter should verify both breakpoints");
  if (verified.size() == 2) {
    Expect(verified[0].line == 5 && verified[1].line == 10,
           "0-based store lines should be sent as 1-based DAP lines");
    Expect(verified[0].verified && verified[1].verified, "breakpoints should report verified");
  }
  Expect(verified_path == source_path, "verification should carry the source path");
  Expect(verified_requested_lines == std::vector<int>({5, 10}),
         "verification should carry the requested 1-based lines in send order");

  Expect(!captured.terminated_failed, "spec-compliant handshake must not fail the session");

  const std::size_t order_pos = captured.output.find("commands:");
  Expect(order_pos != std::string::npos, "adapter should emit its command order");
  const std::size_t launch_pos = captured.output.find("launch", order_pos);
  const std::size_t set_pos = captured.output.find("setBreakpoints", order_pos);
  const std::size_t cfg_pos = captured.output.find("configurationDone", order_pos);
  Expect(launch_pos != std::string::npos && set_pos != std::string::npos &&
             cfg_pos != std::string::npos,
         "adapter should record launch, setBreakpoints, and configurationDone");
  Expect(set_pos < launch_pos,
         "setBreakpoints must be sent before launch, or an adapter that runs on "
         "launch never arms them");
  Expect(launch_pos < cfg_pos, "launch must be sent before configurationDone");
  manager.ShutdownAll();
}

void TestDebugSessionResolvesStackOnStopAndStepsResume() {
  TemporaryDirectory temp_dir;
  const auto server_path = temp_dir.path() / "adapter.py";
  WriteFile(server_path, std::string(MockAdapterSource()));

  DapManager manager;
  manager.RegisterAdapter("mock", platform::LocalProcessLauncher(), MockAdapterCommand(server_path, "stop"));

  CapturedSession captured;
  LaunchConfig config;
  config.type = "mock";
  config.request = "launch";
  Expect(manager.StartSession(config, MakeCallbacks(captured)), "session should start");

  Expect(PollUntil(manager, [&]() { return captured.stop_count >= 1; }),
         "session should resolve a stack on the first stop");
  Expect(captured.last_stop_reason == "breakpoint", "stop reason should propagate");
  Expect(captured.last_frames.size() == 2, "both stack frames should resolve");
  if (captured.last_frames.size() == 2) {
    Expect(captured.last_frames[0].name == "main" && captured.last_frames[0].line == 10,
           "top frame should be the 1-based DAP line from the adapter");
    Expect(captured.last_frames[0].source.path == "/proj/main.py",
           "top frame should carry its source path");
  }

  DebugSession* session = manager.ActiveSession();
  Expect(session != nullptr, "session should be active while stopped");
  const int stops_before = captured.stop_count;
  session->StepOver();
  Expect(PollUntil(manager, [&]() { return captured.stop_count > stops_before; }),
         "step over should drive another stop");
  Expect(captured.output.find("cmd:next") != std::string::npos,
         "StepOver should send the DAP `next` command");
  Expect(captured.resume_count >= 1, "stepping should fire on_resumed optimistically");

  const auto step_and_wait = [&](const char* dap_command, auto&& send) {
    const int stops = captured.stop_count;
    const std::string marker = std::string("cmd:") + dap_command;
    send();
    Expect(PollUntil(manager,
                     [&]() {
                       return captured.output.find(marker) != std::string::npos &&
                              captured.stop_count > stops;
                     }),
           std::string("the session should send the DAP `") + dap_command +
               "` command and stop again (the mock adapter re-stops after every resume)");
  };
  step_and_wait("stepIn", [&] { session->StepIn(); });
  step_and_wait("stepOut", [&] { session->StepOut(); });
  session->Continue();
  Expect(PollUntil(manager,
                   [&]() { return captured.output.find("cmd:continue") != std::string::npos; }),
         "Continue should send the DAP `continue` command");
  manager.ShutdownAll();
}

void TestDebugSessionDropsStaleStackResolvedAfterResume() {
  TemporaryDirectory temp_dir;
  const auto server_path = temp_dir.path() / "adapter.py";
  WriteFile(server_path, std::string(MockAdapterSource()));

  DapManager manager;
  manager.RegisterAdapter("mock", platform::LocalProcessLauncher(), MockAdapterCommand(server_path, "stale_stack"));

  CapturedSession captured;
  LaunchConfig config;
  config.type = "mock";
  config.request = "launch";
  Expect(manager.StartSession(config, MakeCallbacks(captured)), "session should start");

  Expect(PollUntil(manager,
                   [&]() {
                     const auto* s = manager.ActiveSession();
                     return s != nullptr && s->CurrentState() == DebugSession::State::Stopped;
                   }),
         "session should reach Stopped while the stack is still pending");
  Expect(captured.stop_count == 0, "on_stopped must not fire before the stack resolves");

  DebugSession* session = manager.ActiveSession();
  Expect(session != nullptr, "session should be active while stopped");
  session->Continue();
  Expect(session->CurrentState() == DebugSession::State::Running,
         "Continue flips the session to Running synchronously");

  Expect(PollUntil(manager,
                   [&]() {
                     return captured.output.find("stale-stack-flushed") != std::string::npos;
                   }),
         "adapter should flush the stale stack after the resume");

  Expect(captured.stop_count == 0,
         "a stackTrace resolving after a resume must not re-project a stopped view");
  Expect(captured.resume_count == 1, "the resume should have fired exactly once");
  Expect(manager.ActiveSession() == nullptr ||
             manager.ActiveSession()->CurrentState() != DebugSession::State::Stopped,
         "the session must not be dragged back to Stopped by the stale stack");
  manager.ShutdownAll();
}

void TestDebugSessionRejectedResumeRestoresStoppedView() {
  TemporaryDirectory temp_dir;
  const auto server_path = temp_dir.path() / "adapter.py";
  WriteFile(server_path, std::string(MockAdapterSource()));

  DapManager manager;
  manager.RegisterAdapter("mock", platform::LocalProcessLauncher(), MockAdapterCommand(server_path, "reject_resume"));

  CapturedSession captured;
  LaunchConfig config;
  config.type = "mock";
  config.request = "launch";
  Expect(manager.StartSession(config, MakeCallbacks(captured)), "session should start");

  Expect(PollUntil(manager, [&]() { return captured.stop_count >= 1; }),
         "the session should reach the initial stop");
  DebugSession* session = manager.ActiveSession();
  Expect(session != nullptr && session->CurrentState() == DebugSession::State::Stopped,
         "the session should be stopped before the resume");
  const int stops_before = captured.stop_count;

  session->Continue();
  Expect(PollUntil(manager,
                   [&]() {
                     const auto* s = manager.ActiveSession();
                     return s != nullptr &&
                            s->CurrentState() == DebugSession::State::Stopped &&
                            captured.stop_count > stops_before;
                   }),
         "a rejected resume must restore the Stopped state and re-project the stop");
  manager.ShutdownAll();
}

void TestDebugSessionPauseFromRunning() {
  TemporaryDirectory temp_dir;
  const auto server_path = temp_dir.path() / "adapter.py";
  WriteFile(server_path, std::string(MockAdapterSource()));

  DapManager manager;
  manager.RegisterAdapter("mock", platform::LocalProcessLauncher(), MockAdapterCommand(server_path, "pause"));

  CapturedSession captured;
  LaunchConfig config;
  config.type = "mock";
  config.request = "launch";
  Expect(manager.StartSession(config, MakeCallbacks(captured)), "session should start");

  Expect(PollUntil(manager,
                   [&]() {
                     const auto* session = manager.ActiveSession();
                     return session != nullptr &&
                            session->CurrentState() == DebugSession::State::Running;
                   }),
         "session should reach Running with no initial stop");

  manager.ActiveSession()->Pause();
  Expect(PollUntil(manager, [&]() { return captured.stop_count >= 1; }),
         "pause should drive the adapter to a stop");
  Expect(captured.output.find("cmd:pause") != std::string::npos,
         "Pause should send the DAP `pause` command after resolving a thread");
  manager.ShutdownAll();
}

void TestDebugSessionPauseInterruptsAGdbStillStartingTheProgram() {
  TemporaryDirectory temp_dir;
  const auto server_path = temp_dir.path() / "adapter.py";
  WriteFile(server_path, std::string(MockAdapterSource()));

  DapManager manager;
  manager.RegisterAdapter("mock", platform::LocalProcessLauncher(),
                          GdbFlavoredAdapterCommand(server_path, "gdb15_wedged"));
  CapturedSession captured;
  LaunchConfig config;
  config.type = "mock";
  config.request = "launch";
  Expect(manager.StartSession(config, MakeCallbacks(captured)), "session should start");
  Expect(PollUntil(manager,
                   [&]() {
                     const auto* session = manager.ActiveSession();
                     return session != nullptr &&
                            session->CurrentState() == DebugSession::State::Running &&
                            session->debuggee_pid() > 0;
                   }),
         "the session runs and knows its program's process from the `process` event");

  manager.ActiveSession()->Pause();
  Expect(PollUntil(manager, [&]() { return captured.stop_count >= 1; }),
         "Pause stops a program whose gdb is still inside configurationDone");
  manager.ShutdownAll();
}

void TestDebugServiceStopAllDoesNotWaitOnAWedgedGdb() {
  TemporaryDirectory temp_dir;
  const auto server_path = temp_dir.path() / "adapter.py";
  WriteFile(server_path, std::string(MockAdapterSource()));
  microide::workspace::WorkspaceContext context;
  context.current_project_state.root = temp_dir.path();
  microide::workspace::DebugService service;
  service.Configure(context, microide::workspace::DebugService::Operations{});
  DapManager& manager = service.CurrentDapManager();
  manager.RegisterAdapter("mock", platform::LocalProcessLauncher(),
                          GdbFlavoredAdapterCommand(server_path, "gdb15_wedged"));
  LaunchConfig config;
  config.name = "session";
  config.type = "mock";
  config.request = "launch";
  Expect(service.StartDebugging(config), "the session should start");
  const auto pump_until = [&](const std::function<bool()>& done) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (!done() && std::chrono::steady_clock::now() < deadline) {
      service.ConsumeDapCallbacks();
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return done();
  };
  int pid = 0;
  Expect(pump_until([&]() {
           const DebugSession* session = manager.ActiveSession();
           pid = session != nullptr ? session->debuggee_pid() : 0;
           return pid > 0;
         }),
         "the session runs its program, still inside configurationDone");

  service.StopAllDebugging();
  Expect(manager.SessionCount() == 1,
         "Stop All returns without joining the adapter (the session is still ending)");
  Expect(pump_until([&]() { return manager.SessionCount() == 0; }),
         "the session ends and is pruned");
  Expect(::kill(pid, 0) != 0, "and its program is gone");
}

void TestDebugSessionPausesAThreadAnnouncedByEvent() {
  TemporaryDirectory temp_dir;
  const auto server_path = temp_dir.path() / "adapter.py";
  WriteFile(server_path, std::string(MockAdapterSource()));

  DapManager manager;
  manager.RegisterAdapter("mock", platform::LocalProcessLauncher(),
                          MockAdapterCommand(server_path, "pause_silent_threads"));
  CapturedSession captured;
  LaunchConfig config;
  config.type = "mock";
  config.request = "launch";
  Expect(manager.StartSession(config, MakeCallbacks(captured)), "session should start");
  Expect(PollUntil(manager,
                   [&]() {
                     const auto* session = manager.ActiveSession();
                     return session != nullptr && session->CurrentState() == DebugSession::State::Running;
                   }),
         "session should reach Running");
  manager.ActiveSession()->Pause();
  Expect(PollUntil(manager, [&]() { return captured.stop_count >= 1; }),
         "pause should stop a program whose adapter answers no `threads` while it runs");
  Expect(captured.output.find("cmd:pause") != std::string::npos, "the `pause` request is sent");
  manager.ShutdownAll();
}

void TestTeardownEndsALaunchedDebuggeeAndSparesAnAttachedOne() {
  TemporaryDirectory temp_dir;
  const auto server_path = temp_dir.path() / "adapter.py";
  WriteFile(server_path, std::string(MockAdapterSource()));
  const auto record = temp_dir.path() / "disconnect.txt";
  ScopedEnvVar record_env("MOCK_DAP_RECORD", record.string());
  for (const std::string request : {"launch", "attach"}) {
    std::filesystem::remove(record);
    DapManager manager;
    manager.RegisterAdapter("mock", platform::LocalProcessLauncher(), MockAdapterCommand(server_path, "pause"));
    CapturedSession captured;
    LaunchConfig config;
    config.type = "mock";
    config.request = request;
    Expect(manager.StartSession(config, MakeCallbacks(captured)), request + ": the session should start");
    Expect(PollUntil(manager,
                     [&]() {
                       const auto* session = manager.ActiveSession();
                       return session != nullptr && session->CurrentState() == DebugSession::State::Running;
                     }),
           request + ": the session should run");
    manager.ShutdownAll();
    const std::string sent = ReadFile(record);
    const bool terminates = sent.find("\"terminateDebuggee\": true") != std::string::npos;
    Expect(request == "launch" ? terminates : !terminates,
           request + ": the teardown's disconnect " +
               (request == "launch" ? "ends the launched debuggee" : "leaves the attached process") + " (" +
               sent + ")");
  }
}

void TestAStopTheAdapterDoesNotAnswerEndsTheSession() {
  TemporaryDirectory temp_dir;
  const auto server_path = temp_dir.path() / "adapter.py";
  WriteFile(server_path, std::string(MockAdapterSource()));
  DapManager manager;
  manager.RegisterAdapter("mock", platform::LocalProcessLauncher(),
                          MockAdapterCommand(server_path, "silent_terminate"));
  CapturedSession captured;
  LaunchConfig config;
  config.type = "mock";
  config.request = "launch";
  Expect(manager.StartSession(config, MakeCallbacks(captured)), "session should start");
  Expect(PollUntil(manager,
                   [&]() {
                     const auto* session = manager.ActiveSession();
                     return session != nullptr && session->CurrentState() == DebugSession::State::Running;
                   }),
         "session should reach Running");
  manager.StopActiveSession();
  Expect(PollUntil(manager,
                   [&]() {
                     const auto* session = manager.ActiveSession();
                     return session == nullptr || session->CurrentState() == DebugSession::State::Terminated ||
                            session->CurrentState() == DebugSession::State::Failed;
                   }),
         "a stop the adapter never answers still ends the session");
  manager.ShutdownAll();
}

void TestDebugSessionReverseStepAndContinue() {
  TemporaryDirectory temp_dir;
  const auto server_path = temp_dir.path() / "adapter.py";
  WriteFile(server_path, std::string(MockAdapterSource()));

  DapManager manager;
  manager.RegisterAdapter("mock", platform::LocalProcessLauncher(), MockAdapterCommand(server_path, "reverse"));

  CapturedSession captured;
  LaunchConfig config;
  config.type = "mock";
  config.request = "launch";
  Expect(manager.StartSession(config, MakeCallbacks(captured)), "session should start");
  Expect(PollUntil(manager, [&]() { return captured.stop_count >= 1; }), "adapter should stop");

  DebugSession* session = manager.ActiveSession();
  Expect(session != nullptr, "session should be active while stopped");
  Expect(session->Client().Capabilities().supports_step_back,
         "adapter should advertise supportsStepBack");

  const int stops_before = captured.stop_count;
  session->StepBack();
  Expect(PollUntil(manager, [&]() { return captured.stop_count > stops_before; }),
         "step back should drive another stop");
  Expect(captured.output.find("cmd:stepBack") != std::string::npos,
         "StepBack should send the DAP `stepBack` command");
  Expect(captured.resume_count >= 1, "stepping back should fire on_resumed optimistically");

  session->ReverseContinue();
  Expect(PollUntil(manager,
                   [&]() {
                     return captured.output.find("cmd:reverseContinue") != std::string::npos;
                   }),
         "ReverseContinue should send the DAP `reverseContinue` command");
  manager.ShutdownAll();
}

void TestDebugSessionReverseGatedOnCapability() {
  TemporaryDirectory temp_dir;
  const auto server_path = temp_dir.path() / "adapter.py";
  WriteFile(server_path, std::string(MockAdapterSource()));

  DapManager manager;
  manager.RegisterAdapter("mock", platform::LocalProcessLauncher(), MockAdapterCommand(server_path, "stop"));

  CapturedSession captured;
  LaunchConfig config;
  config.type = "mock";
  config.request = "launch";
  Expect(manager.StartSession(config, MakeCallbacks(captured)), "session should start");
  Expect(PollUntil(manager, [&]() { return captured.stop_count >= 1; }), "adapter should stop");

  DebugSession* session = manager.ActiveSession();
  Expect(session != nullptr, "session should be active while stopped");
  Expect(!session->Client().Capabilities().supports_step_back,
         "adapter should not advertise supportsStepBack");

  const int resumes_before = captured.resume_count;
  session->StepBack();
  session->ReverseContinue();
  Expect(!PollStaysFalse(manager,
                    [&]() {
                      return captured.output.find("cmd:stepBack") != std::string::npos ||
                             captured.output.find("cmd:reverseContinue") != std::string::npos ||
                             captured.resume_count > resumes_before;
                    }),
         "reverse commands should be dropped when the adapter lacks supportsStepBack");
  manager.ShutdownAll();
}

void TestDebugSessionReverseEnabledByLateCapabilitiesEvent() {
  TemporaryDirectory temp_dir;
  const auto server_path = temp_dir.path() / "adapter.py";
  WriteFile(server_path, std::string(MockAdapterSource()));

  DapManager manager;
  manager.RegisterAdapter("mock", platform::LocalProcessLauncher(), MockAdapterCommand(server_path, "reverse_late"));

  CapturedSession captured;
  LaunchConfig config;
  config.type = "mock";
  config.request = "launch";
  Expect(manager.StartSession(config, MakeCallbacks(captured)), "session should start");
  Expect(PollUntil(manager, [&]() { return captured.stop_count >= 1; }), "adapter should stop");

  DebugSession* session = manager.ActiveSession();
  Expect(session != nullptr, "session should be active while stopped");
  Expect(!session->Client().Capabilities().supports_step_back,
         "adapter should not advertise supportsStepBack at init");
  const int resumes_before = captured.resume_count;
  session->StepBack();
  Expect(!PollStaysFalse(manager,
                    [&]() {
                      return captured.output.find("cmd:stepBack") != std::string::npos ||
                             captured.resume_count > resumes_before;
                    }),
         "step back is dropped before the capability is advertised");

  session->RequestEvaluate("record", 0, "repl", nullptr);
  Expect(PollUntil(manager,
                   [&]() { return session->Client().Capabilities().supports_step_back; }),
         "late capabilities event should flip supportsStepBack on");
  Expect(captured.capabilities_changed_count >= 1,
         "on_capabilities_changed should fire so capability-gated chrome repaints");

  session->StepBack();
  Expect(PollUntil(manager,
                   [&]() { return captured.output.find("cmd:stepBack") != std::string::npos; }),
         "StepBack should reach the wire once the late capability arrives");
  manager.ShutdownAll();
}

void TestDebugSessionVariablesTreeAndSetVariable() {
  TemporaryDirectory temp_dir;
  const auto server_path = temp_dir.path() / "adapter.py";
  WriteFile(server_path, std::string(MockAdapterSource()));

  DapManager manager;
  manager.RegisterAdapter("mock", platform::LocalProcessLauncher(), MockAdapterCommand(server_path, "variables"));

  CapturedSession captured;
  LaunchConfig config;
  config.type = "mock";
  config.request = "launch";
  Expect(manager.StartSession(config, MakeCallbacks(captured)), "session should start");
  Expect(PollUntil(manager, [&]() { return captured.stop_count >= 1; }),
         "adapter should stop so a frame is focusable");
  Expect(!captured.last_frames.empty(), "a stack frame should resolve on stop");
  DebugSession* session = manager.ActiveSession();
  Expect(session != nullptr, "session should be active while stopped");
  const int frame_id = captured.last_frames.empty() ? 0 : captured.last_frames[0].id;

  DebugVariablesModel model;
  model.BeginFrame(frame_id);

  std::vector<DebugValueTree::ChildFetch> scope_fetches;
  bool scopes_done = false;
  session->RequestScopes(frame_id, [&](std::vector<codec::DapScope> scopes) {
    scope_fetches = model.ApplyScopes(scopes);
    scopes_done = true;
  });
  Expect(PollUntil(manager, [&]() { return scopes_done; }), "scopes should resolve");
  Expect(model.Rows()[0].display_name == "Locals" && model.Rows()[0].has_children &&
             model.Rows()[0].expanded,
         "Locals is the first scope and open by default");

  Expect(scope_fetches.size() == 1 && scope_fetches[0].reference == 1000,
         "auto-expanding Locals requests its variablesReference exactly once");
  const int locals_ref = scope_fetches[0].reference;
  bool locals_done = false;
  session->RequestVariables(locals_ref, 0, DebugValueTree::kChildPageSize,
                            [&](bool ok, std::vector<codec::DapVariable> vars) {
                              (void)ok;
                              (void)model.ApplyVariables(locals_ref, vars, 0);
                              locals_done = true;
                            });
  Expect(PollUntil(manager, [&]() { return locals_done; }), "Locals variables should resolve");
  Expect(model.Rows().size() == 4, "Locals + x + obj + Globals should flatten to four rows");
  Expect(model.Rows()[1].display_name == "x" && model.Rows()[1].depth == 1 &&
             !model.Rows()[1].has_children && model.Rows()[1].editable,
         "x should be an editable depth-1 leaf");
  Expect(model.Rows()[2].display_name == "obj" && model.Rows()[2].has_children,
         "obj should be an expandable child");

  const int obj_ref = model.ToggleRow(2).reference;
  Expect(obj_ref == 1001, "expanding obj should request its variablesReference");
  bool obj_done = false;
  session->RequestVariables(obj_ref, 0, DebugValueTree::kChildPageSize,
                            [&](bool ok, std::vector<codec::DapVariable> vars) {
                              (void)ok;
                              (void)model.ApplyVariables(obj_ref, vars, 0);
                              obj_done = true;
                            });
  Expect(PollUntil(manager, [&]() { return obj_done; }), "obj variables should resolve");
  Expect(model.Rows().size() == 5 && model.Rows()[3].display_name == "field" &&
             model.Rows()[3].depth == 2,
         "obj's field should appear nested at depth 2");
  Expect(model.ToggleRow(2).reference == 0 && model.Rows().size() == 4,
         "collapsing obj should drop its children without a refetch");

  const std::uint32_t x_node = model.Rows()[1].node_id;
  bool set_done = false;
  bool set_ok = false;
  session->SetVariable(1000, "x", "99", [&](bool ok, codec::DapSetVariableResult result) {
    set_ok = ok;
    if (ok) {
      model.ApplySetVariable(x_node, result);
    }
    set_done = true;
  });
  Expect(PollUntil(manager, [&]() { return set_done; }), "setVariable should respond");
  Expect(set_ok, "setVariable should succeed when the adapter supports it");
  Expect(model.Rows()[1].display_value == "99", "x's value should reflect the adapter's echo");
  manager.ShutdownAll();
}

void TestDebugSessionSetVariableGatedOnCapability() {
  TemporaryDirectory temp_dir;
  const auto server_path = temp_dir.path() / "adapter.py";
  WriteFile(server_path, std::string(MockAdapterSource()));

  DapManager manager;
  manager.RegisterAdapter("mock", platform::LocalProcessLauncher(), MockAdapterCommand(server_path, "stop"));

  CapturedSession captured;
  LaunchConfig config;
  config.type = "mock";
  config.request = "launch";
  Expect(manager.StartSession(config, MakeCallbacks(captured)), "session should start");
  Expect(PollUntil(manager, [&]() { return captured.stop_count >= 1; }), "adapter should stop");

  bool callback_fired = false;
  bool callback_ok = true;
  manager.ActiveSession()->SetVariable(
      1000, "x", "5", [&](bool ok, codec::DapSetVariableResult) {
        callback_fired = true;
        callback_ok = ok;
      });
  Expect(PollUntil(manager, [&]() { return callback_fired; }),
         "setVariable callback should fire even when unsupported");
  Expect(!callback_ok, "setVariable should report failure when the adapter lacks the capability");
  manager.ShutdownAll();
}

void TestDebugSessionEvaluateHover() {
  TemporaryDirectory temp_dir;
  const auto server_path = temp_dir.path() / "adapter.py";
  WriteFile(server_path, std::string(MockAdapterSource()));

  DapManager manager;
  manager.RegisterAdapter("mock", platform::LocalProcessLauncher(), MockAdapterCommand(server_path, "evaluate"));

  CapturedSession captured;
  LaunchConfig config;
  config.type = "mock";
  config.request = "launch";
  Expect(manager.StartSession(config, MakeCallbacks(captured)), "session should start");
  Expect(PollUntil(manager, [&]() { return captured.stop_count >= 1; }),
         "adapter should stop so a frame is focusable");
  Expect(!captured.last_frames.empty(), "a stack frame should resolve on stop");
  DebugSession* session = manager.ActiveSession();
  Expect(session != nullptr, "session should be active while stopped");
  Expect(session->Client().Capabilities().supports_evaluate_for_hovers,
         "evaluate-mode adapter advertises supportsEvaluateForHovers");
  const int frame_id = captured.last_frames[0].id;

  DebugHoverModel hover;
  Expect(hover.Classify(frame_id, "count") == DebugHoverModel::Lookup::Miss,
         "fresh model classifies any query as a miss");
  const std::uint64_t generation = hover.Begin(frame_id, "count");
  Expect(hover.Classify(frame_id, "count") == DebugHoverModel::Lookup::Pending,
         "an in-flight query classifies as pending (suppresses re-issue)");
  session->RequestEvaluate("count", frame_id, "hover",
                           [&](bool ok, codec::DapEvaluateResult result) {
                             if (ok) {
                               hover.Resolve(generation, std::move(result.result),
                                             std::move(result.type));
                             } else {
                               hover.Fail(generation);
                             }
                           });
  Expect(PollUntil(manager,
                   [&]() { return hover.status == DebugHoverModel::Status::Resolved; }),
         "evaluate should resolve the hover value");
  Expect(hover.Classify(frame_id, "count") == DebugHoverModel::Lookup::Hit,
         "the resolved value is served as a cache hit");
  Expect(hover.value == "count@" + std::to_string(frame_id) && hover.type == "int",
         "the adapter's evaluated result + type land in the model");
  manager.ShutdownAll();
}

void TestDebugSessionWatchEvaluate() {
  TemporaryDirectory temp_dir;
  const auto server_path = temp_dir.path() / "adapter.py";
  WriteFile(server_path, std::string(MockAdapterSource()));

  DapManager manager;
  manager.RegisterAdapter("mock", platform::LocalProcessLauncher(), MockAdapterCommand(server_path, "evaluate"));

  CapturedSession captured;
  LaunchConfig config;
  config.type = "mock";
  config.request = "launch";
  Expect(manager.StartSession(config, MakeCallbacks(captured)), "session should start");
  Expect(PollUntil(manager, [&]() { return captured.stop_count >= 1; }),
         "adapter should stop so a frame is focusable");
  Expect(!captured.last_frames.empty(), "a stack frame should resolve on stop");
  DebugSession* session = manager.ActiveSession();
  Expect(session != nullptr, "session should be active while stopped");
  const int frame_id = captured.last_frames[0].id;

  DebugWatchModel watch;
  watch.AddExpression("count");
  Expect(watch.Rows().size() == 1 && watch.Rows()[0].display_value.empty(),
         "the watch root starts as a blank placeholder");
  bool resolved = false;
  session->RequestEvaluate(watch.Expressions()[0], frame_id, "watch",
                           [&](bool ok, codec::DapEvaluateResult result) {
                             if (ok) {
                               watch.ApplyEvaluate(0, result);
                             }
                             resolved = true;
                           });
  Expect(PollUntil(manager, [&]() { return resolved; }), "watch evaluate should resolve");
  Expect(watch.Rows()[0].display_value == "count@" + std::to_string(frame_id),
         "the adapter's evaluated value folds onto the watch expression's root");
  manager.ShutdownAll();
}

void TestDebugSessionReplEvaluate() {
  TemporaryDirectory temp_dir;
  const auto server_path = temp_dir.path() / "adapter.py";
  WriteFile(server_path, std::string(MockAdapterSource()));

  DapManager manager;
  manager.RegisterAdapter("mock", platform::LocalProcessLauncher(), MockAdapterCommand(server_path, "evaluate"));

  CapturedSession captured;
  LaunchConfig config;
  config.type = "mock";
  config.request = "launch";
  Expect(manager.StartSession(config, MakeCallbacks(captured)), "session should start");
  Expect(PollUntil(manager, [&]() { return captured.stop_count >= 1; }),
         "adapter should stop so a frame is focusable");
  Expect(!captured.last_frames.empty(), "a stack frame should resolve on stop");
  DebugSession* session = manager.ActiveSession();
  Expect(session != nullptr, "session should be active while stopped");
  const int frame_id = captured.last_frames[0].id;

  bool resolved = false;
  std::string repl_value;
  session->RequestEvaluate("answer", frame_id, "repl",
                           [&](bool ok, codec::DapEvaluateResult result) {
                             if (ok) {
                               repl_value = result.result;
                             }
                             resolved = true;
                           });
  Expect(PollUntil(manager, [&]() { return resolved; }), "repl evaluate should resolve");
  Expect(repl_value == "answer@" + std::to_string(frame_id),
         "the adapter's repl-context result echoes the expression evaluated in the frame");

  bool struct_resolved = false;
  int struct_ref = 0;
  session->RequestEvaluate("obj", frame_id, "repl",
                           [&](bool ok, codec::DapEvaluateResult result) {
                             if (ok) {
                               struct_ref = result.variables_reference;
                             }
                             struct_resolved = true;
                           });
  Expect(PollUntil(manager, [&]() { return struct_resolved; }),
         "structured repl evaluate should resolve");
  Expect(struct_ref == 1001, "a structured repl result carries a variablesReference to expand");
  bool children_resolved = false;
  std::vector<codec::DapVariable> children;
  session->RequestVariables(struct_ref, 0, DebugValueTree::kChildPageSize,
                            [&](bool ok, std::vector<codec::DapVariable> vars) {
                              (void)ok;
                              children = std::move(vars);
                              children_resolved = true;
                            });
  Expect(PollUntil(manager, [&]() { return children_resolved; }),
         "the follow-up variables request should resolve the structured children");
  Expect(children.size() == 1 && children[0].name == "field" && children[0].value == "7",
         "the structured repl result expands one level into its child fields");
  manager.ShutdownAll();
}

void TestDebugSessionHoverFallsBackToReplWithoutCapability() {
  TemporaryDirectory temp_dir;
  const auto server_path = temp_dir.path() / "adapter.py";
  WriteFile(server_path, std::string(MockAdapterSource()));

  DapManager manager;
  manager.RegisterAdapter("mock", platform::LocalProcessLauncher(), MockAdapterCommand(server_path, "stop"));

  CapturedSession captured;
  LaunchConfig config;
  config.type = "mock";
  config.request = "launch";
  Expect(manager.StartSession(config, MakeCallbacks(captured)), "session should start");
  Expect(PollUntil(manager, [&]() { return captured.stop_count >= 1; }), "adapter should stop");
  Expect(!captured.last_frames.empty(), "a stack frame should resolve on stop");
  const int frame_id = captured.last_frames[0].id;

  bool callback_fired = false;
  bool callback_ok = false;
  std::string value;
  manager.ActiveSession()->RequestEvaluate(
      "count", frame_id, "hover", [&](bool ok, codec::DapEvaluateResult result) {
        callback_fired = true;
        callback_ok = ok;
        if (ok) {
          value = result.result;
        }
      });
  Expect(PollUntil(manager, [&]() { return callback_fired; }),
         "hover evaluate callback should fire");
  Expect(callback_ok, "hover evaluate should succeed via the repl fallback without the capability");
  Expect(value == "count@" + std::to_string(frame_id),
         "the fallback evaluate echoes the expression evaluated in the frame");
  manager.ShutdownAll();
}

void TestDebugHoverModelBehavior() {
  DebugHoverModel hover;
  Expect(hover.status == DebugHoverModel::Status::Empty, "model starts Empty");
  Expect(hover.Classify(1, "x") == DebugHoverModel::Lookup::Miss, "empty model is a miss");

  const std::uint64_t gen = hover.Begin(1, "x");
  Expect(hover.Classify(2, "x") == DebugHoverModel::Lookup::Miss, "different frame is a miss");
  Expect(hover.Classify(1, "y") == DebugHoverModel::Lookup::Miss, "different expr is a miss");
  Expect(hover.Classify(1, "x") == DebugHoverModel::Lookup::Pending, "same key while pending");

  hover.Resolve(gen, "42", "int");
  Expect(hover.Classify(1, "x") == DebugHoverModel::Lookup::Hit && hover.value == "42",
         "resolve under the live generation lands a hit");

  const std::uint64_t gen2 = hover.Begin(1, "x");
  hover.Resolve(gen, "stale", "int");
  Expect(hover.status == DebugHoverModel::Status::Pending && hover.value.empty(),
         "a completion from a superseded generation is ignored");
  hover.Fail(gen2);
  Expect(hover.Classify(1, "x") == DebugHoverModel::Lookup::Failed, "fail marks the query failed");

  hover.Clear();
  Expect(hover.status == DebugHoverModel::Status::Empty &&
             hover.Classify(1, "x") == DebugHoverModel::Lookup::Miss,
         "Clear resets to Empty and bumps the generation");
}

void TestDebugVariablesModelTreeBehavior() {
  DebugVariablesModel model;
  model.BeginFrame(7);
  Expect(model.FrameId() == 7, "BeginFrame should record the focused frame id");

  std::vector<codec::DapScope> scopes = {
      codec::DapScope{.name = "Arguments", .variables_reference = 1000},
      codec::DapScope{.name = "Globals", .variables_reference = 2000},
  };
  model.ApplyScopes(scopes);
  Expect(model.Rows().size() == 2, "two scopes flatten to two rows");
  Expect(model.Rows()[0].has_children && !model.Rows()[0].expanded && !model.Rows()[0].editable,
         "a scope row is expandable, collapsed, and not editable");

  Expect(model.ToggleRow(0).reference == 1000,
         "expanding a not-loaded scope returns its reference to fetch");
  (void)model.ApplyVariables(1000,
                       {
                           codec::DapVariable{
                               .name = "x", .value = "1", .type = "int", .variables_reference = 0},
                           codec::DapVariable{.name = "obj",
                                              .value = "{...}",
                                              .type = "Obj",
                                              .variables_reference = 1001},
                       },
                       0);
  Expect(model.Rows().size() == 4, "expanded Arguments interleaves its children before Globals");
  Expect(model.Rows()[1].display_name == "x" && model.Rows()[1].depth == 1,
         "child x sits at depth 1");

  Expect(model.ToggleRow(2).reference == 1001, "expanding obj returns its reference");
  (void)model.ApplyVariables(
      1001, {codec::DapVariable{.name = "field", .value = "7", .variables_reference = 0}}, 0);
  Expect(model.Rows().size() == 5 && model.Rows()[3].depth == 2, "nested field flattens at depth 2");
  Expect(model.ToggleRow(2).reference == 0 && model.Rows().size() == 4,
         "collapse removes the subtree");
  Expect(model.ToggleRow(2).reference == 0 && model.Rows().size() == 5,
         "re-expanding a loaded node does not refetch");

  Expect(!model.BeginEdit(0), "scopes are not editable");
  Expect(model.BeginEdit(1) && model.IsEditing(), "a leaf variable enters edit");
  Expect(model.EditBuffer().text() == "1", "edit buffer seeds with the current value");
  const auto target = model.EditTargetForCommit();
  Expect(target.has_value() && target->container_reference == 1000 && target->name == "x",
         "commit target carries the container ref + name setVariable needs");
  model.ApplySetVariable(model.Rows()[1].node_id,
                         codec::DapSetVariableResult{.value = "42", .type = "int"});
  Expect(model.Rows()[1].display_value == "42", "setVariable result updates the row value");
  model.CancelEdit();
  Expect(!model.IsEditing(), "cancel exits edit mode");

  model.SetSelectedRow(99);
  Expect(model.SelectedRow() == model.Rows().size() - 1, "selection clamps to the last row");
  model.MoveSelection(-100);
  Expect(model.SelectedRow() == 0, "selection clamps to the first row");

  model.Clear();
  Expect(model.Empty() && model.Rows().empty(), "Clear empties the tree");
}

void TestDebugVariablesModelExpansionIsPerSiblingNotByNameOnly() {
  DebugVariablesModel model;
  model.BeginFrame(1);
  model.ApplyScopes({codec::DapScope{.name = "Arguments", .variables_reference = 1000}});
  Expect(model.ToggleRow(0).reference == 1000, "expanding the scope fetches its ref");
  (void)model.ApplyVariables(1000,
                       {
                           codec::DapVariable{.name = "item",
                                              .value = "a",
                                              .type = "T",
                                              .variables_reference = 1001},
                           codec::DapVariable{.name = "item",
                                              .value = "b",
                                              .type = "T",
                                              .variables_reference = 1002},
                       },
                       0);
  Expect(model.Rows().size() == 3, "scope plus two same-named children flatten to three rows");
  Expect(model.ToggleRow(1).reference == 1001, "expanding the first item fetches its ref");
  (void)model.ApplyVariables(
      1001, {codec::DapVariable{.name = "leaf", .value = "1", .variables_reference = 0}}, 0);
  Expect(model.Rows().size() == 4 && model.Rows()[1].expanded && !model.Rows()[3].expanded,
         "only the first item is expanded before the next stop");

  model.BeginFrame(2);
  model.ApplyScopes({codec::DapScope{.name = "Arguments", .variables_reference = 2000}});
  const auto child_fetches = model.ApplyVariables(
      2000,
      {
          codec::DapVariable{.name = "item", .value = "a", .type = "T", .variables_reference = 2001},
          codec::DapVariable{.name = "item", .value = "b", .type = "T", .variables_reference = 2002},
      },
      0);
  Expect(child_fetches.size() == 1 && child_fetches[0].reference == 2001,
         "restore must re-expand only the specific sibling that was open, not every same-named one");
}

void TestDebugVariablesShowMoreClearsOnEmptyOverReportedPage() {
  DebugVariablesModel model;
  model.BeginFrame(1);
  model.ApplyScopes({codec::DapScope{.name = "Arguments", .variables_reference = 1000}});
  model.ToggleRow(0);
  (void)model.ApplyVariables(1000,
                       {codec::DapVariable{.name = "arr",
                                           .value = "[...]",
                                           .type = "int[]",
                                           .variables_reference = 1001,
                                           .indexed_variables = 5,
                                           .count_reported = true}},
                       0);
  std::size_t arr_row = 0;
  for (std::size_t i = 0; i < model.Rows().size(); ++i) {
    if (model.Rows()[i].display_name == "arr") {
      arr_row = i;
      break;
    }
  }
  model.ToggleRow(arr_row);
  (void)model.ApplyVariables(
      1001,
      {codec::DapVariable{.name = "[0]", .value = "1", .variables_reference = 0},
       codec::DapVariable{.name = "[1]", .value = "2", .variables_reference = 0}},
      0);
  bool has_show_more = false;
  for (const auto& row : model.Rows()) {
    has_show_more = has_show_more || row.is_show_more;
  }
  Expect(has_show_more, "an over-reported total leaves a 'show more' affordance after the first page");

  (void)model.ApplyVariables(1001, {}, 2);
  bool still_show_more = false;
  for (const auto& row : model.Rows()) {
    still_show_more = still_show_more || row.is_show_more;
  }
  Expect(!still_show_more,
         "an empty over-range page clears 'show more' instead of persisting it forever");
}

void TestDebugVariablesLocalsOpenByDefault() {
  DebugVariablesModel model;
  model.BeginFrame(1);
  const std::vector<codec::DapScope> scopes = {
      codec::DapScope{.name = "Locals", .variables_reference = 1000},
      codec::DapScope{.name = "Registers", .variables_reference = 2000},
  };

  std::vector<DebugValueTree::ChildFetch> fetches = model.ApplyScopes(scopes);
  Expect(fetches.size() == 1 && fetches[0].reference == 1000,
         "the default Locals scope auto-issues exactly one bounded child fetch");
  Expect(model.Rows()[0].display_name == "Locals" && model.Rows()[0].expanded,
         "Locals is expanded by default on the first stop");
  Expect(model.Rows().back().display_name == "Registers" && !model.Rows().back().expanded,
         "a non-default scope stays collapsed");

  (void)model.ApplyVariables(
      1000, {codec::DapVariable{.name = "x", .value = "1", .type = "int"}}, 0);
  Expect(model.Rows()[0].expanded && model.Rows()[1].display_name == "x",
         "Locals shows its child after the page arrives");
  model.ToggleRow(0);
  Expect(!model.Rows()[0].expanded, "the user collapses Locals");

  model.BeginFrame(1);
  fetches = model.ApplyScopes(scopes);
  Expect(fetches.empty() && !model.Rows()[0].expanded,
         "a collapsed Locals stays collapsed on the next stop (no auto-refetch)");

  model.BeginSession();
  model.BeginFrame(1);
  fetches = model.ApplyScopes(scopes);
  Expect(fetches.size() == 1 && model.Rows()[0].expanded,
         "a new session reopens Locals by default");
}

void TestDebugVariablesScopeCountDoesNotOverflow() {
  DebugVariablesModel model;
  model.BeginFrame(1);
  const std::vector<codec::DapScope> scopes = {
      codec::DapScope{.name = "Locals",
                      .variables_reference = 1000,
                      .named_variables = 2'000'000'000,
                      .indexed_variables = 2'000'000'000,
                      .count_reported = true},
  };
  const std::vector<DebugValueTree::ChildFetch> fetches = model.ApplyScopes(scopes);
  Expect(fetches.size() == 1 && fetches[0].reference == 1000,
         "a huge-count scope still issues one bounded child fetch");
  Expect(fetches[0].count >= 0, "the bounded page count must not be negative");
  Expect(model.Rows()[0].display_name == "Locals",
         "the scope row is produced without overflow UB");
}

void TestDebugVariablesValueKindAndPlaceholder() {
  DebugVariablesModel model;
  model.BeginFrame(1);
  model.ApplyScopes({codec::DapScope{.name = "Registers", .variables_reference = 1000}});
  Expect(model.Rows()[0].kind == DebugValueKind::Scope, "a scope row classifies as Scope");

  Expect(model.ToggleRow(0).reference == 1000,
         "expanding a scope returns its reference to fetch");
  Expect(model.Rows().size() == 2, "an unloaded expanded scope shows one placeholder child");
  Expect(model.Rows()[1].is_placeholder && model.Rows()[1].kind == DebugValueKind::Pending &&
             model.Rows()[1].depth == 1 && !model.Rows()[1].has_children,
         "the placeholder is a dim, non-expandable depth-1 row");

  (void)model.ApplyVariables(
      1000,
      {
          codec::DapVariable{.name = "i", .value = "42", .type = "int"},
          codec::DapVariable{.name = "ratio", .value = "0.5", .type = "double"},
          codec::DapVariable{.name = "label", .value = "\"hi\"", .type = "std::string"},
          codec::DapVariable{.name = "ok", .value = "true", .type = "bool"},
          codec::DapVariable{.name = "name", .value = "0x5555 \"a\"", .type = "const char *"},
          codec::DapVariable{
              .name = "pt", .value = "{...}", .type = "Point", .variables_reference = 1001},
      },
      0);
  Expect(model.Rows().size() == 7, "the placeholder is replaced by the six fetched children");
  const auto kind_of = [&](const char* name) {
    for (const auto& row : model.Rows()) {
      if (row.display_name == name) {
        return row.kind;
      }
    }
    return DebugValueKind::Plain;
  };
  Expect(kind_of("i") == DebugValueKind::Number, "an integer value classifies as Number");
  Expect(kind_of("ratio") == DebugValueKind::Number, "a float value classifies as Number");
  Expect(kind_of("label") == DebugValueKind::String, "a quoted value classifies as String");
  Expect(kind_of("ok") == DebugValueKind::Boolean, "true/false classifies as Boolean");
  Expect(kind_of("name") == DebugValueKind::Pointer, "a 0x… value classifies as Pointer");
  Expect(kind_of("pt") == DebugValueKind::Aggregate, "a structured value classifies as Aggregate");

  Expect(model.ToggleRow(6).reference == 1001, "expanding pt returns its reference");
  Expect(model.Rows().back().is_placeholder, "pt's children are still pending → placeholder");
  (void)model.ApplyVariables(1001, {codec::DapVariable{.name = "x", .value = "1", .type = "int"}}, 0);
  Expect(!model.Rows().back().is_placeholder && model.Rows().back().display_name == "x",
         "pt's fetched child replaces its placeholder");
}

void TestDebugVariablesPagingAndErrors() {
  constexpr int kPage = DebugValueTree::kChildPageSize;
  DebugVariablesModel model;
  model.BeginFrame(1);
  model.ApplyScopes({codec::DapScope{.name = "Registers", .variables_reference = 1000}});
  Expect(model.ToggleRow(0).reference == 1000, "expanding the scope requests its children");
  (void)model.ApplyVariables(1000,
                       {codec::DapVariable{.name = "arr",
                                           .value = "{...}",
                                           .type = "int[300]",
                                           .variables_reference = 1001,
                                           .indexed_variables = 300,
                                           .count_reported = true}},
                       0);
  Expect(model.Rows().size() == 2 && model.Rows()[1].display_name == "arr",
         "the array child is present under the scope");

  const DebugValueTree::ChildFetch first = model.ToggleRow(1);
  Expect(first.reference == 1001 && first.start == 0 && first.count == kPage,
         "expanding a 300-element array fetches a bounded first page, not the whole container");

  std::vector<codec::DapVariable> page0;
  for (int i = 0; i < kPage; ++i) {
    page0.push_back(codec::DapVariable{.name = "[" + std::to_string(i) + "]", .value = "0"});
  }
  (void)model.ApplyVariables(1001, page0, 0);
  Expect(model.Rows().size() == static_cast<std::size_t>(kPage) + 3,
         "the first page is followed by a clickable 'show more' row (300 > kPage)");
  Expect(model.Rows().back().is_show_more && !model.Rows().back().is_placeholder,
         "the trailing row is the 'show more' affordance");

  const DebugValueTree::ChildFetch more = model.ToggleRow(model.Rows().size() - 1);
  Expect(more.reference == 1001 && more.start == kPage && more.count == 300 - kPage,
         "'show more' fetches the remaining children at the loaded offset, clamped to the total");
  Expect(model.Rows().back().is_placeholder && !model.Rows().back().is_show_more,
         "the 'show more' row becomes a loading row while the next page is in flight");
  Expect(model.ToggleRow(model.Rows().size() - 1).reference == 0,
         "an in-flight loading row issues no duplicate fetch (request-storm guard)");

  std::vector<codec::DapVariable> page1;
  for (int i = kPage; i < 300; ++i) {
    page1.push_back(codec::DapVariable{.name = "[" + std::to_string(i) + "]", .value = "0"});
  }
  (void)model.ApplyVariables(1001, page1, kPage);
  Expect(model.Rows().size() == static_cast<std::size_t>(300) + 2,
         "the final page appends to 300 children with no further 'show more'");
  Expect(model.Rows().back().display_name == "[299]" && !model.Rows().back().is_show_more,
         "paging stops once the adapter-reported total is reached");

  DebugVariablesModel errored;
  errored.BeginFrame(1);
  errored.ApplyScopes({codec::DapScope{.name = "Registers", .variables_reference = 2000}});
  Expect(errored.ToggleRow(0).reference == 2000, "expanding requests the scope's children");
  Expect(errored.Rows().size() == 2 && errored.Rows()[1].kind == DebugValueKind::Pending,
         "before the response the child is a loading placeholder");
  errored.MarkChildrenError(2000);
  Expect(errored.Rows().size() == 2 && errored.Rows()[1].is_placeholder &&
             errored.Rows()[1].kind == DebugValueKind::Error,
         "a failed fetch replaces the spinner with a finite error row");

  DebugVariablesModel small_scope;
  small_scope.BeginFrame(1);
  small_scope.ApplyScopes({codec::DapScope{.name = "Registers",
                                           .variables_reference = 3000,
                                           .named_variables = 9,
                                           .count_reported = true}});
  const DebugValueTree::ChildFetch scoped = small_scope.ToggleRow(0);
  Expect(scoped.reference == 3000 && scoped.start == 0 && scoped.count == 9,
         "expanding a 9-variable scope requests exactly 9, not kPage (gdb count clamp)");

  DebugVariablesModel empty_child;
  empty_child.BeginFrame(1);
  empty_child.ApplyScopes(
      {codec::DapScope{.name = "Registers", .variables_reference = 4000, .named_variables = 1}});
  empty_child.ToggleRow(0);
  (void)empty_child.ApplyVariables(4000,
                             {codec::DapVariable{.name = "v",
                                                 .value = "std::vector of length 0",
                                                 .type = "std::vector<int>",
                                                 .variables_reference = 4001,
                                                 .indexed_variables = 0,
                                                 .count_reported = true}},
                             0);
  const DebugValueTree::ChildFetch empty_fetch = empty_child.ToggleRow(1);
  Expect(empty_fetch.reference == 0,
         "expanding a known-empty container issues no fetch (gdb would reject a count request)");

  DebugVariablesModel keep;
  keep.BeginFrame(1);
  Expect(keep.ApplyScopes({codec::DapScope{
                              .name = "Registers", .variables_reference = 1, .named_variables = 2}})
                 .empty(),
         "first stop: nothing was expanded yet, so nothing to restore");
  keep.ToggleRow(0);
  (void)keep.ApplyVariables(1,
                      {codec::DapVariable{.name = "n", .value = "5", .type = "int"},
                       codec::DapVariable{.name = "pt",
                                          .value = "",
                                          .type = "Point",
                                          .variables_reference = 9,
                                          .named_variables = 2}},
                      0);
  keep.ToggleRow(2);
  (void)keep.ApplyVariables(9,
                      {codec::DapVariable{.name = "x", .value = "1", .type = "int"},
                       codec::DapVariable{.name = "y", .value = "2", .type = "int"}},
                      0);
  Expect(keep.Rows().size() == 5, "Locals > {n, pt > {x, y}} is fully expanded");

  const std::vector<DebugValueTree::ChildFetch> restore = keep.ApplyScopes(
      {codec::DapScope{.name = "Registers", .variables_reference = 100, .named_variables = 2}});
  Expect(restore.size() == 1 && restore[0].reference == 100,
         "the re-expanded scope (new ref) is queued for repopulation");
  const std::vector<DebugValueTree::ChildFetch> restore_children =
      keep.ApplyVariables(100,
                          {codec::DapVariable{.name = "n", .value = "6", .type = "int"},
                           codec::DapVariable{.name = "pt",
                                              .value = "",
                                              .type = "Point",
                                              .variables_reference = 109,
                                              .named_variables = 2}},
                          0);
  Expect(restore_children.size() == 1 && restore_children[0].reference == 109,
         "the previously-expanded 'pt' child (new ref) cascades into a follow-up fetch");
  (void)keep.ApplyVariables(109,
                      {codec::DapVariable{.name = "x", .value = "1", .type = "int"},
                       codec::DapVariable{.name = "y", .value = "2", .type = "int"}},
                      0);
  Expect(keep.Rows().size() == 5,
         "after the stop the tree is restored to Locals > {n, pt > {x, y}}");
}

void TestGdbAdapterClampsValueFormatting() {
  TemporaryDirectory temp_dir;
  const auto server_path = temp_dir.path() / "adapter.py";
  WriteFile(server_path, std::string(MockAdapterSource()));

  {
    DapManager manager;
    manager.RegisterAdapter("gdb-mock", platform::LocalProcessLauncher(), GdbFlavoredAdapterCommand(server_path, "no_config_done"));
    CapturedSession captured;
    LaunchConfig config;
    config.type = "gdb-mock";
    config.request = "launch";
    Expect(manager.StartSession(config, MakeCallbacks(captured)), "gdb session should start");
    Expect(PollUntil(manager,
                     [&]() {
                       const auto* session = manager.ActiveSession();
                       return session != nullptr &&
                              session->CurrentState() == DebugSession::State::Terminated;
                     }),
           "gdb session should reach Terminated");
    const std::string& out = captured.output;
    Expect(out.find("limit:set max-value-size 65536|frameId=none") != std::string::npos,
           "gdb adapter clamps max-value-size to the safe value with no frameId");
    Expect(out.find("limit:set print elements 200|frameId=none") != std::string::npos,
           "gdb adapter clamps print elements with no frameId");
    Expect(out.find("limit:set print characters 200|frameId=none") != std::string::npos,
           "gdb adapter clamps print characters with no frameId");
    Expect(out.find("limit:set print repeats 10|frameId=none") != std::string::npos,
           "gdb adapter clamps print repeats with no frameId");
    Expect(out.find("max-value-size 1048576") == std::string::npos,
           "the old unsafe 1 MiB max-value-size is no longer sent");
    manager.ShutdownAll();
  }

  {
    DapManager manager;
    manager.RegisterAdapter("mock", platform::LocalProcessLauncher(), MockAdapterCommand(server_path, "no_config_done"));
    CapturedSession captured;
    LaunchConfig config;
    config.type = "mock";
    config.request = "launch";
    Expect(manager.StartSession(config, MakeCallbacks(captured)), "mock session should start");
    Expect(PollUntil(manager,
                     [&]() {
                       const auto* session = manager.ActiveSession();
                       return session != nullptr &&
                              session->CurrentState() == DebugSession::State::Terminated;
                     }),
           "mock session should reach Terminated");
    Expect(captured.output.find("limit:") == std::string::npos,
           "a non-gdb adapter receives no value-formatting limit commands");
    manager.ShutdownAll();
  }
}

void TestDebugWatchModelBehavior() {
  DebugWatchModel model;
  Expect(!model.HasExpressions() && model.Rows().empty(), "an empty watch model has no rows");

  const std::size_t i0 = model.AddExpression("x");
  const std::size_t i1 = model.AddExpression("obj");
  Expect(i0 == 0 && i1 == 1, "AddExpression returns the new index");
  Expect(model.Expressions().size() == 2, "both expressions are tracked");
  Expect(model.Rows().size() == 2, "each expression pre-creates a placeholder root");
  Expect(model.Rows()[0].display_name == "x" && model.Rows()[0].depth == 0,
         "a root row shows the expression at depth 0");
  Expect(model.Rows()[0].editable && !model.Rows()[0].has_children,
         "an unevaluated scalar root is editable and not yet expandable");

  model.ApplyEvaluate(
      0, codec::DapEvaluateResult{.result = "42", .type = "int", .variables_reference = 0});
  model.ApplyEvaluate(
      1, codec::DapEvaluateResult{.result = "{...}", .type = "Obj", .variables_reference = 900});
  Expect(model.Rows()[0].display_value == "42", "a scalar result folds onto its root");
  Expect(model.Rows()[1].has_children, "a structured result makes the root expandable");

  Expect(model.ExpressionIndexForRow(0) == std::optional<std::size_t>(0),
         "row 0 maps back to expression 0");

  Expect(model.ToggleRow(1).reference == 900,
         "expanding a structured root returns its reference to fetch");
  const auto scalar_child_cascade = model.ApplyVariables(
      900, {codec::DapVariable{.name = "field", .value = "7", .variables_reference = 0}}, 0);
  Expect(scalar_child_cascade.empty(),
         "a freshly-attached scalar child produces no cascade fetch");
  Expect(model.Rows().size() == 3 && model.Rows()[2].depth == 1, "the child folds at depth 1");
  Expect(!model.ExpressionIndexForRow(2).has_value(), "a child row is not an expression root");

  model.EditExpression(0, "x + 1");
  Expect(model.Expressions()[0] == "x + 1", "EditExpression updates the string");
  Expect(model.Rows().size() == 2 && model.Rows()[0].display_value.empty(),
         "editing rebuilds blank placeholder roots");
  model.RemoveExpression(0);
  Expect(model.Expressions().size() == 1 && model.Expressions()[0] == "obj",
         "RemoveExpression drops the expression at the index");
  model.EditExpression(0, "");
  Expect(model.Expressions().empty(), "editing an expression to empty removes it");

  model.SetExpressions({"a", "b"});
  Expect(model.Expressions().size() == 2 && model.Rows().size() == 2,
         "SetExpressions rebuilds one root per restored expression");
  model.ApplyEvaluate(0, codec::DapEvaluateResult{.result = "1"});
  model.ClearResults();
  Expect(model.Expressions().size() == 2, "ClearResults keeps the persistent expression list");
  Expect(model.Rows().size() == 2 && model.Rows()[0].display_value.empty(),
         "ClearResults blanks evaluated values but keeps the placeholder rows");
}

void TestDebugWatchModelPlaceholderRebuildFreshness() {
  DebugWatchModel model;
  Expect(!model.NeedsPlaceholderRebuild(), "a fresh model needs no rebuild");

  model.AddExpression("x");
  Expect(!model.NeedsPlaceholderRebuild(),
         "adding an expression rebuilds placeholders, leaving the tree pristine");

  model.ApplyEvaluate(0, codec::DapEvaluateResult{.result = "42"});
  Expect(model.NeedsPlaceholderRebuild(),
         "folding a value in marks the tree dirty for the next evaluation pass");

  model.BeginEvaluation();
  Expect(!model.NeedsPlaceholderRebuild(), "BeginEvaluation clears the dirty flag");

  model.ApplyEvaluate(0, codec::DapEvaluateResult{.result = "43"});
  model.EditExpression(0, "x + 1");
  Expect(!model.NeedsPlaceholderRebuild(),
         "editing rebuilds placeholders and clears the dirty flag");

  model.ApplyEvaluate(0, codec::DapEvaluateResult{.result = "44"});
  model.ClearResults();
  Expect(!model.NeedsPlaceholderRebuild(), "ClearResults leaves a pristine tree");
}

void TestDebugValueTreeDeepSubtreeEraseDoesNotOverflow() {
  DebugValueTree tree;
  tree.AddRoot("root", "{...}", "T", 1, false);
  int ref = 1;
  constexpr int kDepth = 40000;
  for (int d = 0; d < kDepth; ++d) {
    const int child_ref = ref + 1;
    tree.ApplyVariables(
        ref, {codec::DapVariable{.name = "c", .variables_reference = child_ref}}, 0);
    ref = child_ref;
  }
  tree.ApplyVariables(1, {codec::DapVariable{.name = "fresh", .variables_reference = 0}}, 0);
  tree.Rebuild();
  Expect(tree.Rows().size() == 1, "the deep subtree erase completed and left only the collapsed root");
}

void TestDebugWatchModelCapsExpressions() {
  DebugWatchModel model;
  for (int i = 0; i < 1000; ++i) {
    model.AddExpression("expr" + std::to_string(i));
  }
  Expect(model.Expressions().size() == 512, "watch expression count is capped at 512");
  const std::size_t before = model.Expressions().size();
  const std::size_t returned = model.AddExpression("one-more");
  Expect(model.Expressions().size() == before && returned == before,
         "AddExpression past the cap is a no-op returning the current size");
  model.SetExpressions(std::vector<std::string>(2000, "x"));
  Expect(model.Expressions().size() == 512, "SetExpressions also clamps the restored list");
  model.EditExpression(0, std::string(10000, 'a'));
  Expect(model.Expressions()[0].size() == 4096, "an over-long expression is truncated");
}

void TestDebugWatchNestedChildCascadeFetchIsReturned() {
  DebugWatchModel model;
  model.AddExpression("obj");
  model.ApplyEvaluate(
      0, codec::DapEvaluateResult{.result = "{...}", .type = "Obj", .variables_reference = 900});

  Expect(model.ToggleRow(0).reference == 900, "expanding obj returns its fetch reference");
  const auto obj_cascade = model.ApplyVariables(
      900, {codec::DapVariable{.name = "inner", .value = "{...}", .variables_reference = 901}}, 0);
  Expect(obj_cascade.empty(),
         "inner is not yet expanded, so applying obj's children cascades nothing");
  Expect(model.ToggleRow(1).reference == 901, "expanding inner returns its fetch reference");
  const auto inner_cascade = model.ApplyVariables(
      901, {codec::DapVariable{.name = "leaf", .value = "7", .variables_reference = 0}}, 0);
  Expect(inner_cascade.empty(), "a scalar leaf cascades nothing");

  const auto re_cascade = model.ApplyVariables(
      900, {codec::DapVariable{.name = "inner", .value = "{...}", .variables_reference = 921}}, 0);
  bool has_inner_fetch = false;
  for (const auto& fetch : re_cascade) {
    if (fetch.reference == 921) {
      has_inner_fetch = true;
    }
  }
  Expect(has_inner_fetch,
         "re-applying a watch root's children must return a cascade fetch for the remembered "
         "nested child so the service can issue it");
}

void TestDebugSessionRestartViaRestartRequest() {
  TemporaryDirectory temp_dir;
  const auto server_path = temp_dir.path() / "adapter.py";
  WriteFile(server_path, std::string(MockAdapterSource()));

  DapManager manager;
  manager.RegisterAdapter("mock", platform::LocalProcessLauncher(), MockAdapterCommand(server_path, "restart"));

  CapturedSession captured;
  LaunchConfig config;
  config.type = "mock";
  config.request = "launch";
  Expect(manager.StartSession(config, MakeCallbacks(captured)), "session should start");
  Expect(PollUntil(manager, [&]() { return captured.stop_count >= 1; }),
         "adapter should stop after the first configurationDone");
  DebugSession* session = manager.ActiveSession();
  Expect(session != nullptr && session->Client().Capabilities().supports_restart_request,
         "restart-mode adapter advertises supportsRestartRequest");

  const int stops_before = captured.stop_count;
  session->Restart();
  Expect(PollUntil(manager, [&]() { return captured.stop_count > stops_before; }),
         "restart should drive the adapter to re-stop");
  Expect(captured.output.find("cmd:restart") != std::string::npos,
         "Restart should send the DAP `restart` command");
  manager.ShutdownAll();
}

void TestDebugSessionRestartNoOpWithoutCapability() {
  TemporaryDirectory temp_dir;
  const auto server_path = temp_dir.path() / "adapter.py";
  WriteFile(server_path, std::string(MockAdapterSource()));

  DapManager manager;
  manager.RegisterAdapter("mock", platform::LocalProcessLauncher(), MockAdapterCommand(server_path, "stop"));

  CapturedSession captured;
  LaunchConfig config;
  config.type = "mock";
  config.request = "launch";
  Expect(manager.StartSession(config, MakeCallbacks(captured)), "session should start");
  Expect(PollUntil(manager, [&]() { return captured.stop_count >= 1; }), "adapter should stop");
  DebugSession* session = manager.ActiveSession();
  Expect(!session->Client().Capabilities().supports_restart_request,
         "stop-mode adapter does not advertise supportsRestartRequest");
  session->Restart();
  (void)PollStaysFalse(
      manager, [&]() { return captured.output.find("cmd:restart") != std::string::npos; },
      std::chrono::milliseconds(200));
  Expect(captured.output.find("cmd:restart") == std::string::npos,
         "Restart must not send a `restart` request without the capability");
  manager.ShutdownAll();
}

void TestDebugSessionThreadsCachedAndSwitch() {
  TemporaryDirectory temp_dir;
  const auto server_path = temp_dir.path() / "adapter.py";
  WriteFile(server_path, std::string(MockAdapterSource()));

  DapManager manager;
  manager.RegisterAdapter("mock", platform::LocalProcessLauncher(), MockAdapterCommand(server_path, "threads"));

  CapturedSession captured;
  LaunchConfig config;
  config.type = "mock";
  config.request = "launch";
  Expect(manager.StartSession(config, MakeCallbacks(captured)), "session should start");
  Expect(PollUntil(manager, [&]() { return captured.stop_count >= 1; }), "adapter should stop");
  Expect(PollUntil(manager, [&]() { return captured.last_threads.size() == 2; }),
         "the session should cache the adapter's full thread list on stop");
  Expect(captured.last_threads[0].name == "main" && captured.last_threads[1].name == "worker",
         "both threads should resolve with their names");
  Expect(captured.last_frames.size() == 2 && captured.last_frames[0].name == "main",
         "the initial stop resolves thread 1's frames");

  manager.ActiveSession()->SwitchThread(2);
  Expect(PollUntil(manager,
                   [&]() {
                     return !captured.last_frames.empty() &&
                            captured.last_frames[0].name == "worker";
                   }),
         "SwitchThread should re-resolve the picked thread's frames");
  manager.ShutdownAll();
}

void TestDebugSessionThreadEventRefreshesThreadList() {
  TemporaryDirectory temp_dir;
  const auto server_path = temp_dir.path() / "adapter.py";
  WriteFile(server_path, std::string(MockAdapterSource()));

  DapManager manager;
  manager.RegisterAdapter("mock", platform::LocalProcessLauncher(), MockAdapterCommand(server_path, "thread_event"));

  CapturedSession captured;
  LaunchConfig config;
  config.type = "mock";
  config.request = "launch";
  Expect(manager.StartSession(config, MakeCallbacks(captured)), "session should start");
  Expect(PollUntil(manager, [&]() { return captured.stop_count >= 1; }), "adapter should stop");
  Expect(PollUntil(manager, [&]() { return captured.last_threads.size() == 2; }),
         "a thread event should refresh the cached thread list");
  Expect(captured.last_threads[1].id == 2 && captured.last_threads[1].name == "worker",
         "the refreshed list should include the newly started thread");
  manager.ShutdownAll();
}

void TestDebugSessionExceptionFiltersSentOnLaunchAndToggle() {
  TemporaryDirectory temp_dir;
  const auto server_path = temp_dir.path() / "adapter.py";
  WriteFile(server_path, std::string(MockAdapterSource()));

  DapManager manager;
  manager.RegisterAdapter("mock", platform::LocalProcessLauncher(), MockAdapterCommand(server_path, "exception"));

  CapturedSession captured;
  DebugSession::Callbacks callbacks = MakeCallbacks(captured);
  std::vector<std::string> enabled = {"raised", "bogus"};
  callbacks.exception_filter_provider = [&enabled]() {
    std::vector<microide::workspace::ExceptionFilterRequest> requests;
    for (const std::string& id : enabled) {
      requests.push_back(microide::workspace::ExceptionFilterRequest{.id = id});
    }
    return requests;
  };

  LaunchConfig config;
  config.type = "mock";
  config.request = "launch";
  Expect(manager.StartSession(config, std::move(callbacks)), "session should start");
  Expect(PollUntil(manager, [&]() { return captured.stop_count >= 1; }), "adapter should stop");
  Expect(captured.advertised_filters.size() == 2,
         "the adapter's advertised exception filters should surface to the host");
  Expect(captured.advertised_filters[0].filter == "raised" &&
             captured.advertised_filters[1].default_enabled,
         "advertised filter ids + defaults should parse");
  Expect(captured.output.find("exc:raised\n") != std::string::npos,
         "only advertised enabled ids should be sent via setExceptionBreakpoints");

  enabled = {"raised", "uncaught"};
  manager.ActiveSession()->ResendExceptionFilters();
  Expect(PollUntil(manager,
                   [&]() { return captured.output.find("exc:raised,uncaught\n") != std::string::npos; }),
         "a live re-send should reflect the updated enabled set");
  manager.ShutdownAll();
}

void TestDebugBreakpointsModelPendingIsNotFailed() {
  using microide::workspace::DebugBreakpointsModel;
  using microide::workspace::DebugBreakpointRowView;

  const std::filesystem::path path = "/proj/prog.c";
  editor::BreakpointStore store;
  store.Set(path, 2);
  store.Set(path, 7);
  editor::FunctionBreakpointStore function_store;

  store.ApplyVerification(path, {
                                    editor::VerifiedBreakpoint{
                                        .id = 1, .verified = false, .line = 3,
                                        .message = "pending"},
                                    editor::VerifiedBreakpoint{
                                        .id = 2, .verified = false, .line = 8,
                                        .message = "failed"},
                                });

  DebugBreakpointsModel model;
  model.Rebuild(store, function_store);

  bool saw_pending = false;
  bool saw_failed = false;
  for (const DebugBreakpointRowView& row : model.Rows()) {
    if (row.kind != DebugBreakpointRowView::Kind::Breakpoint) {
      continue;
    }
    if (row.line == 2) {
      saw_pending = true;
      Expect(!row.failed,
             "a `pending` breakpoint must not be tinted as failed — gdb reports every "
             "pre-launch breakpoint that way and it still binds");
      Expect(row.secondary.find("pending") != std::string::npos,
             "the pending reason should still be surfaced in the muted trailer");
    }
    if (row.line == 7) {
      saw_failed = true;
      Expect(row.failed, "a `failed` breakpoint must still be tinted as failed");
      Expect(row.secondary.find("failed") != std::string::npos,
             "the failure reason should be surfaced in the trailer too");
    }
  }
  Expect(saw_pending && saw_failed, "both breakpoint rows should be present");
}

void TestDebugBreakpointsModelRebuildReusesRows() {
  using microide::workspace::DebugBreakpointsModel;
  using microide::workspace::DebugBreakpointRowView;

  editor::BreakpointStore store;
  const std::filesystem::path a = "/proj/alpha.cpp";
  const std::filesystem::path b = "/proj/beta.cpp";
  for (std::size_t line = 0; line < 6; ++line) {
    store.Set(a, line);
    store.Set(b, line);
  }
  store.SetCondition(a, 3, std::string("x > 0"));
  editor::FunctionBreakpointStore functions;
  functions.Add("main");

  DebugBreakpointsModel model;
  model.Rebuild(store, functions);
  const std::size_t wide_rows = model.RowCount();
  Expect(wide_rows == 1 + 1 + 1 + 12, "one function header+row, one breakpoint header, 12 rows");

  store.ClearFile(b);
  store.SetCondition(a, 3, std::nullopt);
  model.Rebuild(store, functions);
  Expect(model.RowCount() == 1 + 1 + 1 + 6, "the shrunken rebuild drops beta's six rows");
  for (const DebugBreakpointRowView& row : model.Rows()) {
    if (row.kind != DebugBreakpointRowView::Kind::Breakpoint) {
      continue;
    }
    Expect(row.path == a, "no row keeps the path of a file that is no longer in the store");
    Expect(row.display.find("alpha.cpp") != std::string::npos,
           "no row keeps a display string built for a different file");
    Expect(row.secondary.empty(),
           "a reused row must not keep the trailer of whatever occupied it before");
  }

  for (std::size_t line = 0; line < 6; ++line) {
    store.Set(b, line);
  }
  model.Rebuild(store, functions);
  Expect(model.RowCount() == wide_rows, "growing back restores every row");
  std::size_t beta_rows = 0;
  for (const DebugBreakpointRowView& row : model.Rows()) {
    if (row.kind == DebugBreakpointRowView::Kind::Breakpoint && row.path == b) {
      ++beta_rows;
      Expect(row.display.find("beta.cpp") != std::string::npos,
             "a re-grown row carries its own display string");
    }
  }
  Expect(beta_rows == 6, "every beta breakpoint comes back");

  for (int i = 0; i < 8; ++i) {
    functions.Add("fn_" + std::to_string(i));
  }
  model.Rebuild(store, functions);
  for (const DebugBreakpointRowView& row : model.Rows()) {
    if (row.kind == DebugBreakpointRowView::Kind::Breakpoint) {
      continue;
    }
    Expect(row.path.empty(),
           "a header/filter/function row must not inherit the path of whatever breakpoint "
           "row previously occupied its slot");
  }
}

void TestDebugBreakpointsModelBehavior() {
  using microide::workspace::DebugBreakpointsModel;
  using microide::workspace::DebugBreakpointRowView;
  DebugBreakpointsModel model;

  std::vector<codec::DapExceptionFilter> filters = {
      codec::DapExceptionFilter{.filter = "raised", .label = "Raised", .default_enabled = false},
      codec::DapExceptionFilter{.filter = "uncaught", .label = "Uncaught", .default_enabled = true},
  };
  Expect(model.SetAdvertisedFilters(filters), "first advertise seeds defaults (a change)");
  Expect(model.Seeded(), "the model records that defaults were seeded");
  Expect(model.IsEnabled("uncaught") && !model.IsEnabled("raised"),
         "the default-on filter is enabled, the default-off one is not");
  Expect((model.EnabledAdvertisedIds() == std::vector<std::string>{"uncaught"}),
         "EnabledAdvertisedIds returns only enabled advertised ids in advertised order");

  Expect(!model.SetAdvertisedFilters(filters), "re-advertise after seeding is not a change");

  Expect(model.ToggleFilter("raised"), "toggling an advertised filter changes the set");
  Expect(!model.ToggleFilter("nope"), "toggling an unadvertised filter is a no-op");
  Expect((model.EnabledAdvertisedIds() == std::vector<std::string>{"raised", "uncaught"}),
         "both filters are now enabled, in advertised order");

  editor::BreakpointStore store;
  store.Set("/proj/a.py", 4);
  store.SetCondition("/proj/a.py", 9, "i > 3");
  editor::FunctionBreakpointStore function_store;
  model.Rebuild(store, function_store);
  const std::vector<DebugBreakpointRowView>& rows = model.Rows();
  Expect(!rows.empty() && rows[0].kind == DebugBreakpointRowView::Kind::Header,
         "the first row is the exception-filters header");
  bool saw_filter = false;
  bool saw_breakpoint = false;
  for (const DebugBreakpointRowView& row : rows) {
    if (row.kind == DebugBreakpointRowView::Kind::ExceptionFilter && row.filter_id == "uncaught") {
      saw_filter = row.enabled;
    }
    if (row.kind == DebugBreakpointRowView::Kind::Breakpoint && row.display == "a.py:10") {
      saw_breakpoint = (row.secondary == "when i > 3");
    }
  }
  Expect(saw_filter, "an enabled filter row reflects its enabled state");
  Expect(saw_breakpoint, "a conditional breakpoint row prebuilds file:line + a condition trailer");

}

void TestDebugManagerRejectsUnknownAdapterType() {
  DapManager manager;
  CapturedSession captured;
  LaunchConfig config;
  config.type = "does-not-exist";
  Expect(!manager.StartSession(config, MakeCallbacks(captured)),
         "starting an unregistered adapter type should fail");
  Expect(!manager.LastError().empty(), "an unknown adapter type should record an error");
  Expect(manager.ActiveSession() == nullptr, "no session should be created for an unknown type");
}

void TestSummarizeCommandForErrorRedactsArgs() {
  using microide::workspace::SummarizeCommandForError;
  const std::string summary =
      SummarizeCommandForError({"/opt/adapters/dap-adapter", "--token=SUPERSECRET", "--port=9"});
  Expect(summary.find("SUPERSECRET") == std::string::npos,
         "argv secrets must not appear in the redacted summary");
  Expect(summary == "dap-adapter (2 args)",
         "the summary is the executable basename plus the argument count");
  Expect(SummarizeCommandForError({}) == "(no command)", "an empty command is summarized safely");
  Expect(SummarizeCommandForError({"clangd"}) == "clangd (0 args)",
         "a bare command reports zero arguments");
}

void TestDebugManagerRetainAdaptersDropsStaleTypes() {
  DapManager manager;
  manager.RegisterAdapter("a", platform::LocalProcessLauncher(), {"true"});
  manager.RegisterAdapter("b", platform::LocalProcessLauncher(), {"true"});
  Expect(manager.HasAdapter("a") && manager.HasAdapter("b"), "both adapters should register");
  manager.RetainAdaptersIn({"b"});
  Expect(!manager.HasAdapter("a"), "stale adapter 'a' should be dropped on reconcile");
  Expect(manager.HasAdapter("b"), "retained adapter 'b' should survive reconcile");
}

void TestDebugManagerAdapterDetailsCarryCommand() {
  DapManager manager;
  manager.RegisterAdapter("gdb", platform::LocalProcessLauncher(), {"gdb", "--interpreter=dap"});
  const std::vector<DapManager::AdapterInfo> details = manager.AdapterDetails();
  Expect(details.size() == 1, "one adapter should be reported");
  Expect(details[0].type == "gdb", "adapter type should be carried");
  Expect(details[0].command == std::vector<std::string>({"gdb", "--interpreter=dap"}),
         "adapter spawn command should be carried");
}

void TestDebugExecutionViewPanelRowDispatch() {
  using microide::workspace::DebugExecutionView;
  using Kind = DebugExecutionView::PanelRowRef::Kind;
  DebugExecutionView view;
  view.stopped = true;
  view.sessions = {{1, "server (paused)", false}, {2, "client (running)", true}};
  view.focused_session_id = 2;
  view.threads = {{1, "Thread 1"}, {2, "Thread 2"}};
  view.frames.resize(2);

  Expect(view.HasSessionSelector() && view.HasThreadSelector(), "both selectors are shown with >1");
  Expect(view.PanelRowCount() == 6, "2 sessions + 2 threads + 2 frames = 6 rows");
  Expect(view.PanelRowAt(0).kind == Kind::Session && view.PanelRowAt(0).index == 0, "row 0 = session 0");
  Expect(view.PanelRowAt(1).kind == Kind::Session && view.PanelRowAt(1).index == 1, "row 1 = session 1");
  Expect(view.PanelRowAt(2).kind == Kind::Thread && view.PanelRowAt(2).index == 0, "row 2 = thread 0");
  Expect(view.PanelRowAt(3).kind == Kind::Thread && view.PanelRowAt(3).index == 1, "row 3 = thread 1");
  Expect(view.PanelRowAt(4).kind == Kind::Frame && view.PanelRowAt(4).index == 0, "row 4 = frame 0");
  Expect(view.PanelRowAt(5).kind == Kind::Frame && view.PanelRowAt(5).index == 1, "row 5 = frame 1");

  view.sessions = {{1, "only", false}};
  Expect(!view.HasSessionSelector(), "one session hides the selector");
  Expect(view.PanelRowCount() == 4, "no session rows: 2 threads + 2 frames");
  Expect(view.PanelRowAt(0).kind == Kind::Thread, "row 0 is now the first thread");

  view.sessions = {{1, "a", false}, {2, "b", false}};
  view.focused_session_id = 2;
  view.Clear();
  Expect(view.sessions.size() == 2 && view.focused_session_id == 2,
         "Clear preserves the manager-sourced session selector");
  Expect(view.frames.empty() && view.threads.empty() && !view.stopped,
         "Clear drops the stop-scoped frames/threads/state");
}

void TestDebugManagerMultipleConcurrentSessions() {
  TemporaryDirectory temp_dir;
  const auto server_path = temp_dir.path() / "adapter.py";
  WriteFile(server_path, std::string(MockAdapterSource()));

  DapManager manager;
  manager.RegisterAdapter("mock", platform::LocalProcessLauncher(), MockAdapterCommand(server_path, "stop"));

  CapturedSession cap_a;
  CapturedSession cap_b;
  LaunchConfig config_a;
  config_a.name = "server";
  config_a.type = "mock";
  config_a.request = "launch";
  LaunchConfig config_b;
  config_b.name = "client";
  config_b.type = "mock";
  config_b.request = "launch";

  const int id_a = manager.StartSession(config_a, MakeCallbacks(cap_a));
  const int id_b = manager.StartSession(config_b, MakeCallbacks(cap_b));
  Expect(id_a != 0 && id_b != 0 && id_a != id_b, "two sessions get distinct non-zero ids");
  Expect(manager.SessionCount() == 2, "both sessions are live concurrently");
  Expect(manager.ActiveSessionId() == id_b, "the most recently started session is active");

  Expect(PollUntil(manager, [&]() { return cap_a.stop_count >= 1 && cap_b.stop_count >= 1; }),
         "both concurrent sessions reach a stop");

  const auto infos = manager.Sessions();
  Expect(infos.size() == 2, "Sessions() lists both sessions");
  Expect(infos[0].id == id_a && infos[0].name == "server", "first row keeps creation order + name");
  Expect(infos[1].id == id_b && infos[1].name == "client", "second row keeps creation order + name");
  Expect(infos[0].state == DebugSession::State::Stopped, "a stopped session reports Stopped");

  manager.SetSessionAttention(id_a, true);
  Expect(manager.Sessions()[0].attention, "attention flag is exposed for the switcher");
  manager.SetActiveSession(id_a);
  Expect(manager.ActiveSessionId() == id_a && manager.ActiveSession() == manager.SessionById(id_a),
         "SetActiveSession repoints the active session");

  const int before = cap_a.stop_count;
  manager.SessionById(id_a)->Reactivate();
  Expect(PollUntil(manager, [&]() { return cap_a.stop_count > before; }),
         "Reactivate re-resolves the stack and re-fires on_stopped");

  manager.StopActiveSession();
  Expect(PollUntil(manager,
                   [&]() {
                     manager.PruneTerminated();
                     return manager.SessionCount() == 1;
                   }),
         "the stopped active session is pruned once its adapter terminates");
  Expect(manager.ActiveSessionId() == id_b, "the active session advances to the survivor");

  manager.StopActiveSession();
  Expect(PollUntil(manager,
                   [&]() {
                     manager.PruneTerminated();
                     return manager.SessionCount() == 0;
                   }),
         "the last session prunes to an empty manager");
  Expect(manager.ActiveSessionId() == 0, "no active session id remains");
  manager.ShutdownAll();
}

void TestDebugManagerReplaceActiveSession() {
  TemporaryDirectory temp_dir;
  const auto server_path = temp_dir.path() / "adapter.py";
  WriteFile(server_path, std::string(MockAdapterSource()));

  DapManager manager;
  manager.RegisterAdapter("mock", platform::LocalProcessLauncher(), MockAdapterCommand(server_path, "config_done"));

  CapturedSession cap1;
  LaunchConfig config;
  config.name = "x";
  config.type = "mock";
  config.request = "launch";
  const int id1 = manager.StartSession(config, MakeCallbacks(cap1));
  Expect(id1 != 0 && manager.SessionCount() == 1, "the first session starts");

  CapturedSession cap2;
  const int id2 = manager.ReplaceActiveSession(config, MakeCallbacks(cap2));
  Expect(id2 != 0 && id2 != id1, "the replacement gets a fresh id");
  Expect(manager.SessionCount() == 1, "replace keeps a single session (no second row)");
  Expect(manager.ActiveSessionId() == id2, "the replacement is the active session");
  manager.ShutdownAll();
}

void TestDebugManagerStopAllSessions() {
  TemporaryDirectory temp_dir;
  const auto server_path = temp_dir.path() / "adapter.py";
  WriteFile(server_path, std::string(MockAdapterSource()));

  DapManager manager;
  manager.RegisterAdapter("mock", platform::LocalProcessLauncher(), MockAdapterCommand(server_path, "stop"));

  CapturedSession cap_a;
  CapturedSession cap_b;
  LaunchConfig config;
  config.type = "mock";
  config.request = "launch";
  const int id_a = manager.StartSession(config, MakeCallbacks(cap_a));
  const int id_b = manager.StartSession(config, MakeCallbacks(cap_b));
  Expect(id_a != 0 && id_b != 0 && manager.SessionCount() == 2, "two sessions start");
  Expect(PollUntil(manager, [&]() { return cap_a.stop_count >= 1 && cap_b.stop_count >= 1; }),
         "both sessions reach a stop");

  manager.BeginShutdownAll();
  manager.ShutdownAll();
  Expect(PollUntil(manager,
                   [&]() {
                     manager.PruneTerminated();
                     return manager.SessionCount() == 0;
                   }),
         "Stop All tears every session down to an empty manager");
  Expect(manager.ActiveSessionId() == 0, "no active session id remains after Stop All");
}

void TestDebugServiceTwoPhaseStopReporting() {
  TemporaryDirectory temp_dir;
  const auto server_path = temp_dir.path() / "adapter.py";
  WriteFile(server_path, std::string(MockAdapterSource()));

  microide::workspace::WorkspaceContext context;
  context.current_project_state.root = temp_dir.path();

  microide::workspace::DebugService service;
  struct Capture {
    int began = 0;
    int resolved = 0;
    std::string began_reason;
    int began_thread = 0;
    bool stopped_at_began = true;
    std::size_t frames_at_began = 99;
    bool stopped_at_resolved = false;
    std::size_t frames_at_resolved = 0;
    int order_began = 0;
    int order_resolved = 0;
    int tick = 0;
  } cap;
  const auto& exec = context.current_project_state.debug_execution;
  service.Configure(
      context,
      microide::workspace::DebugService::Operations{
          .notify_stop_began =
              [&](const std::string& reason, int thread_id) {
                ++cap.began;
                cap.began_reason = reason;
                cap.began_thread = thread_id;
                cap.stopped_at_began = exec.stopped;
                cap.frames_at_began = exec.frames.size();
                cap.order_began = ++cap.tick;
              },
          .notify_stop_resolved =
              [&]() {
                ++cap.resolved;
                cap.stopped_at_resolved = exec.stopped;
                cap.frames_at_resolved = exec.frames.size();
                cap.order_resolved = ++cap.tick;
              },
      });
  service.CurrentDapManager().RegisterAdapter("mock", platform::LocalProcessLauncher(), MockAdapterCommand(server_path, "stop"));

  LaunchConfig config;
  config.name = "session";
  config.type = "mock";
  config.request = "launch";
  Expect(service.StartDebugging(config), "the session should start");

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(4000);
  while (std::chrono::steady_clock::now() < deadline && cap.resolved == 0) {
    service.ConsumeDapCallbacks();
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  service.ConsumeDapCallbacks();

  Expect(cap.began == 1, "notify_stop_began should fire exactly once");
  Expect(cap.began_reason == "breakpoint", "the immediate stop carries the real reason");
  Expect(cap.began_thread == 1, "the immediate stop carries the real thread id");
  Expect(!cap.stopped_at_began && cap.frames_at_began == 0,
         "notify_stop_began fires before frames resolve (execution view still empty)");
  Expect(cap.resolved == 1, "notify_stop_resolved should fire exactly once");
  Expect(cap.stopped_at_resolved && cap.frames_at_resolved > 0,
         "notify_stop_resolved fires after the execution view is populated");
  Expect(cap.order_began != 0 && cap.order_began < cap.order_resolved,
         "the immediate event precedes the resolved one");

  service.StopAllDebugging();
}

void TestDebugServiceBackgroundStopDoesNotBroadcast() {
  TemporaryDirectory temp_dir;
  const auto server_path = temp_dir.path() / "adapter.py";
  WriteFile(server_path, std::string(MockAdapterSource()));

  microide::workspace::WorkspaceContext context;
  context.current_project_state.root = temp_dir.path();

  microide::workspace::DebugService service;
  int began = 0;
  service.Configure(context, microide::workspace::DebugService::Operations{
                                 .notify_stop_began = [&](const std::string&, int) { ++began; },
                                 .notify_stop_resolved = [&]() {}});
  service.CurrentDapManager().RegisterAdapter("mock", platform::LocalProcessLauncher(), MockAdapterCommand(server_path, "stop"));

  DapManager& manager = service.CurrentDapManager();
  LaunchConfig config;
  config.type = "mock";
  config.request = "launch";

  config.name = "A";
  Expect(service.StartDebugging(config), "session A should start");
  const int id_a = manager.ActiveSessionId();
  {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(4000);
    while (std::chrono::steady_clock::now() < deadline &&
           service.SessionState() != DebugSession::State::Stopped) {
      service.ConsumeDapCallbacks();
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
  }
  Expect(service.SessionState() == DebugSession::State::Stopped, "session A is parked at a stop");
  began = 0;

  config.name = "B";
  Expect(service.StartDebugging(config), "session B should start");
  const int id_b = manager.ActiveSessionId();
  Expect(id_b != id_a, "B is a distinct session");
  service.FocusSession(id_a);
  Expect(manager.ActiveSessionId() == id_a, "A is re-activated before B's stop drains");

  {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(4000);
    while (std::chrono::steady_clock::now() < deadline &&
           (manager.SessionById(id_b) == nullptr ||
            manager.SessionById(id_b)->CurrentState() != DebugSession::State::Stopped)) {
      service.ConsumeDapCallbacks();
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
  }
  Expect(manager.SessionById(id_b) != nullptr &&
             manager.SessionById(id_b)->CurrentState() == DebugSession::State::Stopped,
         "session B reached a background stop");
  Expect(began == 0, "a background session's stop does not drive the shared immediate broadcast");

  service.StopAllDebugging();
}

void TestDebugConsoleOutlivesItsSessionUntilTheNext() {
  TemporaryDirectory temp_dir;
  const auto server_path = temp_dir.path() / "adapter.py";
  WriteFile(server_path, std::string(MockAdapterSource()));
  microide::workspace::WorkspaceContext context;
  context.current_project_state.root = temp_dir.path();
  microide::workspace::DebugService service;
  std::vector<int> removed;
  std::string console;
  service.Configure(context, microide::workspace::DebugService::Operations{
                                 .append_console_output =
                                     [&](int, const std::string&, const codec::DapOutputEvent& output) {
                                       console += output.output;
                                     },
                                 .remove_debug_console = [&](int id) { removed.push_back(id); }});
  service.CurrentDapManager().RegisterAdapter("mock", platform::LocalProcessLauncher(),
                                              MockAdapterCommand(server_path, "config_done"));
  LaunchConfig config;
  config.type = "mock";
  config.request = "launch";
  config.name = "first";
  Expect(service.StartDebugging(config), "the first session should start");
  const int first = service.CurrentDapManager().ActiveSessionId();
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(8000);
  while (std::chrono::steady_clock::now() < deadline && service.CurrentDapManager().SessionCount() > 0) {
    service.ConsumeDapCallbacks();
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  Expect(service.CurrentDapManager().SessionCount() == 0, "the first session runs to its end");
  Expect(console.find("hello from adapter") != std::string::npos, "its output reached the console");
  Expect(removed.empty(), "a cleanly ended session keeps its console for reading");

  config.name = "second";
  Expect(service.StartDebugging(config), "the second session should start");
  Expect(std::find(removed.begin(), removed.end(), first) != removed.end(),
         "starting the next session drops the ended one's console");
  service.StopAllDebugging();
}

void TestDebugValueTreeNodeIdsAreMonotonic() {
  DebugVariablesModel model;
  model.BeginFrame(1);
  model.ApplyScopes({codec::DapScope{.name = "Arguments", .variables_reference = 1000}});
  Expect(!model.Rows().empty(), "first frame builds a scope row");
  const std::uint32_t first_id = model.Rows()[0].node_id;

  model.BeginFrame(2);
  model.ApplyScopes({codec::DapScope{.name = "Arguments", .variables_reference = 1000}});
  Expect(!model.Rows().empty(), "second frame builds a scope row");
  const std::uint32_t second_id = model.Rows()[0].node_id;
  Expect(second_id > first_id, "node ids stay globally monotonic across rebuilds");
}

void TestDebugValueTreePagingBoundaries() {
  DebugValueTree tree;
  tree.AddRoot("arr", "[950]", "int[950]", 2000, false,
               950, true);
  tree.Rebuild();

  DebugValueTree::ChildFetch fetch = tree.ToggleRow(0);
  Expect(fetch.reference == 2000 && fetch.start == 0 &&
             fetch.count == DebugValueTree::kChildPageSize,
         "first expand fetches a bounded page (200), not all 950 children");

  int loaded = 0;
  int pages = 0;
  std::vector<int> page_counts;
  while (fetch.reference != 0) {
    Expect(fetch.count <= DebugValueTree::kChildPageSize, "every page stays within the page bound");
    Expect(fetch.start == loaded, "each page resumes exactly where the prior one ended");
    std::vector<codec::DapVariable> vars;
    vars.reserve(static_cast<std::size_t>(fetch.count));
    for (int i = 0; i < fetch.count; ++i) {
      codec::DapVariable v;
      v.name = "e" + std::to_string(fetch.start + i);
      v.value = "0";
      vars.push_back(std::move(v));
    }
    tree.ApplyVariables(fetch.reference, vars, fetch.start);
    loaded += fetch.count;
    page_counts.push_back(fetch.count);
    ++pages;
    const auto& rows = tree.Rows();
    if (!rows.empty() && rows.back().is_show_more) {
      fetch = tree.ToggleRow(rows.size() - 1);
    } else {
      break;
    }
  }

  Expect(loaded == 950, "the whole container loads across the page sequence");
  Expect(pages == 5, "950 elements stream in 5 bounded pages (200*4 + 150)");
  Expect(page_counts.back() == 150, "the final page clamps to the 150-element remainder");
  Expect(tree.Rows().size() == 951, "container row + 950 children, no trailing show-more");
  Expect(!tree.Rows().back().is_show_more, "no show-more affordance once fully paged");
}

void TestDebugValueTreeDeepNestingRestoreAcrossStops() {
  DebugValueTree tree;

  tree.AddRoot("Locals", {}, {}, 1000, true,
               1, true);
  tree.Rebuild();
  Expect(tree.ToggleRow(0).reference == 1000, "expanding Locals fetches its scope ref");
  codec::DapVariable node;
  node.name = "node";
  node.value = "{...}";
  node.variables_reference = 1001;
  node.named_variables = 1;
  node.count_reported = true;
  tree.ApplyVariables(1000, {node}, 0);
  std::size_t node_row = 0;
  for (std::size_t r = 0; r < tree.Rows().size(); ++r) {
    if (tree.Rows()[r].display_name == "node") {
      node_row = r;
    }
  }
  Expect(node_row != 0, "the struct child row is present after the first page");
  Expect(tree.ToggleRow(node_row).reference == 1001, "expanding the struct fetches its child ref");
  codec::DapVariable leaf;
  leaf.name = "leaf";
  leaf.value = "42";
  tree.ApplyVariables(1001, {leaf}, 0);
  Expect(tree.Rows().size() == 3, "Locals + node + leaf flatten to three rows");

  tree.ClearRoots();
  tree.AddRoot("Locals", {}, {}, 2000, true,
               1, true);
  const std::vector<DebugValueTree::ChildFetch> restore = tree.RestoreExpandedRoots();
  tree.Rebuild();
  Expect(restore.size() == 1 && restore[0].reference == 2000,
         "the previously-expanded Locals scope re-expands against its new ref");

  codec::DapVariable node2;
  node2.name = "node";
  node2.value = "{...}";
  node2.variables_reference = 2001;
  node2.named_variables = 1;
  node2.count_reported = true;
  const std::vector<DebugValueTree::ChildFetch> cascade = tree.ApplyVariables(2000, {node2}, 0);
  Expect(cascade.size() == 1 && cascade[0].reference == 2001,
         "nested expansion cascades to the renumbered struct ref by path tracking");
}

void TestDebugValueTreeSameNamedSiblingsExpandIndependently() {
  DebugValueTree tree;
  tree.AddRoot("Locals", {}, {}, 1000,
               true, 2, true);
  tree.Rebuild();
  Expect(tree.ToggleRow(0).reference == 1000, "expanding Locals fetches its scope ref");

  codec::DapVariable a;
  a.name = "item";
  a.value = "A";
  a.variables_reference = 1001;
  a.named_variables = 1;
  a.count_reported = true;
  codec::DapVariable b;
  b.name = "item";
  b.value = "B";
  b.variables_reference = 1002;
  b.named_variables = 1;
  b.count_reported = true;
  tree.ApplyVariables(1000, {a, b}, 0);
  Expect(tree.Rows().size() == 3, "two same-named siblings both flatten under Locals");
  Expect(tree.Rows()[1].display_name == "item" && tree.Rows()[2].display_name == "item",
         "both siblings share the display name");

  Expect(tree.ToggleRow(1).reference == 1001, "expanding the first item fetches its own ref");
  codec::DapVariable leaf;
  leaf.name = "leaf";
  leaf.value = "1";
  tree.ApplyVariables(1001, {leaf}, 0);
  Expect(tree.Rows().size() == 4, "only the first sibling's child is revealed");
  Expect(tree.Rows()[1].expanded && !tree.Rows()[3].expanded,
         "expanding one same-named sibling must not expand the other");
  Expect(tree.Rows()[2].display_name == "leaf" && tree.Rows()[2].depth == 2,
         "the first sibling's child interleaves before the second sibling");

  tree.ClearRoots();
  tree.AddRoot("Locals", {}, {}, 2000, true,
               2, true);
  tree.RestoreExpandedRoots();
  tree.Rebuild();
  codec::DapVariable a2 = a;
  a2.variables_reference = 2001;
  codec::DapVariable b2 = b;
  b2.variables_reference = 2002;
  const std::vector<DebugValueTree::ChildFetch> cascade = tree.ApplyVariables(2000, {a2, b2}, 0);
  Expect(cascade.size() == 1 && cascade[0].reference == 2001,
         "only the first same-named sibling re-expands by its ordinal-qualified path");
}

void TestDebugSessionReconciledWhenAdapterDiesSilently() {
  TemporaryDirectory temp_dir;
  const auto server_path = temp_dir.path() / "adapter.py";
  WriteFile(server_path, std::string(MockAdapterSource()));

  DapManager manager;
  manager.RegisterAdapter("mock", platform::LocalProcessLauncher(), MockAdapterCommand(server_path, "die"));

  CapturedSession captured;
  LaunchConfig config;
  config.type = "mock";
  config.request = "launch";
  Expect(manager.StartSession(config, MakeCallbacks(captured)), "session should start");

  Expect(PollUntil(manager,
                   [&]() {
                     const DebugSession* s = manager.ActiveSession();
                     return s != nullptr && !s->Client().IsRunning();
                   }),
         "adapter process should exit after reaching Running");

  Expect(manager.PruneTerminated().empty(), "a non-terminal session is not prunable on its own");
  const DebugSession* before = manager.ActiveSession();
  Expect(before != nullptr && before->CurrentState() != DebugSession::State::Terminated &&
             before->CurrentState() != DebugSession::State::Failed,
         "the dead-but-unreconciled session is still non-terminal");

  Expect(manager.ReapExitedSessions(), "ReapExitedSessions should transition the dead session");
  const DebugSession* after = manager.ActiveSession();
  Expect(after != nullptr && after->CurrentState() == DebugSession::State::Failed,
         "a silently-dead adapter session becomes Failed");
  Expect(captured.got_terminated_callback,
         "on_terminated fires when the adapter dies without a DAP terminated event");
  Expect(captured.terminated_failed, "a silent adapter death reports failed=true");
  Expect(!captured.terminated_reason.empty(), "the terminated reason is populated for a crash");
  Expect(!captured.got_terminated_event,
         "no clean DAP terminated arrived, so the Terminated-state flag stays false");
  const std::vector<microide::workspace::PrunedSession> pruned = manager.PruneTerminated();
  Expect(pruned.size() == 1, "the reconciled session is then pruned");
  Expect(pruned.front().failed, "the pruned entry is marked failed so its console is kept");
  Expect(!pruned.front().error.empty(), "the pruned entry carries the failure reason");
  Expect(manager.SessionCount() == 0, "no zombie session remains");
}

void TestDebugSessionFunctionBreakpointsSentOnLaunch() {
  TemporaryDirectory temp_dir;
  const auto server_path = temp_dir.path() / "adapter.py";
  WriteFile(server_path, std::string(MockAdapterSource()));

  DapManager manager;
  manager.RegisterAdapter("mock", platform::LocalProcessLauncher(), MockAdapterCommand(server_path, "function"));

  editor::FunctionBreakpointStore fn_store;
  fn_store.Add("add");
  fn_store.Add("compute");
  fn_store.SetCondition(1, "n > 0");

  editor::BreakpointStore line_store;
  line_store.Set("/proj/main.c", 9);

  CapturedSession captured;
  DebugSession::Callbacks callbacks = MakeCallbacks(captured);
  callbacks.breakpoint_provider = [&line_store]() { return line_store.SnapshotAll(); };
  callbacks.function_breakpoint_provider = [&fn_store]() { return fn_store.All(); };
  callbacks.on_function_breakpoints_verified =
      [&fn_store](const std::vector<std::string>& names,
                  const std::vector<codec::DapBreakpoint>& bps) {
        std::vector<editor::VerifiedFunctionBreakpoint> results;
        for (const codec::DapBreakpoint& bp : bps) {
          results.push_back(editor::VerifiedFunctionBreakpoint{
              .id = bp.id, .verified = bp.verified, .message = bp.message});
        }
        fn_store.ApplyVerification(names, results);
      };

  LaunchConfig config;
  config.type = "mock";
  config.request = "launch";
  Expect(manager.StartSession(config, std::move(callbacks)), "session should start");
  Expect(PollUntil(manager, [&]() { return captured.stop_count >= 1; }), "adapter should stop");

  Expect(captured.output.find("fnbp:add,compute\n") != std::string::npos,
         "both function breakpoints should reach the wire by name");
  Expect(captured.output.find("fncond:,n > 0\n") != std::string::npos,
         "the per-breakpoint condition rides along (empty for the first)");
  const std::string& out = captured.output;
  const std::size_t commands_pos = out.find("commands:");
  Expect(commands_pos != std::string::npos, "the adapter echoes its received command order");
  const std::string commands = out.substr(commands_pos);
  const std::size_t set_bp = commands.find("setBreakpoints");
  const std::size_t set_fn = commands.find("setFunctionBreakpoints");
  const std::size_t config_done = commands.find("configurationDone");
  Expect(set_fn != std::string::npos && config_done != std::string::npos,
         "setFunctionBreakpoints + configurationDone are both on the wire");
  Expect(set_bp < set_fn && set_fn < config_done,
         "setFunctionBreakpoints lands after setBreakpoints and before configurationDone");
  Expect(fn_store.All().size() == 2 && fn_store.All()[0].verified && fn_store.All()[1].verified,
         "verification reflects back onto both function breakpoints");
  manager.ShutdownAll();
}

void TestDebugSessionExceptionFilterConditionsSent() {
  TemporaryDirectory temp_dir;
  const auto server_path = temp_dir.path() / "adapter.py";
  WriteFile(server_path, std::string(MockAdapterSource()));

  DapManager manager;
  manager.RegisterAdapter("mock", platform::LocalProcessLauncher(), MockAdapterCommand(server_path, "exception"));

  CapturedSession captured;
  DebugSession::Callbacks callbacks = MakeCallbacks(captured);
  callbacks.exception_filter_provider = []() {
    return std::vector<microide::workspace::ExceptionFilterRequest>{
        microide::workspace::ExceptionFilterRequest{.id = "raised", .condition = "x == 2"},
        microide::workspace::ExceptionFilterRequest{.id = "uncaught"},
    };
  };

  LaunchConfig config;
  config.type = "mock";
  config.request = "launch";
  Expect(manager.StartSession(config, std::move(callbacks)), "session should start");
  Expect(PollUntil(manager, [&]() { return captured.stop_count >= 1; }), "adapter should stop");
  Expect(captured.output.find("excopt:raised=x == 2\n") != std::string::npos,
         "a conditioned filter is sent via filterOptions");
  Expect(captured.output.find("exc:uncaught\n") != std::string::npos,
         "an unconditioned filter stays in the plain filters array");
  manager.ShutdownAll();
}

void TestFunctionBreakpointStoreBehavior() {
  editor::FunctionBreakpointStore store;
  Expect(store.Add("main"), "adding a fresh name succeeds");
  Expect(!store.Add("main"), "adding a duplicate name is a no-op");
  Expect(!store.Add(""), "adding an empty name is a no-op");
  Expect(store.Add("helper"), "adding a second distinct name succeeds");
  Expect(store.Count() == 2, "two function breakpoints are stored");

  store.SetCondition(0, "argc > 1");
  Expect(store.All()[0].condition.has_value() && *store.All()[0].condition == "argc > 1",
         "a condition is stored on the right breakpoint");
  Expect(store.ToggleEnabled(1) && !store.All()[1].enabled, "toggling disables the second");

  std::vector<std::string> requested = {"helper", "main"};
  std::vector<editor::VerifiedFunctionBreakpoint> results = {
      editor::VerifiedFunctionBreakpoint{.id = 7, .verified = false, .message = "pending"},
      editor::VerifiedFunctionBreakpoint{.id = 8, .verified = true},
  };
  store.ApplyVerification(requested, results);
  Expect(store.All()[0].name == "main" && store.All()[0].verified && store.All()[0].adapter_id == 8,
         "main (requested second) verifies true with its adapter id");
  Expect(store.All()[1].name == "helper" && !store.All()[1].verified &&
             store.All()[1].verify_message == "pending",
         "helper (requested first) reflects its pending reason");

  store.ResetVerification();
  Expect(!store.All()[0].verified && store.All()[0].adapter_id == 0,
         "ResetVerification drops transient adapter state");
  store.Remove(0);
  Expect(store.Count() == 1 && store.All()[0].name == "helper", "Remove drops the right row");
}

void TestFunctionBreakpointStoreReplaceAllDedupes() {
  editor::FunctionBreakpointStore store;
  std::vector<editor::FunctionBreakpoint> restored = {
      editor::FunctionBreakpoint{.name = "main", .verified = true, .adapter_id = 9},
      editor::FunctionBreakpoint{.name = ""},
      editor::FunctionBreakpoint{.name = "helper"},
      editor::FunctionBreakpoint{.name = "main"},
      editor::FunctionBreakpoint{.name = "helper"},
      editor::FunctionBreakpoint{.name = "tail"},
  };
  store.ReplaceAll(std::move(restored));
  Expect(store.Count() == 3, "empty and duplicate names are dropped");
  Expect(store.All()[0].name == "main" && store.All()[1].name == "helper" &&
             store.All()[2].name == "tail",
         "surviving names keep first-seen order");
  Expect(!store.All()[0].verified && store.All()[0].adapter_id == 0,
         "ReplaceAll drops persisted transient verification state");
}

void TestFunctionBreakpointStoreApplyVerificationManyNames() {
  editor::FunctionBreakpointStore store;
  constexpr int kCount = 2000;
  for (int i = 0; i < kCount; ++i) {
    Expect(store.Add("fn_" + std::to_string(i)), "each distinct name adds");
  }
  std::vector<std::string> requested = {"fn_1999", "fn_0", "fn_1000", "fn_missing"};
  std::vector<editor::VerifiedFunctionBreakpoint> results = {
      editor::VerifiedFunctionBreakpoint{.id = 11, .verified = true},
      editor::VerifiedFunctionBreakpoint{.id = 22, .verified = true},
      editor::VerifiedFunctionBreakpoint{.id = 33, .verified = false, .message = "no code"},
      editor::VerifiedFunctionBreakpoint{.id = 44, .verified = true},
  };
  store.ApplyVerification(requested, results);
  const auto find = [&](const std::string& name) -> const editor::FunctionBreakpoint* {
    for (const auto& bp : store.All()) {
      if (bp.name == name) {
        return &bp;
      }
    }
    return nullptr;
  };
  Expect(find("fn_1999")->verified && find("fn_1999")->adapter_id == 11, "last name verifies");
  Expect(find("fn_0")->verified && find("fn_0")->adapter_id == 22, "first name verifies");
  Expect(!find("fn_1000")->verified && find("fn_1000")->verify_message == "no code",
         "middle name reflects rejection");
  Expect(find("fn_500")->adapter_id == 0, "unrelated rows stay untouched");
}

bool GdbDapAvailable() {
  FILE* pipe = ::popen("gdb --version 2>/dev/null", "r");
  if (pipe == nullptr) {
    return false;
  }
  std::string out;
  char buffer[256];
  std::size_t n = 0;
  while ((n = std::fread(buffer, 1, sizeof(buffer), pipe)) > 0) {
    out.append(buffer, n);
  }
  ::pclose(pipe);
  return out.find("GNU gdb") != std::string::npos;
}

bool CompileWithGcc(const std::filesystem::path& source, const std::filesystem::path& exe) {
  const std::string command =
      "gcc -g -O0 -o " + exe.string() + " " + source.string() + " 2>/dev/null";
  return std::system(command.c_str()) == 0;
}

void TestGdbRealFunctionBreakpointsE2E() {
  if (!GdbDapAvailable()) {
    return;
  }
  TemporaryDirectory temp_dir;
  const auto source = temp_dir.path() / "prog.c";
  WriteFile(source,
            "#include <stdio.h>\n"
            "#include <unistd.h>\n"
            "int add(int a, int b){ int s = a + b; return s; }\n"
            "int main(){\n"
            "  int z = 0;\n"
            "  for (int i = 0; i < 60000; i++) { z += add(i, i + 1); usleep(2000); }\n"
            "  printf(\"%d\\n\", z);\n"
            "  return 0;\n"
            "}\n");
  const auto exe = temp_dir.path() / "prog";
  if (!CompileWithGcc(source, exe)) {
    return;
  }

  DapManager manager;
  manager.RegisterAdapter("gdb", platform::LocalProcessLauncher(), std::vector<std::string>{"gdb", "--interpreter=dap"});

  editor::FunctionBreakpointStore fn_store;
  fn_store.Add("add");

  LaunchConfig config;
  config.type = "gdb";
  config.request = "launch";
  {
    util::JsonObject args;
    args["program"] = JsonValue(exe.string());
    args["stopAtBeginningOfMainSubprogram"] = JsonValue(true);
    config.arguments = JsonValue(std::move(args));
  }

  CapturedSession captured;
  DebugSession::Callbacks callbacks = MakeCallbacks(captured);
  callbacks.function_breakpoint_provider = [&fn_store]() { return fn_store.All(); };
  callbacks.on_function_breakpoints_verified =
      [&fn_store](const std::vector<std::string>& names,
                  const std::vector<codec::DapBreakpoint>& bps) {
        std::vector<editor::VerifiedFunctionBreakpoint> results;
        for (const codec::DapBreakpoint& bp : bps) {
          results.push_back(editor::VerifiedFunctionBreakpoint{
              .id = bp.id, .verified = bp.verified, .message = bp.message});
        }
        fn_store.ApplyVerification(names, results);
      };
  callbacks.on_breakpoint_changed = [&fn_store](const std::filesystem::path&,
                                                const codec::DapBreakpoint& bp) {
    fn_store.ApplyBreakpointEvent(editor::VerifiedFunctionBreakpoint{
        .id = bp.id, .verified = bp.verified, .message = bp.message});
  };

  Expect(manager.StartSession(config, std::move(callbacks)),
         "the gdb session should start");

  Expect(PollUntil(manager, [&]() { return captured.stop_count >= 1; }),
         "gdb should stop at the beginning of main");
  const bool stopped_at_add_directly =
      !captured.last_frames.empty() &&
      captured.last_frames[0].name.find("add") != std::string::npos;
  Expect(PollUntil(manager,
                   [&]() { return !fn_store.All().empty() && fn_store.All()[0].verified; }),
         "the function breakpoint verifies against real gdb while stopped at main");

  bool stopped_in_add = stopped_at_add_directly;
  if (!stopped_in_add) {
    const int stops_before = captured.stop_count;
    DebugSession* session = manager.ActiveSession();
    Expect(session != nullptr, "the session should be active while stopped at main");
    session->Continue();
    Expect(PollUntil(manager, [&]() { return captured.stop_count > stops_before; }),
           "gdb should stop again once add() is called");
    stopped_in_add = !captured.last_frames.empty() &&
                     captured.last_frames[0].name.find("add") != std::string::npos;
  }
  manager.ShutdownAll();
  Expect(stopped_in_add, "gdb should stop inside add() via the function breakpoint");
}

void TestDebugServiceBreakpointVerificationStaysAtRequestedLine() {
  TemporaryDirectory temp_dir;
  const auto server_path = temp_dir.path() / "adapter.py";
  WriteFile(server_path, std::string(MockAdapterSource()));

  microide::workspace::WorkspaceContext context;
  context.current_project_state.root = temp_dir.path();
  const std::filesystem::path bp_path = temp_dir.path() / "main.c";
  context.current_project_state.breakpoint_store.Set(bp_path, 9);
  context.current_project_state.breakpoint_store.Set(bp_path, 19);

  microide::workspace::DebugService service;
  service.Configure(context, microide::workspace::DebugService::Operations{});
  service.CurrentDapManager().RegisterAdapter("mock", platform::LocalProcessLauncher(), MockAdapterCommand(server_path, "relocate"));

  LaunchConfig config;
  config.name = "session";
  config.type = "mock";
  config.request = "launch";
  Expect(service.StartDebugging(config), "the session should start");

  const auto both_verified = [&]() {
    const auto* v = context.current_project_state.breakpoint_store.FindByPath(bp_path);
    if (v == nullptr || v->size() != 2) {
      return false;
    }
    return (*v)[0].verified && (*v)[1].verified;
  };
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(4000);
  while (std::chrono::steady_clock::now() < deadline && !both_verified()) {
    service.ConsumeDapCallbacks();
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  Expect(both_verified(), "both breakpoints should verify from the setBreakpoints response");
  for (int i = 0; i < 25; ++i) {
    service.ConsumeDapCallbacks();
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }

  const auto* bps = context.current_project_state.breakpoint_store.FindByPath(bp_path);
  Expect(bps != nullptr && bps->size() == 2, "both breakpoints remain, none added/dropped");
  if (bps != nullptr && bps->size() == 2) {
    const editor::Breakpoint* first = nullptr;
    const editor::Breakpoint* second = nullptr;
    for (const auto& bp : *bps) {
      if (bp.line == 9) first = &bp;
      if (bp.line == 19) second = &bp;
    }
    Expect(first != nullptr,
           "the relocated breakpoint stays at the user's requested line 9, not the adapter line");
    Expect(first != nullptr && first->verified && first->adapter_id == 1,
           "the requested-line breakpoint verifies with its own adapter id across response+event");
    Expect(second != nullptr && second->verified && second->adapter_id == 2,
           "an extra non-requested result must not clobber the second breakpoint's adapter id");
  }

  service.StopAllDebugging();
}

void TestDebugSessionPartialContinueDoesNotResume() {
  TemporaryDirectory temp_dir;
  const auto server_path = temp_dir.path() / "adapter.py";
  WriteFile(server_path, std::string(MockAdapterSource()));

  DapManager manager;
  manager.RegisterAdapter("mock", platform::LocalProcessLauncher(), MockAdapterCommand(server_path, "partial_continue"));

  CapturedSession captured;
  LaunchConfig config;
  config.type = "mock";
  config.request = "launch";
  Expect(manager.StartSession(config, MakeCallbacks(captured)), "session should start");

  Expect(PollUntil(manager,
                   [&]() {
                     return captured.output.find("partial-continue-sent") != std::string::npos;
                   }),
         "adapter should emit the stop and the partial continued event");

  DebugSession* session = manager.ActiveSession();
  Expect(session != nullptr && session->CurrentState() == DebugSession::State::Stopped,
         "a partial continued (allThreadsContinued=false) must not resume the session");
  Expect(captured.resume_count == 0,
         "on_resumed must not fire for a single-thread continue");
  manager.ShutdownAll();
}

void TestDebugSessionStopWithoutThreadIdResolvesThread() {
  TemporaryDirectory temp_dir;
  const auto server_path = temp_dir.path() / "adapter.py";
  WriteFile(server_path, std::string(MockAdapterSource()));

  DapManager manager;
  manager.RegisterAdapter("mock", platform::LocalProcessLauncher(), MockAdapterCommand(server_path, "stop_no_thread"));

  CapturedSession captured;
  LaunchConfig config;
  config.type = "mock";
  config.request = "launch";
  Expect(manager.StartSession(config, MakeCallbacks(captured)), "session should start");

  Expect(PollUntil(manager, [&]() { return captured.stop_count >= 1; }),
         "a threadId-less stop should still resolve a stack (via a resolved thread)");
  Expect(captured.output.find("stack-thread:1") != std::string::npos,
         "stackTrace must target the resolved thread, not threadId 0");
  Expect(captured.output.find("stack-thread:0") == std::string::npos,
         "stackTrace must never send threadId 0");

  DebugSession* session = manager.ActiveSession();
  Expect(session != nullptr, "session should be active while stopped");
  session->Continue();
  Expect(PollUntil(manager,
                   [&]() { return captured.output.find("resume-thread:") != std::string::npos; }),
         "continue should reach the adapter");
  Expect(captured.output.find("resume-thread:1") != std::string::npos,
         "continue must target the resolved thread, not threadId 0");
  Expect(captured.output.find("resume-thread:0") == std::string::npos,
         "continue must never send threadId 0");
  manager.ShutdownAll();
}

void TestLaunchArgumentsExpandWorkspaceFolder() {
  auto parsed = util::ParseJson(
      R"({"program": "${workspaceFolder}/build/app", "args": ["--root", "${workspaceFolder}"],)"
      R"( "env": {"HOME": "${workspaceFolder}/home"}, "stopOnEntry": true})");
  Expect(parsed.has_value(), "the fixture parses");
  if (!parsed) {
    return;
  }
  workspace::ExpandLaunchVariables(*parsed, "/p/proj");
  Expect((*parsed)["program"].AsString() == "/p/proj/build/app", "program expands");
  Expect((*parsed)["args"].AsArray()[1].AsString() == "/p/proj", "array items expand");
  Expect((*parsed)["env"]["HOME"].AsString() == "/p/proj/home", "nested objects expand");
  Expect((*parsed)["stopOnEntry"].AsBool(), "non-strings are left alone");
}

void TestStopFocusesTheTopFrameWithSource() {
  using microide::workspace::TopFrameWithSource;
  using microide::workspace::dap_protocol::DapStackFrame;
  const auto frame = [](std::string path, std::string hint = {}) {
    DapStackFrame f;
    f.source.path = std::move(path);
    f.presentation_hint = std::move(hint);
    return f;
  };
  const auto on_disk = [](const std::string& path) { return path.rfind("/project/", 0) == 0; };
  const std::vector<DapStackFrame> paused = {frame("nptl/cancellation.c"),
                                             frame("sysdeps/unix/sysv/linux/clock_nanosleep.c"),
                                             frame(""), frame("/project/spin.c")};
  Expect(TopFrameWithSource(paused, on_disk) == 3, "the program's frame, past the library's");
  Expect(TopFrameWithSource(std::vector<DapStackFrame>{frame("/project/a.c"), frame("/project/b.c")},
                            on_disk) == 0,
         "a top frame with source stays the one");
  Expect(TopFrameWithSource(std::vector<DapStackFrame>{frame("/project/x.c", "label"),
                                                       frame("/project/y.c", "subtle"),
                                                       frame("/project/z.c")},
                            on_disk) == 2,
         "label and subtle frames are not the stop's");
  Expect(TopFrameWithSource(std::vector<DapStackFrame>{frame("libc.c"), frame("")}, on_disk) == 0,
         "with no source anywhere, the top frame, as before");
  Expect(TopFrameWithSource(std::vector<DapStackFrame>{}, on_disk) == 0, "no frames: index 0");
}

}

void RegisterDebugServiceTests(std::vector<TestCase>& tests) {
  AddTest(tests, "DebugService/StopFocusesTheTopFrameWithSource", TestStopFocusesTheTopFrameWithSource);
  AddTest(tests, "DebugService/LaunchArgumentsExpandWorkspaceFolder", TestLaunchArgumentsExpandWorkspaceFolder);
  AddTest(tests, "DebugService/ValueTreeNodeIdsAreMonotonic",
          TestDebugValueTreeNodeIdsAreMonotonic);
  AddTest(tests, "DebugService/ValueTreePagingBoundaries", TestDebugValueTreePagingBoundaries);
  AddTest(tests, "DebugService/ValueTreeDeepNestingRestoreAcrossStops",
          TestDebugValueTreeDeepNestingRestoreAcrossStops);
  AddTest(tests, "DebugService/ValueTreeSameNamedSiblingsExpandIndependently",
          TestDebugValueTreeSameNamedSiblingsExpandIndependently);
  AddTest(tests, "DebugService/SessionReconciledWhenAdapterDiesSilently",
          TestDebugSessionReconciledWhenAdapterDiesSilently);
  AddTest(tests, "DebugService/SessionDrivesLaunchLifecycleWithConfigurationDone",
          TestDebugSessionDrivesLaunchLifecycleWithConfigurationDone);
  AddTest(tests, "DebugService/SessionRunsWithoutConfigurationDoneSupport",
          TestDebugSessionRunsWithoutConfigurationDoneSupport);
  AddTest(tests, "DebugService/SessionLaunchHandshakeOrder",
          TestDebugSessionLaunchHandshakeOrder);
  AddTest(tests, "DebugService/SessionResolvesStackOnStopAndStepsResume",
          TestDebugSessionResolvesStackOnStopAndStepsResume);
  AddTest(tests, "DebugService/SessionDropsStaleStackResolvedAfterResume",
          TestDebugSessionDropsStaleStackResolvedAfterResume);
  AddTest(tests, "DebugService/SessionRejectedResumeRestoresStoppedView",
          TestDebugSessionRejectedResumeRestoresStoppedView);
  AddTest(tests, "DebugService/SessionPauseFromRunning", TestDebugSessionPauseFromRunning);
  AddTest(tests, "DebugService/SessionPauseInterruptsAGdbStillStartingTheProgram",
          TestDebugSessionPauseInterruptsAGdbStillStartingTheProgram);
  AddTest(tests, "DebugService/AStopTheAdapterDoesNotAnswerEndsTheSession",
          TestAStopTheAdapterDoesNotAnswerEndsTheSession);
  AddTest(tests, "DebugService/TeardownEndsALaunchedDebuggeeAndSparesAnAttachedOne",
          TestTeardownEndsALaunchedDebuggeeAndSparesAnAttachedOne);
  AddTest(tests, "DebugService/ConsoleOutlivesItsSessionUntilTheNext",
          TestDebugConsoleOutlivesItsSessionUntilTheNext);
  AddTest(tests, "DebugService/SessionPausesAThreadAnnouncedByEvent",
          TestDebugSessionPausesAThreadAnnouncedByEvent);
  AddTest(tests, "DebugService/SessionReverseStepAndContinue",
          TestDebugSessionReverseStepAndContinue);
  AddTest(tests, "DebugService/SessionReverseGatedOnCapability",
          TestDebugSessionReverseGatedOnCapability);
  AddTest(tests, "DebugService/SessionReverseEnabledByLateCapabilitiesEvent",
          TestDebugSessionReverseEnabledByLateCapabilitiesEvent);
  AddTest(tests, "DebugService/SessionRestartViaRestartRequest",
          TestDebugSessionRestartViaRestartRequest);
  AddTest(tests, "DebugService/SessionRestartNoOpWithoutCapability",
          TestDebugSessionRestartNoOpWithoutCapability);
  AddTest(tests, "DebugService/SessionThreadsCachedAndSwitch",
          TestDebugSessionThreadsCachedAndSwitch);
  AddTest(tests, "DebugService/SessionThreadEventRefreshesThreadList",
          TestDebugSessionThreadEventRefreshesThreadList);
  AddTest(tests, "DebugService/SessionExceptionFiltersSentOnLaunchAndToggle",
          TestDebugSessionExceptionFiltersSentOnLaunchAndToggle);
  AddTest(tests, "DebugService/SessionFunctionBreakpointsSentOnLaunch",
          TestDebugSessionFunctionBreakpointsSentOnLaunch);
  AddTest(tests, "DebugService/SessionExceptionFilterConditionsSent",
          TestDebugSessionExceptionFilterConditionsSent);
  AddTest(tests, "DebugService/BreakpointVerificationStaysAtRequestedLine",
          TestDebugServiceBreakpointVerificationStaysAtRequestedLine);
  AddTest(tests, "DebugService/SessionPartialContinueDoesNotResume",
          TestDebugSessionPartialContinueDoesNotResume);
  AddTest(tests, "DebugService/SessionStopWithoutThreadIdResolvesThread",
          TestDebugSessionStopWithoutThreadIdResolvesThread);
  AddTest(tests, "DebugService/FunctionBreakpointStoreBehavior",
          TestFunctionBreakpointStoreBehavior);
  AddTest(tests, "DebugService/FunctionBreakpointStoreReplaceAllDedupes",
          TestFunctionBreakpointStoreReplaceAllDedupes);
  AddTest(tests, "DebugService/FunctionBreakpointStoreApplyVerificationManyNames",
          TestFunctionBreakpointStoreApplyVerificationManyNames);
  AddTest(tests, "DebugService/GdbRealFunctionBreakpointsE2E", TestGdbRealFunctionBreakpointsE2E);
  AddTest(tests, "DebugService/BreakpointsModelPendingIsNotFailed",
          TestDebugBreakpointsModelPendingIsNotFailed);
  AddTest(tests, "DebugService/BreakpointsModelRebuildReusesRows",
          TestDebugBreakpointsModelRebuildReusesRows);
  AddTest(tests, "DebugService/BreakpointsModelBehavior", TestDebugBreakpointsModelBehavior);
  AddTest(tests, "DebugService/SessionVariablesTreeAndSetVariable",
          TestDebugSessionVariablesTreeAndSetVariable);
  AddTest(tests, "DebugService/SessionEvaluateHover", TestDebugSessionEvaluateHover);
  AddTest(tests, "DebugService/SessionWatchEvaluate", TestDebugSessionWatchEvaluate);
  AddTest(tests, "DebugService/SessionReplEvaluate", TestDebugSessionReplEvaluate);
  AddTest(tests, "DebugService/ManagerStopAllSessions", TestDebugManagerStopAllSessions);
  AddTest(tests, "DebugService/StopAllDoesNotWaitOnAWedgedGdb", TestDebugServiceStopAllDoesNotWaitOnAWedgedGdb);
  AddTest(tests, "DebugService/TwoPhaseStopReporting", TestDebugServiceTwoPhaseStopReporting);
  AddTest(tests, "DebugService/BackgroundStopDoesNotBroadcast",
          TestDebugServiceBackgroundStopDoesNotBroadcast);
  AddTest(tests, "DebugService/HoverFallsBackToReplWithoutCapability",
          TestDebugSessionHoverFallsBackToReplWithoutCapability);
  AddTest(tests, "DebugService/HoverModelBehavior", TestDebugHoverModelBehavior);
  AddTest(tests, "DebugService/SessionSetVariableGatedOnCapability",
          TestDebugSessionSetVariableGatedOnCapability);
  AddTest(tests, "DebugService/VariablesModelTreeBehavior", TestDebugVariablesModelTreeBehavior);
  AddTest(tests, "DebugService/VariablesModelExpansionIsPerSibling",
          TestDebugVariablesModelExpansionIsPerSiblingNotByNameOnly);
  AddTest(tests, "DebugService/VariablesShowMoreClearsOnEmptyOverReportedPage",
          TestDebugVariablesShowMoreClearsOnEmptyOverReportedPage);
  AddTest(tests, "DebugService/VariablesLocalsOpenByDefault",
          TestDebugVariablesLocalsOpenByDefault);
  AddTest(tests, "DebugService/VariablesScopeCountDoesNotOverflow",
          TestDebugVariablesScopeCountDoesNotOverflow);
  AddTest(tests, "DebugService/VariablesValueKindAndPlaceholder",
          TestDebugVariablesValueKindAndPlaceholder);
  AddTest(tests, "DebugService/VariablesPagingAndErrors", TestDebugVariablesPagingAndErrors);
  AddTest(tests, "DebugService/GdbAdapterClampsValueFormatting",
          TestGdbAdapterClampsValueFormatting);
  AddTest(tests, "DebugService/WatchModelBehavior", TestDebugWatchModelBehavior);
  AddTest(tests, "DebugService/WatchModelPlaceholderRebuildFreshness",
          TestDebugWatchModelPlaceholderRebuildFreshness);
  AddTest(tests, "DebugService/ValueTreeDeepSubtreeEraseDoesNotOverflow",
          TestDebugValueTreeDeepSubtreeEraseDoesNotOverflow);
  AddTest(tests, "DebugService/WatchModelCapsExpressions", TestDebugWatchModelCapsExpressions);
  AddTest(tests, "DebugService/WatchNestedChildCascadeFetchIsReturned",
          TestDebugWatchNestedChildCascadeFetchIsReturned);
  AddTest(tests, "DebugService/ManagerRejectsUnknownAdapterType",
          TestDebugManagerRejectsUnknownAdapterType);
  AddTest(tests, "DebugService/SummarizeCommandForErrorRedactsArgs",
          TestSummarizeCommandForErrorRedactsArgs);
  AddTest(tests, "DebugService/ManagerRetainAdaptersDropsStaleTypes",
          TestDebugManagerRetainAdaptersDropsStaleTypes);
  AddTest(tests, "DebugService/ManagerAdapterDetailsCarryCommand",
          TestDebugManagerAdapterDetailsCarryCommand);
  AddTest(tests, "DebugService/ExecutionViewPanelRowDispatch",
          TestDebugExecutionViewPanelRowDispatch);
  AddTest(tests, "DebugService/ManagerMultipleConcurrentSessions",
          TestDebugManagerMultipleConcurrentSessions);
  AddTest(tests, "DebugService/ManagerReplaceActiveSession",
          TestDebugManagerReplaceActiveSession);
}

}
