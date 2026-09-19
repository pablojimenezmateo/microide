// Soft-wrap vertical navigation, checked as a PROPERTY rather than against a
// fixture's expected caret positions.
//
// Wrapped rows are contiguous in visual columns, so the wrap point is one text
// position that TWO rows both answer for -- which is the shape that made Up out
// of a wrapped line do nothing (TD-2026-08-12) and that an affinity bit now
// resolves. A fixture test pins one caret; what actually has to hold is an
// algebraic statement about the whole document:
//
//   walk       pressing Down from the top row lands on visual row k after k
//              presses, for every k, until the last row -- no row skipped, none
//              visited twice, and no press that moves nothing before the end
//   round trip pressing Up the same number of times comes back to row 0, one row
//              per press
//   sticky     the walk holds from ANY starting column, including a column past
//              the end of some of the rows it crosses (the preferred column)
//   agreement  cursor_visual_row() and VisualRowForLine() do not disagree about
//              where a caret on a first row is
//
// These need no model of the expected column, so they cannot go stale when the
// wrap width, the hanging indent, or a language's wrap rule changes -- and every
// one of them fails loudly on the "two rows claim one position" bug class.

#include "TestSupport.h"

#include <string>
#include <vector>

#include "editor/EditTypes.h"
#include "editor/FoldingModel.h"
#include "editor/WrappedCaretRow.h"
#include "editor/TextViewport.h"

namespace microide::tests {
namespace {

using microide::editor::FoldingModel;
using microide::editor::WrapRowAffinity;
using microide::editor::WrappedRowPaintsCaret;
using microide::editor::TextPosition;
using microide::editor::TextViewport;

std::string CaretDump(const TextViewport& viewport) {
  return "(" + std::to_string(viewport.cursor_line()) + "," +
         std::to_string(viewport.cursor_column()) +
         ") row=" + std::to_string(viewport.cursor_visual_row());
}

// Documents whose wrapped-row structure differs in the ways the row arithmetic
// can get wrong: a line exactly the wrap width (does it wrap into an empty second
// row?), one a single cell over, empty lines between wrapped ones, a line with no
// break opportunity at all, wide glyphs that cannot be split mid-cluster, and a
// tab whose width is not its byte count.
struct WrapCase {
  const char* name;
  std::string content;
  // Whether this content is EXPECTED to occupy more visual rows than it has
  // logical lines at the test's viewport width. False for the deliberate
  // boundary case that lands exactly on the width and must NOT wrap into an
  // empty continuation row; every other case is a vacuity guard (see the walk).
  bool wraps = true;
};

std::vector<WrapCase> WrapCases() {
  return {
      {"exactly-the-wrap-width", "abcdefgh\nshort\nabcdefgh\n", /*wraps=*/false},
      {"one-cell-over", "abcdefghi\nshort\n"},
      {"many-rows", std::string(60, 'x') + "\nshort\n" + std::string(31, 'y') + "\n"},
      {"empty-lines-between", "\n" + std::string(20, 'z') + "\n\n\nshort\n"},
      {"spaces-so-it-breaks-on-words", "alpha bravo charlie delta echo foxtrot\nshort\n"},
      {"wide-glyphs", "你好你好你好你好你好\nshort\n"},
      {"combining-marks", "cafe\xcc\x81 cafe\xcc\x81 cafe\xcc\x81 cafe\xcc\x81\nshort\n"},
      {"tabs", "\t\tindented and long enough to wrap around\nshort\n"},
      {"trailing-empty-line", std::string(25, 'q') + "\n"},
      {"single-line-no-newline", std::string(25, 'w')},
  };
}

// One Down per visual row, from row 0 to the last one, then the same number of
// Ups. Run for every start column so the preferred-column path is covered too.
void WalkOneCase(const WrapCase& wrap_case, std::size_t start_column) {
  TextViewport viewport;
  viewport.LoadContent(wrap_case.content, "/tmp/wrap-walk.txt");
  viewport.SetViewportSize(6, /*visible_columns=*/8);
  viewport.SetSoftWrap(true);
  viewport.MoveCursorTo(0, start_column);

  const std::string where =
      std::string(wrap_case.name) + " @col" + std::to_string(start_column) + ": ";
  const std::size_t rows = viewport.visual_line_count();
  Expect(rows >= 1, where + "a loaded document has at least one visual row");
  // NOT asserted to be row 0: with word-boundary wrapping a column inside line 0
  // can already be on a continuation row, which is correct. The property is that
  // Down steps one row at a time from wherever the caret starts.
  const std::size_t first_row = viewport.cursor_visual_row();
  Expect(first_row < rows, where + "the start row is inside the document, got " +
                               CaretDump(viewport));

  for (std::size_t step = first_row + 1; step < rows; ++step) {
    viewport.MoveCursorVertical(1);
    Expect(viewport.cursor_visual_row() == step,
           where + "Down should land on visual row " + std::to_string(step) + ", got " +
               CaretDump(viewport));
  }

  // Past the last row Down is a no-op on the ROW (VS Code moves to the end of the
  // last row; either way the row must not advance and must not wrap to the top).
  const std::size_t last_row = viewport.cursor_visual_row();
  Expect(last_row == rows - 1,
         where + "the walk should have reached the last visual row " +
             std::to_string(rows - 1) + ", got " + CaretDump(viewport));
  viewport.MoveCursorVertical(1);
  Expect(viewport.cursor_visual_row() == last_row,
         where + "Down on the last visual row must stay there, got " + CaretDump(viewport));

  for (std::size_t step = rows - 1; step-- > 0;) {
    viewport.MoveCursorVertical(-1);
    Expect(viewport.cursor_visual_row() == step,
           where + "Up back to row " + std::to_string(step) + " landed on " +
               CaretDump(viewport));
  }
  Expect(viewport.cursor_visual_row() == 0,
         where + "the round trip should end on row 0, got " + CaretDump(viewport));
  viewport.MoveCursorVertical(-1);
  Expect(viewport.cursor_visual_row() == 0,
         where + "Up on the first visual row must stay there, got " + CaretDump(viewport));
}

void TestWrapDownWalkVisitsEveryVisualRowOnce() {
  // Vacuity guard: every property below is trivially true on a document that does
  // not wrap, and "does this content wrap at this width" is a fact about the
  // layout rules rather than about this file -- a wrap rule change could quietly
  // turn the whole suite into a walk down unwrapped lines. Count the cases that
  // actually produced more visual rows than logical lines.
  std::size_t wrapped_cases = 0;
  for (const WrapCase& wrap_case : WrapCases()) {
    TextViewport probe;
    probe.LoadContent(wrap_case.content, "/tmp/wrap-vacuity.txt");
    probe.SetViewportSize(6, /*visible_columns=*/8);
    probe.SetSoftWrap(true);
    const bool wrapped = probe.visual_line_count() > probe.line_count();
    Expect(wrapped == wrap_case.wraps,
           std::string(wrap_case.name) + ": expected wraps=" +
               (wrap_case.wraps ? "true" : "false") + " but got " +
               std::to_string(probe.visual_line_count()) + " visual rows for " +
               std::to_string(probe.line_count()) + " lines");
    if (wrapped) {
      ++wrapped_cases;
    }
    for (std::size_t column : {std::size_t{0}, std::size_t{3}, std::size_t{7}}) {
      WalkOneCase(wrap_case, column);
    }
  }
  Expect(wrapped_cases + 1 == WrapCases().size(),
         "every case but the exactly-the-width boundary must actually wrap, or the walk "
         "is a walk down unwrapped lines: only " +
             std::to_string(wrapped_cases) + " of " + std::to_string(WrapCases().size()) +
             " produced more visual rows than logical lines");
}

// The walk again, but every caret is a SECONDARY caret riding along with the
// primary. Each caret carries its OWN preferred column and affinity; one shared
// between them shows up here as a secondary that drifts off the column its own
// row put it on. Both lines are the same length and wrap identically, so after
// any number of Downs the two carets must sit at the same column.
void TestWrapWalkKeepsEverySecondaryCaretOnItsOwnRow() {
  const std::string long_line(30, 'a');
  TextViewport viewport;
  viewport.LoadContent(long_line + "\nshort\n" + long_line + "\n",
                       "/tmp/wrap-walk-multi.txt");
  viewport.SetViewportSize(6, /*visible_columns=*/8);
  viewport.SetSoftWrap(true);
  viewport.MoveCursorTo(0, 3);
  viewport.SetSecondaryCarets({{2, 3}});

  // Three Downs keeps both carets inside their own wrapped line (30 cells over an
  // 8-cell viewport is four rows), so neither crosses into a line of a different
  // shape and the two columns stay directly comparable.
  for (int step = 1; step <= 3; ++step) {
    viewport.MoveCursorVertical(1);
    Expect(viewport.cursor_line() == 0,
           "the primary should still be inside line 0's wrapped rows, got " +
               CaretDump(viewport));
    Expect(viewport.secondary_carets().size() == 1, "the secondary should survive the walk");
    const TextPosition secondary = viewport.secondary_carets().front();
    Expect(secondary.line == 2,
           "the secondary should still be inside line 2's wrapped rows, got line " +
               std::to_string(secondary.line));
    Expect(secondary.column == viewport.cursor_column(),
           "after Down #" + std::to_string(step) +
               " the secondary should sit at the same column as the primary (identical "
               "lines, identical wrap): primary " +
               std::to_string(viewport.cursor_column()) + " secondary " +
               std::to_string(secondary.column));
  }

  // And back: the pair must return to where it started, not to the logical line
  // starts.
  for (int step = 1; step <= 3; ++step) {
    viewport.MoveCursorVertical(-1);
  }
  Expect(viewport.cursor_line() == 0 && viewport.cursor_column() == 3,
         "the primary should come back to (0,3), got " + CaretDump(viewport));
  Expect(viewport.secondary_carets().size() == 1 &&
             viewport.secondary_carets().front() == TextPosition{2, 3},
         "the secondary should come back to (2,3)");
}

// VisualRowForLine answers "where does this LINE start"; cursor_visual_row
// answers "which row is this CARET on". At column 0 of any line they are the same
// number, and every row-indexed cache in the render path reads both.
void TestVisualRowForLineAgreesWithTheCaretRowAtColumnZero() {
  for (const WrapCase& wrap_case : WrapCases()) {
    TextViewport viewport;
    viewport.LoadContent(wrap_case.content, "/tmp/wrap-agreement.txt");
    viewport.SetViewportSize(6, /*visible_columns=*/8);
    viewport.SetSoftWrap(true);
    for (std::size_t line = 0; line < viewport.line_count(); ++line) {
      viewport.MoveCursorTo(line, 0);
      Expect(viewport.cursor_visual_row() == viewport.VisualRowForLine(line),
             std::string(wrap_case.name) + ": line " + std::to_string(line) +
                 " starts at visual row " + std::to_string(viewport.VisualRowForLine(line)) +
                 " but a caret at its column 0 reports " +
                 std::to_string(viewport.cursor_visual_row()));
    }
  }
}

// Turning wrap OFF must leave the caret on the same text position and report the
// caret's LOGICAL line as its row -- the wrap-width-change path resets affinity
// and re-derives the preferred column, and getting that wrong strands the scroll
// in the old row numbering.
void TestTogglingWrapKeepsTheCaretsTextPosition() {
  for (const WrapCase& wrap_case : WrapCases()) {
    TextViewport viewport;
    viewport.LoadContent(wrap_case.content, "/tmp/wrap-toggle.txt");
    viewport.SetViewportSize(6, /*visible_columns=*/8);
    viewport.SetSoftWrap(true);
    viewport.MoveCursorVertical(1);
    viewport.MoveCursorVertical(1);
    const TextPosition before{viewport.cursor_line(), viewport.cursor_column()};

    viewport.SetSoftWrap(false);
    Expect(viewport.cursor_line() == before.line && viewport.cursor_column() == before.column,
           std::string(wrap_case.name) + ": turning wrap off moved the caret from (" +
               std::to_string(before.line) + "," + std::to_string(before.column) + ") to " +
               CaretDump(viewport));
    Expect(viewport.cursor_visual_row() == before.line,
           std::string(wrap_case.name) +
               ": with wrap off the caret's visual row is its line, got " + CaretDump(viewport));

    viewport.SetSoftWrap(true);
    Expect(viewport.cursor_line() == before.line && viewport.cursor_column() == before.column,
           std::string(wrap_case.name) + ": turning wrap back on moved the caret to " +
               CaretDump(viewport));
  }
}

// The walk again with a COLLAPSED FOLD in the way. This is the combination the
// row table gets wrong most easily: a hidden line has no row of its own, so the
// offset table stores the row where the last visible line STARTED -- and under
// wrap that opener spans several rows, so the naive answer parks a caret at the
// TOP of the opener and Down has to climb back out of a fold it is not in
// (TD-2026-08-12-185). The property is unchanged: one press, one visual row, and
// the caret never lands on a hidden line.
void TestWrapWalkSkipsACollapsedFoldOneRowPerPress() {
  TextViewport viewport;
  viewport.LoadContent(
      "head\n"
      "void f() {\n"
      "  aaaaaaaaaaaaaaaaaaaaaaaaaaaa;\n"
      "  bbbbbbbbbbbbbbbbbbbbbbbbbbbb;\n"
      "}\n"
      "tail is long enough to wrap as well\n",
      "/tmp/wrap-fold-walk.cpp");
  viewport.SetViewportSize(/*visible_lines=*/30, /*visible_columns=*/9);
  viewport.SetSoftWrap(true);

  FoldingModel folding;
  FoldingModel::ComputeOptions options;
  options.bracket_pairs = {{'{', '}'}};
  options.use_indent_source = true;
  options.tab_size = 4;
  Expect(folding.Compute(viewport.lines().Snapshot(), options), "the fold fixture computes");
  const std::size_t rows_expanded = viewport.visual_line_count();
  Expect(folding.Collapse(1), "the function fold collapses");
  viewport.SetFoldingModel(&folding);

  const std::size_t rows = viewport.visual_line_count();
  Expect(rows < rows_expanded,
         "collapsing must remove rows, or this walks the same view twice: " +
             std::to_string(rows) + " of " + std::to_string(rows_expanded));
  Expect(folding.IsLineHidden(2) && folding.IsLineHidden(3),
         "the wrapped body lines are hidden");

  viewport.MoveCursorTo(0, 0);
  Expect(viewport.cursor_visual_row() == 0, "the walk starts on row 0, got " + CaretDump(viewport));
  bool crossed_the_fold = false;
  for (std::size_t step = 1; step < rows; ++step) {
    viewport.MoveCursorVertical(1);
    Expect(viewport.cursor_visual_row() == step,
           "Down should land on visual row " + std::to_string(step) + " across the fold, got " +
               CaretDump(viewport));
    Expect(!folding.IsLineHidden(viewport.cursor_line()),
           "the caret must never land inside a collapsed fold, got " + CaretDump(viewport));
    if (viewport.cursor_line() > 3) {
      crossed_the_fold = true;
    }
  }
  Expect(crossed_the_fold, "the walk must have gone past the fold, or it proved nothing");

  for (std::size_t step = rows - 1; step-- > 0;) {
    viewport.MoveCursorVertical(-1);
    Expect(viewport.cursor_visual_row() == step,
           "Up back across the fold should land on row " + std::to_string(step) + ", got " +
               CaretDump(viewport));
    Expect(!folding.IsLineHidden(viewport.cursor_line()),
           "Up must not land inside the collapsed fold either, got " + CaretDump(viewport));
  }
}

// EXACTLY ONE row paints any given caret. The paint loop used to decide with a
// first-row-only heuristic, which drew a caret on a wrap boundary twice and a
// caret at the end of a wrapped line not at all. Stated as a rule
// (WrappedRowPaintsCaret) it can be checked the way the bug shows up: sweep every
// visual column across a whole line's rows and count the rows that claim it.
void TestExactlyOneWrappedRowPaintsEachCaretColumn() {
  // A 24-cell line over 8-cell rows: rows [0,8) [8,16) [16,24).
  struct Row {
    std::size_t start;
    std::size_t end;
    bool first;
    bool last;
  };
  const std::vector<Row> rows = {{0, 8, true, false}, {8, 16, false, false},
                                 {16, 24, false, true}};

  for (const WrapRowAffinity affinity :
       {WrapRowAffinity::kNextRow, WrapRowAffinity::kPreviousRow}) {
    for (std::size_t column = 0; column <= 24; ++column) {
      int claimed = 0;
      for (const Row& row : rows) {
        if (WrappedRowPaintsCaret(column, row.start, row.end, row.first, row.last, affinity)) {
          ++claimed;
        }
      }
      Expect(claimed == 1,
             "column " + std::to_string(column) + " with affinity " +
                 (affinity == WrapRowAffinity::kNextRow ? "next" : "previous") +
                 " is claimed by " + std::to_string(claimed) +
                 " rows; exactly one row must paint each caret");
    }
  }

  // The two specific answers the affinity decides, spelled out so a change to the
  // tiebreak is visible rather than merely still-one-row.
  Expect(!WrappedRowPaintsCaret(8, 0, 8, true, false, WrapRowAffinity::kNextRow),
         "a boundary caret preferring the NEXT row is not painted at the previous "
         "row's trailing edge");
  Expect(WrappedRowPaintsCaret(8, 8, 16, false, false, WrapRowAffinity::kNextRow),
         "it is painted at the leading edge of the row that begins there");
  Expect(WrappedRowPaintsCaret(8, 0, 8, true, false, WrapRowAffinity::kPreviousRow),
         "a boundary caret preferring the PREVIOUS row is painted at its trailing edge");
  Expect(!WrappedRowPaintsCaret(8, 8, 16, false, false, WrapRowAffinity::kPreviousRow),
         "and not again at the start of the row below");

  // End of a wrapped line: the last row claims it whichever affinity says, because
  // there is no row after it to hand the caret to. This is the one the old
  // heuristic dropped outright.
  for (const WrapRowAffinity affinity :
       {WrapRowAffinity::kNextRow, WrapRowAffinity::kPreviousRow}) {
    Expect(WrappedRowPaintsCaret(24, 16, 24, false, true, affinity),
           "the end of a wrapped line is painted on the line's last row");
  }

  // An unwrapped line is one row that is both first and last, so the rule reduces
  // to "on this row, end-of-line included".
  Expect(WrappedRowPaintsCaret(0, 0, 5, true, true, WrapRowAffinity::kNextRow),
         "column 0 of an unwrapped line");
  Expect(WrappedRowPaintsCaret(5, 0, 5, true, true, WrapRowAffinity::kNextRow),
         "the end of an unwrapped line");
  Expect(!WrappedRowPaintsCaret(6, 0, 5, true, true, WrapRowAffinity::kNextRow),
         "past the end of an unwrapped line");
}

// The wrapped-row table must PARTITION each logical line's visual columns: the
// first row starts at 0, every row begins exactly where the previous one ended,
// and the last ends at the line's full visual width. Every hit test, every
// caret-to-row lookup and every paint reads that table, so a gap silently makes a
// column belong to no row and an overlap makes it belong to two -- which is the
// same "one position, two rows" family the affinity bit exists for, one level
// down.
//
// Oracle-free, so it holds for any content at any width: no reference wrapper to
// disagree with, and nothing to update when the break rule changes.
void TestWrappedRowsPartitionEachLine() {
  const std::vector<std::string> contents = {
      "alpha bravo charlie delta echo foxtrot golf hotel",
      std::string(50, 'x'),
      "\tone\ttwo\tthree\tfour\tfive",
      "  indented and long enough that the hanging indent has to be accounted for",
      "\xe4\xbd\xa0\xe5\xa5\xbd \xe4\xbd\xa0\xe5\xa5\xbd \xe4\xbd\xa0\xe5\xa5\xbd wide glyphs mixed with ascii",
      "cafe\xcc\x81 cafe\xcc\x81 cafe\xcc\x81 combining marks that must not split",
      "",
      "a",
      "one two",
      std::string(7, 'q') + " " + std::string(9, 'r'),
  };

  std::size_t lines_with_several_rows = 0;
  for (std::size_t width = 1; width <= 12; ++width) {
    // One document holding every case, so a line's rows are also checked against
    // the rows of its neighbours (the table is one flat vector).
    std::string document;
    for (const std::string& line : contents) {
      document += line;
      document.push_back('\n');
    }
    TextViewport viewport;
    viewport.LoadContent(document, "/tmp/wrap-partition.txt");
    viewport.SetViewportSize(40, width);
    viewport.SetSoftWrap(true);

    const std::size_t rows = viewport.visual_line_count();
    Expect(rows >= viewport.line_count(),
           "wrapping never produces fewer rows than lines at width " +
               std::to_string(width));

    std::size_t row = 0;
    for (std::size_t line = 0; line < viewport.line_count(); ++line) {
      const std::string where =
          "width " + std::to_string(width) + " line " + std::to_string(line) + ": ";
      Expect(row < rows, where + "the table ran out of rows before the lines did");
      const std::size_t line_width = viewport.VisualColumnAt(line, viewport.lines().LineLength(line));

      std::size_t expected_start = 0;
      std::size_t rows_here = 0;
      while (row < rows) {
        const auto meta = viewport.WrappedVisualRowLayout(row);
        if (meta.line_index != line) {
          break;
        }
        Expect(meta.visual_start == expected_start,
               where + "row " + std::to_string(rows_here) + " starts at " +
                   std::to_string(meta.visual_start) + " but the previous one ended at " +
                   std::to_string(expected_start));
        Expect(meta.visual_end >= meta.visual_start,
               where + "a row may not end before it starts");
        // Only the sole row of an EMPTY line may be zero-width; anything else is a
        // row that can never be reached by a caret and shifts every row below it.
        Expect(meta.visual_end > meta.visual_start || line_width == 0,
               where + "row " + std::to_string(rows_here) + " is empty on a line " +
                   std::to_string(line_width) + " cells wide");
        expected_start = meta.visual_end;
        ++rows_here;
        ++row;
      }
      Expect(rows_here >= 1, where + "every line owns at least one row");
      Expect(expected_start == line_width,
             where + "the rows cover " + std::to_string(expected_start) +
                 " cells but the line is " + std::to_string(line_width) + " wide");
      if (rows_here > 1) {
        ++lines_with_several_rows;
      }
    }
    Expect(row == rows,
           "width " + std::to_string(width) + ": " + std::to_string(rows - row) +
               " rows belong to no line");
  }
  // Vacuity guard: a table where nothing wrapped satisfies all of the above.
  Expect(lines_with_several_rows >= 40,
         "the sweep must have produced plenty of multi-row lines, got " +
             std::to_string(lines_with_several_rows));
}

// Home and End are VIEW-line verbs, PER CARET -- including when two carets sit
// on different wrapped rows of the SAME logical line.
//
// That case is the one a logical-line implementation gets wrong without ever
// looking wrong: both carets share a line, so a per-LINE End sends them to the
// same column and silently merges them into one cursor. Each has to answer for
// its own row instead. The return trip is the sharper half: End lands exactly on
// a wrap point, the one position two rows both claim, so Home is only correct if
// the caret carries the affinity saying which row it belongs to.
void TestWrapHomeAndEndAnswerPerCaretOnOneLine() {
  TextViewport viewport;
  viewport.SetViewportSize(10, 10);
  viewport.SetSoftWrap(true);
  viewport.LoadContent("aaaa bbbb cccc dddd eeee\nshort\n", "/tmp/wrap-per-caret.txt");
  Expect(viewport.visual_line_count() > viewport.line_count(),
         "the fixture must actually wrap, or this proves nothing");

  // Two carets on the SAME logical line, on different wrapped rows.
  viewport.MoveCursorTo(0, 2);
  viewport.AddSecondaryCaret(0, 14);
  Expect(viewport.secondary_carets().size() == 1 && viewport.secondary_carets()[0].line == 0,
         "both carets start on line 0");

  viewport.MoveCursorLineEnd();
  Expect(viewport.cursor_column() == 10,
         "the caret on the first row ends at that ROW's end, got " +
             std::to_string(viewport.cursor_column()));
  Expect(viewport.secondary_carets().size() == 1 && viewport.secondary_carets()[0].column == 20,
         "the caret on the second row ends at ITS row's end, not the same column as the first "
         "-- a per-line End would have merged them into one cursor");

  viewport.MoveCursorLineStart();
  Expect(viewport.cursor_column() == 0,
         "Home from the first row's wrap point returns to that row's start, got " +
             std::to_string(viewport.cursor_column()));
  Expect(viewport.secondary_carets().size() == 1 && viewport.secondary_carets()[0].column == 10,
         "Home from the second row's wrap point returns to the SECOND row's start (10), not the "
         "third's -- this is the affinity bit, and without it the caret walks forward a row on "
         "every End/Home pair");
}


}  // namespace

void RegisterEditorWrapNavigationPropertyTests(std::vector<TestCase>& tests) {
  AddTest(tests, "EditorWrapNavigation/HomeAndEndAnswerPerCaretOnOneLine",
          TestWrapHomeAndEndAnswerPerCaretOnOneLine);
  AddTest(tests, "EditorWrapNavigation/DownWalkVisitsEveryVisualRowOnce",
          TestWrapDownWalkVisitsEveryVisualRowOnce);
  AddTest(tests, "EditorWrapNavigation/WalkKeepsEverySecondaryCaretOnItsOwnRow",
          TestWrapWalkKeepsEverySecondaryCaretOnItsOwnRow);
  AddTest(tests, "EditorWrapNavigation/VisualRowForLineAgreesWithTheCaretRowAtColumnZero",
          TestVisualRowForLineAgreesWithTheCaretRowAtColumnZero);
  AddTest(tests, "EditorWrapNavigation/WalkSkipsACollapsedFoldOneRowPerPress",
          TestWrapWalkSkipsACollapsedFoldOneRowPerPress);
  AddTest(tests, "EditorWrapNavigation/WrappedRowsPartitionEachLine",
          TestWrappedRowsPartitionEachLine);
  AddTest(tests, "EditorWrapNavigation/ExactlyOneWrappedRowPaintsEachCaretColumn",
          TestExactlyOneWrappedRowPaintsEachCaretColumn);
  AddTest(tests, "EditorWrapNavigation/TogglingWrapKeepsTheCaretsTextPosition",
          TestTogglingWrapKeepsTheCaretsTextPosition);
}

}  // namespace microide::tests
