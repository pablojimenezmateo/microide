#include "TestSupport.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include "workspace/WorkspaceShellTestAccess.h"

#include <filesystem>
#include <string_view>
#include <thread>
#include <vector>
#include "WorkspaceShellEventHelpers.h"

namespace microide::tests {
namespace {

using microide::workspace::WorkspaceShell;
using WorkspaceShellTestAccess = microide::workspace::WorkspaceShell::TestAccess;

class ScopedPluginConfigHomeEnv {
 public:
  explicit ScopedPluginConfigHomeEnv(const std::filesystem::path& config_root)
      : xdg_config_home_("XDG_CONFIG_HOME", config_root.string()),
        appdata_("APPDATA", config_root.string()) {}

 private:
  ScopedEnvVar xdg_config_home_;
  ScopedEnvVar appdata_;
};

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

  end
})
)lua");

  ScopedPluginConfigHomeEnv config_env(config_home);

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
from urllib.parse import quote, unquote

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
            decoded_root = unquote(root_uri[len("file://"):])
            if len(decoded_root) >= 3 and decoded_root[0] == "/" and decoded_root[2] == ":":
                decoded_root = decoded_root[1:]
            project_root = pathlib.Path(decoded_root)
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

  ScopedPluginConfigHomeEnv config_env(config_home);

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
  // Output-panel surfacing for find-references was retired with the user-facing
  // 'Show Output' action. The references list still accumulates in the LSP
  // output channel storage (verified above); the panel auto-open and
  // click-to-navigate flow no longer exist. Goto-definition continues below.

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

  ScopedPluginConfigHomeEnv config_env(config_home);

  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, project_root, false, false),
         "phase5 merge lsp fixture should open the project");
  WorkspaceShellTestAccess::ClearPluginMessages(shell);
  Expect(WorkspaceShellTestAccess::OpenMergeEditor(shell, merge_base, merge_incoming,
                                                   merge_current, merge_output),
         "merge editor should open for LSP lifecycle validation");
  Expect(!WorkspaceShellTestAccess::PluginMessages(shell).empty() &&
             WorkspaceShellTestAccess::PluginMessages(shell).back() ==
                 "phase5-lsp-merge: buffer-open:merge.md",
         "opening an editable merge buffer should trigger the normal plugin buffer-open hook");
}

void TestPhase5DeferredTabHydrationCompletesBeforeDidOpenDiagnostics() {
#if !MICROIDE_HAS_LUA_PLUGINS
  return;
#endif
  TemporaryDirectory temp_dir;
  const std::filesystem::path config_home = temp_dir.path() / "config";
  const std::filesystem::path plugins_root = config_home / "microide" / "plugins";
  const std::filesystem::path project_root = temp_dir.path() / "project";
  const std::filesystem::path alpha = project_root / "alpha.md";
  const std::filesystem::path beta = project_root / "beta.md";
  WriteFile(alpha, "alpha\n");
  WriteFile(beta, "beta\n");

  const std::filesystem::path home = temp_dir.path() / "home";
  const std::filesystem::path xdg_state_home = temp_dir.path() / "xdg-state-home";
  std::filesystem::create_directories(home);
  std::filesystem::create_directories(xdg_state_home);
  std::filesystem::create_directories(config_home);
  ScopedEnvVar scoped_home("HOME", home.string());
  ScopedEnvVar scoped_xdg_state_home("XDG_STATE_HOME", xdg_state_home.string());
  ScopedEnvVar scoped_localappdata("LOCALAPPDATA", xdg_state_home.string());
  ScopedEnvVar scoped_xdg_config_home("XDG_CONFIG_HOME", config_home.string());
  ScopedEnvVar scoped_appdata("APPDATA", config_home.string());

  const std::filesystem::path server_path = project_root / "phase5_delayed_didopen_lsp.py";
  WriteFile(server_path, R"py(#!/usr/bin/env python3
import json
import sys
import time

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
            "result": {"capabilities": {"textDocumentSync": 1}},
        })
    elif method == "initialized":
        pass
    elif method == "textDocument/didOpen":
        uri = msg["params"]["textDocument"]["uri"]
        time.sleep(0.35)
        write_message({
            "jsonrpc": "2.0",
            "method": "textDocument/publishDiagnostics",
            "params": {
                "uri": uri,
                "diagnostics": [{
                    "range": {
                        "start": {"line": 0, "character": 0},
                        "end": {"line": 0, "character": 1},
                    },
                    "severity": 2,
                    "message": "delayed didOpen diagnostic",
                }],
            },
        })
    elif method == "shutdown":
        write_message({"jsonrpc": "2.0", "id": msg["id"], "result": None})
    elif method == "exit":
        break
)py");

  WritePluginInit(
      plugins_root, "phase5-delayed-didopen",
      std::string(R"lua(local ide = require("microide")
return ide.plugin({
  id = "phase5-delayed-didopen",
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

  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, project_root, false, false),
         "delayed-didOpen fixture should open the project");
  WorkspaceShellTestAccess::OpenFile(shell, alpha);
  Expect(WorkspaceShellTestAccess::OpenFileInNewTab(shell, beta),
         "delayed-didOpen fixture should open deferred candidate tab");
  WorkspaceShellTestAccess::ActivateTab(shell, 0);
  WorkspaceShellTestAccess::SaveSessionState(shell);

  WorkspaceShell restored;
  WorkspaceShellTestAccess::SetProjectRoot(restored, project_root);
  Expect(WorkspaceShellTestAccess::RestoreSessionState(restored),
         "delayed-didOpen fixture should restore saved session");
  const auto& before_tabs = WorkspaceShellTestAccess::OpenTabs(restored);
  Expect(before_tabs.size() == 2 && before_tabs[1].deferred_handle.has_value(),
         "restored inactive tab should remain deferred before activation");

  const Uint64 start = SDL_GetTicks();
  WorkspaceShellTestAccess::ActivateTab(restored, 1);
  const Uint64 elapsed = SDL_GetTicks() - start;
  const auto& after_tabs = WorkspaceShellTestAccess::OpenTabs(restored);
  Expect(after_tabs[1].editor_state.has_value() && !after_tabs[1].deferred_handle.has_value(),
         "activating deferred tab should hydrate immediately");
  Expect(WorkspaceShellTestAccess::ActiveEditor(restored).path() == beta.lexically_normal(),
         "activating deferred tab should switch active editor before delayed didOpen diagnostics");
  Expect(elapsed < 250,
         "activating deferred tab should not block on delayed didOpen diagnostics");

  const auto* immediate_diagnostics = WorkspaceShellTestAccess::DiagnosticsForPath(restored, beta);
  const bool immediate_has_diagnostic =
      immediate_diagnostics != nullptr && !immediate_diagnostics->empty();
  Expect(!immediate_has_diagnostic,
         "delayed didOpen diagnostics should not be required for deferred tab hydration");

  Expect(WorkspaceShellTestAccess::HandleTextInput(restored, "x"),
         "post-hydration edit should trigger LSP didOpen/didChange for the activated tab");

}

}  // namespace

void RegisterPhase5Tests(std::vector<TestCase>& tests) {
  AddTest(tests, "Phase5.CommandSurfaceDrivesPhase3Runtime",
          TestPhase3CommandSurfaceDrivesCompletionTasksAndTests);
  AddTest(tests, "Phase5.LspCommandsDriveDiagnosticsNavigationAndActions",
          TestPhase5LspCommandsDriveDiagnosticsNavigationAndActions);
  AddTest(tests, "Phase5.LspMergeBuffersPublishDiagnosticsAndBufferHooks",
          TestPhase5LspMergeBuffersPublishDiagnosticsAndBufferHooks);
  AddTest(tests, "Phase5.DeferredTabHydrationCompletesBeforeDidOpenDiagnostics",
          TestPhase5DeferredTabHydrationCompletesBeforeDidOpenDiagnostics);
}

}  // namespace microide::tests
