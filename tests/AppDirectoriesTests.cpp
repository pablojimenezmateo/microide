#include "TestSupport.h"

#include "platform/AppDirectories.h"

#include <filesystem>

namespace microide::tests {
namespace {

using microide::platform::ResolveAppDirectory;
using microide::platform::ResolveUserDirectory;
using microide::platform::ResolveUserHomeDirectory;
using microide::platform::UserDirectoryKind;

void TestAppDirectoriesPreferExplicitEnvironmentRoots() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path home = temp_dir.path() / "home";
  const std::filesystem::path config = temp_dir.path() / "config";
  const std::filesystem::path state = temp_dir.path() / "state";
  const std::filesystem::path data = temp_dir.path() / "data";
  const std::filesystem::path cache = temp_dir.path() / "cache";

  ScopedEnvVar scoped_home("HOME", home.string());
  ScopedEnvVar scoped_config("XDG_CONFIG_HOME", config.string());
  ScopedEnvVar scoped_state("XDG_STATE_HOME", state.string());
  ScopedEnvVar scoped_data("XDG_DATA_HOME", data.string());
  ScopedEnvVar scoped_cache("XDG_CACHE_HOME", cache.string());

  Expect(ResolveUserHomeDirectory() == home,
         "home directory resolution should preserve the HOME override");
  Expect(ResolveUserDirectory(UserDirectoryKind::Config) == config,
         "config directory resolution should prefer XDG_CONFIG_HOME");
  Expect(ResolveUserDirectory(UserDirectoryKind::State) == state,
         "state directory resolution should prefer XDG_STATE_HOME");
  Expect(ResolveUserDirectory(UserDirectoryKind::Data) == data,
         "data directory resolution should prefer XDG_DATA_HOME");
  Expect(ResolveUserDirectory(UserDirectoryKind::Cache) == cache,
         "cache directory resolution should prefer XDG_CACHE_HOME");
  Expect(ResolveAppDirectory(UserDirectoryKind::Config, "microide") == config / "microide",
         "app config directory should append the app name to the resolved root");
}

void TestAppDirectoriesFallBackToHomeRoots() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path home = temp_dir.path() / "home";

  ScopedEnvVar scoped_home("HOME", home.string());
  ScopedEnvVar scoped_config("XDG_CONFIG_HOME", "");
  ScopedEnvVar scoped_state("XDG_STATE_HOME", "");
  ScopedEnvVar scoped_data("XDG_DATA_HOME", "");
  ScopedEnvVar scoped_cache("XDG_CACHE_HOME", "");

  Expect(ResolveUserDirectory(UserDirectoryKind::Config) == home / ".config",
         "config directory resolution should fall back to the HOME-based path");
  Expect(ResolveUserDirectory(UserDirectoryKind::State) == home / ".local" / "state",
         "state directory resolution should fall back to the HOME-based path");
  Expect(ResolveUserDirectory(UserDirectoryKind::Data) == home / ".local" / "share",
         "data directory resolution should fall back to the HOME-based path");
  Expect(ResolveUserDirectory(UserDirectoryKind::Cache) == home / ".cache",
         "cache directory resolution should fall back to the HOME-based path");
}

}  // namespace

void RegisterAppDirectoriesTests(std::vector<TestCase>& tests) {
  AddTest(tests, "AppDirectories/PreferExplicitEnvironmentRoots",
          TestAppDirectoriesPreferExplicitEnvironmentRoots);
  AddTest(tests, "AppDirectories/FallBackToHomeRoots",
          TestAppDirectoriesFallBackToHomeRoots);
}

}  // namespace microide::tests
