#include "TestSupport.h"

#include "editor/FoldingModel.h"
#include "workspace/shell/WorkspaceShellTestAccess.h"

#include <filesystem>
#include <string>
#include <vector>

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

  auto* viewport = WorkspaceShellTestAccess::ActiveEditorOrNull(shell);
  Expect(viewport != nullptr, "the fixture must have an active viewport");
  viewport->JumpCursorTo(4, 0, false);
  Expect(WorkspaceShellTestAccess::ExecuteAction(shell, WorkspaceShell::ActionId::MoveLineUp, {}),
         "moving a line up should be handled");

  Expect(viewport->lines().LineCount() == 7,
         "the edit must preserve the line count, or the fingerprint would catch it "
         "and this proves nothing about MarkDirty");
  Expect(inner_closer() == 4, "after the move the inner fold closes one line later");

  Expect(WorkspaceShellTestAccess::ExecuteAction(shell, WorkspaceShell::ActionId::Undo, {}),
         "undo should be handled");
  Expect(viewport->lines().LineCount() == 7, "undo restores the same line count");
  Expect(inner_closer() == 3,
         "undo must rebuild the fold model: the inner fold closes at line 3 again");

  Expect(WorkspaceShellTestAccess::ExecuteAction(shell, WorkspaceShell::ActionId::Redo, {}),
         "redo should be handled");
  Expect(inner_closer() == 4, "redo must rebuild the fold model as well");
}

void TestUndoWithNothingToUndoLeavesTheBufferAlone() {
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

}

void RegisterEditorUndoRedoWiringTests(std::vector<TestCase>& tests) {
  AddTest(tests, "EditorUndoRedoWiring/UndoRebuildsFoldsAfterASameLineCountEdit",
          TestUndoRebuildsFoldsAfterASameLineCountEdit);
  AddTest(tests, "EditorUndoRedoWiring/UndoWithNothingToUndoLeavesTheBufferAlone",
          TestUndoWithNothingToUndoLeavesTheBufferAlone);
}

}
