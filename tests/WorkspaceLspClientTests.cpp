#include "TestSupport.h"

#include "util/JsonValue.h"
#include "workspace/FileUri.h"
#include "workspace/LspMessageFraming.h"
#include "workspace/WorkspaceLspClient.h"
#include "workspace/WorkspaceLspManager.h"

#include <chrono>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace microide::tests {
namespace {

using microide::workspace::LspClient;

bool WaitForLspReadinessState(LspClient& client,
                              LspClient::ReadinessSnapshot::State state,
                              int timeout_ms = 1000) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (std::chrono::steady_clock::now() < deadline) {
    if (client.GetReadinessSnapshot().state == state) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return client.GetReadinessSnapshot().state == state;
}

void TestWorkspaceLspClientShutdownDoesNotRaceInitialization() {
#if !defined(__unix__) && !defined(__APPLE__)
  return;
#endif

  for (int iteration = 0; iteration < 200; ++iteration) {
    LspClient client;
    const bool started =
        client.Start({"/bin/sh", "-c", "sleep 0.01"}, "file:///tmp", "sh");
    Expect(started, "lsp lifecycle stress fixture should start");
    client.Shutdown();
  }
}

void TestWorkspaceLspClientShutdownWaitsForGracefulExit() {
#if !defined(__unix__) && !defined(__APPLE__)
  return;
#endif

  TemporaryDirectory temp_dir;
  const auto marker_path = temp_dir.path() / "graceful-exit.txt";
  const auto server_path = temp_dir.path() / "server.py";
  WriteFile(
      server_path,
      std::string(R"py(import json
import pathlib
import sys
import time

marker_path = pathlib.Path(sys.argv[1])

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
    data = json.dumps(message).encode("utf-8")
    sys.stdout.buffer.write(f"Content-Length: {len(data)}\r\n\r\n".encode("ascii"))
    sys.stdout.buffer.write(data)
    sys.stdout.buffer.flush()

while True:
    msg = read_message()
    if msg is None:
        break
    method = msg.get("method")
    if method == "initialize":
        write_message({
            "jsonrpc": "2.0",
            "id": msg["id"],
            "result": {"capabilities": {"textDocumentSync": 1}},
        })
    elif method == "shutdown":
        write_message({"jsonrpc": "2.0", "id": msg["id"], "result": None})
        time.sleep(0.15)
    elif method == "exit":
        marker_path.write_text("graceful\n", encoding="utf-8")
        break
)py"));

  LspClient client;
  const bool started = client.Start({"python3", server_path.string(), marker_path.string()},
                                    "file:///tmp", "python");
  Expect(started, "graceful shutdown fixture should start");
  for (int attempt = 0; attempt < 50 && !client.IsInitialized(); ++attempt) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  Expect(client.IsInitialized(), "graceful shutdown fixture should initialize before shutdown");
  client.Shutdown();

  for (int attempt = 0; attempt < 20 && !std::filesystem::exists(marker_path); ++attempt) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  Expect(std::filesystem::exists(marker_path),
         "lsp shutdown should allow the server to process exit before forcing termination");
}

void TestWorkspaceLspClientBeginShutdownDoesNotBlockGracefulExit() {
#if !defined(__unix__) && !defined(__APPLE__)
  return;
#endif

  TemporaryDirectory temp_dir;
  const auto marker_path = temp_dir.path() / "async-graceful-exit.txt";
  const auto server_path = temp_dir.path() / "server.py";
  WriteFile(
      server_path,
      std::string(R"py(import json
import pathlib
import sys
import time

marker_path = pathlib.Path(sys.argv[1])

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
    data = json.dumps(message).encode("utf-8")
    sys.stdout.buffer.write(f"Content-Length: {len(data)}\r\n\r\n".encode("ascii"))
    sys.stdout.buffer.write(data)
    sys.stdout.buffer.flush()

while True:
    msg = read_message()
    if msg is None:
        break
    method = msg.get("method")
    if method == "initialize":
        write_message({
            "jsonrpc": "2.0",
            "id": msg["id"],
            "result": {"capabilities": {"textDocumentSync": 1}},
        })
    elif method == "shutdown":
        write_message({"jsonrpc": "2.0", "id": msg["id"], "result": None})
        time.sleep(0.35)
    elif method == "exit":
        marker_path.write_text("graceful\n", encoding="utf-8")
        break
)py"));

  LspClient client;
  const bool started = client.Start({"python3", server_path.string(), marker_path.string()},
                                    "file:///tmp", "python");
  Expect(started, "async graceful shutdown fixture should start");
  for (int attempt = 0; attempt < 50 && !client.IsInitialized(); ++attempt) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  Expect(client.IsInitialized(), "async graceful shutdown fixture should initialize before shutdown");

  const auto start = std::chrono::steady_clock::now();
  client.BeginShutdown();
  const auto elapsed =
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
  Expect(elapsed.count() < 100, "begin shutdown should not block on graceful server exit");

  client.Shutdown();
  Expect(std::filesystem::exists(marker_path),
         "async begin shutdown should still allow the server to process exit");
}

void TestWorkspaceLspClientShutdownClosesStdinAfterExitNotification() {
#if !defined(__unix__) && !defined(__APPLE__)
  return;
#endif

  TemporaryDirectory temp_dir;
  const auto marker_path = temp_dir.path() / "stdin-eof-exit.txt";
  const auto server_path = temp_dir.path() / "server.py";
  WriteFile(
      server_path,
      std::string(R"py(import json
import pathlib
import sys

marker_path = pathlib.Path(sys.argv[1])
shutdown_seen = False

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
    data = json.dumps(message).encode("utf-8")
    sys.stdout.buffer.write(f"Content-Length: {len(data)}\r\n\r\n".encode("ascii"))
    sys.stdout.buffer.write(data)
    sys.stdout.buffer.flush()

while True:
    msg = read_message()
    if msg is None:
        if shutdown_seen:
            marker_path.write_text("eof\n", encoding="utf-8")
        break
    method = msg.get("method")
    if method == "initialize":
        write_message({
            "jsonrpc": "2.0",
            "id": msg["id"],
            "result": {"capabilities": {"textDocumentSync": 1}},
        })
    elif method == "shutdown":
        shutdown_seen = True
        write_message({"jsonrpc": "2.0", "id": msg["id"], "result": None})
    elif method == "exit":
        continue
)py"));

  LspClient client;
  const bool started = client.Start({"python3", server_path.string(), marker_path.string()},
                                    "file:///tmp", "python");
  Expect(started, "stdin close fixture should start");
  for (int attempt = 0; attempt < 50 && !client.IsInitialized(); ++attempt) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  Expect(client.IsInitialized(), "stdin close fixture should initialize before shutdown");
  client.Shutdown();

  Expect(std::filesystem::exists(marker_path),
         "lsp shutdown should close stdin after exit so servers can finish on EOF");
}

void TestWorkspaceLspClientBeginShutdownCancelsPreInitServerImmediately() {
#if !defined(__unix__) && !defined(__APPLE__)
  return;
#endif

  TemporaryDirectory temp_dir;
  const auto marker_path = temp_dir.path() / "preinit-cancel.txt";
  const auto server_path = temp_dir.path() / "server.py";
  WriteFile(
      server_path,
      std::string(R"py(import pathlib
import sys
import time

marker_path = pathlib.Path(sys.argv[1])
marker_path.write_text("started\n", encoding="utf-8")
time.sleep(10)
)py"));

  LspClient client;
  const bool started = client.Start({"python3", server_path.string(), marker_path.string()},
                                    "file:///tmp", "python");
  Expect(started, "preinit cancel fixture should start");
  for (int attempt = 0; attempt < 20 && !std::filesystem::exists(marker_path); ++attempt) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  Expect(std::filesystem::exists(marker_path), "preinit cancel fixture should launch");

  const auto begin = std::chrono::steady_clock::now();
  client.BeginShutdown();
  const auto elapsed =
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - begin);
  Expect(elapsed.count() < 100, "begin shutdown should not block for pre-init servers");

  client.Shutdown();
  Expect(client.IsShutdownComplete(), "pre-init server shutdown should complete after explicit wait");
}

void TestWorkspaceLspClientReadinessSnapshotTracksProgress() {
#if !defined(__unix__) && !defined(__APPLE__)
  return;
#endif

  TemporaryDirectory temp_dir;
  const auto server_path = temp_dir.path() / "server.py";
  WriteFile(
      server_path,
      std::string(R"py(import json
import sys
import time

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
    data = json.dumps(message).encode("utf-8")
    sys.stdout.buffer.write(f"Content-Length: {len(data)}\r\n\r\n".encode("ascii"))
    sys.stdout.buffer.write(data)
    sys.stdout.buffer.flush()

while True:
    msg = read_message()
    if msg is None:
        break
    method = msg.get("method")
    if method == "initialize":
        time.sleep(0.2)
        write_message({
            "jsonrpc": "2.0",
            "id": msg["id"],
            "result": {"capabilities": {"textDocumentSync": 1}},
        })
    elif method == "initialized":
        write_message({
            "jsonrpc": "2.0",
            "method": "$/progress",
            "params": {
                "token": "workspace-index",
                "value": {"kind": "begin", "title": "Indexing", "message": "Indexed 42 files"},
            },
        })
        time.sleep(0.1)
        write_message({
            "jsonrpc": "2.0",
            "method": "$/progress",
            "params": {
                "token": "workspace-index",
                "value": {"kind": "end", "message": "Done"},
            },
        })
    elif method == "shutdown":
        write_message({"jsonrpc": "2.0", "id": msg["id"], "result": None})
    elif method == "exit":
        break
)py"));

  LspClient client;
  const bool started = client.Start({"python3", server_path.string()}, "file:///tmp", "python");
  Expect(started, "readiness snapshot fixture should start");
  Expect(WaitForLspReadinessState(client, LspClient::ReadinessSnapshot::State::Starting, 200),
         "readiness snapshot should report starting before initialize completes");
  Expect(WaitForLspReadinessState(client, LspClient::ReadinessSnapshot::State::Indexing, 1000),
         "readiness snapshot should report indexing during work-done progress");

  const auto indexing_snapshot = client.GetReadinessSnapshot();
  Expect(indexing_snapshot.indexed_count == 42,
         "readiness snapshot should parse the indexed count from progress messages");

  Expect(WaitForLspReadinessState(client, LspClient::ReadinessSnapshot::State::Ready, 1000),
         "readiness snapshot should return to ready after progress ends");
  Expect(client.GetReadinessSnapshot().message == "Ready",
         "ready readiness snapshot should use the canonical ready message");
  client.Shutdown();
}

void TestWorkspaceLspClientDidOpenQueuedBeforeInitializeStillDeliversFullText() {
#if !defined(__unix__) && !defined(__APPLE__)
  return;
#endif

  TemporaryDirectory temp_dir;
  const auto marker_path = temp_dir.path() / "did-open.txt";
  const auto server_path = temp_dir.path() / "server.py";
  WriteFile(
      server_path,
      std::string(R"py(import json
import pathlib
import sys
import time

marker_path = pathlib.Path(sys.argv[1])

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
    data = json.dumps(message).encode("utf-8")
    sys.stdout.buffer.write(f"Content-Length: {len(data)}\r\n\r\n".encode("ascii"))
    sys.stdout.buffer.write(data)
    sys.stdout.buffer.flush()

while True:
    msg = read_message()
    if msg is None:
        break
    method = msg.get("method")
    if method == "initialize":
        time.sleep(0.2)
        write_message({
            "jsonrpc": "2.0",
            "id": msg["id"],
            "result": {"capabilities": {"textDocumentSync": 1}},
        })
    elif method == "textDocument/didOpen":
        text = msg["params"]["textDocument"]["text"]
        marker_path.write_text(str(len(text)) + "\n" + text[:32], encoding="utf-8")
    elif method == "shutdown":
        write_message({"jsonrpc": "2.0", "id": msg["id"], "result": None})
    elif method == "exit":
        break
)py"));

  std::string full_text;
  for (int i = 0; i < 4000; ++i) {
    full_text += "line " + std::to_string(i) + " abcdefghijklmnopqrstuvwxyz\n";
  }

  LspClient client;
  const bool started = client.Start({"python3", server_path.string(), marker_path.string()},
                                    "file:///tmp", "python");
  Expect(started, "didOpen queue fixture should start");
  Expect(client.DidOpen("file:///tmp/sample.py", "python", full_text),
         "didOpen should enqueue even before initialize finishes");

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (std::chrono::steady_clock::now() < deadline) {
    if (std::filesystem::exists(marker_path)) {
      break;
    }
    if (!client.IsRunning()) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  Expect(std::filesystem::exists(marker_path),
         "queued didOpen should reach the server after initialize completes");
  const std::string written = ReadFile(marker_path);
  const std::string prefix = std::to_string(full_text.size()) + "\n" + full_text.substr(0, 32);
  Expect(written == prefix, "queued didOpen should preserve the full text payload");
  client.Shutdown();
}

void TestWorkspaceLspClientAnswersServerRequestsAndAdvertisesEnablers() {
#if !defined(__unix__) && !defined(__APPLE__)
  return;
#endif

  TemporaryDirectory temp_dir;
  const auto server_path = temp_dir.path() / "server.py";
  const auto init_marker = temp_dir.path() / "init.json";
  const auto config_marker = temp_dir.path() / "config.json";
  const auto error_marker = temp_dir.path() / "error.json";

  WriteFile(server_path, std::string(R"py(import json
import pathlib
import sys

init_marker = pathlib.Path(sys.argv[1])
config_marker = pathlib.Path(sys.argv[2])
error_marker = pathlib.Path(sys.argv[3])

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
    data = json.dumps(message).encode("utf-8")
    sys.stdout.buffer.write(f"Content-Length: {len(data)}\r\n\r\n".encode("ascii"))
    sys.stdout.buffer.write(data)
    sys.stdout.buffer.flush()

while True:
    msg = read_message()
    if msg is None:
        break
    method = msg.get("method")
    if method == "initialize":
        init_marker.write_text(json.dumps(msg.get("params", {})), encoding="utf-8")
        write_message({"jsonrpc": "2.0", "id": msg["id"],
                       "result": {"capabilities": {"textDocumentSync": 1}}})
        # Server -> client requests: the client must reply to both.
        write_message({"jsonrpc": "2.0", "id": 1000, "method": "workspace/configuration",
                       "params": {"items": [{"section": "clangd"}]}})
        write_message({"jsonrpc": "2.0", "id": 1001, "method": "some/unknownRequest",
                       "params": {}})
    elif method is None and msg.get("id") == 1000:
        config_marker.write_text(json.dumps(msg), encoding="utf-8")
    elif method is None and msg.get("id") == 1001:
        error_marker.write_text(json.dumps(msg), encoding="utf-8")
    elif method == "shutdown":
        write_message({"jsonrpc": "2.0", "id": msg["id"], "result": None})
    elif method == "exit":
        break
)py"));

  std::optional<util::JsonValue> init_options =
      util::ParseJson(R"({"clangd":{"arguments":["--background-index"]}})");
  std::optional<util::JsonValue> settings =
      util::ParseJson(R"({"clangd":{"fallbackFlags":["-std=c++20"]}})");
  Expect(init_options.has_value() && settings.has_value(), "fixture JSON should parse");

  LspClient client;
  const bool started = client.Start(
      {"python3", server_path.string(), init_marker.string(), config_marker.string(),
       error_marker.string()},
      "file:///tmp", "cpp", {}, *init_options, *settings);
  Expect(started, "server-request fixture should start");

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (std::chrono::steady_clock::now() < deadline) {
    if (std::filesystem::exists(init_marker) && std::filesystem::exists(config_marker) &&
        std::filesystem::exists(error_marker)) {
      break;
    }
    if (!client.IsRunning()) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  Expect(std::filesystem::exists(init_marker), "initialize params should be captured");
  const std::string init_text = ReadFile(init_marker);
  Expect(init_text.find("snippetSupport") != std::string::npos,
         "initialize should advertise completion snippetSupport");
  Expect(init_text.find("--background-index") != std::string::npos,
         "initialize should forward plugin initializationOptions");
  Expect(init_text.find("workspaceFolders") != std::string::npos,
         "initialize should send workspaceFolders");
  Expect(init_text.find("positionEncodings") != std::string::npos &&
             init_text.find("utf-8") != std::string::npos,
         "initialize should advertise the utf-8 position encoding");

  Expect(std::filesystem::exists(config_marker), "client should reply to workspace/configuration");
  const std::string config_text = ReadFile(config_marker);
  Expect(config_text.find("fallbackFlags") != std::string::npos &&
             config_text.find("-std=c++20") != std::string::npos,
         "configuration reply should return the configured settings for the requested section");

  Expect(std::filesystem::exists(error_marker), "client should reply to an unknown server request");
  const std::string error_text = ReadFile(error_marker);
  Expect(error_text.find("-32601") != std::string::npos,
         "unknown server request should get a MethodNotFound error reply");

  Expect(client.ServerPositionEncoding() == "utf-16",
         "a server that reports no positionEncoding defaults to utf-16 per the LSP spec");

  client.Shutdown();
}

// The client advertises utf-8 first; a server that honors it reports
// positionEncoding "utf-8", which the client must capture so the host knows its
// byte-offset columns are already exact LSP positions (no conversion needed).
void TestWorkspaceLspClientCapturesNegotiatedPositionEncoding() {
#if !defined(__unix__) && !defined(__APPLE__)
  return;
#endif
  TemporaryDirectory temp_dir;
  const auto server_path = temp_dir.path() / "server.py";
  WriteFile(server_path, std::string(R"py(import json
import sys

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
    data = json.dumps(message).encode("utf-8")
    sys.stdout.buffer.write(f"Content-Length: {len(data)}\r\n\r\n".encode("ascii"))
    sys.stdout.buffer.write(data)
    sys.stdout.buffer.flush()

while True:
    msg = read_message()
    if msg is None:
        break
    method = msg.get("method")
    if method == "initialize":
        write_message({"jsonrpc": "2.0", "id": msg["id"],
                       "result": {"capabilities": {"textDocumentSync": 1,
                                                   "positionEncoding": "utf-8"}}})
    elif method == "shutdown":
        write_message({"jsonrpc": "2.0", "id": msg["id"], "result": None})
    elif method == "exit":
        break
)py"));

  LspClient client;
  const bool started = client.Start({"python3", server_path.string()}, "file:///tmp", "cpp");
  Expect(started, "utf-8 negotiation fixture should start");

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (std::chrono::steady_clock::now() < deadline && !client.IsInitialized() &&
         client.IsRunning()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  Expect(client.IsInitialized(), "utf-8 negotiation fixture should initialize");
  Expect(client.ServerPositionEncoding() == "utf-8",
         "the client must capture the server's negotiated utf-8 position encoding");

  client.Shutdown();
}

void TestLspManagerSharesOneSubprocessAcrossLanguageIds() {
  microide::workspace::LspManager manager;

  // One fake client installed for three language ids: clangd-style aliasing.
  auto client = std::make_unique<LspClient>();
  LspClient* const raw = client.get();
  manager.InstallTestClientForTesting({"c", "c++", "objective-c"}, std::move(client));

  Expect(manager.HasServer("c") && manager.HasServer("c++") && manager.HasServer("objective-c"),
         "every aliased language id should report a registered server");
  Expect(!manager.HasServer("csharp"), "unrelated language ids should not resolve");

  Expect(manager.GetServer("c") == raw && manager.GetServer("c++") == raw &&
             manager.GetServer("objective-c") == raw,
         "all aliased language ids should resolve to the same single client");

  // BeginShutdownServersNotIn keeps the shared server when ANY of its ids is active.
  manager.BeginShutdownServersNotIn({"c++"});
  Expect(manager.HasServer("c") && manager.HasServer("c++") && manager.HasServer("objective-c"),
         "a shared server stays alive while any of its language ids is active");

  manager.BeginShutdownServersNotIn({"csharp"});
  Expect(!manager.HasServer("c") && !manager.HasServer("c++") && !manager.HasServer("objective-c"),
         "a shared server is retired once none of its language ids is active");
}

void TestWorkspaceLspClientSemanticTokensStubRoundTrip() {
  LspClient client;
  client.EnableTestStubMode();
  client.SetTestSemanticTokenLegend({"variable", "type", "keyword"});
  client.SetTestSemanticTokensHandler(
      [](std::string uri, LspClient::SemanticTokensCallback cb) {
        (void)uri;
        cb(std::vector<LspClient::SemanticToken>{
            LspClient::SemanticToken{.line = 3, .start_char = 2, .length = 5, .token_type = 1}});
      });

  Expect(client.SupportsSemanticTokens(), "stub legend marks the server as semantic-capable");
  Expect(client.SemanticTokenLegend().size() == 3, "the stub legend is reported back");

  std::optional<std::vector<LspClient::SemanticToken>> received;
  client.RequestSemanticTokensAsync("file:///s.cpp", [&](auto tokens) { received = std::move(tokens); });
  client.DrainCallbacks();  // stub responses dispatch on the main-thread pump

  Expect(received.has_value() && received->size() == 1, "the stubbed token is delivered");
  Expect((*received)[0].line == 3 && (*received)[0].token_type == 1,
         "the delivered token preserves its fields");
}

// Regression: formatting must deliver the FULL TextEdit[] to the caller. A prior
// implementation returned only edits.front().newText, silently dropping every edit
// after the first — corrupting the buffer for the common whole-document reformat
// that comes back as many edits.
void TestWorkspaceLspClientFormattingReturnsAllEdits() {
  LspClient client;
  client.EnableTestStubMode();
  client.SetTestFormattingHandler([](std::string uri, LspClient::FormattingCallback cb) {
    (void)uri;
    cb(std::vector<LspClient::TextEdit>{
        {LspClient::Range{{0, 0}, {0, 3}}, "one"},
        {LspClient::Range{{2, 1}, {2, 4}}, "two"},
        {LspClient::Range{{5, 0}, {5, 0}}, "three"}});
  });

  std::optional<std::vector<LspClient::TextEdit>> received;
  client.RequestFormattingAsync("file:///s.cpp", 4, true, [&](auto edits) { received = std::move(edits); });
  client.DrainCallbacks();

  Expect(received.has_value() && received->size() == 3,
         "formatting must return every TextEdit, not just the first");
  Expect((*received)[1].second == "two" && (*received)[2].second == "three",
         "the trailing edits must survive with their replacement text");
  Expect((*received)[0].first.start.line == 0 && (*received)[1].first.start.line == 2,
         "each edit keeps its own range");
}

// didOpen/didSave/didClose must all address a document under the SAME URI. That
// URI is percent-encoded (FileUriForPath), so a path with a space or non-ASCII
// byte must encode consistently and round-trip back — otherwise a hand-built raw
// "file://" + path used on the save/close side would silently mismatch the open
// URI and leak the document server-side.
void TestFileUriEncodesSpecialCharsAndRoundTrips() {
  const std::filesystem::path spaced = "/home/user/My Project/main file.cpp";
  const std::string uri = microide::workspace::FileUriForPath(spaced);
  Expect(uri.find("file://") == 0, "encoded URI keeps the file scheme");
  Expect(uri.find(' ') == std::string::npos, "spaces must be percent-encoded, not literal");
  Expect(uri.find("%20") != std::string::npos, "a space encodes as %20");
  const auto decoded = microide::workspace::PathFromFileUri(uri);
  Expect(decoded.has_value() && decoded->lexically_normal() == spaced.lexically_normal(),
         "the encoded URI must round-trip back to the original path");

  const std::filesystem::path accented = "/home/user/café/résumé.txt";
  const std::string accented_uri = microide::workspace::FileUriForPath(accented);
  const auto accented_decoded = microide::workspace::PathFromFileUri(accented_uri);
  Expect(accented_decoded.has_value() &&
             accented_decoded->lexically_normal() == accented.lexically_normal(),
         "non-ASCII paths must round-trip through the file URI");
}

}  // namespace

// A single server response whose declared Content-Length exceeds the 64 MiB cap
// must be skipped whole, not tear the session down: the stream has to resync so
// later messages (here a diagnostics push) are still delivered.
void TestWorkspaceLspClientSkipsOversizedFrameAndResyncs() {
#if !defined(__unix__) && !defined(__APPLE__)
  return;
#else
  TemporaryDirectory temp_dir;
  const auto server_path = temp_dir.path() / "server.py";
  WriteFile(
      server_path,
      std::string(R"py(import json
import sys

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
    data = json.dumps(message).encode("utf-8")
    sys.stdout.buffer.write(f"Content-Length: {len(data)}\r\n\r\n".encode("ascii"))
    sys.stdout.buffer.write(data)
    sys.stdout.buffer.flush()

while True:
    msg = read_message()
    if msg is None:
        break
    method = msg.get("method")
    if method == "initialize":
        write_message({
            "jsonrpc": "2.0",
            "id": msg["id"],
            "result": {"capabilities": {"textDocumentSync": 1}},
        })
        # A well-formed frame that is too large to buffer (just over 64 MiB),
        # streamed in full so the client must drain and discard the whole body.
        oversized = 64 * 1024 * 1024 + 1
        sys.stdout.buffer.write(f"Content-Length: {oversized}\r\n\r\n".encode("ascii"))
        sys.stdout.buffer.write(b"x" * oversized)
        sys.stdout.buffer.flush()
        # After the skipped frame the stream must resync and deliver this push.
        write_message({
            "jsonrpc": "2.0",
            "method": "textDocument/publishDiagnostics",
            "params": {
                "uri": "file:///tmp/a.txt",
                "diagnostics": [{
                    "range": {"start": {"line": 0, "character": 0},
                              "end": {"line": 0, "character": 1}},
                    "message": "resynced",
                    "severity": 1,
                }],
            },
        })
    elif method == "shutdown":
        write_message({"jsonrpc": "2.0", "id": msg["id"], "result": None})
    elif method == "exit":
        break
)py"));

  LspClient client;
  bool got_resynced_diag = false;
  client.SetDiagnosticsCallback(
      [&](std::string /*uri*/, std::vector<LspClient::Diagnostic> diags) {
        if (!diags.empty() && diags.front().message == "resynced") {
          got_resynced_diag = true;
        }
      });
  const bool started =
      client.Start({"python3", server_path.string()}, "file:///tmp", "python");
  Expect(started, "oversized-frame fixture should start");
  Expect(WaitForLspReadinessState(client, LspClient::ReadinessSnapshot::State::Ready, 2000) ||
             client.IsInitialized(),
         "oversized-frame fixture should initialize");

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (std::chrono::steady_clock::now() < deadline && !got_resynced_diag) {
    client.DrainCallbacks();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  Expect(got_resynced_diag,
         "client must skip the oversized frame, resync, and deliver the later diagnostic");
  client.Shutdown();
#endif
}

// ---------------------------------------------------------------------------
// LspMessageFramer — direct, subprocess-free unit coverage of the wire codec.
// The parser is a hot path and a hostile-input surface; these exercise its
// cross-chunk state (partial frames, coalesced frames, header EOL variants,
// malformed-length resync, oversized-frame skip) deterministically.
// ---------------------------------------------------------------------------
namespace {

std::string LspFrame(std::string_view body, bool crlf = true) {
  const std::string eol = crlf ? "\r\n" : "\n";
  return "Content-Length: " + std::to_string(body.size()) + eol + eol + std::string(body);
}

}  // namespace

void TestLspMessageFramerSplitFrameAcrossChunks() {
  workspace::LspMessageFramer framer;
  const std::string frame = LspFrame(R"({"jsonrpc":"2.0","method":"a"})");
  const std::size_t split = frame.size() / 2;
  framer.Append(std::string_view(frame).substr(0, split));
  Expect(!framer.Next().has_value(), "a partial frame yields no message");
  framer.Append(std::string_view(frame).substr(split));
  auto msg = framer.Next();
  Expect(msg.has_value(), "the completed frame yields a message");
  Expect(msg->HasKey("method") && (*msg)["method"].AsString() == "a", "method field parses");
  Expect(!framer.Next().has_value(), "no trailing message remains");
}

void TestLspMessageFramerMultipleFramesInOneChunk() {
  workspace::LspMessageFramer framer;
  framer.Append(LspFrame(R"({"method":"one"})") + LspFrame(R"({"method":"two"})"));
  auto a = framer.Next();
  auto b = framer.Next();
  Expect(a.has_value() && (*a)["method"].AsString() == "one", "first coalesced frame parses");
  Expect(b.has_value() && (*b)["method"].AsString() == "two", "second coalesced frame parses");
  Expect(!framer.Next().has_value(), "only two frames were present");
}

void TestLspMessageFramerBareNewlineHeaders() {
  workspace::LspMessageFramer framer;
  framer.Append(LspFrame(R"({"method":"lf"})", /*crlf=*/false));
  auto msg = framer.Next();
  Expect(msg.has_value() && (*msg)["method"].AsString() == "lf",
         "bare-\\n headers (no \\r) still frame a message");
}

void TestLspMessageFramerMalformedLengthResyncs() {
  workspace::LspMessageFramer framer;
  framer.Append("Content-Length: notanumber\r\n\r\n" + LspFrame(R"({"method":"ok"})"));
  std::optional<util::JsonValue> got;
  for (int i = 0; i < 8 && !got.has_value(); ++i) {
    got = framer.Next();
  }
  Expect(got.has_value() && (*got)["method"].AsString() == "ok",
         "a malformed Content-Length header is dropped and the stream resyncs to the next frame");
}

void TestLspMessageFramerOversizedFrameSkips() {
  workspace::LspMessageFramer framer;
  const std::size_t oversized = 64ull * 1024 * 1024 + 1;  // just past kMaxLspMessageBytes
  framer.Append("Content-Length: " + std::to_string(oversized) + "\r\n\r\n");
  Expect(!framer.Next().has_value(), "an oversized header frames no message");
  Expect(framer.skip_body_bytes == oversized, "the oversized body is queued for skipping whole");
  framer.Append(std::string(1000, 'x'));
  Expect(!framer.Next().has_value(), "skipped body bytes never frame a message");
  Expect(framer.skip_body_bytes == oversized - 1000, "the skip counter decrements as body drains");
  Expect(framer.BufferedBytes() == 0, "drained skip bytes leave the buffer empty");
}

void RegisterWorkspaceLspClientTests(std::vector<TestCase>& tests) {
  AddTest(tests, "WorkspaceLspClient/FileUriEncodesSpecialCharsAndRoundTrips",
          TestFileUriEncodesSpecialCharsAndRoundTrips);
  AddTest(tests, "WorkspaceLspClient/SemanticTokensStubRoundTrip",
          TestWorkspaceLspClientSemanticTokensStubRoundTrip);
  AddTest(tests, "WorkspaceLspClient/FormattingReturnsAllEdits",
          TestWorkspaceLspClientFormattingReturnsAllEdits);
  AddTest(tests, "WorkspaceLspClient/ShutdownDoesNotRaceInitialization",
          TestWorkspaceLspClientShutdownDoesNotRaceInitialization);
  AddTest(tests, "WorkspaceLspClient/ShutdownWaitsForGracefulExit",
          TestWorkspaceLspClientShutdownWaitsForGracefulExit);
  AddTest(tests, "WorkspaceLspClient/BeginShutdownDoesNotBlockGracefulExit",
          TestWorkspaceLspClientBeginShutdownDoesNotBlockGracefulExit);
  AddTest(tests, "WorkspaceLspClient/ShutdownClosesStdinAfterExitNotification",
          TestWorkspaceLspClientShutdownClosesStdinAfterExitNotification);
  AddTest(tests, "WorkspaceLspClient/BeginShutdownCancelsPreInitServerImmediately",
          TestWorkspaceLspClientBeginShutdownCancelsPreInitServerImmediately);
  AddTest(tests, "WorkspaceLspClient/ReadinessSnapshotTracksProgress",
          TestWorkspaceLspClientReadinessSnapshotTracksProgress);
  AddTest(tests, "WorkspaceLspClient/DidOpenQueuedBeforeInitializeStillDeliversFullText",
          TestWorkspaceLspClientDidOpenQueuedBeforeInitializeStillDeliversFullText);
  AddTest(tests, "WorkspaceLspClient/CapturesNegotiatedPositionEncoding",
          TestWorkspaceLspClientCapturesNegotiatedPositionEncoding);
  AddTest(tests, "WorkspaceLspClient/AnswersServerRequestsAndAdvertisesEnablers",
          TestWorkspaceLspClientAnswersServerRequestsAndAdvertisesEnablers);
  AddTest(tests, "WorkspaceLspClient/LspManagerSharesOneSubprocessAcrossLanguageIds",
          TestLspManagerSharesOneSubprocessAcrossLanguageIds);
  AddTest(tests, "WorkspaceLspClient/SkipsOversizedFrameAndResyncs",
          TestWorkspaceLspClientSkipsOversizedFrameAndResyncs);
  AddTest(tests, "WorkspaceLspClient/FramerSplitFrameAcrossChunks",
          TestLspMessageFramerSplitFrameAcrossChunks);
  AddTest(tests, "WorkspaceLspClient/FramerMultipleFramesInOneChunk",
          TestLspMessageFramerMultipleFramesInOneChunk);
  AddTest(tests, "WorkspaceLspClient/FramerBareNewlineHeaders",
          TestLspMessageFramerBareNewlineHeaders);
  AddTest(tests, "WorkspaceLspClient/FramerMalformedLengthResyncs",
          TestLspMessageFramerMalformedLengthResyncs);
  AddTest(tests, "WorkspaceLspClient/FramerOversizedFrameSkips",
          TestLspMessageFramerOversizedFrameSkips);
}

}  // namespace microide::tests
