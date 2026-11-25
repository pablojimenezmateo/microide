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
  Expect(Selected(viewport) == "alpha bravo\n", "a triple click selects the line");
}

void TestWordDragForwardKeepsTheSeedStart() {
  TextViewport viewport = MakeViewport("alpha bravo charlie\n");
  InteractionState state;
  viewport.MoveCursorTo(0, 0);
  granularity::ApplyClick(state, viewport, 2);
  Expect(Selected(viewport) == "alpha", "the click seeds on alpha");

  granularity::ExtendToPointer(state, viewport, TextPosition{0, 15});
  Expect(Selected(viewport) == "alpha bravo charlie",
         "the drag covers whole words from the seed start, got: " + Selected(viewport));
}

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

void TestLineDragCoversWholeLines() {
  TextViewport viewport = MakeViewport("one\ntwo\nthree\nfour\n");
  InteractionState state;
  viewport.MoveCursorTo(1, 1);
  granularity::ApplyClick(state, viewport, 3);
  Expect(Selected(viewport) == "two\n", "the click seeds on line two");

  granularity::ExtendToPointer(state, viewport, TextPosition{3, 2});
  Expect(Selected(viewport) == "two\nthree\nfour\n",
         "the drag covers whole lines downward, got: <" + Selected(viewport) + ">");

  granularity::ExtendToPointer(state, viewport, TextPosition{0, 1});
  Expect(Selected(viewport) == "one\ntwo\n",
         "dragging above the seed keeps the seed line, got: <" + Selected(viewport) + ">");
}

void TestClickWithNothingUnderItLeavesCharacterGranularity() {
  TextViewport viewport = MakeViewport("alpha\n\nbravo\n");
  InteractionState state;

  viewport.MoveCursorTo(0, 2);
  granularity::ApplyClick(state, viewport, 2);
  Expect(granularity::DragIsGranular(state), "the first click seeds a word");

  viewport.MoveCursorTo(1, 0);
  granularity::ApplyClick(state, viewport, 2);
  Expect(!granularity::DragIsGranular(state),
         "a double click on an empty line falls back to character granularity");
}

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

void TestTripleClickOnTheLastLineStopsAtEndOfLine() {
  TextViewport viewport = MakeViewport("one\ntwo");
  InteractionState state;
  viewport.MoveCursorTo(1, 1);
  granularity::ApplyClick(state, viewport, 3);
  Expect(Selected(viewport) == "two",
         "the last line selects without a terminator it does not have, got: <" +
             Selected(viewport) + ">");
}

}

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

}
