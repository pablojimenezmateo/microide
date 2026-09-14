// Edge-case probes for multi-caret, soft-wrap, and selection interactions.
//
// Each test here names a boundary the ordinary paths do not reach: a caret set
// that collides after a move, a wrapped row whose Home/End is not the logical
// line's, a shaping action driven from several carets at once. The point is the
// corner, not the happy path -- the happy paths live in EditorMultiCaretTests
// and TextViewportTests.

#include "TestSupport.h"

#include <string>
#include <vector>

#include "editor/LanguageContractView.h"
#include "editor/ShapingActions.h"
#include "editor/TextViewport.h"

namespace microide::tests {
namespace {

using microide::editor::SelectionRange;
using microide::editor::TextPosition;
using microide::editor::TextViewport;

std::string JoinLines(const TextViewport& viewport) {
  std::string out;
  for (std::size_t i = 0; i < viewport.lines().size(); ++i) {
    if (i != 0) out.push_back('\n');
    out.append(viewport.lines()[i]);
  }
  return out;
}

std::string CaretDump(const TextViewport& viewport) {
  std::string out = "P(" + std::to_string(viewport.cursor_line()) + "," +
                    std::to_string(viewport.cursor_column()) + ")";
  for (const TextPosition& caret : viewport.secondary_carets()) {
    out += " S(" + std::to_string(caret.line) + "," + std::to_string(caret.column) + ")";
  }
  return out;
}

// --- 1. Shift+Down at every caret, then typing, replaces every selection. ---
void TestMultiCaretShiftDownThenTypeReplacesEverySelection() {
  TextViewport viewport;
  viewport.LoadContent("aaaa\nbbbb\ncccc\ndddd\n", "/tmp/ec-shift-down.txt");
  viewport.SetViewportSize(10, 40);
  viewport.MoveCursorTo(0, 1);
  viewport.SetSecondaryCarets({{2, 1}});

  viewport.MoveCursorVertical(1, /*extend_selection=*/true);
  Expect(viewport.has_selection(), "primary should own a selection after Shift+Down");
  const auto ranges = viewport.secondary_caret_ranges();
  Expect(ranges.size() == 1 && ranges[0].selection_anchor.has_value(),
         "the secondary caret should have begun a selection too");

  viewport.InsertText("X");
  Expect(JoinLines(viewport) == "aXbbb\ncXddd\n",
         std::string("both selections should be replaced, got: ") + JoinLines(viewport));
}

// --- 2. Two non-empty selections that merely TOUCH stay separate, and the edit
// still applies at both. VS Code's CursorCollection.normalize merges a touching
// pair only when one of the two is collapsed (`isBeforeOrEqual`); two ranged
// selections need a strict overlap (`isBefore`). Getting this wrong in either
// direction is silent: merging eats a cursor, and treating the touch as an
// overlap makes every subsequent keystroke do nothing. ---
void TestMultiCaretTouchingSelectionsStaySeparateAndStillEdit() {
  TextViewport viewport;
  viewport.LoadContent("aaaa\nbbbb\ncccc\n", "/tmp/ec-merge-grow.txt");
  viewport.SetViewportSize(10, 40);
  viewport.MoveCursorTo(0, 0);
  viewport.SetSecondaryCarets({{1, 0}});

  // Primary selects (0,0)->(1,0); secondary selects (1,0)->(2,0). They share the
  // single position (1,0) and neither is collapsed.
  viewport.MoveCursorVertical(1, /*extend_selection=*/true);
  Expect(viewport.secondary_carets().size() == 1,
         std::string("two ranged selections that only touch must both survive: ") +
             CaretDump(viewport));
  viewport.InsertText("X");
  Expect(JoinLines(viewport) == "XXcccc\n",
         std::string("both touching selections should be replaced: ") + JoinLines(viewport));
}

// Strict overlap is the other side of that rule: it must NOT produce two
// surviving carets, because the multi-caret appliers refuse to run over one.
void TestMultiCaretOverlappingSelectionsMerge() {
  TextViewport viewport;
  viewport.LoadContent("aaaa\nbbbb\ncccc\n", "/tmp/ec-merge-overlap.txt");
  viewport.SetViewportSize(10, 40);
  viewport.MoveCursorTo(0, 0);
  viewport.MoveCursorTo(1, 2, /*extend_selection=*/true);
  std::vector<SelectionRange> ranges{
      SelectionRange{TextPosition{1, 1}, TextPosition{2, 2}},
  };
  viewport.SetSecondaryCaretsWithRanges(ranges);

  Expect(viewport.secondary_carets().empty(),
         std::string("overlapping selections must merge into one: ") + CaretDump(viewport));
  const auto merged = viewport.selection_range();
  Expect(merged.has_value() && merged->start == TextPosition{0, 0} &&
             merged->end == TextPosition{2, 2},
         "the surviving selection should span the union of both");
}

// --- 3. Vertical move at the buffer edges collapses duplicate carets. ---
void TestMultiCaretVerticalMoveAtBufferEdgeDedupes() {
  TextViewport viewport;
  viewport.LoadContent("aaaa\nbbbb\ncccc\n", "/tmp/ec-edge-dedupe.txt");
  viewport.SetViewportSize(10, 40);
  viewport.MoveCursorTo(0, 2);
  viewport.SetSecondaryCarets({{1, 2}});

  viewport.MoveCursorVertical(-1);  // both clamp onto line 0
  Expect(viewport.secondary_carets().empty(),
         std::string("carets that clamp onto the same line must merge: ") + CaretDump(viewport));
  Expect(viewport.cursor_line() == 0, "primary should sit on line 0");
}

// --- 4. Undo restores the caret set that produced the edit. ---
void TestMultiCaretUndoRestoresEveryCaret() {
  TextViewport viewport;
  viewport.LoadContent("aaaa\nbbbb\ncccc\n", "/tmp/ec-undo-carets.txt");
  viewport.SetViewportSize(10, 40);
  viewport.MoveCursorTo(0, 2);
  viewport.SetSecondaryCarets({{1, 2}, {2, 2}});

  viewport.InsertText("Z");
  Expect(viewport.Undo(), "the multi-caret insert should undo");
  Expect(JoinLines(viewport) == "aaaa\nbbbb\ncccc\n", "undo should restore the text");
  Expect(viewport.secondary_carets().size() == 2,
         std::string("undo should restore both secondary carets: ") + CaretDump(viewport));
}

// --- 5. Home on a wrapped row goes to the row start, per caret. ---
void TestMultiCaretHomeOnWrappedRowsUsesViewLineStart() {
  TextViewport viewport;
  viewport.LoadContent("abcdefghijklmnop\nabcdefghijklmnop\n", "/tmp/ec-wrap-home.txt");
  viewport.SetViewportSize(10, /*visible_columns=*/8);
  viewport.SetSoftWrap(true);
  viewport.MoveCursorTo(0, 12);           // second visual row of line 0
  viewport.SetSecondaryCarets({{1, 12}});  // second visual row of line 1

  viewport.MoveCursorLineStart();
  Expect(viewport.cursor_column() == 8,
         "Home on a wrapped row should land on that row's first column, got " +
             std::to_string(viewport.cursor_column()));
  Expect(viewport.secondary_carets().size() == 1 &&
             viewport.secondary_carets().front().column == 8,
         std::string("the secondary caret should use its own row start: ") + CaretDump(viewport));
}

// --- 6. End on a wrapped row stops at the wrap point, per caret. ---
void TestMultiCaretEndOnWrappedRowsUsesViewLineEnd() {
  TextViewport viewport;
  viewport.LoadContent("abcdefghijklmnop\nabcdefghijklmnop\n", "/tmp/ec-wrap-end.txt");
  viewport.SetViewportSize(10, /*visible_columns=*/8);
  viewport.SetSoftWrap(true);
  viewport.MoveCursorTo(0, 2);
  viewport.SetSecondaryCarets({{1, 2}});

  viewport.MoveCursorLineEnd();
  Expect(viewport.cursor_column() == 8,
         "End on the first wrapped row should stop at the wrap point, got " +
             std::to_string(viewport.cursor_column()));
  Expect(viewport.secondary_carets().size() == 1 &&
             viewport.secondary_carets().front().column == 8,
         std::string("the secondary caret should stop at its own wrap point: ") +
             CaretDump(viewport));
}

// --- 7. Down on a wrapped line steps one visual row, not one logical line. ---
void TestMultiCaretDownOnWrappedLineStepsOneVisualRow() {
  TextViewport viewport;
  viewport.LoadContent("abcdefghijklmnop\nZZ\nabcdefghijklmnop\n", "/tmp/ec-wrap-down.txt");
  viewport.SetViewportSize(10, /*visible_columns=*/8);
  viewport.SetSoftWrap(true);
  viewport.MoveCursorTo(0, 2);
  viewport.SetSecondaryCarets({{2, 2}});

  viewport.MoveCursorVertical(1);
  Expect(viewport.cursor_line() == 0 && viewport.cursor_column() == 10,
         std::string("primary should stay on line 0's second row: ") + CaretDump(viewport));
  Expect(viewport.secondary_carets().size() == 1 &&
             viewport.secondary_carets().front() == TextPosition{2, 10},
         std::string("the secondary should also step one visual row: ") + CaretDump(viewport));
}

// --- 8. Move-line-up with a caret already on line 0 is a whole no-op. ---
void TestMultiCaretMoveLineUpBlockedByTopCaretIsNoOp() {
  TextViewport viewport;
  viewport.LoadContent("aaaa\nbbbb\ncccc\n", "/tmp/ec-moveline-top.txt");
  viewport.SetViewportSize(10, 40);
  viewport.MoveCursorTo(0, 0);
  viewport.SetSecondaryCarets({{2, 0}});

  microide::editor::MoveLineUp(viewport);
  Expect(viewport.lines()[0] == "aaaa",
         std::string("line 0 cannot move up, so it must stay put: ") + JoinLines(viewport));
}

// --- 9. Delete-line with carets on adjacent lines removes each line once. ---
void TestMultiCaretDeleteLineOnAdjacentLinesRemovesEachOnce() {
  TextViewport viewport;
  viewport.LoadContent("aaaa\nbbbb\ncccc\ndddd\n", "/tmp/ec-deleteline-adj.txt");
  viewport.SetViewportSize(10, 40);
  viewport.MoveCursorTo(1, 0);
  viewport.SetSecondaryCarets({{2, 0}});

  microide::editor::DeleteLine(viewport);
  Expect(JoinLines(viewport) == "aaaa\ndddd\n",
         std::string("both caret lines should be deleted exactly once: ") + JoinLines(viewport));
}

// --- 10. SelectAll drops every secondary caret. ---
void TestSelectAllClearsSecondaryCarets() {
  TextViewport viewport;
  viewport.LoadContent("aaaa\nbbbb\ncccc\n", "/tmp/ec-selectall.txt");
  viewport.SetViewportSize(10, 40);
  viewport.MoveCursorTo(0, 0);
  viewport.SetSecondaryCarets({{1, 0}, {2, 0}});

  viewport.SelectAll();
  Expect(viewport.secondary_carets().empty(),
         std::string("Select All is a single-selection operation: ") + CaretDump(viewport));
}

// --- 11. Multi-caret copy yields one line per caret, in document order. ---
void TestMultiCaretCopyAggregatesInDocumentOrder() {
  TextViewport viewport;
  viewport.LoadContent("alpha\nbeta\ngamma\n", "/tmp/ec-mc-copy.txt");
  viewport.SetViewportSize(10, 40);
  // Primary is BELOW the secondary, so the aggregate must not follow caret
  // insertion order. The primary's selection is established first: a
  // non-extending MoveCursorTo collapses every caret's anchor, so setting the
  // secondary ranges last is what keeps them.
  viewport.MoveCursorTo(2, 0);
  viewport.MoveCursorTo(2, 5, /*extend_selection=*/true);
  std::vector<SelectionRange> ranges{
      SelectionRange{TextPosition{0, 0}, TextPosition{0, 5}},
  };
  viewport.SetSecondaryCaretsWithRanges(ranges);

  const auto text = viewport.MultiCaretSelectedText();
  Expect(text.has_value(), "two ranged carets should produce an aggregate copy");
  Expect(*text == "alpha\ngamma\n" || *text == "alpha\ngamma",
         std::string("copy should be in document order, got: ") + *text);
}

// --- 12. A secondary caret past a shrunken line clamps instead of dangling. ---
void TestSecondaryCaretClampsWhenItsLineShrinks() {
  TextViewport viewport;
  viewport.LoadContent("aaaaaaaa\nbbbbbbbb\n", "/tmp/ec-clamp-shrink.txt");
  viewport.SetViewportSize(10, 40);
  viewport.MoveCursorTo(0, 0);
  viewport.SetSecondaryCarets({{1, 8}});

  // Select all of line 1 from the primary and delete it, shrinking line 1 to
  // nothing under the secondary caret.
  viewport.MoveCursorTo(1, 0);
  viewport.MoveCursorTo(1, 8, /*extend_selection=*/true);
  viewport.DeleteSelectedText();

  for (const TextPosition& caret : viewport.secondary_carets()) {
    Expect(caret.line < viewport.lines().size(),
           "a secondary caret must never name a line past the buffer");
    Expect(caret.column <= viewport.lines().LineLength(caret.line),
           std::string("a secondary caret must never name a column past its line: ") +
               CaretDump(viewport));
  }
}

// --- 13. Box selection over a short line stays inside that line. ---
void TestBoxSelectionOverShortLineClampsToThatLine() {
  TextViewport viewport;
  viewport.LoadContent("aaaaaaaaaa\nbb\ncccccccccc\n", "/tmp/ec-box-short.txt");
  viewport.SetViewportSize(10, 40);
  viewport.SetBoxSelection(TextPosition{0, 4}, TextPosition{2, 8});

  const auto ranges = viewport.secondary_caret_ranges();
  for (const auto& caret : ranges) {
    Expect(caret.position.column <= viewport.lines().LineLength(caret.position.line),
           std::string("box caret past its line end: ") + CaretDump(viewport));
    if (caret.selection_anchor.has_value()) {
      Expect(caret.selection_anchor->column <=
                 viewport.lines().LineLength(caret.selection_anchor->line),
             "box anchor past its line end");
    }
  }
}

// --- 14. Undo after a multi-caret selection delete restores the ranges. ---
void TestMultiCaretDeleteSelectionsUndoRestoresText() {
  TextViewport viewport;
  viewport.LoadContent("alpha\nbeta\ngamma\n", "/tmp/ec-mc-delsel-undo.txt");
  viewport.SetViewportSize(10, 40);
  std::vector<SelectionRange> ranges{
      SelectionRange{TextPosition{1, 0}, TextPosition{1, 4}},
      SelectionRange{TextPosition{2, 0}, TextPosition{2, 5}},
  };
  viewport.MoveCursorTo(0, 0);
  viewport.MoveCursorTo(0, 5, /*extend_selection=*/true);
  viewport.SetSecondaryCaretsWithRanges(ranges);

  Expect(viewport.DeleteMultiCaretSelections(), "every caret owns a selection");
  Expect(JoinLines(viewport) == "\n\n\n",
         std::string("all three selections should be gone: ") + JoinLines(viewport));
  Expect(viewport.Undo(), "the aggregate delete should undo in one step");
  Expect(JoinLines(viewport) == "alpha\nbeta\ngamma\n",
         std::string("undo should restore every deleted range: ") + JoinLines(viewport));
}

// --- 15. Backspace at column 0 with several carets joins each line once. ---
void TestMultiCaretBackspaceAtColumnZeroJoinsEachLineOnce() {
  TextViewport viewport;
  viewport.LoadContent("aa\nbb\ncc\ndd\n", "/tmp/ec-mc-bs-join.txt");
  viewport.SetViewportSize(10, 40);
  viewport.MoveCursorTo(1, 0);
  viewport.SetSecondaryCarets({{3, 0}});

  viewport.Backspace();
  Expect(JoinLines(viewport) == "aabb\nccdd\n",
         std::string("each caret should join its own line once: ") + JoinLines(viewport));
}

}  // namespace

void RegisterEditorEdgeCaseTests(std::vector<TestCase>& tests) {
  AddTest(tests, "EditorEdgeCase/MultiCaretShiftDownThenTypeReplacesEverySelection",
          TestMultiCaretShiftDownThenTypeReplacesEverySelection);
  AddTest(tests, "EditorEdgeCase/MultiCaretTouchingSelectionsStaySeparateAndStillEdit",
          TestMultiCaretTouchingSelectionsStaySeparateAndStillEdit);
  AddTest(tests, "EditorEdgeCase/MultiCaretOverlappingSelectionsMerge",
          TestMultiCaretOverlappingSelectionsMerge);
  AddTest(tests, "EditorEdgeCase/MultiCaretVerticalMoveAtBufferEdgeDedupes",
          TestMultiCaretVerticalMoveAtBufferEdgeDedupes);
  AddTest(tests, "EditorEdgeCase/MultiCaretUndoRestoresEveryCaret",
          TestMultiCaretUndoRestoresEveryCaret);
  AddTest(tests, "EditorEdgeCase/MultiCaretHomeOnWrappedRowsUsesViewLineStart",
          TestMultiCaretHomeOnWrappedRowsUsesViewLineStart);
  AddTest(tests, "EditorEdgeCase/MultiCaretEndOnWrappedRowsUsesViewLineEnd",
          TestMultiCaretEndOnWrappedRowsUsesViewLineEnd);
  AddTest(tests, "EditorEdgeCase/MultiCaretDownOnWrappedLineStepsOneVisualRow",
          TestMultiCaretDownOnWrappedLineStepsOneVisualRow);
  AddTest(tests, "EditorEdgeCase/MultiCaretMoveLineUpBlockedByTopCaretIsNoOp",
          TestMultiCaretMoveLineUpBlockedByTopCaretIsNoOp);
  AddTest(tests, "EditorEdgeCase/MultiCaretDeleteLineOnAdjacentLinesRemovesEachOnce",
          TestMultiCaretDeleteLineOnAdjacentLinesRemovesEachOnce);
  AddTest(tests, "EditorEdgeCase/SelectAllClearsSecondaryCarets", TestSelectAllClearsSecondaryCarets);
  AddTest(tests, "EditorEdgeCase/MultiCaretCopyAggregatesInDocumentOrder",
          TestMultiCaretCopyAggregatesInDocumentOrder);
  AddTest(tests, "EditorEdgeCase/SecondaryCaretClampsWhenItsLineShrinks",
          TestSecondaryCaretClampsWhenItsLineShrinks);
  AddTest(tests, "EditorEdgeCase/BoxSelectionOverShortLineClampsToThatLine",
          TestBoxSelectionOverShortLineClampsToThatLine);
  AddTest(tests, "EditorEdgeCase/MultiCaretDeleteSelectionsUndoRestoresText",
          TestMultiCaretDeleteSelectionsUndoRestoresText);
  AddTest(tests, "EditorEdgeCase/MultiCaretBackspaceAtColumnZeroJoinsEachLineOnce",
          TestMultiCaretBackspaceAtColumnZeroJoinsEachLineOnce);
}

}  // namespace microide::tests
