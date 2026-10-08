#include "TestSupport.h"

#include "RemoteServerTestSupport.h"
#include "project/remote/RemoteProject.h"

#include <chrono>
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
}

#endif

}  // namespace

void RegisterRemoteProjectTests(std::vector<TestCase>& tests) {
#if defined(__unix__) || defined(__APPLE__)
  AddTest(tests, "RemoteProject/OpensAMirrorOverSsh", TestRemoteProjectOpensAMirrorOverSsh);
#else
  (void)tests;
#endif
}

}  // namespace microide::tests
