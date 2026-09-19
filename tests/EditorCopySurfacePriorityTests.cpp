#include "TestSupport.h"

#include "WorkspaceShellEventHelpers.h"
#include "workspace/shell/WorkspaceShellTestAccess.h"

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// Which surface owns Ctrl+C.
//
// CopySelectionText is a 42-line priority chain -- a focused single-line field
// beats a terminal selection beats the editor, and inside the editor a
// multi-caret selection beats the primary selection beats the caret's line --
// and none of it was tested. The primitives underneath it are
// (EditorEdgeCaseTests covers MultiCaretSelectedText and
// MultiCaretLineTextForClipboard); the CHAIN was not, which is the half that
// decides whether Ctrl+C in the find bar copies the query or the transcript.
//
// These drive ActionId::CopySelection and capture what reaches the clipboard,
// so the wiring is covered as well as the choice.

namespace microide::tests {
namespace {

using WorkspaceShellTestAccess = microide::workspace::WorkspaceShell::TestAccess;
using microide::editor::SelectionRange;
using microide::workspace::WorkspaceShell;

struct CopyFixture {
  TemporaryDirectory temp_dir;
  WorkspaceShell shell;
  std::filesystem::path source;
  std::shared_ptr<std::optional<std::string>> written =
      std::make_shared<std::optional<std::string>>();

  CopyFixture() {
    const std::filesystem::path root = temp_dir.path() / "project";
    std::filesystem::create_directories(root);
    source = root / "main.cpp";
    WriteFile(source,
              "alpha one\n"
              "bravo two\n"
              "charlie three\n"
              "delta four\n");
    WorkspaceShellTestAccess::SetProjectRoot(shell, root);
    WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 800);
    WorkspaceShellTestAccess::OpenSingleEditorTab(shell, source);
    auto sink = written;
    WorkspaceShellTestAccess::SetClipboardTextWriter(shell, [sink](std::string_view text) {
      *sink = std::string(text);
      return true;
    });
  }

  microide::editor::TextViewport& viewport() {
    auto* v = WorkspaceShellTestAccess::ActiveEditorOrNull(shell);
    Expect(v != nullptr, "the copy fixture must have an active viewport");
    return *v;
  }

  void Copy() {
    written->reset();
    WorkspaceShellTestAccess::ExecuteAction(shell, WorkspaceShell::ActionId::CopySelection, {});
  }
};

SelectionRange Range(std::size_t line, std::size_t from, std::size_t to) {
  return SelectionRange{{line, from}, {line, to}};
}

void TestMultiCaretSelectionsCopyJoinedByNewline() {
  // VSCode's multi-caret copy: every caret's selection, newline-joined, in
  // document order -- so a Ctrl+D multi-select copies every occurrence rather
  // than just the primary's.
  CopyFixture fixture;
  auto& viewport = fixture.viewport();
  viewport.JumpCursorTo(0, 0, false);
  viewport.MoveCursorTo(0, 5, /*extend_selection=*/true);  // "alpha"
  viewport.AddSecondaryCaretWithRange(Range(1, 0, 5));   // "bravo"
  viewport.AddSecondaryCaretWithRange(Range(2, 0, 7));   // "charlie"
  Expect(viewport.has_multiple_carets(), "the fixture must actually have several carets");

  fixture.Copy();
  Expect(fixture.written->has_value(), "a multi-caret copy should reach the clipboard");
  Expect(**fixture.written == "alpha\nbravo\ncharlie",
         "each caret's selection should be copied, newline-joined, in document order");
}

void TestSingleSelectionCopiesJustThatText() {
  CopyFixture fixture;
  fixture.viewport().JumpCursorTo(1, 6, false);
  fixture.viewport().MoveCursorTo(1, 9, /*extend_selection=*/true);  // "two"
  fixture.Copy();
  Expect(fixture.written->has_value(), "a selection copy should reach the clipboard");
  Expect(**fixture.written == "two", "a single selection copies exactly its text");
}

void TestBareCaretCopiesTheWholeLine() {
  // No selection: VSCode copies the caret's whole line, and the copy is marked
  // as a line copy so a later paste inserts it as a line rather than inline.
  CopyFixture fixture;
  fixture.viewport().JumpCursorTo(2, 4, false);
  fixture.Copy();
  Expect(fixture.written->has_value(), "a bare-caret copy should still reach the clipboard");
  Expect(**fixture.written == "charlie three\n" || **fixture.written == "charlie three",
         "a bare caret copies its whole line");
}

void TestSeveralBareCaretsCopyEveryLine() {
  // The branch the comment in CopySelectionText calls out: the multi-caret
  // AGGREGATE only fires when every caret has a non-empty selection, so without
  // a separate bare-caret path a multi-caret line copy silently took one line --
  // and the cut sharing this fallback deleted all of them.
  CopyFixture fixture;
  auto& viewport = fixture.viewport();
  viewport.JumpCursorTo(0, 2, false);
  viewport.AddSecondaryCaret(2, 3);
  Expect(viewport.has_multiple_carets(), "the fixture must have several bare carets");
  Expect(!viewport.has_selection(), "and none of them may have a selection");

  fixture.Copy();
  Expect(fixture.written->has_value(), "a multi-caret line copy should reach the clipboard");
  const std::string& text = **fixture.written;
  Expect(text.find("alpha one") != std::string::npos,
         "the first caret's line should be copied");
  Expect(text.find("charlie three") != std::string::npos,
         "the second caret's line should be copied too, not just the primary's");
  Expect(text.find("bravo two") == std::string::npos,
         "a line no caret sits on must not be copied");
}

void TestFocusedSingleLineFieldOwnsTheKeystroke() {
  // The documented rule: a focused single-line field owns Ctrl+C even when the
  // editor below it has a selection. Its selection is what gets copied.
  CopyFixture fixture;
  fixture.viewport().JumpCursorTo(0, 0, false);
  fixture.viewport().MoveCursorTo(0, 5, /*extend_selection=*/true);  // editor selects "alpha"

  // Ctrl+F, not ActionId::Find: the widget is non-modal, and it is the key path
  // that opens it AND puts focus in the query field. Dispatching the action
  // alone leaves focus on the editor, which is the whole point of non-modal.
  Expect(SendKeyDown(fixture.shell, SDLK_F, SDL_KMOD_CTRL), "Ctrl+F should open the find widget");
  WorkspaceShellTestAccess::SetBufferSearchQueryAndRefresh(fixture.shell, "needle");
  Expect(WorkspaceShellTestAccess::BufferSearchSurfaceFocused(fixture.shell),
         "opening buffer search must focus its query field, or this test proves nothing");
  WorkspaceShellTestAccess::SelectAllBufferSearchQuery(fixture.shell);

  fixture.Copy();
  Expect(fixture.written->has_value(), "the focused field's selection should be copied");
  Expect(**fixture.written == "needle",
         "a focused single-line field owns Ctrl+C: the query, not the editor selection");
}

}  // namespace

void RegisterEditorCopySurfacePriorityTests(std::vector<TestCase>& tests) {
  AddTest(tests, "EditorCopySurfacePriority/MultiCaretSelectionsJoinedByNewline",
          TestMultiCaretSelectionsCopyJoinedByNewline);
  AddTest(tests, "EditorCopySurfacePriority/SingleSelectionCopiesJustThatText",
          TestSingleSelectionCopiesJustThatText);
  AddTest(tests, "EditorCopySurfacePriority/BareCaretCopiesTheWholeLine",
          TestBareCaretCopiesTheWholeLine);
  AddTest(tests, "EditorCopySurfacePriority/SeveralBareCaretsCopyEveryLine",
          TestSeveralBareCaretsCopyEveryLine);
  AddTest(tests, "EditorCopySurfacePriority/FocusedSingleLineFieldOwnsTheKeystroke",
          TestFocusedSingleLineFieldOwnsTheKeystroke);
}

}  // namespace microide::tests
