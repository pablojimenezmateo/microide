#include "TestSupport.h"

#include "perf/PerfHarnessIsolation.h"
#include "platform/AppDirectories.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>

namespace microide::tests {
namespace {

void TestEstablishIsolatedAppRootExportsAllXdgVars() {
  // Snapshot existing XDG values so we can restore them after the helper
  // mutates the process environment.
  ScopedEnvVar xdg_config("XDG_CONFIG_HOME", "");
  ScopedEnvVar xdg_state("XDG_STATE_HOME", "");
  ScopedEnvVar xdg_cache("XDG_CACHE_HOME", "");
  ScopedEnvVar xdg_data("XDG_DATA_HOME", "");

  std::string error;
  const std::filesystem::path root = perf::EstablishIsolatedAppRoot(false, &error);
  Expect(!root.empty(), "EstablishIsolatedAppRoot returned empty path: " + error);
  Expect(std::filesystem::is_directory(root), "sandbox root must exist");
  for (const char* sub : {"config", "state", "cache", "data"}) {
    Expect(std::filesystem::is_directory(root / sub),
           std::string("sandbox subdir missing: ") + sub);
  }
  const auto envEquals = [&](const char* name, const std::filesystem::path& expected) {
    const char* current = std::getenv(name);
    Expect(current != nullptr && expected.string() == current,
           std::string("env ") + name + " not pointing at sandbox");
  };
  envEquals("XDG_CONFIG_HOME", root / "config");
  envEquals("XDG_STATE_HOME", root / "state");
  envEquals("XDG_CACHE_HOME", root / "cache");
  envEquals("XDG_DATA_HOME", root / "data");

  perf::CleanupIsolatedAppRoot(root, false);
  Expect(!std::filesystem::exists(root), "cleanup must remove sandbox root");
}

void TestEstablishIsolatedAppRootKeepArtifactsPreservesRoot() {
  ScopedEnvVar xdg_config("XDG_CONFIG_HOME", "");
  ScopedEnvVar xdg_state("XDG_STATE_HOME", "");
  ScopedEnvVar xdg_cache("XDG_CACHE_HOME", "");
  ScopedEnvVar xdg_data("XDG_DATA_HOME", "");

  const std::filesystem::path root = perf::EstablishIsolatedAppRoot(true);
  Expect(!root.empty(), "kept-artifacts sandbox must be created");
  perf::CleanupIsolatedAppRoot(root, true);
  Expect(std::filesystem::is_directory(root),
         "CleanupIsolatedAppRoot must not remove kept sandbox");
  // Hand-cleanup so the test does not leave files behind.
  std::error_code ec;
  std::filesystem::remove_all(root, ec);
}

void TestColdStartupIgnoresRealUserWorkspaceSession() {
  // Simulate a developer machine that has a real workspace-session file under
  // ~/.local/state/microide. The harness must not see it after isolation.
  TemporaryDirectory fake_home;
  const std::filesystem::path real_state =
      fake_home.path() / ".local" / "state" / "microide";
  std::filesystem::create_directories(real_state);
  std::ofstream session(real_state / "workspace-session");
  session << "# planted by TestColdStartupIgnoresRealUserWorkspaceSession\n";
  session.close();
  Expect(std::filesystem::exists(real_state / "workspace-session"),
         "planted workspace-session must exist");

  ScopedEnvVar home_var("HOME", fake_home.path().string());
  // Force XDG_*_HOME unset (empty value is treated as unset by AppDirectories).
  ScopedEnvVar xdg_config("XDG_CONFIG_HOME", "");
  ScopedEnvVar xdg_state("XDG_STATE_HOME", "");
  ScopedEnvVar xdg_cache("XDG_CACHE_HOME", "");
  ScopedEnvVar xdg_data("XDG_DATA_HOME", "");
  ScopedHostPlatformOverride linux_platform(platform::HostPlatform::Linux);

  // Sanity: without isolation, AppDirectories points at the planted state dir.
  const std::filesystem::path baseline_state =
      platform::ResolveAppDirectory(platform::UserDirectoryKind::State, "microide");
  Expect(baseline_state == real_state,
         "precondition: AppDirectories should resolve to planted state dir, got " +
             baseline_state.string());

  std::string error;
  const std::filesystem::path sandbox =
      perf::EstablishIsolatedAppRoot(false, &error);
  Expect(!sandbox.empty(), "isolation must succeed: " + error);

  const std::filesystem::path isolated_state =
      platform::ResolveAppDirectory(platform::UserDirectoryKind::State, "microide");
  Expect(isolated_state == sandbox / "state" / "microide",
         "harness state path must live inside sandbox, got " +
             isolated_state.string());
  Expect(!std::filesystem::exists(isolated_state / "workspace-session"),
         "sandboxed state must not contain the planted workspace-session");

  // Real planted file must still exist (the harness did not touch it).
  Expect(std::filesystem::exists(real_state / "workspace-session"),
         "harness must not delete the user's real state file");

  perf::CleanupIsolatedAppRoot(sandbox, false);
}

}  // namespace

void RegisterPerfHarnessIsolationTests(std::vector<TestCase>& tests) {
  AddTest(tests, "PerfHarnessIsolation/ExportsAllXdgVars",
          TestEstablishIsolatedAppRootExportsAllXdgVars);
  AddTest(tests, "PerfHarnessIsolation/KeepArtifactsPreservesRoot",
          TestEstablishIsolatedAppRootKeepArtifactsPreservesRoot);
  AddTest(tests, "PerfHarnessIsolation/ColdStartupIgnoresRealUserSession",
          TestColdStartupIgnoresRealUserWorkspaceSession);
}

}  // namespace microide::tests
