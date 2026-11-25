#include "TestSupport.h"

#include "workspace/shell/WorkspaceShellTestAccess.h"

#include <filesystem>
#include <string>
#include <vector>

namespace microide::tests {
namespace {

using WorkspaceShellTestAccess = microide::workspace::WorkspaceShell::TestAccess;
using microide::workspace::WorkspaceShell;

struct CompareFixture {
  TemporaryDirectory temp_dir;
  WorkspaceShell shell;
  std::filesystem::path source;

  CompareFixture() {
    const std::filesystem::path root = temp_dir.path() / "project";
    std::filesystem::create_directories(root);
    source = root / "main.cpp";
    WriteFile(source, "on disk\n");
    WorkspaceShellTestAccess::SetProjectRoot(shell, root);
    WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 800);
    WorkspaceShellTestAccess::OpenSingleEditorTab(shell, source);
  }

  microide::editor::TextViewport& viewport() {
    auto* v = WorkspaceShellTestAccess::ActiveEditorOrNull(shell);
    Expect(v != nullptr, "the compare fixture must have an active viewport");
    return *v;
  }

  bool SelectForCompare(WorkspaceShell::ActionSource source) {
    return WorkspaceShellTestAccess::ExecuteAction(
        shell, WorkspaceShell::ActionId::SelectForCompare, {}, source);
  }
};

void TestCompareTakesTheLiveBufferNotTheFileOnDisk() {
  CompareFixture fixture;
  fixture.viewport().JumpCursorTo(0, 0, false);
  fixture.viewport().InsertText("unsaved ");
  Expect(fixture.viewport().dirty(), "the fixture must leave the buffer unsaved");

  fixture.SelectForCompare(WorkspaceShell::ActionSource::Menu);
  Expect(WorkspaceShellTestAccess::HasCompareSelection(fixture.shell),
         "selecting for compare should stash something");

  const auto& selection = WorkspaceShellTestAccess::CompareSelection(fixture.shell);
  Expect(selection.content.find("unsaved") != std::string::npos,
         "compare must take the LIVE buffer text, not the file on disk");
  Expect(!WorkspaceShellTestAccess::CompareSelectionCameFromFile(fixture.shell),
         "a buffer-sourced selection must not be flagged as coming from a file");
  Expect(selection.label == "main.cpp", "the label should be the file's name");
  Expect(selection.editable, "a buffer with a path is editable on the compare surface");
}

void TestCompareOfACleanBufferStillUsesTheBuffer() {
  CompareFixture fixture;
  fixture.SelectForCompare(WorkspaceShell::ActionSource::Menu);
  Expect(WorkspaceShellTestAccess::HasCompareSelection(fixture.shell),
         "a clean buffer should still be selectable for compare");

  const auto& selection = WorkspaceShellTestAccess::CompareSelection(fixture.shell);
  Expect(selection.content.find("on disk") != std::string::npos,
         "a clean buffer serializes the text it holds");
  Expect(!WorkspaceShellTestAccess::CompareSelectionCameFromFile(fixture.shell),
         "the buffer branch is still the one taken for a clean buffer");
  Expect(selection.path.filename() == "main.cpp", "the selection carries the buffer's path");
}

void TestContextMenuWithNoTreeTargetReportsNoFileSelected() {
  CompareFixture fixture;
  const bool handled = fixture.SelectForCompare(WorkspaceShell::ActionSource::ContextMenu);
  (void)handled;
  Expect(!WorkspaceShellTestAccess::HasCompareSelection(fixture.shell),
         "a context-menu compare with no tree target must not stash the active buffer");
}

void TestSelectingTwiceReplacesTheStash() {
  CompareFixture fixture;
  fixture.SelectForCompare(WorkspaceShell::ActionSource::Menu);
  Expect(WorkspaceShellTestAccess::CompareSelection(fixture.shell).content.find("unsaved") ==
             std::string::npos,
         "the first stash holds the on-disk text");

  fixture.viewport().JumpCursorTo(0, 0, false);
  fixture.viewport().InsertText("unsaved ");
  fixture.SelectForCompare(WorkspaceShell::ActionSource::Menu);
  Expect(WorkspaceShellTestAccess::CompareSelection(fixture.shell).content.find("unsaved") !=
             std::string::npos,
         "selecting again must replace the stash with the current buffer, not keep the old one");
}

}

void RegisterEditorCompareSourceTests(std::vector<TestCase>& tests) {
  AddTest(tests, "EditorCompareSource/TakesTheLiveBufferNotTheFileOnDisk",
          TestCompareTakesTheLiveBufferNotTheFileOnDisk);
  AddTest(tests, "EditorCompareSource/CleanBufferStillUsesTheBuffer",
          TestCompareOfACleanBufferStillUsesTheBuffer);
  AddTest(tests, "EditorCompareSource/ContextMenuWithNoTreeTargetReportsNoFileSelected",
          TestContextMenuWithNoTreeTargetReportsNoFileSelected);
  AddTest(tests, "EditorCompareSource/SelectingTwiceReplacesTheStash",
          TestSelectingTwiceReplacesTheStash);
}

}
