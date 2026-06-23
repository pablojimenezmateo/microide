#include "TestSupport.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "TerminalSessionTestAccess.h"
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

  ::close(fd);
  service.Stop();
  std::filesystem::remove_all(runtime, ec);
}

// The editor / commands / terminal-output queries surface workspace state for an
// agent: editor groups + layout + per-tab cursor, the runnable command registry,
// and a synchronous snapshot of a terminal's scrollback.
void TestEditorTerminalAndCommandQueries() {
  const std::filesystem::path runtime =
      std::filesystem::temp_directory_path() /
      ("microide-control-editor-" + std::to_string(::getpid()));
  std::error_code ec;
  std::filesystem::remove_all(runtime, ec);
  std::filesystem::create_directories(runtime, ec);
  ::setenv("XDG_RUNTIME_DIR", runtime.string().c_str(), 1);

  microide::workspace::WorkspaceContext context;
  microide::workspace::ProjectWorkspaceState& state = context.current_project_state;
  state.root = "/tmp/proj";

  const auto make_editor_tab = [](const std::string& path, const std::string& content,
                                  std::size_t cursor_line) {
    microide::workspace::TabEntry tab;
    tab.kind = microide::workspace::TabEntry::Kind::Editor;
    tab.path = path;
    tab.title = std::filesystem::path(path).filename().string();
    microide::workspace::TabEntry::EditorTabState editor_state;
    editor_state.viewport.SetViewportSize(10, 80);
    editor_state.viewport.LoadContent(content, path);
    editor_state.viewport.MoveCursorTo(cursor_line, 0);
    tab.editor_state = std::move(editor_state);
    return tab;
  };

  // Two editor groups (a vertical split). Group 0 holds a loaded tab (cursor on
  // 0-based line 2) plus a deferred tab whose position comes from its handle.
  state.editor_groups.clear();
  state.editor_groups.resize(2);
  state.editor_groups[0].open_tabs.push_back(
      make_editor_tab("/tmp/proj/a.cpp", "l1\nl2\nl3\nl4\nl5\n", 2));
  {
    microide::workspace::TabEntry deferred;
    deferred.kind = microide::workspace::TabEntry::Kind::Editor;
    deferred.path = "/tmp/proj/b.cpp";
    deferred.title = "b.cpp";
    microide::workspace::TabEntry::DeferredTabHandle handle;
    handle.path = "/tmp/proj/b.cpp";
    handle.cursor_line = 5;
    handle.cursor_column = 2;
    handle.scroll_line = 4;
    deferred.deferred_handle = handle;
    state.editor_groups[0].open_tabs.push_back(std::move(deferred));
  }
  state.editor_groups[0].active_tab_index = 0;
  state.editor_groups[1].open_tabs.push_back(make_editor_tab("/tmp/proj/c.cpp", "x\ny\n", 1));
  state.editor_groups[1].active_tab_index = 0;
  state.focused_group_index = 0;
  state.group_split_orientation = microide::workspace::EditorSplitOrientation::Vertical;
  state.group_split_fraction = 0.5f;

  // A terminal with two buffered lines, no longer running.
  auto terminal = std::make_unique<microide::workspace::TerminalTabState>();
  TerminalSessionTestAccess::Reset(terminal->session, 24, 80);
  TerminalSessionTestAccess::AppendOutput(terminal->session, "hello\r\nworld\r\n");
  TerminalSessionTestAccess::SetRunning(terminal->session, false);
  state.terminal_tabs.push_back(std::move(terminal));
  state.active_terminal_tab_index = 0;

  microide::workspace::ControlChannelService service;
  service.Configure(
      context, microide::workspace::ControlChannelService::Operations{
                   .execute_command_line =
                       [](const std::string&) {
                         return microide::workspace::ControlChannelService::CommandOutcome{
                             .ok = true};
                       },
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

  // editor query
  const auto editor = util::ParseJson(ExchangeLine(service, fd, R"({"query":"editor"})"));
  Expect(editor.has_value() && (*editor)["ok"].AsBool(), "editor query should succeed");
  const util::JsonValue& result = (*editor)["result"];
  Expect(result["focusedGroupIndex"].AsInt() == 0, "focused group index should be 0");
  Expect(result["splitOrientation"].AsString() == "vertical", "split orientation should surface");
  const util::JsonValue& groups = result["groups"];
  Expect(groups.IsArray() && groups.AsArray().size() == 2, "two editor groups expected");
  const util::JsonValue& group0_tabs = groups.AsArray()[0]["tabs"];
  Expect(groups.AsArray()[0]["focused"].AsBool(), "group 0 should report focused");
  Expect(group0_tabs.AsArray().size() == 2, "group 0 should have two tabs");
  const util::JsonValue& tab0 = group0_tabs.AsArray()[0];
  Expect(tab0["active"].AsBool(), "tab 0 should be active");
  Expect(tab0["cursorLine"].AsInt() == 3, "0-based cursor line 2 should report as 1-based 3");
  Expect(!tab0["dirty"].AsBool(), "an unedited tab should not be dirty");
  Expect(tab0.AsObject().count("visibleCount") == 1,
         "the focused active tab should report its visible range");
  const util::JsonValue& tab1 = group0_tabs.AsArray()[1];
  Expect(tab1["cursorLine"].AsInt() == 6, "deferred tab should report its handle cursor (1-based)");

  // commands query
  const auto commands = util::ParseJson(ExchangeLine(service, fd, R"({"query":"commands"})"));
  Expect(commands.has_value() && (*commands)["ok"].AsBool(), "commands query should succeed");
  const util::JsonValue& command_list = (*commands)["result"];
  Expect(command_list.IsArray() && !command_list.AsArray().empty(), "command list should be non-empty");
  bool found_reveal = false;
  for (const util::JsonValue& command : command_list.AsArray()) {
    Expect(!command["command"].AsString().empty(),
           "context-menu-only specs (empty command) should be excluded");
    if (command["command"].AsString() == "reveal") {
      found_reveal = true;
      Expect(command["usage"].AsString().find("reveal") != std::string::npos,
             "reveal usage should be surfaced");
    }
  }
  Expect(found_reveal, "the reveal command should be listed");

  // terminal-output query (default tab)
  const auto term = util::ParseJson(ExchangeLine(service, fd, R"({"query":"terminal-output"})"));
  Expect(term.has_value() && (*term)["ok"].AsBool(), "terminal-output query should succeed");
  const util::JsonValue& term_result = (*term)["result"];
  Expect(!term_result["running"].AsBool(), "the terminal should report not running");
  const std::string text = term_result["text"].AsString();
  Expect(text.find("hello") != std::string::npos && text.find("world") != std::string::npos,
         "terminal-output should carry the buffered scrollback");

  // terminal-output query (out-of-range tab → failure)
  const auto bad =
      util::ParseJson(ExchangeLine(service, fd, R"({"query":"terminal-output","args":{"tab":9}})"));
  Expect(bad.has_value() && !(*bad)["ok"].AsBool(),
         "an out-of-range terminal tab index should fail");

  ::close(fd);
  service.Stop();
  std::filesystem::remove_all(runtime, ec);
}

#else

void TestQueryAndCommandOverSocket() {}
void TestControlListFiltersDeadPids() {}
void TestLaunchConfigsAndAdaptersOverSocket() {}
void TestSocketSelfHealsAfterExternalDeletion() {}
void TestDebugCommandAutoEnablesDebugger() {}
void TestEditorTerminalAndCommandQueries() {}

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
  frame.source_path = "/tmp/proj/main.cpp";
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
  AddTest(tests, "ControlChannelService/EditorTerminalAndCommandQueries",
          TestEditorTerminalAndCommandQueries);
}

}  // namespace microide::tests
