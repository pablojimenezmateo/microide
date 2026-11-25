#include "TestSupport.h"

#include <vector>

#include "editor/FoldingModel.h"
#include "editor/LanguageContractView.h"
#include "editor/ShapingActions.h"
#include "editor/TextBuffer.h"
#include "editor/TextLayout.h"
#include "editor/TextViewport.h"
#include "util/PerformanceCounters.h"

namespace microide::tests {
namespace {

using microide::editor::FoldingModel;
using microide::editor::TextPosition;
using microide::editor::SelectionRange;
using microide::editor::TextLayout;
using microide::editor::TextViewport;

FoldingModel::ComputeOptions DefaultFoldOptions() {
  FoldingModel::ComputeOptions options;
  options.bracket_pairs = {{'{', '}'}};
  options.use_indent_source = true;
  options.tab_size = 4;
  return options;
}

microide::editor::LanguageContractView MakeCStyleContractView() {
  microide::editor::LanguageContractView view;
  view.auto_close_pairs = {{"(", ")"}, {"[", "]"}, {"{", "}"}};
  view.surround_pairs = view.auto_close_pairs;
  view.indent_after_open_patterns = {"{", "(", "["};
  view.dedent_on_close_chars = {"}", ")", "]"};
  view.line_comment = "//";
  view.auto_close_enabled = true;
  view.surround_enabled = true;
  view.smart_indent_enabled = true;
  return view;
}

void TestMultiCaretSplitBracesAtEveryCaret() {
  TextViewport viewport;
  viewport.LoadContent("foo() {}\nfoo() {}", "/tmp/mc-split-braces.cpp");
  viewport.SetLanguageContractView(MakeCStyleContractView());
  viewport.SetSoftTabs(true);
  viewport.SetIndentWidth(2);
  viewport.MoveCursorTo(0, 7);
  viewport.SetSecondaryCarets({{1, 7}});

  viewport.InsertNewline();

  Expect(viewport.lines().size() == 6,
         "each caret between a brace pair should split into three lines");
  Expect(viewport.lines()[0] == "foo() {" && viewport.lines()[1] == "  " &&
             viewport.lines()[2] == "}",
         "first caret should produce opener / indented body / closer");
  Expect(viewport.lines()[3] == "foo() {" && viewport.lines()[4] == "  " &&
             viewport.lines()[5] == "}",
         "second caret should produce opener / indented body / closer");
  Expect(viewport.cursor_line() == 1 && viewport.cursor_column() == 2,
         "primary caret should land on its indented body line");
  Expect(viewport.secondary_carets().size() == 1 &&
             viewport.secondary_carets().front() == TextPosition{4, 2},
         "secondary caret should land on its body line, shifted by the upper split");

  Expect(viewport.Undo(), "multi-caret brace split should undo as one step");
  Expect(viewport.lines().size() == 2 && viewport.lines()[0] == "foo() {}" &&
             viewport.lines()[1] == "foo() {}",
         "undo should atomically restore both original lines");
}

void TestMultiCaretPasteWithLoneCarriageReturnRemapsCarets() {
  TextViewport viewport;
  viewport.LoadContent("abcdefgh", "/tmp/mc-lone-cr.txt");
  viewport.MoveCursorTo(0, 2);
  viewport.SetSecondaryCarets({{0, 5}});

  viewport.InsertText("X\rY");

  Expect(viewport.lines().size() == 3,
         "the lone CR should insert a real line break at each caret");
  Expect(viewport.lines()[0] == "abX" && viewport.lines()[1] == "YcdeX" &&
             viewport.lines()[2] == "Yfgh",
         "both carets should paste the normalized two-line text");
  Expect(viewport.cursor_line() == 1 && viewport.cursor_column() == 1,
         "the primary caret lands after its pasted 'Y'");
  Expect(viewport.secondary_carets().size() == 1 &&
             viewport.secondary_carets().front() == TextPosition{2, 1},
         "the secondary caret follows its pasted text down to the third line");
}

void TestMultiCaretSplitBracesMixedWithPlainNewline() {
  TextViewport viewport;
  viewport.LoadContent("foo() {}\nbar baz", "/tmp/mc-split-braces-mixed.cpp");
  viewport.SetLanguageContractView(MakeCStyleContractView());
  viewport.SetSoftTabs(true);
  viewport.SetIndentWidth(2);
  viewport.MoveCursorTo(0, 7);
  viewport.SetSecondaryCarets({{1, 3}});

  viewport.InsertNewline();

  Expect(viewport.lines().size() == 5,
         "brace caret splits into three lines, plain caret into two");
  Expect(viewport.lines()[0] == "foo() {" && viewport.lines()[1] == "  " &&
             viewport.lines()[2] == "}",
         "the brace caret should still split across three lines");
  Expect(viewport.lines()[3] == "bar" && viewport.lines()[4] == " baz",
         "the non-brace caret should receive a plain newline split");
  Expect(viewport.cursor_line() == 1 && viewport.cursor_column() == 2,
         "primary caret should land on its indented brace body");
  Expect(viewport.secondary_carets().size() == 1 &&
             viewport.secondary_carets().front() == TextPosition{4, 0},
         "plain-newline caret should land at the start of the wrapped remainder");

  Expect(viewport.Undo(), "mixed multi-caret newline should undo as one step");
  Expect(viewport.lines().size() == 2 && viewport.lines()[0] == "foo() {}" &&
             viewport.lines()[1] == "bar baz",
         "undo should atomically restore both original lines");
}

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
  Expect(folding_model.Compute(viewport.lines().Snapshot(), DefaultFoldOptions()),
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
  Expect(folding_model.Compute(viewport.lines().Snapshot(), DefaultFoldOptions()),
         "page fixture should compute folds");
  Expect(folding_model.Collapse(1), "inner fold should collapse");

  viewport.SetFoldingModel(&folding_model);
  viewport.SetViewportSize(2, 20);

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

void TestSoftWrapMultiCaretInsertAppliesAtEveryCaret() {
  TextViewport viewport;
  viewport.LoadContent("abcdefghijklmnopqrst\nabcdefghijk\nqrstuvwxyzabcdefgh\n",
                       "/tmp/mc-softwrap-insert.txt");
  viewport.SetViewportSize(10, 8);
  viewport.SetSoftWrap(true);
  viewport.MoveCursorTo(0, 7);
  viewport.SetSecondaryCarets({{1, 3}, {2, 5}});

  viewport.InsertText("X");

  Expect(viewport.lines()[0] == "abcdefgXhijklmnopqrst",
         "primary caret on a wrapped line should receive the insertion");
  Expect(viewport.lines()[1] == "abcXdefghijk",
         "secondary caret on a short wrapped line should receive the insertion");
  Expect(viewport.lines()[2] == "qrstuXvwxyzabcdefgh",
         "secondary caret on a wrapped line should receive the insertion");
  Expect(viewport.secondary_carets().size() == 2,
         "inserting across wrapped multi-carets should preserve every secondary caret");
  Expect(viewport.cursor_line() == 0 && viewport.cursor_column() == 8,
         "the primary caret should advance past its own insertion");

  Expect(viewport.Undo(), "soft-wrap multi-caret insert should undo as one step");
  Expect(viewport.lines()[0] == "abcdefghijklmnopqrst" &&
             viewport.lines()[1] == "abcdefghijk" &&
             viewport.lines()[2] == "qrstuvwxyzabcdefgh",
         "undo should atomically restore every wrapped line");
}

void TestSoftWrapMultiCaretBackspaceErasesAtEveryCaret() {
  TextViewport viewport;
  viewport.LoadContent("abcdefghijklmnopqrst\nabcdefghijk\nqrstuvwxyzabcdefgh\n",
                       "/tmp/mc-softwrap-backspace.txt");
  viewport.SetViewportSize(10, 8);
  viewport.SetSoftWrap(true);
  viewport.MoveCursorTo(0, 7);
  viewport.SetSecondaryCarets({{1, 3}, {2, 5}});

  viewport.Backspace();

  Expect(viewport.lines()[0] == "abcdefhijklmnopqrst",
         "primary caret should erase the column before it on a wrapped line");
  Expect(viewport.lines()[1] == "abdefghijk",
         "secondary caret should erase the column before it on a short wrapped line");
  Expect(viewport.lines()[2] == "qrstvwxyzabcdefgh",
         "secondary caret should erase the column before it on a wrapped line");
  Expect(viewport.secondary_carets().size() == 2,
         "backspacing across wrapped multi-carets should preserve every secondary caret");
}

void TestSoftWrapMultiCaretDeleteForwardErasesAtEveryCaret() {
  TextViewport viewport;
  viewport.LoadContent("abcdefghijklmnopqrst\nabcdefghijk\nqrstuvwxyzabcdefgh\n",
                       "/tmp/mc-softwrap-delete.txt");
  viewport.SetViewportSize(10, 8);
  viewport.SetSoftWrap(true);
  viewport.MoveCursorTo(0, 7);
  viewport.SetSecondaryCarets({{1, 3}, {2, 5}});

  viewport.DeleteForward();

  Expect(viewport.lines()[0] == "abcdefgijklmnopqrst",
         "primary caret should delete the column at it on a wrapped line");
  Expect(viewport.lines()[1] == "abcefghijk",
         "secondary caret should delete the column at it on a short wrapped line");
  Expect(viewport.lines()[2] == "qrstuwxyzabcdefgh",
         "secondary caret should delete the column at it on a wrapped line");
  Expect(viewport.secondary_carets().size() == 2,
         "delete-forward across wrapped multi-carets should preserve every secondary caret");
}

void TestMultiCaretInsertPreservesPrimaryAmongSameLineCarets() {
  TextViewport viewport;
  viewport.LoadContent("abcdef\n", "/tmp/mc-sameline-insert.txt");
  viewport.MoveCursorTo(0, 5);
  viewport.SetSecondaryCarets({{0, 1}, {0, 3}});

  viewport.InsertText("X");

  Expect(viewport.lines()[0] == "aXbcXdeXf",
         "each caret on the shared line should receive exactly one insertion");
  Expect(viewport.secondary_carets().size() == 2,
         "both secondary carets should survive a same-line multi-caret insert");
  Expect(viewport.cursor_line() == 0 && viewport.cursor_column() == 8,
         "the primary caret should remain the active cursor and advance past its insertion");
  Expect(viewport.secondary_carets()[0] == TextPosition{0, 2} &&
             viewport.secondary_carets()[1] == TextPosition{0, 5},
         "each same-line caret should sit just after its own insertion");
}

void TestMultiCaretMultiCharInsertShiftsSameLineCarets() {
  TextViewport viewport;
  viewport.LoadContent("abcdef\n", "/tmp/mc-sameline-multichar.txt");
  viewport.MoveCursorTo(0, 5);
  viewport.SetSecondaryCarets({{0, 1}, {0, 3}});

  viewport.InsertText("YZ");

  Expect(viewport.lines()[0] == "aYZbcYZdeYZf",
         "each same-line caret should receive the full multi-character insertion");
  Expect(viewport.cursor_line() == 0 && viewport.cursor_column() == 11,
         "the primary caret should advance past its multi-character insertion accounting for "
         "lower insertions");
  Expect(viewport.secondary_carets().size() == 2 &&
             viewport.secondary_carets()[0] == TextPosition{0, 3} &&
             viewport.secondary_carets()[1] == TextPosition{0, 7},
         "lower multi-character insertions should shift higher same-line carets");
}

void TestMultiCaretBackspaceShiftsSameLineCarets() {
  TextViewport viewport;
  viewport.LoadContent("abcdefgh\n", "/tmp/mc-sameline-backspace.txt");
  viewport.MoveCursorTo(0, 6);
  viewport.SetSecondaryCarets({{0, 2}, {0, 4}});

  viewport.Backspace();

  Expect(viewport.lines()[0] == "acegh",
         "each same-line caret should erase exactly the column before it");
  Expect(viewport.cursor_line() == 0 && viewport.cursor_column() == 3,
         "the primary caret should track its erase after lower erases shift it left");
  Expect(viewport.secondary_carets().size() == 2 &&
             viewport.secondary_carets()[0] == TextPosition{0, 1} &&
             viewport.secondary_carets()[1] == TextPosition{0, 2},
         "lower same-line erases should shift higher carets left without losing any");
}

void TestSetSecondaryCaretsClampCollapseDedupeAndPrimaryDrop() {
  TextViewport viewport;
  viewport.LoadContent("abcdef\nxy\nghijk", "/tmp/mc-clamp-collapse.txt");
  viewport.MoveCursorTo(0, 2);

  viewport.SetSecondaryCarets({{2, 1}, {1, 5}, {0, 2}, {1, 9}, {2, 1}, {99, 0}});

  const auto& carets = viewport.secondary_carets();
  Expect(carets.size() == 3,
         "clamp-collapse + dedupe + primary-drop should leave exactly three carets");
  Expect(carets[0] == TextPosition{1, 2},
         "the two short-line carets clamp-collapse to (1,2) and sort first");
  Expect(carets[1] == TextPosition{2, 0},
         "the out-of-range line 99 clamps into the last line at column 0");
  Expect(carets[2] == TextPosition{2, 1},
         "the duplicate (2,1) carets dedupe to one, sorted last");
}

void TestMultiCaretBackspaceReplacesEachSelection() {
  TextViewport viewport;
  viewport.LoadContent("foo foo", "/tmp/mc-sel-backspace.cpp");
  viewport.MoveCursorTo(0, 1);
  viewport.SelectWordAtCursor();
  viewport.AddSecondaryCaretWithRange(
      microide::editor::SelectionRange{TextPosition{0, 4}, TextPosition{0, 7}});
  Expect(viewport.has_multiple_carets(), "fixture should have two carets");

  viewport.Backspace();
  Expect(viewport.lines().size() == 1 && viewport.lines()[0] == " ",
         "multi-caret Backspace should delete both selected words, leaving the separating space");
}

void TestMultiCaretPasteReplacesEachSelection() {
  TextViewport viewport;
  viewport.LoadContent("foo foo", "/tmp/mc-sel-paste.cpp");
  viewport.MoveCursorTo(0, 1);
  viewport.SelectWordAtCursor();
  viewport.AddSecondaryCaretWithRange(
      microide::editor::SelectionRange{TextPosition{0, 4}, TextPosition{0, 7}});

  viewport.InsertText("XY");
  Expect(viewport.lines().size() == 1 && viewport.lines()[0] == "XY XY",
         "multi-caret paste should replace each selection with the pasted text");
}

void TestMultiCaretCopyAggregatesSelections() {
  TextViewport viewport;
  viewport.LoadContent("foo foo", "/tmp/mc-copy.cpp");
  viewport.MoveCursorTo(0, 1);
  viewport.SelectWordAtCursor();
  viewport.AddSecondaryCaretWithRange(
      microide::editor::SelectionRange{TextPosition{0, 4}, TextPosition{0, 7}});
  const auto text = viewport.MultiCaretSelectedText();
  Expect(text.has_value() && *text == "foo\nfoo",
         "multi-caret copy should join each caret's selection in position order");
}

void TestMultiCaretCopyRequiresAllSelections() {
  TextViewport viewport;
  viewport.LoadContent("foo foo", "/tmp/mc-copy-partial.cpp");
  viewport.MoveCursorTo(0, 1);
  viewport.SelectWordAtCursor();
  viewport.AddSecondaryCaret(0, 5);
  Expect(!viewport.MultiCaretSelectedText().has_value(),
         "aggregate copy should decline (nullopt) when any caret lacks a selection");
}

void TestMultiCaretDistributePasteOneLinePerCaret() {
  TextViewport viewport;
  viewport.LoadContent("foo foo", "/tmp/mc-dist-paste.cpp");
  viewport.MoveCursorTo(0, 1);
  viewport.SelectWordAtCursor();
  viewport.AddSecondaryCaretWithRange(
      microide::editor::SelectionRange{TextPosition{0, 4}, TextPosition{0, 7}});
  viewport.PasteText("AAA\nBBB");
  Expect(viewport.lines().size() == 1 && viewport.lines()[0] == "AAA BBB",
         "distribute-paste should place line 1 at the first caret, line 2 at the second");
}

void TestMultiCaretDistributePasteIgnoresOneTrailingLineBreak() {
  TextViewport viewport;
  viewport.LoadContent("a\nb\nc", "/tmp/mc-trailing-paste.txt");
  viewport.MoveCursorTo(0, 1);
  viewport.AddSecondaryCaret(1, 1);
  viewport.AddSecondaryCaret(2, 1);
  viewport.PasteText("X\nY\nZ\r\n");
  Expect(viewport.lines().size() == 3 && viewport.lines()[0] == "aX" &&
             viewport.lines()[1] == "bY" && viewport.lines()[2] == "cZ",
         "one trailing line break is not a fourth line");
}

void TestMultiCaretPasteCountMismatchInsertsFullTextAtEachCaret() {
  TextViewport viewport;
  viewport.LoadContent("foo foo", "/tmp/mc-mismatch-paste.cpp");
  viewport.MoveCursorTo(0, 1);
  viewport.SelectWordAtCursor();
  viewport.AddSecondaryCaretWithRange(
      microide::editor::SelectionRange{TextPosition{0, 4}, TextPosition{0, 7}});
  viewport.PasteText("A\nB\nC");
  Expect(viewport.lines().size() == 5,
         "a caret/line count mismatch must insert the full payload at each caret, not distribute");
}

void TestMultiCaretDeleteSelectionsRemovesAllAndUndoesAtomically() {
  TextViewport viewport;
  viewport.LoadContent("foo foo", "/tmp/mc-cut.cpp");
  viewport.MoveCursorTo(0, 1);
  viewport.SelectWordAtCursor();
  viewport.AddSecondaryCaretWithRange(
      microide::editor::SelectionRange{TextPosition{0, 4}, TextPosition{0, 7}});
  const auto text = viewport.MultiCaretSelectedText();
  Expect(text.has_value() && *text == "foo\nfoo", "cut should aggregate both selections");
  Expect(viewport.DeleteMultiCaretSelections(), "deleting all caret selections should succeed");
  Expect(viewport.lines().size() == 1 && viewport.lines()[0] == " ",
         "both selections removed, leaving the separating space");
  Expect(viewport.Undo() && viewport.lines()[0] == "foo foo",
         "the multi-caret cut deletion should undo in one step");
}

void TestSetSecondaryCaretsWithRangesPreservesSelections() {
  TextViewport viewport;
  viewport.LoadContent("foo foo foo", "/tmp/mc-ranges.cpp");
  viewport.MoveCursorTo(0, 1);
  viewport.SelectWordAtCursor();
  viewport.SetSecondaryCaretsWithRanges(std::vector<microide::editor::SelectionRange>{
      microide::editor::SelectionRange{TextPosition{0, 4}, TextPosition{0, 7}},
      microide::editor::SelectionRange{TextPosition{0, 8}, TextPosition{0, 11}},
  });
  Expect(viewport.has_multiple_carets(), "should register two secondary carets");

  const auto copied = viewport.MultiCaretSelectedText();
  Expect(copied.has_value() && *copied == "foo\nfoo\nfoo",
         "ranged secondaries let multi-caret copy aggregate every match");

  viewport.InsertText("x");
  Expect(viewport.lines().size() == 1 && viewport.lines()[0] == "x x x",
         "typing over ranged multi-carets replaces each match instead of inserting");
}

void TestSetSecondaryCaretsWithRangesEmptyRangeIsBareCaret() {
  TextViewport viewport;
  viewport.LoadContent("foo foo", "/tmp/mc-ranges-empty.cpp");
  viewport.MoveCursorTo(0, 1);
  viewport.SelectWordAtCursor();
  viewport.SetSecondaryCaretsWithRanges(std::vector<microide::editor::SelectionRange>{
      microide::editor::SelectionRange{TextPosition{0, 5}, TextPosition{0, 5}},
  });
  Expect(!viewport.MultiCaretSelectedText().has_value(),
         "an empty range must produce a selection-less secondary caret");
}

void TestNonExtendingMoveCollapsesSecondarySelections() {
  TextViewport viewport;
  viewport.LoadContent("aaa bbb ccc", "/tmp/mc-move-collapse.cpp");
  viewport.MoveCursorTo(0, 1);
  viewport.SelectWordAtCursor();
  viewport.SetSecondaryCaretsWithRanges(std::vector<microide::editor::SelectionRange>{
      microide::editor::SelectionRange{TextPosition{0, 4}, TextPosition{0, 7}},
      microide::editor::SelectionRange{TextPosition{0, 8}, TextPosition{0, 11}},
  });
  Expect(viewport.MultiCaretSelectedText().has_value(),
         "fixture: every caret starts with a selection");

  viewport.MoveCursorHorizontal(1, false);

  Expect(!viewport.has_selection(),
         "the primary selection collapses on a non-extending move");
  const auto ranges = viewport.secondary_caret_ranges();
  bool any_secondary_selection = false;
  for (const auto& caret : ranges) {
    if (caret.selection_anchor.has_value()) {
      any_secondary_selection = true;
    }
  }
  Expect(!any_secondary_selection,
         "every secondary selection also collapses on a non-extending move");

  viewport.InsertText("X");
  Expect(viewport.lines().size() == 1 && viewport.lines()[0] == "aaaX bbbX cccX",
         "after collapsing to the selection edge, a keystroke inserts at every caret");
}

void TestNonExtendingHorizontalMoveCollapsesToSelectionEdge() {
  TextViewport viewport;
  viewport.LoadContent("hello world", "/tmp/mc-collapse-edge.cpp");
  viewport.MoveCursorTo(0, 1);
  viewport.SelectWordAtCursor();
  Expect(viewport.has_selection(), "fixture: a word is selected");

  viewport.MoveCursorHorizontal(1, false);
  Expect(!viewport.has_selection() && viewport.cursor_column() == 5,
         "Right over a selection lands on its right edge, not one past it");

  viewport.MoveCursorTo(0, 1);
  viewport.SelectWordAtCursor();
  viewport.MoveCursorHorizontal(-1, false);
  Expect(!viewport.has_selection() && viewport.cursor_column() == 0,
         "Left over a selection lands on its left edge");
}

void TestExtendingMoveStartsSelectionAtEverySecondaryCaret() {
  TextViewport viewport;
  viewport.LoadContent("aaa bbb ccc", "/tmp/mc-extend-start.cpp");
  viewport.MoveCursorTo(0, 0);
  viewport.SetSecondaryCarets({{0, 4}, {0, 8}});
  Expect(!viewport.MultiCaretSelectedText().has_value(),
         "fixture: no caret starts with a selection");

  viewport.MoveCursorHorizontal(1, true);

  const auto ranges = viewport.secondary_caret_ranges();
  bool all_secondaries_selected = !ranges.empty();
  for (const auto& caret : ranges) {
    if (!caret.selection_anchor.has_value()) {
      all_secondaries_selected = false;
    }
  }
  Expect(viewport.has_selection() && all_secondaries_selected,
         "an extending move starts a selection at every caret, primary and secondary");
  const auto copied = viewport.MultiCaretSelectedText();
  Expect(copied.has_value() && *copied == "a\nb\nc",
         "each caret's freshly started selection covers the char it moved over");
}

}

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

void TestSetBoxSelectionSelectsEachLineAcrossColumns() {
  TextViewport viewport;
  viewport.LoadContent("abcdef\nghijkl\nmnopqr\n", "/tmp/mc-box-select.txt");

  viewport.SetBoxSelection(TextPosition{0, 1}, TextPosition{2, 4});

  Expect(viewport.cursor_line() == 2 && viewport.cursor_column() == 4,
         "box selection should leave the primary caret on the caret corner");
  const auto primary = viewport.selection_range();
  Expect(primary.has_value() && primary->start == TextPosition{2, 1} &&
             primary->end == TextPosition{2, 4},
         "the caret line should carry the box selection as the primary selection");

  const auto ranges = viewport.secondary_caret_ranges();
  Expect(ranges.size() == 2, "box selection should add a secondary caret per other line");
  Expect(ranges[0].position == TextPosition{0, 4} &&
             ranges[0].selection_anchor == std::optional<TextPosition>(TextPosition{0, 1}),
         "line 0 should select columns 1..4 with its caret at the caret column");
  Expect(ranges[1].position == TextPosition{1, 4} &&
             ranges[1].selection_anchor == std::optional<TextPosition>(TextPosition{1, 1}),
         "line 1 should select columns 1..4 with its caret at the caret column");
}

void TestSetBoxSelectionIsRectangularInVisualColumns() {
  TextViewport viewport;
  viewport.LoadContent("a\tb\nabcdefgh\n\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\n", "/tmp/mc-box-visual.txt");
  viewport.SetTabSize(4);

  viewport.SetBoxSelection(TextPosition{0, 1}, TextPosition{1, 6});
  const auto primary = viewport.selection_range();
  Expect(primary.has_value() && primary->start == TextPosition{1, 1} &&
             primary->end == TextPosition{1, 6},
         "the ASCII caret line spans visual columns 1..6 as bytes 1..6");
  const auto ranges = viewport.secondary_caret_ranges();
  Expect(ranges.size() == 1, "one secondary range for the other line");
  Expect(ranges.size() == 1 && ranges[0].selection_anchor == std::optional<TextPosition>(TextPosition{0, 1}) &&
             ranges[0].position == TextPosition{0, 3},
         "the tab line maps the visual columns through its tab");

  viewport.SetBoxSelection(TextPosition{0, 1}, TextPosition{2, 10});
  const auto primary2 = viewport.selection_range();
  Expect(primary2.has_value() && primary2->start == TextPosition{2, 2} &&
             primary2->end == TextPosition{2, 8},
         "the accented line selects whole characters at the box's visual columns");
  const auto ranges2 = viewport.secondary_caret_ranges();
  Expect(ranges2.size() == 2 && ranges2[1].selection_anchor == std::optional<TextPosition>(TextPosition{1, 1}) &&
             ranges2[1].position == TextPosition{1, 6},
         "the ASCII line keeps bytes 1..6");

  viewport.SetBoxSelection(TextPosition{0, 6}, TextPosition{1, 0});
  const auto primary3 = viewport.selection_range();
  Expect(primary3.has_value() && primary3->start == TextPosition{1, 0} &&
             primary3->end == TextPosition{1, 8},
         "a corner beyond its line's end is one cell per extra byte");
  Expect(viewport.MaxVisualWidthInSpan(0, 2) == 8, "the widest line counts cells, not bytes");
}

void TestBoxColumnsOnLineTabArithmeticAgreesWithTheWalk() {
  std::uint64_t state = 0x2545f4914f6cdd1dULL;
  const auto next = [&](std::size_t bound) {
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    return static_cast<std::size_t>(state % bound);
  };
  for (int round = 0; round < 200; ++round) {
    const std::size_t tabs = next(5);
    const std::size_t tail = next(12);
    std::string line(tabs, '\t');
    for (std::size_t i = 0; i < tail; ++i) {
      line.push_back(static_cast<char>('a' + next(26)));
    }
    if (next(4) == 0) {
      line += "\t";
    }
    if (next(6) == 0) {
      line += "\xc3\xa9";
    }
    TextViewport viewport;
    viewport.LoadContent(line + "\nother\n", "/tmp/mc-box-tabs.txt");
    const std::size_t tab_size = 2 + next(7);
    viewport.SetTabSize(tab_size);
    if (round % 2 == 0) {
      (void)viewport.max_visual_columns();
    }
    const std::size_t width = TextLayout::VisualColumnForTextColumn(line, line.size(), tab_size);
    for (std::size_t v1 = 0; v1 <= width + 2; ++v1) {
      const std::size_t v2 = next(width + 3);
      std::size_t a = 0;
      std::size_t c = 0;
      viewport.BoxColumnsOnLine(0, v1, v2, &a, &c);
      const std::size_t expected_a = TextLayout::TextColumnForVisualColumn(line, v1, tab_size);
      const std::size_t expected_c = TextLayout::TextColumnForVisualColumn(line, v2, tab_size);
      const std::size_t via_accessor = viewport.TextColumnAtVisualColumn(0, v1);
      const std::size_t back = viewport.VisualColumnAt(0, expected_a);
      if (a != expected_a || c != expected_c || via_accessor != expected_a ||
          back != TextLayout::VisualColumnForTextColumn(line, expected_a, tab_size)) {
        Expect(false, "box column mapping disagrees with the walk on tabs=" + std::to_string(tabs) +
                          " tail=" + std::to_string(tail) + " tab_size=" + std::to_string(tab_size) +
                          " v1=" + std::to_string(v1) + " v2=" + std::to_string(v2) + ": got " +
                          std::to_string(a) + "," + std::to_string(c) + " expected " +
                          std::to_string(expected_a) + "," + std::to_string(expected_c));
        return;
      }
    }
    Expect(viewport.MaxVisualWidthInSpan(0, 0) == width, "the span width agrees with the walk");
  }
}

void TestMultiCaretSelectAllAndCutLeaveATypableCaretSet() {
  TextViewport viewport;
  viewport.LoadContent("foo foo\nfoo bar\n", "/tmp/mc-typable.txt");
  const auto select_every_foo = [&] {
    viewport.MoveCursorTo(0, 0, false);
    viewport.MoveCursorTo(0, 3, true);
    const std::vector<SelectionRange> ranges = {
        SelectionRange{TextPosition{0, 4}, TextPosition{0, 7}},
        SelectionRange{TextPosition{1, 0}, TextPosition{1, 3}},
    };
    viewport.SetSecondaryCaretsWithRanges(ranges);
  };

  select_every_foo();
  viewport.SelectAll();
  Expect(!viewport.has_multiple_carets(), "select-all leaves a single selection");
  viewport.InsertText("Q");
  Expect(viewport.lines().Snapshot() == std::vector<std::string>{"Q"},
         "typing after select-all replaces the whole buffer (got " +
             std::to_string(viewport.line_count()) + " lines, first <" + viewport.lines()[0] + ">)");

  viewport.LoadContent("foo foo\nfoo bar\n", "/tmp/mc-typable.txt");
  select_every_foo();
  viewport.DeleteMultiCaretSelections();
  Expect(viewport.lines().Snapshot() == std::vector<std::string>{" ", " bar", ""},
         "the multi-caret delete removes every selection (got <" + viewport.lines()[0] + "|" +
             viewport.lines()[1] + ">)");
  Expect(viewport.has_multiple_carets() && !viewport.has_selection(),
         "the carets survive the delete, collapsed");
  viewport.InsertText("R");
  Expect(viewport.lines().Snapshot() == std::vector<std::string>{"R R", "R bar", ""},
         "typing after the delete lands at every caret (got <" + viewport.lines()[0] + "|" +
             viewport.lines()[1] + ">)");

  TemporaryDirectory temp_dir;
  const std::filesystem::path path = temp_dir.path() / "typable.txt";
  WriteFile(path, "foo foo\nfoo bar\n");
  TextViewport saved;
  Expect(saved.OpenFile(path), "the fixture opens");
  saved.SetSaveTrimTrailingWhitespace(true);
  saved.MoveCursorTo(0, 0, false);
  saved.MoveCursorTo(0, 3, true);
  const std::vector<SelectionRange> foo_ranges = {
      SelectionRange{TextPosition{0, 4}, TextPosition{0, 7}},
      SelectionRange{TextPosition{1, 0}, TextPosition{1, 3}},
  };
  saved.SetSecondaryCaretsWithRanges(foo_ranges);
  saved.DeleteMultiCaretSelections();
  Expect(saved.Save(microide::project::LocalFileWriteGate()), "the save with trim succeeds");
  Expect(saved.lines()[0].empty(), "the trim emptied the line of one space");
  saved.InsertText("R");
  Expect(saved.lines().Snapshot() == std::vector<std::string>{"R", "R bar", ""},
         "typing after the trimming save lands at the merged caret and the other one (got <" +
             saved.lines()[0] + "|" + saved.lines()[1] + ">)");
}

void TestSetBoxSelectionLeftwardKeepsCaretsOnCaretColumn() {
  TextViewport viewport;
  viewport.LoadContent("abcdef\nghijkl\nmnopqr\n", "/tmp/mc-box-left.txt");

  viewport.SetBoxSelection(TextPosition{0, 4}, TextPosition{2, 1});

  Expect(viewport.cursor_line() == 2 && viewport.cursor_column() == 1,
         "primary caret lands on the caret corner (column 1)");
  const auto primary = viewport.selection_range();
  Expect(primary.has_value() && primary->start == TextPosition{2, 1} &&
             primary->end == TextPosition{2, 4},
         "primary row still selects columns 1..4");

  const auto ranges = viewport.secondary_caret_ranges();
  Expect(ranges.size() == 2, "one secondary caret per other line");
  Expect(ranges[0].position == TextPosition{0, 1} &&
             ranges[0].selection_anchor == std::optional<TextPosition>(TextPosition{0, 4}),
         "line 0 caret sits on the caret column (1), anchor on the far edge (4)");
  Expect(ranges[1].position == TextPosition{1, 1} &&
             ranges[1].selection_anchor == std::optional<TextPosition>(TextPosition{1, 4}),
         "line 1 caret sits on the caret column (1), anchor on the far edge (4)");
}

void TestSetBoxSelectionClampsShortLinesToEndOfLine() {
  TextViewport viewport;
  viewport.LoadContent("aaaaaa\nbb\ncccccc\n", "/tmp/mc-box-short.txt");

  viewport.SetBoxSelection(TextPosition{0, 3}, TextPosition{2, 5});

  const auto ranges = viewport.secondary_caret_ranges();
  Expect(ranges.size() == 2, "every line in the span keeps a caret");
  const auto short_line = std::find_if(ranges.begin(), ranges.end(),
                                       [](const auto& c) { return c.position.line == 1; });
  Expect(short_line != ranges.end() && short_line->position == TextPosition{1, 2} &&
             !short_line->selection_anchor.has_value(),
         "a line shorter than both box columns collapses to a zero-width end-of-line caret");
}

void TestSetBoxSelectionEqualColumnsMakesPlainColumnCarets() {
  TextViewport viewport;
  viewport.LoadContent("aaa\nbbb\nccc\n", "/tmp/mc-box-equal.txt");

  viewport.SetBoxSelection(TextPosition{0, 2}, TextPosition{2, 2});

  Expect(!viewport.selection_range().has_value(),
         "equal box columns leave the primary caret without a selection");
  const auto ranges = viewport.secondary_caret_ranges();
  Expect(ranges.size() == 2, "equal box columns still place a caret per other line");
  Expect(!ranges[0].selection_anchor.has_value() && !ranges[1].selection_anchor.has_value(),
         "equal box columns produce selection-less column carets");
}

void TestGrowingBoxSelectionStopsReallocatingTheCaretBuffer() {
  TextViewport viewport;
  std::string content;
  constexpr std::size_t kLines = 512;
  for (std::size_t i = 0; i < kLines; ++i) {
    content += "abcdefgh\n";
  }
  viewport.LoadContent(content, "/tmp/mc-box-grow.txt");

  const TextPosition anchor{0, 2};
  const void* last = nullptr;
  std::size_t moves = 0;
  for (std::size_t line = 1; line < kLines; ++line) {
    viewport.SetBoxSelection(anchor, TextPosition{line, 6});
    const auto positions = viewport.secondary_caret_positions();
    Expect(positions.size() == line, "each step should place one caret per non-primary line");
    if (positions.data() != last) {
      ++moves;
      last = positions.data();
    }
  }

  Expect(moves <= 16, "a growing box selection must not reallocate its caret buffer per step");
}

void TestMultiCaretDisjointEditPublishesNoAppliedEdit() {
  TextViewport viewport;
  viewport.LoadContent("l0\nl1\nl2\nl3\nl4", "/tmp/mc-disjoint.txt");
  viewport.MoveCursorTo(1, 0);
  viewport.SetSecondaryCarets({{3, 0}});

  viewport.InsertText("X");

  Expect(viewport.lines().size() == 5 && viewport.lines()[1] == "Xl1" &&
             viewport.lines()[2] == "l2" && viewport.lines()[3] == "Xl3",
         "the edit inserts at both carets and leaves the interior line untouched");
  Expect(!viewport.last_applied_edit().has_value(),
         "a disjoint multi-caret edit must not publish a single contiguous AppliedEdit");
}

void TestMultiCaretPairInsertPublishesNoAppliedEdit() {
  TextViewport viewport;
  viewport.LoadContent("a\nb\nc\nd\ne", "/tmp/mc-pair.cpp");
  viewport.MoveCursorTo(0, 1);
  viewport.SetSecondaryCarets({{4, 1}});

  viewport.InsertCharacter('(');

  Expect(viewport.secondary_carets().size() == 1,
         "the second caret should survive the multi-caret character insert");
  Expect(!viewport.last_applied_edit().has_value(),
         "multi-caret character insert must not publish a single contiguous AppliedEdit");
}

void TestMultiCaretSoftTabAlignsEachCaretToItsOwnStop() {
  TextViewport viewport;
  viewport.LoadContent("ab\nabcd", "/tmp/mc-softtab.cpp");
  viewport.SetSoftTabs(true);
  viewport.SetIndentWidth(4);
  viewport.MoveCursorTo(0, 2);
  viewport.SetSecondaryCarets({{1, 4}});

  viewport.InsertTab();

  Expect(viewport.lines()[0] == "ab  ",
         "the caret at column 2 pads 2 spaces to its own next tab stop");
  Expect(viewport.lines()[1] == "abcd    ",
         "the caret at column 4 pads a full indent to its own next tab stop");
}

void TestMultiCaretOutdentPreservesCarets() {
  TextViewport viewport;
  viewport.LoadContent("    foo\n    bar", "/tmp/mc-outdent.cpp");
  viewport.SetSoftTabs(true);
  viewport.SetIndentWidth(4);
  viewport.MoveCursorTo(0, 6);
  viewport.SetSecondaryCarets({{1, 6}});

  Expect(microide::editor::OutdentSelection(viewport), "outdent should change both lines");
  Expect(viewport.lines()[0] == "foo" && viewport.lines()[1] == "bar",
         "both lines lose one indent level");
  Expect(viewport.cursor_line() == 0 && viewport.cursor_column() == 2,
         "the primary caret shifts left by the 4 removed spaces (clamped into 'foo')");
  Expect(viewport.secondary_carets().size() == 1 &&
             viewport.secondary_carets().front() == TextPosition{1, 2},
         "the secondary caret survives and shifts left with its line");
}

void TestMultiCaretMergesOverlappingSelections() {
  TextViewport viewport;
  viewport.LoadContent("abcdefghijklmnop\nsecondline\n", "/tmp/overlap.txt");
  viewport.MoveCursorTo(1, 0, false);
  viewport.SetSecondaryCaretsWithRanges(std::vector<microide::editor::SelectionRange>{
      microide::editor::SelectionRange{{0, 0}, {0, 6}},
      microide::editor::SelectionRange{{0, 3}, {0, 9}},
  });
  Expect(viewport.secondary_carets().size() == 1, "the overlapping pair merged into one caret");
  viewport.InsertText("X");
  Expect(viewport.lines()[0] == "Xjklmnop",
         "the merged selection [0,9) is replaced once: " + std::string(viewport.lines()[0]));
  Expect(viewport.lines()[1] == "Xsecondline", "the disjoint primary still edits");
  Expect(viewport.secondary_carets().size() == 1 &&
             viewport.secondary_carets()[0].line == 0 && viewport.secondary_carets()[0].column == 1,
         "the merged caret lands after its replacement");
}

void TestMultiCaretMergesCollapsedCaretTouchingSelection() {
  TextViewport viewport;
  viewport.LoadContent("abcdef\n", "/tmp/touch.txt");
  viewport.MoveCursorTo(0, 1);
  viewport.MoveCursorTo(0, 3, true);
  viewport.AddSecondaryCaret(0, 3);
  Expect(!viewport.has_multiple_carets(), "the touching caret merged into the selection");
  viewport.Backspace();
  Expect(viewport.lines()[0] == "adef", "one Backspace removes exactly the selection: " +
                                             std::string(viewport.lines()[0]));
}

void TestMultiCaretDisjointSelectionsStillApply() {
  TextViewport viewport;
  viewport.LoadContent("abcdefghijklmnop\nsecondline\n", "/tmp/disjoint.txt");
  viewport.MoveCursorTo(1, 0, false);
  viewport.SetSecondaryCaretsWithRanges(std::vector<microide::editor::SelectionRange>{
      microide::editor::SelectionRange{{0, 0}, {0, 3}},
      microide::editor::SelectionRange{{0, 6}, {0, 9}},
  });
  viewport.InsertText("X");
  const std::string& line = viewport.lines()[0];
  Expect(line != "abcdefghijklmnop", "the disjoint replacement should change line 0");
  Expect(line.find("abc") == std::string::npos && line.find("ghi") == std::string::npos,
         "both disjoint selections should be replaced");
  Expect(line.find("def") != std::string::npos, "content between the selections is preserved");
}

namespace {

std::string JoinViewportLines(const TextViewport& viewport) {
  std::string joined;
  for (std::size_t i = 0; i < viewport.lines().size(); ++i) {
    if (i != 0) joined.push_back('\n');
    joined += viewport.lines()[i];
  }
  return joined;
}

std::string MakeNumberedLines(std::size_t count) {
  std::string text;
  for (std::size_t i = 0; i < count; ++i) {
    text += "L";
    text += std::to_string(i);
    text.push_back('\n');
  }
  return text;
}

}

void TestMultiCaretDisjointGroupedInsertNoSnapshotRoundTrips() {
  using microide::editor::TextBuffer;
  TextViewport viewport;
  viewport.LoadContent(MakeNumberedLines(400), "/tmp/mc-disjoint-insert.txt");
  const std::string original = JoinViewportLines(viewport);
  const std::size_t initial_lines = viewport.lines().size();

  viewport.MoveCursorTo(5, 0, false);
  viewport.SetSecondaryCarets({{200, 0}, {350, 0}});

  TextBuffer::reset_snapshot_build_count();
  viewport.InsertText("A\nB");
  Expect(TextBuffer::snapshot_build_count() == 0,
         "a disjoint grouped insert must not materialize the whole buffer");

  const std::string edited = JoinViewportLines(viewport);
  Expect(edited != original, "the grouped insert should have changed the buffer");
  Expect(viewport.lines().size() == initial_lines + 3,
         "three carets each added exactly one line");

  Expect(viewport.Undo(), "the disjoint group should undo as one step");
  Expect(JoinViewportLines(viewport) == original,
         "undo must exactly restore the pre-group buffer across all disjoint ranges");
  Expect(TextBuffer::snapshot_build_count() == 0,
         "applying the stitched undo entry must not snapshot the whole buffer");

  Expect(viewport.Redo(), "the disjoint group should redo as one step");
  Expect(JoinViewportLines(viewport) == edited,
         "redo must exactly reproduce the post-group buffer");
}

void TestMultiCaretDisjointGroupedJoinNoSnapshotRoundTrips() {
  using microide::editor::TextBuffer;
  TextViewport viewport;
  viewport.LoadContent(MakeNumberedLines(300), "/tmp/mc-disjoint-join.txt");
  const std::string original = JoinViewportLines(viewport);
  const std::size_t initial_lines = viewport.lines().size();

  viewport.MoveCursorTo(10, 0, false);
  viewport.SetSecondaryCarets({{150, 0}, {250, 0}});

  TextBuffer::reset_snapshot_build_count();
  viewport.Backspace();
  Expect(TextBuffer::snapshot_build_count() == 0,
         "a disjoint grouped join must not materialize the whole buffer");

  const std::string edited = JoinViewportLines(viewport);
  Expect(edited != original, "the grouped join should have changed the buffer");
  Expect(viewport.lines().size() == initial_lines - 3,
         "three col-0 backspaces removed three lines");

  Expect(viewport.Undo(), "the disjoint join group should undo as one step");
  Expect(JoinViewportLines(viewport) == original,
         "undo must exactly restore the pre-group buffer across all disjoint joins");
  Expect(viewport.Redo() && JoinViewportLines(viewport) == edited,
         "redo must exactly reproduce the joined buffer");
  Expect(TextBuffer::snapshot_build_count() == 0,
         "undo/redo of the stitched entry must not snapshot the whole buffer");
}

void TestMultiCaretManySameLineInsertRemapsEveryCaret() {
  TextViewport viewport;
  constexpr std::size_t kCaretCount = 400;
  std::string line(kCaretCount * 2, '.');
  viewport.LoadContent(line + "\n", "/tmp/mc-scale-insert.txt");
  viewport.MoveCursorTo(0, 0);
  std::vector<TextPosition> secondaries;
  secondaries.reserve(kCaretCount - 1);
  for (std::size_t i = 1; i < kCaretCount; ++i) {
    secondaries.push_back({0, i * 2});
  }
  viewport.SetSecondaryCarets(secondaries);

  viewport.InsertText("X");

  Expect(viewport.secondary_carets().size() == kCaretCount - 1,
         "every caret should survive the large same-line insert");
  Expect(viewport.cursor_line() == 0 && viewport.cursor_column() == 1,
         "the lowest caret should land right after its own insertion");
  bool all_correct = true;
  for (std::size_t i = 1; i < kCaretCount; ++i) {
    const TextPosition expected{0, 3 * i + 1};
    if (viewport.secondary_carets()[i - 1] != expected) {
      all_correct = false;
      break;
    }
  }
  Expect(all_correct, "each same-line caret must sit just after its own insertion once "
                      "all lower insertions have shifted it -- no stale positions");
  Expect(viewport.lines()[0].size() == kCaretCount * 3,
         "exactly one insertion should land at each of the hundreds of carets");
}

void TestMultiCaretManyPairInsertRemapsEveryCaret() {
  TextViewport viewport;
  viewport.SetLanguageContractView(MakeCStyleContractView());
  constexpr std::size_t kCaretCount = 300;
  std::string line(kCaretCount * 2, ' ');
  viewport.LoadContent(line + "\n", "/tmp/mc-scale-pair.txt");
  viewport.MoveCursorTo(0, 0);
  std::vector<TextPosition> secondaries;
  secondaries.reserve(kCaretCount - 1);
  for (std::size_t i = 1; i < kCaretCount; ++i) {
    secondaries.push_back({0, i * 2});
  }
  viewport.SetSecondaryCarets(secondaries);

  viewport.InsertCharacter('(');

  Expect(viewport.secondary_carets().size() == kCaretCount - 1,
         "every caret should survive the large auto-close pair insert");
  Expect(viewport.cursor_line() == 0 && viewport.cursor_column() == 1,
         "the lowest caret should land inside its own inserted pair");
  bool all_correct = true;
  for (std::size_t i = 1; i < kCaretCount; ++i) {
    const TextPosition expected{0, 4 * i + 1};
    if (viewport.secondary_carets()[i - 1] != expected) {
      all_correct = false;
      break;
    }
  }
  Expect(all_correct, "each caret must land inside its own auto-closed pair after all "
                      "lower pairs shift it -- no stale positions");
  Expect(viewport.lines()[0].size() == kCaretCount * 2 + kCaretCount * 2,
         "each caret should insert exactly one '()' pair");
}

void TestAddSecondaryCaretKeepsSortedOrderAndDedupes() {
  TextViewport viewport;
  viewport.LoadContent("alpha line\nbeta line\ngamma line\ndelta line\n",
                       "/tmp/mc-sorted-insert.txt");
  viewport.MoveCursorTo(0, 0);

  viewport.AddSecondaryCaret(3, 2);
  viewport.AddSecondaryCaret(1, 4);
  viewport.AddSecondaryCaret(2, 0);
  viewport.AddSecondaryCaret(1, 1);

  const std::vector<TextPosition> carets = viewport.secondary_carets();
  Expect(carets.size() == 4, "four distinct carets should be recorded");
  for (std::size_t i = 1; i < carets.size(); ++i) {
    const bool ordered =
        carets[i - 1].line < carets[i].line ||
        (carets[i - 1].line == carets[i].line && carets[i - 1].column < carets[i].column);
    Expect(ordered, "secondary carets must stay sorted by position after each insert");
  }

  viewport.AddSecondaryCaret(1, 4);
  viewport.AddSecondaryCaret(3, 2);
  Expect(viewport.secondary_carets().size() == 4, "duplicate carets must not be added twice");

  viewport.AddSecondaryCaret(0, 0);
  Expect(viewport.secondary_carets().size() == 4,
         "the primary caret position must not be added as a secondary");
}

namespace {

std::string AddCaretDump(const TextViewport& viewport) {
  std::string out = "P(" + std::to_string(viewport.cursor_line()) + "," +
                    std::to_string(viewport.cursor_column()) + ")";
  for (const TextPosition& caret : viewport.secondary_carets()) {
    out += " S(" + std::to_string(caret.line) + "," + std::to_string(caret.column) + ")";
  }
  return out;
}

void TestAddCaretBelowGrowsOnePerPress() {
  TextViewport viewport;
  viewport.LoadContent("aaaa\nbbbb\ncccc\ndddd\neeee\n", "/tmp/mc-add-below.txt");
  viewport.SetViewportSize(10, 40);
  viewport.MoveCursorTo(0, 2);

  for (std::size_t press = 1; press <= 3; ++press) {
    Expect(viewport.AddCaretVertical(1), "press " + std::to_string(press) + " adds a caret");
    Expect(viewport.secondary_caret_range_view().size() == press,
           "after " + std::to_string(press) + " press(es) there should be " +
               std::to_string(press) + " secondaries, got " +
               std::to_string(viewport.secondary_caret_range_view().size()));
  }
  Expect(viewport.cursor_line() == 3 && viewport.cursor_column() == 2,
         "the newest caret leads, so the view follows: " + AddCaretDump(viewport));
  std::vector<std::size_t> lines{viewport.cursor_line()};
  for (const TextPosition& caret : viewport.secondary_carets()) {
    lines.push_back(caret.line);
    Expect(caret.column == 2, "every caret keeps column 2");
  }
  std::sort(lines.begin(), lines.end());
  Expect(lines == std::vector<std::size_t>({0, 1, 2, 3}), "lines 0..3 each hold one caret");
}

void TestAddCaretBelowAtTheLastLineAddsNothing() {
  TextViewport viewport;
  viewport.LoadContent("only\n", "/tmp/mc-add-edge.txt");
  viewport.SetViewportSize(10, 40);
  viewport.MoveCursorTo(1, 0);
  Expect(!viewport.AddCaretVertical(1), "there is nothing below the last line");
  Expect(viewport.secondary_caret_range_view().empty(), "and no caret was added");
}

void TestAddCaretBelowStepsOneWrappedRow() {
  TextViewport viewport;
  viewport.LoadContent("abcdefghijklmnop\nshort\n", "/tmp/mc-add-wrap.txt");
  viewport.SetViewportSize(10, 8);
  viewport.SetSoftWrap(true);
  viewport.MoveCursorTo(0, 2);

  Expect(viewport.AddCaretVertical(1), "the press adds a caret");
  Expect(viewport.secondary_caret_range_view().size() == 1, "one caret was added");
  Expect(viewport.cursor_line() == 0 && viewport.cursor_column() == 10,
         "the new caret is on line 0's SECOND wrapped row: " + AddCaretDump(viewport));
}

void TestAddCaretAboveLeadsUpward() {
  TextViewport viewport;
  viewport.LoadContent("aaaa\nbbbb\ncccc\n", "/tmp/mc-add-above.txt");
  viewport.SetViewportSize(10, 40);
  viewport.MoveCursorTo(2, 1);

  Expect(viewport.AddCaretVertical(-1), "the press adds a caret above");
  Expect(viewport.cursor_line() == 1 && viewport.cursor_column() == 1,
         "the caret above leads: " + AddCaretDump(viewport));
  Expect(viewport.secondary_caret_range_view().size() == 1 &&
             viewport.secondary_carets().front() == TextPosition{2, 1},
         "and the original stays as a secondary");
}

void TestTypingAtManyCaretsCopiesTheCaretSetLinearly() {
  constexpr std::size_t kCarets = 2000;
  std::string text;
  for (std::size_t i = 0; i < kCarets; ++i) {
    text += "line " + std::to_string(i) + "\n";
  }
  for (const char typed : {'x', '('}) {
    TextViewport viewport;
    viewport.LoadContent(text, "/tmp/mc-typing.cpp");
    viewport.SetViewportSize(40, 80);
    viewport.SelectAll();
    Expect(viewport.AddCaretsAtSelectedLineEnds(), "a caret at every line's end");
    Expect(viewport.secondary_carets().size() + 1 >= kCarets, "about one caret per line");
    const std::uint64_t before = util::ReadPerformanceCounter(util::PerfCounterId::EditorHistoryCaretCopies);
    viewport.InsertText(std::string(1, typed));
    const std::uint64_t copies =
        util::ReadPerformanceCounter(util::PerfCounterId::EditorHistoryCaretCopies) - before;
    Expect(copies <= 8 * kCarets, std::string("typing '") + typed + "' at " + std::to_string(kCarets) +
                                      " carets copied the caret set " + std::to_string(copies) +
                                      " carets' worth (linear is a few times the carets)");
    std::size_t edited = 0;
    for (std::size_t line = 0; line < viewport.lines().size(); ++line) {
      const std::string_view row = viewport.lines().LineView(line);
      edited += row.find(typed) != std::string_view::npos;
    }
    Expect(edited + 1 >= kCarets, "and every line got it: " + std::to_string(edited));
    Expect(viewport.secondary_carets().size() + 1 >= kCarets, "every caret is still there");
    viewport.Undo();
    Expect(viewport.SerializeDocumentText() == text, "one undo takes the whole keystroke back");
    Expect(viewport.secondary_carets().size() + 1 >= kCarets, "with every caret");
  }
}

void TestAddCaretsAtSelectedLineEnds() {
  TextViewport viewport;
  viewport.LoadContent("one\ntwotwo\nthree\nfour\n", "/tmp/mc-line-ends.txt");
  viewport.SetViewportSize(10, 40);
  viewport.MoveCursorTo(0, 1);
  viewport.MoveCursorTo(2, 2, true);

  Expect(viewport.AddCaretsAtSelectedLineEnds(), "the verb applies");
  Expect(!viewport.has_selection(), "the selection is replaced by the carets");
  std::vector<TextPosition> all{TextPosition{viewport.cursor_line(), viewport.cursor_column()}};
  for (const TextPosition& caret : viewport.secondary_carets()) all.push_back(caret);
  std::sort(all.begin(), all.end(),
            [](const TextPosition& a, const TextPosition& b) { return a.line < b.line; });
  Expect(all.size() == 3 && all[0] == TextPosition{0, 3} && all[1] == TextPosition{1, 6} &&
             all[2] == TextPosition{2, 5},
         std::string("one caret at each line's END: ") + AddCaretDump(viewport));
  Expect(viewport.cursor_line() == 2,
         "the last line's end is the primary, so the view stays at the bottom of the block");
}

void TestAddCaretsAtLineEndsIgnoresTheTrailingLineStart() {
  TextViewport viewport;
  viewport.LoadContent("one\ntwo\nthree\n", "/tmp/mc-line-ends-drag.txt");
  viewport.SetViewportSize(10, 40);
  viewport.MoveCursorTo(0, 0);
  viewport.MoveCursorTo(2, 0, true);

  Expect(viewport.AddCaretsAtSelectedLineEnds(), "the verb applies");
  Expect(viewport.secondary_caret_range_view().size() == 1,
         "two lines were dragged, so two carets, got " +
             std::to_string(viewport.secondary_caret_range_view().size() + 1));
  Expect(viewport.cursor_line() == 1 && viewport.cursor_column() == 3,
         std::string("the last one is the end of line 1, not line 2: ") + AddCaretDump(viewport));
}

void TestExpandLineSelectionCoversEveryCaret() {
  TextViewport viewport;
  viewport.LoadContent("aaa0\nbbb1\nccc2\nddd3\neee4\nfff5\n", "/tmp/mc-expand-line.txt");
  viewport.SetViewportSize(10, 40);
  viewport.MoveCursorTo(1, 2);
  viewport.SetSecondaryCarets({{3, 2}, {5, 2}});

  Expect(viewport.ExpandSelectionToWholeLines(), "the expansion applies");
  const auto primary = viewport.selection_range();
  Expect(primary.has_value() && primary->start == TextPosition{1, 0} &&
             primary->end == TextPosition{2, 0},
         "the primary covers its whole line");
  Expect(viewport.secondary_caret_range_view().size() == 2,
         "both secondaries survive, got " +
             std::to_string(viewport.secondary_caret_range_view().size()));
  for (const auto& caret : viewport.secondary_caret_range_view()) {
    Expect(caret.selection_anchor.has_value() && !(*caret.selection_anchor == caret.position),
           "every secondary holds a REAL selection, not an empty anchor at its old "
           "position");
  }

  viewport.InsertText("Z");
  Expect(JoinViewportLines(viewport) == "aaa0\nZccc2\nZeee4\nZ",
         std::string("each caret's line is replaced: ") + JoinViewportLines(viewport));
}

void TestExpandLineSelectionGrowsAndMerges() {
  TextViewport viewport;
  viewport.LoadContent("l0\nl1\nl2\nl3\nl4\n", "/tmp/mc-expand-merge.txt");
  viewport.SetViewportSize(10, 40);
  viewport.MoveCursorTo(0, 0);
  viewport.SetSecondaryCarets({{1, 0}});

  Expect(viewport.ExpandSelectionToWholeLines(), "the first press applies");
  Expect(viewport.secondary_caret_range_view().empty(),
         "the two expansions meet and become one selection, got " +
             std::to_string(viewport.secondary_caret_range_view().size()) + " secondaries");
  const auto merged = viewport.selection_range();
  Expect(merged.has_value() && merged->start == TextPosition{0, 0} &&
             merged->end == TextPosition{2, 0},
         "the merged selection covers both lines");

  Expect(viewport.ExpandSelectionToWholeLines(), "the second press applies");
  const auto grown = viewport.selection_range();
  Expect(grown.has_value() && grown->start == TextPosition{0, 0} &&
             grown->end == TextPosition{3, 0},
         "and it grows by one more line rather than standing still");
}

void TestExpandLineSelectionOfAPartialSelectionTakesOnlyItsLines() {
  TextViewport viewport;
  viewport.LoadContent("foo bar\nbaz\nqux\nlast\n", "/tmp/mc-expand-partial.txt");
  viewport.SetViewportSize(10, 40);
  viewport.MoveCursorTo(0, 0);
  viewport.MoveCursorTo(0, 3, true);
  Expect(viewport.ExpandSelectionToWholeLines(), "the expansion applies");
  auto range = viewport.selection_range();
  Expect(range.has_value() && range->start == TextPosition{0, 0} && range->end == TextPosition{1, 0},
         "a word on line 0 expands to line 0 alone");

  viewport.MoveCursorTo(2, 1);
  viewport.MoveCursorTo(1, 1, true);
  Expect(viewport.ExpandSelectionToWholeLines(), "the upward expansion applies");
  range = viewport.selection_range();
  Expect(range.has_value() && range->start == TextPosition{1, 0} && range->end == TextPosition{3, 0},
         "an upward selection over lines 1-2 expands to lines 1-2 whole");
}

void TestMultiCaretTypingUndoesAsOneRun() {
  for (const bool adjacent : {true, false}) {
    TextViewport viewport;
    const std::string text = adjacent ? "foo = 1\nfoo = 2\nfoo = 3" : "foo = 1\n\nfoo = 2\n\nfoo = 3";
    viewport.LoadContent(text, "/tmp/mc-undo-run.txt");
    viewport.MoveCursorTo(0, 0);
    viewport.SetSecondaryCarets(adjacent ? std::vector<TextPosition>{{1, 0}, {2, 0}}
                                         : std::vector<TextPosition>{{2, 0}, {4, 0}});
    for (const char* ch : {"b", "a", "r"}) {
      viewport.InsertText(ch);
    }
    const std::string label = adjacent ? "adjacent carets" : "carets apart";
    Expect(viewport.lines()[0] == "barfoo = 1", label + ": every caret should type");
    Expect(viewport.Undo(), label + ": undo should run");
    std::string after;
    viewport.lines().AppendWholeText(after);
    Expect(viewport.lines()[0] == "foo = 1" && viewport.lines()[adjacent ? 2 : 4] == "foo = 3",
           label + ": one undo should take the whole typed word back, got " + after);
    Expect(viewport.Redo(), label + ": redo should run");
    Expect(viewport.lines()[0] == "barfoo = 1" && viewport.lines()[adjacent ? 2 : 4] == "barfoo = 3",
           label + ": one redo should put the whole word back");
  }
}

}

void RegisterEditorMultiCaretTests(std::vector<TestCase>& tests) {
  AddTest(tests, "EditorMultiCaret/TypingUndoesAsOneRun", TestMultiCaretTypingUndoesAsOneRun);
  AddTest(tests, "EditorMultiCaret/ExpandLineSelectionCoversEveryCaret",
          TestExpandLineSelectionCoversEveryCaret);
  AddTest(tests, "EditorMultiCaret/ExpandLineSelectionGrowsAndMerges",
          TestExpandLineSelectionGrowsAndMerges);
  AddTest(tests, "EditorMultiCaret/ExpandLineSelectionOfAPartialSelectionTakesOnlyItsLines",
          TestExpandLineSelectionOfAPartialSelectionTakesOnlyItsLines);
  AddTest(tests, "EditorMultiCaret/AddCaretBelowGrowsOnePerPress", TestAddCaretBelowGrowsOnePerPress);
  AddTest(tests, "EditorMultiCaret/AddCaretBelowAtTheLastLineAddsNothing",
          TestAddCaretBelowAtTheLastLineAddsNothing);
  AddTest(tests, "EditorMultiCaret/AddCaretBelowStepsOneWrappedRow",
          TestAddCaretBelowStepsOneWrappedRow);
  AddTest(tests, "EditorMultiCaret/AddCaretAboveLeadsUpward", TestAddCaretAboveLeadsUpward);
  AddTest(tests, "EditorMultiCaret/AddCaretsAtSelectedLineEnds", TestAddCaretsAtSelectedLineEnds);
  AddTest(tests, "EditorMultiCaret/TypingAtManyCaretsCopiesTheCaretSetLinearly",
          TestTypingAtManyCaretsCopiesTheCaretSetLinearly);
  AddTest(tests, "EditorMultiCaret/AddCaretsAtLineEndsIgnoresTheTrailingLineStart",
          TestAddCaretsAtLineEndsIgnoresTheTrailingLineStart);
  AddTest(tests, "EditorMultiCaret/AddSecondaryCaretKeepsSortedOrderAndDedupes",
          TestAddSecondaryCaretKeepsSortedOrderAndDedupes);
  AddTest(tests, "EditorMultiCaret/ManySameLineInsertRemapsEveryCaret",
          TestMultiCaretManySameLineInsertRemapsEveryCaret);
  AddTest(tests, "EditorMultiCaret/ManyPairInsertRemapsEveryCaret",
          TestMultiCaretManyPairInsertRemapsEveryCaret);
  AddTest(tests, "EditorMultiCaret/DisjointGroupedInsertNoSnapshotRoundTrips",
          TestMultiCaretDisjointGroupedInsertNoSnapshotRoundTrips);
  AddTest(tests, "EditorMultiCaret/DisjointGroupedJoinNoSnapshotRoundTrips",
          TestMultiCaretDisjointGroupedJoinNoSnapshotRoundTrips);
  AddTest(tests, "EditorMultiCaret/MergesOverlappingSelections",
          TestMultiCaretMergesOverlappingSelections);
  AddTest(tests, "EditorMultiCaret/MergesCollapsedCaretTouchingSelection",
          TestMultiCaretMergesCollapsedCaretTouchingSelection);
  AddTest(tests, "EditorMultiCaret/DisjointSelectionsStillApply",
          TestMultiCaretDisjointSelectionsStillApply);
  AddTest(tests, "EditorMultiCaret/MultiCaretSoftTabAlignsEachCaretToItsOwnStop",
          TestMultiCaretSoftTabAlignsEachCaretToItsOwnStop);
  AddTest(tests, "EditorMultiCaret/MultiCaretOutdentPreservesCarets",
          TestMultiCaretOutdentPreservesCarets);
  AddTest(tests, "EditorMultiCaret/PromotedCaretToggleLineCommentAtomicUndo",
          TestPromotedCaretToggleLineCommentAtomicUndo);
  AddTest(tests, "EditorMultiCaret/FoldAwareVerticalMotionSkipsHiddenLines",
          TestFoldAwareMultiCaretVerticalMotionSkipsHiddenLines);
  AddTest(tests, "EditorMultiCaret/PageUpAcrossCollapsedFoldUsesVisibleRows",
          TestMultiCaretPageUpAcrossCollapsedFoldUsesVisibleRows);
  AddTest(tests, "EditorMultiCaret/PlaceColumnCaretsBetweenLinesUsesAnchorColumnOnEveryLine",
          TestPlaceColumnCaretsBetweenLinesUsesAnchorColumnOnEveryLine);
  AddTest(tests, "EditorMultiCaret/SetBoxSelectionSelectsEachLineAcrossColumns",
          TestSetBoxSelectionSelectsEachLineAcrossColumns);
  AddTest(tests, "EditorMultiCaret/SetBoxSelectionIsRectangularInVisualColumns",
          TestSetBoxSelectionIsRectangularInVisualColumns);
  AddTest(tests, "EditorMultiCaret/BoxColumnsOnLineTabArithmeticAgreesWithTheWalk",
          TestBoxColumnsOnLineTabArithmeticAgreesWithTheWalk);
  AddTest(tests, "EditorMultiCaret/SelectAllAndCutLeaveATypableCaretSet",
          TestMultiCaretSelectAllAndCutLeaveATypableCaretSet);
  AddTest(tests, "EditorMultiCaret/SetBoxSelectionLeftwardKeepsCaretsOnCaretColumn",
          TestSetBoxSelectionLeftwardKeepsCaretsOnCaretColumn);
  AddTest(tests, "EditorMultiCaret/SetBoxSelectionClampsShortLinesToEndOfLine",
          TestSetBoxSelectionClampsShortLinesToEndOfLine);
  AddTest(tests, "EditorMultiCaret/SetBoxSelectionEqualColumnsMakesPlainColumnCarets",
          TestSetBoxSelectionEqualColumnsMakesPlainColumnCarets);
  AddTest(tests, "EditorMultiCaret/GrowingBoxSelectionStopsReallocatingTheCaretBuffer",
          TestGrowingBoxSelectionStopsReallocatingTheCaretBuffer);
  AddTest(tests, "EditorMultiCaret/SoftWrapMultiCaretInsertAppliesAtEveryCaret",
          TestSoftWrapMultiCaretInsertAppliesAtEveryCaret);
  AddTest(tests, "EditorMultiCaret/SoftWrapMultiCaretBackspaceErasesAtEveryCaret",
          TestSoftWrapMultiCaretBackspaceErasesAtEveryCaret);
  AddTest(tests, "EditorMultiCaret/SoftWrapMultiCaretDeleteForwardErasesAtEveryCaret",
          TestSoftWrapMultiCaretDeleteForwardErasesAtEveryCaret);
  AddTest(tests, "EditorMultiCaret/MultiCaretInsertPreservesPrimaryAmongSameLineCarets",
          TestMultiCaretInsertPreservesPrimaryAmongSameLineCarets);
  AddTest(tests, "EditorMultiCaret/MultiCaretMultiCharInsertShiftsSameLineCarets",
          TestMultiCaretMultiCharInsertShiftsSameLineCarets);
  AddTest(tests, "EditorMultiCaret/MultiCaretBackspaceShiftsSameLineCarets",
          TestMultiCaretBackspaceShiftsSameLineCarets);
  AddTest(tests, "EditorMultiCaret/MultiCaretSplitBracesAtEveryCaret",
          TestMultiCaretSplitBracesAtEveryCaret);
  AddTest(tests, "EditorMultiCaret/MultiCaretPasteWithLoneCarriageReturnRemapsCarets",
          TestMultiCaretPasteWithLoneCarriageReturnRemapsCarets);
  AddTest(tests, "EditorMultiCaret/MultiCaretSplitBracesMixedWithPlainNewline",
          TestMultiCaretSplitBracesMixedWithPlainNewline);
  AddTest(tests, "EditorMultiCaret/SetSecondaryCaretsClampCollapseDedupeAndPrimaryDrop",
          TestSetSecondaryCaretsClampCollapseDedupeAndPrimaryDrop);
  AddTest(tests, "EditorMultiCaret/MultiCaretBackspaceReplacesEachSelection",
          TestMultiCaretBackspaceReplacesEachSelection);
  AddTest(tests, "EditorMultiCaret/MultiCaretPasteReplacesEachSelection",
          TestMultiCaretPasteReplacesEachSelection);
  AddTest(tests, "EditorMultiCaret/CopyAggregatesSelections",
          TestMultiCaretCopyAggregatesSelections);
  AddTest(tests, "EditorMultiCaret/CopyRequiresAllSelections",
          TestMultiCaretCopyRequiresAllSelections);
  AddTest(tests, "EditorMultiCaret/DistributePasteOneLinePerCaret",
          TestMultiCaretDistributePasteOneLinePerCaret);
  AddTest(tests, "EditorMultiCaret/DistributePasteIgnoresOneTrailingLineBreak",
          TestMultiCaretDistributePasteIgnoresOneTrailingLineBreak);
  AddTest(tests, "EditorMultiCaret/PasteCountMismatchInsertsFullTextAtEachCaret",
          TestMultiCaretPasteCountMismatchInsertsFullTextAtEachCaret);
  AddTest(tests, "EditorMultiCaret/DeleteSelectionsRemovesAllAndUndoesAtomically",
          TestMultiCaretDeleteSelectionsRemovesAllAndUndoesAtomically);
  AddTest(tests, "EditorMultiCaret/SetSecondaryCaretsWithRangesPreservesSelections",
          TestSetSecondaryCaretsWithRangesPreservesSelections);
  AddTest(tests, "EditorMultiCaret/SetSecondaryCaretsWithRangesEmptyRangeIsBareCaret",
          TestSetSecondaryCaretsWithRangesEmptyRangeIsBareCaret);
  AddTest(tests, "EditorMultiCaret/NonExtendingMoveCollapsesSecondarySelections",
          TestNonExtendingMoveCollapsesSecondarySelections);
  AddTest(tests, "EditorMultiCaret/NonExtendingHorizontalMoveCollapsesToSelectionEdge",
          TestNonExtendingHorizontalMoveCollapsesToSelectionEdge);
  AddTest(tests, "EditorMultiCaret/ExtendingMoveStartsSelectionAtEverySecondaryCaret",
          TestExtendingMoveStartsSelectionAtEverySecondaryCaret);
  AddTest(tests, "EditorMultiCaret/DisjointEditPublishesNoAppliedEdit",
          TestMultiCaretDisjointEditPublishesNoAppliedEdit);
  AddTest(tests, "EditorMultiCaret/PairInsertPublishesNoAppliedEdit",
          TestMultiCaretPairInsertPublishesNoAppliedEdit);
}

}
