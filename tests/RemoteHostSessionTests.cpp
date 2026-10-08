#include "TestSupport.h"
#include "RemoteServerTestSupport.h"

#include "platform/ProcessLauncher.h"
#include "platform/Subprocess.h"
#include "project/remote/RemoteHostSession.h"
#include "project/remote/RemoteProcessLauncher.h"
#include "project/remote/RemoteServerClient.h"
#include "terminal/TerminalSession.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

#if defined(__unix__) || defined(__APPLE__)
#include <sys/stat.h>
#endif

namespace microide::tests {
namespace {

namespace remote = project::remote;
using remote::RemoteHostSession;
using State = RemoteHostSession::State;

void TestHostTargetsAreValidatedBeforeAnythingRuns() {
  std::string error;
  const auto full = remote::ParseRemoteHostTarget(" dev@build-box.lan:2222 ", &error);
  Expect(full.has_value() && full->user == "dev" && full->host == "build-box.lan" &&
             full->port == 2222 && full->Display() == "dev@build-box.lan:2222",
         "user, host and port parse: " + error);
  const auto bare = remote::ParseRemoteHostTarget("box", &error);
  Expect(bare.has_value() && bare->user.empty() && bare->port == 0, "a bare host parses");
  for (const char* hostile : {"-oProxyCommand=evil", "box;rm -rf ~", "a b", "user@-host", "",
                              "box:0", "box:65536", "box:22x", "$(id)", "-l@box"}) {
    Expect(!remote::ParseRemoteHostTarget(hostile, &error).has_value(),
           std::string("refused before any process is spawned: ") + hostile);
  }
}

#if defined(__unix__) || defined(__APPLE__)

// The fake host: the ssh shim, a home directory, and the server it starts there,
// stopped when the test ends.
struct FakeHost : FakeSshHost {
  RemoteHostSession::Config Config(bool needs_auth = false) const {
    RemoteHostSession::Config config;
    config.target = *remote::ParseRemoteHostTarget("dev@fake-host", nullptr);
    config.ssh = SshArgv(needs_auth);
    config.control_dir = dir.scratch() / "c";
    config.server_binary = MICROIDE_SERVER_BINARY;
    config.release = "test";
    config.min_backoff = std::chrono::milliseconds(50);
    config.auth_poll = std::chrono::milliseconds(20);
    return config;
  }
};

struct StateLog {
  std::mutex mutex;
  std::vector<State> states;
  std::string last_error;

  RemoteHostSession::Listener Listener() {
    return [this](const RemoteHostSession::Status& status) {
      std::lock_guard lock(mutex);
      states.push_back(status.state);
      if (!status.error.empty()) {
        last_error = status.error;
      }
    };
  }
  bool Saw(State state) {
    std::lock_guard lock(mutex);
    return std::find(states.begin(), states.end(), state) != states.end();
  }
  std::string Error() {
    std::lock_guard lock(mutex);
    return last_error;
  }
};

bool WaitForState(const RemoteHostSession& session, State state) {
  return WaitUntil([&] { return session.status().state == state; }, std::chrono::seconds(20),
                   std::chrono::milliseconds(5));
}

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

bool WaitForText(const terminal::TerminalSession& session, std::string_view needle) {
  return WaitUntil([&] { return ScreenText(session).find(needle) != std::string::npos; },
                   std::chrono::seconds(15), std::chrono::milliseconds(5));
}

// A host with no microide-server anywhere: the session brings up the master,
// finds nothing to attach to, installs the client's own server over the same
// connection (0700 under ~/.local/share/microide/server), and reaches Ready with
// no action from the user. A terminal opened over it is a shell on that host.
void TestFirstConnectInstallsTheServerAndReachesReady() {
  FakeHost host;
  StateLog log;
  RemoteHostSession session(host.Config(), log.Listener());
  session.Connect();
  Expect(WaitForState(session, State::Ready), "the session is ready: " + log.Error());
  Expect(log.Saw(State::Installing), "it installed the server on the way");
  const std::filesystem::path installed = host.home / ".local/share/microide/server/microide-server";
  struct stat st{};
  Expect(::stat(installed.c_str(), &st) == 0 && (st.st_mode & 07777u) == 0700,
         "the server is installed 0700 at " + installed.string());
  Expect(std::filesystem::exists(host.dir.scratch() / "c"), "the control directory exists");

  const remote::RemoteProcessLauncher launcher(session.connection(), remote::RemotePathMap({}, {}),
                                               {.description = "fake-host"});
  terminal::TerminalSession terminal;
  Expect(terminal.Start(launcher, "/nowhere/local", {}, "sh"), "a host terminal opens");
  terminal.SendBytes("echo HOME-IS-$HOME\n");
  Expect(WaitForText(terminal, "HOME-IS-" + host.home.string()),
         "the shell runs on the host, in its home:\n" + ScreenText(terminal));
  terminal.Stop();
  session.Disconnect();
  Expect(WaitForState(session, State::Disconnected), "it disconnects");
}

// A key with a passphrase: the batch master fails, the session reports NeedsAuth,
// and once the user has answered in a terminal (here: the interactive master
// command, run as the terminal would) it carries on by itself.
void TestNeedsAuthWaitsForTheInteractiveMaster() {
  FakeHost host;
  StateLog log;
  RemoteHostSession session(host.Config(/*needs_auth=*/true), log.Listener());
  session.Connect();
  Expect(WaitForState(session, State::NeedsAuth), "the session asks for authentication");
  const std::vector<std::string> interactive = session.InteractiveMasterArgv();
  Expect(std::find(interactive.begin(), interactive.end(), "BatchMode=yes") == interactive.end(),
         "the terminal's ssh is not in batch mode, so it can prompt");
  Expect(platform::LocalProcessLauncher().Run(interactive, {}).success(),
         "the user authenticates in the terminal");
  Expect(WaitForState(session, State::Ready), "and the session reaches Ready: " + log.Error());
}

// The link dies: the session reconnects by itself, and a host terminal that was
// open resumes warm over the new connection — its output kept, nothing repeated,
// input working again.
void TestLinkDeathReconnectsAndTerminalsResume() {
  FakeHost host;
  StateLog log;
  RemoteHostSession session(host.Config(), log.Listener());
  session.Connect();
  Expect(WaitForState(session, State::Ready), "ready: " + log.Error());
  const remote::RemoteProcessLauncher launcher(session.connection(), remote::RemotePathMap({}, {}),
                                               {.description = "fake-host"});
  terminal::TerminalSession terminal;
  Expect(terminal.Start(launcher, {}, {}, "sh"), "a host terminal opens");
  terminal.SendBytes("echo before-$((2*3)); sleep 1; echo during-$((3*3))\n");
  Expect(WaitForText(terminal, "before-6"), "output before the link dies");
  const auto client = session.connection()->client();
  client->peer().Fail("simulated link death");
  Expect(WaitUntil([&] { return log.Saw(State::Reconnecting); }, std::chrono::seconds(10)),
         "the session notices and reconnects");
  Expect(WaitForState(session, State::Ready), "ready again: " + log.Error());
  Expect(session.connection()->client() != client, "over a new connection");
  Expect(WaitForText(terminal, "during-9"), "the terminal resumed, with what it printed meanwhile:\n" +
                                                ScreenText(terminal));
  terminal.SendBytes("echo after-$((4*4))\n");
  Expect(WaitForText(terminal, "after-16"), "and it takes input again");
  const std::string text = ScreenText(terminal);
  Expect(text.find("before-6") == text.rfind("before-6"), "nothing repeated:\n" + text);
  terminal.Stop();
}

// A server command that exits at once (a broken install, a wrong
// remote.server_command) fails the attempt promptly, with a reason the user can
// act on — not a hang, and not an empty error row.
void TestABrokenServerCommandFailsFastWithAReason() {
  FakeHost host;
  StateLog log;
  RemoteHostSession::Config config = host.Config();
  config.server_command = "no-such-microide-server";
  RemoteHostSession session(config, log.Listener());
  const auto started = std::chrono::steady_clock::now();
  session.Connect();
  Expect(WaitUntil([&] { return !log.Error().empty(); }, std::chrono::seconds(20)),
         "the attempt reports an error");
  Expect(std::chrono::steady_clock::now() - started < std::chrono::seconds(5),
         "promptly: the server's exit is the answer, not a timeout");
  Expect(session.status().state == State::Disconnected, "and ends Disconnected");
  Expect(!log.Saw(State::Installing), "a configured server command is never overwritten by install");
  Expect(log.Error().find("remote.server_command") != std::string::npos,
         "the error names the setting to fix: " + log.Error());
}

#endif

}  // namespace

void RegisterRemoteHostSessionTests(std::vector<TestCase>& tests) {
  AddTest(tests, "RemoteHostSession/HostTargetsAreValidatedBeforeAnythingRuns",
          TestHostTargetsAreValidatedBeforeAnythingRuns);
#if defined(__unix__) || defined(__APPLE__)
  AddTest(tests, "RemoteHostSession/FirstConnectInstallsTheServerAndReachesReady",
          TestFirstConnectInstallsTheServerAndReachesReady);
  AddTest(tests, "RemoteHostSession/NeedsAuthWaitsForTheInteractiveMaster",
          TestNeedsAuthWaitsForTheInteractiveMaster);
  AddTest(tests, "RemoteHostSession/LinkDeathReconnectsAndTerminalsResume",
          TestLinkDeathReconnectsAndTerminalsResume);
  AddTest(tests, "RemoteHostSession/ABrokenServerCommandFailsFastWithAReason",
          TestABrokenServerCommandFailsFastWithAReason);
#endif
}

}  // namespace microide::tests
