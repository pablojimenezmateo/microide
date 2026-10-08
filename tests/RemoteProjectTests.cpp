#include "TestSupport.h"

#include "RemoteServerTestSupport.h"
#include "project/remote/RemoteProject.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <filesystem>
#include <string>

namespace microide::tests {
namespace {

namespace remote = project::remote;

#if defined(__unix__) || defined(__APPLE__)

void TestRemoteProjectOpensAMirrorOverSsh() {
  FakeSshHost host;
  const std::filesystem::path host_root = host.home / "src" / "app";
  WriteFile(host_root / "main.c", "int main(void) { return 0; }\n");
  WriteFile(host_root / "lib/util.h", "#pragma once\n");

  remote::RemoteProject::Config config;
  config.session.target = *remote::ParseRemoteHostTarget("dev@fake-host", nullptr);
  config.session.ssh = host.SshArgv();
  config.session.control_dir = host.dir.scratch() / "c";
  config.session.server_binary = MICROIDE_SERVER_BINARY;
  config.session.release = "test";
  config.session.min_backoff = std::chrono::milliseconds(50);
  config.session.workspace_root = host_root.string();
  config.mirror_directory = host.dir.scratch() / "m";
  remote::RemoteProject project(config, {});
  std::string error;
  Expect(project.Open(&error), "the project opens: " + error);
  Expect(project.tree().filename() == "app", "the mirror's tree is named after the host root");

  const bool synced = WaitUntil(
      [&] {
        project.engine().Flush();
        return project.engine().status().synced_once;
      },
      std::chrono::seconds(20), std::chrono::milliseconds(20));
  Expect(synced, "it connects and syncs: " + project.engine().status().error + " / " +
                     project.session().status().error);
  Expect(project.watch_native() == std::optional(true),
         "the host reports how it watches the tree (natively, here)");
  Expect(ReadFile(project.tree() / "main.c") == "int main(void) { return 0; }\n" &&
             ReadFile(project.tree() / "lib/util.h") == "#pragma once\n",
         "the mirror holds the host's tree");

  const project::ProjectLocality locality = project.locality();
  Expect(locality.launcher != nullptr && !locality.launcher->is_local(),
         "its processes run on the host");
  const auto written = locality.write_gate->WriteText(project.tree() / "main.c", "int x;\n");
  Expect(written.ok, "a save through the project's gate lands locally");
  project.engine().Flush();
  Expect(ReadFile(host_root / "main.c") == "int x;\n", "and reaches the host");

  const auto ran = locality.launcher->Run({"cat", "main.c"},
                                          platform::SubprocessOptions{.cwd = project.tree()});
  Expect(ran.success() && ran.stdout_text == "int x;\n",
         "a process started in the mirror runs in the host tree: " + ran.stderr_text);

  const auto record = remote::RemoteProject::ReadRecord(project.tree());
  Expect(record.has_value() && record->host == "dev@fake-host" &&
             record->host_root == host_root.string(),
         "the mirror tree is recognizable as a remote project");
  Expect(!remote::RemoteProject::ReadRecord(host_root).has_value(),
         "an ordinary folder is not");

  project.session().Disconnect();
  Expect(WaitUntil([&] { return project.session().status().state == remote::RemoteHostSession::State::Disconnected; },
                   std::chrono::seconds(10), std::chrono::milliseconds(10)),
         std::string("Disconnect takes the project offline: ") +
             std::string(remote::RemoteHostSession::StateName(project.session().status().state)));
}

// MICROIDE_BENCH_REMOTE=1: open this repository's src/ and tests/ as a remote
// project through the fake host and report how long the first full sync takes.
// A measurement, not a gate (no time assertions in the suite).
void TestRemoteProjectBenchFirstSync() {
  if (std::getenv("MICROIDE_BENCH_REMOTE") == nullptr) {
    return;
  }
  FakeSshHost host;
  const std::filesystem::path host_root = host.home / "src" / "microide";
  const std::filesystem::path repo = std::filesystem::path(MICROIDE_TEST_SOURCE_DIR).parent_path();
  std::filesystem::create_directories(host_root);
  for (const char* part : {"src", "tests"}) {
    std::filesystem::copy(repo / part, host_root / part, std::filesystem::copy_options::recursive);
  }
  std::size_t files = 0;
  std::uintmax_t bytes = 0;
  // A real host tree is mostly old: backdate it, as files edited an hour ago.
  const auto an_hour_ago = std::filesystem::file_time_type::clock::now() - std::chrono::hours(1);
  for (const auto& entry : std::filesystem::recursive_directory_iterator(host_root)) {
    if (entry.is_regular_file()) {
      ++files;
      bytes += entry.file_size();
      std::filesystem::last_write_time(entry.path(), an_hour_ago);
    }
  }
  remote::RemoteProject::Config config;
  config.session.target = *remote::ParseRemoteHostTarget("dev@bench-host", nullptr);
  config.session.ssh = host.SshArgv();
  config.session.control_dir = host.dir.scratch() / "c";
  config.session.server_binary = MICROIDE_SERVER_BINARY;
  config.session.release = "test";
  config.session.workspace_root = host_root.string();
  config.mirror_directory = host.dir.scratch() / "m";
  const auto sync_once = [&]() -> long long {
    const auto start = std::chrono::steady_clock::now();
    remote::RemoteProject project(config, {});
    std::string error;
    Expect(project.Open(&error), "opens: " + error);
    const bool synced = WaitUntil(
        [&] { return project.engine().status().synced_once; }, std::chrono::seconds(120),
        std::chrono::milliseconds(2));
    Expect(synced, "synced: " + project.engine().status().error);
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start)
        .count();
  };
  const long long cold = sync_once();
  // Past the racy window, so the recorded stats vouch for the mirror's bytes.
  std::this_thread::sleep_for(std::chrono::milliseconds(2100));
  const long long warm = sync_once();
  std::fprintf(stderr,
               "bench: %zu files, %.1f MiB: first sync %lld ms (connect + install + manifest + "
               "pulls), reopen %lld ms\n",
               files, static_cast<double>(bytes) / (1024.0 * 1024.0), cold, warm);
}

#endif

}  // namespace

void RegisterRemoteProjectTests(std::vector<TestCase>& tests) {
#if defined(__unix__) || defined(__APPLE__)
  AddTest(tests, "RemoteProject/OpensAMirrorOverSsh", TestRemoteProjectOpensAMirrorOverSsh);
  AddTest(tests, "RemoteProject/BenchFirstSync", TestRemoteProjectBenchFirstSync);
#else
  (void)tests;
#endif
}

}  // namespace microide::tests
