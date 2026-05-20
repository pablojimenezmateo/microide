#include "TestSupport.h"

#include "editor/FoldingModel.h"
#include "editor/ShapingActions.h"
#include "editor/TextViewport.h"

namespace microide::tests {
namespace {

using microide::editor::FoldingModel;
using microide::editor::TextPosition;
using microide::editor::TextViewport;

FoldingModel::ComputeOptions DefaultFoldOptions() {
  FoldingModel::ComputeOptions options;
  options.bracket_pairs = {{'{', '}'}};
  options.use_indent_source = true;
  options.tab_size = 4;
  return options;
}

// Simulates a caret promoted via `Add Cursor At Match`: same `AddSecondaryCaret` API,
// participating in one shaping edit + single undo step (§12.1).
void TestPromotedCaretToggleLineCommentAtomicUndo() {
  TextViewport viewport;
  viewport.LoadContent("a\nb\nc\n", "/tmp/mc-promoted-comment.txt");
  viewport.MoveCursorTo(0, 0);
  viewport.AddSecondaryCaret(2, 0);

  Expect(microide::editor::ToggleLineComment(viewport, "//"),
         "toggle-line-comment should span primary plus promoted secondary caret lines");
  Expect(viewport.lines()[0].size() >= 2 && viewport.lines()[0].find("//") != std::string::npos,
         "first line should receive the line-comment marker");
  Expect(viewport.lines()[2].find("//") != std::string::npos,
         "promoted caret line should receive the line-comment marker");

  Expect(viewport.Undo(), "multi-line toggle-line-comment should undo as one step");
  Expect(viewport.lines()[0] == "a" && viewport.lines()[1] == "b" && viewport.lines()[2] == "c",
         "undo should restore every touched line atomically");
}

void TestFoldAwareMultiCaretVerticalMotionSkipsHiddenLines() {
  TextViewport viewport;
  viewport.LoadContent("before();\nvoid f() {\n  a();\n  b();\n}\nafter();\n",
                       "/tmp/mc-fold-vertical.cpp");

  FoldingModel folding_model;
  Expect(folding_model.Compute(viewport.lines(), DefaultFoldOptions()),
         "fold fixture should compute");
  Expect(folding_model.Collapse(1), "function fold should collapse");
  viewport.SetFoldingModel(&folding_model);

  viewport.MoveCursorTo(1, 0);
  viewport.SetSecondaryCarets({{5, 0}});

  viewport.MoveCursorVertical(-1);
  Expect(viewport.cursor_line() == 0 && viewport.cursor_column() == 0,
         "primary caret should step onto the line above the collapsed opener");
  Expect(viewport.secondary_carets().size() == 1 &&
             viewport.secondary_carets().front() == TextPosition{1, 0},
         "promoted caret after(); should jump to the collapsed opener row (hidden body skipped)");

  viewport.MoveCursorVertical(1);
  Expect(viewport.cursor_line() == 1 && viewport.cursor_column() == 0,
         "primary caret should move back onto the collapsed opener");
  Expect(viewport.secondary_carets().size() == 1 &&
             viewport.secondary_carets().front() == TextPosition{5, 0},
         "promoted caret should land after(); when moving down past the collapsed fold");
}

void TestMultiCaretPageUpAcrossCollapsedFoldUsesVisibleRows() {
  TextViewport viewport;
  viewport.LoadContent("line0\nvoid f() {\n  a();\n  b();\n}\nline5\nline6\n",
                       "/tmp/mc-fold-page.cpp");
  FoldingModel folding_model;
  Expect(folding_model.Compute(viewport.lines(), DefaultFoldOptions()),
         "page fixture should compute folds");
  Expect(folding_model.Collapse(1), "inner fold should collapse");

  viewport.SetFoldingModel(&folding_model);
  viewport.SetViewportSize(/*visible_lines=*/2, /*visible_columns=*/20);

  viewport.MoveCursorTo(6, 0);
  viewport.SetSecondaryCarets({{5, 0}});

  viewport.Page(-1);
  Expect(viewport.cursor_line() == 5,
         "primary caret should page up one visible row onto line5");
  Expect(viewport.secondary_carets().size() == 1 &&
             viewport.secondary_carets().front() == TextPosition{1, 0},
         "secondary caret should page up across the collapsed fold onto the opener row");

  viewport.Page(-1);
  Expect(viewport.cursor_line() == 1,
         "primary caret should reach the collapsed opener after the second page up");
  Expect(viewport.secondary_carets().size() == 1 &&
             viewport.secondary_carets().front() == TextPosition{0, 0},
         "secondary caret should continue paging up onto line0");
}

}  // namespace

void TestPlaceColumnCaretsBetweenLinesUsesAnchorColumnOnEveryLine() {
  TextViewport viewport;
  viewport.LoadContent("aaa\nbb\nccc\n", "/tmp/mc-column-carets.txt");
  viewport.MoveCursorTo(0, 1);

  viewport.PlaceColumnCaretsBetweenLines(0, 2, 1);

  Expect(viewport.cursor_line() == 2 && viewport.cursor_column() == 1,
         "column caret placement should move the primary caret to the target line");
  Expect(viewport.secondary_carets().size() == 2,
         "column caret placement should add secondaries on the other lines in range");
  Expect(viewport.secondary_carets()[0] == TextPosition{0, 1} &&
             viewport.secondary_carets()[1] == TextPosition{1, 1},
         "column caret placement should clamp each line to the shared column");
}

void RegisterEditorMultiCaretTests(std::vector<TestCase>& tests) {
  AddTest(tests, "EditorMultiCaret/PromotedCaretToggleLineCommentAtomicUndo",
          TestPromotedCaretToggleLineCommentAtomicUndo);
  AddTest(tests, "EditorMultiCaret/FoldAwareVerticalMotionSkipsHiddenLines",
          TestFoldAwareMultiCaretVerticalMotionSkipsHiddenLines);
  AddTest(tests, "EditorMultiCaret/PageUpAcrossCollapsedFoldUsesVisibleRows",
          TestMultiCaretPageUpAcrossCollapsedFoldUsesVisibleRows);
  AddTest(tests, "EditorMultiCaret/PlaceColumnCaretsBetweenLinesUsesAnchorColumnOnEveryLine",
          TestPlaceColumnCaretsBetweenLinesUsesAnchorColumnOnEveryLine);
}

}  // namespace microide::tests
