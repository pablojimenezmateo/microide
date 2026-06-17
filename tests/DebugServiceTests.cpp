#include "TestSupport.h"

#include "editor/BreakpointStore.h"
#include "util/JsonValue.h"
#include "workspace/DapProtocol.h"
#include "workspace/DebugSession.h"
#include "workspace/DebugVariablesModel.h"
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
using microide::workspace::DebugVariablesModel;
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
# config_done / stop / pause / variables all advertise configurationDone support.
supports_config_done = mode in ("config_done", "stop", "pause", "variables")
supports_set_variable = (mode == "variables")
stop_on_config = mode in ("stop", "variables")  # emit `stopped` after configurationDone
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
        respond(msg, {"supportsConfigurationDoneRequest": supports_config_done,
                      "supportsSetVariable": supports_set_variable,
                      "supportsTerminateRequest": True})
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
        respond(msg, {"stackFrames": [
            {"id": 1, "name": "main", "line": 10, "column": 1,
             "source": {"name": "main.py", "path": "/proj/main.py"}},
            {"id": 2, "name": "caller", "line": 3, "column": 1,
             "source": {"name": "main.py", "path": "/proj/main.py"}},
        ], "totalFrames": 2})
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
    elif command == "threads":
        respond(msg, {"threads": [{"id": 1, "name": "main"}]})
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
  AddTest(tests, "DebugService/SessionVariablesTreeAndSetVariable",
          TestDebugSessionVariablesTreeAndSetVariable);
  AddTest(tests, "DebugService/SessionSetVariableGatedOnCapability",
          TestDebugSessionSetVariableGatedOnCapability);
  AddTest(tests, "DebugService/VariablesModelTreeBehavior", TestDebugVariablesModelTreeBehavior);
  AddTest(tests, "DebugService/ManagerRejectsUnknownAdapterType",
          TestDebugManagerRejectsUnknownAdapterType);
  AddTest(tests, "DebugService/ManagerRetainAdaptersDropsStaleTypes",
          TestDebugManagerRetainAdaptersDropsStaleTypes);
}

}  // namespace microide::tests
