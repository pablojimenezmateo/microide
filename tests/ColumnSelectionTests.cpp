#include "TestSupport.h"

#include "WorkspaceShellEventHelpers.h"
#include "editor/ColumnSelection.h"
#include "editor/TextViewport.h"
#include "workspace/shell/WorkspaceShellTestAccess.h"

#include <string>
#include <vector>

namespace microide::tests {
namespace {

using microide::editor::ColumnSelectDirection;
using microide::editor::ColumnSelectionState;
using microide::editor::StepColumnSelection;
using microide::editor::TextPosition;
using microide::editor::TextViewport;
using ShellTestAccess = microide::workspace::WorkspaceShell::TestAccess;

constexpr std::size_t kNoColumnLimit = 1000;

TextViewport MakeRaggedViewport() {
  TextViewport viewport;
  viewport.LoadContent(
      "alpha bravo charlie\n"
      "de\n"
      "\n"
      "foxtrot golf hotel\n"
      "ix",
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

  ColumnSelectionState at_limit{.active = true, .anchor = {}, .cursor = TextPosition{1, 19}};
  Expect(StepColumnSelection(at_limit, ColumnSelectDirection::Right, {}, 5, 19)
                 .cursor.column == 19,
         "Right should not grow the virtual column past the longest line in the box");
}

void TestRightDoesNotDragTheVirtualColumnBackWhenTheSpanShrinks() {
  ColumnSelectionState wide{.active = true,
                            .anchor = TextPosition{0, 0},
                            .cursor = TextPosition{0, 30}};
  const ColumnSelectionState after =
      StepColumnSelection(wide, ColumnSelectDirection::Right, {}, 5, 4);
  Expect(after.cursor.column == 30,
         "Right over a span narrower than the virtual column should leave it alone, got " +
             std::to_string(after.cursor.column));

  ColumnSelectionState room{.active = true,
                            .anchor = TextPosition{0, 0},
                            .cursor = TextPosition{0, 30}};
  Expect(StepColumnSelection(room, ColumnSelectDirection::Right, {}, 5, 40)
                 .cursor.column == 31,
         "Right below the span width should still step one cell");
}

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

void TestMaxVisualWidthInSpanFindsTheLongestLine() {
  TextViewport viewport = MakeRaggedViewport();
  Expect(viewport.MaxVisualWidthInSpan(1, 2) == 2,
         "a span of short lines should report the short maximum");
  Expect(viewport.MaxVisualWidthInSpan(0, 4) == 19,
         "a whole-document span should report the longest line");
  Expect(viewport.MaxVisualWidthInSpan(4, 0) == 19, "reversed bounds should be normalized");
  Expect(viewport.MaxVisualWidthInSpan(0, 99) == 19, "an out-of-range span should be clamped");
}

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
  Expect(viewport.secondary_carets().size() == 3,
         "a four-line box should leave three secondary carets plus the primary");
  Expect(viewport.cursor_line() == 3, "the primary caret should sit on the moving corner");

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


void TestColumnSelectGestureEndsOnACompareTabToo() {
  TemporaryDirectory temp;
  const std::filesystem::path root = temp.path() / "repo";
  const std::filesystem::path file = root / "ragged.txt";
  WriteFile(file, "alpha bravo charlie\nde\nfoxtrot golf\n");
  InitializeGitRepo(root);
  CommitAll(root, "column selection fixture", "column selection fixture");
  WriteFile(file, "alpha BRAVO charlie\nde\nfoxtrot GOLF\n");

  microide::workspace::WorkspaceShell shell;
  ShellTestAccess::SetProjectRoot(shell, root);
  ShellTestAccess::SetWindowSize(shell, 1280, 720);
  Expect(ShellTestAccess::OpenWorkingTreeComparison(shell, file, "HEAD", "HEAD"),
         "working-tree comparison should open");

  auto& compare = ShellTestAccess::ActiveCompare(shell);
  Expect(compare.right_editable && compare.right_view_active,
         "the editable pane must be active, or the chord has nowhere to land");
  auto& viewport = compare.right_viewport;
  viewport.MoveCursorTo(0, 6, false);

  const SDL_Keymod chord =
      static_cast<SDL_Keymod>(SDL_KMOD_CTRL | SDL_KMOD_SHIFT | SDL_KMOD_ALT);
  Expect(SendKeyDown(shell, SDLK_DOWN, chord),
         "Ctrl+Shift+Alt+Down must be handled on a compare tab");
  Expect(viewport.column_selection().active,
         "the chord starts a column-selection gesture on the compare pane");

  Expect(SendKeyDown(shell, SDLK_DOWN, SDL_KMOD_NONE), "a plain Down should be handled");
  Expect(!viewport.column_selection().active,
         "ordinary caret movement must end the gesture on the compare pane too");
}

void TestColumnSelectChordDispatchesThroughTheRegistry() {
  TemporaryDirectory temp;
  const std::filesystem::path root = temp.path() / "project";
  const std::filesystem::path file = root / "ragged.txt";
  WriteFile(file, "alpha bravo charlie\nde\n\nfoxtrot golf hotel\nix\n");

  microide::workspace::WorkspaceShell shell;
  ShellTestAccess::SetProjectRoot(shell, root);
  ShellTestAccess::SetWindowSize(shell, 1280, 720);
  ShellTestAccess::OpenSingleEditorTab(shell, file);

  auto& viewport = ShellTestAccess::ActiveEditor(shell);
  viewport.MoveCursorTo(0, 6, false);
  Expect(!viewport.column_selection().active, "no gesture should be in progress yet");

  const SDL_Keymod chord =
      static_cast<SDL_Keymod>(SDL_KMOD_CTRL | SDL_KMOD_SHIFT | SDL_KMOD_ALT);
  Expect(SendKeyDown(shell, SDLK_DOWN, chord),
         "Ctrl+Shift+Alt+Down must be handled -- if this fails the binding is not "
         "registered and the README documents a shortcut the app does not have");

  Expect(viewport.column_selection().active,
         "the chord should start a column-selection gesture");
  Expect(viewport.column_selection().cursor.line == 1,
         "one Down step should move the moving corner one line down");
  Expect(viewport.has_multiple_carets(),
         "a two-line box should place a secondary caret");

  Expect(SendKeyDown(shell, SDLK_DOWN, chord), "the second chord press should be handled");
  Expect(viewport.column_selection().anchor.line == 0,
         "the anchor must stay put across steps");
  Expect(viewport.column_selection().cursor.line == 2,
         "the second step should extend the box, not restart it");

  Expect(SendKeyDown(shell, SDLK_DOWN, SDL_KMOD_NONE), "a plain Down should be handled");
  Expect(!viewport.column_selection().active,
         "ordinary caret movement must end the column-selection gesture");
}

void TestColumnSelectRightKeepsTheWidthAfterSteppingBackOffTheLongLine() {
  TemporaryDirectory temp;
  const std::filesystem::path root = temp.path() / "project";
  const std::filesystem::path file = root / "ragged.txt";
  WriteFile(file, "ab\ncd\na very long line that goes on\n");

  microide::workspace::WorkspaceShell shell;
  ShellTestAccess::SetProjectRoot(shell, root);
  ShellTestAccess::SetWindowSize(shell, 1280, 720);
  ShellTestAccess::OpenSingleEditorTab(shell, file);

  auto& viewport = ShellTestAccess::ActiveEditor(shell);
  viewport.MoveCursorTo(0, 0, false);

  const SDL_Keymod chord =
      static_cast<SDL_Keymod>(SDL_KMOD_CTRL | SDL_KMOD_SHIFT | SDL_KMOD_ALT);
  Expect(SendKeyDown(shell, SDLK_DOWN, chord) && SendKeyDown(shell, SDLK_DOWN, chord),
         "two Down chords should be handled");
  for (int i = 0; i < 10; ++i) {
    Expect(SendKeyDown(shell, SDLK_RIGHT, chord), "a Right chord should be handled");
  }
  Expect(viewport.column_selection().cursor.column == 10,
         "ten Right chords over the long line should reach column 10, got " +
             std::to_string(viewport.column_selection().cursor.column));

  Expect(SendKeyDown(shell, SDLK_UP, chord) && SendKeyDown(shell, SDLK_UP, chord),
         "two Up chords should be handled");
  Expect(viewport.column_selection().cursor.column == 10,
         "Up must keep the virtual column, got " +
             std::to_string(viewport.column_selection().cursor.column));

  Expect(SendKeyDown(shell, SDLK_RIGHT, chord), "the Right chord should be handled");
  Expect(viewport.column_selection().cursor.column == 10,
         "Right over a span narrower than the virtual column must not narrow the box, got " +
             std::to_string(viewport.column_selection().cursor.column));
}

void TestColumnSelectGestureEndsWhenSomebodyElseEditsTheBuffer() {
  microide::editor::TextViewport pane_a;
  pane_a.SetViewportSize(20, 200);
  pane_a.LoadContent("alpha\nbravo\ncharlie\ndelta\necho\n", "/tmp/column-sibling.txt");
  microide::editor::TextViewport pane_b = pane_a;
  pane_b.SetViewportSize(20, 200);

  pane_b.MoveCursorTo(4, 2);
  pane_b.SetColumnSelection(ColumnSelectionState{
      .active = true, .anchor = TextPosition{4, 2}, .cursor = TextPosition{4, 4}});
  Expect(pane_b.column_selection().active, "the gesture is armed on pane B");

  pane_b.SetColumnSelection(ColumnSelectionState{
      .active = true, .anchor = TextPosition{4, 2}, .cursor = TextPosition{4, 5}});
  Expect(pane_b.column_selection().active && pane_b.column_selection().cursor.column == 5,
         "a held chord keeps extending its own box");

  pane_a.MoveCursorTo(0, 0);
  pane_a.MoveCursorTo(3, 0, true);
  pane_a.Backspace();

  Expect(!pane_b.column_selection().active,
         "an edit the gesture did not make ends it, so the next chord re-anchors "
         "instead of extending a box from corners that are gone");
}

}

void RegisterColumnSelectionTests(std::vector<TestCase>& tests) {
  AddTest(tests, "ColumnSelection/GestureEndsWhenSomebodyElseEditsTheBuffer",
          TestColumnSelectGestureEndsWhenSomebodyElseEditsTheBuffer);
  AddTest(tests, "ColumnSelection/RightKeepsTheWidthAfterSteppingBackOffTheLongLine",
          TestColumnSelectRightKeepsTheWidthAfterSteppingBackOffTheLongLine);
  AddTest(tests, "ColumnSelection/FirstStepAnchorsAtTheCaret", TestFirstStepAnchorsAtTheCaret);
  AddTest(tests, "ColumnSelection/SubsequentStepsIgnoreTheLiveCaret",
          TestSubsequentStepsIgnoreTheLiveCaret);
  AddTest(tests, "ColumnSelection/VerticalMotionSaturates",
          TestVerticalMotionSaturatesAtTheDocumentEdges);
  AddTest(tests, "ColumnSelection/HorizontalMotionSaturates",
          TestHorizontalMotionSaturatesAtColumnZeroAndTheLongestLine);
  AddTest(tests, "ColumnSelection/VirtualColumnSurvivesShortLines",
          TestVirtualColumnSurvivesShortLines);
  AddTest(tests, "ColumnSelection/RightDoesNotDragTheVirtualColumnBackWhenTheSpanShrinks",
          TestRightDoesNotDragTheVirtualColumnBackWhenTheSpanShrinks);
  AddTest(tests, "ColumnSelection/MaxVisualWidthInSpan", TestMaxVisualWidthInSpanFindsTheLongestLine);
  AddTest(tests, "ColumnSelection/ProducesOneCaretPerSpannedLine",
          TestSteppedStateProducesOneCaretPerSpannedLine);
  AddTest(tests, "ColumnSelection/ViewportStoresAndClears",
          TestViewportStoresAndClearsColumnSelection);
  AddTest(tests, "ColumnSelection/ChordDispatchesThroughTheRegistry",
          TestColumnSelectChordDispatchesThroughTheRegistry);
  AddTest(tests, "ColumnSelection/GestureEndsOnACompareTabToo",
          TestColumnSelectGestureEndsOnACompareTabToo);
}

}
