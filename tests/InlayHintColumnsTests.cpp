#include "TestSupport.h"

#include "editor/InlayHintColumns.h"

#include <cstddef>
#include <limits>
#include <vector>

namespace microide::tests {
namespace {

using microide::editor::InlayCellSpan;
using microide::editor::InlayRowDisplacement;

constexpr std::size_t kNoAnchor = std::numeric_limits<std::size_t>::max();

void TestEmptyIsIdentity() {
  InlayRowDisplacement d;
  Expect(d.empty(), "default-constructed displacement is empty");
  Expect(d.CellsInsertedBefore(0) == 0, "no cells before column 0");
  Expect(d.CellsInsertedBefore(1000) == 0, "no cells before a far column");
  Expect(d.TotalInsertedCells() == 0, "no total cells");
  Expect(d.NextAnchorAtOrAfter(0) == kNoAnchor, "no next anchor");
  // Identity inverse: display column maps straight to the same visual column.
  Expect(d.VisualColumnForDisplayColumn(0) == 0, "identity inverse at 0");
  Expect(d.VisualColumnForDisplayColumn(7) == 7, "identity inverse at 7");
}

void TestCellsInsertedBeforeCountsAnchorAtColumn() {
  // A single hint of width 5 anchored before column 4.
  const std::vector<InlayCellSpan> spans{{.anchor_visual_column = 4, .cell_width = 5}};
  InlayRowDisplacement d(spans);
  Expect(!d.empty(), "displacement with a span is not empty");
  Expect(d.CellsInsertedBefore(3) == 0, "columns left of the anchor are unshifted");
  // The hint anchored AT 4 precedes the glyph at 4, so glyph 4 IS shifted.
  Expect(d.CellsInsertedBefore(4) == 5, "the anchored glyph is shifted by the hint");
  Expect(d.CellsInsertedBefore(9) == 5, "columns right of the anchor carry the shift");
  Expect(d.TotalInsertedCells() == 5, "total equals the single hint width");
}

void TestMultipleHintsAccumulate() {
  const std::vector<InlayCellSpan> spans{
      {.anchor_visual_column = 2, .cell_width = 3},
      {.anchor_visual_column = 6, .cell_width = 4},
      {.anchor_visual_column = 6, .cell_width = 1},  // two hints at the same column
  };
  InlayRowDisplacement d(spans);
  Expect(d.CellsInsertedBefore(1) == 0, "before the first anchor: 0");
  Expect(d.CellsInsertedBefore(2) == 3, "at the first anchor: 3");
  Expect(d.CellsInsertedBefore(5) == 3, "between anchors: 3");
  Expect(d.CellsInsertedBefore(6) == 8, "at the second column: 3 + 4 + 1");
  Expect(d.TotalInsertedCells() == 8, "total across all hints");
}

void TestNextAnchorAtOrAfter() {
  const std::vector<InlayCellSpan> spans{
      {.anchor_visual_column = 3, .cell_width = 2},
      {.anchor_visual_column = 9, .cell_width = 2},
  };
  InlayRowDisplacement d(spans);
  Expect(d.NextAnchorAtOrAfter(0) == 3, "first anchor from 0");
  Expect(d.NextAnchorAtOrAfter(3) == 3, "inclusive at the anchor");
  Expect(d.NextAnchorAtOrAfter(4) == 9, "skips to the next anchor");
  Expect(d.NextAnchorAtOrAfter(10) == kNoAnchor, "past the last anchor -> none");
}

void TestInverseRoundTripsRealColumns() {
  // Hint width 3 before column 4, hint width 2 before column 8.
  const std::vector<InlayCellSpan> spans{
      {.anchor_visual_column = 4, .cell_width = 3},
      {.anchor_visual_column = 8, .cell_width = 2},
  };
  InlayRowDisplacement d(spans);
  // For every real column, display(v) = v + CellsInsertedBefore(v) must invert
  // back to exactly v (a click on the real glyph lands on the real column).
  for (std::size_t v = 0; v <= 12; ++v) {
    const std::size_t display = v + d.CellsInsertedBefore(v);
    Expect(d.VisualColumnForDisplayColumn(display) == v,
           "display(v) inverts to the real column v");
  }
}

void TestInverseSnapsInsidePhantomRegionToAnchor() {
  // One hint of width 3 before column 4. Real glyph 3 is at display 3; the hint
  // occupies display cells 4,5,6; the annotated glyph (real column 4) is at
  // display 7. Any click on display 4/5/6 must snap to the anchor column 4.
  const std::vector<InlayCellSpan> spans{{.anchor_visual_column = 4, .cell_width = 3}};
  InlayRowDisplacement d(spans);
  Expect(d.VisualColumnForDisplayColumn(3) == 3, "left of the hint stays at 3");
  Expect(d.VisualColumnForDisplayColumn(4) == 4, "phantom cell -> anchor 4");
  Expect(d.VisualColumnForDisplayColumn(5) == 4, "phantom cell -> anchor 4");
  Expect(d.VisualColumnForDisplayColumn(6) == 4, "phantom cell -> anchor 4");
  Expect(d.VisualColumnForDisplayColumn(7) == 4, "the annotated glyph is at display 7");
  Expect(d.VisualColumnForDisplayColumn(8) == 5, "the next glyph follows");
}

}  // namespace

// TD-2026-07-17-070: inlay hints are external plugin data. The displacement
// accumulators must saturate rather than wrap std::size_t so hit-testing /
// display-column mapping stays monotonic even for pathological hint widths.
void TestDisplacementAccumulatorsSaturate() {
  constexpr std::size_t kMax = std::numeric_limits<std::size_t>::max();
  const std::vector<InlayCellSpan> spans{
      {.anchor_visual_column = 2, .cell_width = kMax - 10},
      {.anchor_visual_column = 4, .cell_width = 1000},  // would wrap without saturation
  };
  InlayRowDisplacement d(spans);
  Expect(d.TotalInsertedCells() == kMax,
         "total inserted cells saturate to size_t max instead of wrapping to a tiny value");
  Expect(d.CellsInsertedBefore(4) == kMax,
         "cells-before at the second anchor saturates rather than wrapping");
  // Monotonic: a later column is never reported as having fewer inserted cells.
  Expect(d.CellsInsertedBefore(2) <= d.CellsInsertedBefore(4),
         "inserted-cell count stays monotonic across columns");
}

void RegisterInlayHintColumnsTests(std::vector<TestCase>& tests) {
  AddTest(tests, "InlayHintColumns/DisplacementAccumulatorsSaturate",
          TestDisplacementAccumulatorsSaturate);
  AddTest(tests, "InlayHintColumns/EmptyIsIdentity", TestEmptyIsIdentity);
  AddTest(tests, "InlayHintColumns/CellsInsertedBeforeCountsAnchorAtColumn",
          TestCellsInsertedBeforeCountsAnchorAtColumn);
  AddTest(tests, "InlayHintColumns/MultipleHintsAccumulate", TestMultipleHintsAccumulate);
  AddTest(tests, "InlayHintColumns/NextAnchorAtOrAfter", TestNextAnchorAtOrAfter);
  AddTest(tests, "InlayHintColumns/InverseRoundTripsRealColumns",
          TestInverseRoundTripsRealColumns);
  AddTest(tests, "InlayHintColumns/InverseSnapsInsidePhantomRegionToAnchor",
          TestInverseSnapsInsidePhantomRegionToAnchor);
}

}  // namespace microide::tests
