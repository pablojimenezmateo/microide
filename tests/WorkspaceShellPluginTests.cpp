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
  Expect(WorkspaceShellTestAccess::SidebarPluginId(shell) == "plugin-problems",
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
  Expect(WorkspaceShellTestAccess::ProblemsSidebarEntries(shell).size() == 1 &&
             WorkspaceShellTestAccess::ProblemsSidebarEntries(shell).front().primary_label ==
                 "Alpha issue",
         "switching back should restore the first project's problems entries");
  Expect(WorkspaceShellTestAccess::ProblemsSidebarEntries(shell).front().detail_label ==
             "README.md:1:1 | lint-a",
         "restored problems should preserve their location metadata");
}

}  // namespace

void RegisterWorkspaceShellPluginTests(std::vector<TestCase>& tests) {
  AddTest(tests, "WorkspaceShell/LoadsPluginsAndRunsBufferHooks",
          TestWorkspaceShellLoadsPluginsAndRunsBufferHooks);
  AddTest(tests, "WorkspaceShell/PluginsReloadCommandRefreshesCommands",
          TestWorkspaceShellPluginsReloadCommandRefreshesCommands);
  AddTest(tests, "WorkspaceShell/PluginSidebarOpensItems",
          TestWorkspaceShellPluginSidebarOpensItems);
  AddTest(tests, "WorkspaceShell/PluginDiagnosticsPersistAcrossProjectSwitches",
          TestWorkspaceShellPluginDiagnosticsPersistAcrossProjectSwitches);
  AddTest(tests, "WorkspaceShell/DiagnosticHoverPopupShowsMessages",
          TestWorkspaceShellDiagnosticHoverPopupShowsMessages);
  AddTest(tests, "WorkspaceShell/ProblemsSidebarOpensSelectedDiagnostic",
          TestWorkspaceShellProblemsSidebarOpensSelectedDiagnostic);
  AddTest(tests, "WorkspaceShell/ProblemsSidebarPersistsAcrossProjectSwitches",
          TestWorkspaceShellProblemsSidebarPersistsAcrossProjectSwitches);
}

}  // namespace microide::tests
