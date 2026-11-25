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

void TestSingleLineEditorExtendSelectionToAnchorsAtTheCaret() {
  editor::SingleLineEditor editor("drag me");
  editor.SetCaret(1);
  editor.ExtendSelectionTo(4);
  ExpectEditorState(editor, "drag me", 4, editor::SingleLineSelection{1, 4}, "first extend anchors at 1");
  editor.ExtendSelectionTo(7);
  ExpectEditorState(editor, "drag me", 7, editor::SingleLineSelection{1, 7}, "later extends keep the anchor");
  editor.ExtendSelectionTo(0);
  ExpectEditorState(editor, "drag me", 0, editor::SingleLineSelection{0, 1}, "an extend past the anchor flips");
  editor.ExtendSelectionTo(1);
  ExpectEditorState(editor, "drag me", 1, std::nullopt, "back on the anchor is no selection");
  editor.SetCaret(0);
  editor.ExtendSelectionTo(3);
  Expect(editor.Insert("Y") && editor.text() == "Yg me", "typing replaces a dragged selection (" + editor.text() + ")");
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
  editor::SingleLineEditor editor;
  Expect(editor.Paste("hello\n"), "pasting whole-line-copy text should insert");
  ExpectEditorState(editor, "hello", 5, std::nullopt, "trailing-newline paste");

  editor.SetText("");
  Expect(editor.Insert("a\r\nb\nc\rd"), "inserting multi-line text should insert");
  ExpectEditorState(editor, "a b c d", 7, std::nullopt, "multi-line insert collapses breaks to spaces");

  editor.SetText("keep");
  editor.MoveEnd(false);
  Expect(!editor.Paste("\r\n\n"), "pasting only line breaks inserts nothing");
  ExpectEditorState(editor, "keep", 4, std::nullopt, "newline-only paste is a no-op");
}

void TestSingleLineEditorStripsLineBreaksOnSetText() {
  editor::SingleLineEditor editor;

  editor.SetText("line one\nline two");
  ExpectEditorState(editor, "line one line two", 17, std::nullopt, "SetText collapses a newline");

  editor.SetText("a\r\nb\nc\rd");
  ExpectEditorState(editor, "a b c d", 7, std::nullopt, "SetText collapses every break run");

  editor.SetText("plain");
  ExpectEditorState(editor, "plain", 5, std::nullopt, "SetText leaves break-free text alone");

  editor.SetText("\n\r\n");
  ExpectEditorState(editor, "", 0, std::nullopt, "SetText of only breaks yields an empty field");
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
  ExpectEditorState(editor, "hello_world example 123foo", 19,
                    editor::SingleLineSelection{12, 19}, "word starting at 12");

  editor.SetSelectionAnchor(std::nullopt);
  editor.SetCaret(11);
  Expect(!editor.SelectWordAt(11) == false,
         "boundary case at 11 selects the preceding word, returns true");

  editor::SingleLineEditor blanks("   ");
  blanks.SetCaret(2);
  Expect(blanks.SelectWordAt(1), "a whitespace run is selectable");
  ExpectEditorState(blanks, "   ", 3, editor::SingleLineSelection{0, 3},
                    "the whole whitespace run");

  editor::SingleLineEditor operators("a->b");
  Expect(operators.SelectWordAt(2), "an operator run is selectable");
  ExpectEditorState(operators, "a->b", 3, editor::SingleLineSelection{1, 3},
                    "the whole -> run, not one character");

  editor::SingleLineEditor empty("");
  Expect(!empty.SelectWordAt(0), "an empty field has nothing to select");
  ExpectEditorState(empty, "", 0, std::nullopt, "empty field untouched");
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

  editor::SingleLineEditor sanitized("x");
  sanitized.Append("a\r\nb\nc");
  ExpectEditorState(sanitized, "xa b c", 6, std::nullopt, "append collapses CR/LF to spaces");
}

void TestSingleLineEditorCaretSnapsToUtf8Boundary() {
  editor::SingleLineEditor editor("\xc3\xa9xy");
  editor.SetCaret(1);
  Expect(editor.caret() == 0, "a caret inside a multibyte scalar snaps to its start");

  editor.SetCaret(2);
  Expect(editor.caret() == 2, "a valid UTF-8 boundary caret is preserved");

  editor.SetCaret(2);
  editor.SetSelectionAnchor(1);
  const auto selection = editor.Selection();
  Expect(selection.has_value() && selection->start == 0 && selection->end == 2,
         "a selection anchor inside a multibyte scalar snaps to a boundary");
}

void TestSingleLineEditorSupportsWordMotionAndDeletion() {
  editor::SingleLineEditor editor("foo bar baz");

  Expect(editor.MoveWordLeft(), "word-left should move to the start of the last word");
  ExpectEditorState(editor, "foo bar baz", 8, std::nullopt, "word-left once");
  Expect(editor.MoveWordLeft(), "word-left should move to the start of the previous word");
  ExpectEditorState(editor, "foo bar baz", 4, std::nullopt, "word-left twice");

  Expect(editor.MoveWordRight(), "word-right should move past the current word");
  ExpectEditorState(editor, "foo bar baz", 7, std::nullopt, "word-right once");

  editor.SetCaret(11);
  Expect(editor.MoveWordLeft(true),
         "word-left with extend should grow a selection");
  Expect(editor.Selection().has_value() && editor.Selection()->start == 8 &&
             editor.Selection()->end == 11,
         "extended word-left should select the trailing word");

  editor::SingleLineEditor deleter("foo bar baz");
  Expect(deleter.DeleteWordLeft(), "delete-word-left should remove the trailing word");
  ExpectEditorState(deleter, "foo bar ", 8, std::nullopt, "delete-word-left");
  deleter.SetCaret(0);
  Expect(deleter.DeleteWordRight(), "delete-word-right should remove the leading word");
  ExpectEditorState(deleter, " bar ", 0, std::nullopt, "delete-word-right");
}

void TestSingleLineEditorWordMotionHandlesUnicode() {
  editor::SingleLineEditor editor("café ñoño");
  Expect(editor.MoveWordLeft(), "word-left enters the second accented word");
  ExpectEditorState(editor, "café ñoño", 6, std::nullopt, "word-left to start of ñoño");
  Expect(editor.MoveWordLeft(), "word-left crosses the space to the first word");
  ExpectEditorState(editor, "café ñoño", 0, std::nullopt, "word-left to start of café");
  Expect(editor.MoveWordRight(), "word-right moves past the accented first word");
  ExpectEditorState(editor, "café ñoño", 5, std::nullopt, "word-right past café");
}

void TestSingleLineKeyHandlerBindsWordShortcuts() {
  editor::SingleLineEditor editor("alpha beta");
  const editor::SingleLineKeyHandler::Clipboard io{};

  Expect(editor::SingleLineKeyHandler::HandleKeyDown(editor, SDLK_LEFT, SDL_KMOD_CTRL, io),
         "ctrl+left should be handled");
  Expect(editor.caret() == 6, "ctrl+left should jump to the start of the last word");

  Expect(editor::SingleLineKeyHandler::HandleKeyDown(editor, SDLK_BACKSPACE, SDL_KMOD_CTRL, io),
         "ctrl+backspace should be handled");
  Expect(editor.text() == "beta", "ctrl+backspace should delete the previous word");
}

void TestSingleLineEditorWordStepOverSelectionMovesFromTheCaret() {
  editor::SingleLineEditor editor("alpha beta gamma");
  editor.SetCaret(6);
  editor.SetSelectionAnchor(10);
  Expect(editor.MoveWordLeft(), "word-left over a selection moves");
  Expect(!editor.HasSelection() && editor.caret() == 0,
         "word-left from the selection's caret end reaches the previous word start");

  editor.SetCaret(10);
  editor.SetSelectionAnchor(6);
  Expect(editor.MoveWordRight(), "word-right over a selection moves");
  Expect(!editor.HasSelection() && editor.caret() == 16,
         "word-right from the selection's caret end reaches the next word end");

  editor.SetCaret(6);
  editor.SetSelectionAnchor(10);
  Expect(editor.MoveWordRight(), "word-right with the caret at the selection start");
  Expect(!editor.HasSelection() && editor.caret() == 10,
         "it steps from the caret (6), not from the selection's end");
}

}

void TestSingleLineEditorSelectsNonAsciiWord() {
  const std::string text = "gr\xc3\xb6\xc3\x9f" "e naive";
  editor::SingleLineEditor editor(text);
  const std::size_t word_end = std::string("gr\xc3\xb6\xc3\x9f" "e").size();
  Expect(editor.SelectWordAt(3), "a click on a continuation byte selects the word");
  ExpectEditorState(editor, text, word_end, editor::SingleLineSelection{0, word_end},
                    "whole non-ASCII word");
  editor.SetSelectionAnchor(std::nullopt);
  Expect(editor.SelectWordAt(word_end), "a click at the word's end selects it");
  ExpectEditorState(editor, text, word_end, editor::SingleLineSelection{0, word_end},
                    "whole non-ASCII word from its end");
}

void RegisterSingleLineEditorTests(std::vector<TestCase>& tests) {
  AddTest(tests, "SingleLineEditor/SelectsNonAsciiWord", TestSingleLineEditorSelectsNonAsciiWord);
  AddTest(tests, "SingleLineEditor/SupportsInsertBackspaceAndDeleteForward",
          TestSingleLineEditorSupportsInsertBackspaceAndDeleteForward);
  AddTest(tests, "SingleLineEditor/WordStepOverSelectionMovesFromTheCaret",
          TestSingleLineEditorWordStepOverSelectionMovesFromTheCaret);
  AddTest(tests, "SingleLineEditor/SupportsMovementAndSelectionInvariants",
          TestSingleLineEditorSupportsMovementAndSelectionInvariants);
  AddTest(tests, "SingleLineEditor/SupportsSelectAllCopyCutPaste",
          TestSingleLineEditorSupportsSelectAllCopyCutPaste);
  AddTest(tests, "SingleLineEditor/StripsLineBreaksOnInsert",
          TestSingleLineEditorStripsLineBreaksOnInsert);
  AddTest(tests, "SingleLineEditor/StripsLineBreaksOnSetText",
          TestSingleLineEditorStripsLineBreaksOnSetText);
  AddTest(tests, "SingleLineEditor/KeyHandlerDispatchesClipboardShortcuts",
          TestSingleLineKeyHandlerDispatchesClipboardShortcuts);
  AddTest(tests, "SingleLineEditor/SupportsSnapshotAndAppend",
          TestSingleLineEditorSupportsSnapshotAndAppend);
  AddTest(tests, "SingleLineEditor/CaretSnapsToUtf8Boundary",
          TestSingleLineEditorCaretSnapsToUtf8Boundary);
  AddTest(tests, "SingleLineEditor/SelectsWordAtOffset",
          TestSingleLineEditorSelectsWordAtOffset);
  AddTest(tests, "SingleLineEditor/SupportsWordMotionAndDeletion",
          TestSingleLineEditorSupportsWordMotionAndDeletion);
  AddTest(tests, "SingleLineEditor/WordMotionHandlesUnicode",
          TestSingleLineEditorWordMotionHandlesUnicode);
  AddTest(tests, "SingleLineEditor/ExtendSelectionToAnchorsAtTheCaret",
          TestSingleLineEditorExtendSelectionToAnchorsAtTheCaret);
  AddTest(tests, "SingleLineEditor/KeyHandlerBindsWordShortcuts",
          TestSingleLineKeyHandlerBindsWordShortcuts);
}

}
