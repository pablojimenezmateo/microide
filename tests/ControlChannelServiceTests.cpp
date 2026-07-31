#include "TestSupport.h"

#include <algorithm>
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
  config.arguments = util::JsonValue(util::JsonObject{{"program", util::JsonValue("main.py")}});
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
          .adapters =
              []() {
                return std::vector<microide::workspace::ControlAdapterInfo>{
                    {"debugpy", {"python3", "-m", "debugpy.adapter"}}, {"lldb", {"lldb-dap"}}};
              },
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
  Expect(configs_result.AsArray()[0]["arguments"]["program"].AsString() == "main.py",
         "config arguments (program/args/cwd) should be surfaced");

  const auto adapters = util::ParseJson(ExchangeLine(service, fd, R"({"query":"adapters"})"));
  Expect(adapters.has_value() && (*adapters)["ok"].AsBool(), "adapters query should succeed");
  const util::JsonValue& adapters_result = (*adapters)["result"];
  Expect(adapters_result.IsArray() && adapters_result.AsArray().size() == 2,
         "two adapters expected");
  // Order is unspecified (map iteration); find the debugpy entry explicitly.
  bool found_debugpy = false;
  for (const util::JsonValue& adapter : adapters_result.AsArray()) {
    if (adapter["type"].AsString() == "debugpy") {
      found_debugpy = true;
      Expect(adapter["command"].IsArray() && adapter["command"].AsArray().size() == 3,
             "adapter should carry its spawn command");
      Expect(adapter["command"].AsArray()[0].AsString() == "python3",
             "adapter command argv[0] should match");
    }
  }
  Expect(found_debugpy, "the debugpy adapter should be present with its command");

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

// A single control query must not materialize an unbounded JSON array on the UI
// path. A debug-state response with a huge call stack is capped at
// kMaxControlQueryEntries frames and flagged `framesTruncated`. TD-2026-07-17A-095.
void TestQueryResponseIsBounded() {
  const std::filesystem::path runtime =
      std::filesystem::temp_directory_path() /
      ("microide-control-bound-test-" + std::to_string(::getpid()));
  std::error_code ec;
  std::filesystem::remove_all(runtime, ec);
  std::filesystem::create_directories(runtime, ec);
  ::setenv("XDG_RUNTIME_DIR", runtime.string().c_str(), 1);

  const std::size_t cap = microide::workspace::ControlChannelService::kMaxControlQueryEntries;
  microide::workspace::WorkspaceContext context;
  context.current_project_state.root = "/tmp/proj";
  auto& exec = context.current_project_state.debug_execution;
  exec.stopped = true;
  exec.frames.resize(cap + 5);  // more frames than a single query may return
  for (auto& frame : exec.frames) {
    frame.SetSource("/tmp/proj/a.cpp");
    frame.line = 0;
    frame.display_primary = "f";
  }

  microide::workspace::ControlChannelService service;
  service.Configure(context, microide::workspace::ControlChannelService::Operations{});
  service.SetWakeEventType(0);
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

  // Drain locally with a generous buffer/deadline: the capped response is still a
  // few hundred KiB, and the shared ExchangeLine helper's 3s deadline / 2 KiB reads
  // are too tight for that under sanitizers.
  const std::string request = std::string(R"({"id":1,"query":"debug-state"})") + "\n";
  ::send(fd, request.data(), request.size(), 0);
  std::string response;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
  while (std::chrono::steady_clock::now() < deadline &&
         response.find('\n') == std::string::npos) {
    service.ConsumeControlCallbacks();
    std::vector<char> buffer(65536);
    const ssize_t count = ::recv(fd, buffer.data(), buffer.size(), MSG_DONTWAIT);
    if (count > 0) {
      response.append(buffer.data(), static_cast<std::size_t>(count));
    } else {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }
  const auto json = util::ParseJson(response);
  Expect(json.has_value(), "debug-state response should be JSON");
  const util::JsonValue& result = (*json)["result"];
  Expect(result["frames"].AsArray().size() == cap,
         "the call-stack array must be capped at kMaxControlQueryEntries");
  Expect(result["framesTruncated"].AsBool(),
         "a truncated call stack should be flagged in the response");

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

// Instance discovery reads attacker-droppable descriptor files (world-writable
// on the /tmp fallback). A forged `socket` field must never redirect a driver's
// connection: enumeration reconstructs the canonical socket path from the
// validated pid and ignores the advertised value. A descriptor whose body pid
// disagrees with its <pid>.json filename is rejected outright.
void TestControlDiscoveryIgnoresForgedSocketAndPid() {
  const std::filesystem::path runtime =
      std::filesystem::temp_directory_path() /
      ("microide-control-forge-" + std::to_string(::getpid()));
  std::error_code ec;
  std::filesystem::remove_all(runtime, ec);
  ::setenv("XDG_RUNTIME_DIR", runtime.string().c_str(), 1);

  const std::filesystem::path instances = runtime / "microide" / "instances";
  std::filesystem::create_directories(instances, ec);
  const int alive_pid = static_cast<int>(::getpid());

  // (a) A live descriptor advertising a hostile socket path in its body.
  {
    std::ofstream out(instances / (std::to_string(alive_pid) + ".json"), std::ios::trunc);
    out << R"({"pid":)" << alive_pid
        << R"(,"socket":"/tmp/attacker-controlled.sock","project_root":"/p"})";
  }
  // (b) A descriptor whose filename claims a live pid but whose body pid disagrees
  //     (forged) — filename is a *different* live pid so ProcessIsAlive passes.
  const int other_alive_pid = static_cast<int>(::getppid());
  if (other_alive_pid > 0 && other_alive_pid != alive_pid) {
    std::ofstream out(instances / (std::to_string(other_alive_pid) + ".json"), std::ios::trunc);
    out << R"({"pid":)" << (other_alive_pid + 1) << R"(,"socket":"/tmp/x.sock"})";
  }

  const auto instances_list = microide::workspace::EnumerateControlInstances();
  const std::filesystem::path canonical =
      runtime / "microide" / (std::to_string(alive_pid) + ".sock");
  bool found_alive = false;
  for (const auto& descriptor : instances_list) {
    if (descriptor.pid == alive_pid) {
      found_alive = true;
      Expect(descriptor.socket == canonical,
             "the advertised socket field must be ignored in favor of the canonical path");
    }
    Expect(descriptor.pid != other_alive_pid + 1,
           "a descriptor whose body pid disagrees with its filename must be rejected");
  }
  Expect(found_alive, "the live, well-formed descriptor should still be discovered");

  std::filesystem::remove_all(runtime, ec);
}

// `control-list` is the surface a driver actually reads, and it prints one JSON
// object per line. Echoing the descriptor file body there would have handed back
// the forged `socket` the enumeration above deliberately discards, and a body
// containing a raw newline would have injected a second, wholly attacker-authored
// line into the JSONL stream. The listing is re-serialized from validated fields
// instead, so neither is possible.
void TestControlListPrintsCanonicalSingleLineJson() {
  const std::filesystem::path runtime =
      std::filesystem::temp_directory_path() /
      ("microide-control-listing-" + std::to_string(::getpid()));
  std::error_code ec;
  std::filesystem::remove_all(runtime, ec);
  ::setenv("XDG_RUNTIME_DIR", runtime.string().c_str(), 1);

  const std::filesystem::path instances = runtime / "microide" / "instances";
  std::filesystem::create_directories(instances, ec);
  const int alive_pid = static_cast<int>(::getpid());
  {
    // Pretty-printed (so the body spans several lines) AND advertising a hostile
    // socket. Both are valid JSON, so this parses and is accepted as live.
    std::ofstream out(instances / (std::to_string(alive_pid) + ".json"), std::ios::trunc);
    out << "{\n  \"pid\": " << alive_pid
        << ",\n  \"socket\": \"/tmp/attacker-controlled.sock\",\n"
        << "  \"project_root\": \"/p\"\n}";
  }

  const std::string listing = microide::workspace::ControlListInstancesText();
  Expect(listing.find("attacker-controlled.sock") == std::string::npos,
         "the listing must not echo a forged socket path back to the driver");
  Expect(listing.find(std::to_string(alive_pid)) != std::string::npos,
         "the live instance should still be listed");
  const std::size_t newlines =
      static_cast<std::size_t>(std::count(listing.begin(), listing.end(), '\n'));
  Expect(newlines == 1, "one accepted instance must produce exactly one JSONL line");
  Expect(listing.find((runtime / "microide" / (std::to_string(alive_pid) + ".sock"))
                          .generic_string()) != std::string::npos,
         "the listing should carry the canonical socket path");

  std::filesystem::remove_all(runtime, ec);
}

// An oversized descriptor file must be skipped before it is slurped, so a hostile
// local process cannot OOM `control-list`/`control-send` with one giant file.
void TestControlDiscoveryRejectsOversizedDescriptor() {
  const std::filesystem::path runtime =
      std::filesystem::temp_directory_path() /
      ("microide-control-oversize-" + std::to_string(::getpid()));
  std::error_code ec;
  std::filesystem::remove_all(runtime, ec);
  ::setenv("XDG_RUNTIME_DIR", runtime.string().c_str(), 1);

  const std::filesystem::path instances = runtime / "microide" / "instances";
  std::filesystem::create_directories(instances, ec);
  const int alive_pid = static_cast<int>(::getpid());

  // > 1 MiB body under a valid <pid>.json name.
  {
    std::ofstream out(instances / (std::to_string(alive_pid) + ".json"), std::ios::trunc);
    const std::string chunk(64 * 1024, 'x');
    for (int i = 0; i < 20; ++i) {  // ~1.25 MiB
      out << chunk;
    }
  }

  const auto instances_list = microide::workspace::EnumerateControlInstances();
  for (const auto& descriptor : instances_list) {
    Expect(descriptor.pid != alive_pid, "an oversized descriptor must not be enumerated");
  }

  std::filesystem::remove_all(runtime, ec);
}

// Regression (speed / local-DoS): the sweep bound must count directory entries
// EXAMINED, not instances accepted. A pid-mismatched descriptor is rejected but
// never pruned (unlike the dead-pid path), so a directory full of them used to
// grow `instances` by zero and never trip the `kMaxControlInstances` break — an
// unbounded scan, each entry formerly paying a 1 MiB zero-fill. Enumeration over a
// pile of never-pruned junk descriptors must still return promptly and surface the
// one valid live instance, and the junk must remain on disk (documenting exactly
// why the bound has to be entries-scanned, not results-accepted).
void TestControlDiscoveryBoundsHostileSweep() {
  const std::filesystem::path runtime =
      std::filesystem::temp_directory_path() /
      ("microide-control-sweep-" + std::to_string(::getpid()));
  std::error_code ec;
  std::filesystem::remove_all(runtime, ec);
  ::setenv("XDG_RUNTIME_DIR", runtime.string().c_str(), 1);

  const std::filesystem::path instances = runtime / "microide" / "instances";
  std::filesystem::create_directories(instances, ec);

  // A pile of small `<n>.json` descriptors whose body pid disagrees with the
  // filename: each clears the stem/size pre-checks, hits the pid-mismatch reject,
  // and is NOT pruned — the exact adversarial shape the sweep bound guards.
  const int base = 900000000;  // out of live-pid range, but that path is never reached
  for (int i = 0; i < 300; ++i) {
    const int name_pid = base + i;
    std::ofstream out(instances / (std::to_string(name_pid) + ".json"), std::ios::trunc);
    out << R"({"pid":)" << (name_pid + 7) << R"(,"socket":"/tmp/x.sock"})";
  }
  // One valid, live descriptor mixed in.
  const int alive_pid = static_cast<int>(::getpid());
  WriteDescriptor(instances, alive_pid);

  const auto instances_list = microide::workspace::EnumerateControlInstances();
  bool found_alive = false;
  for (const auto& descriptor : instances_list) {
    if (descriptor.pid == alive_pid) {
      found_alive = true;
    }
    Expect(descriptor.pid < base || descriptor.pid > base + 300,
           "no pid-mismatched junk descriptor should be accepted");
  }
  Expect(found_alive, "the one valid live descriptor must still be discovered amid the junk");
  // The junk is rejected but never pruned: it stays on disk, so only an
  // entries-examined bound (not the accepted-instance cap) keeps the sweep finite.
  Expect(std::filesystem::exists(instances / (std::to_string(base) + ".json")),
         "pid-mismatched descriptors are not pruned, so the scan must be bounded by entries examined");

  std::filesystem::remove_all(runtime, ec);
}

// Self-heal: some environments delete $XDG_RUNTIME_DIR contents while a process
// is still alive, silently severing the advertised socket. The I/O thread must
// detect the missing socket on its idle poll and re-bind a fresh listener at the
// same path; the host re-writes the discovery descriptor on the next drain. After
// the heal a new client must be able to connect and query.
void TestSocketSelfHealsAfterExternalDeletion() {
  const std::filesystem::path runtime =
      std::filesystem::temp_directory_path() /
      ("microide-control-selfheal-" + std::to_string(::getpid()));
  std::error_code ec;
  std::filesystem::remove_all(runtime, ec);
  std::filesystem::create_directories(runtime, ec);
  ::setenv("XDG_RUNTIME_DIR", runtime.string().c_str(), 1);

  microide::workspace::WorkspaceContext context;
  context.current_project_state.root = "/tmp/proj";

  microide::workspace::ControlChannelService service;
  service.Configure(
      context, microide::workspace::ControlChannelService::Operations{
                   .execute_command_line =
                       [](const std::string&) {
                         return microide::workspace::ControlChannelService::CommandOutcome{.ok =
                                                                                               true};
                       }});
  service.SetWakeEventType(0);  // no SDL push; the test polls / drains explicitly.
  Expect(service.Start("/tmp/proj"), "control service should start");

  const std::filesystem::path socket_path =
      runtime / "microide" / (std::to_string(::getpid()) + ".sock");
  const std::filesystem::path descriptor_path =
      runtime / "microide" / "instances" / (std::to_string(::getpid()) + ".json");
  Expect(std::filesystem::exists(descriptor_path), "the discovery descriptor is written at start");

  // A client connects before the deletion (sanity).
  int fd = ConnectUnix(socket_path.string());
  Expect(fd >= 0, "client connects to the original socket");
  ::close(fd);

  // Simulate the external runtime-dir cleanup: drop both the socket and descriptor
  // while the process keeps running.
  ::unlink(socket_path.string().c_str());
  std::filesystem::remove(descriptor_path, ec);
  Expect(!std::filesystem::exists(socket_path), "socket removed out from under the live process");

  // The I/O thread re-binds on its next idle poll (≤1s). Poll a fresh connect.
  int healed_fd = -1;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
  while (healed_fd < 0 && std::chrono::steady_clock::now() < deadline) {
    healed_fd = ConnectUnix(socket_path.string());
    if (healed_fd < 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
  }
  Expect(healed_fd >= 0, "a new client connects after the listener self-heals");

  // Draining the control callbacks observes the rebind and re-writes the descriptor.
  // The rebind (which let `healed_fd` connect) and the republish callback are posted
  // by the I/O thread independently, so the callback may not be enqueued at the exact
  // instant the connect succeeds. Poll-drain until the descriptor reappears rather
  // than draining once, so the test is deterministic instead of racing that window.
  bool descriptor_republished = false;
  const auto republish_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
  while (!descriptor_republished && std::chrono::steady_clock::now() < republish_deadline) {
    service.ConsumeControlCallbacks();
    if (std::filesystem::exists(descriptor_path)) {
      descriptor_republished = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  Expect(descriptor_republished,
         "the discovery descriptor is re-published after the rebind");

  // The healed listener still serves queries end-to-end.
  const auto status = util::ParseJson(ExchangeLine(service, healed_fd, R"({"id":1,"query":"status"})"));
  Expect(status.has_value() && (*status)["ok"].AsBool(),
         "the rebound socket answers queries");
  // The status payload reports kernel-confinement availability so silent fail-open is observable.
  const util::JsonValue& sandbox = (*status)["result"]["sandbox"];
  Expect(sandbox.IsObject(), "status result carries a sandbox object");
  Expect(sandbox["active"].IsBool(), "sandbox reports an active flag");
  Expect(sandbox["landlockAbi"].IsInt(), "sandbox reports a landlock ABI integer");

  ::close(healed_fd);
  service.Stop();
  std::filesystem::remove_all(runtime, ec);
}

// A breakpoint-/debug- command over the channel auto-enables the debugger (no
// `set-setting debug.enabled true` prelude); a non-debug command does not.
void TestDebugCommandAutoEnablesDebugger() {
  const std::filesystem::path runtime =
      std::filesystem::temp_directory_path() /
      ("microide-control-autoenable-" + std::to_string(::getpid()));
  std::error_code ec;
  std::filesystem::remove_all(runtime, ec);
  std::filesystem::create_directories(runtime, ec);
  ::setenv("XDG_RUNTIME_DIR", runtime.string().c_str(), 1);

  microide::workspace::WorkspaceContext context;
  context.current_project_state.root = "/tmp/proj";

  int ensure_calls = 0;
  microide::workspace::ControlChannelService service;
  service.Configure(
      context,
      microide::workspace::ControlChannelService::Operations{
          .execute_command_line =
              [](const std::string&) {
                return microide::workspace::ControlChannelService::CommandOutcome{.ok = true};
              },
          .ensure_debugger_enabled = [&ensure_calls]() { ++ensure_calls; },
      });
  service.SetWakeEventType(0);
  Expect(service.Start("/tmp/proj"), "control service should start");

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

  ExchangeLine(service, fd, R"({"id":1,"command":"breakpoint-function-add main"})");
  Expect(ensure_calls == 1, "a breakpoint- command should auto-enable the debugger");

  ExchangeLine(service, fd, R"({"id":2,"command":"debug-start"})");
  Expect(ensure_calls == 2, "a debug- command should auto-enable the debugger");

  ExchangeLine(service, fd, R"({"id":3,"command":"open /tmp/proj/a.cpp"})");
  Expect(ensure_calls == 2, "a non-debug command should not auto-enable the debugger");

  // ...but not the master switch. Auto-enabling before `debug-toggle-enabled`
  // made it impossible to turn the debugger OFF over the channel: the toggle read
  // "enabled" every time, wrote "false", reported "Debugger disabled", and the
  // next debug- command turned it straight back on. Verified against a live
  // headless instance — three toggles in a row logged the same
  // raw=true / wrote=1 / after=false, i.e. a no-op that always claimed to disable.
  ExchangeLine(service, fd, R"({"id":4,"command":"debug-toggle-enabled"})");
  Expect(ensure_calls == 2,
         "the debug.enabled master toggle must not be auto-enabled before it runs");

  // The prefix rule still holds for everything else that merely starts similarly.
  ExchangeLine(service, fd, R"({"id":5,"command":"debug-toggle-something-else"})");
  Expect(ensure_calls == 3, "other debug- commands should still auto-enable");

  ::close(fd);
  service.Stop();
  std::filesystem::remove_all(runtime, ec);
}

#else

void TestQueryAndCommandOverSocket() {}
void TestQueryResponseIsBounded() {}
void TestControlListFiltersDeadPids() {}
void TestLaunchConfigsAndAdaptersOverSocket() {}
void TestSocketSelfHealsAfterExternalDeletion() {}
void TestDebugCommandAutoEnablesDebugger() {}
void TestControlDiscoveryIgnoresForgedSocketAndPid() {}
void TestControlListPrintsCanonicalSingleLineJson() {}
void TestControlDiscoveryRejectsOversizedDescriptor() {}
void TestControlDiscoveryBoundsHostileSweep() {}

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
  service.OnDebugTerminated(1, "debug adapter exited unexpectedly");
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
  Expect((*terminated)["sessionId"].AsInt() == 1, "terminated event carries the real session id");
  Expect((*terminated)["reason"].AsString() == "debug adapter exited unexpectedly",
         "a non-clean end carries its reason so observers can distinguish a crash");

  // TD-2026-07-17A-096: an oversized DAP output event must be byte-capped before the JSON
  // event is built/serialized, with a truncation marker/flag, so it cannot force a large
  // allocation/escape pass and per-client write-buffer attempt on the control channel.
  emitted.clear();
  service.OnDebugOutput("stdout", std::string(1u * 1024 * 1024, 'z'));  // 1 MiB
  Expect(emitted.size() == 1, "an oversized output event still mirrors exactly one line");
  const auto capped = util::ParseJson(emitted[0]);
  Expect(capped.has_value(), "the capped output event must be valid JSON");
  Expect((*capped)["text"].AsString().size() < 128u * 1024,
         "the emitted text must be byte-capped well below the raw 1 MiB");
  Expect((*capped)["truncated"].AsBool(false),
         "a truncated output event must carry the truncated flag");
}

// Regression: the resolved `stopped` event must carry the populated execution view
// (reason/threadId/file/line/frames) and framesPending:false. The original bug
// fired this against an empty debug_execution, yielding reason="",threadId=0,
// frames=[] — this asserts the populated payload.
void TestStoppedEventCarriesPopulatedExecutionView() {
  microide::workspace::WorkspaceContext context;
  context.current_project_state.root = "/tmp/proj";
  auto& exec = context.current_project_state.debug_execution;
  exec.stopped = true;
  exec.thread_id = 7;
  exec.stop_reason = "breakpoint";
  exec.focused_frame_index = 0;
  microide::workspace::DebugStackFrameView frame;
  frame.SetSource("/tmp/proj/main.cpp");
  frame.line = 41;  // 0-based; the event reports 1-based (42)
  frame.display_primary = "main";
  exec.frames.push_back(frame);

  std::vector<std::string> emitted;
  microide::workspace::ControlChannelService service;
  service.Configure(context, microide::workspace::ControlChannelService::Operations{
                                 .emit_jsonl = [&emitted](const std::string& line) {
                                   emitted.push_back(line);
                                 }});
  service.SetStdoutMirror(true);
  service.OnDebugStopped();
  Expect(emitted.size() == 1, "the resolved stop should mirror one event");

  const auto stopped = util::ParseJson(emitted[0]);
  Expect(stopped.has_value(), "stopped event should parse");
  Expect((*stopped)["event"].AsString() == "stopped", "event should be 'stopped'");
  Expect((*stopped)["reason"].AsString() == "breakpoint", "reason should be populated");
  Expect((*stopped)["threadId"].AsInt() == 7, "threadId should be populated");
  Expect((*stopped)["file"].AsString() == "/tmp/proj/main.cpp", "file should be populated");
  Expect((*stopped)["line"].AsInt() == 42, "line should be 1-based and populated");
  Expect((*stopped)["frames"].AsArray().size() == 1, "frames should be populated");
  Expect((*stopped)["framesPending"].AsBool() == false,
         "resolved stop should report framesPending:false");
}

// The immediate stop event carries the real reason/thread with framesPending:true
// and no file/line/frames, even with an empty (not-yet-resolved) execution view.
void TestStopBeganEmitsImmediatePendingEvent() {
  microide::workspace::WorkspaceContext context;
  context.current_project_state.root = "/tmp/proj";
  // debug_execution intentionally left empty: the immediate phase fires before
  // ProjectStop rebuilds it.

  std::vector<std::string> emitted;
  microide::workspace::ControlChannelService service;
  service.Configure(context, microide::workspace::ControlChannelService::Operations{
                                 .emit_jsonl = [&emitted](const std::string& line) {
                                   emitted.push_back(line);
                                 }});
  service.SetStdoutMirror(true);
  service.OnDebugStopBegan("breakpoint", 1);
  Expect(emitted.size() == 1, "stop-began should mirror one event");

  const auto event = util::ParseJson(emitted[0]);
  Expect(event.has_value(), "stop-began event should parse");
  Expect((*event)["event"].AsString() == "stopped", "event should be 'stopped'");
  Expect((*event)["reason"].AsString() == "breakpoint", "reason should be carried");
  Expect((*event)["threadId"].AsInt() == 1, "threadId should be carried");
  Expect((*event)["framesPending"].AsBool() == true, "immediate stop is framesPending:true");
  Expect(!(*event).AsObject().count("file"), "immediate stop omits file");
  Expect(!(*event).AsObject().count("frames"), "immediate stop omits frames");
}

}  // namespace

void RegisterControlChannelServiceTests(std::vector<TestCase>& tests) {
  AddTest(tests, "ControlChannelService/QueryAndCommandOverSocket",
          TestQueryAndCommandOverSocket);
  AddTest(tests, "ControlChannelService/QueryResponseIsBounded",
          TestQueryResponseIsBounded);
  AddTest(tests, "ControlChannelService/ControlListFiltersDeadPids",
          TestControlListFiltersDeadPids);
  AddTest(tests, "ControlChannelService/LaunchConfigsAndAdaptersOverSocket",
          TestLaunchConfigsAndAdaptersOverSocket);
  AddTest(tests, "ControlChannelService/StdoutMirrorEmitsWithoutConnections",
          TestStdoutMirrorEmitsWithoutConnections);
  AddTest(tests, "ControlChannelService/StoppedEventCarriesPopulatedExecutionView",
          TestStoppedEventCarriesPopulatedExecutionView);
  AddTest(tests, "ControlChannelService/StopBeganEmitsImmediatePendingEvent",
          TestStopBeganEmitsImmediatePendingEvent);
  AddTest(tests, "ControlChannelService/SocketSelfHealsAfterExternalDeletion",
          TestSocketSelfHealsAfterExternalDeletion);
  AddTest(tests, "ControlChannelService/DebugCommandAutoEnablesDebugger",
          TestDebugCommandAutoEnablesDebugger);
  AddTest(tests, "ControlChannelService/DiscoveryIgnoresForgedSocketAndPid",
          TestControlDiscoveryIgnoresForgedSocketAndPid);
  AddTest(tests, "ControlChannelService/ListPrintsCanonicalSingleLineJson",
          TestControlListPrintsCanonicalSingleLineJson);
  AddTest(tests, "ControlChannelService/DiscoveryRejectsOversizedDescriptor",
          TestControlDiscoveryRejectsOversizedDescriptor);
  AddTest(tests, "ControlChannelService/DiscoveryBoundsHostileSweep",
          TestControlDiscoveryBoundsHostileSweep);
}

}  // namespace microide::tests
