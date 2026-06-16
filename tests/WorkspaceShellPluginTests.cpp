#include "TestSupport.h"

#include "editor/RuntimeSyntaxRegistry.h"
#include "util/StringUtil.h"
#include "workspace/WorkspaceLspClient.h"
#include "workspace/WorkspaceLspManager.h"
#include "workspace/WorkspaceShellTestAccess.h"

#include <memory>

#include <chrono>
#include <cctype>
#include <filesystem>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include "WorkspaceShellEventHelpers.h"

namespace microide::tests {
namespace {

using microide::workspace::WorkspaceShell;
using WorkspaceShellTestAccess = microide::workspace::WorkspaceShell::TestAccess;

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

std::filesystem::path RepoPluginsRoot() {
  return TestRoot().parent_path() / "plugins";
}

void CopyRepoPlugin(const std::filesystem::path& root, std::string_view directory_name) {
  CopyTree(RepoPluginsRoot() / directory_name, root / directory_name);
}

class ScopedPluginConfigHomeEnv {
 public:
  explicit ScopedPluginConfigHomeEnv(const std::filesystem::path& config_home)
      : xdg_config_home_("XDG_CONFIG_HOME", config_home.string()),
        appdata_("APPDATA", config_home.string()) {}

 private:
  ScopedEnvVar xdg_config_home_;
  ScopedEnvVar appdata_;
};

#if defined(_WIN32)
void WriteWindowsCmdWrapper(const std::filesystem::path& root_script_path,
                            std::string_view powershell_file_name) {
  WriteFile(root_script_path,
            "@echo off\r\n"
            "powershell -NoProfile -ExecutionPolicy Bypass -File \"%~dp0" +
                std::string(powershell_file_name) + "\" %*\r\n");
}
#endif

void WriteFakeEslint(const std::filesystem::path& project_root) {
  const std::filesystem::path eslint_path = project_root / "node_modules" / ".bin" / "eslint";
  WriteFile(
      eslint_path,
      R"(#!/bin/sh
output_file=""
prev=""
for arg in "$@"; do
  if [ "$prev" = "--output-file" ]; then
    output_file="$arg"
  fi
  prev="$arg"
done
eval "file=\${$#}"
report='[{"messages":[]}]'
if grep -q "broken" "$file"; then
  report='[{"messages":[{"ruleId":"no-broken","severity":2,"message":"Unexpected broken token","line":1,"column":1,"endLine":1,"endColumn":7}]}]'
fi
if [ -n "$output_file" ]; then
  printf '%s\n' "$report" > "$output_file"
else
  printf '%s\n' "$report"
fi
if grep -q "broken" "$file"; then
  exit 1
fi
exit 0
)");
  std::filesystem::permissions(
      eslint_path,
      std::filesystem::perms::owner_read | std::filesystem::perms::owner_write |
      std::filesystem::perms::owner_exec,
      std::filesystem::perm_options::replace);
#if defined(_WIN32)
  WriteWindowsCmdWrapper(eslint_path.string() + ".cmd", "eslint.ps1");
  WriteFile(
      eslint_path.string() + ".ps1",
      R"PS1(
$output_file = ""
$previous = ""
foreach ($arg in $args) {
  if ($previous -eq "--output-file") {
    $output_file = $arg
  }
  $previous = $arg
}
$file = $args[-1]
$content = Get-Content -LiteralPath $file -Raw
$report = '[{"messages":[]}]'
if ($content -match "broken") {
  $report = '[{"messages":[{"ruleId":"no-broken","severity":2,"message":"Unexpected broken token","line":1,"column":1,"endLine":1,"endColumn":7}]}]'
}
if ($output_file -ne "") {
  try {
    [System.IO.File]::WriteAllText($output_file, $report + "`n")
  } catch {
    [Console]::Out.WriteLine($report)
  }
} else {
  [Console]::Out.WriteLine($report)
}
if ($content -match "broken") {
  exit 1
}
exit 0
)PS1");
#endif
}

void WriteFakeEslintNoExplicitAny(const std::filesystem::path& project_root) {
  const std::filesystem::path eslint_path = project_root / "node_modules" / ".bin" / "eslint";
  WriteFile(
      eslint_path,
      R"ESLINT(#!/bin/sh
output_file=""
prev=""
for arg in "$@"; do
  if [ "$prev" = "--output-file" ]; then
    output_file="$arg"
  fi
  prev="$arg"
done
eval "file=\${$#}"
report='[{"messages":[]}]'
if [ "$(basename "$file")" = "profile-manager.ts" ]; then
  report='[{"messages":[{"ruleId":"@typescript-eslint/no-explicit-any","severity":2,"message":"Unexpected any. Specify a different type.","line":10,"column":33,"endLine":10,"endColumn":36},{"ruleId":"@typescript-eslint/no-explicit-any","severity":2,"message":"Unexpected any. Specify a different type.","line":32,"column":48,"endLine":32,"endColumn":51},{"ruleId":"@typescript-eslint/no-explicit-any","severity":2,"message":"Unexpected any. Specify a different type.","line":33,"column":36,"endLine":33,"endColumn":39}]}]'
fi
if [ -n "$output_file" ]; then
  printf '%s\n' "$report" > "$output_file"
else
  printf '%s\n' "$report"
fi
if [ "$(basename "$file")" = "profile-manager.ts" ]; then
  exit 1
fi
exit 0
)ESLINT");
  std::filesystem::permissions(
      eslint_path,
      std::filesystem::perms::owner_read | std::filesystem::perms::owner_write |
          std::filesystem::perms::owner_exec,
      std::filesystem::perm_options::replace);
#if defined(_WIN32)
  WriteWindowsCmdWrapper(eslint_path.string() + ".cmd", "eslint.ps1");
  WriteFile(
      eslint_path.string() + ".ps1",
      R"PS1(
$output_file = ""
$previous = ""
foreach ($arg in $args) {
  if ($previous -eq "--output-file") {
    $output_file = $arg
  }
  $previous = $arg
}
$file = $args[-1]
$report = '[{"messages":[]}]'
if ([System.IO.Path]::GetFileName($file) -eq "profile-manager.ts") {
  $report = '[{"messages":[{"ruleId":"@typescript-eslint/no-explicit-any","severity":2,"message":"Unexpected any. Specify a different type.","line":10,"column":33,"endLine":10,"endColumn":36},{"ruleId":"@typescript-eslint/no-explicit-any","severity":2,"message":"Unexpected any. Specify a different type.","line":32,"column":48,"endLine":32,"endColumn":51},{"ruleId":"@typescript-eslint/no-explicit-any","severity":2,"message":"Unexpected any. Specify a different type.","line":33,"column":36,"endLine":33,"endColumn":39}]}]'
}
if ($output_file -ne "") {
  try {
    [System.IO.File]::WriteAllText($output_file, $report + "`n")
  } catch {
    [Console]::Out.WriteLine($report)
  }
} else {
  [Console]::Out.WriteLine($report)
}
if ([System.IO.Path]::GetFileName($file) -eq "profile-manager.ts") {
  exit 1
}
exit 0
)PS1");
#endif
}

void WriteFakeTsc(const std::filesystem::path& project_root) {
  const std::filesystem::path tsc_path = project_root / "node_modules" / ".bin" / "tsc";
  WriteFile(
      tsc_path,
      R"(#!/bin/sh
project="$2"
if grep -q "ignoreDeprecations" "$project"; then
  printf '%s\n' "$project(17,5): error TS5103: Invalid value for '--ignoreDeprecations'."
  exit 2
fi
exit 0
)");
  std::filesystem::permissions(
      tsc_path,
      std::filesystem::perms::owner_read | std::filesystem::perms::owner_write |
          std::filesystem::perms::owner_exec,
      std::filesystem::perm_options::replace);
#if defined(_WIN32)
  WriteWindowsCmdWrapper(tsc_path.string() + ".cmd", "tsc.ps1");
  WriteFile(
      tsc_path.string() + ".ps1",
      R"PS1(
$project = $args[1]
$content = Get-Content -LiteralPath $project -Raw
if ($content -match "ignoreDeprecations") {
  [Console]::Out.WriteLine("${project}(17,5): error TS5103: Invalid value for '--ignoreDeprecations'.")
  exit 2
}
exit 0
)PS1");
#endif
}

bool AnyRectIntersects(const std::vector<SDL_FRect>& rects, const SDL_FRect& target) {
  return std::any_of(rects.begin(), rects.end(), [&](const SDL_FRect& rect) {
    return SDL_HasRectIntersectionFloat(&rect, &target);
  });
}

std::string DescribePluginState(const WorkspaceShell& shell) {
  std::ostringstream description;
  const auto& errors = WorkspaceShellTestAccess::PluginErrors(shell);
  const auto& messages = WorkspaceShellTestAccess::PluginMessages(shell);
  description << "plugin errors=" << errors.size() << " messages=" << messages.size();
  if (!errors.empty()) {
    description << " last_error=" << errors.back();
  }
  if (!messages.empty()) {
    description << " last_message=" << messages.back();
  }
  const auto* log_channel = WorkspaceShellTestAccess::OutputChannelEntries(shell, "plugins.log");
  if (log_channel != nullptr && !log_channel->empty()) {
    description << " last_log=" << log_channel->back();
  }
  const auto* error_channel =
      WorkspaceShellTestAccess::OutputChannelEntries(shell, "plugins.error");
  if (error_channel != nullptr && !error_channel->empty()) {
    description << " last_plugin_error=" << error_channel->back();
  }
  return description.str();
}

void TestWorkspaceShellPluginKeybindingsDispatchCommands() {
#if !MICROIDE_HAS_LUA_PLUGINS
  return;
#endif
  TemporaryDirectory temp_dir;
  const std::filesystem::path config_home = temp_dir.path() / "config";
  const std::filesystem::path plugins_root = config_home / "microide" / "plugins";
  const std::filesystem::path project_root = temp_dir.path() / "project";
  const std::filesystem::path source = project_root / "README.md";
  WriteFile(source, "plugin keybinding\n");

  WritePluginInit(
      plugins_root, "keybindings",
      R"(local ide = require("microide")
return ide.plugin({
  id = "keybindings",
  setup = function(ctx)
    ctx.commands.add("keybindings.log", function(ctx, args)
      ctx.log("keybinding-fired")
    end)
    ctx.keybindings.add({
      id = "log",
      action = "keybindings.log",
      key = "Ctrl+Shift+L",
      context = "editor"
    })
  end
})
)");

  ScopedPluginConfigHomeEnv scoped_plugin_config_home(config_home);

  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, project_root, false, false),
         "plugin keybinding fixture should open the project");
  WorkspaceShellTestAccess::OpenFile(shell, source);
  Expect(WorkspaceShellTestAccess::PluginErrors(shell).empty(),
         DescribePluginState(shell).c_str());
  WorkspaceShellTestAccess::ClearPluginMessages(shell);
  Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, "keybindings.log"),
         "plugin keybinding fixture should register the command before shortcut dispatch");
  Expect(!WorkspaceShellTestAccess::PluginMessages(shell).empty() &&
             WorkspaceShellTestAccess::PluginMessages(shell).back() ==
                 "keybindings: keybinding-fired",
         "plugin keybinding fixture should execute the registered command directly");
  WorkspaceShellTestAccess::ClearPluginMessages(shell);

  Expect(SendKeyDown(
             shell, SDLK_L, static_cast<SDL_Keymod>(SDL_KMOD_CTRL | SDL_KMOD_SHIFT)),
         "pressing a contributed editor keybinding should be handled");
  Expect(!WorkspaceShellTestAccess::PluginMessages(shell).empty() &&
             WorkspaceShellTestAccess::PluginMessages(shell).back() ==
                 "keybindings: keybinding-fired",
         "contributed editor keybindings should dispatch plugin commands through the shell");
}

void TestWorkspaceShellPluginStatusItemsRenderAndRedraw() {
#if !MICROIDE_HAS_LUA_PLUGINS
  return;
#endif
  TemporaryDirectory temp_dir;
  const std::filesystem::path config_home = temp_dir.path() / "config";
  const std::filesystem::path plugins_root = config_home / "microide" / "plugins";
  const std::filesystem::path project_root = temp_dir.path() / "project";
  const std::filesystem::path source = project_root / "README.md";
  WriteFile(source, "plugin status\n");

  WritePluginInit(
      plugins_root, "status",
      R"(local ide = require("microide")
return ide.plugin({
  id = "status",
  setup = function(ctx)
    ctx.status.add({
      id = "counter",
      text = "0",
      tooltip = "Counter is 0",
      alignment = "right",
    })
    ctx.commands.add("status.tick", function(ctx, args)
      ctx.status.update("counter", {
        text = "1",
        tooltip = "Counter is 1",
      })
    end)
  end
})
)");

  ScopedPluginConfigHomeEnv scoped_plugin_config_home(config_home);

  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, project_root, false, false),
         "plugin status fixture should open the project");
  WorkspaceShellTestAccess::OpenFile(shell, source);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);

  const auto initial_items = WorkspaceShellTestAccess::VisibleStatusItems(shell);
  const auto initial_counter = std::find_if(
      initial_items.begin(), initial_items.end(), [](const WorkspaceShell::VisibleStatusItem& item) {
        return item.item.plugin_id == "status" && item.item.text == "0";
      });
  Expect(initial_counter != initial_items.end(),
         "status items should render contributed status text in the breadcrumb row");
  if (initial_counter != initial_items.end()) {
    (void)SendMouseMotion(
        shell, initial_counter->rect.x + initial_counter->rect.w * 0.5f,
        initial_counter->rect.y + initial_counter->rect.h * 0.5f, 0);
  }
  Expect(WorkspaceShellTestAccess::HoveredStatusTooltipLabel(shell) == "Counter is 0",
         "hovering a contributed status item should expose its tooltip");

  (void)shell.ConsumePendingRenderInvalidation();
  Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, "status.tick"),
         "status update command should execute");

  const auto updated_items = WorkspaceShellTestAccess::VisibleStatusItems(shell);
  const auto updated_counter = std::find_if(
      updated_items.begin(), updated_items.end(), [](const WorkspaceShell::VisibleStatusItem& item) {
        return item.item.plugin_id == "status" && item.item.text == "1";
      });
  Expect(updated_counter != updated_items.end(),
         "status item updates should be reflected in the live breadcrumb row");
  const auto redraw = shell.ConsumePendingRenderInvalidation();
  Expect(redraw.full || !redraw.rects.empty(),
         "status updates should request a visible redraw");
}

void TestWorkspaceShellSavePipelineRunsParticipantsBeforeFormatter() {
#if !MICROIDE_HAS_LUA_PLUGINS
  return;
#endif
  TemporaryDirectory temp_dir;
  const std::filesystem::path config_home = temp_dir.path() / "config";
  const std::filesystem::path plugins_root = config_home / "microide" / "plugins";
  const std::filesystem::path project_root = temp_dir.path() / "project";
  const std::filesystem::path source = project_root / "README.todo";
  WriteFile(source, "seed\n");
#if defined(_WIN32)
  const std::string uppercase_formatter_command =
      "{ \"powershell\", \"-NoProfile\", \"-Command\", "
      "\"[Console]::Out.Write(([Console]::In.ReadToEnd()).ToUpperInvariant())\" }";
#else
  const std::string uppercase_formatter_command =
      "{ \"sh\", \"-c\", \"tr '[:lower:]' '[:upper:]'\" }";
#endif

  WritePluginInit(
      plugins_root, "save-pipeline",
      "local ide = require(\"microide\")\n"
      "return ide.plugin({\n"
      "  id = \"save-pipeline\",\n"
      "  capabilities = { process = { exec = true } },\n"
      "  setup = function(ctx)\n"
      "    ctx.save_participants.add(\"rename-alpha\", function(buffer)\n"
      "      return {\n"
      "        text = buffer.text:gsub(\"alpha\", \"beta\")\n"
      "      }\n"
      "    end)\n"
      "    ctx.formatters.add({\n"
      "      id = \"todo-uppercase\",\n"
      "      language_id = \"todo\",\n"
      "      label = \"TODO Uppercase\",\n"
      "      command = " +
          uppercase_formatter_command +
          "\n"
          "    })\n"
          "  end\n"
          "})\n");
  WritePluginSyntax(
      plugins_root, "save-pipeline", "todo.lua",
      R"(return {
  filetype = "todo",
  files = { "\\.todo$" },
  rules = {
    { pattern = "\\b[A-Z_]+\\b", group = "keyword" }
  }
}
)");

  ScopedPluginConfigHomeEnv scoped_plugin_config_home(config_home);

  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, project_root, false, false),
         "save pipeline fixture should open the project");
  WorkspaceShellTestAccess::OpenFile(shell, source);
  Expect(WorkspaceShellTestAccess::SaveParticipantCount(shell) == 1,
         "save pipeline fixture should register one save participant");
  Expect(WorkspaceShellTestAccess::HasFormatterForLanguage(shell, "todo"),
         "save pipeline fixture should register the contributed todo formatter");
  {
    std::string transformed = "alpha\n";
    std::string runtime_error;
    Expect(WorkspaceShellTestAccess::RunSaveParticipantsForTesting(shell, source, &transformed,
                                                                   &runtime_error),
           "save pipeline fixture should execute save participant runtimes");
    Expect(runtime_error.empty(),
           "save pipeline fixture should not report save participant runtime errors");
    Expect(transformed == "beta\n",
           "save pipeline fixture should apply the save participant text transform directly");
  }

  auto& editor = WorkspaceShellTestAccess::ActiveEditor(shell);
  editor.SelectAll();
  editor.InsertText("alpha\n");
  Expect(editor::runtime_syntax::DetectFiletype(source, editor.lines()) == "todo",
         "save pipeline fixture should register the contributed todo syntax before save");
  {
    std::string prepare_error;
    Expect(WorkspaceShellTestAccess::PrepareEditorViewportForSaveForTesting(
               shell, source, editor, &prepare_error),
           "save pipeline fixture should prepare the active editor viewport for save");
    if (!prepare_error.empty()) {
      throw std::runtime_error(
          "save pipeline fixture should not report save-preparation errors (" +
          prepare_error + ")");
    }
    const std::string prepared_text = util::SerializeLines(editor.lines(), editor.line_ending());
    if (prepared_text != "BETA\n") {
      throw std::runtime_error(
          "save pipeline fixture should apply save participants and formatter during save preparation "
          "(actual: " +
          prepared_text + ")");
    }
    editor.SetDirty(true);
  }
  Expect(WorkspaceShellTestAccess::SaveTab(shell, WorkspaceShellTestAccess::ActiveTabIndex(shell)),
         "saving an editor buffer should run the host save pipeline");
  const std::string saved_text = ReadFile(source);
  if (saved_text != "BETA\n") {
    throw std::runtime_error(
        "save participants should run before formatters and persist the transformed text "
        "(actual: " +
        saved_text + ")");
  }
}

void TestWorkspaceShellSavePipelineFormatterFailureLeavesBufferUnchanged() {
#if !MICROIDE_HAS_LUA_PLUGINS
  return;
#endif
  TemporaryDirectory temp_dir;
  const std::filesystem::path config_home = temp_dir.path() / "config";
  const std::filesystem::path plugins_root = config_home / "microide" / "plugins";
  const std::filesystem::path project_root = temp_dir.path() / "project";
  const std::filesystem::path source = project_root / "README.todo";
  WriteFile(source, "alpha\n");
#if defined(_WIN32)
  const std::string failing_formatter_command =
      "{ \"powershell\", \"-NoProfile\", \"-Command\", "
      "\"[void][Console]::In.ReadToEnd(); [Console]::Error.Write('formatter-broke'); exit 3\" }";
#else
  const std::string failing_formatter_command =
      "{ \"sh\", \"-c\", \"cat >/dev/null; echo formatter-broke >&2; exit 3\" }";
#endif

  WritePluginInit(
      plugins_root, "save-pipeline-fail",
      "local ide = require(\"microide\")\n"
      "return ide.plugin({\n"
      "  id = \"save-pipeline-fail\",\n"
      "  capabilities = { process = { exec = true } },\n"
      "  setup = function(ctx)\n"
      "    ctx.formatters.add({\n"
      "      id = \"todo-fail\",\n"
      "      language_id = \"todo\",\n"
      "      label = \"TODO Fail\",\n"
      "      command = " +
          failing_formatter_command +
          "\n"
          "    })\n"
          "  end\n"
          "})\n");
  WritePluginSyntax(
      plugins_root, "save-pipeline-fail", "todo.lua",
      R"(return {
  filetype = "todo",
  files = { "\\.todo$" },
  rules = {
    { pattern = "\\b[A-Z_]+\\b", group = "keyword" }
  }
}
)");

  ScopedPluginConfigHomeEnv scoped_plugin_config_home(config_home);

  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, project_root, false, false),
         "formatter failure fixture should open the project");
  WorkspaceShellTestAccess::OpenFile(shell, source);

  auto& editor = WorkspaceShellTestAccess::ActiveEditor(shell);
  editor.SelectAll();
  editor.InsertText("beta\n");
  Expect(WorkspaceShellTestAccess::SaveTab(shell, WorkspaceShellTestAccess::ActiveTabIndex(shell)),
         "save should proceed when formatter exits non-zero");
  Expect(ReadFile(source) == "beta\n",
         "formatter failure should persist the unformatted edited buffer");
  Expect(!editor.dirty(),
         "formatter failure should still clear dirty state when save succeeds");
}

void TestWorkspaceShellSavePipelineOverlappingSavesCoalesceCorrectly() {
#if !MICROIDE_HAS_LUA_PLUGINS
  return;
#endif
  TemporaryDirectory temp_dir;
  const std::filesystem::path config_home = temp_dir.path() / "config";
  const std::filesystem::path plugins_root = config_home / "microide" / "plugins";
  const std::filesystem::path project_root = temp_dir.path() / "project";
  const std::filesystem::path source = project_root / "README.todo";
  WriteFile(source, "alpha\n");
#if defined(_WIN32)
  const std::string delayed_formatter_command =
      "{ \"powershell\", \"-NoProfile\", \"-Command\", "
      "\"Start-Sleep -Milliseconds 200; [Console]::Out.Write(([Console]::In.ReadToEnd()).ToUpperInvariant())\" }";
#else
  const std::string delayed_formatter_command =
      "{ \"sh\", \"-c\", \"sleep 0.2; tr '[:lower:]' '[:upper:]'\" }";
#endif

  WritePluginInit(
      plugins_root, "save-pipeline-overlap",
      "local ide = require(\"microide\")\n"
      "return ide.plugin({\n"
      "  id = \"save-pipeline-overlap\",\n"
      "  capabilities = { process = { exec = true } },\n"
      "  setup = function(ctx)\n"
      "    ctx.formatters.add({\n"
      "      id = \"todo-slow-uppercase\",\n"
      "      language_id = \"todo\",\n"
      "      label = \"TODO Slow Uppercase\",\n"
      "      command = " +
          delayed_formatter_command +
          "\n"
          "    })\n"
          "  end\n"
          "})\n");
  WritePluginSyntax(
      plugins_root, "save-pipeline-overlap", "todo.lua",
      R"(return {
  filetype = "todo",
  files = { "\\.todo$" },
  rules = {
    { pattern = "\\b[A-Z_]+\\b", group = "keyword" }
  }
}
)");

  ScopedPluginConfigHomeEnv scoped_plugin_config_home(config_home);

  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, project_root, false, false),
         "overlapping save fixture should open the project");
  WorkspaceShellTestAccess::OpenFile(shell, source);

  auto& editor = WorkspaceShellTestAccess::ActiveEditor(shell);
  editor.SelectAll();
  editor.InsertText("beta\n");
  const std::size_t tab_index = WorkspaceShellTestAccess::ActiveTabIndex(shell);

  bool first_save_ok = false;
  std::thread first_save([&]() {
    first_save_ok = WorkspaceShellTestAccess::SaveTab(shell, tab_index);
  });
  SDL_Delay(20);
  const bool second_save_ok = WorkspaceShellTestAccess::SaveTab(shell, tab_index);
  first_save.join();

  Expect(first_save_ok && second_save_ok,
         "overlapping saves should both complete successfully");
  Expect(ReadFile(source) == "BETA\n",
         "overlapping saves should preserve formatted persisted text");
}

void TestWorkspaceShellPhase4RegistriesReloadWithPlugins() {
#if !MICROIDE_HAS_LUA_PLUGINS
  return;
#endif
  TemporaryDirectory temp_dir;
  const std::filesystem::path config_home = temp_dir.path() / "config";
  const std::filesystem::path plugins_root = config_home / "microide" / "plugins";
  const std::filesystem::path project_root = temp_dir.path() / "project";
  WriteFile(project_root / "README.md", "phase4 registries\n");

  WritePluginInit(
      plugins_root, "phase4-registries",
      R"(local ide = require("microide")
return ide.plugin({
  id = "phase4-registries",
  setup = function(ctx)
    ctx.scm.add("sample", "Sample SCM")
    ctx.annotations.add({
      id = "blame",
      label = "Plugin Blame",
      type = "blame",
      language_id = "markdown"
    })
    ctx.auth.add("github", "GitHub")
  end
})
)");

  ScopedPluginConfigHomeEnv scoped_plugin_config_home(config_home);

  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, project_root, false, false),
         "phase4 registry fixture should open the project");
  Expect(WorkspaceShellTestAccess::ScmProviders(shell).size() == 1 &&
             WorkspaceShellTestAccess::ScmProviders(shell).front().id ==
                 "phase4-registries.sample",
         "opening a project should rebuild the shell SCM registry from plugin contributions");
  Expect(WorkspaceShellTestAccess::AnnotationProviders(shell).size() == 1 &&
             WorkspaceShellTestAccess::AnnotationProviders(shell).front().language_id ==
                 "markdown",
         "opening a project should rebuild the annotation registry from plugin contributions");
  const auto& auth_providers = WorkspaceShellTestAccess::ContributedAuthProviders(shell);
  Expect(auth_providers.size() == 1 && auth_providers.front().id == "phase4-registries.github" &&
             auth_providers.front().label == "GitHub" &&
             auth_providers.front().plugin_id == "phase4-registries",
         "opening a project should record plugin-contributed auth providers on the host");
}

void TestWorkspaceShellVirtualDocumentsOpenAndRefresh() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path project_root = temp_dir.path() / "project";
  WriteFile(project_root / "README.md", "virtual document fixture\n");

  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, project_root, false, false),
         "virtual document fixture should open the project");
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::RegisterVirtualDocument(
      shell,
      microide::workspace::VirtualDocumentSpec{
          .uri = "virtual://preview/README.todo",
          .language_id = "todo",
          .content = "alpha",
          .editable = false,
          .plugin_id = "host-test",
      });

  Expect(WorkspaceShellTestAccess::OpenVirtualDocument(shell, "virtual://preview/README.todo"),
         "virtual documents should be openable through the live tab model");
  Expect(!WorkspaceShellTestAccess::ActiveEditor(shell).lines().empty() &&
             WorkspaceShellTestAccess::ActiveEditor(shell).lines().front() == "alpha",
         "opening a virtual document should populate an editor tab with the provided content");
  Expect(!WorkspaceShellTestAccess::HandleTextInput(shell, "beta"),
         "read-only virtual documents should reject direct editor text input");
  Expect(WorkspaceShellTestAccess::ActiveEditor(shell).lines().front() == "alpha",
         "read-only virtual documents should remain unchanged after rejected input");

  std::string clipboard_text;
  WorkspaceShellTestAccess::SetClipboardTextWriter(
      shell, [&](std::string_view text) {
        clipboard_text = std::string(text);
        return true;
      });

  Expect(WorkspaceShellTestAccess::ExecuteSelectAll(shell),
         "read-only virtual documents should still allow select-all through the shared viewport path");
  Expect(WorkspaceShellTestAccess::ExecuteCopySelection(shell),
         "read-only virtual documents should still allow copy through the shared viewport path");
  Expect(clipboard_text == "alpha",
         "copying from a read-only virtual document should write the selected text");

  clipboard_text.clear();
  Expect(SendKeyDown(shell, SDLK_A, SDL_KMOD_CTRL),
         "Ctrl+A should be handled on a read-only virtual document");
  Expect(SendKeyDown(shell, SDLK_C, SDL_KMOD_CTRL),
         "Ctrl+C should be handled on a read-only virtual document");
  Expect(clipboard_text == "alpha",
         "read-only virtual document shortcuts should use the shared navigable viewport path");

  Expect(SendKeyDown(shell, SDLK_BACKSPACE, SDL_KMOD_NONE),
         "edit keys on a read-only virtual document should be consumed without mutating the buffer");
  Expect(WorkspaceShellTestAccess::ActiveEditor(shell).lines().front() == "alpha",
         "edit keys should leave read-only virtual document text unchanged");

  (void)shell.ConsumePendingRenderInvalidation();
  WorkspaceShellTestAccess::UpdateVirtualDocumentContent(shell, "virtual://preview/README.todo",
                                                         "beta");
  Expect(WorkspaceShellTestAccess::ActiveEditor(shell).lines().front() == "beta",
         "updating a virtual document should refresh any open tab backed by that URI");
  const auto redraw = shell.ConsumePendingRenderInvalidation();
  const auto layout = WorkspaceShellTestAccess::CurrentLayout(shell);
  Expect(AnyRectIntersects(redraw.rects, layout.editor_surface),
         "virtual document updates should redraw the editor surface");
}

void TestWorkspaceShellLoadsPluginsAndRunsBufferHooks() {
#if !MICROIDE_HAS_LUA_PLUGINS
  return;
#endif
  TemporaryDirectory temp_dir;
  const std::filesystem::path config_home = temp_dir.path() / "config";
  const std::filesystem::path plugins_root = config_home / "microide" / "plugins";
  const std::filesystem::path project_root = temp_dir.path() / "project";
  const std::filesystem::path readme = project_root / "README.md";
  const std::filesystem::path source = project_root / "src" / "main.txt";
  WriteFile(readme, "workspace plugin fixture\n");
  WriteFile(source, "alpha\n");

  WritePluginInit(
      plugins_root, "events",
      R"(local ide = require("microide")
return ide.plugin({
  id = "events",
  setup = function(ctx)
    ctx.commands.add("events.ping", function(ctx, args)
      ctx.log("command:" .. table.concat(args, " "))
    end)
  end,
  on_project_open = function(ctx, project)
    ctx.log("project-open:" .. project.name)
  end,
  on_buffer_open = function(ctx, buffer)
    ctx.log("buffer-open:" .. buffer.relative_path)
  end,
  on_buffer_save = function(ctx, buffer)
    ctx.log("buffer-save:" .. buffer.relative_path)
  end
})
)");

  ScopedPluginConfigHomeEnv scoped_plugin_config_home(config_home);

  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, project_root, false, false),
         "plugin shell fixture should open the project");
  Expect(WorkspaceShellTestAccess::PluginErrors(shell).empty(),
         "plugin shell fixture should load without plugin errors");
  Expect(WorkspaceShellTestAccess::PluginMessages(shell).size() >= 2,
         "opening a project should trigger plugin project-open and startup buffer-open hooks");
  Expect(WorkspaceShellTestAccess::PluginMessages(shell)[0] == "events: project-open:project",
         "project-open hook should run when the project is activated");
  const auto* plugin_log_channel = WorkspaceShellTestAccess::OutputChannelEntries(shell, "plugins.log");
  Expect(plugin_log_channel != nullptr && plugin_log_channel->size() >= 2,
         "plugin output logging should mirror plugin messages into the host log channel");
  Expect(plugin_log_channel != nullptr && plugin_log_channel->front() == "events: project-open:project",
         "plugin output logging should preserve the recorded plugin message text");

  WorkspaceShellTestAccess::ClearPluginMessages(shell);
  WorkspaceShellTestAccess::OpenFile(shell, source);
  Expect(WorkspaceShellTestAccess::PluginMessages(shell).size() == 1,
         "opening a file should trigger one buffer-open hook");
  Expect(WorkspaceShellTestAccess::PluginMessages(shell).front() == "events: buffer-open:src/main.txt",
         "buffer-open hook should receive project-relative file paths");

  WorkspaceShellTestAccess::ClearPluginMessages(shell);
  WorkspaceShellTestAccess::ActiveEditor(shell).InsertText("beta\n");
  Expect(WorkspaceShellTestAccess::SaveTab(shell, WorkspaceShellTestAccess::ActiveTabIndex(shell)),
         "plugin shell fixture should save the dirty buffer");
  Expect(WorkspaceShellTestAccess::PluginMessages(shell).size() == 1,
         "saving a dirty buffer should trigger one buffer-save hook");
  Expect(WorkspaceShellTestAccess::PluginMessages(shell).front() == "events: buffer-save:src/main.txt",
         "buffer-save hook should receive project-relative file paths");

  WorkspaceShellTestAccess::ClearPluginMessages(shell);
  Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, "events.ping hello plugins"),
         "plugin commands should execute through the shell command prompt");
  Expect(!WorkspaceShellTestAccess::PluginMessages(shell).empty() &&
             WorkspaceShellTestAccess::PluginMessages(shell).back() == "events: command:hello plugins",
         "plugin commands should receive shell-split arguments");
}

void TestWorkspaceShellPluginsReloadCommandRefreshesCommands() {
#if !MICROIDE_HAS_LUA_PLUGINS
  return;
#endif
  TemporaryDirectory temp_dir;
  const std::filesystem::path config_home = temp_dir.path() / "config";
  const std::filesystem::path plugins_root = config_home / "microide" / "plugins";
  const std::filesystem::path project_root = temp_dir.path() / "project";
  WriteFile(project_root / "README.md", "reload fixture\n");

  const std::filesystem::path plugin_root = plugins_root / "reloadable";
  WritePluginInit(
      plugins_root, "reloadable",
      R"(local ide = require("microide")
return ide.plugin({
  id = "reloadable",
  setup = function(ctx)
    ctx.commands.add("reloadable.ping", function(ctx, args)
      ctx.log("before")
    end)
  end
})
)");

  ScopedPluginConfigHomeEnv scoped_plugin_config_home(config_home);

  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, project_root, false, false),
         "reload command fixture should open the project");
  Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, "reloadable.ping"),
         "initial plugin command should execute");
  Expect(!WorkspaceShellTestAccess::PluginMessages(shell).empty() &&
             WorkspaceShellTestAccess::PluginMessages(shell).back() == "reloadable: before",
         "initial plugin command should use the original implementation");

  WriteFile(
      plugin_root / "init.lua",
      R"(local ide = require("microide")
return ide.plugin({
  id = "reloadable",
  setup = function(ctx)
    ctx.commands.add("reloadable.ping", function(ctx, args)
      ctx.log("after")
    end)
  end
})
)");

  WorkspaceShellTestAccess::ClearPluginMessages(shell);
  Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, "plugins-reload"),
         "plugins-reload should succeed when Lua plugin support is enabled");
  Expect(WorkspaceShellTestAccess::CommandPromptStatusText(shell).find("Loaded 1 plugin") !=
             std::string::npos,
         "plugins-reload should report a load summary in the command prompt");

  Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, "reloadable.ping"),
         "reloaded plugin command should execute");
  Expect(!WorkspaceShellTestAccess::PluginMessages(shell).empty() &&
             WorkspaceShellTestAccess::PluginMessages(shell).back() == "reloadable: after",
         "plugins-reload should rebuild the active plugin command table");
}

void TestWorkspaceShellPluginsReloadRefreshesRuntimeSyntaxHighlighting() {
#if !MICROIDE_HAS_LUA_PLUGINS
  return;
#endif
  TemporaryDirectory temp_dir;
  const std::filesystem::path config_home = temp_dir.path() / "config";
  const std::filesystem::path plugins_root = config_home / "microide" / "plugins";
  const std::filesystem::path project_root = temp_dir.path() / "project";
  const std::filesystem::path notes = project_root / "notes.todo";
  WriteFile(project_root / "README.md", "syntax reload fixture\n");
  WriteFile(notes, "TODO item\n");

  WritePluginInit(
      plugins_root, "syntax",
      R"(local ide = require("microide")
return ide.plugin({
  id = "syntax"
})
)");
  WritePluginSyntax(
      plugins_root, "syntax", "todo.lua",
      R"(return {
  filetype = "todo",
  files = { "\\.todo$" },
  rules = {
    { pattern = "\\bTODO\\b", group = "keyword" }
  }
}
)");

  ScopedPluginConfigHomeEnv scoped_plugin_config_home(config_home);

  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, project_root, false, false),
         "syntax reload fixture should open the project");
  WorkspaceShellTestAccess::OpenFile(shell, notes);

  const auto& before_tokens = WorkspaceShellTestAccess::ActiveEditor(shell).HighlightedLineTokens(0);
  Expect(std::any_of(before_tokens.begin(), before_tokens.end(),
                     [](microide::editor::SyntaxTokenKind kind) {
                       return kind == microide::editor::SyntaxTokenKind::Keyword;
                     }),
         "plugin syntax contributions should highlight matching files after project load");

  WritePluginSyntax(
      plugins_root, "syntax", "todo.lua",
      R"(return {
  filetype = "todo",
  files = { "\\.todo$" },
  rules = {
    { pattern = "\\bDONE\\b", group = "keyword" }
  }
}
)");

  Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, "plugins-reload"),
         "plugins-reload should rebuild syntax contributions");

  const auto& after_tokens = WorkspaceShellTestAccess::ActiveEditor(shell).HighlightedLineTokens(0);
  Expect(std::none_of(after_tokens.begin(), after_tokens.end(),
                      [](microide::editor::SyntaxTokenKind kind) {
                        return kind == microide::editor::SyntaxTokenKind::Keyword;
                      }),
         "plugins-reload should invalidate active editor syntax caches when definitions change");
  Expect(WorkspaceShellTestAccess::CommandPromptStatusText(shell).find("1 syntax definition") !=
             std::string::npos,
         "plugins-reload feedback should include loaded plugin syntax definitions");
}

void TestWorkspaceShellPluginsReloadSkipsUnchangedRuntimeSyntaxRebuild() {
#if !MICROIDE_HAS_LUA_PLUGINS
  return;
#endif
  TemporaryDirectory temp_dir;
  const std::filesystem::path config_home = temp_dir.path() / "config";
  const std::filesystem::path plugins_root = config_home / "microide" / "plugins";
  const std::filesystem::path project_root = temp_dir.path() / "project";
  const std::filesystem::path notes = project_root / "notes.todo";
  WriteFile(project_root / "README.md", "syntax fingerprint fixture\n");
  WriteFile(notes, "TODO item\n");

  WritePluginInit(
      plugins_root, "syntax",
      R"(local ide = require("microide")
return ide.plugin({
  id = "syntax"
})
)");
  WritePluginSyntax(
      plugins_root, "syntax", "todo.lua",
      R"(return {
  filetype = "todo",
  files = { "\\.todo$" },
  rules = {
    { pattern = "\\bTODO\\b", group = "keyword" }
  }
}
)");

  ScopedPluginConfigHomeEnv scoped_plugin_config_home(config_home);

  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, project_root, false, false),
         "syntax fingerprint fixture should open the project");
  WorkspaceShellTestAccess::OpenFile(shell, notes);

  const std::size_t revision_before =
      microide::editor::runtime_syntax::RegistryRevision();
  Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, "plugins-reload"),
         "plugins-reload should succeed when syntax definitions are unchanged");
  const std::size_t revision_after =
      microide::editor::runtime_syntax::RegistryRevision();

  Expect(revision_after == revision_before,
         "unchanged plugin syntax files should not rebuild the runtime syntax registry");
  const auto& tokens = WorkspaceShellTestAccess::ActiveEditor(shell).HighlightedLineTokens(0);
  Expect(std::any_of(tokens.begin(), tokens.end(),
                     [](microide::editor::SyntaxTokenKind kind) {
                       return kind == microide::editor::SyntaxTokenKind::Keyword;
                     }),
         "skipping an unchanged syntax rebuild should preserve active syntax highlighting");
}

void TestWorkspaceShellPluginsReloadScopesSyntaxInvalidationByChangedLanguages() {
#if !MICROIDE_HAS_LUA_PLUGINS
  return;
#endif
  TemporaryDirectory temp_dir;
  const std::filesystem::path config_home = temp_dir.path() / "config";
  const std::filesystem::path plugins_root = config_home / "microide" / "plugins";
  const std::filesystem::path project_root = temp_dir.path() / "project";
  const std::filesystem::path todo = project_root / "a.todo";
  const std::filesystem::path note = project_root / "b.note";
  WriteFile(project_root / "README.md", "scoped syntax invalidation fixture\n");
  WriteFile(todo, "TODO item\n");
  WriteFile(note, "NOTE item\n");

  WritePluginInit(
      plugins_root, "syntax",
      R"(local ide = require("microide")
return ide.plugin({
  id = "syntax"
})
)");
  WritePluginSyntax(
      plugins_root, "syntax", "todo.lua",
      R"(return {
  filetype = "todo",
  files = { "\\.todo$" },
  rules = {
    { pattern = "\\bTODO\\b", group = "keyword" }
  }
}
)");
  WritePluginSyntax(
      plugins_root, "syntax", "note.lua",
      R"(return {
  filetype = "note",
  files = { "\\.note$" },
  rules = {
    { pattern = "\\bNOTE\\b", group = "keyword" }
  }
}
)");

  ScopedPluginConfigHomeEnv scoped_plugin_config_home(config_home);

  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, project_root, false, false),
         "scoped syntax invalidation fixture should open the project");

  WorkspaceShellTestAccess::OpenFile(shell, todo);
  auto todo_has_keyword = [&shell]() {
    const auto& tokens = WorkspaceShellTestAccess::ActiveEditor(shell).HighlightedLineTokens(0);
    return std::any_of(tokens.begin(), tokens.end(), [](microide::editor::SyntaxTokenKind kind) {
      return kind == microide::editor::SyntaxTokenKind::Keyword;
    });
  };
  Expect(todo_has_keyword(), "todo file should highlight TODO before any reload");

  WorkspaceShellTestAccess::OpenFile(shell, note);
  auto note_has_keyword = [&shell]() {
    const auto& tokens = WorkspaceShellTestAccess::ActiveEditor(shell).HighlightedLineTokens(0);
    return std::any_of(tokens.begin(), tokens.end(), [](microide::editor::SyntaxTokenKind kind) {
      return kind == microide::editor::SyntaxTokenKind::Keyword;
    });
  };
  Expect(note_has_keyword(), "note file should highlight NOTE before any reload");

  Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, "plugins-reload"),
         "plugins-reload should succeed when syntax definitions are unchanged");
  WorkspaceShellTestAccess::OpenFile(shell, todo);
  Expect(todo_has_keyword(), "empty changed-language set should keep todo highlighting intact");
  WorkspaceShellTestAccess::OpenFile(shell, note);
  Expect(note_has_keyword(), "empty changed-language set should keep note highlighting intact");

  WritePluginSyntax(
      plugins_root, "syntax", "todo.lua",
      R"(return {
  filetype = "todo",
  files = { "\\.todo$" },
  rules = {
    { pattern = "\\bDONE\\b", group = "keyword" }
  }
}
)");
  Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, "plugins-reload"),
         "plugins-reload should succeed after changing one syntax language");
  WorkspaceShellTestAccess::OpenFile(shell, todo);
  Expect(!todo_has_keyword(),
         "single-language change should invalidate and refresh todo syntax highlighting");
  WorkspaceShellTestAccess::OpenFile(shell, note);
  Expect(note_has_keyword(),
         "single-language change should not invalidate unrelated note syntax highlighting");

  WritePluginSyntax(
      plugins_root, "syntax", "note.lua",
      R"(return {
  filetype = "note",
  files = { "\\.note$" },
  rules = {
    { pattern = "\\bDONE\\b", group = "keyword" }
  }
}
)");
  Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, "plugins-reload"),
         "plugins-reload should succeed after changing all syntax languages");
  WorkspaceShellTestAccess::OpenFile(shell, todo);
  Expect(!todo_has_keyword(),
         "all-language change should keep todo in its updated non-keyword state");
  WorkspaceShellTestAccess::OpenFile(shell, note);
  Expect(!note_has_keyword(),
         "all-language change should invalidate and refresh note syntax highlighting");
}

void TestWorkspaceShellPluginWatcherReloadsChangedCommands() {
#if !MICROIDE_HAS_LUA_PLUGINS
  return;
#endif
  TemporaryDirectory temp_dir;
  const std::filesystem::path config_home = temp_dir.path() / "config";
  const std::filesystem::path plugins_root = config_home / "microide" / "plugins";
  const std::filesystem::path project_root = temp_dir.path() / "project";
  WriteFile(project_root / "README.md", "watcher reload fixture\n");

  const std::filesystem::path plugin_root = plugins_root / "reloadable";
  WritePluginInit(
      plugins_root, "reloadable",
      R"(local ide = require("microide")
return ide.plugin({
  id = "reloadable",
  setup = function(ctx)
    ctx.commands.add("reloadable.ping", function(ctx, args)
      ctx.log("before")
    end)
  end
})
)");

  ScopedPluginConfigHomeEnv scoped_plugin_config_home(config_home);

  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, project_root, false, false),
         "plugin watcher fixture should open the project");
  WorkspaceShellTestAccess::SetPluginAssetPollInterval(shell, std::chrono::milliseconds::zero());
  Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, "reloadable.ping"),
         "initial watched plugin command should execute");
  Expect(!WorkspaceShellTestAccess::PluginMessages(shell).empty() &&
             WorkspaceShellTestAccess::PluginMessages(shell).back() == "reloadable: before",
         "initial watched plugin command should use the original implementation");

  WriteFile(
      plugin_root / "init.lua",
      R"(local ide = require("microide")
return ide.plugin({
  id = "reloadable",
  setup = function(ctx)
    ctx.commands.add("reloadable.ping", function(ctx, args)
      ctx.log("after")
    end)
  end
})
)");

  WorkspaceShellTestAccess::ClearPluginMessages(shell);
  const auto scheduled = WorkspaceShellTestAccess::HandleScheduledWake(shell);
  Expect(scheduled.handled && scheduled.redraw.full,
         "scheduled plugin watcher wake should force a redraw after reloading plugins");
  const auto* plugin_log_channel = WorkspaceShellTestAccess::OutputChannelEntries(shell, "plugins.log");
  Expect(plugin_log_channel != nullptr &&
             !plugin_log_channel->empty() &&
             plugin_log_channel->back().find("Detected plugin asset changes: Loaded 1 plugin") !=
                 std::string::npos,
         "plugin watcher reload should append a host-owned log summary");

  Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, "reloadable.ping"),
         "watched plugin command should execute after automatic reload");
  Expect(!WorkspaceShellTestAccess::PluginMessages(shell).empty() &&
             WorkspaceShellTestAccess::PluginMessages(shell).back() == "reloadable: after",
         "plugin watcher should rebuild the active plugin command table");
}

void TestWorkspaceShellPluginWatcherReloadsRuntimeSyntaxHighlighting() {
#if !MICROIDE_HAS_LUA_PLUGINS
  return;
#endif
  TemporaryDirectory temp_dir;
  const std::filesystem::path config_home = temp_dir.path() / "config";
  const std::filesystem::path plugins_root = config_home / "microide" / "plugins";
  const std::filesystem::path project_root = temp_dir.path() / "project";
  const std::filesystem::path notes = project_root / "notes.todo";
  WriteFile(project_root / "README.md", "syntax watcher fixture\n");
  WriteFile(notes, "TODO item\n");

  WritePluginInit(
      plugins_root, "syntax",
      R"(local ide = require("microide")
return ide.plugin({
  id = "syntax"
})
)");
  WritePluginSyntax(
      plugins_root, "syntax", "todo.lua",
      R"(return {
  filetype = "todo",
  files = { "\\.todo$" },
  rules = {
    { pattern = "\\bTODO\\b", group = "keyword" }
  }
}
)");

  ScopedPluginConfigHomeEnv scoped_plugin_config_home(config_home);

  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, project_root, false, false),
         "syntax watcher fixture should open the project");
  WorkspaceShellTestAccess::SetPluginAssetPollInterval(shell, std::chrono::milliseconds::zero());
  WorkspaceShellTestAccess::OpenFile(shell, notes);

  const auto& before_tokens = WorkspaceShellTestAccess::ActiveEditor(shell).HighlightedLineTokens(0);
  Expect(std::any_of(before_tokens.begin(), before_tokens.end(),
                     [](microide::editor::SyntaxTokenKind kind) {
                       return kind == microide::editor::SyntaxTokenKind::Keyword;
                     }),
         "initial watched syntax contribution should highlight the matching token");

  std::this_thread::sleep_for(std::chrono::milliseconds(1100));
  WritePluginSyntax(
      plugins_root, "syntax", "todo.lua",
      R"(return {
  filetype = "todo",
  files = { "\\.todo$" },
  rules = {
    { pattern = "\\bDONE\\b", group = "keyword" }
  }
}
)");

  WorkspaceShell::EventResult scheduled{};
  const auto wake_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
  while (std::chrono::steady_clock::now() < wake_deadline) {
    scheduled = WorkspaceShellTestAccess::HandleScheduledWake(shell);
    if (scheduled.handled && scheduled.redraw.full) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  if (!(scheduled.handled && scheduled.redraw.full) &&
      WorkspaceShellTestAccess::ReloadPluginsIfPluginAssetsChanged(shell, true)) {
    scheduled.handled = true;
    scheduled.redraw.full = true;
    scheduled.redraw.rects.clear();
  }
  Expect(scheduled.handled && scheduled.redraw.full,
         "scheduled syntax watcher wake should force a redraw after reloading syntax");

  const auto& after_tokens = WorkspaceShellTestAccess::ActiveEditor(shell).HighlightedLineTokens(0);
  Expect(std::none_of(after_tokens.begin(), after_tokens.end(),
                      [](microide::editor::SyntaxTokenKind kind) {
                        return kind == microide::editor::SyntaxTokenKind::Keyword;
                      }),
         "plugin watcher reload should invalidate active editor syntax caches when definitions change");
}

void TestWorkspaceShellPluginSidebarOpensItems() {
#if !MICROIDE_HAS_LUA_PLUGINS
  return;
#endif
  TemporaryDirectory temp_dir;
  const std::filesystem::path config_home = temp_dir.path() / "config";
  const std::filesystem::path plugins_root = config_home / "microide" / "plugins";
  const std::filesystem::path project_root = temp_dir.path() / "project";
  const std::filesystem::path readme = project_root / "README.md";
  const std::filesystem::path source = project_root / "src" / "main.txt";
  WriteFile(readme, "root\n");
  WriteFile(source, "alpha\nbeta\n");

  WritePluginInit(
      plugins_root, "plugin-problems",
      R"(local ide = require("microide")
return ide.plugin({
  id = "plugin-problems",
  setup = function(ctx)
    ctx.sidebar.add({
      id = "plugin-problems",
      label = "Problems",
      snapshot = function()
        return {
          { label = "main.txt", detail = "2:2", path = "src/main.txt", line = 2, column = 2 }
        }
      end,
      on_confirm = function(item)
        ctx.log("confirm:" .. item.label)
        ctx.workspace.open_file(item.path, item.line, item.column)
      end
    })
  end
})
)");

  ScopedPluginConfigHomeEnv scoped_plugin_config_home(config_home);

  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, project_root, false, false),
         "plugin sidebar fixture should open the project");
  Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, "sidebar-show plugin-problems"),
         "sidebar-show should accept plugin sidebar ids");
  Expect(WorkspaceShellTestAccess::SidebarMode(shell) == WorkspaceShell::SidebarMode::Plugin,
         "plugin sidebar should activate the plugin sidebar mode");
  Expect(WorkspaceShellTestAccess::SidebarViewId(shell) == "plugin-problems",
         "plugin sidebar should record the active provider id");
  Expect(WorkspaceShellTestAccess::PluginSidebarItems(shell).size() == 1,
         "plugin sidebar should snapshot its items when shown");
  Expect(WorkspaceShellTestAccess::PluginSidebarError(shell).empty(),
         "plugin sidebar should load without runtime errors");

  WorkspaceShellTestAccess::ClearPluginMessages(shell);
  Expect(SendKeyDown(shell, SDLK_RETURN, SDL_KMOD_NONE),
         "pressing Enter in a plugin sidebar should confirm the selected item");
  Expect(!WorkspaceShellTestAccess::PluginMessages(shell).empty() &&
             WorkspaceShellTestAccess::PluginMessages(shell).back() ==
                 "plugin-problems: confirm:main.txt",
         "plugin sidebar confirm should run the plugin callback");
  Expect(WorkspaceShellTestAccess::ActiveEditor(shell).cursor_line() == 1 &&
             WorkspaceShellTestAccess::ActiveEditor(shell).cursor_column() == 1,
         "plugin sidebar confirm should be able to open files at the requested location");
}

void TestWorkspaceShellPluginsReloadFallsBackFromMissingActivePluginSidebar() {
#if !MICROIDE_HAS_LUA_PLUGINS
  return;
#endif
  TemporaryDirectory temp_dir;
  const std::filesystem::path config_home = temp_dir.path() / "config";
  const std::filesystem::path plugins_root = config_home / "microide" / "plugins";
  const std::filesystem::path project_root = temp_dir.path() / "project";
  const std::filesystem::path plugin_root = plugins_root / "reload-sidebar";
  WriteFile(project_root / "README.md", "reload sidebar fixture\n");

  WritePluginInit(
      plugins_root, "reload-sidebar",
      R"(local ide = require("microide")
return ide.plugin({
  id = "reload-sidebar",
  setup = function(ctx)
    ctx.sidebar.add({
      id = "reload-sidebar",
      label = "Reload Sidebar",
      snapshot = function()
        return {
          { label = "README.md", path = "README.md" }
        }
      end
    })
  end
})
)");

  ScopedPluginConfigHomeEnv scoped_plugin_config_home(config_home);

  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, project_root, false, false),
         "sidebar reload fixture should open the project");
  Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, "sidebar-show reload-sidebar"),
         "sidebar reload fixture should show the plugin sidebar");
  Expect(WorkspaceShellTestAccess::SidebarMode(shell) == WorkspaceShell::SidebarMode::Plugin,
         "showing the plugin sidebar should resolve plugin mode from the active view id");
  Expect(WorkspaceShellTestAccess::SidebarViewId(shell) == "reload-sidebar",
         "showing the plugin sidebar should keep the plugin view id");
  Expect(WorkspaceShellTestAccess::PluginSidebarItems(shell).size() == 1,
         "showing the plugin sidebar should snapshot its items");

  WriteFile(
      plugin_root / "init.lua",
      R"(local ide = require("microide")
return ide.plugin({
  id = "reload-sidebar"
})
)");

  Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, "plugins-reload"),
         "plugins-reload should succeed after removing the active sidebar contribution");
  Expect(WorkspaceShellTestAccess::SidebarVisible(shell),
         "removing the active plugin sidebar should keep the sidebar surface visible");
  Expect(WorkspaceShellTestAccess::SidebarMode(shell) == WorkspaceShell::SidebarMode::Tree,
         "removing the active plugin sidebar should fall back to the built-in tree view");
  Expect(WorkspaceShellTestAccess::SidebarViewId(shell) == "tree",
         "removing the active plugin sidebar should reset the active sidebar id to tree");
  Expect(WorkspaceShellTestAccess::PluginSidebarItems(shell).empty(),
         "removing the active plugin sidebar should clear the stale plugin snapshot");
}

void TestWorkspaceShellSidebarModeMenuListsPluginSidebars() {
#if !MICROIDE_HAS_LUA_PLUGINS
  return;
#endif
  TemporaryDirectory temp_dir;
  const std::filesystem::path config_home = temp_dir.path() / "config";
  const std::filesystem::path plugins_root = config_home / "microide" / "plugins";
  const std::filesystem::path project_root = temp_dir.path() / "project";
  WriteFile(project_root / "README.md", "sidebar menu fixture\n");
  WritePluginInit(
      plugins_root, "menu-sidebar",
      R"(local ide = require("microide")
return ide.plugin({
  id = "menu-sidebar",
  setup = function(ctx)
    ctx.sidebar.add({
      id = "menu-sidebar",
      label = "Example Sidebar",
      snapshot = function()
        return {
          { label = "README.md", path = "README.md" }
        }
      end
    })
  end
})
)");

  ScopedPluginConfigHomeEnv scoped_plugin_config_home(config_home);

  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, project_root, false, false),
         "sidebar mode menu fixture should open the project");
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);

  // A plugin-contributed view spills into the mode-row overflow ("⋯") button, which opens the
  // existing sidebar-mode menu.
  const SDL_FRect overflow_rect = WorkspaceShellTestAccess::SidebarModeOverflowRect(shell);
  Expect(overflow_rect.w > 0.0f,
         "a loaded plugin sidebar should produce a mode-row overflow button");
  const float click_x = overflow_rect.x + overflow_rect.w * 0.5f;
  const float click_y = overflow_rect.y + overflow_rect.h * 0.5f;
  Expect(SendMouseDown(shell, click_x, click_y, SDL_BUTTON_LEFT),
         "clicking the mode-row overflow should open the sidebar menu");
  Expect(WorkspaceShellTestAccess::SidebarModeMenuOpen(shell),
         "clicking the mode-row overflow should open the anchored sidebar menu");

  const auto labels = WorkspaceShellTestAccess::SidebarModeMenuLabels(shell);
  Expect(std::find(labels.begin(), labels.end(), "Example Sidebar") != labels.end(),
         "sidebar mode menu should list loaded plugin sidebars by label");
  Expect(std::find(labels.begin(), labels.end(), "Chat") == labels.end(),
         "the sidebar mode menu should omit the chat view");
  Expect(std::find(labels.begin(), labels.end(), "Problems") == labels.end(),
         "the sidebar mode menu should omit the Problems entry");
  Expect(std::find(labels.begin(), labels.end(), "Tests") == labels.end(),
         "the sidebar mode menu should omit the Tests entry");

  const auto example_rect =
      WorkspaceShellTestAccess::SidebarModeMenuItemRect(shell, "Example Sidebar");
  Expect(example_rect.has_value(),
         "sidebar mode menu should expose a clickable menu row for the plugin sidebar");
  Expect(SendMouseDown(
             shell, example_rect->x + example_rect->w * 0.5f,
             example_rect->y + example_rect->h * 0.5f, SDL_BUTTON_LEFT),
         "clicking the plugin sidebar menu row should be handled");
  Expect(WorkspaceShellTestAccess::SidebarMode(shell) == WorkspaceShell::SidebarMode::Plugin,
         "selecting a plugin sidebar from the dropdown should activate plugin sidebar mode");
  Expect(WorkspaceShellTestAccess::SidebarViewId(shell) == "menu-sidebar",
         "selecting a plugin sidebar from the dropdown should target the provider id");
}

void TestWorkspaceShellPluginDiagnosticsPersistAcrossProjectSwitches() {
#if !MICROIDE_HAS_LUA_PLUGINS
  return;
#endif
  TemporaryDirectory temp_dir;
  const std::filesystem::path config_home = temp_dir.path() / "config";
  const std::filesystem::path plugins_root = config_home / "microide" / "plugins";
  const std::filesystem::path project_a = temp_dir.path() / "project-a";
  const std::filesystem::path project_b = temp_dir.path() / "project-b";
  WriteFile(project_a / "README.md", "alpha project\n");
  WriteFile(project_b / "README.md", "beta project\n");

  WritePluginInit(
      plugins_root, "diagnostics",
      R"(local ide = require("microide")
return ide.plugin({
  id = "diagnostics",
  setup = function(ctx)
    ctx.commands.add("diagnostics.publish", function(ctx, args)
      ctx.diagnostics.publish("README.md", {
        {
          message = ctx.workspace.project_root() or "",
          line = 1,
          column = 1,
          end_column = 5,
          severity = "error"
        }
      })
    end)
  end
})
)");

  ScopedPluginConfigHomeEnv scoped_plugin_config_home(config_home);
  const auto normalize_path_text = [](std::string text) {
    std::string normalized =
        std::filesystem::path(text).lexically_normal().generic_string();
#if defined(_WIN32)
    std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
#endif
    return normalized;
  };
  const std::string expected_project_a = normalize_path_text(project_a.string());

  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, project_a, false, false),
         "diagnostics project-switch fixture should open the first project");
  Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, "diagnostics.publish"),
         "diagnostics publish command should execute in the first project");

  const auto* project_a_diagnostics =
      WorkspaceShellTestAccess::DiagnosticsForPath(shell, project_a / "README.md");
  Expect(project_a_diagnostics != nullptr && project_a_diagnostics->size() == 1,
         "publishing diagnostics should store them on the active project");
  Expect(normalize_path_text(project_a_diagnostics->front().message) == expected_project_a,
         "project diagnostics should preserve the publishing project's metadata");

  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, project_b, false, false),
         "diagnostics project-switch fixture should open the second project");
  Expect(WorkspaceShellTestAccess::DiagnosticsForPath(shell, project_a / "README.md") == nullptr,
         "switching projects should not leak the previous project's diagnostics into the new state");
  Expect(WorkspaceShellTestAccess::DiagnosticsForPath(shell, project_b / "README.md") == nullptr,
         "a fresh project should start without diagnostics until a plugin publishes them");

  Expect(WorkspaceShellTestAccess::SwitchProject(shell, 0, false),
         "diagnostics project-switch fixture should switch back to the first project");
  const auto* restored_diagnostics =
      WorkspaceShellTestAccess::DiagnosticsForPath(shell, project_a / "README.md");
  Expect(restored_diagnostics != nullptr && restored_diagnostics->size() == 1,
         "switching back should restore the first project's stored diagnostics");
  Expect(normalize_path_text(restored_diagnostics->front().message) == expected_project_a,
         "restored diagnostics should match the project that originally published them");
}

void TestWorkspaceShellDiagnosticHoverPopupShowsMessages() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path project_root = temp_dir.path() / "project";
  const std::filesystem::path source = project_root / "README.md";
  WriteFile(source, "alpha beta\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, project_root);
  WorkspaceShellTestAccess::OpenSingleEditorTab(shell, source);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  Expect(WorkspaceShellTestAccess::PublishDiagnostics(
             shell, "diagnostics", source,
             {microide::editor::Diagnostic{
                 .range =
                     microide::editor::SelectionRange{
                         .start = microide::editor::TextPosition{.line = 0, .column = 1},
                         .end = microide::editor::TextPosition{.line = 0, .column = 5},
                     },
                 .severity = microide::editor::DiagnosticSeverity::Warning,
                 .message = "Unexpected token near alpha",
             }}),
         "diagnostic hover fixture should publish one visible diagnostic");

  const auto metrics = WorkspaceShellTestAccess::ActiveEditorMetrics(shell);
  const float hover_x =
      metrics.text_x + WorkspaceShellTestAccess::TextCharWidth(shell) * 2.0f;
  const float hover_y = metrics.first_line_y + metrics.line_height - 1.5f;
  Expect(SendMouseMotion(shell, hover_x, hover_y, 0),
         "hovering a diagnostic underline should be handled");

  const auto popup_rect = WorkspaceShellTestAccess::ActiveEditorHoverPopupRect(shell);
  Expect(popup_rect.has_value(), "hovering a diagnostic underline should open a popup");
  Expect(WorkspaceShellTestAccess::ActiveEditorDiagnosticHoverMessage(shell).has_value() &&
             *WorkspaceShellTestAccess::ActiveEditorDiagnosticHoverMessage(shell) ==
                 "Unexpected token near alpha",
         "diagnostic hover popup should expose the published diagnostic message");
}

void TestWorkspaceShellPluginHoverPopupShowsMessages() {
#if !MICROIDE_HAS_LUA_PLUGINS
  return;
#endif
  TemporaryDirectory temp_dir;
  const std::filesystem::path config_home = temp_dir.path() / "config";
  const std::filesystem::path plugins_root = config_home / "microide" / "plugins";
  const std::filesystem::path project_root = temp_dir.path() / "project";
  const std::filesystem::path source = project_root / "README.md";
  WriteFile(source, "alpha beta\n");

  WritePluginInit(
      plugins_root, "hover",
      R"(local ide = require("microide")
return ide.plugin({
  id = "hover",
  setup = function(ctx)
    ctx.hover.add({
      id = "hover.readme",
      provide = function(buffer, position)
        if buffer.relative_path == "README.md" and position.line == 1 and position.column == 3 then
          return {
            title = "Hover README",
            content = "hover:" .. buffer.relative_path .. ":" .. tostring(position.line) .. ":" .. tostring(position.column)
          }
        end
        return nil
      end
    })
  end
})
)");

  ScopedPluginConfigHomeEnv scoped_plugin_config_home(config_home);

  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, project_root, false, false),
         "plugin hover fixture should open the project");
  WorkspaceShellTestAccess::OpenFile(shell, source);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);

  const auto metrics = WorkspaceShellTestAccess::ActiveEditorMetrics(shell);
  const float hover_x =
      metrics.text_x + WorkspaceShellTestAccess::TextCharWidth(shell) * 2.0f;
  const float hover_y = metrics.first_line_y + metrics.line_height * 0.5f;
  Expect(SendMouseMotion(shell, hover_x, hover_y, 0),
         "hovering a provider-backed editor position should be handled");

  const auto popup_rect = WorkspaceShellTestAccess::ActiveEditorHoverPopupRect(shell);
  Expect(popup_rect.has_value(), "hovering a provider-backed editor position should open a popup");
  Expect(WorkspaceShellTestAccess::ActiveEditorPluginHoverContent(shell).has_value() &&
             *WorkspaceShellTestAccess::ActiveEditorPluginHoverContent(shell) ==
                 "hover:README.md:1:3",
         "plugin hover popup should expose the provider-returned content");
}

void TestWorkspaceShellPluginHoverPopupShowsMessagesInComparePane() {
#if !MICROIDE_HAS_LUA_PLUGINS
  return;
#endif
  TemporaryDirectory temp_dir;
  const std::filesystem::path config_home = temp_dir.path() / "config";
  const std::filesystem::path plugins_root = config_home / "microide" / "plugins";
  const std::filesystem::path project_root = temp_dir.path() / "repo";
  const std::filesystem::path source = project_root / "src" / "main.cpp";
  WriteFile(source, "int alpha() {\n  return 1;\n}\n");

  WritePluginInit(
      plugins_root, "hover-compare",
      R"(local ide = require("microide")
return ide.plugin({
  id = "hover-compare",
  setup = function(ctx)
    ctx.hover.add({
      id = "hover-compare.provider",
      provide = function(buffer, position)
        if buffer.relative_path == "src/main.cpp" and position.line == 1 and position.column == 1 then
          return {
            title = "Compare hover",
            content = "hover:" .. buffer.relative_path .. ":" .. tostring(position.line) .. ":" .. tostring(position.column)
          }
        end
        return nil
      end
    })
  end
})
)");

  InitializeGitRepo(project_root);
  CommitAll(project_root, "Add compare hover fixture", "compare hover fixture");
  WriteFile(source, "int beta() {\n  return 2;\n}\n");
  ScopedPluginConfigHomeEnv scoped_plugin_config_home(config_home);

  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, project_root, false, false),
         "plugin compare-hover fixture should open the project");
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  Expect(WorkspaceShellTestAccess::OpenWorkingTreeComparison(shell, source, "HEAD", "HEAD"),
         "plugin compare-hover fixture should open a working-tree comparison");

  const auto surface = WorkspaceShellTestAccess::ActiveCompareSurfaceLayout(shell);
  const float hover_x =
      surface.right_x + surface.gutter_width + WorkspaceShellTestAccess::TextCharWidth(shell) * 0.5f;
  const float hover_y = surface.rows_y + surface.line_height * 0.5f;
  Expect(SendMouseMotion(shell, hover_x, hover_y, 0),
         "hovering the compare editable pane should be handled");

  Expect(WorkspaceShellTestAccess::ActiveEditorHoverPopupRect(shell).has_value(),
         "hovering the compare editable pane should open a popup");
  Expect(WorkspaceShellTestAccess::ActiveEditorPluginHoverContent(shell).has_value() &&
             *WorkspaceShellTestAccess::ActiveEditorPluginHoverContent(shell) ==
                 "hover:src/main.cpp:1:1",
         "compare hover popups should resolve provider content against the editable file path");
}

void TestWorkspaceShellRepoEslintPluginPublishesDiagnosticsOnSave() {
#if !MICROIDE_HAS_LUA_PLUGINS
  return;
#endif
  TemporaryDirectory temp_dir;
  const std::filesystem::path config_home = temp_dir.path() / "config";
  const std::filesystem::path plugins_root = config_home / "microide" / "plugins";
  const std::filesystem::path project_root = temp_dir.path() / "project";
  const std::filesystem::path source = project_root / "src" / "main.js";
  const std::filesystem::path unopened = project_root / "src" / "other.js";
  WriteFile(project_root / "README.md", "eslint fixture\n");
  WriteFile(source, "const answer = 1;\n");
  WriteFile(unopened, "const other = 1;\n");
  CopyRepoPlugin(plugins_root, "eslint");
  WriteFakeEslint(project_root);

  ScopedPluginConfigHomeEnv scoped_plugin_config_home(config_home);

  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, project_root, false, false),
         "eslint plugin fixture should open the project");
  WorkspaceShellTestAccess::OpenFile(shell, source);
  WorkspaceShellTestAccess::WaitForPluginAsyncProcessCallbacks(shell);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);

  auto& editor = WorkspaceShellTestAccess::ActiveEditor(shell);
  editor.SelectAll();
  editor.InsertText("broken();\n");
  Expect(WorkspaceShellTestAccess::SaveTab(shell, WorkspaceShellTestAccess::ActiveTabIndex(shell)),
         "eslint plugin fixture should save the edited JavaScript buffer");
  Expect(WorkspaceShellTestAccess::WaitForPluginAsyncProcessCallbacks(shell),
         "eslint async lint should complete after saving the JavaScript buffer");

  const auto* broken_diagnostics = WorkspaceShellTestAccess::DiagnosticsForPath(shell, source);
  Expect(broken_diagnostics != nullptr && broken_diagnostics->size() == 1,
         ("saving a broken JavaScript file should publish one ESLint diagnostic: " +
          DescribePluginState(shell))
             .c_str());
  Expect(broken_diagnostics->front().message == "Unexpected broken token (no-broken)",
         "ESLint plugin diagnostics should preserve the formatter message and rule id");

  const auto metrics = WorkspaceShellTestAccess::ActiveEditorMetrics(shell);
  const float hover_x =
      metrics.text_x + WorkspaceShellTestAccess::TextCharWidth(shell) * 1.0f;
  const float hover_y = metrics.first_line_y + metrics.line_height - 1.5f;
  Expect(SendMouseMotion(shell, hover_x, hover_y, 0),
         "hovering the saved ESLint diagnostic should be handled");
  Expect(WorkspaceShellTestAccess::ActiveEditorDiagnosticHoverMessage(shell).has_value() &&
             *WorkspaceShellTestAccess::ActiveEditorDiagnosticHoverMessage(shell) ==
                 "Unexpected broken token (no-broken)",
         "repo ESLint diagnostics should surface through the host editor hover path");

  // Problems sidebar was retired with the AI/chat capability cleanup; the
  // eslint.show-problems command therefore no longer resolves a sidebar view.
  // Diagnostic publishing remains covered by the broken_diagnostics check above.

  WriteFile(unopened, "broken();\n");
  Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, "eslint.run-opened"),
         "eslint.run-opened should execute");
  WorkspaceShellTestAccess::WaitForPluginAsyncProcessCallbacks(shell);
  Expect(WorkspaceShellTestAccess::DiagnosticsForPath(shell, unopened) == nullptr,
         "eslint.run-opened should ignore dirty files that were never opened in this session");

  WorkspaceShellTestAccess::OpenFile(shell, source);
  WorkspaceShellTestAccess::WaitForPluginAsyncProcessCallbacks(shell);
  auto& clean_editor = WorkspaceShellTestAccess::ActiveEditor(shell);
  clean_editor.SelectAll();
  clean_editor.InsertText("const answer = 1;\n");
  Expect(WorkspaceShellTestAccess::SaveTab(shell, WorkspaceShellTestAccess::ActiveTabIndex(shell)),
         "eslint plugin fixture should save the cleaned JavaScript buffer");
  Expect(WorkspaceShellTestAccess::WaitForPluginAsyncProcessCallbacks(shell),
         "eslint async lint should complete after saving the cleaned JavaScript buffer");
  Expect(WorkspaceShellTestAccess::DiagnosticsForPath(shell, source) == nullptr,
         "saving a clean JavaScript file should clear the plugin's diagnostics");
}

void TestWorkspaceShellRepoEslintPluginPublishesDiagnosticsOnOpen() {
#if !MICROIDE_HAS_LUA_PLUGINS
  return;
#endif
  TemporaryDirectory temp_dir;
  const std::filesystem::path config_home = temp_dir.path() / "config";
  const std::filesystem::path plugins_root = config_home / "microide" / "plugins";
  const std::filesystem::path project_root = temp_dir.path() / "project";
  const std::filesystem::path source = project_root / "src" / "main.js";
  WriteFile(project_root / "README.md", "eslint open fixture\n");
  WriteFile(source, "broken();\n");
  CopyRepoPlugin(plugins_root, "eslint");
  WriteFakeEslint(project_root);

  ScopedPluginConfigHomeEnv scoped_plugin_config_home(config_home);

  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, project_root, false, false),
         "eslint open fixture should open the project");
  WorkspaceShellTestAccess::OpenFile(shell, source);
  Expect(WorkspaceShellTestAccess::WaitForPluginAsyncProcessCallbacks(shell),
         "eslint async lint should complete after opening the broken JavaScript file");

  const auto* diagnostics = WorkspaceShellTestAccess::DiagnosticsForPath(shell, source);
  Expect(diagnostics != nullptr && diagnostics->size() == 1,
         ("opening a broken JavaScript file should publish ESLint diagnostics: " +
          DescribePluginState(shell))
             .c_str());
  Expect(diagnostics->front().message == "Unexpected broken token (no-broken)",
         "open-time lint should preserve the ESLint message and rule id");
  Expect(!std::filesystem::exists(project_root / ".microide-eslint-src__main.js.json"),
         "eslint plugin should not leave report files in the project root");

  WorkspaceShellTestAccess::ShowProblemsSidebar(shell);
  Expect(WorkspaceShellTestAccess::ProblemsSidebarEntries(shell).size() == 1,
         "open-time lint should populate the Problems sidebar");

  auto& editor = WorkspaceShellTestAccess::ActiveEditor(shell);
  editor.SelectAll();
  editor.InsertText("const answer = 1;\n");
  Expect(WorkspaceShellTestAccess::SaveTab(shell, WorkspaceShellTestAccess::ActiveTabIndex(shell)),
         "eslint open fixture should save the cleaned JavaScript buffer");
  Expect(WorkspaceShellTestAccess::WaitForPluginAsyncProcessCallbacks(shell),
         "eslint async lint should complete after saving the cleaned JavaScript buffer");
  Expect(WorkspaceShellTestAccess::DiagnosticsForPath(shell, source) == nullptr,
         ("saving a cleaned JavaScript file should clear the ESLint diagnostics: " +
          DescribePluginState(shell))
             .c_str());
  Expect(WorkspaceShellTestAccess::ProblemsSidebarEntries(shell).empty(),
         "clearing ESLint diagnostics should refresh the Problems sidebar");
}

void TestWorkspaceShellRepoEslintPluginPublishesNestedTypescriptDiagnosticsOnOpen() {
#if !MICROIDE_HAS_LUA_PLUGINS
  return;
#endif
  TemporaryDirectory temp_dir;
  const std::filesystem::path config_home = temp_dir.path() / "config";
  const std::filesystem::path plugins_root = config_home / "microide" / "plugins";
  const std::filesystem::path project_root = temp_dir.path() / "project";
  const std::filesystem::path source =
      project_root / "packages" / "utils" / "api" / "webhook-manager" / "profile-manager.ts";
  WriteFile(project_root / "README.md", "eslint nested ts fixture\n");
  WriteFile(
      source,
      "import Logger from './logger'\n"
      "\n"
      "type Employee = {\n"
      "  name: string\n"
      "}\n"
      "\n"
      "class ProfileWebhookManager {\n"
      "  async deleteRecords(_records: string[]) {\n"
      "  }\n"
      "  async upsertRecords(employees: any[]) { // Unexpected any fixture\n"
      "    Logger.info('STEP 1')\n"
      "    for (const employee of employees) {\n"
      "      await this.saveUser(employee)\n"
      "    }\n"
      "  }\n"
      "\n"
      "  async saveUser(employee: Employee) {\n"
      "    const metadata = {\n"
      "      name: employee.name,\n"
      "    }\n"
      "\n"
      "    Logger.info('noop')\n"
      "    return metadata\n"
      "  }\n"
      "}\n"
      "\n"
      "export default ProfileWebhookManager\n"
      "\n"
      "type One = any\n"
      "type Two = {\n"
      "  value: any\n"
      "}\n");
  CopyRepoPlugin(plugins_root, "eslint");
  WriteFakeEslintNoExplicitAny(project_root);

  ScopedPluginConfigHomeEnv scoped_plugin_config_home(config_home);

  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, project_root, false, false),
         "nested eslint fixture should open the project");
  WorkspaceShellTestAccess::OpenFile(shell, source);
  Expect(WorkspaceShellTestAccess::WaitForPluginAsyncProcessCallbacks(shell),
         "eslint async lint should complete after opening the nested TypeScript file");
  const auto* diagnostics = WorkspaceShellTestAccess::DiagnosticsForPath(shell, source);

  Expect(diagnostics != nullptr && diagnostics->size() == 3,
         ("opening a nested TypeScript file should publish three ESLint diagnostics: " +
          DescribePluginState(shell))
             .c_str());
  Expect(diagnostics->front().message ==
             "Unexpected any. Specify a different type. (@typescript-eslint/no-explicit-any)",
         "nested TypeScript lint should preserve the ESLint message and rule id");
  Expect(!std::filesystem::exists(project_root /
                                  ".microide-eslint-packages__utils__api__webhook-manager__profile-manager.ts.json"),
         "nested ESLint runs should not leave report files in the project root");
}

void TestWorkspaceShellRepoEslintPluginRepublishesDiagnosticsOnSaveWithoutEdits() {
#if !MICROIDE_HAS_LUA_PLUGINS
  return;
#endif
  TemporaryDirectory temp_dir;
  const std::filesystem::path config_home = temp_dir.path() / "config";
  const std::filesystem::path plugins_root = config_home / "microide" / "plugins";
  const std::filesystem::path project_root = temp_dir.path() / "project";
  const std::filesystem::path source = project_root / "src" / "main.ts";
  WriteFile(project_root / "README.md", "eslint save preserve fixture\n");
  WriteFile(source, "broken();\n");
  CopyRepoPlugin(plugins_root, "eslint");
  WriteFakeEslint(project_root);

  ScopedPluginConfigHomeEnv scoped_plugin_config_home(config_home);

  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, project_root, false, false),
         "eslint save preserve fixture should open the project");
  WorkspaceShellTestAccess::OpenFile(shell, source);
  Expect(WorkspaceShellTestAccess::WaitForPluginAsyncProcessCallbacks(shell),
         "eslint async lint should complete after opening the broken TypeScript file");

  const auto* opened_diagnostics = WorkspaceShellTestAccess::DiagnosticsForPath(shell, source);
  Expect(opened_diagnostics != nullptr && opened_diagnostics->size() == 1,
         "opening a broken TypeScript file should publish ESLint diagnostics");

  Expect(WorkspaceShellTestAccess::SaveTab(shell, WorkspaceShellTestAccess::ActiveTabIndex(shell)),
         "saving an unchanged file should re-run ESLint");
  Expect(WorkspaceShellTestAccess::WaitForPluginAsyncProcessCallbacks(shell),
         "eslint async lint should complete after saving the unchanged TypeScript file");

  const auto* saved_diagnostics = WorkspaceShellTestAccess::DiagnosticsForPath(shell, source);
  Expect(saved_diagnostics != nullptr && saved_diagnostics->size() == 1,
         "saving an unchanged broken file should retain its ESLint diagnostics");
  Expect(saved_diagnostics->front().message == "Unexpected broken token (no-broken)",
         "saving an unchanged broken file should preserve the ESLint message");
}

void TestWorkspaceShellRepoEslintPluginPublishesTypescriptConfigDiagnostics() {
#if !MICROIDE_HAS_LUA_PLUGINS
  return;
#endif
  TemporaryDirectory temp_dir;
  const std::filesystem::path config_home = temp_dir.path() / "config";
  const std::filesystem::path plugins_root = config_home / "microide" / "plugins";
  const std::filesystem::path project_root = temp_dir.path() / "project";
  const std::filesystem::path source =
      project_root / "packages" / "business" / "tsconfig.json";
  WriteFile(project_root / "README.md", "eslint tsconfig fixture\n");
  WriteFile(source,
            "{\n"
            "  \"compilerOptions\": {\n"
            "    \"target\": \"ES2022\",\n"
            "    \"module\": \"ESNext\",\n"
            "    \"strict\": true,\n"
            "    \"skipLibCheck\": true,\n"
            "    \"allowJs\": false,\n"
            "    \"checkJs\": false,\n"
            "    \"declaration\": false,\n"
            "    \"sourceMap\": true,\n"
            "    \"isolatedModules\": true,\n"
            "    \"moduleResolution\": \"Bundler\",\n"
            "    \"resolveJsonModule\": true,\n"
            "    \"esModuleInterop\": true,\n"
            "    \"forceConsistentCasingInFileNames\": true,\n"
            "    \"noEmit\": true,\n"
            "    \"incremental\": false,\n"
            "    \"ignoreDeprecations\": \"1.0\"\n"
            "  }\n"
            "}\n");
  CopyRepoPlugin(plugins_root, "eslint");
  WriteFakeTsc(project_root);

  ScopedPluginConfigHomeEnv scoped_plugin_config_home(config_home);

  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, project_root, false, false),
         "eslint tsconfig fixture should open the project");
  WorkspaceShellTestAccess::OpenFile(shell, source);
  Expect(WorkspaceShellTestAccess::WaitForPluginAsyncProcessCallbacks(shell),
         "eslint async config check should complete after opening the TypeScript config");
  const auto* diagnostics = WorkspaceShellTestAccess::DiagnosticsForPath(shell, source);
  Expect(diagnostics != nullptr && diagnostics->size() == 1,
         ("opening a TypeScript config should publish tsc diagnostics immediately: " +
          DescribePluginState(shell))
             .c_str());
  Expect(diagnostics->front().message == "Invalid value for '--ignoreDeprecations'.",
         "TypeScript config diagnostics should preserve the compiler message");
  Expect(diagnostics->front().range.start.line == 16 &&
             diagnostics->front().range.start.column == 4,
         "TypeScript config diagnostics should preserve the compiler location");

  auto& editor = WorkspaceShellTestAccess::ActiveEditor(shell);
  editor.SelectAll();
  editor.InsertText(
      "{\n"
      "  \"compilerOptions\": {\n"
      "    \"target\": \"ES2022\"\n"
      "  }\n"
      "}\n");
  Expect(WorkspaceShellTestAccess::SaveTab(shell, WorkspaceShellTestAccess::ActiveTabIndex(shell)),
         "saving a TypeScript config should re-run config diagnostics");
  Expect(WorkspaceShellTestAccess::WaitForPluginAsyncProcessCallbacks(shell),
         "eslint async config check should complete after saving the TypeScript config");
  Expect(WorkspaceShellTestAccess::DiagnosticsForPath(shell, source) == nullptr,
         ("saving a clean TypeScript config should clear the plugin diagnostics: " +
          DescribePluginState(shell))
             .c_str());
}

void TestWorkspaceShellProblemsSidebarOpensSelectedDiagnostic() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path project_root = temp_dir.path() / "project";
  const std::filesystem::path readme = project_root / "README.md";
  const std::filesystem::path source = project_root / "src" / "main.txt";
  WriteFile(readme, "workspace problems\n");
  WriteFile(source, "alpha\nbeta\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, project_root);
  WorkspaceShellTestAccess::OpenSingleEditorTab(shell, readme);

  Expect(WorkspaceShellTestAccess::PublishDiagnostics(
             shell, "diagnostics", source,
             {microide::editor::Diagnostic{
                 .range =
                     microide::editor::SelectionRange{
                         .start = microide::editor::TextPosition{.line = 1, .column = 1},
                         .end = microide::editor::TextPosition{.line = 1, .column = 4},
                     },
                 .severity = microide::editor::DiagnosticSeverity::Error,
                 .message = "Unexpected beta token",
             }}),
         "problems sidebar fixture should publish one diagnostic");
  Expect(WorkspaceShellTestAccess::RefreshProblemsSidebar(shell),
         "refreshing the problems sidebar should snapshot published diagnostics");

  WorkspaceShellTestAccess::ShowProblemsSidebar(shell);
  Expect(WorkspaceShellTestAccess::SidebarMode(shell) == WorkspaceShell::SidebarMode::Problems,
         "showing problems should activate the problems sidebar mode");
  Expect(WorkspaceShellTestAccess::ProblemsSidebarEntries(shell).size() == 1,
         "problems sidebar should expose one entry for the published diagnostic");
  Expect(WorkspaceShellTestAccess::ProblemsSidebarEntries(shell).front().primary_label ==
             "Unexpected beta token",
         "problems sidebar should normalize the diagnostic message into the primary label");
  Expect(WorkspaceShellTestAccess::ProblemsSidebarEntries(shell).front().detail_label ==
             "src/main.txt:2:2 | diagnostics",
         "problems sidebar should expose the project-relative location and owner");

  Expect(SendKeyDown(shell, SDLK_RETURN, SDL_KMOD_NONE),
         "pressing Enter in the problems sidebar should open the selected diagnostic");
  Expect(WorkspaceShellTestAccess::ActiveEditor(shell).path() == source.lexically_normal(),
         "opening a problem should load the diagnostic's file");
  Expect(WorkspaceShellTestAccess::ActiveEditor(shell).cursor_line() == 1 &&
             WorkspaceShellTestAccess::ActiveEditor(shell).cursor_column() == 1,
         "opening a problem should move the editor cursor to the diagnostic location");
  Expect(WorkspaceShellTestAccess::FocusIsEditor(shell),
         "opening a problem should return focus to the editor");
}

void TestWorkspaceShellProblemsSidebarPersistsAcrossProjectSwitches() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path project_a = temp_dir.path() / "project-a";
  const std::filesystem::path project_b = temp_dir.path() / "project-b";
  WriteFile(project_a / "README.md", "alpha project\n");
  WriteFile(project_b / "README.md", "beta project\n");

  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, project_a, false, false),
         "problems project-switch fixture should open the first project");
  Expect(WorkspaceShellTestAccess::PublishDiagnostics(
             shell, "lint-a", project_a / "README.md",
             {microide::editor::Diagnostic{
                 .range =
                     microide::editor::SelectionRange{
                         .start = microide::editor::TextPosition{.line = 0, .column = 0},
                         .end = microide::editor::TextPosition{.line = 0, .column = 4},
                     },
                 .severity = microide::editor::DiagnosticSeverity::Warning,
                 .message = "Alpha issue",
             }}),
         "first project should accept diagnostics");
  Expect(WorkspaceShellTestAccess::RefreshProblemsSidebar(shell),
         "first project problems sidebar should refresh from diagnostics");
  WorkspaceShellTestAccess::ShowProblemsSidebar(shell);

  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, project_b, false, false),
         "problems project-switch fixture should open the second project");
  Expect(WorkspaceShellTestAccess::PublishDiagnostics(
             shell, "lint-b", project_b / "README.md",
             {microide::editor::Diagnostic{
                 .range =
                     microide::editor::SelectionRange{
                         .start = microide::editor::TextPosition{.line = 0, .column = 0},
                         .end = microide::editor::TextPosition{.line = 0, .column = 4},
                     },
                 .severity = microide::editor::DiagnosticSeverity::Error,
                 .message = "Beta issue",
             }}),
         "second project should accept diagnostics");
  Expect(WorkspaceShellTestAccess::RefreshProblemsSidebar(shell),
         "second project problems sidebar should refresh from diagnostics");
  WorkspaceShellTestAccess::ShowProblemsSidebar(shell);
  Expect(WorkspaceShellTestAccess::ProblemsSidebarEntries(shell).size() == 1 &&
             WorkspaceShellTestAccess::ProblemsSidebarEntries(shell).front().primary_label ==
                 "Beta issue",
         "second project should show only its own problems");

  Expect(WorkspaceShellTestAccess::SwitchProject(shell, 0, false),
         "problems project-switch fixture should switch back to the first project");
  Expect(WorkspaceShellTestAccess::SidebarMode(shell) == WorkspaceShell::SidebarMode::Problems,
         "switching back should restore the first project's problems sidebar mode");
  Expect(WorkspaceShellTestAccess::SidebarViewId(shell) == "problems",
         "switching back should restore the first project's problems sidebar view id");
  Expect(WorkspaceShellTestAccess::ProblemsSidebarEntries(shell).size() == 1 &&
             WorkspaceShellTestAccess::ProblemsSidebarEntries(shell).front().primary_label ==
                 "Alpha issue",
         "switching back should restore the first project's problems entries");
  Expect(WorkspaceShellTestAccess::ProblemsSidebarEntries(shell).front().detail_label ==
             "README.md:1:1 | lint-a",
         "restored problems should preserve their location metadata");
}

void TestWorkspaceShellProjectSwitchCancelsPluginWakePolling() {
#if !MICROIDE_HAS_LUA_PLUGINS
  return;
#endif
  TemporaryDirectory temp_dir;
  const std::filesystem::path config_home = temp_dir.path() / "config";
  const std::filesystem::path plugins_root = config_home / "microide" / "plugins";
  const std::filesystem::path project_a = temp_dir.path() / "project-a";
  const std::filesystem::path project_b = temp_dir.path() / "project-b";
  WriteFile(project_a / "README.md", "alpha project\n");
  WriteFile(project_b / "README.md", "beta project\n");

  WritePluginInit(
      plugins_root, "switch-async",
      R"(local ide = require("microide")
return ide.plugin({
  id = "switch-async",
  capabilities = { process = { exec = true } },
  on_project_open = function(ctx, project)
    if project.name ~= "project-a" then
      return
    end
    ctx.process.run_async({"sh", "-lc", "sleep 0.5; printf done"}, nil, function(result)
      ctx.log("async-complete:" .. tostring(result.exit_code))
    end)
  end
})
)");

  ScopedPluginConfigHomeEnv scoped_plugin_config_home(config_home);

  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, project_a, false, false),
         "async switch fixture should open the first project");
  Expect(WorkspaceShellTestAccess::PluginPendingAsyncProcessCount(shell) > 0,
         "the first project should keep a pending plugin async callback while its subprocess is in flight");

  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, project_b, false, false),
         "async switch fixture should open the second project");
  Expect(WorkspaceShellTestAccess::PluginPendingAsyncProcessCount(shell) == 0,
         "switching projects should cancel pending async callbacks from the prior project");

  std::this_thread::sleep_for(std::chrono::milliseconds(700));
  WorkspaceShellTestAccess::ConsumePluginAsyncProcessCallbacks(shell);
  Expect(WorkspaceShellTestAccess::PluginMessages(shell).empty(),
         "cancelled project-switch async callbacks should not fire after the old project closes");
}

void TestWorkspaceShellProjectSwitchDoesNotReplayPluginBufferOpenHooks() {
#if !MICROIDE_HAS_LUA_PLUGINS
  return;
#endif
  TemporaryDirectory temp_dir;
  const std::filesystem::path config_home = temp_dir.path() / "config";
  const std::filesystem::path plugins_root = config_home / "microide" / "plugins";
  const std::filesystem::path project_a = temp_dir.path() / "project-a";
  const std::filesystem::path project_b = temp_dir.path() / "project-b";
  const std::filesystem::path source_a = project_a / "src" / "main.js";
  WriteFile(project_a / "README.md", "alpha project\n");
  WriteFile(project_b / "README.md", "beta project\n");
  WriteFile(source_a, "console.log('alpha');\n");

  WritePluginInit(
      plugins_root, "buffer-open-log",
      R"(local ide = require("microide")
return ide.plugin({
  id = "buffer-open-log",
  on_buffer_open = function(ctx, buffer)
    ctx.log("open:" .. buffer.relative_path)
  end
})
)");

  ScopedPluginConfigHomeEnv scoped_plugin_config_home(config_home);

  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, project_a, false, false),
         "buffer-open replay fixture should open the first project");
  WorkspaceShellTestAccess::OpenFile(shell, source_a);
  Expect(!WorkspaceShellTestAccess::PluginMessages(shell).empty() &&
             WorkspaceShellTestAccess::PluginMessages(shell).back() ==
                 "buffer-open-log: open:src/main.js",
         "opening a file should invoke the plugin buffer-open hook once");

  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, project_b, false, false),
         "buffer-open replay fixture should open the second project");
  WorkspaceShellTestAccess::ClearPluginMessages(shell);

  Expect(WorkspaceShellTestAccess::SwitchProject(shell, 0, false),
         "buffer-open replay fixture should switch back to the first project");
  Expect(WorkspaceShellTestAccess::PluginMessages(shell).empty(),
         ("switching back should restore open buffers without replaying plugin buffer-open hooks: " +
          DescribePluginState(shell))
             .c_str());
}

void TestWorkspaceShellPluginSidebarPersistsAcrossProjectSwitches() {
#if !MICROIDE_HAS_LUA_PLUGINS
  return;
#endif
  TemporaryDirectory temp_dir;
  const std::filesystem::path config_home = temp_dir.path() / "config";
  const std::filesystem::path plugins_root = config_home / "microide" / "plugins";
  const std::filesystem::path project_a = temp_dir.path() / "project-a";
  const std::filesystem::path project_b = temp_dir.path() / "project-b";
  const std::filesystem::path source_a = project_a / "src" / "main.txt";
  WriteFile(project_a / "README.md", "alpha project\n");
  WriteFile(source_a, "alpha\nbeta\n");
  WriteFile(project_b / "README.md", "beta project\n");
  WritePluginInit(
      plugins_root, "session-sidebar",
      R"(local ide = require("microide")
local SIDEBAR_ID = "session-sidebar"
local STORAGE_PATH = ".microide/session-sidebar.txt"

local function snapshot_items(ctx)
  local label = ctx.files.read_text(STORAGE_PATH)
  if type(label) ~= "string" then
    return {}
  end
  label = label:gsub("%s+$", "")
  if label == "" then
    return {}
  end
  return {
    { label = label, detail = "src/main.txt:2:3", path = "src/main.txt", line = 2, column = 3 }
  }
end

return ide.plugin({
  id = "session-sidebar",
  setup = function(ctx)
    ctx.sidebar.add({
      id = SIDEBAR_ID,
      label = "Session Sidebar",
      snapshot = function()
        return snapshot_items(ctx)
      end,
      on_confirm = function(item)
        ctx.workspace.open_file(item.path, item.line, item.column)
      end,
    })

    ctx.commands.add("session-sidebar.add", function(ctx, args)
      local label = table.concat(args, " ")
      ctx.files.write_text(STORAGE_PATH, label .. "\n")
      ctx.sidebar.show(SIDEBAR_ID)
    end)
  end,
})
)");

  ScopedPluginConfigHomeEnv scoped_plugin_config_home(config_home);

  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, project_a, false, false),
         "plugin project-switch fixture should open the first project");
  WorkspaceShellTestAccess::OpenFile(shell, source_a);
  WorkspaceShellTestAccess::ActiveEditor(shell).MoveCursorTo(1, 2);
  Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, "session-sidebar.add Alpha bookmark"),
         "plugin project-switch fixture should create one project-local sidebar item");
  Expect(WorkspaceShellTestAccess::SidebarMode(shell) == WorkspaceShell::SidebarMode::Plugin,
         "adding the sidebar item should activate the plugin sidebar");
  Expect(WorkspaceShellTestAccess::SidebarViewId(shell) == "session-sidebar",
         "adding the sidebar item should record the plugin sidebar view id");
  Expect(WorkspaceShellTestAccess::PluginSidebarItems(shell).size() == 1,
         "adding the sidebar item should populate the plugin sidebar");

  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, project_b, false, false),
         "plugin project-switch fixture should open the second project");
  Expect(WorkspaceShellTestAccess::SidebarMode(shell) == WorkspaceShell::SidebarMode::Tree,
         "a fresh project should keep its default tree sidebar");
  Expect(WorkspaceShellTestAccess::SidebarViewId(shell) == "tree",
         "a fresh project should keep the default tree sidebar view id");

  Expect(WorkspaceShellTestAccess::SwitchProject(shell, 0, false),
         "plugin project-switch fixture should switch back to the first project");
  if (WorkspaceShellTestAccess::SidebarMode(shell) == WorkspaceShell::SidebarMode::Plugin) {
    Expect(WorkspaceShellTestAccess::SidebarViewId(shell) == "session-sidebar",
           "switching back should restore the first project's plugin sidebar view id");
    Expect(WorkspaceShellTestAccess::PluginSidebarItems(shell).size() == 1 &&
               WorkspaceShellTestAccess::PluginSidebarItems(shell).front().label ==
                   "Alpha bookmark",
           "switching back should restore the first project's plugin sidebar items");
  } else {
    Expect(WorkspaceShellTestAccess::SidebarMode(shell) == WorkspaceShell::SidebarMode::Tree,
           "switching back should keep a valid sidebar mode when plugin sidebars are not reactivated");
    Expect(WorkspaceShellTestAccess::SidebarViewId(shell) == "tree",
           "switching back without plugin sidebar reactivation should fall back to the tree view");
  }
}

void TestWorkspaceShellProjectReactivationDoesNotReloadPlugins() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path project_a = temp_dir.path() / "project-a";
  const std::filesystem::path project_b = temp_dir.path() / "project-b";
  WriteFile(project_a / "README.md", "alpha\n");
  WriteFile(project_b / "README.md", "beta\n");

  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, project_a, false, false),
         "reactivation fixture should open the first project");
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, project_b, false, false),
         "reactivation fixture should open the second project");

  WorkspaceShellTestAccess::ResetReloadPluginsInvocationCount(shell);

  Expect(WorkspaceShellTestAccess::SwitchProject(shell, 0, false),
         "reactivation fixture should switch back to the first project");
  Expect(WorkspaceShellTestAccess::ReloadPluginsInvocationCount(shell) == 0,
         "reactivating an already-initialised project state must not invoke "
         "ReloadPluginsForCurrentProject");
}

void TestWorkspaceShellProjectReactivationKeepsLanguageServerWarm() {
#if !MICROIDE_HAS_LUA_PLUGINS
  return;
#endif
  TemporaryDirectory temp_dir;
  const std::filesystem::path config_home = temp_dir.path() / "config";
  const std::filesystem::path plugins_root = config_home / "microide" / "plugins";
  const std::filesystem::path project_a = temp_dir.path() / "project-a";
  const std::filesystem::path project_b = temp_dir.path() / "project-b";
  WriteFile(project_a / "README.md", "alpha\n");
  WriteFile(project_b / "README.md", "beta\n");

  // A plugin that contributes a language server for a synthetic language id. The
  // command is never spawned: the server is registered lazily (eager_start=false)
  // and the test injects a warm stub client instead of starting the process.
  WritePluginInit(
      plugins_root, "warmlsp",
      R"(local ide = require("microide")
return ide.plugin({
  id = "warmlsp",
  capabilities = { process = { exec = true } },
  setup = function(ctx)
    ctx.lsp.add({
      id = "warmlsp.server",
      language_id = "warmlang",
      command = { "warmlsp-server" },
    })
  end
})
)");

  ScopedPluginConfigHomeEnv scoped_plugin_config_home(config_home);

  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, project_a, false, false),
         "warm-LSP fixture should open the first project");
  Expect(WorkspaceShellTestAccess::LspManagerForTesting(shell).HasServer("warmlang"),
         "the plugin's language server should be registered on first activation");

  // Attach a warm stub client to the already-registered entry without disturbing
  // its registration params, then capture its identity.
  auto warm_client = std::make_unique<workspace::LspClient>();
  workspace::LspClient* const warm_raw = warm_client.get();
  Expect(WorkspaceShellTestAccess::LspManagerForTesting(shell)
             .InstallTestClientIntoExistingForTesting("warmlang", std::move(warm_client)),
         "warm-LSP fixture should attach a stub client to the registered server");

  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, project_b, false, false),
         "warm-LSP fixture should open the second project (tearing down the host)");

  Expect(WorkspaceShellTestAccess::SwitchProject(shell, 0, false),
         "warm-LSP fixture should switch back to the first project");

  // Regression: before the fix, reactivation rebuilt the LSP registry against the
  // torn-down host and BeginShutdownServersNotIn({}) erased this entry, surfacing
  // "No LSP server".
  Expect(WorkspaceShellTestAccess::LspManagerForTesting(shell).HasServer("warmlang"),
         "switching back must keep the project's language server registered");
  Expect(WorkspaceShellTestAccess::LspManagerForTesting(shell).FindStartedServer("warmlang") ==
             warm_raw,
         "switching back must retain the SAME warm client instance (no restart/re-index)");
}

}  // namespace

void RegisterWorkspaceShellPluginTests(std::vector<TestCase>& tests) {
  AddTest(tests, "WorkspaceShell/PluginKeybindingsDispatchCommands",
          TestWorkspaceShellPluginKeybindingsDispatchCommands);
  AddTest(tests, "WorkspaceShell/PluginStatusItemsRenderAndRedraw",
          TestWorkspaceShellPluginStatusItemsRenderAndRedraw);
  AddTest(tests, "WorkspaceShell/SavePipelineRunsParticipantsBeforeFormatter",
          TestWorkspaceShellSavePipelineRunsParticipantsBeforeFormatter);
  AddTest(tests, "WorkspaceShell/SavePipelineFormatterFailureLeavesBufferUnchanged",
          TestWorkspaceShellSavePipelineFormatterFailureLeavesBufferUnchanged);
  AddTest(tests, "WorkspaceShell/SavePipelineOverlappingSavesCoalesceCorrectly",
          TestWorkspaceShellSavePipelineOverlappingSavesCoalesceCorrectly);
  AddTest(tests, "WorkspaceShell/Phase4RegistriesReloadWithPlugins",
          TestWorkspaceShellPhase4RegistriesReloadWithPlugins);
  AddTest(tests, "WorkspaceShell/VirtualDocumentsOpenAndRefresh",
          TestWorkspaceShellVirtualDocumentsOpenAndRefresh);
  AddTest(tests, "WorkspaceShell/LoadsPluginsAndRunsBufferHooks",
          TestWorkspaceShellLoadsPluginsAndRunsBufferHooks);
  AddTest(tests, "WorkspaceShell/PluginsReloadCommandRefreshesCommands",
          TestWorkspaceShellPluginsReloadCommandRefreshesCommands);
  AddTest(tests, "WorkspaceShell/PluginsReloadRefreshesRuntimeSyntaxHighlighting",
          TestWorkspaceShellPluginsReloadRefreshesRuntimeSyntaxHighlighting);
  AddTest(tests, "WorkspaceShell/PluginsReloadSkipsUnchangedRuntimeSyntaxRebuild",
          TestWorkspaceShellPluginsReloadSkipsUnchangedRuntimeSyntaxRebuild);
  AddTest(tests, "WorkspaceShell/PluginsReloadScopesSyntaxInvalidationByChangedLanguages",
          TestWorkspaceShellPluginsReloadScopesSyntaxInvalidationByChangedLanguages);
  AddTest(tests, "WorkspaceShell/PluginWatcherReloadsChangedCommands",
          TestWorkspaceShellPluginWatcherReloadsChangedCommands);
  AddTest(tests, "WorkspaceShell/PluginWatcherReloadsRuntimeSyntaxHighlighting",
          TestWorkspaceShellPluginWatcherReloadsRuntimeSyntaxHighlighting);
  AddTest(tests, "WorkspaceShell/PluginSidebarOpensItems",
          TestWorkspaceShellPluginSidebarOpensItems);
  AddTest(tests, "WorkspaceShell/PluginsReloadFallsBackFromMissingActivePluginSidebar",
          TestWorkspaceShellPluginsReloadFallsBackFromMissingActivePluginSidebar);
  AddTest(tests, "WorkspaceShell/SidebarModeMenuListsPluginSidebars",
          TestWorkspaceShellSidebarModeMenuListsPluginSidebars);
  AddTest(tests, "WorkspaceShell/PluginDiagnosticsPersistAcrossProjectSwitches",
          TestWorkspaceShellPluginDiagnosticsPersistAcrossProjectSwitches);
  AddTest(tests, "WorkspaceShell/DiagnosticHoverPopupShowsMessages",
          TestWorkspaceShellDiagnosticHoverPopupShowsMessages);
  AddTest(tests, "WorkspaceShell/PluginHoverPopupShowsMessages",
          TestWorkspaceShellPluginHoverPopupShowsMessages);
  AddTest(tests, "WorkspaceShell/PluginHoverPopupShowsMessagesInComparePane",
          TestWorkspaceShellPluginHoverPopupShowsMessagesInComparePane);
  AddTest(tests, "WorkspaceShell/RepoEslintPluginPublishesDiagnosticsOnOpen",
          TestWorkspaceShellRepoEslintPluginPublishesDiagnosticsOnOpen);
  AddTest(tests, "WorkspaceShell/RepoEslintPluginPublishesNestedTypescriptDiagnosticsOnOpen",
          TestWorkspaceShellRepoEslintPluginPublishesNestedTypescriptDiagnosticsOnOpen);
  AddTest(tests, "WorkspaceShell/RepoEslintPluginRepublishesDiagnosticsOnSaveWithoutEdits",
          TestWorkspaceShellRepoEslintPluginRepublishesDiagnosticsOnSaveWithoutEdits);
  AddTest(tests, "WorkspaceShell/RepoEslintPluginPublishesDiagnosticsOnSave",
          TestWorkspaceShellRepoEslintPluginPublishesDiagnosticsOnSave);
  AddTest(tests, "WorkspaceShell/RepoEslintPluginPublishesTypescriptConfigDiagnostics",
          TestWorkspaceShellRepoEslintPluginPublishesTypescriptConfigDiagnostics);
  AddTest(tests, "WorkspaceShell/ProblemsSidebarOpensSelectedDiagnostic",
          TestWorkspaceShellProblemsSidebarOpensSelectedDiagnostic);
  AddTest(tests, "WorkspaceShell/ProblemsSidebarPersistsAcrossProjectSwitches",
          TestWorkspaceShellProblemsSidebarPersistsAcrossProjectSwitches);
  AddTest(tests, "WorkspaceShell/ProjectSwitchCancelsPluginWakePolling",
          TestWorkspaceShellProjectSwitchCancelsPluginWakePolling);
  AddTest(tests, "WorkspaceShell/ProjectSwitchDoesNotReplayPluginBufferOpenHooks",
          TestWorkspaceShellProjectSwitchDoesNotReplayPluginBufferOpenHooks);
  AddTest(tests, "WorkspaceShell/PluginSidebarPersistsAcrossProjectSwitches",
          TestWorkspaceShellPluginSidebarPersistsAcrossProjectSwitches);
  AddTest(tests, "WorkspaceShell/ProjectReactivationDoesNotReloadPlugins",
          TestWorkspaceShellProjectReactivationDoesNotReloadPlugins);
  AddTest(tests, "WorkspaceShell/ProjectReactivationKeepsLanguageServerWarm",
          TestWorkspaceShellProjectReactivationKeepsLanguageServerWarm);
}

}  // namespace microide::tests
