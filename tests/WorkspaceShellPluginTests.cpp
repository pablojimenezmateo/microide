#include "TestSupport.h"

#include "WorkspaceShellTestAccess.h"

#include <filesystem>
#include <string>
#include <vector>

namespace microide::tests {
namespace {

using microide::workspace::WorkspaceShell;
using microide::workspace::WorkspaceShellTestAccess;

void WritePluginInit(const std::filesystem::path& root,
                     std::string_view directory_name,
                     std::string_view content) {
  WriteFile(root / directory_name / "init.lua", std::string(content));
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

  ScopedEnvVar xdg_config_home("XDG_CONFIG_HOME", config_home.string());

  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, project_root, false, false),
         "plugin shell fixture should open the project");
  Expect(WorkspaceShellTestAccess::PluginErrors(shell).empty(),
         "plugin shell fixture should load without plugin errors");
  Expect(WorkspaceShellTestAccess::PluginMessages(shell).size() >= 2,
         "opening a project should trigger plugin project-open and startup buffer-open hooks");
  Expect(WorkspaceShellTestAccess::PluginMessages(shell)[0] == "events: project-open:project",
         "project-open hook should run when the project is activated");

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

  ScopedEnvVar xdg_config_home("XDG_CONFIG_HOME", config_home.string());

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

}  // namespace

void RegisterWorkspaceShellPluginTests(std::vector<TestCase>& tests) {
  AddTest(tests, "WorkspaceShell/LoadsPluginsAndRunsBufferHooks",
          TestWorkspaceShellLoadsPluginsAndRunsBufferHooks);
  AddTest(tests, "WorkspaceShell/PluginsReloadCommandRefreshesCommands",
          TestWorkspaceShellPluginsReloadCommandRefreshesCommands);
}

}  // namespace microide::tests
