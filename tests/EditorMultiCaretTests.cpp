#include "TestSupport.h"

#include <vector>

#include "editor/FoldingModel.h"
#include "editor/LanguageContractView.h"
#include "editor/ShapingActions.h"
#include "editor/TextBuffer.h"
#include "editor/TextLayout.h"
#include "editor/TextViewport.h"

namespace microide::tests {
namespace {

using microide::editor::FoldingModel;
using microide::editor::TextPosition;
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

void TestMultiCaretCopyAggregatesSelections() {
  TextViewport viewport;
  viewport.LoadContent("foo foo", "/tmp/mc-copy.cpp");
  viewport.MoveCursorTo(0, 1);
  viewport.SelectWordAtCursor();  // primary selection over the first "foo"
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
  viewport.SelectWordAtCursor();     // primary HAS a selection
  viewport.AddSecondaryCaret(0, 5);  // secondary has NO selection
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
  // Clipboard has exactly two lines for two carets -> one line per caret.
  viewport.PasteText("AAA\nBBB");
  Expect(viewport.lines().size() == 1 && viewport.lines()[0] == "AAA BBB",
         "distribute-paste should place line 1 at the first caret, line 2 at the second");
}

void TestMultiCaretPasteCountMismatchInsertsFullTextAtEachCaret() {
  TextViewport viewport;
  viewport.LoadContent("foo foo", "/tmp/mc-mismatch-paste.cpp");
  viewport.MoveCursorTo(0, 1);
  viewport.SelectWordAtCursor();
  viewport.AddSecondaryCaretWithRange(
      microide::editor::SelectionRange{TextPosition{0, 4}, TextPosition{0, 7}});
  // Three clipboard lines but two carets: insert the whole block at every caret
  // (NOT distribute) -> each 3-line block adds two newlines, so 5 lines total.
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

// Regression: the "add cursor at next/all match" (Ctrl+D) executor path now
// registers each match as a RANGED secondary caret via SetSecondaryCaretsWithRanges.
// Before the fix it pushed bare end-positions through SetSecondaryCarets, which
// nulls every anchor -- so multi-caret typing inserted at the secondary sites
// (corrupting text) instead of replacing each occurrence, and copy silently
// grabbed only the primary match.
void TestSetSecondaryCaretsWithRangesPreservesSelections() {
  TextViewport viewport;
  viewport.LoadContent("foo foo foo", "/tmp/mc-ranges.cpp");
  viewport.MoveCursorTo(0, 1);
  viewport.SelectWordAtCursor();  // primary selection over the first "foo"
  // Mirror the "select all matches" executor path: the two other occurrences
  // become ranged secondary carets, not bare positions.
  viewport.SetSecondaryCaretsWithRanges(std::vector<microide::editor::SelectionRange>{
      microide::editor::SelectionRange{TextPosition{0, 4}, TextPosition{0, 7}},
      microide::editor::SelectionRange{TextPosition{0, 8}, TextPosition{0, 11}},
  });
  Expect(viewport.has_multiple_carets(), "should register two secondary carets");

  // Copy aggregates every occurrence -> each secondary kept its selection.
  const auto copied = viewport.MultiCaretSelectedText();
  Expect(copied.has_value() && *copied == "foo\nfoo\nfoo",
         "ranged secondaries let multi-caret copy aggregate every match");

  // Typing replaces every occurrence in one edit (VSCode rename-every-match).
  viewport.InsertText("x");
  Expect(viewport.lines().size() == 1 && viewport.lines()[0] == "x x x",
         "typing over ranged multi-carets replaces each match instead of inserting");
}

// A degenerate (empty) range in the batch must land as a bare caret with no
// selection, matching AddSecondaryCaret.
void TestSetSecondaryCaretsWithRangesEmptyRangeIsBareCaret() {
  TextViewport viewport;
  viewport.LoadContent("foo foo", "/tmp/mc-ranges-empty.cpp");
  viewport.MoveCursorTo(0, 1);
  viewport.SelectWordAtCursor();
  viewport.SetSecondaryCaretsWithRanges(std::vector<microide::editor::SelectionRange>{
      microide::editor::SelectionRange{TextPosition{0, 5}, TextPosition{0, 5}},  // empty
  });
  // The primary has a selection but the lone secondary does not -> aggregate copy
  // must decline, exactly as with AddSecondaryCaret.
  Expect(!viewport.MultiCaretSelectedText().has_value(),
         "an empty range must produce a selection-less secondary caret");
}

// A plain (non-extending) caret move must collapse EVERY caret's selection, not
// just the primary's. Before the fix the move loops advanced each secondary
// caret's position but left its selection_anchor intact, so a Ctrl+D / box set
// kept ghost selections on the secondaries after an arrow key -- and a following
// keystroke then REPLACED those stale selections (or the edit was refused as
// overlapping) instead of inserting at the collapsed caret, diverging from the
// primary and from VSCode.
void TestNonExtendingMoveCollapsesSecondarySelections() {
  TextViewport viewport;
  viewport.LoadContent("aaa bbb ccc", "/tmp/mc-move-collapse.cpp");
  viewport.MoveCursorTo(0, 1);
  viewport.SelectWordAtCursor();  // primary selection over "aaa"
  viewport.SetSecondaryCaretsWithRanges(std::vector<microide::editor::SelectionRange>{
      microide::editor::SelectionRange{TextPosition{0, 4}, TextPosition{0, 7}},   // "bbb"
      microide::editor::SelectionRange{TextPosition{0, 8}, TextPosition{0, 11}},  // "ccc"
  });
  Expect(viewport.MultiCaretSelectedText().has_value(),
         "fixture: every caret starts with a selection");

  // Right arrow with no Shift: collapse-and-move, exactly like the primary.
  viewport.MoveCursorHorizontal(1, /*extend_selection=*/false);

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

  // Right over a selection collapses each caret to its selection's RIGHT edge
  // (end of each word: col 3/7/11) without advancing past it, so typing inserts
  // immediately after each word rather than replacing a stale selection.
  viewport.InsertText("X");
  Expect(viewport.lines().size() == 1 && viewport.lines()[0] == "aaaX bbbX cccX",
         "after collapsing to the selection edge, a keystroke inserts at every caret");
}

// Single-caret parity with SingleLineEditor and VSCode: a plain Left/Right over a
// selection collapses to that selection's edge (left for Left, right for Right)
// WITHOUT advancing past it. The main editor previously cleared the anchor and
// then stepped one column further, landing the caret past the selection.
void TestNonExtendingHorizontalMoveCollapsesToSelectionEdge() {
  TextViewport viewport;
  viewport.LoadContent("hello world", "/tmp/mc-collapse-edge.cpp");
  viewport.MoveCursorTo(0, 1);
  viewport.SelectWordAtCursor();  // selects "hello" -> [0,0)-[0,5)
  Expect(viewport.has_selection(), "fixture: a word is selected");

  // Right collapses to the RIGHT edge (col 5, end of "hello"), not col 6.
  viewport.MoveCursorHorizontal(1, /*extend_selection=*/false);
  Expect(!viewport.has_selection() && viewport.cursor_column() == 5,
         "Right over a selection lands on its right edge, not one past it");

  viewport.MoveCursorTo(0, 1);
  viewport.SelectWordAtCursor();  // re-select "hello"
  // Left collapses to the LEFT edge (col 0, start of "hello"), not col 0-then-back.
  viewport.MoveCursorHorizontal(-1, /*extend_selection=*/false);
  Expect(!viewport.has_selection() && viewport.cursor_column() == 0,
         "Left over a selection lands on its left edge");
}

// The symmetric counterpart: an extending (Shift) move must START a selection at
// EVERY caret that lacks one, not only the primary. Before the fix Shift+Arrow on
// a plain multi-caret set (no selections) anchored only the primary, so the
// secondaries slid without selecting -- multi-caret Shift-select was broken on the
// secondaries, diverging from VSCode.
void TestExtendingMoveStartsSelectionAtEverySecondaryCaret() {
  TextViewport viewport;
  viewport.LoadContent("aaa bbb ccc", "/tmp/mc-extend-start.cpp");
  viewport.MoveCursorTo(0, 0);
  viewport.SetSecondaryCarets({{0, 4}, {0, 8}});  // bare carets, no selections
  Expect(!viewport.MultiCaretSelectedText().has_value(),
         "fixture: no caret starts with a selection");

  // Shift+Right: extend a one-char selection at every caret.
  viewport.MoveCursorHorizontal(1, /*extend_selection=*/true);

  const auto ranges = viewport.secondary_caret_ranges();
  bool all_secondaries_selected = !ranges.empty();
  for (const auto& caret : ranges) {
    if (!caret.selection_anchor.has_value()) {
      all_secondaries_selected = false;
    }
  }
  Expect(viewport.has_selection() && all_secondaries_selected,
         "an extending move starts a selection at every caret, primary and secondary");
  // Each caret now owns a one-char selection -> aggregate copy grabs one char each.
  const auto copied = viewport.MultiCaretSelectedText();
  Expect(copied.has_value() && *copied == "a\nb\nc",
         "each caret's freshly started selection covers the char it moved over");
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

// A rectangular box selection gives every line in the row span a per-line
// selection from the anchor column to the caret column, with the caret line as
// the primary selection and the others as ranged secondary carets. This is the
// mouse-driven multi-line block selection (Shift+Alt+drag).
void TestSetBoxSelectionSelectsEachLineAcrossColumns() {
  TextViewport viewport;
  viewport.LoadContent("abcdef\nghijkl\nmnopqr\n", "/tmp/mc-box-select.txt");

  // Box from (0,1) to (2,4): columns 1..4 on lines 0,1,2.
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

// The box is a rectangle of visual columns, as in VS Code. It used to be built
// from byte columns clamped per line, so over a tab-indented or non-ASCII line
// the selection slid by the byte count: with the anchor after `a` on `a\tb` and
// the caret at visual column 6 on the ASCII line, the accented line got bytes
// 1..6 — starting mid-character, then snapped — instead of the cells 1..6.
void TestSetBoxSelectionIsRectangularInVisualColumns() {
  TextViewport viewport;
  viewport.LoadContent("a\tb\nabcdefgh\n\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\n", "/tmp/mc-box-visual.txt");
  viewport.SetTabSize(4);

  // Anchor after `a` on line 0 (visual column 1); caret at byte/visual column 6 on
  // the ASCII line 1.
  viewport.SetBoxSelection(TextPosition{0, 1}, TextPosition{1, 6});
  const auto primary = viewport.selection_range();
  Expect(primary.has_value() && primary->start == TextPosition{1, 1} &&
             primary->end == TextPosition{1, 6},
         "the ASCII caret line spans visual columns 1..6 as bytes 1..6");
  const auto ranges = viewport.secondary_caret_ranges();
  Expect(ranges.size() == 1, "one secondary range for the other line");
  // On `a\tb` visual 1 is byte 1 (before the tab) and visual 6 is past the line's
  // 5 cells, so the range ends at the line's end (byte 3).
  Expect(ranges.size() == 1 && ranges[0].selection_anchor == std::optional<TextPosition>(TextPosition{0, 1}) &&
             ranges[0].position == TextPosition{0, 3},
         "the tab line maps the visual columns through its tab");

  // Now extend the same box down onto the accented line (4 two-byte characters,
  // 4 cells). A corner is a text position on its own line, so visual column 6
  // there is byte 8 plus two virtual cells: byte column 10. Visual 1 is byte 2.
  viewport.SetBoxSelection(TextPosition{0, 1}, TextPosition{2, 10});
  const auto primary2 = viewport.selection_range();
  Expect(primary2.has_value() && primary2->start == TextPosition{2, 2} &&
             primary2->end == TextPosition{2, 8},
         "the accented line selects whole characters at the box's visual columns");
  const auto ranges2 = viewport.secondary_caret_ranges();
  Expect(ranges2.size() == 2 && ranges2[1].selection_anchor == std::optional<TextPosition>(TextPosition{1, 1}) &&
             ranges2[1].position == TextPosition{1, 6},
         "the ASCII line keeps bytes 1..6");

  // A corner past the end of its line is a virtual column: anchoring at byte 6 of
  // `a\tb` (5 cells wide, 3 bytes) means visual column 8, so the ASCII line
  // selects up to byte 8.
  viewport.SetBoxSelection(TextPosition{0, 6}, TextPosition{1, 0});
  const auto primary3 = viewport.selection_range();
  Expect(primary3.has_value() && primary3->start == TextPosition{1, 0} &&
             primary3->end == TextPosition{1, 8},
         "a corner beyond its line's end is one cell per extra byte");
  // The visual width of the span bounds the keyboard gesture: the accented line
  // is 4 cells, the ASCII line 8, the tab line 5.
  Expect(viewport.MaxVisualWidthInSpan(0, 2) == 8, "the widest line counts cells, not bytes");
}

// The per-line mapping has an arithmetic fast path for "leading tabs, then plain
// ASCII" (every indented line of a tab-indented file) so a held column-select
// gesture over kernel-style code does not walk each line per keystroke. It must
// agree with the general walk, tab rounding included, on every visual column.
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
      line += "\t";  // a tab past the indentation takes the walk
    }
    if (next(6) == 0) {
      line += "\xc3\xa9";  // so does a multi-byte character
    }
    TextViewport viewport;
    viewport.LoadContent(line + "\nother\n", "/tmp/mc-box-tabs.txt");
    const std::size_t tab_size = 2 + next(7);
    viewport.SetTabSize(tab_size);
    if (round % 2 == 0) {
      (void)viewport.max_visual_columns();  // the width-table path, as after a paint
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

// A leftward (caret column < anchor column) box drag must keep every caret on
// the caret column, aligned with the primary, not stranded on the anchor edge.
// Regression: SetSecondaryCaretsWithRanges normalized each range and forced the
// caret to the max column, so a backward drag put the primary at the left edge
// while every secondary sat at the right edge — a broken, misaligned box.
void TestSetBoxSelectionLeftwardKeepsCaretsOnCaretColumn() {
  TextViewport viewport;
  viewport.LoadContent("abcdef\nghijkl\nmnopqr\n", "/tmp/mc-box-left.txt");

  // Drag from anchor (0,4) leftward to caret (2,1): the caret column is 1.
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

// Lines shorter than both box columns collapse to a zero-width caret at
// end-of-line instead of dropping out of the selection (matches VSCode).
void TestSetBoxSelectionClampsShortLinesToEndOfLine() {
  TextViewport viewport;
  viewport.LoadContent("aaaaaa\nbb\ncccccc\n", "/tmp/mc-box-short.txt");

  // Box columns 3..5; line 1 ("bb") is only length 2.
  viewport.SetBoxSelection(TextPosition{0, 3}, TextPosition{2, 5});

  const auto ranges = viewport.secondary_caret_ranges();
  Expect(ranges.size() == 2, "every line in the span keeps a caret");
  // Line 1 clamps both corners to length 2 -> zero-width caret, no selection.
  const auto short_line = std::find_if(ranges.begin(), ranges.end(),
                                       [](const auto& c) { return c.position.line == 1; });
  Expect(short_line != ranges.end() && short_line->position == TextPosition{1, 2} &&
             !short_line->selection_anchor.has_value(),
         "a line shorter than both box columns collapses to a zero-width end-of-line caret");
}

// Dragging a box straight down at a fixed column (equal corners) degenerates to
// plain zero-width column carets, and PlaceColumnCaretsBetweenLines routes
// through the same path.
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

// Regression: a held Ctrl+Shift+Alt+Down gesture rebuilds the whole caret set on
// every keystroke over a span one line longer than the last, and the scratch
// buffers that rebuild feeds were sized with `reserve(n)` — which allocates
// EXACTLY n, so a monotonically growing refill reallocated and copied on every
// single keystroke. The scratch buffers made the code READ allocation-free while
// costing three reallocations per step; the perf gate measured 1,200 allocations
// across a 400-step gesture.
//
// Pointer identity of `secondary_caret_positions()` is the observable tell: the
// span points into the cache buffer, so a reallocation moves it. Geometric growth
// must settle that pointer in O(log N) moves, not one per step.
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

  // log2(512) == 9 doublings from empty; allow slack for the first few steps
  // landing on small capacities. Exact `reserve` produced 511 moves here.
  Expect(moves <= 16, "a growing box selection must not reallocate its caret buffer per step");
}

// Regression: a multi-caret edit whose carets straddle a preserved interior line
// must NOT publish a single contiguous AppliedEdit. The aggregate history entry
// spans first..last caret line, but the interior line is unchanged; a single
// AppliedEdit would drag markers (breakpoints, LSP diagnostics) on that line to
// the span's end. The viewport must leave last_applied_edit() empty so marker
// consumers take their resync fallback.
void TestMultiCaretDisjointEditPublishesNoAppliedEdit() {
  TextViewport viewport;
  viewport.LoadContent("l0\nl1\nl2\nl3\nl4", "/tmp/mc-disjoint.txt");
  viewport.MoveCursorTo(1, 0);            // primary on line 1
  viewport.SetSecondaryCarets({{3, 0}});  // secondary on line 3 -> line 2 preserved

  viewport.InsertText("X");  // plain insert at both carets, no line-count change

  Expect(viewport.lines().size() == 5 && viewport.lines()[1] == "Xl1" &&
             viewport.lines()[2] == "l2" && viewport.lines()[3] == "Xl3",
         "the edit inserts at both carets and leaves the interior line untouched");
  Expect(!viewport.last_applied_edit().has_value(),
         "a disjoint multi-caret edit must not publish a single contiguous AppliedEdit");
}

// Regression: a multi-caret single-character insert (routing through the
// pair-insert / multi-caret insert paths) always spans disjoint regions, so it
// must likewise not publish a contiguous AppliedEdit.
void TestMultiCaretPairInsertPublishesNoAppliedEdit() {
  TextViewport viewport;
  viewport.LoadContent("a\nb\nc\nd\ne", "/tmp/mc-pair.cpp");
  viewport.MoveCursorTo(0, 1);
  viewport.SetSecondaryCarets({{4, 1}});  // carets on lines 0 and 4

  viewport.InsertCharacter('(');  // multi-caret -> pair-insert or multi-caret insert

  Expect(viewport.secondary_carets().size() == 1,
         "the second caret should survive the multi-caret character insert");
  Expect(!viewport.last_applied_edit().has_value(),
         "multi-caret character insert must not publish a single contiguous AppliedEdit");
}

// Regression: a soft-tab insert with several carets at ragged columns aligns EACH
// caret to its own next tab stop, instead of padding every caret with the primary
// caret's space count.
void TestMultiCaretSoftTabAlignsEachCaretToItsOwnStop() {
  TextViewport viewport;
  viewport.LoadContent("ab\nabcd", "/tmp/mc-softtab.cpp");
  viewport.SetSoftTabs(true);
  viewport.SetIndentWidth(4);
  viewport.MoveCursorTo(0, 2);            // column 2 -> needs 2 spaces to reach stop 4
  viewport.SetSecondaryCarets({{1, 4}});  // column 4 -> a full 4 spaces to reach stop 8

  viewport.InsertTab();

  Expect(viewport.lines()[0] == "ab  ",
         "the caret at column 2 pads 2 spaces to its own next tab stop");
  Expect(viewport.lines()[1] == "abcd    ",
         "the caret at column 4 pads a full indent to its own next tab stop");
}

// Regression: multi-caret outdent keeps every caret (shifted left by the amount its
// line de-indented), instead of dropping the secondary carets and snapping the
// primary to (first_line, 0).
void TestMultiCaretOutdentPreservesCarets() {
  TextViewport viewport;
  viewport.LoadContent("    foo\n    bar", "/tmp/mc-outdent.cpp");
  viewport.SetSoftTabs(true);
  viewport.SetIndentWidth(4);
  viewport.MoveCursorTo(0, 6);            // inside "foo"
  viewport.SetSecondaryCarets({{1, 6}});  // inside "bar"

  Expect(microide::editor::OutdentSelection(viewport), "outdent should change both lines");
  Expect(viewport.lines()[0] == "foo" && viewport.lines()[1] == "bar",
         "both lines lose one indent level");
  Expect(viewport.cursor_line() == 0 && viewport.cursor_column() == 2,
         "the primary caret shifts left by the 4 removed spaces (clamped into 'foo')");
  Expect(viewport.secondary_carets().size() == 1 &&
             viewport.secondary_carets().front() == TextPosition{1, 2},
         "the secondary caret survives and shifts left with its line");
}

// Regression: two carets whose selections OVERLAP must not double-edit shared
// content. ApplyMultiCaretInsert refuses the edit (leaving the buffer untouched)
// rather than corrupt it through the reverse-walk apply.
void TestMultiCaretRefusesOverlappingSelections() {
  TextViewport viewport;
  viewport.LoadContent("abcdefghijklmnop\nsecondline\n", "/tmp/overlap.txt");
  // Primary is an empty caret on line 1 (disjoint). Two OVERLAPPING secondary
  // selections on line 0 — [0,0)-[0,6) and [0,3)-[0,9), overlapping at [3,6) — are
  // injected via the ranges API, which dedups only by position, not overlap.
  viewport.MoveCursorTo(1, 0, false);
  viewport.SetSecondaryCaretsWithRanges(std::vector<microide::editor::SelectionRange>{
      microide::editor::SelectionRange{{0, 0}, {0, 6}},
      microide::editor::SelectionRange{{0, 3}, {0, 9}},
  });
  const std::string before0 = viewport.lines()[0];
  const std::string before1 = viewport.lines()[1];
  viewport.InsertText("X");
  Expect(viewport.lines()[0] == before0 && viewport.lines()[1] == before1,
         "a refused overlapping multi-caret edit must not mutate the buffer at all");
}

// Positive control: DISJOINT selections still apply, proving the overlap guard does
// not reject legitimate multi-caret edits.
void TestMultiCaretDisjointSelectionsStillApply() {
  TextViewport viewport;
  viewport.LoadContent("abcdefghijklmnop\nsecondline\n", "/tmp/disjoint.txt");
  viewport.MoveCursorTo(1, 0, false);
  viewport.SetSecondaryCaretsWithRanges(std::vector<microide::editor::SelectionRange>{
      microide::editor::SelectionRange{{0, 0}, {0, 3}},  // "abc"
      microide::editor::SelectionRange{{0, 6}, {0, 9}},  // "ghi"
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

}  // namespace

// TD-2026-07-17-068: a grouped, NON-contiguous multi-caret edit used to drop to a
// whole-buffer-snapshot fallback (two full TextBuffer::Snapshot() copies). The
// disjoint-range model must (a) never materialize the whole buffer and (b) still
// undo/redo the group atomically and exactly, including the net line-count shift
// each earlier caret imposes on the ranges below it (delta reindex + gap stitch).
void TestMultiCaretDisjointGroupedInsertNoSnapshotRoundTrips() {
  using microide::editor::TextBuffer;
  TextViewport viewport;
  viewport.LoadContent(MakeNumberedLines(400), "/tmp/mc-disjoint-insert.txt");
  const std::string original = JoinViewportLines(viewport);
  const std::size_t initial_lines = viewport.lines().size();

  viewport.MoveCursorTo(5, 0, false);
  // Non-contiguous carets far apart, so each lands in its own disjoint range.
  viewport.SetSecondaryCarets({{200, 0}, {350, 0}});

  TextBuffer::reset_snapshot_build_count();
  viewport.InsertText("A\nB");  // +1 line at each caret -> exercises delta reindex
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

// Negative-delta coverage: a grouped multi-caret backspace at column 0 joins each
// caret's line onto the previous one (removing a newline, delta -1 per site). The
// ranges below each join must reindex downward without a whole-buffer snapshot,
// and the group must undo/redo exactly.
void TestMultiCaretDisjointGroupedJoinNoSnapshotRoundTrips() {
  using microide::editor::TextBuffer;
  TextViewport viewport;
  viewport.LoadContent(MakeNumberedLines(300), "/tmp/mc-disjoint-join.txt");
  const std::string original = JoinViewportLines(viewport);
  const std::size_t initial_lines = viewport.lines().size();

  viewport.MoveCursorTo(10, 0, false);
  viewport.SetSecondaryCarets({{150, 0}, {250, 0}});

  TextBuffer::reset_snapshot_build_count();
  viewport.Backspace();  // each col-0 backspace joins with the previous line
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

// The batched lower-edit remap (TD-2026-07-17A-054: O(carets^2) -> O(carets))
// must produce byte-identical results to the old per-edit remap at scale. Insert
// one character at hundreds of carets spread across a single line and verify every
// caret lands exactly after its own insertion, shifted right by the count of lower
// insertions -- the exact stale-caret bug the O(k^2) walk avoided.
void TestMultiCaretManySameLineInsertRemapsEveryCaret() {
  TextViewport viewport;
  constexpr std::size_t kCaretCount = 400;
  // A line of '.'s, one caret every 2 columns: 0, 2, 4, ... (disjoint).
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
  // Primary was at column 0 -> lands at column 1 (its own X, no lower carets).
  Expect(viewport.cursor_line() == 0 && viewport.cursor_column() == 1,
         "the lowest caret should land right after its own insertion");
  // Caret originally at column 2*i now sits after i lower insertions plus its own:
  // 2*i + i (lower Xs) + 1 (own X) = 3*i + 1.
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
  // 800 '.'s + 400 'X's inserted.
  Expect(viewport.lines()[0].size() == kCaretCount * 3,
         "exactly one insertion should land at each of the hundreds of carets");
}

// Same scale check for the auto-close fast path (TryMultiCaretPairInsert): typing
// '(' at many carets on one line inserts a matching '()' pair at each and shifts
// every higher caret by two columns, landing each caret between its own pair.
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
  // Primary at column 0 lands between its own "()" -> column 1.
  Expect(viewport.cursor_line() == 0 && viewport.cursor_column() == 1,
         "the lowest caret should land inside its own inserted pair");
  // Caret originally at 2*i: 2 columns per lower pair (2*i) plus 1 (inside own
  // pair) = 2*i + i*2 ... each lower caret adds 2, so 2*i + 2*i + 1 = 4*i + 1.
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

// AddSecondaryCaret relies on secondary_carets_ already being sorted by position
// to find both the duplicate and the insertion point in one binary probe. Pin the
// two properties that relies on — the result stays sorted whatever order carets
// arrive in, and an exact duplicate (or the primary caret) is still rejected.
void TestAddSecondaryCaretKeepsSortedOrderAndDedupes() {
  TextViewport viewport;
  viewport.LoadContent("alpha line\nbeta line\ngamma line\ndelta line\n",
                       "/tmp/mc-sorted-insert.txt");
  viewport.MoveCursorTo(0, 0);

  // Insert out of order; the vector must end up sorted by (line, column).
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

  // An exact duplicate is rejected wherever it lands in the order.
  viewport.AddSecondaryCaret(1, 4);
  viewport.AddSecondaryCaret(3, 2);
  Expect(viewport.secondary_carets().size() == 4, "duplicate carets must not be added twice");

  // The primary caret position is never duplicated as a secondary.
  viewport.AddSecondaryCaret(0, 0);
  Expect(viewport.secondary_carets().size() == 4,
         "the primary caret position must not be added as a secondary");
}

void RegisterEditorMultiCaretTests(std::vector<TestCase>& tests) {
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
  AddTest(tests, "EditorMultiCaret/RefusesOverlappingSelections",
          TestMultiCaretRefusesOverlappingSelections);
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

}  // namespace microide::tests
