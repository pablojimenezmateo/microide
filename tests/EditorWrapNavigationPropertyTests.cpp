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
#include "editor/TextViewport.h"

namespace microide::tests {
namespace {

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

}  // namespace

void RegisterEditorWrapNavigationPropertyTests(std::vector<TestCase>& tests) {
  AddTest(tests, "EditorWrapNavigation/DownWalkVisitsEveryVisualRowOnce",
          TestWrapDownWalkVisitsEveryVisualRowOnce);
  AddTest(tests, "EditorWrapNavigation/WalkKeepsEverySecondaryCaretOnItsOwnRow",
          TestWrapWalkKeepsEverySecondaryCaretOnItsOwnRow);
  AddTest(tests, "EditorWrapNavigation/VisualRowForLineAgreesWithTheCaretRowAtColumnZero",
          TestVisualRowForLineAgreesWithTheCaretRowAtColumnZero);
  AddTest(tests, "EditorWrapNavigation/TogglingWrapKeepsTheCaretsTextPosition",
          TestTogglingWrapKeepsTheCaretsTextPosition);
}

}  // namespace microide::tests
