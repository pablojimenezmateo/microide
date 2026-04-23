#include "TestSupport.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include "workspace/WorkspaceShellTesting.h"

#include <filesystem>
#include <string_view>
#include <thread>
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

template <typename Predicate>
bool WaitForLspCondition(WorkspaceShell& shell, Predicate&& ready, int timeout_ms = 2000) {
  const Uint64 deadline =
      SDL_GetTicks() + static_cast<Uint64>(timeout_ms > 0 ? timeout_ms : 0);
  while (!ready() && SDL_GetTicks() <= deadline) {
    WorkspaceShellTestAccess::ConsumeLspCallbacks(shell);
    std::this_thread::yield();
  }
  WorkspaceShellTestAccess::ConsumeLspCallbacks(shell);
  return ready();
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
  Expect(WorkspaceShellTestAccess::SidebarMode(shell) == WorkspaceShell::SidebarMode::Chat,
         "chat command should surface the chat sidebar");
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

void TestPhase5LspCommandsDriveDiagnosticsNavigationAndActions() {
#if !MICROIDE_HAS_LUA_PLUGINS
  return;
#endif
  TemporaryDirectory temp_dir;
  const std::filesystem::path config_home = temp_dir.path() / "config";
  const std::filesystem::path plugins_root = config_home / "microide" / "plugins";
  const std::filesystem::path project_root = temp_dir.path() / "project";
  const std::filesystem::path server_path = project_root / "fake_lsp.py";
  const std::filesystem::path source = project_root / "README.md";
  const std::filesystem::path refs = project_root / "refs.md";
  WriteFile(source, "alpha\nusage\nafter\n");
  WriteFile(refs, "before-def\ndefinition\nafter-def\n");
  WriteFile(
      server_path,
      R"py(#!/usr/bin/env python3
import json
import pathlib
import sys
from urllib.parse import quote

project_root = None

def write_message(payload):
    body = json.dumps(payload).encode("utf-8")
    sys.stdout.buffer.write(f"Content-Length: {len(body)}\r\n\r\n".encode("ascii"))
    sys.stdout.buffer.write(body)
    sys.stdout.buffer.flush()

def read_message():
    content_length = None
    while True:
        line = sys.stdin.buffer.readline()
        if not line:
            return None
        line = line.decode("utf-8").strip()
        if not line:
            break
        if line.startswith("Content-Length:"):
            content_length = int(line.split(":", 1)[1].strip())
    if content_length is None:
        return None
    body = sys.stdin.buffer.read(content_length)
    if not body:
        return None
    return json.loads(body.decode("utf-8"))

def file_uri(path):
    return "file://" + quote(str(path).replace("\\", "/"), safe="/-._~")

while True:
    msg = read_message()
    if msg is None:
      break
    method = msg.get("method")
    if method == "initialize":
        root_uri = msg.get("params", {}).get("rootUri", "")
        if root_uri.startswith("file://"):
            project_root = pathlib.Path(root_uri[len("file://"):])
        write_message({
            "jsonrpc": "2.0",
            "id": msg["id"],
            "result": {
                "capabilities": {
                    "textDocumentSync": 2,
                    "completionProvider": {},
                    "codeActionProvider": True,
                    "definitionProvider": True,
                    "referencesProvider": True,
                }
            },
        })
    elif method == "initialized":
        pass
    elif method == "textDocument/didOpen":
        uri = msg["params"]["textDocument"]["uri"]
        write_message({
            "jsonrpc": "2.0",
            "method": "textDocument/publishDiagnostics",
            "params": {
                "uri": uri,
                "diagnostics": [
                    {
                        "range": {
                            "start": {"line": 0, "character": 0},
                            "end": {"line": 0, "character": 5},
                        },
                        "severity": 2,
                        "message": "LSP warning",
                    }
                ],
            },
        })
    elif method == "textDocument/completion":
        write_message({
            "jsonrpc": "2.0",
            "id": msg["id"],
            "result": [
                {
                    "label": "lsp-item",
                    "detail": "detail",
                    "documentation": "docs",
                    "insertText": "lsp_insert()",
                }
            ],
        })
    elif method == "textDocument/codeAction":
        write_message({
            "jsonrpc": "2.0",
            "id": msg["id"],
            "result": [
                {
                    "title": "Echo action",
                    "command": {
                        "command": "phase5-lsp.echo",
                        "arguments": ["README.md", "action"]
                    }
                }
            ],
        })
    elif method == "textDocument/definition":
        write_message({
            "jsonrpc": "2.0",
            "id": msg["id"],
            "result": {
                "uri": file_uri(project_root / "refs.md"),
                "range": {
                    "start": {"line": 0, "character": 0},
                    "end": {"line": 0, "character": 10},
                },
            },
        })
    elif method == "textDocument/references":
        write_message({
            "jsonrpc": "2.0",
            "id": msg["id"],
            "result": [
                {
                    "uri": file_uri(project_root / "README.md"),
                    "range": {
                        "start": {"line": 1, "character": 0},
                        "end": {"line": 1, "character": 5},
                    },
                },
                {
                    "uri": file_uri(project_root / "refs.md"),
                    "range": {
                        "start": {"line": 1, "character": 0},
                        "end": {"line": 1, "character": 10},
                    },
                },
            ],
        })
    elif method == "shutdown":
        write_message({"jsonrpc": "2.0", "id": msg["id"], "result": None})
    elif method == "exit":
        break
)py");

  WritePluginInit(
      plugins_root, "phase5-lsp",
      std::string(R"lua(local ide = require("microide")
return ide.plugin({
  id = "phase5-lsp",
  setup = function(ctx)
    ctx.commands.add("phase5-lsp.echo", function(ctx, args)
      ctx.log("lsp-action:" .. table.concat(args, ":"))
    end)
    ctx.lsp.add({
      id = "markdown",
      language_id = "markdown",
      command = { "python3", ")lua") +
          server_path.generic_string() +
          std::string(R"lua(" }
    })
  end
})
)lua"));

  ScopedEnvVar xdg_config_home("XDG_CONFIG_HOME", config_home.string());

  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, project_root, false, false),
         "phase5 lsp fixture should open the project");
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::OpenFile(shell, source);
  WorkspaceShellTestAccess::ActiveEditor(shell).MoveCursorTo(0, 5);
  Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, "find-references"),
         "find-references should execute before LSP initialization completes");
  Expect(
      WaitForLspCondition(shell, [&] {
        const auto* channel =
            WorkspaceShellTestAccess::OutputChannelEntries(shell, "lsp.references");
        return channel != nullptr &&
               std::find(channel->begin(), channel->end(), "README.md:2:1") != channel->end();
      }),
      "find-references should complete after asynchronous initialization and document open");
  Expect(
      WaitForLspCondition(shell, [&] {
        const auto* diagnostics = WorkspaceShellTestAccess::DiagnosticsForPath(shell, source);
        return diagnostics != nullptr && diagnostics->size() == 1;
      }),
      "didOpen diagnostics should flow into the host diagnostics store");

  const auto* diagnostics = WorkspaceShellTestAccess::DiagnosticsForPath(shell, source);
  Expect(diagnostics != nullptr && diagnostics->size() == 1 &&
             diagnostics->front().message == "LSP warning",
         "didOpen diagnostics should flow into the host diagnostics store");

  WorkspaceShellTestAccess::ActiveEditor(shell).MoveCursorTo(0, 5);
  Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, "completion"),
         "completion should execute with an LSP-backed provider");
  Expect(
      WaitForLspCondition(shell, [&] {
        const auto& session = WorkspaceShellTestAccess::CompletionSession(shell);
        return !session.items.empty() || session.error != "Loading...";
      }),
      "completion should populate overlay items from the language server");
  Expect(WorkspaceShellTestAccess::CompletionSession(shell).items.size() == 1 &&
             WorkspaceShellTestAccess::CompletionSession(shell).items.front().insert_text ==
                 "lsp_insert()",
         "completion should populate overlay items from the language server");
  Expect(WorkspaceShellTestAccess::ApplySelectedCompletion(shell),
         "LSP completion should apply through the existing completion overlay");
  Expect(WorkspaceShellTestAccess::ActiveEditor(shell).lines().front() == "lsp_insert()",
         "applying an LSP completion should edit the active buffer");

  Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, "code-actions"),
         "code-actions should execute with an LSP-backed provider");
  Expect(
      WaitForLspCondition(shell, [&] {
        const auto& session = WorkspaceShellTestAccess::CodeActionSession(shell);
        return !session.items.empty() || session.error != "Loading...";
      }),
      "code actions should parse nested LSP command payloads");
  Expect(WorkspaceShellTestAccess::CodeActionSession(shell).items.size() == 1 &&
             WorkspaceShellTestAccess::CodeActionSession(shell).items.front().command ==
                 "phase5-lsp.echo",
         "code actions should parse nested LSP command payloads");
  Expect(WorkspaceShellTestAccess::ExecuteSelectedCodeAction(shell),
         "selected LSP code action should dispatch through the normal command surface");
  Expect(!WorkspaceShellTestAccess::PluginMessages(shell).empty() &&
             WorkspaceShellTestAccess::PluginMessages(shell).back() ==
                 "phase5-lsp: lsp-action:README.md:action",
         "executing an LSP code action should invoke the returned command");

  Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, "find-references"),
         "find-references should execute");
  Expect(
      WaitForLspCondition(shell, [&] {
        const auto* channel =
            WorkspaceShellTestAccess::OutputChannelEntries(shell, "lsp.references");
        return channel != nullptr && !channel->empty();
      }),
      "find-references should publish host-owned output lines with local context");
  const auto* references_channel =
      WorkspaceShellTestAccess::OutputChannelEntries(shell, "lsp.references");
  Expect(references_channel != nullptr &&
             std::find(references_channel->begin(), references_channel->end(), "README.md:2:1") !=
                 references_channel->end() &&
             std::find(references_channel->begin(), references_channel->end(), "refs.md:2:1") !=
                 references_channel->end() &&
             std::find(references_channel->begin(), references_channel->end(), "") !=
                 references_channel->end() &&
             std::find(references_channel->begin(), references_channel->end(),
                       " > 2 | definition") != references_channel->end() &&
             std::find(references_channel->begin(), references_channel->end(),
                       " > 2 | usage") != references_channel->end(),
         "find-references should publish host-owned output lines with local context");
  const std::vector<std::string> bottom_panel_tabs =
      WorkspaceShellTestAccess::BottomPanelTabDisplayTitles(shell);
  Expect(std::find(bottom_panel_tabs.begin(), bottom_panel_tabs.end(), "LSP References") !=
             bottom_panel_tabs.end(),
         "find-references should expose references in their own bottom panel tab");

  WorkspaceShellTestAccess::EnsureTerminalTab(shell);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, "find-references"),
         "find-references should keep terminal tabs available");
  Expect(
      WaitForLspCondition(shell, [&] {
        return WorkspaceShellTestAccess::PanelContent(shell) ==
               WorkspaceShell::PanelContentKind::Output;
      }),
      "find-references should route output to the output panel");
  const SDL_FRect terminal_tab = WorkspaceShellTestAccess::ActiveTerminalTabRect(shell);
  Expect(WorkspaceShellTestAccess::HandleMouseButtonDown(
             shell, terminal_tab.x + terminal_tab.w * 0.5f,
             terminal_tab.y + terminal_tab.h * 0.5f, SDL_BUTTON_LEFT),
         "clicking a terminal tab while output is visible should be handled");
  Expect(WorkspaceShellTestAccess::PanelContent(shell) ==
             WorkspaceShell::PanelContentKind::Terminal,
         "clicking a terminal tab while output is visible should switch back to the terminal");

  const auto references_tab = WorkspaceShellTestAccess::BottomPanelTabRectByTitle(
      shell, "LSP References");
  Expect(references_tab.has_value() &&
             WorkspaceShellTestAccess::HandleMouseButtonDown(
                 shell, references_tab->x + references_tab->w * 0.5f,
                 references_tab->y + references_tab->h * 0.5f, SDL_BUTTON_LEFT),
         "clicking the references tab should be handled");
  Expect(WorkspaceShellTestAccess::PanelContent(shell) ==
             WorkspaceShell::PanelContentKind::Output,
         "clicking the references tab should switch to output content");

  Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, "find-references"),
         "find-references should execute before clicking output references");
  Expect(
      WaitForLspCondition(shell, [&] {
        return WorkspaceShellTestAccess::PanelContent(shell) ==
               WorkspaceShell::PanelContentKind::Output;
      }),
      "find-references should show output for link navigation");
  const auto* clickable_references_channel =
      WorkspaceShellTestAccess::OutputChannelEntries(shell, "lsp.references");
  Expect(clickable_references_channel != nullptr,
         "find-references should keep output channel entries for click navigation");
  const auto refs_anchor = clickable_references_channel != nullptr
                               ? std::find(clickable_references_channel->begin(),
                                           clickable_references_channel->end(),
                                           "refs.md:2:1")
                               : std::vector<std::string>::const_iterator{};
  Expect(clickable_references_channel != nullptr &&
             refs_anchor != clickable_references_channel->end(),
         "find-references should include a clickable refs anchor");
  const SDL_FPoint output_origin = WorkspaceShellTestAccess::BottomPanelTextOrigin(shell);
  const float line_height = WorkspaceShellTestAccess::TextLineHeight(shell);
  const std::size_t refs_anchor_row =
      clickable_references_channel != nullptr && refs_anchor != clickable_references_channel->end()
          ? static_cast<std::size_t>(
                std::distance(clickable_references_channel->begin(), refs_anchor))
          : 0;
  Expect(WorkspaceShellTestAccess::HandleMouseButtonDown(
             shell, output_origin.x + 8.0f,
             output_origin.y + line_height * (static_cast<float>(refs_anchor_row) + 0.5f),
             SDL_BUTTON_LEFT),
         "clicking a reference line in the output panel should be handled");
  Expect(WorkspaceShellTestAccess::ActiveEditor(shell).path().lexically_normal() == refs &&
             WorkspaceShellTestAccess::ActiveEditor(shell).cursor_line() == 1 &&
             WorkspaceShellTestAccess::ActiveEditor(shell).cursor_column() == 0,
         "clicking a reference line in the output panel should open the referenced location");

  Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, "goto-definition"),
         "goto-definition should execute");
  Expect(
      WaitForLspCondition(shell, [&] {
        return WorkspaceShellTestAccess::ActiveEditor(shell).path().lexically_normal() == refs &&
               WorkspaceShellTestAccess::ActiveEditor(shell).cursor_line() == 0 &&
               WorkspaceShellTestAccess::ActiveEditor(shell).cursor_column() == 0;
      }),
      "goto-definition should open the returned location and move the caret");
  Expect(WorkspaceShellTestAccess::ActiveEditor(shell).path().lexically_normal() == refs &&
             WorkspaceShellTestAccess::ActiveEditor(shell).cursor_line() == 0 &&
             WorkspaceShellTestAccess::ActiveEditor(shell).cursor_column() == 0,
         "goto-definition should open the returned location and move the caret");
}

void TestPhase5LspMergeBuffersPublishDiagnosticsAndBufferHooks() {
#if !MICROIDE_HAS_LUA_PLUGINS
  return;
#endif
  TemporaryDirectory temp_dir;
  const std::filesystem::path config_home = temp_dir.path() / "config";
  const std::filesystem::path plugins_root = config_home / "microide" / "plugins";
  const std::filesystem::path project_root = temp_dir.path() / "project";
  const std::filesystem::path server_path = project_root / "fake_lsp.py";
  const std::filesystem::path readme = project_root / "README.md";
  const std::filesystem::path merge_base = project_root / "merge-base.md";
  const std::filesystem::path merge_incoming = project_root / "merge-incoming.md";
  const std::filesystem::path merge_current = project_root / "merge-current.md";
  const std::filesystem::path merge_output = project_root / "merge.md";
  WriteFile(readme, "readme\n");
  WriteFile(merge_base, "base\n");
  WriteFile(merge_incoming, "incoming\n");
  WriteFile(merge_current, "current\n");
  WriteFile(merge_output, "result\n");
  WriteFile(
      server_path,
      R"py(#!/usr/bin/env python3
import json
import sys

def write_message(payload):
    body = json.dumps(payload).encode("utf-8")
    sys.stdout.buffer.write(f"Content-Length: {len(body)}\r\n\r\n".encode("ascii"))
    sys.stdout.buffer.write(body)
    sys.stdout.buffer.flush()

def read_message():
    content_length = None
    while True:
        line = sys.stdin.buffer.readline()
        if not line:
            return None
        line = line.decode("utf-8").strip()
        if not line:
            break
        if line.startswith("Content-Length:"):
            content_length = int(line.split(":", 1)[1].strip())
    if content_length is None:
        return None
    body = sys.stdin.buffer.read(content_length)
    if not body:
        return None
    return json.loads(body.decode("utf-8"))

while True:
    msg = read_message()
    if msg is None:
        break
    method = msg.get("method")
    if method == "initialize":
        write_message({
            "jsonrpc": "2.0",
            "id": msg["id"],
            "result": {
                "capabilities": {
                    "textDocumentSync": 2
                }
            },
        })
    elif method == "textDocument/didOpen":
        uri = msg["params"]["textDocument"]["uri"]
        write_message({
            "jsonrpc": "2.0",
            "method": "textDocument/publishDiagnostics",
            "params": {
                "uri": uri,
                "diagnostics": [
                    {
                        "range": {
                            "start": {"line": 0, "character": 0},
                            "end": {"line": 0, "character": 6},
                        },
                        "severity": 2,
                        "message": "merge warning",
                    }
                ],
            },
        })
    elif method == "shutdown":
        write_message({"jsonrpc": "2.0", "id": msg["id"], "result": None})
    elif method == "exit":
        break
)py");

  WritePluginInit(
      plugins_root, "phase5-lsp-merge",
      std::string(R"lua(local ide = require("microide")
return ide.plugin({
  id = "phase5-lsp-merge",
  on_buffer_open = function(ctx, buffer)
    ctx.log("buffer-open:" .. buffer.relative_path)
  end,
  setup = function(ctx)
    ctx.lsp.add({
      id = "markdown",
      language_id = "markdown",
      command = { "python3", ")lua") +
          server_path.generic_string() +
          std::string(R"lua(" }
    })
  end
})
)lua"));

  ScopedEnvVar xdg_config_home("XDG_CONFIG_HOME", config_home.string());

  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, project_root, false, false),
         "phase5 merge lsp fixture should open the project");
  WorkspaceShellTestAccess::ClearPluginMessages(shell);
  Expect(WorkspaceShellTestAccess::OpenMergeEditor(shell, merge_base, merge_incoming,
                                                   merge_current, merge_output),
         "merge editor should open for LSP lifecycle validation");
  Expect(
      WaitForLspCondition(shell, [&] {
        const auto* diagnostics = WorkspaceShellTestAccess::DiagnosticsForPath(shell, merge_output);
        return diagnostics != nullptr && diagnostics->size() == 1;
      }),
      "editable merge buffers should publish didOpen diagnostics through the same LSP path");

  const auto* diagnostics =
      WorkspaceShellTestAccess::DiagnosticsForPath(shell, merge_output);
  Expect(diagnostics != nullptr && diagnostics->size() == 1 &&
             diagnostics->front().message == "merge warning",
         "editable merge buffers should publish didOpen diagnostics through the same LSP path");
  Expect(!WorkspaceShellTestAccess::PluginMessages(shell).empty() &&
             WorkspaceShellTestAccess::PluginMessages(shell).back() ==
                 "phase5-lsp-merge: buffer-open:merge.md",
         "opening an editable merge buffer should trigger the normal plugin buffer-open hook");
}

}  // namespace

void RegisterPhase5Tests(std::vector<TestCase>& tests) {
  AddTest(tests, "Phase5.CommandSurfaceDrivesPhase3Runtime",
          TestPhase3CommandSurfaceDrivesCompletionTasksAndTests);
  AddTest(tests, "Phase5.AiCommandsDriveChatAndInlineCompletion",
          TestPhase5AiCommandsDriveChatAndInlineCompletion);
  AddTest(tests, "Phase5.AuthAndMcpCommandsUpdateVisibleHostState",
          TestPhase5AuthAndMcpCommandsUpdateVisibleHostState);
  AddTest(tests, "Phase5.LspCommandsDriveDiagnosticsNavigationAndActions",
          TestPhase5LspCommandsDriveDiagnosticsNavigationAndActions);
  AddTest(tests, "Phase5.LspMergeBuffersPublishDiagnosticsAndBufferHooks",
          TestPhase5LspMergeBuffersPublishDiagnosticsAndBufferHooks);
}

}  // namespace microide::tests
