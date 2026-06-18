#include "TestSupport.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "util/JsonValue.h"
#include "workspace/ControlChannelService.h"
#include "workspace/WorkspaceContext.h"

#if defined(__unix__) || defined(__APPLE__)
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace microide::tests {
namespace {

#if defined(__unix__) || defined(__APPLE__)

// Connect to an AF_UNIX socket, returning the fd or -1.
int ConnectUnix(const std::string& path) {
  const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) {
    return -1;
  }
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  std::snprintf(address.sun_path, sizeof(address.sun_path), "%s", path.c_str());
  if (::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
    ::close(fd);
    return -1;
  }
  return fd;
}

// Drive the service until a response line is read back from `fd` or we time out.
std::string ExchangeLine(microide::workspace::ControlChannelService& service, int fd,
                         const std::string& request) {
  const std::string framed = request + "\n";
  ::send(fd, framed.data(), framed.size(), 0);

  std::string received;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while (std::chrono::steady_clock::now() < deadline) {
    service.ConsumeControlCallbacks();
    char buffer[2048];
    const ssize_t count = ::recv(fd, buffer, sizeof(buffer), MSG_DONTWAIT);
    if (count > 0) {
      received.append(buffer, static_cast<std::size_t>(count));
      if (received.find('\n') != std::string::npos) {
        break;
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return received;
}

void TestQueryAndCommandOverSocket() {
  // Isolate the runtime dir so the socket path is test-local.
  const std::filesystem::path runtime =
      std::filesystem::temp_directory_path() /
      ("microide-control-test-" + std::to_string(::getpid()));
  std::error_code ec;
  std::filesystem::remove_all(runtime, ec);
  std::filesystem::create_directories(runtime, ec);
  ::setenv("XDG_RUNTIME_DIR", runtime.string().c_str(), 1);

  microide::workspace::WorkspaceContext context;
  context.current_project_state.root = "/tmp/proj";
  context.current_project_state.breakpoint_store.Set("/tmp/proj/a.cpp", 0);  // 0-based

  std::string last_command;
  microide::workspace::ControlChannelService service;
  service.Configure(
      context,
      microide::workspace::ControlChannelService::Operations{
          .execute_command_line =
              [&last_command](const std::string& line) {
                last_command = line;
                return microide::workspace::ControlChannelService::CommandOutcome{
                    .ok = true, .feedback = "ack", .error = {}};
              },
      });
  service.SetWakeEventType(0);  // no SDL push; the test polls instead.
  Expect(service.Start("/tmp/proj"), "control service should start");

  const std::string socket_path =
      (runtime / "microide" / (std::to_string(::getpid()) + ".sock")).string();
  int fd = -1;
  const auto connect_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while (fd < 0 && std::chrono::steady_clock::now() < connect_deadline) {
    fd = ConnectUnix(socket_path);
    if (fd < 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  }
  Expect(fd >= 0, "client should connect to the control socket");

  // Query: the seeded breakpoint should report at 1-based line 1.
  const std::string query_response = ExchangeLine(service, fd, R"({"id":1,"query":"breakpoints"})");
  const auto query_json = util::ParseJson(query_response);
  Expect(query_json.has_value(), "query response should be JSON");
  Expect((*query_json)["ok"].AsBool(), "query should succeed");
  const util::JsonValue& result = (*query_json)["result"];
  Expect(result.IsArray() && result.AsArray().size() == 1, "one breakpoint file expected");
  const util::JsonValue& file = result.AsArray()[0];
  Expect(file["file"].AsString() == "/tmp/proj/a.cpp", "breakpoint file should match");
  Expect(file["breakpoints"].AsArray().size() == 1, "one breakpoint expected");
  Expect(file["breakpoints"].AsArray()[0]["line"].AsInt() == 1,
         "0-based store line should report as 1-based");

  // Command: routes through execute_command_line.
  const std::string command_response =
      ExchangeLine(service, fd, R"({"id":2,"command":"debug-continue"})");
  const auto command_json = util::ParseJson(command_response);
  Expect(command_json.has_value(), "command response should be JSON");
  Expect((*command_json)["ok"].AsBool(), "command should succeed");
  Expect(last_command == "debug-continue", "command should reach execute_command_line");

  ::close(fd);
  service.Stop();
  std::filesystem::remove_all(runtime, ec);
}

void WriteDescriptor(const std::filesystem::path& dir, int pid) {
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  std::ofstream out(dir / (std::to_string(pid) + ".json"), std::ios::trunc);
  out << R"({"pid":)" << pid << R"(,"socket":"/tmp/x.sock","project_root":"/p"})";
}

void TestControlListFiltersDeadPids() {
  const std::filesystem::path runtime =
      std::filesystem::temp_directory_path() /
      ("microide-control-list-test-" + std::to_string(::getpid()));
  std::error_code ec;
  std::filesystem::remove_all(runtime, ec);
  ::setenv("XDG_RUNTIME_DIR", runtime.string().c_str(), 1);

  const std::filesystem::path instances = runtime / "microide" / "instances";
  const int alive_pid = static_cast<int>(::getpid());
  const int dead_pid = 1000000000;  // out of pid range → no such process
  WriteDescriptor(instances, alive_pid);
  WriteDescriptor(instances, dead_pid);

  const std::string listing = microide::workspace::ControlListInstancesText();
  Expect(listing.find(std::to_string(alive_pid)) != std::string::npos,
         "live instance should be listed");
  Expect(listing.find(std::to_string(dead_pid)) == std::string::npos,
         "dead instance should be filtered out");
  Expect(!std::filesystem::exists(instances / (std::to_string(dead_pid) + ".json")),
         "dead descriptor should be pruned");

  std::filesystem::remove_all(runtime, ec);
}

#else

void TestQueryAndCommandOverSocket() {}
void TestControlListFiltersDeadPids() {}

#endif

}  // namespace

void RegisterControlChannelServiceTests(std::vector<TestCase>& tests) {
  AddTest(tests, "ControlChannelService/QueryAndCommandOverSocket",
          TestQueryAndCommandOverSocket);
  AddTest(tests, "ControlChannelService/ControlListFiltersDeadPids",
          TestControlListFiltersDeadPids);
}

}  // namespace microide::tests
