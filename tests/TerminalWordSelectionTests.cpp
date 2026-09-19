// Double- and triple-click selection in the TERMINAL.
//
// `TerminalWordBoundsAt` and `TerminalLineBoundsAt` are what the panel mouse
// coordinator calls for a double or triple click, and neither had a single test
// referencing it. The word rule is deliberately NOT the editor's: it adds the
// punctuation that makes a path or a URL one token in a terminal
// (`.`/`-`/`/`/`~`/`+`/`:`/`@`), so that double-clicking `src/foo/bar.cpp:42`
// selects the whole thing rather than `bar`. That is the behaviour worth pinning,
// because it is a deliberate divergence someone could "fix" back toward the
// editor's rule without anything failing.
//
// The bounds are also swept as a property over every column of every fixture
// row: a run has to be maximal, and it has to be a run of word cells.

#include "TestSupport.h"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "terminal/TerminalCell.h"
#include "workspace/WorkspaceTerminalSelection.h"

namespace microide::tests {
namespace {

using microide::terminal::TerminalLine;
using microide::workspace::TerminalLineBoundsAt;
using microide::workspace::TerminalSelectionBounds;
using microide::workspace::TerminalWordBoundsAt;

// One ASCII cell per character; a space is an EMPTY cell, which is what the
// emulator leaves for unwritten columns.
TerminalLine MakeLine(std::string_view text) {
  TerminalLine line;
  line.cells.resize(text.size());
  for (std::size_t i = 0; i < text.size(); ++i) {
    if (text[i] != ' ') {
      line.cells[i].SetAscii(text[i]);
    }
  }
  return line;
}

std::string Slice(std::string_view text, const TerminalSelectionBounds& bounds) {
  return std::string(text.substr(bounds.start.column, bounds.end.column - bounds.start.column));
}

// What a double-click selects, by the documented rule.
void TestTerminalDoubleClickSelectsPathsAndUrlsWhole() {
  struct Case {
    const char* text;
    std::size_t column;
    const char* expected;
  };
  const std::vector<Case> cases = {
      // The case the rule exists for: a compiler diagnostic's file:line.
      {"see src/foo/bar.cpp:42 for details", 12, "src/foo/bar.cpp:42"},
      // A URL stays one token up to its query string: `?` and `&` are NOT in the
      // set, so `?x` is left out. That is the documented rule (`.`/`-`/`/`/`~`/
      // `+`/`:`/`@` on top of identifier bytes), not an oversight to widen
      // silently -- widening it is a product decision, and this case is where it
      // would be made.
      {"open https://example.com/a-b?x here", 10, "https://example.com/a-b"},
      // A home-relative path.
      {"cd ~/work/my-project", 6, "~/work/my-project"},
      // An e-mail-ish token: '@' and '.' are both word bytes here.
      {"from a.b@c.example ok", 8, "a.b@c.example"},
      // A plain identifier still behaves like one.
      {"alpha beta gamma", 7, "beta"},
      // A '+' inside a version token.
      {"v1.2+build3 done", 4, "v1.2+build3"},
  };

  for (const Case& test_case : cases) {
    const TerminalLine line = MakeLine(test_case.text);
    const auto bounds = TerminalWordBoundsAt(line, /*row=*/3, test_case.column);
    const std::string where =
        std::string("\"") + test_case.text + "\" at column " + std::to_string(test_case.column);
    Expect(bounds.has_value(), where + ": a word cell must produce bounds");
    Expect(bounds->start.row == 3 && bounds->end.row == 3,
           where + ": the bounds stay on the clicked row");
    const std::string got = Slice(test_case.text, *bounds);
    Expect(got == test_case.expected,
           where + ": expected \"" + test_case.expected + "\", got \"" + got + "\"");
  }

  // A blank cell, and a cell that is a separator, select nothing -- the caller
  // leaves the caret-style empty selection alone rather than snapping to a
  // neighbour the user did not click.
  const TerminalLine line = MakeLine("alpha beta");
  Expect(!TerminalWordBoundsAt(line, 0, 5).has_value(),
         "a blank cell between two words selects nothing");
  Expect(!TerminalWordBoundsAt(line, 0, 99).has_value(),
         "a column past the end of the row selects nothing");
  const TerminalLine punctuation = MakeLine("a(b)c");
  Expect(!TerminalWordBoundsAt(punctuation, 0, 1).has_value(),
         "'(' is not a terminal word byte, so clicking it selects nothing");
}

// Every column of every fixture row: when bounds come back they must be a
// MAXIMAL run of word cells containing the clicked column.
void TestTerminalWordBoundsAreAlwaysAMaximalRun() {
  const std::vector<std::string> rows = {
      "see src/foo/bar.cpp:42 for details",
      "a(b)c  d--e  ~/x",
      "   ",
      "",
      "one",
      "trailing   ",
  };
  std::size_t selections = 0;
  std::size_t refusals = 0;

  for (const std::string& text : rows) {
    const TerminalLine line = MakeLine(text);
    for (std::size_t column = 0; column <= text.size() + 2; ++column) {
      const auto bounds = TerminalWordBoundsAt(line, /*row=*/1, column);
      const std::string where = "\"" + text + "\" at " + std::to_string(column);
      if (!bounds.has_value()) {
        ++refusals;
        continue;
      }
      ++selections;
      Expect(bounds->start.column <= column && column < bounds->end.column,
             where + ": the run must contain the clicked column");
      Expect(bounds->end.column <= line.cells.size(),
             where + ": the run must stay inside the row");
      // Maximal: every cell inside is a word cell (it round-trips as a
      // one-column selection), and the cells just outside are not.
      for (std::size_t i = bounds->start.column; i < bounds->end.column; ++i) {
        Expect(TerminalWordBoundsAt(line, 1, i).has_value(),
               where + ": cell " + std::to_string(i) + " inside the run is not a word cell");
      }
      if (bounds->start.column > 0) {
        Expect(!TerminalWordBoundsAt(line, 1, bounds->start.column - 1).has_value(),
               where + ": the run is not maximal -- the cell before it is a word cell");
      }
      if (bounds->end.column < line.cells.size()) {
        Expect(!TerminalWordBoundsAt(line, 1, bounds->end.column).has_value(),
               where + ": the run is not maximal -- the cell after it is a word cell");
      }
    }
  }
  // Both outcomes have to occur, or the rules above are checked on one branch.
  Expect(selections > 30, "the sweep barely selected anything: " + std::to_string(selections));
  Expect(refusals > 10, "the sweep never hit a non-word cell: " + std::to_string(refusals));
}

// Triple-click: the whole row without its trailing blanks, and nothing at all
// for a row that is blank.
void TestTerminalTripleClickTakesTheRowWithoutTrailingBlanks() {
  const TerminalLine padded = MakeLine("hello world   ");
  const auto bounds = TerminalLineBoundsAt(padded, /*row=*/7);
  Expect(bounds.has_value(), "a row with content produces bounds");
  Expect(bounds->start.row == 7 && bounds->end.row == 7, "the bounds stay on the row");
  Expect(bounds->start.column == 0, "a line selection starts at column 0");
  Expect(bounds->end.column == 11,
         "the trailing blanks are excluded, got end column " +
             std::to_string(bounds->end.column));

  Expect(!TerminalLineBoundsAt(MakeLine("     "), 0).has_value(),
         "an all-blank row selects nothing");
  Expect(!TerminalLineBoundsAt(MakeLine(""), 0).has_value(),
         "an empty row selects nothing");

  // Interior blanks are kept: only the trailing run is dropped.
  const auto gapped = TerminalLineBoundsAt(MakeLine("a   b  "), 0);
  Expect(gapped.has_value() && gapped->end.column == 5,
         "interior blanks stay inside the line selection");
}

}  // namespace

void RegisterTerminalWordSelectionTests(std::vector<TestCase>& tests) {
  AddTest(tests, "TerminalWordSelection/DoubleClickSelectsPathsAndUrlsWhole",
          TestTerminalDoubleClickSelectsPathsAndUrlsWhole);
  AddTest(tests, "TerminalWordSelection/WordBoundsAreAlwaysAMaximalRun",
          TestTerminalWordBoundsAreAlwaysAMaximalRun);
  AddTest(tests, "TerminalWordSelection/TripleClickTakesTheRowWithoutTrailingBlanks",
          TestTerminalTripleClickTakesTheRowWithoutTrailingBlanks);
}

}  // namespace microide::tests
