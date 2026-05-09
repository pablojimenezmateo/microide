#include "TestSupport.h"

#include "editor/SyntaxDefinitionLoader.h"
#include "editor/FoldingModel.h"
#include "editor/RuntimeSyntaxRegistry.h"
#include "editor/TextViewport.h"

#include <algorithm>
#include <string>
#include <vector>

namespace microide::tests {
namespace {

using microide::editor::SyntaxTokenKind;
using microide::editor::TextPosition;
using microide::editor::TextViewport;
using microide::editor::FoldingModel;

struct ScopedRuntimeSyntaxRegistryReset {
  ~ScopedRuntimeSyntaxRegistryReset() { microide::editor::runtime_syntax::ReloadDefinitions({}); }
};

void TestTextViewportSmallFileKeepsSyntaxHighlighting() {
  TextViewport viewport;
  viewport.LoadContent("int value = 42;\n", "/tmp/sample.cpp");

  Expect(viewport.syntax_highlighting_enabled(),
         "small files should keep syntax highlighting enabled");

  const auto& tokens = viewport.HighlightedLineTokens(0);
  Expect(!tokens.empty(), "small files should still produce syntax tokens");
  const bool saw_non_plain =
      std::any_of(tokens.begin(), tokens.end(),
                  [](SyntaxTokenKind kind) { return kind != SyntaxTokenKind::Plain; });
  Expect(saw_non_plain, "small C++ files should preserve non-plain syntax tokens");
}

void TestTextViewportLargeCodeFixtureKeepsSyntaxHighlighting() {
  TextViewport viewport;
  Expect(viewport.OpenFile(FixturePath("large/code/large_sample.cpp")),
         "large code fixture should open");

  Expect(viewport.syntax_highlighting_enabled(),
         "large code fixtures should keep syntax highlighting enabled");
  const auto& tokens = viewport.HighlightedLineTokens(0);
  Expect(!tokens.empty(),
         "large code fixtures should still produce per-line syntax tokens");
  const bool saw_non_plain =
      std::any_of(tokens.begin(), tokens.end(),
                  [](SyntaxTokenKind kind) { return kind != SyntaxTokenKind::Plain; });
  Expect(saw_non_plain,
         "large code fixtures should keep their non-plain syntax classes");
}

void TestTextViewportLargePlainFixtureKeepsSyntaxHighlighting() {
  TextViewport viewport;
  Expect(viewport.OpenFile(FixturePath("large/plain/large_story.txt")),
         "large plain-text fixture should open");

  Expect(viewport.syntax_highlighting_enabled(),
         "large plain-text fixtures should keep syntax token generation enabled");
  Expect(!viewport.HighlightedLineTokens(0).empty(),
         "large byte-size fixtures should still expose syntax tokens");
}

void TestTextViewportEditingPastFormerLargeFileLineThresholdKeepsSyntaxHighlighting() {
  TextViewport viewport;
  std::string content;
  for (int i = 0; i < 3999; ++i) {
    if (!content.empty()) {
      content.push_back('\n');
    }
    content += "int value = 42;";
  }
  viewport.LoadContent(content, "/tmp/threshold.cpp");

  Expect(viewport.syntax_highlighting_enabled(),
         "content just below the former line threshold should keep syntax highlighting");

  viewport.MoveCursorTo(viewport.line_count() - 1, viewport.lines().back().size());
  viewport.InsertNewline();

  Expect(viewport.syntax_highlighting_enabled(),
         "editing across the former line threshold should keep syntax highlighting enabled");
  Expect(!viewport.HighlightedLineTokens(0).empty(),
         "editing across the former line threshold should keep producing syntax tokens");

  Expect(viewport.Undo(), "undo should succeed after crossing the line threshold");
  Expect(viewport.syntax_highlighting_enabled(),
         "undo below the former line threshold should restore syntax highlighting");
  Expect(!viewport.HighlightedLineTokens(0).empty(),
         "undo below the former line threshold should restore syntax tokens");
}

void TestTextViewportEditingPastFormerLargeFileByteThresholdKeepsSyntaxHighlighting() {
  TextViewport viewport;
  viewport.LoadContent("int value = 42;\n", "/tmp/threshold.cpp");

  viewport.MoveCursorTo(0, viewport.lines().front().size());
  viewport.InsertText(std::string(400000, 'a'));

  Expect(viewport.syntax_highlighting_enabled(),
         "editing across the former byte threshold should keep syntax highlighting enabled");
  Expect(!viewport.HighlightedLineTokens(0).empty(),
         "editing across the former byte threshold should keep producing syntax tokens");

  Expect(viewport.Undo(), "undo should succeed after crossing the byte threshold");
  Expect(viewport.syntax_highlighting_enabled(),
         "undo below the former byte threshold should restore syntax highlighting");
  Expect(!viewport.HighlightedLineTokens(0).empty(),
         "undo below the former byte threshold should restore syntax tokens");
}

void TestTextViewportCacheStatsTrackWarmLayoutAndHighlightHits() {
  TextViewport viewport;
  viewport.LoadContent("int value = 42;\n", "/tmp/cache-stats.cpp");
  viewport.SetViewportSize(12, 80);

  viewport.ResetCacheStats();
  (void)viewport.VisibleLineLayout(0);
  (void)viewport.HighlightedLineTokens(0);
  (void)viewport.VisibleLineLayout(0);
  (void)viewport.HighlightedLineTokens(0);

  const auto stats = viewport.CacheStats();
  Expect(stats.visible_line_queries == 2,
         "viewport cache stats should count visible-line queries");
  Expect(stats.visible_line_hits == 1,
         "viewport cache stats should treat a repeated visible-line lookup as a hit");
  Expect(stats.highlight_queries == 2,
         "viewport cache stats should count highlight queries");
  Expect(stats.highlight_hits == 1,
         "viewport cache stats should treat a repeated highlight lookup as a hit");
  Expect(stats.highlight_state_advances == 0,
         "repeated highlight lookups for the same line should not replay prior line state");
}

void TestTextViewportCaretMovementKeepsVisibleLineLayoutCached() {
  TextViewport viewport;
  viewport.LoadContent("int value = 42;\n", "/tmp/cache-caret.cpp");
  viewport.SetViewportSize(12, 80);

  (void)viewport.VisibleLineLayout(0);
  viewport.ResetCacheStats();
  viewport.MoveCursorHorizontal(1);
  (void)viewport.VisibleLineLayout(0);

  const auto stats = viewport.CacheStats();
  Expect(stats.visible_line_queries == 1,
         "cursor-only movement should still query the visible-line cache once");
  Expect(stats.visible_line_hits == 1,
         "cursor-only movement should reuse cached visible-line text layout");
}

void TestTextViewportHighlightCheckpointsBoundFarReplay() {
  TextViewport viewport;
  std::string content;
  for (int i = 0; i < 4096; ++i) {
    if (!content.empty()) {
      content.push_back('\n');
    }
    content += "int value = 42;";
  }
  viewport.LoadContent(content, "/tmp/highlight-checkpoints.cpp");

  (void)viewport.HighlightedLineTokens(0);
  viewport.ResetCacheStats();

  const auto& tokens = viewport.HighlightedLineTokens(4095);
  Expect(!tokens.empty(), "far-line syntax queries should still produce tokens");

  const auto stats = viewport.CacheStats();
  Expect(stats.highlight_state_advances < 128,
         "far-line syntax queries should replay at most one checkpoint window");
}

void TestTextViewportHighlightCheckpointsPreserveMultilineState() {
  TextViewport viewport;
  std::string content;
  for (int i = 0; i < 200; ++i) {
    if (!content.empty()) {
      content.push_back('\n');
    }
    if (i == 10) {
      content += "/* begin comment";
    } else if (i == 170) {
      content += "end comment */ int value = 42;";
    } else {
      content += "comment payload";
    }
  }
  viewport.LoadContent(content, "/tmp/highlight-comment.cpp");

  (void)viewport.HighlightedLineTokens(0);
  const auto& inside_comment_tokens = viewport.HighlightedLineTokens(150);
  Expect(!inside_comment_tokens.empty(),
         "multiline syntax queries should still produce tokens inside far regions");
  Expect(std::all_of(inside_comment_tokens.begin(), inside_comment_tokens.end(),
                     [](SyntaxTokenKind kind) { return kind == SyntaxTokenKind::Comment; }),
         "checkpointed syntax replay should preserve multiline comment state across checkpoints");

  const auto& after_comment_tokens = viewport.HighlightedLineTokens(170);
  Expect(std::any_of(after_comment_tokens.begin(), after_comment_tokens.end(),
                     [](SyntaxTokenKind kind) { return kind != SyntaxTokenKind::Comment; }),
         "syntax should still recover to non-comment token classes after a multiline region closes");
}

void TestTextViewportEditingNearTailDoesNotRebuildFarCheckpoints() {
  TextViewport viewport;
  std::string content;
  for (int i = 0; i < 4096; ++i) {
    if (!content.empty()) {
      content.push_back('\n');
    }
    content += "int value = 42;";
  }
  viewport.LoadContent(content, "/tmp/highlight-edit-tail.cpp");

  (void)viewport.HighlightedLineTokens(4095);
  viewport.MoveCursorTo(4095, viewport.lines().back().size());
  viewport.InsertText(" // tail");
  viewport.ResetCacheStats();

  const auto& tokens = viewport.HighlightedLineTokens(4095);
  Expect(!tokens.empty(), "tail edits should preserve syntax tokens for the edited line");

  const auto stats = viewport.CacheStats();
  Expect(stats.highlight_checkpoint_advances == 0,
         "tail edits should not rebuild previously valid highlight checkpoints");
  Expect(stats.highlight_state_advances < 128,
         "tail edits should only replay the local checkpoint window");
}

void TestTextViewportInsertNewlineCopiesLeadingIndentation() {
  TextViewport viewport;
  viewport.LoadContent("  const value = 1;", "/tmp/indent-newline.cpp");

  viewport.MoveCursorTo(0, viewport.lines()[0].size());
  viewport.InsertNewline();

  Expect(viewport.line_count() == 2,
         "newline indentation fixture should split the current line");
  Expect(viewport.lines()[0] == "  const value = 1;",
         "newline indentation fixture should keep the original line text");
  Expect(viewport.lines()[1] == "  ",
         "newline insertion should copy the previous line indentation");
  Expect(viewport.cursor_line() == 1 && viewport.cursor_column() == 2,
         "newline insertion should place the caret after the copied indentation");
}

void TestTextViewportInsertNewlineOnWhitespaceOnlyLineDoesNotCarryIndentForward() {
  TextViewport viewport;
  viewport.LoadContent("  const value = 1;", "/tmp/indent-newline-reset.cpp");

  viewport.MoveCursorTo(0, viewport.lines()[0].size());
  viewport.InsertNewline();
  viewport.InsertNewline();

  Expect(viewport.line_count() == 3,
         "double-newline indentation fixture should produce an extra blank line");
  Expect(viewport.lines()[1] == "  ",
         "the first inserted line should keep the inherited indentation");
  Expect(viewport.lines()[2].empty(),
         "pressing Enter on a whitespace-only line should start the next line at column zero");
  Expect(viewport.cursor_line() == 2 && viewport.cursor_column() == 0,
         "pressing Enter on a whitespace-only line should move the caret to column zero");
}

void TestTextViewportMultiCaretNewlineCopiesIndentationPerCaret() {
  TextViewport viewport;
  viewport.LoadContent("  alpha\n\tbeta\n", "/tmp/multi-caret-newline.cpp");
  viewport.MoveCursorTo(0, viewport.lines()[0].size());
  viewport.SetSecondaryCarets({{1, viewport.lines()[1].size()}});

  viewport.InsertNewline();

  Expect(viewport.line_count() == 5,
         "multi-caret newline indentation fixture should split both touched lines");
  Expect(viewport.lines()[0] == "  alpha" && viewport.lines()[1] == "  ",
         "multi-caret newline should preserve and copy space indentation");
  Expect(viewport.lines()[2] == "\tbeta" && viewport.lines()[3] == "\t",
         "multi-caret newline should preserve and copy tab indentation");
}

void TestTextViewportLastAppliedEditTracksInsertUndoRedo() {
  TextViewport viewport;
  viewport.LoadContent("alpha\n", "/tmp/applied-edit-insert.txt");
  viewport.MoveCursorTo(0, 2);

  viewport.InsertText("XYZ");

  Expect(viewport.last_applied_edit().has_value(),
         "single-range insert should publish an applied edit");
  Expect(viewport.last_applied_edit()->range_before.start == microide::editor::TextPosition{0, 2} &&
             viewport.last_applied_edit()->range_before.end ==
                 microide::editor::TextPosition{0, 2},
         "single-range insert should describe the pre-edit insertion point");
  Expect(viewport.last_applied_edit()->replacement_text == "XYZ",
         "single-range insert should publish the inserted text");

  Expect(viewport.Undo(), "undo should succeed after an insert");
  Expect(viewport.last_applied_edit().has_value(),
         "undo should publish the reverse applied edit");
  Expect(viewport.last_applied_edit()->range_before.start == microide::editor::TextPosition{0, 2} &&
             viewport.last_applied_edit()->range_before.end ==
                 microide::editor::TextPosition{0, 5},
         "undo should describe the inserted range it removes");
  Expect(viewport.last_applied_edit()->replacement_text.empty(),
         "undo should publish empty replacement text for a pure deletion");

  Expect(viewport.Redo(), "redo should succeed after an undo");
  Expect(viewport.last_applied_edit().has_value(),
         "redo should republish the forward applied edit");
  Expect(viewport.last_applied_edit()->range_before.start == microide::editor::TextPosition{0, 2} &&
             viewport.last_applied_edit()->range_before.end ==
                 microide::editor::TextPosition{0, 2},
         "redo should restore the forward insertion point");
  Expect(viewport.last_applied_edit()->replacement_text == "XYZ",
         "redo should restore the forward replacement text");
}

void TestTextViewportLastAppliedEditTracksMultilineReplacement() {
  TextViewport viewport;
  viewport.LoadContent("alpha\nbeta\ngamma\n", "/tmp/applied-edit-range.txt");

  Expect(viewport.ReplaceRange(microide::editor::SelectionRange{
                                   .start = microide::editor::TextPosition{0, 2},
                                   .end = microide::editor::TextPosition{1, 2},
                               },
                               "X\nY"),
         "multiline replacement fixture should apply");
  Expect(viewport.last_applied_edit().has_value(),
         "multiline replacement should publish an applied edit");
  Expect(viewport.last_applied_edit()->range_before.start == microide::editor::TextPosition{0, 2} &&
             viewport.last_applied_edit()->range_before.end ==
                 microide::editor::TextPosition{1, 2},
         "multiline replacement should preserve the original replaced range");
  Expect(viewport.last_applied_edit()->replacement_text == "X\nY",
         "multiline replacement should publish normalized replacement text");
}

void TestTextViewportUndoRedoPreservesLatestViewState() {
  TextViewport viewport;
  viewport.LoadContent("zero\none\ntwo\nthree\nfour\nfive\nsix\nseven\n", "/tmp/history.cpp");
  viewport.SetViewportSize(8, 12);

  viewport.MoveCursorTo(4, 4);
  viewport.InsertText("!\nmore");
  viewport.MoveCursorTo(5, 2);
  viewport.SetScrollLine(2);

  Expect(viewport.Undo(), "undo should succeed after a multiline insertion");
  Expect(viewport.lines().size() == 9 && viewport.lines()[4] == "four" &&
             viewport.lines()[5] == "five",
         "undo should restore the original document text");
  Expect(viewport.cursor_line() == 4 && viewport.cursor_column() == 4,
         "undo should restore the pre-edit cursor position");

  Expect(viewport.Redo(), "redo should succeed after undoing a multiline insertion");
  Expect(viewport.lines().size() == 10 && viewport.lines()[4] == "four!" &&
             viewport.lines()[5] == "more",
         "redo should restore the edited document text");
  Expect(viewport.cursor_line() == 5 && viewport.cursor_column() == 2,
         "redo should restore the latest cursor position from before undo");
  Expect(viewport.scroll_line() == 2,
         "redo should restore the latest scroll position from before undo");
}

void TestTextViewportUndoRedoPreservesSecondaryCarets() {
  TextViewport viewport;
  viewport.LoadContent("alpha\nbeta\ngamma\n", "/tmp/multi-caret-history.txt");
  viewport.MoveCursorTo(1, 2);
  viewport.SetSecondaryCarets({{0, 1}, {2, 3}});

  viewport.InsertText("X");

  Expect(viewport.has_multiple_carets(),
         "multi-caret fixture should still have secondary carets after edit");
  Expect(viewport.secondary_carets().size() == 2,
         "multi-caret fixture should keep the expected secondary caret count");
  const std::vector<microide::editor::TextPosition> secondary_after_edit =
      viewport.secondary_carets();

  Expect(viewport.Undo(), "undo should succeed after an edit with secondary carets");
  Expect(viewport.cursor_line() == 1 && viewport.cursor_column() == 2,
         "undo should restore the pre-edit primary caret position");
  Expect(viewport.secondary_carets().size() == 2 &&
             viewport.secondary_carets()[0] == microide::editor::TextPosition{0, 1} &&
             viewport.secondary_carets()[1] == microide::editor::TextPosition{2, 3},
         "undo should restore the pre-edit secondary caret set");

  Expect(viewport.Redo(), "redo should succeed after undoing an edit with secondary carets");
  Expect(viewport.secondary_carets() == secondary_after_edit,
         "redo should restore the secondary caret set captured at edit time");
}

void TestTextViewportMultiCaretInsertAndUndoAreAtomic() {
  TextViewport viewport;
  viewport.LoadContent("abc\ndef\nghi\n", "/tmp/multi-caret-insert.txt");
  viewport.MoveCursorTo(1, 1);
  viewport.SetSecondaryCarets({{0, 1}, {2, 1}});

  viewport.InsertText("X");
  Expect(viewport.lines()[0] == "aXbc" && viewport.lines()[1] == "dXef" &&
             viewport.lines()[2] == "gXhi",
         "multi-caret insert should fan out to primary and secondary caret positions");

  Expect(viewport.Undo(), "undo should succeed after a multi-caret insert");
  Expect(viewport.lines()[0] == "abc" && viewport.lines()[1] == "def" &&
             viewport.lines()[2] == "ghi",
         "undo should revert all multi-caret insertions atomically");
}

void TestTextViewportMultiCaretBackspaceAndDeleteForward() {
  TextViewport viewport;
  viewport.LoadContent("abcd\nefgh\n", "/tmp/multi-caret-delete.txt");
  viewport.MoveCursorTo(0, 2);
  viewport.SetSecondaryCarets({{1, 2}});

  viewport.Backspace();
  Expect(viewport.lines()[0] == "acd" && viewport.lines()[1] == "egh",
         "multi-caret backspace should erase one text column for each caret");

  viewport.DeleteForward();
  Expect(viewport.lines()[0] == "ad" && viewport.lines()[1] == "eh",
         "multi-caret delete-forward should erase one text column for each caret");
}

void TestTextViewportMultiCaretDeleteCurrentLineIsAtomic() {
  TextViewport viewport;
  viewport.LoadContent("line0\nline1\nline2\nline3\n", "/tmp/multi-caret-line-delete.txt");
  viewport.MoveCursorTo(1, 0);
  viewport.SetSecondaryCarets({{3, 2}});

  Expect(viewport.DeleteCurrentLine(),
         "multi-caret delete current line should succeed");
  const bool has_line1 = std::find(viewport.lines().begin(), viewport.lines().end(), "line1") !=
                         viewport.lines().end();
  const bool has_line3 = std::find(viewport.lines().begin(), viewport.lines().end(), "line3") !=
                         viewport.lines().end();
  Expect(!has_line1 && !has_line3,
         "multi-caret line deletion should remove every targeted line");

  Expect(viewport.Undo(), "undo should succeed after multi-caret line deletion");
  Expect(viewport.lines().size() >= 4 && viewport.lines()[1] == "line1" &&
             viewport.lines()[3] == "line3",
         "undo should restore all deleted lines atomically");
}

void TestTextViewportSelectAllCopiesLargeDocumentRoundTrip() {
  TextViewport viewport;
  std::string content;
  content.reserve(10000 * 16);
  for (int i = 0; i < 10000; ++i) {
    if (!content.empty()) {
      content.push_back('\n');
    }
    content += "line ";
    content += std::to_string(i);
  }

  viewport.LoadContent(content, "/tmp/copy-large.txt");
  viewport.SelectAll();

  const std::string copied = viewport.SelectedText();
  Expect(copied == content, "select-all copy should round-trip a 10k-line document");
}

void TestTextViewportSoftWrapExposesVisualRowsAndWrappedCaret() {
  TextViewport viewport;
  viewport.LoadContent("abcdefghijklmnopqrst\n", "/tmp/soft-wrap-rows.txt");
  viewport.SetViewportSize(10, 8);
  viewport.SetSoftWrap(true);
  viewport.MoveCursorTo(0, 10);

  Expect(viewport.VisualRowCount() == 4,
         "soft wrap should expose wrapped rows through the dedicated visual row API");
  Expect(viewport.visual_line_count() == 4,
         "soft wrap should expose three content rows plus the trailing empty-line row");

  const auto row0 = viewport.VisibleWrappedRowLayout(0);
  const auto row1 = viewport.VisibleWrappedRowLayout(1);
  const auto row2 = viewport.VisibleWrappedRowLayout(2);
  Expect(row0.text.rfind("abcdefgh", 0) == 0,
         "first wrapped row should start at the beginning of the line");
  Expect(row1.text.rfind("ijklmnop", 0) == 0,
         "second wrapped row should expose the middle slice");
  Expect(row2.text.rfind("qrst", 0) == 0,
         "last wrapped row should expose the tail slice");
  Expect(row1.caret_visible && row1.caret_column == 2,
         "wrapped row layout should place the caret on the correct wrapped row");
}

void TestTextViewportSoftWrapMoveCursorVerticalUsesWrappedRows() {
  TextViewport viewport;
  viewport.LoadContent("1234567\nabcdefghijklmnopqrst\nABCDEFGH12345678\n",
                       "/tmp/soft-wrap-vertical.txt");
  viewport.SetViewportSize(3, 8);
  viewport.SetSoftWrap(true);
  viewport.MoveCursorTo(0, 7);

  viewport.MoveCursorVertical(1);
  Expect(viewport.cursor_line() == 1 && viewport.cursor_column() == 7,
         "moving down into a wrapped logical line should land on its first visual row");

  viewport.MoveCursorVertical(1);
  Expect(viewport.cursor_line() == 1 && viewport.cursor_column() == 15,
         "moving down within a wrapped logical line should follow visual rows, not logical lines");

  viewport.MoveCursorVertical(1);
  Expect(viewport.cursor_line() == 1 && viewport.cursor_column() == 20,
         "moving onto a short continuation row should clamp at that row's visual end");

  viewport.MoveCursorVertical(1);
  Expect(viewport.cursor_line() == 2 && viewport.cursor_column() == 7,
         "preferred column should reapply after leaving a short wrapped continuation row");

  viewport.MoveCursorVertical(-1);
  Expect(viewport.cursor_line() == 1 && viewport.cursor_column() == 20,
         "moving back onto the short continuation row should clamp again while preserving the target column");
}

void TestTextViewportSoftWrapPageMovesByVisibleRows() {
  TextViewport viewport;
  viewport.LoadContent("abcdefghijklmnopqrst\nABCDEFGH12345678\nxyz\n", "/tmp/soft-wrap-page.txt");
  viewport.SetViewportSize(3, 8);
  viewport.SetSoftWrap(true);
  viewport.MoveCursorTo(0, 0);

  viewport.Page(1);
  Expect(viewport.cursor_line() == 0 && viewport.cursor_column() == 16,
         "page down should advance by visible wrapped rows within the same logical line");

  viewport.Page(1);
  Expect(viewport.cursor_line() == 1 && viewport.cursor_column() == 8,
         "page down should continue counting wrapped rows across logical line boundaries");

  viewport.Page(-1);
  Expect(viewport.cursor_line() == 0 && viewport.cursor_column() == 16,
         "page up should move back by visible wrapped rows under soft wrap");
}

void TestTextViewportSoftWrapMovesSecondaryCaretsByVisibleRows() {
  TextViewport viewport;
  viewport.LoadContent("abcdefghijklmnopqrst\nabcdefghijk\nqrstuvwxyzabcdefgh\n",
                       "/tmp/soft-wrap-secondary-carets.txt");
  viewport.SetViewportSize(10, 8);
  viewport.SetSoftWrap(true);
  viewport.MoveCursorTo(0, 7);
  viewport.SetSecondaryCarets({{1, 3}, {2, 5}});

  viewport.MoveCursorVertical(1);

  Expect(viewport.cursor_line() == 0 && viewport.cursor_column() == 15,
         "the primary caret should move down by one wrapped row");
  const auto carets = viewport.secondary_carets();
  Expect(carets.size() == 2, "moving wrapped multi-carets should preserve every secondary caret");
  Expect(carets[0] == TextPosition{1, 11},
         "a secondary caret on a short wrapped line should clamp at that row's end");
  Expect(carets[1] == TextPosition{2, 13},
         "each secondary caret should preserve its own preferred column when moving by wrapped rows");
}

void TestTextViewportSoftWrapVisualHitRoundTripsContinuationRows() {
  TextViewport viewport;
  viewport.LoadContent("abcdefghijklmnopqrst\n", "/tmp/soft-wrap-hit-roundtrip.txt");
  viewport.SetViewportSize(10, 8);
  viewport.SetSoftWrap(true);

  const TextPosition hit = viewport.LogicalPositionForVisualHit(1, 3);
  Expect(hit == TextPosition{0, 11},
         "wrapped hit-testing should offset continuation rows by their visual start");

  viewport.MoveCursorTo(hit.line, hit.column);
  const auto row1 = viewport.VisibleWrappedRowLayout(1);
  Expect(row1.caret_visible && row1.caret_column == 3,
         "placing the caret from a wrapped hit-test should round-trip back onto the same continuation row");
}

void TestTextViewportSoftWrapContinuationHitsUseVisualOffset() {
  TextViewport viewport;
  viewport.LoadContent("abcdefghijklmnopqrst\n", "/tmp/soft-wrap-hit-offset.txt");
  viewport.SetViewportSize(10, 8);
  viewport.SetSoftWrap(true);

  const TextPosition hit = viewport.LogicalPositionForVisualHit(1, 2);
  Expect(hit == TextPosition{0, 10},
         "continuation-row hit-testing should resolve relative to the wrapped row start, not column zero");

  const TextPosition end_hit = viewport.LogicalPositionForVisualHit(2, 8);
  Expect(end_hit == TextPosition{0, 20},
         "wrapped hit-testing should allow landing on the end of a short continuation row");
}

#ifndef NDEBUG
void TestTextViewportSoftWrapViewportResizeRebuildsWrapCacheLazily() {
  TextViewport viewport;
  viewport.LoadContent("abcdefghijklmnopqrst\n", "/tmp/soft-wrap-cache.txt");
  viewport.SetViewportSize(10, 8);
  viewport.SetSoftWrap(true);

  (void)viewport.VisibleWrappedRowLayout(0);
  const std::size_t first_build_count = viewport.WrappedRowLayoutBuildCountForDebug();

  viewport.SetViewportSize(10, 12);
  Expect(viewport.WrappedRowLayoutBuildCountForDebug() == first_build_count,
         "resizing the viewport should not eagerly rebuild wrapped rows");

  (void)viewport.VisibleWrappedRowLayout(0);
  Expect(viewport.WrappedRowLayoutBuildCountForDebug() == first_build_count + 1,
         "the first wrapped-row query after resize should rebuild the cache once");

  (void)viewport.VisibleWrappedRowLayout(0);
  Expect(viewport.WrappedRowLayoutBuildCountForDebug() == first_build_count + 1,
         "repeated wrapped-row queries without edits or resize should reuse the cached layout");
}
#endif

void TestTextViewportSoftWrapForcesHorizontalScrollToZero() {
  TextViewport viewport;
  viewport.LoadContent("abcdefghijklmnopqrst\n", "/tmp/soft-wrap-scroll.txt");
  viewport.SetViewportSize(10, 8);
  viewport.SetHorizontalScroll(5);
  Expect(viewport.horizontal_scroll() == 5,
         "horizontal scrolling should remain available before soft wrap is enabled");

  viewport.SetSoftWrap(true);
  Expect(viewport.horizontal_scroll() == 0,
         "enabling soft wrap should clear any existing horizontal scroll offset");

  viewport.SetHorizontalScroll(3);
  Expect(viewport.horizontal_scroll() == 0,
         "soft wrap should reject new horizontal scrolling while wrap is enabled");
}

FoldingModel::ComputeOptions DefaultFoldOptions() {
  FoldingModel::ComputeOptions options;
  options.bracket_pairs = {{'{', '}'}};
  options.use_indent_source = true;
  options.tab_size = 4;
  return options;
}

void TestTextViewportCollapsedFoldHidesBodyRows() {
  TextViewport viewport;
  viewport.LoadContent("void f() {\n  a();\n  b();\n}\nafter();\n", "/tmp/fold.cpp");
  FoldingModel folding_model;
  Expect(folding_model.Compute(viewport.lines(), DefaultFoldOptions()),
         "fold compute should complete for viewport fold fixture");
  Expect(folding_model.Collapse(0), "outer fold should collapse");
  viewport.SetFoldingModel(&folding_model);

  Expect(viewport.VisualRowCount() == 3,
         "collapsed fold should expose the opener row, later visible lines, and the trailing empty line");
  const auto row0 = viewport.WrappedVisualRowLayout(0);
  const auto row1 = viewport.WrappedVisualRowLayout(1);
  const auto row2 = viewport.WrappedVisualRowLayout(2);
  Expect(row0.line_index == 0,
         "collapsed fold should keep the opener as the visible anchor row");
  Expect(row1.line_index == 4,
         "collapsed fold should hide the folded body and closer from visual rows");
  Expect(row2.line_index == 5,
         "collapsed fold should leave the trailing empty line visible after the folded block");
}

void TestTextViewportCollapsedFoldVerticalMotionSkipsHiddenLines() {
  TextViewport viewport;
  viewport.LoadContent("before();\nvoid f() {\n  a();\n  b();\n}\nafter();\n", "/tmp/fold-motion.cpp");
  FoldingModel folding_model;
  Expect(folding_model.Compute(viewport.lines(), DefaultFoldOptions()),
         "fold compute should complete for motion fixture");
  Expect(folding_model.Collapse(1), "inner function fold should collapse");
  viewport.SetFoldingModel(&folding_model);
  viewport.MoveCursorTo(0, 3);

  viewport.MoveCursorVertical(1);
  Expect(viewport.cursor_line() == 1,
         "moving down should land on the collapsed opener row");

  viewport.MoveCursorVertical(1);
  Expect(viewport.cursor_line() == 5,
         "moving down again should skip the hidden folded body");

  viewport.MoveCursorVertical(-1);
  Expect(viewport.cursor_line() == 1,
         "moving back up should treat the collapsed fold as one visible row");
}

void TestTextViewportCollapsedFoldPageMovesByVisibleRows() {
  TextViewport viewport;
  viewport.LoadContent("line0\nvoid f() {\n  a();\n  b();\n}\nline5\nline6\n", "/tmp/fold-page.cpp");
  FoldingModel folding_model;
  Expect(folding_model.Compute(viewport.lines(), DefaultFoldOptions()),
         "fold compute should complete for page fixture");
  Expect(folding_model.Collapse(1), "function fold should collapse");
  viewport.SetFoldingModel(&folding_model);
  viewport.SetViewportSize(2, 20);
  viewport.MoveCursorTo(0, 0);

  viewport.Page(1);
  Expect(viewport.cursor_line() == 1,
         "page down should still follow the existing visible_lines - 1 step across a collapsed fold");

  viewport.Page(1);
  Expect(viewport.cursor_line() == 5,
         "page down should count a collapsed fold as a single visible row when advancing past it");

  viewport.Page(-1);
  Expect(viewport.cursor_line() == 1,
         "page up should reverse the same visible-row step back onto the collapsed opener row");

  viewport.Page(-1);
  Expect(viewport.cursor_line() == 0,
         "page up should reverse the same visible-row count across a collapsed fold");
}

void TestTextViewportCollapsedFoldHitTestingUsesVisibleRows() {
  TextViewport viewport;
  viewport.LoadContent("void f() {\n  a();\n  b();\n}\nafter();\n", "/tmp/fold-hit.cpp");
  FoldingModel folding_model;
  Expect(folding_model.Compute(viewport.lines(), DefaultFoldOptions()),
         "fold compute should complete for hit-testing fixture");
  Expect(folding_model.Collapse(0), "outer fold should collapse");
  viewport.SetFoldingModel(&folding_model);

  const TextPosition hit = viewport.LogicalPositionForVisualHit(1, 2);
  Expect(hit.line == 4,
         "visual hit-testing should map rows after a collapsed fold to the next visible line");
}

void TestTextViewportReplaceLinesAppendMovesCursorToInsertedBlock() {
  TextViewport viewport;
  viewport.LoadContent("alpha\nbeta", "/tmp/replace-lines.txt");

  Expect(viewport.ReplaceLines(viewport.line_count(), viewport.line_count(), {"tail"}),
         "replace lines should allow appending at the end of the buffer");
  Expect(viewport.line_count() == 3 && viewport.lines()[2] == "tail",
         "replace lines append should add the replacement line at the end");
  Expect(viewport.cursor_line() == 2 && viewport.cursor_column() == 0,
         "replace lines append should move the cursor to the inserted block");

  Expect(viewport.Undo(), "undo should succeed after an appended line replacement");
  Expect(viewport.line_count() == 2 && viewport.lines()[1] == "beta",
         "undo should remove the appended replacement block");
}

void TestTextViewportMaxVisualColumnsUpdatesIncrementally() {
  TextViewport viewport;
  viewport.LoadContent("short\nvery very long line\nmid\n", "/tmp/max-columns.txt");
  const std::size_t initial_max = viewport.max_visual_columns();

  viewport.MoveCursorTo(0, viewport.lines()[0].size());
  viewport.InsertText("!");
  Expect(viewport.max_visual_columns() == initial_max,
         "editing a non-maximum line should keep the cached maximum width");

  viewport.MoveCursorTo(1, 0);
  viewport.ReplaceRange({{1, 0}, {1, viewport.lines()[1].size()}}, "tiny");
  Expect(viewport.max_visual_columns() == 6,
         "shrinking the former widest line should recompute the new maximum width");
}

void TestTextViewportReplaceAllUndoRedoHandlesLargeSparseDocument() {
  TextViewport viewport;
  std::string content;
  content.reserve(50000 * 16);
  for (int i = 0; i < 50000; ++i) {
    if (!content.empty()) {
      content.push_back('\n');
    }
    content += "row ";
    content += std::to_string(i);
    if (i % 10000 == 0) {
      content += " target";
    }
  }
  viewport.LoadContent(content, "/tmp/replace-all-large.txt");

  const std::size_t replaced = viewport.ReplaceAll("TARGET", "hit");
  Expect(replaced == 5, "replace-all should update sparse case-insensitive matches");
  Expect(viewport.lines()[0].find("hit") != std::string::npos &&
             viewport.lines()[10000].find("hit") != std::string::npos &&
             viewport.lines()[20000].find("hit") != std::string::npos &&
             viewport.lines()[30000].find("hit") != std::string::npos &&
             viewport.lines()[40000].find("hit") != std::string::npos,
         "replace-all should rewrite each sparse matching line");

  Expect(viewport.Undo(), "undo should succeed after replace-all on a large sparse document");
  Expect(viewport.lines()[0].find("target") != std::string::npos &&
             viewport.lines()[10000].find("target") != std::string::npos &&
             viewport.lines()[20000].find("target") != std::string::npos &&
             viewport.lines()[30000].find("target") != std::string::npos &&
             viewport.lines()[40000].find("target") != std::string::npos,
         "undo should restore sparse replace-all lines");

  Expect(viewport.Redo(), "redo should succeed after undoing sparse replace-all");
  Expect(viewport.lines()[0].find("hit") != std::string::npos &&
             viewport.lines()[10000].find("hit") != std::string::npos &&
             viewport.lines()[20000].find("hit") != std::string::npos &&
             viewport.lines()[30000].find("hit") != std::string::npos &&
             viewport.lines()[40000].find("hit") != std::string::npos,
         "redo should reapply sparse replace-all changes");
}

void TestRuntimeSyntaxDetectFiletypeDisambiguatesCppHeader() {
  const std::vector<std::string> lines = {
      "#pragma once",
      "namespace demo {",
      "class Widget {",
      "public:",
      "  Widget() = default;",
      "};",
      "}",
  };

  Expect(microide::editor::runtime_syntax::DetectFiletype("/tmp/widget.h", lines) == "c++",
         "C++ headers should still resolve to the C++ syntax definition");
}

void TestRuntimeSyntaxDetectFiletypeDisambiguatesObjectiveCSource() {
  const std::vector<std::string> lines = {
      "#import <Foundation/Foundation.h>",
      "@interface Widget : NSObject",
      "@end",
  };

  Expect(microide::editor::runtime_syntax::DetectFiletype("/tmp/widget.m", lines) == "objective-c",
         "Objective-C source files should still resolve to the Objective-C syntax definition");
}

void TestRuntimeSyntaxDetectFiletypeKeepsCMakeLists() {
  const std::vector<std::string> lines = {
      "cmake_minimum_required(VERSION 3.25)",
      "project(microide)",
  };

  Expect(microide::editor::runtime_syntax::DetectFiletype("/tmp/CMakeLists.txt", lines) == "cmake",
         "CMakeLists.txt should still resolve to the CMake syntax definition");
}

void TestTextViewportLoadsRuntimeSyntaxDefinitionsFromPluginDataDirectories() {
#if !MICROIDE_HAS_LUA_PLUGINS
  return;
#endif
  ScopedRuntimeSyntaxRegistryReset syntax_reset;
  TemporaryDirectory temp_dir;
  const std::filesystem::path syntax_dir = temp_dir.path() / "syntax";
  WriteFile(
      syntax_dir / "todo.lua",
      R"(return {
  filetype = "todo",
  files = { "\\.todo$" },
  rules = {
    { pattern = "\\b(TODO|DONE)\\b", group = "keyword" },
    {
      start = "\"",
      ["end"] = "\"",
      skip = "\\\\.",
      group = "string",
      rules = {
        { pattern = "\\\\.", group = "string" }
      }
    }
  }
}
)");

  std::vector<std::string> loader_errors;
  const auto definitions =
      microide::editor::runtime_syntax::LoadDefinitionsFromDirectories({syntax_dir}, &loader_errors);
  Expect(loader_errors.empty(), "runtime syntax loader should accept valid plugin syntax data");

  std::vector<std::string> reload_errors;
  const auto reload_result =
      microide::editor::runtime_syntax::ReloadDefinitions(definitions, &reload_errors);
  Expect(reload_result.plugin_definition_count == 1,
         "runtime syntax reload should register one plugin definition");
  Expect(reload_errors.empty(), "runtime syntax reload should accept valid plugin syntax regexes");

  TextViewport viewport;
  viewport.LoadContent("TODO \"value\"\n", "/tmp/items.todo");

  const auto& tokens = viewport.HighlightedLineTokens(0);
  Expect(tokens.size() == viewport.lines().front().size(),
         "runtime syntax highlighting should still return one token per byte");
  Expect(std::any_of(tokens.begin(), tokens.begin() + 4,
                     [](SyntaxTokenKind kind) { return kind == SyntaxTokenKind::Keyword; }),
         "plugin filename syntax definitions should highlight matched keywords");
  Expect(std::any_of(tokens.begin() + 5, tokens.end(),
                     [](SyntaxTokenKind kind) { return kind == SyntaxTokenKind::String; }),
         "plugin region syntax definitions should highlight string spans");
}

void TestTextViewportOccurrenceSeedSpanUsesWordUnderCaret() {
  TextViewport viewport;
  viewport.LoadContent("aaa name bbb name\n", "/tmp/oc-caret.txt");
  viewport.MoveCursorTo(0, 5, false);
  const auto seed = viewport.OccurrenceSeedSpanForHighlight();
  Expect(seed.has_value(), "caret inside a word should produce an occurrence seed span");
  Expect(seed->start.line == 0 && seed->end.line == 0,
         "occurrence seed should stay on a single logical line");
  Expect(seed->start.column == 4 && seed->end.column == 8,
         "occurrence seed should cover the word under the caret");
}

void TestTextViewportOccurrenceSeedSpanUsesTrailingEdgeAdjacentWord() {
  TextViewport viewport;
  viewport.LoadContent("word", "/tmp/oc-adjacent.txt");
  viewport.MoveCursorTo(0, 4, false);
  const auto seed = viewport.OccurrenceSeedSpanForHighlight();
  Expect(seed.has_value(), "caret after the final word character should still seed that word");
  Expect(seed->start.column == 0 && seed->end.column == 4,
         "trailing-edge caret should expand to the full prior word");
}

void TestTextViewportOccurrenceSeedSpanHonorsSingleLineSelection() {
  TextViewport viewport;
  viewport.LoadContent("foo bar baz\n", "/tmp/oc-sel.txt");
  viewport.MoveCursorTo(0, 4, false);
  viewport.MoveCursorTo(0, 7, true);
  const auto seed = viewport.OccurrenceSeedSpanForHighlight();
  Expect(seed.has_value(), "non-empty selection should supply the occurrence seed");
  Expect(seed->start.column == 4 && seed->end.column == 7,
         "selection bounds should seed literal substring occurrences");
}

void TestTextViewportOccurrenceSeedSpanRejectsMultiLineSelection() {
  TextViewport viewport;
  viewport.LoadContent("a\nb\n", "/tmp/oc-multi.txt");
  viewport.MoveCursorTo(0, 0, false);
  viewport.MoveCursorTo(1, 1, true);
  Expect(!viewport.OccurrenceSeedSpanForHighlight().has_value(),
         "multi-line selections should not drive occurrence seeding");
}

void TestTextViewportOccurrenceSeedSpanNoWordInWhitespace() {
  TextViewport viewport;
  viewport.LoadContent("x  y\n", "/tmp/oc-space.txt");
  viewport.MoveCursorTo(0, 2, false);
  Expect(!viewport.OccurrenceSeedSpanForHighlight().has_value(),
         "caret positioned in whitespace should not yield a seed span");
}

}  // namespace

void RegisterTextViewportTests(std::vector<TestCase>& tests) {
  AddTest(tests, "TextViewport/SmallFileKeepsSyntaxHighlighting",
          TestTextViewportSmallFileKeepsSyntaxHighlighting);
  AddTest(tests, "TextViewport/LargeCodeFixtureKeepsSyntaxHighlighting",
          TestTextViewportLargeCodeFixtureKeepsSyntaxHighlighting);
  AddTest(tests, "TextViewport/LargePlainFixtureKeepsSyntaxHighlighting",
          TestTextViewportLargePlainFixtureKeepsSyntaxHighlighting);
  AddTest(tests, "TextViewport/EditingPastFormerLargeFileLineThresholdKeepsSyntaxHighlighting",
          TestTextViewportEditingPastFormerLargeFileLineThresholdKeepsSyntaxHighlighting);
  AddTest(tests, "TextViewport/EditingPastFormerLargeFileByteThresholdKeepsSyntaxHighlighting",
          TestTextViewportEditingPastFormerLargeFileByteThresholdKeepsSyntaxHighlighting);
  AddTest(tests, "TextViewport/CacheStatsTrackWarmLayoutAndHighlightHits",
          TestTextViewportCacheStatsTrackWarmLayoutAndHighlightHits);
  AddTest(tests, "TextViewport/CaretMovementKeepsVisibleLineLayoutCached",
          TestTextViewportCaretMovementKeepsVisibleLineLayoutCached);
  AddTest(tests, "TextViewport/HighlightCheckpointsBoundFarReplay",
          TestTextViewportHighlightCheckpointsBoundFarReplay);
  AddTest(tests, "TextViewport/HighlightCheckpointsPreserveMultilineState",
          TestTextViewportHighlightCheckpointsPreserveMultilineState);
  AddTest(tests, "TextViewport/EditingNearTailDoesNotRebuildFarCheckpoints",
          TestTextViewportEditingNearTailDoesNotRebuildFarCheckpoints);
  AddTest(tests, "TextViewport/InsertNewlineCopiesLeadingIndentation",
          TestTextViewportInsertNewlineCopiesLeadingIndentation);
  AddTest(tests, "TextViewport/InsertNewlineOnWhitespaceOnlyLineDoesNotCarryIndentForward",
          TestTextViewportInsertNewlineOnWhitespaceOnlyLineDoesNotCarryIndentForward);
  AddTest(tests, "TextViewport/MultiCaretNewlineCopiesIndentationPerCaret",
          TestTextViewportMultiCaretNewlineCopiesIndentationPerCaret);
  AddTest(tests, "TextViewport/LastAppliedEditTracksInsertUndoRedo",
          TestTextViewportLastAppliedEditTracksInsertUndoRedo);
  AddTest(tests, "TextViewport/LastAppliedEditTracksMultilineReplacement",
          TestTextViewportLastAppliedEditTracksMultilineReplacement);
  AddTest(tests, "TextViewport/UndoRedoPreservesLatestViewState",
          TestTextViewportUndoRedoPreservesLatestViewState);
  AddTest(tests, "TextViewport/UndoRedoPreservesSecondaryCarets",
          TestTextViewportUndoRedoPreservesSecondaryCarets);
  AddTest(tests, "TextViewport/MultiCaretInsertAndUndoAreAtomic",
          TestTextViewportMultiCaretInsertAndUndoAreAtomic);
  AddTest(tests, "TextViewport/MultiCaretBackspaceAndDeleteForward",
          TestTextViewportMultiCaretBackspaceAndDeleteForward);
  AddTest(tests, "TextViewport/MultiCaretDeleteCurrentLineIsAtomic",
          TestTextViewportMultiCaretDeleteCurrentLineIsAtomic);
  AddTest(tests, "TextViewport/SelectAllCopiesLargeDocumentRoundTrip",
          TestTextViewportSelectAllCopiesLargeDocumentRoundTrip);
  AddTest(tests, "TextViewport/SoftWrapExposesVisualRowsAndWrappedCaret",
          TestTextViewportSoftWrapExposesVisualRowsAndWrappedCaret);
  AddTest(tests, "TextViewport/SoftWrapMoveCursorVerticalUsesWrappedRows",
          TestTextViewportSoftWrapMoveCursorVerticalUsesWrappedRows);
  AddTest(tests, "TextViewport/SoftWrapPageMovesByVisibleRows",
          TestTextViewportSoftWrapPageMovesByVisibleRows);
  AddTest(tests, "TextViewport/SoftWrapMovesSecondaryCaretsByVisibleRows",
          TestTextViewportSoftWrapMovesSecondaryCaretsByVisibleRows);
  AddTest(tests, "TextViewport/SoftWrapVisualHitRoundTripsContinuationRows",
          TestTextViewportSoftWrapVisualHitRoundTripsContinuationRows);
  AddTest(tests, "TextViewport/SoftWrapContinuationHitsUseVisualOffset",
          TestTextViewportSoftWrapContinuationHitsUseVisualOffset);
#ifndef NDEBUG
  AddTest(tests, "TextViewport/SoftWrapViewportResizeRebuildsWrapCacheLazily",
          TestTextViewportSoftWrapViewportResizeRebuildsWrapCacheLazily);
#endif
  AddTest(tests, "TextViewport/SoftWrapForcesHorizontalScrollToZero",
          TestTextViewportSoftWrapForcesHorizontalScrollToZero);
  AddTest(tests, "TextViewport/CollapsedFoldHidesBodyRows",
          TestTextViewportCollapsedFoldHidesBodyRows);
  AddTest(tests, "TextViewport/CollapsedFoldVerticalMotionSkipsHiddenLines",
          TestTextViewportCollapsedFoldVerticalMotionSkipsHiddenLines);
  AddTest(tests, "TextViewport/CollapsedFoldPageMovesByVisibleRows",
          TestTextViewportCollapsedFoldPageMovesByVisibleRows);
  AddTest(tests, "TextViewport/CollapsedFoldHitTestingUsesVisibleRows",
          TestTextViewportCollapsedFoldHitTestingUsesVisibleRows);
  AddTest(tests, "TextViewport/ReplaceLinesAppendMovesCursorToInsertedBlock",
          TestTextViewportReplaceLinesAppendMovesCursorToInsertedBlock);
  AddTest(tests, "TextViewport/MaxVisualColumnsUpdatesIncrementally",
          TestTextViewportMaxVisualColumnsUpdatesIncrementally);
  AddTest(tests, "TextViewport/OccurrenceSeedSpanUsesWordUnderCaret",
          TestTextViewportOccurrenceSeedSpanUsesWordUnderCaret);
  AddTest(tests, "TextViewport/OccurrenceSeedSpanUsesTrailingEdgeAdjacentWord",
          TestTextViewportOccurrenceSeedSpanUsesTrailingEdgeAdjacentWord);
  AddTest(tests, "TextViewport/OccurrenceSeedSpanHonorsSingleLineSelection",
          TestTextViewportOccurrenceSeedSpanHonorsSingleLineSelection);
  AddTest(tests, "TextViewport/OccurrenceSeedSpanRejectsMultiLineSelection",
          TestTextViewportOccurrenceSeedSpanRejectsMultiLineSelection);
  AddTest(tests, "TextViewport/OccurrenceSeedSpanNoWordInWhitespace",
          TestTextViewportOccurrenceSeedSpanNoWordInWhitespace);
  AddTest(tests, "TextViewport/ReplaceAllUndoRedoHandlesLargeSparseDocument",
          TestTextViewportReplaceAllUndoRedoHandlesLargeSparseDocument);
  AddTest(tests, "TextViewport/RuntimeSyntaxDetectFiletypeDisambiguatesCppHeader",
          TestRuntimeSyntaxDetectFiletypeDisambiguatesCppHeader);
  AddTest(tests, "TextViewport/RuntimeSyntaxDetectFiletypeDisambiguatesObjectiveCSource",
          TestRuntimeSyntaxDetectFiletypeDisambiguatesObjectiveCSource);
  AddTest(tests, "TextViewport/RuntimeSyntaxDetectFiletypeKeepsCMakeLists",
          TestRuntimeSyntaxDetectFiletypeKeepsCMakeLists);
  AddTest(tests, "TextViewport/LoadsRuntimeSyntaxDefinitionsFromPluginDataDirectories",
          TestTextViewportLoadsRuntimeSyntaxDefinitionsFromPluginDataDirectories);
}

}  // namespace microide::tests
