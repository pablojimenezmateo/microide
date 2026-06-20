#include "TestSupport.h"

#include "editor/BreakpointStore.h"
#include "editor/FunctionBreakpointStore.h"
#include "util/JsonValue.h"
#include "workspace/DapProtocol.h"
#include "workspace/DebugBreakpointsModel.h"
#include "workspace/DebugService.h"
#include "workspace/DebugSession.h"
#include "workspace/DebugVariablesModel.h"
#include "workspace/DebugViewModel.h"
#include "workspace/DebugWatchModel.h"
#include "workspace/LaunchConfig.h"
#include "workspace/WorkspaceContext.h"
#include "workspace/WorkspaceDapManager.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
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

// A minimal DAP adapter that walks the Phase 1 launch lifecycle:
//   initialize -> (initialized event) -> launch -> configurationDone ->
//   output event -> terminated event. Capabilities are toggled by argv[1] so a
//   single script covers both the configurationDone-supported and
//   not-supported code paths.
const char* MockAdapterSource() {
  return R"py(import json
import sys

mode = sys.argv[1] if len(sys.argv) > 1 else ""
# config_done / stop / pause / variables / evaluate / restart / threads / exception
# all advertise configurationDone support.
supports_config_done = mode in ("config_done", "stop", "pause", "variables", "evaluate",
                                "restart", "threads", "exception", "die", "thread_event",
                                "function", "reverse")
supports_set_variable = (mode == "variables")
supports_evaluate = (mode == "evaluate")
supports_restart = (mode == "restart")
supports_step_back = (mode == "reverse")  # advertise supportsStepBack (reverse execution)
supports_function = (mode == "function")  # advertise supportsFunctionBreakpoints
multi_thread = (mode == "threads")     # `threads` returns two threads + per-thread frames
exception_mode = (mode == "exception")  # advertise exceptionBreakpointFilters (+ filter options)
# `thread_event`: a worker thread spawns after the first `threads` fetch; the mock
# emits a `thread` started event so the host must refresh the list a second time.
thread_event_mode = (mode == "thread_event")
thread_started = False
# emit `stopped` after configurationDone
stop_on_config = mode in ("stop", "variables", "evaluate", "restart", "threads", "exception",
                          "thread_event", "function", "reverse")
running_no_stop = mode in ("pause", "die")  # stay running (die: then exit silently)
# `die`: reach Running (respond to launch) then exit WITHOUT a terminated event, to
# exercise the host's dead-adapter reconciliation (no zombie session).
die_after_launch = (mode == "die")
seq = 0

def read_message():
    content_length = None
    while True:
        line = sys.stdin.buffer.readline()
        if not line:
            return None
        if line in (b"\r\n", b"\n"):
            break
        if line.lower().startswith(b"content-length:"):
            content_length = int(line.split(b":", 1)[1].strip())
    if content_length is None:
        return None
    body = sys.stdin.buffer.read(content_length)
    if not body:
        return None
    return json.loads(body.decode("utf-8"))

def write_message(message):
    global seq
    seq += 1
    message["seq"] = seq
    data = json.dumps(message).encode("utf-8")
    sys.stdout.buffer.write(f"Content-Length: {len(data)}\r\n\r\n".encode("ascii"))
    sys.stdout.buffer.write(data)
    sys.stdout.buffer.flush()

def respond(req, body=None, success=True, message=None):
    resp = {"type": "response", "request_seq": req["seq"], "success": success,
            "command": req.get("command", "")}
    if body is not None:
        resp["body"] = body
    if message is not None:
        resp["message"] = message
    write_message(resp)

def event(name, body=None):
    msg = {"type": "event", "event": name}
    if body is not None:
        msg["body"] = body
    write_message(msg)

received = []

def finish_launch():
    # Emit the command order so tests can assert setBreakpoints precedes
    # configurationDone on the wire.
    event("output", {"category": "stdout", "output": "commands:" + ",".join(received) + "\n"})
    event("output", {"category": "stdout", "output": "hello from adapter\n"})
    event("terminated")

def emit_stop():
    event("stopped", {"reason": "breakpoint", "threadId": 1, "allThreadsStopped": True})

while True:
    msg = read_message()
    if msg is None:
        break
    if msg.get("type") != "request":
        continue
    command = msg.get("command")
    received.append(command)
    if command == "initialize":
        caps = {"supportsConfigurationDoneRequest": supports_config_done,
                "supportsSetVariable": supports_set_variable,
                "supportsEvaluateForHovers": supports_evaluate,
                "supportsRestartRequest": supports_restart,
                "supportsStepBack": supports_step_back,
                "supportsFunctionBreakpoints": supports_function,
                "supportsConditionalBreakpoints": supports_function,
                "supportsTerminateRequest": True}
        if exception_mode:
            caps["supportsExceptionFilterOptions"] = True
            caps["exceptionBreakpointFilters"] = [
                {"filter": "raised", "label": "Raised Exceptions", "default": False,
                 "supportsCondition": True},
                {"filter": "uncaught", "label": "Uncaught Exceptions", "default": True,
                 "supportsCondition": True},
            ]
        respond(msg, caps)
        event("initialized")
    elif command == "setBreakpoints":
        args = msg.get("arguments", {})
        verified = []
        for i, bp in enumerate(args.get("breakpoints", [])):
            verified.append({"id": i + 1, "verified": True, "line": bp.get("line", 0)})
        respond(msg, {"breakpoints": verified})
    elif command == "launch":
        respond(msg, {})
        if not supports_config_done:
            finish_launch()
        elif die_after_launch:
            # Now Running (launch acknowledged); exit abruptly with NO terminated
            # event, simulating a crash / external kill / RLIMIT_AS cap.
            sys.stdout.flush()
            sys.exit(0)
    elif command == "configurationDone":
        # Spec-compliant adapters (gdb 17.2, lldb-dap, debugpy) defer the run to
        # configurationDone and reject it if no launch/attach is pending. Mirror
        # that so a regression to "launch last" fails loudly here.
        if "launch" not in received and "attach" not in received:
            respond(msg, success=False, message="configurationDone before launch")
            continue
        respond(msg, {})
        if stop_on_config:
            # Echo the received command order (as finish_launch does) so stop-based
            # tests can assert handshake ordering too.
            event("output", {"category": "stdout", "output": "commands:" + ",".join(received) + "\n"})
            emit_stop()
        elif running_no_stop:
            pass  # stay running; the test will issue a pause
        else:
            finish_launch()
    elif command == "stackTrace":
        tid = msg.get("arguments", {}).get("threadId", 1)
        # Encode the threadId into the top frame name so a thread switch is
        # observable (the frames differ per thread).
        top_name = "main" if tid == 1 else "worker"
        respond(msg, {"stackFrames": [
            {"id": tid * 10 + 1, "name": top_name, "line": 10, "column": 1,
             "source": {"name": "main.py", "path": "/proj/main.py"}},
            {"id": tid * 10 + 2, "name": "caller", "line": 3, "column": 1,
             "source": {"name": "main.py", "path": "/proj/main.py"}},
        ], "totalFrames": 2})
    elif command == "restart":
        event("output", {"category": "stdout", "output": "cmd:restart\n"})
        respond(msg, {})
        # Re-run the configuration handshake (some adapters re-emit initialized).
        event("initialized")
    elif command in ("continue", "next", "stepIn", "stepOut", "reverseContinue", "stepBack"):
        event("output", {"category": "stdout", "output": "cmd:" + command + "\n"})
        respond(msg, {"allThreadsContinued": True} if command == "continue" else {})
        emit_stop()  # re-stop so the test can drive the next step
    elif command == "scopes":
        respond(msg, {"scopes": [
            {"name": "Locals", "variablesReference": 1000, "expensive": False},
            {"name": "Globals", "variablesReference": 2000, "expensive": False},
        ]})
    elif command == "variables":
        ref = msg.get("arguments", {}).get("variablesReference", 0)
        if ref == 1000:
            respond(msg, {"variables": [
                {"name": "x", "value": "1", "type": "int", "variablesReference": 0},
                {"name": "obj", "value": "{...}", "type": "Obj", "variablesReference": 1001},
            ]})
        elif ref == 1001:
            respond(msg, {"variables": [
                {"name": "field", "value": "7", "type": "int", "variablesReference": 0},
            ]})
        else:
            respond(msg, {"variables": []})
    elif command == "setVariable":
        args = msg.get("arguments", {})
        respond(msg, {"value": args.get("value", ""), "type": "int", "variablesReference": 0})
    elif command == "evaluate":
        args = msg.get("arguments", {})
        # Echo the expression + frame so the test can assert both reached the wire.
        expr = args.get("expression", "")
        # Surface gdb value-formatting limit commands (and whether a frameId rode
        # along) so a test can assert they were sent with the safe values and with
        # no frameId (a present frameId:0 pre-launch makes gdb error).
        if expr.startswith("set "):
            event("output", {"category": "stdout",
                             "output": "limit:" + expr + "|frameId=" +
                                       str(args.get("frameId", "none")) + "\n"})
        value = expr + "@" + str(args.get("frameId", 0))
        # "obj" yields a structured result (variablesReference 1001) so the REPL can
        # expand one level via a follow-up variables request; everything else is a leaf.
        ref = 1001 if expr == "obj" else 0
        respond(msg, {"result": value, "type": "Obj" if ref else "int",
                      "variablesReference": ref})
    elif command == "threads":
        if multi_thread:
            respond(msg, {"threads": [{"id": 1, "name": "main"}, {"id": 2, "name": "worker"}]})
        elif thread_event_mode and not thread_started:
            # First fetch (from the stop): only main exists. A worker then spawns,
            # announced via a `thread` event so the host refreshes the list again.
            respond(msg, {"threads": [{"id": 1, "name": "main"}]})
            thread_started = True
            event("thread", {"reason": "started", "threadId": 2})
        elif thread_event_mode:
            respond(msg, {"threads": [{"id": 1, "name": "main"}, {"id": 2, "name": "worker"}]})
        else:
            respond(msg, {"threads": [{"id": 1, "name": "main"}]})
    elif command == "setFunctionBreakpoints":
        args = msg.get("arguments", {})
        names = [bp.get("name", "") for bp in args.get("breakpoints", [])]
        conds = [bp.get("condition", "") for bp in args.get("breakpoints", [])]
        event("output", {"category": "stdout", "output": "fnbp:" + ",".join(names) + "\n"})
        event("output", {"category": "stdout", "output": "fncond:" + ",".join(conds) + "\n"})
        verified = [{"id": i + 1, "verified": True} for i, _ in enumerate(names)]
        respond(msg, {"breakpoints": verified})
    elif command == "setExceptionBreakpoints":
        args = msg.get("arguments", {})
        filters = args.get("filters", [])
        event("output", {"category": "stdout", "output": "exc:" + ",".join(filters) + "\n"})
        opts = args.get("filterOptions", [])
        if opts:
            rendered = ",".join(o.get("filterId", "") + "=" + o.get("condition", "") for o in opts)
            event("output", {"category": "stdout", "output": "excopt:" + rendered + "\n"})
        respond(msg, {})
    elif command == "pause":
        event("output", {"category": "stdout", "output": "cmd:pause\n"})
        respond(msg, {})
        emit_stop()
    elif command == "terminate":
        respond(msg, {})
        event("terminated")
    elif command == "disconnect":
        respond(msg, {})
        break
    else:
        respond(msg, success=False, message="unknown command")
)py";
}

std::vector<std::string> MockAdapterCommand(const std::filesystem::path& server_path,
                                            const std::string& mode) {
  return {"python3", server_path.string(), mode};
}

// Same mock, but with a trailing "gdb" token (ignored by the script, which only
// reads argv[1]) so DebugSession flags it as a gdb adapter and emits the
// value-formatting limit commands on `initialized`.
std::vector<std::string> GdbFlavoredAdapterCommand(const std::filesystem::path& server_path,
                                                   const std::string& mode) {
  return {"python3", server_path.string(), mode, "gdb"};
}

bool PollUntil(DapManager& manager, const std::function<bool()>& predicate, int timeout_ms = 4000) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (std::chrono::steady_clock::now() < deadline) {
    manager.DrainCallbacks();
    if (predicate()) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  manager.DrainCallbacks();
  return predicate();
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
  return callbacks;
}

bool SawState(const CapturedSession& captured, DebugSession::State state) {
  return std::find(captured.states.begin(), captured.states.end(), state) != captured.states.end();
}

void TestDebugSessionDrivesLaunchLifecycleWithConfigurationDone() {
#if !defined(__unix__) && !defined(__APPLE__)
  return;
#endif
  TemporaryDirectory temp_dir;
  const auto server_path = temp_dir.path() / "adapter.py";
  WriteFile(server_path, std::string(MockAdapterSource()));

  DapManager manager;
  manager.RegisterAdapter("mock", MockAdapterCommand(server_path, "config_done"));
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
  // A clean DAP terminated also fires on_terminated, but reports failed=false with no
  // reason — so the control channel emits a plain `terminated` and the console is
  // dropped rather than retained.
  Expect(captured.got_terminated_callback, "on_terminated fires on a clean terminated event");
  Expect(!captured.terminated_failed, "a clean exit reports failed=false");
  Expect(captured.terminated_reason.empty(), "a clean exit carries no reason");
  manager.ShutdownAll();
}

void TestDebugSessionRunsWithoutConfigurationDoneSupport() {
#if !defined(__unix__) && !defined(__APPLE__)
  return;
#endif
  TemporaryDirectory temp_dir;
  const auto server_path = temp_dir.path() / "adapter.py";
  WriteFile(server_path, std::string(MockAdapterSource()));

  DapManager manager;
  manager.RegisterAdapter("mock", MockAdapterCommand(server_path, "no_config_done"));

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
#if !defined(__unix__) && !defined(__APPLE__)
  return;
#endif
  TemporaryDirectory temp_dir;
  const auto server_path = temp_dir.path() / "adapter.py";
  WriteFile(server_path, std::string(MockAdapterSource()));

  DapManager manager;
  manager.RegisterAdapter("mock", MockAdapterCommand(server_path, "config_done"));

  CapturedSession captured;
  DebugSession::Callbacks callbacks = MakeCallbacks(captured);

  // Two breakpoints on one file at 0-based lines 4 and 9 (DAP lines 5 and 10).
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

  // The strict mock rejects configurationDone that arrives before launch (as gdb
  // 17.2 does), so reaching Terminated cleanly already proves launch preceded it.
  Expect(!captured.terminated_failed, "spec-compliant handshake must not fail the session");

  // Pin the DAP handshake order: launch  ->  setBreakpoints  ->  configurationDone.
  // gdb's DAP defers running the debuggee until configurationDone and rejects a
  // configurationDone with no launch pending, so launch must be in flight first;
  // breakpoints still precede configurationDone, so they are armed before the run.
  // finish_launch() emits this command list while handling configurationDone, so
  // by then launch, setBreakpoints, and configurationDone have all been recorded.
  const std::size_t order_pos = captured.output.find("commands:");
  Expect(order_pos != std::string::npos, "adapter should emit its command order");
  const std::size_t launch_pos = captured.output.find("launch", order_pos);
  const std::size_t set_pos = captured.output.find("setBreakpoints", order_pos);
  const std::size_t cfg_pos = captured.output.find("configurationDone", order_pos);
  Expect(launch_pos != std::string::npos && set_pos != std::string::npos &&
             cfg_pos != std::string::npos,
         "adapter should record launch, setBreakpoints, and configurationDone");
  Expect(launch_pos < set_pos, "launch must be sent before setBreakpoints");
  Expect(launch_pos < cfg_pos, "launch must be sent before configurationDone");
  Expect(set_pos < cfg_pos, "setBreakpoints must be sent before configurationDone");
  manager.ShutdownAll();
}

void TestDebugSessionResolvesStackOnStopAndStepsResume() {
#if !defined(__unix__) && !defined(__APPLE__)
  return;
#endif
  TemporaryDirectory temp_dir;
  const auto server_path = temp_dir.path() / "adapter.py";
  WriteFile(server_path, std::string(MockAdapterSource()));

  DapManager manager;
  manager.RegisterAdapter("mock", MockAdapterCommand(server_path, "stop"));

  CapturedSession captured;
  LaunchConfig config;
  config.type = "mock";
  config.request = "launch";
  Expect(manager.StartSession(config, MakeCallbacks(captured)), "session should start");

  // The adapter stops after configurationDone; the session resolves stackTrace
  // and hands the frames to on_stopped (top frame first).
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

  // Step over -> the adapter records `next` and re-stops.
  DebugSession* session = manager.ActiveSession();
  Expect(session != nullptr, "session should be active while stopped");
  const int stops_before = captured.stop_count;
  session->StepOver();
  Expect(PollUntil(manager, [&]() { return captured.stop_count > stops_before; }),
         "step over should drive another stop");
  Expect(captured.output.find("cmd:next") != std::string::npos,
         "StepOver should send the DAP `next` command");
  Expect(captured.resume_count >= 1, "stepping should fire on_resumed optimistically");

  // Step in / out map to their DAP commands.
  session->StepIn();
  Expect(PollUntil(manager, [&]() { return captured.output.find("cmd:stepIn") != std::string::npos; }),
         "StepIn should send the DAP `stepIn` command");
  session->StepOut();
  Expect(PollUntil(manager,
                   [&]() { return captured.output.find("cmd:stepOut") != std::string::npos; }),
         "StepOut should send the DAP `stepOut` command");
  session->Continue();
  Expect(PollUntil(manager,
                   [&]() { return captured.output.find("cmd:continue") != std::string::npos; }),
         "Continue should send the DAP `continue` command");
  manager.ShutdownAll();
}

void TestDebugSessionPauseFromRunning() {
#if !defined(__unix__) && !defined(__APPLE__)
  return;
#endif
  TemporaryDirectory temp_dir;
  const auto server_path = temp_dir.path() / "adapter.py";
  WriteFile(server_path, std::string(MockAdapterSource()));

  DapManager manager;
  manager.RegisterAdapter("mock", MockAdapterCommand(server_path, "pause"));

  CapturedSession captured;
  LaunchConfig config;
  config.type = "mock";
  config.request = "launch";
  Expect(manager.StartSession(config, MakeCallbacks(captured)), "session should start");

  // Adapter stays running after configurationDone.
  Expect(PollUntil(manager,
                   [&]() {
                     const auto* session = manager.ActiveSession();
                     return session != nullptr &&
                            session->CurrentState() == DebugSession::State::Running;
                   }),
         "session should reach Running with no initial stop");

  // Pause resolves a thread via `threads` then sends `pause`; the adapter stops.
  manager.ActiveSession()->Pause();
  Expect(PollUntil(manager, [&]() { return captured.stop_count >= 1; }),
         "pause should drive the adapter to a stop");
  Expect(captured.output.find("cmd:pause") != std::string::npos,
         "Pause should send the DAP `pause` command after resolving a thread");
  manager.ShutdownAll();
}

// Reverse execution: an adapter advertising supportsStepBack accepts `stepBack`
// and `reverseContinue`, each optimistically resuming like a forward step.
void TestDebugSessionReverseStepAndContinue() {
#if !defined(__unix__) && !defined(__APPLE__)
  return;
#endif
  TemporaryDirectory temp_dir;
  const auto server_path = temp_dir.path() / "adapter.py";
  WriteFile(server_path, std::string(MockAdapterSource()));

  DapManager manager;
  manager.RegisterAdapter("mock", MockAdapterCommand(server_path, "reverse"));

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

// Without supportsStepBack the reverse commands are dropped at the session layer:
// nothing reaches the wire and no optimistic resume fires.
void TestDebugSessionReverseGatedOnCapability() {
#if !defined(__unix__) && !defined(__APPLE__)
  return;
#endif
  TemporaryDirectory temp_dir;
  const auto server_path = temp_dir.path() / "adapter.py";
  WriteFile(server_path, std::string(MockAdapterSource()));

  DapManager manager;
  manager.RegisterAdapter("mock", MockAdapterCommand(server_path, "stop"));  // no supportsStepBack

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
  // Give the (suppressed) requests a chance to round-trip, then assert nothing did.
  Expect(!PollUntil(manager,
                    [&]() {
                      return captured.output.find("cmd:stepBack") != std::string::npos ||
                             captured.output.find("cmd:reverseContinue") != std::string::npos ||
                             captured.resume_count > resumes_before;
                    }),
         "reverse commands should be dropped when the adapter lacks supportsStepBack");
  manager.ShutdownAll();
}

// Drives the real scopes/variables/setVariable round-trips against the mock
// adapter, feeding responses into a DebugVariablesModel exactly as DebugService
// does — exercising protocol + session + model end to end.
void TestDebugSessionVariablesTreeAndSetVariable() {
#if !defined(__unix__) && !defined(__APPLE__)
  return;
#endif
  TemporaryDirectory temp_dir;
  const auto server_path = temp_dir.path() / "adapter.py";
  WriteFile(server_path, std::string(MockAdapterSource()));

  DapManager manager;
  manager.RegisterAdapter("mock", MockAdapterCommand(server_path, "variables"));

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

  // scopes → Locals is open by default (auto-expanded); a sibling scope stays
  // collapsed. ApplyScopes returns the bounded fetch the default expansion needs.
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

  // The default Locals expansion fetches its variables (x scalar, obj structured).
  Expect(scope_fetches.size() == 1 && scope_fetches[0].reference == 1000,
         "auto-expanding Locals requests its variablesReference exactly once");
  const int locals_ref = scope_fetches[0].reference;
  bool locals_done = false;
  session->RequestVariables(locals_ref, 0, DebugValueTree::kChildPageSize,
                            [&](bool ok, std::vector<codec::DapVariable> vars) {
                              (void)ok;
                              model.ApplyVariables(locals_ref, vars, 0);
                              locals_done = true;
                            });
  Expect(PollUntil(manager, [&]() { return locals_done; }), "Locals variables should resolve");
  Expect(model.Rows().size() == 4, "Locals + x + obj + Globals should flatten to four rows");
  Expect(model.Rows()[1].display_name == "x" && model.Rows()[1].depth == 1 &&
             !model.Rows()[1].has_children && model.Rows()[1].editable,
         "x should be an editable depth-1 leaf");
  Expect(model.Rows()[2].display_name == "obj" && model.Rows()[2].has_children,
         "obj should be an expandable child");

  // Expand obj → nested child at depth 2; collapsing removes it.
  const int obj_ref = model.ToggleRow(2).reference;
  Expect(obj_ref == 1001, "expanding obj should request its variablesReference");
  bool obj_done = false;
  session->RequestVariables(obj_ref, 0, DebugValueTree::kChildPageSize,
                            [&](bool ok, std::vector<codec::DapVariable> vars) {
                              (void)ok;
                              model.ApplyVariables(obj_ref, vars, 0);
                              obj_done = true;
                            });
  Expect(PollUntil(manager, [&]() { return obj_done; }), "obj variables should resolve");
  Expect(model.Rows().size() == 5 && model.Rows()[3].display_name == "field" &&
             model.Rows()[3].depth == 2,
         "obj's field should appear nested at depth 2");
  Expect(model.ToggleRow(2).reference == 0 && model.Rows().size() == 4,
         "collapsing obj should drop its children without a refetch");

  // setVariable on x → the adapter echoes the new value, applied authoritatively.
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

// The session-level capability gate: an adapter without supportsSetVariable
// rejects setVariable without sending anything on the wire.
void TestDebugSessionSetVariableGatedOnCapability() {
#if !defined(__unix__) && !defined(__APPLE__)
  return;
#endif
  TemporaryDirectory temp_dir;
  const auto server_path = temp_dir.path() / "adapter.py";
  WriteFile(server_path, std::string(MockAdapterSource()));

  DapManager manager;
  manager.RegisterAdapter("mock", MockAdapterCommand(server_path, "stop"));

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

// Drives the real `evaluate(context:"hover")` round-trip against the mock adapter
// and feeds the result into a DebugHoverModel exactly as DebugService::EvaluateHover
// does — protocol + session + model end to end, including dedup + generation guard.
void TestDebugSessionEvaluateHover() {
#if !defined(__unix__) && !defined(__APPLE__)
  return;
#endif
  TemporaryDirectory temp_dir;
  const auto server_path = temp_dir.path() / "adapter.py";
  WriteFile(server_path, std::string(MockAdapterSource()));

  DapManager manager;
  manager.RegisterAdapter("mock", MockAdapterCommand(server_path, "evaluate"));

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

  // Mirror DebugService::EvaluateHover's model interaction: Begin → request →
  // Resolve under the in-flight generation.
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

// Watch expressions evaluate against the real mock adapter via the same
// RequestEvaluate path as hover, but with context "watch" (which is NOT
// capability-gated) and folding the result onto a DebugWatchModel root —
// mirroring DebugService::EvaluateWatches.
void TestDebugSessionWatchEvaluate() {
#if !defined(__unix__) && !defined(__APPLE__)
  return;
#endif
  TemporaryDirectory temp_dir;
  const auto server_path = temp_dir.path() / "adapter.py";
  WriteFile(server_path, std::string(MockAdapterSource()));

  DapManager manager;
  manager.RegisterAdapter("mock", MockAdapterCommand(server_path, "evaluate"));

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

// The debug-console REPL evaluates against the real mock adapter via the same
// RequestEvaluate path as watch/hover, but with context "repl" (ungated, like
// watch) — mirroring DebugService::EvaluateRepl's wire interaction. Frame 0 is the
// fallback when running; here we evaluate against the stopped top frame.
void TestDebugSessionReplEvaluate() {
#if !defined(__unix__) && !defined(__APPLE__)
  return;
#endif
  TemporaryDirectory temp_dir;
  const auto server_path = temp_dir.path() / "adapter.py";
  WriteFile(server_path, std::string(MockAdapterSource()));

  DapManager manager;
  manager.RegisterAdapter("mock", MockAdapterCommand(server_path, "evaluate"));

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

  // Structured result: a "repl" evaluate of "obj" carries a variablesReference, which
  // EvaluateRepl expands one level via a follow-up variables request (the two-step
  // path exercised here: evaluate -> variables).
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

// Hover-to-inspect without supportsEvaluateForHovers must NOT be dropped: the
// session falls back to the universally-supported "repl" context so hovering a
// symbol still resolves a value. (GDB-style adapters omit the capability but
// evaluate fine; silently gating left users with no hover value at all.)
void TestDebugSessionHoverFallsBackToReplWithoutCapability() {
#if !defined(__unix__) && !defined(__APPLE__)
  return;
#endif
  TemporaryDirectory temp_dir;
  const auto server_path = temp_dir.path() / "adapter.py";
  WriteFile(server_path, std::string(MockAdapterSource()));

  DapManager manager;
  manager.RegisterAdapter("mock", MockAdapterCommand(server_path, "stop"));  // no evaluate cap

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

// Pure DebugHoverModel behavior (no adapter): classify keying, generation guard
// dropping stale completions, and Clear.
void TestDebugHoverModelBehavior() {
  DebugHoverModel hover;
  Expect(hover.status == DebugHoverModel::Status::Empty, "model starts Empty");
  Expect(hover.Classify(1, "x") == DebugHoverModel::Lookup::Miss, "empty model is a miss");

  // A different frame or expression is always a miss (key = frame_id + expression).
  const std::uint64_t gen = hover.Begin(1, "x");
  Expect(hover.Classify(2, "x") == DebugHoverModel::Lookup::Miss, "different frame is a miss");
  Expect(hover.Classify(1, "y") == DebugHoverModel::Lookup::Miss, "different expr is a miss");
  Expect(hover.Classify(1, "x") == DebugHoverModel::Lookup::Pending, "same key while pending");

  hover.Resolve(gen, "42", "int");
  Expect(hover.Classify(1, "x") == DebugHoverModel::Lookup::Hit && hover.value == "42",
         "resolve under the live generation lands a hit");

  // A stale completion (older generation, e.g. after a frame switch) is dropped.
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

// Pure DebugVariablesModel behavior (no adapter): flatten ordering, lazy
// expand/collapse, setVariable application, inline-edit targeting, selection.
void TestDebugVariablesModelTreeBehavior() {
  DebugVariablesModel model;
  model.BeginFrame(7);
  Expect(model.FrameId() == 7, "BeginFrame should record the focused frame id");

  // Use non-default scope names so this generic mechanics test is unaffected by the
  // Locals-open-by-default policy (covered separately).
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
  model.ApplyVariables(1000,
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
  model.ApplyVariables(
      1001, {codec::DapVariable{.name = "field", .value = "7", .variables_reference = 0}}, 0);
  Expect(model.Rows().size() == 5 && model.Rows()[3].depth == 2, "nested field flattens at depth 2");
  Expect(model.ToggleRow(2).reference == 0 && model.Rows().size() == 4,
         "collapse removes the subtree");
  Expect(model.ToggleRow(2).reference == 0 && model.Rows().size() == 5,
         "re-expanding a loaded node does not refetch");

  // Inline edit targeting + setVariable application.
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

  // Selection cursor clamps to row bounds.
  model.SetSelectedRow(99);
  Expect(model.SelectedRow() == model.Rows().size() - 1, "selection clamps to the last row");
  model.MoveSelection(-100);
  Expect(model.SelectedRow() == 0, "selection clamps to the first row");

  model.Clear();
  Expect(model.Empty() && model.Rows().empty(), "Clear empties the tree");
}

// Locals is auto-expanded once per session (open by default) via the same bounded
// re-expansion path a manual expand uses; a manual collapse is respected for the
// rest of the session, and a new session (Clear) reopens it.
void TestDebugVariablesLocalsOpenByDefault() {
  DebugVariablesModel model;
  model.BeginFrame(1);
  const std::vector<codec::DapScope> scopes = {
      codec::DapScope{.name = "Locals", .variables_reference = 1000},
      codec::DapScope{.name = "Registers", .variables_reference = 2000},
  };

  // First stop: Locals comes up expanded with a fetch issued; a sibling scope stays
  // collapsed. ApplyScopes returns the bounded fetch the service would issue. While
  // the fetch is in flight Locals shows a loading placeholder, so the sibling scope
  // is the last row.
  std::vector<DebugValueTree::ChildFetch> fetches = model.ApplyScopes(scopes);
  Expect(fetches.size() == 1 && fetches[0].reference == 1000,
         "the default Locals scope auto-issues exactly one bounded child fetch");
  Expect(model.Rows()[0].display_name == "Locals" && model.Rows()[0].expanded,
         "Locals is expanded by default on the first stop");
  Expect(model.Rows().back().display_name == "Registers" && !model.Rows().back().expanded,
         "a non-default scope stays collapsed");

  // Populate Locals, then the user collapses it.
  model.ApplyVariables(
      1000, {codec::DapVariable{.name = "x", .value = "1", .type = "int"}}, 0);
  Expect(model.Rows()[0].expanded && model.Rows()[1].display_name == "x",
         "Locals shows its child after the page arrives");
  model.ToggleRow(0);
  Expect(!model.Rows()[0].expanded, "the user collapses Locals");

  // Next stop in the SAME session (BeginFrame clears the tree but not the remembered
  // collapse): Locals stays closed, no auto-refetch.
  model.BeginFrame(1);
  fetches = model.ApplyScopes(scopes);
  Expect(fetches.empty() && !model.Rows()[0].expanded,
         "a collapsed Locals stays collapsed on the next stop (no auto-refetch)");

  // A new session re-arms the one-shot (DebugService calls BeginSession on launch):
  // Locals opens again.
  model.BeginSession();
  model.BeginFrame(1);
  fetches = model.ApplyScopes(scopes);
  Expect(fetches.size() == 1 && model.Rows()[0].expanded,
         "a new session reopens Locals by default");
}

// Value-kind classification (drives render coloring) and the synthetic
// "loading…" placeholder shown while a node's children are in flight.
void TestDebugVariablesValueKindAndPlaceholder() {
  DebugVariablesModel model;
  model.BeginFrame(1);
  // Non-default scope name keeps this placeholder test independent of the
  // Locals-open-by-default policy.
  model.ApplyScopes({codec::DapScope{.name = "Registers", .variables_reference = 1000}});
  Expect(model.Rows()[0].kind == DebugValueKind::Scope, "a scope row classifies as Scope");

  // Expanding before the children arrive shows exactly one placeholder child.
  Expect(model.ToggleRow(0).reference == 1000,
         "expanding a scope returns its reference to fetch");
  Expect(model.Rows().size() == 2, "an unloaded expanded scope shows one placeholder child");
  Expect(model.Rows()[1].is_placeholder && model.Rows()[1].kind == DebugValueKind::Pending &&
             model.Rows()[1].depth == 1 && !model.Rows()[1].has_children,
         "the placeholder is a dim, non-expandable depth-1 row");

  // The response replaces the placeholder with the real children, each classified.
  model.ApplyVariables(
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

  // A loaded, expanded node recurses to children — no placeholder reappears.
  Expect(model.ToggleRow(6).reference == 1001, "expanding pt returns its reference");
  Expect(model.Rows().back().is_placeholder, "pt's children are still pending → placeholder");
  model.ApplyVariables(1001, {codec::DapVariable{.name = "x", .value = "1", .type = "int"}}, 0);
  Expect(!model.Rows().back().is_placeholder && model.Rows().back().display_name == "x",
         "pt's fetched child replaces its placeholder");
}

// Bounded paging (the freeze fix), the in-flight guard, and the failure path. A
// container is fetched in bounded pages — never all at once — so an adapter can
// never be asked to enumerate a (possibly garbage / billions-long) container in
// one shot; a failed fetch ends in an error row, never a permanent spinner.
void TestDebugVariablesPagingAndErrors() {
  constexpr int kPage = DebugValueTree::kChildPageSize;
  DebugVariablesModel model;
  model.BeginFrame(1);
  // A non-default scope name keeps these container-paging mechanics independent of
  // the Locals-open-by-default policy (covered by VariablesLocalsOpenByDefault).
  model.ApplyScopes({codec::DapScope{.name = "Registers", .variables_reference = 1000}});
  Expect(model.ToggleRow(0).reference == 1000, "expanding the scope requests its children");
  // The scope holds one big array that reports 300 indexed children (gdb reports
  // indexedVariables; a garbage container would report billions here).
  model.ApplyVariables(1000,
                       {codec::DapVariable{.name = "arr",
                                           .value = "{...}",
                                           .type = "int[300]",
                                           .variables_reference = 1001,
                                           .indexed_variables = 300,
                                           .count_reported = true}},
                       0);
  Expect(model.Rows().size() == 2 && model.Rows()[1].display_name == "arr",
         "the array child is present under the scope");

  // Expanding the array must fetch only a bounded first page, NOT all 300.
  const DebugValueTree::ChildFetch first = model.ToggleRow(1);
  Expect(first.reference == 1001 && first.start == 0 && first.count == kPage,
         "expanding a 300-element array fetches a bounded first page, not the whole container");

  std::vector<codec::DapVariable> page0;
  for (int i = 0; i < kPage; ++i) {
    page0.push_back(codec::DapVariable{.name = "[" + std::to_string(i) + "]", .value = "0"});
  }
  model.ApplyVariables(1001, page0, 0);
  // Locals + arr + kPage children + one trailing "show more…" row.
  Expect(model.Rows().size() == static_cast<std::size_t>(kPage) + 3,
         "the first page is followed by a clickable 'show more' row (300 > kPage)");
  Expect(model.Rows().back().is_show_more && !model.Rows().back().is_placeholder,
         "the trailing row is the 'show more' affordance");

  // Clicking 'show more' fetches the next page at the loaded offset, clamped to the
  // children that remain (300 - kPage) so we never ask gdb for more than exist (its
  // DAP throws "list index out of range" on count > available).
  const DebugValueTree::ChildFetch more = model.ToggleRow(model.Rows().size() - 1);
  Expect(more.reference == 1001 && more.start == kPage && more.count == 300 - kPage,
         "'show more' fetches the remaining children at the loaded offset, clamped to the total");
  Expect(model.Rows().back().is_placeholder && !model.Rows().back().is_show_more,
         "the 'show more' row becomes a loading row while the next page is in flight");
  // The in-flight loading row is inert — no duplicate fetch on a second click.
  Expect(model.ToggleRow(model.Rows().size() - 1).reference == 0,
         "an in-flight loading row issues no duplicate fetch (request-storm guard)");

  // The final 100-element page reaches the reported total → paging stops.
  std::vector<codec::DapVariable> page1;
  for (int i = kPage; i < 300; ++i) {
    page1.push_back(codec::DapVariable{.name = "[" + std::to_string(i) + "]", .value = "0"});
  }
  model.ApplyVariables(1001, page1, kPage);
  Expect(model.Rows().size() == static_cast<std::size_t>(300) + 2,
         "the final page appends to 300 children with no further 'show more'");
  Expect(model.Rows().back().display_name == "[299]" && !model.Rows().back().is_show_more,
         "paging stops once the adapter-reported total is reached");

  // Failure path: a first-page fetch that errors ends in a finite error row.
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

  // Regression: a scope that reports only 9 children must not be expanded with
  // count = kPage. gdb's DAP throws "list index out of range" when the request asks
  // for more children than exist, which is what left the scope showing
  // "<unavailable>". The fetch count is clamped to the scope's reported total.
  DebugVariablesModel small_scope;
  small_scope.BeginFrame(1);
  small_scope.ApplyScopes({codec::DapScope{.name = "Registers",
                                           .variables_reference = 3000,
                                           .named_variables = 9,
                                           .count_reported = true}});
  const DebugValueTree::ChildFetch scoped = small_scope.ToggleRow(0);
  Expect(scoped.reference == 3000 && scoped.start == 0 && scoped.count == 9,
         "expanding a 9-variable scope requests exactly 9, not kPage (gdb count clamp)");

  // Regression: an empty container (adapter reports zero children, e.g. an empty
  // std::vector) is known-empty — expanding it must NOT fetch (a count request
  // would make gdb throw "list index out of range"); it reads as expanded-empty.
  DebugVariablesModel empty_child;
  empty_child.BeginFrame(1);
  empty_child.ApplyScopes(
      {codec::DapScope{.name = "Registers", .variables_reference = 4000, .named_variables = 1}});
  empty_child.ToggleRow(0);
  empty_child.ApplyVariables(4000,
                             {codec::DapVariable{.name = "v",
                                                 .value = "std::vector of length 0",
                                                 .type = "std::vector<int>",
                                                 .variables_reference = 4001,
                                                 .indexed_variables = 0,
                                                 .count_reported = true}},
                             0);
  const DebugValueTree::ChildFetch empty_fetch = empty_child.ToggleRow(1);  // expand "v"
  Expect(empty_fetch.reference == 0,
         "expanding a known-empty container issues no fetch (gdb would reject a count request)");

  // Regression: expansion survives a stop. Variables references are NOT stable
  // across stops, so expansion is tracked by path. Expand the scope → a struct
  // child, then re-apply scopes with DIFFERENT refs (a new stop) and confirm the
  // model asks to repopulate exactly the paths that were open, cascading into the
  // child. Uses a non-default scope so the restore (not the default-open) is tested.
  DebugVariablesModel keep;
  keep.BeginFrame(1);
  Expect(keep.ApplyScopes({codec::DapScope{
                              .name = "Registers", .variables_reference = 1, .named_variables = 2}})
                 .empty(),
         "first stop: nothing was expanded yet, so nothing to restore");
  keep.ToggleRow(0);  // expand the scope (ref 1)
  keep.ApplyVariables(1,
                      {codec::DapVariable{.name = "n", .value = "5", .type = "int"},
                       codec::DapVariable{.name = "pt",
                                          .value = "",
                                          .type = "Point",
                                          .variables_reference = 9,
                                          .named_variables = 2}},
                      0);
  keep.ToggleRow(2);  // expand "pt" (ref 9)
  keep.ApplyVariables(9,
                      {codec::DapVariable{.name = "x", .value = "1", .type = "int"},
                       codec::DapVariable{.name = "y", .value = "2", .type = "int"}},
                      0);
  Expect(keep.Rows().size() == 5, "Locals > {n, pt > {x, y}} is fully expanded");

  // Next stop: same names, brand-new references (gdb reassigns them).
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
  keep.ApplyVariables(109,
                      {codec::DapVariable{.name = "x", .value = "1", .type = "int"},
                       codec::DapVariable{.name = "y", .value = "2", .type = "int"}},
                      0);
  Expect(keep.Rows().size() == 5,
         "after the stop the tree is restored to Locals > {n, pt > {x, y}}");
}

// A gdb adapter must clamp value formatting on `initialized` (before the program
// runs) so the first expand of an uninitialized/garbage STL container cannot drive
// gdb into unbounded formatting (the host-freeze signature). The limits ride DAP
// `evaluate` "repl" commands with the safe values and, critically, NO frameId — a
// present frameId:0 pre-launch makes gdb's DAP error "list index out of range" and
// the limits would silently never apply. A non-gdb adapter must send none.
void TestGdbAdapterClampsValueFormatting() {
#if !defined(__unix__) && !defined(__APPLE__)
  return;
#endif
  TemporaryDirectory temp_dir;
  const auto server_path = temp_dir.path() / "adapter.py";
  WriteFile(server_path, std::string(MockAdapterSource()));

  // gdb-flavored adapter: the limit commands must be on the wire with safe values.
  {
    DapManager manager;
    manager.RegisterAdapter("gdb-mock", GdbFlavoredAdapterCommand(server_path, "no_config_done"));
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

  // Non-gdb adapter: no value-limit commands at all.
  {
    DapManager manager;
    manager.RegisterAdapter("mock", MockAdapterCommand(server_path, "no_config_done"));
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

// Pure DebugWatchModel behavior (no adapter): persistent expression list +
// transient evaluated tree, placeholder roots, lazy expand, edit/remove,
// ExpressionIndexForRow, and ClearResults keeping the expressions.
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

  // Fold async evaluate results onto the roots by index.
  model.ApplyEvaluate(
      0, codec::DapEvaluateResult{.result = "42", .type = "int", .variables_reference = 0});
  model.ApplyEvaluate(
      1, codec::DapEvaluateResult{.result = "{...}", .type = "Obj", .variables_reference = 900});
  Expect(model.Rows()[0].display_value == "42", "a scalar result folds onto its root");
  Expect(model.Rows()[1].has_children, "a structured result makes the root expandable");

  Expect(model.ExpressionIndexForRow(0) == std::optional<std::size_t>(0),
         "row 0 maps back to expression 0");

  // Expand the structured watch root via the shared tree machinery.
  Expect(model.ToggleRow(1).reference == 900,
         "expanding a structured root returns its reference to fetch");
  model.ApplyVariables(
      900, {codec::DapVariable{.name = "field", .value = "7", .variables_reference = 0}}, 0);
  Expect(model.Rows().size() == 3 && model.Rows()[2].depth == 1, "the child folds at depth 1");
  Expect(!model.ExpressionIndexForRow(2).has_value(), "a child row is not an expression root");

  // Edit rebuilds placeholder roots (dropping stale results); remove drops by index.
  model.EditExpression(0, "x + 1");
  Expect(model.Expressions()[0] == "x + 1", "EditExpression updates the string");
  Expect(model.Rows().size() == 2 && model.Rows()[0].display_value.empty(),
         "editing rebuilds blank placeholder roots");
  model.RemoveExpression(0);
  Expect(model.Expressions().size() == 1 && model.Expressions()[0] == "obj",
         "RemoveExpression drops the expression at the index");
  model.EditExpression(0, "");
  Expect(model.Expressions().empty(), "editing an expression to empty removes it");

  // SetExpressions (persistence restore) + ClearResults keeps the expressions.
  model.SetExpressions({"a", "b"});
  Expect(model.Expressions().size() == 2 && model.Rows().size() == 2,
         "SetExpressions rebuilds one root per restored expression");
  model.ApplyEvaluate(0, codec::DapEvaluateResult{.result = "1"});
  model.ClearResults();
  Expect(model.Expressions().size() == 2, "ClearResults keeps the persistent expression list");
  Expect(model.Rows().size() == 2 && model.Rows()[0].display_value.empty(),
         "ClearResults blanks evaluated values but keeps the placeholder rows");
}

// Restart via the DAP `restart` request: the adapter re-runs its handshake and
// re-stops; the session re-sends configurationDone and reports a second stop.
void TestDebugSessionRestartViaRestartRequest() {
#if !defined(__unix__) && !defined(__APPLE__)
  return;
#endif
  TemporaryDirectory temp_dir;
  const auto server_path = temp_dir.path() / "adapter.py";
  WriteFile(server_path, std::string(MockAdapterSource()));

  DapManager manager;
  manager.RegisterAdapter("mock", MockAdapterCommand(server_path, "restart"));

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

// A session whose adapter lacks supportsRestartRequest performs no in-place
// restart (the service layer terminates + relaunches instead).
void TestDebugSessionRestartNoOpWithoutCapability() {
#if !defined(__unix__) && !defined(__APPLE__)
  return;
#endif
  TemporaryDirectory temp_dir;
  const auto server_path = temp_dir.path() / "adapter.py";
  WriteFile(server_path, std::string(MockAdapterSource()));

  DapManager manager;
  manager.RegisterAdapter("mock", MockAdapterCommand(server_path, "stop"));  // no restart cap

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
  // Give any erroneous restart a chance to surface, then confirm none was sent.
  PollUntil(manager, [&]() { return captured.output.find("cmd:restart") != std::string::npos; },
            200);
  Expect(captured.output.find("cmd:restart") == std::string::npos,
         "Restart must not send a `restart` request without the capability");
  manager.ShutdownAll();
}

// Multi-thread: on stop the session caches the full thread list via `threads`,
// and SwitchThread re-resolves the picked thread's frames (observably different).
void TestDebugSessionThreadsCachedAndSwitch() {
#if !defined(__unix__) && !defined(__APPLE__)
  return;
#endif
  TemporaryDirectory temp_dir;
  const auto server_path = temp_dir.path() / "adapter.py";
  WriteFile(server_path, std::string(MockAdapterSource()));

  DapManager manager;
  manager.RegisterAdapter("mock", MockAdapterCommand(server_path, "threads"));

  CapturedSession captured;
  LaunchConfig config;
  config.type = "mock";
  config.request = "launch";
  Expect(manager.StartSession(config, MakeCallbacks(captured)), "session should start");
  Expect(PollUntil(manager, [&]() { return captured.stop_count >= 1; }), "adapter should stop");
  // The thread list lands a beat after the stop (a second `threads` request).
  Expect(PollUntil(manager, [&]() { return captured.last_threads.size() == 2; }),
         "the session should cache the adapter's full thread list on stop");
  Expect(captured.last_threads[0].name == "main" && captured.last_threads[1].name == "worker",
         "both threads should resolve with their names");
  Expect(captured.last_frames.size() == 2 && captured.last_frames[0].name == "main",
         "the initial stop resolves thread 1's frames");

  // Switch to thread 2 → its frames re-resolve (top frame name differs).
  manager.ActiveSession()->SwitchThread(2);
  Expect(PollUntil(manager,
                   [&]() {
                     return !captured.last_frames.empty() &&
                            captured.last_frames[0].name == "worker";
                   }),
         "SwitchThread should re-resolve the picked thread's frames");
  manager.ShutdownAll();
}

// A `thread` event (a thread started/exited mid-run) refreshes the cached thread
// list, so the Call Stack thread selector stays current between stops. The adapter
// reports a single thread at the stop, then announces a worker via a `thread`
// event; the host must fetch the list a second time and grow it to two.
void TestDebugSessionThreadEventRefreshesThreadList() {
#if !defined(__unix__) && !defined(__APPLE__)
  return;
#endif
  TemporaryDirectory temp_dir;
  const auto server_path = temp_dir.path() / "adapter.py";
  WriteFile(server_path, std::string(MockAdapterSource()));

  DapManager manager;
  manager.RegisterAdapter("mock", MockAdapterCommand(server_path, "thread_event"));

  CapturedSession captured;
  LaunchConfig config;
  config.type = "mock";
  config.request = "launch";
  Expect(manager.StartSession(config, MakeCallbacks(captured)), "session should start");
  Expect(PollUntil(manager, [&]() { return captured.stop_count >= 1; }), "adapter should stop");
  // First fetch (from the stop) sees one thread; the `thread` event then drives a
  // second fetch that adds the worker. Reaching two proves the event refreshed it.
  Expect(PollUntil(manager, [&]() { return captured.last_threads.size() == 2; }),
         "a thread event should refresh the cached thread list");
  Expect(captured.last_threads[1].id == 2 && captured.last_threads[1].name == "worker",
         "the refreshed list should include the newly started thread");
  manager.ShutdownAll();
}

// Exception filters: the adapter advertises filters at initialize; the session
// surfaces them and sends the enabled ids via setExceptionBreakpoints on launch;
// a live re-send reflects a toggle.
void TestDebugSessionExceptionFiltersSentOnLaunchAndToggle() {
#if !defined(__unix__) && !defined(__APPLE__)
  return;
#endif
  TemporaryDirectory temp_dir;
  const auto server_path = temp_dir.path() / "adapter.py";
  WriteFile(server_path, std::string(MockAdapterSource()));

  DapManager manager;
  manager.RegisterAdapter("mock", MockAdapterCommand(server_path, "exception"));

  CapturedSession captured;
  DebugSession::Callbacks callbacks = MakeCallbacks(captured);
  // The host's enabled-filter set (intersected with advertised filters by the
  // session). "bogus" must be filtered out; only "raised" should reach the wire.
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

  // Toggle: enable both, live re-send.
  enabled = {"raised", "uncaught"};
  manager.ActiveSession()->ResendExceptionFilters();
  Expect(PollUntil(manager,
                   [&]() { return captured.output.find("exc:raised,uncaught\n") != std::string::npos; }),
         "a live re-send should reflect the updated enabled set");
  manager.ShutdownAll();
}

// Pure DebugBreakpointsModel behavior (no adapter): default seeding, toggle,
// EnabledAdvertisedIds intersection, and Rebuild's prebuilt rows.
void TestDebugBreakpointsModelBehavior() {
  using microide::workspace::DebugBreakpointsModel;
  using microide::workspace::DebugBreakpointRowView;
  DebugBreakpointsModel model;

  std::vector<codec::DapExceptionFilter> filters = {
      codec::DapExceptionFilter{.filter = "raised", .label = "Raised", .default_enabled = false},
      codec::DapExceptionFilter{.filter = "uncaught", .label = "Uncaught", .default_enabled = true},
  };
  // First advertise seeds defaults (only "uncaught" is default-on).
  Expect(model.SetAdvertisedFilters(filters), "first advertise seeds defaults (a change)");
  Expect(model.Seeded(), "the model records that defaults were seeded");
  Expect(model.IsEnabled("uncaught") && !model.IsEnabled("raised"),
         "the default-on filter is enabled, the default-off one is not");
  Expect((model.EnabledAdvertisedIds() == std::vector<std::string>{"uncaught"}),
         "EnabledAdvertisedIds returns only enabled advertised ids in advertised order");

  // Re-advertising does not re-seed (a user's choice persists).
  Expect(!model.SetAdvertisedFilters(filters), "re-advertise after seeding is not a change");

  // Toggle raised on; an unknown id is a no-op.
  Expect(model.ToggleFilter("raised"), "toggling an advertised filter changes the set");
  Expect(!model.ToggleFilter("nope"), "toggling an unadvertised filter is a no-op");
  Expect((model.EnabledAdvertisedIds() == std::vector<std::string>{"raised", "uncaught"}),
         "both filters are now enabled, in advertised order");

  // Rebuild rows: a header + one row per filter, then a breakpoints section.
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

  // Clearing advertised filters (session stop) drops the filter section.
  model.ClearAdvertisedFilters();
  model.Rebuild(store, function_store);
  for (const DebugBreakpointRowView& row : model.Rows()) {
    Expect(row.kind != DebugBreakpointRowView::Kind::ExceptionFilter,
           "no exception-filter rows remain after the advertised set is cleared");
  }
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

void TestDebugManagerRetainAdaptersDropsStaleTypes() {
  DapManager manager;
  manager.RegisterAdapter("a", {"true"});
  manager.RegisterAdapter("b", {"true"});
  Expect(manager.HasAdapter("a") && manager.HasAdapter("b"), "both adapters should register");
  manager.RetainAdaptersIn({"b"});
  Expect(!manager.HasAdapter("a"), "stale adapter 'a' should be dropped on reconcile");
  Expect(manager.HasAdapter("b"), "retained adapter 'b' should survive reconcile");
}

// Pure DebugExecutionView: the Call Stack panel lays out optional session +
// thread selectors above the frame list. PanelRowAt/PanelRowCount must dispatch
// sessions → threads → frames, and Clear() must preserve the session selector
// (sourced from DapManager) while dropping the stop-scoped state.
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

  // A single session shows no session selector; the rows shift up.
  view.sessions = {{1, "only", false}};
  Expect(!view.HasSessionSelector(), "one session hides the selector");
  Expect(view.PanelRowCount() == 4, "no session rows: 2 threads + 2 frames");
  Expect(view.PanelRowAt(0).kind == Kind::Thread, "row 0 is now the first thread");

  // Clear() keeps the session selector but drops the stop-scoped state.
  view.sessions = {{1, "a", false}, {2, "b", false}};
  view.focused_session_id = 2;
  view.Clear();
  Expect(view.sessions.size() == 2 && view.focused_session_id == 2,
         "Clear preserves the manager-sourced session selector");
  Expect(view.frames.empty() && view.threads.empty() && !view.stopped,
         "Clear drops the stop-scoped frames/threads/state");
}

// Two concurrent sessions against the real mock adapter: distinct ids, both reach
// a stop, Sessions() exposes id/name/state/attention, the active session switches
// and Reactivate re-projects, and stopping the active session prunes it so the
// active advances — down to an empty manager.
void TestDebugManagerMultipleConcurrentSessions() {
#if !defined(__unix__) && !defined(__APPLE__)
  return;
#endif
  TemporaryDirectory temp_dir;
  const auto server_path = temp_dir.path() / "adapter.py";
  WriteFile(server_path, std::string(MockAdapterSource()));

  DapManager manager;
  manager.RegisterAdapter("mock", MockAdapterCommand(server_path, "stop"));

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

  manager.StopActiveSession();  // stops id_a
  Expect(PollUntil(manager,
                   [&]() {
                     manager.PruneTerminated();
                     return manager.SessionCount() == 1;
                   }),
         "the stopped active session is pruned once its adapter terminates");
  Expect(manager.ActiveSessionId() == id_b, "the active session advances to the survivor");

  manager.StopActiveSession();  // stops id_b
  Expect(PollUntil(manager,
                   [&]() {
                     manager.PruneTerminated();
                     return manager.SessionCount() == 0;
                   }),
         "the last session prunes to an empty manager");
  Expect(manager.ActiveSessionId() == 0, "no active session id remains");
  manager.ShutdownAll();
}

// The restart terminate+relaunch fallback replaces the active session in place:
// the session set stays size 1 (no second row) and the replacement is active with
// a fresh id.
void TestDebugManagerReplaceActiveSession() {
#if !defined(__unix__) && !defined(__APPLE__)
  return;
#endif
  TemporaryDirectory temp_dir;
  const auto server_path = temp_dir.path() / "adapter.py";
  WriteFile(server_path, std::string(MockAdapterSource()));

  DapManager manager;
  manager.RegisterAdapter("mock", MockAdapterCommand(server_path, "config_done"));

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

// "Stop All Sessions" (Phase 10): BeginShutdownAll + ShutdownAll tears down every
// live session at once; the subsequent prune empties the manager. This is the
// manager path DebugService::StopAllDebugging wraps.
void TestDebugManagerStopAllSessions() {
#if !defined(__unix__) && !defined(__APPLE__)
  return;
#endif
  TemporaryDirectory temp_dir;
  const auto server_path = temp_dir.path() / "adapter.py";
  WriteFile(server_path, std::string(MockAdapterSource()));

  DapManager manager;
  manager.RegisterAdapter("mock", MockAdapterCommand(server_path, "stop"));

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
  manager.ShutdownAll();  // blocks until both adapter I/O threads join
  Expect(PollUntil(manager,
                   [&]() {
                     manager.PruneTerminated();
                     return manager.SessionCount() == 0;
                   }),
         "Stop All tears every session down to an empty manager");
  Expect(manager.ActiveSessionId() == 0, "no active session id remains after Stop All");
}

// Two-phase stop reporting at the DebugService level: notify_stop_began must fire
// once with the real reason/thread BEFORE frames resolve, and notify_stop_resolved
// must fire once AFTER ProjectStop populates the execution view. Regression for the
// empty/late control-channel `stopped` event (the immediate phase let a headless
// agent learn of the halt while a slow adapter still resolved the stack).
void TestDebugServiceTwoPhaseStopReporting() {
#if !defined(__unix__) && !defined(__APPLE__)
  return;
#endif
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
    bool stopped_at_began = true;       // sentinel: should read false (not yet resolved)
    std::size_t frames_at_began = 99;   // sentinel: should read 0 (not yet resolved)
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
  service.CurrentDapManager().RegisterAdapter("mock", MockAdapterCommand(server_path, "stop"));

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

// Multi-session: a background session's stop must NOT drive the shared immediate
// broadcast — notify_stop_began fires for the active session only (mirrors the
// active-session guard the shared thread/stop selector already enforces).
void TestDebugServiceBackgroundStopDoesNotBroadcast() {
#if !defined(__unix__) && !defined(__APPLE__)
  return;
#endif
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
  service.CurrentDapManager().RegisterAdapter("mock", MockAdapterCommand(server_path, "stop"));

  DapManager& manager = service.CurrentDapManager();
  LaunchConfig config;
  config.type = "mock";
  config.request = "launch";

  // Session A starts, stops, and stays parked (active + Stopped).
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
  began = 0;  // ignore A's (active) immediate broadcast; isolate B's behavior.

  // Session B starts (active on launch); immediately re-focus A so A stays the
  // active/parked session and B's incoming stop drains as a background stop.
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

// Phase A defense-in-depth: node ids stay globally monotonic across the tree
// rebuilds a frame switch / stop triggers, so a stale async response can never
// alias a freshly-created node that would otherwise have reused the same id.
void TestDebugValueTreeNodeIdsAreMonotonic() {
  DebugVariablesModel model;
  model.BeginFrame(1);
  model.ApplyScopes({codec::DapScope{.name = "Locals", .variables_reference = 1000}});
  Expect(!model.Rows().empty(), "first frame builds a scope row");
  const std::uint32_t first_id = model.Rows()[0].node_id;

  // A new frame Clears + rebuilds the tree. Ids must not restart at 1.
  model.BeginFrame(2);
  model.ApplyScopes({codec::DapScope{.name = "Locals", .variables_reference = 1000}});
  Expect(!model.Rows().empty(), "second frame builds a scope row");
  const std::uint32_t second_id = model.Rows()[0].node_id;
  Expect(second_id > first_id, "node ids stay globally monotonic across rebuilds");
}

// A large indexed container (here 950 elements) must stream in bounded pages, not
// in one shot — the bound is what stops a garbage/uninitialized container that
// reports billions of elements from freezing the host. Walk the full paging
// sequence and assert every fetch is bounded and the final page clamps to the
// remainder.
void TestDebugValueTreePagingBoundaries() {
  DebugValueTree tree;
  tree.AddRoot("arr", "[950]", "int[950]", /*variables_reference=*/2000, /*is_scope=*/false,
               /*total_count=*/950, /*total_known=*/true);
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

// Variables references are not stable across stops, so expansion is tracked by
// node path (the root→node name chain). A deep expansion (scope → struct → leaf)
// must be restored on the next stop even though every reference is renumbered,
// cascading down as each newly-attached child arrives.
void TestDebugValueTreeDeepNestingRestoreAcrossStops() {
  DebugValueTree tree;

  // Stop 1: expand Locals → its child struct `node` → reveal `leaf`.
  tree.AddRoot("Locals", /*value=*/{}, /*type=*/{}, /*variables_reference=*/1000, /*is_scope=*/true,
               /*total_count=*/1, /*total_known=*/true);
  tree.Rebuild();
  Expect(tree.ToggleRow(0).reference == 1000, "expanding Locals fetches its scope ref");
  codec::DapVariable node;
  node.name = "node";
  node.value = "{...}";
  node.variables_reference = 1001;
  node.named_variables = 1;
  node.count_reported = true;
  tree.ApplyVariables(1000, {node}, 0);
  // Find and expand `node`.
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

  // Stop 2: every reference is renumbered. Re-install the scope root with a fresh
  // ref and restore the open tree by path.
  tree.ClearRoots();
  tree.AddRoot("Locals", {}, {}, /*variables_reference=*/2000, /*is_scope=*/true,
               /*total_count=*/1, /*total_known=*/true);
  const std::vector<DebugValueTree::ChildFetch> restore = tree.RestoreExpandedRoots();
  tree.Rebuild();
  Expect(restore.size() == 1 && restore[0].reference == 2000,
         "the previously-expanded Locals scope re-expands against its new ref");

  // The struct child comes back with a renumbered ref; the cascade must request it
  // because the path Locals→node was open before the stop.
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

// Phase C / Finding 1: an adapter that reaches Running and then exits WITHOUT a
// DAP terminated/exited event must be reconciled to a terminal state and pruned,
// not left as a zombie session forever.
void TestDebugSessionReconciledWhenAdapterDiesSilently() {
#if !defined(__unix__) && !defined(__APPLE__)
  return;
#endif
  TemporaryDirectory temp_dir;
  const auto server_path = temp_dir.path() / "adapter.py";
  WriteFile(server_path, std::string(MockAdapterSource()));

  DapManager manager;
  manager.RegisterAdapter("mock", MockAdapterCommand(server_path, "die"));

  CapturedSession captured;
  LaunchConfig config;
  config.type = "mock";
  config.request = "launch";
  Expect(manager.StartSession(config, MakeCallbacks(captured)), "session should start");

  // The adapter acknowledges launch (reaching Running) then exits silently.
  Expect(PollUntil(manager,
                   [&]() {
                     const DebugSession* s = manager.ActiveSession();
                     return s != nullptr && !s->Client().IsRunning();
                   }),
         "adapter process should exit after reaching Running");

  // With no terminated event and no pending requests, nothing has moved the session
  // terminal: PruneTerminated alone would leave it forever.
  Expect(manager.PruneTerminated().empty(), "a non-terminal session is not prunable on its own");
  const DebugSession* before = manager.ActiveSession();
  Expect(before != nullptr && before->CurrentState() != DebugSession::State::Terminated &&
             before->CurrentState() != DebugSession::State::Failed,
         "the dead-but-unreconciled session is still non-terminal");

  // Reconciliation transitions it to Failed (no stop was requested), then it prunes.
  Expect(manager.ReapExitedSessions(), "ReapExitedSessions should transition the dead session");
  const DebugSession* after = manager.ActiveSession();
  Expect(after != nullptr && after->CurrentState() == DebugSession::State::Failed,
         "a silently-dead adapter session becomes Failed");
  // The terminal transition must fire on_terminated so a push observer (the control
  // channel) emits `terminated` even though the adapter never sent a DAP terminated
  // event. `failed` + a non-empty reason distinguish the crash from a clean exit.
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

// Function breakpoints reach the wire (mock adapter): installed after setBreakpoints
// and before configurationDone (the gdb launch order), with verification reflected
// back into a FunctionBreakpointStore by requested name.
void TestDebugSessionFunctionBreakpointsSentOnLaunch() {
#if !defined(__unix__) && !defined(__APPLE__)
  return;
#endif
  TemporaryDirectory temp_dir;
  const auto server_path = temp_dir.path() / "adapter.py";
  WriteFile(server_path, std::string(MockAdapterSource()));

  DapManager manager;
  manager.RegisterAdapter("mock", MockAdapterCommand(server_path, "function"));

  editor::FunctionBreakpointStore fn_store;
  fn_store.Add("add");
  fn_store.Add("compute");
  fn_store.SetCondition(1, "n > 0");

  // A line breakpoint too, so the full ordering (setBreakpoints first) is observable.
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
  // Ordering: setBreakpoints, then setFunctionBreakpoints, then configurationDone.
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

// Exception-filter conditions reach the wire as filterOptions when the adapter
// advertises supportsExceptionFilterOptions + the filter advertises supportsCondition.
void TestDebugSessionExceptionFilterConditionsSent() {
#if !defined(__unix__) && !defined(__APPLE__)
  return;
#endif
  TemporaryDirectory temp_dir;
  const auto server_path = temp_dir.path() / "adapter.py";
  WriteFile(server_path, std::string(MockAdapterSource()));

  DapManager manager;
  manager.RegisterAdapter("mock", MockAdapterCommand(server_path, "exception"));

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

// Pure FunctionBreakpointStore behavior (no adapter): add/dedup/remove/toggle,
// condition setters, and positional-by-name verification.
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

  // Verification is positional to the requested names, matched by name (not index).
  // Request in reverse order; results must still land on the right rows.
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

// ---- Real gdb 17.x end-to-end (gated; skipped when gdb/gcc are unavailable) ----
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

// Drive the real gdb 17.x DAP adapter through the production DapManager/DebugSession:
// install a function breakpoint on `add`, launch a compiled C program, and assert
// gdb stops inside add() with the breakpoint verified. This exercises the exact
// launch ordering (launch -> setBreakpoints -> setFunctionBreakpoints ->
// configurationDone) against the real adapter. Skipped when gdb/gcc are absent.
void TestGdbRealFunctionBreakpointsE2E() {
#if !defined(__unix__) && !defined(__APPLE__)
  return;
#endif
  if (!GdbDapAvailable()) {
    return;  // gated: no gdb on this host
  }
  TemporaryDirectory temp_dir;
  const auto source = temp_dir.path() / "prog.c";
  WriteFile(source,
            "#include <stdio.h>\n"
            "int add(int a, int b){ int s = a + b; return s; }\n"
            "int main(){ int x = 42, y = 58; int z = add(x, y); printf(\"%d\\n\", z); return 0; }\n");
  const auto exe = temp_dir.path() / "prog";
  if (!CompileWithGcc(source, exe)) {
    return;  // gated: no working gcc
  }

  DapManager manager;
  manager.RegisterAdapter("gdb", std::vector<std::string>{"gdb", "--interpreter=dap"});

  editor::FunctionBreakpointStore fn_store;
  fn_store.Add("add");

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
  // gdb reports a function breakpoint `pending` in the response, then verifies it
  // via a `breakpoint` event once the inferior binds it — route that to the store.
  callbacks.on_breakpoint_changed = [&fn_store](const std::filesystem::path&,
                                                const codec::DapBreakpoint& bp) {
    fn_store.ApplyBreakpointEvent(editor::VerifiedFunctionBreakpoint{
        .id = bp.id, .verified = bp.verified, .message = bp.message});
  };

  LaunchConfig config;
  config.type = "gdb";
  config.request = "launch";
  {
    util::JsonObject args;
    args["program"] = JsonValue(exe.string());
    config.arguments = JsonValue(std::move(args));
  }
  Expect(manager.StartSession(config, std::move(callbacks)), "gdb session should start");
  // gdb indexes DWARF + spawns the inferior; give it a generous window.
  Expect(PollUntil(manager, [&]() { return captured.stop_count >= 1; }, 20000),
         "gdb should stop at the 'add' function breakpoint");
  Expect(!captured.last_frames.empty(), "the stop resolves a call stack");
  Expect(!captured.last_frames.empty() &&
             captured.last_frames[0].name.find("add") != std::string::npos,
         "the top frame is inside add()");
  Expect(PollUntil(manager,
                   [&]() { return !fn_store.All().empty() && fn_store.All()[0].verified; }, 5000),
         "the function breakpoint verifies against real gdb 17.x");
  manager.ShutdownAll();
}

}  // namespace

void RegisterDebugServiceTests(std::vector<TestCase>& tests) {
  AddTest(tests, "DebugService/ValueTreeNodeIdsAreMonotonic",
          TestDebugValueTreeNodeIdsAreMonotonic);
  AddTest(tests, "DebugService/ValueTreePagingBoundaries", TestDebugValueTreePagingBoundaries);
  AddTest(tests, "DebugService/ValueTreeDeepNestingRestoreAcrossStops",
          TestDebugValueTreeDeepNestingRestoreAcrossStops);
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
  AddTest(tests, "DebugService/SessionPauseFromRunning", TestDebugSessionPauseFromRunning);
  AddTest(tests, "DebugService/SessionReverseStepAndContinue",
          TestDebugSessionReverseStepAndContinue);
  AddTest(tests, "DebugService/SessionReverseGatedOnCapability",
          TestDebugSessionReverseGatedOnCapability);
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
  AddTest(tests, "DebugService/FunctionBreakpointStoreBehavior",
          TestFunctionBreakpointStoreBehavior);
  AddTest(tests, "DebugService/GdbRealFunctionBreakpointsE2E", TestGdbRealFunctionBreakpointsE2E);
  AddTest(tests, "DebugService/BreakpointsModelBehavior", TestDebugBreakpointsModelBehavior);
  AddTest(tests, "DebugService/SessionVariablesTreeAndSetVariable",
          TestDebugSessionVariablesTreeAndSetVariable);
  AddTest(tests, "DebugService/SessionEvaluateHover", TestDebugSessionEvaluateHover);
  AddTest(tests, "DebugService/SessionWatchEvaluate", TestDebugSessionWatchEvaluate);
  AddTest(tests, "DebugService/SessionReplEvaluate", TestDebugSessionReplEvaluate);
  AddTest(tests, "DebugService/ManagerStopAllSessions", TestDebugManagerStopAllSessions);
  AddTest(tests, "DebugService/TwoPhaseStopReporting", TestDebugServiceTwoPhaseStopReporting);
  AddTest(tests, "DebugService/BackgroundStopDoesNotBroadcast",
          TestDebugServiceBackgroundStopDoesNotBroadcast);
  AddTest(tests, "DebugService/HoverFallsBackToReplWithoutCapability",
          TestDebugSessionHoverFallsBackToReplWithoutCapability);
  AddTest(tests, "DebugService/HoverModelBehavior", TestDebugHoverModelBehavior);
  AddTest(tests, "DebugService/SessionSetVariableGatedOnCapability",
          TestDebugSessionSetVariableGatedOnCapability);
  AddTest(tests, "DebugService/VariablesModelTreeBehavior", TestDebugVariablesModelTreeBehavior);
  AddTest(tests, "DebugService/VariablesLocalsOpenByDefault",
          TestDebugVariablesLocalsOpenByDefault);
  AddTest(tests, "DebugService/VariablesValueKindAndPlaceholder",
          TestDebugVariablesValueKindAndPlaceholder);
  AddTest(tests, "DebugService/VariablesPagingAndErrors", TestDebugVariablesPagingAndErrors);
  AddTest(tests, "DebugService/GdbAdapterClampsValueFormatting",
          TestGdbAdapterClampsValueFormatting);
  AddTest(tests, "DebugService/WatchModelBehavior", TestDebugWatchModelBehavior);
  AddTest(tests, "DebugService/ManagerRejectsUnknownAdapterType",
          TestDebugManagerRejectsUnknownAdapterType);
  AddTest(tests, "DebugService/ManagerRetainAdaptersDropsStaleTypes",
          TestDebugManagerRetainAdaptersDropsStaleTypes);
  AddTest(tests, "DebugService/ExecutionViewPanelRowDispatch",
          TestDebugExecutionViewPanelRowDispatch);
  AddTest(tests, "DebugService/ManagerMultipleConcurrentSessions",
          TestDebugManagerMultipleConcurrentSessions);
  AddTest(tests, "DebugService/ManagerReplaceActiveSession",
          TestDebugManagerReplaceActiveSession);
}

}  // namespace microide::tests
