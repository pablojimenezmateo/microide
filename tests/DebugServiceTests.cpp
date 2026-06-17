#include "TestSupport.h"

#include "util/JsonValue.h"
#include "workspace/DapProtocol.h"
#include "workspace/DebugSession.h"
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

supports_config_done = (len(sys.argv) > 1 and sys.argv[1] == "config_done")
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

def finish_launch():
    event("output", {"category": "stdout", "output": "hello from adapter\n"})
    event("terminated")

while True:
    msg = read_message()
    if msg is None:
        break
    if msg.get("type") != "request":
        continue
    command = msg.get("command")
    if command == "initialize":
        respond(msg, {"supportsConfigurationDoneRequest": supports_config_done,
                      "supportsTerminateRequest": True})
        event("initialized")
    elif command == "launch":
        respond(msg, {})
        if not supports_config_done:
            finish_launch()
    elif command == "configurationDone":
        respond(msg, {})
        finish_launch()
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
  AddTest(tests, "DebugService/ManagerRejectsUnknownAdapterType",
          TestDebugManagerRejectsUnknownAdapterType);
  AddTest(tests, "DebugService/ManagerRetainAdaptersDropsStaleTypes",
          TestDebugManagerRetainAdaptersDropsStaleTypes);
}

}  // namespace microide::tests
