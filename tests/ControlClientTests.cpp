#include "TestSupport.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "platform/ControlSocketClient.h"
#include "platform/ControlSocketServer.h"
#include "util/JsonValue.h"
#include "workspace/ControlClient.h"
#include "workspace/WorkspaceCommandParsing.h"

#if defined(__unix__) || defined(__APPLE__)
#include <cstring>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace microide::tests {
namespace {

using microide::workspace::BuildControlSendRequest;
using microide::workspace::ControlSendOptions;
using microide::workspace::ParseControlSendArgs;

std::optional<util::JsonValue> RequestObject(const ControlSendOptions& options) {
  std::string error;
  const std::optional<std::string> line = BuildControlSendRequest(options, &error);
  if (!line) {
    return std::nullopt;
  }
  return util::ParseJson(*line);
}

void TestParseRawCommand() {
  const ControlSendOptions options =
      ParseControlSendArgs({"breakpoint-function-add", "main"});
  Expect(options.valid, "raw command should parse");
  const std::optional<util::JsonValue> request = RequestObject(options);
  Expect(request.has_value() && request->IsObject(), "request should build");
  Expect((*request)["command"].AsString() == "breakpoint-function-add main",
         "command tokens should join with spaces");
  Expect((*request)["id"].AsInt() == 1, "default id should be 1");
}

void TestParseCommandKeepsTrailingFlags() {
  // Unknown --flags (debug-run's --type) belong to the command; recognized
  // control-send flags (--type is not one) are consumed wherever they appear.
  const ControlSendOptions options =
      ParseControlSendArgs({"--timeout", "9", "debug-run", "./app", "--type", "gdb"});
  Expect(options.valid, "command with trailing flags should parse");
  Expect(options.timeout_seconds == 9, "leading --timeout should be consumed");
  const std::optional<util::JsonValue> request = RequestObject(options);
  Expect(request.has_value(), "request should build");
  Expect((*request)["command"].AsString() == "debug-run ./app --type gdb",
         "unknown --flags (--type) belong to the command");
}

void TestParseConsumesControlFlagsAnywhere() {
  // Agents naturally append --wait/--timeout after the command; they must still
  // be consumed by control-send, not leaked into the command.
  const ControlSendOptions options =
      ParseControlSendArgs({"debug-run", "./app", "--wait", "stopped", "--timeout", "30"});
  Expect(options.valid, "trailing control-send flags should parse");
  Expect(options.wait_event.has_value() && *options.wait_event == "stopped",
         "a trailing --wait should be consumed by control-send");
  Expect(options.timeout_seconds == 30, "a trailing --timeout should be consumed");
  const std::optional<util::JsonValue> request = RequestObject(options);
  Expect(request.has_value() && (*request)["command"].AsString() == "debug-run ./app",
         "the command should not absorb control-send flags");
}

void TestParseQuery() {
  const ControlSendOptions options = ParseControlSendArgs({"--query", "debug-state"});
  Expect(options.valid, "query should parse");
  const std::optional<util::JsonValue> request = RequestObject(options);
  Expect(request.has_value() && (*request)["query"].AsString() == "debug-state",
         "query verb should be emitted");
  Expect(!request->HasKey("command"), "query request should not carry a command");
}

void TestParseQueryWithArgs() {
  const ControlSendOptions options =
      ParseControlSendArgs({"--query", "tabs", "--args", R"({"limit":3})"});
  Expect(options.valid, "query with args should parse");
  const std::optional<util::JsonValue> request = RequestObject(options);
  Expect(request.has_value() && (*request)["args"]["limit"].AsInt() == 3,
         "args JSON should be embedded");
}

void TestParseJsonPassthroughInjectsId() {
  const ControlSendOptions options =
      ParseControlSendArgs({"--json", R"({"command":"debug-start"})"});
  Expect(options.valid, "raw json should parse");
  const std::optional<util::JsonValue> request = RequestObject(options);
  Expect(request.has_value() && (*request)["command"].AsString() == "debug-start",
         "raw json should pass through");
  Expect(request->HasKey("id"), "an id should be injected when absent");
}

void TestParseJsonPreservesOwnId() {
  const ControlSendOptions options =
      ParseControlSendArgs({"--json", R"({"id":42,"query":"status"})"});
  const std::optional<util::JsonValue> request = RequestObject(options);
  Expect(request.has_value() && (*request)["id"].AsInt() == 42,
         "an explicit id in --json should be preserved");
}

// The receiving instance re-parses the serialized `command` string with
// ParseCommandLine, so each argv token must round-trip verbatim through the client's
// quoting. A bare space-join would split a path with spaces and mangle quotes /
// backslashes / apostrophes. Regression for inventory J14.
void TestControlSendCommandTokensRoundTripThroughParser() {
  const std::vector<std::vector<std::string>> cases = {
      {"open", "dir/a b.cpp"},          // embedded space
      {"open", "O'Brien.cpp"},          // apostrophe
      {"open", "a\\b\\c"},              // backslashes
      {"open", "say \"hi\""},           // embedded double quotes + space
      {"debug-run", "./app", "--type", "gdb"},  // an argument that starts with --
      {"open", "plain.cpp"},            // no special bytes (unquoted fast path)
  };
  for (const std::vector<std::string>& tokens : cases) {
    const ControlSendOptions options = ParseControlSendArgs(tokens);
    Expect(options.valid, "command tokens should parse");
    const std::optional<util::JsonValue> request = RequestObject(options);
    Expect(request.has_value() && request->HasKey("command"),
           "request should carry a serialized command");
    const std::string command = (*request)["command"].AsString();

    const microide::workspace::ParsedCommandLine parsed =
        microide::workspace::ParseCommandLine(command);
    Expect(parsed.open_quote == '\0' && !parsed.dangling_escape,
           std::string("the serialized command must be well-formed: ") + command);
    bool all_match = parsed.tokens.size() == tokens.size();
    for (std::size_t i = 0; i < tokens.size() && all_match; ++i) {
      all_match = parsed.tokens[i].text == tokens[i];
    }
    Expect(all_match,
           std::string("each argv token must round-trip verbatim through ParseCommandLine: ") +
               command);
  }
}

void TestParseRejectsBadShapes() {
  Expect(!ParseControlSendArgs({}).valid, "no shape should be rejected");
  Expect(!ParseControlSendArgs({"--query", "status", "debug-start"}).valid,
         "command + query should be rejected");
  Expect(!ParseControlSendArgs({"--args", R"({"a":1})"}).valid,
         "--args without --query should be rejected");
  Expect(!ParseControlSendArgs({"--pid", "x", "debug-start"}).valid,
         "non-integer --pid should be rejected");
  Expect(!ParseControlSendArgs({"--pid", "1", "--socket", "/tmp/x", "debug-start"}).valid,
         "--pid and --socket together should be rejected");
}

#if defined(__unix__) || defined(__APPLE__)

using microide::platform::ControlInboundMessage;
using microide::platform::ControlSocketServer;
using namespace std::chrono_literals;

// A background host driver: replies to each inbound request with caller-supplied
// lines (keyed off the request id), so RunControlSend can run against a real
// socket. Stops when the flag is cleared.
struct ScopedDriver {
  ControlSocketServer& server;
  std::atomic<bool> stop{false};
  std::thread thread;

  ScopedDriver(ControlSocketServer& s, std::function<std::vector<std::string>(int)> responder)
      : server(s) {
    thread = std::thread([this, responder = std::move(responder)]() {
      while (!stop.load()) {
        for (ControlInboundMessage& message : server.TakeInbound()) {
          int id = 0;
          if (const std::optional<util::JsonValue> parsed = util::ParseJson(message.line);
              parsed && parsed->IsObject()) {
            id = static_cast<int>((*parsed)["id"].AsInt());
          }
          for (const std::string& line : responder(id)) {
            server.SendLine(message.connection_id, line);
          }
        }
        std::this_thread::sleep_for(2ms);
      }
    });
  }
  ~ScopedDriver() {
    stop.store(true);
    if (thread.joinable()) thread.join();
  }
};

// Run RunControlSend with a captured stdout/stderr and an argv built from tokens.
int RunCaptured(const std::vector<std::string>& tokens, std::string* out) {
  std::vector<std::string> argv_storage = {"microide", "control-send"};
  argv_storage.insert(argv_storage.end(), tokens.begin(), tokens.end());
  std::vector<char*> argv;
  for (std::string& s : argv_storage) {
    argv.push_back(s.data());
  }

  std::ostringstream captured;
  std::streambuf* old_out = std::cout.rdbuf(captured.rdbuf());
  std::ostringstream captured_err;
  std::streambuf* old_err = std::cerr.rdbuf(captured_err.rdbuf());
  const int code =
      microide::workspace::RunControlSend(static_cast<int>(argv.size()), argv.data());
  std::cout.rdbuf(old_out);
  std::cerr.rdbuf(old_err);
  if (out) {
    *out = captured.str();
  }
  return code;
}

void TestRunControlSendQuerySucceeds() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path socket_path = temp_dir.path() / "control.sock";
  ControlSocketServer server;
  Expect(server.Start(socket_path), "server should start");
  ScopedDriver driver(server, [](int id) {
    return std::vector<std::string>{
        R"({"id":)" + std::to_string(id) + R"(,"ok":true,"result":{"x":1}})"};
  });

  std::string out;
  const int code = RunCaptured({"--socket", socket_path.string(), "--query", "status"}, &out);
  Expect(code == 0, "an ok reply should exit 0");
  Expect(out.find("\"result\"") != std::string::npos, "the reply line should reach stdout");
}

void TestRunControlSendFailureExitsOne() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path socket_path = temp_dir.path() / "control.sock";
  ControlSocketServer server;
  Expect(server.Start(socket_path), "server should start");
  ScopedDriver driver(server, [](int id) {
    return std::vector<std::string>{
        R"({"id":)" + std::to_string(id) + R"(,"ok":false,"error":"nope"})"};
  });

  std::string out;
  const int code = RunCaptured({"--socket", socket_path.string(), "debug-start"}, &out);
  Expect(code == 1, "an ok:false reply should exit 1");
}

void TestRunControlSendWaitsForResolvedStop() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path socket_path = temp_dir.path() / "control.sock";
  ControlSocketServer server;
  Expect(server.Start(socket_path), "server should start");
  // Reply, then the two-phase stop: framesPending:true first, resolved second.
  ScopedDriver driver(server, [](int id) {
    return std::vector<std::string>{
        R"({"id":)" + std::to_string(id) + R"(,"ok":true})",
        R"({"event":"stopped","framesPending":true,"threadId":1})",
        R"({"event":"stopped","framesPending":false,"file":"main.cpp","line":8})"};
  });

  std::string out;
  const int code = RunCaptured(
      {"--socket", socket_path.string(), "--wait", "stopped", "--timeout", "3", "debug-start"},
      &out);
  Expect(code == 0, "the resolved stop should satisfy --wait and exit 0");
  Expect(out.find("\"line\":8") != std::string::npos, "the resolved stop should reach stdout");
}

void TestRunControlSendWaitTimesOut() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path socket_path = temp_dir.path() / "control.sock";
  ControlSocketServer server;
  Expect(server.Start(socket_path), "server should start");
  // Reply ok, but only ever emit the first phase — --wait must time out (exit 3).
  ScopedDriver driver(server, [](int id) {
    return std::vector<std::string>{
        R"({"id":)" + std::to_string(id) + R"(,"ok":true})",
        R"({"event":"stopped","framesPending":true,"threadId":1})"};
  });

  std::string out;
  const int code = RunCaptured(
      {"--socket", socket_path.string(), "--wait", "stopped", "--timeout", "1", "debug-start"},
      &out);
  Expect(code == 3, "an unfulfilled --wait should exit 3");
}

void TestRunControlSendConnectFailureExitsTwo() {
  std::string out;
  const int code = RunCaptured({"--socket", "/nonexistent/microide.sock", "--query", "status"}, &out);
  Expect(code == 2, "a failed connection should exit 2");
}

// A raw AF_UNIX peer that accepts the connection but never reads from it. Used to
// prove SendLine honors its write deadline instead of blocking forever once the
// socket buffers fill. It never recv()s, so the kernel send buffer saturates.
struct SilentPeer {
  int listen_fd = -1;
  int conn_fd = -1;
  std::thread accept_thread;

  bool Start(const std::filesystem::path& path) {
    listen_fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (listen_fd < 0) {
      return false;
    }
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    const std::string path_string = path.string();
    if (path_string.size() + 1 > sizeof(address.sun_path)) {
      return false;
    }
    std::memcpy(address.sun_path, path_string.c_str(), path_string.size() + 1);
    if (::bind(listen_fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
      return false;
    }
    if (::listen(listen_fd, 1) != 0) {
      return false;
    }
    // Accept once on a background thread, then hold the connection without ever
    // reading a byte from it.
    accept_thread = std::thread([this]() { conn_fd = ::accept(listen_fd, nullptr, nullptr); });
    return true;
  }

  ~SilentPeer() {
    if (accept_thread.joinable()) {
      accept_thread.join();
    }
    if (conn_fd >= 0) {
      ::close(conn_fd);
    }
    if (listen_fd >= 0) {
      ::close(listen_fd);
    }
  }
};

void TestSendLineTimesOutWhenPeerNeverReads() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path socket_path = temp_dir.path() / "silent.sock";
  SilentPeer peer;
  Expect(peer.Start(socket_path), "the silent peer should bind and listen");

  platform::ControlSocketClient client;
  Expect(client.Connect(socket_path), "connecting to the silent peer should succeed");

  // A payload comfortably larger than any socket buffer, with a short deadline: the
  // peer never drains, so the kernel send buffer fills and SendLine must give up at
  // the deadline instead of blocking indefinitely.
  const std::string big_line(8u << 20, 'x');  // 8 MiB, under the 16 MiB cap
  const auto start = std::chrono::steady_clock::now();
  const bool sent = client.SendLine(big_line, std::chrono::milliseconds(400));
  const auto elapsed = std::chrono::steady_clock::now() - start;

  Expect(!sent, "SendLine to a non-draining peer must fail at the deadline, not hang");
  Expect(elapsed < std::chrono::seconds(5),
         "SendLine must return promptly around its deadline rather than blocking forever");
}

void TestSendLineSucceedsForShortRequest() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path socket_path = temp_dir.path() / "control.sock";
  ControlSocketServer server;
  Expect(server.Start(socket_path), "server should start");

  platform::ControlSocketClient client;
  Expect(client.Connect(socket_path), "connecting should succeed");
  Expect(client.SendLine(R"({"id":1,"query":"status"})", std::chrono::seconds(2)),
         "a normal short request must still send successfully with a deadline");
}

void TestSendLineRejectsOversizeLine() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path socket_path = temp_dir.path() / "control.sock";
  ControlSocketServer server;
  Expect(server.Start(socket_path), "server should start");

  platform::ControlSocketClient client;
  Expect(client.Connect(socket_path), "connecting should succeed");
  const std::string oversize(17u << 20, 'x');  // 17 MiB, above the 16 MiB cap
  Expect(!client.SendLine(oversize, std::chrono::seconds(2)),
         "an over-cap outbound line must be rejected before framing");
}

#endif  // POSIX

}  // namespace

void RegisterControlClientTests(std::vector<TestCase>& tests) {
  AddTest(tests, "ControlClient/ParseRawCommand", TestParseRawCommand);
  AddTest(tests, "ControlClient/ParseCommandKeepsTrailingFlags", TestParseCommandKeepsTrailingFlags);
  AddTest(tests, "ControlClient/ParseConsumesControlFlagsAnywhere",
          TestParseConsumesControlFlagsAnywhere);
  AddTest(tests, "ControlClient/ParseQuery", TestParseQuery);
  AddTest(tests, "ControlClient/ParseQueryWithArgs", TestParseQueryWithArgs);
  AddTest(tests, "ControlClient/ParseJsonPassthroughInjectsId", TestParseJsonPassthroughInjectsId);
  AddTest(tests, "ControlClient/ParseJsonPreservesOwnId", TestParseJsonPreservesOwnId);
  AddTest(tests, "ControlClient/ParseRejectsBadShapes", TestParseRejectsBadShapes);
  AddTest(tests, "ControlClient/CommandTokensRoundTripThroughParser",
          TestControlSendCommandTokensRoundTripThroughParser);
#if defined(__unix__) || defined(__APPLE__)
  AddTest(tests, "ControlClient/RunControlSendQuerySucceeds", TestRunControlSendQuerySucceeds);
  AddTest(tests, "ControlClient/RunControlSendFailureExitsOne", TestRunControlSendFailureExitsOne);
  AddTest(tests, "ControlClient/RunControlSendWaitsForResolvedStop",
          TestRunControlSendWaitsForResolvedStop);
  AddTest(tests, "ControlClient/RunControlSendWaitTimesOut", TestRunControlSendWaitTimesOut);
  AddTest(tests, "ControlClient/RunControlSendConnectFailureExitsTwo",
          TestRunControlSendConnectFailureExitsTwo);
  AddTest(tests, "ControlClient/SendLineTimesOutWhenPeerNeverReads",
          TestSendLineTimesOutWhenPeerNeverReads);
  AddTest(tests, "ControlClient/SendLineSucceedsForShortRequest",
          TestSendLineSucceedsForShortRequest);
  AddTest(tests, "ControlClient/SendLineRejectsOversizeLine",
          TestSendLineRejectsOversizeLine);
#endif
}

}  // namespace microide::tests
