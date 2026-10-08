#include "TestSupport.h"
#include "RemoteServerTestSupport.h"

#include "terminal/TerminalSession.h"
#include "util/Sha256.h"
#include "workspace/services/RemoteHostService.h"
#include "workspace/shell/WorkspaceShellTestAccess.h"

#include <chrono>
#include <filesystem>
#include <string>

#if defined(__unix__) || defined(__APPLE__)
#include <unistd.h>
#endif

namespace microide::tests {
namespace {

using workspace::StatusBarSegmentId;
using workspace::WorkspaceShell;
using WorkspaceShellTestAccess = workspace::WorkspaceShell::TestAccess;

#if defined(__unix__) || defined(__APPLE__)

std::string ScreenText(const terminal::TerminalSession& session) {
  std::string text;
  for (const auto& line : session.SnapshotLines()) {
    for (const auto& cell : line.cells) {
      text += cell.DisplayText().empty() ? std::string(" ") : std::string(cell.DisplayText());
    }
    text += '\n';
  }
  return text;
}

// The frame loop's part, for a test: drain what the sessions reported (the
// project-file wake's drain site) and the terminals' frames.
void Pump(WorkspaceShell& shell) {
  (void)WorkspaceShellTestAccess::ReloadProjectIfFilesChanged(shell, false);
  WorkspaceShellTestAccess::ConsumeTerminalSessionUpdates(shell);
}

// Remote: Open Terminal on Host… end to end, through the command a palette entry
// or the host prompt runs: no project open, the host has no server (it is
// installed), the tab is titled with the host, the shell is the host's, the
// status bar names the host, and Disconnect ends the terminal's link.
void TestOpenTerminalOnHostFromTheCommand() {
  FakeSshHost host;
  WorkspaceShell shell;
  std::string ssh;
  for (const std::string& word : host.SshArgv()) {
    ssh += (ssh.empty() ? "" : " ") + word;
  }
  Expect(WorkspaceShellTestAccess::SetSettingValueTransient(shell, "remote.ssh_command", ssh),
         "the ssh seam is set");
  // A per-run host name: the control socket's path is derived from it.
  const std::string target = "dev@fake-" + std::to_string(::getpid());
  Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, "remote-terminal " + target),
         "the command is accepted");
  Expect(!WorkspaceShellTestAccess::ExecuteCommandLine(shell, "remote-terminal -oProxyCommand=x"),
         "a hostile host is refused by the command itself");

  Expect(WaitUntil(
             [&] {
               Pump(shell);
               return WorkspaceShellTestAccess::TerminalTabCount(shell) == 1 &&
                      WorkspaceShellTestAccess::TerminalTabSession(shell, 0).running();
             },
             std::chrono::seconds(30), std::chrono::milliseconds(5)),
         "a host terminal tab opens once the connection is ready");
  terminal::TerminalSession& session = WorkspaceShellTestAccess::TerminalTabSession(shell, 0);
  Expect(session.is_host_terminal(), "it is a host terminal");
  Expect(WorkspaceShellTestAccess::TerminalTabLabelPrefix(shell, 0) == target + " · ",
         "its title names the host");
  Expect(WorkspaceShellTestAccess::StatusBarSegmentVisible(shell, StatusBarSegmentId::Remote) &&
             WorkspaceShellTestAccess::StatusBarSegmentText(shell, StatusBarSegmentId::Remote) == target,
         "the status bar names the connected host: '" +
             WorkspaceShellTestAccess::StatusBarSegmentText(shell, StatusBarSegmentId::Remote) + "'");

  session.SendBytes("echo ON-HOST-$((20+22)) \"$HOME\"\n");
  Expect(WaitUntil(
             [&] {
               Pump(shell);
               return ScreenText(session).find("ON-HOST-42 " + host.home.string()) != std::string::npos;
             },
             std::chrono::seconds(15), std::chrono::milliseconds(5)),
         "the shell runs on the host:\n" + ScreenText(session));

  Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, "remote-disconnect"), "disconnect");
  Expect(WaitUntil(
             [&] {
               Pump(shell);
               return !session.running();
             },
             std::chrono::seconds(10), std::chrono::milliseconds(5)),
         "the terminal shows its link ended");
  // The shim's stand-in master is a plain file at this target's control path;
  // remove exactly that one (the directory may hold the user's real masters).
  if (const char* runtime = std::getenv("XDG_RUNTIME_DIR"); runtime != nullptr) {
    const std::filesystem::path control = std::filesystem::path(runtime) / "microide-ssh" /
                                          ("m-" + util::Sha256Hex(target).substr(0, 16));
    std::error_code ec;
    std::filesystem::remove(control, ec);
    std::filesystem::remove(control.string() + ".log", ec);
  }
}

#endif

}  // namespace

void RegisterRemoteHostServiceTests(std::vector<TestCase>& tests) {
#if defined(__unix__) || defined(__APPLE__)
  AddTest(tests, "RemoteHostService/OpenTerminalOnHostFromTheCommand",
          TestOpenTerminalOnHostFromTheCommand);
#else
  (void)tests;
#endif
}

}  // namespace microide::tests
