#include "TestSupport.h"

#include "util/PerformanceCounters.h"
#include "workspace/registries/WorkspaceSettingsRegistry.h"
#include "workspace/shell/WorkspaceShellTestAccess.h"

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

namespace microide::tests {
namespace {

using microide::workspace::FindBuiltinSettingSpec;
using microide::workspace::SettingSpec;
using microide::workspace::SettingType;
using microide::workspace::WorkspaceShell;
using WorkspaceShellTestAccess = microide::workspace::WorkspaceShell::TestAccess;

// Scope the app config/state homes so persistence is test-local on every
// platform (XDG on Linux, *APPDATA on Windows).

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

// A built-in write must STORE the parsed + range-clamped value, not the raw input,
// so the stored/displayed/persisted value matches the value the editor applies.
// Previously `set-setting editor.font_size 999` stored "999" while the editor
// rendered at the clamped 32, so the overlay showed 999 and its stepper wrapped.
void TestSetSettingStoresClampedBuiltinValue() {
  TemporaryDirectory temp;
  ScopedAppHomes homes(temp.path() / "state", temp.path() / "config");

  WorkspaceShell shell;
  shell.Initialize({});

  const SettingSpec* spec = FindBuiltinSettingSpec("editor.font_size");
  Expect(spec != nullptr && spec->type == SettingType::Int, "font_size must be an int spec");

  Expect(WorkspaceShellTestAccess::SetSettingValue(shell, "editor.font_size", "999"),
         "an out-of-range int write should still succeed (clamped)");
  Expect(WorkspaceShellTestAccess::GetSettingValue(shell, "editor.font_size") ==
             std::to_string(spec->max_int),
         "the stored value must be clamped to the spec max, not the raw 999");

  Expect(WorkspaceShellTestAccess::SetSettingValue(shell, "editor.font_size", "1"),
         "a below-range int write should succeed (clamped)");
  Expect(WorkspaceShellTestAccess::GetSettingValue(shell, "editor.font_size") ==
             std::to_string(spec->min_int),
         "the stored value must be clamped to the spec min, not the raw 1");
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

// Regression: headless `--control` force-starts the channel independent of the
// `control.enabled` setting. A live settings change — e.g. a cold-start spec's
// `set-setting debug.enabled true` — runs ApplyLiveSettings -> MaybeStartControlChannel,
// which must NOT tear the force-started socket down. Before the fix, this stranded
// the headless driver: the advertised socket vanished mid-run.
void TestForceStartedControlChannelSurvivesSettingChange() {
  TemporaryDirectory temp;
  ScopedAppHomes homes(temp.path() / "state", temp.path() / "config");
  // Isolate the runtime dir so the socket path is test-local.
  std::filesystem::create_directories(temp.path() / "run");
  ScopedEnvVar xdg_runtime("XDG_RUNTIME_DIR", (temp.path() / "run").string());

  WorkspaceShell shell;
  microide::workspace::WorkspaceStartupOptions options;
  options.control_stdout = true;  // mirror `--control`
  shell.SetStartupOptions(std::move(options));
  shell.Initialize({});
  shell.ForceStartControlChannel();
  Expect(WorkspaceShellTestAccess::IsControlChannelRunning(shell),
         "--control force-starts the control channel socket");

  // control.enabled is OFF (only the --control flag is set). A live setting change
  // must leave the force-started channel running.
  Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, "set-setting debug.enabled true"),
         "the live setting change applies");
  Expect(WorkspaceShellTestAccess::IsControlChannelRunning(shell),
         "the force-started control channel survives a live settings change");
}

}  // namespace

// Opening a project applies `project.files_exclude` and
// `project.follow_out_of_root_symlinks` itself. `ApplyLiveSettings` then compares
// the resolved settings against the last-applied memos to decide whether a live
// EDIT happened — and those memos started empty, so for any user who has either
// setting set, the FIRST prepared frame read the RESTORED CONFIGURATION as an
// edit: a whole-tree index rescan, a directory-tree refresh, and a full re-arm of
// the native watcher (a second tree walk plus one inotify_add_watch per
// directory), on every launch. Measured on this repo before the fix, with the
// setting present in the user config: two `watch::NativeSetupWalk` calls
// totalling 214 ms and a 49 ms project scan, none of which changed anything.
//
// The setting has to arrive the way a launch delivers it — persisted, then
// restored by a fresh shell's Initialize. Writing it through SetSettingValue on
// a live shell cannot exhibit this: that path ends in ApplyLiveSettings, which
// seeds the memo before the project is even open.
void TestRestoredExcludeGlobsDoNotRescanOnFirstLiveSettingsPass() {
  TemporaryDirectory temp;
  ScopedAppHomes homes(temp.path() / "state", temp.path() / "config");
  const std::filesystem::path root = temp.path() / "project";
  WriteFile(root / "README.md", "root\n");
  WriteFile(root / "src" / "keep.cpp", "int keep() { return 1; }\n");
  WriteFile(root / "vendor" / "skip.cpp", "int skip() { return 2; }\n");

  {
    WorkspaceShell writer;
    writer.Initialize({});
    Expect(WorkspaceShellTestAccess::SetSettingValue(writer, "project.files_exclude", "vendor/"),
           "the exclude-glob setting should accept a value");
    WorkspaceShellTestAccess::SaveUserConfig(writer);
  }

  WorkspaceShell shell;
  Expect(shell.Initialize(root), "a fresh shell should restore the config and open the project");
  Expect(WorkspaceShellTestAccess::GetSettingValue(shell, "project.files_exclude") == "vendor/",
         "the restored config should carry the exclude glob");

  util::ResetPerformanceCounters();
  WorkspaceShellTestAccess::ApplyLiveSettings(shell);
  Expect(util::ReadPerformanceCounter(util::PerfCounterId::FileIndexRefreshRequests) == 0,
         "the first live-settings pass must not rescan for exclude globs the open already applied");
  Expect(util::ReadPerformanceCounter(util::PerfCounterId::FileIndexWatcherStarts) == 0,
         "the first live-settings pass must not re-arm the file index watcher");

  // Not vacuous: an actual edit still does both.
  Expect(WorkspaceShellTestAccess::SetSettingValueTransient(shell, "project.files_exclude",
                                                            "vendor/\nout/"),
         "the exclude-glob setting should accept an edited value");
  Expect(util::ReadPerformanceCounter(util::PerfCounterId::FileIndexRefreshRequests) >= 1,
         "an actual exclude-glob edit should still request a rescan");
  Expect(util::ReadPerformanceCounter(util::PerfCounterId::FileIndexWatcherStarts) >= 1,
         "an actual exclude-glob edit should still re-arm the watcher");
}

// A control-channel reply attributed feedback to the command by diffing the panel
// feedback text against a pre-dispatch snapshot, so a rejection repeating the
// previous message (`project-next` twice with one project open) read as a stale
// line and came back as a bare "command failed".
void TestControlCommandRepeatsTheSameRejectionMessage() {
  TemporaryDirectory temp;
  ScopedAppHomes homes(temp.path() / "state", temp.path() / "config");

  WorkspaceShell shell;
  shell.Initialize({});

  const auto first = WorkspaceShellTestAccess::ExecuteControlCommand(shell, "project-next");
  Expect(!first.ok, "project-next with no project open should be rejected");
  Expect(first.error == "No active project",
         "the first rejection should carry its message: " + first.error);
  const auto second = WorkspaceShellTestAccess::ExecuteControlCommand(shell, "project-prev");
  Expect(!second.ok, "project-prev with no project open should be rejected");
  Expect(second.error == first.error,
         "a rejection repeating the previous message must not read as 'command failed': " +
             second.error);

  // A success after a rejection must not inherit the rejection's message.
  const auto ok = WorkspaceShellTestAccess::ExecuteControlCommand(shell, "set-setting control.enabled true");
  Expect(ok.ok && ok.feedback.empty(), "a silent success reports no feedback: " + ok.feedback);
}

void RegisterWorkspaceShellControlSettingsTests(std::vector<TestCase>& tests) {
  AddTest(tests, "WorkspaceShellControlSettings/ControlCommandRepeatsTheSameRejectionMessage",
          TestControlCommandRepeatsTheSameRejectionMessage);
  AddTest(tests, "WorkspaceShellControlSettings/ForceStartedControlChannelSurvivesSettingChange",
          TestForceStartedControlChannelSurvivesSettingChange);
  AddTest(tests, "WorkspaceShellControlSettings/SetSettingCommandFlipsAndRejects",
          TestSetSettingCommandFlipsAndRejects);
  AddTest(tests, "WorkspaceShellControlSettings/SetSettingStoresClampedBuiltinValue",
          TestSetSettingStoresClampedBuiltinValue);
  AddTest(tests, "WorkspaceShellControlSettings/TransientSettingNotPersisted",
          TestTransientSettingNotPersisted);
  AddTest(tests, "WorkspaceShellControlSettings/StartupOverridesAreTransient",
          TestStartupOverridesAreTransient);
  AddTest(tests, "WorkspaceShellControlSettings/ExplicitProjectWinsOverRestore",
          TestExplicitProjectWinsOverRestore);
  AddTest(tests, "WorkspaceShellControlSettings/RestoredExcludeGlobsDoNotRescanOnFirstLiveSettingsPass",
          TestRestoredExcludeGlobsDoNotRescanOnFirstLiveSettingsPass);
}

}  // namespace microide::tests
