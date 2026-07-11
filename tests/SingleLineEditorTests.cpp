#include "TestSupport.h"

#include <optional>
#include <string>

#include <SDL3/SDL.h>

#include "editor/SingleLineEditor.h"
#include "editor/SingleLineKeyHandler.h"

namespace microide::tests {
namespace {

void ExpectEditorState(const editor::SingleLineEditor& editor,
                       std::string_view text,
                       std::size_t caret,
                       std::optional<editor::SingleLineSelection> selection,
                       std::string_view context) {
  Expect(editor.text() == text, std::string(context) + ": unexpected text");
  Expect(editor.caret() == caret, std::string(context) + ": unexpected caret");
  Expect(editor.Selection().has_value() == selection.has_value(),
         std::string(context) + ": unexpected selection presence");
  if (selection.has_value()) {
    Expect(editor.Selection()->start == selection->start && editor.Selection()->end == selection->end,
           std::string(context) + ": unexpected selection range");
  }
}

void TestSingleLineEditorSupportsInsertBackspaceAndDeleteForward() {
  editor::SingleLineEditor editor("ac");
  editor.MoveLeft();
  Expect(editor.Insert("b"), "insert should report a mutation");
  ExpectEditorState(editor, "abc", 2, std::nullopt, "insert");
  Expect(editor.Backspace(), "backspace should delete the previous codepoint");
  ExpectEditorState(editor, "ac", 1, std::nullopt, "backspace");
  Expect(editor.DeleteForward(), "delete-forward should delete the next codepoint");
  ExpectEditorState(editor, "a", 1, std::nullopt, "delete-forward");
}

void TestSingleLineEditorSupportsMovementAndSelectionInvariants() {
  editor::SingleLineEditor editor("alpha");
  Expect(editor.MoveLeft(), "move left should move the caret");
  Expect(editor.MoveLeft(true), "shift-left should extend selection");
  ExpectEditorState(editor, "alpha", 3, editor::SingleLineSelection{3, 4}, "shift-left");
  Expect(editor.MoveHome(true), "shift-home should extend selection to the start");
  ExpectEditorState(editor, "alpha", 0, editor::SingleLineSelection{0, 4}, "shift-home");
  Expect(editor.MoveRight(), "move right should collapse the selection to its end");
  ExpectEditorState(editor, "alpha", 4, std::nullopt, "collapse selection");
  Expect(editor.MoveEnd(), "move end should move to the end");
  ExpectEditorState(editor, "alpha", 5, std::nullopt, "move end");
}

void TestSingleLineEditorSupportsSelectAllCopyCutPaste() {
  editor::SingleLineEditor editor("hello");
  Expect(editor.SelectAll(), "select-all should select non-empty text");
  Expect(editor.CopySelection() == "hello", "copy should return the selected text");
  const auto cut = editor.CutSelection();
  Expect(cut.has_value() && *cut == "hello", "cut should return the selected text");
  ExpectEditorState(editor, "", 0, std::nullopt, "cut");
  Expect(editor.Paste("world"), "paste should insert clipboard text");
  ExpectEditorState(editor, "world", 5, std::nullopt, "paste");
}

void TestSingleLineEditorStripsLineBreaksOnInsert() {
  // A single-line surface must never hold CR/LF. The common trigger is pasting a
  // whole-line copy, which carries a trailing '\n'; multi-line clipboard content
  // carries interior breaks too. Both Ctrl+V (Paste) and a paste delivered as a
  // text-input event funnel through Insert, so the stripping lives there.
  editor::SingleLineEditor editor;
  Expect(editor.Paste("hello\n"), "pasting whole-line-copy text should insert");
  ExpectEditorState(editor, "hello", 5, std::nullopt, "trailing-newline paste");

  editor.SetText("");
  Expect(editor.Insert("a\r\nb\nc\rd"), "inserting multi-line text should insert");
  ExpectEditorState(editor, "abcd", 4, std::nullopt, "multi-line insert collapses breaks");

  // Pasting only line breaks inserts nothing (sanitized to empty).
  editor.SetText("keep");
  editor.MoveEnd(false);
  Expect(!editor.Paste("\r\n\n"), "pasting only line breaks inserts nothing");
  ExpectEditorState(editor, "keep", 4, std::nullopt, "newline-only paste is a no-op");
}

void TestSingleLineKeyHandlerDispatchesClipboardShortcuts() {
  editor::SingleLineEditor editor("hello");
  std::string clipboard;
  const editor::SingleLineKeyHandler::Clipboard io{
      .write_text = [&](const std::string& text) { clipboard = text; },
      .read_text = [&]() -> std::optional<std::string> { return clipboard; },
  };

  Expect(editor::SingleLineKeyHandler::HandleKeyDown(editor, SDLK_A, SDL_KMOD_CTRL, io),
         "ctrl+a should select all");
  Expect(editor::SingleLineKeyHandler::HandleKeyDown(editor, SDLK_C, SDL_KMOD_CTRL, io),
         "ctrl+c should copy the selection");
  Expect(clipboard == "hello", "ctrl+c should populate the clipboard");
  Expect(editor::SingleLineKeyHandler::HandleKeyDown(editor, SDLK_X, SDL_KMOD_CTRL, io),
         "ctrl+x should cut the selection");
  ExpectEditorState(editor, "", 0, std::nullopt, "ctrl+x");
  clipboard = "restored";
  Expect(editor::SingleLineKeyHandler::HandleKeyDown(editor, SDLK_V, SDL_KMOD_CTRL, io),
         "ctrl+v should paste clipboard text");
  ExpectEditorState(editor, "restored", 8, std::nullopt, "ctrl+v");
}

void TestSingleLineEditorSelectsWordAtOffset() {
  editor::SingleLineEditor editor("hello_world example 123foo");

  Expect(editor.SelectWordAt(2), "click inside first word should select it");
  ExpectEditorState(editor, "hello_world example 123foo", 11,
                    editor::SingleLineSelection{0, 11}, "word at offset 2");

  Expect(editor.SelectWordAt(11), "caret straddling end of word should still select it");
  ExpectEditorState(editor, "hello_world example 123foo", 11,
                    editor::SingleLineSelection{0, 11}, "word at boundary 11");

  editor.SetSelectionAnchor(std::nullopt);
  editor.SetCaret(12);
  Expect(editor.SelectWordAt(12), "click on whitespace adjacent to next word selects nothing");
  // Position 12 = 'e' of "example", whitespace at 11, but 12 indexes a word char.
  ExpectEditorState(editor, "hello_world example 123foo", 19,
                    editor::SingleLineSelection{12, 19}, "word starting at 12");

  editor.SetSelectionAnchor(std::nullopt);
  editor.SetCaret(11);
  Expect(!editor.SelectWordAt(11) == false,
         "boundary case at 11 selects the preceding word, returns true");

  // Click in pure whitespace returns false and does not mutate state.
  editor::SingleLineEditor blanks("   ");
  blanks.SetCaret(2);
  Expect(!blanks.SelectWordAt(1), "selecting in whitespace should report no word");
  ExpectEditorState(blanks, "   ", 2, std::nullopt, "no word at whitespace");
}

void TestSingleLineEditorSupportsSnapshotAndAppend() {
  editor::SingleLineEditor editor("alpha");
  editor.MoveLeft();
  editor.MoveLeft(true);
  const editor::SingleLineSnapshot snapshot = editor.Snapshot();
  Expect(snapshot.text == "alpha", "snapshot should preserve text");
  Expect(snapshot.caret == 3, "snapshot should preserve caret");
  Expect(snapshot.selection_anchor.has_value() && *snapshot.selection_anchor == 4,
         "snapshot should preserve selection anchor");

  editor.Append("-omega");
  ExpectEditorState(editor, "alpha-omega", 11, std::nullopt,
                    "append should extend text and collapse selection");
}

void TestSingleLineEditorSupportsWordMotionAndDeletion() {
  editor::SingleLineEditor editor("foo bar baz");

  // Word-left from end stops at each word edge.
  Expect(editor.MoveWordLeft(), "word-left should move to the start of the last word");
  ExpectEditorState(editor, "foo bar baz", 8, std::nullopt, "word-left once");
  Expect(editor.MoveWordLeft(), "word-left should move to the start of the previous word");
  ExpectEditorState(editor, "foo bar baz", 4, std::nullopt, "word-left twice");

  // Word-right advances past each word.
  Expect(editor.MoveWordRight(), "word-right should move past the current word");
  ExpectEditorState(editor, "foo bar baz", 7, std::nullopt, "word-right once");

  // Ctrl+Shift+Left extends the selection by a word.
  editor.SetCaret(11);
  Expect(editor.MoveWordLeft(/*extend_selection=*/true),
         "word-left with extend should grow a selection");
  Expect(editor.Selection().has_value() && editor.Selection()->start == 8 &&
             editor.Selection()->end == 11,
         "extended word-left should select the trailing word");

  // Word deletion removes whole words.
  editor::SingleLineEditor deleter("foo bar baz");
  Expect(deleter.DeleteWordLeft(), "delete-word-left should remove the trailing word");
  ExpectEditorState(deleter, "foo bar ", 8, std::nullopt, "delete-word-left");
  deleter.SetCaret(0);
  Expect(deleter.DeleteWordRight(), "delete-word-right should remove the leading word");
  ExpectEditorState(deleter, " bar ", 0, std::nullopt, "delete-word-right");
}

void TestSingleLineKeyHandlerBindsWordShortcuts() {
  editor::SingleLineEditor editor("alpha beta");
  const editor::SingleLineKeyHandler::Clipboard io{};

  // Ctrl+Left is word-granular (not line-home).
  Expect(editor::SingleLineKeyHandler::HandleKeyDown(editor, SDLK_LEFT, SDL_KMOD_CTRL, io),
         "ctrl+left should be handled");
  Expect(editor.caret() == 6, "ctrl+left should jump to the start of the last word");

  // Ctrl+Backspace deletes the previous word.
  Expect(editor::SingleLineKeyHandler::HandleKeyDown(editor, SDLK_BACKSPACE, SDL_KMOD_CTRL, io),
         "ctrl+backspace should be handled");
  Expect(editor.text() == "beta", "ctrl+backspace should delete the previous word");
}

}  // namespace

void RegisterSingleLineEditorTests(std::vector<TestCase>& tests) {
  AddTest(tests, "SingleLineEditor/SupportsInsertBackspaceAndDeleteForward",
          TestSingleLineEditorSupportsInsertBackspaceAndDeleteForward);
  AddTest(tests, "SingleLineEditor/SupportsMovementAndSelectionInvariants",
          TestSingleLineEditorSupportsMovementAndSelectionInvariants);
  AddTest(tests, "SingleLineEditor/SupportsSelectAllCopyCutPaste",
          TestSingleLineEditorSupportsSelectAllCopyCutPaste);
  AddTest(tests, "SingleLineEditor/StripsLineBreaksOnInsert",
          TestSingleLineEditorStripsLineBreaksOnInsert);
  AddTest(tests, "SingleLineEditor/KeyHandlerDispatchesClipboardShortcuts",
          TestSingleLineKeyHandlerDispatchesClipboardShortcuts);
  AddTest(tests, "SingleLineEditor/SupportsSnapshotAndAppend",
          TestSingleLineEditorSupportsSnapshotAndAppend);
  AddTest(tests, "SingleLineEditor/SelectsWordAtOffset",
          TestSingleLineEditorSelectsWordAtOffset);
  AddTest(tests, "SingleLineEditor/SupportsWordMotionAndDeletion",
          TestSingleLineEditorSupportsWordMotionAndDeletion);
  AddTest(tests, "SingleLineEditor/KeyHandlerBindsWordShortcuts",
          TestSingleLineKeyHandlerBindsWordShortcuts);
}

}  // namespace microide::tests
