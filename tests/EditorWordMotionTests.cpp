#include "TestSupport.h"

#include <string>
#include <string_view>

#include "editor/TextViewport.h"
#include "editor/WordBoundary.h"

namespace microide::tests {
namespace {

using microide::editor::TextViewport;

std::string DocumentText(const TextViewport& viewport) {
  std::string out;
  for (std::size_t line = 0; line < viewport.line_count(); ++line) {
    if (line > 0) {
      out.push_back('\n');
    }
    out.append(viewport.lines().LineView(line));
  }
  return out;
}

void ExpectCaret(const TextViewport& viewport,
                 std::size_t line,
                 std::size_t column,
                 std::string_view context) {
  Expect(viewport.cursor_line() == line && viewport.cursor_column() == column,
         std::string(context) + ": caret at " + std::to_string(viewport.cursor_line()) + ":" +
             std::to_string(viewport.cursor_column()) + ", expected " + std::to_string(line) + ":" +
             std::to_string(column));
}

// The classification is the whole contract: a run of separators is its own word,
// so `foo === bar` has three stops and not two. This is VS Code's rule and it is
// what makes Ctrl+Right usable inside an expression rather than a line skipper.
void TestWordBoundaryTreatsSeparatorRunsAsWords() {
  constexpr std::string_view kText = "foo === bar";
  Expect(editor::WordBoundaryRight(kText, 0) == 3, "word-right stops at the end of foo");
  Expect(editor::WordBoundaryRight(kText, 3) == 7, "word-right stops at the end of the === run");
  Expect(editor::WordBoundaryRight(kText, 7) == 11, "word-right stops at the end of bar");
  Expect(editor::WordBoundaryRight(kText, 11) == 11, "word-right saturates at the end");

  Expect(editor::WordBoundaryLeft(kText, 11) == 8, "word-left stops at the start of bar");
  Expect(editor::WordBoundaryLeft(kText, 8) == 4, "word-left stops at the start of the === run");
  Expect(editor::WordBoundaryLeft(kText, 4) == 0, "word-left stops at the start of foo");
  Expect(editor::WordBoundaryLeft(kText, 0) == 0, "word-left saturates at the start");
}

void TestWordBoundaryKeepsMultibyteWordsWhole() {
  // "café ñoño": é is two bytes, ñ is two bytes. A byte-wise rule stops inside
  // each of them; the codepoint rule treats both words as single stops.
  constexpr std::string_view kText = "café ñoño";
  Expect(editor::WordBoundaryRight(kText, 0) == 5, "word-right clears the whole of café");
  Expect(editor::WordBoundaryRight(kText, 5) == 12, "word-right clears the whole of ñoño");
  Expect(editor::WordBoundaryLeft(kText, 12) == 6, "word-left lands on the start of ñoño");
  Expect(editor::WordBoundaryLeft(kText, 6) == 0, "word-left lands on the start of café");
}

// The delete boundary differs from the motion boundary in exactly one place: a
// run of two or more spaces goes on its own, so Ctrl+Backspace out of an indent
// lands on column 0 instead of eating the previous word with it.
void TestDeleteWordBoundaryTakesAWhitespaceRunOnItsOwn() {
  constexpr std::string_view kIndented = "    value";
  Expect(editor::DeleteWordBoundaryLeft(kIndented, 4) == 0,
         "delete-word-left removes the indent run alone");
  Expect(editor::WordBoundaryLeft(kIndented, 4) == 0,
         "motion also stops at 0 here -- there is no word before the indent");

  constexpr std::string_view kGap = "foo    bar";
  Expect(editor::DeleteWordBoundaryLeft(kGap, 7) == 3,
         "delete-word-left removes only the gap, leaving foo");
  Expect(editor::WordBoundaryLeft(kGap, 7) == 0,
         "motion, by contrast, walks past the gap onto foo");

  // A single space is not a run: it goes with the word, which is what makes one
  // Ctrl+Backspace remove `bar ` rather than needing two.
  constexpr std::string_view kSingle = "foo bar";
  Expect(editor::DeleteWordBoundaryLeft(kSingle, 4) == 0,
         "a lone space does not trigger the whitespace-only delete");

  Expect(editor::DeleteWordBoundaryRight(kGap, 3) == 7,
         "delete-word-right removes only the gap");
  Expect(editor::DeleteWordBoundaryRight(kSingle, 3) == 7,
         "a lone space is deleted together with the following word");
}

// Double-click-to-select and the occurrence highlight both stand on the same
// identifier run. Both used to scan with a byte-wise ASCII predicate, so a word
// with any non-ASCII letter in it was selected only up to that letter.
void TestViewportSelectsMultibyteWordsWhole() {
  TextViewport viewport;
  viewport.SetViewportSize(10, 80);
  viewport.LoadContent("int café = résumé;\n", "/tmp/word.txt");

  // Caret on the 'c' of café.
  viewport.MoveCursorTo(0, 4);
  viewport.SelectWordAtCursor();
  Expect(viewport.SelectedText() == "café",
         "double-click on café should select all of it, not just caf");

  // And from inside the accented tail, where the byte-wise rule selected nothing.
  const auto range = viewport.WordRangeAt(editor::TextPosition{0, 7});
  Expect(range.has_value(), "a position inside the multi-byte tail is still inside the word");
  Expect(range->start.column == 4 && range->end.column == 9,
         "the run should span the whole of café");

  // A caret mid-scalar (a mouse hit can land there) resolves to the same word
  // rather than to nothing.
  const auto mid_scalar = viewport.WordRangeAt(editor::TextPosition{0, 8});
  Expect(mid_scalar.has_value() && mid_scalar->start.column == 4 && mid_scalar->end.column == 9,
         "a caret inside a multi-byte character still resolves to its word");
}

void TestViewportWordMotionCrossesLines() {
  TextViewport viewport;
  viewport.SetViewportSize(10, 80);
  viewport.LoadContent("alpha beta\ngamma\n", "/tmp/word.txt");

  viewport.MoveCursorTo(0, 0);
  viewport.MoveCursorWord(1);
  ExpectCaret(viewport, 0, 5, "word-right past alpha");
  viewport.MoveCursorWord(1);
  ExpectCaret(viewport, 0, 10, "word-right past beta");
  // At the line end the next step is the line break itself.
  viewport.MoveCursorWord(1);
  ExpectCaret(viewport, 1, 0, "word-right crosses onto the next line");
  viewport.MoveCursorWord(1);
  ExpectCaret(viewport, 1, 5, "word-right past gamma");

  viewport.MoveCursorWord(-1);
  ExpectCaret(viewport, 1, 0, "word-left back to the start of gamma");
  viewport.MoveCursorWord(-1);
  ExpectCaret(viewport, 0, 10, "word-left crosses back onto the previous line");
  viewport.MoveCursorWord(-1);
  ExpectCaret(viewport, 0, 6, "word-left to the start of beta");
  viewport.MoveCursorWord(-1);
  ExpectCaret(viewport, 0, 0, "word-left to the start of alpha");
  viewport.MoveCursorWord(-1);
  ExpectCaret(viewport, 0, 0, "word-left saturates at the document start");
}

// Shift+Ctrl+Arrow extends rather than collapsing, and a plain Ctrl+Arrow over a
// selection MOVES from the caret (VS Code cursorWordStartLeft) rather than
// collapsing to the selection edge the way plain Left/Right does.
void TestViewportWordMotionSelectionSemantics() {
  TextViewport viewport;
  viewport.SetViewportSize(10, 80);
  viewport.LoadContent("alpha beta gamma\n", "/tmp/word.txt");

  viewport.MoveCursorTo(0, 0);
  viewport.MoveCursorWord(1, /*extend_selection=*/true);
  const auto extended = viewport.selection_range();
  Expect(extended.has_value(), "shift+ctrl+right should start a selection");
  Expect(extended->start.column == 0 && extended->end.column == 5,
         "shift+ctrl+right should select the first word");
  viewport.MoveCursorWord(1, /*extend_selection=*/true);
  const auto grown = viewport.selection_range();
  Expect(grown.has_value() && grown->end.column == 10,
         "a second shift+ctrl+right should grow the selection by a word");

  // Plain Ctrl+Left from the active caret at column 10: it does not collapse to
  // column 0 (the anchor), it steps one word back from 10.
  viewport.MoveCursorWord(-1);
  Expect(!viewport.has_selection(), "a plain word step should drop the selection");
  ExpectCaret(viewport, 0, 6, "plain ctrl+left moves from the caret, not the anchor");
}

void TestViewportDeleteWordBackwardAndForward() {
  TextViewport viewport;
  viewport.SetViewportSize(10, 80);
  viewport.LoadContent("alpha beta\ngamma\n", "/tmp/word.txt");

  viewport.MoveCursorTo(0, 10);
  viewport.DeleteWord(-1);
  Expect(DocumentText(viewport) == "alpha \ngamma\n", "ctrl+backspace should remove beta");
  ExpectCaret(viewport, 0, 6, "caret lands where the word started");

  // At column 0 the backward word delete is the line join, matching Backspace.
  viewport.MoveCursorTo(1, 0);
  viewport.DeleteWord(-1);
  Expect(DocumentText(viewport) == "alpha gamma\n", "ctrl+backspace at column 0 joins the lines");

  viewport.MoveCursorTo(0, 0);
  viewport.DeleteWord(1);
  Expect(DocumentText(viewport) == " gamma\n", "ctrl+delete should remove the leading word");

  // One Undo puts the whole word back: a word delete is one user action and must
  // not be split across per-character history entries.
  Expect(viewport.Undo(), "undo should report a change");
  Expect(DocumentText(viewport) == "alpha gamma\n", "one undo restores the whole word");
}

void TestViewportWordDeleteRemovesSelectionInstead() {
  TextViewport viewport;
  viewport.SetViewportSize(10, 80);
  viewport.LoadContent("alpha beta gamma\n", "/tmp/word.txt");

  viewport.MoveCursorTo(0, 6);
  viewport.MoveCursorTo(0, 10, /*extend_selection=*/true);
  viewport.DeleteWord(-1);
  Expect(DocumentText(viewport) == "alpha  gamma\n",
         "a word delete over a selection removes exactly the selection");
}

void TestViewportWordVerbsApplyToEveryCaret() {
  TextViewport viewport;
  viewport.SetViewportSize(10, 80);
  viewport.LoadContent("alpha one\nbravo two\n", "/tmp/word.txt");

  viewport.MoveCursorTo(0, 9);
  viewport.AddSecondaryCaret(1, 9);
  Expect(viewport.has_multiple_carets(), "expected two carets");

  viewport.MoveCursorWord(-1);
  ExpectCaret(viewport, 0, 6, "primary caret stepped a word left");
  Expect(viewport.has_multiple_carets(), "the secondary caret survives a word step");

  // A fresh viewport: MoveCursorTo does not clear the secondary set, so reusing
  // the one above would delete at three carets and prove nothing about two.
  TextViewport deleter;
  deleter.SetViewportSize(10, 80);
  deleter.LoadContent("alpha one\nbravo two\n", "/tmp/word.txt");
  deleter.MoveCursorTo(0, 9);
  deleter.AddSecondaryCaret(1, 9);
  deleter.DeleteWord(-1);
  Expect(DocumentText(deleter) == "alpha \nbravo \n",
         "ctrl+backspace should remove a word at every caret");
}

}  // namespace

void RegisterEditorWordMotionTests(std::vector<TestCase>& tests) {
  AddTest(tests, "EditorWordMotion/BoundaryTreatsSeparatorRunsAsWords",
          TestWordBoundaryTreatsSeparatorRunsAsWords);
  AddTest(tests, "EditorWordMotion/BoundaryKeepsMultibyteWordsWhole",
          TestWordBoundaryKeepsMultibyteWordsWhole);
  AddTest(tests, "EditorWordMotion/DeleteBoundaryTakesWhitespaceRunAlone",
          TestDeleteWordBoundaryTakesAWhitespaceRunOnItsOwn);
  AddTest(tests, "EditorWordMotion/ViewportSelectsMultibyteWordsWhole",
          TestViewportSelectsMultibyteWordsWhole);
  AddTest(tests, "EditorWordMotion/ViewportMotionCrossesLines",
          TestViewportWordMotionCrossesLines);
  AddTest(tests, "EditorWordMotion/ViewportMotionSelectionSemantics",
          TestViewportWordMotionSelectionSemantics);
  AddTest(tests, "EditorWordMotion/ViewportDeleteWordBackwardAndForward",
          TestViewportDeleteWordBackwardAndForward);
  AddTest(tests, "EditorWordMotion/ViewportWordDeleteRemovesSelectionInstead",
          TestViewportWordDeleteRemovesSelectionInstead);
  AddTest(tests, "EditorWordMotion/ViewportWordVerbsApplyToEveryCaret",
          TestViewportWordVerbsApplyToEveryCaret);
}

}  // namespace microide::tests
