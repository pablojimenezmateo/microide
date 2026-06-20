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
#endif

namespace microide::tests {
namespace {

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
    const ssize_t written = ::send(fd, data.data(), data.size(), 0);
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

#endif  // POSIX

}  // namespace

void RegisterControlSocketServerTests(std::vector<TestCase>& tests) {
#if defined(__unix__) || defined(__APPLE__)
  AddTest(tests, "ControlSocketServer/HalfClosedClientStillReceivesReply",
          TestHalfClosedClientStillReceivesReply);
  AddTest(tests, "ControlSocketServer/PartialLineAtEofReapsCleanly",
          TestPartialLineAtEofReapsCleanly);
  AddTest(tests, "ControlSocketServer/UnansweredRequestReapsAfterGrace",
          TestUnansweredRequestReapsAfterGrace);
  AddTest(tests, "ControlSocketServer/OpenConnectionHandlesMultipleRequests",
          TestOpenConnectionHandlesMultipleRequests);
#else
  (void)tests;
#endif
}

}  // namespace microide::tests
