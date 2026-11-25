#include "TestSupport.h"

#include "platform/ControlSocketLimits.h"

#include <chrono>
#include <cstring>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "platform/ControlSocketServer.h"
#include "util/Log.h"

#include <mutex>

#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

namespace microide::tests {
namespace {

using microide::platform::ControlRequestLineScan;
using microide::platform::ScanControlRequestLines;

void TestScanControlRequestLinesRejectsOversizeCompleteLine() {
  {
    const ControlRequestLineScan scan = ScanControlRequestLines("a\r\nbb\nccc", 1024);
    Expect(scan.lines.size() == 2, "two complete lines are produced");
    Expect(scan.lines[0] == "a", "trailing CR is stripped");
    Expect(scan.lines[1] == "bb", "second complete line is intact");
    Expect(scan.consumed_bytes == 6, "consumed covers 'a\\r\\n' + 'bb\\n', not the trailing 'ccc'");
    Expect(!scan.oversize_line, "no complete line exceeded the cap");
  }
  {
    const ControlRequestLineScan scan = ScanControlRequestLines("\n\nx\n", 1024);
    Expect(scan.lines.size() == 1 && scan.lines[0] == "x",
           "blank lines are skipped, only the real line surfaces");
    Expect(scan.consumed_bytes == 4, "all three newlines are consumed");
    Expect(!scan.oversize_line, "blank lines never trip the cap");
  }
  {
    std::string buffer = "ok\n";
    const std::size_t prefix = buffer.size();
    buffer.append(std::string(64, 'z'));
    buffer.push_back('\n');
    const ControlRequestLineScan scan = ScanControlRequestLines(buffer, 8);
    Expect(scan.oversize_line, "a complete line past the cap is flagged");
    Expect(scan.lines.size() == 1 && scan.lines[0] == "ok",
           "valid lines before the over-cap line are still produced");
    Expect(scan.consumed_bytes == prefix,
           "the scan stops before the over-cap line (its bytes are not consumed)");
  }
  {
    const ControlRequestLineScan at_cap = ScanControlRequestLines("12345678\n", 8);
    Expect(!at_cap.oversize_line && at_cap.lines.size() == 1,
           "a line exactly at the cap is accepted");
    const ControlRequestLineScan over_cap = ScanControlRequestLines("123456789\n", 8);
    Expect(over_cap.oversize_line && over_cap.lines.empty(),
           "a line one byte over the cap is rejected");
  }
}

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
    const ssize_t written = ::send(fd, data.data(), data.size(), MSG_NOSIGNAL);
    if (written <= 0) {
      return false;
    }
    data.remove_prefix(static_cast<std::size_t>(written));
  }
  return true;
}

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

void TestStartRefusesToDeleteNonSocketFile() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path socket_path = temp_dir.path() / "not-a-socket";
  WriteFile(socket_path, "precious user data\n");

  ControlSocketServer server;
  Expect(!server.Start(socket_path),
         "Start must refuse when a non-socket file occupies the socket path");
  Expect(std::filesystem::exists(socket_path), "the user's file must not be deleted");
  Expect(ReadFile(socket_path) == "precious user data\n", "the file's contents are intact");

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

void TestUnansweredRequestReapsAfterGrace() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path socket_path = temp_dir.path() / "control.sock";
  ControlSocketServer server;
  Expect(server.Start(socket_path), "server should start");

  const int client = ConnectUnix(socket_path);
  Expect(client >= 0, "client should connect");
  Expect(WriteAll(client, "{\"id\":1,\"query\":\"status\"}\n"), "client should send a request");
  ::shutdown(client, SHUT_WR);

  Expect(WaitForConnectionCount(server, 0, 5s),
         "an unanswered half-closed connection should reap after the grace period");

  ::close(client);
  server.Stop();
}

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

void TestOversizedUnterminatedLineIsShed() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path socket_path = temp_dir.path() / "control.sock";
  ControlSocketServer server;
  Expect(server.Start(socket_path), "server should start");

  const int client = ConnectUnix(socket_path);
  Expect(client >= 0, "client should connect");
  Expect(WaitForConnectionCount(server, 1, 2s), "server should accept the connection");

  const std::string flood(4u * 1024 * 1024, 'x');
  (void)WriteAll(client, flood);

  Expect(WaitForConnectionCount(server, 0, 3s),
         "an oversized unterminated line should shed the connection, not OOM");

  ::close(client);
  server.Stop();
}

void TestInboundQueueFloodIsShed() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path socket_path = temp_dir.path() / "control.sock";
  ControlSocketServer server;
  Expect(server.Start(socket_path), "server should start");

  const int client = ConnectUnix(socket_path);
  Expect(client >= 0, "client should connect");
  Expect(WaitForConnectionCount(server, 1, 2s), "server should accept the connection");

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

void TestOversizedCompleteLineIsShed() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path socket_path = temp_dir.path() / "control.sock";
  ControlSocketServer server;
  Expect(server.Start(socket_path), "server should start");

  const int client = ConnectUnix(socket_path);
  Expect(client >= 0, "client should connect");
  Expect(WaitForConnectionCount(server, 1, 2s), "server should accept the connection");

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

void TestLargeMultiLineBroadcastDeliveredInOrder() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path socket_path = temp_dir.path() / "control.sock";
  ControlSocketServer server;
  Expect(server.Start(socket_path), "server should start");

  const int client = ConnectUnix(socket_path);
  Expect(client >= 0, "client should connect");
  Expect(WaitForConnectionCount(server, 1, 2s), "server should accept the connection");

  constexpr int kLineCount = 100;
  constexpr std::size_t kPadBytes = 16u << 10;
  std::vector<std::string> expected;
  expected.reserve(kLineCount);
  for (int i = 0; i < kLineCount; ++i) {
    std::string line = "evt-" + std::to_string(i) + ":";
    line.append(kPadBytes, static_cast<char>('a' + (i % 26)));
    expected.push_back(line);
    server.Broadcast(line);
  }

  std::vector<std::string> got;
  std::string buf;
  const auto deadline = std::chrono::steady_clock::now() + 10s;
  while (static_cast<int>(got.size()) < kLineCount &&
         std::chrono::steady_clock::now() < deadline) {
    std::size_t newline = buf.find('\n');
    while (newline != std::string::npos && static_cast<int>(got.size()) < kLineCount) {
      got.push_back(buf.substr(0, newline));
      buf.erase(0, newline + 1);
      newline = buf.find('\n');
    }
    if (static_cast<int>(got.size()) >= kLineCount) {
      break;
    }
    pollfd p{client, POLLIN, 0};
    if (::poll(&p, 1, 25) <= 0) {
      continue;
    }
    char chunk[4096];
    const ssize_t count = ::recv(client, chunk, sizeof(chunk), 0);
    if (count > 0) {
      buf.append(chunk, static_cast<std::size_t>(count));
    } else if (count == 0) {
      break;
    }
  }

  Expect(static_cast<int>(got.size()) == kLineCount,
         "every broadcast line should be delivered");
  bool all_match = got.size() == expected.size();
  for (std::size_t i = 0; i < got.size() && i < expected.size(); ++i) {
    if (got[i] != expected[i]) {
      all_match = false;
      break;
    }
  }
  Expect(all_match, "broadcast lines should arrive intact and in order");

  ::close(client);
  Expect(WaitForConnectionCount(server, 0, 2s), "closing the client should reap the connection");
  server.Stop();
}

void TestControlRequestCeilingIsOneValue() {
  Expect(microide::platform::kMaxControlRequestLineBytes == (1u << 20),
         "the shared control request ceiling is 1 MiB");
}

void TestShedClientIsLoggedWithItsReason() {
  std::mutex mutex;
  std::vector<std::string> lines;
  microide::util::SetLogSink([&](std::string_view line) {
    std::lock_guard<std::mutex> lock(mutex);
    lines.emplace_back(line);
  });
  struct RestoreSink {
    ~RestoreSink() { microide::util::SetLogSink(nullptr); }
  } restore;

  TemporaryDirectory temp_dir;
  ControlSocketServer server;
  Expect(server.Start(temp_dir.path() / "control.sock"), "server should start");
  const int client = ConnectUnix(temp_dir.path() / "control.sock");
  Expect(client >= 0, "client should connect");
  Expect(WaitForConnectionCount(server, 1, 2s), "server should accept the connection");

  const std::string event(64u * 1024, 'e');
  for (int i = 0; i < 400 && server.ConnectionCount() == 1; ++i) {
    server.Broadcast(event);
  }
  Expect(WaitForConnectionCount(server, 0, 3s), "a client that never reads should be shed");
  server.Stop();
  ::close(client);

  std::lock_guard<std::mutex> lock(mutex);
  bool logged = false;
  for (const std::string& line : lines) {
    logged = logged || (line.find("dropped client") != std::string::npos &&
                        line.find("not reading") != std::string::npos);
  }
  Expect(logged, "the shed should be logged with its reason");
}

}

void RegisterControlSocketServerTests(std::vector<TestCase>& tests) {
  AddTest(tests, "ControlSocketServer/RequestCeilingIsOneValue",
          TestControlRequestCeilingIsOneValue);
  AddTest(tests, "ControlSocketServer/ScanRejectsOversizeCompleteLine",
          TestScanControlRequestLinesRejectsOversizeCompleteLine);
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
  AddTest(tests, "ControlSocketServer/ShedClientIsLoggedWithItsReason",
          TestShedClientIsLoggedWithItsReason);
  AddTest(tests, "ControlSocketServer/LargeMultiLineBroadcastDeliveredInOrder",
          TestLargeMultiLineBroadcastDeliveredInOrder);
}

}
