#include "TestSupport.h"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "terminal/TerminalCell.h"
#include "util/StringUtil.h"
#include <random>
#include "workspace/WorkspaceTerminalSelection.h"

namespace microide::tests {
namespace {

using microide::terminal::TerminalLine;
using microide::workspace::TerminalLineBoundsAt;
using microide::workspace::TerminalSelectionBounds;
using microide::workspace::TerminalWordBoundsAt;

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

void TestTerminalDoubleClickSelectsPathsAndUrlsWhole() {
  struct Case {
    const char* text;
    std::size_t column;
    const char* expected;
  };
  const std::vector<Case> cases = {
      {"see src/foo/bar.cpp:42 for details", 12, "src/foo/bar.cpp:42"},
      {"open https://example.com/a-b?x here", 10, "https://example.com/a-b"},
      {"cd ~/work/my-project", 6, "~/work/my-project"},
      {"from a.b@c.example ok", 8, "a.b@c.example"},
      {"alpha beta gamma", 7, "beta"},
      {"v1.2+build3 done", 4, "v1.2+build3"},
  };

  for (const Case& test_case : cases) {
    const TerminalLine line = MakeLine(test_case.text);
    const auto bounds = TerminalWordBoundsAt(line, 3, test_case.column);
    const std::string where =
        std::string("\"") + test_case.text + "\" at column " + std::to_string(test_case.column);
    Expect(bounds.has_value(), where + ": a word cell must produce bounds");
    Expect(bounds->start.row == 3 && bounds->end.row == 3,
           where + ": the bounds stay on the clicked row");
    const std::string got = Slice(test_case.text, *bounds);
    Expect(got == test_case.expected,
           where + ": expected \"" + test_case.expected + "\", got \"" + got + "\"");
  }

  const TerminalLine line = MakeLine("alpha beta");
  Expect(!TerminalWordBoundsAt(line, 0, 5).has_value(),
         "a blank cell between two words selects nothing");
  Expect(!TerminalWordBoundsAt(line, 0, 99).has_value(),
         "a column past the end of the row selects nothing");
  const TerminalLine punctuation = MakeLine("a(b)c");
  Expect(!TerminalWordBoundsAt(punctuation, 0, 1).has_value(),
         "'(' is not a terminal word byte, so clicking it selects nothing");
}

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
      const auto bounds = TerminalWordBoundsAt(line, 1, column);
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
  Expect(selections > 30, "the sweep barely selected anything: " + std::to_string(selections));
  Expect(refusals > 10, "the sweep never hit a non-word cell: " + std::to_string(refusals));
}

void TestTerminalTripleClickTakesTheRowWithoutTrailingBlanks() {
  const TerminalLine padded = MakeLine("hello world   ");
  const auto bounds = TerminalLineBoundsAt(padded, 7);
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

  const auto gapped = TerminalLineBoundsAt(MakeLine("a   b  "), 0);
  Expect(gapped.has_value() && gapped->end.column == 5,
         "interior blanks stay inside the line selection");
}

void TestTerminalSelectionExtendsByItsUnit() {
  using microide::workspace::ExtendTerminalSelection;
  using microide::workspace::TerminalSelectionPoint;
  using microide::workspace::TerminalSelectionUnit;
  const std::string_view text = "one two three four";
  const TerminalLine line = MakeLine(text);
  const TerminalSelectionBounds two{.start = {0, 4}, .end = {0, 7}};
  const auto slice = [&](const auto& ends) {
    const auto lo = std::min(ends.anchor.column, ends.head.column);
    const auto hi = std::max(ends.anchor.column, ends.head.column);
    return std::string(text.substr(lo, hi - lo));
  };

  auto ends = ExtendTerminalSelection(TerminalSelectionUnit::Word, two, line, TerminalSelectionPoint{0, 16});
  Expect(slice(ends) == "two three four", "a word drag forward ends on a word end: [" + slice(ends) + "]");
  ends = ExtendTerminalSelection(TerminalSelectionUnit::Word, two, line, TerminalSelectionPoint{0, 1});
  Expect(slice(ends) == "one two" && ends.anchor.column == 7,
         "a word drag backward keeps the clicked word: [" + slice(ends) + "]");
  ends = ExtendTerminalSelection(TerminalSelectionUnit::Word, two, line, TerminalSelectionPoint{0, 5});
  Expect(slice(ends) == "two", "the pointer inside the word selects the word: [" + slice(ends) + "]");
  ends = ExtendTerminalSelection(TerminalSelectionUnit::Word, two, line, TerminalSelectionPoint{0, 13});
  Expect(slice(ends) == "two three", "the edge after a word takes the word: [" + slice(ends) + "]");

  const TerminalLine row4 = MakeLine("last row   ");
  const TerminalSelectionBounds line2{.start = {2, 0}, .end = {2, 9}};
  ends = ExtendTerminalSelection(TerminalSelectionUnit::Line, line2, row4, TerminalSelectionPoint{4, 2});
  Expect(ends.anchor.row == 2 && ends.anchor.column == 0 && ends.head.row == 4 && ends.head.column == 8,
         "a row drag down ends at the pointer row's content end");
  ends = ExtendTerminalSelection(TerminalSelectionUnit::Line, line2, row4, TerminalSelectionPoint{0, 5});
  Expect(ends.anchor.row == 2 && ends.anchor.column == 9 && ends.head.row == 0 && ends.head.column == 0,
         "a row drag up keeps the clicked row and starts at the pointer row's start");

  const TerminalSelectionBounds press{.start = {0, 6}, .end = {0, 6}};
  ends = ExtendTerminalSelection(TerminalSelectionUnit::Cell, press, line, TerminalSelectionPoint{0, 2});
  Expect(ends.anchor.column == 6 && ends.head.column == 2, "a cell drag runs from the press to the pointer");
}

std::string ReferenceSelectionText(const std::vector<TerminalLine>& lines,
                                   const TerminalSelectionBounds& selection, std::size_t max_bytes) {
  if (lines.empty() || selection.start.row >= lines.size()) {
    return {};
  }
  const auto content_end = [](const TerminalLine& line) {
    std::size_t end = line.cells.size();
    while (end > 0) {
      const std::string_view text = line.cells[end - 1].DisplayText();
      if (!text.empty() && text != " ") {
        break;
      }
      --end;
    }
    return end;
  };
  const std::size_t end_row = std::min(selection.end.row, lines.size() - 1);
  std::string text;
  for (std::size_t row = selection.start.row; row <= end_row; ++row) {
    const auto& line = lines[row];
    const std::size_t size = line.cells.size();
    const std::size_t start_column = row == selection.start.row ? std::min(selection.start.column, size) : 0;
    std::size_t end_column = row == end_row ? std::min(selection.end.column, size) : size;
    const bool continued = row + 1 < lines.size() && lines[row + 1].wrapped_from_previous;
    if (!continued) {
      end_column = std::max(start_column, std::min(end_column, content_end(line)));
    }
    text.append(workspace::TerminalLineSliceText(line, start_column, end_column, false));
    if (text.size() >= max_bytes) {
      text.resize(util::Utf8ByteBudgetPrefixLength(text, max_bytes));
      text += "\n[selection truncated]";
      return text;
    }
    if (row != end_row && !lines[row + 1].wrapped_from_previous) {
      text.push_back('\n');
    }
  }
  return text;
}

void TestTerminalSelectionCopyStreamsIdentically() {
  using microide::workspace::ExtractTerminalSelectionText;
  using microide::workspace::TerminalSelectionTextBuilder;
  std::mt19937 rng(1105);
  const auto pick = [&](std::size_t lo, std::size_t hi) {
    return std::uniform_int_distribution<std::size_t>(lo, hi)(rng);
  };
  std::size_t compared = 0;
  for (int trial = 0; trial < 600; ++trial) {
    std::vector<TerminalLine> lines(pick(1, 9));
    for (TerminalLine& line : lines) {
      line.cells.resize(pick(0, 12));
      line.wrapped_from_previous = pick(0, 3) == 0;
      for (std::size_t c = 0; c < line.cells.size(); ++c) {
        const std::size_t kind = pick(0, 9);
        if (kind < 5) {
          line.cells[c].SetAscii(static_cast<char>('a' + pick(0, 25)));
        } else if (kind == 5 && c + 1 < line.cells.size()) {
          line.cells[c].SetUtf8("\xe4\xb8\xad");
          line.cells[c + 1].style.set(terminal::cell_attr::kWideTrailing, true);
          ++c;
        }
      }
    }
    const TerminalSelectionBounds bounds = *workspace::NormalizeTerminalSelection(
        workspace::TerminalSelectionPoint{pick(0, lines.size() - 1), pick(0, 13)},
        workspace::TerminalSelectionPoint{pick(0, lines.size() - 1), pick(0, 13)});
    const std::size_t budget = pick(0, 3) == 0 ? pick(1, 20) : 1u << 20;
    const std::string expected = ReferenceSelectionText(lines, bounds, budget);
    const std::string got = ExtractTerminalSelectionText(lines, bounds, budget);
    if (got != expected) {
      Expect(false, "the streaming copy differs from the one-pass copy: [" + got + "] vs [" + expected + "]");
      return;
    }
    if (bounds.start.row < lines.size()) {
      TerminalSelectionTextBuilder builder(bounds, budget);
      const std::size_t end_row = std::min(bounds.end.row, lines.size() - 1);
      bool more = true;
      for (std::size_t row = bounds.start.row; row <= end_row && more; ++row) {
        more = builder.AddRow(row, lines[row]);
      }
      const std::string fed =
          builder.Finish(more && end_row + 1 < lines.size() && lines[end_row + 1].wrapped_from_previous);
      Expect(fed == expected, "the builder fed row by row matches: [" + fed + "] vs [" + expected + "]");
    }
    ++compared;
  }
  Expect(compared == 600, "every trial compared");
}

}

void RegisterTerminalWordSelectionTests(std::vector<TestCase>& tests) {
  AddTest(tests, "TerminalWordSelection/DoubleClickSelectsPathsAndUrlsWhole",
          TestTerminalDoubleClickSelectsPathsAndUrlsWhole);
  AddTest(tests, "TerminalWordSelection/WordBoundsAreAlwaysAMaximalRun",
          TestTerminalWordBoundsAreAlwaysAMaximalRun);
  AddTest(tests, "TerminalWordSelection/TripleClickTakesTheRowWithoutTrailingBlanks",
          TestTerminalTripleClickTakesTheRowWithoutTrailingBlanks);
  AddTest(tests, "TerminalWordSelection/SelectionExtendsByItsUnit", TestTerminalSelectionExtendsByItsUnit);
  AddTest(tests, "TerminalWordSelection/SelectionCopyStreamsIdentically",
          TestTerminalSelectionCopyStreamsIdentically);
}

}
