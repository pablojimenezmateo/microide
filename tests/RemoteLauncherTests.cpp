#include "TestSupport.h"

#include "platform/AsyncSubprocess.h"
#include "project/GitRepository.h"
#include "project/GitStatusRefresh.h"
#include "project/ProjectSearchService.h"
#include "project/remote/RemoteProcessLauncher.h"
#include "project/remote/RemoteProtocol.h"
#include "project/remote/RemoteServerClient.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
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
      remote::RemoteServerClient::Create();
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

  auto client = remote::RemoteServerClient::Create();
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
  // At least the first push and the host commit's: nothing but a push can bring
  // the latter. The add's staged status may come from the add's own answer and
  // its push be coalesced with the next on a slow runner (CI ASan counted 2 on
  // 2026-10-21) -- the content checks above are what each step is held to.
  Expect(pushes.load() >= 2, "the host's changes arrive as pushes (" + std::to_string(pushes.load()) + ")");
}

// A process's exit is sent when it happens. The server found exits by polling
// every 50 ms, and a process's pipes reach EOF a moment before it can be reaped:
// on a real host (worky, 2026-10-15) every remote process -- each git the sidebar
// runs -- waited out the poll, 50 ms after its output, a round trip on that link.
// The exit now wakes the loop (a pidfd). Deterministic here: the child closes its
// output, then exits 20 ms later, so polling cannot see the exit before 50 ms.
void TestHostProcessExitIsSentWhenItHappens() {
  TemporaryDirectory temp;
  const std::filesystem::path local = temp.path() / "mirror";
  const std::filesystem::path host = temp.path() / "host";
  std::filesystem::create_directories(local);
  std::filesystem::create_directories(host);
  auto client = remote::RemoteServerClient::Create();
  std::string error;
  Expect(client->ConnectCommand({MICROIDE_SERVER_BINARY, "serve-stdio"},
                                remote::HelloRequest{.release = "test", .root = host.string()},
                                &error),
         "connects: " + error);
  const remote::RemoteProcessLauncher launcher(client, remote::RemotePathMap(local, host),
                                               {.host_paths_readable_locally = true});
  std::vector<double> ms;
  for (int i = 0; i < 9; ++i) {
    const auto start = std::chrono::steady_clock::now();
    const auto result = launcher.Run({"/bin/sh", "-c", "exec >&- 2>&-; sleep 0.02; exit 3"},
                                     platform::SubprocessOptions{.cwd = local});
    ms.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
    Expect(result.exit_code == 3, "the exit status arrives: " + std::to_string(result.exit_code));
  }
  std::sort(ms.begin(), ms.end());
  const double median = ms[ms.size() / 2];
  if (!IsSanitizerBuild()) {
    // ~20 ms of sleep plus a few of spawn; polled, the exit waited for the 50 ms tick.
    Expect(median < 40.0, "the median run is " + std::to_string(median) + " ms");
  }
}

// A read-only git the client runs on the host (the sidebar's own `git diff
// <base>...HEAD`, a blame, a log) changes nothing, so its exit must not make the
// host compute and push a status. It did: every push refreshed the sidebar, the
// refresh ran its outgoing diff, and that diff's exit caused the next push -- a
// git status and a git diff ~2.5 times a second on an idle host, measured on a
// real one (worky, 2026-10-15), with the workspace queue too busy to publish a
// file change for 11 s.
void TestReadOnlyGitDoesNotTriggerAStatusPush() {
  TemporaryDirectory temp;
  const std::filesystem::path local = temp.path() / "mirror";
  const std::filesystem::path host = temp.path() / "host";
  std::filesystem::create_directories(local);
  InitializeGitRepo(host);
  WriteFile(host / "a.txt", "one\n");
  CommitAll(host, "init", "read-only git");
  WriteFile(host / "a.txt", "two\n");

  auto client = remote::RemoteServerClient::Create();
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
  Expect(WaitUntil([&] { return launcher.CurrentStatus(local).has_value(); }, std::chrono::seconds(10)),
         "the first push arrives");
  // A second push for the subscribe itself may still be in flight.
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  const int settled = pushes.load();

  // What the sidebar does on every push: read the status, then run its own diff.
  for (int round = 0; round < 6; ++round) {
    for (const std::vector<std::string>& argv :
         {std::vector<std::string>{"git", "diff", "--name-status", "-z", "HEAD"},
          std::vector<std::string>{"git", "log", "-1", "--format=%H"},
          std::vector<std::string>{"git", "--no-optional-locks", "status", "--porcelain=v2"}}) {
      Expect(launcher.Run(argv, platform::SubprocessOptions{.cwd = local}).success(),
             "read-only git ran on the host");
    }
  }
  // Longer than the host's 1 s repository sampler, so a push it caused would land.
  (void)WaitUntil([&] { return pushes.load() != settled; }, std::chrono::milliseconds(1500));
  Expect(pushes.load() == settled,
         "read-only git caused no status push: " + std::to_string(pushes.load() - settled));
  Expect(client->last_git_generation() == 0, "and no exit carried a git generation");

  // A mutation still does: its exit advances the generation and a push follows.
  Expect(launcher.Run({"git", "add", "a.txt"}, platform::SubprocessOptions{.cwd = local}).success(),
         "git add ran on the host");
  Expect(client->last_git_generation() > 0, "the mutation's exit carried a generation");
  Expect(WaitUntil([&] { return pushes.load() > settled; }, std::chrono::seconds(10)),
         "and its status was pushed");
}

// Project search in a remote project runs ON THE HOST (remote-projects.md § 6.11):
// over the host's real bytes — a mirror behind it would answer wrong without
// saying so — and only over the content set, never build/ or .git. With no host
// to ask, the local copy is searched and the update says so.
void TestProjectSearchRunsOnTheHostOverTheContentSet() {
  TemporaryDirectory temp;
  const std::filesystem::path local = temp.path() / "mirror";
  const std::filesystem::path host = temp.path() / "host";
  InitializeGitRepo(host);
  WriteFile(host / ".gitignore", "build/\n");
  WriteFile(host / "src/a.txt", "one needle here\n");
  WriteFile(host / "build/out.txt", "needle in an ignored file\n");
  CommitAll(host, "init", "remote search");
  WriteFile(host / "src/b.txt", "a fresh needle the mirror has not pulled\n");
  // The mirror is behind: a needle the host no longer has, and no b.txt.
  WriteFile(local / "src/a.txt", "old needle, old needle\n");

  auto client = remote::RemoteServerClient::Create();
  std::string error;
  Expect(client->ConnectCommand({MICROIDE_SERVER_BINARY, "serve-stdio"},
                                remote::HelloRequest{.release = "test", .root = host.string()},
                                &error),
         "connects: " + error);
  const remote::RemoteProcessLauncher launcher(client, remote::RemotePathMap(local, host), {});
  const auto search = [&](const project::ProjectSearchHost* search_host) {
    project::ProjectSearchService service;
    service.Start(local, "needle", {},
                  std::make_shared<const std::vector<std::string>>(std::vector<std::string>{"src/a.txt"}),
                  search_host);
    Expect(WaitUntil([&] { return service.WorkerFinished(); }, std::chrono::seconds(20)),
           "the search finishes");
    service.WaitForWorkersIdle();
    return service.TakePendingUpdate();
  };

  const project::ProjectSearchUpdate on_host = search(project::ProjectSearchHostFor(launcher));
  std::vector<std::string> files;
  for (const auto& result : on_host.results) {
    files.push_back(result.relative_path_string + ":" + std::to_string(result.line) + ":" +
                    result.preview);
  }
  std::sort(files.begin(), files.end());
  Expect(on_host.error.empty() && !on_host.searched_local_copy, "searched on the host: " + on_host.error);
  Expect(files.size() == 2 && files[0].find("src/a.txt") == 0 && files[0].find("one needle") != std::string::npos &&
             files[1].find("src/b.txt") == 0,
         "the host's bytes, every content-set file, nothing under build/: " +
             (files.empty() ? std::string("<none>") : files[0]));
  Expect(on_host.total_files == 3, "the denominator is the host's content set (.gitignore, a, b)");

  client->peer().Fail("host went away");
  const project::ProjectSearchUpdate offline = search(project::ProjectSearchHostFor(launcher));
  Expect(offline.searched_local_copy && offline.results.size() == 2 &&
             offline.results[0].preview.find("old needle") != std::string::npos,
         "offline, the local copy is searched — and labelled as such");
}

// Over a real ssh link, when one is configured: MICROIDE_TEST_SSH_HOST names a host
// with microide-server at MICROIDE_TEST_SSH_SERVER (default ~/microide-test/microide-server).
void TestOverARealSshLink() {
  const char* host = std::getenv("MICROIDE_TEST_SSH_HOST");
  if (host == nullptr || *host == '\0') {
    return;
  }
  const char* server = std::getenv("MICROIDE_TEST_SSH_SERVER");
  auto client = remote::RemoteServerClient::Create();
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

// A repository created on the host after connecting (`git init` in a host
// terminal) is seen: the push says what the root is, and it replaces the answer
// the launcher asked for and held.
void TestGitRepositoryCreatedOnTheHostIsSeen() {
  TemporaryDirectory temp;
  const std::filesystem::path local = temp.path() / "mirror";
  const std::filesystem::path host = temp.path() / "host";
  std::filesystem::create_directories(local);
  WriteFile(host / "a.txt", "one\n");

  auto client = remote::RemoteServerClient::Create();
  std::string error;
  Expect(client->ConnectCommand({MICROIDE_SERVER_BINARY, "serve-stdio"},
                                remote::HelloRequest{.release = "test", .root = host.string()}, &error),
         "connects: " + error);
  const remote::RemoteProcessLauncher launcher(client, remote::RemotePathMap(local, host), {});
  Expect(client->SubscribeGitStatus([](const std::string&) {}, &error), "git status is subscribed: " + error);
  Expect(WaitUntil([&] { return launcher.Availability(local) == project::GitAvailability::NotARepository; },
                   std::chrono::seconds(10)),
         "not a repository yet");
  InitializeGitRepo(host);
  Expect(WaitUntil([&] { return launcher.Availability(local) == project::GitAvailability::Repository; },
                   std::chrono::seconds(20)),
         "the host's git init is noticed");
}

// A command running when the link drops returns at once with the link's failure,
// timeout or not: it used to sit out its whole timeout (60 s for git), holding the
// worker that ran it.
void TestRunReturnsWhenTheLinkDrops() {
  TemporaryDirectory temp;
  auto client = remote::RemoteServerClient::Create();
  std::string error;
  Expect(client->ConnectCommand({MICROIDE_SERVER_BINARY, "serve-stdio"},
                                remote::HelloRequest{.release = "test", .root = temp.path().string()}, &error),
         "connects: " + error);
  const remote::RemoteProcessLauncher launcher(client, remote::RemotePathMap({}, {}), {});
  std::atomic<bool> returned{false};
  platform::SubprocessResult result;
  std::thread runner([&] {
    result = launcher.Run({"sleep", "30"}, platform::SubprocessOptions{.timeout_ms = 60000});
    returned = true;
  });
  Expect(WaitUntil([&] { return launcher.spawn_count() == 1; }, std::chrono::seconds(10)), "the command runs");
  client->peer().Fail("simulated link death");
  const bool prompt = WaitUntil([&] { return returned.load(); }, std::chrono::seconds(5));
  runner.join();
  Expect(prompt, "it returns when the link drops, not at its timeout");
  Expect(!result.timed_out && !result.success() &&
             result.stderr_text.find("connection to the host was lost") != std::string::npos,
         "and says why: " + result.stderr_text);
}

#endif

}  // namespace

void RegisterRemoteLauncherTests(std::vector<TestCase>& tests) {
#if defined(__unix__) || defined(__APPLE__)
  AddTest(tests, "RemoteLauncher/RunMapsCwdAndReportsLikeALocalRun",
          TestRunMapsCwdAndReportsLikeALocalRun);
  AddTest(tests, "RemoteLauncher/StartAsyncAdoptsAHostProcess", TestStartAsyncAdoptsAHostProcess);
  AddTest(tests, "RemoteLauncher/GitMetadataComesFromTheHost", TestGitMetadataComesFromTheHost);
  AddTest(tests, "RemoteLauncher/ProjectSearchRunsOnTheHostOverTheContentSet",
          TestProjectSearchRunsOnTheHostOverTheContentSet);
  AddTest(tests, "RemoteLauncher/GitRepositoryCreatedOnTheHostIsSeen", TestGitRepositoryCreatedOnTheHostIsSeen);
  AddTest(tests, "RemoteLauncher/RunReturnsWhenTheLinkDrops", TestRunReturnsWhenTheLinkDrops);
  AddTest(tests, "RemoteLauncher/HostProcessExitIsSentWhenItHappens",
          TestHostProcessExitIsSentWhenItHappens);
  AddTest(tests, "RemoteLauncher/ReadOnlyGitDoesNotTriggerAStatusPush",
          TestReadOnlyGitDoesNotTriggerAStatusPush);
  AddTest(tests, "RemoteLauncher/GitStatusIsPushedAndNeverStaleAfterAMutation",
          TestGitStatusIsPushedAndNeverStaleAfterAMutation);
  AddTest(tests, "RemoteLauncher/OverARealSshLink", TestOverARealSshLink);
#else
  (void)tests;
#endif
}

}  // namespace microide::tests
