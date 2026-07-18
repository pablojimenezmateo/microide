#include "TestSupport.h"

#include "workspace/WorkspaceLspClient.h"
#include "workspace/WorkspaceLspManager.h"
#include "workspace/WorkspaceShellTestAccess.h"

#include <algorithm>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace microide::tests {
namespace {

using microide::workspace::WorkspaceShell;
using WorkspaceShellTestAccess = microide::workspace::WorkspaceShell::TestAccess;
using ActionId = WorkspaceShell::ActionId;

bool LabelsContain(const std::vector<std::string>& labels, std::string_view needle) {
  return std::any_of(labels.begin(), labels.end(), [&](const std::string& label) {
    return label.find(needle) != std::string::npos;
  });
}

// The action-availability gate honors the per-feature and master LSP toggles. Uses
// FormatDocument / RenameSymbol / SignatureHelp, which gate on the buffer + feature
// alone (no language-server presence), so the toggle is the only variable.
void TestLspActionGatingRespectsToggles() {
  TemporaryDirectory temp;
  const std::filesystem::path project = temp.path() / "proj";
  const std::filesystem::path file = project / "main.py";
  WriteFile(file, "value = 1\n");

  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, project, false, false),
         "fixture project should open");
  WorkspaceShellTestAccess::OpenFile(shell, file);

  // Defaults: an editable buffer is active and every LSP feature is on.
  Expect(WorkspaceShellTestAccess::IsActionEnabled(shell, ActionId::FormatDocument),
         "Format Document is offered by default");
  Expect(WorkspaceShellTestAccess::IsActionEnabled(shell, ActionId::RenameSymbol),
         "Rename Symbol is offered by default");
  Expect(WorkspaceShellTestAccess::IsActionEnabled(shell, ActionId::SignatureHelp),
         "Signature Help is offered by default");

  // Per-feature toggles (set AFTER opening so the project config reload cannot reset
  // them) disable exactly their own action.
  Expect(WorkspaceShellTestAccess::SetSettingValue(shell, "lsp.formatting.enabled", "false"),
         "formatting toggle should write");
  Expect(!WorkspaceShellTestAccess::IsActionEnabled(shell, ActionId::FormatDocument),
         "disabling lsp.formatting.enabled hides Format Document");
  Expect(WorkspaceShellTestAccess::IsActionEnabled(shell, ActionId::RenameSymbol),
         "disabling formatting must not affect Rename Symbol");

  // The master switch overrides all per-feature toggles.
  Expect(WorkspaceShellTestAccess::SetSettingValue(shell, "lsp.enabled", "false"),
         "master toggle should write");
  Expect(!WorkspaceShellTestAccess::IsActionEnabled(shell, ActionId::RenameSymbol),
         "master off disables Rename Symbol");
  Expect(!WorkspaceShellTestAccess::IsActionEnabled(shell, ActionId::SignatureHelp),
         "master off disables Signature Help");

  Expect(WorkspaceShellTestAccess::SetSettingValue(shell, "lsp.enabled", "true"),
         "master toggle should flip back");
  Expect(WorkspaceShellTestAccess::IsActionEnabled(shell, ActionId::RenameSymbol),
         "re-enabling the master restores Rename Symbol");
}

// LSP-driven context-menu items are HIDDEN (not merely greyed) when their feature is
// off, the master is off, or nothing is configured to serve the active file. A
// configured (test-stub) server is used so the baseline shows the items present.
void TestLspMenuItemsHideWhenDisabled() {
  TemporaryDirectory temp;
  const std::filesystem::path project = temp.path() / "proj";
  const std::filesystem::path file = project / "main.py";
  WriteFile(file, "value = 1\n");

  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, project, false, false),
         "fixture project should open");

  // Before any server is configured, the LSP entries are hidden (not configured).
  WorkspaceShellTestAccess::OpenFile(shell, file);
  {
    const auto labels =
        WorkspaceShellTestAccess::MenuItemLabels(shell, WorkspaceShell::MenuId::EditorContext);
    Expect(!LabelsContain(labels, "Go to Definition"),
           "with no server configured, Go to Definition is hidden, not greyed");
    Expect(LabelsContain(labels, "Cut") || LabelsContain(labels, "Paste"),
           "non-LSP editor items stay present when LSP items are hidden");
  }

  // Attach an in-process stub server for python so the provider is now configured.
  auto stub = std::make_unique<workspace::LspClient>();
  stub->EnableTestStubMode();
  WorkspaceShellTestAccess::LspManagerForTesting(shell).InstallTestClientForTesting(
      "python", std::move(stub));

  {
    const auto labels =
        WorkspaceShellTestAccess::MenuItemLabels(shell, WorkspaceShell::MenuId::EditorContext);
    Expect(LabelsContain(labels, "Go to Definition"),
           "a configured server makes Go to Definition appear");
    Expect(LabelsContain(labels, "Find References"), "Find References appears too");
    Expect(LabelsContain(labels, "Code Actions"), "Code Actions appears too");
  }

  // Disabling one feature hides only that entry.
  Expect(WorkspaceShellTestAccess::SetSettingValue(shell, "lsp.goto_definition.enabled", "false"),
         "goto-definition toggle should write");
  {
    const auto labels =
        WorkspaceShellTestAccess::MenuItemLabels(shell, WorkspaceShell::MenuId::EditorContext);
    Expect(!LabelsContain(labels, "Go to Definition"),
           "disabling lsp.goto_definition.enabled hides Go to Definition");
    Expect(LabelsContain(labels, "Find References"),
           "sibling LSP entries stay while one feature is disabled");
  }

  // The master switch hides every LSP entry at once, leaving non-LSP items intact.
  Expect(WorkspaceShellTestAccess::SetSettingValue(shell, "lsp.enabled", "false"),
         "master toggle should write");
  {
    const auto labels =
        WorkspaceShellTestAccess::MenuItemLabels(shell, WorkspaceShell::MenuId::EditorContext);
    Expect(!LabelsContain(labels, "Go to Definition") &&
               !LabelsContain(labels, "Find References") &&
               !LabelsContain(labels, "Code Actions") && !LabelsContain(labels, "Completions"),
           "master off hides every LSP context-menu entry");
    Expect(LabelsContain(labels, "Cut") || LabelsContain(labels, "Paste"),
           "master off leaves ordinary editor items in place");
  }
}

// Passive menu measurement must NOT start a language server. Menu geometry,
// labels, and enablement compute LSP readiness only to display availability;
// starting the server there means merely opening or hovering the editor context
// menu spawns a server process (TD-2026-07-17A-001). Register a server WITHOUT
// starting it (a long-lived `/bin/cat` command that would stay alive if spawned),
// then assert reading the menu leaves it unstarted.
void TestMenuReadDoesNotStartLspServer() {
  TemporaryDirectory temp;
  const std::filesystem::path project = temp.path() / "proj";
  const std::filesystem::path file = project / "main.py";
  WriteFile(file, "value = 1\n");

  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, project, false, false),
         "fixture project should open");
  WorkspaceShellTestAccess::OpenFile(shell, file);

  // A registered-but-unstarted server: `HasServer` is true (so the LSP menu
  // entries surface and their labels/enablement query readiness), but nothing is
  // running yet. `/bin/cat` blocks on stdin, so IF the menu read starts it the
  // process stays alive and IsServerRunning flips true — a deterministic probe.
  workspace::LspManager& manager = WorkspaceShellTestAccess::LspManagerForTesting(shell);
  manager.RegisterServer({"python"}, {"/bin/cat"}, "file://" + project.string(),
                         project.string(), /*eager_start=*/false);
  Expect(manager.HasServer("python"), "server is registered");
  Expect(!manager.IsServerRunning("python"), "server is not started yet");

  // Reading the editor context menu labels is a passive UI read that touches the
  // LSP-driven entries' labels (and thus ActiveLspReadinessSnapshot).
  const auto labels =
      WorkspaceShellTestAccess::MenuItemLabels(shell, WorkspaceShell::MenuId::EditorContext);
  Expect(LabelsContain(labels, "Go to Definition"),
         "a registered server surfaces the LSP menu entry");

  // The fix: passive reads pass ensure_started=false, so GetServer/Start was never
  // called and the server was never spawned.
  Expect(!manager.IsServerRunning("python"),
         "reading the menu must not start the language server");
  Expect(manager.LastServerError("python").empty(),
         "reading the menu must not even attempt to spawn the server");
}

// TD-2026-07-17-091: retiring an LSP server hands the client to a host-owned pool
// (which outlives per-project state) instead of blocking the shell thread on the
// shutdown handshake. Retire clears the project manager's servers, and the pool
// drains asynchronously via ConsumeLspCallbacks.
void TestLspServerRetirementRoutesThroughHostPool() {
  TemporaryDirectory temp;
  const std::filesystem::path project = temp.path() / "proj";
  const std::filesystem::path file = project / "main.py";
  WriteFile(file, "value = 1\n");

  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, project, false, false),
         "fixture project should open");
  WorkspaceShellTestAccess::OpenFile(shell, file);

  auto stub = std::make_unique<workspace::LspClient>();
  stub->EnableTestStubMode();
  WorkspaceShellTestAccess::LspManagerForTesting(shell).InstallTestClientForTesting(
      "python", std::move(stub));
  Expect(WorkspaceShellTestAccess::LspManagerForTesting(shell).HasServer("python"),
         "stub server should be registered");

  WorkspaceShellTestAccess::RetireCurrentProjectLspServers(shell);

  // Retire moved the client out of the (still-live) project manager without blocking.
  Expect(!WorkspaceShellTestAccess::LspManagerForTesting(shell).HasServer("python"),
         "retire should clear the project manager's servers");

  // The client now drains from the host-owned pool over frames. Pump callbacks until
  // the pool empties (bounded, no sleeps — the stub's shutdown thread completes fast).
  bool drained = false;
  for (int i = 0; i < 100000; ++i) {
    if (WorkspaceShellTestAccess::LspRetiringClientCount(shell) == 0) {
      drained = true;
      break;
    }
    WorkspaceShellTestAccess::ConsumeLspCallbacks(shell);
    // Yield so the stub client's shutdown thread is scheduled even when the test
    // suite saturates every core; the reap then observes IsShutdownComplete.
    std::this_thread::yield();
  }
  Expect(drained, "the host-owned retirement pool should drain the retiring client");
}

}  // namespace

void RegisterWorkspaceShellLspSettingsTests(std::vector<TestCase>& tests) {
  AddTest(tests, "WorkspaceShellLspSettings/ServerRetirementRoutesThroughHostPool",
          TestLspServerRetirementRoutesThroughHostPool);
  AddTest(tests, "WorkspaceShellLspSettings/ActionGatingRespectsToggles",
          TestLspActionGatingRespectsToggles);
  AddTest(tests, "WorkspaceShellLspSettings/MenuItemsHideWhenDisabled",
          TestLspMenuItemsHideWhenDisabled);
  AddTest(tests, "WorkspaceShellLspSettings/MenuReadDoesNotStartLspServer",
          TestMenuReadDoesNotStartLspServer);
}

}  // namespace microide::tests
