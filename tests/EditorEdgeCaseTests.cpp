#include "TestSupport.h"

#include <string>
#include <vector>

#include "editor/FoldingModel.h"
#include "editor/LanguageContractView.h"
#include "editor/ShapingActions.h"
#include "editor/TextViewport.h"

namespace microide::tests {
namespace {

using microide::editor::FoldingModel;
using microide::editor::SelectionRange;
using microide::editor::TextPosition;
using microide::editor::TextViewport;

std::string JoinLines(const TextViewport& viewport) {
  std::string out;
  for (std::size_t i = 0; i < viewport.lines().size(); ++i) {
    if (i != 0) out.push_back('\n');
    out.append(viewport.lines()[i]);
  }
  return out;
}

std::string CaretDump(const TextViewport& viewport) {
  std::string out = "P(" + std::to_string(viewport.cursor_line()) + "," +
                    std::to_string(viewport.cursor_column()) + ")";
  for (const TextPosition& caret : viewport.secondary_carets()) {
    out += " S(" + std::to_string(caret.line) + "," + std::to_string(caret.column) + ")";
  }
  return out;
}

void TestMultiCaretShiftDownThenTypeReplacesEverySelection() {
  TextViewport viewport;
  viewport.LoadContent("aaaa\nbbbb\ncccc\ndddd\n", "/tmp/ec-shift-down.txt");
  viewport.SetViewportSize(10, 40);
  viewport.MoveCursorTo(0, 1);
  viewport.SetSecondaryCarets({{2, 1}});

  viewport.MoveCursorVertical(1, true);
  Expect(viewport.has_selection(), "primary should own a selection after Shift+Down");
  const auto ranges = viewport.secondary_caret_ranges();
  Expect(ranges.size() == 1 && ranges[0].selection_anchor.has_value(),
         "the secondary caret should have begun a selection too");

  viewport.InsertText("X");
  Expect(JoinLines(viewport) == "aXbbb\ncXddd\n",
         std::string("both selections should be replaced, got: ") + JoinLines(viewport));
}

void TestMultiCaretTouchingSelectionsStaySeparateAndStillEdit() {
  TextViewport viewport;
  viewport.LoadContent("aaaa\nbbbb\ncccc\n", "/tmp/ec-merge-grow.txt");
  viewport.SetViewportSize(10, 40);
  viewport.MoveCursorTo(0, 0);
  viewport.SetSecondaryCarets({{1, 0}});

  viewport.MoveCursorVertical(1, true);
  Expect(viewport.secondary_carets().size() == 1,
         std::string("two ranged selections that only touch must both survive: ") +
             CaretDump(viewport));
  viewport.InsertText("X");
  Expect(JoinLines(viewport) == "XXcccc\n",
         std::string("both touching selections should be replaced: ") + JoinLines(viewport));
}

void TestMultiCaretOverlappingSelectionsMerge() {
  TextViewport viewport;
  viewport.LoadContent("aaaa\nbbbb\ncccc\n", "/tmp/ec-merge-overlap.txt");
  viewport.SetViewportSize(10, 40);
  viewport.MoveCursorTo(0, 0);
  viewport.MoveCursorTo(1, 2, true);
  std::vector<SelectionRange> ranges{
      SelectionRange{TextPosition{1, 1}, TextPosition{2, 2}},
  };
  viewport.SetSecondaryCaretsWithRanges(ranges);

  Expect(viewport.secondary_carets().empty(),
         std::string("overlapping selections must merge into one: ") + CaretDump(viewport));
  const auto merged = viewport.selection_range();
  Expect(merged.has_value() && merged->start == TextPosition{0, 0} &&
             merged->end == TextPosition{2, 2},
         "the surviving selection should span the union of both");
}

void TestMultiCaretVerticalMoveAtBufferEdgeDedupes() {
  TextViewport viewport;
  viewport.LoadContent("aaaa\nbbbb\ncccc\n", "/tmp/ec-edge-dedupe.txt");
  viewport.SetViewportSize(10, 40);
  viewport.MoveCursorTo(0, 2);
  viewport.SetSecondaryCarets({{1, 2}});

  viewport.MoveCursorVertical(-1);
  viewport.MoveCursorVertical(-1);
  Expect(viewport.secondary_carets().empty(),
         std::string("carets driven onto the same position must merge: ") + CaretDump(viewport));
  Expect(viewport.cursor_line() == 0 && viewport.cursor_column() == 0,
         "primary should sit on the document start");
}

void TestMultiCaretUndoRestoresEveryCaret() {
  TextViewport viewport;
  viewport.LoadContent("aaaa\nbbbb\ncccc\n", "/tmp/ec-undo-carets.txt");
  viewport.SetViewportSize(10, 40);
  viewport.MoveCursorTo(0, 2);
  viewport.SetSecondaryCarets({{1, 2}, {2, 2}});

  viewport.InsertText("Z");
  Expect(viewport.Undo(), "the multi-caret insert should undo");
  Expect(JoinLines(viewport) == "aaaa\nbbbb\ncccc\n", "undo should restore the text");
  Expect(viewport.secondary_carets().size() == 2,
         std::string("undo should restore both secondary carets: ") + CaretDump(viewport));
}

void TestMultiCaretHomeOnWrappedRowsUsesViewLineStart() {
  TextViewport viewport;
  viewport.LoadContent("abcdefghijklmnop\nabcdefghijklmnop\n", "/tmp/ec-wrap-home.txt");
  viewport.SetViewportSize(10, 8);
  viewport.SetSoftWrap(true);
  viewport.MoveCursorTo(0, 12);
  viewport.SetSecondaryCarets({{1, 12}});

  viewport.MoveCursorLineStart();
  Expect(viewport.cursor_column() == 8,
         "Home on a wrapped row should land on that row's first column, got " +
             std::to_string(viewport.cursor_column()));
  Expect(viewport.secondary_carets().size() == 1 &&
             viewport.secondary_carets().front().column == 8,
         std::string("the secondary caret should use its own row start: ") + CaretDump(viewport));
}

void TestMultiCaretEndOnWrappedRowsUsesViewLineEnd() {
  TextViewport viewport;
  viewport.LoadContent("abcdefghijklmnop\nabcdefghijklmnop\n", "/tmp/ec-wrap-end.txt");
  viewport.SetViewportSize(10, 8);
  viewport.SetSoftWrap(true);
  viewport.MoveCursorTo(0, 2);
  viewport.SetSecondaryCarets({{1, 2}});

  viewport.MoveCursorLineEnd();
  Expect(viewport.cursor_column() == 8,
         "End on the first wrapped row should stop at the wrap point, got " +
             std::to_string(viewport.cursor_column()));
  Expect(viewport.secondary_carets().size() == 1 &&
             viewport.secondary_carets().front().column == 8,
         std::string("the secondary caret should stop at its own wrap point: ") +
             CaretDump(viewport));
  viewport.MoveCursorLineEnd();
  Expect(viewport.cursor_column() == 16,
         "a second End should reach the line's end, got " + std::to_string(viewport.cursor_column()));
  Expect(viewport.secondary_carets().size() == 1 &&
             viewport.secondary_carets().front().column == 16,
         std::string("and so should the secondary caret: ") + CaretDump(viewport));
  viewport.MoveCursorLineEnd();
  Expect(viewport.cursor_column() == 16, "a third End stays at the line's end");
}

void TestMultiCaretDownOnWrappedLineStepsOneVisualRow() {
  TextViewport viewport;
  viewport.LoadContent("abcdefghijklmnop\nZZ\nabcdefghijklmnop\n", "/tmp/ec-wrap-down.txt");
  viewport.SetViewportSize(10, 8);
  viewport.SetSoftWrap(true);
  viewport.MoveCursorTo(0, 2);
  viewport.SetSecondaryCarets({{2, 2}});

  viewport.MoveCursorVertical(1);
  Expect(viewport.cursor_line() == 0 && viewport.cursor_column() == 10,
         std::string("primary should stay on line 0's second row: ") + CaretDump(viewport));
  Expect(viewport.secondary_carets().size() == 1 &&
             viewport.secondary_carets().front() == TextPosition{2, 10},
         std::string("the secondary should also step one visual row: ") + CaretDump(viewport));
}

void TestMultiCaretMoveLineUpBlockedByTopCaretIsNoOp() {
  TextViewport viewport;
  viewport.LoadContent("aaaa\nbbbb\ncccc\n", "/tmp/ec-moveline-top.txt");
  viewport.SetViewportSize(10, 40);
  viewport.MoveCursorTo(0, 0);
  viewport.SetSecondaryCarets({{2, 0}});

  microide::editor::MoveLineUp(viewport);
  Expect(viewport.lines()[0] == "aaaa",
         std::string("line 0 cannot move up, so it must stay put: ") + JoinLines(viewport));
}

void TestMultiCaretDeleteLineOnAdjacentLinesRemovesEachOnce() {
  TextViewport viewport;
  viewport.LoadContent("aaaa\nbbbb\ncccc\ndddd\n", "/tmp/ec-deleteline-adj.txt");
  viewport.SetViewportSize(10, 40);
  viewport.MoveCursorTo(1, 0);
  viewport.SetSecondaryCarets({{2, 0}});

  microide::editor::DeleteLine(viewport);
  Expect(JoinLines(viewport) == "aaaa\ndddd\n",
         std::string("both caret lines should be deleted exactly once: ") + JoinLines(viewport));
}

void TestSelectAllClearsSecondaryCarets() {
  TextViewport viewport;
  viewport.LoadContent("aaaa\nbbbb\ncccc\n", "/tmp/ec-selectall.txt");
  viewport.SetViewportSize(10, 40);
  viewport.MoveCursorTo(0, 0);
  viewport.SetSecondaryCarets({{1, 0}, {2, 0}});

  viewport.SelectAll();
  Expect(viewport.secondary_carets().empty(),
         std::string("Select All is a single-selection operation: ") + CaretDump(viewport));
}

void TestMultiCaretCopyAggregatesInDocumentOrder() {
  TextViewport viewport;
  viewport.LoadContent("alpha\nbeta\ngamma\n", "/tmp/ec-mc-copy.txt");
  viewport.SetViewportSize(10, 40);
  viewport.MoveCursorTo(2, 0);
  viewport.MoveCursorTo(2, 5, true);
  std::vector<SelectionRange> ranges{
      SelectionRange{TextPosition{0, 0}, TextPosition{0, 5}},
  };
  viewport.SetSecondaryCaretsWithRanges(ranges);

  const auto text = viewport.MultiCaretSelectedText();
  Expect(text.has_value(), "two ranged carets should produce an aggregate copy");
  Expect(*text == "alpha\ngamma\n" || *text == "alpha\ngamma",
         std::string("copy should be in document order, got: ") + *text);
}

void TestSecondaryCaretClampsWhenItsLineShrinks() {
  TextViewport viewport;
  viewport.LoadContent("aaaaaaaa\nbbbbbbbb\n", "/tmp/ec-clamp-shrink.txt");
  viewport.SetViewportSize(10, 40);
  viewport.MoveCursorTo(0, 0);
  viewport.SetSecondaryCarets({{1, 8}});

  viewport.MoveCursorTo(1, 0);
  viewport.MoveCursorTo(1, 8, true);
  viewport.DeleteSelectedText();

  for (const TextPosition& caret : viewport.secondary_carets()) {
    Expect(caret.line < viewport.lines().size(),
           "a secondary caret must never name a line past the buffer");
    Expect(caret.column <= viewport.lines().LineLength(caret.line),
           std::string("a secondary caret must never name a column past its line: ") +
               CaretDump(viewport));
  }
}

void TestBoxSelectionOverShortLineClampsToThatLine() {
  TextViewport viewport;
  viewport.LoadContent("aaaaaaaaaa\nbb\ncccccccccc\n", "/tmp/ec-box-short.txt");
  viewport.SetViewportSize(10, 40);
  viewport.SetBoxSelection(TextPosition{0, 4}, TextPosition{2, 8});

  const auto ranges = viewport.secondary_caret_ranges();
  for (const auto& caret : ranges) {
    Expect(caret.position.column <= viewport.lines().LineLength(caret.position.line),
           std::string("box caret past its line end: ") + CaretDump(viewport));
    if (caret.selection_anchor.has_value()) {
      Expect(caret.selection_anchor->column <=
                 viewport.lines().LineLength(caret.selection_anchor->line),
             "box anchor past its line end");
    }
  }
}

void TestMultiCaretDeleteSelectionsUndoRestoresText() {
  TextViewport viewport;
  viewport.LoadContent("alpha\nbeta\ngamma\n", "/tmp/ec-mc-delsel-undo.txt");
  viewport.SetViewportSize(10, 40);
  std::vector<SelectionRange> ranges{
      SelectionRange{TextPosition{1, 0}, TextPosition{1, 4}},
      SelectionRange{TextPosition{2, 0}, TextPosition{2, 5}},
  };
  viewport.MoveCursorTo(0, 0);
  viewport.MoveCursorTo(0, 5, true);
  viewport.SetSecondaryCaretsWithRanges(ranges);

  Expect(viewport.DeleteMultiCaretSelections(), "every caret owns a selection");
  Expect(JoinLines(viewport) == "\n\n\n",
         std::string("all three selections should be gone: ") + JoinLines(viewport));
  Expect(viewport.Undo(), "the aggregate delete should undo in one step");
  Expect(JoinLines(viewport) == "alpha\nbeta\ngamma\n",
         std::string("undo should restore every deleted range: ") + JoinLines(viewport));
}

void TestMultiCaretBackspaceAtColumnZeroJoinsEachLineOnce() {
  TextViewport viewport;
  viewport.LoadContent("aa\nbb\ncc\ndd\n", "/tmp/ec-mc-bs-join.txt");
  viewport.SetViewportSize(10, 40);
  viewport.MoveCursorTo(1, 0);
  viewport.SetSecondaryCarets({{3, 0}});

  viewport.Backspace();
  Expect(JoinLines(viewport) == "aabb\nccdd\n",
         std::string("each caret should join its own line once: ") + JoinLines(viewport));
}

FoldingModel::ComputeOptions CStyleFoldOptions() {
  FoldingModel::ComputeOptions options;
  options.bracket_pairs = {{'{', '}'}};
  options.use_indent_source = true;
  options.tab_size = 4;
  return options;
}

void TestCollapsedFoldRemovesEveryWrappedRowOfItsBody() {
  TextViewport viewport;
  viewport.LoadContent(
      "head\n"
      "f() {\n"
      "  aaaaaaaaaaaaaaaaaaaa;\n"
      "  b();\n"
      "}\n"
      "tail\n",
      "/tmp/ec-fold-wrap.cpp");
  viewport.SetViewportSize(20, 8);
  viewport.SetSoftWrap(true);

  const std::size_t tail_row_expanded = viewport.VisualRowForLine(5);

  FoldingModel folding_model;
  Expect(folding_model.Compute(viewport.lines().Snapshot(), CStyleFoldOptions()),
         "the fold fixture should compute");
  Expect(folding_model.Collapse(1), "the function fold should collapse");
  viewport.SetFoldingModel(&folding_model);

  const std::size_t tail_row_collapsed = viewport.VisualRowForLine(5);
  Expect(tail_row_collapsed < tail_row_expanded,
         "collapsing a fold must pull the following line up");

  for (const std::size_t line : {std::size_t{2}, std::size_t{3}, std::size_t{4}}) {
    Expect(folding_model.IsLineHidden(line),
           "line " + std::to_string(line) + " is inside the collapsed fold and must be hidden");
  }
  Expect(!folding_model.IsLineHidden(1) && !folding_model.IsLineHidden(5),
         "the opener and the line after the fold stay visible");

  const std::size_t opener_row = viewport.VisualRowForLine(1);
  Expect(tail_row_collapsed == opener_row + 1,
         "the line after a collapsed fold follows its opener with no gap, got row " +
             std::to_string(tail_row_collapsed) + " after opener row " +
             std::to_string(opener_row));
  Expect(tail_row_expanded - tail_row_collapsed >= 4,
         "collapsing must remove every wrapped row of the body, removed only " +
             std::to_string(tail_row_expanded - tail_row_collapsed));
}

void TestVisualRowsStayMonotonicAcrossAFoldUnderWrap() {
  TextViewport viewport;
  viewport.LoadContent(
      "head\n"
      "void f() {\n"
      "  aaaaaaaaaaaaaaaaaaaa;\n"
      "  void g() {\n"
      "    bbbbbbbbbbbbbbbbbbbb;\n"
      "  }\n"
      "}\n"
      "tail is also a fairly long line here\n",
      "/tmp/ec-fold-monotonic.cpp");
  viewport.SetViewportSize(30, 9);
  viewport.SetSoftWrap(true);

  FoldingModel folding_model;
  Expect(folding_model.Compute(viewport.lines().Snapshot(), CStyleFoldOptions()),
         "the nested fold fixture should compute");
  Expect(folding_model.Collapse(3), "the inner fold should collapse");
  viewport.SetFoldingModel(&folding_model);

  std::size_t previous_row = 0;
  bool first = true;
  for (std::size_t line = 0; line < viewport.lines().size(); ++line) {
    const std::size_t row = viewport.VisualRowForLine(line);
    if (folding_model.IsLineHidden(line)) {
      continue;
    }
    if (!first) {
      Expect(row > previous_row,
             "visible line " + std::to_string(line) + " should sit below the one before it, row " +
                 std::to_string(row) + " after " + std::to_string(previous_row));
    }
    first = false;
    previous_row = row;
  }

  Expect(folding_model.Collapse(1), "the outer fold should collapse as well");
  viewport.SetFoldingModel(nullptr);
  viewport.SetFoldingModel(&folding_model);
  previous_row = 0;
  first = true;
  for (std::size_t line = 0; line < viewport.lines().size(); ++line) {
    if (folding_model.IsLineHidden(line)) continue;
    const std::size_t row = viewport.VisualRowForLine(line);
    if (!first) {
      Expect(row > previous_row,
             "nested collapse reordered visible line " + std::to_string(line));
    }
    first = false;
    previous_row = row;
  }
}

void TestMultiCaretEditAcrossACollapsedFoldKeepsBothCarets() {
  TextViewport viewport;
  viewport.LoadContent("head\nvoid f() {\n  a();\n}\ntail\n", "/tmp/ec-fold-mc-edit.cpp");
  viewport.SetViewportSize(20, 40);

  FoldingModel folding_model;
  Expect(folding_model.Compute(viewport.lines().Snapshot(), CStyleFoldOptions()),
         "the fold fixture should compute");
  Expect(folding_model.Collapse(1), "the function fold should collapse");
  viewport.SetFoldingModel(&folding_model);

  viewport.MoveCursorTo(0, 0);
  viewport.SetSecondaryCarets({{4, 0}});
  viewport.InsertText("X");

  Expect(viewport.lines()[0] == "Xhead" && viewport.lines()[4] == "Xtail",
         std::string("both carets should have inserted: ") + JoinLines(viewport));
  Expect(viewport.lines()[2] == "  a();",
         "the hidden body must be untouched by an edit outside it");
  Expect(viewport.secondary_carets().size() == 1,
         std::string("the caret past the fold should survive: ") + CaretDump(viewport));
}

void TestDeletingAcrossACollapsedFoldRemovesTheHiddenLines() {
  TextViewport viewport;
  viewport.LoadContent("head\nvoid f() {\n  a();\n}\ntail\n", "/tmp/ec-fold-delete.cpp");
  viewport.SetViewportSize(20, 40);

  FoldingModel folding_model;
  Expect(folding_model.Compute(viewport.lines().Snapshot(), CStyleFoldOptions()),
         "the fold fixture should compute");
  Expect(folding_model.Collapse(1), "the function fold should collapse");
  viewport.SetFoldingModel(&folding_model);

  viewport.MoveCursorTo(0, 4);
  viewport.MoveCursorTo(4, 0, true);
  Expect(viewport.DeleteSelectedText(), "the selection across the fold should delete");
  Expect(JoinLines(viewport) == "headtail\n",
         std::string("the hidden body should go with the selection: ") + JoinLines(viewport));
}

void TestSortLinesDescending() {
  TextViewport viewport;
  viewport.LoadContent("b\nd\na\nc\n", "/tmp/ec-sort-desc.txt");
  viewport.SetViewportSize(10, 40);
  viewport.MoveCursorTo(0, 0);
  viewport.MoveCursorTo(3, 1, true);
  Expect(microide::editor::SortLines(viewport, false), "sort should report a change");
  Expect(JoinLines(viewport) == "d\nc\nb\na\n",
         std::string("descending sort, got: ") + JoinLines(viewport));
}

void TestSortLinesTwoDisjointRegions() {
  TextViewport viewport;
  viewport.LoadContent("b\na\nZZ\nYY\nd\nc\n", "/tmp/ec-sort-two.txt");
  viewport.SetViewportSize(10, 40);
  viewport.MoveCursorTo(0, 0);
  viewport.MoveCursorTo(1, 1, true);
  std::vector<SelectionRange> ranges = {SelectionRange{TextPosition{4, 0}, TextPosition{5, 1}}};
  viewport.SetSecondaryCaretsWithRanges(ranges);
  Expect(microide::editor::SortLines(viewport, true), "two-region sort changes");
  Expect(JoinLines(viewport) == "a\nb\nZZ\nYY\nc\nd\n",
         std::string("each region sorts alone, got: ") + JoinLines(viewport));
}

void TestToggleLineCommentMixedRegionCommentsAll() {
  TextViewport viewport;
  viewport.LoadContent("// a\nb\n// c\n", "/tmp/ec-mixed-comment.cpp");
  viewport.SetViewportSize(10, 40);
  viewport.MoveCursorTo(0, 0);
  viewport.MoveCursorTo(2, 4, true);
  Expect(microide::editor::ToggleLineComment(viewport, "//"), "mixed toggle changes");
  Expect(JoinLines(viewport) == "// // a\n// b\n// // c\n",
         std::string("mixed region should comment every line, got: ") + JoinLines(viewport));
}

void TestCopyLinesUpWithAdjacentCarets() {
  TextViewport viewport;
  viewport.LoadContent("a\nb\nc\nd\n", "/tmp/ec-copy-adj.txt");
  viewport.SetViewportSize(10, 40);
  viewport.MoveCursorTo(1, 0);
  viewport.SetSecondaryCarets({{2, 0}});
  Expect(microide::editor::CopyLines(viewport, false), "copy lines up changes");
  Expect(JoinLines(viewport) == "a\nb\nc\nb\nc\nd\n",
         std::string("adjacent carets duplicate the merged region once, got: ") +
             JoinLines(viewport));
}

void TestBoxSelectionAcrossATabKeepsTheRectangle() {
  TextViewport viewport;
  viewport.LoadContent("\tabcd\n    abcd\n", "/tmp/ec-box-tab.txt");
  viewport.SetViewportSize(10, 40);
  viewport.SetTabSize(4);
  viewport.SetBoxSelectionVisual(0, 4, 1, 6);
  Expect(viewport.secondary_carets().size() == 1,
         "the box should put a ranged caret on the second line");
  viewport.InsertText("X");
  Expect(JoinLines(viewport) == "\tXcd\n    Xcd\n",
         std::string("box typing replaces the same visual slice, got: ") + JoinLines(viewport));
}

void TestBoxSelectionTypingUndoRestores() {
  TextViewport viewport;
  viewport.LoadContent("abcdef\nabcdef\nabcdef\n", "/tmp/ec-box-undo.txt");
  viewport.SetViewportSize(10, 40);
  viewport.SetBoxSelection(TextPosition{0, 1}, TextPosition{2, 3});
  viewport.InsertText("Z");
  Expect(JoinLines(viewport) == "aZdef\naZdef\naZdef\n",
         std::string("box typing, got: ") + JoinLines(viewport));
  Expect(viewport.Undo(), "box typing undoes as one step");
  Expect(JoinLines(viewport) == "abcdef\nabcdef\nabcdef\n",
         std::string("undo restores, got: ") + JoinLines(viewport));
}

void TestOutdentMixedTabAndSpaces() {
  TextViewport viewport;
  viewport.LoadContent("\tone\n    two\n  three\n", "/tmp/ec-outdent-mixed.txt");
  viewport.SetViewportSize(10, 40);
  viewport.SetIndentWidth(4);
  viewport.MoveCursorTo(0, 0);
  viewport.MoveCursorTo(2, 7, true);
  Expect(microide::editor::OutdentSelection(viewport), "outdent changes");
  Expect(JoinLines(viewport) == "one\ntwo\nthree\n",
         std::string("each line loses one indent unit, got: ") + JoinLines(viewport));
}

void TestSelectWordAtCursorOnWhitespaceRun() {
  TextViewport viewport;
  viewport.LoadContent("ab    cd\n", "/tmp/ec-word-ws.txt");
  viewport.SetViewportSize(10, 40);
  viewport.MoveCursorTo(0, 3);
  viewport.SelectWordOrRunAtCursor();
  Expect(viewport.SelectedText() == "    ",
         std::string("whitespace run selects as a word, got: '") + viewport.SelectedText() + "'");
}

void TestDeleteWordForwardAtLineEndJoins() {
  TextViewport viewport;
  viewport.LoadContent("abc\ndef\n", "/tmp/ec-delword-eol.txt");
  viewport.SetViewportSize(10, 40);
  viewport.MoveCursorTo(0, 3);
  viewport.DeleteWord(1);
  Expect(JoinLines(viewport) == "abcdef\n",
         std::string("VSCode joins the next line, got: ") + JoinLines(viewport));
}


void TestSelectWordAtCursorAtWordEnd() {
  TextViewport viewport;
  viewport.LoadContent("foo bar\n", "/tmp/ec-word-end.txt");
  viewport.SetViewportSize(10, 40);
  viewport.MoveCursorTo(0, 3);
  viewport.SelectWordAtCursor();
  Expect(viewport.SelectedText() == "foo",
         std::string("caret just past a word selects it, got: '") + viewport.SelectedText() + "'");
}

void TestSelectWordAtCursorAtLineEnd() {
  TextViewport viewport;
  viewport.LoadContent("foo\n", "/tmp/ec-word-lineend.txt");
  viewport.SetViewportSize(10, 40);
  viewport.MoveCursorTo(0, 3);
  viewport.SelectWordAtCursor();
  Expect(viewport.SelectedText() == "foo",
         std::string("caret at EOL just past a word selects it, got: '") + viewport.SelectedText() + "'");
}

void TestSelectWordAtCursorOnSeparatorRun() {
  TextViewport viewport;
  viewport.LoadContent("a->b\n", "/tmp/ec-word-sep.txt");
  viewport.SetViewportSize(10, 40);
  viewport.MoveCursorTo(0, 2);
  viewport.SelectWordOrRunAtCursor();
  Expect(viewport.SelectedText() == "->",
         std::string("separator run selects whole, got: '") + viewport.SelectedText() + "'");
}


void TestMultiCaretBareCaretsCopyEveryCaretsLine() {
  TextViewport viewport;
  viewport.LoadContent("alpha\nbeta\ngamma\ndelta\n", "/tmp/ec-mc-line-copy.txt");
  viewport.SetViewportSize(10, 40);
  viewport.MoveCursorTo(2, 1);
  viewport.SetSecondaryCarets({{0, 1}});

  const auto text = viewport.MultiCaretLineTextForClipboard();
  Expect(text.has_value(), "two bare carets should produce a line aggregate");
  Expect(*text == "alpha\ngamma\n",
         std::string("both lines, in document order, each with its terminator, got: ") + *text);
}

void TestMultiCaretLineCutCapturesWhatItDeletes() {
  TextViewport viewport;
  viewport.LoadContent("alpha\nbeta\ngamma\ndelta\nepsilon\n", "/tmp/ec-mc-line-cut.txt");
  viewport.SetViewportSize(10, 40);
  viewport.MoveCursorTo(1, 0);
  viewport.SetSecondaryCarets({{3, 0}});

  const auto captured = viewport.MultiCaretLineTextForClipboard();
  Expect(captured.has_value(), "the cut captures a line aggregate");
  const std::string before = JoinLines(viewport);
  Expect(viewport.DeleteCurrentLine(), "the cut deletes");
  const std::string after = JoinLines(viewport);
  Expect(after == "alpha\ngamma\nepsilon\n",
         std::string("only the caret lines go, got: ") + after);

  std::string removed;
  for (const std::string& line : {std::string("beta"), std::string("delta")}) {
    removed += line;
    removed.push_back('\n');
  }
  Expect(*captured == removed,
         std::string("captured text equals the removed lines, got: ") + *captured);
  Expect(before.size() > after.size(), "the buffer actually shrank");
}

void TestMultiCaretLineVerbsExpandACollapsedFold() {
  TextViewport viewport;
  viewport.LoadContent("head {\n  body\n}\ntail\nlast\n", "/tmp/ec-mc-fold-line.txt");
  viewport.SetViewportSize(10, 40);
  FoldingModel folding;
  Expect(folding.Compute(viewport.lines().Snapshot(), CStyleFoldOptions()),
         "the fold fixture should compute");
  Expect(folding.Collapse(0), "the brace fold at line 0 should collapse");
  viewport.SetFoldingModel(&folding);
  Expect(viewport.CollapsedFoldEndAt(0) == 2, "the fold at line 0 covers lines 0..2");

  viewport.MoveCursorTo(0, 0);
  viewport.SetSecondaryCarets({{4, 0}});

  const auto captured = viewport.MultiCaretLineTextForClipboard();
  Expect(captured.has_value() && *captured == "head {\n  body\n}\nlast\n",
         std::string("the whole block is captured, not just the opener, got: ") +
             (captured.has_value() ? *captured : std::string("<none>")));

  Expect(viewport.DeleteCurrentLine(), "the multi-caret line delete applies");
  Expect(JoinLines(viewport) == "tail\n",
         std::string("the fold goes whole and the body is not orphaned, got: ") +
             JoinLines(viewport));
}

void TestSingleCaretLineClipboardIsUnchanged() {
  TextViewport viewport;
  viewport.LoadContent("alpha\nbeta\n", "/tmp/ec-single-line-copy.txt");
  viewport.SetViewportSize(10, 40);
  viewport.MoveCursorTo(1, 2);
  Expect(!viewport.MultiCaretLineTextForClipboard().has_value(),
         "one caret has no multi-caret line aggregate");
  Expect(viewport.CurrentLineTextForClipboard() == "beta\n",
         "the single-caret path still takes the caret's own line");
}


void TestMultiCaretPageUnderWrapStepsVisualRows() {
  TextViewport viewport;
  viewport.LoadContent("aaaaaaaaaaaa\nbbbbbbbbbbbb\ncccccccccccc\ndddddddddddd\n",
                       "/tmp/ec-mc-page-wrap.txt");
  viewport.SetSoftWrap(true);
  viewport.SetViewportSize(4, 8);
  viewport.MoveCursorTo(0, 0);
  viewport.SetSecondaryCarets({{1, 0}});

  const std::size_t primary_line_before = viewport.cursor_line();
  viewport.Page(1);

  Expect(viewport.cursor_line() > primary_line_before,
         "the primary caret advanced past its starting line, got line " +
             std::to_string(viewport.cursor_line()));
  Expect(viewport.VisualRowForLine(viewport.cursor_line()) >=
             viewport.VisualRowForLine(primary_line_before) + 2,
         "it advanced by visual rows, not by one logical line");
  Expect(viewport.secondary_carets().size() == 1,
         "the secondary caret survives the page move");
  Expect(viewport.secondary_carets().front().line == viewport.cursor_line() + 1,
         "both carets moved by the same number of rows");
}

void TestMultiCaretPageAtBufferEndDedupes() {
  TextViewport viewport;
  viewport.LoadContent("a\nb\nc\n", "/tmp/ec-mc-page-end.txt");
  viewport.SetViewportSize(4, 40);
  viewport.MoveCursorTo(0, 0);
  viewport.SetSecondaryCarets({{1, 0}});

  viewport.Page(1);
  viewport.Page(1);

  const std::size_t total = viewport.secondary_carets().size() + 1;
  Expect(total <= 2, "carets driven onto the same row collapse, got " + std::to_string(total));
  for (const TextPosition& caret : viewport.secondary_carets()) {
    Expect(!(caret.line == viewport.cursor_line() && caret.column == viewport.cursor_column()),
           "no secondary caret sits exactly on the primary");
  }
}

void TestSelectAllThenTypeUnderWrapLeavesOneCaret() {
  TextViewport viewport;
  viewport.LoadContent("aaaaaaaaaaaa\nbbbbbbbbbbbb\ncccccccccccc\n", "/tmp/ec-wrap-selectall.txt");
  viewport.SetSoftWrap(true);
  viewport.SetViewportSize(6, 8);
  viewport.MoveCursorTo(0, 0);
  viewport.SetSecondaryCarets({{1, 0}});

  viewport.SelectAll();
  viewport.InsertText("x");

  Expect(JoinLines(viewport) == "x", std::string("the buffer is replaced, got: ") + JoinLines(viewport));
  Expect(viewport.secondary_carets().empty(), "select-all collapses to a single caret");
  Expect(viewport.cursor_line() == 0 && viewport.cursor_column() == 1,
         "the caret lands after the typed character");
  Expect(viewport.VisualRowForLine(0) == 0 && viewport.VisualRowCount() == 1,
         "the wrap table is rebuilt for the one-line document, rows=" +
             std::to_string(viewport.VisualRowCount()));
}

void TestToggleBlockCommentReachesEveryCaret() {
  TextViewport viewport;
  viewport.LoadContent("aaa();\nbbb();\nccc();\nddd();\n", "/tmp/ec-block-multi.cpp");
  viewport.SetViewportSize(10, 40);
  viewport.MoveCursorTo(0, 1);
  viewport.SetSecondaryCarets({{2, 1}});

  Expect(microide::editor::ToggleBlockComment(viewport, "/*", "*/"),
         "the toggle should apply");
  Expect(JoinLines(viewport) == "/* aaa(); */\nbbb();\n/* ccc(); */\nddd();\n",
         std::string("both carets' lines should be wrapped, got: ") + JoinLines(viewport));

  Expect(microide::editor::ToggleBlockComment(viewport, "/*", "*/"),
         "the second toggle should apply");
  Expect(JoinLines(viewport) == "aaa();\nbbb();\nccc();\nddd();\n",
         std::string("toggling twice should restore the buffer, got: ") + JoinLines(viewport));

  Expect(viewport.Undo(), "the multi-region toggle should undo");
  Expect(JoinLines(viewport) == "/* aaa(); */\nbbb();\n/* ccc(); */\nddd();\n",
         std::string("one undo should take back the whole toggle, got: ") + JoinLines(viewport));
}

void TestToggleBlockCommentWithTwoCaretsOnOneLine() {
  TextViewport viewport;
  viewport.LoadContent("alpha bravo\nsecond\n", "/tmp/ec-block-same-line.cpp");
  viewport.SetViewportSize(10, 40);
  viewport.MoveCursorTo(0, 1);
  viewport.SetSecondaryCarets({{0, 8}});

  Expect(microide::editor::ToggleBlockComment(viewport, "/*", "*/"), "the toggle applies");
  Expect(JoinLines(viewport) == "/* alpha bravo */\nsecond\n",
         std::string("one line, one wrap: ") + JoinLines(viewport));
}

void TestToggleBlockCommentOnAWholeLineSelection() {
  TextViewport viewport;
  viewport.LoadContent("keepA();\nsel1();\nsel2();\nsel3();\nkeepB();\n",
                       "/tmp/ec-block-whole-lines.cpp");
  viewport.SetViewportSize(10, 40);
  viewport.MoveCursorTo(1, 0);
  viewport.MoveCursorTo(4, 0, true);

  Expect(microide::editor::ToggleBlockComment(viewport, "/*", "*/"), "the toggle applies");
  Expect(JoinLines(viewport) ==
             "keepA();\n/* sel1();\nsel2();\nsel3(); */\nkeepB();\n",
         std::string("the close marker belongs on the last SELECTED line: ") +
             JoinLines(viewport));

  Expect(microide::editor::ToggleBlockComment(viewport, "/*", "*/"), "the second toggle applies");
  Expect(JoinLines(viewport) == "keepA();\nsel1();\nsel2();\nsel3();\nkeepB();\n",
         std::string("and toggling twice restores it: ") + JoinLines(viewport));
}

void TestJoinLinesBareCaretTakesTheLineBelow() {
  TextViewport viewport;
  viewport.LoadContent("alpha\n    beta\ngamma\n", "/tmp/ec-join-below.txt");
  viewport.SetViewportSize(10, 40);
  viewport.MoveCursorTo(0, 2);

  Expect(microide::editor::JoinLinesAtCarets(viewport), "the join applies");
  Expect(JoinLines(viewport) == "alpha beta\ngamma\n",
         std::string("the line below is appended with one space, its indent trimmed: ") +
             JoinLines(viewport));

  Expect(microide::editor::JoinLinesAtCarets(viewport), "a second press applies");
  Expect(JoinLines(viewport) == "alpha beta gamma\n",
         std::string("and pulls the next line up too: ") + JoinLines(viewport));
}

void TestJoinLinesSpacingEdges() {
  TextViewport viewport;
  viewport.LoadContent("alpha \nbeta\n", "/tmp/ec-join-space.txt");
  viewport.SetViewportSize(10, 40);
  viewport.MoveCursorTo(0, 0);
  Expect(microide::editor::JoinLinesAtCarets(viewport), "the join applies");
  Expect(JoinLines(viewport) == "alpha beta\n",
         std::string("no double space after a trailing one: ") + JoinLines(viewport));

  TextViewport blanks;
  blanks.LoadContent("alpha\n\n\nbeta\n", "/tmp/ec-join-blank.txt");
  blanks.SetViewportSize(10, 40);
  blanks.MoveCursorTo(0, 0);
  blanks.MoveCursorTo(3, 4, true);
  Expect(microide::editor::JoinLinesAtCarets(blanks), "the join applies over blanks");
  Expect(JoinLines(blanks) == "alpha beta\n",
         std::string("empty lines contribute nothing, not stray spaces: ") + JoinLines(blanks));
}

void TestJoinLinesAtEveryCaret() {
  TextViewport viewport;
  viewport.LoadContent("a1\na2\nkeep\nb1\nb2\n", "/tmp/ec-join-multi.txt");
  viewport.SetViewportSize(10, 40);
  viewport.MoveCursorTo(0, 0);
  viewport.SetSecondaryCarets({{3, 0}});

  Expect(microide::editor::JoinLinesAtCarets(viewport), "the join applies");
  Expect(JoinLines(viewport) == "a1 a2\nkeep\nb1 b2\n",
         std::string("each caret joined its own pair: ") + JoinLines(viewport));
  Expect(viewport.secondary_caret_range_view().size() == 1,
         "both carets survive the join");
}

void TestJoinLinesAtTheLastLineIsANoOp() {
  TextViewport viewport;
  viewport.LoadContent("only\n", "/tmp/ec-join-last.txt");
  viewport.SetViewportSize(10, 40);
  viewport.MoveCursorTo(1, 0);
  Expect(!microide::editor::JoinLinesAtCarets(viewport), "there is nothing below to join");
  Expect(JoinLines(viewport) == "only\n", "and the buffer is untouched");
}

void TestUpOnTheFirstRowGoesToRowStartPerCaret() {
  TextViewport viewport;
  viewport.LoadContent("hello\nworld\n", "/tmp/ec-up-first-row.txt");
  viewport.SetViewportSize(10, 40);
  viewport.MoveCursorTo(0, 3);
  viewport.SetSecondaryCarets({{1, 3}});

  viewport.MoveCursorVertical(-1);
  Expect(viewport.cursor_line() == 0 && viewport.cursor_column() == 0,
         std::string("Up on the first row lands on its start: ") + CaretDump(viewport));
  Expect(viewport.secondary_carets().size() == 1 &&
             viewport.secondary_carets().front() == TextPosition{0, 3},
         std::string("the secondary caret still steps one row: ") + CaretDump(viewport));

  viewport.MoveCursorVertical(-1);
  Expect(viewport.cursor_line() == 0 && viewport.cursor_column() == 0,
         "a second Up stays put");
  Expect(viewport.secondary_carets().empty(),
         std::string("the secondary caret reaches (0,0) too and merges: ") + CaretDump(viewport));
}

void TestDownOnTheLastRowGoesToRowEndPerCaret() {
  TextViewport viewport;
  viewport.LoadContent("hello\nworld", "/tmp/ec-down-last-row.txt");
  viewport.SetViewportSize(10, 40);
  Expect(viewport.lines().size() == 2, "fixture: no phantom line after the last one");
  viewport.MoveCursorTo(1, 2);
  viewport.SetSecondaryCarets({{0, 2}});

  viewport.MoveCursorVertical(1);
  Expect(viewport.cursor_line() == 1 && viewport.cursor_column() == 5,
         std::string("Down on the last row lands on its end: ") + CaretDump(viewport));
  Expect(viewport.secondary_carets().size() == 1 &&
             viewport.secondary_carets().front() == TextPosition{1, 2},
         std::string("the secondary caret still steps one row: ") + CaretDump(viewport));
  Expect(!viewport.has_selection(), "a plain move leaves no selection");
}

void TestShiftUpOnTheFirstRowExtendsToRowStart() {
  TextViewport viewport;
  viewport.LoadContent("hello\n", "/tmp/ec-shift-up-first-row.txt");
  viewport.SetViewportSize(10, 40);
  viewport.MoveCursorTo(0, 3);
  viewport.MoveCursorVertical(-1, true);
  const auto range = viewport.selection_range();
  Expect(range.has_value() && range->start == TextPosition{0, 0} &&
             range->end == TextPosition{0, 3},
         std::string("Shift+Up on the first row selects back to column 0: ") + CaretDump(viewport));
}

void TestUpDownAtTheEdgeUnderWrapUsesVisualRows() {
  TextViewport viewport;
  viewport.LoadContent("abcdefghijklmnop", "/tmp/ec-wrap-edge-rows.txt");
  viewport.SetViewportSize(10, 8);
  viewport.SetSoftWrap(true);
  Expect(viewport.visual_line_count() == 2, "fixture: the one line wraps into two rows");
  viewport.MoveCursorTo(0, 12);

  viewport.MoveCursorVertical(-1);
  Expect(viewport.cursor_visual_row() == 0 && viewport.cursor_column() == 4,
         "Up from the second row climbs onto the first under the same screen column, got column " +
             std::to_string(viewport.cursor_column()));
  viewport.MoveCursorVertical(-1);
  Expect(viewport.cursor_column() == 0,
         "Up on the first row snaps to the row start, got column " +
             std::to_string(viewport.cursor_column()));

  viewport.MoveCursorTo(0, 3);
  viewport.MoveCursorVertical(1);
  Expect(viewport.cursor_visual_row() == 1 && viewport.cursor_column() == 11,
         "Down from the first row steps onto the second, got column " +
             std::to_string(viewport.cursor_column()));
  viewport.MoveCursorVertical(1);
  Expect(viewport.cursor_column() == 16,
         "Down on the last row goes to its end, got column " +
             std::to_string(viewport.cursor_column()));
}

void TestPlainVerticalMoveOverASelectionStartsFromItsEdge() {
  TextViewport viewport;
  viewport.LoadContent("aaaa\nbbbb\ncccc\ndddd\neeee\n", "/tmp/ec-vertical-from-edge.txt");
  viewport.SetViewportSize(10, 40);
  viewport.MoveCursorTo(2, 2);
  viewport.MoveCursorTo(0, 2, true);
  viewport.SetSecondaryCaretsWithRanges(std::vector<SelectionRange>{
      SelectionRange{TextPosition{3, 1}, TextPosition{4, 1}},
  });

  viewport.MoveCursorVertical(1);
  Expect(!viewport.has_selection(), "a plain Down collapses the selection");
  Expect(viewport.cursor_line() == 3 && viewport.cursor_column() == 2,
         std::string("Down starts from the selection END (2,2), so it lands on line 3: ") +
             CaretDump(viewport));
  Expect(viewport.secondary_carets().size() == 1 &&
             viewport.secondary_carets().front() == TextPosition{5, 0},
         std::string("the secondary steps from its own selection end: ") + CaretDump(viewport));
}

void TestPlainUpOverAForwardSelectionStartsFromItsStart() {
  TextViewport viewport;
  viewport.LoadContent("aaaa\nbbbb\ncccc\ndddd\n", "/tmp/ec-up-from-start.txt");
  viewport.SetViewportSize(10, 40);
  viewport.MoveCursorTo(1, 2);
  viewport.MoveCursorTo(3, 2, true);

  viewport.MoveCursorVertical(-1);
  Expect(!viewport.has_selection(), "a plain Up collapses the selection");
  Expect(viewport.cursor_line() == 0 && viewport.cursor_column() == 2,
         std::string("Up starts from the selection START (1,2), so it lands on line 0: ") +
             CaretDump(viewport));
}

void TestTouchingReversedSelectionAtThePrimaryCaretSurvives() {
  TextViewport viewport;
  viewport.LoadContent("alpha beta gamma delta\n", "/tmp/ec-touch-at-primary.txt");
  viewport.SetViewportSize(10, 40);
  viewport.MoveCursorTo(0, 0);
  viewport.MoveCursorTo(0, 5, true);
  viewport.SetSecondaryCaretsWithRanges(std::vector<SelectionRange>{
      SelectionRange{TextPosition{0, 16}, TextPosition{0, 7}},
  });
  Expect(viewport.secondary_carets().size() == 1, "fixture: the reversed secondary exists");

  viewport.MoveCursorWord(1, true);
  Expect(viewport.cursor_column() == 10, "primary word-steps to the end of 'beta'");
  const auto ranges = viewport.secondary_caret_ranges();
  Expect(ranges.size() == 1 && ranges[0].position == TextPosition{0, 10} &&
             ranges[0].selection_anchor == std::optional<TextPosition>(TextPosition{0, 16}),
         std::string("the secondary shares the primary's position but keeps its own selection: ") +
             CaretDump(viewport));

  viewport.InsertText("X");
  Expect(JoinLines(viewport) == "XX delta\n",
         std::string("both touching selections are replaced: ") + JoinLines(viewport));
}

void TestMultiCaretTabAtASharedPositionPadsBothSites() {
  TextViewport viewport;
  viewport.LoadContent("alpha beta gamma delta\n", "/tmp/ec-tab-shared-position.txt");
  viewport.SetViewportSize(10, 40);
  viewport.SetSoftTabs(true);
  viewport.SetIndentWidth(4);
  viewport.MoveCursorTo(0, 0);
  viewport.MoveCursorTo(0, 5, true);
  viewport.SetSecondaryCaretsWithRanges(std::vector<SelectionRange>{
      SelectionRange{TextPosition{0, 16}, TextPosition{0, 7}},
  });
  viewport.MoveCursorWord(1, true);
  Expect(viewport.secondary_carets().size() == 1 && viewport.cursor_column() == 10 &&
             viewport.secondary_carets().front() == TextPosition{0, 10},
         std::string("fixture: two carets share column 10: ") + CaretDump(viewport));

  viewport.InsertTab();
  Expect(JoinLines(viewport) == "      " + std::string(" delta\n"),
         std::string("each selection is replaced by its own soft tab: '") + JoinLines(viewport) +
             "'");
}

void TestMultiCaretTabReplacesSingleLineSelectionsFromTheirStart() {
  TextViewport viewport;
  viewport.LoadContent("abcdef\nabcdef\n", "/tmp/ec-tab-selections.txt");
  viewport.SetViewportSize(10, 40);
  viewport.SetSoftTabs(true);
  viewport.SetIndentWidth(4);
  viewport.MoveCursorTo(0, 1);
  viewport.MoveCursorTo(0, 3, true);
  viewport.SetSecondaryCaretsWithRanges(std::vector<SelectionRange>{
      SelectionRange{TextPosition{1, 2}, TextPosition{1, 5}},
  });

  viewport.InsertTab();
  Expect(JoinLines(viewport) == "a   def\nab  f\n",
         std::string("column 1 pads 3, column 2 pads 2: '") + JoinLines(viewport) + "'");
  Expect(viewport.cursor_column() == 4 && !viewport.has_selection(),
         std::string("the primary lands after its padding, collapsed: ") + CaretDump(viewport));
}

void TestTabKeyClassifierIndentsWholeLineAndSecondarySelections() {
  using microide::editor::ClassifyTabKey;
  using microide::editor::TabKeyIntent;
  TextViewport viewport;
  viewport.LoadContent("  foo\nbar\nbaz\n", "/tmp/ec-tab-classify.txt");
  viewport.SetViewportSize(10, 40);

  viewport.MoveCursorTo(0, 2);
  Expect(ClassifyTabKey(viewport, false) == TabKeyIntent::kInsertTab,
         "a bare caret inserts a tab");
  Expect(ClassifyTabKey(viewport, true) == TabKeyIntent::kOutdent, "Shift always outdents");

  viewport.MoveCursorTo(0, 4, true);
  Expect(ClassifyTabKey(viewport, false) == TabKeyIntent::kInsertTab,
         "a partial single-line selection is replaced by the indent");

  viewport.MoveCursorTo(0, 0);
  viewport.MoveCursorTo(0, 5, true);
  Expect(ClassifyTabKey(viewport, false) == TabKeyIntent::kIndentBlock,
         "a selection covering the whole line indents the line instead of eating it");

  viewport.MoveCursorTo(0, 2);
  viewport.SetSecondaryCaretsWithRanges(std::vector<SelectionRange>{
      SelectionRange{TextPosition{1, 1}, TextPosition{2, 1}},
  });
  Expect(ClassifyTabKey(viewport, false) == TabKeyIntent::kMixed,
         "a bare primary beside a secondary's multi-line selection is a mixed set: one "
         "shifts lines, the other inserts at its own column");
  viewport.MoveCursorTo(1, 0);
  viewport.MoveCursorTo(1, 3, true);
  viewport.SetSecondaryCaretsWithRanges(std::vector<SelectionRange>{
      SelectionRange{TextPosition{2, 0}, TextPosition{2, 3}},
  });
  Expect(ClassifyTabKey(viewport, false) == TabKeyIntent::kIndentBlock,
         "when every cursor is a block the whole-set applier still runs");
  viewport.MoveCursorTo(0, 2);
  viewport.SetSecondaryCaretsWithRanges(std::vector<SelectionRange>{
      SelectionRange{TextPosition{1, 2}, TextPosition{1, 1}},
  });
  Expect(ClassifyTabKey(viewport, false) == TabKeyIntent::kInsertTab,
         "a secondary's partial single-line selection still inserts");
}

void TestTabKeyOnAMixedCaretSetIndentsPerCursor() {
  TextViewport viewport;
  viewport.SetViewportSize(20, 200);
  viewport.SetSoftTabs(true);
  viewport.SetIndentWidth(4);
  viewport.LoadContent("aaaa\nbbbb\ncccc\n", "/tmp/ec-tab-mixed.cpp");
  viewport.MoveCursorTo(0, 0);
  viewport.MoveCursorTo(0, 4, true);
  viewport.AddSecondaryCaret(2, 2);

  Expect(microide::editor::ClassifyTabKey(viewport, false) ==
             microide::editor::TabKeyIntent::kMixed,
         "a block selection beside a bare caret is a mixed set");
  Expect(microide::editor::ApplyMixedTabKey(viewport), "the mixed apply should change the buffer");
  const auto joined = [&viewport]() {
    return std::string(viewport.lines()[0]) + "/" + std::string(viewport.lines()[1]) + "/" +
           std::string(viewport.lines()[2]);
  };
  Expect(joined() == "    aaaa/bbbb/cc  cc",
         "the block selection shifts its line and the bare caret indents at its own column: " +
             joined());

  const auto selection = viewport.selection_range();
  Expect(selection.has_value() && selection->start.line == 0 && selection->start.column == 4 &&
             selection->end.line == 0 && selection->end.column == 8,
         "the block cursor's selection should still cover 'aaaa' after the shift");
  const auto secondaries = viewport.secondary_caret_range_view();
  Expect(secondaries.size() == 1 && secondaries[0].position.line == 2 &&
             secondaries[0].position.column == 4,
         "the bare caret should sit just past the indent it inserted");

  viewport.Undo();
  Expect(joined() == "aaaa/bbbb/cccc", "one undo should take the whole mixed edit back: " + joined());
}

void TestTabKeyOnALargeMixedCaretSetStaysCorrect() {
  TextViewport viewport;
  viewport.SetViewportSize(20, 200);
  viewport.SetSoftTabs(true);
  viewport.SetIndentWidth(2);
  std::string content;
  constexpr std::size_t kLines = 400;
  for (std::size_t i = 0; i < kLines; ++i) {
    content += "xxxx\n";
  }
  viewport.LoadContent(content, "/tmp/ec-tab-mixed-large.cpp");

  viewport.MoveCursorTo(0, 0);
  viewport.MoveCursorTo(kLines / 2, 0, true);
  std::vector<SelectionRange> secondaries;
  for (std::size_t line = kLines / 2; line < kLines; ++line) {
    secondaries.push_back(
        SelectionRange{TextPosition{line, 2}, TextPosition{line, 2}});
  }
  viewport.SetSecondaryCaretsWithRanges(secondaries);

  Expect(microide::editor::ClassifyTabKey(viewport, false) ==
             microide::editor::TabKeyIntent::kMixed,
         "a block selection plus hundreds of bare carets is a mixed set");
  Expect(microide::editor::ApplyMixedTabKey(viewport), "the mixed apply should change the buffer");

  for (std::size_t line = 0; line < kLines / 2; ++line) {
    Expect(std::string(viewport.lines()[line]) == "  xxxx",
           "every line of the block selection should gain one indent");
  }
  for (std::size_t line = kLines / 2; line < kLines; ++line) {
    Expect(std::string(viewport.lines()[line]) == "xx  xx",
           "every bare caret should indent at its own column");
  }
  Expect(viewport.secondary_caret_range_view().size() == kLines / 2,
         "no caret should be dropped by the mixed apply");
  for (const auto& secondary : viewport.secondary_caret_range_view()) {
    Expect(secondary.position.column == 4,
           "each bare caret should sit just past the two spaces it inserted");
  }
  viewport.Undo();
  Expect(std::string(viewport.lines()[0]) == "xxxx" &&
             std::string(viewport.lines()[kLines - 1]) == "xxxx",
         "one undo should take the whole mixed edit back at scale too");
}

void TestTabKeyOnAMixedCaretSetNeverEatsASecondarySelection() {
  TextViewport viewport;
  viewport.SetViewportSize(20, 200);
  viewport.SetSoftTabs(true);
  viewport.SetIndentWidth(2);
  viewport.LoadContent("one\ntwo\nthree\nfour\n", "/tmp/ec-tab-mixed-secondary.cpp");
  viewport.MoveCursorTo(0, 1);
  viewport.AddSecondaryCaretWithRange(SelectionRange{TextPosition{2, 0}, TextPosition{3, 4}});

  Expect(microide::editor::ClassifyTabKey(viewport, false) ==
             microide::editor::TabKeyIntent::kMixed,
         "a bare primary beside a multi-line secondary selection is a mixed set");
  Expect(microide::editor::ApplyMixedTabKey(viewport), "the mixed apply should change the buffer");
  const std::string text = std::string(viewport.lines()[0]) + "/" +
                           std::string(viewport.lines()[1]) + "/" +
                           std::string(viewport.lines()[2]) + "/" +
                           std::string(viewport.lines()[3]);
  Expect(text == "o ne/two/  three/  four",
         "the secondary's lines shift and the bare primary indents at its column: " + text);
}

void TestTabKeyOnAMixedCaretSetIndentsASharedLineOnce() {
  TextViewport viewport;
  viewport.SetViewportSize(20, 200);
  viewport.SetSoftTabs(true);
  viewport.SetIndentWidth(2);
  viewport.LoadContent("alpha\nbeta\ngamma\n", "/tmp/ec-tab-mixed-shared.cpp");
  viewport.MoveCursorTo(0, 0);
  viewport.MoveCursorTo(1, 0, true);
  viewport.AddSecondaryCaretWithRange(SelectionRange{TextPosition{0, 0}, TextPosition{0, 5}});
  viewport.AddSecondaryCaret(2, 3);

  Expect(microide::editor::ClassifyTabKey(viewport, false) ==
             microide::editor::TabKeyIntent::kMixed,
         "two block sites plus a bare caret is a mixed set");
  Expect(microide::editor::ApplyMixedTabKey(viewport), "the mixed apply should change the buffer");
  const std::string text = std::string(viewport.lines()[0]) + "/" +
                           std::string(viewport.lines()[1]) + "/" +
                           std::string(viewport.lines()[2]);
  Expect(text == "  alpha/beta/gam ma",
         "a line two block sites both cover gains one indent, not two: " + text);
}

void TestTabKeyOnAMixedCaretSetWithHardTabs() {
  TextViewport viewport;
  viewport.SetViewportSize(20, 200);
  viewport.SetSoftTabs(false);
  viewport.SetIndentWidth(4);
  viewport.LoadContent("aaaa\nbbbb\ncccc\n", "/tmp/ec-tab-mixed-hard.cpp");
  viewport.MoveCursorTo(0, 0);
  viewport.MoveCursorTo(0, 4, true);
  viewport.AddSecondaryCaret(2, 2);

  Expect(microide::editor::ApplyMixedTabKey(viewport), "the mixed apply should change the buffer");
  const std::string text = std::string(viewport.lines()[0]) + "/" +
                           std::string(viewport.lines()[1]) + "/" +
                           std::string(viewport.lines()[2]);
  Expect(text == "\taaaa/bbbb/cc\tcc", "hard tabs shift and insert one tab each: " + text);
}

}

void RegisterEditorEdgeCaseTests(std::vector<TestCase>& tests) {
  AddTest(tests, "EditorEdgeCase/TabKeyOnAMixedCaretSetIndentsPerCursor",
          TestTabKeyOnAMixedCaretSetIndentsPerCursor);
  AddTest(tests, "EditorEdgeCase/TabKeyOnALargeMixedCaretSetStaysCorrect",
          TestTabKeyOnALargeMixedCaretSetStaysCorrect);
  AddTest(tests, "EditorEdgeCase/TabKeyOnAMixedCaretSetNeverEatsASecondarySelection",
          TestTabKeyOnAMixedCaretSetNeverEatsASecondarySelection);
  AddTest(tests, "EditorEdgeCase/TabKeyOnAMixedCaretSetIndentsASharedLineOnce",
          TestTabKeyOnAMixedCaretSetIndentsASharedLineOnce);
  AddTest(tests, "EditorEdgeCase/TabKeyOnAMixedCaretSetWithHardTabs",
          TestTabKeyOnAMixedCaretSetWithHardTabs);
  AddTest(tests, "EditorEdgeCase/CollapsedFoldRemovesEveryWrappedRowOfItsBody",
          TestCollapsedFoldRemovesEveryWrappedRowOfItsBody);
  AddTest(tests, "EditorEdgeCase/VisualRowsStayMonotonicAcrossAFoldUnderWrap",
          TestVisualRowsStayMonotonicAcrossAFoldUnderWrap);
  AddTest(tests, "EditorEdgeCase/MultiCaretEditAcrossACollapsedFoldKeepsBothCarets",
          TestMultiCaretEditAcrossACollapsedFoldKeepsBothCarets);
  AddTest(tests, "EditorEdgeCase/DeletingAcrossACollapsedFoldRemovesTheHiddenLines",
          TestDeletingAcrossACollapsedFoldRemovesTheHiddenLines);
  AddTest(tests, "EditorEdgeCase/MultiCaretShiftDownThenTypeReplacesEverySelection",
          TestMultiCaretShiftDownThenTypeReplacesEverySelection);
  AddTest(tests, "EditorEdgeCase/MultiCaretTouchingSelectionsStaySeparateAndStillEdit",
          TestMultiCaretTouchingSelectionsStaySeparateAndStillEdit);
  AddTest(tests, "EditorEdgeCase/MultiCaretOverlappingSelectionsMerge",
          TestMultiCaretOverlappingSelectionsMerge);
  AddTest(tests, "EditorEdgeCase/MultiCaretVerticalMoveAtBufferEdgeDedupes",
          TestMultiCaretVerticalMoveAtBufferEdgeDedupes);
  AddTest(tests, "EditorEdgeCase/MultiCaretUndoRestoresEveryCaret",
          TestMultiCaretUndoRestoresEveryCaret);
  AddTest(tests, "EditorEdgeCase/MultiCaretHomeOnWrappedRowsUsesViewLineStart",
          TestMultiCaretHomeOnWrappedRowsUsesViewLineStart);
  AddTest(tests, "EditorEdgeCase/MultiCaretEndOnWrappedRowsUsesViewLineEnd",
          TestMultiCaretEndOnWrappedRowsUsesViewLineEnd);
  AddTest(tests, "EditorEdgeCase/MultiCaretDownOnWrappedLineStepsOneVisualRow",
          TestMultiCaretDownOnWrappedLineStepsOneVisualRow);
  AddTest(tests, "EditorEdgeCase/MultiCaretMoveLineUpBlockedByTopCaretIsNoOp",
          TestMultiCaretMoveLineUpBlockedByTopCaretIsNoOp);
  AddTest(tests, "EditorEdgeCase/MultiCaretDeleteLineOnAdjacentLinesRemovesEachOnce",
          TestMultiCaretDeleteLineOnAdjacentLinesRemovesEachOnce);
  AddTest(tests, "EditorEdgeCase/SelectAllClearsSecondaryCarets", TestSelectAllClearsSecondaryCarets);
  AddTest(tests, "EditorEdgeCase/MultiCaretCopyAggregatesInDocumentOrder",
          TestMultiCaretCopyAggregatesInDocumentOrder);
  AddTest(tests, "EditorEdgeCase/SecondaryCaretClampsWhenItsLineShrinks",
          TestSecondaryCaretClampsWhenItsLineShrinks);
  AddTest(tests, "EditorEdgeCase/BoxSelectionOverShortLineClampsToThatLine",
          TestBoxSelectionOverShortLineClampsToThatLine);
  AddTest(tests, "EditorEdgeCase/MultiCaretDeleteSelectionsUndoRestoresText",
          TestMultiCaretDeleteSelectionsUndoRestoresText);
  AddTest(tests, "EditorEdgeCase/MultiCaretBackspaceAtColumnZeroJoinsEachLineOnce",
          TestMultiCaretBackspaceAtColumnZeroJoinsEachLineOnce);
  AddTest(tests, "EditorEdgeCase/MultiCaretBareCaretsCopyEveryCaretsLine",
          TestMultiCaretBareCaretsCopyEveryCaretsLine);
  AddTest(tests, "EditorEdgeCase/MultiCaretLineCutCapturesWhatItDeletes",
          TestMultiCaretLineCutCapturesWhatItDeletes);
  AddTest(tests, "EditorEdgeCase/MultiCaretLineVerbsExpandACollapsedFold",
          TestMultiCaretLineVerbsExpandACollapsedFold);
  AddTest(tests, "EditorEdgeCase/SingleCaretLineClipboardIsUnchanged",
          TestSingleCaretLineClipboardIsUnchanged);
  AddTest(tests, "EditorEdgeCase/MultiCaretPageUnderWrapStepsVisualRows",
          TestMultiCaretPageUnderWrapStepsVisualRows);
  AddTest(tests, "EditorEdgeCase/MultiCaretPageAtBufferEndDedupes",
          TestMultiCaretPageAtBufferEndDedupes);
  AddTest(tests, "EditorEdgeCase/SelectAllThenTypeUnderWrapLeavesOneCaret",
          TestSelectAllThenTypeUnderWrapLeavesOneCaret);
  AddTest(tests, "EditorEdgeCase/SortLinesDescending",
          TestSortLinesDescending);
  AddTest(tests, "EditorEdgeCase/SortLinesTwoDisjointRegions",
          TestSortLinesTwoDisjointRegions);
  AddTest(tests, "EditorEdgeCase/ToggleLineCommentMixedRegionCommentsAll",
          TestToggleLineCommentMixedRegionCommentsAll);
  AddTest(tests, "EditorEdgeCase/CopyLinesUpWithAdjacentCarets",
          TestCopyLinesUpWithAdjacentCarets);
  AddTest(tests, "EditorEdgeCase/BoxSelectionAcrossATabKeepsTheRectangle",
          TestBoxSelectionAcrossATabKeepsTheRectangle);
  AddTest(tests, "EditorEdgeCase/BoxSelectionTypingUndoRestores",
          TestBoxSelectionTypingUndoRestores);
  AddTest(tests, "EditorEdgeCase/OutdentMixedTabAndSpaces",
          TestOutdentMixedTabAndSpaces);
  AddTest(tests, "EditorEdgeCase/SelectWordAtCursorOnWhitespaceRun",
          TestSelectWordAtCursorOnWhitespaceRun);
  AddTest(tests, "EditorEdgeCase/DeleteWordForwardAtLineEndJoins",
          TestDeleteWordForwardAtLineEndJoins);
  AddTest(tests, "EditorEdgeCase/SelectWordAtCursorAtWordEnd", TestSelectWordAtCursorAtWordEnd);
  AddTest(tests, "EditorEdgeCase/SelectWordAtCursorAtLineEnd", TestSelectWordAtCursorAtLineEnd);
  AddTest(tests, "EditorEdgeCase/SelectWordAtCursorOnSeparatorRun", TestSelectWordAtCursorOnSeparatorRun);
  AddTest(tests, "EditorEdgeCase/JoinLinesBareCaretTakesTheLineBelow",
          TestJoinLinesBareCaretTakesTheLineBelow);
  AddTest(tests, "EditorEdgeCase/JoinLinesSpacingEdges", TestJoinLinesSpacingEdges);
  AddTest(tests, "EditorEdgeCase/JoinLinesAtEveryCaret", TestJoinLinesAtEveryCaret);
  AddTest(tests, "EditorEdgeCase/JoinLinesAtTheLastLineIsANoOp",
          TestJoinLinesAtTheLastLineIsANoOp);
  AddTest(tests, "EditorEdgeCase/ToggleBlockCommentOnAWholeLineSelection",
          TestToggleBlockCommentOnAWholeLineSelection);
  AddTest(tests, "EditorEdgeCase/ToggleBlockCommentReachesEveryCaret",
          TestToggleBlockCommentReachesEveryCaret);
  AddTest(tests, "EditorEdgeCase/ToggleBlockCommentWithTwoCaretsOnOneLine",
          TestToggleBlockCommentWithTwoCaretsOnOneLine);
  AddTest(tests, "EditorEdgeCase/UpOnTheFirstRowGoesToRowStartPerCaret",
          TestUpOnTheFirstRowGoesToRowStartPerCaret);
  AddTest(tests, "EditorEdgeCase/DownOnTheLastRowGoesToRowEndPerCaret",
          TestDownOnTheLastRowGoesToRowEndPerCaret);
  AddTest(tests, "EditorEdgeCase/ShiftUpOnTheFirstRowExtendsToRowStart",
          TestShiftUpOnTheFirstRowExtendsToRowStart);
  AddTest(tests, "EditorEdgeCase/UpDownAtTheEdgeUnderWrapUsesVisualRows",
          TestUpDownAtTheEdgeUnderWrapUsesVisualRows);
  AddTest(tests, "EditorEdgeCase/PlainVerticalMoveOverASelectionStartsFromItsEdge",
          TestPlainVerticalMoveOverASelectionStartsFromItsEdge);
  AddTest(tests, "EditorEdgeCase/PlainUpOverAForwardSelectionStartsFromItsStart",
          TestPlainUpOverAForwardSelectionStartsFromItsStart);
  AddTest(tests, "EditorEdgeCase/TouchingReversedSelectionAtThePrimaryCaretSurvives",
          TestTouchingReversedSelectionAtThePrimaryCaretSurvives);
  AddTest(tests, "EditorEdgeCase/MultiCaretTabAtASharedPositionPadsBothSites",
          TestMultiCaretTabAtASharedPositionPadsBothSites);
  AddTest(tests, "EditorEdgeCase/MultiCaretTabReplacesSingleLineSelectionsFromTheirStart",
          TestMultiCaretTabReplacesSingleLineSelectionsFromTheirStart);
  AddTest(tests, "EditorEdgeCase/TabKeyClassifierIndentsWholeLineAndSecondarySelections",
          TestTabKeyClassifierIndentsWholeLineAndSecondarySelections);
}

}
