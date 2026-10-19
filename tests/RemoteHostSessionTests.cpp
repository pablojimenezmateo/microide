#include "TestSupport.h"
#include "platform/RuntimePaths.h"
#include "RemoteServerTestSupport.h"

#include "platform/AsyncSubprocess.h"
#include "platform/ProcessLauncher.h"
#include "platform/Subprocess.h"
#include "project/remote/RemoteHostSession.h"
#include "project/remote/RemoteProcessLauncher.h"
#include "project/remote/RemoteServerClient.h"
#include "project/remote/RemoteServerPaths.h"
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
    config.server_binary = [](std::string_view) { return std::filesystem::path(MICROIDE_SERVER_BINARY); };
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

// A session that starts somewhere other than $HOME (sshd starts it in the passwd
// home; sshd's SetEnv, a login script or a container can point $HOME elsewhere).
// The install wrote relative to the working directory and the launch chain looked
// under $HOME, so the server landed where it is never looked for and every connect
// failed after the install (found standing up tools/qa/local-sshd.sh, whose
// sessions get a HOME of their own).
void TestTheServerInstallsUnderHomeWhereverTheSessionStarts() {
  FakeHost host;
  host.login_dir = host.dir.scratch() / "login";
  std::filesystem::create_directories(host.login_dir);
  StateLog log;
  RemoteHostSession session(host.Config(), log.Listener());
  session.Connect();
  Expect(WaitForState(session, State::Ready), "the session is ready: " + log.Error());
  Expect(std::filesystem::exists(host.home / ".local/share/microide/server/microide-server"),
         "the server is installed under $HOME, where the launch looks");
  Expect(!std::filesystem::exists(host.login_dir / ".local"), "and nothing lands in the login directory");
  session.Disconnect();
  Expect(WaitForState(session, State::Disconnected), "it disconnects");
}

// A compatible but OLDER server keeps serving (and keeps its terminals), and this
// release's binary is installed beside it for the next start; a restart picks it
// up. A server that is not older -- equal, newer, or unorderable -- is left alone.
void TestAnOlderServerGetsTheNewBinaryBesideIt() {
  FakeHost host;
  const std::filesystem::path installed = host.home / ".local/share/microide/server/microide-server";
  {
    StateLog log;
    RemoteHostSession::Config config = host.Config();
    config.release = "0.0.1";  // an editor older than the server: no downgrade
    RemoteHostSession session(config, log.Listener());
    session.Connect();
    Expect(WaitForState(session, State::Ready), "ready: " + log.Error());
    Expect(!session.update_installed(), "a newer server is left alone");
    session.Disconnect();
    Expect(WaitForState(session, State::Disconnected), "disconnects");
  }
  std::filesystem::remove(installed);  // so a reinstall below is observable
  {
    StateLog log;
    RemoteHostSession::Config config = host.Config();
    config.release = "999.0.0";  // an editor newer than the running server
    RemoteHostSession session(config, log.Listener());
    session.Connect();
    Expect(WaitForState(session, State::Ready), "ready: " + log.Error());
    Expect(session.update_installed(), "the older server is reported as having an update waiting");
    Expect(std::filesystem::exists(installed), "with this release's binary installed beside it");
    Expect(session.connection()->client() != nullptr, "while the old server keeps serving");
    session.Disconnect();
    Expect(WaitForState(session, State::Disconnected), "disconnects");
  }
}

// Install picks the binary for the HOST's architecture (`uname -m` over the same
// connection): the package ships one static server per architecture
// (tools/build-server.sh). A host whose architecture this build has no server for
// fails with a reason naming it, rather than installing one that cannot run there.
void TestInstallPicksTheBinaryForTheHostsArchitecture() {
  {
    FakeHost host;
    StateLog log;
    std::mutex mutex;
    std::vector<std::string> asked;
    RemoteHostSession::Config config = host.Config();
    config.server_binary = [&](std::string_view arch) {
      std::lock_guard lock(mutex);
      asked.emplace_back(arch);
      return std::filesystem::path(MICROIDE_SERVER_BINARY);
    };
    RemoteHostSession session(config, log.Listener());
    session.Connect();
    Expect(WaitForState(session, State::Ready), "ready: " + log.Error());
    std::lock_guard lock(mutex);
    Expect(!asked.empty() && asked.front() == platform::ThisMachineArchitecture(),
           "the binary was chosen for the host's own architecture (the fake host is this "
           "machine): " + (asked.empty() ? std::string("none") : asked.front()));
    session.Disconnect();
    Expect(WaitForState(session, State::Disconnected), "disconnects");
  }
  {
    FakeHost host;
    StateLog log;
    RemoteHostSession::Config config = host.Config();
    config.server_binary = [](std::string_view) { return std::filesystem::path(); };
    RemoteHostSession session(config, log.Listener());
    session.Connect();
    Expect(WaitUntil([&] { return !log.Error().empty(); }, std::chrono::seconds(20)),
           "an unpackaged architecture fails");
    Expect(log.Error().find("no microide-server for " + platform::ThisMachineArchitecture()) !=
               std::string::npos,
           "naming the architecture: " + log.Error());
    session.Disconnect();
  }
  Expect(platform::NormalizeArchitecture("arm64\n") == "aarch64" &&
             platform::NormalizeArchitecture("x86_64\n") == "x86_64",
         "uname -m output is normalized to the package's names");
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

// Port forwarding rides the ControlMaster: a forward is `ssh -O forward -L`
// against it, asking again for a forwarded port answers the same address, a new
// master (the old one died with the link) gets every forward back, and a cancel
// takes it off the master and out of the list.
void TestPortForwardsRideTheMasterAndSurviveANewOne() {
  FakeHost host;
  StateLog log;
  RemoteHostSession session(host.Config(), log.Listener());
  session.Connect();
  Expect(WaitForState(session, State::Ready), "ready: " + log.Error());
  std::string error;
  const auto forward = session.ForwardPort(39217, &error);
  Expect(forward.has_value() && forward->host_port == 39217 && forward->local_port != 0,
         "the port is forwarded: " + error);
  const std::filesystem::path held = session.control_path().string() + ".forwards";
  const std::string spec = forward.has_value()
                               ? "127.0.0.1:" + std::to_string(forward->local_port) + ":localhost:39217"
                               : std::string();
  Expect(ReadFile(held) == spec + "\n", "the master holds it: '" + ReadFile(held) + "'");
  Expect(session.ForwardPort(39217, &error) == forward && ReadFile(held) == spec + "\n",
         "asking again answers the same address and adds no second listener");

  // The master dies with the link: the new one is given the forward again.
  std::filesystem::remove(session.control_path());
  session.connection()->client()->peer().Fail("simulated link death");
  Expect(WaitUntil([&] { return log.Saw(State::Reconnecting); }, std::chrono::seconds(10)),
         "the session reconnects");
  Expect(WaitForState(session, State::Ready), "ready again: " + log.Error());
  Expect(ReadFile(held) == spec + "\n", "the new master forwards it too: '" + ReadFile(held) + "'");

  // A disconnect takes the listener down (the master outlives it); a reconnect
  // brings it back.
  session.Disconnect();
  Expect(WaitForState(session, State::Disconnected), "disconnected");
  Expect(ReadFile(held).empty(), "no listener while disconnected: '" + ReadFile(held) + "'");
  session.Connect();
  Expect(WaitForState(session, State::Ready), "ready once more: " + log.Error());
  Expect(ReadFile(held) == spec + "\n", "and forwarded again: '" + ReadFile(held) + "'");

  Expect(session.CancelForward(39217, &error), "cancel: " + error);
  Expect(ReadFile(held).empty() && session.forwards().empty(), "and it is gone from both");
  Expect(!session.CancelForward(39217, &error), "a port that is not forwarded is refused");
}

// Two sessions to one host (a project's and a host terminal's) share its
// ControlMaster, so they share its forwards: either one answers for a port the
// other forwarded, and a disconnect of one leaves the ports up for the other — the
// last one to go takes them down.
void TestPortForwardsAreTheHosts() {
  FakeHost host;
  StateLog log_a;
  StateLog log_b;
  RemoteHostSession::Config config = host.Config();
  config.forwards = std::make_shared<remote::HostPortForwards>();
  RemoteHostSession a(config, log_a.Listener());
  RemoteHostSession b(config, log_b.Listener());
  a.Connect();
  Expect(WaitForState(a, State::Ready), "a is ready: " + log_a.Error());
  b.Connect();
  Expect(WaitForState(b, State::Ready), "b is ready: " + log_b.Error());
  std::string error;
  const auto forward = a.ForwardPort(39219, &error);
  Expect(forward.has_value(), "a forwards the port: " + error);
  Expect(b.forwards().size() == 1 && b.ForwardPort(39219, &error) == forward,
         "b knows it: the same address, no second listener");
  const std::filesystem::path held = a.control_path().string() + ".forwards";
  const std::string spec = forward.has_value()
                               ? "127.0.0.1:" + std::to_string(forward->local_port) + ":localhost:39219\n"
                               : std::string();
  a.Disconnect();
  Expect(WaitForState(a, State::Disconnected), "a disconnects");
  Expect(ReadFile(held) == spec, "b still holds the host, so the port stays up: '" + ReadFile(held) + "'");
  b.Disconnect();
  Expect(WaitForState(b, State::Disconnected), "b disconnects");
  Expect(ReadFile(held).empty(), "the last one closes it: '" + ReadFile(held) + "'");
  Expect(a.forwards().size() == 1, "and the list is kept for the next connection");
}

// The hosts "Connect to Host…" offers: every concrete alias of every `Host` line,
// through `Include` (relative to ~/.ssh, globbed), never a pattern.
void TestSshConfigHostsFollowsIncludesAndSkipsPatterns() {
  TemporaryDirectory temp;
  const std::filesystem::path ssh = temp.path() / ".ssh";
  WriteFile(ssh / "config",
            "# personal\n"
            "Host worky build-box\n"
            "  HostName 10.0.0.7\n"
            "Host *.internal !bastion gw?\n"
            "  User me\n"
            "Include config.d/*\n"
            "include=\"~/.ssh/extra\"\n"
            "Host worky\n"
            "Host -oProxyCommand=x\n");
  WriteFile(ssh / "config.d" / "b-work", "Host db\nHost cache\n");
  WriteFile(ssh / "config.d" / "a-home", "Host nas\n");
  WriteFile(ssh / "extra", "Host  =  pi\n");
  const std::vector<std::string> hosts = remote::SshConfigHosts(ssh / "config", ssh);
  const std::vector<std::string> expected = {"worky", "build-box", "nas", "db", "cache", "pi"};
  std::string got;
  for (const std::string& host : hosts) {
    got += host + " ";
  }
  Expect(hosts == expected, "aliases in file order, includes in name order, no patterns, no "
                            "duplicates, nothing ssh would read as an option: " + got);
  Expect(remote::SshConfigHosts(ssh / "missing", ssh).empty(), "no config, no hosts");
}

// A language server or debug adapter on the host is a KEPT process: the link
// dying does not end it, input written during the outage is held and delivered
// after the reconnect, and its output keeps flowing — then a Disconnect ends it
// rather than leaving it running on the host for nobody.
void TestHostProcessSurvivesAReconnectAndEndsOnDisconnect() {
  FakeHost host;
  StateLog log;
  RemoteHostSession session(host.Config(), log.Listener());
  session.Connect();
  Expect(WaitForState(session, State::Ready), "ready: " + log.Error());
  const remote::RemoteProcessLauncher launcher(session.connection(), remote::RemotePathMap({}, {}),
                                               {.description = "fake-host"});
  platform::AsyncSubprocess process;
  Expect(launcher.StartAsync(process, {"cat"}, {}, {}), "a host process starts");
  const auto read_line = [&](std::string_view expected) {
    std::string got;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (got.find(expected) == std::string::npos && std::chrono::steady_clock::now() < deadline) {
      if (auto chunk = process.Read(4096, 100)) {
        got += *chunk;
      }
    }
    return got.find(expected) != std::string::npos;
  };
  Expect(process.Write("one\n") && read_line("one"), "it echoes before the drop");
  session.connection()->client()->peer().Fail("simulated link death");
  Expect(WaitUntil([&] { return log.Saw(State::Reconnecting); }, std::chrono::seconds(10)),
         "the link is down");
  Expect(process.Write("two\n"), "the consumer can still write during the outage");
  Expect(WaitForState(session, State::Ready), "ready again: " + log.Error());
  Expect(read_line("two"), "what was written during the outage reached the same process");
  Expect(process.IsRunning(), "the host process survived the reconnect");
  session.Disconnect();
  Expect(WaitUntil([&] { return !process.IsRunning(); }, std::chrono::seconds(10)),
         "a Disconnect ends it");
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

// `remote.server_socket_dir` reaches the host server as --socket-dir (a home on
// a filesystem without AF_UNIX support needs it), and `remote.backfill_inflight_
// bytes` travels in the hello: the server applies it, echoes it, and the client
// pins the same bound — one bulk window in both directions.
void TestSocketDirAndBackfillBoundReachTheServer() {
  FakeHost host;
  StateLog log;
  const std::filesystem::path socket_dir = host.dir.scratch() / "s";
  RemoteHostSession::Config config = host.Config();
  config.server_socket_dir = socket_dir.string();
  config.backfill_inflight_bytes = 200 * 1024;
  {
    RemoteHostSession session(config, log.Listener());
    session.Connect();
    Expect(WaitForState(session, State::Ready), "ready: " + log.Error());
    Expect(std::filesystem::exists(remote::ServerSocketPath(socket_dir)),
           "the server bound its socket in the configured directory");
    const auto client = session.connection()->client();
    Expect(client->hello().settings.backfill_inflight_bytes == 200 * 1024,
           "the server echoes the bound it applied");
    Expect(client->peer().bulk_window() == 200 * 1024, "and the client pins the same window");
    session.Disconnect();
  }
  (void)platform::RunSubprocess({MICROIDE_SERVER_BINARY, "stop", "--socket-dir", socket_dir.string()},
                                platform::SubprocessOptions{.timeout_ms = 10000});

  // A directory a login shell could read as syntax never reaches the host.
  StateLog bad_log;
  config.server_socket_dir = "/tmp/x; rm -rf ~";
  RemoteHostSession refused(config, bad_log.Listener());
  refused.Connect();
  Expect(WaitUntil([&] { return !bad_log.Error().empty(); }, std::chrono::seconds(10)),
         "an unsafe socket directory is refused");
  Expect(bad_log.Error().find("remote.server_socket_dir") != std::string::npos,
         "naming the setting: " + bad_log.Error());
}

#endif

}  // namespace

void RegisterRemoteHostSessionTests(std::vector<TestCase>& tests) {
  AddTest(tests, "RemoteHostSession/HostTargetsAreValidatedBeforeAnythingRuns",
          TestHostTargetsAreValidatedBeforeAnythingRuns);
#if defined(__unix__) || defined(__APPLE__)
  AddTest(tests, "RemoteHostSession/SshConfigHostsFollowsIncludesAndSkipsPatterns",
          TestSshConfigHostsFollowsIncludesAndSkipsPatterns);
  AddTest(tests, "RemoteHostSession/SocketDirAndBackfillBoundReachTheServer",
          TestSocketDirAndBackfillBoundReachTheServer);
  AddTest(tests, "RemoteHostSession/InstallPicksTheBinaryForTheHostsArchitecture",
          TestInstallPicksTheBinaryForTheHostsArchitecture);
  AddTest(tests, "RemoteHostSession/AnOlderServerGetsTheNewBinaryBesideIt",
          TestAnOlderServerGetsTheNewBinaryBesideIt);
  AddTest(tests, "RemoteHostSession/TheServerInstallsUnderHomeWhereverTheSessionStarts",
          TestTheServerInstallsUnderHomeWhereverTheSessionStarts);
  AddTest(tests, "RemoteHostSession/FirstConnectInstallsTheServerAndReachesReady",
          TestFirstConnectInstallsTheServerAndReachesReady);
  AddTest(tests, "RemoteHostSession/NeedsAuthWaitsForTheInteractiveMaster",
          TestNeedsAuthWaitsForTheInteractiveMaster);
  AddTest(tests, "RemoteHostSession/LinkDeathReconnectsAndTerminalsResume",
          TestLinkDeathReconnectsAndTerminalsResume);
  AddTest(tests, "RemoteHostSession/HostProcessSurvivesAReconnectAndEndsOnDisconnect",
          TestHostProcessSurvivesAReconnectAndEndsOnDisconnect);
  AddTest(tests, "RemoteHostSession/PortForwardsRideTheMasterAndSurviveANewOne",
          TestPortForwardsRideTheMasterAndSurviveANewOne);
  AddTest(tests, "RemoteHostSession/PortForwardsAreTheHosts", TestPortForwardsAreTheHosts);
  AddTest(tests, "RemoteHostSession/ABrokenServerCommandFailsFastWithAReason",
          TestABrokenServerCommandFailsFastWithAReason);
#endif
}

}  // namespace microide::tests
