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

void TestAppDirectoriesUseMacOSConventions() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path home = temp_dir.path() / "home";

  ScopedHostPlatformOverride scoped_platform(microide::platform::HostPlatform::MacOS);
  ScopedEnvVar scoped_home("HOME", home.string());

  Expect(ResolveUserDirectory(UserDirectoryKind::Config) ==
             home / "Library" / "Application Support",
         "macOS config roots should resolve to Application Support");
  Expect(ResolveUserDirectory(UserDirectoryKind::State) ==
             home / "Library" / "Application Support",
         "macOS state roots should resolve to Application Support");
  Expect(ResolveUserDirectory(UserDirectoryKind::Data) ==
             home / "Library" / "Application Support",
         "macOS data roots should resolve to Application Support");
  Expect(ResolveUserDirectory(UserDirectoryKind::Cache) ==
             home / "Library" / "Caches",
         "macOS cache roots should resolve to Library/Caches");
  Expect(ResolveAppDirectory(UserDirectoryKind::Config, "microide") ==
             home / "Library" / "Application Support" / "microide",
         "macOS app config should live under Application Support");
  Expect(ResolveAppDirectory(UserDirectoryKind::Cache, "microide") ==
             home / "Library" / "Caches" / "microide",
         "macOS app cache should live under Library/Caches");
}

void TestAppDirectoriesUseWindowsConventions() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path roaming = temp_dir.path() / "AppData" / "Roaming";
  const std::filesystem::path local = temp_dir.path() / "AppData" / "Local";

  ScopedHostPlatformOverride scoped_platform(microide::platform::HostPlatform::Windows);
  ScopedEnvVar scoped_appdata("APPDATA", roaming.string());
  ScopedEnvVar scoped_localappdata("LOCALAPPDATA", local.string());

  Expect(ResolveUserDirectory(UserDirectoryKind::Config) == roaming,
         "Windows config roots should resolve to APPDATA");
  Expect(ResolveUserDirectory(UserDirectoryKind::State) == local,
         "Windows state roots should resolve to LOCALAPPDATA");
  Expect(ResolveUserDirectory(UserDirectoryKind::Data) == local,
         "Windows data roots should resolve to LOCALAPPDATA");
  Expect(ResolveUserDirectory(UserDirectoryKind::Cache) == local,
         "Windows cache roots should resolve to LOCALAPPDATA");
  Expect(ResolveAppDirectory(UserDirectoryKind::Config, "microide") == roaming / "microide",
         "Windows app config should live under APPDATA/app");
  Expect(ResolveAppDirectory(UserDirectoryKind::State, "microide") ==
             local / "microide" / "State",
         "Windows app state should live under LOCALAPPDATA/app/State");
  Expect(ResolveAppDirectory(UserDirectoryKind::Data, "microide") ==
             local / "microide" / "Data",
         "Windows app data should live under LOCALAPPDATA/app/Data");
  Expect(ResolveAppDirectory(UserDirectoryKind::Cache, "microide") ==
             local / "microide" / "Cache",
         "Windows app cache should live under LOCALAPPDATA/app/Cache");
}

}  // namespace

void RegisterAppDirectoriesTests(std::vector<TestCase>& tests) {
  AddTest(tests, "AppDirectories/PreferExplicitEnvironmentRoots",
          TestAppDirectoriesPreferExplicitEnvironmentRoots);
  AddTest(tests, "AppDirectories/FallBackToHomeRoots",
          TestAppDirectoriesFallBackToHomeRoots);
  AddTest(tests, "AppDirectories/UseMacOSConventions",
          TestAppDirectoriesUseMacOSConventions);
  AddTest(tests, "AppDirectories/UseWindowsConventions",
          TestAppDirectoriesUseWindowsConventions);
}

}  // namespace microide::tests
