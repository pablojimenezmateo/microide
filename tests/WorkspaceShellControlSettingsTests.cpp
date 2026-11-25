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

bool RootsContain(const std::vector<std::filesystem::path>& roots,
                  const std::filesystem::path& target) {
  const std::filesystem::path normalized = target.lexically_normal();
  return std::any_of(roots.begin(), roots.end(), [&](const std::filesystem::path& root) {
    return root.lexically_normal() == normalized;
  });
}

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

void TestSettingsStepperStopsAtTheBounds() {
  TemporaryDirectory temp;
  ScopedAppHomes homes(temp.path() / "state", temp.path() / "config");

  WorkspaceShell shell;
  shell.Initialize({});

  const SettingSpec* tab = FindBuiltinSettingSpec("editor.tab_size");
  Expect(tab != nullptr && tab->type == SettingType::Int, "tab_size must be an int spec");
  WorkspaceShellTestAccess::SetSettingValue(shell, "editor.tab_size", std::to_string(tab->max_int));
  WorkspaceShellTestAccess::StepSetting(shell, "editor.tab_size", true);
  Expect(WorkspaceShellTestAccess::GetSettingValue(shell, "editor.tab_size") == std::to_string(tab->max_int),
         "stepping up from the maximum stays at the maximum");
  WorkspaceShellTestAccess::SetSettingValue(shell, "editor.tab_size", std::to_string(tab->min_int));
  WorkspaceShellTestAccess::StepSetting(shell, "editor.tab_size", false);
  Expect(WorkspaceShellTestAccess::GetSettingValue(shell, "editor.tab_size") == std::to_string(tab->min_int),
         "stepping down from the minimum stays at the minimum");

  const SettingSpec* scale = FindBuiltinSettingSpec("ui.scale");
  Expect(scale != nullptr && scale->type == SettingType::Float, "ui.scale must be a float spec");
  WorkspaceShellTestAccess::SetSettingValue(shell, "ui.scale", microide::workspace::SerializeSettingValue(scale->max_float));
  WorkspaceShellTestAccess::StepSetting(shell, "ui.scale", true);
  Expect(WorkspaceShellTestAccess::GetSettingValue(shell, "ui.scale") == microide::workspace::SerializeSettingValue(scale->max_float),
         "stepping the UI scale up from its maximum stays there");

  const SettingSpec* autosave = FindBuiltinSettingSpec("editor.autosave");
  Expect(autosave != nullptr && autosave->type == SettingType::Enum, "autosave must be an enum spec");
  WorkspaceShellTestAccess::SetSettingValue(shell, "editor.autosave", std::string(autosave->enum_values.back().value));
  WorkspaceShellTestAccess::StepSetting(shell, "editor.autosave", true);
  Expect(WorkspaceShellTestAccess::GetSettingValue(shell, "editor.autosave") == std::string(autosave->enum_values.front().value),
         "an enum keeps cycling past its last option");
}

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

  WorkspaceShellTestAccess::SaveUserConfig(shell);
  WorkspaceShellTestAccess::RestoreUserConfig(shell);

  Expect(WorkspaceShellTestAccess::GetSettingValue(shell, "control.enabled") == "true",
         "durable setting should survive the persist/restore round-trip");
  Expect(WorkspaceShellTestAccess::GetSettingValue(shell, "debug.enabled") != "true",
         "transient setting must not persist to the saved config");
}

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

  {
    WorkspaceShell shell;
    Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, project_a, false, false),
           "project A should open");
    shell.Shutdown();
  }

  {
    WorkspaceShell shell;
    shell.Initialize({});
    const auto roots = WorkspaceShellTestAccess::ProjectRoots(shell);
    Expect(RootsContain(roots, project_a), "bare launch should restore the saved session");
  }

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

void TestForceStartedControlChannelSurvivesSettingChange() {
  TemporaryDirectory temp;
  ScopedAppHomes homes(temp.path() / "state", temp.path() / "config");
  std::filesystem::create_directories(temp.path() / "run");
  ScopedEnvVar xdg_runtime("XDG_RUNTIME_DIR", (temp.path() / "run").string());

  WorkspaceShell shell;
  microide::workspace::WorkspaceStartupOptions options;
  options.control_stdout = true;
  shell.SetStartupOptions(std::move(options));
  shell.Initialize({});
  shell.ForceStartControlChannel();
  Expect(WorkspaceShellTestAccess::IsControlChannelRunning(shell),
         "--control force-starts the control channel socket");

  Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, "set-setting debug.enabled true"),
         "the live setting change applies");
  Expect(WorkspaceShellTestAccess::IsControlChannelRunning(shell),
         "the force-started control channel survives a live settings change");
}

}

void TestRestoredSettingsAreCanonicalized() {
  TemporaryDirectory temp;
  ScopedAppHomes homes(temp.path() / "state", temp.path() / "config");
  {
    WorkspaceShell writer;
    writer.Initialize({});
    WorkspaceShellTestAccess::SetRawUserSetting(writer, "debug.enabled", "no");
    WorkspaceShellTestAccess::SetRawUserSetting(writer, "editor.fold.enabled", "On");
    WorkspaceShellTestAccess::SetRawUserSetting(writer, "editor.font_size", "999");
    WorkspaceShellTestAccess::SetRawUserSetting(writer, "lsp.enabled", "maybe");
    WorkspaceShellTestAccess::SetRawUserSetting(writer, "someplugin.flag", "no");
    WorkspaceShellTestAccess::SaveUserConfig(writer);
  }
  WorkspaceShell shell;
  shell.Initialize({});
  const auto value = [&](std::string_view id) {
    return WorkspaceShellTestAccess::GetSettingValue(shell, id).value_or("<unset>");
  };
  Expect(value("debug.enabled") == "false", "a hand-edited \"no\" loads as false: " + value("debug.enabled"));
  Expect(value("editor.fold.enabled") == "true", "\"On\" loads as true: " + value("editor.fold.enabled"));
  Expect(value("editor.font_size") != "999", "an int loads clamped to its range: " + value("editor.font_size"));
  Expect(value("lsp.enabled") == "maybe", "a value that does not parse is kept as written");
  Expect(value("someplugin.flag") == "no", "a setting no built-in spec names is kept as written");
}

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

  Expect(WorkspaceShellTestAccess::SetSettingValueTransient(shell, "project.files_exclude",
                                                            "vendor/\nout/"),
         "the exclude-glob setting should accept an edited value");
  Expect(util::ReadPerformanceCounter(util::PerfCounterId::FileIndexRefreshRequests) >= 1,
         "an actual exclude-glob edit should still request a rescan");
  Expect(util::ReadPerformanceCounter(util::PerfCounterId::FileIndexWatcherStarts) >= 1,
         "an actual exclude-glob edit should still re-arm the watcher");
}

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
  AddTest(tests, "WorkspaceShellControlSettings/SettingsStepperStopsAtTheBounds",
          TestSettingsStepperStopsAtTheBounds);
  AddTest(tests, "WorkspaceShellControlSettings/TransientSettingNotPersisted",
          TestTransientSettingNotPersisted);
  AddTest(tests, "WorkspaceShellControlSettings/StartupOverridesAreTransient",
          TestStartupOverridesAreTransient);
  AddTest(tests, "WorkspaceShellControlSettings/RestoredSettingsAreCanonicalized",
          TestRestoredSettingsAreCanonicalized);
  AddTest(tests, "WorkspaceShellControlSettings/ExplicitProjectWinsOverRestore",
          TestExplicitProjectWinsOverRestore);
  AddTest(tests, "WorkspaceShellControlSettings/RestoredExcludeGlobsDoNotRescanOnFirstLiveSettingsPass",
          TestRestoredExcludeGlobsDoNotRescanOnFirstLiveSettingsPass);
}

}
