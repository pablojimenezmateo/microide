#include "TestSupport.h"

#include "workspace/WorkspaceShellTesting.h"

#include <filesystem>
#include <string_view>
#include <vector>

namespace microide::tests {
namespace {

using microide::workspace::MessageRole;
using microide::workspace::WorkspaceShell;
using WorkspaceShellTestAccess = microide::workspace::WorkspaceShell::TestAccess;

void WritePluginInit(const std::filesystem::path& root,
                     std::string_view directory_name,
                     std::string_view content) {
  WriteFile(root / directory_name / "init.lua", std::string(content));
}

void TestPhase3CommandSurfaceDrivesCompletionTasksAndTests() {
#if !MICROIDE_HAS_LUA_PLUGINS
  return;
#endif
  TemporaryDirectory temp_dir;
  const std::filesystem::path config_home = temp_dir.path() / "config";
  const std::filesystem::path plugins_root = config_home / "microide" / "plugins";
  const std::filesystem::path project_root = temp_dir.path() / "project";
  const std::filesystem::path source = project_root / "README.md";
  WriteFile(source, "alpha\n");

  WritePluginInit(
      plugins_root, "phase-runtime",
      R"lua(local ide = require("microide")
return ide.plugin({
  id = "phase-runtime",
  setup = function(ctx)
    ctx.commands.add("phase-runtime.echo", function(ctx, args)
      ctx.log("code-action:" .. table.concat(args, ":"))
    end)

    ctx.completion.add({
      id = "markdown",
      language_id = "markdown",
      provide = function(buffer, position, trigger)
        return {
          {
            label = "ALPHA",
            detail = buffer.relative_path,
            documentation = "phase3",
            insert_text = "ALPHA()"
          }
        }
      end
    })

    ctx.code_actions.add({
      id = "markdown",
      language_id = "markdown",
      provide = function(buffer, range)
        return {
          {
            title = "Echo action",
            command = "phase-runtime.echo",
            arguments = { buffer.relative_path, tostring(range.start.line) }
          }
        }
      end
    })

    ctx.tasks.add({
      id = "build",
      label = "Build",
      group = "build",
      command = { "printf", "task-line\n" }
    })

    ctx.tests.add({
      id = "markdown",
      language_id = "markdown",
      discover = function(buffer)
        return {
          {
            id = "phase-runtime.case",
            label = "Markdown case",
            file = buffer.relative_path,
            line = 1
          }
        }
      end,
      run = function(test_ids)
        return {
          {
            test_id = test_ids[1],
            state = "passed",
            message = "ok",
            duration_ms = 5
          }
        }
      end
    })
  end
})
)lua");

  ScopedEnvVar xdg_config_home("XDG_CONFIG_HOME", config_home.string());

  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, project_root, false, false),
         "phase3 command fixture should open the project");
  WorkspaceShellTestAccess::OpenFile(shell, source);
  WorkspaceShellTestAccess::ActiveEditor(shell).MoveCursorTo(0, 5);

  Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, "completion"),
         "completion command should execute");
  Expect(WorkspaceShellTestAccess::OverlayVisible(shell) &&
             WorkspaceShellTestAccess::ActiveOverlayMode(shell) ==
                 WorkspaceShell::OverlayMode::Completion,
         "completion command should open the completion overlay");
  Expect(WorkspaceShellTestAccess::CompletionSession(shell).items.size() == 1 &&
             WorkspaceShellTestAccess::CompletionSession(shell).items.front().insert_text ==
                 "ALPHA()",
         "completion command should populate overlay items from runtime providers");
  Expect(WorkspaceShellTestAccess::ApplySelectedCompletion(shell),
         "selected completion should apply through the shell");
  Expect(WorkspaceShellTestAccess::ActiveEditor(shell).lines().front() == "ALPHA()",
         "applying the selected completion should edit the active buffer");

  Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, "code-actions"),
         "code-actions command should execute");
  Expect(WorkspaceShellTestAccess::OverlayVisible(shell) &&
             WorkspaceShellTestAccess::ActiveOverlayMode(shell) ==
                 WorkspaceShell::OverlayMode::CodeActions,
         "code-actions command should open the action overlay");
  Expect(WorkspaceShellTestAccess::ExecuteSelectedCodeAction(shell),
         "selected code action should execute");
  Expect(!WorkspaceShellTestAccess::PluginMessages(shell).empty() &&
             WorkspaceShellTestAccess::PluginMessages(shell).back() ==
                 "phase-runtime: code-action:README.md:1",
         "executing a code action from the overlay should dispatch the returned command");

  Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, "tasks phase-runtime.build"),
         "tasks command should run a named task");
  Expect(WorkspaceShellTestAccess::WaitForTaskRuntimeIdle(shell),
         "task runtime should finish for the test task");
  const auto* task_channel =
      WorkspaceShellTestAccess::OutputChannelEntries(shell, "task.phase-runtime.build");
  Expect(task_channel != nullptr && !task_channel->empty() && task_channel->front() == "task-line",
         "task execution should stream host-owned output into the task output channel");

  Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, "tests-discover"),
         "tests-discover command should execute");
  Expect(WorkspaceShellTestAccess::SidebarViewId(shell) == "tests" &&
             WorkspaceShellTestAccess::TestsSidebarEntries(shell).size() == 1 &&
             WorkspaceShellTestAccess::TestsSidebarEntries(shell).front().id ==
                 "phase-runtime.case",
         "tests-discover should populate the tests sidebar from the runtime provider");
  Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, "tests-run"),
         "tests-run command should execute");
  Expect(WorkspaceShellTestAccess::TestsSidebarEntries(shell).front().status == "passed",
         "tests-run should update the tests sidebar with the provider result");
}

void TestPhase5AiCommandsDriveChatAndInlineCompletion() {
#if !MICROIDE_HAS_LUA_PLUGINS
  return;
#endif
  TemporaryDirectory temp_dir;
  const std::filesystem::path config_home = temp_dir.path() / "config";
  const std::filesystem::path plugins_root = config_home / "microide" / "plugins";
  const std::filesystem::path project_root = temp_dir.path() / "project";
  const std::filesystem::path source = project_root / "README.md";
  WriteFile(source, "value = ");

  WritePluginInit(
      plugins_root, "phase5-ai",
      R"lua(local ide = require("microide")
return ide.plugin({
  id = "phase5-ai",
  setup = function(ctx)
    ctx.external_agents.add({
      id = "chat",
      label = "Chat Agent",
      protocol = "stdio",
      endpoint = "sh -lc \"printf 'assistant reply'\"",
      capabilities = { "chat" }
    })
    ctx.external_agents.add({
      id = "inline",
      label = "Inline Agent",
      protocol = "stdio",
      endpoint = "sh -lc \"printf 'inline_tail'\"",
      capabilities = { "inline-completion" }
    })
  end
})
)lua");

  ScopedEnvVar xdg_config_home("XDG_CONFIG_HOME", config_home.string());

  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, project_root, false, false),
         "phase5 ai fixture should open the project");
  WorkspaceShellTestAccess::OpenFile(shell, source);

  Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, "chat hello from tests"),
         "chat command should execute");
  Expect(WorkspaceShellTestAccess::WaitForAiRuntimeIdle(shell),
         "chat runtime should complete");
  const auto messages = WorkspaceShellTestAccess::ActiveConversationMessages(shell);
  Expect(WorkspaceShellTestAccess::PanelContent(shell) == WorkspaceShell::PanelContentKind::Chat,
         "chat command should surface the chat panel");
  Expect(messages.size() == 2 && messages.front().role == MessageRole::User &&
             messages.front().content == "hello from tests" &&
             messages.back().role == MessageRole::Assistant &&
             messages.back().content == "assistant reply",
         "chat command should record both the user message and the assistant reply");
  Expect(WorkspaceShellTestAccess::ActiveConversationProviderId(shell) == "phase5-ai.chat",
         "chat conversations should record the selected external agent");

  WorkspaceShellTestAccess::ActiveEditor(shell).MoveCursorTo(0, 8);
  Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, "inline-complete"),
         "inline-complete command should execute");
  Expect(WorkspaceShellTestAccess::WaitForAiRuntimeIdle(shell),
         "inline completion runtime should complete");
  Expect(WorkspaceShellTestAccess::InlineCompletion(shell).visible &&
             WorkspaceShellTestAccess::InlineCompletion(shell).text == "inline_tail",
         "inline-complete should populate visible ghost text");
  Expect(WorkspaceShellTestAccess::AcceptInlineCompletion(shell),
         "accepting the inline completion should succeed");
  Expect(WorkspaceShellTestAccess::ActiveEditor(shell).lines().front() == "value = inline_tail",
         "accepting the inline completion should insert the returned text");
}

void TestPhase5AuthAndMcpCommandsUpdateVisibleHostState() {
#if !MICROIDE_HAS_LUA_PLUGINS
  return;
#endif
  TemporaryDirectory temp_dir;
  const std::filesystem::path config_home = temp_dir.path() / "config";
  const std::filesystem::path plugins_root = config_home / "microide" / "plugins";
  const std::filesystem::path project_root = temp_dir.path() / "project";
  WriteFile(project_root / "README.md", "phase5 auth and mcp\n");

  WritePluginInit(
      plugins_root, "phase5-systems",
      R"lua(local ide = require("microide")
return ide.plugin({
  id = "phase5-systems",
  setup = function(ctx)
    ctx.scm.add("sample", "Sample SCM")
    ctx.auth.add({
      id = "github",
      label = "GitHub",
      login = function(scopes)
        return {
          id = "session-1",
          account = "octocat",
          access_token = "token-alpha",
          scopes = scopes
        }
      end,
      refresh = function(session_id)
        return {
          id = session_id,
          account = "octocat",
          access_token = "token-beta",
          scopes = { "repo" }
        }
      end,
      logout = function(session_id)
      end
    })
    ctx.mcp_tools.add({
      id = "echo",
      name = "Echo",
      description = "Echoes input",
      input_schema = "{}",
      run = function(input_json)
        return { output = "echo:" .. input_json }
      end
    })
  end
})
)lua");

  ScopedEnvVar xdg_config_home("XDG_CONFIG_HOME", config_home.string());

  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, project_root, false, false),
         "phase5 auth fixture should open the project");
  WorkspaceShellTestAccess::ShowGitSidebar(shell);
  const auto initial_summary = WorkspaceShellTestAccess::GitSidebarSummaryLines(shell);
  Expect(initial_summary.size() >= 2 &&
             initial_summary.front().find("Sample SCM") != std::string::npos &&
             initial_summary[1].find("GitHub") != std::string::npos,
         "git sidebar summaries should expose SCM and auth providers");

  Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, "auth-login phase5-systems.github repo"),
         "auth-login command should execute");
  auto sessions =
      WorkspaceShellTestAccess::AuthSessions(shell, "phase5-systems.github");
  Expect(sessions.size() == 1 && sessions.front().account == "octocat" &&
             sessions.front().access_token == "token-alpha",
         "auth-login should create a host-managed auth session");
  const auto logged_in_summary = WorkspaceShellTestAccess::GitSidebarSummaryLines(shell);
  Expect(logged_in_summary.size() >= 2 &&
             logged_in_summary[1].find("GitHub (1)") != std::string::npos,
         "git sidebar summaries should reflect live auth session counts");

  Expect(WorkspaceShellTestAccess::ExecuteCommandLine(
             shell, "auth-refresh phase5-systems.github session-1"),
         "auth-refresh command should execute");
  sessions = WorkspaceShellTestAccess::AuthSessions(shell, "phase5-systems.github");
  Expect(sessions.size() == 1 && sessions.front().access_token == "token-beta",
         "auth-refresh should replace the stored session data");

  Expect(WorkspaceShellTestAccess::ExecuteCommandLine(
             shell, "mcp phase5-systems.echo '{\"ping\":1}'"),
         "mcp command should execute");
  Expect(WorkspaceShellTestAccess::PanelContent(shell) == WorkspaceShell::PanelContentKind::Output,
         "mcp command should surface the output panel");
  const auto* mcp_channel =
      WorkspaceShellTestAccess::OutputChannelEntries(shell, "mcp.phase5-systems.echo");
  Expect(mcp_channel != nullptr && !mcp_channel->empty() &&
             mcp_channel->back() == "echo:{\"ping\":1}",
         "mcp command should append tool output to a host-owned output channel");

  Expect(WorkspaceShellTestAccess::ExecuteCommandLine(
             shell, "auth-logout phase5-systems.github session-1"),
         "auth-logout command should execute");
  Expect(WorkspaceShellTestAccess::AuthSessions(shell, "phase5-systems.github").empty(),
         "auth-logout should remove the stored session");
}

}  // namespace

void RegisterPhase5Tests(std::vector<TestCase>& tests) {
  AddTest(tests, "Phase5.CommandSurfaceDrivesPhase3Runtime",
          TestPhase3CommandSurfaceDrivesCompletionTasksAndTests);
  AddTest(tests, "Phase5.AiCommandsDriveChatAndInlineCompletion",
          TestPhase5AiCommandsDriveChatAndInlineCompletion);
  AddTest(tests, "Phase5.AuthAndMcpCommandsUpdateVisibleHostState",
          TestPhase5AuthAndMcpCommandsUpdateVisibleHostState);
}

}  // namespace microide::tests
