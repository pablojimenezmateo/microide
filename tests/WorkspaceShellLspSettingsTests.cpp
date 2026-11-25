#include "TestSupport.h"

#include "workspace/lsp/WorkspaceLspClient.h"
#include "workspace/lsp/WorkspaceLspManager.h"
#include "workspace/shell/WorkspaceShellTestAccess.h"

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

void TestLspActionGatingRespectsToggles() {
  TemporaryDirectory temp;
  const std::filesystem::path project = temp.path() / "proj";
  const std::filesystem::path file = project / "main.py";
  WriteFile(file, "value = 1\n");

  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, project, false, false),
         "fixture project should open");
  WorkspaceShellTestAccess::OpenFile(shell, file);

  Expect(WorkspaceShellTestAccess::IsActionEnabled(shell, ActionId::FormatDocument),
         "Format Document is offered by default");
  Expect(WorkspaceShellTestAccess::IsActionEnabled(shell, ActionId::RenameSymbol),
         "Rename Symbol is offered by default");
  Expect(WorkspaceShellTestAccess::IsActionEnabled(shell, ActionId::SignatureHelp),
         "Signature Help is offered by default");

  Expect(WorkspaceShellTestAccess::SetSettingValue(shell, "lsp.formatting.enabled", "false"),
         "formatting toggle should write");
  Expect(!WorkspaceShellTestAccess::IsActionEnabled(shell, ActionId::FormatDocument),
         "disabling lsp.formatting.enabled hides Format Document");
  Expect(WorkspaceShellTestAccess::IsActionEnabled(shell, ActionId::RenameSymbol),
         "disabling formatting must not affect Rename Symbol");

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

void TestLspMenuItemsHideWhenDisabled() {
  TemporaryDirectory temp;
  const std::filesystem::path project = temp.path() / "proj";
  const std::filesystem::path file = project / "main.py";
  WriteFile(file, "value = 1\n");

  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, project, false, false),
         "fixture project should open");

  WorkspaceShellTestAccess::OpenFile(shell, file);
  {
    const auto labels =
        WorkspaceShellTestAccess::MenuItemLabels(shell, WorkspaceShell::MenuId::EditorContext);
    Expect(!LabelsContain(labels, "Go to Definition"),
           "with no server configured, Go to Definition is hidden, not greyed");
    Expect(!LabelsContain(labels, "Rename Symbol") && !LabelsContain(labels, "Format Document"),
           "with no server configured, Rename Symbol and Format Document are hidden");
    Expect(LabelsContain(labels, "Cut") || LabelsContain(labels, "Paste"),
           "non-LSP editor items stay present when LSP items are hidden");
  }

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
    Expect(LabelsContain(labels, "Rename Symbol") && LabelsContain(labels, "Format Document"),
           "a configured server surfaces Rename Symbol and Format Document (VS Code's context menu)");
  }

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

void TestMenuReadDoesNotStartLspServer() {
  TemporaryDirectory temp;
  const std::filesystem::path project = temp.path() / "proj";
  const std::filesystem::path file = project / "main.py";
  WriteFile(file, "value = 1\n");

  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, project, false, false),
         "fixture project should open");
  WorkspaceShellTestAccess::OpenFile(shell, file);

  workspace::LspManager& manager = WorkspaceShellTestAccess::LspManagerForTesting(shell);
  manager.RegisterServer({"python"}, platform::LocalProcessLauncher(), {"/bin/cat"}, "file://" + project.string(),
                         project.string(), false);
  Expect(manager.HasServer("python"), "server is registered");
  Expect(!manager.IsServerRunning("python"), "server is not started yet");

  const auto labels =
      WorkspaceShellTestAccess::MenuItemLabels(shell, WorkspaceShell::MenuId::EditorContext);
  Expect(LabelsContain(labels, "Go to Definition"),
         "a registered server surfaces the LSP menu entry");

  Expect(!manager.IsServerRunning("python"),
         "reading the menu must not start the language server");
  Expect(manager.LastServerError("python").empty(),
         "reading the menu must not even attempt to spawn the server");
}

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

  Expect(!WorkspaceShellTestAccess::LspManagerForTesting(shell).HasServer("python"),
         "retire should clear the project manager's servers");

  bool drained = false;
  for (int i = 0; i < 100000; ++i) {
    if (WorkspaceShellTestAccess::LspRetiringClientCount(shell) == 0) {
      drained = true;
      break;
    }
    WorkspaceShellTestAccess::ConsumeLspCallbacks(shell);
    std::this_thread::yield();
  }
  Expect(drained, "the host-owned retirement pool should drain the retiring client");
}

}

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

}
