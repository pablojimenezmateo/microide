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

  viewport.MoveCursorVertical(-1);  // primary off the top -> (0,0); secondary -> (0,2)
  viewport.MoveCursorVertical(-1);  // secondary off the top too -> (0,0)
  Expect(viewport.secondary_carets().empty(),
         std::string("carets driven onto the same position must merge: ") + CaretDump(viewport));
  Expect(viewport.cursor_line() == 0 && viewport.cursor_column() == 0,
         "primary should sit on the document start");
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


// A multi-caret CUT must put on the clipboard exactly what it removes. The
// aggregate copy path only fires when every caret has a non-empty selection, so
// N BARE carets fell through to the primary caret's line -- and then deleted all
// N. The other N-1 lines were destroyed and never reached the clipboard.
void TestMultiCaretBareCaretsCopyEveryCaretsLine() {
  TextViewport viewport;
  viewport.LoadContent("alpha\nbeta\ngamma\ndelta\n", "/tmp/ec-mc-line-copy.txt");
  viewport.SetViewportSize(10, 40);
  viewport.MoveCursorTo(2, 1);            // primary BELOW the secondary
  viewport.SetSecondaryCarets({{0, 1}});  // so document order is not caret order

  const auto text = viewport.MultiCaretLineTextForClipboard();
  Expect(text.has_value(), "two bare carets should produce a line aggregate");
  Expect(*text == "alpha\ngamma\n",
         std::string("both lines, in document order, each with its terminator, got: ") + *text);
}

// The invariant behind the fix: the text the cut captures is exactly the text the
// cut removes, for whatever caret arrangement.
void TestMultiCaretLineCutCapturesWhatItDeletes() {
  TextViewport viewport;
  viewport.LoadContent("alpha\nbeta\ngamma\ndelta\nepsilon\n", "/tmp/ec-mc-line-cut.txt");
  viewport.SetViewportSize(10, 40);
  viewport.MoveCursorTo(1, 0);
  viewport.SetSecondaryCarets({{3, 0}});

  const auto captured = viewport.MultiCaretLineTextForClipboard();
  Expect(captured.has_value(), "the cut captures a line aggregate");
  const std::string before = JoinLines(viewport);
  Expect(viewport.DeleteCurrentLine(), "the cut deletes");
  const std::string after = JoinLines(viewport);
  Expect(after == "alpha\ngamma\nepsilon\n",
         std::string("only the caret lines go, got: ") + after);

  // Every line that disappeared is on the clipboard, and nothing else is.
  std::string removed;
  for (const std::string& line : {std::string("beta"), std::string("delta")}) {
    removed += line;
    removed.push_back('\n');
  }
  Expect(*captured == removed,
         std::string("captured text equals the removed lines, got: ") + *captured);
  Expect(before.size() > after.size(), "the buffer actually shrank");
}

// A caret on a collapsed fold's opener names the whole block -- the rule the
// single-caret path already followed and the multi-caret path did not, so
// Ctrl+X with two carets deleted a fold's opener and orphaned its body.
void TestMultiCaretLineVerbsExpandACollapsedFold() {
  TextViewport viewport;
  viewport.LoadContent("head {\n  body\n}\ntail\nlast\n", "/tmp/ec-mc-fold-line.txt");
  viewport.SetViewportSize(10, 40);
  FoldingModel folding;
  Expect(folding.Compute(viewport.lines().Snapshot(), CStyleFoldOptions()),
         "the fold fixture should compute");
  Expect(folding.Collapse(0), "the brace fold at line 0 should collapse");
  viewport.SetFoldingModel(&folding);
  Expect(viewport.CollapsedFoldEndAt(0) == 2, "the fold at line 0 covers lines 0..2");

  viewport.MoveCursorTo(0, 0);            // on the collapsed fold's opener
  viewport.SetSecondaryCarets({{4, 0}});  // and a plain line further down

  const auto captured = viewport.MultiCaretLineTextForClipboard();
  Expect(captured.has_value() && *captured == "head {\n  body\n}\nlast\n",
         std::string("the whole block is captured, not just the opener, got: ") +
             (captured.has_value() ? *captured : std::string("<none>")));

  Expect(viewport.DeleteCurrentLine(), "the multi-caret line delete applies");
  Expect(JoinLines(viewport) == "tail\n",
         std::string("the fold goes whole and the body is not orphaned, got: ") +
             JoinLines(viewport));
}

// One caret is unchanged: the single-caret path still answers, so the line-paste
// marker and every existing behaviour keep working.
void TestSingleCaretLineClipboardIsUnchanged() {
  TextViewport viewport;
  viewport.LoadContent("alpha\nbeta\n", "/tmp/ec-single-line-copy.txt");
  viewport.SetViewportSize(10, 40);
  viewport.MoveCursorTo(1, 2);
  Expect(!viewport.MultiCaretLineTextForClipboard().has_value(),
         "one caret has no multi-caret line aggregate");
  Expect(viewport.CurrentLineTextForClipboard() == "beta\n",
         "the single-caret path still takes the caret's own line");
}


// Page up/down is a vertical move of `visible_lines - 1`, so under wrap it must
// step VISUAL rows and carry every caret -- the same contract the single-step
// move has. Page delegates to MoveCursorVertical for exactly that reason; this
// pins that it keeps delegating rather than growing its own line arithmetic.
void TestMultiCaretPageUnderWrapStepsVisualRows() {
  TextViewport viewport;
  // Each line wraps into two rows at width 8.
  viewport.LoadContent("aaaaaaaaaaaa\nbbbbbbbbbbbb\ncccccccccccc\ndddddddddddd\n",
                       "/tmp/ec-mc-page-wrap.txt");
  viewport.SetSoftWrap(true);
  viewport.SetViewportSize(4, 8);  // 4 visible rows -> a page is 3 rows
  viewport.MoveCursorTo(0, 0);
  viewport.SetSecondaryCarets({{1, 0}});

  const std::size_t primary_line_before = viewport.cursor_line();
  viewport.Page(1);

  // Three visual rows at two rows per line: line +1 and onto its second row, i.e.
  // the caret's line advances by at least one and the row offset moved.
  Expect(viewport.cursor_line() > primary_line_before,
         "the primary caret advanced past its starting line, got line " +
             std::to_string(viewport.cursor_line()));
  Expect(viewport.VisualRowForLine(viewport.cursor_line()) >=
             viewport.VisualRowForLine(primary_line_before) + 2,
         "it advanced by visual rows, not by one logical line");
  Expect(viewport.secondary_carets().size() == 1,
         "the secondary caret survives the page move");
  // The secondary started one logical line (two visual rows) below the primary and
  // must still be there: both moved by the same number of rows.
  Expect(viewport.secondary_carets().front().line == viewport.cursor_line() + 1,
         "both carets moved by the same number of rows");
}

// Paging past the end collapses the carets onto the last row, and the coincident
// ones dedupe rather than piling up -- N carets on one position would apply an
// edit N times there.
void TestMultiCaretPageAtBufferEndDedupes() {
  TextViewport viewport;
  viewport.LoadContent("a\nb\nc\n", "/tmp/ec-mc-page-end.txt");
  viewport.SetViewportSize(4, 40);
  viewport.MoveCursorTo(0, 0);
  viewport.SetSecondaryCarets({{1, 0}});

  viewport.Page(1);
  viewport.Page(1);

  const std::size_t total = viewport.secondary_carets().size() + 1;
  Expect(total <= 2, "carets driven onto the same row collapse, got " + std::to_string(total));
  for (const TextPosition& caret : viewport.secondary_carets()) {
    Expect(!(caret.line == viewport.cursor_line() && caret.column == viewport.cursor_column()),
           "no secondary caret sits exactly on the primary");
  }
}

// Select-all then type replaces the whole buffer and leaves ONE caret, with the
// wrap table rebuilt for the much shorter document -- a stale row table here puts
// the caret on a row that no longer exists.
void TestSelectAllThenTypeUnderWrapLeavesOneCaret() {
  TextViewport viewport;
  viewport.LoadContent("aaaaaaaaaaaa\nbbbbbbbbbbbb\ncccccccccccc\n", "/tmp/ec-wrap-selectall.txt");
  viewport.SetSoftWrap(true);
  viewport.SetViewportSize(6, 8);
  viewport.MoveCursorTo(0, 0);
  viewport.SetSecondaryCarets({{1, 0}});

  viewport.SelectAll();
  viewport.InsertText("x");

  Expect(JoinLines(viewport) == "x", std::string("the buffer is replaced, got: ") + JoinLines(viewport));
  Expect(viewport.secondary_carets().empty(), "select-all collapses to a single caret");
  Expect(viewport.cursor_line() == 0 && viewport.cursor_column() == 1,
         "the caret lands after the typed character");
  Expect(viewport.VisualRowForLine(0) == 0 && viewport.VisualRowCount() == 1,
         "the wrap table is rebuilt for the one-line document, rows=" +
             std::to_string(viewport.VisualRowCount()));
}

// Every shaping verb in ShapingActions.cpp unions the caret set through
// ResolveLineRanges -- except ToggleBlockComment, which read only the primary. So
// three cursors wrapped ONE line in `/* */` and left the other two untouched,
// while Ctrl+/ next to it commented all three. VS Code toggles each cursor's
// region independently.
//
// Found by classifying every registered action by how many caret neighbourhoods
// it changed; it was the only verb whose count came back 1.
void TestToggleBlockCommentReachesEveryCaret() {
  TextViewport viewport;
  viewport.LoadContent("aaa();\nbbb();\nccc();\nddd();\n", "/tmp/ec-block-multi.cpp");
  viewport.SetViewportSize(10, 40);
  viewport.MoveCursorTo(0, 1);
  viewport.SetSecondaryCarets({{2, 1}});

  Expect(microide::editor::ToggleBlockComment(viewport, "/*", "*/"),
         "the toggle should apply");
  Expect(JoinLines(viewport) == "/* aaa(); */\nbbb();\n/* ccc(); */\nddd();\n",
         std::string("both carets' lines should be wrapped, got: ") + JoinLines(viewport));

  // And it is still its own inverse at every caret -- the property the invariant
  // sweep checks, now over more than one of them.
  Expect(microide::editor::ToggleBlockComment(viewport, "/*", "*/"),
         "the second toggle should apply");
  Expect(JoinLines(viewport) == "aaa();\nbbb();\nccc();\nddd();\n",
         std::string("toggling twice should restore the buffer, got: ") + JoinLines(viewport));

  // One undo step for the pair of regions, not one per region.
  Expect(viewport.Undo(), "the multi-region toggle should undo");
  Expect(JoinLines(viewport) == "/* aaa(); */\nbbb();\n/* ccc(); */\nddd();\n",
         std::string("one undo should take back the whole toggle, got: ") + JoinLines(viewport));
}

// Two carets on ONE line must wrap it once, not nest a second pair of markers
// inside the first.
void TestToggleBlockCommentWithTwoCaretsOnOneLine() {
  TextViewport viewport;
  viewport.LoadContent("alpha bravo\nsecond\n", "/tmp/ec-block-same-line.cpp");
  viewport.SetViewportSize(10, 40);
  viewport.MoveCursorTo(0, 1);
  viewport.SetSecondaryCarets({{0, 8}});

  Expect(microide::editor::ToggleBlockComment(viewport, "/*", "*/"), "the toggle applies");
  Expect(JoinLines(viewport) == "/* alpha bravo */\nsecond\n",
         std::string("one line, one wrap: ") + JoinLines(viewport));
}

// A whole-line drag selects N lines by ending at column 0 of line N+1 -- that is
// what `RangeForCaret` normalizes for every LINE op ("a whole-line drag selects N
// lines rather than N+1"). ToggleBlockComment never did, so wrapping three
// dragged lines put the closing marker at the START of the fourth, joining a line
// the user never selected:
//
//     /* sel1();          instead of      /* sel1();
//     sel2();                             sel2();
//     sel3();                             sel3(); */
//      */keepB();                         keepB();
//
// VS Code shrinks such a selection before wrapping. Found by classifying every
// action by how many lines of a multi-line selection it touched: this was the one
// that reported touching a line OUTSIDE the selection.
void TestToggleBlockCommentOnAWholeLineSelection() {
  TextViewport viewport;
  viewport.LoadContent("keepA();\nsel1();\nsel2();\nsel3();\nkeepB();\n",
                       "/tmp/ec-block-whole-lines.cpp");
  viewport.SetViewportSize(10, 40);
  viewport.MoveCursorTo(1, 0);
  viewport.MoveCursorTo(4, 0, /*extend_selection=*/true);

  Expect(microide::editor::ToggleBlockComment(viewport, "/*", "*/"), "the toggle applies");
  Expect(JoinLines(viewport) ==
             "keepA();\n/* sel1();\nsel2();\nsel3(); */\nkeepB();\n",
         std::string("the close marker belongs on the last SELECTED line: ") +
             JoinLines(viewport));

  Expect(microide::editor::ToggleBlockComment(viewport, "/*", "*/"), "the second toggle applies");
  Expect(JoinLines(viewport) == "keepA();\nsel1();\nsel2();\nsel3();\nkeepB();\n",
         std::string("and toggling twice restores it: ") + JoinLines(viewport));
}

// VS Code's editor.action.joinLines. A bare caret joins its line with the one
// BELOW -- that is what makes a repeated press pull a block up a line at a time --
// and a selection joins every line it touches. Separated by ONE space, with the
// appended lines' leading whitespace trimmed.
void TestJoinLinesBareCaretTakesTheLineBelow() {
  TextViewport viewport;
  viewport.LoadContent("alpha\n    beta\ngamma\n", "/tmp/ec-join-below.txt");
  viewport.SetViewportSize(10, 40);
  viewport.MoveCursorTo(0, 2);

  Expect(microide::editor::JoinLinesAtCarets(viewport), "the join applies");
  Expect(JoinLines(viewport) == "alpha beta\ngamma\n",
         std::string("the line below is appended with one space, its indent trimmed: ") +
             JoinLines(viewport));

  Expect(microide::editor::JoinLinesAtCarets(viewport), "a second press applies");
  Expect(JoinLines(viewport) == "alpha beta gamma\n",
         std::string("and pulls the next line up too: ") + JoinLines(viewport));
}

// A line already ending in whitespace does not gain a second space, and an empty
// line contributes nothing rather than a stray one.
void TestJoinLinesSpacingEdges() {
  TextViewport viewport;
  viewport.LoadContent("alpha \nbeta\n", "/tmp/ec-join-space.txt");
  viewport.SetViewportSize(10, 40);
  viewport.MoveCursorTo(0, 0);
  Expect(microide::editor::JoinLinesAtCarets(viewport), "the join applies");
  Expect(JoinLines(viewport) == "alpha beta\n",
         std::string("no double space after a trailing one: ") + JoinLines(viewport));

  TextViewport blanks;
  blanks.LoadContent("alpha\n\n\nbeta\n", "/tmp/ec-join-blank.txt");
  blanks.SetViewportSize(10, 40);
  blanks.MoveCursorTo(0, 0);
  blanks.MoveCursorTo(3, 4, /*extend_selection=*/true);
  Expect(microide::editor::JoinLinesAtCarets(blanks), "the join applies over blanks");
  Expect(JoinLines(blanks) == "alpha beta\n",
         std::string("empty lines contribute nothing, not stray spaces: ") + JoinLines(blanks));
}

// Every caret joins its own region, and two carets on adjacent lines join once.
void TestJoinLinesAtEveryCaret() {
  TextViewport viewport;
  viewport.LoadContent("a1\na2\nkeep\nb1\nb2\n", "/tmp/ec-join-multi.txt");
  viewport.SetViewportSize(10, 40);
  viewport.MoveCursorTo(0, 0);
  viewport.SetSecondaryCarets({{3, 0}});

  Expect(microide::editor::JoinLinesAtCarets(viewport), "the join applies");
  Expect(JoinLines(viewport) == "a1 a2\nkeep\nb1 b2\n",
         std::string("each caret joined its own pair: ") + JoinLines(viewport));
  Expect(viewport.secondary_caret_range_view().size() == 1,
         "both carets survive the join");
}

// The last line has nothing below it, so the press is a no-op rather than an
// edit that eats the trailing newline.
void TestJoinLinesAtTheLastLineIsANoOp() {
  TextViewport viewport;
  viewport.LoadContent("only\n", "/tmp/ec-join-last.txt");
  viewport.SetViewportSize(10, 40);
  viewport.MoveCursorTo(1, 0);  // the phantom line after the final newline
  Expect(!microide::editor::JoinLinesAtCarets(viewport), "there is nothing below to join");
  Expect(JoinLines(viewport) == "only\n", "and the buffer is untouched");
}

// --- Up on the first visual row goes to that row's START, Down on the last
// visual row goes to that row's END (VS Code's MoveOperations.up/down with
// allowMoveOnFirstLine / allowMoveOnLastLine). Without it the edge key is a
// silent no-op, which is also the one place a caret can never reach column 0 of
// the document by vertical motion alone. Per caret: the primary clamps to the
// edge while a secondary a row further in still steps normally. ---
void TestUpOnTheFirstRowGoesToRowStartPerCaret() {
  TextViewport viewport;
  viewport.LoadContent("hello\nworld\n", "/tmp/ec-up-first-row.txt");
  viewport.SetViewportSize(10, 40);
  viewport.MoveCursorTo(0, 3);
  viewport.SetSecondaryCarets({{1, 3}});

  viewport.MoveCursorVertical(-1);
  Expect(viewport.cursor_line() == 0 && viewport.cursor_column() == 0,
         std::string("Up on the first row lands on its start: ") + CaretDump(viewport));
  Expect(viewport.secondary_carets().size() == 1 &&
             viewport.secondary_carets().front() == TextPosition{0, 3},
         std::string("the secondary caret still steps one row: ") + CaretDump(viewport));

  viewport.MoveCursorVertical(-1);
  Expect(viewport.cursor_line() == 0 && viewport.cursor_column() == 0,
         "a second Up stays put");
  Expect(viewport.secondary_carets().empty(),
         std::string("the secondary caret reaches (0,0) too and merges: ") + CaretDump(viewport));
}

void TestDownOnTheLastRowGoesToRowEndPerCaret() {
  TextViewport viewport;
  viewport.LoadContent("hello\nworld", "/tmp/ec-down-last-row.txt");
  viewport.SetViewportSize(10, 40);
  Expect(viewport.lines().size() == 2, "fixture: no phantom line after the last one");
  viewport.MoveCursorTo(1, 2);
  viewport.SetSecondaryCarets({{0, 2}});

  viewport.MoveCursorVertical(1);
  Expect(viewport.cursor_line() == 1 && viewport.cursor_column() == 5,
         std::string("Down on the last row lands on its end: ") + CaretDump(viewport));
  Expect(viewport.secondary_carets().size() == 1 &&
             viewport.secondary_carets().front() == TextPosition{1, 2},
         std::string("the secondary caret still steps one row: ") + CaretDump(viewport));
  Expect(!viewport.has_selection(), "a plain move leaves no selection");
}

// Shift+Up at the edge extends the selection to the row start (VS Code).
void TestShiftUpOnTheFirstRowExtendsToRowStart() {
  TextViewport viewport;
  viewport.LoadContent("hello\n", "/tmp/ec-shift-up-first-row.txt");
  viewport.SetViewportSize(10, 40);
  viewport.MoveCursorTo(0, 3);
  viewport.MoveCursorVertical(-1, /*extend_selection=*/true);
  const auto range = viewport.selection_range();
  Expect(range.has_value() && range->start == TextPosition{0, 0} &&
             range->end == TextPosition{0, 3},
         std::string("Shift+Up on the first row selects back to column 0: ") + CaretDump(viewport));
}

// Under soft wrap "the first row" is the first VISUAL row: Up from the second
// wrapped row still climbs onto the first, and only then snaps to column 0.
// Symmetrically, Down on the last wrapped row of the last line goes to the
// line end, which is that row's end.
void TestUpDownAtTheEdgeUnderWrapUsesVisualRows() {
  TextViewport viewport;
  viewport.LoadContent("abcdefghijklmnop", "/tmp/ec-wrap-edge-rows.txt");
  viewport.SetViewportSize(10, /*visible_columns=*/8);
  viewport.SetSoftWrap(true);
  Expect(viewport.visual_line_count() == 2, "fixture: the one line wraps into two rows");
  viewport.MoveCursorTo(0, 12);  // second row

  viewport.MoveCursorVertical(-1);
  // The sticky column is the ON-SCREEN column (4 into the row), as in VS Code.
  Expect(viewport.cursor_visual_row() == 0 && viewport.cursor_column() == 4,
         "Up from the second row climbs onto the first under the same screen column, got column " +
             std::to_string(viewport.cursor_column()));
  viewport.MoveCursorVertical(-1);
  Expect(viewport.cursor_column() == 0,
         "Up on the first row snaps to the row start, got column " +
             std::to_string(viewport.cursor_column()));

  viewport.MoveCursorTo(0, 3);  // first row
  viewport.MoveCursorVertical(1);
  Expect(viewport.cursor_visual_row() == 1 && viewport.cursor_column() == 11,
         "Down from the first row steps onto the second, got column " +
             std::to_string(viewport.cursor_column()));
  viewport.MoveCursorVertical(1);
  Expect(viewport.cursor_column() == 16,
         "Down on the last row goes to its end, got column " +
             std::to_string(viewport.cursor_column()));
}

// --- A plain (non-extending) Up/Down over a selection moves relative to the
// selection's START for Up and its END for Down (VS Code's MoveOperations
// .moveUp/.moveDown: "if we are not in selection mode, move acts relative to
// the beginning/end of selection"), not relative to the caret. With a
// reversed selection (caret at the start) Down therefore starts from the far
// end. Per caret. ---
void TestPlainVerticalMoveOverASelectionStartsFromItsEdge() {
  TextViewport viewport;
  viewport.LoadContent("aaaa\nbbbb\ncccc\ndddd\neeee\n", "/tmp/ec-vertical-from-edge.txt");
  viewport.SetViewportSize(10, 40);
  // Primary: reversed selection (2,2)->(0,2), caret at the start.
  viewport.MoveCursorTo(2, 2);
  viewport.MoveCursorTo(0, 2, /*extend_selection=*/true);
  // Secondary: forward selection (3,1)->(4,1), caret at the end.
  viewport.SetSecondaryCaretsWithRanges(std::vector<SelectionRange>{
      SelectionRange{TextPosition{3, 1}, TextPosition{4, 1}},
  });

  viewport.MoveCursorVertical(1);
  Expect(!viewport.has_selection(), "a plain Down collapses the selection");
  Expect(viewport.cursor_line() == 3 && viewport.cursor_column() == 2,
         std::string("Down starts from the selection END (2,2), so it lands on line 3: ") +
             CaretDump(viewport));
  // The secondary's caret already sits at its selection's end (4,1); one row
  // down is the phantom line after the final newline, column 0.
  Expect(viewport.secondary_carets().size() == 1 &&
             viewport.secondary_carets().front() == TextPosition{5, 0},
         std::string("the secondary steps from its own selection end: ") + CaretDump(viewport));
}

void TestPlainUpOverAForwardSelectionStartsFromItsStart() {
  TextViewport viewport;
  viewport.LoadContent("aaaa\nbbbb\ncccc\ndddd\n", "/tmp/ec-up-from-start.txt");
  viewport.SetViewportSize(10, 40);
  viewport.MoveCursorTo(1, 2);
  viewport.MoveCursorTo(3, 2, /*extend_selection=*/true);  // caret at the end

  viewport.MoveCursorVertical(-1);
  Expect(!viewport.has_selection(), "a plain Up collapses the selection");
  Expect(viewport.cursor_line() == 0 && viewport.cursor_column() == 2,
         std::string("Up starts from the selection START (1,2), so it lands on line 0: ") +
             CaretDump(viewport));
}

// --- A reversed secondary selection whose caret lands exactly on the primary's
// caret, with the two selections merely TOUCHING there, must survive: VS Code
// keeps two cursors at one position when neither selection is collapsed. The
// position-only dedupe used to eat the secondary, and with it the text it had
// selected. Reachable with an Alt+drag leftwards followed by Ctrl+Shift+Right. ---
void TestTouchingReversedSelectionAtThePrimaryCaretSurvives() {
  TextViewport viewport;
  viewport.LoadContent("alpha beta gamma delta\n", "/tmp/ec-touch-at-primary.txt");
  viewport.SetViewportSize(10, 40);
  viewport.MoveCursorTo(0, 0);
  viewport.MoveCursorTo(0, 5, /*extend_selection=*/true);  // "alpha" selected, caret at 5
  // Secondary: anchor 16, caret 7 -- "ta gam" selected leftwards.
  viewport.SetSecondaryCaretsWithRanges(std::vector<SelectionRange>{
      SelectionRange{TextPosition{0, 16}, TextPosition{0, 7}},
  });
  Expect(viewport.secondary_carets().size() == 1, "fixture: the reversed secondary exists");

  viewport.MoveCursorWord(1, /*extend_selection=*/true);  // both carets land on 10
  Expect(viewport.cursor_column() == 10, "primary word-steps to the end of 'beta'");
  const auto ranges = viewport.secondary_caret_ranges();
  Expect(ranges.size() == 1 && ranges[0].position == TextPosition{0, 10} &&
             ranges[0].selection_anchor == std::optional<TextPosition>(TextPosition{0, 16}),
         std::string("the secondary shares the primary's position but keeps its own selection: ") +
             CaretDump(viewport));

  viewport.InsertText("X");
  Expect(JoinLines(viewport) == "XX delta\n",
         std::string("both touching selections are replaced: ") + JoinLines(viewport));
}

// --- Tab at two carets that share a position but own different (touching)
// selections must pad BOTH sites. The soft tab used to be planned as one string
// per position-deduped caret and handed to the pipeline by index; the pipeline
// dedupes by (position, selection), so the two lists disagreed in length and the
// fallback inserted the empty string at every caret -- Tab did nothing. ---
void TestMultiCaretTabAtASharedPositionPadsBothSites() {
  TextViewport viewport;
  viewport.LoadContent("alpha beta gamma delta\n", "/tmp/ec-tab-shared-position.txt");
  viewport.SetViewportSize(10, 40);
  viewport.SetSoftTabs(true);
  viewport.SetIndentWidth(4);
  viewport.MoveCursorTo(0, 0);
  viewport.MoveCursorTo(0, 5, /*extend_selection=*/true);  // "alpha", leading right
  viewport.SetSecondaryCaretsWithRanges(std::vector<SelectionRange>{
      SelectionRange{TextPosition{0, 16}, TextPosition{0, 7}},  // "ta gam", leading left
  });
  viewport.MoveCursorWord(1, /*extend_selection=*/true);  // both carets word-step to 10
  Expect(viewport.secondary_carets().size() == 1 && viewport.cursor_column() == 10 &&
             viewport.secondary_carets().front() == TextPosition{0, 10},
         std::string("fixture: two carets share column 10: ") + CaretDump(viewport));

  viewport.InsertTab();
  // [0,10) starts at visual column 0 -> a full indent; [10,16) starts at 10 -> 2.
  Expect(JoinLines(viewport) == "      " + std::string(" delta\n"),
         std::string("each selection is replaced by its own soft tab: '") + JoinLines(viewport) +
             "'");
}

// Tab over single-line selections at several carets replaces each with an
// indent sized from ITS selection start (VS Code's _replaceJumpToNextIndent).
void TestMultiCaretTabReplacesSingleLineSelectionsFromTheirStart() {
  TextViewport viewport;
  viewport.LoadContent("abcdef\nabcdef\n", "/tmp/ec-tab-selections.txt");
  viewport.SetViewportSize(10, 40);
  viewport.SetSoftTabs(true);
  viewport.SetIndentWidth(4);
  viewport.MoveCursorTo(0, 1);
  viewport.MoveCursorTo(0, 3, /*extend_selection=*/true);
  viewport.SetSecondaryCaretsWithRanges(std::vector<SelectionRange>{
      SelectionRange{TextPosition{1, 2}, TextPosition{1, 5}},
  });

  viewport.InsertTab();
  Expect(JoinLines(viewport) == "a   def\nab  f\n",
         std::string("column 1 pads 3, column 2 pads 2: '") + JoinLines(viewport) + "'");
  Expect(viewport.cursor_column() == 4 && !viewport.has_selection(),
         std::string("the primary lands after its padding, collapsed: ") + CaretDump(viewport));
}

// --- What the Tab key does is decided by ClassifyTabKey (VS Code's
// TypeOperations.tab), per cursor. Three things the hand-copied checks got wrong:
// only the primary was consulted, so a secondary's multi-line selection was
// replaced by four spaces; a selection covering a whole line (Home, Shift+End)
// was treated as "single line", so Tab ate the line where VS Code shifts it; and
// the fix for the first of those decided ONE intent for every cursor, so a bare
// caret beside a block selection had its line shifted rather than indenting at
// its own column (TD-2026-09-19-295). That last case is kMixed. ---
void TestTabKeyClassifierIndentsWholeLineAndSecondarySelections() {
  using microide::editor::ClassifyTabKey;
  using microide::editor::TabKeyIntent;
  TextViewport viewport;
  viewport.LoadContent("  foo\nbar\nbaz\n", "/tmp/ec-tab-classify.txt");
  viewport.SetViewportSize(10, 40);

  viewport.MoveCursorTo(0, 2);
  Expect(ClassifyTabKey(viewport, false) == TabKeyIntent::kInsertTab,
         "a bare caret inserts a tab");
  Expect(ClassifyTabKey(viewport, true) == TabKeyIntent::kOutdent, "Shift always outdents");

  viewport.MoveCursorTo(0, 4, /*extend_selection=*/true);  // "fo": partial, single line
  Expect(ClassifyTabKey(viewport, false) == TabKeyIntent::kInsertTab,
         "a partial single-line selection is replaced by the indent");

  viewport.MoveCursorTo(0, 0);
  viewport.MoveCursorTo(0, 5, /*extend_selection=*/true);  // the whole line's content
  Expect(ClassifyTabKey(viewport, false) == TabKeyIntent::kIndentBlock,
         "a selection covering the whole line indents the line instead of eating it");

  viewport.MoveCursorTo(0, 2);  // bare primary again
  viewport.SetSecondaryCaretsWithRanges(std::vector<SelectionRange>{
      SelectionRange{TextPosition{1, 1}, TextPosition{2, 1}},
  });
  Expect(ClassifyTabKey(viewport, false) == TabKeyIntent::kMixed,
         "a bare primary beside a secondary's multi-line selection is a mixed set: one "
         "shifts lines, the other inserts at its own column");
  viewport.MoveCursorTo(1, 0);
  viewport.MoveCursorTo(1, 3, /*extend_selection=*/true);  // the whole of "bar"
  viewport.SetSecondaryCaretsWithRanges(std::vector<SelectionRange>{
      SelectionRange{TextPosition{2, 0}, TextPosition{2, 3}},
  });
  Expect(ClassifyTabKey(viewport, false) == TabKeyIntent::kIndentBlock,
         "when every cursor is a block the whole-set applier still runs");
  viewport.MoveCursorTo(0, 2);  // bare primary again
  viewport.SetSecondaryCaretsWithRanges(std::vector<SelectionRange>{
      SelectionRange{TextPosition{1, 2}, TextPosition{1, 1}},  // reversed, partial
  });
  Expect(ClassifyTabKey(viewport, false) == TabKeyIntent::kInsertTab,
         "a secondary's partial single-line selection still inserts");
}

// Tab decides PER CURSOR, as VS Code's TypeOperations.tab does: a cursor whose
// selection covers a whole line (or spans lines) shifts those lines, and a bare
// cursor beside it inserts at its OWN column, both in one edit and one undo
// entry.
//
//   "aaaa" selected whole + a bare caret at (2,2) in "cccc"
//     "    aaaa" / "bbbb" / "cc  cc"
//
// This used to resolve ONE intent for the whole set, so the bare caret's line was
// shifted instead (TD-2026-09-19-295). The whole-set rule was itself the fix for
// something worse — a per-PRIMARY check that silently replaced a secondary's
// multi-line selection with four spaces — so what the applier must never do is
// destroy a selection, which the round trip below is here to say.
void TestTabKeyOnAMixedCaretSetIndentsPerCursor() {
  TextViewport viewport;
  viewport.SetViewportSize(20, 200);
  viewport.SetSoftTabs(true);
  viewport.SetIndentWidth(4);
  viewport.LoadContent("aaaa\nbbbb\ncccc\n", "/tmp/ec-tab-mixed.cpp");
  viewport.MoveCursorTo(0, 0);
  viewport.MoveCursorTo(0, 4, /*extend_selection=*/true);  // the whole of line 0
  viewport.AddSecondaryCaret(2, 2);                        // a BARE caret mid-line 2

  Expect(microide::editor::ClassifyTabKey(viewport, false) ==
             microide::editor::TabKeyIntent::kMixed,
         "a block selection beside a bare caret is a mixed set");
  Expect(microide::editor::ApplyMixedTabKey(viewport), "the mixed apply should change the buffer");
  const auto joined = [&viewport]() {
    return std::string(viewport.lines()[0]) + "/" + std::string(viewport.lines()[1]) + "/" +
           std::string(viewport.lines()[2]);
  };
  Expect(joined() == "    aaaa/bbbb/cc  cc",
         "the block selection shifts its line and the bare caret indents at its own column: " +
             joined());

  // The block cursor keeps its selection, shifted by what its line gained; the
  // bare caret lands just past what it inserted.
  const auto selection = viewport.selection_range();
  Expect(selection.has_value() && selection->start.line == 0 && selection->start.column == 4 &&
             selection->end.line == 0 && selection->end.column == 8,
         "the block cursor's selection should still cover 'aaaa' after the shift");
  const auto secondaries = viewport.secondary_caret_range_view();
  Expect(secondaries.size() == 1 && secondaries[0].position.line == 2 &&
             secondaries[0].position.column == 4,
         "the bare caret should sit just past the indent it inserted");

  // One undo entry, not two: the block half and the per-site half are one edit.
  viewport.Undo();
  Expect(joined() == "aaaa/bbbb/cccc", "one undo should take the whole mixed edit back: " + joined());
}

// A mixed set at the scale the gesture actually reaches: an
// add-cursor-at-all-matches run can put hundreds of carets in the set while one
// of them selects hundreds of lines. Both halves are unbounded on the same
// gesture, which is why the caret remap indexes the edits by line instead of
// scanning all of them per caret.
void TestTabKeyOnALargeMixedCaretSetStaysCorrect() {
  TextViewport viewport;
  viewport.SetViewportSize(20, 200);
  viewport.SetSoftTabs(true);
  viewport.SetIndentWidth(2);
  std::string content;
  constexpr std::size_t kLines = 400;
  for (std::size_t i = 0; i < kLines; ++i) {
    content += "xxxx\n";
  }
  viewport.LoadContent(content, "/tmp/ec-tab-mixed-large.cpp");

  // A block selection over the first half, plus a bare caret mid-line on every
  // line of the second half.
  viewport.MoveCursorTo(0, 0);
  viewport.MoveCursorTo(kLines / 2, 0, /*extend_selection=*/true);
  std::vector<SelectionRange> secondaries;
  for (std::size_t line = kLines / 2; line < kLines; ++line) {
    secondaries.push_back(
        SelectionRange{TextPosition{line, 2}, TextPosition{line, 2}});
  }
  viewport.SetSecondaryCaretsWithRanges(secondaries);

  Expect(microide::editor::ClassifyTabKey(viewport, false) ==
             microide::editor::TabKeyIntent::kMixed,
         "a block selection plus hundreds of bare carets is a mixed set");
  Expect(microide::editor::ApplyMixedTabKey(viewport), "the mixed apply should change the buffer");

  // The block half shifted; the point half inserted at its own column.
  for (std::size_t line = 0; line < kLines / 2; ++line) {
    Expect(std::string(viewport.lines()[line]) == "  xxxx",
           "every line of the block selection should gain one indent");
  }
  for (std::size_t line = kLines / 2; line < kLines; ++line) {
    Expect(std::string(viewport.lines()[line]) == "xx  xx",
           "every bare caret should indent at its own column");
  }
  // Every caret survived, and each landed just past what it inserted.
  Expect(viewport.secondary_caret_range_view().size() == kLines / 2,
         "no caret should be dropped by the mixed apply");
  for (const auto& secondary : viewport.secondary_caret_range_view()) {
    Expect(secondary.position.column == 4,
           "each bare caret should sit just past the two spaces it inserted");
  }
  viewport.Undo();
  Expect(std::string(viewport.lines()[0]) == "xxxx" &&
             std::string(viewport.lines()[kLines - 1]) == "xxxx",
         "one undo should take the whole mixed edit back at scale too");
}

// The mixed applier must never do what the per-primary check it descends from did:
// replace a secondary's multi-line selection with an indent unit.
void TestTabKeyOnAMixedCaretSetNeverEatsASecondarySelection() {
  TextViewport viewport;
  viewport.SetViewportSize(20, 200);
  viewport.SetSoftTabs(true);
  viewport.SetIndentWidth(2);
  viewport.LoadContent("one\ntwo\nthree\nfour\n", "/tmp/ec-tab-mixed-secondary.cpp");
  viewport.MoveCursorTo(0, 1);  // a bare PRIMARY caret, mid-line
  // A secondary whose selection spans lines 2-3.
  viewport.AddSecondaryCaretWithRange(SelectionRange{TextPosition{2, 0}, TextPosition{3, 4}});

  Expect(microide::editor::ClassifyTabKey(viewport, false) ==
             microide::editor::TabKeyIntent::kMixed,
         "a bare primary beside a multi-line secondary selection is a mixed set");
  Expect(microide::editor::ApplyMixedTabKey(viewport), "the mixed apply should change the buffer");
  const std::string text = std::string(viewport.lines()[0]) + "/" +
                           std::string(viewport.lines()[1]) + "/" +
                           std::string(viewport.lines()[2]) + "/" +
                           std::string(viewport.lines()[3]);
  Expect(text == "o ne/two/  three/  four",
         "the secondary's lines shift and the bare primary indents at its column: " + text);
}

// Two block sites covering one line must indent it ONCE.
void TestTabKeyOnAMixedCaretSetIndentsASharedLineOnce() {
  TextViewport viewport;
  viewport.SetViewportSize(20, 200);
  viewport.SetSoftTabs(true);
  viewport.SetIndentWidth(2);
  viewport.LoadContent("alpha\nbeta\ngamma\n", "/tmp/ec-tab-mixed-shared.cpp");
  viewport.MoveCursorTo(0, 0);
  viewport.MoveCursorTo(1, 0, /*extend_selection=*/true);  // lines 0-1 (end col 0 drops line 1)
  viewport.AddSecondaryCaretWithRange(SelectionRange{TextPosition{0, 0}, TextPosition{0, 5}});
  viewport.AddSecondaryCaret(2, 3);

  Expect(microide::editor::ClassifyTabKey(viewport, false) ==
             microide::editor::TabKeyIntent::kMixed,
         "two block sites plus a bare caret is a mixed set");
  Expect(microide::editor::ApplyMixedTabKey(viewport), "the mixed apply should change the buffer");
  const std::string text = std::string(viewport.lines()[0]) + "/" +
                           std::string(viewport.lines()[1]) + "/" +
                           std::string(viewport.lines()[2]);
  Expect(text == "  alpha/beta/gam ma",
         "a line two block sites both cover gains one indent, not two: " + text);
}

// Hard tabs: the block half inserts a tab and the point half inserts a tab, so
// nothing about the mixed path depends on the soft-tab arithmetic.
void TestTabKeyOnAMixedCaretSetWithHardTabs() {
  TextViewport viewport;
  viewport.SetViewportSize(20, 200);
  viewport.SetSoftTabs(false);
  viewport.SetIndentWidth(4);
  viewport.LoadContent("aaaa\nbbbb\ncccc\n", "/tmp/ec-tab-mixed-hard.cpp");
  viewport.MoveCursorTo(0, 0);
  viewport.MoveCursorTo(0, 4, /*extend_selection=*/true);
  viewport.AddSecondaryCaret(2, 2);

  Expect(microide::editor::ApplyMixedTabKey(viewport), "the mixed apply should change the buffer");
  const std::string text = std::string(viewport.lines()[0]) + "/" +
                           std::string(viewport.lines()[1]) + "/" +
                           std::string(viewport.lines()[2]);
  Expect(text == "\taaaa/bbbb/cc\tcc", "hard tabs shift and insert one tab each: " + text);
}

}  // namespace

void RegisterEditorEdgeCaseTests(std::vector<TestCase>& tests) {
  AddTest(tests, "EditorEdgeCase/TabKeyOnAMixedCaretSetIndentsPerCursor",
          TestTabKeyOnAMixedCaretSetIndentsPerCursor);
  AddTest(tests, "EditorEdgeCase/TabKeyOnALargeMixedCaretSetStaysCorrect",
          TestTabKeyOnALargeMixedCaretSetStaysCorrect);
  AddTest(tests, "EditorEdgeCase/TabKeyOnAMixedCaretSetNeverEatsASecondarySelection",
          TestTabKeyOnAMixedCaretSetNeverEatsASecondarySelection);
  AddTest(tests, "EditorEdgeCase/TabKeyOnAMixedCaretSetIndentsASharedLineOnce",
          TestTabKeyOnAMixedCaretSetIndentsASharedLineOnce);
  AddTest(tests, "EditorEdgeCase/TabKeyOnAMixedCaretSetWithHardTabs",
          TestTabKeyOnAMixedCaretSetWithHardTabs);
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
  AddTest(tests, "EditorEdgeCase/MultiCaretBareCaretsCopyEveryCaretsLine",
          TestMultiCaretBareCaretsCopyEveryCaretsLine);
  AddTest(tests, "EditorEdgeCase/MultiCaretLineCutCapturesWhatItDeletes",
          TestMultiCaretLineCutCapturesWhatItDeletes);
  AddTest(tests, "EditorEdgeCase/MultiCaretLineVerbsExpandACollapsedFold",
          TestMultiCaretLineVerbsExpandACollapsedFold);
  AddTest(tests, "EditorEdgeCase/SingleCaretLineClipboardIsUnchanged",
          TestSingleCaretLineClipboardIsUnchanged);
  AddTest(tests, "EditorEdgeCase/MultiCaretPageUnderWrapStepsVisualRows",
          TestMultiCaretPageUnderWrapStepsVisualRows);
  AddTest(tests, "EditorEdgeCase/MultiCaretPageAtBufferEndDedupes",
          TestMultiCaretPageAtBufferEndDedupes);
  AddTest(tests, "EditorEdgeCase/SelectAllThenTypeUnderWrapLeavesOneCaret",
          TestSelectAllThenTypeUnderWrapLeavesOneCaret);
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
  AddTest(tests, "EditorEdgeCase/JoinLinesBareCaretTakesTheLineBelow",
          TestJoinLinesBareCaretTakesTheLineBelow);
  AddTest(tests, "EditorEdgeCase/JoinLinesSpacingEdges", TestJoinLinesSpacingEdges);
  AddTest(tests, "EditorEdgeCase/JoinLinesAtEveryCaret", TestJoinLinesAtEveryCaret);
  AddTest(tests, "EditorEdgeCase/JoinLinesAtTheLastLineIsANoOp",
          TestJoinLinesAtTheLastLineIsANoOp);
  AddTest(tests, "EditorEdgeCase/ToggleBlockCommentOnAWholeLineSelection",
          TestToggleBlockCommentOnAWholeLineSelection);
  AddTest(tests, "EditorEdgeCase/ToggleBlockCommentReachesEveryCaret",
          TestToggleBlockCommentReachesEveryCaret);
  AddTest(tests, "EditorEdgeCase/ToggleBlockCommentWithTwoCaretsOnOneLine",
          TestToggleBlockCommentWithTwoCaretsOnOneLine);
  AddTest(tests, "EditorEdgeCase/UpOnTheFirstRowGoesToRowStartPerCaret",
          TestUpOnTheFirstRowGoesToRowStartPerCaret);
  AddTest(tests, "EditorEdgeCase/DownOnTheLastRowGoesToRowEndPerCaret",
          TestDownOnTheLastRowGoesToRowEndPerCaret);
  AddTest(tests, "EditorEdgeCase/ShiftUpOnTheFirstRowExtendsToRowStart",
          TestShiftUpOnTheFirstRowExtendsToRowStart);
  AddTest(tests, "EditorEdgeCase/UpDownAtTheEdgeUnderWrapUsesVisualRows",
          TestUpDownAtTheEdgeUnderWrapUsesVisualRows);
  AddTest(tests, "EditorEdgeCase/PlainVerticalMoveOverASelectionStartsFromItsEdge",
          TestPlainVerticalMoveOverASelectionStartsFromItsEdge);
  AddTest(tests, "EditorEdgeCase/PlainUpOverAForwardSelectionStartsFromItsStart",
          TestPlainUpOverAForwardSelectionStartsFromItsStart);
  AddTest(tests, "EditorEdgeCase/TouchingReversedSelectionAtThePrimaryCaretSurvives",
          TestTouchingReversedSelectionAtThePrimaryCaretSurvives);
  AddTest(tests, "EditorEdgeCase/MultiCaretTabAtASharedPositionPadsBothSites",
          TestMultiCaretTabAtASharedPositionPadsBothSites);
  AddTest(tests, "EditorEdgeCase/MultiCaretTabReplacesSingleLineSelectionsFromTheirStart",
          TestMultiCaretTabReplacesSingleLineSelectionsFromTheirStart);
  AddTest(tests, "EditorEdgeCase/TabKeyClassifierIndentsWholeLineAndSecondarySelections",
          TestTabKeyClassifierIndentsWholeLineAndSecondarySelections);
}

}  // namespace microide::tests
