#include "TestSupport.h"

#include "workspace/WorkspaceLspClient.h"
#include "workspace/WorkspaceShellTestAccess.h"

#include <algorithm>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
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

}  // namespace

void RegisterWorkspaceShellLspSettingsTests(std::vector<TestCase>& tests) {
  AddTest(tests, "WorkspaceShellLspSettings/ActionGatingRespectsToggles",
          TestLspActionGatingRespectsToggles);
  AddTest(tests, "WorkspaceShellLspSettings/MenuItemsHideWhenDisabled",
          TestLspMenuItemsHideWhenDisabled);
}

}  // namespace microide::tests
