#include "TestSupport.h"

#include "workspace/WorkspaceLspClient.h"

#include <chrono>
#include <thread>
#include <vector>

namespace microide::tests {
namespace {

using microide::workspace::LspClient;

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

}  // namespace

void RegisterWorkspaceLspClientTests(std::vector<TestCase>& tests) {
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
}

}  // namespace microide::tests
