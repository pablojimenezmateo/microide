#include "TestSupport.h"
#include "RemoteServerTestSupport.h"

#include "platform/UnixSocket.h"
#include "project/remote/RemoteProtocol.h"
#include "project/remote/RemoteServerClient.h"
#include "project/remote/RemoteServerPaths.h"
#include "project/remote/RemoteTerminalChannel.h"
#include "terminal/TerminalSession.h"
#include "workspace/services/RemoteHostService.h"
#include "workspace/shell/WorkspaceShellTestAccess.h"

#include <algorithm>
#include <chrono>
#include <functional>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <unistd.h>

// remote-terminal-restore end to end, over the fake ssh host: a remote project's
// terminals exist from the moment it opens, survive quitting microide and closing
// the project, come back as the same host shells on reopen (or fresh ones, said
// so), and a host terminal no tab claims is offered.
namespace microide::tests {
namespace {

namespace remote = project::remote;
using workspace::WorkspaceShell;
using WorkspaceShellTestAccess = workspace::WorkspaceShell::TestAccess;
using terminal::TerminalSession;

std::string ScreenText(const TerminalSession& session) {
  std::string text;
  for (const auto& line : session.SnapshotLines()) {
    for (const auto& cell : line.cells) {
      text += cell.DisplayText().empty() ? std::string(" ") : std::string(cell.DisplayText());
    }
    text += '\n';
  }
  return text;
}

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

// What the host's server lists, asked over a connection of the test's own (it
// is attached to nothing, so it never counts as a terminal's client).
std::vector<remote::ServerTerminal> HostTerminals(const FakeSshHost& host) {
  const int fd = platform::ConnectUnixSocket(remote::ServerSocketPath(host.server_dir()));
  if (fd < 0) {
    return {};
  }
  auto client = remote::RemoteServerClient::Create();
  std::string error;
  if (!client->ConnectFds(fd, fd, remote::HelloRequest{.release = "test"}, &error)) {
    return {};
  }
  const std::optional<remote::ServerStatus> status = client->Status(&error);
  return status.has_value() && status->terminals.has_value() ? *status->terminals
                                                             : std::vector<remote::ServerTerminal>{};
}

const remote::ServerTerminal* Listed(const std::vector<remote::ServerTerminal>& rows, std::uint64_t handle) {
  for (const remote::ServerTerminal& row : rows) {
    if (row.handle == handle) {
      return &row;
    }
  }
  return nullptr;
}

const workspace::NotificationService::Notification* RowContaining(WorkspaceShell& shell,
                                                                   std::string_view text) {
  for (const auto& row : WorkspaceShellTestAccess::ActiveNotifications(shell)) {
    if (row.message.find(text) != std::string::npos) {
      return &row;
    }
  }
  return nullptr;
}

// State and config homes of the test's own: sessions are written and read back.
struct ScopedAppHomes {
  TemporaryDirectory dir;
  ScopedEnvVar state{"XDG_STATE_HOME", (dir.path() / "state").string()};
  ScopedEnvVar config{"XDG_CONFIG_HOME", (dir.path() / "config").string()};
};

std::string Target(std::string_view name) { return "dev@" + std::string(name) + "-" + std::to_string(::getpid()); }

// Gap 1 of the proposal: the project initializes while its link is still
// connecting, and its terminal used to be dropped. Now it is a pending tab from
// the moment the project opens, a `+` meanwhile adds another, typing into them is
// dropped, and both become host shells once the link is Ready -- no tab added or
// removed.
void TestARemoteProjectOpensWithItsTerminal() {
  ScopedAppHomes homes;
  FakeSshHost host;
  host.master_delay = "2";
  const std::filesystem::path host_root = host.home / "src" / "opens";
  WriteFile(host_root / "a.txt", "a\n");
  WorkspaceShell shell;
  PointAtFakeHost(shell, host);
  const std::string target = Target("fake-opens");
  Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, "remote-open " + target + ":" + host_root.string()),
         "the remote project opens");
  Expect(WorkspaceShellTestAccess::TerminalTabCount(shell) == 1, "with its terminal tab, at once");
  TerminalSession& first = WorkspaceShellTestAccess::TerminalTabSession(shell, 0);
  Expect(first.host_pending() && !first.running(), "waiting for the connection");
  Expect(PumpUntil(shell, [&] { return ScreenText(first).find("connecting to " + target) != std::string::npos; },
                   std::chrono::seconds(10)),
         "and saying so:\n" + ScreenText(first));
  Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, "term"), "+ while connecting");
  Expect(WorkspaceShellTestAccess::TerminalTabCount(shell) == 2, "a second tab, at once");
  TerminalSession& second = WorkspaceShellTestAccess::TerminalTabSession(shell, 1);
  Expect(second.host_pending(), "pending too");
  Expect(!WorkspaceShellTestAccess::ExecuteCommandLine(shell, "term-relaunch"),
         "a pending tab is not an exited one: nothing to relaunch");
  second.SendBytes("echo TYPED-WHILE-PENDING\n");

  Expect(PumpUntil(shell, [&] { return first.running() && second.running(); }),
         "both start once the link is Ready:\n" + ScreenText(first));
  Expect(WorkspaceShellTestAccess::TerminalTabCount(shell) == 2, "no tab added or removed");
  for (TerminalSession* session : {&first, &second}) {
    Expect(session->is_host_terminal(), "a host terminal");
    session->SendBytes("echo ON-HOST-$((40+2))\n");
    Expect(PumpUntil(shell, [&] { return ScreenText(*session).find("ON-HOST-42") != std::string::npos; },
                     std::chrono::seconds(15)),
           "its shell runs on the host:\n" + ScreenText(*session));
  }
  Expect(ScreenText(second).find("TYPED-WHILE-PENDING") == std::string::npos,
         "what was typed while pending never reached a shell");
}

// Quit and close-project DETACH (gap 3); reopening REATTACHES from the session's
// recorded identity (gaps 2 and 4) -- the screen, what was printed meanwhile, the
// rule -- and the host server restarting in between gives fresh shells, said so in
// the host's row. Quit reaches an inactive project's terminals too, in this test
// binary whose destructors run at quit (quick_exit is off here).
void TestHostTerminalsSurviveQuitAndReattach() {
  ScopedAppHomes homes;
  FakeSshHost host;
  const std::filesystem::path first_root = host.home / "src" / "keep";
  const std::filesystem::path second_root = host.home / "src" / "other";
  WriteFile(first_root / "a.txt", "a\n");
  WriteFile(second_root / "b.txt", "b\n");
  const std::string target = Target("fake-keep");
  const std::string open_first = "remote-open " + target + ":" + first_root.string();
  const std::string open_second = "remote-open " + target + ":" + second_root.string();

  std::uint64_t kept = 0;
  std::uint64_t inactive = 0;
  {
    WorkspaceShell shell;
    PointAtFakeHost(shell, host);
    Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, open_second), "a second remote project");
    TerminalSession& other = WorkspaceShellTestAccess::TerminalTabSession(shell, 0);
    Expect(PumpUntil(shell, [&] { return other.running(); }), "its terminal starts");
    Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, open_first), "the first project, now active");
    TerminalSession& session = WorkspaceShellTestAccess::TerminalTabSession(shell, 0);
    Expect(PumpUntil(shell, [&] { return session.running(); }), "its terminal starts");
    session.SendBytes("echo BEFORE-$((6*7)); sleep 2; echo AFTER-$((7*7))\n");
    Expect(PumpUntil(shell, [&] { return ScreenText(session).find("BEFORE-42") != std::string::npos; }),
           "the shell answers:\n" + ScreenText(session));
    kept = session.host_identity().handle;
    inactive = other.host_identity().handle;
    Expect(kept != 0 && inactive != 0 && kept != inactive, "two host terminals");
    shell.Shutdown();  // quit
  }  // and every destructor runs, as the shipping binary's quick_exit would not
  Expect(WaitUntil(
             [&] {
               const auto rows = HostTerminals(host);
               const auto* a = Listed(rows, kept);
               const auto* b = Listed(rows, inactive);
               return a != nullptr && a->clients == 0 && b != nullptr && b->clients == 0;
             },
             std::chrono::seconds(10)),
         "both shells -- the active project's and the inactive one's -- still run on the host, "
         "attached to nobody");

  {
    WorkspaceShell shell;
    PointAtFakeHost(shell, host);
    Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, open_first), "reopen the first project");
    Expect(WorkspaceShellTestAccess::TerminalTabCount(shell) == 1, "its tab is there at once");
    TerminalSession& session = WorkspaceShellTestAccess::TerminalTabSession(shell, 0);
    Expect(session.host_identity().handle == kept, "naming the recorded host terminal");
    Expect(PumpUntil(shell, [&] { return ScreenText(session).find("AFTER-49") != std::string::npos; }),
           "the same shell: what it printed after the quit is on screen:\n" + ScreenText(session));
    const std::string text = ScreenText(session);
    Expect(text.find("BEFORE-42") != std::string::npos, "and what it printed before");
    Expect(text.find("\xe2\x8b\xaf reattached \xe2\x8b\xaf") != std::string::npos, "under a reattached rule");
    Expect(PumpUntil(shell, [&] { return RowContaining(shell, "Reattached to " + target) != nullptr; },
                     std::chrono::seconds(10)),
           "one row says so");
    // Close the project (not the app): its shell stays.
    WorkspaceShellTestAccess::CloseProject(shell, WorkspaceShellTestAccess::ActiveProjectIndex(shell));
    Expect(WaitUntil(
               [&] {
                 const auto* row = Listed(HostTerminals(host), kept);
                 return row != nullptr && row->clients == 0;
               },
               std::chrono::seconds(10)),
           "close-project leaves the shell running, attached to nobody");
  }

  // The host server restarted between the quit and the reopen: the handles name
  // nothing now. The tab starts fresh in its directory, and the row says why.
  Expect(platform::RunSubprocess({MICROIDE_SERVER_BINARY, "stop", "--socket-dir", host.server_dir().string()},
                                 platform::SubprocessOptions{.timeout_ms = 10000})
             .success(),
         "the host server stops");
  {
    WorkspaceShell shell;
    PointAtFakeHost(shell, host);
    Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, open_first), "reopen once more");
    TerminalSession& session = WorkspaceShellTestAccess::TerminalTabSession(shell, 0);
    Expect(PumpUntil(shell, [&] { return session.running(); }), "a shell starts");
    Expect(session.host_reattach() == terminal::HostReattach::ServerRestarted, "a fresh one");
    Expect(PumpUntil(shell,
                     [&] {
                       return RowContaining(shell, "1 started fresh (the host server restarted)") != nullptr;
                     },
                     std::chrono::seconds(10)),
           "the host's row says one started fresh because the server restarted");
    session.SendBytes("echo FRESH-$((3*3))\n");
    Expect(PumpUntil(shell, [&] { return ScreenText(session).find("FRESH-9") != std::string::npos; }),
           "and it runs");
  }
}

// The other side of the lifetime rule: a tab closed with its X ends its shell;
// a shell the user ended (Ctrl+D) is closed at quit, not kept as a dead buffer
// that holds the server; terminal.restore_sessions off closes at quit. A terminal
// no tab claims (a crash, another client's detached one) is offered once per
// connect, and Attach and Close act on it; one another client is attached to is
// never offered.
void TestHostTerminalLifetimeAndUnclaimedTerminals() {
  ScopedAppHomes homes;
  FakeSshHost host;
  const std::filesystem::path root = host.home / "src" / "life";
  WriteFile(root / "a.txt", "a\n");
  const std::string target = Target("fake-life");
  const std::string open = "remote-open " + target + ":" + root.string();

  std::uint64_t closed_by_x = 0;
  std::uint64_t exited = 0;
  std::uint64_t opted_out = 0;
  {
    WorkspaceShell shell;
    PointAtFakeHost(shell, host);
    Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, open), "the remote project opens");
    TerminalSession& first = WorkspaceShellTestAccess::TerminalTabSession(shell, 0);
    Expect(PumpUntil(shell, [&] { return first.running() && first.host_identity().handle != 0; }),
           "its terminal starts");
    closed_by_x = first.host_identity().handle;
    Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, "term"), "a second tab");
    TerminalSession& second = WorkspaceShellTestAccess::TerminalTabSession(shell, 1);
    Expect(PumpUntil(shell, [&] { return second.running() && second.host_identity().handle != 0; }),
           "it starts");
    exited = second.host_identity().handle;
    second.SendBytes("exit\n");
    Expect(PumpUntil(shell, [&] { return !second.running(); }), "the second shell is ended (Ctrl+D)");
    Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, "term-prev"), "back to the first tab");
    Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, "term-close"), "close it with its X");
    Expect(WaitUntil([&] { return Listed(HostTerminals(host), closed_by_x) == nullptr; }, std::chrono::seconds(10)),
           "closing a running tab ends its shell on the host");
    Expect(Listed(HostTerminals(host), exited) != nullptr, "the exited one is kept until seen");
    shell.Shutdown();
  }
  Expect(WaitUntil([&] { return Listed(HostTerminals(host), exited) == nullptr; }, std::chrono::seconds(10)),
         "a shell that exited while watched is closed at quit, not kept as a dead buffer");

  {
    WorkspaceShell shell;
    PointAtFakeHost(shell, host);
    Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, open), "reopen");
    TerminalSession& session = WorkspaceShellTestAccess::TerminalTabSession(shell, 0);
    Expect(PumpUntil(shell, [&] { return session.running() && session.host_identity().handle != 0; }),
           "the exited tab revives as a fresh shell");
    opted_out = session.host_identity().handle;
    Expect(opted_out != exited, "a new one");
    Expect(WorkspaceShellTestAccess::SetSettingValueTransient(shell, "terminal.restore_sessions", "false"),
           "opt out of revival");
    shell.Shutdown();
  }
  Expect(WaitUntil([&] { return Listed(HostTerminals(host), opted_out) == nullptr; }, std::chrono::seconds(10)),
         "opted out, quit closes the host terminal: nothing is left behind");

  // A terminal left by a crash: opened and dropped by a client that recorded it
  // nowhere. And one another client is still attached to.
  const auto open_raw = [&](const std::shared_ptr<remote::RemoteServerClient>& client, TerminalSession& session) {
    auto channel = remote::RemoteTerminalChannel::Open(
        client, terminal::HostTerminalSource::OpenRequest{.working_directory = root, .shell = {"sh"}}, session,
        [](const std::filesystem::path& path) { return path; });
    Expect(channel != nullptr && WaitUntil([&] { return channel->handle() != 0; }, std::chrono::seconds(10)),
           "a raw host terminal opens");
    return channel;
  };
  const auto connect_raw = [&] {
    auto client = remote::RemoteServerClient::Create();
    const int fd = platform::ConnectUnixSocket(remote::ServerSocketPath(host.server_dir()));
    std::string error;
    Expect(fd >= 0 && client->ConnectFds(fd, fd, remote::HelloRequest{.release = "test", .root = root.string()},
                                         &error),
           "a second client connects: " + error);
    return client;
  };
  std::uint64_t orphan = 0;
  std::uint64_t orphan_to_close = 0;
  {
    auto crashed = connect_raw();
    TerminalSession a;
    TerminalSession b;
    auto channel_a = open_raw(crashed, a);
    auto channel_b = open_raw(crashed, b);
    a.SendBytes("echo ORPHAN-$((5*5))\n");
    Expect(WaitUntil([&] { return ScreenText(a).find("ORPHAN-25") != std::string::npos; }, std::chrono::seconds(10)),
           "the orphan's shell answers");
    orphan = channel_a->handle();
    orphan_to_close = channel_b->handle();
    channel_a->Detach();
    channel_b->Detach();
  }
  auto holder = connect_raw();
  TerminalSession held;
  auto held_channel = open_raw(holder, held);

  WorkspaceShell shell;
  PointAtFakeHost(shell, host);
  Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, open), "reopen");
  const workspace::NotificationService::Notification* row = nullptr;
  Expect(PumpUntil(shell,
                   [&] {
                     row = RowContaining(shell, "2 running with no tab");
                     return row != nullptr;
                   },
                   std::chrono::seconds(15)),
         "the connect's row offers the two terminals nobody is attached to");
  Expect(row->actions.size() == 2 && row->actions[0].label == "Attach" && row->actions[1].label == "Close",
         "with Attach and Close");
  const std::vector<std::string> offered = row->actions[0].args;
  Expect(std::find(offered.begin(), offered.end(), std::to_string(held_channel->handle())) == offered.end(),
         "a terminal another client is attached to is not offered");
  Expect(WorkspaceShellTestAccess::RemoteHosts(shell).StatusText().find("2 host terminals with no tab") !=
             std::string::npos,
         "Show Status lists them: " + WorkspaceShellTestAccess::RemoteHosts(shell).StatusText());

  const std::size_t tabs = WorkspaceShellTestAccess::TerminalTabCount(shell);
  Expect(WorkspaceShellTestAccess::ExecuteCommandLine(
             shell, "remote-attach-terminals " + target + " " + std::to_string(orphan)),
         "Attach the orphan");
  Expect(WorkspaceShellTestAccess::TerminalTabCount(shell) == tabs + 1, "in a new tab");
  TerminalSession& attached = WorkspaceShellTestAccess::TerminalTabSession(shell, tabs);
  Expect(PumpUntil(shell, [&] { return ScreenText(attached).find("ORPHAN-25") != std::string::npos; }),
         "showing its screen:\n" + ScreenText(attached));
  Expect(attached.host_identity().handle == orphan, "the orphan itself");
  Expect(WorkspaceShellTestAccess::ExecuteCommandLine(
             shell, "remote-close-terminals " + target + " " + std::to_string(orphan_to_close)),
         "Close the other");
  Expect(WaitUntil([&] { return Listed(HostTerminals(host), orphan_to_close) == nullptr; }, std::chrono::seconds(10)),
         "Close ends it on the host");
  Expect(RowContaining(shell, "running with no tab") == nullptr, "and with none left the row goes");
  held.Stop();
}

}  // namespace

void RegisterRemoteTerminalRestoreTests(std::vector<TestCase>& tests) {
  AddTest(tests, "RemoteTerminalRestore/ARemoteProjectOpensWithItsTerminal", TestARemoteProjectOpensWithItsTerminal);
  AddTest(tests, "RemoteTerminalRestore/HostTerminalsSurviveQuitAndReattach",
          TestHostTerminalsSurviveQuitAndReattach);
  AddTest(tests, "RemoteTerminalRestore/HostTerminalLifetimeAndUnclaimedTerminals",
          TestHostTerminalLifetimeAndUnclaimedTerminals);
}

}  // namespace microide::tests
