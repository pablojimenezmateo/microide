#include "TestSupport.h"

#include "util/JsonValue.h"
#include "workspace/FileUri.h"
#include "workspace/LspMessageFraming.h"
#include "workspace/WorkspaceLspClient.h"
#include "workspace/WorkspaceLspManager.h"

#include <algorithm>
#include <atomic>
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
        # Write atomically: the test's poll loop breaks on marker existence, so a
        # non-atomic write would let it read an empty/partial file (create-then-fill
        # race). Fill a temp file, then os.replace onto the marker (atomic on the
        # same filesystem) so existence implies the full payload is present.
        tmp_path = marker_path.with_name(marker_path.name + ".tmp")
        tmp_path.write_text(str(len(text)) + "\n" + text[:32], encoding="utf-8")
        tmp_path.replace(marker_path)
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

def write_marker(path, text):
    # The test polls for marker existence, then reads + inspects its content, so a
    # plain write_text (create-then-fill) would let the reader see an empty/partial
    # file. Fill a temp then os.replace onto the marker (atomic on the same
    # filesystem) so existence implies the full payload is present.
    tmp = path.with_name(path.name + ".tmp")
    tmp.write_text(text, encoding="utf-8")
    tmp.replace(path)

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
        write_marker(init_marker, json.dumps(msg.get("params", {})))
        write_message({"jsonrpc": "2.0", "id": msg["id"],
                       "result": {"capabilities": {"textDocumentSync": 1}}})
        # Server -> client requests: the client must reply to both.
        write_message({"jsonrpc": "2.0", "id": 1000, "method": "workspace/configuration",
                       "params": {"items": [{"section": "clangd"}]}})
        write_message({"jsonrpc": "2.0", "id": 1001, "method": "some/unknownRequest",
                       "params": {}})
    elif method is None and msg.get("id") == 1000:
        write_marker(config_marker, json.dumps(msg))
    elif method is None and msg.get("id") == 1001:
        write_marker(error_marker, json.dumps(msg))
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

void TestWorkspaceLspClientInlayHintStubRoundTrip() {
  LspClient client;
  client.EnableTestStubMode();
  // The stub handler marks the capability supported.
  LspClient::Range requested_range{};
  client.SetTestInlayHintHandler(
      [&](std::string uri, LspClient::Range range, LspClient::InlayHintCallback cb) {
        (void)uri;
        requested_range = range;
        cb(std::vector<LspClient::InlayHint>{
            LspClient::InlayHint{.position = LspClient::Position{4, 10},
                                 .label = ": i32",
                                 .kind = 1,
                                 .padding_left = true,
                                 .padding_right = false}});
      });

  Expect(client.SupportsInlayHints(), "the stub handler marks the server inlay-capable");

  std::optional<std::vector<LspClient::InlayHint>> received;
  client.RequestInlayHintsAsync("file:///s.cpp", LspClient::Range{{0, 0}, {20, 0}},
                                [&](auto hints) { received = std::move(hints); });
  client.DrainCallbacks();

  Expect(received.has_value() && received->size() == 1, "the stubbed hint is delivered");
  Expect((*received)[0].label == ": i32" && (*received)[0].position.line == 4,
         "the delivered hint preserves its fields");
  Expect(requested_range.end.line == 20, "the request forwards the whole-document range");

  // Clearing the handler drops the capability advertisement path (still stub mode,
  // so a fresh request with no handler reports nullopt).
  client.ClearTestInlayHintHandler();
  std::optional<std::vector<LspClient::InlayHint>> after_clear;
  bool called = false;
  client.RequestInlayHintsAsync("file:///s.cpp", LspClient::Range{{0, 0}, {1, 0}},
                                [&](auto hints) { after_clear = std::move(hints); called = true; });
  client.DrainCallbacks();
  Expect(called && !after_clear.has_value(), "with no handler the stub reports no hints");
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

// A frame that arrives before the initialize response must be dispatched, not
// dropped. The init loop used to `continue` on any non-init frame — but
// framer_.Next() had already consumed the bytes, so early server pushes
// (window/logMessage, $/progress) were lost and pre-init server requests went
// unanswered (a request-blocking server then hangs). Here the server pushes a
// diagnostics notification *before* the initialize response; the client must
// still deliver it. Mirrors the DAP client's pre-initialize dispatch.
void TestWorkspaceLspClientDispatchesPreInitializeFrames() {
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
        # Push a diagnostics notification BEFORE the initialize response.
        write_message({
            "jsonrpc": "2.0",
            "method": "textDocument/publishDiagnostics",
            "params": {
                "uri": "file:///tmp/pre.txt",
                "diagnostics": [{
                    "range": {"start": {"line": 0, "character": 0},
                              "end": {"line": 0, "character": 1}},
                    "message": "preinit-hello",
                    "severity": 1,
                }],
            },
        })
        write_message({
            "jsonrpc": "2.0",
            "id": msg["id"],
            "result": {"capabilities": {"textDocumentSync": 1}},
        })
    elif method == "shutdown":
        write_message({"jsonrpc": "2.0", "id": msg["id"], "result": None})
    elif method == "exit":
        break
)py"));

  LspClient client;
  bool got_preinit_diag = false;
  client.SetDiagnosticsCallback(
      [&](std::string /*uri*/, std::vector<LspClient::Diagnostic> diags) {
        if (!diags.empty() && diags.front().message == "preinit-hello") {
          got_preinit_diag = true;
        }
      });
  Expect(client.Start({"python3", server_path.string()}, "file:///tmp", "python"),
         "pre-initialize fixture should start");

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (std::chrono::steady_clock::now() < deadline && !got_preinit_diag) {
    client.DrainCallbacks();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  Expect(got_preinit_diag,
         "a frame arriving before the initialize response must be dispatched, not dropped");
  client.Shutdown();
#endif
}

// Diagnostics computed against a document version the client has already
// superseded with a newer edit must be dropped — applying stale ranges to the
// newer buffer paints squiggles on the wrong spans. The server replies to the
// v2 didChange with a stale (v1) push followed by a fresh (v2) push; only the
// fresh one may reach the callback.
void TestWorkspaceLspClientDropsStaleDiagnosticsVersion() {
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

def diag(version, message):
    write_message({
        "jsonrpc": "2.0",
        "method": "textDocument/publishDiagnostics",
        "params": {
            "uri": "file:///tmp/stale.py",
            "version": version,
            "diagnostics": [{
                "range": {"start": {"line": 0, "character": 0},
                          "end": {"line": 0, "character": 1}},
                "message": message,
                "severity": 1,
            }],
        },
    })

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
    elif method == "textDocument/didChange":
        version = msg["params"]["textDocument"]["version"]
        if version == 2:
            diag(1, "stale-v1")   # superseded — must be dropped by the client
            diag(2, "fresh-v2")   # current — must be delivered
    elif method == "shutdown":
        write_message({"jsonrpc": "2.0", "id": msg["id"], "result": None})
    elif method == "exit":
        break
)py"));

  LspClient client;
  std::vector<std::string> received;
  client.SetDiagnosticsCallback(
      [&](std::string /*uri*/, std::vector<LspClient::Diagnostic> diags) {
        if (!diags.empty()) {
          received.push_back(diags.front().message);
        }
      });
  Expect(client.Start({"python3", server_path.string()}, "file:///tmp", "python"),
         "stale-diagnostics fixture should start");
  Expect(WaitForLspReadinessState(client, LspClient::ReadinessSnapshot::State::Ready, 2000) ||
             client.IsInitialized(),
         "stale-diagnostics fixture should initialize");

  const std::string uri = "file:///tmp/stale.py";
  Expect(client.DidOpen(uri, "python", "first"), "didOpen should enqueue (version 1)");
  Expect(client.DidChange(uri, "second"), "didChange should enqueue (version 2)");

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (std::chrono::steady_clock::now() < deadline &&
         std::find(received.begin(), received.end(), "fresh-v2") == received.end()) {
    client.DrainCallbacks();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  // Give any stale push that slipped through a chance to surface before asserting.
  client.DrainCallbacks();
  Expect(std::find(received.begin(), received.end(), "fresh-v2") != received.end(),
         "current-version diagnostics must be delivered");
  Expect(std::find(received.begin(), received.end(), "stale-v1") == received.end(),
         "superseded-version diagnostics must be dropped");
  client.Shutdown();
#endif
}

// Same as above, but the server echoes the document version as a JSON FLOAT
// ("version": 1.0 / 2.0). Some servers round-trip integers through a float; the
// stale-drop gate must accept IsDouble() as well as IsInt() (mirroring the
// response-id gate) or the stale v1 push slips through onto the v2 buffer.
void TestWorkspaceLspClientDropsStaleDiagnosticsFloatVersion() {
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

def diag(version, message):
    write_message({
        "jsonrpc": "2.0",
        "method": "textDocument/publishDiagnostics",
        "params": {
            "uri": "file:///tmp/stalef.py",
            "version": float(version),  # emit the version as a JSON float
            "diagnostics": [{
                "range": {"start": {"line": 0, "character": 0},
                          "end": {"line": 0, "character": 1}},
                "message": message,
                "severity": 1,
            }],
        },
    })

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
    elif method == "textDocument/didChange":
        version = msg["params"]["textDocument"]["version"]
        if version == 2:
            diag(1, "stale-v1")   # superseded — must be dropped even as a float
            diag(2, "fresh-v2")   # current — must be delivered
    elif method == "shutdown":
        write_message({"jsonrpc": "2.0", "id": msg["id"], "result": None})
    elif method == "exit":
        break
)py"));

  LspClient client;
  std::vector<std::string> received;
  client.SetDiagnosticsCallback(
      [&](std::string /*uri*/, std::vector<LspClient::Diagnostic> diags) {
        if (!diags.empty()) {
          received.push_back(diags.front().message);
        }
      });
  Expect(client.Start({"python3", server_path.string()}, "file:///tmp", "python"),
         "float-version stale-diagnostics fixture should start");
  Expect(WaitForLspReadinessState(client, LspClient::ReadinessSnapshot::State::Ready, 2000) ||
             client.IsInitialized(),
         "float-version stale-diagnostics fixture should initialize");

  const std::string uri = "file:///tmp/stalef.py";
  Expect(client.DidOpen(uri, "python", "first"), "didOpen should enqueue (version 1)");
  Expect(client.DidChange(uri, "second"), "didChange should enqueue (version 2)");

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (std::chrono::steady_clock::now() < deadline &&
         std::find(received.begin(), received.end(), "fresh-v2") == received.end()) {
    client.DrainCallbacks();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  client.DrainCallbacks();
  Expect(std::find(received.begin(), received.end(), "fresh-v2") != received.end(),
         "current-version diagnostics must be delivered (float version)");
  Expect(std::find(received.begin(), received.end(), "stale-v1") == received.end(),
         "superseded-version diagnostics must be dropped even when the version is a float");
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

// Regression: an oversized frame whose header block is not yet fully buffered
// (a recv split right on the Content-Length line's newline, before the blank-line
// terminator) must NOT commit the skip yet. Committing it early would count the
// still-unseen "\r\n" terminator bytes as body, drain two bytes short, and desync
// the stream (re-parsing trailing body bytes as a new frame). The framer must wait
// for the terminator, then skip with a body_start that is correctly past it.
void TestLspMessageFramerOversizedFrameSplitOnHeaderNewlineDoesNotDesync() {
  workspace::LspMessageFramer framer;
  const std::size_t oversized = 64ull * 1024 * 1024 + 1;  // just past kMaxLspMessageBytes
  // Feed only the Content-Length line, ending exactly on its '\n' — no blank line.
  framer.Append("Content-Length: " + std::to_string(oversized) + "\r\n");
  Expect(!framer.Next().has_value(), "an incomplete header block frames no message");
  Expect(framer.skip_body_bytes == 0,
         "the skip must NOT be committed before the header terminator is seen");
  Expect(framer.BufferedBytes() > 0,
         "the header line must stay buffered until the block terminates");
  // Now the blank-line terminator arrives; the skip commits past it (not before).
  framer.Append("\r\n");
  Expect(!framer.Next().has_value(), "the oversized frame still frames no message");
  Expect(framer.skip_body_bytes == oversized,
         "the whole body is queued for skipping once the header block terminates");
  Expect(framer.BufferedBytes() == 0,
         "the header block (and only it) is consumed before the body drain begins");
}

// Track A regression: serialization is deferred to the I/O thread (the outbound
// builder runs SerializeMessage lazily). This must preserve FIFO order and the full
// per-change payload — a reorder or a captured-by-reference bug would corrupt sync.
void TestWorkspaceLspClientDidChangePreservesOrderAndPayload() {
#if !defined(__unix__) && !defined(__APPLE__)
  return;
#endif
  TemporaryDirectory temp_dir;
  const auto log_path = temp_dir.path() / "didchange.log";
  const auto server_path = temp_dir.path() / "server.py";
  WriteFile(server_path, std::string(R"py(import json
import pathlib
import sys

log_path = pathlib.Path(sys.argv[1])

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
        write_message({"jsonrpc": "2.0", "id": msg["id"],
                       "result": {"capabilities": {"textDocumentSync": 1}}})
    elif method == "textDocument/didChange":
        version = msg["params"]["textDocument"]["version"]
        text = msg["params"]["contentChanges"][0]["text"]
        with log_path.open("a", encoding="utf-8") as handle:
            handle.write(f"{version}:{len(text)}:{text[:24]}\n")
            handle.flush()
    elif method == "shutdown":
        write_message({"jsonrpc": "2.0", "id": msg["id"], "result": None})
    elif method == "exit":
        break
)py"));

  LspClient client;
  const bool started = client.Start({"python3", server_path.string(), log_path.string()},
                                    "file:///tmp", "python");
  Expect(started, "didChange order fixture should start");

  constexpr int kChanges = 6;
  std::vector<std::string> texts;
  for (int i = 1; i <= kChanges; ++i) {
    texts.push_back("CHANGE_" + std::to_string(i) + "_" + std::string(2000, 'x'));
    Expect(client.DidChange("file:///tmp/s.py", texts.back()), "didChange should enqueue");
  }

  std::string expected;
  for (int i = 0; i < kChanges; ++i) {
    expected += std::to_string(i + 1) + ":" + std::to_string(texts[static_cast<std::size_t>(i)].size()) +
                ":" + texts[static_cast<std::size_t>(i)].substr(0, 24) + "\n";
  }

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  std::string contents;
  while (std::chrono::steady_clock::now() < deadline) {
    if (std::filesystem::exists(log_path)) {
      contents = ReadFile(log_path);
      const auto newlines = static_cast<std::size_t>(std::count(contents.begin(), contents.end(), '\n'));
      if (newlines >= static_cast<std::size_t>(kChanges)) {
        break;
      }
    }
    if (!client.IsRunning()) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }

  // Exact match asserts both FIFO order (versions 1..N in sequence) and full payload
  // integrity through the deferred builder.
  Expect(contents == expected,
         "didChange order + full payload must survive deferred serialization");
  client.Shutdown();
}

// Track A' coverage: drive the real (non-stub) completion parser against a server
// that returns a JSON CompletionList. The stub path bypasses the parser, so this is
// the only place the move-out parsing of label/detail/insertText/textEdit runs.
void TestWorkspaceLspClientCompletionParsesJsonResult() {
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
        write_message({"jsonrpc": "2.0", "id": msg["id"],
                       "result": {"capabilities": {"completionProvider": {}}}})
    elif method == "textDocument/completion":
        write_message({"jsonrpc": "2.0", "id": msg["id"], "result": {"items": [
            {"label": "alpha", "detail": "int", "insertText": "alpha", "kind": 6},
            {"label": "beta", "textEdit": {
                "range": {"start": {"line": 1, "character": 2},
                          "end": {"line": 1, "character": 5}},
                "newText": "beta_x"}},
        ]}})
    elif method == "shutdown":
        write_message({"jsonrpc": "2.0", "id": msg["id"], "result": None})
    elif method == "exit":
        break
)py"));

  LspClient client;
  const bool started =
      client.Start({"python3", server_path.string()}, "file:///tmp", "python");
  Expect(started, "completion fixture should start");

  std::optional<std::vector<LspClient::CompletionItem>> received;
  client.RequestCompletionAsync("file:///tmp/s.py", LspClient::Position{0, 0},
                                [&](auto items) { received = std::move(items); });

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (std::chrono::steady_clock::now() < deadline && !received.has_value()) {
    client.DrainCallbacks();
    if (!client.IsRunning()) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }

  Expect(received.has_value(), "completion result should be delivered");
  Expect(received->size() == 2, "both completion items should parse");
  Expect((*received)[0].label == "alpha" && (*received)[0].insert_text == "alpha" &&
             (*received)[0].detail == "int",
         "plain item fields parse (moved out of the JSON)");
  Expect((*received)[1].label == "beta" && (*received)[1].insert_text == "beta_x",
         "textEdit.newText wins over insertText and is moved out");
  Expect((*received)[1].replace_range.has_value() &&
             (*received)[1].replace_range->start.character == 2 &&
             (*received)[1].replace_range->end.character == 5,
         "textEdit.range becomes the authoritative replace range");
  client.Shutdown();
}

// Regression: a server that echoes the integer request id as a JSON float (e.g.
// "id": 1.0) — common when the id round-trips through a float-typed JSON layer —
// must still correlate to its pending request. The dispatch gate used to accept
// only IsInt(), silently dropping float-id responses and hanging the request.
void TestWorkspaceLspClientAcceptsFloatEchoedResponseId() {
#if !defined(__unix__) && !defined(__APPLE__)
  return;
#endif
  TemporaryDirectory temp_dir;
  const auto server_path = temp_dir.path() / "float-id-server.py";
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
        # Echo the id as a float on the init handshake AND on feature responses, so
        # both the init-response gate and the steady-state dispatch gate are exercised.
        write_message({"jsonrpc": "2.0", "id": float(msg["id"]),
                       "result": {"capabilities": {"completionProvider": {}}}})
    elif method == "textDocument/completion":
        write_message({"jsonrpc": "2.0", "id": float(msg["id"]),
                       "result": {"items": [{"label": "gamma", "insertText": "gamma"}]}})
    elif method == "shutdown":
        write_message({"jsonrpc": "2.0", "id": float(msg["id"]), "result": None})
    elif method == "exit":
        break
)py"));

  LspClient client;
  const bool started =
      client.Start({"python3", server_path.string()}, "file:///tmp", "python");
  Expect(started, "float-id fixture should start");

  std::optional<std::vector<LspClient::CompletionItem>> received;
  client.RequestCompletionAsync("file:///tmp/s.py", LspClient::Position{0, 0},
                                [&](auto items) { received = std::move(items); });

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (std::chrono::steady_clock::now() < deadline && !received.has_value()) {
    client.DrainCallbacks();
    if (!client.IsRunning()) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }

  Expect(received.has_value(),
         "a completion response with a float-echoed id must still be delivered");
  Expect(received.has_value() && received->size() == 1 && (*received)[0].label == "gamma",
         "the float-id response body parses normally");
  client.Shutdown();
}

void TestWorkspaceLspClientInitFailureFailsPendingRequest() {
#if !defined(__unix__) && !defined(__APPLE__)
  return;
#endif
  TemporaryDirectory temp_dir;
  const auto server_path = temp_dir.path() / "stall-server.py";
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

  LspClient client;
  const bool started = client.Start({"python3", server_path.string()}, "file:///tmp", "python");
  Expect(started, "init-failure fixture should start");

  // Issue a feature request while the client is still initializing: it registers a
  // pending request and queues the wire message. When init fails, the request must
  // be failed (callback invoked with no result), not silently dropped -> otherwise
  // its UI loading state strands forever.
  std::atomic<bool> hover_called{false};
  client.RequestHoverAsync("file:///tmp/a.py", LspClient::Position{0, 0},
                           [&hover_called](std::optional<util::JsonValue>) {
                             hover_called.store(true, std::memory_order_release);
                           });

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while (std::chrono::steady_clock::now() < deadline &&
         !hover_called.load(std::memory_order_acquire)) {
    client.DrainCallbacks();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  client.DrainCallbacks();
  Expect(!client.IsInitialized(), "init should have failed (server never responded)");
  Expect(hover_called.load(std::memory_order_acquire),
         "a request registered during a failed init must have its callback failed, not dropped");
  client.Shutdown();
}

void RegisterWorkspaceLspClientTests(std::vector<TestCase>& tests) {
  AddTest(tests, "WorkspaceLspClient/InitFailureFailsPendingRequest",
          TestWorkspaceLspClientInitFailureFailsPendingRequest);
  AddTest(tests, "WorkspaceLspClient/AcceptsFloatEchoedResponseId",
          TestWorkspaceLspClientAcceptsFloatEchoedResponseId);
  AddTest(tests, "WorkspaceLspClient/DidChangePreservesOrderAndPayload",
          TestWorkspaceLspClientDidChangePreservesOrderAndPayload);
  AddTest(tests, "WorkspaceLspClient/CompletionParsesJsonResult",
          TestWorkspaceLspClientCompletionParsesJsonResult);
  AddTest(tests, "WorkspaceLspClient/FileUriEncodesSpecialCharsAndRoundTrips",
          TestFileUriEncodesSpecialCharsAndRoundTrips);
  AddTest(tests, "WorkspaceLspClient/SemanticTokensStubRoundTrip",
          TestWorkspaceLspClientSemanticTokensStubRoundTrip);
  AddTest(tests, "WorkspaceLspClient/InlayHintStubRoundTrip",
          TestWorkspaceLspClientInlayHintStubRoundTrip);
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
  AddTest(tests, "WorkspaceLspClient/DispatchesPreInitializeFrames",
          TestWorkspaceLspClientDispatchesPreInitializeFrames);
  AddTest(tests, "WorkspaceLspClient/DropsStaleDiagnosticsVersion",
          TestWorkspaceLspClientDropsStaleDiagnosticsVersion);
  AddTest(tests, "WorkspaceLspClient/DropsStaleDiagnosticsFloatVersion",
          TestWorkspaceLspClientDropsStaleDiagnosticsFloatVersion);
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
  AddTest(tests, "WorkspaceLspClient/FramerOversizedFrameSplitOnHeaderNewlineDoesNotDesync",
          TestLspMessageFramerOversizedFrameSplitOnHeaderNewlineDoesNotDesync);
}

}  // namespace microide::tests
