// What one terminal row paints (workspace/render/TerminalRowPaint).
//
// The selection used to paint the theme's hover tint under the theme's text
// colour: barely visible, and it erased the output's own colours -- red text went
// grey and a coloured background vanished inside it. These pin the replacement:
// a wash composited over each cell's own background, the cell's foreground kept
// (moved only as far as legibility needs), the line break of a row the selection
// runs past washed to the pane's edge, and a double-width glyph emphasized whole
// exactly when the copy takes it.

#include "TestSupport.h"

#include <cstddef>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include "render/ColorMath.h"
#include "terminal/TerminalCell.h"
#include "workspace/WorkspaceTerminalSelection.h"
#include "workspace/render/TerminalRowPaint.h"

namespace microide::tests {
namespace {

using microide::terminal::TerminalLine;
using microide::workspace::PlanTerminalRow;
using microide::workspace::TerminalPaintPalette;
using microide::workspace::TerminalRowEmphasis;
using microide::workspace::TerminalRowPaintPlan;
using microide::workspace::TerminalRowSelectionEmphasis;
using microide::workspace::TerminalSelectionBounds;

constexpr SDL_Color kForeground{0xdd, 0xdd, 0xdd, 0xff};
constexpr SDL_Color kBackground{0x10, 0x14, 0x1c, 0xff};
constexpr SDL_Color kSelection{0x2a, 0x5a, 0xb0, 0xb4};
constexpr SDL_Color kRed{0xe0, 0x40, 0x40, 0xff};
constexpr SDL_Color kGreen{0x40, 0xa0, 0x40, 0xff};

TerminalPaintPalette Palette() {
  return TerminalPaintPalette{
      .foreground = kForeground,
      .background = kBackground,
      .selection = kSelection,
      .find_match = SDL_Color{0x90, 0x80, 0x20, 0x8f},
      .find_current = SDL_Color{0xb0, 0x90, 0x20, 0xc8},
  };
}

bool Same(SDL_Color a, SDL_Color b) {
  return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
}

std::string Describe(SDL_Color c) {
  return std::to_string(c.r) + "," + std::to_string(c.g) + "," + std::to_string(c.b) + "," +
         std::to_string(c.a);
}

util::Rgba8 Rgba(SDL_Color c) { return util::Rgba8{c.r, c.g, c.b, 0xff}; }

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

// The fill covering `column`, if any (none: the base colour shows).
std::optional<SDL_Color> FillAt(const TerminalRowPaintPlan& plan, std::size_t column) {
  for (const auto& fill : plan.fills) {
    if (column >= fill.begin && column < fill.end) {
      return fill.color;
    }
  }
  return std::nullopt;
}

std::optional<SDL_Color> TextColorAt(const TerminalRowPaintPlan& plan, std::size_t column) {
  for (const auto& run : plan.text_runs) {
    if (column >= run.column && column < run.column + run.length) {
      return run.color;
    }
  }
  return std::nullopt;
}

TerminalRowEmphasis Selecting(std::size_t begin, std::size_t end, bool runs_past = false) {
  TerminalRowEmphasis emphasis;
  emphasis.selection_begin = begin;
  emphasis.selection_end = runs_past ? TerminalRowEmphasis::kToRowEnd : end;
  emphasis.selection_runs_past_row = runs_past;
  return emphasis;
}

// The reported case: `printf '\e[31mred\e[0m'`, selected, stayed red.
void TestSelectedTextKeepsItsOwnColour() {
  TerminalLine line = MakeLine("red plain");
  for (std::size_t i = 0; i < 3; ++i) {
    line.cells[i].style.foreground = Rgba(kRed);
  }
  TerminalRowPaintPlan plan;
  PlanTerminalRow(line, 40, Selecting(0, 9), Palette(), plan);

  const SDL_Color wash = render::CompositeOver(kSelection, kBackground);
  Expect(FillAt(plan, 0).has_value() && Same(*FillAt(plan, 0), wash),
         "a selected cell's background is the wash over its own background");
  const auto red = TextColorAt(plan, 0);
  Expect(red.has_value() && red->r > red->g + 0x40 && red->r > red->b + 0x40,
         "selected red text is still red, got " + (red ? Describe(*red) : std::string("none")));
  Expect(render::Contrast(*red, wash) >= workspace::kTerminalEmphasisMinimumContrast - 0.01f ||
             Same(*red, kRed),
         "and legible on the wash");
  const auto plain = TextColorAt(plan, 4);
  Expect(plain.has_value() && !Same(*plain, *red), "the plain text keeps its own colour");
}

// A coloured background is washed, not replaced: `\e[42m` inside a selection
// still reads as green-ish, and differs from a selected default cell.
void TestSelectionWashesAColouredBackground() {
  TerminalLine line = MakeLine("ab");
  line.cells[0].style.background = Rgba(kGreen);
  TerminalRowPaintPlan plan;
  PlanTerminalRow(line, 10, Selecting(0, 2), Palette(), plan);
  const auto green_cell = FillAt(plan, 0);
  const auto plain_cell = FillAt(plan, 1);
  Expect(green_cell.has_value() && Same(*green_cell, render::CompositeOver(kSelection, kGreen)),
         "the selected green cell is the wash over green");
  Expect(plain_cell.has_value() && !Same(*green_cell, *plain_cell),
         "and differs from a selected default cell");
}

// Nothing is drawn for the colour the pane body already has: an unemphasized
// default row emits no fill at all.
void TestPlainRowEmitsNoFills() {
  TerminalRowPaintPlan plan;
  PlanTerminalRow(MakeLine("just text   "), 40, TerminalRowEmphasis{}, Palette(), plan);
  Expect(plan.fills.empty(), "a default row has no background fill, got " +
                                 std::to_string(plan.fills.size()));
  Expect(plan.text_runs.size() == 1 && plan.RunText(plan.text_runs[0]) == "just text",
         "one glyph run, blanks around it not drawn: [" +
             (plan.text_runs.empty() ? std::string() : std::string(plan.RunText(plan.text_runs[0]))) + "]");
}

// A row the selection continues past has its line break selected: the wash runs
// to the pane's edge, even past the row's cells and on an empty row. The last
// row of the selection stops where the selection does.
void TestSelectionRunningPastARowWashesToTheEdge() {
  TerminalRowPaintPlan plan;
  PlanTerminalRow(MakeLine("short"), 40, Selecting(2, 0, /*runs_past=*/true), Palette(), plan);
  const SDL_Color wash = render::CompositeOver(kSelection, kBackground);
  Expect(!FillAt(plan, 1).has_value(), "before the selection: no fill");
  Expect(FillAt(plan, 2).has_value() && FillAt(plan, 39).has_value() && Same(*FillAt(plan, 39), wash),
         "from the selection's start to the pane's last column");
  Expect(!FillAt(plan, 40).has_value(), "and not past the pane");
  Expect(plan.fills.size() == 1, "one coalesced fill, got " + std::to_string(plan.fills.size()));

  PlanTerminalRow(TerminalLine{}, 30, Selecting(0, 0, /*runs_past=*/true), Palette(), plan);
  Expect(plan.fills.size() == 1 && plan.fills[0].begin == 0 && plan.fills[0].end == 30,
         "an empty row inside a selection is washed across");

  PlanTerminalRow(MakeLine("short"), 40, Selecting(0, 3), Palette(), plan);
  Expect(FillAt(plan, 2).has_value() && !FillAt(plan, 3).has_value() && !FillAt(plan, 20).has_value(),
         "the selection's last row stops at its end column");
}

// Hidden text (SGR 8) stays hidden when selected; a foreground that would vanish
// into the wash is pulled to a legible one.
void TestEmphasisKeepsHiddenHiddenAndLowContrastLegible() {
  TerminalLine line = MakeLine("hx");
  line.cells[0].style.set(terminal::cell_attr::kHidden, true);
  const SDL_Color wash = render::CompositeOver(kSelection, kBackground);
  line.cells[1].style.foreground = Rgba(wash);  // exactly the washed background
  TerminalRowPaintPlan plan;
  PlanTerminalRow(line, 10, Selecting(0, 2), Palette(), plan);
  const auto hidden = TextColorAt(plan, 0);
  Expect(!hidden.has_value() || Same(*hidden, wash), "hidden text paints in its background");
  const auto x = TextColorAt(plan, 1);
  Expect(x.has_value() && render::Contrast(*x, wash) >= workspace::kTerminalEmphasisMinimumContrast,
         "a foreground equal to the wash is made legible");
}

// A double-width glyph is emphasized whole, and exactly when the copy takes it:
// the copy keys on the lead cell (the trailing half has no text).
void TestWideGlyphIsEmphasizedWithItsLead() {
  TerminalLine line = MakeLine("a  b");
  line.cells[1].SetUtf8("\xe4\xb8\xad");  // 中, lead
  line.cells[2].style.set(terminal::cell_attr::kWideTrailing, true);
  const std::vector<TerminalLine> lines{line};
  TerminalRowPaintPlan plan;

  // Selection [2, 4): starts on the trailing half. The copy leaves the glyph out,
  // so neither half is washed.
  PlanTerminalRow(line, 10, Selecting(2, 4), Palette(), plan);
  const std::string copy_from_trailing = workspace::ExtractTerminalSelectionText(
      lines, TerminalSelectionBounds{.start = {0, 2}, .end = {0, 4}});
  Expect(copy_from_trailing == "b", "the copy from the trailing half skips the glyph: [" +
                                        copy_from_trailing + "]");
  Expect(!FillAt(plan, 1).has_value() && !FillAt(plan, 2).has_value(),
         "so neither half of the glyph is washed");

  // Selection [0, 2): ends on the trailing half. The copy takes the glyph, so
  // both halves are washed.
  PlanTerminalRow(line, 10, Selecting(0, 2), Palette(), plan);
  const std::string copy_to_trailing = workspace::ExtractTerminalSelectionText(
      lines, TerminalSelectionBounds{.start = {0, 0}, .end = {0, 2}});
  Expect(copy_to_trailing == "a\xe4\xb8\xad", "the copy to the lead takes the glyph: [" +
                                                  copy_to_trailing + "]");
  Expect(FillAt(plan, 1).has_value() && FillAt(plan, 2).has_value(),
         "so both halves of the glyph are washed");
}

// A find hit is a wash too, the current one stronger; the selection outranks it.
void TestFindMatchesWashAndSelectionOutranksThem() {
  const TerminalLine line = MakeLine("foo foo foo");
  const std::vector<terminal::TerminalSearchMatch> matches{
      {.row = 0, .column = 0, .length = 3},
      {.row = 0, .column = 4, .length = 3},
      {.row = 0, .column = 8, .length = 3},
  };
  TerminalRowEmphasis emphasis = Selecting(8, 11);
  emphasis.matches = matches.data();
  emphasis.match_count = matches.size();
  emphasis.current_match = 1;
  TerminalRowPaintPlan plan;
  PlanTerminalRow(line, 20, emphasis, Palette(), plan);
  const TerminalPaintPalette palette = Palette();
  Expect(Same(*FillAt(plan, 0), render::CompositeOver(palette.find_match, kBackground)),
         "a match is the match wash");
  Expect(Same(*FillAt(plan, 4), render::CompositeOver(palette.find_current, kBackground)),
         "the current match is the current wash");
  Expect(Same(*FillAt(plan, 8), render::CompositeOver(kSelection, kBackground)),
         "a selected match shows the selection");
  Expect(!FillAt(plan, 3).has_value(), "the blank between matches is not washed");
}

// The row slice of a selection, swept: a column is selected on a row exactly when
// TerminalSelectionContainsCell says so, and the row is washed past its end
// exactly when the selection continues on the next row.
// An underline takes its SGR 58 colour and shape, and strokes coalesce only
// over cells that agree on both; a strikethrough keeps the text's colour, and a
// cell with no colour of its own underlines in its text's.
void TestUnderlinesTakeTheirColourAndShape() {
  using microide::terminal::TerminalUnderlineColor;
  using microide::terminal::TerminalUnderlineVariant;
  namespace attr = microide::terminal::cell_attr;
  TerminalLine line;
  line.cells.resize(6);
  for (auto& cell : line.cells) {
    cell.SetAscii('x');
  }
  for (std::size_t i = 0; i < 4; ++i) {
    line.cells[i].style.attrs = attr::kUnderline;
    line.cells[i].style.set_underline_variant(TerminalUnderlineVariant::Curly);
    line.cells[i].extra = 3;
  }
  line.cells[2].extra = 4;  // same shape, another colour
  line.cells[3].style.set_underline_variant(TerminalUnderlineVariant::Dashed);
  line.cells[3].extra = 4;
  line.cells[4].style.attrs = attr::kUnderline | attr::kStrikethrough;
  line.cells[4].style.foreground = microide::util::Rgba8{kRed.r, kRed.g, kRed.b, 255};
  line.cells[4].extra = 3;
  line.cells[5].style.attrs = attr::kUnderline;
  const std::vector<TerminalUnderlineColor> colors = {
      {.extra = 3, .color = microide::util::Rgba8{10, 200, 10, 255}},
      {.extra = 4, .color = microide::util::Rgba8{200, 10, 10, 255}},
  };
  TerminalRowPaintPlan plan;
  PlanTerminalRow(line, 10, TerminalRowEmphasis{}, Palette(), plan, colors);
  Expect(plan.decorations.size() == 5, "five strokes: " + std::to_string(plan.decorations.size()));
  if (plan.decorations.size() != 5) {
    return;
  }
  const auto& d = plan.decorations;
  Expect(d[0].begin == 0 && d[0].end == 2 && d[0].variant == TerminalUnderlineVariant::Curly &&
             Same(d[0].underline_color, SDL_Color{10, 200, 10, 255}),
         "two curly cells of one colour are one stroke in it");
  Expect(d[1].begin == 2 && d[1].end == 3 && Same(d[1].underline_color, SDL_Color{200, 10, 10, 255}),
         "a new colour starts a stroke");
  Expect(d[2].begin == 3 && d[2].variant == TerminalUnderlineVariant::Dashed, "so does a new shape");
  Expect(d[3].begin == 4 && Same(d[3].color, kRed) && Same(d[3].underline_color, SDL_Color{10, 200, 10, 255}),
         "the strikethrough keeps the text's colour while the underline takes its own");
  Expect(d[4].begin == 5 && Same(d[4].underline_color, kForeground) &&
             d[4].variant == TerminalUnderlineVariant::Straight,
         "without SGR 58 the underline is the text's colour");
}

// SGR 1 / 3 text is drawn in its face, so a glyph run ends where the face
// changes, and knows the cells it covers (a face whose advance is not the grid's
// is placed cell by cell).
void TestTextRunsSplitOnTheFace() {
  using microide::render::FontStyle;
  namespace attr = microide::terminal::cell_attr;
  TerminalLine line;
  line.cells.resize(7);
  const char* text = "abcdefg";
  for (std::size_t i = 0; i < 7; ++i) {
    line.cells[i].SetAscii(text[i]);
  }
  line.cells[2].style.attrs = attr::kBold;
  line.cells[3].style.attrs = attr::kBold;
  line.cells[4].style.attrs = attr::kItalic;
  line.cells[5].style.attrs = attr::kBold | attr::kItalic;
  TerminalRowPaintPlan plan;
  PlanTerminalRow(line, 10, TerminalRowEmphasis{}, Palette(), plan);
  Expect(plan.text_runs.size() == 5, "five runs: " + std::to_string(plan.text_runs.size()));
  if (plan.text_runs.size() != 5) {
    return;
  }
  const auto& r = plan.text_runs;
  Expect(r[0].face == FontStyle::Regular && plan.RunText(r[0]) == "ab" && r[0].column == 0 && r[0].end == 2,
         "the regular run");
  Expect(r[1].face == FontStyle::Bold && plan.RunText(r[1]) == "cd" && r[1].column == 2 && r[1].end == 4,
         "bold, with the cells it covers");
  Expect(r[2].face == FontStyle::Italic && plan.RunText(r[2]) == "e", "italic");
  Expect(r[3].face == FontStyle::BoldItalic && plan.RunText(r[3]) == "f", "bold italic");
  Expect(r[4].face == FontStyle::Regular && plan.RunText(r[4]) == "g" && r[4].end == 7, "and regular again");
}

void TestRowSelectionEmphasisMatchesContainsCell() {
  std::mt19937 rng(20261105);
  std::uniform_int_distribution<std::size_t> pick_row(0, 6);
  std::uniform_int_distribution<std::size_t> pick_column(0, 12);
  for (int trial = 0; trial < 400; ++trial) {
    auto a = workspace::TerminalSelectionPoint{pick_row(rng), pick_column(rng)};
    auto b = workspace::TerminalSelectionPoint{pick_row(rng), pick_column(rng)};
    const auto bounds = workspace::NormalizeTerminalSelection(a, b);
    for (std::size_t row = 0; row <= 7; ++row) {
      const TerminalRowEmphasis emphasis = TerminalRowSelectionEmphasis(bounds, row);
      for (std::size_t column = 0; column < 14; ++column) {
        const bool painted = column >= emphasis.selection_begin && column < emphasis.selection_end;
        if (painted != workspace::TerminalSelectionContainsCell(*bounds, row, column)) {
          Expect(false, "row " + std::to_string(row) + " column " + std::to_string(column) +
                            " disagrees with TerminalSelectionContainsCell");
          return;
        }
      }
      Expect(emphasis.selection_runs_past_row == (row >= bounds->start.row && row < bounds->end.row),
             "the line break is selected exactly on rows the selection continues past");
    }
  }
  Expect(TerminalRowSelectionEmphasis(std::nullopt, 3).selection_begin ==
             TerminalRowSelectionEmphasis(std::nullopt, 3).selection_end,
         "no selection, no selected columns");
}

}  // namespace

void RegisterTerminalRowPaintTests(std::vector<TestCase>& tests) {
  AddTest(tests, "TerminalRowPaint/SelectedTextKeepsItsOwnColour", TestSelectedTextKeepsItsOwnColour);
  AddTest(tests, "TerminalRowPaint/UnderlinesTakeTheirColourAndShape", TestUnderlinesTakeTheirColourAndShape);
  AddTest(tests, "TerminalRowPaint/TextRunsSplitOnTheFace", TestTextRunsSplitOnTheFace);
  AddTest(tests, "TerminalRowPaint/SelectionWashesAColouredBackground",
          TestSelectionWashesAColouredBackground);
  AddTest(tests, "TerminalRowPaint/PlainRowEmitsNoFills", TestPlainRowEmitsNoFills);
  AddTest(tests, "TerminalRowPaint/SelectionRunningPastARowWashesToTheEdge",
          TestSelectionRunningPastARowWashesToTheEdge);
  AddTest(tests, "TerminalRowPaint/EmphasisKeepsHiddenHiddenAndLowContrastLegible",
          TestEmphasisKeepsHiddenHiddenAndLowContrastLegible);
  AddTest(tests, "TerminalRowPaint/WideGlyphIsEmphasizedWithItsLead", TestWideGlyphIsEmphasizedWithItsLead);
  AddTest(tests, "TerminalRowPaint/FindMatchesWashAndSelectionOutranksThem",
          TestFindMatchesWashAndSelectionOutranksThem);
  AddTest(tests, "TerminalRowPaint/RowSelectionEmphasisMatchesContainsCell",
          TestRowSelectionEmphasisMatchesContainsCell);
}

}  // namespace microide::tests
