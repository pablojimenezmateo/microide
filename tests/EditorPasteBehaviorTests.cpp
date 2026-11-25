#include "TestSupport.h"

#include "workspace/shell/WorkspaceShellTestAccess.h"

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace microide::tests {
namespace {

using WorkspaceShellTestAccess = microide::workspace::WorkspaceShell::TestAccess;
using microide::workspace::WorkspaceShell;

struct PasteFixture {
  TemporaryDirectory temp_dir;
  WorkspaceShell shell;
  std::filesystem::path source;
  std::shared_ptr<std::optional<std::string>> board =
      std::make_shared<std::optional<std::string>>();

  PasteFixture() {
    const std::filesystem::path root = temp_dir.path() / "project";
    std::filesystem::create_directories(root);
    source = root / "main.cpp";
    WriteFile(source,
              "alpha\n"
              "bravo\n"
              "charlie\n"
              "delta\n");
    WorkspaceShellTestAccess::SetProjectRoot(shell, root);
    WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 800);
    WorkspaceShellTestAccess::OpenSingleEditorTab(shell, source);
    auto sink = board;
    WorkspaceShellTestAccess::SetClipboardTextWriter(shell, [sink](std::string_view text) {
      *sink = std::string(text);
      return true;
    });
    WorkspaceShellTestAccess::SetClipboardTextReader(
        shell, [sink]() -> std::optional<std::string> { return *sink; });
  }

  microide::editor::TextViewport& viewport() {
    auto* v = WorkspaceShellTestAccess::ActiveEditorOrNull(shell);
    Expect(v != nullptr, "the paste fixture must have an active viewport");
    return *v;
  }

  void Copy() {
    WorkspaceShellTestAccess::ExecuteAction(shell, WorkspaceShell::ActionId::CopySelection, {});
  }
  void Paste() { WorkspaceShellTestAccess::ExecutePasteClipboard(shell); }

  std::string Line(std::size_t index) {
    return std::string(viewport().lines().LineView(index));
  }
  std::size_t LineCount() { return viewport().lines().LineCount(); }
};

void TestLineCopyPastesAboveTheCaretsLine() {
  PasteFixture fixture;
  fixture.viewport().JumpCursorTo(0, 2, false);
  fixture.Copy();
  Expect(fixture.board->has_value() && fixture.board->value().find("alpha") != std::string::npos,
         "the line copy should have put alpha on the clipboard");

  fixture.viewport().JumpCursorTo(2, 3, false);
  fixture.Paste();

  Expect(fixture.Line(2) == "alpha",
         "the copied line should be inserted above the caret's line, not into it");
  Expect(fixture.Line(3) == "charlie", "the caret's original line should follow it intact");
}

void TestForeignClipboardEndingInNewlinePastesInline() {
  PasteFixture fixture;
  *fixture.board = std::string("injected\n");

  fixture.viewport().JumpCursorTo(2, 3, false);
  fixture.Paste();

  Expect(fixture.Line(2) != "injected",
         "a foreign clipboard must not be treated as a line copy");
  Expect(fixture.Line(2).rfind("cha", 0) == 0,
         "the ordinary paste lands inside the caret's line, which still starts with cha");
}

void TestLineCopyWithASelectionPastesInline() {
  PasteFixture fixture;
  fixture.viewport().JumpCursorTo(0, 2, false);
  fixture.Copy();

  fixture.viewport().JumpCursorTo(2, 0, false);
  fixture.viewport().MoveCursorTo(2, 7, true);
  Expect(fixture.viewport().has_selection(), "the fixture must have a selection to replace");
  fixture.Paste();

  Expect(fixture.Line(2).find("charlie") == std::string::npos,
         "pasting over a selection replaces it rather than inserting a line above");
}

void TestMultiCaretPasteDistributesOneLinePerCaret() {
  PasteFixture fixture;
  auto& viewport = fixture.viewport();

  viewport.JumpCursorTo(0, 0, false);
  viewport.MoveCursorTo(0, 5, true);
  viewport.AddSecondaryCaretWithRange({{1, 0}, {1, 5}});
  viewport.AddSecondaryCaretWithRange({{2, 0}, {2, 7}});
  fixture.Copy();
  Expect(fixture.board->has_value() && **fixture.board == "alpha\nbravo\ncharlie",
         "the fixture must produce a three-line clipboard");

  viewport.ClearSecondaryCarets();
  viewport.JumpCursorTo(0, 5, false);
  viewport.AddSecondaryCaret(1, 5);
  viewport.AddSecondaryCaret(2, 7);
  fixture.Paste();

  Expect(fixture.Line(0) == "alphaalpha", "caret 0 should receive the clipboard's first line");
  Expect(fixture.Line(1) == "bravobravo", "caret 1 should receive the second, not the first");
  Expect(fixture.Line(2) == "charliecharlie", "caret 2 should receive the third");
}

void TestMultiCaretPasteOfAMismatchedLineCountGoesEverywhere() {
  PasteFixture fixture;
  auto& viewport = fixture.viewport();
  *fixture.board = std::string("XY");

  viewport.JumpCursorTo(0, 5, false);
  viewport.AddSecondaryCaret(1, 5);
  fixture.Paste();

  Expect(fixture.Line(0) == "alphaXY" && fixture.Line(1) == "bravoXY",
         "a clipboard that is not one line per caret goes in whole at every caret");
}

}

void RegisterEditorPasteBehaviorTests(std::vector<TestCase>& tests) {
  AddTest(tests, "EditorPasteBehavior/LineCopyPastesAboveTheCaretsLine",
          TestLineCopyPastesAboveTheCaretsLine);
  AddTest(tests, "EditorPasteBehavior/ForeignClipboardEndingInNewlinePastesInline",
          TestForeignClipboardEndingInNewlinePastesInline);
  AddTest(tests, "EditorPasteBehavior/LineCopyWithASelectionPastesInline",
          TestLineCopyWithASelectionPastesInline);
  AddTest(tests, "EditorPasteBehavior/MultiCaretPasteDistributesOneLinePerCaret",
          TestMultiCaretPasteDistributesOneLinePerCaret);
  AddTest(tests, "EditorPasteBehavior/MultiCaretPasteOfAMismatchedLineCountGoesEverywhere",
          TestMultiCaretPasteOfAMismatchedLineCountGoesEverywhere);
}

}
