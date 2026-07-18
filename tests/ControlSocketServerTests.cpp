#include "TestSupport.h"

#include <chrono>
#include <cstring>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "platform/ControlSocketServer.h"

#if defined(__unix__) || defined(__APPLE__)
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif
#endif

namespace microide::tests {
namespace {

using microide::platform::ControlRequestLineScan;
using microide::platform::ScanControlRequestLines;

// Pure, platform-independent: the byte-cap decision must reject a complete
// over-cap line BEFORE it is copied out — the old ingest substr'd every complete
// line and only bounded the residual (un-newlined) trailing line, so a hostile
// peer could queue a multi-megabyte request just by terminating it with '\n'.
void TestScanControlRequestLinesRejectsOversizeCompleteLine() {
  // Baseline: complete lines split, CR stripped, incomplete trailing kept.
  {
    const ControlRequestLineScan scan = ScanControlRequestLines("a\r\nbb\nccc", 1024);
    Expect(scan.lines.size() == 2, "two complete lines are produced");
    Expect(scan.lines[0] == "a", "trailing CR is stripped");
    Expect(scan.lines[1] == "bb", "second complete line is intact");
    Expect(scan.consumed_bytes == 6, "consumed covers 'a\\r\\n' + 'bb\\n', not the trailing 'ccc'");
    Expect(!scan.oversize_line, "no complete line exceeded the cap");
  }
  // Empty lines are consumed but not queued.
  {
    const ControlRequestLineScan scan = ScanControlRequestLines("\n\nx\n", 1024);
    Expect(scan.lines.size() == 1 && scan.lines[0] == "x",
           "blank lines are skipped, only the real line surfaces");
    Expect(scan.consumed_bytes == 4, "all three newlines are consumed");
    Expect(!scan.oversize_line, "blank lines never trip the cap");
  }
  // A complete newline-terminated line over the cap trips the flag and stops the
  // scan BEFORE it — earlier valid lines are still returned, the over-cap line is
  // never materialized, and the caller sheds the connection.
  {
    std::string buffer = "ok\n";
    const std::size_t prefix = buffer.size();
    buffer.append(std::string(64, 'z'));  // 64-byte complete line...
    buffer.push_back('\n');
    const ControlRequestLineScan scan = ScanControlRequestLines(buffer, 8);
    Expect(scan.oversize_line, "a complete line past the cap is flagged");
    Expect(scan.lines.size() == 1 && scan.lines[0] == "ok",
           "valid lines before the over-cap line are still produced");
    Expect(scan.consumed_bytes == prefix,
           "the scan stops before the over-cap line (its bytes are not consumed)");
  }
  // A boundary line exactly at the cap is accepted; one byte over is rejected.
  {
    const ControlRequestLineScan at_cap = ScanControlRequestLines("12345678\n", 8);
    Expect(!at_cap.oversize_line && at_cap.lines.size() == 1,
           "a line exactly at the cap is accepted");
    const ControlRequestLineScan over_cap = ScanControlRequestLines("123456789\n", 8);
    Expect(over_cap.oversize_line && over_cap.lines.empty(),
           "a line one byte over the cap is rejected");
  }
}

#if defined(__unix__) || defined(__APPLE__)

using microide::platform::ControlInboundMessage;
using microide::platform::ControlSocketServer;
using namespace std::chrono_literals;

int ConnectUnix(const std::filesystem::path& path) {
  const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) {
    return -1;
  }
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  const std::string path_string = path.string();
  std::memcpy(address.sun_path, path_string.c_str(), path_string.size() + 1);
  if (::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
    ::close(fd);
    return -1;
  }
  return fd;
}

bool WriteAll(int fd, std::string_view data) {
  while (!data.empty()) {
    // MSG_NOSIGNAL: the server may shed us mid-write (oversized-line / flood
    // resilience tests), and a raw send() would otherwise raise SIGPIPE and kill
    // the test process instead of returning an error.
    const ssize_t written = ::send(fd, data.data(), data.size(), MSG_NOSIGNAL);
    if (written <= 0) {
      return false;
    }
    data.remove_prefix(static_cast<std::size_t>(written));
  }
  return true;
}

// Read a single newline-terminated line with a wall-clock timeout. Returns
// nullopt on EOF-without-line or timeout.
std::optional<std::string> ReadLine(int fd, std::chrono::milliseconds timeout) {
  std::string buf;
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    pollfd p{fd, POLLIN, 0};
    const int ready = ::poll(&p, 1, 25);
    if (ready <= 0) {
      continue;
    }
    char chunk[256];
    const ssize_t count = ::recv(fd, chunk, sizeof(chunk), 0);
    if (count > 0) {
      buf.append(chunk, static_cast<std::size_t>(count));
      const std::size_t newline = buf.find('\n');
      if (newline != std::string::npos) {
        return buf.substr(0, newline);
      }
    } else if (count == 0) {
      const std::size_t newline = buf.find('\n');
      return newline == std::string::npos ? std::nullopt
                                          : std::optional<std::string>(buf.substr(0, newline));
    }
  }
  return std::nullopt;
}

// Drain inbound until one message arrives (acting as the host main thread).
std::optional<ControlInboundMessage> DrainOne(ControlSocketServer& server,
                                              std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    for (ControlInboundMessage& message : server.TakeInbound()) {
      return message;
    }
    std::this_thread::sleep_for(5ms);
  }
  return std::nullopt;
}

bool WaitForConnectionCount(const ControlSocketServer& server, std::size_t target,
                            std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (server.ConnectionCount() == target) {
      return true;
    }
    std::this_thread::sleep_for(5ms);
  }
  return server.ConnectionCount() == target;
}

// The original bug: a client that half-closes its write side after sending (as
// socat / `echo | nc` do) lost its reply because the server reaped the
// connection in the same poll iteration as the EOF, before the host drained and
// answered the request.
// Regression: Start must not blindly unlink whatever sits at the socket path. If
// a regular (non-socket) file is there, it refuses to start and leaves the file
// intact rather than deleting a user-owned file.
void TestStartRefusesToDeleteNonSocketFile() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path socket_path = temp_dir.path() / "not-a-socket";
  WriteFile(socket_path, "precious user data\n");

  ControlSocketServer server;
  Expect(!server.Start(socket_path),
         "Start must refuse when a non-socket file occupies the socket path");
  Expect(std::filesystem::exists(socket_path), "the user's file must not be deleted");
  Expect(ReadFile(socket_path) == "precious user data\n", "the file's contents are intact");

  // A clean path still starts (and a subsequent restart clears its own stale socket).
  const std::filesystem::path clean_path = temp_dir.path() / "control.sock";
  ControlSocketServer clean_server;
  Expect(clean_server.Start(clean_path), "Start succeeds on a clean path");
}

void TestHalfClosedClientStillReceivesReply() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path socket_path = temp_dir.path() / "control.sock";
  ControlSocketServer server;
  Expect(server.Start(socket_path), "server should start");

  const int client = ConnectUnix(socket_path);
  Expect(client >= 0, "client should connect");
  Expect(WriteAll(client, "{\"id\":1,\"query\":\"status\"}\n"), "client should send a request");
  // Half-close: client is done writing but still wants to read the reply.
  ::shutdown(client, SHUT_WR);

  const std::optional<ControlInboundMessage> message = DrainOne(server, 2s);
  Expect(message.has_value(), "server should surface the half-closed client's request");

  const std::string reply = "{\"id\":1,\"ok\":true}";
  server.SendLine(message->connection_id, reply);

  const std::optional<std::string> got = ReadLine(client, 2s);
  Expect(got.has_value() && *got == reply,
         "a half-closed client must still receive its reply");

  ::close(client);
  Expect(WaitForConnectionCount(server, 0, 2s),
         "the connection should be reaped once drained");
  server.Stop();
}

// A client that closes after sending only a partial (un-newlined) line produces
// no request; the connection must still be reaped promptly, not hang.
void TestPartialLineAtEofReapsCleanly() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path socket_path = temp_dir.path() / "control.sock";
  ControlSocketServer server;
  Expect(server.Start(socket_path), "server should start");

  const int client = ConnectUnix(socket_path);
  Expect(client >= 0, "client should connect");
  Expect(WriteAll(client, "{\"id\":1,\"quer"), "client should send a partial line");
  ::close(client);

  Expect(WaitForConnectionCount(server, 0, 2s),
         "a partial-line connection should reap without hanging");
  server.Stop();
}

// Backstop: a request that is never answered must not leak the fd forever — the
// connection is reaped after the bounded linger grace period (~2s).
void TestUnansweredRequestReapsAfterGrace() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path socket_path = temp_dir.path() / "control.sock";
  ControlSocketServer server;
  Expect(server.Start(socket_path), "server should start");

  const int client = ConnectUnix(socket_path);
  Expect(client >= 0, "client should connect");
  Expect(WriteAll(client, "{\"id\":1,\"query\":\"status\"}\n"), "client should send a request");
  ::shutdown(client, SHUT_WR);

  // Deliberately never reply. The connection must still reap within the grace
  // period plus slack.
  Expect(WaitForConnectionCount(server, 0, 5s),
         "an unanswered half-closed connection should reap after the grace period");

  ::close(client);
  server.Stop();
}

// A connection kept open across several request/reply round-trips is not reaped
// until the client actually closes — the linger logic must not disturb the
// common full-duplex case.
void TestOpenConnectionHandlesMultipleRequests() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path socket_path = temp_dir.path() / "control.sock";
  ControlSocketServer server;
  Expect(server.Start(socket_path), "server should start");

  const int client = ConnectUnix(socket_path);
  Expect(client >= 0, "client should connect");

  for (int i = 0; i < 3; ++i) {
    const std::string request = "{\"id\":" + std::to_string(i) + "}\n";
    Expect(WriteAll(client, request), "client should send a request");
    const std::optional<ControlInboundMessage> message = DrainOne(server, 2s);
    Expect(message.has_value(), "server should surface each request");
    const std::string reply = "{\"id\":" + std::to_string(i) + ",\"ok\":true}";
    server.SendLine(message->connection_id, reply);
    const std::optional<std::string> got = ReadLine(client, 2s);
    Expect(got.has_value() && *got == reply, "each reply should arrive on the open connection");
  }
  Expect(server.ConnectionCount() == 1, "the connection should stay open between requests");

  ::close(client);
  Expect(WaitForConnectionCount(server, 0, 2s), "closing the client should reap the connection");
  server.Stop();
}

// Resilience: a hostile local client that streams bytes with no newline must not
// grow the server's per-connection read buffer without bound (memory exhaustion).
// Once the unframed line passes the per-request ceiling the connection is shed.
void TestOversizedUnterminatedLineIsShed() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path socket_path = temp_dir.path() / "control.sock";
  ControlSocketServer server;
  Expect(server.Start(socket_path), "server should start");

  const int client = ConnectUnix(socket_path);
  Expect(client >= 0, "client should connect");
  Expect(WaitForConnectionCount(server, 1, 2s), "server should accept the connection");

  // Stream ~4 MiB with no newline. WriteAll may fail partway once the server
  // sheds us mid-stream (EPIPE) — that is the expected outcome, so ignore it.
  const std::string flood(4u * 1024 * 1024, 'x');
  (void)WriteAll(client, flood);

  Expect(WaitForConnectionCount(server, 0, 3s),
         "an oversized unterminated line should shed the connection, not OOM");

  ::close(client);
  server.Stop();
}

// Resilience: a client that floods complete request lines faster than the host
// drains them must not grow the shared inbound queue without bound. This test
// deliberately never drains (never calls TakeInbound), so the queue fills to its
// cap and the offending connection is shed.
void TestInboundQueueFloodIsShed() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path socket_path = temp_dir.path() / "control.sock";
  ControlSocketServer server;
  Expect(server.Start(socket_path), "server should start");

  const int client = ConnectUnix(socket_path);
  Expect(client >= 0, "client should connect");
  Expect(WaitForConnectionCount(server, 1, 2s), "server should accept the connection");

  // Far more small complete lines than the inbound-queue cap, none drained.
  std::string flood;
  flood.reserve(20000 * 3);
  for (int i = 0; i < 20000; ++i) {
    flood += "{}\n";
  }
  (void)WriteAll(client, flood);

  Expect(WaitForConnectionCount(server, 0, 3s),
         "an undrained request flood should shed the connection, not grow the queue");

  ::close(client);
  server.Stop();
}

// Resilience: a single COMPLETE (newline-terminated) request line larger than
// the per-request cap must be shed, not copied into the shared queue. Regression
// for the bug where the cap was only checked against the residual incomplete
// trailing line, so a >1 MiB line ending in '\n' was queued verbatim. The host
// (never draining here proves the point is the shed, not the drain) must see the
// connection reaped and no giant message surface.
void TestOversizedCompleteLineIsShed() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path socket_path = temp_dir.path() / "control.sock";
  ControlSocketServer server;
  Expect(server.Start(socket_path), "server should start");

  const int client = ConnectUnix(socket_path);
  Expect(client >= 0, "client should connect");
  Expect(WaitForConnectionCount(server, 1, 2s), "server should accept the connection");

  // ~2 MiB single line TERMINATED with a newline (past the 1 MiB per-request
  // cap). WriteAll may fail partway as the server sheds us mid-stream — expected.
  std::string oversize(2u * 1024 * 1024, 'x');
  oversize.push_back('\n');
  (void)WriteAll(client, oversize);

  Expect(WaitForConnectionCount(server, 0, 3s),
         "an oversized complete line must shed the connection, not queue it");
  Expect(!DrainOne(server, 200ms).has_value(),
         "the oversized line must never surface as an inbound message");

  ::close(client);
  server.Stop();
}

// Resilience: idle local clients that connect and never send data must not grow
// the server's connection map / poll set without bound.
void TestIdleConnectionFloodIsCapped() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path socket_path = temp_dir.path() / "control.sock";
  ControlSocketServer server;
  Expect(server.Start(socket_path), "server should start");

  std::vector<int> clients;
  for (int i = 0; i < 180; ++i) {
    const int client = ConnectUnix(socket_path);
    if (client >= 0) {
      clients.push_back(client);
    }
  }

  const auto deadline = std::chrono::steady_clock::now() + 3s;
  while (std::chrono::steady_clock::now() < deadline && server.ConnectionCount() < 128) {
    std::this_thread::sleep_for(5ms);
  }
  // Give the I/O thread a short window to accept and shed any excess sockets.
  std::this_thread::sleep_for(100ms);

  Expect(server.ConnectionCount() <= 128,
         "idle connection floods must be capped so the poll set stays bounded");

  for (int client : clients) {
    ::close(client);
  }
  Expect(WaitForConnectionCount(server, 0, 3s),
         "closing flood clients should reap every accepted connection");
  server.Stop();
}

#endif  // POSIX

}  // namespace

void RegisterControlSocketServerTests(std::vector<TestCase>& tests) {
  AddTest(tests, "ControlSocketServer/ScanRejectsOversizeCompleteLine",
          TestScanControlRequestLinesRejectsOversizeCompleteLine);
#if defined(__unix__) || defined(__APPLE__)
  AddTest(tests, "ControlSocketServer/StartRefusesToDeleteNonSocketFile",
          TestStartRefusesToDeleteNonSocketFile);
  AddTest(tests, "ControlSocketServer/OversizedCompleteLineIsShed",
          TestOversizedCompleteLineIsShed);
  AddTest(tests, "ControlSocketServer/HalfClosedClientStillReceivesReply",
          TestHalfClosedClientStillReceivesReply);
  AddTest(tests, "ControlSocketServer/PartialLineAtEofReapsCleanly",
          TestPartialLineAtEofReapsCleanly);
  AddTest(tests, "ControlSocketServer/UnansweredRequestReapsAfterGrace",
          TestUnansweredRequestReapsAfterGrace);
  AddTest(tests, "ControlSocketServer/OpenConnectionHandlesMultipleRequests",
          TestOpenConnectionHandlesMultipleRequests);
  AddTest(tests, "ControlSocketServer/OversizedUnterminatedLineIsShed",
          TestOversizedUnterminatedLineIsShed);
  AddTest(tests, "ControlSocketServer/InboundQueueFloodIsShed",
          TestInboundQueueFloodIsShed);
  AddTest(tests, "ControlSocketServer/IdleConnectionFloodIsCapped",
          TestIdleConnectionFloodIsCapped);
#else
  (void)tests;
#endif
}

}  // namespace microide::tests
