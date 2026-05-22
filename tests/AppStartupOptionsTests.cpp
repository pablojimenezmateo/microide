#include "TestSupport.h"

#include "app/AppStartupOptions.h"
#include "workspace/WorkspaceShellTestAccess.h"

#include <vector>

namespace microide::tests {
namespace {

using microide::app::ParseAppStartupOptions;
using microide::workspace::WorkspaceShell;
using WorkspaceShellTestAccess = microide::workspace::WorkspaceShell::TestAccess;

class ScopedPluginConfigHomeEnv {
 public:
  explicit ScopedPluginConfigHomeEnv(const std::filesystem::path& config_home)
      : xdg_config_home_("XDG_CONFIG_HOME", config_home.string()),
        appdata_("APPDATA", config_home.string()) {}

 private:
  ScopedEnvVar xdg_config_home_;
  ScopedEnvVar appdata_;
};

void WritePluginInit(const std::filesystem::path& root,
                     std::string_view directory_name,
                     std::string_view content) {
  WriteFile(root / directory_name / "init.lua", std::string(content));
}

void WritePluginSyntax(const std::filesystem::path& root,
                       std::string_view directory_name,
                       std::string_view file_name,
                       std::string_view content) {
  WriteFile(root / directory_name / "syntax" / file_name, std::string(content));
}

std::vector<char*> ArgvFromStrings(std::vector<std::string>& storage) {
  std::vector<char*> argv;
  argv.reserve(storage.size());
  for (std::string& arg : storage) {
    argv.push_back(arg.data());
  }
  return argv;
}

void TestParseDisablePluginsFlag() {
  std::vector<std::string> args = {"microide", "--disable-plugins"};
  auto argv = ArgvFromStrings(args);
  const auto parsed = ParseAppStartupOptions(static_cast<int>(argv.size()), argv.data());
  Expect(!parsed.show_usage, "help should not be requested");
  Expect(parsed.options.disable_plugins, "--disable-plugins should be set");
  Expect(!parsed.options.safe_mode, "safe mode should remain off");
}

void TestParseSafeModeImpliesDisablePlugins() {
  std::vector<std::string> args = {"microide", "--safe-mode", "/tmp/project"};
  auto argv = ArgvFromStrings(args);
  const auto parsed = ParseAppStartupOptions(static_cast<int>(argv.size()), argv.data());
  Expect(parsed.options.safe_mode, "safe mode should be set");
  Expect(parsed.options.plugins_disabled(), "safe mode should disable plugins");
  Expect(parsed.options.project_path.has_value() &&
             parsed.options.project_path->generic_string() == "/tmp/project",
         "positional project path should be captured");
}

void TestDisablePluginsSkipsUserPluginsAndSyntax() {
#if !MICROIDE_HAS_LUA_PLUGINS
  return;
#endif
  TemporaryDirectory temp_dir;
  const std::filesystem::path config_home = temp_dir.path() / "config";
  const std::filesystem::path plugins_home = config_home / "microide" / "plugins";
  WritePluginInit(plugins_home, "trust-test-plugin",
                  "function init(api)\n"
                  "  api.register_command({name='trust-test-command'})\n"
                  "end\n");
  WritePluginSyntax(plugins_home, "trust-test-plugin", "trust-test.yaml",
                    "filetype: trusttest\nrules:\n  - pattern: TRUSTTEST\n    name: keyword\n");

  ScopedPluginConfigHomeEnv env(config_home);
  WorkspaceShell shell;
  microide::workspace::WorkspaceStartupOptions options;
  options.disable_plugins = true;
  shell.SetStartupOptions(std::move(options));
  shell.Initialize({});

  Expect(!WorkspaceShellTestAccess::PluginHostEnabled(shell),
         "plugin host should be disabled");
  Expect(WorkspaceShellTestAccess::ReloadPluginsInvocationCount(shell) >= 1,
         "startup should run a plugin reload pass");
  Expect(WorkspaceShellTestAccess::LoadedPluginCount(shell) == 0,
         "user-scope plugins should not load when disabled");
  const std::string summary = WorkspaceShellTestAccess::PluginRuntimeReloadSummary(shell);
  Expect(summary.find("0 syntax definition") != std::string::npos,
         "plugin syntax should not be loaded when plugins are disabled");
}

void TestSafeModeSurfacesStartupState() {
#if !MICROIDE_HAS_LUA_PLUGINS
  return;
#endif
  TemporaryDirectory temp_dir;
  const std::filesystem::path config_home = temp_dir.path() / "config";
  WritePluginInit(config_home / "microide" / "plugins", "safe-mode-plugin",
                  "function init(api)\n"
                  "  api.register_command({name='safe-mode-command'})\n"
                  "end\n");
  ScopedPluginConfigHomeEnv env(config_home);

  WorkspaceShell shell;
  microide::workspace::WorkspaceStartupOptions options;
  options.safe_mode = true;
  shell.SetStartupOptions(std::move(options));
  shell.Initialize({});

  Expect(WorkspaceShellTestAccess::StartupOptions(shell).safe_mode,
         "safe mode flag should remain set");
  Expect(!WorkspaceShellTestAccess::PluginHostEnabled(shell),
         "safe mode should disable plugins");
  Expect(WorkspaceShellTestAccess::ProjectRoots(shell).empty(),
         "safe mode without a project path should not restore workspace session projects");

  WorkspaceShellTestAccess::RefreshStatusBar(shell);
  Expect(WorkspaceShellTestAccess::StatusBarSegmentVisible(shell,
                                                           microide::workspace::StatusBarSegmentId::Branch),
         "status bar branch segment should be visible for startup mode");
  Expect(WorkspaceShellTestAccess::StatusBarSegmentText(
             shell, microide::workspace::StatusBarSegmentId::Branch) == "Safe mode",
         "status bar should show safe mode state");

  WorkspaceShellTestAccess::OpenHelpAboutOverlay(shell);
  const auto rows = WorkspaceShellTestAccess::HelpAboutRows(shell);
  const bool has_startup_row = std::any_of(
      rows.begin(), rows.end(), [](const microide::workspace::HelpAboutRow& row) {
        return row.label == "Startup mode" && row.detail.find("Safe mode") != std::string::npos;
      });
  Expect(has_startup_row, "Help/About should document safe mode");
}

}  // namespace

void RegisterAppStartupOptionsTests(std::vector<TestCase>& tests) {
  AddTest(tests, "AppStartupOptions/ParseDisablePlugins", TestParseDisablePluginsFlag);
  AddTest(tests, "AppStartupOptions/ParseSafeMode", TestParseSafeModeImpliesDisablePlugins);
  AddTest(tests, "AppStartupOptions/DisablePluginsSkipsPluginsAndSyntax",
          TestDisablePluginsSkipsUserPluginsAndSyntax);
  AddTest(tests, "AppStartupOptions/SafeModeSurfacesStartupState",
          TestSafeModeSurfacesStartupState);
}

}  // namespace microide::tests
