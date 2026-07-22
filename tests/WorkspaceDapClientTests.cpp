#include "TestSupport.h"

#include "util/JsonValue.h"
#include "workspace/DapProtocol.h"
#include "workspace/WorkspaceDapClient.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <functional>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace microide::tests {
namespace {

using microide::util::JsonValue;
using microide::workspace::DapClient;
namespace codec = microide::workspace::dap_protocol;

// A minimal DAP adapter in Python. Speaks Content-Length framed JSON and answers
// the handful of commands the client tests exercise. argv[1] is a marker path the
// adapter touches when it processes `disconnect`.
const char* MockAdapterSource() {
  return R"py(import json
import pathlib
import sys

marker_path = pathlib.Path(sys.argv[1]) if len(sys.argv) > 1 else None
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

while True:
    msg = read_message()
    if msg is None:
        break
    if msg.get("type") != "request":
        continue
    command = msg.get("command")
    if command == "initialize":
        respond(msg, {"supportsConfigurationDoneRequest": True,
                      "supportsConditionalBreakpoints": True})
        event("initialized")
    elif command == "threads":
        respond(msg, {"threads": [{"id": 1, "name": "main"}, {"id": 2, "name": "worker"}]})
    elif command == "evaluate":
        expr = msg.get("arguments", {}).get("expression", "")
        respond(msg, {"result": expr + "=ok", "variablesReference": 0})
    elif command == "emitStopped":
        respond(msg, {})
        event("stopped", {"reason": "breakpoint", "threadId": 1})
    elif command == "disconnect":
        respond(msg, {})
        if marker_path is not None:
            marker_path.write_text("disconnected\n", encoding="utf-8")
        break
    else:
        respond(msg, success=False, message="unknown command")
)py";
}

bool PollUntil(DapClient& client, const std::function<bool()>& predicate, int timeout_ms = 3000) {
  return WaitUntil(predicate, std::chrono::milliseconds(timeout_ms),
                   std::chrono::milliseconds(5),
                   [&client]() { client.DrainCallbacks(); });
}

bool WaitForInitialized(DapClient& client, int timeout_ms = 3000) {
  return PollUntil(client, [&]() { return client.IsInitialized(); }, timeout_ms);
}

std::vector<std::string> MockAdapterCommand(const std::filesystem::path& server_path,
                                            const std::filesystem::path& marker_path) {
  return {"python3", server_path.string(), marker_path.string()};
}

void TestWorkspaceDapClientInitializeHandshakeReportsCapabilities() {
#if !defined(__unix__) && !defined(__APPLE__)
  return;
#endif
  TemporaryDirectory temp_dir;
  const auto server_path = temp_dir.path() / "adapter.py";
  const auto marker_path = temp_dir.path() / "disconnect.txt";
  WriteFile(server_path, std::string(MockAdapterSource()));

  DapClient client;
  const bool started = client.Start(MockAdapterCommand(server_path, marker_path), "mock");
  Expect(started, "mock adapter should start");
  Expect(WaitForInitialized(client), "client should complete the initialize handshake");

  const codec::DapCapabilities caps = client.Capabilities();
  Expect(caps.supports_configuration_done_request,
         "capabilities from the initialize response should be retained");
  Expect(caps.supports_conditional_breakpoints, "conditional breakpoint capability retained");
  client.Shutdown();
}

void TestWorkspaceDapClientCorrelatesResponsesBySeq() {
#if !defined(__unix__) && !defined(__APPLE__)
  return;
#endif
  TemporaryDirectory temp_dir;
  const auto server_path = temp_dir.path() / "adapter.py";
  const auto marker_path = temp_dir.path() / "disconnect.txt";
  WriteFile(server_path, std::string(MockAdapterSource()));

  DapClient client;
  Expect(client.Start(MockAdapterCommand(server_path, marker_path), "mock"),
         "mock adapter should start");
  Expect(WaitForInitialized(client), "client should initialize");

  // Two concurrently in-flight requests must each get their own response.
  bool got_threads = false;
  bool got_eval = false;
  std::size_t thread_count = 0;
  std::string eval_result;
  client.SendRequestAsync("threads", JsonValue(nullptr),
                          [&](const codec::DapResponse& response) {
                            got_threads = true;
                            thread_count = codec::ParseThreads(response.body).size();
                          });
  JsonValue eval_args = *util::ParseJson(R"({"expression":"foo"})");
  client.SendRequestAsync("evaluate", eval_args, [&](const codec::DapResponse& response) {
    got_eval = true;
    eval_result = codec::ParseEvaluateResult(response.body).result;
  });

  Expect(PollUntil(client, [&]() { return got_threads && got_eval; }),
         "both responses should be delivered");
  Expect(thread_count == 2, "threads response should round-trip the body to the threads callback");
  Expect(eval_result == "foo=ok", "evaluate response should reach the evaluate callback, not threads");
  client.Shutdown();
}

void TestWorkspaceDapClientDispatchesEventsOnMainThread() {
#if !defined(__unix__) && !defined(__APPLE__)
  return;
#endif
  TemporaryDirectory temp_dir;
  const auto server_path = temp_dir.path() / "adapter.py";
  const auto marker_path = temp_dir.path() / "disconnect.txt";
  WriteFile(server_path, std::string(MockAdapterSource()));

  DapClient client;
  std::vector<std::string> events;
  std::string stopped_reason;
  client.SetEventCallback([&](const std::string& event, const JsonValue& body) {
    events.push_back(event);
    if (event == "stopped") {
      stopped_reason = codec::ParseStoppedEvent(body).reason;
    }
  });
  Expect(client.Start(MockAdapterCommand(server_path, marker_path), "mock"),
         "mock adapter should start");
  Expect(WaitForInitialized(client), "client should initialize");

  // The adapter emits `initialized` during the handshake; drain it first.
  Expect(PollUntil(client, [&]() {
           return std::find(events.begin(), events.end(), "initialized") != events.end();
         }),
         "initialized event should be delivered");

  client.SendRequestAsync("emitStopped", JsonValue(nullptr), {});
  Expect(PollUntil(client, [&]() { return !stopped_reason.empty(); }),
         "stopped event should be delivered on the main thread");
  Expect(stopped_reason == "breakpoint", "stopped event body should parse");
  client.Shutdown();
}

void TestWorkspaceDapClientReportsAdapterThatExitsBeforeInitialize() {
#if !defined(__unix__) && !defined(__APPLE__)
  return;
#endif
  TemporaryDirectory temp_dir;
  const auto server_path = temp_dir.path() / "adapter.py";
  WriteFile(server_path, std::string("import sys\nsys.exit(3)\n"));

  DapClient client;
  Expect(client.Start({"python3", server_path.string()}, "mock"),
         "client should launch the adapter process");
  // It never answers initialize; the client must give up rather than hang. The
  // init thread starts asynchronously, so wait for the failure to surface rather
  // than racing it.
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while (std::chrono::steady_clock::now() < deadline) {
    if (!client.IsInitializing() && !client.LastError().empty()) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  Expect(!client.IsInitialized(), "client should not report initialization after adapter exit");
  Expect(!client.LastError().empty(), "client should record an error when initialize never completes");
  client.Shutdown();
}

// TD-2026-07-17A-098: an adapter can stream many valid small event frames before its
// initialize response; the pre-initialize replay buffer must be bounded so it cannot grow
// without limit for the whole timeout window. A flooding adapter must fail initialization
// cleanly rather than accumulating an unbounded backlog.
void TestWorkspaceDapClientBoundsPreInitializeEventFlood() {
#if !defined(__unix__) && !defined(__APPLE__)
  return;
#endif
  TemporaryDirectory temp_dir;
  const auto server_path = temp_dir.path() / "flood_adapter.py";
  // On the initialize request, emit far more than kMaxDapEarlyMessages (10000) output
  // events and never send the initialize response. The client must shed the session.
  WriteFile(server_path, std::string(R"py(import json
import sys

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
    return json.loads(sys.stdin.buffer.read(content_length).decode("utf-8"))

def write_message(message):
    global seq
    seq += 1
    message["seq"] = seq
    data = json.dumps(message).encode("utf-8")
    sys.stdout.buffer.write(f"Content-Length: {len(data)}\r\n\r\n".encode("ascii"))
    sys.stdout.buffer.write(data)
    sys.stdout.buffer.flush()

msg = read_message()
if msg is not None and msg.get("command") == "initialize":
    for _ in range(10050):
        write_message({"type": "event", "event": "output",
                       "body": {"category": "stdout", "output": "x"}})
# Never send the initialize response; block until the client tears us down.
while sys.stdin.buffer.readline():
    pass
)py"));

  DapClient client;
  Expect(client.Start({"python3", server_path.string()}, "mock"),
         "client should launch the flooding adapter");
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (std::chrono::steady_clock::now() < deadline) {
    if (!client.IsInitializing() && !client.LastError().empty()) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  Expect(!client.IsInitialized(),
         "a pre-initialize event flood must not complete initialization");
  Expect(!client.LastError().empty(),
         "the client must record an error when the adapter floods pre-initialize events");
  client.Shutdown();
}

void TestWorkspaceDapClientShutdownSendsDisconnect() {
#if !defined(__unix__) && !defined(__APPLE__)
  return;
#endif
  TemporaryDirectory temp_dir;
  const auto server_path = temp_dir.path() / "adapter.py";
  const auto marker_path = temp_dir.path() / "disconnect.txt";
  WriteFile(server_path, std::string(MockAdapterSource()));

  DapClient client;
  Expect(client.Start(MockAdapterCommand(server_path, marker_path), "mock"),
         "mock adapter should start");
  Expect(WaitForInitialized(client), "client should initialize");
  client.Shutdown();

  Expect(std::filesystem::exists(marker_path),
         "shutdown should send a disconnect request the adapter can act on");
  Expect(client.IsShutdownComplete(), "shutdown should complete");
}

void TestWorkspaceDapClientStubModeAnswersRequestsAndInjectsEvents() {
  DapClient client;
  client.EnableTestStubMode();
  Expect(client.IsInitialized(), "stub mode should report an initialized client");
  Expect(client.IsRunning(), "stub mode should report a running client");

  client.SetTestRequestHandler([](const std::string& command, const JsonValue& arguments,
                                  DapClient::ResponseCallback callback) {
    codec::DapResponse response;
    response.success = true;
    response.command = command;
    if (command == "evaluate") {
      util::JsonObject body;
      body["result"] = JsonValue(arguments["expression"].AsString() + "!");
      response.body = JsonValue(std::move(body));
    }
    callback(response);
  });

  std::string result;
  client.SendRequestAsync("evaluate", *util::ParseJson(R"({"expression":"x"})"),
                          [&](const codec::DapResponse& response) {
                            result = codec::ParseEvaluateResult(response.body).result;
                          });
  Expect(PollUntil(client, [&]() { return !result.empty(); }, 1000),
         "stub request handler response should be drained on the main thread");
  Expect(result == "x!", "stub handler should answer the evaluate request");

  std::string injected;
  client.SetEventCallback([&](const std::string& event, const JsonValue&) { injected = event; });
  client.InjectTestEvent("stopped", JsonValue(nullptr));
  Expect(PollUntil(client, [&]() { return !injected.empty(); }, 1000),
         "injected stub event should be drained on the main thread");
  Expect(injected == "stopped", "stub event should reach the event callback");
  client.Shutdown();
}

void TestWorkspaceDapClientInitFailureFailsPendingRequest() {
#if !defined(__unix__) && !defined(__APPLE__)
  return;
#endif
  TemporaryDirectory temp_dir;
  const auto server_path = temp_dir.path() / "stall-adapter.py";
  // Consume the initialize request, then stall briefly and exit WITHOUT ever
  // responding, so the client's init loop hits EOF and takes the !got_init
  // failure path. The stall gives the test time to register a pending request
  // before init fails.
  WriteFile(server_path, std::string(R"py(import sys
import time

content_length = None
while True:
    line = sys.stdin.buffer.readline()
    if not line:
        break
    if line in (b"\r\n", b"\n"):
        break
    if line.lower().startswith(b"content-length:"):
        content_length = int(line.split(b":", 1)[1].strip())
if content_length:
    sys.stdin.buffer.read(content_length)
time.sleep(0.3)
)py"));

  DapClient client;
  Expect(client.Start({"python3", server_path.string()}, "mock"),
         "stall adapter should start");

  // Issue a request while the client is still initializing. When init fails, the
  // request must be failed (callback invoked with success=false), not dropped.
  std::atomic<bool> responded{false};
  bool success = true;
  client.SendRequestAsync("threads", JsonValue(nullptr),
                          [&](const codec::DapResponse& response) {
                            success = response.success;
                            responded.store(true, std::memory_order_release);
                          });

  Expect(PollUntil(client, [&]() { return responded.load(std::memory_order_acquire); }),
         "a request registered during a failed init must have its callback failed, not dropped");
  Expect(!client.IsInitialized(), "init should have failed (adapter never responded)");
  Expect(!success, "the failed request should report success=false");
  client.Shutdown();
}

// Regression (symmetric with the LSP client): the initialize wait is bounded by
// wall-clock, not an attempt count. proc.Read returns as soon as any data is available,
// so an adapter that emits a burst of events before its initialize response used to
// exhaust the attempt budget in milliseconds and get force-killed while healthy. This
// adapter floods 200 `output` events ahead of the response; the client must still
// initialize.
void TestWorkspaceDapClientPreInitializeEventFloodStillInitializes() {
#if !defined(__unix__) && !defined(__APPLE__)
  return;
#endif
  TemporaryDirectory temp_dir;
  const auto server_path = temp_dir.path() / "flood-adapter.py";
  WriteFile(server_path, std::string(R"py(import json
import sys
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

while True:
    msg = read_message()
    if msg is None:
        break
    if msg.get("type") != "request":
        continue
    command = msg.get("command")
    if command == "initialize":
        # Flood 200 output events (> the old 120 attempt budget), THEN respond.
        for i in range(200):
            write_message({"type": "event", "event": "output",
                           "body": {"category": "stdout", "output": "pre-init " + str(i) + "\n"}})
        write_message({"type": "response", "request_seq": msg["seq"], "success": True,
                       "command": "initialize", "body": {}})
        write_message({"type": "event", "event": "initialized"})
    elif command == "disconnect":
        write_message({"type": "response", "request_seq": msg["seq"], "success": True,
                       "command": "disconnect"})
        break
    else:
        write_message({"type": "response", "request_seq": msg["seq"], "success": False,
                       "command": command, "message": "unknown"})
)py"));

  DapClient client;
  Expect(client.Start({"python3", server_path.string()}, "mock"),
         "flood adapter should start");
  Expect(WaitForInitialized(client, 10000),
         "a burst of pre-initialize events must not exhaust the init budget and kill "
         "a healthy adapter");
  client.Shutdown();
}

// Regression: a non-conformant adapter that emits the `initialized` event BEFORE
// the initialize response must not make the client dispatch that event while
// capabilities are still default-constructed. If it did, the main-thread handshake
// would read supports_configuration_done_request==false and skip configurationDone,
// hanging a debuggee that needs it. The client now buffers pre-response messages and
// replays them only after capabilities are stored.
void TestWorkspaceDapClientEmitsInitializedBeforeResponseSeesCapabilities() {
#if !defined(__unix__) && !defined(__APPLE__)
  return;
#endif
  TemporaryDirectory temp_dir;
  const auto server_path = temp_dir.path() / "reorder-adapter.py";
  WriteFile(server_path, std::string(R"py(import json
import sys
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

while True:
    msg = read_message()
    if msg is None:
        break
    if msg.get("type") != "request":
        continue
    command = msg.get("command")
    if command == "initialize":
        # Non-conformant ordering: emit the `initialized` event BEFORE the response.
        write_message({"type": "event", "event": "initialized"})
        write_message({"type": "response", "request_seq": msg["seq"], "success": True,
                       "command": "initialize",
                       "body": {"supportsConfigurationDoneRequest": True,
                                "supportsConditionalBreakpoints": True}})
    elif command == "disconnect":
        write_message({"type": "response", "request_seq": msg["seq"], "success": True,
                       "command": "disconnect"})
        break
    else:
        write_message({"type": "response", "request_seq": msg["seq"], "success": False,
                       "command": command, "message": "unknown"})
)py"));

  DapClient client;
  std::optional<bool> caps_when_initialized;
  client.SetEventCallback([&](const std::string& event, const util::JsonValue&) {
    if (event == "initialized") {
      caps_when_initialized = client.Capabilities().supports_configuration_done_request;
    }
  });
  Expect(client.Start({"python3", server_path.string()}, "mock"),
         "reorder adapter should start");
  Expect(WaitForInitialized(client, 10000),
         "client should initialize despite the reordered messages");
  Expect(PollUntil(client, [&]() { return caps_when_initialized.has_value(); }, 10000),
         "the buffered initialized event should be delivered");
  Expect(caps_when_initialized.value_or(false),
         "capabilities must be parsed before a pre-response initialized event is dispatched");
  client.Shutdown();
}

}  // namespace

void RegisterWorkspaceDapClientTests(std::vector<TestCase>& tests) {
  AddTest(tests, "WorkspaceDapClient/EmitsInitializedBeforeResponseSeesCapabilities",
          TestWorkspaceDapClientEmitsInitializedBeforeResponseSeesCapabilities);
  AddTest(tests, "WorkspaceDapClient/PreInitializeEventFloodStillInitializes",
          TestWorkspaceDapClientPreInitializeEventFloodStillInitializes);
  AddTest(tests, "WorkspaceDapClient/InitFailureFailsPendingRequest",
          TestWorkspaceDapClientInitFailureFailsPendingRequest);
  AddTest(tests, "WorkspaceDapClient/InitializeHandshakeReportsCapabilities",
          TestWorkspaceDapClientInitializeHandshakeReportsCapabilities);
  AddTest(tests, "WorkspaceDapClient/CorrelatesResponsesBySeq",
          TestWorkspaceDapClientCorrelatesResponsesBySeq);
  AddTest(tests, "WorkspaceDapClient/DispatchesEventsOnMainThread",
          TestWorkspaceDapClientDispatchesEventsOnMainThread);
  AddTest(tests, "WorkspaceDapClient/ReportsAdapterThatExitsBeforeInitialize",
          TestWorkspaceDapClientReportsAdapterThatExitsBeforeInitialize);
  AddTest(tests, "WorkspaceDapClient/BoundsPreInitializeEventFlood",
          TestWorkspaceDapClientBoundsPreInitializeEventFlood);
  AddTest(tests, "WorkspaceDapClient/ShutdownSendsDisconnect",
          TestWorkspaceDapClientShutdownSendsDisconnect);
  AddTest(tests, "WorkspaceDapClient/StubModeAnswersRequestsAndInjectsEvents",
          TestWorkspaceDapClientStubModeAnswersRequestsAndInjectsEvents);
}

}  // namespace microide::tests
