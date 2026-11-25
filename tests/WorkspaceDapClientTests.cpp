#include "TestSupport.h"

#include "util/JsonValue.h"
#include "workspace/debug/DapProtocol.h"
#include "workspace/debug/WorkspaceDapClient.h"

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
  TemporaryDirectory temp_dir;
  const auto server_path = temp_dir.path() / "adapter.py";
  const auto marker_path = temp_dir.path() / "disconnect.txt";
  WriteFile(server_path, std::string(MockAdapterSource()));

  DapClient client;
  const bool started = client.Start(platform::LocalProcessLauncher(), MockAdapterCommand(server_path, marker_path), "mock");
  Expect(started, "mock adapter should start");
  Expect(WaitForInitialized(client), "client should complete the initialize handshake");

  const codec::DapCapabilities caps = client.Capabilities();
  Expect(caps.supports_configuration_done_request,
         "capabilities from the initialize response should be retained");
  Expect(caps.supports_conditional_breakpoints, "conditional breakpoint capability retained");
  client.Shutdown();
}

void TestWorkspaceDapClientCorrelatesResponsesBySeq() {
  TemporaryDirectory temp_dir;
  const auto server_path = temp_dir.path() / "adapter.py";
  const auto marker_path = temp_dir.path() / "disconnect.txt";
  WriteFile(server_path, std::string(MockAdapterSource()));

  DapClient client;
  Expect(client.Start(platform::LocalProcessLauncher(), MockAdapterCommand(server_path, marker_path), "mock"),
         "mock adapter should start");
  Expect(WaitForInitialized(client), "client should initialize");

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
  Expect(client.Start(platform::LocalProcessLauncher(), MockAdapterCommand(server_path, marker_path), "mock"),
         "mock adapter should start");
  Expect(WaitForInitialized(client), "client should initialize");

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
  TemporaryDirectory temp_dir;
  const auto server_path = temp_dir.path() / "adapter.py";
  WriteFile(server_path, std::string("import sys\nsys.exit(3)\n"));

  DapClient client;
  Expect(client.Start(platform::LocalProcessLauncher(), {"python3", server_path.string()}, "mock"),
         "client should launch the adapter process");
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

void TestWorkspaceDapClientBoundsPreInitializeEventFlood() {
  TemporaryDirectory temp_dir;
  const auto server_path = temp_dir.path() / "flood_adapter.py";
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
  Expect(client.Start(platform::LocalProcessLauncher(), {"python3", server_path.string()}, "mock"),
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
  TemporaryDirectory temp_dir;
  const auto server_path = temp_dir.path() / "adapter.py";
  const auto marker_path = temp_dir.path() / "disconnect.txt";
  WriteFile(server_path, std::string(MockAdapterSource()));

  DapClient client;
  Expect(client.Start(platform::LocalProcessLauncher(), MockAdapterCommand(server_path, marker_path), "mock"),
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
  TemporaryDirectory temp_dir;
  const auto server_path = temp_dir.path() / "stall-adapter.py";
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
  Expect(client.Start(platform::LocalProcessLauncher(), {"python3", server_path.string()}, "mock"),
         "stall adapter should start");

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

void TestWorkspaceDapClientPreInitializeEventFloodStillInitializes() {
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
  Expect(client.Start(platform::LocalProcessLauncher(), {"python3", server_path.string()}, "mock"),
         "flood adapter should start");
  Expect(WaitForInitialized(client, 10000),
         "a burst of pre-initialize events must not exhaust the init budget and kill "
         "a healthy adapter");
  client.Shutdown();
}

void TestWorkspaceDapClientEmitsInitializedBeforeResponseSeesCapabilities() {
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
  Expect(client.Start(platform::LocalProcessLauncher(), {"python3", server_path.string()}, "mock"),
         "reorder adapter should start");
  Expect(WaitForInitialized(client, 10000),
         "client should initialize despite the reordered messages");
  Expect(PollUntil(client, [&]() { return caps_when_initialized.has_value(); }, 10000),
         "the buffered initialized event should be delivered");
  Expect(caps_when_initialized.value_or(false),
         "capabilities must be parsed before a pre-response initialized event is dispatched");
  client.Shutdown();
}

void TestWorkspaceDapClientAnswersEveryRequestOnceWhateverOrderTheAdapterReplies() {
  TemporaryDirectory temp_dir;
  const auto server_path = temp_dir.path() / "adapter.py";
  WriteFile(server_path, std::string(R"py(import json
import random
import sys
import threading
import time

rng = random.Random(int(sys.argv[1]))
pending = []
reverse_open = set()
lock = threading.Lock()
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
    return json.loads(body.decode("utf-8")) if body else None

def write_message(message):
    global seq
    seq += 1
    message["seq"] = seq
    if message["type"] != "request":
        message.setdefault("body", {})["n"] = seq
    data = json.dumps(message).encode("utf-8")
    sys.stdout.buffer.write(f"Content-Length: {len(data)}\r\n\r\n".encode("ascii"))
    sys.stdout.buffer.write(data)
    sys.stdout.buffer.flush()
    return seq

def respond(req, body):
    write_message({"type": "response", "request_seq": req["seq"], "success": True,
                   "command": req["command"], "body": body})

def answer(req):
    body = {"slot": req.get("arguments", {}).get("slot", -1)}
    respond(req, dict(body))
    if rng.random() < 0.1:
        respond(req, dict(body))

def flush_some(everything=False):
    rng.shuffle(pending)
    take = len(pending) if everything else rng.randint(0, len(pending))
    for _ in range(take):
        if rng.random() < 0.2:
            time.sleep(rng.random() * 0.02)
        answer(pending.pop())

def late_answers():
    while True:
        time.sleep(0.03)
        with lock:
            flush_some(everything=True)

def chatter():
    roll = rng.random()
    if roll < 0.15:
        write_message({"type": "event", "event": "output",
                       "body": {"category": "stdout", "output": "x" * rng.choice([10, 200000])}})
    elif roll < 0.25:
        write_message({"type": "event", "event": rng.choice(["stopped", "continued"]),
                       "body": {"threadId": 1, "reason": "step"}})
    elif roll < 0.32:
        reverse_open.add(write_message({"type": "request", "command": "runInTerminal",
                                        "arguments": {"args": ["true"]}}))
    elif roll < 0.37:
        write_message({"type": "response", "request_seq": 900000 + seq, "success": True,
                       "command": "threads"})

threading.Thread(target=late_answers, daemon=True).start()
while True:
    msg = read_message()
    if msg is None:
        break
    lock.acquire()
    if msg.get("type") == "response":
        reverse_open.discard(msg.get("request_seq"))
        lock.release()
        continue
    command = msg.get("command")
    if command == "initialize":
        respond(msg, {"supportsConfigurationDoneRequest": True})
        write_message({"type": "event", "event": "initialized"})
    elif command == "report":
        flush_some(everything=True)
        respond(msg, {"unanswered_reverse": len(reverse_open)})
    elif command == "disconnect":
        respond(msg, {})
        lock.release()
        break
    else:
        chatter()
        pending.append(msg)
        flush_some()
    if command not in ("initialize", "report") and rng.random() < 0.3:
        flush_some(everything=True)
    lock.release()
)py"));
  const std::vector<std::string> commands = {"threads", "stackTrace", "scopes", "variables", "evaluate"};
  for (const int seed : {1, 2, 3, 4}) {
    DapClient client;
    std::vector<std::int64_t> order;
    client.SetEventCallback([&order](const std::string&, const JsonValue& body) {
      if (body.IsObject()) {
        order.push_back(body["n"].AsInt());
      }
    });
    Expect(client.Start(platform::LocalProcessLauncher(), {"python3", server_path.string(), std::to_string(seed)},
                        "mock"),
           "the scripted adapter starts");
    Expect(WaitForInitialized(client, 5000), "the adapter initializes");
    std::vector<int> answers;
    std::size_t misrouted = 0;
    std::uint64_t state = static_cast<std::uint64_t>(seed) * 6364136223846793005ull + 1442695040888963407ull;
    const auto next = [&state](std::uint64_t bound) {
      state = state * 6364136223846793005ull + 1442695040888963407ull;
      return (state >> 33) % bound;
    };
    for (int step = 0; step < 150; ++step) {
      const int slot = static_cast<int>(answers.size());
      answers.push_back(0);
      const std::string& command = commands[next(commands.size())];
      util::JsonObject arguments;
      arguments["slot"] = JsonValue(static_cast<std::int64_t>(slot));
      if (command == "evaluate" && next(8) == 0) {
        arguments["expression"] = JsonValue(std::string(200000, 'e'));
      }
      (void)client.SendRequestAsync(command, JsonValue(std::move(arguments)),
                                    [&, slot, command](const codec::DapResponse& response) {
                                      ++answers[static_cast<std::size_t>(slot)];
                                      if (!response.success || response.command != command ||
                                          response.body["slot"].AsInt(-1) != slot) {
                                        ++misrouted;
                                      }
                                      order.push_back(response.body["n"].AsInt());
                                    });
      client.DrainCallbacks();
    }
    const bool settled = PollUntil(
        client, [&] { return std::all_of(answers.begin(), answers.end(), [](int count) { return count >= 1; }); },
        30000);
    const auto unanswered = static_cast<std::size_t>(std::count(answers.begin(), answers.end(), 0));
    std::optional<std::int64_t> unanswered_reverse;
    (void)client.SendRequestAsync("report", JsonValue(nullptr), [&](const codec::DapResponse& response) {
      unanswered_reverse = response.body["unanswered_reverse"].AsInt(-1);
    });
    Expect(PollUntil(client, [&] { return unanswered_reverse.has_value(); }, 10000), "the adapter reports");
    const std::string label = "seed " + std::to_string(seed) + ": ";
    const auto twice = static_cast<std::size_t>(
        std::count_if(answers.begin(), answers.end(), [](int count) { return count > 1; }));
    Expect(settled && unanswered == 0,
           label + "every request is answered within 30 s, " + std::to_string(unanswered) + " of 150 were not");
    Expect(twice == 0, label + "no request is answered twice, " + std::to_string(twice) +
                           " were (the adapter repeats some responses)");
    Expect(misrouted == 0, label + "each response reaches its own request, " + std::to_string(misrouted) + " did not");
    Expect(std::is_sorted(order.begin(), order.end()) &&
               std::adjacent_find(order.begin(), order.end()) == order.end(),
           label + "responses and events reach the main thread in the order the adapter sent them");
    Expect(unanswered_reverse == 0, label + "every reverse request the adapter sent is answered, " +
                                        std::to_string(unanswered_reverse.value_or(-1)) + " were not");
    Expect(client.IsRunning(), label + "the client survives stray and repeated responses");
    client.Shutdown();
  }
}

}

void RegisterWorkspaceDapClientTests(std::vector<TestCase>& tests) {
  AddTest(tests, "WorkspaceDapClient/AnswersEveryRequestOnceWhateverOrderTheAdapterReplies",
          TestWorkspaceDapClientAnswersEveryRequestOnceWhateverOrderTheAdapterReplies);
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

}
