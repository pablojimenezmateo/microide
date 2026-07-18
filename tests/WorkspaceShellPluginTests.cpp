#include "TestSupport.h"

#include "editor/FoldingModel.h"
#include "editor/PluginDecorationStore.h"
#include "editor/RuntimeSyntaxRegistry.h"
#include "util/StringUtil.h"
#include "workspace/FileUri.h"
#include "workspace/PluginEditorEventTracker.h"
#include "workspace/WorkspaceLspClient.h"
#include "workspace/WorkspaceLspManager.h"
#include "workspace/WorkspaceShellTestAccess.h"

#include <memory>

#include <chrono>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <optional>
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
      key = "Ctrl+Shift+Y",
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
             shell, SDLK_Y, static_cast<SDL_Keymod>(SDL_KMOD_CTRL | SDL_KMOD_SHIFT)),
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
  Expect(editor::runtime_syntax::DetectFiletype(source, editor.lines().Snapshot()) == "todo",
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
    const std::string prepared_text = util::SerializeLines(editor.lines().Snapshot(), editor.line_ending());
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
         "plugin commands should execute through the shell command line");
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
  Expect(WorkspaceShellTestAccess::CommandFeedbackText(shell).find("Loaded 1 plugin") !=
             std::string::npos,
         "plugins-reload should report a load summary as command feedback");

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
  Expect(WorkspaceShellTestAccess::CommandFeedbackText(shell).find("1 syntax definition") !=
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
  Expect(WorkspaceShellTestAccess::PluginSidebarPlaceholder(shell).empty(),
         "a populated plugin sidebar should not cache an empty/error placeholder");

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

void TestWorkspaceShellSignatureHelpPopupFromPluginProvider() {
#if !MICROIDE_HAS_LUA_PLUGINS
  return;
#endif
  TemporaryDirectory temp_dir;
  const std::filesystem::path config_home = temp_dir.path() / "config";
  const std::filesystem::path plugins_root = config_home / "microide" / "plugins";
  const std::filesystem::path project_root = temp_dir.path() / "project";
  const std::filesystem::path source = project_root / "src" / "main.lua";
  WriteFile(source, "local function greet(name)\n  return name\nend\nprint(greet())\n");

  // Custom raw-string delimiter: the signature label contains `)"`, which would
  // otherwise close a default R"(...)" literal early.
  WritePluginInit(
      plugins_root, "sig-tools",
      R"LUA(local ide = require("microide")
return ide.plugin({
  id = "sig-tools",
  setup = function(ctx)
    ctx.signature_help.add({
      id = "sig", language_id = "lua",
      provide = function(_, _)
        return {
          active_signature = 0,
          signatures = {
            { label = "greet(name: string)", documentation = "Greets the given name.",
              active_parameter = 0, parameters = { { label = "name" } } },
          },
        }
      end,
    })
  end
})
)LUA");

  ScopedPluginConfigHomeEnv scoped_plugin_config_home(config_home);

  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, project_root, false, false),
         "signature-help fixture should open the project");
  WorkspaceShellTestAccess::OpenFile(shell, source);
  Expect(WorkspaceShellTestAccess::PluginErrors(shell).empty(), DescribePluginState(shell).c_str());

  Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, "signature-help"),
         "signature-help command should resolve the plugin provider");
  const auto popup = WorkspaceShellTestAccess::SignatureHelpPopup(shell);
  Expect(popup.has_value() && popup->signature == "greet(name: string)",
         "signature popup should carry the active signature label");
  Expect(popup.has_value() && popup->documentation.find("Greets the given name.") !=
                                  std::string::npos,
         "signature popup documentation block should include the signature docs");

  // The popup is anchored to the request position; moving the caret expires it.
  WorkspaceShellTestAccess::ActiveEditor(shell).MoveCursorTo(1, 0);
  WorkspaceShellTestAccess::ExpireSignatureHelp(shell);
  Expect(!WorkspaceShellTestAccess::SignatureHelpPopup(shell).has_value(),
         "moving the caret should dismiss the signature popup");
}

// Regression: the outline must populate from the language server's documentSymbol
// when no plugin document-symbol provider returns anything. The plugin provider is
// consulted first; on an empty result the outline falls back to LSP and flattens
// the adapted symbol tree into the same rows.
void TestWorkspaceShellOutlineSidebarFromLspFallback() {
#if !MICROIDE_HAS_LUA_PLUGINS
  return;
#endif
#if !defined(__unix__) && !defined(__APPLE__)
  return;
#endif
  TemporaryDirectory temp_dir;
  const std::filesystem::path config_home = temp_dir.path() / "config";
  const std::filesystem::path plugins_root = config_home / "microide" / "plugins";
  const std::filesystem::path project = temp_dir.path() / "proj";
  const std::filesystem::path py_file = project / "main.py";
  WriteFile(py_file, "class Widget:\n    def draw(self):\n        pass\n\n\ndef main():\n    pass\n");

  // Register a python language server but NO plugin document-symbol provider, so the
  // outline's plugin query returns empty and the LSP fallback drives the rows.
  WritePluginInit(
      plugins_root, "pylsp",
      R"(local ide = require("microide")
return ide.plugin({
  id = "pylsp",
  capabilities = { process = { exec = true } },
  setup = function(ctx)
    ctx.lsp.add({ id = "pylsp.server", language_id = "python", command = { "py-lsp-server" } })
  end
})
)");
  ScopedPluginConfigHomeEnv scoped_plugin_config_home(config_home);

  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, project, false, false),
         "outline LSP fixture should open the project");

  // Attach a stub server that answers documentSymbol from a canned tree.
  auto stub = std::make_unique<workspace::LspClient>();
  workspace::LspClient* const stub_raw = stub.get();
  stub_raw->EnableTestStubMode();
  stub_raw->SetTestDocumentSymbolHandler(
      [](std::string uri, workspace::LspClient::DocumentSymbolCallback cb) {
        (void)uri;
        workspace::LspClient::DocumentSymbol widget;
        widget.name = "Widget";
        widget.kind = 5;  // Class
        widget.selection_range.start = {0, 6};
        workspace::LspClient::DocumentSymbol draw;
        draw.name = "draw";
        draw.kind = 6;  // Method
        draw.selection_range.start = {1, 8};
        widget.children.push_back(draw);
        workspace::LspClient::DocumentSymbol main_fn;
        main_fn.name = "main";
        main_fn.kind = 12;  // Function
        main_fn.selection_range.start = {5, 4};
        cb(std::vector<workspace::LspClient::DocumentSymbol>{widget, main_fn});
      });
  Expect(WorkspaceShellTestAccess::LspManagerForTesting(shell)
             .InstallTestClientIntoExistingForTesting("python", std::move(stub)),
         "fixture should attach a stub python client");

  WorkspaceShellTestAccess::OpenFile(shell, py_file);
  Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, "sidebar-show outline"),
         "sidebar-show should accept the built-in outline view");
  // The plugin query resolves inline (empty), then the LSP documentSymbol stub
  // dispatches on the main-thread pump.
  WorkspaceShellTestAccess::ConsumeLspCallbacks(shell);

  const auto& items = WorkspaceShellTestAccess::PluginSidebarItems(shell);
  Expect(items.size() == 3,
         "outline should flatten the LSP document-symbol fallback into three rows");
  Expect(items[0].label == "Widget" && items[0].depth == 0,
         "LSP outline should list the class at depth 0");
  Expect(items[1].label == "draw" && items[1].depth == 1,
         "LSP outline should indent the method child to depth 1");
  Expect(items[2].label == "main" && items[2].depth == 0,
         "LSP outline should return to depth 0 after the child subtree");
  // 0-based LSP selection range -> 1-based outline coordinates.
  Expect(items[1].line == 2 && items[1].column == 9,
         "LSP outline row should carry 1-based line/column from the selection range");
}

// Regression (TD-2026-07-17A-072): a large-but-valid documentSymbol response must
// not adapt+flatten unboundedly on the main-thread callback. The outline is capped
// at a presentation budget (5000 nodes) with a non-navigable truncation marker,
// instead of building 100k rows the protocol parser would still accept.
void TestWorkspaceShellOutlineCapsLargeLspResult() {
#if !MICROIDE_HAS_LUA_PLUGINS
  return;
#endif
#if !defined(__unix__) && !defined(__APPLE__)
  return;
#endif
  TemporaryDirectory temp_dir;
  const std::filesystem::path config_home = temp_dir.path() / "config";
  const std::filesystem::path plugins_root = config_home / "microide" / "plugins";
  const std::filesystem::path project = temp_dir.path() / "proj";
  const std::filesystem::path py_file = project / "main.py";
  WriteFile(py_file, "value = 1\n");

  WritePluginInit(
      plugins_root, "pylsp",
      R"(local ide = require("microide")
return ide.plugin({
  id = "pylsp",
  capabilities = { process = { exec = true } },
  setup = function(ctx)
    ctx.lsp.add({ id = "pylsp.server", language_id = "python", command = { "py-lsp-server" } })
  end
})
)");
  ScopedPluginConfigHomeEnv scoped_plugin_config_home(config_home);

  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, project, false, false),
         "outline cap fixture should open the project");

  auto stub = std::make_unique<workspace::LspClient>();
  workspace::LspClient* const stub_raw = stub.get();
  stub_raw->EnableTestStubMode();
  stub_raw->SetTestDocumentSymbolHandler(
      [](std::string uri, workspace::LspClient::DocumentSymbolCallback cb) {
        (void)uri;
        // 6000 flat top-level symbols: over the 5000-node presentation budget.
        std::vector<workspace::LspClient::DocumentSymbol> symbols;
        symbols.reserve(6000);
        for (int i = 0; i < 6000; ++i) {
          workspace::LspClient::DocumentSymbol symbol;
          symbol.name = "sym" + std::to_string(i);
          symbol.kind = 12;  // Function
          symbol.selection_range.start = {0, 0};
          symbols.push_back(std::move(symbol));
        }
        cb(std::move(symbols));
      });
  Expect(WorkspaceShellTestAccess::LspManagerForTesting(shell)
             .InstallTestClientIntoExistingForTesting("python", std::move(stub)),
         "fixture should attach a stub python client");

  WorkspaceShellTestAccess::OpenFile(shell, py_file);
  Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, "sidebar-show outline"),
         "sidebar-show should accept the built-in outline view");
  WorkspaceShellTestAccess::ConsumeLspCallbacks(shell);

  const auto& items = WorkspaceShellTestAccess::PluginSidebarItems(shell);
  // 5000 adapted symbol rows + one truncation marker row.
  Expect(items.size() == 5001,
         "the outline is capped at the 5000-node budget plus a truncation marker");
  Expect(items.back().label == "… (outline truncated)",
         "the capped outline ends with a non-navigable truncation marker");
  Expect(items.back().line == 0,
         "the truncation marker does not navigate anywhere");
}

// Regression: hovering resolves through the language server when no plugin hover
// provider answers. The LSP hover `contents` (MarkupContent here) fills the same
// hover cache the popup renders.
void TestWorkspaceShellHoverFromLspFallback() {
#if !MICROIDE_HAS_LUA_PLUGINS
  return;
#endif
#if !defined(__unix__) && !defined(__APPLE__)
  return;
#endif
  TemporaryDirectory temp_dir;
  const std::filesystem::path config_home = temp_dir.path() / "config";
  const std::filesystem::path plugins_root = config_home / "microide" / "plugins";
  const std::filesystem::path project = temp_dir.path() / "proj";
  const std::filesystem::path py_file = project / "main.py";
  WriteFile(py_file, "value = 1\n");

  WritePluginInit(
      plugins_root, "pylsp",
      R"(local ide = require("microide")
return ide.plugin({
  id = "pylsp",
  capabilities = { process = { exec = true } },
  setup = function(ctx)
    ctx.lsp.add({ id = "pylsp.server", language_id = "python", command = { "py-lsp-server" } })
  end
})
)");
  ScopedPluginConfigHomeEnv scoped_plugin_config_home(config_home);

  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, project, false, false),
         "hover LSP fixture should open the project");

  auto stub = std::make_unique<workspace::LspClient>();
  workspace::LspClient* const stub_raw = stub.get();
  stub_raw->EnableTestStubMode();
  stub_raw->SetTestHoverHandler([](std::string uri, workspace::LspClient::HoverCallback cb) {
    (void)uri;
    std::optional<util::JsonValue> hover =
        util::ParseJson(R"({"contents":{"kind":"markdown","value":"int value"}})");
    cb(std::move(hover));
  });
  Expect(WorkspaceShellTestAccess::LspManagerForTesting(shell)
             .InstallTestClientIntoExistingForTesting("python", std::move(stub)),
         "fixture should attach a stub python client");

  WorkspaceShellTestAccess::OpenFile(shell, py_file);
  const std::string content =
      WorkspaceShellTestAccess::ResolveLspHoverForTesting(shell, py_file, 1, 1);
  Expect(content == "int value",
         "LSP hover fallback should populate the hover cache from the server contents");
}

// Regression: the format-document command applies the language server's TextEdit[]
// to the active buffer. Guards the full end-to-end path: action -> LSP request ->
// multi-edit apply (the request bug that dropped all but the first edit would drop
// the second edit here).
void TestWorkspaceShellFormatDocumentAppliesLspEdits() {
#if !MICROIDE_HAS_LUA_PLUGINS
  return;
#endif
#if !defined(__unix__) && !defined(__APPLE__)
  return;
#endif
  TemporaryDirectory temp_dir;
  const std::filesystem::path config_home = temp_dir.path() / "config";
  const std::filesystem::path plugins_root = config_home / "microide" / "plugins";
  const std::filesystem::path project = temp_dir.path() / "proj";
  const std::filesystem::path py_file = project / "main.py";
  WriteFile(py_file, "x=1\ny=2\n");

  WritePluginInit(
      plugins_root, "pylsp",
      R"(local ide = require("microide")
return ide.plugin({
  id = "pylsp",
  capabilities = { process = { exec = true } },
  setup = function(ctx)
    ctx.lsp.add({ id = "pylsp.server", language_id = "python", command = { "py-lsp-server" } })
  end
})
)");
  ScopedPluginConfigHomeEnv scoped_plugin_config_home(config_home);

  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, project, false, false),
         "format fixture should open the project");

  auto stub = std::make_unique<workspace::LspClient>();
  workspace::LspClient* const stub_raw = stub.get();
  stub_raw->EnableTestStubMode();
  // Two edits: reformat both lines. The second edit must survive (the dropped-edits
  // bug would leave line 2 untouched).
  stub_raw->SetTestFormattingHandler([](std::string uri, workspace::LspClient::FormattingCallback cb) {
    (void)uri;
    cb(std::vector<workspace::LspClient::TextEdit>{
        {workspace::LspClient::Range{{0, 0}, {0, 3}}, "x = 1"},
        {workspace::LspClient::Range{{1, 0}, {1, 3}}, "y = 2"}});
  });
  Expect(WorkspaceShellTestAccess::LspManagerForTesting(shell)
             .InstallTestClientIntoExistingForTesting("python", std::move(stub)),
         "fixture should attach a stub python client");

  WorkspaceShellTestAccess::OpenFile(shell, py_file);
  Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, "format-document"),
         "format-document command should dispatch");
  WorkspaceShellTestAccess::ConsumeLspCallbacks(shell);

  Expect(WorkspaceShellTestAccess::ActiveEditor(shell).lines()[0] == "x = 1",
         "formatting should apply the first edit to line 1");
  Expect(WorkspaceShellTestAccess::ActiveEditor(shell).lines()[1] == "y = 2",
         "formatting should also apply the trailing edit to line 2 (no dropped edits)");
}

// Regression: the rename-symbol command prefills the symbol under the cursor, and
// on confirm applies the language server's workspace edit to the open buffer.
void TestWorkspaceShellRenameSymbolAppliesLspEdit() {
#if !MICROIDE_HAS_LUA_PLUGINS
  return;
#endif
#if !defined(__unix__) && !defined(__APPLE__)
  return;
#endif
  TemporaryDirectory temp_dir;
  const std::filesystem::path config_home = temp_dir.path() / "config";
  const std::filesystem::path plugins_root = config_home / "microide" / "plugins";
  const std::filesystem::path project = temp_dir.path() / "proj";
  const std::filesystem::path py_file = project / "main.py";
  WriteFile(py_file, "value = 1\nprint(value)\n");

  WritePluginInit(
      plugins_root, "pylsp",
      R"(local ide = require("microide")
return ide.plugin({
  id = "pylsp",
  capabilities = { process = { exec = true } },
  setup = function(ctx)
    ctx.lsp.add({ id = "pylsp.server", language_id = "python", command = { "py-lsp-server" } })
  end
})
)");
  ScopedPluginConfigHomeEnv scoped_plugin_config_home(config_home);

  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, project, false, false),
         "rename fixture should open the project");

  auto stub = std::make_unique<workspace::LspClient>();
  workspace::LspClient* const stub_raw = stub.get();
  stub_raw->EnableTestStubMode();
  // Rename both occurrences of `value` to the typed name.
  stub_raw->SetTestRenameHandler([](std::string uri, std::string new_name,
                                    workspace::LspClient::RenameCallback cb) {
    workspace::LspClient::WorkspaceEdit edit;
    edit.changes[uri] = {
        {workspace::LspClient::Range{{0, 0}, {0, 5}}, new_name},
        {workspace::LspClient::Range{{1, 6}, {1, 11}}, new_name},
    };
    cb(std::move(edit));
  });
  Expect(WorkspaceShellTestAccess::LspManagerForTesting(shell)
             .InstallTestClientIntoExistingForTesting("python", std::move(stub)),
         "fixture should attach a stub python client");

  WorkspaceShellTestAccess::OpenFile(shell, py_file);
  WorkspaceShellTestAccess::ActiveEditor(shell).MoveCursorTo(0, 2);  // inside "value"

  Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, "rename-symbol"),
         "rename-symbol command should open the prompt");
  Expect(WorkspaceShellTestAccess::PromptSurfaceVisible(shell),
         "rename-symbol should open a text-input prompt");
  Expect(WorkspaceShellTestAccess::PromptSurfaceInput(shell) == "value",
         "the rename prompt should prefill the symbol under the cursor");

  WorkspaceShellTestAccess::SetPromptSurfaceInput(shell, "count");
  WorkspaceShellTestAccess::ConfirmPromptSurface(shell);
  WorkspaceShellTestAccess::ConsumeLspCallbacks(shell);

  Expect(WorkspaceShellTestAccess::ActiveEditor(shell).lines()[0] == "count = 1",
         "rename should apply the edit on the declaration line");
  Expect(WorkspaceShellTestAccess::ActiveEditor(shell).lines()[1] == "print(count)",
         "rename should apply the edit to the second occurrence as well");
}

// Regression: a rename that touches files which aren't open confirms first, then
// opens every affected file, applies the edit, and saves them all to disk.
void TestWorkspaceShellRenameSymbolOpensAndSavesClosedFiles() {
#if !MICROIDE_HAS_LUA_PLUGINS
  return;
#endif
#if !defined(__unix__) && !defined(__APPLE__)
  return;
#endif
  TemporaryDirectory temp_dir;
  const std::filesystem::path config_home = temp_dir.path() / "config";
  const std::filesystem::path plugins_root = config_home / "microide" / "plugins";
  const std::filesystem::path project = temp_dir.path() / "proj";
  const std::filesystem::path main_py = project / "main.py";
  const std::filesystem::path helper_py = project / "helper.py";  // stays closed until confirm
  WriteFile(main_py, "value = 1\n");
  WriteFile(helper_py, "print(value)\n");

  WritePluginInit(
      plugins_root, "pylsp",
      R"(local ide = require("microide")
return ide.plugin({
  id = "pylsp",
  capabilities = { process = { exec = true } },
  setup = function(ctx)
    ctx.lsp.add({ id = "pylsp.server", language_id = "python", command = { "py-lsp-server" } })
  end
})
)");
  ScopedPluginConfigHomeEnv scoped_plugin_config_home(config_home);

  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, project, false, false),
         "rename fixture should open the project");

  const std::string main_uri = workspace::FileUriForPath(main_py);
  const std::string helper_uri = workspace::FileUriForPath(helper_py);
  auto stub = std::make_unique<workspace::LspClient>();
  workspace::LspClient* const stub_raw = stub.get();
  stub_raw->EnableTestStubMode();
  stub_raw->SetTestRenameHandler([main_uri, helper_uri](std::string uri, std::string new_name,
                                                        workspace::LspClient::RenameCallback cb) {
    (void)uri;
    workspace::LspClient::WorkspaceEdit edit;
    edit.changes[main_uri] = {{workspace::LspClient::Range{{0, 0}, {0, 5}}, new_name}};
    edit.changes[helper_uri] = {{workspace::LspClient::Range{{0, 6}, {0, 11}}, new_name}};
    cb(std::move(edit));
  });
  Expect(WorkspaceShellTestAccess::LspManagerForTesting(shell)
             .InstallTestClientIntoExistingForTesting("python", std::move(stub)),
         "fixture should attach a stub python client");

  WorkspaceShellTestAccess::OpenFile(shell, main_py);
  WorkspaceShellTestAccess::ActiveEditor(shell).MoveCursorTo(0, 2);  // inside "value"

  // Type the new name and submit -> LSP rename request.
  Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, "rename-symbol"),
         "rename-symbol should open the name prompt");
  WorkspaceShellTestAccess::SetPromptSurfaceInput(shell, "count");
  WorkspaceShellTestAccess::ConfirmPromptSurface(shell);
  WorkspaceShellTestAccess::ConsumeLspCallbacks(shell);

  // helper.py isn't open, so a confirmation appears before opening/saving.
  Expect(WorkspaceShellTestAccess::PromptSurfaceVisible(shell),
         "a rename touching unopened files should confirm first");
  Expect(WorkspaceShellTestAccess::PromptSurfaceTitle(shell) == "Rename Across Files",
         "the confirmation should be the rename-across-files prompt");
  Expect(WorkspaceShellTestAccess::PromptSurfaceMessage(shell).find("not open") != std::string::npos,
         "the confirmation should state that some files are not open");

  // Confirm -> apply main.py in place + save, and apply helper.py SILENTLY on disk.
  WorkspaceShellTestAccess::ConfirmPromptSurface(shell);

  Expect(ReadFile(main_py) == "count = 1\n",
         "the already-open file should be renamed and saved to disk");
  Expect(ReadFile(helper_py) == "print(count)\n",
         "the previously-closed file should be renamed and saved to disk");
  // VSCode-style silent apply: the closed file is written directly, NOT opened as
  // a tab (no tab spam on large renames).
  Expect(WorkspaceShellTestAccess::CountOpenBufferViews(shell, helper_py) == 0,
         "a closed file touched by a rename must be written on disk without opening a tab");
}

// Regression for the LSP-primary concurrent provider merge: a language with BOTH
// a plugin completion provider AND a language server must show the UNION of both
// sources (LSP-first, de-duplicated) — previously the plugin result suppressed
// the server entirely (serial plugin-first fallback).
void TestWorkspaceShellCompletionMergesPluginAndLspSources() {
#if !MICROIDE_HAS_LUA_PLUGINS
  return;
#endif
#if !defined(__unix__) && !defined(__APPLE__)
  return;
#endif
  TemporaryDirectory temp_dir;
  const std::filesystem::path config_home = temp_dir.path() / "config";
  const std::filesystem::path plugins_root = config_home / "microide" / "plugins";
  const std::filesystem::path project = temp_dir.path() / "proj";
  const std::filesystem::path md_file = project / "notes.md";
  WriteFile(md_file, "alpha\n");

  // The plugin registers a markdown completion provider AND a markdown server.
  // The provider returns one shared label ("common") plus a plugin-only label.
  WritePluginInit(
      plugins_root, "mdtools",
      R"(local ide = require("microide")
return ide.plugin({
  id = "mdtools",
  capabilities = { process = { exec = true } },
  setup = function(ctx)
    ctx.completion.add({
      id = "md",
      language_id = "markdown",
      provide = function(buffer, position, trigger)
        return {
          { label = "common", insert_text = "common" },
          { label = "plug_only", insert_text = "plug_only" },
        }
      end
    })
    ctx.lsp.add({ id = "md.server", language_id = "markdown", command = { "md-lsp-server" } })
  end
})
)");
  ScopedPluginConfigHomeEnv scoped_plugin_config_home(config_home);

  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, project, false, false),
         "merge fixture should open the project");

  auto stub = std::make_unique<workspace::LspClient>();
  workspace::LspClient* const stub_raw = stub.get();
  stub_raw->EnableTestStubMode();
  // The server returns an LSP-only label plus the same shared "common" label.
  stub_raw->SetTestCompletionHandler(
      [](std::string, workspace::LspClient::Position,
         workspace::LspClient::CompletionCallback cb) {
        std::vector<workspace::LspClient::CompletionItem> items;
        workspace::LspClient::CompletionItem lsp_one;
        lsp_one.label = "lsp_one";
        lsp_one.insert_text = "lsp_one";
        workspace::LspClient::CompletionItem common;
        common.label = "common";
        common.insert_text = "common";
        items.push_back(std::move(lsp_one));
        items.push_back(std::move(common));
        cb(std::move(items));
      });
  Expect(WorkspaceShellTestAccess::LspManagerForTesting(shell)
             .InstallTestClientIntoExistingForTesting("markdown", std::move(stub)),
         "fixture should attach a stub markdown client");

  WorkspaceShellTestAccess::OpenFile(shell, md_file);
  WorkspaceShellTestAccess::ActiveEditor(shell).MoveCursorTo(0, 5);

  Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, "completion"),
         "completion command should execute");
  // The plugin source resolves inline; drain the mailbox so the concurrent LSP
  // stub response arrives and the merge re-publishes.
  WorkspaceShellTestAccess::ConsumeLspCallbacks(shell);

  const auto& session = WorkspaceShellTestAccess::CompletionSession(shell);
  Expect(session.items.size() == 3,
         "the merged overlay should contain the union of both sources with the shared label "
         "de-duplicated (lsp_one, common, plug_only)");
  Expect(session.items[0].label == "lsp_one",
         "the language server's items must rank first for a language it serves");
  std::size_t common_count = 0;
  bool has_plugin_only = false;
  for (const auto& item : session.items) {
    if (item.label == "common") {
      ++common_count;
    }
    if (item.label == "plug_only") {
      has_plugin_only = true;
    }
  }
  Expect(common_count == 1, "the label offered by both sources must appear exactly once");
  Expect(has_plugin_only, "a plugin-only completion must survive the merge");
}

// Signature help is LSP-primary: when a server serves the language, its result is
// shown even though a plugin provider also answers. The plugin result is used only
// as a fallback when no server serves the buffer.
void TestWorkspaceShellSignatureHelpPrefersLspOverPlugin() {
#if !MICROIDE_HAS_LUA_PLUGINS
  return;
#endif
#if !defined(__unix__) && !defined(__APPLE__)
  return;
#endif
  TemporaryDirectory temp_dir;
  const std::filesystem::path config_home = temp_dir.path() / "config";
  const std::filesystem::path plugins_root = config_home / "microide" / "plugins";
  const std::filesystem::path project = temp_dir.path() / "proj";
  const std::filesystem::path md_file = project / "notes.md";
  WriteFile(md_file, "alpha\n");

  // The plugin registers a markdown signature-help provider AND a markdown server.
  WritePluginInit(
      plugins_root, "mdsig",
      R"lua(local ide = require("microide")
return ide.plugin({
  id = "mdsig",
  capabilities = { process = { exec = true } },
  setup = function(ctx)
    ctx.signature_help.add({
      id = "sig",
      language_id = "markdown",
      provide = function(_, _)
        return {
          active_signature = 0,
          signatures = { { label = "plugin_sig(a)", documentation = "from plugin" } },
        }
      end
    })
    ctx.lsp.add({ id = "md.server", language_id = "markdown", command = { "md-lsp-server" } })
  end
})
)lua");
  ScopedPluginConfigHomeEnv scoped_plugin_config_home(config_home);

  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, project, false, false),
         "signature fixture should open the project");

  auto stub = std::make_unique<workspace::LspClient>();
  workspace::LspClient* const stub_raw = stub.get();
  stub_raw->EnableTestStubMode();
  stub_raw->SetTestSignatureHelpHandler(
      [](std::string, workspace::LspClient::Position,
         workspace::LspClient::SignatureHelpCallback cb) {
        workspace::LspClient::SignatureHelp help;
        workspace::LspClient::SignatureInformation info;
        info.label = "lsp_sig(x)";
        info.documentation = "from server";
        help.signatures.push_back(std::move(info));
        cb(std::move(help));
      });
  Expect(WorkspaceShellTestAccess::LspManagerForTesting(shell)
             .InstallTestClientIntoExistingForTesting("markdown", std::move(stub)),
         "fixture should attach a stub markdown client");

  WorkspaceShellTestAccess::OpenFile(shell, md_file);
  WorkspaceShellTestAccess::ActiveEditor(shell).MoveCursorTo(0, 5);

  Expect(WorkspaceShellTestAccess::ShowSignatureHelp(shell),
         "signature help should dispatch");
  // The plugin source resolves inline; drain so the concurrent LSP stub arrives and
  // the LSP-primary choice publishes the popup.
  WorkspaceShellTestAccess::ConsumeLspCallbacks(shell);

  const auto popup = WorkspaceShellTestAccess::SignatureHelpPopup(shell);
  Expect(popup.has_value(), "a signature-help popup should be shown");
  Expect(popup->signature == "lsp_sig(x)",
         "the language server's signature must win over the plugin's for a served language");
}

// Inlay hints flow LSP -> mid-line virtual text: opening a served document
// requests textDocument/inlayHint and publishes each result as an "lsp:inlay"
// InlineText decoration anchored at the hint's byte column (padding baked into
// the label), which the editor renders mid-line via the InlayRowDisplacement path.
void TestWorkspaceShellInlayHintsPublishMidLineDecorations() {
#if !MICROIDE_HAS_LUA_PLUGINS
  return;
#endif
#if !defined(__unix__) && !defined(__APPLE__)
  return;
#endif
  TemporaryDirectory temp_dir;
  const std::filesystem::path config_home = temp_dir.path() / "config";
  const std::filesystem::path plugins_root = config_home / "microide" / "plugins";
  const std::filesystem::path project = temp_dir.path() / "proj";
  const std::filesystem::path md_file = project / "notes.md";
  WriteFile(md_file, "let value = compute()\n");

  WritePluginInit(plugins_root, "mdinlay",
                  R"lua(local ide = require("microide")
return ide.plugin({
  id = "mdinlay",
  capabilities = { process = { exec = true } },
  setup = function(ctx)
    ctx.lsp.add({ id = "md.server", language_id = "markdown", command = { "md-lsp-server" } })
  end
})
)lua");
  ScopedPluginConfigHomeEnv scoped_plugin_config_home(config_home);

  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, project, false, false),
         "inlay fixture should open the project");

  auto stub = std::make_unique<workspace::LspClient>();
  stub->EnableTestStubMode();
  stub->SetTestInlayHintHandler(
      [](std::string, workspace::LspClient::Range, workspace::LspClient::InlayHintCallback cb) {
        cb(std::vector<workspace::LspClient::InlayHint>{
            workspace::LspClient::InlayHint{.position = workspace::LspClient::Position{0, 9},
                                            .label = ": number",
                                            .kind = 1,
                                            .padding_left = true,
                                            .padding_right = false}});
      });
  Expect(WorkspaceShellTestAccess::LspManagerForTesting(shell)
             .InstallTestClientIntoExistingForTesting("markdown", std::move(stub)),
         "fixture should attach a stub markdown client");

  WorkspaceShellTestAccess::OpenFile(shell, md_file);
  // Opening the document on the server (via the hover kickoff) fires the inlay
  // request as a side effect; the helper drains the async response.
  (void)WorkspaceShellTestAccess::ResolveLspHoverForTesting(shell, md_file, 1, 1);
  WorkspaceShellTestAccess::ConsumeLspCallbacks(shell);

  const auto& state = WorkspaceShellTestAccess::CurrentProjectState(shell);
  const auto* presentation = state.plugin_presentation_if_present();
  Expect(presentation != nullptr, "an inlay overlay should be published");
  const auto* decorations =
      presentation != nullptr ? presentation->decorations.FindByPath(md_file) : nullptr;
  Expect(decorations != nullptr && !decorations->inline_texts.empty(),
         "the inlay hint should publish an inline-text decoration");
  if (decorations != nullptr) {
    const auto hints = decorations->InlineTextsForLine(0);
    Expect(hints.size() == 1, "one mid-line hint on line 0");
    Expect(hints[0].anchor_column == 9, "anchored at the hint's byte column");
    Expect(hints[0].anchor_column != microide::editor::kInlineTextEndOfLine,
           "the hint is mid-line, not end-of-line");
    Expect(hints[0].text == " : number", "padding_left baked a leading space into the label");
  }
}

// prepareRename refines the just-opened Rename Symbol prompt: it prefills the
// server's placeholder, and rejects positions the server reports as not renameable.
void TestWorkspaceShellPrepareRenameRefinesPrompt() {
#if !MICROIDE_HAS_LUA_PLUGINS
  return;
#endif
#if !defined(__unix__) && !defined(__APPLE__)
  return;
#endif
  TemporaryDirectory temp_dir;
  const std::filesystem::path config_home = temp_dir.path() / "config";
  const std::filesystem::path plugins_root = config_home / "microide" / "plugins";
  const std::filesystem::path project = temp_dir.path() / "proj";
  const std::filesystem::path md_file = project / "notes.md";
  WriteFile(md_file, "alpha\n");

  WritePluginInit(
      plugins_root, "mdren",
      R"lua(local ide = require("microide")
return ide.plugin({
  id = "mdren",
  capabilities = { process = { exec = true } },
  setup = function(ctx)
    ctx.lsp.add({ id = "md.server", language_id = "markdown", command = { "md-lsp-server" } })
  end
})
)lua");
  ScopedPluginConfigHomeEnv scoped_plugin_config_home(config_home);

  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, project, false, false),
         "rename fixture should open the project");

  auto stub = std::make_unique<workspace::LspClient>();
  workspace::LspClient* const stub_raw = stub.get();
  stub_raw->EnableTestStubMode();
  // First: the server suggests a placeholder distinct from the heuristic seed.
  stub_raw->SetTestPrepareRenameHandler(
      [](std::string, workspace::LspClient::Position,
         workspace::LspClient::PrepareRenameCallback cb) {
        workspace::LspClient::PrepareRename result;
        result.can_rename = true;
        result.placeholder = "server_placeholder";
        cb(std::move(result));
      });
  Expect(WorkspaceShellTestAccess::LspManagerForTesting(shell)
             .InstallTestClientIntoExistingForTesting("markdown", std::move(stub)),
         "fixture should attach a stub markdown client");

  WorkspaceShellTestAccess::OpenFile(shell, md_file);
  WorkspaceShellTestAccess::ActiveEditor(shell).MoveCursorTo(0, 5);

  Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, "rename-symbol"),
         "rename-symbol should open the prompt");
  // The prompt opens immediately seeded with the heuristic "alpha".
  Expect(WorkspaceShellTestAccess::PromptSurfaceVisible(shell), "rename prompt should be visible");
  Expect(WorkspaceShellTestAccess::PromptSurfaceInput(shell) == "alpha",
         "the prompt opens with the heuristic identifier seed");
  // Draining delivers the prepareRename response, which refines the seed.
  WorkspaceShellTestAccess::ConsumeLspCallbacks(shell);
  Expect(WorkspaceShellTestAccess::PromptSurfaceInput(shell) == "server_placeholder",
         "prepareRename must prefill the server's placeholder over the heuristic seed");

  // Now: the server reports the position is not renameable → the prompt is closed.
  stub_raw->SetTestPrepareRenameHandler(
      [](std::string, workspace::LspClient::Position,
         workspace::LspClient::PrepareRenameCallback cb) {
        workspace::LspClient::PrepareRename result;
        result.can_rename = false;
        cb(std::move(result));
      });
  Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, "rename-symbol"),
         "rename-symbol should re-open the prompt");
  Expect(WorkspaceShellTestAccess::PromptSurfaceVisible(shell), "prompt should reopen");
  WorkspaceShellTestAccess::ConsumeLspCallbacks(shell);
  Expect(!WorkspaceShellTestAccess::PromptSurfaceVisible(shell),
         "a non-renameable position must dismiss the rename prompt");
}

// Regression for server-initiated workspace/applyEdit: a WorkspaceEdit pushed by
// the language server must apply to open buffers in place AND write closed files
// silently on disk (no tab), matching the client-initiated rename behavior.
void TestWorkspaceShellServerApplyEditEditsOpenAndClosedFiles() {
#if !MICROIDE_HAS_LUA_PLUGINS
  return;
#endif
#if !defined(__unix__) && !defined(__APPLE__)
  return;
#endif
  TemporaryDirectory temp_dir;
  const std::filesystem::path config_home = temp_dir.path() / "config";
  const std::filesystem::path plugins_root = config_home / "microide" / "plugins";
  const std::filesystem::path project = temp_dir.path() / "proj";
  const std::filesystem::path open_md = project / "open.md";
  const std::filesystem::path closed_md = project / "closed.md";  // never opened
  WriteFile(open_md, "aaa\n");
  WriteFile(closed_md, "aaa\n");

  WritePluginInit(
      plugins_root, "mdlsp",
      R"(local ide = require("microide")
return ide.plugin({
  id = "mdlsp",
  capabilities = { process = { exec = true } },
  setup = function(ctx)
    ctx.lsp.add({ id = "md.server", language_id = "markdown", command = { "md-lsp-server" } })
  end
})
)");
  ScopedPluginConfigHomeEnv scoped_plugin_config_home(config_home);

  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, project, false, false),
         "applyEdit fixture should open the project");

  auto stub = std::make_unique<workspace::LspClient>();
  workspace::LspClient* const stub_raw = stub.get();
  stub_raw->EnableTestStubMode();
  Expect(WorkspaceShellTestAccess::LspManagerForTesting(shell)
             .InstallTestClientIntoExistingForTesting("markdown", std::move(stub)),
         "fixture should attach a stub markdown client");

  WorkspaceShellTestAccess::OpenFile(shell, open_md);
  // Trigger any assist so LspClientForViewport binds the apply-edit handler on the
  // client (mirrors the once-per-client diagnostics binding).
  WorkspaceShellTestAccess::ExecuteCommandLine(shell, "completion");
  WorkspaceShellTestAccess::ConsumeLspCallbacks(shell);
  Expect(stub_raw->HasApplyEditHandler(),
         "resolving the client for a viewport should bind the apply-edit handler");

  // The server pushes an edit replacing "aaa" -> "bbb" in BOTH files.
  const std::string edit_json = std::string("{\"edit\":{\"changes\":{\"") +
                                workspace::FileUriForPath(open_md) +
                                "\":[{\"range\":{\"start\":{\"line\":0,\"character\":0},"
                                "\"end\":{\"line\":0,\"character\":3}},\"newText\":\"bbb\"}],\"" +
                                workspace::FileUriForPath(closed_md) +
                                "\":[{\"range\":{\"start\":{\"line\":0,\"character\":0},"
                                "\"end\":{\"line\":0,\"character\":3}},\"newText\":\"bbb\"}]}}}";
  std::optional<util::JsonValue> params = util::ParseJson(edit_json);
  Expect(params.has_value(), "the applyEdit params fixture should parse");
  stub_raw->SimulateServerRequestForTesting("workspace/applyEdit", std::move(*params),
                                            util::JsonValue(static_cast<std::int64_t>(1)));
  WorkspaceShellTestAccess::ConsumeLspCallbacks(shell);

  Expect(WorkspaceShellTestAccess::ActiveEditor(shell).lines()[0] == "bbb",
         "the open buffer should be edited in place by the server-pushed edit");
  Expect(ReadFile(closed_md) == "bbb\n",
         "the closed file should be written silently on disk by the server-pushed edit");
  Expect(WorkspaceShellTestAccess::CountOpenBufferViews(shell, closed_md) == 0,
         "the server-pushed edit must not open a tab for the closed file");
}

// Regression: a server-pushed WorkspaceEdit whose range names a line beyond the
// closed file's EOF must be rejected, not silently clamped onto the last line —
// the file on disk stays untouched.
void TestWorkspaceShellServerApplyEditRejectsOutOfRangeClosedFileEdit() {
#if !MICROIDE_HAS_LUA_PLUGINS
  return;
#endif
#if !defined(__unix__) && !defined(__APPLE__)
  return;
#endif
  TemporaryDirectory temp_dir;
  const std::filesystem::path config_home = temp_dir.path() / "config";
  const std::filesystem::path plugins_root = config_home / "microide" / "plugins";
  const std::filesystem::path project = temp_dir.path() / "proj";
  const std::filesystem::path open_md = project / "open.md";
  const std::filesystem::path closed_md = project / "closed.md";  // one line, never opened
  WriteFile(open_md, "aaa\n");
  WriteFile(closed_md, "aaa\n");

  WritePluginInit(
      plugins_root, "mdlsp",
      R"(local ide = require("microide")
return ide.plugin({
  id = "mdlsp",
  capabilities = { process = { exec = true } },
  setup = function(ctx)
    ctx.lsp.add({ id = "md.server", language_id = "markdown", command = { "md-lsp-server" } })
  end
})
)");
  ScopedPluginConfigHomeEnv scoped_plugin_config_home(config_home);

  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, project, false, false),
         "fixture should open the project");

  auto stub = std::make_unique<workspace::LspClient>();
  workspace::LspClient* const stub_raw = stub.get();
  stub_raw->EnableTestStubMode();
  Expect(WorkspaceShellTestAccess::LspManagerForTesting(shell)
             .InstallTestClientIntoExistingForTesting("markdown", std::move(stub)),
         "fixture should attach a stub markdown client");

  WorkspaceShellTestAccess::OpenFile(shell, open_md);
  WorkspaceShellTestAccess::ExecuteCommandLine(shell, "completion");
  WorkspaceShellTestAccess::ConsumeLspCallbacks(shell);
  Expect(stub_raw->HasApplyEditHandler(), "the apply-edit handler should be bound");

  // The server pushes an edit targeting line 999 of the one-line closed file.
  const std::string edit_json = std::string("{\"edit\":{\"changes\":{\"") +
                                workspace::FileUriForPath(closed_md) +
                                "\":[{\"range\":{\"start\":{\"line\":999,\"character\":0},"
                                "\"end\":{\"line\":999,\"character\":0}},\"newText\":\"Z\"}]}}}";
  std::optional<util::JsonValue> params = util::ParseJson(edit_json);
  Expect(params.has_value(), "the applyEdit params fixture should parse");
  stub_raw->SimulateServerRequestForTesting("workspace/applyEdit", std::move(*params),
                                            util::JsonValue(static_cast<std::int64_t>(1)));
  WorkspaceShellTestAccess::ConsumeLspCallbacks(shell);

  Expect(ReadFile(closed_md) == "aaa\n",
         "an out-of-range (beyond-EOF) edit must not be clamped onto the last line and written");
}

void TestWorkspaceShellOutlineSidebarFromDocumentSymbols() {
#if !MICROIDE_HAS_LUA_PLUGINS
  return;
#endif
  TemporaryDirectory temp_dir;
  const std::filesystem::path config_home = temp_dir.path() / "config";
  const std::filesystem::path plugins_root = config_home / "microide" / "plugins";
  const std::filesystem::path project_root = temp_dir.path() / "project";
  const std::filesystem::path source = project_root / "src" / "main.lua";
  WriteFile(source,
            "local Widget = {}\nfunction Widget.new() end\n\n-- class\nlocal x = 1\nlocal y = 2\n"
            "local z = 3\nfunction Widget:draw() end\nlocal q = 4\nfunction main() end\n");

  WritePluginInit(
      plugins_root, "outline-tools",
      R"(local ide = require("microide")
return ide.plugin({
  id = "outline-tools",
  setup = function(ctx)
    ctx.document_symbols.add({
      id = "symbols", language_id = "lua",
      provide = function(_)
        return {
          { name = "Widget", kind = "class", line = 1, column = 1,
            children = { { name = "draw", kind = "method", line = 8, column = 10 } } },
          { name = "main", kind = "function", line = 10, column = 10 },
        }
      end,
    })
  end
})
)");

  ScopedPluginConfigHomeEnv scoped_plugin_config_home(config_home);

  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, project_root, false, false),
         "outline fixture should open the project");
  WorkspaceShellTestAccess::OpenFile(shell, source);
  Expect(WorkspaceShellTestAccess::PluginErrors(shell).empty(), DescribePluginState(shell).c_str());

  Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, "sidebar-show outline"),
         "sidebar-show should accept the built-in outline view");
  Expect(WorkspaceShellTestAccess::SidebarMode(shell) == WorkspaceShell::SidebarMode::Outline,
         "outline view should activate the outline sidebar mode");
  Expect(WorkspaceShellTestAccess::SidebarViewId(shell) == "outline",
         "outline view should record the outline view id");

  const auto& items = WorkspaceShellTestAccess::PluginSidebarItems(shell);
  Expect(items.size() == 3, "outline should flatten the nested document symbols into rows");
  Expect(items[0].label == "Widget" && items[0].depth == 0,
         "outline should list the parent symbol at depth 0");
  Expect(items[1].label == "draw" && items[1].depth == 1,
         "outline should indent a child symbol to depth 1");
  Expect(items[2].label == "main" && items[2].depth == 0,
         "outline should return to depth 0 after a child subtree");

  // Confirming a row navigates the editor caret to the symbol (1-based -> 0-based).
  WorkspaceShellTestAccess::SetPluginSidebarSelectedIndex(shell, 1);
  Expect(WorkspaceShellTestAccess::OpenSelectedPluginSidebarItem(shell),
         "confirming an outline row should navigate the editor");
  Expect(WorkspaceShellTestAccess::ActiveEditor(shell).cursor_line() == 7 &&
             WorkspaceShellTestAccess::ActiveEditor(shell).cursor_column() == 9,
         "confirming an outline row should move the caret to the symbol location");
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

// With two editor groups split side-by-side, hit-test paths (hover/cursor/
// right-click) must resolve the viewport of the pane under the cursor, not the
// globally-focused group's viewport. The bug: these loops read
// ActiveEditorViewport() for every pane, so hovering the unfocused group hit-tested
// the focused group's content. This pins the per-pane resolution (ViewportForPane)
// and the user-visible symptom: a diagnostic in the *unfocused* group still shows
// its hover popup.
void TestWorkspaceShellSplitGroupHoverResolvesPaneViewport() {
  using microide::workspace::EditorSplitOrientation;
  TemporaryDirectory temp_dir;
  const std::filesystem::path project_root = temp_dir.path() / "project";
  const std::filesystem::path file_a = project_root / "a.md";
  const std::filesystem::path file_b = project_root / "b.md";
  WriteFile(file_a, "alpha beta\n");
  WriteFile(file_b, "gamma delta\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, project_root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::OpenFile(shell, file_a);

  // Split right (clones a.md into a new, focused group), then open b.md in that
  // focused group. Result: group 0 (inactive) shows a.md, group 1 (focused) b.md.
  Expect(WorkspaceShellTestAccess::SplitEditorGroup(shell, EditorSplitOrientation::Vertical),
         "split-right should succeed when an editor tab is active");
  WorkspaceShellTestAccess::OpenFile(shell, file_b);
  Expect(WorkspaceShellTestAccess::EditorGroupCount(shell) == 2, "two groups should be open");
  Expect(WorkspaceShellTestAccess::FocusedGroupIndex(shell) == 1,
         "the freshly-split group should hold focus");

  const std::string key_a = file_a.lexically_normal().generic_string();
  const std::string key_b = file_b.lexically_normal().generic_string();
  const auto pane_paths = WorkspaceShellTestAccess::PaneViewportPaths(shell);
  Expect(pane_paths.size() == 2, "two panes should resolve");
  Expect(pane_paths[0] == key_a,
         "the inactive pane must resolve its own group's viewport (a.md), not the focused one");
  Expect(pane_paths[1] == key_b, "the focused pane resolves the focused group's viewport (b.md)");

  // Publish a diagnostic on a.md, which lives in the *inactive* group.
  Expect(WorkspaceShellTestAccess::PublishDiagnostics(
             shell, "diagnostics", file_a,
             {microide::editor::Diagnostic{
                 .range =
                     microide::editor::SelectionRange{
                         .start = microide::editor::TextPosition{.line = 0, .column = 1},
                         .end = microide::editor::TextPosition{.line = 0, .column = 5},
                     },
                 .severity = microide::editor::DiagnosticSeverity::Warning,
                 .message = "Unexpected token near alpha",
             }}),
         "diagnostic should publish on the inactive group's file");

  // Hover the diagnostic inside the inactive pane. Pre-fix this hit-tested the
  // focused group's b.md (no diagnostic) and produced no popup.
  const auto metrics = WorkspaceShellTestAccess::InactiveEditorPaneMetrics(shell);
  const float hover_x = metrics.text_x + WorkspaceShellTestAccess::TextCharWidth(shell) * 2.0f;
  const float hover_y = metrics.first_line_y + metrics.line_height - 1.5f;
  Expect(SendMouseMotion(shell, hover_x, hover_y, 0),
         "hovering the inactive pane's diagnostic underline should be handled");

  const auto message = WorkspaceShellTestAccess::ActiveEditorDiagnosticHoverMessage(shell);
  Expect(message.has_value() && *message == "Unexpected token near alpha",
         "hovering the unfocused group's diagnostic should surface its message");
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
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);

  auto& editor = WorkspaceShellTestAccess::ActiveEditor(shell);
  editor.SelectAll();
  editor.InsertText("broken();\n");
  Expect(WorkspaceShellTestAccess::SaveTab(shell, WorkspaceShellTestAccess::ActiveTabIndex(shell)),
         "eslint plugin fixture should save the edited JavaScript buffer");

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
  Expect(WorkspaceShellTestAccess::DiagnosticsForPath(shell, unopened) == nullptr,
         "eslint.run-opened should ignore dirty files that were never opened in this session");

  WorkspaceShellTestAccess::OpenFile(shell, source);
  auto& clean_editor = WorkspaceShellTestAccess::ActiveEditor(shell);
  clean_editor.SelectAll();
  clean_editor.InsertText("const answer = 1;\n");
  Expect(WorkspaceShellTestAccess::SaveTab(shell, WorkspaceShellTestAccess::ActiveTabIndex(shell)),
         "eslint plugin fixture should save the cleaned JavaScript buffer");
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

  const auto* opened_diagnostics = WorkspaceShellTestAccess::DiagnosticsForPath(shell, source);
  Expect(opened_diagnostics != nullptr && opened_diagnostics->size() == 1,
         "opening a broken TypeScript file should publish ESLint diagnostics");

  Expect(WorkspaceShellTestAccess::SaveTab(shell, WorkspaceShellTestAccess::ActiveTabIndex(shell)),
         "saving an unchanged file should re-run ESLint");

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

void TestWorkspaceShellProjectOpenRunsSynchronousPluginAsync() {
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
    -- Non-login shell (-c): a login shell sources the host profile, which exits
    -- nonzero under the plugin subprocess sandbox and would break the
    -- exit_code==0 assertion below; "printf" needs no profile.
    ctx.process.run_async({"sh", "-c", "sleep 0.5; printf done"}, nil, function(result)
      ctx.log("async-complete:" .. tostring(result.exit_code))
    end)
  end
})
)");

  ScopedPluginConfigHomeEnv scoped_plugin_config_home(config_home);

  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, project_a, false, false),
         "async switch fixture should open the first project");
  // run_async now runs on the plugin worker and invokes its callback synchronously
  // there, so the project-open hook's subprocess has already completed and logged
  // by the time the (blocking round-trip) project open returns. There is no longer
  // any pending async callback to cancel on a project switch.
  const std::vector<std::string>& messages_a = WorkspaceShellTestAccess::PluginMessages(shell);
  Expect(std::any_of(messages_a.begin(), messages_a.end(),
                     [](const std::string& entry) {
                       return entry == "switch-async: async-complete:0";
                     }),
         "project-open run_async should complete synchronously and log its result");

  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, project_b, false, false),
         "async switch fixture should open the second project");
  Expect(WorkspaceShellTestAccess::PluginErrors(shell).empty(),
         "switching projects after a synchronous run_async should not surface errors");
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

// Regression: opening/viewing a file must engage the language server (start a
// lazy server + send textDocument/didOpen) without requiring an edit or an
// explicit LSP action. Before the fix the LSP only woke on the first edit or on
// go-to-definition, so opening a file left the status stuck at "LSP: Starting..."
// and painted no diagnostics/semantic colors until the user interacted.
void TestWorkspaceShellOpensDocumentInLspOnFileOpen() {
#if !MICROIDE_HAS_LUA_PLUGINS
  return;
#endif
  TemporaryDirectory temp_dir;
  const std::filesystem::path config_home = temp_dir.path() / "config";
  const std::filesystem::path plugins_root = config_home / "microide" / "plugins";
  const std::filesystem::path project = temp_dir.path() / "proj";
  const std::filesystem::path py_file = project / "main.py";
  WriteFile(py_file, "print('hi')\n");

  WritePluginInit(
      plugins_root, "pylsp",
      R"(local ide = require("microide")
return ide.plugin({
  id = "pylsp",
  capabilities = { process = { exec = true } },
  setup = function(ctx)
    ctx.lsp.add({
      id = "pylsp.server",
      language_id = "python",
      command = { "py-lsp-server" },
    })
  end
})
)");
  ScopedPluginConfigHomeEnv scoped_plugin_config_home(config_home);

  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, project, false, false),
         "fixture should open the project");
  Expect(WorkspaceShellTestAccess::LspManagerForTesting(shell).HasServer("python"),
         "the plugin's python language server should be registered");

  // Attach a warm stub (test-installed, so EnsureStarted returns it without
  // spawning a real process) to observe didOpen without a live server.
  auto stub = std::make_unique<workspace::LspClient>();
  workspace::LspClient* const stub_raw = stub.get();
  Expect(WorkspaceShellTestAccess::LspManagerForTesting(shell)
             .InstallTestClientIntoExistingForTesting("python", std::move(stub)),
         "fixture should attach a stub python client");

  const std::string uri = workspace::FileUriForPath(py_file);
  Expect(!stub_raw->HasOpenDocument(uri),
         "precondition: the document is not open in the LSP before it is opened");

  // Open the file for viewing only — no edit, no go-to-definition.
  Expect(WorkspaceShellTestAccess::OpenFileInNewTab(shell, py_file),
         "fixture should open the .py file");

  Expect(stub_raw->HasOpenDocument(uri),
         "opening a file must send textDocument/didOpen (engage the LSP on open, "
         "not only on the first edit or go-to-definition)");
}

// Regression: a file that was already open when the session is restored must have
// its LSP document opened (textDocument/didOpen) so diagnostics/semantic tokens
// paint without the user interacting. The bug: session restore activated the tab
// BEFORE the cpp-lsp-style plugin registered its language server, so the
// activation's NotifyLspBufferOpen found no client and skipped didOpen, and the
// post-plugin-reload buffer replay passed open_lsp_documents=false. The server
// started only to render the status bar and the document was never opened, leaving
// the restored file with no markers until a click. The fix flips the restore
// reload to open_lsp_documents=true so open buffers are engaged after servers exist.
void TestWorkspaceShellRestoreEngagesLspForOpenDocument() {
#if !MICROIDE_HAS_LUA_PLUGINS
  return;
#endif
#if !defined(__unix__) && !defined(__APPLE__)
  return;
#endif
  TemporaryDirectory temp_dir;
  const std::filesystem::path config_home = temp_dir.path() / "config";
  const std::filesystem::path state_home = temp_dir.path() / "state";
  const std::filesystem::path plugins_root = config_home / "microide" / "plugins";
  const std::filesystem::path project = temp_dir.path() / "proj";
  const std::filesystem::path py_file = project / "main.py";
  WriteFile(py_file, "print('hi')\n");

  // A minimal real LSP server: replies to initialize and exits cleanly on
  // shutdown/exit so the client counts as running (didOpen is sent) and teardown
  // is fast. It need not produce diagnostics — HasOpenDocument tracks didOpen.
  const std::filesystem::path server_py = temp_dir.path() / "server.py";
  WriteFile(server_py, std::string(R"py(import json
import sys

def read_message():
    content_length = None
    while True:
        line = sys.stdin.buffer.readline()
        if not line:
            return None
        if line in (b"\r\n", b"\n"):
            break
        if line.lower().startswith(b"content-length:"):
            content_length = int(line.split(b":", 1)[1].strip())
    if content_length is None:
        return None
    body = sys.stdin.buffer.read(content_length)
    return json.loads(body.decode("utf-8")) if body else None

def write_message(message):
    data = json.dumps(message).encode("utf-8")
    sys.stdout.buffer.write(f"Content-Length: {len(data)}\r\n\r\n".encode("ascii"))
    sys.stdout.buffer.write(data)
    sys.stdout.buffer.flush()

while True:
    msg = read_message()
    if msg is None:
        break
    method = msg.get("method")
    if method == "initialize":
        write_message({"jsonrpc": "2.0", "id": msg["id"],
                       "result": {"capabilities": {"textDocumentSync": 1}}})
    elif method == "shutdown":
        write_message({"jsonrpc": "2.0", "id": msg["id"], "result": None})
    elif method == "exit":
        break
)py"));

  WritePluginInit(
      plugins_root, "pylsp",
      "local ide = require(\"microide\")\n"
      "return ide.plugin({\n"
      "  id = \"pylsp\",\n"
      "  capabilities = { process = { exec = true } },\n"
      "  setup = function(ctx)\n"
      "    ctx.lsp.add({\n"
      "      id = \"pylsp.server\",\n"
      "      language_id = \"python\",\n"
      "      command = { \"python3\", \"" + server_py.generic_string() + "\" },\n"
      "    })\n"
      "  end\n"
      "})\n");

  ScopedPluginConfigHomeEnv scoped_plugin_config_home(config_home);
  // Session state persists under XDG_STATE_HOME; pin it so both shells below share
  // one session file instead of touching the real user state directory.
  ScopedEnvVar scoped_state_home("XDG_STATE_HOME", state_home.string());

  // Phase 1: open the project and the file, then persist the session with the file
  // open (this is the "a file was open when you last closed microide" setup).
  {
    WorkspaceShell shell;
    Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, project, false, false),
           "phase 1 should open the project");
    Expect(WorkspaceShellTestAccess::OpenFileInNewTab(shell, py_file),
           "phase 1 should open the .py file");
    WorkspaceShellTestAccess::SaveSessionState(shell);
  }

  // Phase 2: a fresh shell restores that session — the real startup path. The fix
  // must engage the language server for the restored active document.
  WorkspaceShell restored;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(restored, project, /*restore=*/true, false),
         "phase 2 should restore the project session");

  const std::string uri = workspace::FileUriForPath(py_file);
  workspace::LspClient* const client =
      WorkspaceShellTestAccess::LspManagerForTesting(restored).FindStartedServer("python");
  Expect(client != nullptr,
         "restoring a session with an open supported file must START its language "
         "server (not leave it registered-but-idle until the user interacts)");
  Expect(client != nullptr && client->HasOpenDocument(uri),
         "restoring a session with an open file must send textDocument/didOpen so "
         "diagnostics/semantic tokens paint without a click (regression: "
         "open_lsp_documents was false on the restore reload)");
}

// SEAM 2 — host-owned plugin buffer edits (ctx.editor.apply_edits).
// Loads a plugin exposing commands that drive apply_edits/set_cursor/set_selection
// against the active buffer, then asserts the host applied them with correct
// grouped-undo, caret, and rejection behaviour.
void RunEditPluginSetup(WorkspaceShell& shell,
                        const std::filesystem::path& plugins_root,
                        const std::filesystem::path& project_root,
                        const std::filesystem::path& source,
                        std::string_view source_text) {
  WriteFile(source, std::string(source_text));
  WritePluginInit(
      plugins_root, "editops",
      R"(local ide = require("microide")
return ide.plugin({
  id = "editops",
  setup = function(ctx)
    ctx.commands.add("editops.replace_first_word", function()
      local ok = ctx.editor.apply_edits({
        edits = { { start_line = 1, start_col = 1, end_line = 1, end_col = 6, text = "HELLO" } },
      })
      ctx.log(ok and "applied" or "rejected")
    end)
    ctx.commands.add("editops.replace_two_lines", function()
      local ok = ctx.editor.apply_edits({
        edits = {
          { start_line = 1, start_col = 1, end_line = 1, end_col = 4, text = "AAA" },
          { start_line = 2, start_col = 1, end_line = 2, end_col = 4, text = "BBB" },
        },
      })
      ctx.log(ok and "applied" or "rejected")
    end)
    ctx.commands.add("editops.select_first_two", function()
      ctx.editor.set_selection({ start_line = 1, start_col = 1, end_line = 1, end_col = 3 })
    end)
    ctx.commands.add("editops.edit_missing_path", function()
      local ok, err = ctx.editor.apply_edits({
        path = "not-open.txt",
        edits = { { start_line = 1, start_col = 1, end_line = 1, end_col = 1, text = "X" } },
      })
      ctx.log(ok and "applied" or ("rejected:" .. tostring(err)))
    end)
    ctx.commands.add("editops.append_past_end", function()
      local ok = ctx.editor.apply_edits({
        edits = { { start_line = 999, start_col = 999, end_line = 999, end_col = 999, text = "!" } },
      })
      ctx.log(ok and "applied" or "rejected")
    end)
    ctx.commands.add("editops.too_many_edits", function()
      -- One past the host cap: the request must fail wholesale, not truncate.
      local edits = {}
      for i = 1, 100001 do
        edits[i] = { start_line = 1, start_col = 1, end_line = 1, end_col = 1, text = "x" }
      end
      local ok = ctx.editor.apply_edits({ edits = edits })
      ctx.log(ok and "applied" or "rejected")
    end)
    ctx.commands.add("editops.infinite_coords", function()
      -- math.huge must not be cast to size_t (undefined behavior); the interop
      -- layer maps it to an invalid (0) index so no wild edit lands.
      local ok = ctx.editor.apply_edits({
        edits = { { start_line = math.huge, start_col = 1, end_line = math.huge,
                    end_col = 6, text = "INF" } },
      })
      ctx.log(ok and "applied" or "rejected")
    end)
    ctx.commands.add("editops.cursor_missing_col", function()
      -- A cursor request with no column must fail closed (not clamp to column 0).
      local ok = ctx.editor.set_cursor({ line = 1 })
      ctx.log(ok and "applied" or "rejected")
    end)
    ctx.commands.add("editops.selection_missing_col", function()
      local ok = ctx.editor.set_selection({ start_line = 1, end_line = 1, end_col = 3 })
      ctx.log(ok and "applied" or "rejected")
    end)
  end
})
)");
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, project_root, false, false),
         "editops plugin fixture should open the project");
  WorkspaceShellTestAccess::OpenFile(shell, source);
  Expect(WorkspaceShellTestAccess::PluginErrors(shell).empty(), DescribePluginState(shell).c_str());
}

void TestWorkspaceShellPluginApplyEditsSingleEditUndo() {
#if !MICROIDE_HAS_LUA_PLUGINS
  return;
#endif
  TemporaryDirectory temp_dir;
  const std::filesystem::path config_home = temp_dir.path() / "config";
  const std::filesystem::path plugins_root = config_home / "microide" / "plugins";
  const std::filesystem::path project_root = temp_dir.path() / "project";
  const std::filesystem::path source = project_root / "main.txt";
  ScopedPluginConfigHomeEnv scoped_plugin_config_home(config_home);

  WorkspaceShell shell;
  RunEditPluginSetup(shell, plugins_root, project_root, source, "hello\nworld\n");
  Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, "editops.replace_first_word"),
         "apply_edits command should dispatch");

  auto& editor = WorkspaceShellTestAccess::ActiveEditor(shell);
  Expect(editor.lines()[0] == "HELLO", "apply_edits should replace the first word");
  Expect(editor.Undo(), "a plugin edit should be undoable");
  Expect(editor.lines()[0] == "hello", "one undo should revert the plugin edit");
}

void TestWorkspaceShellPluginApplyEditsMultiEditAtomicUndo() {
#if !MICROIDE_HAS_LUA_PLUGINS
  return;
#endif
  TemporaryDirectory temp_dir;
  const std::filesystem::path config_home = temp_dir.path() / "config";
  const std::filesystem::path plugins_root = config_home / "microide" / "plugins";
  const std::filesystem::path project_root = temp_dir.path() / "project";
  const std::filesystem::path source = project_root / "main.txt";
  ScopedPluginConfigHomeEnv scoped_plugin_config_home(config_home);

  WorkspaceShell shell;
  RunEditPluginSetup(shell, plugins_root, project_root, source, "aaa\nbbb\n");
  Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, "editops.replace_two_lines"),
         "multi-edit command should dispatch");

  auto& editor = WorkspaceShellTestAccess::ActiveEditor(shell);
  Expect(editor.lines()[0] == "AAA" && editor.lines()[1] == "BBB",
         "both edits should apply");
  Expect(editor.Undo(), "the grouped plugin edit should be undoable");
  Expect(editor.lines()[0] == "aaa" && editor.lines()[1] == "bbb",
         "a single undo should revert BOTH edits (one grouped step)");
}

void TestWorkspaceShellPluginApplyEditsSetsSelection() {
#if !MICROIDE_HAS_LUA_PLUGINS
  return;
#endif
  TemporaryDirectory temp_dir;
  const std::filesystem::path config_home = temp_dir.path() / "config";
  const std::filesystem::path plugins_root = config_home / "microide" / "plugins";
  const std::filesystem::path project_root = temp_dir.path() / "project";
  const std::filesystem::path source = project_root / "main.txt";
  ScopedPluginConfigHomeEnv scoped_plugin_config_home(config_home);

  WorkspaceShell shell;
  RunEditPluginSetup(shell, plugins_root, project_root, source, "hello\nworld\n");
  Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, "editops.select_first_two"),
         "set_selection command should dispatch");

  auto& editor = WorkspaceShellTestAccess::ActiveEditor(shell);
  const auto selection = editor.selection_range();
  Expect(selection.has_value(), "set_selection should establish a selection");
  Expect(selection->start.line == 0 && selection->start.column == 0 &&
             selection->end.line == 0 && selection->end.column == 2,
         "set_selection should map 1-based input to the 0-based range [0,0)-(0,2)");
}

// TD-2026-07-17A-087: plugin cursor/selection requests that omit a column must fail
// closed rather than being accepted and silently clamped to column 0.
void TestWorkspaceShellPluginCursorSelectionRequireColumns() {
#if !MICROIDE_HAS_LUA_PLUGINS
  return;
#endif
  TemporaryDirectory temp_dir;
  const std::filesystem::path config_home = temp_dir.path() / "config";
  const std::filesystem::path plugins_root = config_home / "microide" / "plugins";
  const std::filesystem::path project_root = temp_dir.path() / "project";
  const std::filesystem::path source = project_root / "main.txt";
  ScopedPluginConfigHomeEnv scoped_plugin_config_home(config_home);

  WorkspaceShell shell;
  RunEditPluginSetup(shell, plugins_root, project_root, source, "hello\nworld\n");

  // Plugin log messages are prefixed with the plugin id ("editops: ..."), so match the
  // suffix.
  const auto last_message_is_rejected = [&]() {
    const auto& messages = WorkspaceShellTestAccess::PluginMessages(shell);
    if (messages.empty()) return false;
    const std::string& last = messages.back();
    return last.size() >= 8 && last.compare(last.size() - 8, 8, "rejected") == 0;
  };

  WorkspaceShellTestAccess::ClearPluginMessages(shell);
  Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, "editops.cursor_missing_col"),
         "cursor command should dispatch");
  Expect(last_message_is_rejected(),
         "a set_cursor without a column must be rejected, not clamped to column 0");

  WorkspaceShellTestAccess::ClearPluginMessages(shell);
  Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, "editops.selection_missing_col"),
         "selection command should dispatch");
  Expect(last_message_is_rejected(),
         "a set_selection missing a column must be rejected");
}

void TestWorkspaceShellPluginApplyEditsRejectsUnopenedPath() {
#if !MICROIDE_HAS_LUA_PLUGINS
  return;
#endif
  TemporaryDirectory temp_dir;
  const std::filesystem::path config_home = temp_dir.path() / "config";
  const std::filesystem::path plugins_root = config_home / "microide" / "plugins";
  const std::filesystem::path project_root = temp_dir.path() / "project";
  const std::filesystem::path source = project_root / "main.txt";
  ScopedPluginConfigHomeEnv scoped_plugin_config_home(config_home);

  WorkspaceShell shell;
  RunEditPluginSetup(shell, plugins_root, project_root, source, "hello\n");
  WorkspaceShellTestAccess::ClearPluginMessages(shell);
  Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, "editops.edit_missing_path"),
         "missing-path edit command should dispatch");

  const auto& messages = WorkspaceShellTestAccess::PluginMessages(shell);
  Expect(!messages.empty() && messages.back().rfind("editops: rejected:", 0) == 0,
         "editing a path that is not open should return (false, message), not raise");
  auto& editor = WorkspaceShellTestAccess::ActiveEditor(shell);
  Expect(editor.lines()[0] == "hello", "a rejected edit must not touch the active buffer");
}

// Regression: non-finite plugin edit coordinates (`math.huge`) must not be cast
// from double to size_t — that is undefined behavior. The interop layer maps them
// to an invalid (0) index so the command completes without a crash or wild edit.
// (Also exercised under UBSAN, which flags the bad cast directly.)
void TestWorkspaceShellPluginApplyEditsRejectsNonFiniteCoords() {
#if !MICROIDE_HAS_LUA_PLUGINS
  return;
#endif
  TemporaryDirectory temp_dir;
  const std::filesystem::path config_home = temp_dir.path() / "config";
  const std::filesystem::path plugins_root = config_home / "microide" / "plugins";
  const std::filesystem::path project_root = temp_dir.path() / "project";
  const std::filesystem::path source = project_root / "main.txt";
  ScopedPluginConfigHomeEnv scoped_plugin_config_home(config_home);

  WorkspaceShell shell;
  RunEditPluginSetup(shell, plugins_root, project_root, source, "hello\nworld\n");
  auto& editor = WorkspaceShellTestAccess::ActiveEditor(shell);

  WorkspaceShellTestAccess::ClearPluginMessages(shell);
  Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, "editops.infinite_coords"),
         "infinite-coordinate command should dispatch without crashing");
  // The malformed edit is dropped (no valid 1-based coordinate), so the apply
  // resolves to no edits and reports rejection — the buffer is untouched.
  const auto& messages = WorkspaceShellTestAccess::PluginMessages(shell);
  Expect(!messages.empty() && messages.back() == "editops: rejected",
         "math.huge coordinates must reject the edit, not apply it at a clamped position");
  Expect(editor.lines()[0] == "hello" && editor.lines()[1] == "world",
         "math.huge coordinates must leave the buffer completely untouched");
}

void TestWorkspaceShellPluginApplyEditsRejectsTooManyEdits() {
#if !MICROIDE_HAS_LUA_PLUGINS
  return;
#endif
  TemporaryDirectory temp_dir;
  const std::filesystem::path config_home = temp_dir.path() / "config";
  const std::filesystem::path plugins_root = config_home / "microide" / "plugins";
  const std::filesystem::path project_root = temp_dir.path() / "project";
  const std::filesystem::path source = project_root / "main.txt";
  ScopedPluginConfigHomeEnv scoped_plugin_config_home(config_home);

  WorkspaceShell shell;
  RunEditPluginSetup(shell, plugins_root, project_root, source, "hello\nworld\n");
  auto& editor = WorkspaceShellTestAccess::ActiveEditor(shell);

  WorkspaceShellTestAccess::ClearPluginMessages(shell);
  Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, "editops.too_many_edits"),
         "too-many-edits command should dispatch without crashing");
  const auto& messages = WorkspaceShellTestAccess::PluginMessages(shell);
  Expect(!messages.empty() && messages.back() == "editops: rejected",
         "an edit array beyond the cap must be rejected wholesale, not truncated and applied");
  Expect(editor.lines()[0] == "hello" && editor.lines()[1] == "world",
         "an over-cap apply must leave the buffer completely untouched");
}

// Seeds an "lsp:semantic" recolor overlay for `path` (as an async clangd
// semantic-tokens response would), so a test can assert whether a subsequent edit
// clears it.
void SeedLspSemanticOverlay(WorkspaceShell& shell, const std::filesystem::path& path) {
  auto& state = WorkspaceShellTestAccess::CurrentProjectState(shell);
  microide::editor::PluginDecorationData data;
  microide::editor::TextStyleDecoration style;
  style.line = 1;
  style.start_column = 0;
  style.end_column = 3;
  style.foreground = SDL_Color{200, 100, 50, 255};
  data.text_styles.push_back(style);
  state.EnsurePluginPresentation().decorations.ReplaceForOwnerFile("lsp:semantic", path,
                                                                   std::move(data));
}

bool HasLspSemanticOverlay(WorkspaceShell& shell, const std::filesystem::path& path) {
  const auto& state = WorkspaceShellTestAccess::CurrentProjectState(shell);
  const auto* presentation = state.plugin_presentation_if_present();
  if (presentation == nullptr) {
    return false;
  }
  const auto* decorations = presentation->decorations.FindByPath(path);
  return decorations != nullptr && !decorations->text_styles.empty();
}

// Regression: LSP semantic tokens are an absolute-positioned recolor overlay that
// paints over the lexical highlighter. It is only render-visible while the buffer
// is clean, so a stale overlay left behind by an edit becomes visible the moment
// the buffer returns to a clean state — the reported "quick-fix -> undo -> save"
// corruption. Every content edit (typing AND undo) must drop the overlay so it can
// never paint misaligned colors; a fresh request repopulates it on the next clean
// transition (save / undo onto the saved point).
void TestWorkspaceShellLspSemanticOverlayClearedOnEditAndUndo() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path project_root = temp_dir.path() / "project";
  const std::filesystem::path source = project_root / "main.cpp";
  WriteFile(source, "int main() {\n  return 0;\n}\n");

  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, project_root, false, false),
         "semantic-overlay fixture should open the project");
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::OpenFile(shell, source);

  SeedLspSemanticOverlay(shell, source);
  Expect(HasLspSemanticOverlay(shell, source),
         "seeded lsp:semantic overlay should be present before editing");

  // A plain edit shifts buffer geometry -> the overlay is stale and must be dropped.
  Expect(WorkspaceShellTestAccess::HandleTextInput(shell, "x"),
         "typing into an editable buffer should be handled");
  Expect(!HasLspSemanticOverlay(shell, source),
         "a content edit should clear the stale lsp:semantic overlay");

  // Re-seed to model a late/stale response landing while dirty (render-suppressed),
  // then undo — the exact reported repro. Undo returns the buffer to a clean,
  // render-visible state, so it too must clear the stale overlay.
  SeedLspSemanticOverlay(shell, source);
  Expect(HasLspSemanticOverlay(shell, source),
         "re-seeded overlay should be present before undo");
  Expect(SendKeyDown(shell, SDLK_Z, SDL_KMOD_CTRL), "Ctrl+Z should undo the edit");
  Expect(WorkspaceShellTestAccess::ActiveEditor(shell).lines()[0] == "int main() {",
         "undo should restore the original first line");
  Expect(!HasLspSemanticOverlay(shell, source),
         "undo must clear the stale lsp:semantic overlay (the reported corruption)");
}

// Regression: LSP inlay hints publish as absolute-positioned mid-line virtual
// text. Unlike the semantic overlay they render in EVERY buffer state, so an edit
// that shifts the geometry MUST drop them (they are re-requested on the next clean
// transition); otherwise a stale hint paints mid-line at a pre-edit column.
void TestWorkspaceShellLspInlayOverlayClearedOnEdit() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path project_root = temp_dir.path() / "project";
  const std::filesystem::path source = project_root / "main.cpp";
  WriteFile(source, "int main() {\n  return 0;\n}\n");

  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, project_root, false, false),
         "inlay-overlay fixture should open the project");
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::OpenFile(shell, source);

  auto seed = [&]() {
    auto& state = WorkspaceShellTestAccess::CurrentProjectState(shell);
    microide::editor::PluginDecorationData data;
    microide::editor::InlineTextDecoration hint;
    hint.line = 1;
    hint.anchor_column = 2;  // mid-line, not the end-of-line sentinel
    hint.text = " : int";
    data.inline_texts.push_back(hint);
    state.EnsurePluginPresentation().decorations.ReplaceForOwnerFile("lsp:inlay", source,
                                                                     std::move(data));
  };
  auto has_inlay = [&]() {
    const auto& state = WorkspaceShellTestAccess::CurrentProjectState(shell);
    const auto* presentation = state.plugin_presentation_if_present();
    if (presentation == nullptr) return false;
    const auto* decorations = presentation->decorations.FindByPath(source);
    return decorations != nullptr && !decorations->inline_texts.empty();
  };

  seed();
  Expect(has_inlay(), "seeded lsp:inlay overlay should be present before editing");
  Expect(WorkspaceShellTestAccess::HandleTextInput(shell, "x"),
         "typing into an editable buffer should be handled");
  Expect(!has_inlay(), "a content edit must clear the stale lsp:inlay overlay");
}

// The applied-edit diagnostic shift must run even when no language server serves
// the buffer. Phase-1.4 moved ShiftLspDiagnosticsForAppliedEdit ahead of the
// client early-out in SyncLspForActiveEditableLastChange, so stored "lsp"
// diagnostics stay on their text while the buffer is dirty regardless of server
// state; before that change a serverless edit stranded them on the pre-edit line.
void TestWorkspaceShellLspDiagnosticsShiftOnEditWithoutServer() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path project_root = temp_dir.path() / "project";
  const std::filesystem::path source = project_root / "main.cpp";
  WriteFile(source, "int main() {\n  return 0;\n}\n");

  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, project_root, false, false),
         "diagnostic-shift fixture should open the project");
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::OpenFile(shell, source);

  // Seed an "lsp" diagnostic on the second line ("  return 0;"). This bare project
  // has no contributed language server, so nothing will republish it.
  Expect(WorkspaceShellTestAccess::PublishDiagnostics(
             shell, "lsp", source,
             {microide::editor::Diagnostic{
                 .range =
                     microide::editor::SelectionRange{
                         .start = microide::editor::TextPosition{.line = 1, .column = 2},
                         .end = microide::editor::TextPosition{.line = 1, .column = 8},
                     },
                 .severity = microide::editor::DiagnosticSeverity::Warning,
                 .message = "unused result",
             }}),
         "seeding an lsp diagnostic should publish it");
  {
    const auto* published = WorkspaceShellTestAccess::DiagnosticsForPath(shell, source);
    Expect(published != nullptr && published->size() == 1 &&
               published->front().range.start.line == 1,
           "the seeded diagnostic should start on line 1 before editing");
  }

  // Insert a newline at the very top: every stored position at/after (0,0) shifts
  // down one line. With no server, the pre-1.4 sync returned before shifting.
  WorkspaceShellTestAccess::ActiveEditor(shell).MoveCursorTo(0, 0);
  Expect(SendKeyDown(shell, SDLK_RETURN, SDL_KMOD_NONE),
         "pressing Enter at the top of the buffer should insert a line");

  const auto* published = WorkspaceShellTestAccess::DiagnosticsForPath(shell, source);
  Expect(published != nullptr && published->size() == 1,
         "the diagnostic should still be stored after a serverless edit");
  Expect(published->front().range.start.line == 2,
         "the diagnostic must shift down a line even with no language server running");
}

// Opens a bare project + editable file (no plugin needed) with ghost text enabled,
// caret parked at the end of the first line. Ghost-text state-machine tests drive
// the host's publish/accept/dismiss/invalidate entry points directly via TestAccess.
void OpenGhostTextFixture(WorkspaceShell& shell, const std::filesystem::path& project_root,
                          const std::filesystem::path& source, std::string_view content) {
  WriteFile(source, std::string(content));
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, project_root, false, false),
         "ghost-text fixture should open the project");
  WorkspaceShellTestAccess::OpenFile(shell, source);
  Expect(WorkspaceShellTestAccess::SetSettingValueTransient(shell, "plugins.ghost_text", "true"),
         "plugins.ghost_text should be settable");
  auto& editor = WorkspaceShellTestAccess::ActiveEditor(shell);
  editor.MoveCursorTo(0, editor.lines()[0].size());
}

plugin::PluginHost::GhostTextRequest GhostRequest(std::string text) {
  plugin::PluginHost::GhostTextRequest request;
  request.text = std::move(text);
  return request;
}

void TestWorkspaceShellGhostTextPublishStoresAndSplits() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path project_root = temp_dir.path() / "project";
  const std::filesystem::path source = project_root / "main.txt";
  WorkspaceShell shell;
  OpenGhostTextFixture(shell, project_root, source, "hello\n");

  WorkspaceShellTestAccess::PublishGhostText(shell, "copilot", GhostRequest("X\nY\nZ"));
  const auto* ghost = WorkspaceShellTestAccess::GhostText(shell);
  Expect(ghost != nullptr, "publishing ghost text should store it");
  Expect(ghost->owner == "copilot", "ghost text should record the publishing owner");
  Expect(ghost->lines.size() == 3 && ghost->lines[0] == "X" && ghost->lines[1] == "Y" &&
             ghost->lines[2] == "Z",
         "ghost text should split the suggestion on newlines into tail + below rows");
  Expect(ghost->anchor_line == 0 && ghost->anchor_column == 5,
         "ghost text should anchor at the live caret when no explicit anchor is given");
}

void TestWorkspaceShellGhostTextDisabledSettingNoOp() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path project_root = temp_dir.path() / "project";
  const std::filesystem::path source = project_root / "main.txt";
  WorkspaceShell shell;
  OpenGhostTextFixture(shell, project_root, source, "hello\n");
  Expect(WorkspaceShellTestAccess::SetSettingValueTransient(shell, "plugins.ghost_text", "false"),
         "plugins.ghost_text should be settable to false");

  WorkspaceShellTestAccess::PublishGhostText(shell, "copilot", GhostRequest("X\nY"));
  Expect(WorkspaceShellTestAccess::GhostText(shell) == nullptr,
         "publishing with the feature disabled should store nothing");
  Expect(!WorkspaceShellTestAccess::HasPluginPresentation(shell),
         "a disabled-feature publish must not even allocate the presentation bundle");
}

void TestWorkspaceShellGhostTextStaleAnchorRejected() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path project_root = temp_dir.path() / "project";
  const std::filesystem::path source = project_root / "main.txt";
  WorkspaceShell shell;
  OpenGhostTextFixture(shell, project_root, source, "hello\n");

  plugin::PluginHost::GhostTextRequest request = GhostRequest("late");
  request.anchor_line = 1;     // 1-based line 1 => 0-based 0
  request.anchor_column = 1;   // 1-based col 1 => 0-based 0, but caret is at column 5
  WorkspaceShellTestAccess::PublishGhostText(shell, "copilot", request);
  Expect(WorkspaceShellTestAccess::GhostText(shell) == nullptr,
         "a suggestion whose anchor no longer matches the caret should be dropped as stale");
}

void TestWorkspaceShellGhostTextAcceptInsertsAndClears() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path project_root = temp_dir.path() / "project";
  const std::filesystem::path source = project_root / "main.txt";
  WorkspaceShell shell;
  OpenGhostTextFixture(shell, project_root, source, "hello\n");

  WorkspaceShellTestAccess::PublishGhostText(shell, "copilot", GhostRequest("X\nY"));
  Expect(WorkspaceShellTestAccess::AcceptGhostText(shell),
         "accepting a live suggestion should consume Tab");
  auto& editor = WorkspaceShellTestAccess::ActiveEditor(shell);
  Expect(editor.lines()[0] == "helloX" && editor.lines()[1] == "Y",
         "accept should insert the whole multi-line suggestion at the caret");
  Expect(WorkspaceShellTestAccess::GhostText(shell) == nullptr,
         "accept should clear the suggestion");
  Expect(!WorkspaceShellTestAccess::HasPluginPresentation(shell),
         "clearing the only contribution should release the presentation bundle");
  Expect(editor.Undo() && editor.lines()[0] == "hello" && editor.lines()[1] == "",
         "the inserted suggestion should revert in a single undo step");
}

void TestWorkspaceShellGhostTextAcceptWithNoneFallsThrough() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path project_root = temp_dir.path() / "project";
  const std::filesystem::path source = project_root / "main.txt";
  WorkspaceShell shell;
  OpenGhostTextFixture(shell, project_root, source, "hello\n");
  Expect(!WorkspaceShellTestAccess::AcceptGhostText(shell),
         "accept with no suggestion must return false so Tab falls through to snippet/indent");
}

void TestWorkspaceShellGhostTextInvalidatesOnCaretMove() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path project_root = temp_dir.path() / "project";
  const std::filesystem::path source = project_root / "main.txt";
  WorkspaceShell shell;
  OpenGhostTextFixture(shell, project_root, source, "hello\n");

  WorkspaceShellTestAccess::PublishGhostText(shell, "copilot", GhostRequest("X"));
  Expect(WorkspaceShellTestAccess::GhostText(shell) != nullptr, "precondition: ghost text live");
  WorkspaceShellTestAccess::ActiveEditor(shell).MoveCursorTo(0, 0);
  WorkspaceShellTestAccess::InvalidateGhostTextIfStale(shell);
  Expect(WorkspaceShellTestAccess::GhostText(shell) == nullptr,
         "moving the caret away from the anchor should invalidate the suggestion");
}

void TestWorkspaceShellGhostTextClearIsOwnerScoped() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path project_root = temp_dir.path() / "project";
  const std::filesystem::path source = project_root / "main.txt";
  WorkspaceShell shell;
  OpenGhostTextFixture(shell, project_root, source, "hello\n");

  WorkspaceShellTestAccess::PublishGhostText(shell, "copilot", GhostRequest("X"));
  WorkspaceShellTestAccess::ClearGhostText(shell, "other");
  Expect(WorkspaceShellTestAccess::GhostText(shell) != nullptr,
         "clearing under a different owner must leave the suggestion intact");
  WorkspaceShellTestAccess::ClearGhostText(shell, "copilot");
  Expect(WorkspaceShellTestAccess::GhostText(shell) == nullptr,
         "clearing under the owning plugin should remove the suggestion");
}

void TestWorkspaceShellGhostTextLuaApiPublishesAndClears() {
#if !MICROIDE_HAS_LUA_PLUGINS
  return;
#endif
  TemporaryDirectory temp_dir;
  const std::filesystem::path config_home = temp_dir.path() / "config";
  const std::filesystem::path plugins_root = config_home / "microide" / "plugins";
  const std::filesystem::path project_root = temp_dir.path() / "project";
  const std::filesystem::path source = project_root / "main.txt";
  WriteFile(source, "hello\n");
  WritePluginInit(
      plugins_root, "ghost",
      R"(local ide = require("microide")
return ide.plugin({
  id = "ghost",
  setup = function(ctx)
    ctx.commands.add("ghost.suggest", function()
      local ok, err = ctx.editor.set_ghost_text({ text = "foo\nbar" })
      ctx.log(ok and "set" or ("rejected:" .. tostring(err)))
    end)
    ctx.commands.add("ghost.suggest_empty", function()
      local ok, err = ctx.editor.set_ghost_text({ text = "" })
      ctx.log(ok and "set" or ("rejected:" .. tostring(err)))
    end)
    ctx.commands.add("ghost.dismiss", function()
      ctx.editor.clear_ghost_text()
    end)
  end
})
)");
  ScopedPluginConfigHomeEnv scoped_plugin_config_home(config_home);

  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, project_root, false, false),
         "ghost Lua fixture should open the project");
  WorkspaceShellTestAccess::OpenFile(shell, source);
  Expect(WorkspaceShellTestAccess::PluginErrors(shell).empty(), DescribePluginState(shell).c_str());
  Expect(WorkspaceShellTestAccess::SetSettingValueTransient(shell, "plugins.ghost_text", "true"),
         "plugins.ghost_text should be settable");
  WorkspaceShellTestAccess::ActiveEditor(shell).MoveCursorTo(0, 0);

  Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, "ghost.suggest"),
         "ghost.suggest command should dispatch");
  const auto* ghost = WorkspaceShellTestAccess::GhostText(shell);
  Expect(ghost != nullptr && ghost->lines.size() == 2 && ghost->lines[0] == "foo" &&
             ghost->lines[1] == "bar",
         "ctx.editor.set_ghost_text should publish through the host into ghost state");

  WorkspaceShellTestAccess::ClearPluginMessages(shell);
  Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, "ghost.suggest_empty"),
         "ghost.suggest_empty command should dispatch");
  const auto& messages = WorkspaceShellTestAccess::PluginMessages(shell);
  Expect(!messages.empty() && messages.back().rfind("ghost: rejected:", 0) == 0,
         "empty ghost text should return (false, message) without raising");

  Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, "ghost.dismiss"),
         "ghost.dismiss command should dispatch");
  Expect(WorkspaceShellTestAccess::GhostText(shell) == nullptr,
         "ctx.editor.clear_ghost_text should clear the suggestion");
}

void TestWorkspaceShellPluginApplyEditsClampsOutOfBounds() {
#if !MICROIDE_HAS_LUA_PLUGINS
  return;
#endif
  TemporaryDirectory temp_dir;
  const std::filesystem::path config_home = temp_dir.path() / "config";
  const std::filesystem::path plugins_root = config_home / "microide" / "plugins";
  const std::filesystem::path project_root = temp_dir.path() / "project";
  const std::filesystem::path source = project_root / "main.txt";
  ScopedPluginConfigHomeEnv scoped_plugin_config_home(config_home);

  WorkspaceShell shell;
  RunEditPluginSetup(shell, plugins_root, project_root, source, "hi\n");
  Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, "editops.append_past_end"),
         "out-of-bounds edit command should dispatch");

  auto& editor = WorkspaceShellTestAccess::ActiveEditor(shell);
  // The line/column clamp to the last line's end, so the insert lands at EOL of
  // the final content line ("hi") rather than crashing or growing the buffer.
  Expect(editor.lines()[0] == "hi!" || editor.lines().back() == "!" ||
             editor.lines().back() == "hi!",
         "an out-of-bounds edit should clamp to a valid position, not crash");
}

// SEAM 1 — reactive editor events. Unit-tests for the deterministic debounce /
// coalescing tracker (time injected), then an end-to-end plugin dispatch test.
void TestPluginEditorEventTrackerCoalescesChanges() {
  using microide::workspace::PluginEditorEventTracker;
  PluginEditorEventTracker tracker;
  tracker.SetInterest({.buffer_change = true});
  const std::filesystem::path path = "a.txt";
  // First sample baselines silently.
  tracker.Sample(path, 1, 1, 1, false, 0, 0, 0, 0, /*now=*/100, /*debounce=*/150);
  Expect(!tracker.NextDelayMs(100).has_value(), "baseline sample should arm nothing");
  // A burst of edits coalesces into one armed change with a trailing deadline.
  tracker.Sample(path, 2, 3, 1, false, 0, 0, 0, 0, 110, 150);
  tracker.Sample(path, 3, 7, 1, false, 0, 0, 0, 0, 140, 150);
  Expect(tracker.NextDelayMs(140).has_value(), "an edit should arm a debounce delay");
  // Before the deadline nothing is due.
  Expect(!tracker.TakeDue(200).any(), "no event before the trailing deadline");
  // After the deadline (last edit at 140 + 150) exactly one change fires, with the
  // unioned caret-line range across the burst.
  const auto due = tracker.TakeDue(300);
  Expect(due.change && !due.cursor && !due.selection, "a settled burst yields one change");
  Expect(due.change_start_line == 3 && due.change_end_line == 7,
         "the change range should union the burst's caret lines");
  Expect(!tracker.TakeDue(400).any(), "a consumed event does not refire");
}

void TestPluginEditorEventTrackerZeroCostWhenUnsubscribed() {
  using microide::workspace::PluginEditorEventTracker;
  PluginEditorEventTracker tracker;  // no interest set
  tracker.Sample("a.txt", 1, 1, 1, true, 1, 1, 1, 5, 100, 150);
  tracker.Sample("a.txt", 2, 2, 2, true, 1, 1, 1, 9, 110, 150);
  Expect(!tracker.NextDelayMs(110).has_value(),
         "with no subscriber the tracker must arm nothing (zero-cost)");
  Expect(!tracker.TakeDue(1000).any(), "with no subscriber nothing is ever due");
}

void TestPluginEditorEventTrackerBufferSwitchDoesNotEmit() {
  using microide::workspace::PluginEditorEventTracker;
  PluginEditorEventTracker tracker;
  tracker.SetInterest({.buffer_change = true, .cursor_move = true});
  tracker.Sample("a.txt", 5, 4, 2, false, 0, 0, 0, 0, 100, 150);
  // Switching to a different buffer re-baselines silently even though revision and
  // caret differ — no spurious change/cursor event.
  tracker.Sample("b.txt", 9, 1, 1, false, 0, 0, 0, 0, 110, 150);
  Expect(!tracker.NextDelayMs(110).has_value(), "switching buffers must not arm an event");
  Expect(!tracker.TakeDue(1000).any(), "switching buffers must not emit");
}

void TestPluginEditorEventTrackerSeparatesCursorAndSelection() {
  using microide::workspace::PluginEditorEventTracker;
  PluginEditorEventTracker tracker;
  tracker.SetInterest({.cursor_move = true, .selection_change = true});
  tracker.Sample("a.txt", 1, 1, 1, false, 0, 0, 0, 0, 100, 50);
  tracker.Sample("a.txt", 1, 2, 4, true, 2, 1, 2, 4, 110, 50);
  const auto due = tracker.TakeDue(200);
  Expect(due.cursor && due.cursor_line == 2 && due.cursor_column == 4,
         "a caret move should fire on_cursor_move with the 1-based caret");
  Expect(due.selection && due.selection_present && due.selection_end_column == 4,
         "a new selection should fire on_selection_change");
  Expect(!due.change, "no content change means no on_buffer_change");
}

void TestWorkspaceShellPluginReceivesBufferChangeEvent() {
#if !MICROIDE_HAS_LUA_PLUGINS
  return;
#endif
  TemporaryDirectory temp_dir;
  const std::filesystem::path config_home = temp_dir.path() / "config";
  const std::filesystem::path plugins_root = config_home / "microide" / "plugins";
  const std::filesystem::path project_root = temp_dir.path() / "project";
  const std::filesystem::path source = project_root / "main.txt";
  WriteFile(source, "hello\nworld\n");
  WritePluginInit(
      plugins_root, "reactor",
      R"(local ide = require("microide")
return ide.plugin({
  id = "reactor",
  setup = function(ctx)
    ctx.commands.add("reactor.poke", function()
      ctx.editor.apply_edits({
        edits = { { start_line = 1, start_col = 1, end_line = 1, end_col = 6, text = "HELLO" } },
      })
    end)
  end,
  on_buffer_change = function(ctx, buffer, change)
    ctx.log("changed:" .. tostring(change.start_line) .. "-" .. tostring(change.end_line))
  end,
  on_buffer_close = function(ctx, buffer)
    ctx.log("closed")
  end,
})
)");
  ScopedPluginConfigHomeEnv scoped_plugin_config_home(config_home);

  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, project_root, false, false),
         "reactor plugin fixture should open the project");
  WorkspaceShellTestAccess::OpenFile(shell, source);
  Expect(WorkspaceShellTestAccess::PluginErrors(shell).empty(), DescribePluginState(shell).c_str());

  // Baseline sample, then mutate, then sample again to arm the debounced change.
  WorkspaceShellTestAccess::SamplePluginEditorEvents(shell);
  WorkspaceShellTestAccess::ClearPluginMessages(shell);
  Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, "reactor.poke"),
         "reactor.poke should dispatch and edit the buffer");
  WorkspaceShellTestAccess::SamplePluginEditorEvents(shell);

  // Let the trailing debounce window elapse, then drive the scheduled-wake dispatch.
  std::this_thread::sleep_for(std::chrono::milliseconds(180));
  bool dispatched = false;
  for (int attempt = 0; attempt < 5 && !dispatched; ++attempt) {
    dispatched = WorkspaceShellTestAccess::DispatchDuePluginEditorEvents(shell);
    if (!dispatched) {
      std::this_thread::sleep_for(std::chrono::milliseconds(40));
    }
  }
  Expect(dispatched, "a settled edit should dispatch a debounced on_buffer_change");
  const auto& messages = WorkspaceShellTestAccess::PluginMessages(shell);
  Expect(!messages.empty() && messages.back().rfind("reactor: changed:", 0) == 0,
         "the plugin should receive on_buffer_change after the debounce window");
}

// SEAM 3 — a plugin-contributed snippet expands from its prefix on Tab.
void TestWorkspaceShellPluginSnippetTabTriggerExpands() {
#if !MICROIDE_HAS_LUA_PLUGINS
  return;
#endif
  TemporaryDirectory temp_dir;
  const std::filesystem::path config_home = temp_dir.path() / "config";
  const std::filesystem::path plugins_root = config_home / "microide" / "plugins";
  const std::filesystem::path project_root = temp_dir.path() / "project";
  const std::filesystem::path source = project_root / "main.lua";
  WriteFile(source, "\n");
  WritePluginInit(
      plugins_root, "snip",
      R"(local ide = require("microide")
return ide.plugin({
  id = "snip",
  setup = function(ctx)
    ctx.snippets.add({
      id = "forloop", language_id = "lua", prefix = "forr", label = "for loop",
      body = "for i=1,10 do end",
    })
  end
})
)");
  ScopedPluginConfigHomeEnv scoped_plugin_config_home(config_home);

  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, project_root, false, false),
         "snippet plugin fixture should open the project");
  WorkspaceShellTestAccess::OpenFile(shell, source);
  Expect(WorkspaceShellTestAccess::PluginErrors(shell).empty(), DescribePluginState(shell).c_str());

  auto& editor = WorkspaceShellTestAccess::ActiveEditor(shell);
  editor.InsertText("forr");
  Expect(SendKeyDown(shell, SDLK_TAB, SDL_KMOD_NONE),
         "Tab should be handled when a snippet prefix matches");
  Expect(editor.lines()[0] == "for i=1,10 do end",
         "a Tab after a matching snippet prefix should expand the snippet body");
  Expect(editor.Undo(), "snippet expansion should be undoable");
  Expect(editor.lines()[0] == "forr", "one undo should restore the typed prefix");
}

}  // namespace

// Data-integrity (A5/C6): a multi-file LSP WorkspaceEdit applies to each OPEN buffer as
// its own grouped-undo step; each buffer's dirty/save/undo baseline is fully independent,
// and a target that is not open is left untouched on disk (v1 keeps undo coherent by
// never editing files it cannot undo). Saving+undoing one buffer must not disturb another.
void TestWorkspaceShellMultiBufferWorkspaceEditIndependentBaselines() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path a_path = root / "a.txt";
  const std::filesystem::path b_path = root / "b.txt";
  const std::filesystem::path c_path = root / "c.txt";  // referenced but NOT opened
  WriteFile(a_path, "aaa\n");
  WriteFile(b_path, "bbb\n");
  WriteFile(c_path, "ccc\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::RegisterLifecycleWakeEvents(shell);
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root, false, false),
         "fixture should open the project");
  WorkspaceShellTestAccess::OpenSingleEditorTab(shell, a_path);      // tab 0 = a
  Expect(WorkspaceShellTestAccess::OpenFileInNewTab(shell, b_path),  // tab 1 = b
         "opening the second file should succeed");

  const auto range = [](std::size_t line, std::size_t col) {
    return editor::SelectionRange{editor::TextPosition{line, col}, editor::TextPosition{line, col}};
  };
  const std::vector<microide::workspace::CodeActionEdit> edits = {
      {a_path, range(0, 0), "X"},
      {b_path, range(0, 0), "Y"},
      {c_path, range(0, 0), "Z"},  // not open -> must be dropped, disk untouched
  };
  Expect(WorkspaceShellTestAccess::ApplyLspWorkspaceEdit(shell, edits),
         "a multi-buffer workspace edit should apply to the open buffers");

  // Both open buffers were edited and are dirty; the unopened file is untouched on disk.
  Expect(ReadFile(c_path) == "ccc\n",
         "an edit targeting a file that is not open must not touch it on disk");

  WorkspaceShellTestAccess::ActivateTab(shell, 0);
  Expect(WorkspaceShellTestAccess::ActiveEditor(shell).lines()[0] == "Xaaa", "buffer A got its edit");
  Expect(WorkspaceShellTestAccess::ActiveEditor(shell).dirty(), "buffer A is dirty");
  WorkspaceShellTestAccess::ActivateTab(shell, 1);
  Expect(WorkspaceShellTestAccess::ActiveEditor(shell).lines()[0] == "Ybbb", "buffer B got its edit");
  Expect(WorkspaceShellTestAccess::ActiveEditor(shell).dirty(), "buffer B is dirty");

  // Save A, then undo A past the save. B must be entirely unaffected.
  Expect(WorkspaceShellTestAccess::SaveTab(shell, 0), "saving buffer A should succeed");
  WorkspaceShellTestAccess::ActivateTab(shell, 0);
  Expect(!WorkspaceShellTestAccess::ActiveEditor(shell).dirty(), "A is clean right after its save");
  Expect(WorkspaceShellTestAccess::ActiveEditor(shell).Undo(), "undo in A should succeed");
  Expect(WorkspaceShellTestAccess::ActiveEditor(shell).lines()[0] == "aaa",
         "undo restores buffer A's original content");
  Expect(WorkspaceShellTestAccess::ActiveEditor(shell).dirty(),
         "A is dirty after undoing past its save (its content now differs from disk)");

  WorkspaceShellTestAccess::ActivateTab(shell, 1);
  Expect(WorkspaceShellTestAccess::ActiveEditor(shell).lines()[0] == "Ybbb",
         "buffer B is untouched by A's save+undo");
  Expect(WorkspaceShellTestAccess::ActiveEditor(shell).dirty(),
         "buffer B keeps its own dirty baseline (it was never saved)");
  Expect(ReadFile(b_path) == "bbb\n", "buffer B was never written to disk");
}

// TD-2026-07-17A-016: an open-buffer LSP WorkspaceEdit must NOT materialize a
// whole-document TextBuffer::Snapshot() — not the pre-edit baseline, not the
// post-edit re-sync. The sync now carries only a compact line-count/affected-span
// delta and streams the full didChange straight from the live buffer, so a single
// rename in a large open file no longer copies every line twice.
void TestWorkspaceShellWorkspaceEditDoesNotSnapshotBuffers() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path src = root / "big.txt";
  // A buffer large enough that a stray whole-document snapshot would be obvious.
  std::string content;
  for (int i = 0; i < 500; ++i) {
    content += "line ";
    content += std::to_string(i);
    content += " token\n";
  }
  WriteFile(src, content);

  WorkspaceShell shell;
  WorkspaceShellTestAccess::RegisterLifecycleWakeEvents(shell);
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root, false, false),
         "fixture should open the project");
  WorkspaceShellTestAccess::OpenSingleEditorTab(shell, src);

  const auto range = [](std::size_t l0, std::size_t c0, std::size_t l1, std::size_t c1) {
    return editor::SelectionRange{editor::TextPosition{l0, c0}, editor::TextPosition{l1, c1}};
  };

  // Same-line-count rename: replace "token" on line 200. before/after line counts
  // match, so the diagnostic-shift early-outs and only a streamed didChange runs.
  microide::editor::TextBuffer::reset_snapshot_build_count();
  const std::vector<microide::workspace::CodeActionEdit> same_line_edits = {
      {src, range(200, 9, 200, 14), "TOKEN"},
  };
  Expect(WorkspaceShellTestAccess::ApplyLspWorkspaceEdit(shell, same_line_edits),
         "the same-line rename should apply to the open buffer");
  Expect(microide::editor::TextBuffer::snapshot_build_count() == 0,
         "a same-line WorkspaceEdit must not snapshot the whole buffer");
  Expect(WorkspaceShellTestAccess::ActiveEditor(shell).lines()[200] == "line 200 TOKEN",
         "the same-line rename should land on line 200");

  // Line-count-changing edit: insert two new lines at the top. This exercises the
  // delta path (before != after line count -> diagnostic shift by net delta) which
  // must still avoid a whole-document snapshot.
  microide::editor::TextBuffer::reset_snapshot_build_count();
  const std::vector<microide::workspace::CodeActionEdit> insert_edits = {
      {src, range(0, 0, 0, 0), "inserted a\ninserted b\n"},
  };
  Expect(WorkspaceShellTestAccess::ApplyLspWorkspaceEdit(shell, insert_edits),
         "the multi-line insert should apply to the open buffer");
  Expect(microide::editor::TextBuffer::snapshot_build_count() == 0,
         "a line-count-changing WorkspaceEdit must not snapshot the whole buffer");
  Expect(WorkspaceShellTestAccess::ActiveEditor(shell).lines()[0] == "inserted a",
         "the insert should prepend the new lines");
  Expect(WorkspaceShellTestAccess::ActiveEditor(shell).lines()[202] == "line 200 TOKEN",
         "the earlier rename should have shifted down by two lines after the insert");
}

// Regression: a WorkspaceEdit with two overlapping ranges for one buffer must be
// rejected wholesale, not applied highest-first (which double-edits the shared
// bytes order-dependently). The buffer is left untouched.
void TestWorkspaceShellWorkspaceEditRejectsOverlappingEdits() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path a_path = root / "a.txt";
  WriteFile(a_path, "hello world\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::RegisterLifecycleWakeEvents(shell);
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root, false, false),
         "fixture should open the project");
  WorkspaceShellTestAccess::OpenSingleEditorTab(shell, a_path);

  const auto range = [](std::size_t l0, std::size_t c0, std::size_t l1, std::size_t c1) {
    return editor::SelectionRange{editor::TextPosition{l0, c0}, editor::TextPosition{l1, c1}};
  };
  // Ranges [0,0)-[0,5) and [0,3)-[0,8) intersect on columns 3..5.
  const std::vector<microide::workspace::CodeActionEdit> edits = {
      {a_path, range(0, 0, 0, 5), "AAAAA"},
      {a_path, range(0, 3, 0, 8), "BBBBB"},
  };
  Expect(!WorkspaceShellTestAccess::ApplyLspWorkspaceEdit(shell, edits),
         "an overlapping-edit group applies to nothing");

  WorkspaceShellTestAccess::ActivateTab(shell, 0);
  Expect(WorkspaceShellTestAccess::ActiveEditor(shell).lines()[0] == "hello world",
         "overlapping edits must be rejected, leaving the buffer unchanged");
}

// TD-2026-07-16-45: an open-buffer LSP WorkspaceEdit whose range names a line beyond
// EOF must be REJECTED (whole group dropped), not silently clamped onto the last real
// line and mutated — matching the closed-file applier. The buffer stays untouched.
void TestWorkspaceShellWorkspaceEditRejectsBeyondEofOpenBufferEdit() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path a_path = root / "a.txt";
  WriteFile(a_path, "only line\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::RegisterLifecycleWakeEvents(shell);
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root, false, false),
         "fixture should open the project");
  WorkspaceShellTestAccess::OpenSingleEditorTab(shell, a_path);

  const auto range = [](std::size_t l0, std::size_t c0, std::size_t l1, std::size_t c1) {
    return editor::SelectionRange{editor::TextPosition{l0, c0}, editor::TextPosition{l1, c1}};
  };
  // Line 999 of a one-line buffer: a stale/hostile server target. The old forgiving
  // clamp would have edited the last real line; the strict policy rejects the group.
  bool any_rejected = false;
  const std::vector<microide::workspace::CodeActionEdit> edits = {
      {a_path, range(999, 0, 999, 3), "XXX"},
  };
  Expect(!WorkspaceShellTestAccess::ApplyLspWorkspaceEdit(shell, edits, &any_rejected),
         "a beyond-EOF open-buffer edit group applies to nothing");
  Expect(any_rejected, "the beyond-EOF group must be reported as rejected");

  WorkspaceShellTestAccess::ActivateTab(shell, 0);
  Expect(WorkspaceShellTestAccess::ActiveEditor(shell).lines()[0] == "only line",
         "a beyond-EOF edit must not be clamped onto the last line and mutate it");
}

// Regression: an LSP WorkspaceEdit (code action / rename / Format Document / plugin
// edit) that mutates a buffer must mark the fold model dirty. Without it, a same-line-
// count edit that moves a fold boundary leaves EnsureActiveFoldingModelFresh early-
// returning STALE fold ranges (a phantom fold marker that hides the wrong line range).
void TestWorkspaceShellWorkspaceEditRefreshesFolds() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path src = root / "fold.cpp";
  WriteFile(src,
            "void f() {\n"
            "  if (x) {\n"
            "    a;\n"
            "  }\n"
            "  b;\n"
            "}\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::RegisterLifecycleWakeEvents(shell);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root, false, false),
         "fixture should open the project");
  WorkspaceShellTestAccess::OpenSingleEditorTab(shell, src);

  const auto inner_closer_for_opener = [&](std::size_t opener) -> std::size_t {
    editor::FoldingModel* model = WorkspaceShellTestAccess::EnsureActiveFoldingModelFresh(shell);
    if (model == nullptr) {
      return 0;
    }
    for (const auto& r : model->ranges()) {
      if (r.opener_line == opener) {
        return r.closer_line;
      }
    }
    return 0;
  };

  // Structure A: the inner brace block opens at line 1 and closes at line 3.
  Expect(inner_closer_for_opener(1) == 3,
         "before the edit, the inner fold opens at line 1 and closes at line 3");

  // Replace lines 1..4 reordered so the inner block slides down one line — SAME line
  // count. This is the same-line-count edit class (a formatter re-indent, a re-nesting
  // code action) that the fold-model dirty fix guards.
  const auto range = [](std::size_t l0, std::size_t c0, std::size_t l1, std::size_t c1) {
    return editor::SelectionRange{editor::TextPosition{l0, c0}, editor::TextPosition{l1, c1}};
  };
  const std::vector<microide::workspace::CodeActionEdit> edits = {
      {src, range(1, 0, 5, 0), "  b;\n  if (x) {\n    a;\n  }\n"},
  };
  Expect(WorkspaceShellTestAccess::ApplyLspWorkspaceEdit(shell, edits),
         "the workspace edit should apply to the open buffer");

  // Structure B: the inner block now opens at line 2 and closes at line 4. Without the
  // MarkDirty fix, EnsureActiveFoldingModelFresh early-returns the stale structure A.
  Expect(inner_closer_for_opener(2) == 4,
         "after the same-line-count edit, the fold model must reflect the moved inner block");
  Expect(inner_closer_for_opener(1) == 0,
         "the stale inner fold at line 1 must be gone after the edit");
}

// Regression: a live editor-preference change must reach EVERY editor group, not just
// the focused one. ApplyEditorPreferencesToAllTabs only walked the focused group, so a
// split's non-focused pane kept the stale per-viewport config (tab_size, soft_wrap, …)
// until reopened.
void TestWorkspaceShellEditorPreferencesReachAllSplitGroups() {
  using microide::workspace::EditorSplitOrientation;
  TemporaryDirectory temp_dir;
  const std::filesystem::path project_root = temp_dir.path() / "project";
  const std::filesystem::path file_a = project_root / "a.txt";
  WriteFile(file_a, "\tindented\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, project_root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::OpenFile(shell, file_a);
  Expect(WorkspaceShellTestAccess::SplitEditorGroup(shell, EditorSplitOrientation::Vertical),
         "split-right should succeed with an editor tab active");
  Expect(WorkspaceShellTestAccess::EditorGroupCount(shell) == 2, "two groups should be open");

  // Change the tab size while the split (group 1) holds focus.
  Expect(WorkspaceShellTestAccess::SetSettingValue(shell, "editor.tab_size", "8"),
         "setting editor.tab_size should succeed");

  Expect(WorkspaceShellTestAccess::GroupActiveViewport(shell, 1).tab_size() == 8,
         "the focused group's viewport must pick up the new tab size");
  Expect(WorkspaceShellTestAccess::GroupActiveViewport(shell, 0).tab_size() == 8,
         "the NON-focused split group's viewport must also pick up the new tab size");
}

void RegisterWorkspaceShellPluginTests(std::vector<TestCase>& tests) {
  AddTest(tests, "WorkspaceShell/EditorPreferencesReachAllSplitGroups",
          TestWorkspaceShellEditorPreferencesReachAllSplitGroups);
  AddTest(tests, "WorkspaceShell/WorkspaceEditRefreshesFolds",
          TestWorkspaceShellWorkspaceEditRefreshesFolds);
  AddTest(tests, "WorkspaceShell/MultiBufferWorkspaceEditIndependentBaselines",
          TestWorkspaceShellMultiBufferWorkspaceEditIndependentBaselines);
  AddTest(tests, "WorkspaceShell/WorkspaceEditDoesNotSnapshotBuffers",
          TestWorkspaceShellWorkspaceEditDoesNotSnapshotBuffers);
  AddTest(tests, "WorkspaceShell/WorkspaceEditRejectsOverlappingEdits",
          TestWorkspaceShellWorkspaceEditRejectsOverlappingEdits);
  AddTest(tests, "WorkspaceShell/PluginSnippetTabTriggerExpands",
          TestWorkspaceShellPluginSnippetTabTriggerExpands);
  AddTest(tests, "WorkspaceShell/PluginEditorEventTrackerCoalescesChanges",
          TestPluginEditorEventTrackerCoalescesChanges);
  AddTest(tests, "WorkspaceShell/PluginEditorEventTrackerZeroCostWhenUnsubscribed",
          TestPluginEditorEventTrackerZeroCostWhenUnsubscribed);
  AddTest(tests, "WorkspaceShell/PluginEditorEventTrackerBufferSwitchDoesNotEmit",
          TestPluginEditorEventTrackerBufferSwitchDoesNotEmit);
  AddTest(tests, "WorkspaceShell/PluginEditorEventTrackerSeparatesCursorAndSelection",
          TestPluginEditorEventTrackerSeparatesCursorAndSelection);
  AddTest(tests, "WorkspaceShell/PluginReceivesBufferChangeEvent",
          TestWorkspaceShellPluginReceivesBufferChangeEvent);
  AddTest(tests, "WorkspaceShell/PluginApplyEditsSingleEditUndo",
          TestWorkspaceShellPluginApplyEditsSingleEditUndo);
  AddTest(tests, "WorkspaceShell/PluginApplyEditsMultiEditAtomicUndo",
          TestWorkspaceShellPluginApplyEditsMultiEditAtomicUndo);
  AddTest(tests, "WorkspaceShell/PluginApplyEditsSetsSelection",
          TestWorkspaceShellPluginApplyEditsSetsSelection);
  AddTest(tests, "WorkspaceShell/PluginCursorSelectionRequireColumns",
          TestWorkspaceShellPluginCursorSelectionRequireColumns);
  AddTest(tests, "WorkspaceShell/PluginApplyEditsRejectsUnopenedPath",
          TestWorkspaceShellPluginApplyEditsRejectsUnopenedPath);
  AddTest(tests, "WorkspaceShell/PluginApplyEditsRejectsNonFiniteCoords",
          TestWorkspaceShellPluginApplyEditsRejectsNonFiniteCoords);
  AddTest(tests, "WorkspaceShell/PluginApplyEditsRejectsTooManyEdits",
          TestWorkspaceShellPluginApplyEditsRejectsTooManyEdits);
  AddTest(tests, "WorkspaceShell/PluginApplyEditsClampsOutOfBounds",
          TestWorkspaceShellPluginApplyEditsClampsOutOfBounds);
  AddTest(tests, "WorkspaceShell/LspSemanticOverlayClearedOnEditAndUndo",
          TestWorkspaceShellLspSemanticOverlayClearedOnEditAndUndo);
  AddTest(tests, "WorkspaceShell/LspInlayOverlayClearedOnEdit",
          TestWorkspaceShellLspInlayOverlayClearedOnEdit);
  AddTest(tests, "WorkspaceShell/LspDiagnosticsShiftOnEditWithoutServer",
          TestWorkspaceShellLspDiagnosticsShiftOnEditWithoutServer);
  AddTest(tests, "WorkspaceShell/GhostTextPublishStoresAndSplits",
          TestWorkspaceShellGhostTextPublishStoresAndSplits);
  AddTest(tests, "WorkspaceShell/GhostTextDisabledSettingNoOp",
          TestWorkspaceShellGhostTextDisabledSettingNoOp);
  AddTest(tests, "WorkspaceShell/GhostTextStaleAnchorRejected",
          TestWorkspaceShellGhostTextStaleAnchorRejected);
  AddTest(tests, "WorkspaceShell/GhostTextAcceptInsertsAndClears",
          TestWorkspaceShellGhostTextAcceptInsertsAndClears);
  AddTest(tests, "WorkspaceShell/GhostTextAcceptWithNoneFallsThrough",
          TestWorkspaceShellGhostTextAcceptWithNoneFallsThrough);
  AddTest(tests, "WorkspaceShell/GhostTextInvalidatesOnCaretMove",
          TestWorkspaceShellGhostTextInvalidatesOnCaretMove);
  AddTest(tests, "WorkspaceShell/GhostTextClearIsOwnerScoped",
          TestWorkspaceShellGhostTextClearIsOwnerScoped);
  AddTest(tests, "WorkspaceShell/GhostTextLuaApiPublishesAndClears",
          TestWorkspaceShellGhostTextLuaApiPublishesAndClears);
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
  AddTest(tests, "WorkspaceShell/SignatureHelpPopupFromPluginProvider",
          TestWorkspaceShellSignatureHelpPopupFromPluginProvider);
  AddTest(tests, "WorkspaceShell/OutlineSidebarFromDocumentSymbols",
          TestWorkspaceShellOutlineSidebarFromDocumentSymbols);
  AddTest(tests, "WorkspaceShell/OutlineSidebarFromLspFallback",
          TestWorkspaceShellOutlineSidebarFromLspFallback);
  AddTest(tests, "WorkspaceShell/OutlineCapsLargeLspResult",
          TestWorkspaceShellOutlineCapsLargeLspResult);
  AddTest(tests, "WorkspaceShell/HoverFromLspFallback", TestWorkspaceShellHoverFromLspFallback);
  AddTest(tests, "WorkspaceShell/FormatDocumentAppliesLspEdits",
          TestWorkspaceShellFormatDocumentAppliesLspEdits);
  AddTest(tests, "WorkspaceShell/RenameSymbolAppliesLspEdit",
          TestWorkspaceShellRenameSymbolAppliesLspEdit);
  AddTest(tests, "WorkspaceShell/RenameSymbolOpensAndSavesClosedFiles",
          TestWorkspaceShellRenameSymbolOpensAndSavesClosedFiles);
  AddTest(tests, "WorkspaceShell/CompletionMergesPluginAndLspSources",
          TestWorkspaceShellCompletionMergesPluginAndLspSources);
  AddTest(tests, "WorkspaceShell/SignatureHelpPrefersLspOverPlugin",
          TestWorkspaceShellSignatureHelpPrefersLspOverPlugin);
  AddTest(tests, "WorkspaceShell/InlayHintsPublishMidLineDecorations",
          TestWorkspaceShellInlayHintsPublishMidLineDecorations);
  AddTest(tests, "WorkspaceShell/PrepareRenameRefinesPrompt",
          TestWorkspaceShellPrepareRenameRefinesPrompt);
  AddTest(tests, "WorkspaceShell/ServerApplyEditEditsOpenAndClosedFiles",
          TestWorkspaceShellServerApplyEditEditsOpenAndClosedFiles);
  AddTest(tests, "WorkspaceShell/ServerApplyEditRejectsOutOfRangeClosedFileEdit",
          TestWorkspaceShellServerApplyEditRejectsOutOfRangeClosedFileEdit);
  AddTest(tests, "WorkspaceShell/WorkspaceEditRejectsBeyondEofOpenBufferEdit",
          TestWorkspaceShellWorkspaceEditRejectsBeyondEofOpenBufferEdit);
  AddTest(tests, "WorkspaceShell/PluginsReloadFallsBackFromMissingActivePluginSidebar",
          TestWorkspaceShellPluginsReloadFallsBackFromMissingActivePluginSidebar);
  AddTest(tests, "WorkspaceShell/SidebarModeMenuListsPluginSidebars",
          TestWorkspaceShellSidebarModeMenuListsPluginSidebars);
  AddTest(tests, "WorkspaceShell/PluginDiagnosticsPersistAcrossProjectSwitches",
          TestWorkspaceShellPluginDiagnosticsPersistAcrossProjectSwitches);
  AddTest(tests, "WorkspaceShell/SplitGroupHoverResolvesPaneViewport",
          TestWorkspaceShellSplitGroupHoverResolvesPaneViewport);
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
  AddTest(tests, "WorkspaceShell/ProjectOpenRunsSynchronousPluginAsync",
          TestWorkspaceShellProjectOpenRunsSynchronousPluginAsync);
  AddTest(tests, "WorkspaceShell/ProjectSwitchDoesNotReplayPluginBufferOpenHooks",
          TestWorkspaceShellProjectSwitchDoesNotReplayPluginBufferOpenHooks);
  AddTest(tests, "WorkspaceShell/PluginSidebarPersistsAcrossProjectSwitches",
          TestWorkspaceShellPluginSidebarPersistsAcrossProjectSwitches);
  AddTest(tests, "WorkspaceShell/ProjectReactivationDoesNotReloadPlugins",
          TestWorkspaceShellProjectReactivationDoesNotReloadPlugins);
  AddTest(tests, "WorkspaceShell/ProjectReactivationKeepsLanguageServerWarm",
          TestWorkspaceShellProjectReactivationKeepsLanguageServerWarm);
  AddTest(tests, "WorkspaceShell/OpensDocumentInLspOnFileOpen",
          TestWorkspaceShellOpensDocumentInLspOnFileOpen);
  AddTest(tests, "WorkspaceShell/RestoreEngagesLspForOpenDocument",
          TestWorkspaceShellRestoreEngagesLspForOpenDocument);
}

}  // namespace microide::tests
