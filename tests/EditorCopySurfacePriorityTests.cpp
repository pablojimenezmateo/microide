#include "TestSupport.h"

#include "WorkspaceShellEventHelpers.h"
#include "workspace/SystemClipboard.h"
#include "workspace/shell/WorkspaceShellTestAccess.h"

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

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
  CopyFixture fixture;
  auto& viewport = fixture.viewport();
  viewport.JumpCursorTo(0, 0, false);
  viewport.MoveCursorTo(0, 5, true);
  viewport.AddSecondaryCaretWithRange(Range(1, 0, 5));
  viewport.AddSecondaryCaretWithRange(Range(2, 0, 7));
  Expect(viewport.has_multiple_carets(), "the fixture must actually have several carets");

  fixture.Copy();
  Expect(fixture.written->has_value(), "a multi-caret copy should reach the clipboard");
  Expect(**fixture.written == "alpha\nbravo\ncharlie",
         "each caret's selection should be copied, newline-joined, in document order");
}

void TestSingleSelectionCopiesJustThatText() {
  CopyFixture fixture;
  fixture.viewport().JumpCursorTo(1, 6, false);
  fixture.viewport().MoveCursorTo(1, 9, true);
  fixture.Copy();
  Expect(fixture.written->has_value(), "a selection copy should reach the clipboard");
  Expect(**fixture.written == "two", "a single selection copies exactly its text");
}

void TestBareCaretCopiesTheWholeLine() {
  CopyFixture fixture;
  fixture.viewport().JumpCursorTo(2, 4, false);
  fixture.Copy();
  Expect(fixture.written->has_value(), "a bare-caret copy should still reach the clipboard");
  Expect(**fixture.written == "charlie three\n" || **fixture.written == "charlie three",
         "a bare caret copies its whole line");
}

void TestSeveralBareCaretsCopyEveryLine() {
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
  CopyFixture fixture;
  fixture.viewport().JumpCursorTo(0, 0, false);
  fixture.viewport().MoveCursorTo(0, 5, true);

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

void TestSystemClipboardKeepsACopyTheSystemRefused() {
  std::optional<std::string> system = std::string("another app's text");
  bool refuse = true;
  workspace::SystemClipboard clipboard(workspace::SystemClipboard::Backend{
      .set =
          [&](std::string_view text) {
            if (!refuse) {
              system = std::string(text);
            }
            return true;
          },
      .get = [&]() { return system; },
  });

  Expect(clipboard.Write("copied here"), "a refused copy still lands on the app's clipboard");
  Expect(clipboard.Read() == std::optional<std::string>("copied here"),
         "the app's own paste gets the copy the system refused");
  Expect(clipboard.Read() == std::optional<std::string>("copied here"), "and keeps getting it");

  system = std::string("copied in another app");
  Expect(clipboard.Read() == std::optional<std::string>("copied in another app"),
         "once the system clipboard moves on, it answers again");

  refuse = false;
  Expect(clipboard.Write("accepted") && clipboard.Read() == std::optional<std::string>("accepted"),
         "a copy the system takes is the system's");
  system = std::string("later");
  Expect(clipboard.Read() == std::optional<std::string>("later"), "with nothing held over it");
}

}

void RegisterEditorCopySurfacePriorityTests(std::vector<TestCase>& tests) {
  AddTest(tests, "SystemClipboard/KeepsACopyTheSystemRefused",
          TestSystemClipboardKeepsACopyTheSystemRefused);
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

}
