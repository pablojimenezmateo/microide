#include "TestSupport.h"

#include "WorkspaceShellTestAccess.h"

#include <chrono>
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
file="$4"
if grep -q "broken" "$file"; then
  printf '%s\n' '[{"messages":[{"ruleId":"no-broken","severity":2,"message":"Unexpected broken token","line":1,"column":1,"endLine":1,"endColumn":7}]}]'
  exit 1
fi
printf '%s\n' '[{"messages":[]}]'
exit 0
)");
  std::filesystem::permissions(
      eslint_path,
      std::filesystem::perms::owner_read | std::filesystem::perms::owner_write |
          std::filesystem::perms::owner_exec,
      std::filesystem::perm_options::replace);
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

  ScopedEnvVar xdg_config_home("XDG_CONFIG_HOME", config_home.string());

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

  ScopedEnvVar xdg_config_home("XDG_CONFIG_HOME", config_home.string());

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

  ScopedEnvVar xdg_config_home("XDG_CONFIG_HOME", config_home.string());

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

  const auto scheduled = WorkspaceShellTestAccess::HandleScheduledWake(shell);
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

  ScopedEnvVar xdg_config_home("XDG_CONFIG_HOME", config_home.string());

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
  Expect(WorkspaceShellTestAccess::HandleKeyEvent(shell, SDLK_RETURN, SDL_KMOD_NONE),
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

  ScopedEnvVar xdg_config_home("XDG_CONFIG_HOME", config_home.string());

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
  CopyRepoPlugin(plugins_root, "bookmarks");

  ScopedEnvVar xdg_config_home("XDG_CONFIG_HOME", config_home.string());

  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, project_root, false, false),
         "sidebar mode menu fixture should open the project");
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);

  const SDL_FRect button_rect = WorkspaceShellTestAccess::SidebarModeButtonRect(shell);
  const float click_x = button_rect.x + button_rect.w * 0.5f;
  const float click_y = button_rect.y + button_rect.h * 0.5f;
  Expect(WorkspaceShellTestAccess::HandleMouseButtonDown(shell, click_x, click_y, SDL_BUTTON_LEFT),
         "clicking the sidebar mode control should open the sidebar menu");
  Expect(WorkspaceShellTestAccess::SidebarModeMenuOpen(shell),
         "clicking the sidebar mode control should open the anchored sidebar menu");

  const auto labels = WorkspaceShellTestAccess::SidebarModeMenuLabels(shell);
  Expect(std::find(labels.begin(), labels.end(), "Bookmarks") != labels.end(),
         "sidebar mode menu should list loaded plugin sidebars by label");

  const auto bookmarks_rect = WorkspaceShellTestAccess::SidebarModeMenuItemRect(shell, "Bookmarks");
  Expect(bookmarks_rect.has_value(),
         "sidebar mode menu should expose a clickable menu row for the plugin sidebar");
  Expect(WorkspaceShellTestAccess::HandleMouseButtonDown(
             shell, bookmarks_rect->x + bookmarks_rect->w * 0.5f,
             bookmarks_rect->y + bookmarks_rect->h * 0.5f, SDL_BUTTON_LEFT),
         "clicking the plugin sidebar menu row should be handled");
  Expect(WorkspaceShellTestAccess::SidebarMode(shell) == WorkspaceShell::SidebarMode::Plugin,
         "selecting a plugin sidebar from the dropdown should activate plugin sidebar mode");
  Expect(WorkspaceShellTestAccess::SidebarViewId(shell) == "project-bookmarks",
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

  ScopedEnvVar xdg_config_home("XDG_CONFIG_HOME", config_home.string());

  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, project_a, false, false),
         "diagnostics project-switch fixture should open the first project");
  Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, "diagnostics.publish"),
         "diagnostics publish command should execute in the first project");

  const auto* project_a_diagnostics =
      WorkspaceShellTestAccess::DiagnosticsForPath(shell, project_a / "README.md");
  Expect(project_a_diagnostics != nullptr && project_a_diagnostics->size() == 1,
         "publishing diagnostics should store them on the active project");
  Expect(project_a_diagnostics->front().message == project_a.lexically_normal().string(),
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
  Expect(restored_diagnostics->front().message == project_a.lexically_normal().string(),
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
  Expect(WorkspaceShellTestAccess::HandleMouseMotion(shell, hover_x, hover_y, 0),
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

  ScopedEnvVar xdg_config_home("XDG_CONFIG_HOME", config_home.string());

  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, project_root, false, false),
         "plugin hover fixture should open the project");
  WorkspaceShellTestAccess::OpenFile(shell, source);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);

  const auto metrics = WorkspaceShellTestAccess::ActiveEditorMetrics(shell);
  const float hover_x =
      metrics.text_x + WorkspaceShellTestAccess::TextCharWidth(shell) * 2.0f;
  const float hover_y = metrics.first_line_y + metrics.line_height * 0.5f;
  Expect(WorkspaceShellTestAccess::HandleMouseMotion(shell, hover_x, hover_y, 0),
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
  ScopedEnvVar xdg_config_home("XDG_CONFIG_HOME", config_home.string());

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
  Expect(WorkspaceShellTestAccess::HandleMouseMotion(shell, hover_x, hover_y, 0),
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
  WriteFile(project_root / "README.md", "eslint fixture\n");
  WriteFile(source, "const answer = 1;\n");
  CopyRepoPlugin(plugins_root, "eslint");
  WriteFakeEslint(project_root);

  ScopedEnvVar xdg_config_home("XDG_CONFIG_HOME", config_home.string());

  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, project_root, false, false),
         "eslint plugin fixture should open the project");
  WorkspaceShellTestAccess::OpenFile(shell, source);

  auto& editor = WorkspaceShellTestAccess::ActiveEditor(shell);
  editor.SelectAll();
  editor.InsertText("broken();\n");
  Expect(WorkspaceShellTestAccess::SaveTab(shell, WorkspaceShellTestAccess::ActiveTabIndex(shell)),
         "eslint plugin fixture should save the edited JavaScript buffer");

  const auto* broken_diagnostics = WorkspaceShellTestAccess::DiagnosticsForPath(shell, source);
  Expect(broken_diagnostics != nullptr && broken_diagnostics->size() == 1,
         "saving a broken JavaScript file should publish one ESLint diagnostic");
  Expect(broken_diagnostics->front().message == "Unexpected broken token (no-broken)",
         "ESLint plugin diagnostics should preserve the formatter message and rule id");

  Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, "eslint.show-problems"),
         "eslint.show-problems should open the built-in Problems sidebar");
  Expect(WorkspaceShellTestAccess::SidebarMode(shell) == WorkspaceShell::SidebarMode::Problems,
         "eslint.show-problems should route through the host Problems sidebar");
  Expect(WorkspaceShellTestAccess::ProblemsSidebarEntries(shell).size() == 1,
         "published ESLint diagnostics should appear in the Problems sidebar");

  WorkspaceShellTestAccess::OpenFile(shell, source);
  auto& clean_editor = WorkspaceShellTestAccess::ActiveEditor(shell);
  clean_editor.SelectAll();
  clean_editor.InsertText("const answer = 1;\n");
  Expect(WorkspaceShellTestAccess::SaveTab(shell, WorkspaceShellTestAccess::ActiveTabIndex(shell)),
         "eslint plugin fixture should save the cleaned JavaScript buffer");
  Expect(WorkspaceShellTestAccess::DiagnosticsForPath(shell, source) == nullptr,
         "saving a clean JavaScript file should clear the plugin's diagnostics");
  Expect(WorkspaceShellTestAccess::ProblemsSidebarEntries(shell).empty(),
         "clearing ESLint diagnostics should refresh the Problems sidebar");
}

void TestWorkspaceShellRepoBookmarksPluginAddsSidebarItems() {
#if !MICROIDE_HAS_LUA_PLUGINS
  return;
#endif
  TemporaryDirectory temp_dir;
  const std::filesystem::path config_home = temp_dir.path() / "config";
  const std::filesystem::path plugins_root = config_home / "microide" / "plugins";
  const std::filesystem::path project_root = temp_dir.path() / "project";
  const std::filesystem::path readme = project_root / "README.md";
  const std::filesystem::path source = project_root / "src" / "main.txt";
  WriteFile(readme, "bookmarks fixture\n");
  WriteFile(source, "alpha\nbeta\n");
  CopyRepoPlugin(plugins_root, "bookmarks");

  ScopedEnvVar xdg_config_home("XDG_CONFIG_HOME", config_home.string());

  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, project_root, false, false),
         "bookmarks plugin fixture should open the project");
  WorkspaceShellTestAccess::OpenFile(shell, source);
  WorkspaceShellTestAccess::ActiveEditor(shell).MoveCursorTo(1, 2);

  Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, "bookmarks.add Beta hotspot"),
         "bookmarks.add should execute through the shell command prompt");
  Expect(WorkspaceShellTestAccess::SidebarMode(shell) == WorkspaceShell::SidebarMode::Plugin,
         "bookmarks.add should show the plugin sidebar after writing the bookmark");
  Expect(WorkspaceShellTestAccess::SidebarViewId(shell) == "project-bookmarks",
         "bookmarks.add should activate the repo plugin's sidebar id");
  Expect(WorkspaceShellTestAccess::PluginSidebarItems(shell).size() == 1,
         "bookmarks sidebar should expose the saved bookmark");
  Expect(WorkspaceShellTestAccess::PluginSidebarItems(shell).front().label == "Beta hotspot" &&
             WorkspaceShellTestAccess::PluginSidebarItems(shell).front().detail == "src/main.txt:2:3",
         "bookmarks sidebar items should preserve the active buffer location");
  Expect(ReadFile(project_root / ".microide" / "bookmarks.tsv") ==
             "src/main.txt\t2\t3\tBeta hotspot\n",
         "bookmarks plugin should persist project-local storage in .microide/bookmarks.tsv");

  WorkspaceShellTestAccess::OpenFile(shell, readme);
  Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, "bookmarks.show"),
         "bookmarks.show should reopen the plugin sidebar");
  Expect(WorkspaceShellTestAccess::HandleKeyEvent(shell, SDLK_RETURN, SDL_KMOD_NONE),
         "pressing Enter in the bookmarks sidebar should confirm the selected item");
  Expect(WorkspaceShellTestAccess::ActiveEditor(shell).path() == source.lexically_normal(),
         "bookmark confirmation should reopen the target file");
  Expect(WorkspaceShellTestAccess::ActiveEditor(shell).cursor_line() == 1 &&
             WorkspaceShellTestAccess::ActiveEditor(shell).cursor_column() == 2,
         "bookmark confirmation should restore the saved cursor location");
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

  Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, "sidebar-show problems"),
         "sidebar-show should activate the built-in problems sidebar");
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

  Expect(WorkspaceShellTestAccess::HandleKeyEvent(shell, SDLK_RETURN, SDL_KMOD_NONE),
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
  CopyRepoPlugin(plugins_root, "bookmarks");

  ScopedEnvVar xdg_config_home("XDG_CONFIG_HOME", config_home.string());

  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, project_a, false, false),
         "plugin project-switch fixture should open the first project");
  WorkspaceShellTestAccess::OpenFile(shell, source_a);
  WorkspaceShellTestAccess::ActiveEditor(shell).MoveCursorTo(1, 2);
  Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, "bookmarks.add Alpha bookmark"),
         "plugin project-switch fixture should create one project-local bookmark");
  Expect(WorkspaceShellTestAccess::SidebarMode(shell) == WorkspaceShell::SidebarMode::Plugin,
         "adding the bookmark should activate the plugin sidebar");
  Expect(WorkspaceShellTestAccess::SidebarViewId(shell) == "project-bookmarks",
         "adding the bookmark should record the plugin sidebar view id");
  Expect(WorkspaceShellTestAccess::PluginSidebarItems(shell).size() == 1,
         "adding the bookmark should populate the plugin sidebar");

  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, project_b, false, false),
         "plugin project-switch fixture should open the second project");
  Expect(WorkspaceShellTestAccess::SidebarMode(shell) == WorkspaceShell::SidebarMode::Tree,
         "a fresh project should keep its default tree sidebar");
  Expect(WorkspaceShellTestAccess::SidebarViewId(shell) == "tree",
         "a fresh project should keep the default tree sidebar view id");

  Expect(WorkspaceShellTestAccess::SwitchProject(shell, 0, false),
         "plugin project-switch fixture should switch back to the first project");
  Expect(WorkspaceShellTestAccess::SidebarMode(shell) == WorkspaceShell::SidebarMode::Plugin,
         "switching back should restore the first project's plugin sidebar mode");
  Expect(WorkspaceShellTestAccess::SidebarViewId(shell) == "project-bookmarks",
         "switching back should restore the first project's plugin sidebar view id");
  Expect(WorkspaceShellTestAccess::PluginSidebarItems(shell).size() == 1 &&
             WorkspaceShellTestAccess::PluginSidebarItems(shell).front().label ==
                 "Alpha bookmark",
         "switching back should restore the first project's plugin sidebar items");
}

}  // namespace

void RegisterWorkspaceShellPluginTests(std::vector<TestCase>& tests) {
  AddTest(tests, "WorkspaceShell/LoadsPluginsAndRunsBufferHooks",
          TestWorkspaceShellLoadsPluginsAndRunsBufferHooks);
  AddTest(tests, "WorkspaceShell/PluginsReloadCommandRefreshesCommands",
          TestWorkspaceShellPluginsReloadCommandRefreshesCommands);
  AddTest(tests, "WorkspaceShell/PluginsReloadRefreshesRuntimeSyntaxHighlighting",
          TestWorkspaceShellPluginsReloadRefreshesRuntimeSyntaxHighlighting);
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
  AddTest(tests, "WorkspaceShell/RepoEslintPluginPublishesDiagnosticsOnSave",
          TestWorkspaceShellRepoEslintPluginPublishesDiagnosticsOnSave);
  AddTest(tests, "WorkspaceShell/RepoBookmarksPluginAddsSidebarItems",
          TestWorkspaceShellRepoBookmarksPluginAddsSidebarItems);
  AddTest(tests, "WorkspaceShell/ProblemsSidebarOpensSelectedDiagnostic",
          TestWorkspaceShellProblemsSidebarOpensSelectedDiagnostic);
  AddTest(tests, "WorkspaceShell/ProblemsSidebarPersistsAcrossProjectSwitches",
          TestWorkspaceShellProblemsSidebarPersistsAcrossProjectSwitches);
  AddTest(tests, "WorkspaceShell/PluginSidebarPersistsAcrossProjectSwitches",
          TestWorkspaceShellPluginSidebarPersistsAcrossProjectSwitches);
}

}  // namespace microide::tests
