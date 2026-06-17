#include "TestSupport.h"

#include "editor/BreakpointStore.h"
#include "util/JsonValue.h"
#include "workspace/DapProtocol.h"
#include "workspace/DebugBreakpointsModel.h"
#include "workspace/DebugSession.h"
#include "workspace/DebugVariablesModel.h"
#include "workspace/DebugViewModel.h"
#include "workspace/DebugWatchModel.h"
#include "workspace/LaunchConfig.h"
#include "workspace/WorkspaceDapManager.h"

#include <algorithm>
#include <chrono>
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
                                "restart", "threads", "exception")
supports_set_variable = (mode == "variables")
supports_evaluate = (mode == "evaluate")
supports_restart = (mode == "restart")
multi_thread = (mode == "threads")     # `threads` returns two threads + per-thread frames
exception_mode = (mode == "exception")  # advertise exceptionBreakpointFilters
# emit `stopped` after configurationDone
stop_on_config = mode in ("stop", "variables", "evaluate", "restart", "threads", "exception")
running_no_stop = (mode == "pause")   # stay running so the test can pause
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
                "supportsTerminateRequest": True}
        if exception_mode:
            caps["exceptionBreakpointFilters"] = [
                {"filter": "raised", "label": "Raised Exceptions", "default": False},
                {"filter": "uncaught", "label": "Uncaught Exceptions", "default": True},
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
    elif command == "configurationDone":
        respond(msg, {})
        if stop_on_config:
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
    elif command in ("continue", "next", "stepIn", "stepOut"):
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
        value = args.get("expression", "") + "@" + str(args.get("frameId", 0))
        respond(msg, {"result": value, "type": "int", "variablesReference": 0})
    elif command == "threads":
        if multi_thread:
            respond(msg, {"threads": [{"id": 1, "name": "main"}, {"id": 2, "name": "worker"}]})
        else:
            respond(msg, {"threads": [{"id": 1, "name": "main"}]})
    elif command == "setExceptionBreakpoints":
        filters = msg.get("arguments", {}).get("filters", [])
        event("output", {"category": "stdout", "output": "exc:" + ",".join(filters) + "\n"})
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
  };
  callbacks.on_event = [&captured](const std::string& event, const JsonValue&) {
    if (event == "terminated") {
      captured.got_terminated_event = true;
    }
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
  Expect(captured.got_terminated_event, "raw terminated event should reach on_event");
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

void TestDebugSessionSendsBreakpointsBeforeConfigurationDone() {
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
  std::filesystem::path verified_path;
  callbacks.on_breakpoints_verified =
      [&](const std::filesystem::path& path, const std::vector<codec::DapBreakpoint>& breakpoints) {
        verified_path = path;
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

  // The recorded command order must place setBreakpoints before configurationDone.
  const std::size_t order_pos = captured.output.find("commands:");
  Expect(order_pos != std::string::npos, "adapter should emit its command order");
  const std::size_t set_pos = captured.output.find("setBreakpoints", order_pos);
  const std::size_t cfg_pos = captured.output.find("configurationDone", order_pos);
  Expect(set_pos != std::string::npos && cfg_pos != std::string::npos && set_pos < cfg_pos,
         "setBreakpoints must be sent before configurationDone");
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

  // scopes → two collapsed top-level rows.
  bool scopes_done = false;
  session->RequestScopes(frame_id, [&](std::vector<codec::DapScope> scopes) {
    model.ApplyScopes(scopes);
    scopes_done = true;
  });
  Expect(PollUntil(manager, [&]() { return scopes_done; }), "scopes should resolve");
  Expect(model.Rows().size() == 2, "two scopes should produce two rows");
  Expect(model.Rows()[0].display_name == "Locals" && model.Rows()[0].has_children,
         "first scope should be an expandable Locals row");

  // Expand Locals → fetch its variables (x scalar, obj structured).
  const int locals_ref = model.ToggleRow(0);
  Expect(locals_ref == 1000, "expanding Locals should request its variablesReference");
  bool locals_done = false;
  session->RequestVariables(locals_ref, [&](std::vector<codec::DapVariable> vars) {
    model.ApplyVariables(locals_ref, vars);
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
  const int obj_ref = model.ToggleRow(2);
  Expect(obj_ref == 1001, "expanding obj should request its variablesReference");
  bool obj_done = false;
  session->RequestVariables(obj_ref, [&](std::vector<codec::DapVariable> vars) {
    model.ApplyVariables(obj_ref, vars);
    obj_done = true;
  });
  Expect(PollUntil(manager, [&]() { return obj_done; }), "obj variables should resolve");
  Expect(model.Rows().size() == 5 && model.Rows()[3].display_name == "field" &&
             model.Rows()[3].depth == 2,
         "obj's field should appear nested at depth 2");
  Expect(model.ToggleRow(2) == 0 && model.Rows().size() == 4,
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

// The session-level capability gate: an adapter without supportsEvaluateForHovers
// rejects a hover evaluate without sending anything on the wire.
void TestDebugSessionEvaluateGatedOnCapability() {
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

  bool callback_fired = false;
  bool callback_ok = true;
  manager.ActiveSession()->RequestEvaluate(
      "count", captured.last_frames.empty() ? 0 : captured.last_frames[0].id, "hover",
      [&](bool ok, codec::DapEvaluateResult) {
        callback_fired = true;
        callback_ok = ok;
      });
  Expect(PollUntil(manager, [&]() { return callback_fired; }),
         "hover evaluate callback should fire even when unsupported");
  Expect(!callback_ok, "hover evaluate should report failure without the capability");
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

  std::vector<codec::DapScope> scopes = {
      codec::DapScope{.name = "Locals", .variables_reference = 1000},
      codec::DapScope{.name = "Globals", .variables_reference = 2000},
  };
  model.ApplyScopes(scopes);
  Expect(model.Rows().size() == 2, "two scopes flatten to two rows");
  Expect(model.Rows()[0].has_children && !model.Rows()[0].expanded && !model.Rows()[0].editable,
         "a scope row is expandable, collapsed, and not editable");

  Expect(model.ToggleRow(0) == 1000, "expanding a not-loaded scope returns its reference to fetch");
  model.ApplyVariables(1000, {
      codec::DapVariable{.name = "x", .value = "1", .type = "int", .variables_reference = 0},
      codec::DapVariable{.name = "obj", .value = "{...}", .type = "Obj", .variables_reference = 1001},
  });
  Expect(model.Rows().size() == 4, "expanded Locals interleaves its children before Globals");
  Expect(model.Rows()[1].display_name == "x" && model.Rows()[1].depth == 1,
         "child x sits at depth 1");

  Expect(model.ToggleRow(2) == 1001, "expanding obj returns its reference");
  model.ApplyVariables(1001,
                       {codec::DapVariable{.name = "field", .value = "7", .variables_reference = 0}});
  Expect(model.Rows().size() == 5 && model.Rows()[3].depth == 2, "nested field flattens at depth 2");
  Expect(model.ToggleRow(2) == 0 && model.Rows().size() == 4, "collapse removes the subtree");
  Expect(model.ToggleRow(2) == 0 && model.Rows().size() == 5,
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
  Expect(model.ToggleRow(1) == 900, "expanding a structured root returns its reference to fetch");
  model.ApplyVariables(
      900, {codec::DapVariable{.name = "field", .value = "7", .variables_reference = 0}});
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
  callbacks.exception_filter_provider = [&enabled]() { return enabled; };

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
  model.Rebuild(store);
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
  model.Rebuild(store);
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

}  // namespace

void RegisterDebugServiceTests(std::vector<TestCase>& tests) {
  AddTest(tests, "DebugService/SessionDrivesLaunchLifecycleWithConfigurationDone",
          TestDebugSessionDrivesLaunchLifecycleWithConfigurationDone);
  AddTest(tests, "DebugService/SessionRunsWithoutConfigurationDoneSupport",
          TestDebugSessionRunsWithoutConfigurationDoneSupport);
  AddTest(tests, "DebugService/SessionSendsBreakpointsBeforeConfigurationDone",
          TestDebugSessionSendsBreakpointsBeforeConfigurationDone);
  AddTest(tests, "DebugService/SessionResolvesStackOnStopAndStepsResume",
          TestDebugSessionResolvesStackOnStopAndStepsResume);
  AddTest(tests, "DebugService/SessionPauseFromRunning", TestDebugSessionPauseFromRunning);
  AddTest(tests, "DebugService/SessionRestartViaRestartRequest",
          TestDebugSessionRestartViaRestartRequest);
  AddTest(tests, "DebugService/SessionRestartNoOpWithoutCapability",
          TestDebugSessionRestartNoOpWithoutCapability);
  AddTest(tests, "DebugService/SessionThreadsCachedAndSwitch",
          TestDebugSessionThreadsCachedAndSwitch);
  AddTest(tests, "DebugService/SessionExceptionFiltersSentOnLaunchAndToggle",
          TestDebugSessionExceptionFiltersSentOnLaunchAndToggle);
  AddTest(tests, "DebugService/BreakpointsModelBehavior", TestDebugBreakpointsModelBehavior);
  AddTest(tests, "DebugService/SessionVariablesTreeAndSetVariable",
          TestDebugSessionVariablesTreeAndSetVariable);
  AddTest(tests, "DebugService/SessionEvaluateHover", TestDebugSessionEvaluateHover);
  AddTest(tests, "DebugService/SessionWatchEvaluate", TestDebugSessionWatchEvaluate);
  AddTest(tests, "DebugService/SessionEvaluateGatedOnCapability",
          TestDebugSessionEvaluateGatedOnCapability);
  AddTest(tests, "DebugService/HoverModelBehavior", TestDebugHoverModelBehavior);
  AddTest(tests, "DebugService/SessionSetVariableGatedOnCapability",
          TestDebugSessionSetVariableGatedOnCapability);
  AddTest(tests, "DebugService/VariablesModelTreeBehavior", TestDebugVariablesModelTreeBehavior);
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
