#include "TestSupport.h"

#include "platform/AsyncSubprocess.h"
#include "project/GitRepository.h"
#include "project/GitStatusRefresh.h"
#include "project/ProjectSearchService.h"
#include "project/remote/RemotePathMap.h"
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

#include <csignal>

namespace microide::tests {
namespace {

namespace remote = project::remote;

remote::HelloRequest TestHello() {
  return remote::HelloRequest{.release = "test", .root = ""};
}

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

void TestPathMapReRootsUnderARootWithATrailingSeparator() {
  for (const char* host_root : {"/home/me/repo", "/home/me/repo/"}) {
    for (const char* local_root : {"/mirror/tree", "/mirror/tree/"}) {
      const remote::RemotePathMap map(local_root, host_root);
      const std::string where = std::string(" (host root ") + host_root + ", local root " + local_root + ")";
      Expect(map.ToLocal("/home/me/repo/src/main.cpp") == std::optional<std::filesystem::path>("/mirror/tree/src/main.cpp"),
             "a host path keeps every component" + where);
      Expect(map.ToHost("/mirror/tree/src/main.cpp") == std::optional<std::filesystem::path>("/home/me/repo/src/main.cpp"),
             "a local path keeps every component" + where);
      Expect(!map.ToLocal("/home/me/repository/x").has_value(), "a sibling with a longer name is outside" + where);
      Expect(map.ToLocal("/home/me/repo") == std::optional<std::filesystem::path>("/mirror/tree"),
             "the root maps to the root" + where);
    }
  }
}

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

  RequireGitCommandSuccess(host, {"commit", "-q", "-m", "on the host"}, "host-side commit");
  Expect(WaitUntil([&] {
           const auto current = launcher.CurrentStatus(local);
           return current.has_value() && current->output.find("1 M.") == std::string::npos &&
                  current->output.find("# branch.head") != std::string::npos;
         },
                   std::chrono::seconds(10)),
         "the host's own commit is noticed and pushed");
  Expect(pushes.load() >= 2, "the host's changes arrive as pushes (" + std::to_string(pushes.load()) + ")");
}

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
  const auto median_ms = [&](const char* script) {
    std::vector<double> ms;
    for (int i = 0; i < 9; ++i) {
      const auto start = std::chrono::steady_clock::now();
      const auto result = launcher.Run({"/bin/sh", "-c", script}, platform::SubprocessOptions{.cwd = local});
      ms.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
      Expect(result.exit_code == 3, "the exit status arrives: " + std::to_string(result.exit_code));
    }
    std::sort(ms.begin(), ms.end());
    return ms[ms.size() / 2];
  };
  const double control = median_ms("exec >&- 2>&-; sleep 0; exit 3");
  const double slept = median_ms("exec >&- 2>&-; sleep 0.02; exit 3");
  if (!IsSanitizerBuild()) {
    Expect(slept - control < 40.0, "the 20 ms sleep cost " + std::to_string(slept - control) +
                                       " ms over the control (" + std::to_string(control) + " ms)");
  }
}

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
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  const int settled = pushes.load();

  for (int round = 0; round < 6; ++round) {
    for (const std::vector<std::string>& argv :
         {std::vector<std::string>{"git", "diff", "--name-status", "-z", "HEAD"},
          std::vector<std::string>{"git", "log", "-1", "--format=%H"},
          std::vector<std::string>{"git", "--no-optional-locks", "status", "--porcelain=v2"}}) {
      Expect(launcher.Run(argv, platform::SubprocessOptions{.cwd = local}).success(),
             "read-only git ran on the host");
    }
  }
  (void)WaitUntil([&] { return pushes.load() != settled; }, std::chrono::milliseconds(1500));
  Expect(pushes.load() == settled,
         "read-only git caused no status push: " + std::to_string(pushes.load() - settled));
  Expect(client->last_git_generation() == 0, "and no exit carried a git generation");

  Expect(launcher.Run({"git", "add", "a.txt"}, platform::SubprocessOptions{.cwd = local}).success(),
         "git add ran on the host");
  Expect(client->last_git_generation() > 0, "the mutation's exit carried a generation");
  Expect(WaitUntil([&] { return pushes.load() > settled; }, std::chrono::seconds(10)),
         "and its status was pushed");
}

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

}

void RegisterRemoteLauncherTests(std::vector<TestCase>& tests) {
  AddTest(tests, "RemoteLauncher/PathMapReRootsUnderARootWithATrailingSeparator",
          TestPathMapReRootsUnderARootWithATrailingSeparator);
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
}

}
