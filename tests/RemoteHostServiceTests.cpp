#include "TestSupport.h"
#include "RemoteServerTestSupport.h"

#include "terminal/TerminalSession.h"
#include "util/Sha256.h"
#include "workspace/services/RemoteHostService.h"
#include "workspace/FileUri.h"
#include "workspace/WorkspaceUiText.h"
#include "workspace/services/AssistService.h"
#include "workspace/shell/WorkspaceShellTestAccess.h"
#include "workspace/persistence/PersistenceService.h"
#include "project/remote/RemoteProject.h"
#include "support/SoftwareCanvas.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <string>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

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

// The shell's ssh is the fake host's shim. Automatic port forwarding is off: on a
// shared machine (a parallel test run) another test's listener would be forwarded
// and counted; the one test about it turns it back on.
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

// Remote: Open Terminal on Host… end to end, through the command a palette entry
// or the host prompt runs: no project open, the host has no server (it is
// installed), the tab is titled with the host, the shell is the host's, the
// status bar names the host, and Disconnect ends the terminal's link.
void TestOpenTerminalOnHostFromTheCommand() {
  FakeSshHost host;
  WorkspaceShell shell;
  PointAtFakeHost(shell, host);
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

  // The host prompt completes from the hosts in use (and ~/.ssh/config).
  Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, "remote-terminal"), "the prompt opens");
  Expect(WorkspaceShellTestAccess::PromptSurfaceVisible(shell), "it is up");
  WorkspaceShellTestAccess::SetPromptSurfaceInput(shell, target.substr(0, target.size() - 2));
  {
    SDL_Event tab{};
    tab.type = SDL_EVENT_KEY_DOWN;
    tab.key.key = SDLK_TAB;
    (void)shell.HandleEvent(tab);
  }
  Expect(WorkspaceShellTestAccess::PromptSurfaceInput(shell).rfind(target, 0) == 0,
         "Tab completes the connected host: '" + WorkspaceShellTestAccess::PromptSurfaceInput(shell) + "'");
  {
    SDL_Event escape{};
    escape.type = SDL_EVENT_KEY_DOWN;
    escape.key.key = SDLK_ESCAPE;
    (void)shell.HandleEvent(escape);
  }
  Expect(!WorkspaceShellTestAccess::PromptSurfaceVisible(shell), "Escape closes it");

  // Forward a Port: the row says where it is and offers the browser and the
  // address; the status bar counts it; Stop Forwarding takes it away again.
  Expect(!WorkspaceShellTestAccess::ExecuteCommandLine(shell, "remote-forward-port 0"),
         "a port out of range is refused");
  Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, "remote-forward-port 39218"),
         "the forward is accepted (the only host)");
  const std::string forward_key = "remote.forward." + target + ".39218";
  const auto forward_row = [&]() -> const workspace::NotificationService::Notification* {
    for (const auto& row : WorkspaceShellTestAccess::ActiveNotifications(shell)) {
      if (row.key == forward_key) {
        return &row;
      }
    }
    return nullptr;
  };
  Expect(WaitUntil(
             [&] {
               Pump(shell);
               return forward_row() != nullptr;
             },
             std::chrono::seconds(15), std::chrono::milliseconds(5)),
         "a row reports the forward");
  {
    const auto* row = forward_row();
    Expect(row != nullptr && row->message.find("is available at localhost:") != std::string::npos &&
               row->actions.size() == 2 && row->actions[0].label == "Open in Browser" &&
               row->actions[0].args.size() == 1 && row->actions[0].args[0].rfind("http://localhost:", 0) == 0 &&
               row->actions[1].label == "Copy Address",
           "with the local address, Open in Browser and Copy Address: '" +
               (row != nullptr ? row->message : std::string()) + "'");
  }
  Expect(WorkspaceShellTestAccess::StatusBarSegmentText(shell, StatusBarSegmentId::Remote) ==
             target + " · 1 port",
         "the status bar counts it: '" +
             WorkspaceShellTestAccess::StatusBarSegmentText(shell, StatusBarSegmentId::Remote) + "'");
  // A localhost link in a HOST terminal is the host's: clicking it forwards that
  // port and opens the local end, path and query kept. A local terminal's would
  // open as it is.
  std::vector<std::string> opened;
  WorkspaceShellTestAccess::SetExternalUrlOpener(shell, [&](std::string_view url) {
    opened.emplace_back(url);
    return true;
  });
  Expect(WorkspaceShellTestAccess::RemoteHosts(shell).OpenHostLocalUrl(
             WorkspaceShellTestAccess::TerminalTabLauncher(shell, 0), "http://localhost:39220/app?x=1"),
         "the link is taken");
  Expect(!WorkspaceShellTestAccess::RemoteHosts(shell).OpenHostLocalUrl(
             platform::LocalProcessLauncher(), "http://localhost:39220/"),
         "a local terminal's link is not the host's");
  Expect(WaitUntil(
             [&] {
               Pump(shell);
               return !opened.empty();
             },
             std::chrono::seconds(15), std::chrono::milliseconds(5)),
         "and opens once forwarded");
  Expect(opened.size() == 1 && opened[0].rfind("http://localhost:", 0) == 0 &&
             opened[0].ends_with("/app?x=1"),
         "at the local end of the forward: '" + (opened.empty() ? std::string() : opened[0]) + "'");
  Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, "remote-unforward-port 39220"),
         "the clicked port's forward stops too");
  Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, "remote-unforward-port 39218"),
         "stop forwarding is accepted");
  Expect(WaitUntil(
             [&] {
               Pump(shell);
               return WorkspaceShellTestAccess::StatusBarSegmentText(shell, StatusBarSegmentId::Remote) ==
                      target;
             },
             std::chrono::seconds(15), std::chrono::milliseconds(5)),
         "and the count goes");

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
  PointAtFakeHost(shell, host);
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

// The host-terminal settings shape a remote PROJECT's terminals: they once reached
// only the launcher of the removed terminal-only host session, so in a project
// both read as their defaults whatever the user set.
void TestHostTerminalSettingsReachTheProjectLauncher() {
  FakeSshHost host;
  const std::filesystem::path host_root = host.home / "src" / "settings";
  WriteFile(host_root / "a.txt", "a\n");
  WorkspaceShell shell;
  PointAtFakeHost(shell, host);
  Expect(WorkspaceShellTestAccess::SetSettingValueTransient(shell, "remote.term_credit_bytes", "65536") &&
             WorkspaceShellTestAccess::SetSettingValueTransient(shell, "remote.scrollback_prefetch_lines",
                                                                "1234"),
         "the settings are set");
  const std::string target = "dev@fake-settings-" + std::to_string(::getpid());
  Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, "remote-open " + target + ":" + host_root.string()),
         "the remote project opens");
  const auto* launcher = dynamic_cast<const project::remote::RemoteProcessLauncher*>(
      &WorkspaceShellTestAccess::ProjectLauncher(shell));
  Expect(launcher != nullptr, "the project's launcher is the host's");
  Expect(launcher != nullptr && launcher->options().terminal_credit_bytes == 65536 &&
             launcher->options().terminal_prefetch_lines == 1234,
         "and carries both settings");
  RemoveFakeMaster(target);
}

// § 7.3: a file the mirror has not fetched (here: over remote.backfill_max_file_mib,
// so it stays unfetched deterministically) is still listed — dimmed in the tree, in
// quick-open — and opening it fetches it, then opens it.
void TestUnfetchedFilesAreListedAndOpenWhenFetched() {
  FakeSshHost host;
  const std::filesystem::path host_root = host.home / "src" / "lazy";
  WriteFile(host_root / "main.c", "int main(void) { return 0; }\n");
  const std::string big(2 * 1024 * 1024, 'x');
  WriteFile(host_root / "logs" / "big.log", big);
  const std::string target = "dev@fake-lazy-" + std::to_string(::getpid());
  WorkspaceShell shell;
  PointAtFakeHost(shell, host);
  Expect(WorkspaceShellTestAccess::SetSettingValueTransient(shell, "remote.backfill_max_file_mib", "1"),
         "files over 1 MiB wait to be opened");
  Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, "remote-open " + target + ":" + host_root.string()),
         "the command is accepted");
  const std::filesystem::path mirror = WorkspaceShellTestAccess::ProjectRoot(shell);
  const auto listed = [&] {
    const std::vector<std::string> paths = WorkspaceShellTestAccess::ProjectFinderPaths(shell);
    return std::find(paths.begin(), paths.end(), "logs/big.log") != paths.end();
  };
  Expect(WaitUntil(
             [&] {
               Pump(shell);
               return std::filesystem::exists(mirror / "main.c") && listed();
             },
             std::chrono::seconds(30), std::chrono::milliseconds(10)),
         "the small file arrives, and quick-open lists the big one too");
  Expect(!std::filesystem::exists(mirror / "logs" / "big.log"), "which is not on disk");
  const auto row = [&](const std::filesystem::path& path) -> const project::TreeEntry* {
    for (const auto& entry : WorkspaceShellTestAccess::TreeEntries(shell)) {
      if (entry.path == path) {
        return &entry;
      }
    }
    return nullptr;
  };
  const project::TreeEntry* logs = row(mirror / "logs");
  Expect(logs != nullptr && logs->pending && logs->is_directory, "the tree lists its folder, dimmed");

  WorkspaceShellTestAccess::OpenFile(shell, mirror / "logs" / "big.log");
  const std::string fetching_key = "remote.fetching." + (mirror / "logs" / "big.log").string();
  const auto fetching_row = [&] {
    for (const auto& row : WorkspaceShellTestAccess::ActiveNotifications(shell)) {
      if (row.key == fetching_key) {
        return true;
      }
    }
    return false;
  };
  Expect(fetching_row(), "a row says it is being fetched");
  Expect(WaitUntil(
             [&] {
               Pump(shell);
               return WorkspaceShellTestAccess::ActiveEditor(shell).path() == mirror / "logs" / "big.log";
             },
             std::chrono::seconds(30), std::chrono::milliseconds(10)),
         "opening it fetches it, then opens it");
  Expect(!fetching_row(), "and the row goes");
  Expect(ReadFile(mirror / "logs" / "big.log") == big, "with the host's bytes");
  Expect(WaitUntil(
             [&] {
               Pump(shell);
               const project::TreeEntry* fetched = row(mirror / "logs");
               return fetched != nullptr && !fetched->pending;
             },
             std::chrono::seconds(20), std::chrono::milliseconds(10)),
         "and nothing is pending any more");
  RemoveFakeMaster(target);
}

// A save conflict merged three ways in the merge editor: Merge opens the version
// both started from, the host's and the mirror's; Mark Resolved pushes the result to
// the host (not to git's index) and the conflict row goes.
void TestConflictMergesThreeWaysToTheHost() {
  FakeSshHost host;
  const std::filesystem::path host_root = host.home / "src" / "merge";
  WriteFile(host_root / "notes.txt", "a\nb\nc\n");
  const std::string target = "dev@fake-merge-" + std::to_string(::getpid());
  WorkspaceShell shell;
  PointAtFakeHost(shell, host);
  Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, "remote-open " + target + ":" + host_root.string()),
         "the command is accepted");
  const std::filesystem::path mirror = WorkspaceShellTestAccess::ProjectRoot(shell);
  const std::filesystem::path file = mirror / "notes.txt";
  Expect(WaitUntil(
             [&] {
               Pump(shell);
               return std::filesystem::exists(file) && ReadFile(file) == "a\nb\nc\n";
             },
             std::chrono::seconds(30), std::chrono::milliseconds(10)),
         "the mirror fills");
  WorkspaceShellTestAccess::OpenFile(shell, file);
  // The agent edits the first line while the user saves an edit to the last.
  WriteFile(host_root / "notes.txt", "A\nb\nc\n");
  WorkspaceShellTestAccess::ActiveEditor(shell).MoveCursorTo(2, 0);
  WorkspaceShellTestAccess::ActiveEditor(shell).InsertText("X");
  Expect(WorkspaceShellTestAccess::SaveTab(shell, 0), "the save succeeds locally");
  const std::string row_key = "remote.conflict." + file.string();
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
  Expect(row != nullptr && row->actions.size() == 3 && row->actions[0].label == "Merge",
         "with Merge first");
  Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, "remote-resolve " + file.string() + " merge"),
         "Merge is accepted");
  Expect(WaitUntil(
             [&] {
               Pump(shell);
               return WorkspaceShellTestAccess::ActiveTabIsMerge(shell);
             },
             std::chrono::seconds(20), std::chrono::milliseconds(10)),
         "the merge editor opens");
  Expect(WorkspaceShellTestAccess::ActiveMerge(shell).result_viewport.SerializeDocumentText() == "A\nb\nXc\n",
         "both sides' edits merge cleanly: '" + WorkspaceShellTestAccess::ActiveMerge(shell).result_viewport.SerializeDocumentText() +
             "'");
  WorkspaceShellTestAccess::MarkMergeResolved(shell);
  Expect(WorkspaceShellTestAccess::ActiveMerge(shell).marked_resolved,
         "Mark Resolved is accepted: " + WorkspaceShellTestAccess::ActiveMerge(shell).status_message);
  Expect(WaitUntil(
             [&] {
               Pump(shell);
               return ReadFile(host_root / "notes.txt") == "A\nb\nXc\n" && find_row() == nullptr;
             },
             std::chrono::seconds(20), std::chrono::milliseconds(10)),
         "the result reaches the host and the row goes: '" + ReadFile(host_root / "notes.txt") + "'");
  RemoveFakeMaster(target);
}

// Automatic port forwarding (VS Code's): a program on the host that starts
// listening after the editor connected is forwarded and announced, with no
// command; what already listened at connect is not.
void TestAPortThatStartsListeningIsForwarded() {
  FakeSshHost host;
  WorkspaceShell shell;
  PointAtFakeHost(shell, host);
  Expect(WorkspaceShellTestAccess::SetSettingValueTransient(shell, "remote.auto_forward_ports", "true"),
         "automatic forwards on");
  const std::string target = "dev@fake-ports-" + std::to_string(::getpid());
  Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, "remote-terminal " + target), "connect");
  Expect(WaitUntil(
             [&] {
               Pump(shell);
               return WorkspaceShellTestAccess::RemoteHosts(shell).PortsWatched(target);
             },
             std::chrono::seconds(30), std::chrono::milliseconds(5)),
         "connected, and the host's ports are watched");
  // The host is this machine: a listener here is one on the host.
  const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  socklen_t length = sizeof(address);
  Expect(fd >= 0 && ::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0 &&
             ::listen(fd, 1) == 0 && ::getsockname(fd, reinterpret_cast<sockaddr*>(&address), &length) == 0,
         "a dev server starts listening on the host");
  const std::uint16_t port = ntohs(address.sin_port);
  const std::string row_key = "remote.forward." + target + "." + std::to_string(port);
  const auto announced = [&] {
    for (const auto& row : WorkspaceShellTestAccess::ActiveNotifications(shell)) {
      if (row.key == row_key) {
        return row.message.find("is available at localhost:") != std::string::npos;
      }
    }
    return false;
  };
  Expect(WaitUntil(
             [&] {
               Pump(shell);
               return announced();
             },
             std::chrono::seconds(30), std::chrono::milliseconds(10)),
         "it is forwarded and announced");
  ::close(fd);
  const auto row_present = [&] {
    for (const auto& row : WorkspaceShellTestAccess::ActiveNotifications(shell)) {
      if (row.key == row_key) {
        return true;
      }
    }
    return false;
  };
  Expect(WaitUntil(
             [&] {
               Pump(shell);
               // This port's forward only: on a shared machine another test's
               // listener may be forwarded meanwhile.
               return !row_present() && !WorkspaceShellTestAccess::RemoteHosts(shell)
                                             .CommandText(target, "port:" + std::to_string(port))
                                             .has_value();
             },
             std::chrono::seconds(30), std::chrono::milliseconds(10)),
         "when it stops listening the automatic forward goes, quietly");
  RemoveFakeMaster(target);
}

// § 7.3's status block: each stage as far as the connection got, a failure's
// reason with the command that reproduces it and Retry, the sync's progress.
void TestProjectHomeStatusBlockFollowsTheConnection() {
  using State = project::remote::RemoteHostSession::State;
  using Stage = editor::WelcomeStageState;
  project::remote::MirrorSyncEngine::Status sync;
  const auto block = [&](State state, std::string error = {}) {
    return workspace::RemoteHostService::StatusBlock(
        "box", "/srv/app", project::remote::RemoteHostSession::Status{.state = state, .message = {}, .error = error},
        sync, "ssh -- box", "2.13.0");
  };
  const editor::WelcomeRemoteStatus failed = block(State::Offline, "Connection refused");
  Expect(failed.heading == "box:/srv/app" && failed.stages.size() == 1 && failed.stages[0].state == Stage::Failed &&
             failed.retry && failed.detail == "Connection refused — try: ssh -- box",
         "a failure says why, how to reproduce it, and offers Retry: '" + failed.detail + "'");
  const editor::WelcomeRemoteStatus starting = block(State::StartingServer);
  Expect(starting.stages.size() == 2 && starting.stages[0].state == Stage::Done &&
             starting.stages[1].state == Stage::Active && !starting.retry,
         "connected, the server starting");
  sync.files = 10;
  sync.absent = 7;
  const editor::WelcomeRemoteStatus syncing = block(State::Ready);
  Expect(syncing.stages.size() == 3 && syncing.stages[1].text == "server 2.13.0" &&
             syncing.stages[2].text == "syncing 3 / 10 files" &&
             syncing.stages[2].state == Stage::Active,
         "a first sync's progress: '" + (syncing.stages.size() == 3 ? syncing.stages[2].text : std::string()) + "'");
  sync.synced_once = true;
  sync.absent = 0;
  sync.dirty = 2;
  const editor::WelcomeRemoteStatus synced = block(State::Ready);
  Expect(synced.stages.size() == 3 && synced.stages[2].text == "synced 10 files · 2 unsent" &&
             synced.stages[2].state == Stage::Done,
         "then what it holds and what is not on the host yet");
}

// Remote: Open Folder on Host… end to end: the command prepares the mirror, the
// editor opens it as the project root with the host's launcher, the mirror fills
// from the host, and an edit saved in the editor lands in the HOST's file. Then
// the mirror reopened as a plain path (what the recents list and a restored session
// do) is recognized and given its remote locality again.
// Remote: Remove Mirror. A mirror leaves the disk only through this (a copy of
// the host's tree plus its metadata is a real cost: 47 MB + up to 170 MB for this
// repository after churn), and only when nothing in it is still owed to the host.
void TestRemoveMirrorDeletesAClosedMirrorAndKeepsUnsentSaves() {
  FakeSshHost host;
  const std::filesystem::path host_root = host.home / "src" / "app";
  WriteFile(host_root / "main.c", "int main(void) { return 0; }\n");
  const std::string target = "dev@fake-remove-" + std::to_string(::getpid());
  WorkspaceShell shell;
  PointAtFakeHost(shell, host);
  Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell,
                                                      "remote-open " + target + ":" + host_root.string()),
         "the remote project opens");
  const std::filesystem::path mirror = WorkspaceShellTestAccess::ProjectRoot(shell);
  Expect(WaitUntil(
             [&] {
               Pump(shell);
               return std::filesystem::exists(mirror / "main.c");
             },
             std::chrono::seconds(30), std::chrono::milliseconds(10)),
         "the mirror fills from the host");
  auto outcome = WorkspaceShellTestAccess::ExecuteControlCommand(shell, "remote-remove-mirror");
  Expect(!outcome.ok && outcome.error.find("close the project first") != std::string::npos,
         "an open project's mirror is not removed under it: " + outcome.error);
  Expect(std::filesystem::exists(mirror / "main.c"), "and nothing was deleted");

  // A save the host never receives: disconnected, then edited and saved.
  Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, "remote-disconnect"), "disconnect");
  // The link goes down asynchronously: a save made before it has would still be
  // pushed, leaving nothing unsent (a parallel ctest run hit exactly that).
  Expect(WaitUntil(
             [&] {
               Pump(shell);
               return WorkspaceShellTestAccess::StatusBarSegmentText(shell, StatusBarSegmentId::Remote)
                          .find("disconnected") != std::string::npos;
             },
             std::chrono::seconds(10), std::chrono::milliseconds(10)),
         "disconnected");
  WorkspaceShellTestAccess::OpenFile(shell, mirror / "main.c");
  WorkspaceShellTestAccess::ActiveEditor(shell).MoveCursorTo(0, 0);
  WorkspaceShellTestAccess::ActiveEditor(shell).InsertText("// offline\n");
  Expect(WorkspaceShellTestAccess::SaveTab(shell, 0), "the offline save succeeds locally");
  WorkspaceShellTestAccess::CloseProject(shell, 0);
  Expect(WaitUntil(
             [&] {
               Pump(shell);
               return !WorkspaceShellTestAccess::StatusBarSegmentVisible(shell, StatusBarSegmentId::Remote);
             },
             std::chrono::seconds(10), std::chrono::milliseconds(10)),
         "the project is closed");

  outcome = WorkspaceShellTestAccess::ExecuteControlCommand(shell, "remote-remove-mirror " + mirror.string());
  Expect(!outcome.ok && outcome.error.find("1 save") != std::string::npos &&
             outcome.error.find("discard") != std::string::npos,
         "a mirror with an unsent save is kept, and the refusal names the count: " + outcome.error);
  Expect(std::filesystem::exists(mirror / "main.c"), "the mirror is still there");

  outcome = WorkspaceShellTestAccess::ExecuteControlCommand(
      shell, "remote-remove-mirror " + mirror.string() + " discard");
  Expect(outcome.ok, "with discard the mirror is removed: " + outcome.error);
  Expect(!std::filesystem::exists(mirror.parent_path()),
         "the tree and its metadata are gone: " + mirror.parent_path().string());
  const auto& recents = WorkspaceShellTestAccess::RecentProjects(shell);
  Expect(std::none_of(recents.begin(), recents.end(),
                      [&](const project::ProjectId& id) { return id.IoRoot() == mirror; }),
         "and the recents list no longer offers it");
  outcome = WorkspaceShellTestAccess::ExecuteControlCommand(shell, "remote-remove-mirror " + mirror.string());
  Expect(!outcome.ok && outcome.error.find("not the mirror") != std::string::npos,
         "a path that is not a mirror is refused by name: " + outcome.error);
}

void TestOpenFolderOnHostEditsTheHostTree() {
  FakeSshHost host;
  const std::filesystem::path host_root = host.home / "src" / "app";
  WriteFile(host_root / "main.c", "int main(void) { return 0; }\n");
  // A repository, so git on the host has something to answer once it can.
  Expect(std::system(("git -C '" + host_root.string() +
                      "' init -q && git -C '" + host_root.string() +
                      "' -c user.name=t -c user.email=t@t add -A && git -C '" + host_root.string() +
                      "' -c user.name=t -c user.email=t@t commit -qm init")
                         .c_str()) == 0,
         "the host folder is a repository");
  const std::string target = "dev@fake-open-" + std::to_string(::getpid());
  std::filesystem::path mirror;
  {
  WorkspaceShell shell;
  PointAtFakeHost(shell, host);
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
  // Named by its host folder wherever it is shown (project-identity), never by the
  // mirror's directory under the data directory.
  {
    const std::string location = target + ":" + host_root.string();
    Expect(WorkspaceShellTestAccess::ProjectCatalogId(shell, 0) ==
               project::ProjectId::ForRemote(target, host_root.string()),
           "the project's id is the host folder: " + WorkspaceShellTestAccess::ProjectCatalogId(shell, 0).Location());
    Expect(WorkspaceShellTestAccess::ProjectTabDisplayTitle(shell, 0) == "app",
           "the project tab reads the host folder's name: " + WorkspaceShellTestAccess::ProjectTabDisplayTitle(shell, 0));
    Expect(WorkspaceShellTestAccess::ProjectTabTooltipLabel(shell, 0) == location,
           "its tooltip is host:/path, not the mirror path: " + WorkspaceShellTestAccess::ProjectTabTooltipLabel(shell, 0));
    Expect(WorkspaceShellTestAccess::ProjectLabel(shell) == "app",
           "prompts name the host folder: " + WorkspaceShellTestAccess::ProjectLabel(shell));
    SoftwareCanvas canvas(1280, 800);
    WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 800);
    shell.Render(canvas.renderer(), 1280, 800);
    const std::string title = WorkspaceShellTestAccess::ControlUi(shell)["windowTitle"].AsString();
    Expect(title == "app \u2014 microide", "the window title names the host folder: " + title);
  }

  Expect(WaitUntil(
             [&] {
               Pump(shell);
               return std::filesystem::exists(mirror / "main.c");
             },
             std::chrono::seconds(30), std::chrono::milliseconds(10)),
         "the mirror fills from the host");
  // The home's status block follows it (§ 7.3).
  Expect(WaitUntil(
             [&] {
               Pump(shell);
               const editor::WelcomeRemoteStatus& block = WorkspaceShellTestAccess::ProjectRemoteStatus(shell);
               return block.heading == target + ":" + host_root.string() && block.stages.size() == 3 &&
                      block.stages[2].text.rfind("synced ", 0) == 0;
             },
             std::chrono::seconds(30), std::chrono::milliseconds(10)),
         "the project home shows the connection and the sync");
  // The control channel reports the same, as a driver reads it: no notification
  // text to parse for readiness.
  {
    const util::JsonValue status = WorkspaceShellTestAccess::ExecuteControlQuery(shell, "status");
    Expect(status.HasKey("remote"), "status carries the remote project");
    const util::JsonValue& remote = status["remote"];
    Expect(remote["host"].AsString() == target && remote["hostRoot"].AsString() == host_root.string(),
           "status.remote names the host and its root");
    Expect(remote["state"].AsString() == "ready" && remote["files"].AsInt() == 1 &&
               remote["filesPending"].AsInt() == 0 && remote["unsentPushes"].AsInt() == 0,
           "status.remote is ready with everything pulled: " + remote["state"].AsString());
    const util::JsonValue projects = WorkspaceShellTestAccess::ExecuteControlQuery(shell, "projects");
    bool marked = false;
    for (const util::JsonValue& project : projects.AsArray()) {
      marked = marked || (project["root"].AsString() == mirror.string() &&
                          project["host"].AsString() == target);
    }
    Expect(marked, "the projects query marks the mirror with its host");
  }
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
  Expect(row != nullptr && row->actions.size() == 3 && row->actions[0].label == "Merge" &&
             row->actions[1].label == "Keep Mine" && row->actions[2].label == "Take Host's",
         "with Merge, Keep Mine and Take Host's");
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
  Expect(WorkspaceShellTestAccess::ActiveCompare(shell).left_label == "main.c (on " + target + ")" &&
             WorkspaceShellTestAccess::ActiveCompare(shell).right_label == "main.c (yours)",
         "each side says whose it is: '" + WorkspaceShellTestAccess::ActiveCompare(shell).left_label +
             "' / '" + WorkspaceShellTestAccess::ActiveCompare(shell).right_label + "'");
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

  // Changed on Host (§ 7.9): an agent adds a file and rewrites one; one counted row
  // with Show, a view listing both newest first, and a modified row opening a
  // before/after compare.
  // The row is timed and raised at most every 30 s, so it is watched for while the
  // changes arrive rather than looked for afterwards.
  bool counted = false;
  const auto pump_watching = [&] {
    Pump(shell);
    for (const auto& note : WorkspaceShellTestAccess::ActiveNotifications(shell)) {
      counted = counted || (note.key == "remote.changes." + mirror.string() && !note.actions.empty() &&
                            note.actions[0].label == "Show");
    }
  };
  WriteFile(host_root / "agent.c", "int agent;\n");
  Expect(WaitUntil([&] { pump_watching(); return std::filesystem::exists(mirror / "agent.c"); },
                   std::chrono::seconds(20), std::chrono::milliseconds(10)),
         "the agent's new file arrives");
  WriteFile(host_root / "main.c", "// the agent's second version\n");
  Expect(WaitUntil(
             [&] {
               pump_watching();
               return ReadFile(mirror / "main.c") == "// the agent's second version\n";
             },
             std::chrono::seconds(20), std::chrono::milliseconds(10)),
         "and its rewrite");
  Expect(counted, "one counted 'changed on host' row with a Show button");
  Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, "remote-changes"), "Show is accepted");
  Expect(WorkspaceShellTestAccess::SidebarViewId(shell) == "host-changes", "the view opens");
  {
    const auto& rows = WorkspaceShellTestAccess::PluginSidebarItems(shell);
    Expect(rows.size() >= 2 && rows[0].id == "main.c" && rows[0].detail.rfind("modified", 0) == 0 &&
               rows[1].id == "agent.c" && rows[1].detail == "added",
           "newest first: the rewrite, then the added file (" + std::to_string(rows.size()) + " rows)");
  }
  WorkspaceShellTestAccess::SetPluginSidebarSelectedIndex(shell, 0);
  Expect(WorkspaceShellTestAccess::OpenSelectedPluginSidebarItem(shell), "the modified row opens");
  Expect(WaitUntil(
             [&] {
               Pump(shell);
               return WorkspaceShellTestAccess::ActiveTabIsCompare(shell) &&
                      WorkspaceShellTestAccess::ActiveCompare(shell).left_label == "main.c (before)";
             },
             std::chrono::seconds(20), std::chrono::milliseconds(10)),
         "as a compare of the version before against the host's" + [&] {
           std::string all = WorkspaceShellTestAccess::ActiveTabIsCompare(shell)
                                 ? " compare:" + WorkspaceShellTestAccess::ActiveCompare(shell).left_label
                                 : std::string(" not a compare");
           for (const auto& n : WorkspaceShellTestAccess::ActiveNotifications(shell)) all += " | " + n.message;
           return all;
         }());
  Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, "remote-clear-changes"), "Clear is accepted");
  Expect(WaitUntil(
             [&] {
               Pump(shell);
               return WorkspaceShellTestAccess::PluginSidebarItems(shell).empty();
             },
             std::chrono::seconds(10), std::chrono::milliseconds(10)),
         "and the cleared view is empty");

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
  // A save made offline is kept for the host, and the status bar counts it (§ 7.4).
  WorkspaceShellTestAccess::OpenFile(shell, mirror / "main.c");
  WorkspaceShellTestAccess::ActiveEditor(shell).MoveCursorTo(0, 0);
  WorkspaceShellTestAccess::ActiveEditor(shell).InsertText("// offline\n");
  Expect(WorkspaceShellTestAccess::SaveTab(shell, 0), "an offline save succeeds locally");
  Expect(WaitUntil(
             [&] {
               Pump(shell);
               return segment().find("1 unsent") != std::string::npos;
             },
             std::chrono::seconds(10), std::chrono::milliseconds(10)),
         "the segment counts the unsent save: '" + segment() + "'");
  Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, "remote-reconnect " + target), "reconnect");
  Expect(WaitUntil(
             [&] {
               Pump(shell);
               return segment() == target;
             },
             std::chrono::seconds(60), std::chrono::milliseconds(10)),
         "and comes back: '" + segment() + "'");

  // Git while the link is down answers nothing — the status bar says unknown, not
  // a made-up clean HEAD — and the reconnect asks git again by itself.
  // Pump() also consumes terminal updates, which request a (throttled) git refresh
  // of their own — an idle session does not, so this section must not either.
  const auto pump_quietly = [&] { (void)WorkspaceShellTestAccess::ReloadProjectIfFilesChanged(shell, false); };
  const auto settle_git = [&] {
    pump_quietly();
    WorkspaceShellTestAccess::DrainProjectBackgroundExecutor(shell);
    WorkspaceShellTestAccess::ConsumeGitSidebarRefresh(shell);
    WorkspaceShellTestAccess::RefreshStatusBar(shell);
    return WorkspaceShellTestAccess::StatusBarSegmentText(shell, StatusBarSegmentId::Project);
  };
  Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, "remote-disconnect"), "disconnect again");
  Expect(WaitUntil([&] { pump_quietly(); return segment().find("disconnected") != std::string::npos; },
                   std::chrono::seconds(10), std::chrono::milliseconds(10)),
         "down again");
  (void)WorkspaceShellTestAccess::ExecuteCommandLine(shell, "git-refresh");
  // 30 s, not 10: the refresh queues behind any one already in flight over the
  // link, and under a parallel ctest run that took longer than 10 s.
  Expect(WaitUntil([&] { return settle_git().find("scm-unknown") != std::string::npos; },
                   std::chrono::seconds(30), std::chrono::milliseconds(10)),
         "a git refresh that could not reach the host reads as unknown: '" + settle_git() + "'");
  Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, "remote-reconnect " + target), "reconnect");
  Expect(WaitUntil([&] { return settle_git().find("scm-unknown") == std::string::npos; },
                   std::chrono::seconds(60), std::chrono::milliseconds(10)),
         "and the reconnect refreshes git by itself: '" + settle_git() + "'");

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
  // It is a copy of a file outside the project: read-only, and never trusted on
  // reopen — a local copy that went stale (or was tampered with) is refetched.
  Expect(WorkspaceShellTestAccess::ActiveEditor(shell).read_only(), "the host file's copy is read-only");
  {
    // The path bar names the host's file; the cache it lives in is an
    // implementation detail.
    const std::string crumb = WorkspaceShellTestAccess::BreadcrumbLabel(shell);
    const std::string host_file = ":" + (host.home / "include" / "other.h").generic_string();
    Expect(crumb.size() > host_file.size() &&
               crumb.compare(crumb.size() - host_file.size(), host_file.size(), host_file) == 0 &&
               crumb.find("host-files") == std::string::npos,
           "the path bar shows host:/abs/path, got '" + crumb + "'");
  }
  WorkspaceShellTestAccess::ActiveEditor(shell).InsertText("typed");
  Expect(!WorkspaceShellTestAccess::ActiveEditor(shell).dirty(), "and typing does not change it");
  {
    // A real keystroke is refused out loud, not swallowed.
    SDL_Event typed{};
    typed.type = SDL_EVENT_TEXT_INPUT;
    const std::string text = "x";
    typed.text.text = text.c_str();
    (void)shell.HandleEvent(typed);
    bool said = false;
    for (const auto& row : WorkspaceShellTestAccess::ActiveNotifications(shell)) {
      said = said || row.message == "Cannot edit in read-only editor";
    }
    Expect(said && !WorkspaceShellTestAccess::ActiveEditor(shell).dirty(),
           "typing into the read-only copy says so");
    // However the copy's view is made — a restored session, a split — it is
    // read-only: every view's content passes through the preferences.
    editor::TextViewport restored;
    restored.LoadContent("int restored;\n",
                         WorkspaceShellTestAccess::ActiveEditor(shell).path());
    WorkspaceShellTestAccess::ApplyEditorPreferences(shell, restored);
    Expect(restored.read_only(), "a copy's view made any other way is read-only too");
  }
  const std::filesystem::path other_copy =
      WorkspaceShellTestAccess::ProjectLauncher(shell).LocalPathFromHost(host.home / "include" / "other.h");
  std::filesystem::permissions(other_copy, std::filesystem::perms::owner_all);
  WriteFile(other_copy, "stale local copy\n");
  WriteFile(host.home / "include" / "other.h", "\n\n\nint target; // updated on the host\n");
  Expect(!WorkspaceShellTestAccess::OpenFileInNewTab(shell, other_copy), "reopening fetches again");
  Expect(WaitUntil(
             [&] {
               Pump(shell);
               return ReadFile(other_copy) == "\n\n\nint target; // updated on the host\n";
             },
             std::chrono::seconds(20), std::chrono::milliseconds(10)),
         "the copy is the host's current bytes, not the stale local ones");

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
    PointAtFakeHost(shell, host);
    Expect(WorkspaceShellTestAccess::OpenProjectTabWithLocality(shell, mirror, {}),
           "the mirror opens as a path");
    Expect(!WorkspaceShellTestAccess::ProjectLauncher(shell).is_local(),
           "and is recognized as the remote project it mirrors");
  }
  {
    // The mirror deleted outside the editor (a wiped data directory). A recents
    // row for it reopens by host and folder (Open Folder on Host, what the welcome
    // row runs), which makes the mirror again and syncs it.
    std::filesystem::remove_all(mirror.parent_path());
    WorkspaceShell shell;
    PointAtFakeHost(shell, host);
    Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, "remote-open " + target + ":" + host_root.string()),
           "the remote project reopens by its name");
    Expect(WorkspaceShellTestAccess::ProjectRoot(shell) == mirror &&
               WorkspaceShellTestAccess::ProjectCatalogId(shell, 0).IsRemote(),
           "as the same remote project, at the same mirror");
    Expect(WaitUntil(
               [&] {
                 Pump(shell);
                 return std::filesystem::exists(mirror / "main.c");
               },
               std::chrono::seconds(30), std::chrono::milliseconds(10)),
           "and the mirror fills from the host again");
  }
  RemoveFakeMaster(target);
}

// A session listing a local project (active) and a remote one (inactive): the
// remote project's catalog entry is remote, and named by its host folder, from the
// moment it is restored -- before any connection, without being activated. Both
// session forms: one with identity records, and one written before them that names
// the mirror tree by path alone.
void TestARestoredRemoteProjectIsNamedBeforeItIsActivated() {
  TemporaryDirectory state_home;
  ScopedEnvVar scoped_state("XDG_STATE_HOME", state_home.path().string());
  TemporaryDirectory local_dir;
  const std::filesystem::path local = local_dir.path() / "local";
  std::filesystem::create_directories(local);
  const std::string host = "dev@restored-" + std::to_string(::getpid());
  const std::string host_root = "/home/dev/repo";
  const project::ProjectId remote_id = project::ProjectId::ForRemote(host, host_root);
  // The mirror as an earlier run left it; nothing here connects.
  const std::filesystem::path tree = remote_id.IoRoot();
  std::filesystem::create_directories(tree);
  std::filesystem::create_directories(tree.parent_path() / "meta");
  Expect(project::remote::RemoteProject::WriteRecord(tree, {.host = host, .host_root = host_root}),
         "the mirror's record is written");

  for (const bool legacy : {false, true}) {
    const std::string form = legacy ? " (legacy session)" : "";
    workspace::PersistedWorkspaceSessionState session;
    session.project_roots = {local, tree};
    if (!legacy) {
      session.projects = {project::ProjectId::ForLocal(local), remote_id};
    }
    const workspace::PersistenceService persistence;
    Expect(persistence.SaveWorkspaceSession(state_home.path() / "microide" / "workspace-session", session),
           "the session is written" + form);
    // A save is accepted, not landed: the shell below reads the file through its
    // own service, whose load flushes only its own queue. Unflushed, the restore
    // raced the write on a slow runner, and the second form's write could land
    // after the shell's save and overwrite it (CI, from 9201d8d6).
    persistence.FlushPendingWrites();

    WorkspaceShell shell;
    Expect(WorkspaceShellTestAccess::RestoreWorkspaceSession(shell), "the session restores" + form);
    Expect(WorkspaceShellTestAccess::ProjectCount(shell) == 2, "both projects are listed" + form);
    Expect(!WorkspaceShellTestAccess::ProjectCatalogEntryInitialized(shell, 1),
           "the remote project is not activated" + form);
    Expect(WorkspaceShellTestAccess::ProjectCatalogId(shell, 1) == remote_id,
           "its entry is already remote" + form + ": " + WorkspaceShellTestAccess::ProjectCatalogId(shell, 1).Location());
    Expect(WorkspaceShellTestAccess::ProjectTabDisplayTitle(shell, 1) == "repo" &&
               WorkspaceShellTestAccess::ProjectTabTooltipLabel(shell, 1) == host + ":" + host_root,
           "its tab is named by the host folder" + form + ": " +
               WorkspaceShellTestAccess::ProjectTabTooltipLabel(shell, 1));
    const util::JsonValue projects = WorkspaceShellTestAccess::ExecuteControlQuery(shell, "projects");
    Expect(projects.AsArray().size() == 2 && projects.AsArray()[1]["host"].AsString() == host &&
               projects.AsArray()[1]["hostRoot"].AsString() == host_root && !projects.AsArray()[0].HasKey("host"),
           "the projects query marks it remote before it connects" + form);
    Expect(!WorkspaceShellTestAccess::StatusBarSegmentVisible(shell, StatusBarSegmentId::Remote),
           "and nothing has connected" + form);
    WorkspaceShellTestAccess::SaveWorkspaceSession(shell);
    workspace::PersistedWorkspaceSessionState saved;
    Expect(persistence.LoadWorkspaceSession(state_home.path() / "microide" / "workspace-session", &saved) &&
               saved.projects.size() == 2 && saved.projects[1] == remote_id && saved.project_roots[1] == tree,
           "saving writes the identity beside its root" + form);
  }
}

// What counts as a host-local link: http(s), a loopback name, an explicit port.
// A mass delete the sync holds (§ 6.3) is a prompt the user can answer: its row
// carries Apply and Disconnect, and Apply deletes what the host deleted. The row
// once said "the deletion is held" with no button at all, so a hold lasted until
// the next reconnect.
void TestHeldMassDeleteIsAppliedFromItsRow() {
  FakeSshHost host;
  const std::filesystem::path host_root = host.home / "held";
  for (int i = 0; i < 150; ++i) {
    WriteFile(host_root / ("f" + std::to_string(i)), "x");
  }
  const std::string target = "dev@fake-held-" + std::to_string(::getpid());
  WorkspaceShell shell;
  PointAtFakeHost(shell, host);
  Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, "remote-open " + target + ":" + host_root.string()),
         "the command is accepted");
  const std::filesystem::path mirror = WorkspaceShellTestAccess::ProjectRoot(shell);
  Expect(WaitUntil(
             [&] {
               Pump(shell);
               return std::filesystem::exists(mirror / "f149");
             },
             std::chrono::seconds(30), std::chrono::milliseconds(10)),
         "the mirror fills from the host");

  // Not a repository, so nothing explains the deletes: they are held.
  for (int i = 0; i < 140; ++i) {
    std::filesystem::remove(host_root / ("f" + std::to_string(i)));
  }
  const auto held_row = [&]() -> const workspace::NotificationService::Notification* {
    for (const auto& row : WorkspaceShellTestAccess::ActiveNotifications(shell)) {
      if (row.message.find("would delete 140 files") != std::string::npos) {
        return &row;
      }
    }
    return nullptr;
  };
  Expect(WaitUntil(
             [&] {
               Pump(shell);
               return held_row() != nullptr;
             },
             std::chrono::seconds(20), std::chrono::milliseconds(10)),
         "the held delete is a row");
  const workspace::NotificationService::Notification* row = held_row();
  Expect(row->actions.size() == 2 && row->actions[0].label == "Apply" && row->actions[1].label == "Disconnect",
         "with Apply and Disconnect");
  Expect(std::filesystem::exists(mirror / "f0"), "and nothing was deleted yet");

  Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, "notification-action " + row->key + " Apply"),
         "Apply is pressed");
  Expect(WaitUntil(
             [&] {
               Pump(shell);
               return !std::filesystem::exists(mirror / "f0") && held_row() == nullptr;
             },
             std::chrono::seconds(20), std::chrono::milliseconds(10)),
         "the deletes apply and the row goes");
  Expect(std::filesystem::exists(mirror / "f149"), "what the host kept stays");
}

void TestLoopbackUrlsAreRecognized() {
  using workspace::RemoteHostService;
  const auto parsed = RemoteHostService::ParseLoopbackUrl("http://localhost:5173/src/main.ts?t=1#top");
  Expect(parsed.has_value() && parsed->scheme == "http" && parsed->port == 5173 &&
             parsed->rest == "/src/main.ts?t=1#top",
         "a Vite address splits into scheme, port and the rest");
  Expect(RemoteHostService::ParseLoopbackUrl("https://127.0.0.1:8443").has_value() &&
             RemoteHostService::ParseLoopbackUrl("http://0.0.0.0:8000/").has_value() &&
             RemoteHostService::ParseLoopbackUrl("http://[::1]:3000").has_value(),
         "127.0.0.1, 0.0.0.0 and [::1] are loopback too");
  Expect(!RemoteHostService::ParseLoopbackUrl("http://localhost/").has_value() &&
             !RemoteHostService::ParseLoopbackUrl("http://example.com:3000").has_value() &&
             !RemoteHostService::ParseLoopbackUrl("ftp://localhost:21").has_value() &&
             !RemoteHostService::ParseLoopbackUrl("http://localhost:99999").has_value() &&
             !RemoteHostService::ParseLoopbackUrl("http://localhost.evil.com:80").has_value(),
         "no port, another host, another scheme or a bad port is not one");
}

// Show Status names the mirror under ~/.local/share: the home prefix is spelled
// "~" so the part that differs survives the toast's three lines — and only a
// whole leading component is, never a longer sibling's prefix.
void TestStatusPathsAbbreviateHome() {
  using workspace::HomeAbbreviatedPath;
  Expect(HomeAbbreviatedPath("/home/u/.local/share/microide/remote/h/app-1/app", "/home/u") ==
             "~/.local/share/microide/remote/h/app-1/app",
         "the home prefix becomes ~");
  Expect(HomeAbbreviatedPath("/home/u", "/home/u/") == "~", "home itself, trailing slash or not");
  Expect(HomeAbbreviatedPath("/home/u2/x", "/home/u") == "/home/u2/x", "a sibling is not home");
  Expect(HomeAbbreviatedPath("/srv/x", "/home/u") == "/srv/x", "elsewhere is unchanged");
  Expect(HomeAbbreviatedPath("/home/u/x", "") == "/home/u/x" &&
             HomeAbbreviatedPath("/home/u/x", "/") == "/home/u/x",
         "no HOME, or HOME=/, abbreviates nothing");
}

}  // namespace

void RegisterRemoteHostServiceTests(std::vector<TestCase>& tests) {
  AddTest(tests, "RemoteHostService/ARestoredRemoteProjectIsNamedBeforeItIsActivated",
          TestARestoredRemoteProjectIsNamedBeforeItIsActivated);
  AddTest(tests, "RemoteHostService/HeldMassDeleteIsAppliedFromItsRow", TestHeldMassDeleteIsAppliedFromItsRow);
  AddTest(tests, "RemoteHostService/StatusPathsAbbreviateHome", TestStatusPathsAbbreviateHome);
  AddTest(tests, "RemoteHostService/LoopbackUrlsAreRecognized", TestLoopbackUrlsAreRecognized);
  AddTest(tests, "RemoteHostService/OpenTerminalOnHostFromTheCommand",
          TestOpenTerminalOnHostFromTheCommand);
  AddTest(tests, "RemoteHostService/MixedTerminalsSayWhereTheyRun", TestMixedTerminalsSayWhereTheyRun);
  AddTest(tests, "RemoteHostService/HostTerminalSettingsReachTheProjectLauncher",
          TestHostTerminalSettingsReachTheProjectLauncher);
  AddTest(tests, "RemoteHostService/UnfetchedFilesAreListedAndOpenWhenFetched",
          TestUnfetchedFilesAreListedAndOpenWhenFetched);
  AddTest(tests, "RemoteHostService/ConflictMergesThreeWaysToTheHost", TestConflictMergesThreeWaysToTheHost);
  AddTest(tests, "RemoteHostService/APortThatStartsListeningIsForwarded", TestAPortThatStartsListeningIsForwarded);
  AddTest(tests, "RemoteHostService/ProjectHomeStatusBlockFollowsTheConnection",
          TestProjectHomeStatusBlockFollowsTheConnection);
  AddTest(tests, "RemoteHostService/RemoveMirrorDeletesAClosedMirrorAndKeepsUnsentSaves",
          TestRemoveMirrorDeletesAClosedMirrorAndKeepsUnsentSaves);
  AddTest(tests, "RemoteHostService/OpenFolderOnHostEditsTheHostTree",
          TestOpenFolderOnHostEditsTheHostTree);
}

}  // namespace microide::tests
