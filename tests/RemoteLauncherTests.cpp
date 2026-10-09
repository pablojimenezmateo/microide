#include "TestSupport.h"

#include "platform/AsyncSubprocess.h"
#include "project/GitRepository.h"
#include "project/GitStatusRefresh.h"
#include "project/remote/RemoteProcessLauncher.h"
#include "project/remote/RemoteProtocol.h"
#include "project/remote/RemoteServerClient.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#if defined(__unix__) || defined(__APPLE__)
#include <csignal>
#endif

namespace microide::tests {
namespace {

namespace remote = project::remote;

#if defined(__unix__) || defined(__APPLE__)

remote::HelloRequest TestHello() {
  return remote::HelloRequest{.release = "test", .root = ""};
}

// A launcher over a real `microide-server serve-stdio`, with an editor-side tree
// (`local`) that is a DIFFERENT directory from the host tree, so an unmapped path
// shows up as the wrong answer rather than passing by accident.
struct Session {
  TemporaryDirectory temp;
  std::filesystem::path local = temp.path() / "mirror";
  std::filesystem::path host = temp.path() / "host";
  std::shared_ptr<remote::RemoteServerClient> client =
      std::make_shared<remote::RemoteServerClient>();
  std::unique_ptr<remote::RemoteProcessLauncher> launcher;

  explicit Session(std::vector<std::string> command = {MICROIDE_SERVER_BINARY, "serve-stdio"}) {
    std::filesystem::create_directories(local);
    std::filesystem::create_directories(host);
    std::string error;
    Expect(client->ConnectCommand(command, TestHello(), &error), "connects: " + error);
    launcher = std::make_unique<remote::RemoteProcessLauncher>(
        client, remote::RemotePathMap(local, host),
        remote::RemoteProcessLauncher::Options{.host_paths_readable_locally = true});
  }
};

// Run maps the working directory onto the host tree, passes argv and stdin through
// untouched, and reports exit status like the local layer.
void TestRunMapsCwdAndReportsLikeALocalRun() {
  Session session;
  const auto pwd = session.launcher->Run({"pwd"}, platform::SubprocessOptions{.cwd = session.local});
  Expect(pwd.success() && pwd.stdout_text == std::filesystem::canonical(session.host).string() + "\n",
         "the process ran in the HOST tree: " + pwd.stdout_text);
  const auto cat = session.launcher->Run(
      {"cat"}, platform::SubprocessOptions{.cwd = session.local, .stdin_text = "fed\nthrough\n"});
  Expect(cat.stdout_text == "fed\nthrough\n", "stdin reaches the process");
  const auto failing =
      session.launcher->Run({"sh", "-c", "echo err >&2; exit 4"}, platform::SubprocessOptions{});
  Expect(failing.exit_code == 4 && failing.stderr_text == "err\n", "exit code and stderr");
  const auto killed = session.launcher->Run({"sh", "-c", "kill -TERM $$"}, platform::SubprocessOptions{});
  Expect(killed.exit_code == 128 + SIGTERM, "a signal reads as 128 + signo, like a local run");
  platform::SubprocessOptions env_options;
  env_options.environment_overrides = {{"MICROIDE_SET", "yes"}, {"HOME", std::nullopt}};
  const auto env = session.launcher->Run({"sh", "-c", "echo \"$MICROIDE_SET:${HOME-unset}\""},
                                         env_options);
  Expect(env.stdout_text == "yes:unset\n", "environment overrides set and unset: " + env.stdout_text);
  const auto slow = session.launcher->Run({"sleep", "30"}, platform::SubprocessOptions{.timeout_ms = 300});
  Expect(slow.timed_out && !slow.success(), "a timeout kills it and says so");
  Expect(session.launcher->spawn_count() == 6, "one host process per Run");
}

// A long-lived process adopted through StartAsync behaves like a local
// AsyncSubprocess: write, read, close stdin, exit — and Shutdown terminates it.
void TestStartAsyncAdoptsAHostProcess() {
  Session session;
  platform::AsyncSubprocess echo;
  Expect(session.launcher->StartAsync(echo, {"cat"}, session.local, {}), "started");
  Expect(echo.IsRunning() && echo.pid() > 0, "running with the host's pid");
  Expect(echo.Write("ping\n"), "write");
  std::string seen;
  Expect(WaitUntil([&]() {
           if (auto bytes = echo.Read(4096, 50); bytes.has_value()) seen += *bytes;
           return seen == "ping\n";
         }),
         "the echo comes back through the protocol: " + seen);
  echo.CloseStdin();
  Expect(WaitUntil([&]() { return !echo.IsRunning(); }, std::chrono::seconds(10)),
         "closing stdin ends it");
  Expect(echo.exit_code() == 0, "exit 0");

  platform::AsyncSubprocess sleeper;
  Expect(session.launcher->StartAsync(sleeper, {"sleep", "60"}, session.local, {}), "started");
  sleeper.Shutdown(1000);
  Expect(!sleeper.IsRunning() && sleeper.exit_code() == 128 + SIGTERM,
         "Shutdown terminates it: running=" + std::to_string(sleeper.IsRunning()) +
             " exit=" + std::to_string(sleeper.exit_code().value_or(-999)));
}

// Git metadata comes from the host (the mirror has no .git) and is cached: asking
// twice is one round trip, and a probe never costs a host process.
void TestGitMetadataComesFromTheHost() {
  Session session;
  std::filesystem::create_directories(session.host / ".git");
  WriteFile(session.host / ".git" / "HEAD", "ref: refs/heads/main\n");
  Expect(session.launcher->Availability(session.local) == project::GitAvailability::Repository,
         "the host tree is a repository though the mirror has no .git");
  Expect(session.launcher->ReadableGitDirectory(session.local) == session.host / ".git",
         "and its git directory is the host's");
  Expect(session.launcher->Availability(session.temp.path() / "elsewhere") ==
             project::GitAvailability::Unknown,
         "a path outside the project is not answered");
  Expect(session.launcher->spawn_count() == 0, "no host process was started to find out");
}

// The git sidebar's status is PUSHED (remote-projects.md § 6.5): the host runs
// `git status` after every change to the worktree or the repository and the
// client builds the sidebar from it without a host process. A git command the
// client runs that may change the repository makes any earlier push stale until
// one computed after it arrives — the sidebar never shows a stage as undone.
void TestGitStatusIsPushedAndNeverStaleAfterAMutation() {
  TemporaryDirectory temp;
  const std::filesystem::path local = temp.path() / "mirror";
  const std::filesystem::path host = temp.path() / "host";
  std::filesystem::create_directories(local);
  InitializeGitRepo(host);
  WriteFile(host / "a.txt", "one\n");
  CommitAll(host, "init", "pushed git status");
  WriteFile(host / "a.txt", "two\n");

  auto client = std::make_shared<remote::RemoteServerClient>();
  std::string error;
  Expect(client->ConnectCommand({MICROIDE_SERVER_BINARY, "serve-stdio"},
                                remote::HelloRequest{.release = "test", .root = host.string()},
                                &error),
         "connects: " + error);
  const remote::RemoteProcessLauncher launcher(client, remote::RemotePathMap(local, host),
                                               {.host_paths_readable_locally = true});
  client->SetWatchHandler([](std::uint64_t, std::string) {});
  Expect(client->Call(remote::method::kWatchSubscribe, util::JsonValue(util::JsonObject{}), &error)
             .has_value(),
         "the watch is subscribed: " + error);
  std::atomic<int> pushes{0};
  Expect(client->SubscribeGitStatus([&](const std::string&) { ++pushes; }, &error),
         "git status is subscribed: " + error);
  const auto status_has = [&](std::string_view needle) {
    const auto current = launcher.CurrentStatus(local);
    return current.has_value() && current->output.find(needle) != std::string::npos;
  };
  Expect(WaitUntil([&] { return status_has("1 .M N... 100644 100644 100644"); },
                   std::chrono::seconds(10)),
         "the first push shows the modified file");

  const std::size_t spawns = launcher.spawn_count();
  const project::GitRepository repo(local, launcher);
  const project::GitRepositoryState state =
      project::BuildGitRepositoryStateFromStatus(repo, local, 1, 0);
  Expect(launcher.spawn_count() == spawns, "the sidebar's state was built with no host process");
  Expect(state.entries.size() == 1 && state.entries[0].path.relative_path == "a.txt",
         "from the pushed status");

  const auto added = launcher.Run({"git", "add", "a.txt"}, platform::SubprocessOptions{.cwd = local});
  Expect(added.success(), "git add ran on the host");
  const auto right_after = launcher.CurrentStatus(local);
  Expect(!right_after.has_value() || right_after->output.find("1 M. N...") != std::string::npos,
         "right after the add, the status is either unknown or staged — never the unstaged one");
  Expect(WaitUntil([&] { return status_has("1 M. N..."); }, std::chrono::seconds(10)),
         "and the push computed after it arrives");

  // A commit made on the host by something else (an agent in a host terminal).
  RequireGitCommandSuccess(host, {"commit", "-q", "-m", "on the host"}, "host-side commit");
  Expect(WaitUntil([&] {
           const auto current = launcher.CurrentStatus(local);
           return current.has_value() && current->output.find("1 M.") == std::string::npos &&
                  current->output.find("# branch.head") != std::string::npos;
         },
                   std::chrono::seconds(10)),
         "the host's own commit is noticed and pushed");
  Expect(pushes.load() >= 3, "each change was a push");
}

// Over a real ssh link, when one is configured: MICROIDE_TEST_SSH_HOST names a host
// with microide-server at MICROIDE_TEST_SSH_SERVER (default ~/microide-test/microide-server).
void TestOverARealSshLink() {
  const char* host = std::getenv("MICROIDE_TEST_SSH_HOST");
  if (host == nullptr || *host == '\0') {
    return;
  }
  const char* server = std::getenv("MICROIDE_TEST_SSH_SERVER");
  auto client = std::make_shared<remote::RemoteServerClient>();
  std::string error;
  Expect(client->ConnectCommand({"ssh", "-o", "BatchMode=yes", host, "--",
                                 server != nullptr ? server : "microide-test/microide-server",
                                 "attach"},
                                TestHello(), &error),
         "connects over ssh: " + error);
  remote::RemoteProcessLauncher launcher(client, remote::RemotePathMap("/mirror", "/tmp"), {});
  const auto uname = launcher.Run({"uname", "-n"}, platform::SubprocessOptions{.cwd = "/mirror"});
  Expect(uname.success() && !uname.stdout_text.empty(), "a host process runs: " + uname.stderr_text);
  platform::AsyncSubprocess cat;
  Expect(launcher.StartAsync(cat, {"cat"}, "/mirror", {}) && cat.Write("over ssh\n"), "async");
  std::string seen;
  Expect(WaitUntil([&]() {
           if (auto bytes = cat.Read(4096, 50); bytes.has_value()) seen += *bytes;
           return seen == "over ssh\n";
         },
                   std::chrono::seconds(10)),
         "an adopted process echoes over the link");
  cat.Shutdown(1000);
}

#endif

}  // namespace

void RegisterRemoteLauncherTests(std::vector<TestCase>& tests) {
#if defined(__unix__) || defined(__APPLE__)
  AddTest(tests, "RemoteLauncher/RunMapsCwdAndReportsLikeALocalRun",
          TestRunMapsCwdAndReportsLikeALocalRun);
  AddTest(tests, "RemoteLauncher/StartAsyncAdoptsAHostProcess", TestStartAsyncAdoptsAHostProcess);
  AddTest(tests, "RemoteLauncher/GitMetadataComesFromTheHost", TestGitMetadataComesFromTheHost);
  AddTest(tests, "RemoteLauncher/GitStatusIsPushedAndNeverStaleAfterAMutation",
          TestGitStatusIsPushedAndNeverStaleAfterAMutation);
  AddTest(tests, "RemoteLauncher/OverARealSshLink", TestOverARealSshLink);
#else
  (void)tests;
#endif
}

}  // namespace microide::tests
