#include "TestSupport.h"

#include "editor/FoldingModel.h"
#include "editor/LanguageContractView.h"
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

// §12.1: pressing Enter with several carets each sitting between a matching
// auto-close pair must split every brace pair across three lines (mirroring the
// single-caret TryInsertNewlineSplitBraces path) in one undoable step.
void TestMultiCaretSplitBracesAtEveryCaret() {
  TextViewport viewport;
  viewport.LoadContent("foo() {}\nfoo() {}", "/tmp/mc-split-braces.cpp");
  viewport.SetLanguageContractView(MakeCStyleContractView());
  viewport.SetSoftTabs(true);
  viewport.SetIndentWidth(2);
  viewport.MoveCursorTo(0, 7);            // between '{' and '}' on line 0
  viewport.SetSecondaryCarets({{1, 7}});  // between '{' and '}' on line 1

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

// Pasting text with a lone '\r' (or a reversed "\n\r") at multiple carets: the
// buffer edit normalizes line endings (lone '\r' -> '\n'), so the caret-remap
// shape must count that '\r' as a newline. Otherwise a higher caret is stranded
// one line above the text it actually moved to.
void TestMultiCaretPasteWithLoneCarriageReturnRemapsCarets() {
  TextViewport viewport;
  viewport.LoadContent("abcdefgh", "/tmp/mc-lone-cr.txt");
  viewport.MoveCursorTo(0, 2);            // primary caret
  viewport.SetSecondaryCarets({{0, 5}});  // secondary caret (higher position)

  viewport.InsertText("X\rY");  // normalizes to "X\nY" -> one extra line per caret

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

// A caret not between a matching pair must fall back to a plain newline +
// auto-indent while its sibling caret still splits its braces, all in one step.
void TestMultiCaretSplitBracesMixedWithPlainNewline() {
  TextViewport viewport;
  viewport.LoadContent("foo() {}\nbar baz", "/tmp/mc-split-braces-mixed.cpp");
  viewport.SetLanguageContractView(MakeCStyleContractView());
  viewport.SetSoftTabs(true);
  viewport.SetIndentWidth(2);
  viewport.MoveCursorTo(0, 7);            // between '{' and '}' -> brace split
  viewport.SetSecondaryCarets({{1, 3}});  // middle of "bar baz" -> plain newline

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

void TestSoftWrapMultiCaretInsertAppliesAtEveryCaret() {
  TextViewport viewport;
  viewport.LoadContent("abcdefghijklmnopqrst\nabcdefghijk\nqrstuvwxyzabcdefgh\n",
                       "/tmp/mc-softwrap-insert.txt");
  viewport.SetViewportSize(/*visible_lines=*/10, /*visible_columns=*/8);
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
  viewport.SetViewportSize(/*visible_lines=*/10, /*visible_columns=*/8);
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
  viewport.SetViewportSize(/*visible_lines=*/10, /*visible_columns=*/8);
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

// The primary caret is identified by its index in the collected caret vector,
// not by value-equality on its clamped position. Inserting with multiple carets
// on the primary's own line must keep the primary as the active cursor and apply
// exactly one insertion per caret.
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
  // Lower insertions shift the higher carets right; positions must not be stale.
  Expect(viewport.secondary_carets()[0] == TextPosition{0, 2} &&
             viewport.secondary_carets()[1] == TextPosition{0, 5},
         "each same-line caret should sit just after its own insertion");
}

void TestMultiCaretMultiCharInsertShiftsSameLineCarets() {
  TextViewport viewport;
  viewport.LoadContent("abcdef\n", "/tmp/mc-sameline-multichar.txt");
  viewport.MoveCursorTo(0, 5);
  viewport.SetSecondaryCarets({{0, 1}, {0, 3}});

  // A multi-character insert routes through ApplyMultiCaretInsert.
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

  // Erase the column before each caret: 'a[b]cdefgh' style at cols 2,4,6 → remove
  // chars at 1,3,5 ('b','d','f').
  Expect(viewport.lines()[0] == "acegh",
         "each same-line caret should erase exactly the column before it");
  Expect(viewport.cursor_line() == 0 && viewport.cursor_column() == 3,
         "the primary caret should track its erase after lower erases shift it left");
  Expect(viewport.secondary_carets().size() == 2 &&
             viewport.secondary_carets()[0] == TextPosition{0, 1} &&
             viewport.secondary_carets()[1] == TextPosition{0, 2},
         "lower same-line erases should shift higher carets left without losing any");
}

// The single-pass SetSecondaryCarets rebuild (which the multi-caret apply paths
// now funnel through) must reproduce the old AddSecondaryCaret-in-a-loop result:
// clamp every position into the document, sort, drop duplicates -- including two
// positions that clamp to the same column on a short line -- and drop any caret
// coinciding with the primary. This pins the edge cases that the O(k^2) -> O(k log k)
// rewrite could have regressed.
void TestSetSecondaryCaretsClampCollapseDedupeAndPrimaryDrop() {
  TextViewport viewport;
  viewport.LoadContent("abcdef\nxy\nghijk", "/tmp/mc-clamp-collapse.txt");
  viewport.MoveCursorTo(0, 2);  // primary at (0,2)

  // Deliberately unsorted and adversarial:
  //  - (1,5) and (1,9) both clamp to (1,2) on the 2-char line -> collapse to one
  //  - (2,1) appears twice -> dedupe
  //  - (0,2) coincides with the primary -> dropped
  //  - line 99 is out of range -> clamps into the last line (index 2) at column 0
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

// Multi-caret Backspace/Delete/paste must replace each caret's active selection
// (like the single-caret paths and VSCode), not drop the selections and edit one
// character per caret.
void TestMultiCaretBackspaceReplacesEachSelection() {
  TextViewport viewport;
  viewport.LoadContent("foo foo", "/tmp/mc-sel-backspace.cpp");
  viewport.MoveCursorTo(0, 1);    // inside the first "foo"
  viewport.SelectWordAtCursor();  // primary selection over [0,0)-[0,3)
  viewport.AddSecondaryCaretWithRange(
      microide::editor::SelectionRange{TextPosition{0, 4}, TextPosition{0, 7}});  // second "foo"
  Expect(viewport.has_multiple_carets(), "fixture should have two carets");

  viewport.Backspace();
  Expect(viewport.lines().size() == 1 && viewport.lines()[0] == " ",
         "multi-caret Backspace should delete both selected words, leaving the separating space");
}

void TestMultiCaretPasteReplacesEachSelection() {
  TextViewport viewport;
  viewport.LoadContent("foo foo", "/tmp/mc-sel-paste.cpp");
  viewport.MoveCursorTo(0, 1);
  viewport.SelectWordAtCursor();  // primary selection over the first "foo"
  viewport.AddSecondaryCaretWithRange(
      microide::editor::SelectionRange{TextPosition{0, 4}, TextPosition{0, 7}});

  viewport.InsertText("XY");  // multi-char paste over both selections
  Expect(viewport.lines().size() == 1 && viewport.lines()[0] == "XY XY",
         "multi-caret paste should replace each selection with the pasted text");
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
}

}  // namespace microide::tests
