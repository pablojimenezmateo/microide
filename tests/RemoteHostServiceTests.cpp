#include "TestSupport.h"
#include "RemoteServerTestSupport.h"

#include "terminal/TerminalSession.h"
#include "util/Sha256.h"
#include "workspace/services/RemoteHostService.h"
#include "workspace/FileUri.h"
#include "workspace/services/AssistService.h"
#include "workspace/shell/WorkspaceShellTestAccess.h"

#include <chrono>
#include <filesystem>
#include <string>

#if defined(__unix__) || defined(__APPLE__)
#include <unistd.h>
#endif

namespace microide::workspace {
struct AssistServiceTestAccess {
  static void NavigateToLspLocation(AssistService& assist, const LspClient::Location& location) {
    assist.NavigateToLspLocation(location, lsp_encoding::PositionEncoding::Utf16);
  }
};
}  // namespace microide::workspace

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

// With a host terminal and a local one side by side, every tab says where its
// shell runs: `local · ` on the local one, the host on the host one.
void RemoveFakeMaster(const std::string& target);

void TestMixedTerminalsSayWhereTheyRun() {
  FakeSshHost host;
  TemporaryDirectory project;
  WorkspaceShell shell;
  std::string ssh;
  for (const std::string& word : host.SshArgv()) {
    ssh += (ssh.empty() ? "" : " ") + word;
  }
  Expect(WorkspaceShellTestAccess::SetSettingValueTransient(shell, "remote.ssh_command", ssh), "ssh seam");
  Expect(WorkspaceShellTestAccess::OpenProjectTabWithLocality(shell, project.path(), {}), "a local project");
  if (WorkspaceShellTestAccess::TerminalTabCount(shell) == 0) {
    WorkspaceShellTestAccess::ExecuteCommandLine(shell, "terminal");
  }
  const std::size_t local_tabs = WorkspaceShellTestAccess::TerminalTabCount(shell);
  Expect(local_tabs >= 1, "a local terminal is open");
  const auto titles = [&] { return WorkspaceShellTestAccess::BottomPanelTerminalLabels(shell); };
  Expect(titles()[0].rfind("local", 0) != 0, "alone, a local terminal carries no prefix: " + titles()[0]);
  const std::string target = "dev@fake-mixed-" + std::to_string(::getpid());
  Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, "remote-terminal " + target), "host terminal");
  Expect(WaitUntil(
             [&] {
               Pump(shell);
               return WorkspaceShellTestAccess::TerminalTabCount(shell) == local_tabs + 1;
             },
             std::chrono::seconds(30), std::chrono::milliseconds(5)),
         "the host terminal opens");
  const auto all = titles();
  Expect(all[0].rfind("local \xc2\xb7 ", 0) == 0 && all.back().rfind(target + " \xc2\xb7 ", 0) == 0,
         "mixed, each says where it runs: '" + all[0] + "' / '" + all.back() + "'");
  RemoveFakeMaster(target);
}

void RemoveFakeMaster(const std::string& target) {
  if (const char* runtime = std::getenv("XDG_RUNTIME_DIR"); runtime != nullptr) {
    const std::filesystem::path control = std::filesystem::path(runtime) / "microide-ssh" /
                                          ("m-" + util::Sha256Hex(target).substr(0, 16));
    std::error_code ec;
    std::filesystem::remove(control, ec);
    std::filesystem::remove(control.string() + ".log", ec);
  }
}

// Remote: Open Folder on Host… end to end: the command prepares the mirror, the
// editor opens it as the project root with the host's launcher, the mirror fills
// from the host, and an edit saved in the editor lands in the HOST's file. Then
// the mirror reopened as a plain path (what the recents list and a restored session
// do) is recognized and given its remote locality again.
void TestOpenFolderOnHostEditsTheHostTree() {
  FakeSshHost host;
  const std::filesystem::path host_root = host.home / "src" / "app";
  WriteFile(host_root / "main.c", "int main(void) { return 0; }\n");
  const std::string target = "dev@fake-open-" + std::to_string(::getpid());
  std::filesystem::path mirror;
  {
  WorkspaceShell shell;
  std::string ssh;
  for (const std::string& word : host.SshArgv()) {
    ssh += (ssh.empty() ? "" : " ") + word;
  }
  Expect(WorkspaceShellTestAccess::SetSettingValueTransient(shell, "remote.ssh_command", ssh),
         "the ssh seam is set");
  Expect(!WorkspaceShellTestAccess::ExecuteCommandLine(shell, "remote-open " + target + ":relative"),
         "a relative host path is refused");
  Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell,
                                                      "remote-open " + target + ":" + host_root.string()),
         "the command is accepted");
  mirror = WorkspaceShellTestAccess::ProjectRoot(shell);
  Expect(mirror.filename() == "app" && mirror != host_root,
         "the project is the mirror, named after the host folder: " + mirror.string());
  Expect(!WorkspaceShellTestAccess::ProjectLauncher(shell).is_local(),
         "the project's processes run on the host");

  Expect(WaitUntil(
             [&] {
               Pump(shell);
               return std::filesystem::exists(mirror / "main.c");
             },
             std::chrono::seconds(30), std::chrono::milliseconds(10)),
         "the mirror fills from the host");
  WorkspaceShellTestAccess::OpenFile(shell, mirror / "main.c");
  WorkspaceShellTestAccess::ActiveEditor(shell).MoveCursorTo(0, 0);
  WorkspaceShellTestAccess::ActiveEditor(shell).InsertText("// edited\n");
  Expect(WorkspaceShellTestAccess::SaveTab(shell, 0), "the save succeeds locally");
  Expect(WaitUntil(
             [&] {
               Pump(shell);
               return ReadFile(host_root / "main.c") == "// edited\nint main(void) { return 0; }\n";
             },
             std::chrono::seconds(20), std::chrono::milliseconds(10)),
         "and the edit reaches the host's file: '" + ReadFile(host_root / "main.c") + "'");

  // An agent rewrites the file on the host while the user saves another edit: the
  // push is refused, both versions survive, and a row offers the choice.
  WriteFile(host_root / "main.c", "// the agent's version\n");
  WorkspaceShellTestAccess::ActiveEditor(shell).MoveCursorTo(0, 0);
  WorkspaceShellTestAccess::ActiveEditor(shell).InsertText("// mine\n");
  Expect(WorkspaceShellTestAccess::SaveTab(shell, 0), "the second save succeeds locally");
  const std::string row_key = "remote.conflict." + (mirror / "main.c").string();
  const auto find_row = [&]() -> const workspace::NotificationService::Notification* {
    for (const auto& row : WorkspaceShellTestAccess::ActiveNotifications(shell)) {
      if (row.key == row_key) {
        return &row;
      }
    }
    return nullptr;
  };
  Expect(WaitUntil(
             [&] {
               Pump(shell);
               return find_row() != nullptr;
             },
             std::chrono::seconds(20), std::chrono::milliseconds(10)),
         "a conflict row appears");
  const auto* row = find_row();
  Expect(row != nullptr && row->actions.size() == 3 && row->actions[0].label == "Compare" &&
             row->actions[1].label == "Keep Mine" && row->actions[2].label == "Take Host's",
         "with Compare, Keep Mine and Take Host's");
  Expect(WorkspaceShellTestAccess::ExecuteCommandLine(
             shell, "remote-resolve " + (mirror / "main.c").string() + " compare"),
         "Compare is accepted");
  Expect(WaitUntil(
             [&] {
               Pump(shell);
               return WorkspaceShellTestAccess::ActiveTabIsCompare(shell);
             },
             std::chrono::seconds(20), std::chrono::milliseconds(10)),
         "a compare tab opens: the host's version beside the mirror's" + [&] {
           std::string all;
           for (const auto& n : WorkspaceShellTestAccess::ActiveNotifications(shell)) all += " | " + n.message;
           return all;
         }());
  Expect(ReadFile(host_root / "main.c") == "// the agent's version\n" &&
             ReadFile(mirror / "main.c").rfind("// mine\n", 0) == 0,
         "and neither side was overwritten");
  Expect(WorkspaceShellTestAccess::ExecuteCommandLine(
             shell, "remote-resolve " + (mirror / "main.c").string() + " host"),
         "taking the host's version is accepted");
  Expect(WaitUntil(
             [&] {
               Pump(shell);
               return ReadFile(mirror / "main.c") == "// the agent's version\n" && find_row() == nullptr;
             },
             std::chrono::seconds(20), std::chrono::milliseconds(10)),
         "the mirror takes the host's bytes and the row goes");

  // Disconnect and Reconnect reach a remote project's own channel too.
  const auto segment = [&] {
    return WorkspaceShellTestAccess::StatusBarSegmentText(shell, StatusBarSegmentId::Remote);
  };
  Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, "remote-disconnect"), "disconnect");
  Expect(WaitUntil(
             [&] {
               Pump(shell);
               return segment().find("disconnected") != std::string::npos;
             },
             std::chrono::seconds(10), std::chrono::milliseconds(10)),
         "the project's connection goes down: '" + segment() + "'");
  Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, "remote-reconnect " + target), "reconnect");
  Expect(WaitUntil(
             [&] {
               Pump(shell);
               return segment() == target;
             },
             std::chrono::seconds(60), std::chrono::milliseconds(10)),
         "and comes back: '" + segment() + "'");

  // The host server's log, in an output channel.
  Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, "remote-show-log " + target),
         "show log is accepted");
  Expect(WaitUntil(
             [&] {
               Pump(shell);
               const auto* entries =
                   WorkspaceShellTestAccess::OutputChannelEntries(shell, "remote.log." + target);
               return entries != nullptr && entries->size() > 1 &&
                      entries->front().find("server.log") != std::string::npos;
             },
             std::chrono::seconds(20), std::chrono::milliseconds(10)),
         "the host's server log arrives in an output channel");

  // A host file outside the project — what a language server's definition in a
  // system header names — maps to the out-of-project cache, never to a same-named
  // local path, and opening it fetches it read-only from the host first.
  WriteFile(host.home / "include" / "outside.h", "#define FROM_THE_HOST 1\nint the_definition;\n");
  const std::filesystem::path cached = WorkspaceShellTestAccess::ProjectLauncher(shell).LocalPathFromHost(
      host.home / "include" / "outside.h");
  Expect(cached != host.home / "include" / "outside.h" &&
             cached.string().find("host-files") != std::string::npos,
         "an out-of-project host path maps into the cache: " + cached.string());
  Expect(WorkspaceShellTestAccess::ProjectLauncher(shell).ResolveWorkingDirectory(cached) ==
             host.home / "include" / "outside.h",
         "and maps back to the host path");
  Expect(!WorkspaceShellTestAccess::OpenFileInNewTab(shell, cached),
         "the open waits for the fetch");
  Expect(WaitUntil(
             [&] {
               Pump(shell);
               const auto* viewport = WorkspaceShellTestAccess::ActiveEditorOrNull(shell);
               return viewport != nullptr && viewport->path() == cached;
             },
             std::chrono::seconds(20), std::chrono::milliseconds(10)),
         "then opens the host's file");
  Expect(ReadFile(cached) == "#define FROM_THE_HOST 1\nint the_definition;\n", "with the host's bytes");

  // Go to definition into a host header nobody fetched yet still lands on the line.
  WriteFile(host.home / "include" / "other.h", "\n\n\nint target;\n");
  workspace::LspClient::Location location;
  location.uri = workspace::FileUriForPath(
      WorkspaceShellTestAccess::ProjectLauncher(shell).LocalPathFromHost(host.home / "include" / "other.h"));
  location.range.start.line = 3;
  location.range.start.character = 4;
  workspace::AssistServiceTestAccess::NavigateToLspLocation(WorkspaceShellTestAccess::Assist(shell), location);
  Expect(WaitUntil(
             [&] {
               Pump(shell);
               const auto* viewport = WorkspaceShellTestAccess::ActiveEditorOrNull(shell);
               return viewport != nullptr && viewport->path().filename() == "other.h";
             },
             std::chrono::seconds(20), std::chrono::milliseconds(10)),
         "the definition's file is fetched and opened");
  Expect(WorkspaceShellTestAccess::ActiveEditor(shell).cursor_line() == 3 &&
             WorkspaceShellTestAccess::ActiveEditor(shell).cursor_column() == 4,
         "at the definition, not at the top");

  // Closing the project drops its connection (and its status segment); reopening
  // the mirror connects again.
  WorkspaceShellTestAccess::CloseProject(shell, 0);
  Expect(WaitUntil(
             [&] {
               Pump(shell);
               return !WorkspaceShellTestAccess::StatusBarSegmentVisible(shell, StatusBarSegmentId::Remote);
             },
             std::chrono::seconds(10), std::chrono::milliseconds(10)),
         "a closed remote project leaves no connection behind: '" +
             WorkspaceShellTestAccess::StatusBarSegmentText(shell, StatusBarSegmentId::Remote) + "'");
  Expect(WorkspaceShellTestAccess::OpenProjectTabWithLocality(shell, mirror, {}), "reopened");
  Expect(WaitUntil(
             [&] {
               Pump(shell);
               return WorkspaceShellTestAccess::StatusBarSegmentText(shell, StatusBarSegmentId::Remote) ==
                      target;
             },
             std::chrono::seconds(60), std::chrono::milliseconds(10)),
         "and connected again: '" +
             WorkspaceShellTestAccess::StatusBarSegmentText(shell, StatusBarSegmentId::Remote) + "' " + [&] {
               (void)WorkspaceShellTestAccess::ExecuteCommandLine(shell, "remote-status");
               std::string all;
               for (const auto& n : WorkspaceShellTestAccess::ActiveNotifications(shell)) all += " | " + n.message;
               return all;
             }());
  }
  {
    // A later run opening the mirror as a plain folder.
    WorkspaceShell shell;
    std::string ssh;
    for (const std::string& word : host.SshArgv()) {
      ssh += (ssh.empty() ? "" : " ") + word;
    }
    Expect(WorkspaceShellTestAccess::SetSettingValueTransient(shell, "remote.ssh_command", ssh),
           "the ssh seam is set again");
    Expect(WorkspaceShellTestAccess::OpenProjectTabWithLocality(shell, mirror, {}),
           "the mirror opens as a path");
    Expect(!WorkspaceShellTestAccess::ProjectLauncher(shell).is_local(),
           "and is recognized as the remote project it mirrors");
  }
  RemoveFakeMaster(target);
}

#endif

}  // namespace

void RegisterRemoteHostServiceTests(std::vector<TestCase>& tests) {
#if defined(__unix__) || defined(__APPLE__)
  AddTest(tests, "RemoteHostService/OpenTerminalOnHostFromTheCommand",
          TestOpenTerminalOnHostFromTheCommand);
  AddTest(tests, "RemoteHostService/MixedTerminalsSayWhereTheyRun", TestMixedTerminalsSayWhereTheyRun);
  AddTest(tests, "RemoteHostService/OpenFolderOnHostEditsTheHostTree",
          TestOpenFolderOnHostEditsTheHostTree);
#else
  (void)tests;
#endif
}

}  // namespace microide::tests
