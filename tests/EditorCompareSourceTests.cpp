#include "TestSupport.h"

#include "workspace/shell/WorkspaceShellTestAccess.h"

#include <filesystem>
#include <string>
#include <vector>

// What "compare this" actually compares.
//
// ResolveCurrentCompareInput is a 51-line priority chain with no test: a
// context-menu invocation acts on the file targeted in the tree, otherwise the
// ACTIVE EDITOR BUFFER wins -- deliberately, so a comparison shows the live,
// possibly-unsaved text the user is looking at rather than what is on disk --
// and only failing both does it fall back to a file.
//
// The dirty-buffer case is the one that matters and the one that would regress
// silently: reading the file instead would still produce a plausible diff, just
// of the wrong content.

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
  // Type into the buffer without saving. The file still says "on disk".
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
  // Even clean, the buffer is the source -- the path matters for the label and
  // the editable flag, and the content must round-trip unchanged.
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
  // The context-menu branch acts on the tree target and does NOT fall through to
  // the active buffer -- right-clicking nothing must say so rather than quietly
  // comparing whatever happens to be open.
  CompareFixture fixture;
  const bool handled = fixture.SelectForCompare(WorkspaceShell::ActionSource::ContextMenu);
  (void)handled;
  Expect(!WorkspaceShellTestAccess::HasCompareSelection(fixture.shell),
         "a context-menu compare with no tree target must not stash the active buffer");
}

void TestSelectingTwiceReplacesTheStash() {
  // The stash is a single slot: a second select-for-compare replaces it, so a
  // stale first pick cannot leak into a later comparison.
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

}  // namespace

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

}  // namespace microide::tests
