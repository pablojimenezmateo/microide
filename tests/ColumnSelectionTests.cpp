// Keyboard column (box) selection — VSCode's Ctrl+Shift+Alt+Arrow.
//
// Mouse box selection (Shift+Alt+drag) already worked and TextViewport::SetBoxSelection
// already handled per-line clamping and the caret-span cap. The keyboard half did not
// exist, so a rectangular selection was unreachable without a pointer.
//
// Two layers here: the arrow arithmetic (StepColumnSelection, pure) and the carets it
// produces once fed through the viewport.

#include "TestSupport.h"

#include "editor/ColumnSelection.h"
#include "editor/TextViewport.h"

#include <string>
#include <vector>

namespace microide::tests {
namespace {

using microide::editor::ColumnSelectDirection;
using microide::editor::ColumnSelectionState;
using microide::editor::StepColumnSelection;
using microide::editor::TextPosition;
using microide::editor::TextViewport;

constexpr std::size_t kNoColumnLimit = 1000;

// Ragged on purpose: a box over lines of differing length is the case that
// distinguishes a virtual column from a clamped one.
TextViewport MakeRaggedViewport() {
  TextViewport viewport;
  viewport.LoadContent(
      "alpha bravo charlie\n"  // 19
      "de\n"                   // 2
      "\n"                     // 0
      "foxtrot golf hotel\n"   // 18
      "ix",                    // 2
      "/tmp/column-selection-fixture.txt");
  return viewport;
}

void TestFirstStepAnchorsAtTheCaret() {
  const ColumnSelectionState stepped = StepColumnSelection(
      ColumnSelectionState{}, ColumnSelectDirection::Down, TextPosition{2, 5}, 10, kNoColumnLimit);
  Expect(stepped.active, "the first step should activate the gesture");
  Expect(stepped.anchor.line == 2 && stepped.anchor.column == 5,
         "the anchor should be captured at the caret");
  Expect(stepped.cursor.line == 3 && stepped.cursor.column == 5,
         "the cursor should have moved one line down from the caret");
}

// Once active the gesture must ignore the live caret: SetBoxSelection moves the
// primary caret to a clamped column, and re-reading it as the anchor would make the
// box collapse as soon as it crossed a short line.
void TestSubsequentStepsIgnoreTheLiveCaret() {
  ColumnSelectionState state{.active = true,
                             .anchor = TextPosition{1, 8},
                             .cursor = TextPosition{3, 8}};
  const ColumnSelectionState stepped = StepColumnSelection(
      state, ColumnSelectDirection::Down, TextPosition{99, 0}, 10, kNoColumnLimit);
  Expect(stepped.anchor.line == 1 && stepped.anchor.column == 8,
         "an active gesture must keep its original anchor");
  Expect(stepped.cursor.line == 4 && stepped.cursor.column == 8,
         "an active gesture should step its own cursor, not the caret");
}

void TestVerticalMotionSaturatesAtTheDocumentEdges() {
  ColumnSelectionState at_top{.active = true, .anchor = {}, .cursor = TextPosition{0, 3}};
  Expect(StepColumnSelection(at_top, ColumnSelectDirection::Up, {}, 5, kNoColumnLimit)
                 .cursor.line == 0,
         "Up at line 0 should stay at line 0, not wrap to the end of the file");

  ColumnSelectionState at_bottom{.active = true, .anchor = {}, .cursor = TextPosition{4, 3}};
  Expect(StepColumnSelection(at_bottom, ColumnSelectDirection::Down, {}, 5, kNoColumnLimit)
                 .cursor.line == 4,
         "Down on the last line should stay on the last line");
}

void TestHorizontalMotionSaturatesAtColumnZeroAndTheLongestLine() {
  ColumnSelectionState at_left{.active = true, .anchor = {}, .cursor = TextPosition{1, 0}};
  Expect(StepColumnSelection(at_left, ColumnSelectDirection::Left, {}, 5, kNoColumnLimit)
                 .cursor.column == 0,
         "Left at column 0 should stay at column 0 rather than underflow");

  // The cap is what stops Right running forever over a document of short lines.
  ColumnSelectionState at_limit{.active = true, .anchor = {}, .cursor = TextPosition{1, 19}};
  Expect(StepColumnSelection(at_limit, ColumnSelectDirection::Right, {}, 5, /*max_column=*/19)
                 .cursor.column == 19,
         "Right should not grow the virtual column past the longest line in the box");
}

// The virtual column is the whole point: crossing a two-character line must not
// permanently narrow the box.
void TestVirtualColumnSurvivesShortLines() {
  ColumnSelectionState state{.active = true,
                             .anchor = TextPosition{0, 15},
                             .cursor = TextPosition{0, 15}};
  state = StepColumnSelection(state, ColumnSelectDirection::Down, {}, 5, kNoColumnLimit);
  Expect(state.cursor.line == 1 && state.cursor.column == 15,
         "crossing a short line should keep the virtual column");
  state = StepColumnSelection(state, ColumnSelectDirection::Down, {}, 5, kNoColumnLimit);
  state = StepColumnSelection(state, ColumnSelectDirection::Down, {}, 5, kNoColumnLimit);
  Expect(state.cursor.line == 3 && state.cursor.column == 15,
         "the virtual column should be intact on reaching a long line again");
}

void TestMaxLineLengthInSpanFindsTheLongestLine() {
  TextViewport viewport = MakeRaggedViewport();
  Expect(viewport.MaxLineLengthInSpan(1, 2) == 2,
         "a span of short lines should report the short maximum");
  Expect(viewport.MaxLineLengthInSpan(0, 4) == 19,
         "a whole-document span should report the longest line");
  Expect(viewport.MaxLineLengthInSpan(4, 0) == 19, "reversed bounds should be normalized");
  Expect(viewport.MaxLineLengthInSpan(0, 99) == 19, "an out-of-range span should be clamped");
}

// End to end: the stepped state fed through SetBoxSelection must produce one caret
// per spanned line, with short lines collapsing instead of dropping out.
void TestSteppedStateProducesOneCaretPerSpannedLine() {
  TextViewport viewport = MakeRaggedViewport();
  viewport.MoveCursorTo(0, 6, false);

  ColumnSelectionState state;
  for (int i = 0; i < 3; ++i) {
    state = StepColumnSelection(state, ColumnSelectDirection::Down,
                                TextPosition{viewport.cursor_line(), viewport.cursor_column()},
                                viewport.line_count(), kNoColumnLimit);
  }
  viewport.SetBoxSelection(state.anchor, state.cursor);

  Expect(state.cursor.line == 3, "three Down steps from line 0 should land on line 3");
  // Four lines spanned, one of which holds the primary caret.
  Expect(viewport.secondary_carets().size() == 3,
         "a four-line box should leave three secondary carets plus the primary");
  Expect(viewport.cursor_line() == 3, "the primary caret should sit on the moving corner");

  // The empty line 2 has nowhere to put column 6; it must still carry a caret, at 0.
  bool saw_empty_line_caret = false;
  for (const TextPosition& caret : viewport.secondary_carets()) {
    if (caret.line == 2) {
      saw_empty_line_caret = true;
      Expect(caret.column == 0, "a caret on an empty line should clamp to column 0");
    }
  }
  Expect(saw_empty_line_caret, "the empty line inside the box must still get a caret");
}

void TestViewportStoresAndClearsColumnSelection() {
  TextViewport viewport = MakeRaggedViewport();
  Expect(!viewport.column_selection().active,
         "a fresh viewport should have no column selection in progress");

  viewport.SetColumnSelection(ColumnSelectionState{
      .active = true, .anchor = TextPosition{0, 1}, .cursor = TextPosition{2, 4}});
  Expect(viewport.column_selection().active && viewport.column_selection().cursor.line == 2,
         "the viewport should hold the column selection state it was given");

  viewport.ClearColumnSelection();
  Expect(!viewport.column_selection().active,
         "clearing should end the gesture so the next chord re-anchors");
}

}  // namespace

void RegisterColumnSelectionTests(std::vector<TestCase>& tests) {
  AddTest(tests, "ColumnSelection/FirstStepAnchorsAtTheCaret", TestFirstStepAnchorsAtTheCaret);
  AddTest(tests, "ColumnSelection/SubsequentStepsIgnoreTheLiveCaret",
          TestSubsequentStepsIgnoreTheLiveCaret);
  AddTest(tests, "ColumnSelection/VerticalMotionSaturates",
          TestVerticalMotionSaturatesAtTheDocumentEdges);
  AddTest(tests, "ColumnSelection/HorizontalMotionSaturates",
          TestHorizontalMotionSaturatesAtColumnZeroAndTheLongestLine);
  AddTest(tests, "ColumnSelection/VirtualColumnSurvivesShortLines",
          TestVirtualColumnSurvivesShortLines);
  AddTest(tests, "ColumnSelection/MaxLineLengthInSpan", TestMaxLineLengthInSpanFindsTheLongestLine);
  AddTest(tests, "ColumnSelection/ProducesOneCaretPerSpannedLine",
          TestSteppedStateProducesOneCaretPerSpannedLine);
  AddTest(tests, "ColumnSelection/ViewportStoresAndClears",
          TestViewportStoresAndClearsColumnSelection);
}

}  // namespace microide::tests
