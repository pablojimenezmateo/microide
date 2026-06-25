#include "TestSupport.h"

#include "editor/EditorRowYLayout.h"

#include <cmath>
#include <vector>

namespace microide::tests {
namespace {

using microide::editor::EditorRowYLayout;
using microide::editor::RowGap;

bool Near(float a, float b) { return std::fabs(a - b) < 0.001f; }

void TestNoGapsMatchesLegacyFormula() {
  // The whole point of the helper: with no insets it must be bit-identical to the
  // old `first_line_y + row*line_height` math the renderer used to inline.
  const float first = 40.0f;
  const float lh = 14.0f;
  const EditorRowYLayout layout(first, lh, /*scroll_line=*/5);
  for (std::size_t row = 0; row < 10; ++row) {
    Expect(Near(layout.RowTop(row), first + static_cast<float>(row) * lh),
           "RowTop must equal the legacy formula when there are no gaps");
    Expect(layout.GapHeightBelow(row) == 0.0f, "no gap height without gaps");
  }
  Expect(!layout.has_gaps(), "layout reports no gaps");
}

void TestGapShiftsRowsBelowIt() {
  const float first = 40.0f;
  const float lh = 14.0f;
  const std::uint32_t scroll = 5;
  // A 20px inset sits below visual row 6 (the second visible row).
  const std::vector<RowGap> gaps{RowGap{.visual_row = 6, .height = 20.0f}};
  const EditorRowYLayout layout(first, lh, scroll, gaps);

  Expect(Near(layout.RowTop(0), first), "row 0 (visual 5) is above the gap, unshifted");
  Expect(Near(layout.RowTop(1), first + lh), "row 1 (visual 6) is the gap's anchor, unshifted");
  Expect(Near(layout.RowTop(2), first + 2 * lh + 20.0f), "row 2 (visual 7) is pushed down by the gap");
  Expect(Near(layout.RowTop(3), first + 3 * lh + 20.0f), "rows further down keep the offset");

  Expect(Near(layout.GapHeightBelow(1), 20.0f), "the gap is below row offset 1");
  Expect(layout.GapHeightBelow(0) == 0.0f, "no gap below row 0");
  Expect(layout.GapHeightBelow(2) == 0.0f, "no gap below row 2");
}

void TestHitTestDistinguishesGap() {
  const float first = 40.0f;
  const float lh = 14.0f;
  const std::uint32_t scroll = 0;
  const std::vector<RowGap> gaps{RowGap{.visual_row = 0, .height = 20.0f}};
  const EditorRowYLayout layout(first, lh, scroll, gaps);

  // Inside row 0's text band.
  EditorRowYLayout::HitResult hit = layout.HitTest(first + 2.0f, /*visible_rows=*/5);
  Expect(hit.row == 0 && !hit.in_gap, "a click on the text row maps to that row, not the gap");

  // Inside the inert gap below row 0.
  hit = layout.HitTest(first + lh + 5.0f, 5);
  Expect(hit.row == 0 && hit.in_gap, "a click in the inert gap is reported as in_gap");

  // On the next text row.
  hit = layout.HitTest(first + lh + 20.0f + 2.0f, 5);
  Expect(hit.row == 1 && !hit.in_gap, "a click past the gap maps to the following row");
}

void TestWindowHeightIncludesGaps() {
  const float lh = 10.0f;
  const std::vector<RowGap> gaps{RowGap{.visual_row = 1, .height = 30.0f}};
  const EditorRowYLayout layout(0.0f, lh, 0, gaps);
  // 4 rows * 10 + one 30px gap = 70.
  Expect(Near(layout.WindowHeight(4), 70.0f), "window height folds in the visible gaps");
}

}  // namespace

void RegisterEditorRowYLayoutTests(std::vector<TestCase>& tests) {
  AddTest(tests, "EditorRowYLayout/NoGapsMatchesLegacyFormula", TestNoGapsMatchesLegacyFormula);
  AddTest(tests, "EditorRowYLayout/GapShiftsRowsBelowIt", TestGapShiftsRowsBelowIt);
  AddTest(tests, "EditorRowYLayout/HitTestDistinguishesGap", TestHitTestDistinguishesGap);
  AddTest(tests, "EditorRowYLayout/WindowHeightIncludesGaps", TestWindowHeightIncludesGaps);
}

}  // namespace microide::tests
