#include "TestSupport.h"

#include "workspace/shell/WorkspaceShellTestAccess.h"

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// Paste: the line-clipboard rule and multi-caret distribution.
//
// PasteClipboard implements VSCode's emptySelectionClipboard behaviour -- a copy
// made with nothing selected puts the whole LINE on the clipboard, and pasting
// it at a bare caret inserts it above that caret's line rather than into the
// middle of it. The guard that decides this is a marker comparison against the
// text our own copy wrote, so a clipboard produced by another application never
// triggers it however it happens to be punctuated.
//
// None of that chain was tested, and neither was the paste-side counterpart:
// N carets with an N-line clipboard take line i at caret i.

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
  // VSCode: a copy with nothing selected takes the whole line, and pasting it at
  // a bare caret puts it ABOVE that line rather than splitting it.
  PasteFixture fixture;
  fixture.viewport().JumpCursorTo(0, 2, false);  // inside "alpha", no selection
  fixture.Copy();
  Expect(fixture.board->has_value() && fixture.board->value().find("alpha") != std::string::npos,
         "the line copy should have put alpha on the clipboard");

  fixture.viewport().JumpCursorTo(2, 3, false);  // mid-"charlie"
  fixture.Paste();

  Expect(fixture.Line(2) == "alpha",
         "the copied line should be inserted above the caret's line, not into it");
  Expect(fixture.Line(3) == "charlie", "the caret's original line should follow it intact");
}

void TestForeignClipboardEndingInNewlinePastesInline() {
  // The guard that makes the rule above safe: the line marker only matches text
  // our own copy wrote. A clipboard from another application that happens to end
  // in a newline must take the ORDINARY path and land at the caret.
  PasteFixture fixture;
  *fixture.board = std::string("injected\n");  // never produced by our copy

  fixture.viewport().JumpCursorTo(2, 3, false);  // mid-"charlie"
  fixture.Paste();

  Expect(fixture.Line(2) != "injected",
         "a foreign clipboard must not be treated as a line copy");
  Expect(fixture.Line(2).rfind("cha", 0) == 0,
         "the ordinary paste lands inside the caret's line, which still starts with cha");
}

void TestLineCopyWithASelectionPastesInline() {
  // Same clipboard, but the paste target HAS a selection, so the line rule does
  // not apply and the text replaces the selection.
  PasteFixture fixture;
  fixture.viewport().JumpCursorTo(0, 2, false);
  fixture.Copy();

  fixture.viewport().JumpCursorTo(2, 0, false);
  fixture.viewport().MoveCursorTo(2, 7, /*extend_selection=*/true);  // select "charlie"
  Expect(fixture.viewport().has_selection(), "the fixture must have a selection to replace");
  fixture.Paste();

  Expect(fixture.Line(2).find("charlie") == std::string::npos,
         "pasting over a selection replaces it rather than inserting a line above");
}

void TestMultiCaretPasteDistributesOneLinePerCaret() {
  // VSCode: N carets and a clipboard of exactly N lines inserts line i at caret
  // i. This is the paste-side mirror of the multi-caret copy.
  PasteFixture fixture;
  auto& viewport = fixture.viewport();

  // Build a three-line clipboard by copying three whole-line selections.
  viewport.JumpCursorTo(0, 0, false);
  viewport.MoveCursorTo(0, 5, /*extend_selection=*/true);
  viewport.AddSecondaryCaretWithRange({{1, 0}, {1, 5}});
  viewport.AddSecondaryCaretWithRange({{2, 0}, {2, 7}});
  fixture.Copy();
  Expect(fixture.board->has_value() && **fixture.board == "alpha\nbravo\ncharlie",
         "the fixture must produce a three-line clipboard");

  // Three bare carets at the ends of three different lines.
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
  // The other half of the rule: when the line count does NOT match the caret
  // count, the whole text goes in at every caret.
  PasteFixture fixture;
  auto& viewport = fixture.viewport();
  *fixture.board = std::string("XY");

  viewport.JumpCursorTo(0, 5, false);
  viewport.AddSecondaryCaret(1, 5);
  fixture.Paste();

  Expect(fixture.Line(0) == "alphaXY" && fixture.Line(1) == "bravoXY",
         "a clipboard that is not one line per caret goes in whole at every caret");
}

}  // namespace

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

}  // namespace microide::tests
