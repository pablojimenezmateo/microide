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

#include "editor/FoldingModel.h"
#include "editor/LanguageContractView.h"
#include "editor/ShapingActions.h"
#include "editor/TextViewport.h"

namespace microide::tests {
namespace {

using microide::editor::FoldingModel;
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

FoldingModel::ComputeOptions CStyleFoldOptions() {
  FoldingModel::ComputeOptions options;
  options.bracket_pairs = {{'{', '}'}};
  options.use_indent_source = true;
  options.tab_size = 4;
  return options;
}

// --- 16. Folding and soft wrap compose: a collapsed fold removes its body's
// wrapped rows, not one row per hidden LINE. Getting this wrong scrolls the
// view by the wrong amount and puts the caret on a different row than the one
// the mouse hit. ---
void TestCollapsedFoldRemovesEveryWrappedRowOfItsBody() {
  TextViewport viewport;
  // Line 2 is long enough to occupy three visual rows at width 8.
  // The opener is deliberately SHORT enough not to wrap at width 8, so the row
  // arithmetic below is about the hidden body's height and nothing else.
  viewport.LoadContent(
      "head\n"
      "f() {\n"
      "  aaaaaaaaaaaaaaaaaaaa;\n"
      "  b();\n"
      "}\n"
      "tail\n",
      "/tmp/ec-fold-wrap.cpp");
  viewport.SetViewportSize(/*visible_lines=*/20, /*visible_columns=*/8);
  viewport.SetSoftWrap(true);

  const std::size_t tail_row_expanded = viewport.VisualRowForLine(5);

  FoldingModel folding_model;
  Expect(folding_model.Compute(viewport.lines().Snapshot(), CStyleFoldOptions()),
         "the fold fixture should compute");
  Expect(folding_model.Collapse(1), "the function fold should collapse");
  viewport.SetFoldingModel(&folding_model);

  const std::size_t tail_row_collapsed = viewport.VisualRowForLine(5);
  Expect(tail_row_collapsed < tail_row_expanded,
         "collapsing a fold must pull the following line up");

  // The whole body -- including the closing brace, as in VS Code, which draws
  // the fold as `void f() { … }` on the opener's row -- is hidden.
  for (const std::size_t line : {std::size_t{2}, std::size_t{3}, std::size_t{4}}) {
    Expect(folding_model.IsLineHidden(line),
           "line " + std::to_string(line) + " is inside the collapsed fold and must be hidden");
  }
  Expect(!folding_model.IsLineHidden(1) && !folding_model.IsLineHidden(5),
         "the opener and the line after the fold stay visible");

  // The collapsed fold occupies exactly the opener's own rows. Line 1 is short,
  // so that is one row -- and `tail` follows immediately, even though the hidden
  // body included a line that wrapped across three rows. A row table that
  // removed one row per hidden LINE rather than per hidden ROW would leave a
  // two-row gap here.
  const std::size_t opener_row = viewport.VisualRowForLine(1);
  Expect(tail_row_collapsed == opener_row + 1,
         "the line after a collapsed fold follows its opener with no gap, got row " +
             std::to_string(tail_row_collapsed) + " after opener row " +
             std::to_string(opener_row));
  // And the gap that closed is the wrapped body's full height: line 2 alone took
  // three rows at width 8, so more than three rows disappeared.
  Expect(tail_row_expanded - tail_row_collapsed >= 4,
         "collapsing must remove every wrapped row of the body, removed only " +
             std::to_string(tail_row_expanded - tail_row_collapsed));
}

// --- 17. Every visible line's visual row must be strictly increasing and every
// hidden line must map to a row inside its collapsed opener's span. The row
// table is what the renderer and every hit test read. ---
void TestVisualRowsStayMonotonicAcrossAFoldUnderWrap() {
  TextViewport viewport;
  viewport.LoadContent(
      "head\n"
      "void f() {\n"
      "  aaaaaaaaaaaaaaaaaaaa;\n"
      "  void g() {\n"
      "    bbbbbbbbbbbbbbbbbbbb;\n"
      "  }\n"
      "}\n"
      "tail is also a fairly long line here\n",
      "/tmp/ec-fold-monotonic.cpp");
  viewport.SetViewportSize(/*visible_lines=*/30, /*visible_columns=*/9);
  viewport.SetSoftWrap(true);

  FoldingModel folding_model;
  Expect(folding_model.Compute(viewport.lines().Snapshot(), CStyleFoldOptions()),
         "the nested fold fixture should compute");
  Expect(folding_model.Collapse(3), "the inner fold should collapse");
  viewport.SetFoldingModel(&folding_model);

  std::size_t previous_row = 0;
  bool first = true;
  for (std::size_t line = 0; line < viewport.lines().size(); ++line) {
    const std::size_t row = viewport.VisualRowForLine(line);
    if (folding_model.IsLineHidden(line)) {
      continue;
    }
    if (!first) {
      Expect(row > previous_row,
             "visible line " + std::to_string(line) + " should sit below the one before it, row " +
                 std::to_string(row) + " after " + std::to_string(previous_row));
    }
    first = false;
    previous_row = row;
  }

  // Collapsing the outer fold too must not reorder anything.
  Expect(folding_model.Collapse(1), "the outer fold should collapse as well");
  viewport.SetFoldingModel(nullptr);
  viewport.SetFoldingModel(&folding_model);
  previous_row = 0;
  first = true;
  for (std::size_t line = 0; line < viewport.lines().size(); ++line) {
    if (folding_model.IsLineHidden(line)) continue;
    const std::size_t row = viewport.VisualRowForLine(line);
    if (!first) {
      Expect(row > previous_row,
             "nested collapse reordered visible line " + std::to_string(line));
    }
    first = false;
    previous_row = row;
  }
}

// --- 18. A multi-caret edit whose carets sit on both sides of a collapsed fold
// still edits the hidden lines' neighbours correctly, and the fold survives. ---
void TestMultiCaretEditAcrossACollapsedFoldKeepsBothCarets() {
  TextViewport viewport;
  viewport.LoadContent("head\nvoid f() {\n  a();\n}\ntail\n", "/tmp/ec-fold-mc-edit.cpp");
  viewport.SetViewportSize(20, 40);

  FoldingModel folding_model;
  Expect(folding_model.Compute(viewport.lines().Snapshot(), CStyleFoldOptions()),
         "the fold fixture should compute");
  Expect(folding_model.Collapse(1), "the function fold should collapse");
  viewport.SetFoldingModel(&folding_model);

  viewport.MoveCursorTo(0, 0);
  viewport.SetSecondaryCarets({{4, 0}});
  viewport.InsertText("X");

  Expect(viewport.lines()[0] == "Xhead" && viewport.lines()[4] == "Xtail",
         std::string("both carets should have inserted: ") + JoinLines(viewport));
  Expect(viewport.lines()[2] == "  a();",
         "the hidden body must be untouched by an edit outside it");
  Expect(viewport.secondary_carets().size() == 1,
         std::string("the caret past the fold should survive: ") + CaretDump(viewport));
}

// --- 19. Deleting a selection that spans a collapsed fold deletes the hidden
// lines with it, as in VS Code -- the fold is a display state, not a barrier. ---
void TestDeletingAcrossACollapsedFoldRemovesTheHiddenLines() {
  TextViewport viewport;
  viewport.LoadContent("head\nvoid f() {\n  a();\n}\ntail\n", "/tmp/ec-fold-delete.cpp");
  viewport.SetViewportSize(20, 40);

  FoldingModel folding_model;
  Expect(folding_model.Compute(viewport.lines().Snapshot(), CStyleFoldOptions()),
         "the fold fixture should compute");
  Expect(folding_model.Collapse(1), "the function fold should collapse");
  viewport.SetFoldingModel(&folding_model);

  viewport.MoveCursorTo(0, 4);
  viewport.MoveCursorTo(4, 0, /*extend_selection=*/true);
  Expect(viewport.DeleteSelectedText(), "the selection across the fold should delete");
  Expect(JoinLines(viewport) == "headtail\n",
         std::string("the hidden body should go with the selection: ") + JoinLines(viewport));
}

// --- Sort descending. The comparator is the ascending one with its arguments
// swapped, which is easy to write as a non-strict-weak-ordering `>=` by mistake;
// a duplicate line then makes std::sort read out of bounds. ---
void TestSortLinesDescending() {
  TextViewport viewport;
  viewport.LoadContent("b\nd\na\nc\n", "/tmp/ec-sort-desc.txt");
  viewport.SetViewportSize(10, 40);
  viewport.MoveCursorTo(0, 0);
  viewport.MoveCursorTo(3, 1, /*extend_selection=*/true);
  Expect(microide::editor::SortLines(viewport, /*ascending=*/false), "sort should report a change");
  Expect(JoinLines(viewport) == "d\nc\nb\na\n",
         std::string("descending sort, got: ") + JoinLines(viewport));
}

// --- Two carets with selections far apart sort their own regions and leave the
// lines between them alone. ResolveLineRanges merges only touching ranges, so a
// gap must survive as two edits rather than one span over everything. ---
void TestSortLinesTwoDisjointRegions() {
  TextViewport viewport;
  viewport.LoadContent("b\na\nZZ\nYY\nd\nc\n", "/tmp/ec-sort-two.txt");
  viewport.SetViewportSize(10, 40);
  viewport.MoveCursorTo(0, 0);
  viewport.MoveCursorTo(1, 1, /*extend_selection=*/true);
  std::vector<SelectionRange> ranges = {SelectionRange{TextPosition{4, 0}, TextPosition{5, 1}}};
  viewport.SetSecondaryCaretsWithRanges(ranges);
  Expect(microide::editor::SortLines(viewport, /*ascending=*/true), "two-region sort changes");
  Expect(JoinLines(viewport) == "a\nb\nZZ\nYY\nc\nd\n",
         std::string("each region sorts alone, got: ") + JoinLines(viewport));
}

// --- A region where some lines are already commented and some are not comments
// ALL of them (VS Code: uncomment only when every non-blank line is commented),
// so a second press is a clean round trip rather than a half-toggle. ---
void TestToggleLineCommentMixedRegionCommentsAll() {
  TextViewport viewport;
  viewport.LoadContent("// a\nb\n// c\n", "/tmp/ec-mixed-comment.cpp");
  viewport.SetViewportSize(10, 40);
  viewport.MoveCursorTo(0, 0);
  viewport.MoveCursorTo(2, 4, /*extend_selection=*/true);
  Expect(microide::editor::ToggleLineComment(viewport, "//"), "mixed toggle changes");
  Expect(JoinLines(viewport) == "// // a\n// b\n// // c\n",
         std::string("mixed region should comment every line, got: ") + JoinLines(viewport));
}

// --- Two carets on ADJACENT lines are one region (ranges touching merge), so
// Ctrl+Shift+Up duplicates the pair once. Duplicating per caret would emit the
// shared lines twice. ---
void TestCopyLinesUpWithAdjacentCarets() {
  TextViewport viewport;
  viewport.LoadContent("a\nb\nc\nd\n", "/tmp/ec-copy-adj.txt");
  viewport.SetViewportSize(10, 40);
  viewport.MoveCursorTo(1, 0);
  viewport.SetSecondaryCarets({{2, 0}});
  Expect(microide::editor::CopyLines(viewport, /*downward=*/false), "copy lines up changes");
  Expect(JoinLines(viewport) == "a\nb\nc\nb\nc\nd\n",
         std::string("adjacent carets duplicate the merged region once, got: ") +
             JoinLines(viewport));
}

// --- A box selection is a rectangle of VISUAL columns: a tab-indented line and a
// space-indented line of the same visual width must cut at the same place on
// screen even though their byte columns differ. ---
void TestBoxSelectionAcrossATabKeepsTheRectangle() {
  TextViewport viewport;
  viewport.LoadContent("\tabcd\n    abcd\n", "/tmp/ec-box-tab.txt");
  viewport.SetViewportSize(10, 40);
  viewport.SetTabSize(4);
  // Visual columns 4..6 on both lines: "ab" on line 0 (after the tab) and "ab"
  // on line 1 (after four spaces).
  viewport.SetBoxSelectionVisual(0, 4, 1, 6);
  Expect(viewport.secondary_carets().size() == 1,
         "the box should put a ranged caret on the second line");
  viewport.InsertText("X");
  Expect(JoinLines(viewport) == "\tXcd\n    Xcd\n",
         std::string("box typing replaces the same visual slice, got: ") + JoinLines(viewport));
}

// --- Typing over a box selection replaces every line's slice and undoes as ONE
// step: the box is n selections, and n undo entries would need n presses to
// get back. ---
void TestBoxSelectionTypingUndoRestores() {
  TextViewport viewport;
  viewport.LoadContent("abcdef\nabcdef\nabcdef\n", "/tmp/ec-box-undo.txt");
  viewport.SetViewportSize(10, 40);
  viewport.SetBoxSelection(TextPosition{0, 1}, TextPosition{2, 3});
  viewport.InsertText("Z");
  Expect(JoinLines(viewport) == "aZdef\naZdef\naZdef\n",
         std::string("box typing, got: ") + JoinLines(viewport));
  Expect(viewport.Undo(), "box typing undoes as one step");
  Expect(JoinLines(viewport) == "abcdef\nabcdef\nabcdef\n",
         std::string("undo restores, got: ") + JoinLines(viewport));
}

// --- Outdent strips one indent unit per line independently: a leading tab goes
// whole, a space indent gives up to indent_width spaces, and a SHORT space
// indent gives only what it has rather than eating the first character. ---
void TestOutdentMixedTabAndSpaces() {
  TextViewport viewport;
  viewport.LoadContent("\tone\n    two\n  three\n", "/tmp/ec-outdent-mixed.txt");
  viewport.SetViewportSize(10, 40);
  viewport.SetIndentWidth(4);
  viewport.MoveCursorTo(0, 0);
  viewport.MoveCursorTo(2, 7, /*extend_selection=*/true);
  Expect(microide::editor::OutdentSelection(viewport), "outdent changes");
  Expect(JoinLines(viewport) == "one\ntwo\nthree\n",
         std::string("each line loses one indent unit, got: ") + JoinLines(viewport));
}

// --- Double-clicking a run of whitespace selects the run (VS Code
// `WordOperations.word`: with no word under the pointer it takes the gap between
// the neighbouring runs). Selecting nothing made double-click look broken in
// indentation, which is most of the left edge of a file. ---
void TestSelectWordAtCursorOnWhitespaceRun() {
  TextViewport viewport;
  viewport.LoadContent("ab    cd\n", "/tmp/ec-word-ws.txt");
  viewport.SetViewportSize(10, 40);
  viewport.MoveCursorTo(0, 3);
  viewport.SelectWordOrRunAtCursor();
  Expect(viewport.SelectedText() == "    ",
         std::string("whitespace run selects as a word, got: '") + viewport.SelectedText() + "'");
}

// --- Ctrl+Delete with the caret at end-of-line joins the next line, rather than
// stopping at the boundary and doing nothing for one press. ---
void TestDeleteWordForwardAtLineEndJoins() {
  TextViewport viewport;
  viewport.LoadContent("abc\ndef\n", "/tmp/ec-delword-eol.txt");
  viewport.SetViewportSize(10, 40);
  viewport.MoveCursorTo(0, 3);
  viewport.DeleteWord(1);
  Expect(JoinLines(viewport) == "abcdef\n",
         std::string("VSCode joins the next line, got: ") + JoinLines(viewport));
}


// --- A caret just PAST a word still names that word. VS Code's
// getWordAtPosition is inclusive at both ends, and the mouse rounds to the
// nearest column -- so clicking the right half of `foo`'s last glyph lands here,
// and reading only the code point to the right selected nothing. ---
void TestSelectWordAtCursorAtWordEnd() {
  TextViewport viewport;
  viewport.LoadContent("foo bar\n", "/tmp/ec-word-end.txt");
  viewport.SetViewportSize(10, 40);
  viewport.MoveCursorTo(0, 3);
  viewport.SelectWordAtCursor();
  Expect(viewport.SelectedText() == "foo",
         std::string("caret just past a word selects it, got: '") + viewport.SelectedText() + "'");
}

// --- Same at end-of-line, where there is no code point to the right at all. ---
void TestSelectWordAtCursorAtLineEnd() {
  TextViewport viewport;
  viewport.LoadContent("foo\n", "/tmp/ec-word-lineend.txt");
  viewport.SetViewportSize(10, 40);
  viewport.MoveCursorTo(0, 3);
  viewport.SelectWordAtCursor();
  Expect(viewport.SelectedText() == "foo",
         std::string("caret at EOL just past a word selects it, got: '") + viewport.SelectedText() + "'");
}

// --- Double-clicking inside an operator run selects the whole run (`->`), not
// one character and not nothing: the gap rule sees the same run on both sides
// and normalises the crossed bounds back into it. ---
void TestSelectWordAtCursorOnSeparatorRun() {
  TextViewport viewport;
  viewport.LoadContent("a->b\n", "/tmp/ec-word-sep.txt");
  viewport.SetViewportSize(10, 40);
  viewport.MoveCursorTo(0, 2);
  viewport.SelectWordOrRunAtCursor();
  Expect(viewport.SelectedText() == "->",
         std::string("separator run selects whole, got: '") + viewport.SelectedText() + "'");
}

}  // namespace

void RegisterEditorEdgeCaseTests(std::vector<TestCase>& tests) {
  AddTest(tests, "EditorEdgeCase/CollapsedFoldRemovesEveryWrappedRowOfItsBody",
          TestCollapsedFoldRemovesEveryWrappedRowOfItsBody);
  AddTest(tests, "EditorEdgeCase/VisualRowsStayMonotonicAcrossAFoldUnderWrap",
          TestVisualRowsStayMonotonicAcrossAFoldUnderWrap);
  AddTest(tests, "EditorEdgeCase/MultiCaretEditAcrossACollapsedFoldKeepsBothCarets",
          TestMultiCaretEditAcrossACollapsedFoldKeepsBothCarets);
  AddTest(tests, "EditorEdgeCase/DeletingAcrossACollapsedFoldRemovesTheHiddenLines",
          TestDeletingAcrossACollapsedFoldRemovesTheHiddenLines);
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
  AddTest(tests, "EditorEdgeCase/SortLinesDescending",
          TestSortLinesDescending);
  AddTest(tests, "EditorEdgeCase/SortLinesTwoDisjointRegions",
          TestSortLinesTwoDisjointRegions);
  AddTest(tests, "EditorEdgeCase/ToggleLineCommentMixedRegionCommentsAll",
          TestToggleLineCommentMixedRegionCommentsAll);
  AddTest(tests, "EditorEdgeCase/CopyLinesUpWithAdjacentCarets",
          TestCopyLinesUpWithAdjacentCarets);
  AddTest(tests, "EditorEdgeCase/BoxSelectionAcrossATabKeepsTheRectangle",
          TestBoxSelectionAcrossATabKeepsTheRectangle);
  AddTest(tests, "EditorEdgeCase/BoxSelectionTypingUndoRestores",
          TestBoxSelectionTypingUndoRestores);
  AddTest(tests, "EditorEdgeCase/OutdentMixedTabAndSpaces",
          TestOutdentMixedTabAndSpaces);
  AddTest(tests, "EditorEdgeCase/SelectWordAtCursorOnWhitespaceRun",
          TestSelectWordAtCursorOnWhitespaceRun);
  AddTest(tests, "EditorEdgeCase/DeleteWordForwardAtLineEndJoins",
          TestDeleteWordForwardAtLineEndJoins);
  AddTest(tests, "EditorEdgeCase/SelectWordAtCursorAtWordEnd", TestSelectWordAtCursorAtWordEnd);
  AddTest(tests, "EditorEdgeCase/SelectWordAtCursorAtLineEnd", TestSelectWordAtCursorAtLineEnd);
  AddTest(tests, "EditorEdgeCase/SelectWordAtCursorOnSeparatorRun", TestSelectWordAtCursorOnSeparatorRun);
}

}  // namespace microide::tests
