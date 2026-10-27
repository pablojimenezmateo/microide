#include "TestSupport.h"
#include "RemoteServerTestSupport.h"

#include "platform/UnixSocket.h"
#include "project/remote/RemoteProtocol.h"
#include "project/remote/RemoteServerClient.h"
#include "project/remote/RemoteServerPaths.h"
#include "terminal/TerminalSession.h"
#include "util/JsonValue.h"
#include "workspace/services/RemoteHostActivityRows.h"
#include "workspace/services/RemoteHostService.h"
#include "workspace/shell/WorkspaceShellTestAccess.h"

#include <SDL3/SDL.h>

#include <chrono>
#include <csignal>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>

// remote-host-activity's Host view over the fake ssh host: Remote: Show Status
// opens it, its rows carry what the host's terminals and processes cost, Enter and
// Delete act on them, and hiding it stops the host's pushes.
namespace microide::tests {
namespace {

namespace remote = project::remote;
using workspace::WorkspaceShell;
using WorkspaceShellTestAccess = workspace::WorkspaceShell::TestAccess;
using Item = plugin::PluginHost::SidebarItem;

void Pump(WorkspaceShell& shell) {
  (void)WorkspaceShellTestAccess::ReloadProjectIfFilesChanged(shell, false);
  WorkspaceShellTestAccess::ConsumeTerminalSessionUpdates(shell);
}

bool PumpUntil(WorkspaceShell& shell, const std::function<bool()>& done,
               std::chrono::seconds timeout = std::chrono::seconds(30)) {
  return WaitUntil(
      [&] {
        Pump(shell);
        return done();
      },
      timeout, std::chrono::milliseconds(5));
}

void PointAtFakeHost(WorkspaceShell& shell, const FakeSshHost& host) {
  std::string ssh;
  for (const std::string& word : host.SshArgv()) {
    ssh += (ssh.empty() ? "" : " ") + word;
  }
  Expect(WorkspaceShellTestAccess::SetSettingValueTransient(shell, "remote.ssh_command", ssh),
         "the ssh seam is set");
  Expect(WorkspaceShellTestAccess::SetSettingValueTransient(shell, "remote.auto_forward_ports", "false"),
         "no automatic forwards");
}

// A connection of the test's own to the host's server, under `root`.
std::shared_ptr<remote::RemoteServerClient> ConnectRaw(const FakeSshHost& host, const std::filesystem::path& root) {
  auto client = remote::RemoteServerClient::Create();
  const int fd = platform::ConnectUnixSocket(remote::ServerSocketPath(host.server_dir()));
  std::string error;
  Expect(fd >= 0 && client->ConnectFds(fd, fd, remote::HelloRequest{.release = "test", .root = root.string()},
                                       &error),
         "a raw connection to the host's server: " + error);
  return client;
}

const Item* RowWithId(WorkspaceShell& shell, std::string_view id) {
  for (const Item& item : WorkspaceShellTestAccess::PluginSidebarItems(shell)) {
    if (item.id == id) {
      return &item;
    }
  }
  return nullptr;
}

std::string Rows(WorkspaceShell& shell) {
  std::string all;
  for (const Item& item : WorkspaceShellTestAccess::PluginSidebarItems(shell)) {
    all += "\n  " + std::string(static_cast<std::size_t>(item.depth) * 2, ' ') + item.id + " | " + item.label +
           " | " + item.detail;
  }
  return all;
}

bool SelectRow(WorkspaceShell& shell, std::string_view id) {
  const auto& items = WorkspaceShellTestAccess::PluginSidebarItems(shell);
  for (std::size_t i = 0; i < items.size(); ++i) {
    if (items[i].id == id) {
      WorkspaceShellTestAccess::SetPluginSidebarSelectedIndex(shell, i);
      return true;
    }
  }
  return false;
}

void PressKey(WorkspaceShell& shell, SDL_Keycode key) {
  SDL_Event event{};
  event.type = SDL_EVENT_KEY_DOWN;
  event.key.key = key;
  event.key.down = true;
  (void)shell.HandleEvent(event);
}

// What the CPU figure in a row's detail says, in per mille; nullopt for "…".
std::optional<double> CpuOf(const Item& row) {
  const std::size_t percent = row.detail.find('%');
  if (percent == std::string::npos) {
    return std::nullopt;
  }
  std::size_t start = percent;
  while (start > 0 && (std::isdigit(static_cast<unsigned char>(row.detail[start - 1])) != 0 ||
                       row.detail[start - 1] == '.')) {
    --start;
  }
  return std::stod(row.detail.substr(start, percent - start)) * 10.0;
}

// The proposal's scenario end to end: a project with a terminal running `yes` and
// a kept tool. Show Status opens the view with both; the terminal's CPU rises on
// a push; Delete ends each on the host (the running ones after a confirm); hiding
// the view leaves the host with no subscriber and the rows still.
void TestHostViewShowsAndEndsWhatTheHostRuns() {
  FakeSshHost host;
  const std::filesystem::path host_root = host.home / "src" / "activity";
  WriteFile(host_root / "a.txt", "a\n");
  WorkspaceShell shell;
  PointAtFakeHost(shell, host);
  const std::string target = "dev@fake-activity-" + std::to_string(::getpid());
  Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, "remote-open " + target + ":" + host_root.string()),
         "the remote project opens");
  terminal::TerminalSession& session = WorkspaceShellTestAccess::TerminalTabSession(shell, 0);
  Expect(PumpUntil(shell, [&] { return session.running() && session.host_identity().handle != 0; }),
         "its terminal starts on the host");
  const std::uint64_t terminal_handle = session.host_identity().handle;
  session.SendBytes("yes > /dev/null\n");

  // A tool the project left running and kept: spawned by a connection that then dropped.
  std::uint64_t tool_handle = 0;
  int tool_pid = -1;
  {
    auto tool_client = ConnectRaw(host, host_root);
    const remote::RemoteServerClient::Spawned spawned =
        tool_client->Spawn({"sleep", "300"}, host_root, {}, /*keep_on_detach=*/true, {});
    Expect(spawned.handle != 0, "the tool spawns: " + spawned.error);
    tool_handle = spawned.handle;
    tool_pid = spawned.pid;
    tool_client->peer().Fail("link dropped");
  }

  const auto shown = WorkspaceShellTestAccess::ExecuteControlCommand(shell, "remote-status");
  Expect(shown.ok, "Show Status runs: " + shown.error);
  Expect(WorkspaceShellTestAccess::SidebarViewId(shell) == "host", "and opens the Host view");
  Expect(shown.feedback.find(target) != std::string::npos, "the control channel still gets the text: " + shown.feedback);
  const std::string terminal_id = "terminal:" + std::to_string(terminal_handle);
  const std::string tool_id = "process:" + std::to_string(tool_handle);
  Expect(PumpUntil(shell,
                   [&] {
                     const Item* terminal = RowWithId(shell, terminal_id);
                     const Item* tool = RowWithId(shell, tool_id);
                     return terminal != nullptr && tool != nullptr && CpuOf(*terminal).value_or(0) > 100 &&
                            tool->detail.rfind("kept", 0) == 0;
                   }),
         "the terminal running yes uses CPU, the tool is kept:" + Rows(shell));
  Expect(RowWithId(shell, "group:server") != nullptr && RowWithId(shell, "server") != nullptr &&
             RowWithId(shell, "workspace:" + host_root.string()) != nullptr,
         "with the server and the workspace:" + Rows(shell));
  Expect(RowWithId(shell, tool_id)->label == "sleep 300", "the tool named by its argv");
  Expect(RowWithId(shell, "workspace:" + host_root.string())->detail.find("this window") != std::string::npos,
         "the workspace is this window's");
  Expect(WorkspaceShellTestAccess::RemoteHosts(shell).StatusText().find(target + " server") != std::string::npos,
         "Show Status's text carries the host's totals: " + WorkspaceShellTestAccess::RemoteHosts(shell).StatusText());

  // Collapse and expand a group.
  Expect(SelectRow(shell, "group:processes"), "the Processes group");
  PressKey(shell, SDLK_LEFT);
  Expect(RowWithId(shell, tool_id) == nullptr, "collapsing hides its rows");
  PressKey(shell, SDLK_RIGHT);
  Expect(RowWithId(shell, tool_id) != nullptr, "expanding shows them again");

  // Enter on the terminal: the tab that holds it, not a second one.
  const std::size_t tabs = WorkspaceShellTestAccess::TerminalTabCount(shell);
  Expect(SelectRow(shell, terminal_id), "the terminal row");
  PressKey(shell, SDLK_RETURN);
  Expect(WorkspaceShellTestAccess::TerminalTabCount(shell) == tabs, "its tab is focused, none added");
  Expect(WorkspaceShellTestAccess::FocusIsPanel(shell), "with the keyboard in it");
  WorkspaceShellTestAccess::SetFocusSidebar(shell);  // back to the view, as a click on it would

  // Delete on the running terminal asks first, then ends it.
  Expect(SelectRow(shell, terminal_id), "the terminal row again");
  PressKey(shell, SDLK_DELETE);
  Expect(WorkspaceShellTestAccess::PromptSurfaceVisible(shell) &&
             WorkspaceShellTestAccess::PromptSurfaceMessage(shell).find("End terminal #") == 0,
         "a confirm: " + WorkspaceShellTestAccess::PromptSurfaceMessage(shell));
  WorkspaceShellTestAccess::ConfirmPromptSurface(shell);
  Expect(PumpUntil(shell, [&] { return RowWithId(shell, terminal_id) == nullptr; }),
         "the terminal ends on the host and leaves the view:" + Rows(shell));

  // Delete on the kept tool: confirm, proc/end, gone from the host.
  Expect(SelectRow(shell, tool_id), "the tool row");
  PressKey(shell, SDLK_DELETE);
  Expect(WorkspaceShellTestAccess::PromptSurfaceVisible(shell) &&
             WorkspaceShellTestAccess::PromptSurfaceMessage(shell).find("End sleep 300 (pid ") == 0,
         "a confirm naming it: " + WorkspaceShellTestAccess::PromptSurfaceMessage(shell));
  WorkspaceShellTestAccess::ConfirmPromptSurface(shell);
  Expect(PumpUntil(shell, [&] { return RowWithId(shell, tool_id) == nullptr && ::kill(tool_pid, 0) != 0; }),
         "the tool ends and leaves the view:" + Rows(shell));

  // Hide it: the host keeps no subscriber, and nothing changes the rows any more.
  const auto hidden = WorkspaceShellTestAccess::ExecuteControlCommand(shell, "sidebar-show tree");
  Expect(hidden.ok && WorkspaceShellTestAccess::SidebarViewId(shell) == "tree", "the tree replaces it");
  auto observer = ConnectRaw(host, host_root);
  std::string error;
  Expect(WaitUntil(
             [&] {
               const std::optional<remote::ServerStatus> status = observer->Status(&error);
               return status.has_value() && status->activity_subscribers == 0;
             },
             std::chrono::seconds(10)),
         "the host has no activity subscriber once the view hides");
  std::this_thread::sleep_for(remote::kActivityInterval * 2 + std::chrono::milliseconds(500));
  const std::optional<remote::ServerStatus> later = observer->Status(&error);
  Expect(later.has_value() && later->activity_subscribers == 0, "and gains none while it stays hidden");
}

// The rows from a reply an older server sends: no processes, no figures, a
// terminal count instead of rows -- what it has is shown, the rest says so.
void TestHostRowsFromAnOlderServer() {
  const std::optional<util::JsonValue> json = util::ParseJson(R"({
    "release": "2.10.0", "daemon_epoch": "e",
    "workspaces": [{"root": "/srv/p", "clients": 1, "terminals": 1, "processes": 0}],
    "terminals": [{"handle": 4, "root": "/srv/p", "cwd": "/srv/p/src", "clients": 0, "exited": false}],
    "processes": 2
  })");
  Expect(json.has_value(), "fixture parses");
  const remote::ServerStatus status = remote::ServerStatusFromJson(*json);
  const std::set<std::string, std::less<>> collapsed;
  const std::vector<Item> rows = workspace::BuildHostActivityRows(workspace::HostActivityInput{
      .host = "box",
      .connection_state = "connected",
      .server_release = "2.10.0",
      .status = &status,
      .own_roots = {"/srv/p"},
      .collapsed = &collapsed,
  });
  const auto find = [&](std::string_view id) -> const Item* {
    for (const Item& row : rows) {
      if (row.id == id) {
        return &row;
      }
    }
    return nullptr;
  };
  Expect(find("server") != nullptr && find("server")->label == "microide-server 2.10.0" &&
             find("server")->detail.find("\xe2\x80\xa6") != std::string::npos,
         "the server row, its figures not reported");
  Expect(find("terminal:4") != nullptr && find("terminal:4")->label == "#4 \xc2\xb7 src" &&
             find("terminal:4")->detail.rfind("\xe2\x80\xa6 \xc2\xb7 \xe2\x80\xa6 \xc2\xb7 no tab", 0) == 0,
         "the terminal row, figures not reported: " + (find("terminal:4") ? find("terminal:4")->detail : ""));
  Expect(find("group:processes") != nullptr && find("group:processes")->detail == "not reported",
         "processes not reported, rather than none");
  Expect(workspace::ParseHostRowId("terminal:4")->kind == workspace::HostRowId::Kind::Terminal &&
             !workspace::ParseHostRowId("terminal:-1").has_value() && !workspace::ParseHostRowId("bogus").has_value(),
         "row ids parse, and a hostile one does not");
  const std::vector<Item> before_any = workspace::BuildHostActivityRows(
      workspace::HostActivityInput{.host = "box", .connection_state = "reconnecting"});
  Expect(before_any.size() == 2 && before_any[1].detail == "reconnecting",
         "before any status: the connection's own row");
}

}  // namespace

void RegisterRemoteHostViewTests(std::vector<TestCase>& tests) {
  AddTest(tests, "RemoteHostView/ShowsAndEndsWhatTheHostRuns", TestHostViewShowsAndEndsWhatTheHostRuns);
  AddTest(tests, "RemoteHostView/RowsFromAnOlderServer", TestHostRowsFromAnOlderServer);
}

}  // namespace microide::tests
