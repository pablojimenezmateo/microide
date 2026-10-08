#include "TestSupport.h"

#include "platform/AsyncSubprocess.h"
#include "project/remote/RemoteProcessLauncher.h"
#include "project/remote/RemoteServerClient.h"

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
  Expect(!sleeper.IsRunning() && sleeper.exit_code() == 128 + SIGTERM, "Shutdown terminates it");
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
  AddTest(tests, "RemoteLauncher/OverARealSshLink", TestOverARealSshLink);
#else
  (void)tests;
#endif
}

}  // namespace microide::tests
