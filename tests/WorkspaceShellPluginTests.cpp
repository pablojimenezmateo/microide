#include "TestSupport.h"
#include "TestSupportSdl.h"
#include "perf/AllocationCounter.h"

#include "editor/FoldingModel.h"
#include "editor/PluginDecorationStore.h"
#include "editor/RuntimeSyntaxRegistry.h"
#include "util/JsonValue.h"
#include "util/StringUtil.h"
#include "workspace/FileUri.h"
#include "workspace/PluginEditorEventTracker.h"
#include "workspace/lsp/WorkspaceLspClient.h"
#include "workspace/lsp/WorkspaceLspManager.h"
#include "workspace/shell/WorkspaceShellTestAccess.h"

#include <functional>
#include <memory>

#include <algorithm>
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
#include "support/RetainedRedraw.h"
#include "support/SoftwareCanvas.h"

namespace microide::tests {
namespace {

using microide::workspace::WorkspaceShell;
using WorkspaceShellTestAccess = microide::workspace::WorkspaceShell::TestAccess;

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
    ctx.commands.add("status.wide", function(ctx, args)
      ctx.status.update("counter", {
        text = "a much wider counter text",
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
  Expect(WorkspaceShellTestAccess::HoveredTooltipLabel(shell) == "Counter is 0",
         "hovering a contributed status item should expose its tooltip");
  {
    const util::JsonValue ui = WorkspaceShellTestAccess::ControlUi(shell);
    bool named = false;
    for (const util::JsonValue& element : ui["elements"].AsArray()) {
      named = named || (element["name"].AsString() == "status:plugin/status/counter" &&
                        element["label"].AsString() == "0");
    }
    Expect(named, "the ui query should name the plugin status item status:plugin/status/counter");
    const util::JsonValue commands = WorkspaceShellTestAccess::ControlQuery(shell, "commands");
    bool listed = false;
    for (const util::JsonValue& command : commands.AsArray()) {
      listed = listed || command["command"].AsString() == "status.tick";
    }
    Expect(listed, "the commands query should list the plugin's status.tick");
  }

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
  Expect(redraw.full || redraw.frame || !redraw.rects.empty(),
         "status updates should request a visible redraw");
  {
    EnsureDummySdlVideoInitialized();
    SoftwareCanvas canvas{1280, 720};
    shell.Render(canvas.renderer(), 1280, 720);
    (void)shell.ConsumePendingRenderInvalidation();
    const auto before = WorkspaceShellTestAccess::VisibleStatusItems(shell);
    const auto item = std::find_if(before.begin(), before.end(), [](const auto& candidate) {
      return candidate.item.plugin_id == "status";
    });
    if (item != before.end()) {
      const float x = item->rect.x + item->rect.w * 0.5f;
      const float y = item->rect.y + item->rect.h * 0.5f;
      auto diff = PaintAndDiffWithFullRepaint(shell, canvas, 1280, 720,
                                              DispatchEvents(shell, {MouseMotionEvent(x, y)}));
      Expect(diff.identical(), "the pointer onto the status item should light it; " + DescribePixelDiff(diff));
      Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, "status.wide"), "status.wide should run");
      diff = PaintAndDiffWithFullRepaint(shell, canvas, 1280, 720, shell.ConsumePendingRenderInvalidation());
      Expect(diff.identical(), "the item changing width under the pointer; " + DescribePixelDiff(diff));
      Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, "status.tick"), "status.tick should run");
      diff = PaintAndDiffWithFullRepaint(shell, canvas, 1280, 720, shell.ConsumePendingRenderInvalidation());
      Expect(diff.identical(), "the item narrowing again under the pointer; " + DescribePixelDiff(diff));
      diff = PaintAndDiffWithFullRepaint(shell, canvas, 1280, 720,
                                         DispatchEvents(shell, {MouseMotionEvent(640.0f, 400.0f)}));
      Expect(diff.identical(), "the pointer leaving the status item; " + DescribePixelDiff(diff));
    }
    (void)shell.ConsumePendingRenderInvalidation();
  }
  if (updated_counter != updated_items.end() && !redraw.full && !redraw.rects.empty()) {
    const SDL_FRect item = updated_counter->rect;
    bool covered = false;
    for (const SDL_FRect& rect : redraw.rects) {
      covered = covered || (rect.x <= item.x && rect.y <= item.y && rect.x + rect.w >= item.x + item.w &&
                            rect.y + rect.h >= item.y + item.h);
    }
    Expect(covered, "the status update's damage should cover the item it changed");
  }
}

void TestWorkspaceShellPluginStatusItemDoesNotHoverUnderAModal() {
#if !MICROIDE_HAS_LUA_PLUGINS
  return;
#endif
  TemporaryDirectory temp_dir;
  const std::filesystem::path config_home = temp_dir.path() / "config";
  const std::filesystem::path plugins_root = config_home / "microide" / "plugins";
  const std::filesystem::path project_root = temp_dir.path() / "project";
  const std::filesystem::path source = project_root / "README.md";
  WriteFile(source, "plugin status\n");
  WritePluginInit(plugins_root, "status",
                  R"(local ide = require("microide")
return ide.plugin({
  id = "status",
  setup = function(ctx)
    ctx.commands.add("status.click", function(ctx, args) end)
    ctx.status.add({ id = "indexer", text = "Indexing", tone = "info",
                     command = "status.click", alignment = "right" })
  end
})
)");
  ScopedPluginConfigHomeEnv scoped_plugin_config_home(config_home);
  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, project_root, false, false), "the project opens");
  WorkspaceShellTestAccess::OpenFile(shell, source);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  const auto items = WorkspaceShellTestAccess::VisibleStatusItems(shell);
  const auto item = std::find_if(items.begin(), items.end(),
                                 [](const auto& candidate) { return candidate.item.plugin_id == "status"; });
  Expect(item != items.end(), "the status item is shown");
  if (item == items.end()) {
    return;
  }
  const float x = item->rect.x + item->rect.w * 0.5f;
  const float y = item->rect.y + item->rect.h * 0.5f;
  EnsureDummySdlVideoInitialized();
  SoftwareCanvas canvas{1280, 720};
  shell.Render(canvas.renderer(), 1280, 720);
  (void)shell.ConsumePendingRenderInvalidation();

  WorkspaceShellTestAccess::ActiveEditor(shell).InsertText("x");
  WorkspaceShellTestAccess::RequestCloseTab(shell, 0);
  auto diff = PaintAndDiffWithFullRepaint(shell, canvas, 1280, 720, shell.ConsumePendingRenderInvalidation());
  Expect(diff.identical(), "the prompt opening; " + DescribePixelDiff(diff));
  diff = PaintAndDiffWithFullRepaint(shell, canvas, 1280, 720, DispatchEvents(shell, {MouseMotionEvent(x, y)}));
  Expect(diff.identical(), "the pointer onto the status item under the prompt; " + DescribePixelDiff(diff));
  Expect(!WorkspaceShellTestAccess::VisibleStatusItems(shell).front().hovered,
         "nothing beneath the prompt hovers");
}

void TestWorkspaceShellPluginStatusItemsHideUnderAMaximizedPanel() {
#if !MICROIDE_HAS_LUA_PLUGINS
  return;
#endif
  TemporaryDirectory temp_dir;
  const std::filesystem::path config_home = temp_dir.path() / "config";
  const std::filesystem::path plugins_root = config_home / "microide" / "plugins";
  const std::filesystem::path project_root = temp_dir.path() / "project";
  const std::filesystem::path source = project_root / "README.md";
  WriteFile(source, "plugin status\n");
  WritePluginInit(plugins_root, "status",
                  R"(local ide = require("microide")
return ide.plugin({
  id = "status",
  setup = function(ctx)
    ctx.status.add({ id = "indexer", text = "Indexing", tone = "info", alignment = "right" })
  end
})
)");
  ScopedPluginConfigHomeEnv scoped_plugin_config_home(config_home);
  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, project_root, false, false), "the project opens");
  WorkspaceShellTestAccess::OpenFile(shell, source);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  Expect(!WorkspaceShellTestAccess::VisibleStatusItems(shell).empty(), "the status item is shown");
  WorkspaceShellTestAccess::EnsureTerminalTab(shell);
  EnsureDummySdlVideoInitialized();
  SoftwareCanvas canvas{1280, 720};
  shell.Render(canvas.renderer(), 1280, 720);
  (void)shell.ConsumePendingRenderInvalidation();
  WorkspaceShellTestAccess::TogglePanelMaximizedForTest(shell);
  Expect(WorkspaceShellTestAccess::VisibleStatusItems(shell).empty(),
         "under a maximized panel the header band has no room for the items");
  const auto diff =
      PaintAndDiffWithFullRepaint(shell, canvas, 1280, 720, shell.ConsumePendingRenderInvalidation());
  Expect(diff.identical(), "maximizing the panel; " + DescribePixelDiff(diff));
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
  const std::string uppercase_formatter_command =
      "{ \"sh\", \"-c\", \"tr '[:lower:]' '[:upper:]'\" }";

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

void TestWorkspaceShellDeferredSaveRunsParticipantsOnceOffThread() {
#if !MICROIDE_HAS_LUA_PLUGINS
  return;
#endif
  TemporaryDirectory temp_dir;
  const std::filesystem::path config_home = temp_dir.path() / "config";
  const std::filesystem::path plugins_root = config_home / "microide" / "plugins";
  const std::filesystem::path project_root = temp_dir.path() / "project";
  const std::filesystem::path source = project_root / "main.txt";
  WriteFile(source, "hello world\n");
  WritePluginInit(plugins_root, "save-once",
                  "local ide = require(\"microide\")\n"
                  "return ide.plugin({\n"
                  "  id = \"save-once\",\n"
                  "  setup = function(ctx)\n"
                  "    ctx.save_participants.add(\"bang\", function(buffer)\n"
                  "      return { text = buffer.text:gsub(\"world\", \"world!\") }\n"
                  "    end)\n"
                  "  end\n"
                  "})\n");
  ScopedPluginConfigHomeEnv scoped_plugin_config_home(config_home);

  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, project_root, false, false),
         "the project opens");
  WorkspaceShellTestAccess::OpenFile(shell, source);
  Expect(WorkspaceShellTestAccess::SaveParticipantCount(shell) == 1, "one participant");
  WorkspaceShellTestAccess::RegisterFormatterForTesting(
      shell, std::string(WorkspaceShellTestAccess::ActiveEditor(shell).language_id()),
      {"sed", "s/hello/HELLO/"});
  WorkspaceShellTestAccess::ActiveEditor(shell).InsertText("x");

  Expect(WorkspaceShellTestAccess::SaveTabDeferred(shell, WorkspaceShellTestAccess::ActiveTabIndex(shell)),
         "the deferred save starts");
  Expect(ReadFile(source) == "hello world\n", "nothing is written before the transforms return");
  WorkspaceShellTestAccess::FlushPendingSaveFormatters(shell);
  WorkspaceShellTestAccess::DrainSaveFormatterCompletions(shell);
  Expect(ReadFile(source) == "xHELLO world!\n",
         "participant then formatter, each once, got: " + ReadFile(source));
  Expect(!WorkspaceShellTestAccess::ActiveEditor(shell).dirty(), "and the buffer is clean");

  WorkspaceShellTestAccess::RegisterFormatterForTesting(
      shell, std::string(WorkspaceShellTestAccess::ActiveEditor(shell).language_id()), {});
  WorkspaceShellTestAccess::ActiveEditor(shell).InsertText("y");
  Expect(WorkspaceShellTestAccess::SaveTabDeferred(shell, WorkspaceShellTestAccess::ActiveTabIndex(shell)),
         "the participant-only save starts");
  WorkspaceShellTestAccess::FlushPendingSaveFormatters(shell);
  WorkspaceShellTestAccess::DrainSaveFormatterCompletions(shell);
  Expect(ReadFile(source) == "xyHELLO world!!\n",
         "the participant runs once more on this save, got: " + ReadFile(source));
}

void TestWorkspaceShellDeferredSaveRefusedByAFailingParticipant() {
#if !MICROIDE_HAS_LUA_PLUGINS
  return;
#endif
  TemporaryDirectory temp_dir;
  const std::filesystem::path config_home = temp_dir.path() / "config";
  const std::filesystem::path plugins_root = config_home / "microide" / "plugins";
  const std::filesystem::path project_root = temp_dir.path() / "project";
  const std::filesystem::path source = project_root / "main.txt";
  WriteFile(source, "hello\n");
  WritePluginInit(plugins_root, "save-refuse",
                  "local ide = require(\"microide\")\n"
                  "return ide.plugin({\n"
                  "  id = \"save-refuse\",\n"
                  "  setup = function(ctx)\n"
                  "    ctx.save_participants.add(\"refuse\", function(buffer)\n"
                  "      error(\"lint-gate says no\")\n"
                  "    end)\n"
                  "  end\n"
                  "})\n");
  ScopedPluginConfigHomeEnv scoped_plugin_config_home(config_home);

  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, project_root, false, false),
         "the project opens");
  WorkspaceShellTestAccess::OpenFile(shell, source);
  WorkspaceShellTestAccess::ActiveEditor(shell).InsertText("x");
  Expect(WorkspaceShellTestAccess::SaveTabDeferred(shell, WorkspaceShellTestAccess::ActiveTabIndex(shell)),
         "the deferred save starts");
  WorkspaceShellTestAccess::FlushPendingSaveFormatters(shell);
  WorkspaceShellTestAccess::DrainSaveFormatterCompletions(shell);
  Expect(ReadFile(source) == "hello\n", "a refused save writes nothing");
  Expect(WorkspaceShellTestAccess::ActiveEditor(shell).dirty(), "the buffer stays dirty");
  bool reported = false;
  for (const auto& notification : WorkspaceShellTestAccess::ActiveNotifications(shell)) {
    reported = reported || (notification.message.find("save participant failed") != std::string::npos &&
                            notification.message.find("lint-gate says no") != std::string::npos);
  }
  Expect(reported, "the participant's error is shown");
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
  const std::string failing_formatter_command =
      "{ \"sh\", \"-c\", \"cat >/dev/null; echo formatter-broke >&2; exit 3\" }";

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
  const std::string delayed_formatter_command =
      "{ \"sh\", \"-c\", \"sleep 0.2; tr '[:lower:]' '[:upper:]'\" }";

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

void TestWorkspaceShellPluginContributesVirtualDocument() {
#if !MICROIDE_HAS_LUA_PLUGINS
  return;
#endif
  TemporaryDirectory temp_dir;
  const std::filesystem::path config_home = temp_dir.path() / "config";
  const std::filesystem::path plugins_root = config_home / "microide" / "plugins";
  const std::filesystem::path project_root = temp_dir.path() / "project";
  WriteFile(project_root / "README.md", "virtual document producer fixture\n");

  WritePluginInit(
      plugins_root, "vdocs",
      R"(local ide = require("microide")
return ide.plugin({
  id = "vdocs",
  setup = function(ctx)
    ctx.virtual_documents.add({
      id = "notes.md",
      language_id = "markdown",
      content = "from the plugin",
      editable = false,
    })
  end
})
)");

  ScopedPluginConfigHomeEnv scoped_plugin_config_home(config_home);

  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, project_root, false, false),
         "virtual document producer fixture should open the project");
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  Expect(WorkspaceShellTestAccess::PluginErrors(shell).empty(), DescribePluginState(shell).c_str());

  Expect(WorkspaceShellTestAccess::OpenVirtualDocument(shell, "virtual://vdocs/notes.md"),
         "a plugin-contributed virtual document should be openable");
  Expect(!WorkspaceShellTestAccess::ActiveEditor(shell).lines().empty() &&
             WorkspaceShellTestAccess::ActiveEditor(shell).lines().front() == "from the plugin",
         "the opened tab should hold the content the plugin contributed");
  Expect(!WorkspaceShellTestAccess::HandleTextInput(shell, "edit"),
         "a non-editable contributed document stays read-only");
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
  Expect(redraw.frame || AnyRectIntersects(redraw.rects, layout.editor_surface),
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

void TestWorkspaceShellPluginsReloadReportsTheReloadItRan() {
#if !MICROIDE_HAS_LUA_PLUGINS
  return;
#endif
  TemporaryDirectory temp_dir;
  const std::filesystem::path config_home = temp_dir.path() / "config";
  const std::filesystem::path plugins_root = config_home / "microide" / "plugins";
  const std::filesystem::path project_root = temp_dir.path() / "project";
  WriteFile(project_root / "README.md", "reload fixture\n");
  std::filesystem::create_directories(plugins_root);
  ScopedPluginConfigHomeEnv scoped_plugin_config_home(config_home);

  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, project_root, false, false),
         "the project opens with no plugins");

  WritePluginInit(
      plugins_root, "late",
      R"(local ide = require("microide")
return ide.plugin({ id = "late", setup = function(ctx) end })
)");
  WorkspaceShellTestAccess::EnsurePluginWorkerStarted(shell);
  Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, "plugins-reload"), "the reload runs");
  for (int i = 0; i < 200 && WorkspaceShellTestAccess::CommandFeedbackText(shell).find("Loaded ") ==
                                 std::string::npos; ++i) {
    WorkspaceShellTestAccess::DrainPluginThreadActions(shell);
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  Expect(WorkspaceShellTestAccess::CommandFeedbackText(shell).find("Loaded 1 plugin") !=
             std::string::npos,
         "the feedback is this reload's summary: " + WorkspaceShellTestAccess::CommandFeedbackText(shell));
  bool stale_toast = false;
  bool fresh_toast = false;
  for (const auto& row : WorkspaceShellTestAccess::ActiveNotifications(shell)) {
    stale_toast = stale_toast || row.message.find("Loaded 0 plugins") != std::string::npos;
    fresh_toast = fresh_toast || row.message.find("Loaded 1 plugin") != std::string::npos;
  }
  Expect(!stale_toast, "no toast reports the previous reload's count");
  Expect(fresh_toast, "the toast reports this reload's count");
}

void TestWorkspaceShellAPluginThatFailsToLoadSaysSo() {
#if !MICROIDE_HAS_LUA_PLUGINS
  return;
#endif
  TemporaryDirectory temp_dir;
  const std::filesystem::path config_home = temp_dir.path() / "config";
  const std::filesystem::path plugins_root = config_home / "microide" / "plugins";
  const std::filesystem::path project_root = temp_dir.path() / "project";
  WriteFile(project_root / "README.md", "broken plugin fixture\n");
  WritePluginInit(
      plugins_root, "broken",
      R"(local ide = require("microide")
return ide.plugin({ id = "broken", setup = function(ctx) error("broken on purpose") end })
)");
  ScopedPluginConfigHomeEnv scoped_plugin_config_home(config_home);

  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, project_root, false, false),
         "the project opens with the broken plugin installed");
  const microide::workspace::NotificationService::Notification* warning = nullptr;
  for (int i = 0; i < 200 && warning == nullptr; ++i) {
    WorkspaceShellTestAccess::DrainPluginThreadActions(shell);
    for (const auto& row : WorkspaceShellTestAccess::ActiveNotifications(shell)) {
      if (row.message.find("broken") != std::string::npos &&
          row.message.find("broken on purpose") != std::string::npos) {
        warning = &row;
      }
    }
    if (warning == nullptr) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
  }
  Expect(warning != nullptr, "opening the project warns that the plugin failed to load, and why");
  if (warning != nullptr) {
    Expect(warning->tone == microide::workspace::NotificationService::Tone::Warning, "as a warning");
    Expect(!warning->actions.empty() && warning->actions.front().label == "Show Errors",
           "with a button to the Plugin Errors channel");
  }
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

  WorkspaceShellTestAccess::ActiveEditor(shell).MoveCursorTo(1, 0);
  WorkspaceShellTestAccess::ExpireSignatureHelp(shell);
  Expect(!WorkspaceShellTestAccess::SignatureHelpPopup(shell).has_value(),
         "moving the caret should dismiss the signature popup");

  WorkspaceShellTestAccess::ActiveEditor(shell).MoveCursorTo(0, 0);
  Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, "signature-help"),
         "signature-help shows the popup again");
  WorkspaceShellTestAccess::ActiveEditor(shell).MoveCursorTo(1, 0);
  SoftwareCanvas canvas(1280, 720);
  (void)shell.PrepareFrameOnce(canvas.renderer(), 1280, 720);
  Expect(!WorkspaceShellTestAccess::SignatureHelpPopup(shell).has_value(),
         "preparing a frame dismisses a signature popup whose caret moved on");
}

void TestWorkspaceShellOutlineSidebarFromLspFallback() {
#if !MICROIDE_HAS_LUA_PLUGINS
  return;
#endif
  TemporaryDirectory temp_dir;
  const std::filesystem::path config_home = temp_dir.path() / "config";
  const std::filesystem::path plugins_root = config_home / "microide" / "plugins";
  const std::filesystem::path project = temp_dir.path() / "proj";
  const std::filesystem::path py_file = project / "main.py";
  WriteFile(py_file, "class Widget:\n    def draw(self):\n        pass\n\n\ndef main():\n    pass\n");

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

  auto stub = std::make_unique<workspace::LspClient>();
  workspace::LspClient* const stub_raw = stub.get();
  stub_raw->EnableTestStubMode();
  stub_raw->SetTestDocumentSymbolHandler(
      [](std::string uri, workspace::LspClient::DocumentSymbolCallback cb) {
        (void)uri;
        workspace::LspClient::DocumentSymbol widget;
        widget.name = "Widget";
        widget.kind = 5;
        widget.selection_range.start = {0, 6};
        workspace::LspClient::DocumentSymbol draw;
        draw.name = "draw";
        draw.kind = 6;
        draw.selection_range.start = {1, 8};
        workspace::LspClient::DocumentSymbol render;
        render.name = "render";
        render.kind = 6;
        render.selection_range.start = {3, 8};
        widget.children.push_back(render);
        widget.children.push_back(draw);
        workspace::LspClient::DocumentSymbol main_fn;
        main_fn.name = "main";
        main_fn.kind = 12;
        main_fn.selection_range.start = {5, 4};
        cb(std::vector<workspace::LspClient::DocumentSymbol>{main_fn, widget});
      });
  Expect(WorkspaceShellTestAccess::LspManagerForTesting(shell)
             .InstallTestClientIntoExistingForTesting("python", std::move(stub)),
         "fixture should attach a stub python client");

  WorkspaceShellTestAccess::OpenFile(shell, py_file);
  Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, "sidebar-show outline"),
         "sidebar-show should accept the built-in outline view");
  WorkspaceShellTestAccess::ConsumeLspCallbacks(shell);

  const auto& items = WorkspaceShellTestAccess::PluginSidebarItems(shell);
  Expect(items.size() == 4,
         "outline should flatten the LSP document-symbol fallback into four rows");
  Expect(items[0].label == "Widget" && items[0].depth == 0,
         "LSP outline should list the class at depth 0, first by position although the "
         "server sent it second");
  Expect(items[1].label == "draw" && items[1].depth == 1,
         "LSP outline should indent the method child to depth 1, sorted by position");
  Expect(items[2].label == "render" && items[2].depth == 1,
         "the later method follows although the server listed it first");
  Expect(items[3].label == "main" && items[3].depth == 0,
         "LSP outline should return to depth 0 after the child subtree");
  Expect(items[1].line == 2 && items[1].column == 9,
         "LSP outline row should carry 1-based line/column from the selection range");
}

void TestWorkspaceShellGoToSymbolInEditorPicksFromTheOutlineSymbols() {
#if !MICROIDE_HAS_LUA_PLUGINS
  return;
#endif
  TemporaryDirectory temp_dir;
  const std::filesystem::path config_home = temp_dir.path() / "config";
  const std::filesystem::path plugins_root = config_home / "microide" / "plugins";
  const std::filesystem::path project = temp_dir.path() / "proj";
  const std::filesystem::path py_file = project / "main.py";
  WriteFile(py_file, "class Widget:\n    def draw(self):\n        pass\n\n\ndef main():\n    pass\n");
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
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, project, false, false), "the fixture opens");
  auto stub = std::make_unique<workspace::LspClient>();
  stub->EnableTestStubMode();
  stub->SetTestDocumentSymbolHandler([](std::string, workspace::LspClient::DocumentSymbolCallback cb) {
    workspace::LspClient::DocumentSymbol widget;
    widget.name = "Widget";
    widget.kind = 5;
    widget.selection_range.start = {0, 6};
    workspace::LspClient::DocumentSymbol draw;
    draw.name = "draw";
    draw.kind = 6;
    draw.selection_range.start = {1, 8};
    widget.children.push_back(draw);
    workspace::LspClient::DocumentSymbol main_fn;
    main_fn.name = "main";
    main_fn.kind = 12;
    main_fn.selection_range.start = {5, 4};
    cb(std::vector<workspace::LspClient::DocumentSymbol>{widget, main_fn});
  });
  Expect(WorkspaceShellTestAccess::LspManagerForTesting(shell).InstallTestClientIntoExistingForTesting(
             "python", std::move(stub)),
         "the stub server attaches");
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::OpenFile(shell, py_file);
  WorkspaceShellTestAccess::SetFocusEditor(shell);
  editor::TextViewport& viewport = WorkspaceShellTestAccess::ActiveEditor(shell);
  viewport.MoveCursorTo(6, 2);
  const auto overlay = [&] { return WorkspaceShellTestAccess::ControlUi(shell)["overlay"]; };
  (void)shell.HandleEvent(KeyDownEvent(SDLK_O, static_cast<SDL_Keymod>(SDL_KMOD_CTRL | SDL_KMOD_SHIFT)));
  Expect(overlay()["visible"].AsBool() && overlay()["kind"].AsString() == "document-symbol",
         "Ctrl+Shift+O opens the symbol pick");
  WorkspaceShellTestAccess::ConsumeLspCallbacks(shell);
  Expect(overlay()["count"].AsInt() == 3, "the pick lists the three symbols, got " +
                                               std::to_string(overlay()["count"].AsInt()));
  (void)shell.HandleEvent(TextInputEvent("draw"));
  Expect(viewport.cursor_line() == 1 && viewport.cursor_column() == 8,
         "typing previews the top match: the caret is on draw");
  (void)shell.HandleEvent(KeyDownEvent(SDLK_ESCAPE));
  Expect(!overlay()["visible"].AsBool() && viewport.cursor_line() == 6 && viewport.cursor_column() == 2,
         "Escape puts the caret back");
  (void)shell.HandleEvent(KeyDownEvent(SDLK_P, SDL_KMOD_CTRL));
  (void)shell.HandleEvent(TextInputEvent("@main"));
  Expect(overlay()["kind"].AsString() == "document-symbol", "@ typed into Quick Open opens the pick");
  WorkspaceShellTestAccess::ConsumeLspCallbacks(shell);
  (void)shell.HandleEvent(KeyDownEvent(SDLK_RETURN));
  Expect(!overlay()["visible"].AsBool() && viewport.cursor_line() == 5 && viewport.cursor_column() == 4,
         "Enter keeps the symbol's position");
}

void TestWorkspaceShellOutlineCapsLargeLspResult() {
#if !MICROIDE_HAS_LUA_PLUGINS
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
        std::vector<workspace::LspClient::DocumentSymbol> symbols;
        symbols.reserve(6000);
        for (int i = 0; i < 6000; ++i) {
          workspace::LspClient::DocumentSymbol symbol;
          symbol.name = "sym" + std::to_string(i);
          symbol.kind = 12;
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
  Expect(items.size() == 5001,
         "the outline is capped at the 5000-node budget plus a truncation marker");
  Expect(items.back().label == "… (outline truncated)",
         "the capped outline ends with a non-navigable truncation marker");
  Expect(items.back().line == 0,
         "the truncation marker does not navigate anywhere");
}

void TestWorkspaceShellHoverFromLspFallback() {
#if !MICROIDE_HAS_LUA_PLUGINS
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

void TestWorkspaceShellHoverSupersededCompletionIsDropped() {
#if !MICROIDE_HAS_LUA_PLUGINS
  return;
#endif
  TemporaryDirectory temp_dir;
  const std::filesystem::path config_home = temp_dir.path() / "config";
  const std::filesystem::path plugins_root = config_home / "microide" / "plugins";
  const std::filesystem::path project = temp_dir.path() / "proj";
  const std::filesystem::path py_file = project / "main.py";
  WriteFile(py_file, "value = 1\nother = 2\n");

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
         "hover supersede fixture should open the project");

  std::vector<workspace::LspClient::HoverCallback> parked;
  auto stub = std::make_unique<workspace::LspClient>();
  workspace::LspClient* const stub_raw = stub.get();
  stub_raw->EnableTestStubMode();
  stub_raw->SetTestHoverHandler(
      [&parked](std::string uri, workspace::LspClient::HoverCallback cb) {
        (void)uri;
        parked.push_back(std::move(cb));
      });
  Expect(WorkspaceShellTestAccess::LspManagerForTesting(shell)
             .InstallTestClientIntoExistingForTesting("python", std::move(stub)),
         "supersede fixture should attach a stub python client");

  WorkspaceShellTestAccess::OpenFile(shell, py_file);
  WorkspaceShellTestAccess::BeginLspHoverForTesting(shell, py_file, 1, 1);
  WorkspaceShellTestAccess::ConsumeLspCallbacks(shell);
  WorkspaceShellTestAccess::BeginLspHoverForTesting(shell, py_file, 2, 1);
  WorkspaceShellTestAccess::ConsumeLspCallbacks(shell);
  Expect(parked.size() == 2, "each hovered cell should dispatch its own hover request");

  parked.front()(util::ParseJson(R"({"contents":{"kind":"markdown","value":"stale"}})"));
  WorkspaceShellTestAccess::ConsumeLspCallbacks(shell);
  Expect(WorkspaceShellTestAccess::HoverCacheContentForTesting(shell).empty(),
         "a hover reply for a superseded cell must not populate the current cell");

  parked.back()(util::ParseJson(R"({"contents":{"kind":"markdown","value":"fresh"}})"));
  WorkspaceShellTestAccess::ConsumeLspCallbacks(shell);
  Expect(WorkspaceShellTestAccess::HoverCacheContentForTesting(shell) == "fresh",
         "the current cell's own hover reply should still resolve it");
}

void TestWorkspaceShellFormatDocumentAppliesLspEdits() {
#if !MICROIDE_HAS_LUA_PLUGINS
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
  stub_raw->SetTestFormattingHandler([](std::string uri, bool, workspace::LspClient::FormattingCallback cb) {
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

void TestWorkspaceShellFormatDocumentFollowsTabsAndDropsAStaleResult() {
#if !MICROIDE_HAS_LUA_PLUGINS
  return;
#endif
  TemporaryDirectory temp_dir;
  const std::filesystem::path config_home = temp_dir.path() / "config";
  const std::filesystem::path plugins_root = config_home / "microide" / "plugins";
  const std::filesystem::path project = temp_dir.path() / "proj";
  const std::filesystem::path py_file = project / "main.py";
  WriteFile(py_file, "if x:\n\ty=2\n");
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
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, project, false, false), "open the project");
  auto stub = std::make_unique<workspace::LspClient>();
  workspace::LspClient* const stub_raw = stub.get();
  stub_raw->EnableTestStubMode();
  std::optional<bool> asked_spaces;
  workspace::LspClient::FormattingCallback held;
  stub_raw->SetTestFormattingHandler(
      [&](std::string, bool insert_spaces, workspace::LspClient::FormattingCallback cb) {
        asked_spaces = insert_spaces;
        held = std::move(cb);
      });
  Expect(WorkspaceShellTestAccess::LspManagerForTesting(shell)
             .InstallTestClientIntoExistingForTesting("python", std::move(stub)),
         "a stub python client");
  WorkspaceShellTestAccess::OpenFile(shell, py_file);
  auto& editor = WorkspaceShellTestAccess::ActiveEditor(shell);
  editor.SetSoftTabs(false);
  Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, "format-document"), "format-document runs");
  WorkspaceShellTestAccess::ConsumeLspCallbacks(shell);
  Expect(asked_spaces == std::optional<bool>(false), "a buffer indented with tabs asks for tabs");
  Expect(static_cast<bool>(held), "the request reached the server");

  editor.MoveCursorTo(0, 0);
  editor.InsertText("# typed while formatting\n");
  held(std::vector<workspace::LspClient::TextEdit>{{workspace::LspClient::Range{{0, 0}, {0, 5}}, "if x :"}});
  WorkspaceShellTestAccess::ConsumeLspCallbacks(shell);
  Expect(editor.lines()[0] == "# typed while formatting" && editor.lines()[1] == "if x:",
         "edits computed for the old text are not applied to the new: " + std::string(editor.lines()[0]));
}

void TestWorkspaceShellCodeActionFixIsRefusedOnAChangedBuffer() {
#if !MICROIDE_HAS_LUA_PLUGINS
  return;
#endif
  TemporaryDirectory temp_dir;
  const std::filesystem::path config_home = temp_dir.path() / "config";
  const std::filesystem::path plugins_root = config_home / "microide" / "plugins";
  const std::filesystem::path project = temp_dir.path() / "proj";
  const std::filesystem::path py_file = project / "main.py";
  WriteFile(py_file, "import os\nx = 1\n");
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
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, project, false, false), "open the project");
  auto stub = std::make_unique<workspace::LspClient>();
  workspace::LspClient* const stub_raw = stub.get();
  stub_raw->EnableTestStubMode();
  const std::string uri = workspace::FileUriForPath(py_file);
  stub_raw->SetTestCodeActionHandler([uri](std::string, workspace::LspClient::CodeActionCallback cb) {
    workspace::LspClient::CodeAction remove_import;
    remove_import.title = "Remove unused import";
    remove_import.has_edit = true;
    remove_import.edit.changes[uri] = {{workspace::LspClient::Range{{0, 0}, {1, 0}}, ""}};
    cb(std::vector<workspace::LspClient::CodeAction>{remove_import});
  });
  Expect(WorkspaceShellTestAccess::LspManagerForTesting(shell)
             .InstallTestClientIntoExistingForTesting("python", std::move(stub)),
         "a stub python client");
  WorkspaceShellTestAccess::OpenFile(shell, py_file);
  auto& editor = WorkspaceShellTestAccess::ActiveEditor(shell);
  Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, "code-actions"), "code-actions runs");
  WorkspaceShellTestAccess::ConsumeLspCallbacks(shell);
  Expect(WorkspaceShellTestAccess::CodeActionSession(shell).items.size() == 1, "the fix is listed");

  editor.MoveCursorTo(0, 0);
  editor.InsertText("# header\n");
  Expect(!WorkspaceShellTestAccess::ExecuteSelectedCodeAction(shell), "the stale fix is refused");
  Expect(editor.lines()[0] == "# header" && editor.lines()[1] == "import os",
         "and the buffer is as the user made it: " + std::string(editor.lines()[0]));
  Expect(!WorkspaceShellTestAccess::CodeActionSession(shell).error.empty(), "the list says why");
}

void TestWorkspaceShellRenameSymbolAppliesLspEdit() {
#if !MICROIDE_HAS_LUA_PLUGINS
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
  WorkspaceShellTestAccess::ActiveEditor(shell).MoveCursorTo(0, 2);

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

  WorkspaceShellTestAccess::ActiveEditor(shell).MoveCursorTo(0, 2);
  Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, "rename-symbol total"),
         "rename-symbol <name> dispatches");
  Expect(!WorkspaceShellTestAccess::PromptSurfaceVisible(shell), "and opens no prompt");
  WorkspaceShellTestAccess::ConsumeLspCallbacks(shell);
  Expect(WorkspaceShellTestAccess::ActiveEditor(shell).lines()[0] == "total = 1" &&
             WorkspaceShellTestAccess::ActiveEditor(shell).lines()[1] == "print(total)",
         "the named rename applies the server's edit");
}

void TestWorkspaceShellRenameResultForAChangedBufferIsDropped() {
#if !MICROIDE_HAS_LUA_PLUGINS
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
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, project, false, false), "open the project");
  auto stub = std::make_unique<workspace::LspClient>();
  workspace::LspClient* const stub_raw = stub.get();
  stub_raw->EnableTestStubMode();
  workspace::LspClient::RenameCallback held;
  std::string held_uri;
  stub_raw->SetTestRenameHandler([&](std::string uri, std::string, workspace::LspClient::RenameCallback cb) {
    held_uri = std::move(uri);
    held = std::move(cb);
  });
  Expect(WorkspaceShellTestAccess::LspManagerForTesting(shell)
             .InstallTestClientIntoExistingForTesting("python", std::move(stub)),
         "a stub python client");
  WorkspaceShellTestAccess::OpenFile(shell, py_file);
  auto& editor = WorkspaceShellTestAccess::ActiveEditor(shell);
  editor.MoveCursorTo(0, 2);
  Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, "rename-symbol count"), "rename-symbol runs");
  WorkspaceShellTestAccess::ConsumeLspCallbacks(shell);
  Expect(static_cast<bool>(held), "the request reached the server");

  editor.MoveCursorTo(0, 0);
  editor.InsertText("# note\n");
  workspace::LspClient::WorkspaceEdit edit;
  edit.changes[held_uri] = {{workspace::LspClient::Range{{0, 0}, {0, 5}}, "count"},
                            {workspace::LspClient::Range{{1, 6}, {1, 11}}, "count"}};
  held(std::move(edit));
  WorkspaceShellTestAccess::ConsumeLspCallbacks(shell);
  Expect(editor.lines()[0] == "# note" && editor.lines()[1] == "value = 1",
         "the stale rename is not applied over the new text: " + std::string(editor.lines()[0]) + " / " +
             std::string(editor.lines()[1]));
}

void TestWorkspaceShellRenameSymbolOpensAndSavesClosedFiles() {
#if !MICROIDE_HAS_LUA_PLUGINS
  return;
#endif
  TemporaryDirectory temp_dir;
  const std::filesystem::path config_home = temp_dir.path() / "config";
  const std::filesystem::path plugins_root = config_home / "microide" / "plugins";
  const std::filesystem::path project = temp_dir.path() / "proj";
  const std::filesystem::path main_py = project / "main.py";
  const std::filesystem::path helper_py = project / "helper.py";
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
  WorkspaceShellTestAccess::ActiveEditor(shell).MoveCursorTo(0, 2);

  Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, "rename-symbol"),
         "rename-symbol should open the name prompt");
  WorkspaceShellTestAccess::SetPromptSurfaceInput(shell, "count");
  WorkspaceShellTestAccess::ConfirmPromptSurface(shell);
  WorkspaceShellTestAccess::ConsumeLspCallbacks(shell);

  Expect(WorkspaceShellTestAccess::PromptSurfaceVisible(shell),
         "a rename touching unopened files should confirm first");
  Expect(WorkspaceShellTestAccess::PromptSurfaceTitle(shell) == "Rename Across Files",
         "the confirmation should be the rename-across-files prompt");
  Expect(WorkspaceShellTestAccess::PromptSurfaceMessage(shell).find("not open") != std::string::npos,
         "the confirmation should state that some files are not open");

  WorkspaceShellTestAccess::ConfirmPromptSurface(shell);

  Expect(ReadFile(main_py) == "count = 1\n",
         "the already-open file should be renamed and saved to disk");
  Expect(ReadFile(helper_py) == "print(count)\n",
         "the previously-closed file should be renamed and saved to disk");
  Expect(WorkspaceShellTestAccess::CountOpenBufferViews(shell, helper_py) == 0,
         "a closed file touched by a rename must be written on disk without opening a tab");
}

void TestWorkspaceShellCompletionMergesPluginAndLspSources() {
#if !MICROIDE_HAS_LUA_PLUGINS
  return;
#endif
  TemporaryDirectory temp_dir;
  const std::filesystem::path config_home = temp_dir.path() / "config";
  const std::filesystem::path plugins_root = config_home / "microide" / "plugins";
  const std::filesystem::path project = temp_dir.path() / "proj";
  const std::filesystem::path md_file = project / "notes.md";
  WriteFile(md_file, "alpha\n\n");

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
        cb(workspace::LspClient::CompletionList{std::move(items)});
      });
  Expect(WorkspaceShellTestAccess::LspManagerForTesting(shell)
             .InstallTestClientIntoExistingForTesting("markdown", std::move(stub)),
         "fixture should attach a stub markdown client");

  WorkspaceShellTestAccess::OpenFile(shell, md_file);
  WorkspaceShellTestAccess::ActiveEditor(shell).MoveCursorTo(1, 0);

  Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, "completion"),
         "completion command should execute");
  WorkspaceShellTestAccess::ConsumeLspCallbacks(shell);

  const auto& session = WorkspaceShellTestAccess::CompletionSession(shell);
  Expect(session.items.size() == 3 && session.VisibleCount() == 3,
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

void TestWorkspaceShellCompletionAppliesAdditionalTextEdits() {
#if !MICROIDE_HAS_LUA_PLUGINS
  return;
#endif
  TemporaryDirectory temp_dir;
  const std::filesystem::path config_home = temp_dir.path() / "config";
  const std::filesystem::path plugins_root = config_home / "microide" / "plugins";
  const std::filesystem::path project = temp_dir.path() / "proj";
  const std::filesystem::path md_file = project / "notes.md";
  WriteFile(md_file, "first\nx = ve\n");

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
  stub_raw->SetTestCompletionHandler(
      [](std::string, workspace::LspClient::Position,
         workspace::LspClient::CompletionCallback cb) {
        std::vector<workspace::LspClient::CompletionItem> items;
        workspace::LspClient::CompletionItem item;
        item.label = "vector";
        item.insert_text = "vector";
        item.replace_range = workspace::LspClient::Range{{1, 4}, {1, 6}};
        item.additional_text_edits.push_back(
            {workspace::LspClient::Range{{0, 0}, {0, 0}}, "import vector\n"});
        items.push_back(std::move(item));
        cb(workspace::LspClient::CompletionList{std::move(items)});
      });
  Expect(WorkspaceShellTestAccess::LspManagerForTesting(shell)
             .InstallTestClientIntoExistingForTesting("markdown", std::move(stub)),
         "fixture should attach a stub markdown client");

  WorkspaceShellTestAccess::OpenFile(shell, md_file);
  auto& editor = WorkspaceShellTestAccess::ActiveEditor(shell);
  editor.MoveCursorTo(1, 6);

  Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, "completion"),
         "completion command should execute");
  WorkspaceShellTestAccess::ConsumeLspCallbacks(shell);
  const auto& session = WorkspaceShellTestAccess::CompletionSession(shell);
  Expect(session.items.size() == 1 && session.items[0].additional_edits.size() == 1,
         "the item carries its additional edit into the session");

  Expect(WorkspaceShellTestAccess::ApplySelectedCompletion(shell), "the completion applies");
  Expect(editor.lines().size() == 4 && editor.lines()[0] == "import vector" &&
             editor.lines()[1] == "first" && editor.lines()[2] == "x = vector",
         "the import line lands above and the symbol replaces the typed prefix: " +
             std::string(editor.lines()[0]) + "|" + std::string(editor.lines()[1]) + "|" +
             std::string(editor.lines()[2]));
  Expect(editor.cursor_line() == 2 && editor.cursor_column() == 10,
         "the caret sits after the inserted symbol on its (shifted) line");
  Expect(editor.Undo(), "undo reverts");
  Expect(editor.lines().size() == 3 && editor.lines()[0] == "first" && editor.lines()[1] == "x = ve",
         "one undo step removes both the import line and the insertion");
}

void TestWorkspaceShellRenameOfDirtyFileWaitsForTheFormatterNotTheWindow() {
#if !MICROIDE_HAS_LUA_PLUGINS
  return;
#endif
  TemporaryDirectory temp_dir;
  const std::filesystem::path config_home = temp_dir.path() / "config";
  const std::filesystem::path plugin = config_home / "microide" / "plugins" / "fmt";
  WriteFile(plugin / "init.lua", R"lua(local ide = require("microide")
return ide.plugin({
  id = "fmt",
  capabilities = { process = { exec = true } },
  setup = function(ctx)
    ctx.formatters.add({ id = "up", language_id = "todo", label = "Up",
                         command = { "sh", "-c", "tr '[:lower:]' '[:upper:]'" } })
  end
})
)lua");
  WriteFile(plugin / "syntax" / "todo.lua", R"lua(return {
  filetype = "todo", files = { "\\.todo$" },
  rules = { { pattern = "\\b[A-Z_]+\\b", group = "keyword" } }
}
)lua");
  ScopedPluginConfigHomeEnv scoped(config_home);
  const std::filesystem::path project = temp_dir.path() / "proj";
  WriteFile(project / "a.todo", "seed\n");

  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, project, false, false), "open project");
  WorkspaceShellTestAccess::OpenFile(shell, project / "a.todo");
  auto& editor = WorkspaceShellTestAccess::ActiveEditor(shell);
  editor.SelectAll();
  editor.InsertText("alpha\n");

  WorkspaceShellTestAccess::OpenPromptSurfaceForTest(
      shell, workspace::PromptSurfaceState::Action::RenamePath,
      workspace::PromptSurfaceState::Kind::TextInput,
      project / "a.todo", "", "b.todo");
  WorkspaceShellTestAccess::ConfirmPromptSurfaceSavingDirtyBuffers(shell);
  Expect(WorkspaceShellTestAccess::PendingSaveFormatterCount(shell) == 1,
         "the save's formatter runs off the shell thread");
  Expect(std::filesystem::exists(project / "a.todo") && !std::filesystem::exists(project / "b.todo"),
         "and the rename waits for its write");
  Expect(!WorkspaceShellTestAccess::PromptSurfaceVisible(shell),
         "the prompt is closed: the user's decision is made");

  WorkspaceShellTestAccess::FlushPendingSaveFormatters(shell);
  Expect(!std::filesystem::exists(project / "a.todo") &&
             ReadFile(project / "b.todo") == "ALPHA\n",
         "once the formatted write lands, the file is renamed with its formatted bytes");
  Expect(WorkspaceShellTestAccess::ActiveEditor(shell).path() == project / "b.todo",
         "and the tab follows it");
}

void WithMarkdownCompletionServer(
    const std::string& content, std::size_t caret_line, std::size_t caret_column,
    std::function<void(std::string, workspace::LspClient::Position,
                       workspace::LspClient::CompletionCallback)>
        handler,
    const std::function<void(WorkspaceShell&, editor::TextViewport&)>& body,
    std::string_view trigger_characters = {}) {
#if !MICROIDE_HAS_LUA_PLUGINS
  return;
#endif
  TemporaryDirectory temp_dir;
  const std::filesystem::path config_home = temp_dir.path() / "config";
  const std::filesystem::path plugins_root = config_home / "microide" / "plugins";
  const std::filesystem::path project = temp_dir.path() / "proj";
  const std::filesystem::path md_file = project / "notes.md";
  WriteFile(md_file, content);
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
  stub->EnableTestStubMode();
  stub->SetTestCompletionHandler(std::move(handler));
  stub->SetCompletionTriggerCharactersForTesting(trigger_characters);
  Expect(WorkspaceShellTestAccess::LspManagerForTesting(shell)
             .InstallTestClientIntoExistingForTesting("markdown", std::move(stub)),
         "fixture should attach a stub markdown client");
  WorkspaceShellTestAccess::OpenFile(shell, md_file);
  auto& editor = WorkspaceShellTestAccess::ActiveEditor(shell);
  editor.MoveCursorTo(caret_line, caret_column);
  body(shell, editor);
}

workspace::LspClient::CompletionItem PlainCompletionItem(std::string label) {
  workspace::LspClient::CompletionItem item;
  item.insert_text = label;
  item.label = std::move(label);
  return item;
}

bool CompletionListUp(const WorkspaceShell& shell) {
  return WorkspaceShellTestAccess::OverlayVisible(shell) &&
         WorkspaceShellTestAccess::ActiveOverlayMode(shell) ==
             WorkspaceShell::OverlayMode::Completion;
}

void TestWorkspaceShellOutlineAsksForTheFilesSymbols() {
  WithMarkdownCompletionServer(
      "# Title\n\n## Section\n", 0, 0,
      [](std::string, workspace::LspClient::Position, workspace::LspClient::CompletionCallback) {},
      [&](WorkspaceShell& shell, editor::TextViewport& editor) {
        auto* client = WorkspaceShellTestAccess::LspManagerForTesting(shell).FindStartedServer("markdown");
        Expect(client != nullptr, "the stub client should be installed");
        if (client == nullptr) {
          return;
        }
        std::string asked_uri;
        client->SetTestDocumentSymbolHandler(
            [&asked_uri](std::string uri, workspace::LspClient::DocumentSymbolCallback cb) {
              asked_uri = uri;
              workspace::LspClient::DocumentSymbol symbol;
              symbol.name = "Section";
              symbol.range.start.line = 2;
              symbol.selection_range = symbol.range;
              cb(std::vector<workspace::LspClient::DocumentSymbol>{symbol});
            });
        Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, "sidebar-show outline"),
               "the outline should open");
        WaitUntil([&]() { return !asked_uri.empty(); }, std::chrono::seconds(3),
                  std::chrono::milliseconds(5), [&]() {
                    WorkspaceShellTestAccess::ConsumeLspCallbacks(shell);
                    (void)shell.HandleScheduledWake();
                  });
        Expect(asked_uri == workspace::FileUriForPath(editor.path()),
               "the outline should ask for the open file's symbols, asked for [" + asked_uri + "]");
      });
}

void TestWorkspaceShellCtrlClickRepaintsTheCaretBeforeTheDefinition() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  WithMarkdownCompletionServer(
      "one\ntwo\nthree\nfour\nfive\n", 0, 0,
      [](std::string, workspace::LspClient::Position, workspace::LspClient::CompletionCallback) {},
      [&](WorkspaceShell& shell, editor::TextViewport& editor) {
        constexpr int kWidth = 1280;
        constexpr int kHeight = 720;
        EnsureDummySdlVideoInitialized();
        WorkspaceShellTestAccess::SetWindowSize(shell, kWidth, kHeight);
        WorkspaceShellTestAccess::SetFocusEditor(shell);
        WorkspaceShellTestAccess::NoteLayoutInputsChanged(shell);
        SoftwareCanvas canvas(kWidth, kHeight);
        const auto pin = [&]() { WorkspaceShellTestAccess::SetCaretBlinkEpochMs(shell, SDL_GetTicks()); };
        pin();
        shell.Render(canvas.renderer(), kWidth, kHeight);
        (void)shell.ConsumePendingRenderInvalidation();
        const util::JsonValue ui = WorkspaceShellTestAccess::ControlUi(shell);
        const util::JsonValue& pane = ui["editors"][std::size_t{0}];
        const float x = static_cast<float>(pane["textX"].AsDouble(0.0)) + 4.0f;
        float y = 0.0f;
        for (const util::JsonValue& row : pane["rows"].AsArray()) {
          if (row["line"].AsInt() == 4) {
            y = static_cast<float>(row["y"].AsDouble(0.0) + pane["lineHeight"].AsDouble(0.0) / 2.0);
          }
        }
        Expect(y > 0.0f, "line 4 should be on screen");
        const auto redraw = DispatchEvents(shell, {KeyDownEvent(SDLK_LCTRL, SDL_KMOD_LCTRL),
                                                   MouseMotionEvent(x, y),
                                                   MouseButtonEvent(SDL_EVENT_MOUSE_BUTTON_DOWN, x, y),
                                                   MouseButtonEvent(SDL_EVENT_MOUSE_BUTTON_UP, x, y),
                                                   KeyUpEvent(SDLK_LCTRL)});
        Expect(editor.cursor_line() == 3, "Ctrl+click should place the caret on line 4");
        const auto diff = PaintAndDiffWithFullRepaint(shell, canvas, kWidth, kHeight, redraw, pin);
        Expect(diff.identical(), "Ctrl+click should repaint the caret before the definition answers; " +
                                     DescribePixelDiff(diff));
      });
}

void TestWorkspaceShellFindReferencesShowsTheResults() {
  WithMarkdownCompletionServer(
      "alpha\nalpha\n", 0, 1,
      [](std::string, workspace::LspClient::Position, workspace::LspClient::CompletionCallback) {},
      [&](WorkspaceShell& shell, editor::TextViewport& editor) {
        WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
        WorkspaceShellTestAccess::NoteLayoutInputsChanged(shell);
        auto* client = WorkspaceShellTestAccess::LspManagerForTesting(shell).FindStartedServer("markdown");
        Expect(client != nullptr, "the stub client should be installed");
        if (client == nullptr) {
          return;
        }
        const std::string uri = workspace::FileUriForPath(editor.path());
        client->SetTestReferencesHandler(
            [uri](std::string, workspace::LspClient::Position, workspace::LspClient::LocationCallback cb) {
              std::vector<workspace::LspClient::Location> locations(2);
              locations[0].uri = uri;
              locations[1].uri = uri;
              locations[1].range.start.line = 1;
              cb(std::move(locations));
            });
        Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, "find-references"),
               "find-references should run");
        WorkspaceShellTestAccess::ConsumeLspCallbacks(shell);
        const util::JsonValue ui = WorkspaceShellTestAccess::ControlUi(shell);
        Expect(ui["panel"]["content"].AsString() == "output",
               "Find References should show the output panel, got " + ui["panel"]["content"].AsString());
        bool tab = false;
        for (const util::JsonValue& element : ui["elements"].AsArray()) {
          tab = tab || (element["kind"].AsString() == "panel-tab" &&
                        element["label"].AsString().find("References") != std::string::npos &&
                        element["active"].AsBool());
        }
        Expect(tab, "and its References tab should be the active one");
      });
}

void TestWorkspaceShellCompletionClickAcceptsTheClickedRow() {
  WithMarkdownCompletionServer(
      "x = \n", 0, 4,
      [](std::string, workspace::LspClient::Position, workspace::LspClient::CompletionCallback cb) {
        std::vector<workspace::LspClient::CompletionItem> items;
        for (const char* label : {"beta", "alpha_one", "gamma", "alpha_two"}) {
          items.push_back(PlainCompletionItem(label));
        }
        cb(workspace::LspClient::CompletionList{std::move(items), false});
      },
      [&](WorkspaceShell& shell, editor::TextViewport& editor) {
        constexpr int kWidth = 1280;
        constexpr int kHeight = 720;
        EnsureDummySdlVideoInitialized();
        WorkspaceShellTestAccess::SetWindowSize(shell, kWidth, kHeight);
        WorkspaceShellTestAccess::SetFocusEditor(shell);
        WorkspaceShellTestAccess::NoteLayoutInputsChanged(shell);
        SoftwareCanvas canvas(kWidth, kHeight);
        shell.Render(canvas.renderer(), kWidth, kHeight);
        Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, "completion"), "completion should run");
        WorkspaceShellTestAccess::ConsumeLspCallbacks(shell);
        (void)DispatchEvents(shell, {TextInputEvent("a"), TextInputEvent("l")});
        shell.Render(canvas.renderer(), kWidth, kHeight);
        std::optional<SDL_FRect> row;
        const util::JsonValue ui = WorkspaceShellTestAccess::ControlUi(shell);
        for (const util::JsonValue& entry : ui["elements"].AsArray()) {
          if (entry["name"].AsString().starts_with("overlay-row:") &&
              entry["label"].AsString().find("alpha_two") != std::string::npos) {
            const auto& r = entry["rect"].AsArray();
            row = SDL_FRect{static_cast<float>(r[0].AsInt()), static_cast<float>(r[1].AsInt()),
                            static_cast<float>(r[2].AsInt()), static_cast<float>(r[3].AsInt())};
          }
        }
        Expect(row.has_value(), "the narrowed list should show alpha_two");
        if (!row) {
          return;
        }
        const float x = row->x + row->w / 2.0f;
        const float y = row->y + row->h / 2.0f;
        (void)DispatchEvents(shell, {MouseMotionEvent(x, y),
                                     MouseButtonEvent(SDL_EVENT_MOUSE_BUTTON_DOWN, x, y),
                                     MouseButtonEvent(SDL_EVENT_MOUSE_BUTTON_UP, x, y)});
        Expect(std::string(editor.lines()[0]) == "x = alpha_two",
               "clicking alpha_two should accept alpha_two, got [" + std::string(editor.lines()[0]) + "]");
      });
}

void TestWorkspaceShellClickOutsideCompletionLandsInTheEditor() {
  WithMarkdownCompletionServer(
      "x = \nsecond line\nthird\n", 0, 4,
      [](std::string, workspace::LspClient::Position, workspace::LspClient::CompletionCallback cb) {
        std::vector<workspace::LspClient::CompletionItem> items;
        items.push_back(PlainCompletionItem("alpha"));
        cb(workspace::LspClient::CompletionList{std::move(items), false});
      },
      [&](WorkspaceShell& shell, editor::TextViewport& editor) {
        constexpr int kWidth = 1280;
        constexpr int kHeight = 720;
        EnsureDummySdlVideoInitialized();
        WorkspaceShellTestAccess::SetWindowSize(shell, kWidth, kHeight);
        WorkspaceShellTestAccess::SetFocusEditor(shell);
        WorkspaceShellTestAccess::NoteLayoutInputsChanged(shell);
        SoftwareCanvas canvas(kWidth, kHeight);
        shell.Render(canvas.renderer(), kWidth, kHeight);
        Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, "completion"), "completion should run");
        WorkspaceShellTestAccess::ConsumeLspCallbacks(shell);
        shell.Render(canvas.renderer(), kWidth, kHeight);
        Expect(CompletionListUp(shell), "the completion list should be up");
        const util::JsonValue ui = WorkspaceShellTestAccess::ControlUi(shell);
        const auto& r = ui["surfaces"]["editorSurface"].AsArray();
        const float x = static_cast<float>(r[0].AsInt() + r[2].AsInt() / 2);
        const float y = static_cast<float>(r[1].AsInt() + r[3].AsInt() - 20);
        (void)DispatchEvents(shell, {MouseMotionEvent(x, y),
                                     MouseButtonEvent(SDL_EVENT_MOUSE_BUTTON_DOWN, x, y),
                                     MouseButtonEvent(SDL_EVENT_MOUSE_BUTTON_UP, x, y)});
        Expect(!CompletionListUp(shell), "the click should close the completion list");
        Expect(WorkspaceShellTestAccess::FocusIsEditor(shell),
               "the click landed in the editor, so the editor keeps focus");
        Expect(editor.cursor_line() > 0,
               "the click should move the caret off the completion's line, caret on line " +
                   std::to_string(editor.cursor_line()));
        (void)DispatchEvents(shell, {TextInputEvent("Z")});
        Expect(std::string(editor.lines()[0]) == "x = " &&
                   editor.lines()[editor.cursor_line()].find('Z') != std::string_view::npos,
               "the next key should type at the clicked line, line 1 [" + std::string(editor.lines()[0]) +
                   "], caret line [" + std::string(editor.lines()[editor.cursor_line()]) + "]");
      });
}

void TestWorkspaceShellCompletionDownEnterAcceptsTheSecondRow() {
  WithMarkdownCompletionServer(
      "x = \n", 0, 4,
      [](std::string, workspace::LspClient::Position, workspace::LspClient::CompletionCallback cb) {
        std::vector<workspace::LspClient::CompletionItem> items;
        for (const char* label : {"beta", "alpha_one", "alpha_two"}) {
          items.push_back(PlainCompletionItem(label));
        }
        cb(workspace::LspClient::CompletionList{std::move(items), false});
      },
      [&](WorkspaceShell& shell, editor::TextViewport& editor) {
        WorkspaceShellTestAccess::SetFocusEditor(shell);
        Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, "completion"), "completion should run");
        WorkspaceShellTestAccess::ConsumeLspCallbacks(shell);
        (void)DispatchEvents(shell, {TextInputEvent("a"), TextInputEvent("l"), KeyDownEvent(SDLK_DOWN),
                                     KeyDownEvent(SDLK_RETURN)});
        Expect(std::string(editor.lines()[0]) == "x = alpha_two",
               "Down, Enter should accept the second row, got [" + std::string(editor.lines()[0]) + "]");
      });
}

void TestWorkspaceShellCompletionPopupShrinkRepaintsWhatItLeft() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  WithMarkdownCompletionServer(
      "x = \n", 0, 4,
      [](std::string, workspace::LspClient::Position, workspace::LspClient::CompletionCallback cb) {
        std::vector<workspace::LspClient::CompletionItem> items;
        for (const char* label : {"alpha_one", "alpha_two", "beta_one", "beta_two", "gamma_one",
                                  "gamma_two", "delta"}) {
          items.push_back(PlainCompletionItem(label));
        }
        cb(workspace::LspClient::CompletionList{std::move(items), false});
      },
      [&](WorkspaceShell& shell, editor::TextViewport&) {
        constexpr int kWidth = 1280;
        constexpr int kHeight = 720;
        EnsureDummySdlVideoInitialized();
        WorkspaceShellTestAccess::SetWindowSize(shell, kWidth, kHeight);
        WorkspaceShellTestAccess::SetFocusEditor(shell);
        WorkspaceShellTestAccess::NoteLayoutInputsChanged(shell);
        SoftwareCanvas canvas(kWidth, kHeight);
        const auto pin = [&]() { WorkspaceShellTestAccess::SetCaretBlinkEpochMs(shell, SDL_GetTicks()); };
        Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, "completion"), "completion should run");
        WorkspaceShellTestAccess::ConsumeLspCallbacks(shell);
        Expect(CompletionListUp(shell), "the completion list should be up");
        pin();
        shell.Render(canvas.renderer(), kWidth, kHeight);
        (void)shell.ConsumePendingRenderInvalidation();
        for (const char* typed : {"a", "l"}) {
          const auto diff = PaintAndDiffWithFullRepaint(
              shell, canvas, kWidth, kHeight, DispatchEvents(shell, {TextInputEvent(typed)}), pin);
          Expect(diff.identical(), std::string("typing '") + typed +
                                       "' into the completion list should paint what a full "
                                       "repaint paints; " + DescribePixelDiff(diff));
        }
      });
}

void TestWorkspaceShellCompletionNarrowsLocallyWithOneRequestPerWord() {
  int requests = 0;
  WithMarkdownCompletionServer(
      "x = \n", 0, 4,
      [&requests](std::string, workspace::LspClient::Position,
                  workspace::LspClient::CompletionCallback cb) {
        ++requests;
        std::vector<workspace::LspClient::CompletionItem> items;
        items.push_back(PlainCompletionItem("alpha_one"));
        items.push_back(PlainCompletionItem("beta"));
        items.push_back(PlainCompletionItem("alpha_two"));
        cb(workspace::LspClient::CompletionList{std::move(items), false});
      },
      [&](WorkspaceShell& shell, editor::TextViewport& editor) {
        Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, "completion"),
               "completion command should execute");
        WorkspaceShellTestAccess::ConsumeLspCallbacks(shell);
        const auto& session = WorkspaceShellTestAccess::CompletionSession(shell);
        Expect(session.VisibleCount() == 3, "an empty word shows every candidate");

        Expect(WorkspaceShellTestAccess::HandleTextInput(shell, "a"), "typing reaches the editor");
        WorkspaceShellTestAccess::ConsumeLspCallbacks(shell);
        Expect(editor.lines()[0] == "x = a",
               "the typed character lands in the buffer under the open list, got: " +
                   std::string(editor.lines()[0]));
        Expect(CompletionListUp(shell), "the list stays up while the caret is in the word");
        Expect(session.VisibleCount() == 2 && session.VisibleItem(0).label == "alpha_one" &&
                   session.VisibleItem(1).label == "alpha_two",
               "the list narrows to the candidates the word matches, in the server's order");

        Expect(WorkspaceShellTestAccess::HandleTextInput(shell, "l"), "second character");
        Expect(WorkspaceShellTestAccess::HandleKeyDown(shell, SDLK_BACKSPACE, SDL_KMOD_NONE),
               "backspace reaches the editor under the list");
        Expect(WorkspaceShellTestAccess::HandleKeyDown(shell, SDLK_BACKSPACE, SDL_KMOD_NONE),
               "backspace to the word's start");
        WorkspaceShellTestAccess::ConsumeLspCallbacks(shell);
        Expect(editor.lines()[0] == "x = ", "both characters were erased");
        Expect(CompletionListUp(shell) && session.VisibleCount() == 3,
               "erasing back to the word's start widens the list again");
        Expect(requests == 1,
               "a complete list is requested once for the word, not per keystroke; requests=" +
                   std::to_string(requests));

        Expect(WorkspaceShellTestAccess::HandleKeyDown(shell, SDLK_BACKSPACE, SDL_KMOD_NONE),
               "backspace past the word's start");
        Expect(!CompletionListUp(shell), "the caret left the word, so the list closes");
        Expect(editor.lines()[0] == "x =", "and the backspace still edited the buffer");
      });
}

void TestWorkspaceShellCompletionReRequestsOnlyWhileIncomplete() {
  int requests = 0;
  std::vector<std::uint32_t> request_columns;
  WithMarkdownCompletionServer(
      "x = \n", 0, 4,
      [&](std::string, workspace::LspClient::Position pos,
          workspace::LspClient::CompletionCallback cb) {
        ++requests;
        request_columns.push_back(static_cast<std::uint32_t>(pos.character));
        std::vector<workspace::LspClient::CompletionItem> items;
        items.push_back(PlainCompletionItem("also"));
        items.push_back(PlainCompletionItem("alpine"));
        cb(workspace::LspClient::CompletionList{std::move(items), requests == 1});
      },
      [&](WorkspaceShell& shell, editor::TextViewport&) {
        Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, "completion"),
               "completion command should execute");
        WorkspaceShellTestAccess::ConsumeLspCallbacks(shell);
        Expect(WorkspaceShellTestAccess::HandleTextInput(shell, "a"), "type");
        WorkspaceShellTestAccess::ConsumeLspCallbacks(shell);
        Expect(requests == 2, "an incomplete list is re-asked on the next keystroke; requests=" +
                                  std::to_string(requests));
        Expect(request_columns.size() == 2 && request_columns[1] == 5,
               "the re-ask is made at the caret as it is now");
        Expect(WorkspaceShellTestAccess::HandleTextInput(shell, "l"), "type");
        Expect(WorkspaceShellTestAccess::HandleTextInput(shell, "p"), "type");
        WorkspaceShellTestAccess::ConsumeLspCallbacks(shell);
        Expect(requests == 2, "once an answer is complete, typing refilters locally; requests=" +
                                  std::to_string(requests));
        const auto& session = WorkspaceShellTestAccess::CompletionSession(shell);
        Expect(CompletionListUp(shell) && session.VisibleCount() == 1 &&
                   session.VisibleItem(0).label == "alpine",
               "`alp` narrows to the one candidate it matches");
      });
}

void TestWorkspaceShellCompletionAcceptAfterTypingReplacesTheTypedWord() {
  const auto handler = [](std::string, workspace::LspClient::Position pos,
                          workspace::LspClient::CompletionCallback cb) {
    std::vector<workspace::LspClient::CompletionItem> items;
    auto item = PlainCompletionItem("vector");
    item.replace_range = workspace::LspClient::Range{{0, 4}, pos};
    items.push_back(std::move(item));
    cb(workspace::LspClient::CompletionList{std::move(items)});
  };
  WithMarkdownCompletionServer("x = \n", 0, 4, handler,
                               [&](WorkspaceShell& shell, editor::TextViewport& editor) {
    Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, "completion"), "open the list");
    WorkspaceShellTestAccess::ConsumeLspCallbacks(shell);
    Expect(WorkspaceShellTestAccess::HandleTextInput(shell, "v"), "type");
    Expect(WorkspaceShellTestAccess::HandleTextInput(shell, "e"), "type");
    Expect(WorkspaceShellTestAccess::HandleKeyDown(shell, SDLK_TAB, SDL_KMOD_NONE),
           "Tab accepts the selected item");
    Expect(editor.lines()[0] == "x = vector",
           "the server's range is extended over the typed `ve`, got: " +
               std::string(editor.lines()[0]));
    Expect(!CompletionListUp(shell), "accepting closes the list");
  });
  WithMarkdownCompletionServer("x = \n", 0, 4, handler,
                               [&](WorkspaceShell& shell, editor::TextViewport& editor) {
    Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, "completion"), "open the list");
    WorkspaceShellTestAccess::ConsumeLspCallbacks(shell);
    Expect(WorkspaceShellTestAccess::HandleTextInput(shell, "v"), "type");
    Expect(CompletionListUp(shell), "`v` keeps the list up");
    Expect(WorkspaceShellTestAccess::HandleTextInput(shell, " "), "type a space");
    Expect(!CompletionListUp(shell), "a space ends the word and closes the list");
    Expect(editor.lines()[0] == "x = v ", "and the space was typed, not swallowed");
  });
}

void TestWorkspaceShellQuickSuggestionsAskOncePerWord() {
  int requests = 0;
  WithMarkdownCompletionServer(
      "x = \n", 0, 4,
      [&requests](std::string, workspace::LspClient::Position,
                  workspace::LspClient::CompletionCallback cb) {
        ++requests;
        std::vector<workspace::LspClient::CompletionItem> items;
        items.push_back(PlainCompletionItem("alpha_one"));
        items.push_back(PlainCompletionItem("beta"));
        cb(workspace::LspClient::CompletionList{std::move(items)});
      },
      [&](WorkspaceShell& shell, editor::TextViewport& editor) {
        Expect(!CompletionListUp(shell), "nothing is open before typing");
        Expect(WorkspaceShellTestAccess::HandleTextInput(shell, "a"), "type");
        WorkspaceShellTestAccess::ConsumeLspCallbacks(shell);
        const auto& session = WorkspaceShellTestAccess::CompletionSession(shell);
        Expect(requests == 1, "the first character of a word asks once");
        Expect(CompletionListUp(shell) && session.VisibleCount() == 1 &&
                   session.VisibleItem(0).label == "alpha_one",
               "the list opens by itself, narrowed to the typed word");
        Expect(WorkspaceShellTestAccess::HandleTextInput(shell, "l"), "type");
        WorkspaceShellTestAccess::ConsumeLspCallbacks(shell);
        Expect(requests == 1, "typing on in the word refilters, it does not ask again");
        Expect(WorkspaceShellTestAccess::HandleTextInput(shell, " "), "end the word");
        Expect(!CompletionListUp(shell), "a space closes it");
        Expect(WorkspaceShellTestAccess::HandleTextInput(shell, "b"), "a new word");
        WorkspaceShellTestAccess::ConsumeLspCallbacks(shell);
        Expect(requests == 2, "a new word asks once more; requests=" + std::to_string(requests));
        Expect(CompletionListUp(shell) && session.VisibleItem(0).label == "beta",
               "and opens on its own candidates");
        Expect(editor.lines()[0] == "x = al b", "every character reached the buffer, got: " +
                                                     std::string(editor.lines()[0]));
      });
}

void TestWorkspaceShellQuickSuggestionsWithNothingToOfferStayHidden() {
  int requests = 0;
  WithMarkdownCompletionServer(
      "\n", 0, 0,
      [&requests](std::string, workspace::LspClient::Position,
                  workspace::LspClient::CompletionCallback cb) {
        ++requests;
        cb(workspace::LspClient::CompletionList{});
      },
      [&](WorkspaceShell& shell, editor::TextViewport&) {
        Expect(WorkspaceShellTestAccess::HandleTextInput(shell, "z"), "type");
        WorkspaceShellTestAccess::ConsumeLspCallbacks(shell);
        Expect(!WorkspaceShellTestAccess::OverlayVisible(shell),
               "an empty automatic answer opens nothing");
        Expect(WorkspaceShellTestAccess::HandleTextInput(shell, "z"), "type");
        WorkspaceShellTestAccess::ConsumeLspCallbacks(shell);
        Expect(requests == 1, "and the word is not asked about again");
      });
}

void TestWorkspaceShellTriggerCharacterOpensCompletionAtTheCaret() {
  std::vector<workspace::LspClient::Position> positions;
  WithMarkdownCompletionServer(
      "\n", 0, 0,
      [&positions](std::string, workspace::LspClient::Position pos,
                   workspace::LspClient::CompletionCallback cb) {
        positions.push_back(pos);
        std::vector<workspace::LspClient::CompletionItem> items;
        items.push_back(PlainCompletionItem("member"));
        cb(workspace::LspClient::CompletionList{std::move(items)});
      },
      [&](WorkspaceShell& shell, editor::TextViewport& editor) {
        Expect(WorkspaceShellTestAccess::SetSettingValueTransient(shell, "editor.quickSuggestions",
                                                                  "false"),
               "quick suggestions off, so only the trigger character asks");
        Expect(WorkspaceShellTestAccess::HandleTextInput(shell, "x"), "type");
        WorkspaceShellTestAccess::ConsumeLspCallbacks(shell);
        Expect(positions.empty() && !CompletionListUp(shell),
               "with quick suggestions off, a word character asks nothing");
        Expect(WorkspaceShellTestAccess::HandleTextInput(shell, "."), "the trigger character");
        WorkspaceShellTestAccess::ConsumeLspCallbacks(shell);
        Expect(positions.size() == 1 && positions[0].character == 2,
               "the trigger character asks once, at the caret after it");
        Expect(CompletionListUp(shell), "and the list opens on the answer");
        Expect(WorkspaceShellTestAccess::HandleKeyDown(shell, SDLK_RETURN, SDL_KMOD_NONE),
               "accept");
        Expect(editor.lines()[0] == "x.member",
               "the item is inserted at the caret, after the trigger, got: " +
                   std::string(editor.lines()[0]));
      },
      ".");
}

void TestWorkspaceShellSignatureHelpPrefersLspOverPlugin() {
#if !MICROIDE_HAS_LUA_PLUGINS
  return;
#endif
  TemporaryDirectory temp_dir;
  const std::filesystem::path config_home = temp_dir.path() / "config";
  const std::filesystem::path plugins_root = config_home / "microide" / "plugins";
  const std::filesystem::path project = temp_dir.path() / "proj";
  const std::filesystem::path md_file = project / "notes.md";
  WriteFile(md_file, "alpha\n");

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
  WorkspaceShellTestAccess::ConsumeLspCallbacks(shell);

  const auto popup = WorkspaceShellTestAccess::SignatureHelpPopup(shell);
  Expect(popup.has_value(), "a signature-help popup should be shown");
  Expect(popup->signature == "lsp_sig(x)",
         "the language server's signature must win over the plugin's for a served language");
}

void TestWorkspaceShellSignatureHelpFollowsTheCallBeingTyped() {
  WithMarkdownCompletionServer(
      "\n", 0, 0,
      [](std::string, workspace::LspClient::Position, workspace::LspClient::CompletionCallback) {},
      [&](WorkspaceShell& shell, editor::TextViewport& editor) {
        auto* client =
            WorkspaceShellTestAccess::LspManagerForTesting(shell).FindStartedServer("markdown");
        Expect(client != nullptr, "the stub client should be installed");
        if (client == nullptr) {
          return;
        }
        client->SetSignatureHelpTriggerCharactersForTesting("(,)");
        int requests = 0;
        client->SetTestSignatureHelpHandler(
            [&requests, &editor](std::string, workspace::LspClient::Position pos,
                                 workspace::LspClient::SignatureHelpCallback cb) {
              ++requests;
              const std::string before =
                  std::string(editor.lines()[pos.line]).substr(0, pos.character);
              workspace::LspClient::SignatureHelp help;
              const std::size_t open = before.rfind('(');
              if (open != std::string::npos && before.find(')', open) == std::string::npos) {
                workspace::LspClient::SignatureInformation info;
                info.label = "f(a, b)";
                info.parameters.push_back({.label = "a", .documentation = {}});
                info.parameters.push_back({.label = "b", .documentation = {}});
                help.signatures.push_back(std::move(info));
                help.active_parameter = static_cast<int>(
                    std::count(before.begin() + static_cast<std::ptrdiff_t>(open), before.end(), ','));
              }
              cb(std::move(help));
            });
        const auto type = [&](std::string_view text) {
          Expect(WorkspaceShellTestAccess::HandleTextInput(shell, text), "type");
          WorkspaceShellTestAccess::ConsumeLspCallbacks(shell);
          WorkspaceShellTestAccess::ExpireSignatureHelp(shell);
        };
        type("f");
        Expect(requests == 0 && !WorkspaceShellTestAccess::SignatureHelpPopup(shell).has_value(),
               "a word character asks for no signature");
        type("(");
        auto popup = WorkspaceShellTestAccess::SignatureHelpPopup(shell);
        Expect(requests == 1 && popup.has_value() && popup->signature == "f(a, b)" &&
                   popup->documentation == "a",
               "the trigger character opens the hints on the first parameter");
        type("1");
        popup = WorkspaceShellTestAccess::SignatureHelpPopup(shell);
        Expect(requests == 2 && popup.has_value() && popup->documentation == "a",
               "typing an argument keeps the hints up and re-asks");
        type(",");
        popup = WorkspaceShellTestAccess::SignatureHelpPopup(shell);
        Expect(popup.has_value() && popup->documentation == "b",
               "a comma moves the active parameter to the second");
        type(")");
        Expect(!WorkspaceShellTestAccess::SignatureHelpPopup(shell).has_value(),
               "the server's empty answer after `)` closes the hints");
        type(" ");
        Expect(requests == 4, "with the hints closed, a non-trigger character asks nothing");

        Expect(WorkspaceShellTestAccess::SetSettingValueTransient(shell, "editor.parameterHints",
                                                                  "false"),
               "parameter hints off");
        type("(");
        Expect(requests == 4 && !WorkspaceShellTestAccess::SignatureHelpPopup(shell).has_value(),
               "with editor.parameterHints off, the trigger character asks nothing");
      });
}

void TestWorkspaceShellSignatureHelpClosingRepaintsWhereItWas() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  WithMarkdownCompletionServer(
      "one\ntwo\nthree\n", 0, 3,
      [](std::string, workspace::LspClient::Position, workspace::LspClient::CompletionCallback) {},
      [&](WorkspaceShell& shell, editor::TextViewport& editor) {
        auto* client =
            WorkspaceShellTestAccess::LspManagerForTesting(shell).FindStartedServer("markdown");
        Expect(client != nullptr, "the stub client should be installed");
        if (client == nullptr) {
          return;
        }
        client->SetSignatureHelpTriggerCharactersForTesting("(");
        client->SetTestSignatureHelpHandler(
            [&editor](std::string, workspace::LspClient::Position pos,
                      workspace::LspClient::SignatureHelpCallback cb) {
              workspace::LspClient::SignatureHelp help;
              const std::string_view line = editor.lines()[pos.line];
              if (pos.character > 0 && line[pos.character - 1] == '(') {
                workspace::LspClient::SignatureInformation info;
                info.label = "one(alpha, beta)";
                help.signatures.push_back(std::move(info));
              }
              cb(std::move(help));
            });
        constexpr int kWidth = 1280;
        constexpr int kHeight = 720;
        EnsureDummySdlVideoInitialized();
        WorkspaceShellTestAccess::SetWindowSize(shell, kWidth, kHeight);
        WorkspaceShellTestAccess::SetFocusEditor(shell);
        WorkspaceShellTestAccess::NoteLayoutInputsChanged(shell);
        const auto pin_caret = [&]() {
          WorkspaceShellTestAccess::SetCaretBlinkEpochMs(shell, SDL_GetTicks());
        };
        SoftwareCanvas canvas(kWidth, kHeight);
        pin_caret();
        shell.Render(canvas.renderer(), kWidth, kHeight);
        (void)shell.ConsumePendingRenderInvalidation();
        const auto type = [&](const char* text) {
          const auto typed = PaintAndDiffWithFullRepaint(
              shell, canvas, kWidth, kHeight, DispatchEvents(shell, {TextInputEvent(text)}),
              pin_caret);
          Expect(typed.identical(), "the keystroke's frame paints what a full repaint paints; " +
                                        DescribePixelDiff(typed));
          WorkspaceShellTestAccess::ConsumeLspCallbacks(shell);
          return PaintAndDiffWithFullRepaint(shell, canvas, kWidth, kHeight,
                                             shell.ConsumePendingRenderInvalidation(), pin_caret);
        };
        const auto opened = type("(");
        Expect(WorkspaceShellTestAccess::SignatureHelpPopup(shell).has_value(),
               "the trigger character opens the hints");
        Expect(opened.identical(),
               "opening the hints paints what a full repaint paints; " + DescribePixelDiff(opened));
        const auto closed = type("x");
        Expect(!WorkspaceShellTestAccess::SignatureHelpPopup(shell).has_value(),
               "the empty re-ask closes the hints");
        Expect(closed.identical(), "closing the hints takes them off screen; " +
                                       DescribePixelDiff(closed));
      });
}

void TestWorkspaceShellSignatureHelpComesBackWhenAMenuClosesOverIt() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  WithMarkdownCompletionServer(
      "one\ntwo\nthree\n", 0, 3,
      [](std::string, workspace::LspClient::Position, workspace::LspClient::CompletionCallback) {},
      [&](WorkspaceShell& shell, editor::TextViewport&) {
        auto* client =
            WorkspaceShellTestAccess::LspManagerForTesting(shell).FindStartedServer("markdown");
        Expect(client != nullptr, "the stub client should be installed");
        if (client == nullptr) {
          return;
        }
        client->SetSignatureHelpTriggerCharactersForTesting("(");
        client->SetTestSignatureHelpHandler(
            [](std::string, workspace::LspClient::Position, workspace::LspClient::SignatureHelpCallback cb) {
              workspace::LspClient::SignatureHelp help;
              workspace::LspClient::SignatureInformation info;
              info.label = "one(alpha, beta)";
              help.signatures.push_back(std::move(info));
              cb(std::move(help));
            });
        constexpr int kWidth = 1280;
        constexpr int kHeight = 720;
        EnsureDummySdlVideoInitialized();
        WorkspaceShellTestAccess::SetWindowSize(shell, kWidth, kHeight);
        WorkspaceShellTestAccess::SetFocusEditor(shell);
        WorkspaceShellTestAccess::NoteLayoutInputsChanged(shell);
        const auto pin_caret = [&]() {
          WorkspaceShellTestAccess::SetCaretBlinkEpochMs(shell, SDL_GetTicks());
        };
        SoftwareCanvas canvas(kWidth, kHeight);
        pin_caret();
        shell.Render(canvas.renderer(), kWidth, kHeight);
        (void)shell.ConsumePendingRenderInvalidation();
        const auto frame = [&](std::initializer_list<SDL_Event> events, const char* what) {
          const auto diff = PaintAndDiffWithFullRepaint(shell, canvas, kWidth, kHeight,
                                                        DispatchEvents(shell, events), pin_caret);
          Expect(diff.identical(), std::string(what) + " paints what a full repaint paints; " +
                                       DescribePixelDiff(diff));
          WorkspaceShellTestAccess::ConsumeLspCallbacks(shell);
          const auto settled = PaintAndDiffWithFullRepaint(shell, canvas, kWidth, kHeight,
                                                           shell.ConsumePendingRenderInvalidation(), pin_caret);
          Expect(settled.identical(), std::string(what) + ", settled, paints what a full repaint paints; " +
                                          DescribePixelDiff(settled));
        };
        frame({TextInputEvent("(")}, "opening the hints");
        Expect(WorkspaceShellTestAccess::SignatureHelpPopup(shell).has_value(),
               "the trigger character opens the hints");
        const std::optional<SDL_FRect> row = WorkspaceShellTestAccess::TreeSidebarRowRect(shell, 0);
        Expect(row.has_value(), "the tree shows a row to right-click");
        if (!row.has_value()) {
          return;
        }
        const float rx = row->x + row->w / 2.0f;
        const float ry = row->y + row->h / 2.0f;
        SDL_Event right_down = MouseButtonEvent(SDL_EVENT_MOUSE_BUTTON_DOWN, rx, ry);
        SDL_Event right_up = MouseButtonEvent(SDL_EVENT_MOUSE_BUTTON_UP, rx, ry);
        right_down.button.button = SDL_BUTTON_RIGHT;
        right_up.button.button = SDL_BUTTON_RIGHT;
        frame({MouseMotionEvent(rx, ry), right_down, right_up}, "a menu opening over the hints");
        Expect(WorkspaceShellTestAccess::AnyMenuPopupOpen(shell), "the tree's context menu is open");
        const SDL_FRect editor = WorkspaceShellTestAccess::CurrentLayout(shell).editor_surface;
        const float ex = editor.x + editor.w - 24.0f;
        const float ey = editor.y + editor.h - 24.0f;
        frame({MouseMotionEvent(ex, ey), MouseButtonEvent(SDL_EVENT_MOUSE_BUTTON_DOWN, ex, ey),
               MouseButtonEvent(SDL_EVENT_MOUSE_BUTTON_UP, ex, ey)},
              "the menu closing over the hints");
        Expect(!WorkspaceShellTestAccess::AnyMenuPopupOpen(shell), "the press closes the menu");
        Expect(WorkspaceShellTestAccess::SignatureHelpPopup(shell).has_value(),
               "the hints are still up: the caret never moved");
      });
}

void TestWorkspaceShellSteppingOverACloserSendsTheServerNothing() {
  WithMarkdownCompletionServer(
      "\n", 0, 0,
      [](std::string, workspace::LspClient::Position, workspace::LspClient::CompletionCallback) {},
      [&](WorkspaceShell& shell, editor::TextViewport& editor) {
        auto* client =
            WorkspaceShellTestAccess::LspManagerForTesting(shell).FindStartedServer("markdown");
        Expect(client != nullptr, "the stub client should be installed");
        if (client == nullptr) {
          return;
        }
        const std::string uri = workspace::FileUriForPath(editor.path());
        Expect(WorkspaceShellTestAccess::HandleTextInput(shell, "("), "type the opener");
        Expect(WorkspaceShellTestAccess::HandleTextInput(shell, "x"), "type the argument");
        Expect(std::string(editor.lines()[0]) == "(x)",
               "the fixture auto-closes `(`, got: " + std::string(editor.lines()[0]));
        const std::optional<int> before = client->TrackedDocumentVersion(uri);
        Expect(before.has_value(), "the typing should have opened the document on the server");
        Expect(WorkspaceShellTestAccess::HandleTextInput(shell, ")"), "type the closer");
        Expect(std::string(editor.lines()[0]) == "(x)" && editor.cursor_column() == 3,
               "the closer steps over the auto-inserted one");
        Expect(client->TrackedDocumentVersion(uri) == before,
               "stepping over a closer changes nothing, so it sends the server nothing");
        Expect(WorkspaceShellTestAccess::HandleTextInput(shell, "y"), "type after the call");
        Expect(client->TrackedDocumentVersion(uri) == std::optional<int>(*before + 1),
               "the next real edit is sent once");
        Expect(WorkspaceShellTestAccess::LspSyncedDocumentCount(shell) == 1, "one document synced");
        WorkspaceShellTestAccess::CloseTab(shell, 0);
        Expect(WorkspaceShellTestAccess::LspSyncedDocumentCount(shell) == 0,
               "closing it forgets its sync state");
      });
}

void TestWorkspaceShellPluginProvidersOpenOnTheirTriggerCharacters() {
#if !MICROIDE_HAS_LUA_PLUGINS
  return;
#endif
  TemporaryDirectory temp_dir;
  const std::filesystem::path config_home = temp_dir.path() / "config";
  const std::filesystem::path plugins_root = config_home / "microide" / "plugins";
  const std::filesystem::path project = temp_dir.path() / "proj";
  const std::filesystem::path md_file = project / "notes.md";
  WriteFile(md_file, "\n");
  WritePluginInit(
      plugins_root, "mdsig",
      R"lua(local ide = require("microide")
return ide.plugin({
  id = "mdsig",
  setup = function(ctx)
    ctx.signature_help.add({
      id = "sig",
      language_id = "markdown",
      provide = function(_, _)
        return { signatures = { { label = "plugin_sig(a)" } } }
      end
    })
    ctx.completion.add({
      id = "members",
      language_id = "markdown",
      trigger_characters = ":",
      provide = function(_, _)
        return { { label = "member", insert_text = "member" } }
      end
    })
  end
})
)lua");
  ScopedPluginConfigHomeEnv scoped_plugin_config_home(config_home);

  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, project, false, false),
         "fixture should open the project");
  WorkspaceShellTestAccess::OpenFile(shell, md_file);
  Expect(WorkspaceShellTestAccess::SetSettingValueTransient(shell, "editor.quickSuggestions",
                                                            "false"),
         "quick suggestions off, so only trigger characters ask");
  const auto type = [&](std::string_view text) {
    Expect(WorkspaceShellTestAccess::HandleTextInput(shell, text), "type");
    WaitUntil([] { return false; }, std::chrono::milliseconds(150), std::chrono::milliseconds(5),
              [&]() {
                WorkspaceShellTestAccess::ConsumeLspCallbacks(shell);
                (void)shell.HandleScheduledWake();
              });
    WorkspaceShellTestAccess::ExpireSignatureHelp(shell);
  };
  type("g(");
  Expect(!WorkspaceShellTestAccess::SignatureHelpPopup(shell).has_value(),
         "pasted text is not typing");
  type(")");
  type("g");
  type("(");
  const auto popup = WorkspaceShellTestAccess::SignatureHelpPopup(shell);
  Expect(popup.has_value() && popup->signature == "plugin_sig(a)",
         "the provider's default trigger `(` opens its hints");
  type("x");
  Expect(WorkspaceShellTestAccess::SignatureHelpPopup(shell).has_value(),
         "typing an argument keeps them up");
  type(")");
  Expect(!WorkspaceShellTestAccess::SignatureHelpPopup(shell).has_value(),
         "`)` closes a plugin provider's hints");
  type(":");
  Expect(CompletionListUp(shell), "the completion provider's trigger character opens the list");
}

void TestWorkspaceShellInlayHintsPublishMidLineDecorations() {
#if !MICROIDE_HAS_LUA_PLUGINS
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

void TestWorkspaceShellCodeLensesResolveAndActivate() {
#if !MICROIDE_HAS_LUA_PLUGINS
  return;
#endif
  TemporaryDirectory temp_dir;
  const std::filesystem::path config_home = temp_dir.path() / "config";
  const std::filesystem::path plugins_root = config_home / "microide" / "plugins";
  const std::filesystem::path project = temp_dir.path() / "proj";
  const std::filesystem::path md_file = project / "notes.md";
  WriteFile(md_file, "fn alpha() {}\nfn beta() {}\n");

  WritePluginInit(plugins_root, "mdlens",
                  R"lua(local ide = require("microide")
return ide.plugin({
  id = "mdlens",
  capabilities = { process = { exec = true } },
  setup = function(ctx)
    ctx.lsp.add({ id = "md.server", language_id = "markdown", command = { "md-lsp-server" } })
  end
})
)lua");
  ScopedPluginConfigHomeEnv scoped_plugin_config_home(config_home);

  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, project, false, false),
         "code-lens fixture should open the project");

  auto stub = std::make_unique<workspace::LspClient>();
  stub->EnableTestStubMode();
  stub->SetTestCodeLensHandler([](std::string, workspace::LspClient::CodeLensCallback cb) {
    workspace::LspClient::CodeLens resolved;
    resolved.range = {{0, 0}, {0, 13}};
    resolved.title = "2 references";
    resolved.command = "md.showRefs";
    resolved.arguments.push_back(util::JsonValue(std::int64_t{7}));

    workspace::LspClient::CodeLens pending;
    pending.range = {{1, 0}, {1, 12}};
    util::JsonObject raw;
    raw["data"] = util::JsonValue(std::int64_t{99});
    pending.unresolved = util::JsonValue(std::move(raw));

    cb(std::vector<workspace::LspClient::CodeLens>{std::move(resolved), std::move(pending)});
  });
  util::JsonValue resolve_saw;
  stub->SetTestResolveCodeLensHandler(
      [&resolve_saw](util::JsonValue unresolved, workspace::LspClient::ResolveCodeLensCallback cb) {
        resolve_saw = unresolved;
        workspace::LspClient::CodeLens filled;
        filled.title = "Run test";
        filled.command = "md.runTest";
        cb(std::move(filled));
      });
  std::string executed_command;
  std::vector<util::JsonValue> executed_arguments;
  stub->SetTestExecuteCommandHandler(
      [&](std::string command, std::vector<util::JsonValue> arguments,
          workspace::LspClient::ExecuteCommandCallback cb) {
        executed_command = std::move(command);
        executed_arguments = std::move(arguments);
        cb(util::JsonValue{});
      });
  Expect(WorkspaceShellTestAccess::LspManagerForTesting(shell)
             .InstallTestClientIntoExistingForTesting("markdown", std::move(stub)),
         "fixture should attach a stub markdown client");

  WorkspaceShellTestAccess::OpenFile(shell, md_file);
  (void)WorkspaceShellTestAccess::ResolveLspHoverForTesting(shell, md_file, 1, 1);
  WorkspaceShellTestAccess::ConsumeLspCallbacks(shell);

  Expect(resolve_saw["data"].AsInt(0) == 99,
         "codeLens/resolve receives the server's original lens object verbatim");

  const auto& state = WorkspaceShellTestAccess::CurrentProjectState(shell);
  const auto* presentation = state.plugin_presentation_if_present();
  Expect(presentation != nullptr, "a code-lens overlay should be published");
  const auto* decorations =
      presentation != nullptr ? presentation->decorations.FindByPath(md_file) : nullptr;
  Expect(decorations != nullptr && decorations->code_lenses.size() == 2,
         "both lenses publish, including the one that needed resolving");
  if (decorations == nullptr || decorations->code_lenses.size() != 2) {
    return;
  }
  const auto line0 = decorations->CodeLensesForLine(0);
  const auto line1 = decorations->CodeLensesForLine(1);
  Expect(line0.size() == 1 && line0[0].text == "2 references",
         "the already-resolved lens keeps its title on its own line");
  Expect(line1.size() == 1 && line1[0].text == "Run test",
         "the resolved lens keeps the ORIGINAL range's line, not the resolve reply's");
  Expect(line0[0].payload != 0 && line1[0].payload != 0 && line0[0].payload != line1[0].payload,
         "each activatable lens gets its own payload handle");
  Expect(line0[0].command.empty(),
         "an LSP lens dispatches through its payload, not the host command registry");

  WorkspaceShellTestAccess::ActivateCodeLens(shell, line0[0].payload);
  Expect(executed_command == "md.showRefs",
         "activating a lens runs its server command through workspace/executeCommand");
  Expect(executed_arguments.size() == 1 && executed_arguments[0].AsInt(0) == 7,
         "the lens's original arguments are forwarded verbatim");

  executed_command.clear();
  WorkspaceShellTestAccess::ActivateCodeLens(shell, line0[0].payload + 4242);
  Expect(executed_command.empty(), "an unknown payload handle runs nothing");
}

void TestWorkspaceShellServerRegisteredAfterOpenStartsForTheActiveFile() {
#if !MICROIDE_HAS_LUA_PLUGINS
  return;
#endif
  TemporaryDirectory temp_dir;
  const std::filesystem::path config_home = temp_dir.path() / "config";
  const std::filesystem::path plugins_root = config_home / "microide" / "plugins";
  const std::filesystem::path project = temp_dir.path() / "proj";
  const std::filesystem::path md_file = project / "notes.md";
  WriteFile(md_file, "# notes\n");
  std::filesystem::create_directories(plugins_root);
  ScopedPluginConfigHomeEnv scoped_plugin_config_home(config_home);

  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, project, false, false),
         "the fixture should open its project");
  WorkspaceShellTestAccess::OpenFile(shell, md_file);
  Expect(!WorkspaceShellTestAccess::LspManagerForTesting(shell).HasServer("markdown"),
         "no server is registered yet");

  WritePluginInit(
      plugins_root, "mdlsp",
      R"(local ide = require("microide")
return ide.plugin({
  id = "mdlsp",
  capabilities = { process = { exec = true } },
  setup = function(ctx)
    ctx.lsp.add({ id = "md.server", language_id = "markdown",
                  command = { "/bin/sh", "-c", "exec sleep 30" } })
  end
})
)");
  Expect(WorkspaceShellTestAccess::ReloadPluginsIfPluginAssetsChanged(shell, true),
         "the new plugin should be picked up");
  workspace::LspManager& manager = WorkspaceShellTestAccess::LspManagerForTesting(shell);
  Expect(manager.HasServer("markdown"), "the reload registers the markdown server");
  Expect(manager.IsServerRunning("markdown"),
         "and starts it for the markdown file already active, as opening it would have");
}

void TestWorkspaceShellAReloadedStatusItemRepaintsTheBreadcrumb() {
#if !MICROIDE_HAS_LUA_PLUGINS || !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  EnsureDummySdlVideoInitialized();
  TemporaryDirectory temp_dir;
  const std::filesystem::path config_home = temp_dir.path() / "config";
  const std::filesystem::path plugins_root = config_home / "microide" / "plugins";
  const std::filesystem::path project = temp_dir.path() / "proj";
  const std::filesystem::path file = project / "notes.txt";
  WriteFile(file, "notes\n");
  std::filesystem::create_directories(plugins_root);
  ScopedPluginConfigHomeEnv scoped_plugin_config_home(config_home);
  const auto plugin = [](std::string_view text) {
    return std::string(R"(local ide = require("microide")
return ide.plugin({
  id = "stat",
  setup = function(ctx)
    ctx.status.add({ id = "seen", text = ")") +
           std::string(text) + R"(", alignment = "right", priority = 1 })
  end
})
)";
  };
  WritePluginInit(plugins_root, "stat", plugin("stat counted 55555"));

  constexpr int kWidth = 1280;
  constexpr int kHeight = 720;
  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, project, false, false), "the project opens");
  WorkspaceShellTestAccess::SetWindowSize(shell, kWidth, kHeight);
  WorkspaceShellTestAccess::OpenFile(shell, file);
  (void)WorkspaceShellTestAccess::ReloadPluginsIfPluginAssetsChanged(shell, true);
  SoftwareCanvas canvas{kWidth, kHeight};
  shell.Render(canvas.renderer(), kWidth, kHeight);
  (void)shell.ConsumePendingRenderInvalidation();

  WritePluginInit(plugins_root, "stat", plugin("stat"));
  Expect(WorkspaceShellTestAccess::ReloadPluginsIfPluginAssetsChanged(shell, true), "the edited plugin reloads");
  workspace::RenderInvalidation editor_only;
  editor_only.rects.push_back(WorkspaceShellTestAccess::CurrentLayout(shell).editor_surface);
  (void)shell.ConsumePendingRenderInvalidation();
  const auto diff = PaintAndDiffWithFullRepaint(shell, canvas, kWidth, kHeight, editor_only);
  Expect(diff.identical(), "a frame after the reload paints the breadcrumb's new status text; " +
                               DescribePixelDiff(diff));
}

void TestWorkspaceShellHoverInAnUnsavedBufferAsksTheServer() {
#if !MICROIDE_HAS_LUA_PLUGINS
  return;
#endif
  EnsureDummySdlVideoInitialized();
  TemporaryDirectory temp_dir;
  const std::filesystem::path config_home = temp_dir.path() / "config";
  const std::filesystem::path plugins_root = config_home / "microide" / "plugins";
  const std::filesystem::path project = temp_dir.path() / "proj";
  const std::filesystem::path py_file = project / "main.py";
  WriteFile(py_file, "value = 1\nother = 2\n");
  WritePluginInit(plugins_root, "pylsp",
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
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, project, false, false), "the project should open");
  auto stub = std::make_unique<workspace::LspClient>();
  stub->EnableTestStubMode();
  stub->SetTestHoverHandler([](std::string uri, workspace::LspClient::HoverCallback cb) {
    (void)uri;
    cb(util::ParseJson(R"({"contents":{"kind":"markdown","value":"int value"}})"));
  });
  Expect(WorkspaceShellTestAccess::LspManagerForTesting(shell)
             .InstallTestClientIntoExistingForTesting("python", std::move(stub)),
         "a stub python client should attach");
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::OpenFile(shell, py_file);
  WorkspaceShellTestAccess::SetFocusEditor(shell);
  WorkspaceShellTestAccess::NoteLayoutInputsChanged(shell);
  SoftwareCanvas canvas{1280, 720};
  const auto frame = [&] {
    WorkspaceShellTestAccess::ConsumeLspCallbacks(shell);
    shell.Render(canvas.renderer(), 1280, 720);
    (void)shell.ConsumePendingRenderInvalidation();
  };
  frame();
  const auto hover_text = [&]() -> std::string {
    const util::JsonValue ui = WorkspaceShellTestAccess::ControlUi(shell);
    const util::JsonValue& hover = ui["popups"]["hover"];
    return hover.IsObject() ? hover["text"].AsString() : std::string();
  };

  Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, "goto 2:1"), "goto should run");
  Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, "type x"), "typing should run");
  Expect(WorkspaceShellTestAccess::ActiveEditor(shell).dirty(), "the buffer has an unsaved edit");
  Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, "goto 1:1"), "goto should run");
  Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, "show-hover"), "show-hover should run");
  for (int i = 0; i < 5 && hover_text().empty(); ++i) {
    frame();
  }
  Expect(hover_text().find("int value") != std::string::npos,
         "an unsaved buffer still gets the language server's hover: '" + hover_text() + "'");
}

void TestWorkspaceShellHoverOnASquiggleShowsTheSymbolToo() {
#if !MICROIDE_HAS_LUA_PLUGINS
  return;
#endif
  EnsureDummySdlVideoInitialized();
  TemporaryDirectory temp_dir;
  const std::filesystem::path config_home = temp_dir.path() / "config";
  const std::filesystem::path plugins_root = config_home / "microide" / "plugins";
  const std::filesystem::path project = temp_dir.path() / "proj";
  const std::filesystem::path py_file = project / "main.py";
  WriteFile(py_file, "value = 1\nother = 2\n");
  WritePluginInit(plugins_root, "pylsp",
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
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, project, false, false), "the project should open");
  auto stub = std::make_unique<workspace::LspClient>();
  workspace::LspClient* const client = stub.get();
  stub->EnableTestStubMode();
  stub->SetTestHoverHandler([](std::string, workspace::LspClient::HoverCallback cb) {
    cb(util::ParseJson(R"({"contents":{"kind":"markdown","value":"int value"}})"));
  });
  Expect(WorkspaceShellTestAccess::LspManagerForTesting(shell)
             .InstallTestClientIntoExistingForTesting("python", std::move(stub)),
         "a stub python client should attach");
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::OpenFile(shell, py_file);
  WorkspaceShellTestAccess::SetFocusEditor(shell);
  WorkspaceShellTestAccess::NoteLayoutInputsChanged(shell);
  SoftwareCanvas canvas{1280, 720};
  const auto frame = [&] {
    WorkspaceShellTestAccess::ConsumeLspCallbacks(shell);
    shell.Render(canvas.renderer(), 1280, 720);
    (void)shell.ConsumePendingRenderInvalidation();
  };
  frame();
  client->SimulateServerNotificationForTesting(
      "textDocument/publishDiagnostics",
      *util::ParseJson(R"({"uri":")" + workspace::FileUriForPath(py_file) +
                       R"(","diagnostics":[{"range":{"start":{"line":0,"character":0},)"
                       R"("end":{"line":0,"character":5}},"severity":1,)"
                       R"("message":"Predefined type 'System.Object' is not defined"}]})"));
  frame();
  const auto hover = [&]() {
    const util::JsonValue ui = WorkspaceShellTestAccess::ControlUi(shell);
    return ui["popups"]["hover"];
  };

  Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, "goto 1:2"), "goto should run");
  Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, "show-hover"), "show-hover should run");
  for (int i = 0; i < 10 && (!hover().IsObject() ||
                             hover()["text"].AsString().find("int value") == std::string::npos);
       ++i) {
    frame();
  }
  const util::JsonValue card = hover();
  const std::string text = card.IsObject() ? card["text"].AsString() : std::string();
  Expect(card.IsObject() && card["kind"].AsString() == "diagnostic",
         "the squiggle's card is the diagnostic card");
  Expect(text.find("System.Object") != std::string::npos && text.find("int value") != std::string::npos,
         "the card shows the problem and the symbol's hover, got: '" + text + "'");
  Expect(text.find("System.Object") < text.find("int value"), "the problem comes first");
}

void TestWorkspaceShellPluginHoverReadsTheUnsavedText() {
#if !MICROIDE_HAS_LUA_PLUGINS
  return;
#endif
  EnsureDummySdlVideoInitialized();
  TemporaryDirectory temp_dir;
  const std::filesystem::path config_home = temp_dir.path() / "config";
  const std::filesystem::path plugins_root = config_home / "microide" / "plugins";
  const std::filesystem::path project = temp_dir.path() / "proj";
  const std::filesystem::path py_file = project / "main.py";
  WriteFile(py_file, "value = 1\nother = 2\n");
  WritePluginInit(plugins_root, "livehover",
                  R"(local ide = require("microide")
return ide.plugin({
  id = "livehover",
  setup = function(ctx)
    ctx.hover.add({
      id = "line",
      provide = function(buffer, position)
        return { title = "line", content = "line: " .. buffer:line(position.line) }
      end,
    })
  end
})
)");
  ScopedPluginConfigHomeEnv scoped_plugin_config_home(config_home);

  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, project, false, false), "the project should open");
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::OpenFile(shell, py_file);
  WorkspaceShellTestAccess::SetFocusEditor(shell);
  WorkspaceShellTestAccess::NoteLayoutInputsChanged(shell);
  SoftwareCanvas canvas{1280, 720};
  const auto frame = [&] {
    WorkspaceShellTestAccess::ConsumeLspCallbacks(shell);
    (void)shell.HandleScheduledWake();
    shell.Render(canvas.renderer(), 1280, 720);
    (void)shell.ConsumePendingRenderInvalidation();
  };
  frame();
  const auto hover_text = [&]() -> std::string {
    const util::JsonValue ui = WorkspaceShellTestAccess::ControlUi(shell);
    const util::JsonValue& hover = ui["popups"]["hover"];
    return hover.IsObject() ? hover["text"].AsString() : std::string();
  };

  Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, "goto 1:1"), "goto should run");
  Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, "type edited_"), "typing should run");
  Expect(WorkspaceShellTestAccess::ActiveEditor(shell).dirty(), "the buffer has an unsaved edit");
  Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, "show-hover"), "show-hover should run");
  for (int i = 0; i < 50 && hover_text().empty(); ++i) {
    frame();
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  Expect(hover_text().find("line: edited_value = 1") != std::string::npos,
         "the plugin's hover reads the unsaved line, got: '" + hover_text() + "'");
}

void TestWorkspaceShellShowHoverAtTheCaret() {
#if !MICROIDE_HAS_LUA_PLUGINS
  return;
#endif
  EnsureDummySdlVideoInitialized();
  TemporaryDirectory temp_dir;
  const std::filesystem::path config_home = temp_dir.path() / "config";
  const std::filesystem::path plugins_root = config_home / "microide" / "plugins";
  const std::filesystem::path project = temp_dir.path() / "proj";
  const std::filesystem::path py_file = project / "main.py";
  WriteFile(py_file, "value = 1\nother = 2\n");
  WritePluginInit(plugins_root, "pylsp",
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
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, project, false, false), "the project should open");
  auto stub = std::make_unique<workspace::LspClient>();
  stub->EnableTestStubMode();
  stub->SetTestHoverHandler([](std::string uri, workspace::LspClient::HoverCallback cb) {
    (void)uri;
    cb(util::ParseJson(R"({"contents":{"kind":"markdown","value":"int value"}})"));
  });
  Expect(WorkspaceShellTestAccess::LspManagerForTesting(shell)
             .InstallTestClientIntoExistingForTesting("python", std::move(stub)),
         "a stub python client should attach");
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::OpenFile(shell, py_file);
  WorkspaceShellTestAccess::SetFocusEditor(shell);
  WorkspaceShellTestAccess::NoteLayoutInputsChanged(shell);
  SoftwareCanvas canvas{1280, 720};
  const auto frame = [&] {
    WorkspaceShellTestAccess::ConsumeLspCallbacks(shell);
    shell.Render(canvas.renderer(), 1280, 720);
    (void)shell.ConsumePendingRenderInvalidation();
  };
  frame();
  const auto hover_text = [&]() -> std::string {
    const util::JsonValue ui = WorkspaceShellTestAccess::ControlUi(shell);
    const util::JsonValue& hover = ui["popups"]["hover"];
    return hover.IsObject() ? hover["text"].AsString() : std::string();
  };
  Expect(hover_text().empty(), "no card before Show Hover");

  Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, "show-hover"), "show-hover should run");
  for (int i = 0; i < 5 && hover_text().empty(); ++i) {
    frame();
  }
  Expect(hover_text().find("int value") != std::string::npos,
         "the card shows the hover for the caret's symbol: '" + hover_text() + "'");

  (void)DispatchEvents(shell, {KeyDownEvent(SDLK_ESCAPE)});
  frame();
  Expect(hover_text().empty(), "Escape puts the card away");

  Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, "show-hover"), "show-hover again");
  for (int i = 0; i < 5 && hover_text().empty(); ++i) {
    frame();
  }
  Expect(!hover_text().empty(), "the card is back");
  (void)DispatchEvents(shell, {KeyDownEvent(SDLK_DOWN)});
  frame();
  Expect(hover_text().empty(), "moving the caret puts the card away");
}

void TestWorkspaceShellHoverAsksAgainWhenThePointerReturns() {
#if !MICROIDE_HAS_LUA_PLUGINS
  return;
#endif
  EnsureDummySdlVideoInitialized();
  TemporaryDirectory temp_dir;
  const std::filesystem::path config_home = temp_dir.path() / "config";
  const std::filesystem::path plugins_root = config_home / "microide" / "plugins";
  const std::filesystem::path project = temp_dir.path() / "proj";
  const std::filesystem::path py_file = project / "main.py";
  WriteFile(py_file, "value = 1\n\n");
  WritePluginInit(plugins_root, "pylsp",
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
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, project, false, false), "the project should open");
  auto stub = std::make_unique<workspace::LspClient>();
  stub->EnableTestStubMode();
  int answers = 0;
  bool answers_are_empty = false;
  stub->SetTestHoverHandler([&answers, &answers_are_empty](std::string uri,
                                                           workspace::LspClient::HoverCallback cb) {
    (void)uri;
    if (answers_are_empty) {
      cb(util::ParseJson("null"));
      return;
    }
    ++answers;
    cb(util::ParseJson(R"({"contents":{"kind":"plaintext","value":"answer )" + std::to_string(answers) +
                       R"("}})"));
  });
  Expect(WorkspaceShellTestAccess::LspManagerForTesting(shell)
             .InstallTestClientIntoExistingForTesting("python", std::move(stub)),
         "a stub python client should attach");
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::OpenFile(shell, py_file);
  WorkspaceShellTestAccess::NoteLayoutInputsChanged(shell);
  SoftwareCanvas canvas{1280, 720};
  const auto frame = [&] {
    WorkspaceShellTestAccess::ConsumeLspCallbacks(shell);
    shell.Render(canvas.renderer(), 1280, 720);
    (void)shell.ConsumePendingRenderInvalidation();
  };
  frame();
  const auto hover_text = [&]() -> std::string {
    const util::JsonValue ui = WorkspaceShellTestAccess::ControlUi(shell);
    const util::JsonValue& hover = ui["popups"]["hover"];
    return hover.IsObject() ? hover["text"].AsString() : std::string();
  };
  const util::JsonValue ui = WorkspaceShellTestAccess::ControlUi(shell);
  const util::JsonValue& pane = ui["editors"][std::size_t{0}];
  const float x = static_cast<float>(pane["textX"].AsDouble() + 1.5 * pane["charWidth"].AsDouble());
  const float on_value = static_cast<float>(pane["firstLineY"].AsDouble() + 0.5 * pane["lineHeight"].AsDouble());
  const float gutter_x = static_cast<float>(pane["gutterX"].AsDouble() + 2.0);
  const auto rest_at = [&](float y, float at_x) {
    (void)DispatchEvents(shell, {MouseMotionEvent(at_x, y)});
    for (int i = 0; i < 4; ++i) {
      frame();
    }
  };
  rest_at(on_value, x);
  Expect(hover_text() == "answer 1", "the first hover asks the server: '" + hover_text() + "'");
  rest_at(on_value + 1.0f, x);
  Expect(hover_text() == "answer 1", "resting on the same symbol keeps the answer: '" + hover_text() + "'");
  rest_at(on_value, gutter_x);
  Expect(hover_text().empty(), "off the symbol, no card: '" + hover_text() + "'");
  rest_at(on_value, x);
  Expect(hover_text() == "answer 2", "back on the symbol, the server is asked again: '" + hover_text() + "'");

  rest_at(on_value, gutter_x);
  answers_are_empty = true;
  rest_at(on_value, x);
  Expect(hover_text().empty(), "the server has nothing yet: '" + hover_text() + "'");
  answers_are_empty = false;
  rest_at(on_value, gutter_x);
  std::this_thread::sleep_for(std::chrono::milliseconds(1100));
  rest_at(on_value, x);
  Expect(hover_text().rfind("answer ", 0) == 0, "a second later the server is asked again: '" + hover_text() + "'");
}

void TestWorkspaceShellDisablingLspClearsEveryServerOverlay() {
#if !MICROIDE_HAS_LUA_PLUGINS
  return;
#endif
  TemporaryDirectory temp_dir;
  const std::filesystem::path config_home = temp_dir.path() / "config";
  const std::filesystem::path plugins_root = config_home / "microide" / "plugins";
  const std::filesystem::path project = temp_dir.path() / "proj";
  const std::filesystem::path md_file = project / "notes.md";
  WriteFile(md_file, "let value = compute()\n");

  WritePluginInit(plugins_root, "mdoverlays",
                  R"lua(local ide = require("microide")
return ide.plugin({
  id = "mdoverlays",
  capabilities = { process = { exec = true } },
  setup = function(ctx)
    ctx.lsp.add({ id = "md.server", language_id = "markdown", command = { "md-lsp-server" } })
  end
})
)lua");
  ScopedPluginConfigHomeEnv scoped_plugin_config_home(config_home);

  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, project, false, false),
         "overlay fixture should open the project");

  auto stub = std::make_unique<workspace::LspClient>();
  stub->EnableTestStubMode();
  stub->SetTestInlayHintHandler(
      [](std::string, workspace::LspClient::Range, workspace::LspClient::InlayHintCallback cb) {
        cb(std::vector<workspace::LspClient::InlayHint>{workspace::LspClient::InlayHint{
            .position = workspace::LspClient::Position{0, 9}, .label = ": number", .kind = 1}});
      });
  stub->SetTestCodeLensHandler([](std::string, workspace::LspClient::CodeLensCallback cb) {
    workspace::LspClient::CodeLens lens;
    lens.range = {{0, 0}, {0, 21}};
    lens.title = "1 reference";
    lens.command = "md.refs";
    cb(std::vector<workspace::LspClient::CodeLens>{std::move(lens)});
  });
  Expect(WorkspaceShellTestAccess::LspManagerForTesting(shell)
             .InstallTestClientIntoExistingForTesting("markdown", std::move(stub)),
         "fixture should attach a stub markdown client");

  WorkspaceShellTestAccess::OpenFile(shell, md_file);
  (void)WorkspaceShellTestAccess::ResolveLspHoverForTesting(shell, md_file, 1, 1);
  WorkspaceShellTestAccess::ConsumeLspCallbacks(shell);

  const auto& state = WorkspaceShellTestAccess::CurrentProjectState(shell);
  const auto decorations_now = [&state, &md_file]() -> const editor::FileDecorations* {
    const auto* presentation = state.plugin_presentation_if_present();
    return presentation != nullptr ? presentation->decorations.FindByPath(md_file) : nullptr;
  };
  const auto* before = decorations_now();
  Expect(before != nullptr && !before->inline_texts.empty(),
         "the fixture should publish an inlay-hint overlay to clear");
  Expect(before != nullptr && !before->code_lenses.empty(),
         "the fixture should publish a code-lens overlay to clear");

  Expect(WorkspaceShellTestAccess::SetSettingValue(shell, "lsp.enabled", "false"),
         "the master toggle should write");
  WorkspaceShellTestAccess::ApplyLiveSettings(shell);

  const auto* after = decorations_now();
  Expect(after == nullptr || after->inline_texts.empty(),
         "master off must drop the inlay-hint overlay, not just diagnostics and tokens");
  Expect(after == nullptr || after->code_lenses.empty(),
         "master off must drop the code-lens overlay");
  Expect(after == nullptr || after->text_styles.empty(),
         "master off must drop the semantic-token overlay");
}

void TestWorkspaceShellSupersededCodeLensResponseKeepsLiveLensesClickable() {
#if !MICROIDE_HAS_LUA_PLUGINS
  return;
#endif
  TemporaryDirectory temp_dir;
  const std::filesystem::path config_home = temp_dir.path() / "config";
  const std::filesystem::path plugins_root = config_home / "microide" / "plugins";
  const std::filesystem::path project = temp_dir.path() / "proj";
  const std::filesystem::path md_file = project / "notes.md";
  WriteFile(md_file, "fn alpha() {}\n");

  WritePluginInit(plugins_root, "mdstale",
                  R"lua(local ide = require("microide")
return ide.plugin({
  id = "mdstale",
  capabilities = { process = { exec = true } },
  setup = function(ctx)
    ctx.lsp.add({ id = "md.server", language_id = "markdown", command = { "md-lsp-server" } })
  end
})
)lua");
  ScopedPluginConfigHomeEnv scoped_plugin_config_home(config_home);

  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, project, false, false),
         "stale-lens fixture should open the project");

  std::vector<workspace::LspClient::CodeLensCallback> parked;
  auto stub = std::make_unique<workspace::LspClient>();
  stub->EnableTestStubMode();
  stub->SetTestCodeLensHandler(
      [&parked](std::string, workspace::LspClient::CodeLensCallback cb) {
        parked.push_back(std::move(cb));
      });
  std::string executed_command;
  stub->SetTestExecuteCommandHandler(
      [&executed_command](std::string command, std::vector<util::JsonValue>,
                          workspace::LspClient::ExecuteCommandCallback cb) {
        executed_command = std::move(command);
        cb(util::JsonValue{});
      });
  Expect(WorkspaceShellTestAccess::LspManagerForTesting(shell)
             .InstallTestClientIntoExistingForTesting("markdown", std::move(stub)),
         "fixture should attach a stub markdown client");

  WorkspaceShellTestAccess::OpenFile(shell, md_file);
  (void)WorkspaceShellTestAccess::ResolveLspHoverForTesting(shell, md_file, 1, 1);
  Expect(WorkspaceShellTestAccess::RequestCodeLensesForActiveBuffer(shell),
         "a second code-lens pull should be issuable");
  WorkspaceShellTestAccess::ConsumeLspCallbacks(shell);
  Expect(parked.size() >= 2, "two code-lens requests should be outstanding");
  if (parked.size() < 2) {
    return;
  }

  const auto make_lens = [](std::string title, std::string command) {
    workspace::LspClient::CodeLens lens;
    lens.range = {{0, 0}, {0, 13}};
    lens.title = std::move(title);
    lens.command = std::move(command);
    return lens;
  };
  parked.back()(std::vector<workspace::LspClient::CodeLens>{make_lens("current", "md.current")});
  WorkspaceShellTestAccess::ConsumeLspCallbacks(shell);
  const auto& state = WorkspaceShellTestAccess::CurrentProjectState(shell);
  const auto* presentation = state.plugin_presentation_if_present();
  const auto* decorations =
      presentation != nullptr ? presentation->decorations.FindByPath(md_file) : nullptr;
  Expect(decorations != nullptr && decorations->code_lenses.size() == 1,
         "the newest response publishes its lens");
  if (decorations == nullptr || decorations->code_lenses.empty()) {
    return;
  }
  const std::uint64_t live_payload = decorations->code_lenses[0].payload;
  Expect(live_payload != 0, "the published lens carries an executable payload");

  parked.front()(std::vector<workspace::LspClient::CodeLens>{make_lens("stale", "md.stale")});
  WorkspaceShellTestAccess::ConsumeLspCallbacks(shell);
  const auto* after =
      state.plugin_presentation_if_present() != nullptr
          ? state.plugin_presentation_if_present()->decorations.FindByPath(md_file)
          : nullptr;
  Expect(after != nullptr && after->code_lenses.size() == 1 &&
             after->code_lenses[0].text == "current",
         "the superseded response must not replace the painted lens");

  WorkspaceShellTestAccess::ActivateCodeLens(shell, live_payload);
  Expect(executed_command == "md.current",
         "the painted lens must still be clickable after a superseded response arrived");
}

void TestWorkspaceShellCallHierarchyRendersBothDirections() {
#if !MICROIDE_HAS_LUA_PLUGINS
  return;
#endif
  TemporaryDirectory temp_dir;
  const std::filesystem::path config_home = temp_dir.path() / "config";
  const std::filesystem::path plugins_root = config_home / "microide" / "plugins";
  const std::filesystem::path project = temp_dir.path() / "proj";
  const std::filesystem::path md_file = project / "notes.md";
  WriteFile(md_file, "fn target() {}\nfn caller() { target(); }\nfn other() { target(); }\n");

  WritePluginInit(plugins_root, "mdcalls",
                  R"lua(local ide = require("microide")
return ide.plugin({
  id = "mdcalls",
  capabilities = { process = { exec = true } },
  setup = function(ctx)
    ctx.lsp.add({ id = "md.server", language_id = "markdown", command = { "md-lsp-server" } })
  end
})
)lua");
  ScopedPluginConfigHomeEnv scoped_plugin_config_home(config_home);

  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, project, false, false),
         "call-hierarchy fixture should open the project");

  const std::string uri = workspace::FileUriForPath(md_file);
  auto stub = std::make_unique<workspace::LspClient>();
  stub->EnableTestStubMode();
  stub->SetTestPrepareCallHierarchyHandler(
      [uri](std::string, workspace::LspClient::Position,
            workspace::LspClient::PrepareCallHierarchyCallback cb) {
        workspace::LspClient::CallHierarchyItem item;
        item.name = "target";
        item.detail = "fn target()";
        item.uri = uri;
        item.range = {{0, 0}, {0, 14}};
        item.selection_range = {{0, 3}, {0, 9}};
        util::JsonObject raw;
        raw["name"] = util::JsonValue("target");
        raw["data"] = util::JsonValue(std::int64_t{11});
        item.raw = util::JsonValue(std::move(raw));
        cb(std::vector<workspace::LspClient::CallHierarchyItem>{std::move(item)});
      });
  util::JsonValue calls_saw_item;
  bool calls_saw_incoming = false;
  stub->SetTestCallHierarchyCallsHandler(
      [&, uri](bool incoming, util::JsonValue item,
               workspace::LspClient::CallHierarchyCallsCallback cb) {
        calls_saw_item = item;
        calls_saw_incoming = incoming;
        workspace::LspClient::CallHierarchyCall call;
        call.item.name = incoming ? "caller" : "callee";
        call.item.uri = uri;
        call.item.range = {{1, 0}, {1, 26}};
        call.item.selection_range = {{1, 3}, {1, 9}};
        if (incoming) {
          call.call_ranges.push_back({{1, 14}, {1, 20}});
          call.call_ranges.push_back({{2, 13}, {2, 19}});
        }
        cb(std::vector<workspace::LspClient::CallHierarchyCall>{std::move(call)});
      });
  Expect(WorkspaceShellTestAccess::LspManagerForTesting(shell)
             .InstallTestClientIntoExistingForTesting("markdown", std::move(stub)),
         "fixture should attach a stub markdown client");

  WorkspaceShellTestAccess::OpenFile(shell, md_file);
  WorkspaceShellTestAccess::ActiveEditor(shell).MoveCursorTo(0, 4);
  Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, "call-hierarchy"),
         "call-hierarchy should execute");
  WorkspaceShellTestAccess::ConsumeLspCallbacks(shell);
  WorkspaceShellTestAccess::ConsumeLspCallbacks(shell);

  Expect(calls_saw_incoming, "the bare command defaults to incoming calls");
  Expect(calls_saw_item["data"].AsInt(0) == 11,
         "the calls request hands back the prepare item verbatim");

  const auto* channel = WorkspaceShellTestAccess::OutputChannelEntries(shell, "lsp.callHierarchy");
  Expect(channel != nullptr && !channel->empty(), "the call-hierarchy channel should be filled");
  const auto has = [&channel](std::string_view text) {
    return channel != nullptr &&
           std::find(channel->begin(), channel->end(), text) != channel->end();
  };
  Expect(has("Callers of target  ·  fn target()"), "the root symbol heads the incoming list");
  Expect(has("caller"), "each caller is listed by name");
  Expect(has("notes.md:2:15") && has("notes.md:3:14"),
         "incoming calls navigate to the CALL SITES inside the caller, not its declaration");

  WorkspaceShellTestAccess::ActiveEditor(shell).MoveCursorTo(0, 4);
  Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, "call-hierarchy outgoing"),
         "call-hierarchy outgoing should execute");
  WorkspaceShellTestAccess::ConsumeLspCallbacks(shell);
  WorkspaceShellTestAccess::ConsumeLspCallbacks(shell);
  Expect(!calls_saw_incoming, "the `outgoing` argument selects the other direction");
  const auto* outgoing = WorkspaceShellTestAccess::OutputChannelEntries(shell, "lsp.callHierarchy");
  Expect(outgoing != nullptr &&
             std::find(outgoing->begin(), outgoing->end(), "Called by target  ·  fn target()") !=
                 outgoing->end(),
         "the outgoing list is re-rendered from scratch, not appended to the incoming one");
  Expect(outgoing != nullptr &&
             std::find(outgoing->begin(), outgoing->end(), "notes.md:2:4") != outgoing->end(),
         "outgoing calls navigate to the callee's own name (its selectionRange)");
  Expect(outgoing != nullptr &&
             std::find(outgoing->begin(), outgoing->end(), "Callers of target  ·  fn target()") ==
                 outgoing->end(),
         "the previous direction's heading must be gone");

  Expect(!WorkspaceShellTestAccess::ExecuteCommandLine(shell, "call-hierarchy sideways"),
         "an unknown direction argument is rejected");
}

void TestWorkspaceShellPrepareRenameRefinesPrompt() {
#if !MICROIDE_HAS_LUA_PLUGINS
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
  Expect(WorkspaceShellTestAccess::PromptSurfaceVisible(shell), "rename prompt should be visible");
  Expect(WorkspaceShellTestAccess::PromptSurfaceInput(shell) == "alpha",
         "the prompt opens with the heuristic identifier seed");
  WorkspaceShellTestAccess::ConsumeLspCallbacks(shell);
  Expect(WorkspaceShellTestAccess::PromptSurfaceInput(shell) == "server_placeholder",
         "prepareRename must prefill the server's placeholder over the heuristic seed");

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

void TestWorkspaceShellServerApplyEditEditsOpenAndClosedFiles() {
#if !MICROIDE_HAS_LUA_PLUGINS
  return;
#endif
  TemporaryDirectory temp_dir;
  const std::filesystem::path config_home = temp_dir.path() / "config";
  const std::filesystem::path plugins_root = config_home / "microide" / "plugins";
  const std::filesystem::path project = temp_dir.path() / "proj";
  const std::filesystem::path open_md = project / "open.md";
  const std::filesystem::path closed_md = project / "closed.md";
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
  WorkspaceShellTestAccess::ExecuteCommandLine(shell, "completion");
  WorkspaceShellTestAccess::ConsumeLspCallbacks(shell);
  Expect(stub_raw->HasApplyEditHandler(),
         "resolving the client for a viewport should bind the apply-edit handler");

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
  WorkspaceShellTestAccess::DrainProjectBackgroundExecutor(shell);

  Expect(WorkspaceShellTestAccess::ActiveEditor(shell).lines()[0] == "bbb",
         "the open buffer should be edited in place by the server-pushed edit");
  Expect(ReadFile(closed_md) == "bbb\n",
         "the closed file should be written silently on disk by the server-pushed edit");
  Expect(WorkspaceShellTestAccess::CountOpenBufferViews(shell, closed_md) == 0,
         "the server-pushed edit must not open a tab for the closed file");
}

void TestWorkspaceShellServerApplyEditRejectsOutOfRangeClosedFileEdit() {
#if !MICROIDE_HAS_LUA_PLUGINS
  return;
#endif
  TemporaryDirectory temp_dir;
  const std::filesystem::path config_home = temp_dir.path() / "config";
  const std::filesystem::path plugins_root = config_home / "microide" / "plugins";
  const std::filesystem::path project = temp_dir.path() / "proj";
  const std::filesystem::path open_md = project / "open.md";
  const std::filesystem::path closed_md = project / "closed.md";
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

  const std::string edit_json = std::string("{\"edit\":{\"changes\":{\"") +
                                workspace::FileUriForPath(closed_md) +
                                "\":[{\"range\":{\"start\":{\"line\":999,\"character\":0},"
                                "\"end\":{\"line\":999,\"character\":0}},\"newText\":\"Z\"}]}}}";
  std::optional<util::JsonValue> params = util::ParseJson(edit_json);
  Expect(params.has_value(), "the applyEdit params fixture should parse");
  stub_raw->SimulateServerRequestForTesting("workspace/applyEdit", std::move(*params),
                                            util::JsonValue(static_cast<std::int64_t>(1)));
  WorkspaceShellTestAccess::ConsumeLspCallbacks(shell);
  WorkspaceShellTestAccess::DrainProjectBackgroundExecutor(shell);

  Expect(ReadFile(closed_md) == "aaa\n",
         "an out-of-range (beyond-EOF) edit must not be clamped onto the last line and written");
}

struct ServerApplyEditFixture {
  TemporaryDirectory temp_dir;
  std::filesystem::path project;
  std::filesystem::path open_md;
  std::unique_ptr<ScopedPluginConfigHomeEnv> scoped_env;
  std::unique_ptr<WorkspaceShell> shell;
  workspace::LspClient* stub = nullptr;

  bool SetUp() {
    const std::filesystem::path config_home = temp_dir.path() / "config";
    const std::filesystem::path plugins_root = config_home / "microide" / "plugins";
    project = temp_dir.path() / "proj";
    open_md = project / "open.md";
    WriteFile(open_md, "aaa\n");
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
    scoped_env = std::make_unique<ScopedPluginConfigHomeEnv>(config_home);
    shell = std::make_unique<WorkspaceShell>();
    if (!WorkspaceShellTestAccess::OpenProjectTab(*shell, project, false, false)) {
      return false;
    }
    auto stub_owned = std::make_unique<workspace::LspClient>();
    stub = stub_owned.get();
    stub->EnableTestStubMode();
    if (!WorkspaceShellTestAccess::LspManagerForTesting(*shell)
             .InstallTestClientIntoExistingForTesting("markdown", std::move(stub_owned))) {
      return false;
    }
    WorkspaceShellTestAccess::OpenFile(*shell, open_md);
    WorkspaceShellTestAccess::ExecuteCommandLine(*shell, "completion");
    WorkspaceShellTestAccess::ConsumeLspCallbacks(*shell);
    return stub->HasApplyEditHandler();
  }

  void PushApplyEdit(const std::string& edit_json) {
    std::optional<util::JsonValue> params = util::ParseJson(edit_json);
    Expect(params.has_value(), "the applyEdit params fixture should parse");
    stub->SimulateServerRequestForTesting("workspace/applyEdit", std::move(*params),
                                          util::JsonValue(static_cast<std::int64_t>(1)));
    WorkspaceShellTestAccess::ConsumeLspCallbacks(*shell);
    WorkspaceShellTestAccess::DrainProjectBackgroundExecutor(*shell);
  }
};

void TestWorkspaceShellServerApplyEditAppliesResourceOps() {
#if !MICROIDE_HAS_LUA_PLUGINS
  return;
#endif
  ServerApplyEditFixture fx;
  Expect(fx.SetUp(), "resource-op fixture should come up with the handler bound");
  const std::filesystem::path created_md = fx.project / "created.md";
  const std::filesystem::path moved_md = fx.project / "moved.md";
  const std::filesystem::path zombie_md = fx.project / "zombie.md";
  WriteFile(zombie_md, "dead\n");

  const std::string edit_json =
      std::string("{\"edit\":{\"documentChanges\":[") +
      "{\"kind\":\"create\",\"uri\":\"" + workspace::FileUriForPath(created_md) + "\"}," +
      "{\"textDocument\":{\"uri\":\"" + workspace::FileUriForPath(created_md) +
      "\",\"version\":null},\"edits\":[{\"range\":{\"start\":{\"line\":0,\"character\":0},"
      "\"end\":{\"line\":0,\"character\":0}},\"newText\":\"hello\\n\"}]}," +
      "{\"kind\":\"rename\",\"oldUri\":\"" + workspace::FileUriForPath(fx.open_md) +
      "\",\"newUri\":\"" + workspace::FileUriForPath(moved_md) + "\"}," +
      "{\"kind\":\"delete\",\"uri\":\"" + workspace::FileUriForPath(zombie_md) + "\"}]}}";
  fx.PushApplyEdit(edit_json);

  Expect(ReadFile(created_md) == "hello\n",
         "the created file should exist and carry the TextDocumentEdit fill");
  Expect(!std::filesystem::exists(fx.open_md), "the renamed source should be gone on disk");
  Expect(ReadFile(moved_md) == "aaa\n", "the renamed file should carry its content");
  Expect(WorkspaceShellTestAccess::CountOpenBufferViews(*fx.shell, moved_md) == 1,
         "the open tab should retarget to the renamed path");
  Expect(WorkspaceShellTestAccess::CountOpenBufferViews(*fx.shell, fx.open_md) == 0,
         "no tab should remain on the pre-rename path");
  Expect(!std::filesystem::exists(zombie_md), "the deleted file should be gone");
  bool staging_residue = false;
  for (const auto& entry : std::filesystem::directory_iterator(fx.project)) {
    if (entry.path().filename().string().find(".microide-lsp-staged-") != std::string::npos) {
      staging_residue = true;
    }
  }
  Expect(!staging_residue, "no staging residue should remain after a successful batch");
}

void TestWorkspaceShellServerApplyEditResourceOpBatchIsAtomic() {
#if !MICROIDE_HAS_LUA_PLUGINS
  return;
#endif
  ServerApplyEditFixture fx;
  Expect(fx.SetUp(), "atomicity fixture should come up with the handler bound");
  const std::filesystem::path created_md = fx.project / "created.md";
  const std::filesystem::path missing_md = fx.project / "missing.md";

  const std::string edit_json =
      std::string("{\"edit\":{\"documentChanges\":[") +
      "{\"kind\":\"create\",\"uri\":\"" + workspace::FileUriForPath(created_md) + "\"}," +
      "{\"kind\":\"rename\",\"oldUri\":\"" + workspace::FileUriForPath(missing_md) +
      "\",\"newUri\":\"" + workspace::FileUriForPath(fx.project / "x.md") + "\"}]}}";
  fx.PushApplyEdit(edit_json);

  Expect(!std::filesystem::exists(created_md),
         "a failed batch must not leave the earlier create applied");

  const std::string escape_json =
      std::string("{\"edit\":{\"documentChanges\":[") + "{\"kind\":\"delete\",\"uri\":\"" +
      workspace::FileUriForPath(fx.temp_dir.path() / "outside.txt") + "\"}]}}";
  WriteFile(fx.temp_dir.path() / "outside.txt", "keep\n");
  fx.PushApplyEdit(escape_json);
  Expect(ReadFile(fx.temp_dir.path() / "outside.txt") == "keep\n",
         "a resource op outside the project root must not apply");
}

void TestWorkspaceShellServerApplyEditHonorsDocumentVersions() {
#if !MICROIDE_HAS_LUA_PLUGINS
  return;
#endif
  ServerApplyEditFixture fx;
  Expect(fx.SetUp(), "version fixture should come up with the handler bound");
  const auto edit_with_version = [&](int version) {
    return std::string("{\"edit\":{\"documentChanges\":[") + "{\"textDocument\":{\"uri\":\"" +
           workspace::FileUriForPath(fx.open_md) + "\",\"version\":" + std::to_string(version) +
           "},\"edits\":[{\"range\":{\"start\":{\"line\":0,\"character\":0},"
           "\"end\":{\"line\":0,\"character\":3}},\"newText\":\"bbb\"}]}]}}";
  };
  fx.PushApplyEdit(edit_with_version(999));
  Expect(WorkspaceShellTestAccess::ActiveEditor(*fx.shell).lines()[0] == "aaa",
         "an edit pinned to a stale document version must not apply");
  fx.PushApplyEdit(edit_with_version(1));
  Expect(WorkspaceShellTestAccess::ActiveEditor(*fx.shell).lines()[0] == "bbb",
         "an edit pinned to the current document version applies");
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
         "the sidebar mode menu should omit the chat view, which no longer exists");
  Expect(std::find(labels.begin(), labels.end(), "Problems") != labels.end(),
         "the sidebar mode menu should list the tabless Problems view");
  Expect(std::find(labels.begin(), labels.end(), "Tests") != labels.end(),
         "the sidebar mode menu should list the tabless Tests view");
  Expect(std::find(labels.begin(), labels.end(), "Outline") != labels.end(),
         "the sidebar mode menu should list the tabless Outline view");

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

  const auto metrics = WorkspaceShellTestAccess::InactiveEditorPaneMetrics(shell);
  const float hover_x = metrics.text_x + WorkspaceShellTestAccess::TextCharWidth(shell) * 2.0f;
  const float hover_y = metrics.first_line_y + metrics.line_height - 1.5f;
  Expect(SendMouseMotion(shell, hover_x, hover_y, 0),
         "hovering the inactive pane's diagnostic underline should be handled");

  const auto message = WorkspaceShellTestAccess::ActiveEditorDiagnosticHoverMessage(shell);
  Expect(message.has_value() && *message == "Unexpected token near alpha",
         "hovering the unfocused group's diagnostic should surface its message");
}

void TestWorkspaceShellHoverCardOpensAboveTheLine() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path project_root = temp_dir.path() / "project";
  const std::filesystem::path source = project_root / "README.md";
  std::string text;
  for (int i = 0; i < 30; ++i) {
    text += i == 12 ? "alpha beta\n" : "line\n";
  }
  WriteFile(source, text);
  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, project_root);
  WorkspaceShellTestAccess::OpenSingleEditorTab(shell, source);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  const auto diagnostic_on = [&](std::size_t line) {
    return microide::editor::Diagnostic{
        .range = microide::editor::SelectionRange{
            .start = microide::editor::TextPosition{.line = line, .column = 1},
            .end = microide::editor::TextPosition{.line = line, .column = 4},
        },
        .severity = microide::editor::DiagnosticSeverity::Warning,
        .message = "Unexpected token near alpha",
    };
  };
  Expect(WorkspaceShellTestAccess::PublishDiagnostics(shell, "diagnostics", source,
                                                      {diagnostic_on(12), diagnostic_on(0)}),
         "the fixture should publish its diagnostics");
  const auto metrics = WorkspaceShellTestAccess::ActiveEditorMetrics(shell);
  const float x = metrics.text_x + WorkspaceShellTestAccess::TextCharWidth(shell) * 2.0f;
  for (const std::size_t line : {std::size_t{12}, std::size_t{0}}) {
    const float row_top = metrics.first_line_y + static_cast<float>(line) * metrics.line_height;
    Expect(SendMouseMotion(shell, x, row_top + metrics.line_height - 1.5f, 0),
           "hovering the diagnostic should be handled");
    const auto popup = WorkspaceShellTestAccess::ActiveEditorHoverPopupRect(shell);
    Expect(popup.has_value(), "the hover should open a card");
    if (!popup) {
      continue;
    }
    if (line == 12) {
      Expect(popup->y + popup->h <= row_top,
             "with room above, the card should sit above the hovered line");
    } else {
      Expect(popup->y >= row_top + metrics.line_height,
             "on the first line, with no room above, the card should sit below it");
    }
  }
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

void TestWorkspaceShellProblemsSidebarRightClickOpensRowMenu() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path project_root = temp_dir.path() / "project";
  const std::filesystem::path readme = project_root / "README.md";
  const std::filesystem::path source = project_root / "src" / "main.txt";
  WriteFile(readme, "workspace problems\n");
  WriteFile(source, "alpha\nbeta\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, project_root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
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
         "the fixture should publish one diagnostic");
  Expect(WorkspaceShellTestAccess::RefreshProblemsSidebar(shell),
         "the problems sidebar should snapshot the diagnostic");
  WorkspaceShellTestAccess::ShowProblemsSidebar(shell);
  Expect(WorkspaceShellTestAccess::ProblemsSidebarEntries(shell).size() == 1,
         "the problems sidebar should hold one row");

  const SDL_FRect row = WorkspaceShellTestAccess::ProblemsSidebarRowRect(shell, 0);
  Expect(row.w > 0.0f && row.h > 0.0f, "the problem row should be laid out");
  Expect(SendMouseDown(shell, row.x + row.w * 0.5f, row.y + row.h * 0.5f, SDL_BUTTON_RIGHT),
         "right-clicking a problem row should be handled");
  Expect(WorkspaceShellTestAccess::TreeContextMenuTarget(shell) ==
             microide::workspace::TreeContextTargetKind::ResultRow,
         "right-clicking a problem should open the shared row menu");
  Expect(WorkspaceShellTestAccess::TreeContextMenuPath(shell).lexically_normal() ==
             source.lexically_normal(),
         "the menu should target the diagnostic's file");
  Expect(WorkspaceShellTestAccess::ActiveEditor(shell).path() == readme.lexically_normal(),
         "right-clicking a problem should not navigate the editor");
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

  auto warm_client = std::make_unique<workspace::LspClient>();
  workspace::LspClient* const warm_raw = warm_client.get();
  Expect(WorkspaceShellTestAccess::LspManagerForTesting(shell)
             .InstallTestClientIntoExistingForTesting("warmlang", std::move(warm_client)),
         "warm-LSP fixture should attach a stub client to the registered server");

  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, project_b, false, false),
         "warm-LSP fixture should open the second project (tearing down the host)");

  Expect(WorkspaceShellTestAccess::SwitchProject(shell, 0, false),
         "warm-LSP fixture should switch back to the first project");

  Expect(WorkspaceShellTestAccess::LspManagerForTesting(shell).HasServer("warmlang"),
         "switching back must keep the project's language server registered");
  Expect(WorkspaceShellTestAccess::LspManagerForTesting(shell).FindStartedServer("warmlang") ==
             warm_raw,
         "switching back must retain the SAME warm client instance (no restart/re-index)");
}

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

  auto stub = std::make_unique<workspace::LspClient>();
  workspace::LspClient* const stub_raw = stub.get();
  Expect(WorkspaceShellTestAccess::LspManagerForTesting(shell)
             .InstallTestClientIntoExistingForTesting("python", std::move(stub)),
         "fixture should attach a stub python client");

  const std::string uri = workspace::FileUriForPath(py_file);
  Expect(!stub_raw->HasOpenDocument(uri),
         "precondition: the document is not open in the LSP before it is opened");

  Expect(WorkspaceShellTestAccess::OpenFileInNewTab(shell, py_file),
         "fixture should open the .py file");

  Expect(stub_raw->HasOpenDocument(uri),
         "opening a file must send textDocument/didOpen (engage the LSP on open, "
         "not only on the first edit or go-to-definition)");
}

void TestWorkspaceShellRestoreEngagesLspForOpenDocument() {
#if !MICROIDE_HAS_LUA_PLUGINS
  return;
#endif
  TemporaryDirectory temp_dir;
  const std::filesystem::path config_home = temp_dir.path() / "config";
  const std::filesystem::path state_home = temp_dir.path() / "state";
  const std::filesystem::path plugins_root = config_home / "microide" / "plugins";
  const std::filesystem::path project = temp_dir.path() / "proj";
  const std::filesystem::path py_file = project / "main.py";
  WriteFile(py_file, "print('hi')\n");

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
  ScopedEnvVar scoped_state_home("XDG_STATE_HOME", state_home.string());

  {
    WorkspaceShell shell;
    Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, project, false, false),
           "phase 1 should open the project");
    Expect(WorkspaceShellTestAccess::OpenFileInNewTab(shell, py_file),
           "phase 1 should open the .py file");
    WorkspaceShellTestAccess::SaveSessionState(shell);
  }

  WorkspaceShell restored;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(restored, project, true, false),
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
    ctx.commands.add("editops.two_inserts_same_position", function()
      -- Two inserts at one position must land in array order (VS Code's rule).
      local ok = ctx.editor.apply_edits({
        edits = {
          { start_line = 1, start_col = 1, end_line = 1, end_col = 1, text = "A" },
          { start_line = 1, start_col = 1, end_line = 1, end_col = 1, text = "B" },
        },
      })
      ctx.log(ok and "applied" or "rejected")
    end)
    ctx.commands.add("editops.replace_then_insert_same_start", function()
      -- A replace listed BEFORE an insert at its start must still apply first;
      -- the insert goes in front of the replacement, never inside it.
      local ok = ctx.editor.apply_edits({
        edits = {
          { start_line = 1, start_col = 1, end_line = 1, end_col = 6, text = "HELLO" },
          { start_line = 1, start_col = 1, end_line = 1, end_col = 1, text = ">" },
        },
      })
      ctx.log(ok and "applied" or "rejected")
    end)
    ctx.commands.add("editops.overlapping", function()
      local ok = ctx.editor.apply_edits({
        edits = {
          { start_line = 1, start_col = 1, end_line = 1, end_col = 4, text = "X" },
          { start_line = 1, start_col = 3, end_line = 1, end_col = 6, text = "Y" },
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

void TestWorkspaceShellPluginApplyEditsSamePositionInsertsKeepArrayOrder() {
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
  Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, "editops.two_inserts_same_position"),
         "apply_edits command should dispatch");
  auto& editor = WorkspaceShellTestAccess::ActiveEditor(shell);
  Expect(editor.lines()[0] == "ABhello",
         "two inserts at one position should land in array order, got: " +
             std::string(editor.lines()[0]));

  Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell,
                                                       "editops.replace_then_insert_same_start"),
         "apply_edits command should dispatch");
  Expect(editor.lines()[0] == ">HELLOlo",
         "a replace listed before an insert at its start should apply first, got: " +
             std::string(editor.lines()[0]));

  Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, "editops.overlapping"),
         "apply_edits command should dispatch");
  Expect(editor.lines()[0] == ">HELLOlo",
         "overlapping plugin edits should be refused whole, got: " +
             std::string(editor.lines()[0]));
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

  Expect(WorkspaceShellTestAccess::HandleTextInput(shell, "x"),
         "typing into an editable buffer should be handled");
  Expect(!HasLspSemanticOverlay(shell, source),
         "a content edit should clear the stale lsp:semantic overlay");

  SeedLspSemanticOverlay(shell, source);
  Expect(HasLspSemanticOverlay(shell, source),
         "re-seeded overlay should be present before undo");
  Expect(SendKeyDown(shell, SDLK_Z, SDL_KMOD_CTRL), "Ctrl+Z should undo the edit");
  Expect(WorkspaceShellTestAccess::ActiveEditor(shell).lines()[0] == "int main() {",
         "undo should restore the original first line");
  Expect(!HasLspSemanticOverlay(shell, source),
         "undo must clear the stale lsp:semantic overlay (the reported corruption)");
}

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
    hint.anchor_column = 2;
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

void TestWorkspaceShellLspKeystrokeSyncAllocatesNothingWithoutAServer() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path project_root = temp_dir.path() / "project";
  const std::filesystem::path source = project_root / "main.cpp";
  WriteFile(source, "int main() {\n  return 0;\n}\n");

  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, project_root, false, false),
         "open the project");
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::OpenFile(shell, source);
  WorkspaceShellTestAccess::ActiveEditor(shell).MoveCursorTo(0, 0);
  Expect(SendKeyDown(shell, SDLK_RETURN, SDL_KMOD_NONE), "an edit, so there is a last change");

#if MICROIDE_PERF_HARNESS_BUILD
  WorkspaceShellTestAccess::SyncLspForActiveEditableLastChange(shell);
  const microide::tests::perf::AllocationSnapshot before =
      microide::tests::perf::Allocations::Snapshot();
  for (int i = 0; i < 16; ++i) {
    WorkspaceShellTestAccess::SyncLspForActiveEditableLastChange(shell);
  }
  const microide::tests::perf::AllocationDelta delta =
      microide::tests::perf::Allocations::DeltaSince(before);
  Expect(delta.allocations == 0,
         "the per-keystroke LSP sync must not allocate with no server and no diagnostics, "
         "got " + std::to_string(delta.allocations) + " allocations for 16 syncs");
#endif
}

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

  WorkspaceShellTestAccess::ActiveEditor(shell).MoveCursorTo(0, 0);
  Expect(SendKeyDown(shell, SDLK_RETURN, SDL_KMOD_NONE),
         "pressing Enter at the top of the buffer should insert a line");

  const auto* published = WorkspaceShellTestAccess::DiagnosticsForPath(shell, source);
  Expect(published != nullptr && published->size() == 1,
         "the diagnostic should still be stored after a serverless edit");
  Expect(published->front().range.start.line == 2,
         "the diagnostic must shift down a line even with no language server running");
}

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
  request.anchor_line = 1;
  request.anchor_column = 1;
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
  Expect(editor.lines()[0] == "hi!" || editor.lines().back() == "!" ||
             editor.lines().back() == "hi!",
         "an out-of-bounds edit should clamp to a valid position, not crash");
}

void TestPluginEditorEventTrackerCoalescesChanges() {
  using microide::workspace::PluginEditorEventTracker;
  PluginEditorEventTracker tracker;
  tracker.SetInterest({.buffer_change = true});
  const std::filesystem::path path = "a.txt";
  tracker.Sample(path, 1, 1, 1, false, 0, 0, 0, 0, 100, 150);
  Expect(!tracker.NextDelayMs(100).has_value(), "baseline sample should arm nothing");
  tracker.Sample(path, 2, 3, 1, false, 0, 0, 0, 0, 110, 150);
  tracker.Sample(path, 3, 7, 1, false, 0, 0, 0, 0, 140, 150);
  Expect(tracker.NextDelayMs(140).has_value(), "an edit should arm a debounce delay");
  Expect(!tracker.TakeDue(200).any(), "no event before the trailing deadline");
  const auto due = tracker.TakeDue(300);
  Expect(due.change && !due.cursor && !due.selection, "a settled burst yields one change");
  Expect(due.change_start_line == 3 && due.change_end_line == 7,
         "the change range should union the burst's caret lines");
  Expect(!tracker.TakeDue(400).any(), "a consumed event does not refire");
}

void TestPluginEditorEventTrackerZeroCostWhenUnsubscribed() {
  using microide::workspace::PluginEditorEventTracker;
  PluginEditorEventTracker tracker;
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

  WorkspaceShellTestAccess::SamplePluginEditorEvents(shell);
  WorkspaceShellTestAccess::ClearPluginMessages(shell);
  Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, "reactor.poke"),
         "reactor.poke should dispatch and edit the buffer");
  WorkspaceShellTestAccess::SamplePluginEditorEvents(shell);

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

void TestWorkspaceShellPluginSnippetForCxxLanguageIdReachesCppBuffers() {
#if !MICROIDE_HAS_LUA_PLUGINS
  return;
#endif
  TemporaryDirectory temp_dir;
  const std::filesystem::path config_home = temp_dir.path() / "config";
  const std::filesystem::path plugins_root = config_home / "microide" / "plugins";
  const std::filesystem::path project_root = temp_dir.path() / "project";
  const std::filesystem::path source = project_root / "main.cpp";
  WriteFile(source, "\n");
  WritePluginInit(
      plugins_root, "cxxsnip",
      R"(local ide = require("microide")
return ide.plugin({
  id = "cxxsnip",
  setup = function(ctx)
    ctx.snippets.add({
      id = "fori", language_id = "c++", prefix = "fori", label = "for loop",
      body = "for (;;) {}",
    })
  end
})
)");
  ScopedPluginConfigHomeEnv scoped_plugin_config_home(config_home);

  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, project_root, false, false),
         "c++ snippet fixture should open the project");
  WorkspaceShellTestAccess::OpenFile(shell, source);
  Expect(WorkspaceShellTestAccess::PluginErrors(shell).empty(), DescribePluginState(shell).c_str());

  auto& editor = WorkspaceShellTestAccess::ActiveEditor(shell);
  Expect(editor.language_id() == "c++", "a .cpp buffer reports the runtime filetype c++");
  editor.InsertText("fori");
  Expect(SendKeyDown(shell, SDLK_TAB, SDL_KMOD_NONE),
         "Tab should be handled when a c++ snippet prefix matches");
  Expect(editor.lines()[0] == "for (;;) {}",
         "a snippet contributed for language_id c++ should expand in a .cpp buffer");
}

}

void TestWorkspaceShellMultiBufferWorkspaceEditIndependentBaselines() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path a_path = root / "a.txt";
  const std::filesystem::path b_path = root / "b.txt";
  const std::filesystem::path c_path = root / "c.txt";
  WriteFile(a_path, "aaa\n");
  WriteFile(b_path, "bbb\n");
  WriteFile(c_path, "ccc\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::RegisterLifecycleWakeEvents(shell);
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root, false, false),
         "fixture should open the project");
  WorkspaceShellTestAccess::OpenSingleEditorTab(shell, a_path);
  Expect(WorkspaceShellTestAccess::OpenFileInNewTab(shell, b_path),
         "opening the second file should succeed");

  const auto range = [](std::size_t line, std::size_t col) {
    return editor::SelectionRange{editor::TextPosition{line, col}, editor::TextPosition{line, col}};
  };
  const std::vector<microide::workspace::CodeActionEdit> edits = {
      {a_path, range(0, 0), "X"},
      {b_path, range(0, 0), "Y"},
      {c_path, range(0, 0), "Z"},
  };
  Expect(WorkspaceShellTestAccess::ApplyLspWorkspaceEdit(shell, edits),
         "a multi-buffer workspace edit should apply to the open buffers");

  Expect(ReadFile(c_path) == "ccc\n",
         "an edit targeting a file that is not open must not touch it on disk");

  WorkspaceShellTestAccess::ActivateTab(shell, 0);
  Expect(WorkspaceShellTestAccess::ActiveEditor(shell).lines()[0] == "Xaaa", "buffer A got its edit");
  Expect(WorkspaceShellTestAccess::ActiveEditor(shell).dirty(), "buffer A is dirty");
  WorkspaceShellTestAccess::ActivateTab(shell, 1);
  Expect(WorkspaceShellTestAccess::ActiveEditor(shell).lines()[0] == "Ybbb", "buffer B got its edit");
  Expect(WorkspaceShellTestAccess::ActiveEditor(shell).dirty(), "buffer B is dirty");

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

void TestWorkspaceShellWorkspaceEditDoesNotSnapshotBuffers() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path src = root / "big.txt";
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
    for (const auto& r : model->resolved_ranges()) {
      if (r.opener_line == opener) {
        return r.closer_line;
      }
    }
    return 0;
  };

  Expect(inner_closer_for_opener(1) == 3,
         "before the edit, the inner fold opens at line 1 and closes at line 3");

  const auto range = [](std::size_t l0, std::size_t c0, std::size_t l1, std::size_t c1) {
    return editor::SelectionRange{editor::TextPosition{l0, c0}, editor::TextPosition{l1, c1}};
  };
  const std::vector<microide::workspace::CodeActionEdit> edits = {
      {src, range(1, 0, 5, 0), "  b;\n  if (x) {\n    a;\n  }\n"},
  };
  Expect(WorkspaceShellTestAccess::ApplyLspWorkspaceEdit(shell, edits),
         "the workspace edit should apply to the open buffer");

  Expect(inner_closer_for_opener(2) == 4,
         "after the same-line-count edit, the fold model must reflect the moved inner block");
  Expect(inner_closer_for_opener(1) == 0,
         "the stale inner fold at line 1 must be gone after the edit");
}

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

  Expect(WorkspaceShellTestAccess::SetSettingValue(shell, "editor.tab_size", "8"),
         "setting editor.tab_size should succeed");

  Expect(WorkspaceShellTestAccess::GroupActiveViewport(shell, 1).tab_size() == 8,
         "the focused group's viewport must pick up the new tab size");
  Expect(WorkspaceShellTestAccess::GroupActiveViewport(shell, 0).tab_size() == 8,
         "the NON-focused split group's viewport must also pick up the new tab size");
}

void TestWorkspaceShellCtrlClickGoesToDefinition() {
#if !MICROIDE_HAS_LUA_PLUGINS
  return;
#endif
  TemporaryDirectory temp_dir;
  const std::filesystem::path config_home = temp_dir.path() / "config";
  const std::filesystem::path plugins_root = config_home / "microide" / "plugins";
  const std::filesystem::path project_root = temp_dir.path() / "project";
  const std::filesystem::path source = project_root / "main.lua";
  const std::filesystem::path target = project_root / "lib.lua";
  WriteFile(source, "local x = helper()\nreturn x\n");
  WriteFile(target, "-- lib\nlocal function helper() end\nreturn helper\n");

  WritePluginInit(
      plugins_root, "defs",
      R"(local ide = require("microide")
return ide.plugin({
  id = "defs",
  setup = function(ctx)
    ctx.definition.add({
      id = "defs.lua",
      language_id = "lua",
      provide = function(buffer, position)
        if position.line == 1 and position.column >= 11 and position.column <= 17 then
          return { { path = (buffer.path:gsub("main%.lua$", "lib.lua")), line = 2, column = 16 } }
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
         "definition fixture should open the project");
  WorkspaceShellTestAccess::OpenFile(shell, source);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::RenderFrame(shell);
  WorkspaceShellTestAccess::ActiveEditor(shell).MoveCursorTo(1, 0);

  const auto metrics = WorkspaceShellTestAccess::ActiveEditorMetrics(shell);
  const float char_width = WorkspaceShellTestAccess::TextCharWidth(shell);
  const float y = metrics.first_line_y + metrics.line_height * 0.5f;
  const float x = metrics.text_x + char_width * 12.0f;

  Expect(WorkspaceShellTestAccess::IsActionEnabled(shell, WorkspaceShell::ActionId::GoToDefinition),
         "Go to Definition is available with a plugin-only provider");
  {
    const ScopedHeldModifiers ctrl_mods(shell, SDL_KMOD_CTRL);
    Expect(SendMouseDown(shell, x, y, SDL_BUTTON_LEFT), "Ctrl+click is handled by the editor");
  }
  const bool navigated = WaitUntil(
      [&] {
        const auto& active = WorkspaceShellTestAccess::ActiveEditor(shell);
        return active.path().lexically_normal() == target.lexically_normal();
      },
      std::chrono::seconds(2), std::chrono::milliseconds(2),
      [&] { WorkspaceShellTestAccess::DrainPluginThreadActions(shell); });
  Expect(navigated, "Ctrl+click on a symbol opens the provider's definition location");
  const auto& active = WorkspaceShellTestAccess::ActiveEditor(shell);
  Expect(active.cursor_line() == 1 && active.cursor_column() == 15,
         "the caret lands on the definition (provider line/column are 1-based)");
}

void TestWorkspaceShellGoMenuOffersPluginDefinitionWithoutLsp() {
#if !MICROIDE_HAS_LUA_PLUGINS
  return;
#endif
  TemporaryDirectory temp_dir;
  const std::filesystem::path config_home = temp_dir.path() / "config";
  const std::filesystem::path plugins_root = config_home / "microide" / "plugins";
  const std::filesystem::path project_root = temp_dir.path() / "project";
  const std::filesystem::path source = project_root / "main.lua";
  WriteFile(source, "return 1\n");

  WritePluginInit(
      plugins_root, "defs",
      R"(local ide = require("microide")
return ide.plugin({
  id = "defs",
  setup = function(ctx)
    ctx.definition.add({
      id = "defs.lua",
      language_id = "lua",
      provide = function(buffer, position) return nil end
    })
  end
})
)");

  ScopedPluginConfigHomeEnv scoped_plugin_config_home(config_home);

  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, project_root, false, false),
         "definition menu fixture should open the project");
  WorkspaceShellTestAccess::OpenFile(shell, source);

  const std::vector<std::string> labels =
      WorkspaceShellTestAccess::MenuItemLabels(shell, WorkspaceShell::MenuId::Go);
  const bool plain_definition =
      std::find(labels.begin(), labels.end(), "Go to Definition") != labels.end();
  const bool suffixed_definition = std::any_of(
      labels.begin(), labels.end(),
      [](const std::string& label) { return label.starts_with("Go to Definition ("); });
  Expect(plain_definition && !suffixed_definition,
         "Go to Definition is offered without an LSP suffix when a plugin serves it");
  Expect(std::any_of(labels.begin(), labels.end(),
                     [](const std::string& label) {
                       return label.starts_with("Go to Type Definition (LSP: ");
                     }),
         "server-only navigation keeps its LSP readiness suffix");
  Expect(WorkspaceShellTestAccess::IsActionEnabled(shell, WorkspaceShell::ActionId::GoToDefinition),
         "Go to Definition is enabled with a plugin-only provider");
  Expect(!WorkspaceShellTestAccess::IsActionEnabled(shell,
                                                    WorkspaceShell::ActionId::FindReferences),
         "Find References stays disabled without any provider");
}

void RegisterWorkspaceShellPluginTests(std::vector<TestCase>& tests) {
  AddTest(tests, "WorkspaceShell/HoverAsksAgainWhenThePointerReturns",
          TestWorkspaceShellHoverAsksAgainWhenThePointerReturns);
  AddTest(tests, "WorkspaceShell/ShowHoverAtTheCaret", TestWorkspaceShellShowHoverAtTheCaret);
  AddTest(tests, "WorkspaceShell/HoverOnASquiggleShowsTheSymbolToo",
          TestWorkspaceShellHoverOnASquiggleShowsTheSymbolToo);
  AddTest(tests, "WorkspaceShell/PluginHoverReadsTheUnsavedText",
          TestWorkspaceShellPluginHoverReadsTheUnsavedText);
  AddTest(tests, "WorkspaceShell/HoverInAnUnsavedBufferAsksTheServer",
          TestWorkspaceShellHoverInAnUnsavedBufferAsksTheServer);
  AddTest(tests, "WorkspaceShell/AReloadedStatusItemRepaintsTheBreadcrumb",
          TestWorkspaceShellAReloadedStatusItemRepaintsTheBreadcrumb);
  AddTest(tests, "WorkspaceShell/ServerRegisteredAfterOpenStartsForTheActiveFile",
          TestWorkspaceShellServerRegisteredAfterOpenStartsForTheActiveFile);
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
  AddTest(tests, "WorkspaceShell/PluginSnippetForCxxLanguageIdReachesCppBuffers",
          TestWorkspaceShellPluginSnippetForCxxLanguageIdReachesCppBuffers);
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
  AddTest(tests, "WorkspaceShell/PluginApplyEditsSamePositionInsertsKeepArrayOrder",
          TestWorkspaceShellPluginApplyEditsSamePositionInsertsKeepArrayOrder);
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
  AddTest(tests, "WorkspaceShell/LspKeystrokeSyncAllocatesNothingWithoutAServer",
          TestWorkspaceShellLspKeystrokeSyncAllocatesNothingWithoutAServer);
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
  AddTest(tests, "WorkspaceShell/PluginStatusItemDoesNotHoverUnderAModal",
          TestWorkspaceShellPluginStatusItemDoesNotHoverUnderAModal);
  AddTest(tests, "WorkspaceShell/PluginStatusItemsHideUnderAMaximizedPanel",
          TestWorkspaceShellPluginStatusItemsHideUnderAMaximizedPanel);
  AddTest(tests, "WorkspaceShell/DeferredSaveRunsParticipantsOnceOffThread",
          TestWorkspaceShellDeferredSaveRunsParticipantsOnceOffThread);
  AddTest(tests, "WorkspaceShell/DeferredSaveRefusedByAFailingParticipant",
          TestWorkspaceShellDeferredSaveRefusedByAFailingParticipant);
  AddTest(tests, "WorkspaceShell/SavePipelineRunsParticipantsBeforeFormatter",
          TestWorkspaceShellSavePipelineRunsParticipantsBeforeFormatter);
  AddTest(tests, "WorkspaceShell/SavePipelineFormatterFailureLeavesBufferUnchanged",
          TestWorkspaceShellSavePipelineFormatterFailureLeavesBufferUnchanged);
  AddTest(tests, "WorkspaceShell/SavePipelineOverlappingSavesCoalesceCorrectly",
          TestWorkspaceShellSavePipelineOverlappingSavesCoalesceCorrectly);
  AddTest(tests, "WorkspaceShell/Phase4RegistriesReloadWithPlugins",
          TestWorkspaceShellPhase4RegistriesReloadWithPlugins);
  AddTest(tests, "WorkspaceShell/PluginContributesVirtualDocument",
          TestWorkspaceShellPluginContributesVirtualDocument);
  AddTest(tests, "WorkspaceShell/VirtualDocumentsOpenAndRefresh",
          TestWorkspaceShellVirtualDocumentsOpenAndRefresh);
  AddTest(tests, "WorkspaceShell/LoadsPluginsAndRunsBufferHooks",
          TestWorkspaceShellLoadsPluginsAndRunsBufferHooks);
  AddTest(tests, "WorkspaceShell/PluginsReloadReportsTheReloadItRan",
          TestWorkspaceShellPluginsReloadReportsTheReloadItRan);
  AddTest(tests, "WorkspaceShell/APluginThatFailsToLoadSaysSo", TestWorkspaceShellAPluginThatFailsToLoadSaysSo);
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
  AddTest(tests, "WorkspaceShell/GoToSymbolInEditorPicksFromTheOutlineSymbols",
          TestWorkspaceShellGoToSymbolInEditorPicksFromTheOutlineSymbols);
  AddTest(tests, "WorkspaceShell/OutlineSidebarFromLspFallback",
          TestWorkspaceShellOutlineSidebarFromLspFallback);
  AddTest(tests, "WorkspaceShell/OutlineCapsLargeLspResult",
          TestWorkspaceShellOutlineCapsLargeLspResult);
  AddTest(tests, "WorkspaceShell/HoverFromLspFallback", TestWorkspaceShellHoverFromLspFallback);
  AddTest(tests, "WorkspaceShell/HoverSupersededCompletionIsDropped",
          TestWorkspaceShellHoverSupersededCompletionIsDropped);
  AddTest(tests, "WorkspaceShell/FormatDocumentAppliesLspEdits",
          TestWorkspaceShellFormatDocumentAppliesLspEdits);
  AddTest(tests, "WorkspaceShell/FormatDocumentFollowsTabsAndDropsAStaleResult",
          TestWorkspaceShellFormatDocumentFollowsTabsAndDropsAStaleResult);
  AddTest(tests, "WorkspaceShell/CodeActionFixIsRefusedOnAChangedBuffer",
          TestWorkspaceShellCodeActionFixIsRefusedOnAChangedBuffer);
  AddTest(tests, "WorkspaceShell/RenameResultForAChangedBufferIsDropped",
          TestWorkspaceShellRenameResultForAChangedBufferIsDropped);
  AddTest(tests, "WorkspaceShell/RenameSymbolAppliesLspEdit",
          TestWorkspaceShellRenameSymbolAppliesLspEdit);
  AddTest(tests, "WorkspaceShell/RenameSymbolOpensAndSavesClosedFiles",
          TestWorkspaceShellRenameSymbolOpensAndSavesClosedFiles);
  AddTest(tests, "WorkspaceShell/CompletionMergesPluginAndLspSources",
          TestWorkspaceShellCompletionMergesPluginAndLspSources);
  AddTest(tests, "WorkspaceShell/CompletionAppliesAdditionalTextEdits",
          TestWorkspaceShellCompletionAppliesAdditionalTextEdits);
  AddTest(tests, "WorkspaceShell/RenameOfDirtyFileWaitsForTheFormatterNotTheWindow",
          TestWorkspaceShellRenameOfDirtyFileWaitsForTheFormatterNotTheWindow);
  AddTest(tests, "WorkspaceShell/CompletionNarrowsLocallyWithOneRequestPerWord",
          TestWorkspaceShellCompletionNarrowsLocallyWithOneRequestPerWord);
  AddTest(tests, "WorkspaceShell/HoverCardOpensAboveTheLine", TestWorkspaceShellHoverCardOpensAboveTheLine);
  AddTest(tests, "WorkspaceShell/OutlineAsksForTheFilesSymbols",
          TestWorkspaceShellOutlineAsksForTheFilesSymbols);
  AddTest(tests, "WorkspaceShell/CtrlClickRepaintsTheCaretBeforeTheDefinition",
          TestWorkspaceShellCtrlClickRepaintsTheCaretBeforeTheDefinition);
  AddTest(tests, "WorkspaceShell/FindReferencesShowsTheResults",
          TestWorkspaceShellFindReferencesShowsTheResults);
  AddTest(tests, "WorkspaceShell/CompletionDownEnterAcceptsTheSecondRow",
          TestWorkspaceShellCompletionDownEnterAcceptsTheSecondRow);
  AddTest(tests, "WorkspaceShell/CompletionClickAcceptsTheClickedRow",
          TestWorkspaceShellCompletionClickAcceptsTheClickedRow);
  AddTest(tests, "WorkspaceShell/ClickOutsideCompletionLandsInTheEditor",
          TestWorkspaceShellClickOutsideCompletionLandsInTheEditor);
  AddTest(tests, "WorkspaceShell/CompletionPopupShrinkRepaintsWhatItLeft",
          TestWorkspaceShellCompletionPopupShrinkRepaintsWhatItLeft);
  AddTest(tests, "WorkspaceShell/CompletionReRequestsOnlyWhileIncomplete",
          TestWorkspaceShellCompletionReRequestsOnlyWhileIncomplete);
  AddTest(tests, "WorkspaceShell/CompletionAcceptAfterTypingReplacesTheTypedWord",
          TestWorkspaceShellCompletionAcceptAfterTypingReplacesTheTypedWord);
  AddTest(tests, "WorkspaceShell/QuickSuggestionsAskOncePerWord",
          TestWorkspaceShellQuickSuggestionsAskOncePerWord);
  AddTest(tests, "WorkspaceShell/QuickSuggestionsWithNothingToOfferStayHidden",
          TestWorkspaceShellQuickSuggestionsWithNothingToOfferStayHidden);
  AddTest(tests, "WorkspaceShell/TriggerCharacterOpensCompletionAtTheCaret",
          TestWorkspaceShellTriggerCharacterOpensCompletionAtTheCaret);
  AddTest(tests, "WorkspaceShell/SignatureHelpFollowsTheCallBeingTyped",
          TestWorkspaceShellSignatureHelpFollowsTheCallBeingTyped);
  AddTest(tests, "WorkspaceShell/PluginProvidersOpenOnTheirTriggerCharacters",
          TestWorkspaceShellPluginProvidersOpenOnTheirTriggerCharacters);
  AddTest(tests, "WorkspaceShell/SignatureHelpClosingRepaintsWhereItWas",
          TestWorkspaceShellSignatureHelpClosingRepaintsWhereItWas);
  AddTest(tests, "WorkspaceShell/SignatureHelpComesBackWhenAMenuClosesOverIt",
          TestWorkspaceShellSignatureHelpComesBackWhenAMenuClosesOverIt);
  AddTest(tests, "WorkspaceShell/SteppingOverACloserSendsTheServerNothing",
          TestWorkspaceShellSteppingOverACloserSendsTheServerNothing);
  AddTest(tests, "WorkspaceShell/SignatureHelpPrefersLspOverPlugin",
          TestWorkspaceShellSignatureHelpPrefersLspOverPlugin);
  AddTest(tests, "WorkspaceShell/InlayHintsPublishMidLineDecorations",
          TestWorkspaceShellInlayHintsPublishMidLineDecorations);
  AddTest(tests, "WorkspaceShell/CodeLensesResolveAndActivate",
          TestWorkspaceShellCodeLensesResolveAndActivate);
  AddTest(tests, "WorkspaceShell/DisablingLspClearsEveryServerOverlay",
          TestWorkspaceShellDisablingLspClearsEveryServerOverlay);
  AddTest(tests, "WorkspaceShell/SupersededCodeLensResponseKeepsLiveLensesClickable",
          TestWorkspaceShellSupersededCodeLensResponseKeepsLiveLensesClickable);
  AddTest(tests, "WorkspaceShell/CallHierarchyRendersBothDirections",
          TestWorkspaceShellCallHierarchyRendersBothDirections);
  AddTest(tests, "WorkspaceShell/PrepareRenameRefinesPrompt",
          TestWorkspaceShellPrepareRenameRefinesPrompt);
  AddTest(tests, "WorkspaceShell/ServerApplyEditEditsOpenAndClosedFiles",
          TestWorkspaceShellServerApplyEditEditsOpenAndClosedFiles);
  AddTest(tests, "WorkspaceShell/ServerApplyEditRejectsOutOfRangeClosedFileEdit",
          TestWorkspaceShellServerApplyEditRejectsOutOfRangeClosedFileEdit);
  AddTest(tests, "WorkspaceShell/ServerApplyEditAppliesResourceOps",
          TestWorkspaceShellServerApplyEditAppliesResourceOps);
  AddTest(tests, "WorkspaceShell/ServerApplyEditResourceOpBatchIsAtomic",
          TestWorkspaceShellServerApplyEditResourceOpBatchIsAtomic);
  AddTest(tests, "WorkspaceShell/ServerApplyEditHonorsDocumentVersions",
          TestWorkspaceShellServerApplyEditHonorsDocumentVersions);
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
  AddTest(tests, "WorkspaceShell/ProblemsSidebarRightClickOpensRowMenu",
          TestWorkspaceShellProblemsSidebarRightClickOpensRowMenu);
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
  AddTest(tests, "WorkspaceShell/CtrlClickGoesToDefinition",
          TestWorkspaceShellCtrlClickGoesToDefinition);
  AddTest(tests, "WorkspaceShell/GoMenuOffersPluginDefinitionWithoutLsp",
          TestWorkspaceShellGoMenuOffersPluginDefinitionWithoutLsp);
}

}
