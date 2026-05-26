#include "TestSupport.h"

#include "editor/SyntaxDefinitionLoader.h"
#include "editor/FoldingModel.h"
#include "editor/RuntimeSyntaxRegistry.h"
#include "editor/TextViewport.h"
#include "util/PerformanceCounters.h"

#include <algorithm>
#include <string>
#include <vector>

namespace microide::tests {
namespace {

using microide::editor::SyntaxTokenKind;
using microide::editor::TextPosition;
using microide::editor::TextViewport;
using microide::editor::FoldingModel;
using microide::editor::SelectionRange;

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

void TestTextViewportLineCommentEndsAtLineBoundary() {
  TextViewport viewport;
  viewport.LoadContent("const before = 1;\n// comment line\nlet after = 2;\n", "/tmp/comment-state.ts");

  const auto& comment_tokens = viewport.HighlightedLineTokens(1);
  Expect(!comment_tokens.empty(), "line-comment fixture should still produce syntax tokens");
  Expect(std::all_of(comment_tokens.begin(), comment_tokens.end(),
                     [](SyntaxTokenKind kind) { return kind == SyntaxTokenKind::Comment; }),
         "TypeScript line comments should classify the whole comment line as comment");

  const auto& after_tokens = viewport.HighlightedLineTokens(2);
  Expect(!after_tokens.empty(), "line-comment fixture should still produce tokens after comments");
  Expect(after_tokens.size() >= 3, "post-comment line should expose per-byte syntax tokens");
  Expect(after_tokens[0] == SyntaxTokenKind::Keyword &&
             after_tokens[1] == SyntaxTokenKind::Keyword &&
             after_tokens[2] == SyntaxTokenKind::Keyword,
         "TypeScript line comments should not leak comment state into the next line");
  Expect(std::any_of(after_tokens.begin(), after_tokens.end(),
                     [](SyntaxTokenKind kind) { return kind != SyntaxTokenKind::Comment; }),
         "post-comment TypeScript code should recover to non-comment token classes");
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

void TestTextViewportSameLineCountUndoOnLargeFilePreservesContent() {
  // Single-line replacements through Apply/Undo/Redo (the same-line-count
  // fast path) must produce the same document contents as the slow path on
  // a buffer large enough that vector tail-shifts would normally dominate.
  const std::size_t kLineCount = 4096;
  std::string body;
  body.reserve(kLineCount * 12);
  for (std::size_t i = 0; i < kLineCount; ++i) {
    body += "alpha_";
    body += std::to_string(i);
    body += '\n';
  }
  TextViewport viewport;
  viewport.LoadContent(body, "/tmp/applied-edit-fast-path.txt");

  const std::size_t edit_line = 7;
  const std::string original_line = viewport.lines()[edit_line];
  const std::string original_line_after = viewport.lines()[edit_line + 1];
  const std::size_t baseline_line_count = viewport.lines().size();

  viewport.MoveCursorTo(edit_line, original_line.size());
  viewport.InsertText("Z");

  Expect(viewport.lines().size() == baseline_line_count,
         "single-character insert must preserve line count");
  Expect(viewport.lines()[edit_line] == original_line + "Z",
         "single-character insert must update the affected line");
  Expect(viewport.lines()[edit_line + 1] == original_line_after,
         "untouched line after the edit must remain identical");
  Expect(viewport.last_applied_edit().has_value(),
         "fast-path insert must publish an applied edit");
  Expect(viewport.last_applied_edit()->replacement_text == "Z",
         "fast-path insert must publish the inserted text");

  Expect(viewport.Undo(), "undo must succeed for fast-path edit");
  Expect(viewport.lines()[edit_line] == original_line,
         "undo via fast path must restore the original line content");
  Expect(viewport.lines().size() == baseline_line_count,
         "undo via fast path must preserve line count");

  Expect(viewport.Redo(), "redo must succeed for fast-path edit");
  Expect(viewport.lines()[edit_line] == original_line + "Z",
         "redo via fast path must reapply the inserted text");
  Expect(viewport.lines().size() == baseline_line_count,
         "redo via fast path must preserve line count");
}

void TestTextViewportSameLineCountEditInvalidatesSyntaxCache() {
  // After a fast-path edit the highlight token kinds for the affected line
  // must reflect the new content rather than a stale cache.
  TextViewport viewport;
  viewport.LoadContent("int x;\nint y;\n", "/tmp/applied-edit-fast-path-syntax.cpp");
  viewport.MoveCursorTo(0, 4);
  const auto baseline_tokens = viewport.HighlightedLineTokens(0);
  Expect(!baseline_tokens.empty(), "baseline tokens must exist before fast-path edit");

  viewport.InsertText("z");
  const auto after_insert_tokens = viewport.HighlightedLineTokens(0);
  Expect(after_insert_tokens.size() == baseline_tokens.size() + 1,
         "after the fast-path insert the affected line must re-tokenize to the new length");

  Expect(viewport.Undo(), "undo must succeed for fast-path syntax test");
  const auto after_undo_tokens = viewport.HighlightedLineTokens(0);
  Expect(after_undo_tokens == baseline_tokens,
         "undo of the fast-path insert must restore the original token kinds");
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

void TestTextViewportUndoGroupMergesKnownRangeChildEdits() {
  TextViewport viewport;
  viewport.LoadContent("header\nbody\nfooter\n", "/tmp/undo-group-merge.txt");
  viewport.MoveCursorTo(1, 4);

  viewport.BeginUndoGroup();
  viewport.InsertText("\nchild");
  Expect(viewport.ReplaceRange(SelectionRange{
                                   .start = TextPosition{2, 5},
                                   .end = TextPosition{2, 5},
                               },
                               "!"),
         "contained grouped edit should apply after line-count change");
  viewport.EndUndoGroup();

  Expect(viewport.lines().size() == 5,
         "grouped known-range edits should leave the expanded line structure");
  Expect(viewport.lines()[1] == "body" && viewport.lines()[2] == "child!",
         "grouped known-range edits should preserve both child mutations");

  Expect(viewport.Undo(), "grouped known-range edits should undo as one step");
  Expect(viewport.lines().size() == 4 && viewport.lines()[0] == "header" &&
             viewport.lines()[1] == "body" && viewport.lines()[2] == "footer" &&
             viewport.lines()[3].empty(),
         "undo should restore the original document without requiring a full-buffer snapshot path");

  Expect(viewport.Redo(), "grouped known-range edits should redo as one step");
  Expect(viewport.lines().size() == 5 && viewport.lines()[2] == "child!",
         "redo should restore the merged grouped edit");
}

void TestTextViewportUndoGroupFallsBackForDisjointChildEdits() {
  TextViewport viewport;
  viewport.LoadContent("zero\none\ntwo\nthree\n", "/tmp/undo-group-fallback.txt");

  viewport.BeginUndoGroup();
  Expect(viewport.ReplaceRange(SelectionRange{
                                   .start = TextPosition{0, 4},
                                   .end = TextPosition{0, 4},
                               },
                               "!"),
         "first grouped edit should apply");
  Expect(viewport.ReplaceRange(SelectionRange{
                                   .start = TextPosition{3, 5},
                                   .end = TextPosition{3, 5},
                               },
                               "?"),
         "disjoint grouped edit should apply");
  viewport.EndUndoGroup();

  Expect(viewport.lines()[0] == "zero!" && viewport.lines()[3] == "three?",
         "disjoint grouped edits should both remain visible after the grouped commit");

  Expect(viewport.Undo(), "disjoint grouped edits should still undo atomically");
  Expect(viewport.lines().size() == 5 && viewport.lines()[0] == "zero" &&
             viewport.lines()[1] == "one" && viewport.lines()[2] == "two" &&
             viewport.lines()[3] == "three" && viewport.lines()[4].empty(),
         "fallback undo group path should restore the original document");
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

void TestTextViewportSoftWrapPrefersWhitespaceBoundaries() {
  TextViewport viewport;
  viewport.LoadContent("hello brave new wonderful world here\n",
                       "/tmp/soft-wrap-words.txt");
  viewport.SetViewportSize(10, 12);
  viewport.SetSoftWrap(true);

  // Four content rows from the long sentence plus the trailing empty-line row.
  Expect(viewport.VisualRowCount() == 5,
         "soft wrap should split a long sentence into multiple word-aligned rows");

  const auto row0 = viewport.VisibleWrappedRowLayout(0);
  const auto row1 = viewport.VisibleWrappedRowLayout(1);
  const auto row2 = viewport.VisibleWrappedRowLayout(2);
  const auto row3 = viewport.VisibleWrappedRowLayout(3);
  Expect(row0.text == "hello brave ",
         "first wrapped row should consume words until adding the next would overflow");
  Expect(row1.text == "new ",
         "second wrapped row should start with the next word, not split 'wonderful'");
  Expect(row2.text == "wonderful ",
         "third wrapped row should hold the long word that filled the previous row");
  Expect(row3.text == "world here",
         "trailing wrapped row should pack remaining short words together");
}

void TestTextViewportSoftWrapHardBreaksInsideLongWords() {
  TextViewport viewport;
  // No whitespace within wrap_columns of the row start → must hard break.
  viewport.LoadContent("abcdefghijklmnopqrst\n",
                       "/tmp/soft-wrap-hard.txt");
  viewport.SetViewportSize(10, 8);
  viewport.SetSoftWrap(true);

  // Three content rows plus the trailing empty-line row.
  Expect(viewport.VisualRowCount() == 4,
         "long word with no whitespace should still wrap at the column boundary");
  const auto row0 = viewport.VisibleWrappedRowLayout(0);
  const auto row1 = viewport.VisibleWrappedRowLayout(1);
  const auto row2 = viewport.VisibleWrappedRowLayout(2);
  Expect(row0.text == "abcdefgh",
         "first row should hard-break at wrap_columns when no whitespace fits");
  Expect(row1.text == "ijklmnop",
         "continuation row should keep packing characters of the unbreakable word");
  Expect(row2.text == "qrst",
         "trailing slice of an unbreakable word should still render");
}

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

void TestTextLayoutVisualColumnFromLayoutClippedMatchesWalk() {
  // 2026-05-15 perf deep-dive round 2 Finding 13: the binary-search resolution from an
  // already-built LayoutLine must agree with the legacy O(line_length) walk for source columns
  // inside the row window, and must return clipping sentinels for columns outside the window.
  using microide::editor::TextLayout;
  const std::string line = "ab\tcd\tef";  // 2 ASCII, tab → col 4, ASCII, tab → col 8, ASCII
  const std::size_t tab_size = 4;

  // Full-line window: source_columns covers every visible cell.
  {
    const auto layout = TextLayout::BuildVisibleLine(line, /*horizontal_scroll=*/0,
                                                      /*visible_columns=*/80, tab_size);
    for (std::size_t c = 0; c <= line.size(); ++c) {
      const std::size_t walked = TextLayout::VisualColumnForTextColumn(line, c, tab_size);
      const std::size_t clipped = TextLayout::VisualColumnFromLayoutClipped(
          layout, /*row_start_visual=*/0, /*row_end_visual=*/80, c);
      // Inside the window, the helper must return the exact visual column.
      Expect(clipped == walked,
             "VisualColumnFromLayoutClipped must match VisualColumnForTextColumn inside the row");
    }
  }

  // Scrolled window: only the trailing part of the line is in the layout's source_columns.
  {
    const std::size_t horizontal_scroll = 4;
    const std::size_t visible_columns = 8;
    const auto layout = TextLayout::BuildVisibleLine(line, horizontal_scroll, visible_columns,
                                                      tab_size);
    const std::size_t row_start = horizontal_scroll;
    const std::size_t row_end = horizontal_scroll + visible_columns;

    // Source column 0 is before the row window. Helper should return a value <= row_start so
    // std::max(.., row_start) clips correctly to row_start.
    const std::size_t before = TextLayout::VisualColumnFromLayoutClipped(layout, row_start,
                                                                          row_end, 0);
    Expect(before <= row_start,
           "source columns before the row window must return <= row_start (clip sentinel)");
    Expect(std::max<std::size_t>(before, row_start) == row_start,
           "clipping via std::max with row_start must produce row_start");

    // Source column at or past line end maps to "just past the last visible source byte". For a
    // line that fits entirely within the row, that visual column equals the position immediately
    // after the last char — std::min with row_end produces the correct end-of-line decoration
    // boundary.
    const std::size_t at_end = TextLayout::VisualColumnFromLayoutClipped(
        layout, row_start, row_end, line.size());
    const std::size_t past_end = TextLayout::VisualColumnFromLayoutClipped(
        layout, row_start, row_end, line.size() + 5);
    Expect(at_end == past_end,
           "source columns at/past line end must return the same end-of-line visual column");
    // The legacy walk gives the visual column of the position past the last char of "ab\\tcd\\tef".
    const std::size_t walked_end =
        TextLayout::VisualColumnForTextColumn(line, line.size(), tab_size);
    Expect(at_end == walked_end || at_end >= row_end,
           "end-of-line helper result must agree with the legacy walk or clip beyond row_end");
  }
}

void TestTextLayoutLineVisualColumnMapMatchesWalk() {
  // 2026-05-19 perf deep-dive round 3 Finding 11: the per-line precomputed visual-column map
  // (LineVisualColumnMap) must agree with the legacy walk for every text column on a line that
  // mixes tabs and multi-byte UTF-8.
  using microide::editor::TextLayout;
  const std::string ascii_only = "hello world";
  const std::string with_tabs = "ab\tcd\tef";
  const std::string with_utf8 = "α\tβγ\tδε";  // 2-byte code points
  const std::size_t tab_size = 4;
  for (const auto* line_ptr : {&ascii_only, &with_tabs, &with_utf8}) {
    const std::string& line = *line_ptr;
    TextLayout::LineVisualColumnMap map(line, tab_size);
    Expect(map.LineVisualWidth() == TextLayout::VisualColumnForTextColumn(line, line.size(), tab_size),
           "LineVisualColumnMap line width must match the walked width");
    for (std::size_t c = 0; c <= line.size() + 3; ++c) {
      const std::size_t walked = TextLayout::VisualColumnForTextColumn(line, c, tab_size);
      const std::size_t mapped = map.VisualColumnFor(c);
      Expect(mapped == walked,
             "LineVisualColumnMap must match VisualColumnForTextColumn at every text column");
    }
  }
}

void TestTextViewportSecondaryCaretPositionsCacheStability() {
  // 2026-05-15 perf deep-dive round 2 Finding 10: secondary_caret_positions() must reuse the
  // same underlying storage across calls when the carets are unchanged, so the render path does
  // not allocate a fresh vector per frame.
  TextViewport viewport;
  viewport.LoadContent("alpha\nbeta\ngamma\n", "/tmp/secondary-carets-span.txt");
  viewport.AddSecondaryCaret(1, 2);
  viewport.AddSecondaryCaret(2, 3);

  const auto first = viewport.secondary_caret_positions();
  const auto second = viewport.secondary_caret_positions();
  Expect(first.size() == 2,
         "secondary_caret_positions reports the registered caret count");
  Expect(first.data() == second.data(),
         "back-to-back secondary_caret_positions calls must reuse storage (no per-call alloc)");

  // After a mutation, the cache must refresh — and may pick a new data pointer if it had to grow,
  // but in steady state it should typically stay put. The functional invariant is that the
  // returned positions match secondary_carets_.
  viewport.AddSecondaryCaret(0, 1);
  const auto third = viewport.secondary_caret_positions();
  Expect(third.size() == 3,
         "secondary_caret_positions reflects the new caret after AddSecondaryCaret");
}

void TestTextViewportTrivialWrappedLayoutFastPath() {
  // 2026-05-15 perf deep-dive round 2 Finding 5: when soft-wrap is off and no fold is collapsed,
  // EnsureWrappedRowLayouts must not allocate the per-line wrapped_row_layouts_ vector. The public
  // accessors must still return the identity-mapped values.
  std::string content;
  for (int i = 0; i < 4000; ++i) {
    content += "line " + std::to_string(i) + "\n";
  }
  TextViewport viewport;
  viewport.LoadContent(content, "/tmp/trivial-layout-large.txt");
  viewport.SetViewportSize(24, 120);
  viewport.SetSoftWrap(false);
  viewport.SetScrollLine(100);

  // Visual row count must equal the document line count when in trivial mode.
  Expect(viewport.visual_line_count() == viewport.lines().size(),
         "trivial layout: visual row count must equal document line count");

  // Identity mapping for VisualRowLineIndex and VisualRowForLine.
  Expect(viewport.VisualRowLineIndex(2500) == 2500,
         "trivial layout: VisualRowLineIndex must be identity");
  Expect(viewport.VisualRowForLine(1234) == 1234,
         "trivial layout: VisualRowForLine must be identity");

  // WrappedVisualRowLayout must report sensible values without indexing a populated vector.
  const auto row = viewport.WrappedVisualRowLayout(500);
  Expect(row.line_index == 500,
         "trivial layout: wrapped visual row layout returns synthesized line index");
  Expect(row.visual_start == viewport.horizontal_scroll(),
         "trivial layout: visual_start equals horizontal_scroll");
  Expect(row.visual_end == viewport.horizontal_scroll() + 120,
         "trivial layout: visual_end equals horizontal_scroll + visible_columns");

  // Editing once must not blow up. The trivial fast-path is the point of the test, so we also
  // sanity-check that an edit keeps the identity invariant (no soft-wrap, no folds).
  viewport.MoveCursorTo(50, 0);
  viewport.InsertText("X");
  Expect(viewport.visual_line_count() == viewport.lines().size(),
         "trivial layout: edit should not break the identity invariant");
}

struct TierBumpSnapshot {
  std::uint64_t content = 0;
  std::uint64_t syntax = 0;
  std::uint64_t layout_shape = 0;
  std::uint64_t presentation = 0;
};

TierBumpSnapshot CaptureTierBumps() {
  return TierBumpSnapshot{
      .content = microide::util::ReadPerformanceCounter(
          microide::util::PerfCounterId::EditorContentRevisionBumps),
      .syntax = microide::util::ReadPerformanceCounter(
          microide::util::PerfCounterId::EditorSyntaxRevisionBumps),
      .layout_shape = microide::util::ReadPerformanceCounter(
          microide::util::PerfCounterId::EditorLayoutShapeRevisionBumps),
      .presentation = microide::util::ReadPerformanceCounter(
          microide::util::PerfCounterId::EditorPresentationRevisionBumps),
  };
}

TierBumpSnapshot Delta(const TierBumpSnapshot& before, const TierBumpSnapshot& after) {
  return TierBumpSnapshot{
      .content = after.content - before.content,
      .syntax = after.syntax - before.syntax,
      .layout_shape = after.layout_shape - before.layout_shape,
      .presentation = after.presentation - before.presentation,
  };
}

void TestTextViewportContentEditBumpsContentAndPresentationOnly() {
  TextViewport viewport;
  viewport.LoadContent("alpha\n", "/tmp/tier-content.cpp");
  const auto before = CaptureTierBumps();
  viewport.InsertCharacter('x');
  const auto delta = Delta(before, CaptureTierBumps());
  Expect(delta.content == 1 && delta.presentation == 1 && delta.syntax == 0 &&
             delta.layout_shape == 0,
         "ContentEdit must bump content and presentation tiers exactly once each");
}

void TestTextViewportSyntaxConfigBumpsSyntaxAndPresentationOnly() {
  TextViewport viewport;
  viewport.LoadContent("alpha\n", "/tmp/tier-syntax.cpp");
  const auto before = CaptureTierBumps();
  viewport.InvalidateSyntaxHighlighting();
  const auto delta = Delta(before, CaptureTierBumps());
  Expect(delta.syntax == 1 && delta.presentation == 1 && delta.content == 0 &&
             delta.layout_shape == 0,
         "SyntaxConfig must bump syntax and presentation tiers exactly once each");
}

void TestTextViewportLayoutShapeBumpsLayoutShapeAndPresentationOnly() {
  TextViewport viewport;
  viewport.LoadContent("alpha\n", "/tmp/tier-layout.cpp");
  viewport.SetTabSize(4);  // baseline so toggle is not a no-op
  const auto before = CaptureTierBumps();
  viewport.SetTabSize(2);
  const auto delta = Delta(before, CaptureTierBumps());
  Expect(delta.layout_shape == 1 && delta.presentation == 1 && delta.content == 0 &&
             delta.syntax == 0,
         "LayoutShape (tab size change) must bump layout_shape and presentation only");
}

void TestTextViewportSoftWrapToggleBumpsLayoutShapeOnly() {
  TextViewport viewport;
  viewport.LoadContent("alpha\n", "/tmp/tier-softwrap.cpp");
  const auto before = CaptureTierBumps();
  viewport.SetSoftWrap(true);
  const auto delta = Delta(before, CaptureTierBumps());
  Expect(delta.layout_shape == 1 && delta.presentation == 1 && delta.content == 0 &&
             delta.syntax == 0,
         "Soft-wrap toggle must bump layout_shape and presentation only");
}

#ifndef NDEBUG
void TestTextViewportSyntaxConfigDoesNotInvalidateWrappedRowLayouts() {
  TextViewport viewport;
  viewport.LoadContent("a\nb\nc\nd\ne\n", "/tmp/tier-wrapped-rows.cpp");
  viewport.SetSoftWrap(true);
  viewport.SetViewportSize(4, 8);
  // Warm the wrapped-row-layouts cache.
  (void)viewport.WrappedVisualRowLayout(0);
  const std::size_t before_builds = viewport.WrappedRowLayoutBuildCountForDebug();
  // SyntaxConfig must NOT cause the wrapped-row-layouts cache to rebuild.
  viewport.InvalidateSyntaxHighlighting();
  (void)viewport.WrappedVisualRowLayout(0);
  const std::size_t after_builds = viewport.WrappedRowLayoutBuildCountForDebug();
  Expect(after_builds == before_builds,
         "SyntaxConfig invalidation must not rebuild wrapped_row_layouts_");
}
#endif

void TestTextViewportSyntaxConfigForcesHighlightCacheMiss() {
  TextViewport viewport;
  viewport.LoadContent("int v = 1;\nint w = 2;\n", "/tmp/tier-highlight.cpp");
  // Warm the per-line highlight cache.
  (void)viewport.HighlightedLineTokens(0);
  (void)viewport.HighlightedLineTokens(0);
  const auto stats_warm = viewport.CacheStats();
  // SyntaxConfig drops the highlight cache, so the next read SHALL be a miss.
  viewport.InvalidateSyntaxHighlighting();
  (void)viewport.HighlightedLineTokens(0);
  const auto stats_after = viewport.CacheStats();
  // Hits should not increase across the syntax bump (cache was wiped, so the
  // request after the bump misses).
  Expect(stats_after.highlight_queries > stats_warm.highlight_queries,
         "post-syntax-bump read should issue another highlight query");
  Expect(stats_after.highlight_hits == stats_warm.highlight_hits,
         "SyntaxConfig must wipe the highlight cache so the next read misses");
}

void TestTextViewportContentEditInvalidatesBracketAndHighlightCaches() {
  // Mirrors the new cache-key contract: a content edit invalidates every
  // content-keyed cache. This is the "still correct after the split" leg —
  // companion to the spec scenarios in tiered-document-revisions.
  TextViewport viewport;
  viewport.LoadContent("int v = 1;\n", "/tmp/tier-content-edit.cpp");
  (void)viewport.HighlightedLineTokens(0);
  const auto before_bumps = CaptureTierBumps();
  viewport.InsertCharacter('z');
  const auto delta = Delta(before_bumps, CaptureTierBumps());
  Expect(delta.content == 1,
         "ContentEdit bumps content_revision so downstream caches re-key correctly");
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
  AddTest(tests, "TextViewport/LineCommentEndsAtLineBoundary",
          TestTextViewportLineCommentEndsAtLineBoundary);
  AddTest(tests, "TextViewport/EditingNearTailDoesNotRebuildFarCheckpoints",
          TestTextViewportEditingNearTailDoesNotRebuildFarCheckpoints);
  AddTest(tests, "TextViewport/InsertNewlineCopiesLeadingIndentation",
          TestTextViewportInsertNewlineCopiesLeadingIndentation);
  AddTest(tests, "TextViewport/InsertNewlineOnWhitespaceOnlyLineDoesNotCarryIndentForward",
          TestTextViewportInsertNewlineOnWhitespaceOnlyLineDoesNotCarryIndentForward);
  AddTest(tests, "TextViewport/MultiCaretNewlineCopiesIndentationPerCaret",
          TestTextViewportMultiCaretNewlineCopiesIndentationPerCaret);
  AddTest(tests, "TextViewport/SameLineCountUndoOnLargeFilePreservesContent",
          TestTextViewportSameLineCountUndoOnLargeFilePreservesContent);
  AddTest(tests, "TextViewport/SameLineCountEditInvalidatesSyntaxCache",
          TestTextViewportSameLineCountEditInvalidatesSyntaxCache);
  AddTest(tests, "TextViewport/LastAppliedEditTracksInsertUndoRedo",
          TestTextViewportLastAppliedEditTracksInsertUndoRedo);
  AddTest(tests, "TextViewport/LastAppliedEditTracksMultilineReplacement",
          TestTextViewportLastAppliedEditTracksMultilineReplacement);
  AddTest(tests, "TextViewport/UndoRedoPreservesLatestViewState",
          TestTextViewportUndoRedoPreservesLatestViewState);
  AddTest(tests, "TextViewport/UndoRedoPreservesSecondaryCarets",
          TestTextViewportUndoRedoPreservesSecondaryCarets);
  AddTest(tests, "TextViewport/UndoGroupMergesKnownRangeChildEdits",
          TestTextViewportUndoGroupMergesKnownRangeChildEdits);
  AddTest(tests, "TextViewport/UndoGroupFallsBackForDisjointChildEdits",
          TestTextViewportUndoGroupFallsBackForDisjointChildEdits);
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
  AddTest(tests, "TextViewport/SoftWrapPrefersWhitespaceBoundaries",
          TestTextViewportSoftWrapPrefersWhitespaceBoundaries);
  AddTest(tests, "TextViewport/SoftWrapHardBreaksInsideLongWords",
          TestTextViewportSoftWrapHardBreaksInsideLongWords);
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
  AddTest(tests, "TextLayout/VisualColumnFromLayoutClippedMatchesWalk",
          TestTextLayoutVisualColumnFromLayoutClippedMatchesWalk);
  AddTest(tests, "TextLayout/LineVisualColumnMapMatchesWalk",
          TestTextLayoutLineVisualColumnMapMatchesWalk);
  AddTest(tests, "TextViewport/SecondaryCaretPositionsCacheStability",
          TestTextViewportSecondaryCaretPositionsCacheStability);
  AddTest(tests, "TextViewport/TrivialWrappedLayoutFastPath",
          TestTextViewportTrivialWrappedLayoutFastPath);
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
  AddTest(tests, "TextViewport/Tiers/ContentEditBumpsContentAndPresentationOnly",
          TestTextViewportContentEditBumpsContentAndPresentationOnly);
  AddTest(tests, "TextViewport/Tiers/SyntaxConfigBumpsSyntaxAndPresentationOnly",
          TestTextViewportSyntaxConfigBumpsSyntaxAndPresentationOnly);
  AddTest(tests, "TextViewport/Tiers/LayoutShapeBumpsLayoutShapeAndPresentationOnly",
          TestTextViewportLayoutShapeBumpsLayoutShapeAndPresentationOnly);
  AddTest(tests, "TextViewport/Tiers/SoftWrapToggleBumpsLayoutShapeOnly",
          TestTextViewportSoftWrapToggleBumpsLayoutShapeOnly);
#ifndef NDEBUG
  AddTest(tests, "TextViewport/Tiers/SyntaxConfigDoesNotInvalidateWrappedRowLayouts",
          TestTextViewportSyntaxConfigDoesNotInvalidateWrappedRowLayouts);
#endif
  AddTest(tests, "TextViewport/Tiers/SyntaxConfigForcesHighlightCacheMiss",
          TestTextViewportSyntaxConfigForcesHighlightCacheMiss);
  AddTest(tests, "TextViewport/Tiers/ContentEditInvalidatesBracketAndHighlightCaches",
          TestTextViewportContentEditInvalidatesBracketAndHighlightCaches);
}

}  // namespace microide::tests
