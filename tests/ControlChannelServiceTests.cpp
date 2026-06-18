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
#include "workspace/LaunchConfig.h"
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

void TestLaunchConfigsAndAdaptersOverSocket() {
  const std::filesystem::path runtime =
      std::filesystem::temp_directory_path() /
      ("microide-control-discovery-" + std::to_string(::getpid()));
  std::error_code ec;
  std::filesystem::remove_all(runtime, ec);
  std::filesystem::create_directories(runtime, ec);
  ::setenv("XDG_RUNTIME_DIR", runtime.string().c_str(), 1);

  microide::workspace::WorkspaceContext context;
  context.current_project_state.root = "/tmp/proj";
  microide::workspace::LaunchConfig config;
  config.name = "Run pytest";
  config.type = "debugpy";
  config.request = "launch";
  context.current_project_state.launch_configs.push_back(config);
  context.current_project_state.selected_launch_config_index = 0;

  microide::workspace::ControlChannelService service;
  service.Configure(
      context,
      microide::workspace::ControlChannelService::Operations{
          .execute_command_line =
              [](const std::string&) {
                return microide::workspace::ControlChannelService::CommandOutcome{.ok = true};
              },
          .adapter_types = []() { return std::vector<std::string>{"debugpy", "lldb"}; },
      });
  service.SetWakeEventType(0);
  Expect(service.Start("/tmp/proj"), "service should start");

  const std::string socket_path =
      (runtime / "microide" / (std::to_string(::getpid()) + ".sock")).string();
  int fd = -1;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while (fd < 0 && std::chrono::steady_clock::now() < deadline) {
    fd = ConnectUnix(socket_path);
    if (fd < 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  }
  Expect(fd >= 0, "client should connect");

  const auto configs = util::ParseJson(ExchangeLine(service, fd, R"({"query":"launch-configs"})"));
  Expect(configs.has_value() && (*configs)["ok"].AsBool(), "launch-configs query should succeed");
  const util::JsonValue& configs_result = (*configs)["result"];
  Expect(configs_result.IsArray() && configs_result.AsArray().size() == 1,
         "one launch config expected");
  Expect(configs_result.AsArray()[0]["name"].AsString() == "Run pytest", "config name should match");
  Expect(configs_result.AsArray()[0]["type"].AsString() == "debugpy", "config type should match");
  Expect(configs_result.AsArray()[0]["selected"].AsBool(), "config should report selected");

  const auto adapters = util::ParseJson(ExchangeLine(service, fd, R"({"query":"adapters"})"));
  Expect(adapters.has_value() && (*adapters)["ok"].AsBool(), "adapters query should succeed");
  const util::JsonValue& adapters_result = (*adapters)["result"];
  Expect(adapters_result.IsArray() && adapters_result.AsArray().size() == 2,
         "two adapter types expected");
  Expect(adapters_result.AsArray()[0].AsString() == "debugpy", "first adapter should match");

  ::close(fd);
  service.Stop();
  std::filesystem::remove_all(runtime, ec);
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
void TestLaunchConfigsAndAdaptersOverSocket() {}

#endif

// Cross-platform: the stdout JSONL mirror must surface debug events even when no
// socket client is connected (the whole point of `--control`).
void TestStdoutMirrorEmitsWithoutConnections() {
  microide::workspace::WorkspaceContext context;
  context.current_project_state.root = "/tmp/proj";

  std::vector<std::string> emitted;
  microide::workspace::ControlChannelService service;
  service.Configure(
      context,
      microide::workspace::ControlChannelService::Operations{
          .execute_command_line =
              [](const std::string&) {
                return microide::workspace::ControlChannelService::CommandOutcome{.ok = true};
              },
          .emit_jsonl = [&emitted](const std::string& line) { emitted.push_back(line); },
      });
  // No Start(): the socket server is not running and there are zero connections.
  Expect(!service.IsRunning(), "service should not be running");

  // Mirror off → no emission.
  service.OnDebugOutput("stdout", "ignored");
  Expect(emitted.empty(), "events should not emit while the mirror is off");

  service.SetStdoutMirror(true);
  service.OnDebugOutput("stdout", "hello");
  service.OnDebugStopped();
  service.OnDebugTerminated(1);
  Expect(emitted.size() == 3, "three events should mirror to stdout with no client");

  const auto output = util::ParseJson(emitted[0]);
  Expect(output.has_value() && (*output)["event"].AsString() == "output",
         "first mirrored line should be the output event");
  Expect((*output)["text"].AsString() == "hello", "output text should round-trip");
  const auto stopped = util::ParseJson(emitted[1]);
  Expect(stopped.has_value() && (*stopped)["event"].AsString() == "stopped",
         "second mirrored line should be the stopped event");
  const auto terminated = util::ParseJson(emitted[2]);
  Expect(terminated.has_value() && (*terminated)["event"].AsString() == "terminated",
         "third mirrored line should be the terminated event");
}

}  // namespace

void RegisterControlChannelServiceTests(std::vector<TestCase>& tests) {
  AddTest(tests, "ControlChannelService/QueryAndCommandOverSocket",
          TestQueryAndCommandOverSocket);
  AddTest(tests, "ControlChannelService/ControlListFiltersDeadPids",
          TestControlListFiltersDeadPids);
  AddTest(tests, "ControlChannelService/LaunchConfigsAndAdaptersOverSocket",
          TestLaunchConfigsAndAdaptersOverSocket);
  AddTest(tests, "ControlChannelService/StdoutMirrorEmitsWithoutConnections",
          TestStdoutMirrorEmitsWithoutConnections);
}

}  // namespace microide::tests
