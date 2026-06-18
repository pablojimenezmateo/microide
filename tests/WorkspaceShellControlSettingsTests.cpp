#include "TestSupport.h"

#include "workspace/WorkspaceShellTestAccess.h"

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

namespace microide::tests {
namespace {

using microide::workspace::WorkspaceShell;
using WorkspaceShellTestAccess = microide::workspace::WorkspaceShell::TestAccess;

// Scope the app config/state homes so persistence is test-local on every
// platform (XDG on Linux, *APPDATA on Windows).
class ScopedAppHomes {
 public:
  ScopedAppHomes(const std::filesystem::path& state_home,
                 const std::filesystem::path& config_home)
      : xdg_state_home_("XDG_STATE_HOME", state_home.string()),
        xdg_config_home_("XDG_CONFIG_HOME", config_home.string()),
        localappdata_("LOCALAPPDATA", state_home.string()),
        appdata_("APPDATA", config_home.string()) {}

 private:
  ScopedEnvVar xdg_state_home_;
  ScopedEnvVar xdg_config_home_;
  ScopedEnvVar localappdata_;
  ScopedEnvVar appdata_;
};

bool RootsContain(const std::vector<std::filesystem::path>& roots,
                  const std::filesystem::path& target) {
  const std::filesystem::path normalized = target.lexically_normal();
  return std::any_of(roots.begin(), roots.end(), [&](const std::filesystem::path& root) {
    return root.lexically_normal() == normalized;
  });
}

// The `set-setting` command flips a value through the chokepoint; an unknown id
// is rejected.
void TestSetSettingCommandFlipsAndRejects() {
  TemporaryDirectory temp;
  ScopedAppHomes homes(temp.path() / "state", temp.path() / "config");

  WorkspaceShell shell;
  shell.Initialize({});

  Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, "set-setting control.enabled true"),
         "set-setting should succeed for a known setting");
  Expect(WorkspaceShellTestAccess::GetSettingValue(shell, "control.enabled") == "true",
         "set-setting should flip the value through the chokepoint");

  Expect(!WorkspaceShellTestAccess::ExecuteCommandLine(shell, "set-setting not.a.real.setting x"),
         "set-setting should reject an unknown id");
  Expect(!WorkspaceShellTestAccess::ExecuteCommandLine(shell, "set-setting"),
         "set-setting with no arguments should reject");
}

// A transient (`--set` / spec) override applies live but is stripped before the
// user config is serialized, so it never clobbers the saved config; a persisting
// write of a sibling setting survives the round-trip.
void TestTransientSettingNotPersisted() {
  TemporaryDirectory temp;
  ScopedAppHomes homes(temp.path() / "state", temp.path() / "config");

  WorkspaceShell shell;
  shell.Initialize({});

  Expect(WorkspaceShellTestAccess::SetSettingValue(shell, "control.enabled", "true"),
         "durable set should succeed");
  Expect(WorkspaceShellTestAccess::SetSettingValueTransient(shell, "debug.enabled", "true"),
         "transient set should succeed");

  Expect(WorkspaceShellTestAccess::IsSettingTransient(shell, "debug.enabled"),
         "transiently-set key should be marked transient");
  Expect(!WorkspaceShellTestAccess::IsSettingTransient(shell, "control.enabled"),
         "durably-set key should not be marked transient");
  Expect(WorkspaceShellTestAccess::GetSettingValue(shell, "debug.enabled") == "true",
         "transient value should be visible live");

  // Simulate a shutdown save while the transient key is still live, then reload.
  WorkspaceShellTestAccess::SaveUserConfig(shell);
  WorkspaceShellTestAccess::RestoreUserConfig(shell);

  Expect(WorkspaceShellTestAccess::GetSettingValue(shell, "control.enabled") == "true",
         "durable setting should survive the persist/restore round-trip");
  Expect(WorkspaceShellTestAccess::GetSettingValue(shell, "debug.enabled") != "true",
         "transient setting must not persist to the saved config");
}

// `--set` overrides applied at startup land transiently.
void TestStartupOverridesAreTransient() {
  TemporaryDirectory temp;
  ScopedAppHomes homes(temp.path() / "state", temp.path() / "config");

  WorkspaceShell shell;
  microide::workspace::WorkspaceStartupOptions options;
  options.setting_overrides = {{"control.enabled", "true"}};
  shell.SetStartupOptions(std::move(options));
  shell.Initialize({});

  WorkspaceShellTestAccess::ApplyStartupSettingOverrides(shell);
  Expect(WorkspaceShellTestAccess::GetSettingValue(shell, "control.enabled") == "true",
         "startup override should apply live");
  Expect(WorkspaceShellTestAccess::IsSettingTransient(shell, "control.enabled"),
         "startup override should be transient");
}

// An explicit project wins over a saved workspace session; a bare launch still
// restores the saved session.
void TestExplicitProjectWinsOverRestore() {
  TemporaryDirectory temp;
  const std::filesystem::path project_a = temp.path() / "projA";
  const std::filesystem::path project_b = temp.path() / "projB";
  WriteFile(project_a / "a.txt", "alpha\n");
  WriteFile(project_b / "b.txt", "beta\n");

  const std::filesystem::path home = temp.path() / "home";
  std::filesystem::create_directories(home);
  ScopedEnvVar scoped_home("HOME", home.string());
  ScopedAppHomes homes(temp.path() / "state", temp.path() / "config");

  // Open project A and persist it as the saved session (Shutdown runs the full
  // persistence path the restore reads back).
  {
    WorkspaceShell shell;
    Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, project_a, false, false),
           "project A should open");
    shell.Shutdown();
  }

  // Control: a bare launch (no explicit project) restores the saved session
  // (project A). Checked before opening project B, since opening a project
  // rewrites the saved session.
  {
    WorkspaceShell shell;
    shell.Initialize({});
    const auto roots = WorkspaceShellTestAccess::ProjectRoots(shell);
    Expect(RootsContain(roots, project_a), "bare launch should restore the saved session");
  }

  // The new behavior: a launch naming project B must open B, not restore A.
  {
    WorkspaceShell shell;
    microide::workspace::WorkspaceStartupOptions options;
    options.project_path = project_b;
    shell.SetStartupOptions(std::move(options));
    shell.Initialize(project_b);
    const auto roots = WorkspaceShellTestAccess::ProjectRoots(shell);
    Expect(RootsContain(roots, project_b), "explicit project B should be opened");
    Expect(!RootsContain(roots, project_a),
           "explicit project should win over the saved session");
  }
}

}  // namespace

void RegisterWorkspaceShellControlSettingsTests(std::vector<TestCase>& tests) {
  AddTest(tests, "WorkspaceShellControlSettings/SetSettingCommandFlipsAndRejects",
          TestSetSettingCommandFlipsAndRejects);
  AddTest(tests, "WorkspaceShellControlSettings/TransientSettingNotPersisted",
          TestTransientSettingNotPersisted);
  AddTest(tests, "WorkspaceShellControlSettings/StartupOverridesAreTransient",
          TestStartupOverridesAreTransient);
  AddTest(tests, "WorkspaceShellControlSettings/ExplicitProjectWinsOverRestore",
          TestExplicitProjectWinsOverRestore);
}

}  // namespace microide::tests
