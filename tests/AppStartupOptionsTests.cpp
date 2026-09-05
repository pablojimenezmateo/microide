#include "TestSupport.h"

#include "app/AppStartupOptions.h"
#include "workspace/shell/WorkspaceShellTestAccess.h"

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
  Expect(parsed.options.disable_plugins, "safe mode should imply disable-plugins");
  Expect(parsed.options.plugins_disabled(), "safe mode should disable plugins");
  Expect(parsed.options.project_path.has_value() &&
             parsed.options.project_path->generic_string() == "/tmp/project",
         "positional project path should be captured");
}

void TestParseControlFlag() {
  std::vector<std::string> args = {"microide", "--control"};
  auto argv = ArgvFromStrings(args);
  const auto parsed = ParseAppStartupOptions(static_cast<int>(argv.size()), argv.data());
  Expect(parsed.exit_code == 0, "--control should parse cleanly");
  Expect(parsed.options.control_stdout, "--control should set control_stdout");
}

void TestParseSetOverridesAreRepeatable() {
  std::vector<std::string> args = {"microide", "--set",     "control.enabled", "true",
                                   "--set",    "debug.enabled", "true",        "/tmp/project"};
  auto argv = ArgvFromStrings(args);
  const auto parsed = ParseAppStartupOptions(static_cast<int>(argv.size()), argv.data());
  Expect(parsed.exit_code == 0, "repeated --set should parse cleanly");
  Expect(parsed.options.setting_overrides.size() == 2, "two overrides expected");
  Expect(parsed.options.setting_overrides[0] ==
             std::pair<std::string, std::string>("control.enabled", "true"),
         "first override should be control.enabled=true");
  Expect(parsed.options.setting_overrides[1] ==
             std::pair<std::string, std::string>("debug.enabled", "true"),
         "second override should be debug.enabled=true");
  Expect(parsed.options.project_path.has_value() &&
             parsed.options.project_path->generic_string() == "/tmp/project",
         "positional path after --set pairs should still be captured");
}

void TestParseDapLogDoesNotSwallowProjectPath() {
  // Regression: `--dap-log /repo` used to consume /repo as the log path and open
  // no project. The bare flag now defaults its path and the token is the project.
  std::vector<std::string> args = {"microide", "--dap-log", "/tmp/project"};
  auto argv = ArgvFromStrings(args);
  const auto parsed = ParseAppStartupOptions(static_cast<int>(argv.size()), argv.data());
  Expect(parsed.exit_code == 0, "--dap-log should parse cleanly");
  Expect(parsed.options.dap_log_path.has_value() &&
             parsed.options.dap_log_path->generic_string() == "/tmp/microide-dap.log",
         "bare --dap-log uses the default sink");
  Expect(parsed.options.project_path.has_value() &&
             parsed.options.project_path->generic_string() == "/tmp/project",
         "the following token is the project path, not the log path");
}

void TestParseDapLogAttachedPath() {
  std::vector<std::string> args = {"microide", "--dap-log=/tmp/trace.log", "/tmp/project"};
  auto argv = ArgvFromStrings(args);
  const auto parsed = ParseAppStartupOptions(static_cast<int>(argv.size()), argv.data());
  Expect(parsed.exit_code == 0, "--dap-log=<path> should parse cleanly");
  Expect(parsed.options.dap_log_path.has_value() &&
             parsed.options.dap_log_path->generic_string() == "/tmp/trace.log",
         "attached --dap-log=<path> sets the custom sink");
  Expect(parsed.options.project_path.has_value() &&
             parsed.options.project_path->generic_string() == "/tmp/project",
         "the project path is still captured alongside --dap-log=<path>");
  std::vector<std::string> empty_args = {"microide", "--dap-log="};
  auto empty_argv = ArgvFromStrings(empty_args);
  const auto empty_parsed =
      ParseAppStartupOptions(static_cast<int>(empty_argv.size()), empty_argv.data());
  Expect(empty_parsed.exit_code == 2, "--dap-log= with an empty path exits 2");
}

void TestParseRejectsSecondPositionalPath() {
  std::vector<std::string> args = {"microide", "/tmp/repo-a", "/tmp/repo-b"};
  auto argv = ArgvFromStrings(args);
  const auto parsed = ParseAppStartupOptions(static_cast<int>(argv.size()), argv.data());
  Expect(parsed.exit_code == 2, "a second positional project path should exit 2");
}

void TestParseDapLogBeforeFlagUsesDefault() {
  std::vector<std::string> args = {"microide", "--dap-log", "--disable-plugins"};
  auto argv = ArgvFromStrings(args);
  const auto parsed = ParseAppStartupOptions(static_cast<int>(argv.size()), argv.data());
  Expect(parsed.exit_code == 0, "--dap-log followed by a flag should parse cleanly");
  Expect(parsed.options.dap_log_path.has_value() &&
             parsed.options.dap_log_path->generic_string() == "/tmp/microide-dap.log",
         "--dap-log should default its path when the next token is a flag");
  Expect(parsed.options.disable_plugins, "the following flag should still be parsed");
}

void TestParseSetMissingTokensFails() {
  std::vector<std::string> args = {"microide", "--set", "only-id"};
  auto argv = ArgvFromStrings(args);
  const auto parsed = ParseAppStartupOptions(static_cast<int>(argv.size()), argv.data());
  Expect(parsed.exit_code == 2, "--set with a missing value token should exit 2");
}

void TestParseVersionFlag() {
  for (const char* flag : {"--version", "-V"}) {
    std::vector<std::string> args = {"microide", flag};
    auto argv = ArgvFromStrings(args);
    const auto parsed = ParseAppStartupOptions(static_cast<int>(argv.size()), argv.data());
    Expect(parsed.show_version, "version flag should request the version print");
    Expect(parsed.exit_code == 0, "version flag should parse cleanly");
    Expect(!parsed.show_usage, "version flag should not request usage");
    Expect(!parsed.options.project_path.has_value(),
           "version flag should not be treated as a project path");
  }
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

// `microide notes.txt`: a startup path that names a regular file opens its
// directory as the project and the file as the first tab, as `code notes.txt`
// does. It used to be handed to the tree and the index as a project root, which
// refused it, and the launch failed with "Workspace initialization failed".
void TestFilePathAtStartupOpensItsDirectoryAndTheFile() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path config_home = temp_dir.path() / "config";
  ScopedPluginConfigHomeEnv env(config_home);
  const std::filesystem::path project = temp_dir.path() / "proj";
  const std::filesystem::path file = project / "notes.txt";
  WriteFile(file, "hello\n");

  WorkspaceShell shell;
  microide::workspace::WorkspaceStartupOptions options;
  options.project_path = file;
  shell.SetStartupOptions(std::move(options));
  Expect(shell.Initialize(file), "a file path must not fail initialization");

  const auto roots = WorkspaceShellTestAccess::ProjectRoots(shell);
  Expect(roots.size() == 1 && roots.front() == project.lexically_normal(),
         "the file's directory is the project root");
  const auto& editor = WorkspaceShellTestAccess::ActiveEditor(shell);
  Expect(editor.path().lexically_normal() == file.lexically_normal(),
         "the file itself is open in the active tab: " + editor.path().string());
  Expect(editor.line_count() >= 1 && editor.lines()[0] == "hello", "with its content loaded");
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
  AddTest(tests, "AppStartupOptions/ParseControlFlag", TestParseControlFlag);
  AddTest(tests, "AppStartupOptions/ParseSetOverridesAreRepeatable",
          TestParseSetOverridesAreRepeatable);
  AddTest(tests, "AppStartupOptions/ParseSetMissingTokensFails", TestParseSetMissingTokensFails);
  AddTest(tests, "AppStartupOptions/ParseVersionFlag", TestParseVersionFlag);
  AddTest(tests, "AppStartupOptions/ParseDapLogDoesNotSwallowProjectPath",
          TestParseDapLogDoesNotSwallowProjectPath);
  AddTest(tests, "AppStartupOptions/ParseDapLogAttachedPath", TestParseDapLogAttachedPath);
  AddTest(tests, "AppStartupOptions/ParseRejectsSecondPositionalPath",
          TestParseRejectsSecondPositionalPath);
  AddTest(tests, "AppStartupOptions/ParseDapLogBeforeFlagUsesDefault",
          TestParseDapLogBeforeFlagUsesDefault);
  AddTest(tests, "AppStartupOptions/DisablePluginsSkipsPluginsAndSyntax",
          TestDisablePluginsSkipsUserPluginsAndSyntax);
  AddTest(tests, "AppStartupOptions/FilePathAtStartupOpensItsDirectoryAndTheFile",
          TestFilePathAtStartupOpensItsDirectoryAndTheFile);
  AddTest(tests, "AppStartupOptions/SafeModeSurfacesStartupState",
          TestSafeModeSurfacesStartupState);
}

}  // namespace microide::tests
