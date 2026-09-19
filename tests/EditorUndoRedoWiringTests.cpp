#include "TestSupport.h"

#include "editor/FoldingModel.h"
#include "workspace/shell/WorkspaceShellTestAccess.h"

#include <filesystem>
#include <string>
#include <vector>

// The side effects ApplyUndoRedo performs BESIDES the undo itself.
//
// Undo/redo of the buffer is covered thoroughly at the TextViewport level; the
// 54-line dispatcher above it had no test at all. What this pins is the
// end-to-end behaviour: after an undo, the fold model reflects the RESTORED
// text. FoldingModelTests/StaleWithoutMarkDirtyAfterSameLineCountEdit shows why
// that is not free -- a same-line-count edit moves a fold boundary without
// changing anything the model's coarse freshness gate looks at, and stale fold
// ranges mean a phantom fold marker hiding an arbitrary line range.
//
// What this test does NOT isolate, and a probe proved it: deleting
// ApplyUndoRedo's `folding_model->MarkDirty()` leaves it passing. The rebuild
// here comes from the other mechanism -- `viewport.ConsumeFoldEditSpan()` in
// WorkspaceFoldingRefresh, which rescans the reported edit range. MarkDirty
// forces a FULL rebuild, so the two differ only for an edit whose effect on
// fold structure lands outside the span it reports; this fixture's edit is
// local, so both cover it. Whether MarkDirty is now redundant is an open
// question and NOT one to answer by deleting it speculatively -- getting it
// wrong reintroduces phantom folds.

namespace microide::tests {
namespace {

using WorkspaceShellTestAccess = microide::workspace::WorkspaceShell::TestAccess;
using microide::editor::FoldRange;
using microide::workspace::WorkspaceShell;

FoldRange FindByOpener(const std::vector<FoldRange>& ranges, std::size_t opener_line) {
  for (const auto& range : ranges) {
    if (range.opener_line == opener_line) {
      return range;
    }
  }
  return FoldRange{};
}

// Structure A. Moving `  b;` up one line swaps it with the inner closer, which
// moves the inner fold's closer from line 3 to line 4 WITHOUT changing the line
// count -- exactly the shape the fold model's fingerprint cannot see.
const char* kStructureA =
    "void f() {\n"
    "  if (x) {\n"
    "    a;\n"
    "  }\n"
    "  b;\n"
    "}\n";

void TestUndoRebuildsFoldsAfterASameLineCountEdit() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  std::filesystem::create_directories(root);
  const std::filesystem::path source = root / "main.cpp";
  WriteFile(source, kStructureA);

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 800);
  WorkspaceShellTestAccess::OpenSingleEditorTab(shell, source);

  const auto inner_closer = [&]() -> std::size_t {
    auto* model = WorkspaceShellTestAccess::EnsureActiveFoldingModelFresh(shell);
    Expect(model != nullptr, "the fixture must have a folding model");
    return FindByOpener(model->resolved_ranges(), 1).closer_line;
  };

  Expect(inner_closer() == 3,
         "structure A: the inner bracket fold opens at line 1 and closes at line 3");

  // Move `  b;` (line 4) up one, swapping it with the inner closer.
  auto* viewport = WorkspaceShellTestAccess::ActiveEditorOrNull(shell);
  Expect(viewport != nullptr, "the fixture must have an active viewport");
  viewport->JumpCursorTo(4, 0, false);
  Expect(WorkspaceShellTestAccess::ExecuteAction(shell, WorkspaceShell::ActionId::MoveLineUp, {}),
         "moving a line up should be handled");

  Expect(viewport->lines().LineCount() == 7,
         "the edit must preserve the line count, or the fingerprint would catch it "
         "and this proves nothing about MarkDirty");
  Expect(inner_closer() == 4, "after the move the inner fold closes one line later");

  // The assertion this test exists for: after the undo the model must report the
  // RESTORED structure, not the pre-undo one whose marker would point at lines
  // that have moved back.
  Expect(WorkspaceShellTestAccess::ExecuteAction(shell, WorkspaceShell::ActionId::Undo, {}),
         "undo should be handled");
  Expect(viewport->lines().LineCount() == 7, "undo restores the same line count");
  Expect(inner_closer() == 3,
         "undo must rebuild the fold model: the inner fold closes at line 3 again");

  // Redo takes the same path and must rebuild it too.
  Expect(WorkspaceShellTestAccess::ExecuteAction(shell, WorkspaceShell::ActionId::Redo, {}),
         "redo should be handled");
  Expect(inner_closer() == 4, "redo must rebuild the fold model as well");
}

void TestUndoWithNothingToUndoLeavesTheBufferAlone() {
  // ApplyUndoRedo early-returns when the viewport reports no change. Nothing
  // downstream of that -- the fold rebuild, the compare refresh, the merge
  // retrack -- may run on a no-op.
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  std::filesystem::create_directories(root);
  const std::filesystem::path source = root / "main.cpp";
  WriteFile(source, kStructureA);

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 800);
  WorkspaceShellTestAccess::OpenSingleEditorTab(shell, source);

  auto* viewport = WorkspaceShellTestAccess::ActiveEditorOrNull(shell);
  Expect(viewport != nullptr, "the fixture must have an active viewport");
  const bool dirty_before = viewport->dirty();

  for (int i = 0; i < 3; ++i) {
    WorkspaceShellTestAccess::ExecuteAction(shell, WorkspaceShell::ActionId::Undo, {});
    WorkspaceShellTestAccess::ExecuteAction(shell, WorkspaceShell::ActionId::Redo, {});
  }
  Expect(viewport->lines().LineCount() == 7, "a no-op undo/redo must not change the buffer");
  Expect(viewport->dirty() == dirty_before,
         "a no-op undo/redo must not make a clean buffer dirty");
}

}  // namespace

void RegisterEditorUndoRedoWiringTests(std::vector<TestCase>& tests) {
  AddTest(tests, "EditorUndoRedoWiring/UndoRebuildsFoldsAfterASameLineCountEdit",
          TestUndoRebuildsFoldsAfterASameLineCountEdit);
  AddTest(tests, "EditorUndoRedoWiring/UndoWithNothingToUndoLeavesTheBufferAlone",
          TestUndoWithNothingToUndoLeavesTheBufferAlone);
}

}  // namespace microide::tests
