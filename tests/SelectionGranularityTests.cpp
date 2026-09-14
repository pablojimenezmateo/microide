// Word- and line-granular drag selection: the behaviour a double- or
// triple-click hands to the drag that follows it.
//
// This file had no coverage at all, and the module's whole contract is a
// negative one -- a granular drag must never shrink below the unit the
// initiating click selected, in either direction of travel. That is invisible in
// a unit test of the viewport and only shows up as a selection that "sticks"
// when you drag back over the word you started on.

#include "TestSupport.h"

#include <string>
#include <vector>

#include "editor/TextViewport.h"
#include "workspace/coordinators/SelectionGranularity.h"

namespace microide::tests {
namespace {

using microide::editor::TextPosition;
using microide::editor::TextViewport;
using microide::workspace::InteractionState;
namespace granularity = microide::workspace::selection_granularity;

TextViewport MakeViewport(std::string_view content) {
  TextViewport viewport;
  viewport.SetViewportSize(20, 80);
  viewport.LoadContent(content, "/tmp/selection-granularity.txt");
  return viewport;
}

std::string Selected(const TextViewport& viewport) { return viewport.SelectedText(); }

// A double-click seeds Word granularity; a triple-click seeds Line; a single
// click leaves Character, which is what keeps a plain drag from snapping.
void TestClickCountChoosesTheGranularity() {
  TextViewport viewport = MakeViewport("alpha bravo\ncharlie\n");
  InteractionState state;

  viewport.MoveCursorTo(0, 7);
  granularity::ApplyClick(state, viewport, 1);
  Expect(!granularity::DragIsGranular(state), "a single click drags by characters");
  Expect(Selected(viewport).empty(), "a single click selects nothing on its own");

  granularity::ApplyClick(state, viewport, 2);
  Expect(granularity::DragIsGranular(state), "a double click drags by words");
  Expect(Selected(viewport) == "bravo", "a double click selects the word under the caret");

  granularity::ApplyClick(state, viewport, 3);
  Expect(granularity::DragIsGranular(state), "a triple click drags by lines");
  // Through the start of the next line, as VS Code's does -- so Delete after a
  // triple-click removes the line rather than blanking it.
  Expect(Selected(viewport) == "alpha bravo\n", "a triple click selects the line");
}

// Dragging forward from a double-clicked word covers whole words, and the
// selection still starts at the seed's start -- it may not creep forward off the
// word the click selected.
void TestWordDragForwardKeepsTheSeedStart() {
  TextViewport viewport = MakeViewport("alpha bravo charlie\n");
  InteractionState state;
  viewport.MoveCursorTo(0, 0);
  granularity::ApplyClick(state, viewport, 2);
  Expect(Selected(viewport) == "alpha", "the click seeds on alpha");

  // Pointer lands in the middle of "charlie".
  granularity::ExtendToPointer(state, viewport, TextPosition{0, 15});
  Expect(Selected(viewport) == "alpha bravo charlie",
         "the drag covers whole words from the seed start, got: " + Selected(viewport));
}

// Dragging BACKWARD flips the active end to the seed's end, so the word the
// click selected stays inside the selection instead of being eaten into.
void TestWordDragBackwardAnchorsAtTheSeedEnd() {
  TextViewport viewport = MakeViewport("alpha bravo charlie\n");
  InteractionState state;
  viewport.MoveCursorTo(0, 12);
  granularity::ApplyClick(state, viewport, 2);
  Expect(Selected(viewport) == "charlie", "the click seeds on charlie");

  granularity::ExtendToPointer(state, viewport, TextPosition{0, 2});
  Expect(Selected(viewport) == "alpha bravo charlie",
         "dragging back still contains the seed word, got: " + Selected(viewport));
}

// The unit under the pointer is the same one a double-click there would take, so
// a drag that reaches an operator run covers the run. Using the identifier-only
// rule here degraded the drag to one character a step the moment it left a word.
void TestWordDragCoversAnOperatorRun() {
  TextViewport viewport = MakeViewport("foo === bar\n");
  InteractionState state;
  viewport.MoveCursorTo(0, 0);
  granularity::ApplyClick(state, viewport, 2);
  Expect(Selected(viewport) == "foo", "the click seeds on foo");

  granularity::ExtendToPointer(state, viewport, TextPosition{0, 5});
  Expect(Selected(viewport) == "foo ===",
         "the drag takes the whole === run, got: " + Selected(viewport));
}

// Line granularity spans whole lines in both directions.
void TestLineDragCoversWholeLines() {
  TextViewport viewport = MakeViewport("one\ntwo\nthree\nfour\n");
  InteractionState state;
  viewport.MoveCursorTo(1, 1);
  granularity::ApplyClick(state, viewport, 3);
  Expect(Selected(viewport) == "two\n", "the click seeds on line two");

  // End-exclusive at the start of the line after the pointer's, so a downward
  // line drag carries the newlines and a delete removes whole lines rather than
  // leaving blanks behind (LineRangeAt).
  granularity::ExtendToPointer(state, viewport, TextPosition{3, 2});
  Expect(Selected(viewport) == "two\nthree\nfour\n",
         "the drag covers whole lines downward, got: <" + Selected(viewport) + ">");

  granularity::ExtendToPointer(state, viewport, TextPosition{0, 1});
  Expect(Selected(viewport) == "one\ntwo\n",
         "dragging above the seed keeps the seed line, got: <" + Selected(viewport) + ">");
}

// A double-click with nothing selectable under it must NOT leave a stale seed
// behind: the granularity stays Character, so the drag does not snap to a word
// the user never clicked.
void TestClickWithNothingUnderItLeavesCharacterGranularity() {
  TextViewport viewport = MakeViewport("alpha\n\nbravo\n");
  InteractionState state;

  viewport.MoveCursorTo(0, 2);
  granularity::ApplyClick(state, viewport, 2);
  Expect(granularity::DragIsGranular(state), "the first click seeds a word");

  // An empty line has no word and no run.
  viewport.MoveCursorTo(1, 0);
  granularity::ApplyClick(state, viewport, 2);
  Expect(!granularity::DragIsGranular(state),
         "a double click on an empty line falls back to character granularity");
}

// Triple-click then Delete removes the line, rather than leaving a blank one
// where it was: the whole point of carrying the terminator.
void TestTripleClickDeleteRemovesTheLine() {
  TextViewport viewport = MakeViewport("one\ntwo\nthree\n");
  InteractionState state;
  viewport.MoveCursorTo(1, 1);
  granularity::ApplyClick(state, viewport, 3);
  Expect(viewport.DeleteSelectedText(), "the line selection deletes");
  Expect(viewport.lines().size() == 3 && viewport.lines()[0] == "one" &&
             viewport.lines()[1] == "three",
         "the line is gone, not blanked");
}

// The last line has no next line to end at, so it selects to end-of-line and a
// delete there leaves the buffer's final empty line alone.
void TestTripleClickOnTheLastLineStopsAtEndOfLine() {
  TextViewport viewport = MakeViewport("one\ntwo");
  InteractionState state;
  viewport.MoveCursorTo(1, 1);
  granularity::ApplyClick(state, viewport, 3);
  Expect(Selected(viewport) == "two",
         "the last line selects without a terminator it does not have, got: <" +
             Selected(viewport) + ">");
}

}  // namespace

void RegisterSelectionGranularityTests(std::vector<TestCase>& tests) {
  AddTest(tests, "SelectionGranularity/ClickCountChoosesTheGranularity",
          TestClickCountChoosesTheGranularity);
  AddTest(tests, "SelectionGranularity/WordDragForwardKeepsTheSeedStart",
          TestWordDragForwardKeepsTheSeedStart);
  AddTest(tests, "SelectionGranularity/WordDragBackwardAnchorsAtTheSeedEnd",
          TestWordDragBackwardAnchorsAtTheSeedEnd);
  AddTest(tests, "SelectionGranularity/WordDragCoversAnOperatorRun",
          TestWordDragCoversAnOperatorRun);
  AddTest(tests, "SelectionGranularity/LineDragCoversWholeLines", TestLineDragCoversWholeLines);
  AddTest(tests, "SelectionGranularity/ClickWithNothingUnderItLeavesCharacterGranularity",
          TestClickWithNothingUnderItLeavesCharacterGranularity);
  AddTest(tests, "SelectionGranularity/TripleClickDeleteRemovesTheLine",
          TestTripleClickDeleteRemovesTheLine);
  AddTest(tests, "SelectionGranularity/TripleClickOnTheLastLineStopsAtEndOfLine",
          TestTripleClickOnTheLastLineStopsAtEndOfLine);
}

}  // namespace microide::tests
