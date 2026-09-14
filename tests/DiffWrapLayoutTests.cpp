// The row table behind soft wrap on the compare and merge surfaces.
//
// Those surfaces align their panes row-for-row, which soft wrap breaks: one
// aligned unit occupies N rows on the left and M on the right. The table gives
// the unit max(N, M) rows and pads the shorter side with absent rows, so the
// panes stay locked together while each side wraps to its own width. Everything
// downstream -- gutters, diff markers, hit tests, the caret -- reads that
// padding through `left_present` / `right_present`, so a unit whose two sides
// disagree about their row count is the case worth enumerating.
//
// This is header-only arithmetic shared by both surfaces and had no direct
// tests: the checks here rebuild the expected rows straight from
// `TextLayout::WrapLineSegments`, which is itself property-tested against a
// partition/resume oracle in TextLayoutTests.

#include "TestSupport.h"

#include <cstdint>
#include <string>
#include <vector>

#include "editor/TextLayout.h"
#include "workspace/DiffWrapLayout.h"

namespace microide::tests {
namespace {

using microide::editor::TextLayout;
using microide::workspace::DiffWrapLayout;
using microide::workspace::DiffWrapRow;

struct Unit {
  std::string left;
  std::string right;
  bool has_left = true;
  bool has_right = true;
};

struct Segment {
  std::size_t start = 0;
  std::size_t end = 0;
  std::size_t indent = 0;
};

std::vector<Segment> SegmentsOf(std::string_view text, std::size_t tab_size, std::size_t columns) {
  std::vector<Segment> segments;
  TextLayout::WrapLineSegments(text, tab_size, columns,
                               [&](std::size_t start, std::size_t end, std::size_t indent) {
                                 segments.push_back(Segment{start, end, indent});
                               });
  return segments;
}

void Build(const DiffWrapLayout& layout,
           const std::vector<Unit>& units,
           std::size_t columns,
           std::size_t tab_size,
           std::uint64_t revision = 1) {
  layout.Ensure(revision, /*soft_wrap=*/true, columns, columns, tab_size, units.size(),
                [&](std::size_t unit) {
                  DiffWrapLayout::UnitText text;
                  text.has_left = units[unit].has_left;
                  text.has_right = units[unit].has_right;
                  text.left = units[unit].left;
                  text.right = units[unit].right;
                  return text;
                });
}

// Wrap off is the identity: no rows resident, every accessor answers from the
// unit index, and `RowAt` synthesizes a both-panes-present row so callers need
// no branch.
void TestInactiveLayoutIsTheIdentity() {
  DiffWrapLayout layout;
  layout.Ensure(1, /*soft_wrap=*/false, 40, 40, 4, 5,
                [](std::size_t) { return DiffWrapLayout::UnitText{}; });
  Expect(!layout.active(), "wrap off leaves the table inactive");
  Expect(layout.RowCount(5) == 5, "row count is the unit count");
  Expect(layout.FirstRowForUnit(3) == 3 && layout.UnitForRow(3) == 3,
         "unit and row indices coincide");
  Expect(layout.RowSpanForUnit(3) == 1, "every unit is one row");
  const DiffWrapRow row = layout.RowAt(2);
  Expect(row.unit == 2 && row.first && row.left_present && row.right_present,
         "the synthesized row has both panes");
}

// The core invariant, over units whose two sides wrap to different row counts in
// both directions, plus units present on one side only and an empty unit.
void TestRowsMatchTheSegmentsAndPadTheShorterSide() {
  const std::vector<Unit> units = {
      {"short", "a much longer right side that has to wrap several times", true, true},
      {"a much longer left side that has to wrap several times over", "short", true, true},
      {"same", "same", true, true},
      {"", "", true, true},
      {"left only", "", true, false},
      {"", "right only", false, true},
      {"\tindented line that wraps past the pane width and then some", "x", true, true},
  };
  for (const std::size_t columns : {4u, 7u, 12u, 40u}) {
    for (const std::size_t tab_size : {2u, 4u, 8u}) {
      DiffWrapLayout layout;
      Build(layout, units, columns, tab_size);
      const std::string context =
          " (columns " + std::to_string(columns) + ", tab " + std::to_string(tab_size) + ")";
      Expect(layout.active(), "wrap on builds a table" + context);

      std::size_t expected_row = 0;
      for (std::size_t unit = 0; unit < units.size(); ++unit) {
        const std::vector<Segment> left =
            units[unit].has_left ? SegmentsOf(units[unit].left, tab_size, columns)
                                 : std::vector<Segment>{};
        const std::vector<Segment> right =
            units[unit].has_right ? SegmentsOf(units[unit].right, tab_size, columns)
                                  : std::vector<Segment>{};
        const std::size_t span = std::max<std::size_t>(std::max(left.size(), right.size()), 1);
        const std::string unit_context = " unit " + std::to_string(unit) + context;

        Expect(layout.FirstRowForUnit(unit) == expected_row,
               "the unit starts where the previous one ended:" + unit_context);
        Expect(layout.RowSpanForUnit(unit) == span,
               "the unit spans max(left rows, right rows, 1):" + unit_context + " got " +
                   std::to_string(layout.RowSpanForUnit(unit)) + " want " + std::to_string(span));

        std::size_t first_flags = 0;
        for (std::size_t i = 0; i < span; ++i) {
          const DiffWrapRow row = layout.RowAt(expected_row + i);
          const std::string row_context = " row " + std::to_string(i) + " of" + unit_context;
          Expect(row.unit == unit, "the row names its unit:" + row_context);
          Expect(layout.UnitForRow(expected_row + i) == unit,
                 "UnitForRow agrees with the row:" + row_context);
          first_flags += row.first ? 1 : 0;

          Expect(row.left_present == (i < left.size()),
                 "the left pane is present exactly for its own rows:" + row_context);
          if (row.left_present) {
            Expect(row.left_start == left[i].start && row.left_end == left[i].end &&
                       row.left_indent == left[i].indent,
                   "the left segment matches the wrap:" + row_context);
          }
          Expect(row.right_present == (i < right.size()),
                 "the right pane is present exactly for its own rows:" + row_context);
          if (row.right_present) {
            Expect(row.right_start == right[i].start && row.right_end == right[i].end &&
                       row.right_indent == right[i].indent,
                   "the right segment matches the wrap:" + row_context);
          }
        }
        Expect(first_flags == 1,
               "exactly one row of the unit is its first:" + unit_context + " got " +
                   std::to_string(first_flags));
        expected_row += span;
      }
      Expect(layout.RowCount(units.size()) == expected_row,
             "the table holds exactly the rows the units asked for" + context);
    }
  }
}

// Column -> row within a unit: a column inside a segment lands on that segment's
// row, a column past end-of-line lands on the LAST row carrying that side (where
// the caret goes), and a side that ran out of rows falls back onto its own last
// row rather than onto a row it does not occupy.
void TestRowForUnitColumnLandsOnTheSegmentOrTheLastRowOfThatSide() {
  const std::vector<Unit> units = {
      {"aaaabbbbcccc", "dddd", true, true},
  };
  DiffWrapLayout layout;
  Build(layout, units, /*columns=*/4, /*tab_size=*/4);
  const std::size_t first = layout.FirstRowForUnit(0);
  Expect(layout.RowSpanForUnit(0) == 3, "the left side takes three rows, the right one");

  Expect(layout.RowForUnitColumn(0, 0, /*right_side=*/false) == first,
         "column 0 is on the first row");
  Expect(layout.RowForUnitColumn(0, 5, /*right_side=*/false) == first + 1,
         "a column in the second segment is on the second row");
  Expect(layout.RowForUnitColumn(0, 12, /*right_side=*/false) == first + 2,
         "a column at end-of-line falls back to the last row");
  Expect(layout.RowForUnitColumn(0, 99, /*right_side=*/false) == first + 2,
         "a column past the line falls back to the last row");

  Expect(layout.RowForUnitColumn(0, 2, /*right_side=*/true) == first,
         "the right side's only row holds its columns");
  Expect(layout.RowForUnitColumn(0, 99, /*right_side=*/true) == first,
         "a right column past the line stays on the row the right side occupies");
}

// A unit index or row index from a stale build must clamp into the table rather
// than index past it -- these are read from const geometry paths that can run
// between a content change and the rebuild that follows it.
void TestStaleIndicesClamp() {
  const std::vector<Unit> units = {{"a", "b", true, true}, {"cc", "dd", true, true}};
  DiffWrapLayout layout;
  Build(layout, units, /*columns=*/8, /*tab_size=*/4);
  const std::size_t rows = layout.RowCount(units.size());

  Expect(layout.FirstRowForUnit(99) < rows, "a stale unit index lands inside the table");
  Expect(layout.RowSpanForUnit(99) >= 1, "a stale unit index still spans at least one row");
  Expect(layout.UnitForRow(99) < units.size(), "a stale row index names a real unit");
  Expect(layout.RowForUnitColumn(99, 0, false) < rows, "a stale unit column lands inside");
}

// Rebuild keying: the table is rebuilt when the width, tab size, unit count or
// revision moves, and reused when none of them has.
void TestRebuildFollowsItsKey() {
  const std::vector<Unit> units = {{"aaaaaaaaaaaa", "aaaaaaaaaaaa", true, true}};
  DiffWrapLayout layout;
  Build(layout, units, /*columns=*/4, /*tab_size=*/4, /*revision=*/1);
  Expect(layout.RowSpanForUnit(0) == 3, "at width 4 the line takes three rows");
  Expect(layout.built_left_columns() == 4, "the table records the width it was built at");

  Build(layout, units, /*columns=*/16, /*tab_size=*/4, /*revision=*/1);
  Expect(layout.RowSpanForUnit(0) == 1, "a width change rebuilds even at the same revision");
  Expect(layout.built_left_columns() == 16, "the recorded width follows");

  // Wrap off resets, so nothing is resident while it is unused.
  layout.Ensure(1, /*soft_wrap=*/false, 16, 16, 4, units.size(),
                [](std::size_t) { return DiffWrapLayout::UnitText{}; });
  Expect(!layout.active() && layout.RowCount(7) == 7, "turning wrap off resets to the identity");
}

}  // namespace

void RegisterDiffWrapLayoutTests(std::vector<TestCase>& tests) {
  AddTest(tests, "DiffWrapLayout/InactiveLayoutIsTheIdentity", TestInactiveLayoutIsTheIdentity);
  AddTest(tests, "DiffWrapLayout/RowsMatchTheSegmentsAndPadTheShorterSide",
          TestRowsMatchTheSegmentsAndPadTheShorterSide);
  AddTest(tests, "DiffWrapLayout/RowForUnitColumnLandsOnTheSegmentOrTheLastRowOfThatSide",
          TestRowForUnitColumnLandsOnTheSegmentOrTheLastRowOfThatSide);
  AddTest(tests, "DiffWrapLayout/StaleIndicesClamp", TestStaleIndicesClamp);
  AddTest(tests, "DiffWrapLayout/RebuildFollowsItsKey", TestRebuildFollowsItsKey);
}

}  // namespace microide::tests
