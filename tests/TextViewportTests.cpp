#include "TestSupport.h"

#include "editor/SyntaxDefinitionLoader.h"
#include "editor/FoldingModel.h"
#include "editor/RuntimeSyntaxRegistry.h"
#include "editor/HighlightPrefetchService.h"
#include "editor/SyntaxHighlighter.h"
#include "editor/TextBuffer.h"
#include "editor/TextViewport.h"
#include "perf/AllocationCounter.h"
#include "util/PerformanceCounters.h"
#include "util/TextFileIO.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <string>
#include <thread>
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

// Regression: a split pane shares its sibling's DocumentState but keeps its own
// highlight caches. When the sibling edits the shared buffer, this pane's stored
// content_revision goes stale; EnsureHighlightCaches must drop the per-line token
// cache on that mismatch (the cache is NOT cursor-gated -- HighlightedLineTokens
// does a bare find()), or the pane keeps painting pre-edit colors until eviction.
void TestTextViewportSplitSiblingEditRefreshesHighlightTokens() {
  TextViewport pane_a;
  pane_a.LoadContent("int a = 1;\nint b = 2;\nint c = 3;\n", "/tmp/split-highlight.cpp");
  (void)pane_a.HighlightedLineTokens(0);
  (void)pane_a.HighlightedLineTokens(1);

  // Split: pane B shares the same DocumentState (shared_ptr) but copies the caches.
  TextViewport pane_b = pane_a;
  const auto before_span = pane_b.HighlightedLineTokens(1);
  const std::vector<SyntaxTokenKind> before(before_span.begin(), before_span.end());
  Expect(std::any_of(before.begin(), before.end(),
                     [](SyntaxTokenKind kind) { return kind != SyntaxTokenKind::Comment; }),
         "line 1 should start as non-comment code");

  // Edit through pane A only: open an unterminated block comment on line 0. This
  // bumps the shared content_revision and purges pane A's caches -- but not pane
  // B's, whose stored revision now trails the document.
  pane_a.MoveCursorTo(0, 0);
  pane_a.InsertText("/*");

  // Pane B repaints and must recompute line 1 as inside the multiline comment,
  // not serve its stale cached tokens.
  const auto& after = pane_b.HighlightedLineTokens(1);
  Expect(!after.empty() &&
             std::all_of(after.begin(), after.end(),
                         [](SyntaxTokenKind kind) { return kind == SyntaxTokenKind::Comment; }),
         "the split sibling pane must recompute tokens after the shared buffer changed");
}

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
  // Two lines: the edit below grows line 0 past the per-line tokenization cutoff,
  // while line 1 stays short. The point of this test is that crossing a byte
  // threshold does not disable highlighting for the DOCUMENT — so line 1 is what
  // carries the assertion once line 0 is individually too long to scan.
  viewport.LoadContent("int value = 42;\nint other = 7;\n", "/tmp/threshold.cpp");

  viewport.MoveCursorTo(0, viewport.lines().front().size());
  viewport.InsertText(std::string(400000, 'a'));

  Expect(viewport.syntax_highlighting_enabled(),
         "editing across the former byte threshold should keep syntax highlighting enabled");
  Expect(!viewport.HighlightedLineTokens(1).empty(),
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

// Drives the off-thread checkpoint backfill synchronously to completion for
// `target_line`, mirroring the shell's schedule (TakeHighlightCheckpointBackfillRequest)
// + worker (ComputeHighlightCheckpoints) + install (InstallHighlightCheckpoints)
// loop. Lets single-threaded tests observe the converged (exact) highlight state.
void DrainHighlightCheckpointBackfill(TextViewport& viewport, std::size_t target_line) {
  for (int guard = 0; guard < 1000; ++guard) {
    (void)viewport.HighlightedLineTokens(target_line);
    auto request = viewport.TakeHighlightCheckpointBackfillRequest();
    if (!request.has_value()) {
      return;
    }
    viewport.InstallHighlightCheckpoints(
        microide::editor::ComputeHighlightCheckpoints(*request));
  }
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

  // A deep cold jump must NOT synchronously replay the whole prefix (that was the
  // multi-second first-paint freeze). The first far query bounds its synchronous
  // replay and arms an off-thread checkpoint backfill for the rest.
  viewport.ResetCacheStats();
  const auto& first_tokens = viewport.HighlightedLineTokens(4095);
  Expect(!first_tokens.empty(), "far-line syntax queries should produce tokens immediately");
  const auto first_stats = viewport.CacheStats();
  Expect(first_stats.highlight_state_advances <= 600,
         "a deep cold jump must bound synchronous replay (remainder deferred off-thread)");
  Expect(viewport.TakeHighlightCheckpointBackfillRequest().has_value(),
         "a deep cold jump should arm an off-thread checkpoint backfill");

  // Once the backfill converges, a far query resumes from a nearby checkpoint and
  // replays at most one checkpoint window.
  DrainHighlightCheckpointBackfill(viewport, 4095);
  viewport.ResetCacheStats();
  const auto& tokens = viewport.HighlightedLineTokens(4095);
  Expect(!tokens.empty(), "far-line syntax queries should still produce tokens");
  const auto stats = viewport.CacheStats();
  Expect(stats.highlight_state_advances < 128,
         "once the checkpoint chain is built, far queries replay at most one window");
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

void TestTextViewportCheckpointBackfillConvergesToExactMultilineState() {
  TextViewport viewport;
  std::string content;
  for (int i = 0; i < 2000; ++i) {
    if (!content.empty()) {
      content.push_back('\n');
    }
    if (i == 5) {
      content += "/* begin block comment";
    } else if (i == 1500) {
      content += "end comment */ int value = 42;";
    } else {
      content += "comment payload";
    }
  }
  viewport.LoadContent(content, "/tmp/highlight-backfill-comment.cpp");
  (void)viewport.HighlightedLineTokens(0);

  // Deep cold jump well beyond the synchronous replay cap, into the middle of a
  // multiline comment. The first paint is bounded (possibly approximate); the
  // off-thread checkpoint backfill must converge to the exact comment state —
  // this is the correctness guarantee for the first-paint freeze fix.
  DrainHighlightCheckpointBackfill(viewport, 1000);
  const auto& inside = viewport.HighlightedLineTokens(1000);
  Expect(!inside.empty(), "deep query inside a multiline comment should produce tokens");
  Expect(std::all_of(inside.begin(), inside.end(),
                     [](SyntaxTokenKind k) { return k == SyntaxTokenKind::Comment; }),
         "checkpoint backfill must converge to the exact multiline comment state at depth");

  DrainHighlightCheckpointBackfill(viewport, 1600);
  const auto& after = viewport.HighlightedLineTokens(1600);
  Expect(std::any_of(after.begin(), after.end(),
                     [](SyntaxTokenKind k) { return k != SyntaxTokenKind::Comment; }),
         "highlighting must recover to non-comment classes after the comment closes");
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
  // Build the full checkpoint chain (the off-thread backfill in production) so
  // this exercises a *post-convergence* tail edit, matching the invariant under
  // test: a tail edit must not rebuild previously valid far checkpoints.
  DrainHighlightCheckpointBackfill(viewport, 4095);
  viewport.MoveCursorTo(4095, viewport.lines().back().size());
  viewport.InsertText(" // tail");
  viewport.ResetCacheStats();

  const auto& tokens = viewport.HighlightedLineTokens(4095);
  Expect(!tokens.empty(), "tail edits should preserve syntax tokens for the edited line");

  const auto stats = viewport.CacheStats();
  // The edit truncates the checkpoint chain at the edited window only; checkpoints
  // below it stay valid (no replay from line 0). Rebuilding the single edited
  // window may replay up to one checkpoint interval when its per-line states were
  // filled off-thread (the backfill snapshots checkpoint states, not every line
  // state) rather than by a synchronous paint — that is at most one window, never
  // the whole chain, which is the invariant under test.
  Expect(stats.highlight_checkpoint_advances <= 128,
         "tail edits must not rebuild the whole checkpoint chain (at most one window)");
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

// Regression: Enter over a top-to-bottom (downward) selection must take its
// auto-indent from the insertion point (range.start, the surviving prefix line),
// not from cursor_line_ which sits on the removed line at range.end. Previously
// the inherited indent came from the bottom line, so replacing a selection could
// drop or corrupt the leading indentation of the new line.
void TestTextViewportInsertNewlineOverDownwardSelectionIndentsFromStartLine() {
  TextViewport viewport;
  viewport.LoadContent("    alpha\nbeta\n", "/tmp/indent-newline-sel.cpp");

  // Select from end of the indented line 0 down to end of the unindented line 1.
  viewport.MoveCursorTo(0, viewport.lines()[0].size(), false);
  viewport.MoveCursorTo(1, viewport.lines()[1].size(), true);
  viewport.InsertNewline();

  Expect(viewport.lines()[0] == "    alpha",
         "the surviving prefix line should keep its original text and indent");
  Expect(viewport.lines()[1] == "    ",
         "newline over a downward selection should inherit the START line's indent, "
         "not the removed end line's (empty) indent");
  Expect(viewport.cursor_line() == 1 && viewport.cursor_column() == 4,
         "caret should land after the inherited four-space indent");
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

// TD-2026-07-17A-092: a range replacement whose text equals the covered content is a
// no-op. It must not bump content_revision, dirty the buffer, or create an undo entry.
void TestTextViewportNoOpRangeReplaceDoesNotDirty() {
  TextViewport viewport;
  viewport.LoadContent("hello world\nsecond line\n", "/tmp/noop-range.txt");
  const std::uint64_t revision_before = viewport.content_revision();
  Expect(!viewport.dirty(), "freshly loaded buffer is clean");

  // Replace "world" (line 0, cols 6..11) with the identical text.
  const microide::editor::SelectionRange range{
      .start = microide::editor::TextPosition{0, 6},
      .end = microide::editor::TextPosition{0, 11}};
  const bool changed = viewport.ReplaceRange(range, "world", /*record_undo=*/true);
  Expect(!changed, "an identical range replacement reports no change");
  Expect(viewport.content_revision() == revision_before,
         "a no-op range replacement must not bump content_revision");
  Expect(!viewport.dirty(), "a no-op range replacement must not dirty the buffer");
  Expect(!viewport.Undo(), "a no-op range replacement must not create an undo entry");

  // A genuine change still applies.
  Expect(viewport.ReplaceRange(range, "WORLD", /*record_undo=*/true),
         "a real range replacement still applies");
  Expect(viewport.content_revision() != revision_before, "a real edit bumps content_revision");
  Expect(viewport.dirty(), "a real edit dirties the buffer");
}

// TD-2026-07-17A-091/093: replacing a line span with byte-identical content (e.g. Sort
// Lines on an already-sorted selection) is a no-op and must not dirty or create undo.
void TestTextViewportNoOpLineReplaceDoesNotDirty() {
  TextViewport viewport;
  viewport.LoadContent("alpha\nbeta\ngamma\n", "/tmp/noop-line.txt");
  const std::uint64_t revision_before = viewport.content_revision();

  const bool changed = viewport.ReplaceLines(0, 2, {"alpha", "beta"}, /*record_undo=*/true);
  Expect(!changed, "an identical line replacement reports no change");
  Expect(viewport.content_revision() == revision_before,
         "a no-op line replacement must not bump content_revision");
  Expect(!viewport.dirty(), "a no-op line replacement must not dirty the buffer");
  Expect(!viewport.Undo(), "a no-op line replacement must not create an undo entry");

  // Sorting an already-sorted selection is the canonical no-op (routes to ReplaceLines).
  Expect(viewport.ReplaceLines(0, 2, {"beta", "alpha"}, /*record_undo=*/true),
         "a reordering line replacement still applies");
  Expect(viewport.dirty(), "a real line replacement dirties the buffer");
}

// A line replacement that shrinks the span (real deletion of content) must still
// apply — the no-op guard only skips edits that leave the buffer byte-identical.
void TestTextViewportEmptyLineDeleteIsNotTreatedAsNoOp() {
  TextViewport viewport;
  viewport.LoadContent("a\nb\nc\n", "/tmp/line-shrink.txt");
  const std::size_t lines_before = viewport.lines().size();

  // Collapse lines [0,2) ("a","b") into a single line "ab": a real edit.
  const bool changed = viewport.ReplaceLines(0, 2, {"ab"}, /*record_undo=*/true);
  Expect(changed, "collapsing two lines into one is a real edit, not a no-op");
  Expect(viewport.lines().size() == lines_before - 1, "the line count shrinks by one");
  Expect(viewport.dirty(), "a real line-count change dirties the buffer");
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

// A disjoint multi-caret line delete cannot be represented as one contiguous
// AppliedEdit. It must clear last_applied_edit_ (like the other aggregate paths)
// so the incremental LSP sync and breakpoint shifter fall back to a full resync
// instead of replaying the previous single-caret edit's stale range/replacement.
void TestTextViewportMultiCaretDeleteLineClearsLastAppliedEdit() {
  TextViewport viewport;
  viewport.LoadContent("alpha\nbeta\ngamma\ndelta\n", "/tmp/multi-delete-applied.txt");

  // A prior single-caret insert leaves a concrete applied edit behind.
  viewport.MoveCursorTo(0, 2);
  viewport.InsertText("Z");
  Expect(viewport.last_applied_edit().has_value(),
         "single-caret insert should publish an applied edit as the stale baseline");

  // Drop a second caret on a non-adjacent line and delete both lines at once.
  viewport.MoveCursorTo(0, 0);
  viewport.AddSecondaryCaret(2, 0);
  Expect(viewport.has_multiple_carets(),
         "test setup should establish a multi-caret configuration");
  Expect(viewport.DeleteCurrentLine(),
         "multi-caret DeleteCurrentLine should report a successful edit");

  Expect(!viewport.last_applied_edit().has_value(),
         "multi-caret line delete must clear last_applied_edit_ so LSP/breakpoints resync");
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

// TD-2026-07-16-31: merge conflict tracking derives its changed-line span from
// last_applied_edit_line_span() instead of diffing a whole-buffer snapshot. The
// span must be whole-line-trimmed (matching WorkspaceShell::ComputeChangedLineSpan),
// which differs from the character-level AppliedEdit at line boundaries — e.g.
// pressing Enter at the end of a line is a PURE line insertion (old_start ==
// old_end) even though the char edit sits on the preceding line.
void TestTextViewportLastAppliedEditLineSpanMatchesWholeLineDiff() {
  using microide::editor::AppliedEditLineSpan;

  // Mid-line insert: exactly one before-line changed, no line-count delta.
  {
    TextViewport viewport;
    viewport.LoadContent("alpha\nbeta\ngamma\n", "/tmp/line-span-insert.txt");
    viewport.MoveCursorTo(0, 2);
    viewport.InsertText("XYZ");
    const auto& span = viewport.last_applied_edit_line_span();
    Expect(span.has_value(), "mid-line insert must publish a line span");
    Expect(span->old_start == 0 && span->old_end == 1 && span->new_end == 1,
           "mid-line insert must report a single changed line, no line delta");
  }

  // Enter at end of a line: the char edit is on line 0, but the whole-line diff
  // sees line 0 unchanged and a new line inserted after it -> PURE insertion.
  {
    TextViewport viewport;
    viewport.LoadContent("alpha\nbeta\ngamma\n", "/tmp/line-span-newline.txt");
    viewport.MoveCursorTo(0, 5);
    viewport.InsertText("\n");
    const auto& span = viewport.last_applied_edit_line_span();
    Expect(span.has_value(), "newline insert must publish a line span");
    Expect(span->old_start == 1 && span->old_end == 1 && span->new_end == 2,
           "Enter at end-of-line must trim to a pure line insertion (old_start == old_end)");
  }

  // Backspace joining line 1 into line 0: two before-lines collapse to one.
  {
    TextViewport viewport;
    viewport.LoadContent("alpha\nbeta\ngamma\n", "/tmp/line-span-join.txt");
    viewport.MoveCursorTo(1, 0);
    viewport.Backspace();
    const auto& span = viewport.last_applied_edit_line_span();
    Expect(span.has_value(), "line-join backspace must publish a line span");
    Expect(span->old_start == 0 && span->old_end == 2 && span->new_end == 1,
           "line-join must span the two collapsed before-lines and end one line shorter");
  }

  // Undo/redo of a single edit publishes the reverse/forward span.
  {
    TextViewport viewport;
    viewport.LoadContent("alpha\nbeta\ngamma\n", "/tmp/line-span-undo.txt");
    viewport.MoveCursorTo(0, 5);
    viewport.InsertText("\n");
    Expect(viewport.Undo(), "undo must succeed");
    const auto& undo_span = viewport.last_applied_edit_line_span();
    Expect(undo_span.has_value() && undo_span->old_start == 1 && undo_span->old_end == 2 &&
               undo_span->new_end == 1,
           "undo of the pure insertion must report the reverse (deletion) span");
    Expect(viewport.Redo(), "redo must succeed");
    const auto& redo_span = viewport.last_applied_edit_line_span();
    Expect(redo_span.has_value() && redo_span->old_start == 1 && redo_span->old_end == 1 &&
               redo_span->new_end == 2,
           "redo must restore the forward pure-insertion span");
  }

  // A disjoint multi-caret edit cannot be one contiguous span; it must be empty so
  // merge tracking (and other single-range consumers) take the resync fallback.
  {
    TextViewport viewport;
    viewport.LoadContent("alpha\nbeta\ngamma\ndelta\n", "/tmp/line-span-multicaret.txt");
    viewport.MoveCursorTo(0, 0);
    viewport.AddSecondaryCaret(2, 0);
    Expect(viewport.has_multiple_carets(), "multi-caret setup must hold");
    Expect(viewport.DeleteCurrentLine(), "multi-caret delete must apply");
    Expect(!viewport.last_applied_edit_line_span().has_value(),
           "a disjoint multi-caret edit must clear the line span");
  }
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

void TestTextViewportTypedCharactersCoalesceIntoWordUndoSteps() {
  TextViewport viewport;
  viewport.LoadContent("", "/tmp/undo-coalesce.txt");

  for (char ch : std::string("foo bar")) {
    viewport.InsertCharacter(ch);
  }
  Expect(viewport.lines().size() == 1 && viewport.lines()[0] == "foo bar",
         "typing should build the full word run");

  // First undo removes the second word; the trailing space stays attached to
  // the first word's run.
  Expect(viewport.Undo(), "first undo after a typing run should succeed");
  Expect(viewport.lines()[0] == "foo ",
         "undo should remove a whole word, not a single character");

  Expect(viewport.Undo(), "second undo should remove the first word run");
  Expect(viewport.lines()[0].empty(),
         "second undo should clear the remaining typed run");

  Expect(!viewport.Undo(),
         "two undos should exhaust a 'foo bar' typing run (no per-character entries)");

  // Redo should restore the runs symmetrically.
  Expect(viewport.Redo() && viewport.lines()[0] == "foo ",
         "redo should restore the first word run");
  Expect(viewport.Redo() && viewport.lines()[0] == "foo bar",
         "redo should restore the second word run");
}

void TestTextViewportCaretJumpBreaksTypingCoalesce() {
  TextViewport viewport;
  viewport.LoadContent("", "/tmp/undo-coalesce-break.txt");

  viewport.InsertCharacter('a');
  viewport.InsertCharacter('b');
  viewport.InsertCharacter('c');
  // Typing resumes at a different caret offset; that discontinuity must start a
  // fresh undo entry rather than extending the "abc" run.
  viewport.MoveCursorTo(0, 1);
  viewport.InsertCharacter('X');
  Expect(viewport.lines()[0] == "aXbc", "fixture should insert 'X' inside 'abc'");

  Expect(viewport.Undo() && viewport.lines()[0] == "abc",
         "a caret jump should isolate the relocated insertion in its own undo step");
  Expect(viewport.Undo() && viewport.lines()[0].empty(),
         "the original contiguous run should undo as one step");
}

void TestTextViewportCaretRoundTripBreaksTypingCoalesce() {
  TextViewport viewport;
  viewport.LoadContent("Z", "/tmp/undo-coalesce-roundtrip.txt");
  viewport.MoveCursorTo(0, 0);

  viewport.InsertCharacter('a');  // "aZ", caret between 'a' and 'Z' at column 1
  // Arrow right past 'Z' then back returns the caret to the exact same column.
  // The run must break on the explicit navigation; otherwise 'a' and 'b' coalesce
  // into one undo step even though the user moved the caret in between.
  viewport.MoveCursorHorizontal(1);
  viewport.MoveCursorHorizontal(-1);
  viewport.InsertCharacter('b');  // "abZ"
  Expect(viewport.lines()[0] == "abZ", "fixture should type 'ab' around a caret round-trip");

  Expect(viewport.Undo() && viewport.lines()[0] == "aZ",
         "a same-column caret round-trip should isolate 'b' in its own undo step");
  Expect(viewport.Undo() && viewport.lines()[0] == "Z",
         "the first character should undo separately");
}

// A mouse-click / goto caret jump goes through MoveCursorTo, which must also end
// the typing coalesce run. Clicking away and back to the exact column the previous
// run ended at (same before-cursor == run's after-cursor) would otherwise let the
// next character merge into the prior run's single undo step.
void TestTextViewportMoveCursorToSameColumnBreaksTypingCoalesce() {
  TextViewport viewport;
  viewport.LoadContent("", "/tmp/undo-coalesce-clickback.txt");

  viewport.InsertCharacter('a');
  viewport.InsertCharacter('b');
  viewport.InsertCharacter('c');  // "abc", run's after-cursor at column 3
  // Simulate a mouse click elsewhere then a click back to the SAME column 3.
  viewport.MoveCursorTo(0, 0);
  viewport.MoveCursorTo(0, 3);
  viewport.InsertCharacter('d');  // "abcd"
  Expect(viewport.lines()[0] == "abcd", "fixture should append 'd' after a click round-trip");

  Expect(viewport.Undo() && viewport.lines()[0] == "abc",
         "a MoveCursorTo round-trip must isolate 'd' in its own undo step");
  Expect(viewport.Undo() && viewport.lines()[0].empty(),
         "the original 'abc' run should undo as one step");
}

// TD-2026-08-05-131. An edit that stays inside one line is now recorded as
// {column, removed_text, inserted_text} rather than as the whole line before and
// after. That is a SECOND representation of "what changed", and a second
// representation is exactly where undo/redo silently drifts from the buffer.
//
// Differential test: drive a viewport with random in-line edits against a naive
// string model, then walk the entire undo stack back and the entire redo stack
// forward, comparing at every step. A caret move before each edit ends the
// coalesce run so one Undo reverses exactly one recorded edit.
void TestTextViewportInlineUndoRedoMatchesNaiveModel() {
  struct Rng {
    std::uint64_t state = 0x2026080531ull;
    std::uint64_t Next() {
      state ^= state << 13;
      state ^= state >> 7;
      state ^= state << 17;
      return state;
    }
    std::size_t Below(std::size_t bound) { return bound == 0 ? 0 : Next() % bound; }
  };

  for (std::uint64_t seed : {1ull, 7ull, 0xC0FFEEull, 0xBEEFull}) {
    Rng rng{seed};
    // Tabs and a multi-byte code point, because the caret's preferred (visual)
    // column is computed from the pre-edit line plus the replacement rather than
    // from a composed post-edit line.
    const std::string initial = "alpha\tbeta gamma\nsecond line\n\tthird\xC3\xA9 line\n";
    TextViewport viewport;
    viewport.LoadContent(initial, "/tmp/inline-undo-model.txt");

    std::vector<std::string> model = {"alpha\tbeta gamma", "second line", "\tthird\xC3\xA9 line",
                                      ""};
    std::vector<std::vector<std::string>> states;
    states.push_back(model);

    const auto snapshot = [&] {
      std::vector<std::string> out;
      for (std::size_t i = 0; i < viewport.lines().size(); ++i) {
        out.emplace_back(viewport.lines().LineView(i));
      }
      return out;
    };
    Expect(snapshot() == model, "fixture: the model must start equal to the buffer");

    for (int step = 0; step < 60; ++step) {
      const std::size_t line = rng.Below(model.size());
      const std::size_t length = model[line].size();
      std::size_t start = rng.Below(length + 1);
      std::size_t end = start + rng.Below(length - start + 1);
      // Byte columns must land on code-point boundaries or the edit clamps and
      // the model would have to model the clamp too.
      const auto snap = [&](std::size_t column) {
        while (column > 0 && column < model[line].size() &&
               (static_cast<unsigned char>(model[line][column]) & 0xC0u) == 0x80u) {
          --column;
        }
        return column;
      };
      start = snap(start);
      end = std::max(start, snap(end));

      std::string replacement;
      for (std::size_t i = 0, count = rng.Below(4); i < count; ++i) {
        replacement.push_back(static_cast<char>('A' + static_cast<char>(rng.Below(26))));
      }
      if (start == end && replacement.empty()) {
        continue;  // exact no-op: records nothing by design
      }
      if (model[line].compare(start, end - start, replacement) == 0) {
        continue;  // exact no-op
      }

      // End any open coalesce run so undo steps map 1:1 onto recorded edits.
      viewport.MoveCursorTo(line, start, false);
      Expect(viewport.ReplaceRange(SelectionRange{TextPosition{line, start},
                                                  TextPosition{line, end}},
                                   replacement, true),
             "an in-line replacement that changes content must be recorded");
      model[line].replace(start, end - start, replacement);
      Expect(snapshot() == model, "the buffer must track the model after an in-line edit");
      states.push_back(model);
    }

    Expect(states.size() > 20, "the fixture must actually produce edits to undo");
    for (std::size_t i = states.size(); i-- > 1;) {
      Expect(viewport.Undo(), "every recorded in-line edit must be undoable");
      Expect(snapshot() == states[i - 1], "undo must restore the previous document exactly");
    }
    Expect(!viewport.Undo(), "the undo stack must be exhausted after undoing every edit");
    for (std::size_t i = 1; i < states.size(); ++i) {
      Expect(viewport.Redo(), "every undone in-line edit must be redoable");
      Expect(snapshot() == states[i], "redo must restore the next document exactly");
    }
    Expect(!viewport.Redo(), "the redo stack must be exhausted after redoing every edit");
  }
}

// The routing itself, pinned. Both entry shapes produce identical buffer content,
// so nothing above this line would notice if in-line edits quietly went back to
// storing the whole affected line twice -- only the allocation gate would, and
// that one only runs under a perf-harness build.
void TestTextViewportInLineEditsRecordColumnScopedUndoEntries() {
  TextViewport viewport;
  viewport.LoadContent("alpha beta\nsecond line\n", "/tmp/inline-entry-shape.txt");

  viewport.MoveCursorTo(0, 5, false);
  viewport.InsertCharacter('X');
  Expect(viewport.TopUndoEntryIsColumnScopedForTesting(),
         "a typed character must record a column-scoped undo entry");

  viewport.MoveCursorTo(0, 3, false);
  viewport.Backspace();
  Expect(viewport.TopUndoEntryIsColumnScopedForTesting(),
         "a backspace inside a line must record a column-scoped undo entry");

  viewport.MoveCursorTo(0, 0, false);
  Expect(viewport.ReplaceRange(SelectionRange{TextPosition{0, 0}, TextPosition{0, 3}}, "ZZ",
                               true),
         "the in-line range replacement applies");
  Expect(viewport.TopUndoEntryIsColumnScopedForTesting(),
         "an in-line range replacement must record a column-scoped undo entry");

  // Anything that changes the line structure must NOT -- the column form cannot
  // express it, and silently taking it would corrupt the document.
  viewport.MoveCursorTo(0, 2, false);
  viewport.InsertNewline();
  Expect(!viewport.TopUndoEntryIsColumnScopedForTesting(),
         "splitting a line must fall back to the line-vector entry");

  viewport.MoveCursorTo(1, 0, false);
  viewport.Backspace();  // joins the split back together
  Expect(!viewport.TopUndoEntryIsColumnScopedForTesting(),
         "joining two lines must fall back to the line-vector entry");

  viewport.MoveCursorTo(0, 4, false);
  viewport.InsertText("multi\nline");
  Expect(!viewport.TopUndoEntryIsColumnScopedForTesting(),
         "a multi-line insertion must fall back to the line-vector entry");

  // And a grouped in-line child is widened on the way into the frame, because the
  // group's aggregation is expressed in line coordinates.
  viewport.BeginUndoGroup();
  Expect(viewport.ReplaceRange(SelectionRange{TextPosition{0, 0}, TextPosition{0, 1}}, "Q", true),
         "the grouped child edit applies");
  viewport.EndUndoGroup();
  Expect(!viewport.TopUndoEntryIsColumnScopedForTesting(),
         "a grouped in-line child must be widened to the line form before it is recorded");
}

// The column-scoped coalesce has three compose shapes, and only two of them are
// reachable by typing (append: an insert or delete-forward run; prepend: a
// backspace run). The third -- an edit landing wholly inside what the run itself
// inserted -- is defensive, mirroring the line form's contained-splice case, so
// drive all three through the history's own API rather than leaving one untested.
//
// Oracle: applying the coalesced entry must give the same buffer as applying the
// two entries in sequence, and undoing it must give back the original.
void TestInlineUndoEntriesCoalesceLikeSequentialApplication() {
  using microide::editor::CoalesceHint;
  using microide::editor::CoalesceKind;
  using microide::editor::TextViewportUndoHistory;

  struct Shape {
    const char* name;
    std::size_t first_column;
    const char* first_removed;
    const char* first_inserted;
    std::size_t second_column;
    const char* second_removed;
    const char* second_inserted;
  };
  const Shape shapes[] = {
      // Typing run: "ab" appended at column 3 as two entries.
      {"append", 3, "", "a", 4, "", "b"},
      // Backspace run: delete column 4 then column 3.
      {"prepend", 4, "e", "", 3, "d", ""},
      // Delete-forward run: both delete at the same column.
      {"delete-forward", 3, "d", "", 3, "e", ""},
      // Contained: the second edit rewrites a byte the first one inserted.
      {"contained", 3, "", "XY", 4, "Y", "Z"},
      // Contained, widening: replaces the whole insertion.
      {"contained-wide", 2, "cd", "PQ", 2, "PQ", "R"},
  };

  for (const Shape& shape : shapes) {
    const std::string line = "abcdefgh";
    const auto make = [&](std::size_t column, const char* removed, const char* inserted,
                          std::size_t before_column, std::size_t after_column) {
      TextViewportUndoHistory::Entry entry;
      entry.is_inline = true;
      entry.start_line = 0;
      entry.start_column = column;
      entry.removed_text = removed;
      entry.inserted_text = inserted;
      entry.before_state.cursor_column = before_column;
      entry.after_state.cursor_column = after_column;
      return entry;
    };
    // The run only continues when the next edit begins exactly where the previous
    // one left off, so thread the caret through both entries.
    const std::size_t first_after =
        shape.first_column + std::string(shape.first_inserted).size();
    const TextViewportUndoHistory::Entry first =
        make(shape.first_column, shape.first_removed, shape.first_inserted, shape.first_column,
             first_after);
    const TextViewportUndoHistory::Entry second =
        make(shape.second_column, shape.second_removed, shape.second_inserted, first_after,
             shape.second_column + std::string(shape.second_inserted).size());

    // Oracle: apply both entries in order to a buffer.
    microide::editor::TextBuffer sequential({line});
    TextViewportUndoHistory::ApplyEntryToBuffer(sequential, first, /*forward=*/true);
    TextViewportUndoHistory::ApplyEntryToBuffer(sequential, second, /*forward=*/true);

    TextViewportUndoHistory history;
    const CoalesceHint hint{.kind = CoalesceKind::Insert, .changed_is_space = false};
    history.RecordEntry(first, hint);
    history.RecordEntry(second, hint);
    Expect(history.CanUndo(), std::string("history must hold an entry for ")
                                  .append(shape.name)
                                  .c_str());
    TextViewportUndoHistory::Entry merged = history.PopUndo();
    Expect(!history.CanUndo(),
           std::string("the two in-line edits must coalesce into ONE undo step for ")
               .append(shape.name)
               .c_str());
    Expect(merged.is_inline,
           std::string("a coalesced in-line run must stay column-scoped for ")
               .append(shape.name)
               .c_str());

    microide::editor::TextBuffer coalesced({line});
    TextViewportUndoHistory::ApplyEntryToBuffer(coalesced, merged, /*forward=*/true);
    Expect(coalesced.LineView(0) == sequential.LineView(0),
           std::string("the coalesced entry must produce the same text as applying both for ")
               .append(shape.name)
               .c_str());

    TextViewportUndoHistory::ApplyEntryToBuffer(coalesced, merged, /*forward=*/false);
    Expect(coalesced.LineView(0) == line,
           std::string("undoing the coalesced entry must restore the original line for ")
               .append(shape.name)
               .c_str());
  }
}

// A non-contiguous second edit must NOT fold into the run: it would splice at a
// column the aggregate says nothing about. This is the guard that keeps the merge
// shapes above from being an "always true" test.
void TestInlineUndoEntriesRefuseNonContiguousCoalesce() {
  using microide::editor::CoalesceHint;
  using microide::editor::CoalesceKind;
  using microide::editor::TextViewportUndoHistory;

  TextViewportUndoHistory history;
  TextViewportUndoHistory::Entry first;
  first.is_inline = true;
  first.start_column = 2;
  first.inserted_text = "a";
  first.after_state.cursor_column = 3;

  TextViewportUndoHistory::Entry second = first;
  second.start_column = 7;  // a gap the aggregate does not cover
  second.inserted_text = "b";
  second.before_state.cursor_column = 3;  // caret continuity holds; the columns do not
  second.after_state.cursor_column = 8;

  const CoalesceHint hint{.kind = CoalesceKind::Insert, .changed_is_space = false};
  history.RecordEntry(first, hint);
  history.RecordEntry(second, hint);
  Expect(history.CanUndo(), "the history must hold the second edit");
  (void)history.PopUndo();
  Expect(history.CanUndo(),
         "a non-contiguous in-line edit must start a fresh undo entry, not splice into the run");
}

// An in-line edit reports its delta as a character-level range, which is what the
// LSP incremental-sync bridge and the marker stores shift by. The column-scoped
// entry has to derive that from {column, removed, inserted} instead of from a
// prefix/suffix diff of two whole lines.
void TestTextViewportInlineEditPublishesCharacterLevelAppliedEdit() {
  TextViewport viewport;
  viewport.LoadContent("alpha beta gamma\n", "/tmp/inline-applied-edit.txt");

  Expect(viewport.ReplaceRange(
             SelectionRange{TextPosition{0, 6}, TextPosition{0, 10}}, "BETA", true),
         "the replacement must apply");
  const auto applied = viewport.last_applied_edit();
  Expect(applied.has_value(), "an in-line edit must publish an applied edit");
  Expect(applied->range_before.start.line == 0 && applied->range_before.start.column == 6,
         "the applied edit must start at the replaced range's column");
  Expect(applied->range_before.end.line == 0 && applied->range_before.end.column == 10,
         "the applied edit must end at the replaced range's end column");
  Expect(applied->replacement_text == "BETA", "the applied edit must carry the replacement");

  // Undo publishes the inverse.
  Expect(viewport.Undo(), "undo must succeed");
  const auto undone = viewport.last_applied_edit();
  Expect(undone.has_value() && undone->replacement_text == "beta",
         "undo must publish the inverse replacement");

  // Shared bytes at either end are trimmed off, exactly as the line form does, so
  // markers are not dragged across text that did not change.
  viewport.MoveCursorTo(0, 0, false);
  Expect(viewport.ReplaceRange(
             SelectionRange{TextPosition{0, 0}, TextPosition{0, 5}}, "alpaca", true),
         "the second replacement must apply");
  const auto trimmed = viewport.last_applied_edit();
  Expect(trimmed.has_value(), "the second edit must publish an applied edit");
  // "alpha" -> "alpaca": shared prefix "alp", shared suffix "a", so the reported
  // delta is the single byte "h" becoming "ac".
  Expect(trimmed->range_before.start.column == 3 && trimmed->range_before.end.column == 4,
         "a shared prefix and suffix must be trimmed out of the reported range");
  Expect(trimmed->replacement_text == "ac",
         "only the changed bytes belong in the replacement text");
}

// The encoding classification is upgrade-only and incremental. The column-scoped
// entry scans only the bytes it splices in -- every other byte on the line was
// classified when it entered the document -- so the scan must still notice a
// non-ASCII insert, and must not be fooled into a downgrade.
void TestTextViewportInlineEditUpgradesEncodingFromTheDeltaAlone() {
  TextViewport viewport;
  viewport.LoadContent("plain ascii line\n", "/tmp/inline-encoding.txt");
  using TextEncoding = microide::editor::TextViewport::TextEncoding;
  Expect(viewport.encoding() == TextEncoding::ASCII,
         "fixture: an ASCII document should be classified ASCII");

  viewport.MoveCursorTo(0, 6, false);
  viewport.InsertText("\xC3\xA9");
  Expect(viewport.encoding() == TextEncoding::UTF8,
         "an in-line insert of a non-ASCII code point must raise the classification");

  // Undo restores ASCII content but the classification is upgrade-only, matching
  // the line form (a full re-detection happens on reload).
  Expect(viewport.Undo(), "undo must succeed");
  Expect(viewport.encoding() == TextEncoding::UTF8,
         "the incremental encoding refresh never downgrades");

  TextViewport bytes_viewport;
  bytes_viewport.LoadContent("plain\n", "/tmp/inline-encoding-nul.txt");
  bytes_viewport.MoveCursorTo(0, 2, false);
  bytes_viewport.InsertText(std::string("\0", 1));
  Expect(bytes_viewport.encoding() == TextEncoding::Bytes,
         "an in-line insert of a NUL must raise the classification to Bytes");
}

// A grouped edit's aggregation is expressed entirely in line coordinates, so an
// in-line child has to be widened before it enters a group frame. Get that wrong
// and a snippet / multi-caret / plugin apply_edits group silently records an undo
// entry that restores the wrong text.
void TestTextViewportGroupedInlineEditsUndoAtomically() {
  TextViewport viewport;
  const std::string initial = "one two\nthree four\nfive six\n";
  viewport.LoadContent(initial, "/tmp/grouped-inline.txt");

  viewport.BeginUndoGroup();
  Expect(viewport.ReplaceRange(SelectionRange{TextPosition{2, 0}, TextPosition{2, 4}}, "FIVE",
                               true),
         "third child edit applies");
  Expect(viewport.ReplaceRange(SelectionRange{TextPosition{0, 4}, TextPosition{0, 7}}, "TWO",
                               true),
         "first child edit applies");
  Expect(viewport.ReplaceRange(SelectionRange{TextPosition{1, 0}, TextPosition{1, 5}}, "THREE",
                               true),
         "second child edit applies");
  viewport.EndUndoGroup();

  Expect(viewport.lines().LineView(0) == "one TWO" &&
             viewport.lines().LineView(1) == "THREE four" &&
             viewport.lines().LineView(2) == "FIVE six",
         "all three grouped in-line edits must be applied");

  Expect(viewport.Undo(), "one undo must reverse the whole group");
  Expect(viewport.lines().LineView(0) == "one two" &&
             viewport.lines().LineView(1) == "three four" &&
             viewport.lines().LineView(2) == "five six",
         "a single undo must restore every line the group touched");
  Expect(!viewport.Undo(), "the group must have recorded exactly one undo entry");

  Expect(viewport.Redo(), "redo must re-apply the whole group");
  Expect(viewport.lines().LineView(0) == "one TWO" &&
             viewport.lines().LineView(2) == "FIVE six",
         "redo must restore every line the group touched");
}

// A run of keystrokes coalesces by appending to the entry's two small strings, so
// the entry's recorded byte size has to be restamped on every merge -- otherwise
// the total-byte history budget trims against a stale size.
void TestInlineCoalescedEntryRestampsItsByteSize() {
  using microide::editor::CoalesceHint;
  using microide::editor::CoalesceKind;
  using microide::editor::TextViewportUndoHistory;

  TextViewportUndoHistory history;
  const CoalesceHint hint{.kind = CoalesceKind::Insert, .changed_is_space = false};
  for (std::size_t i = 0; i < 8; ++i) {
    TextViewportUndoHistory::Entry entry;
    entry.is_inline = true;
    entry.start_column = i;
    entry.inserted_text = "x";
    entry.before_state.cursor_column = i;
    entry.after_state.cursor_column = i + 1;
    history.RecordEntry(entry, hint);
  }
  const TextViewportUndoHistory::Entry merged = history.PopUndo();
  Expect(merged.inserted_text == "xxxxxxxx", "the run must coalesce into one 8-byte insertion");
  Expect(merged.byte_size == merged.inserted_text.size() + merged.removed_text.size(),
         "the coalesced entry's stamped byte size must match its content");
}

void TestTextViewportBackspaceCoalescesIntoWordUndoSteps() {
  TextViewport viewport;
  viewport.LoadContent("foo bar", "/tmp/undo-coalesce-backspace.txt");
  viewport.MoveCursorTo(0, 7);

  for (int i = 0; i < 7; ++i) {
    viewport.Backspace();
  }
  Expect(viewport.lines()[0].empty(), "backspacing should clear the line");

  // The deletion run splits on the word boundary, mirroring the typing rule.
  Expect(viewport.Undo() && viewport.lines()[0] == "foo",
         "first undo should restore the most recently deleted word run");
  Expect(viewport.Undo() && viewport.lines()[0] == "foo bar",
         "second undo should restore the remaining deleted run");
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

// Regression: with soft-wrap on, a collapsed fold whose opener line wraps into
// multiple visual rows used to report a single-row range ({R, R}) for the opener,
// so vertical motion got stuck on it and never crossed the fold.
void TestTextViewportSoftWrapCollapsedFoldOpenerVerticalMotionEscapes() {
  TextViewport viewport;
  viewport.LoadContent(
      "top\nvoid aaaaaaaaaaaaaaaaaaaaaaaa() {\n  x();\n  y();\n}\nbottom\n",
      "/tmp/soft-wrap-fold.cpp");
  viewport.SetViewportSize(6, 8);  // narrow: the long opener wraps into several rows
  viewport.SetSoftWrap(true);

  FoldingModel folding_model;
  FoldingModel::ComputeOptions fold_options;
  fold_options.bracket_pairs = {{'{', '}'}};
  fold_options.use_indent_source = true;
  fold_options.tab_size = 4;
  Expect(folding_model.Compute(viewport.lines().Snapshot(), fold_options),
         "fold compute should complete for the soft-wrap fold fixture");
  Expect(folding_model.Collapse(1), "the wrapping function opener should collapse");
  viewport.SetFoldingModel(&folding_model);

  // Caret at the end of the collapsed opener (its last wrapped visual row).
  viewport.MoveCursorTo(1, 1000);
  Expect(viewport.cursor_line() == 1, "caret starts on the collapsed opener line");

  // Moving down must leave the opener and land on the line after the folded body,
  // not stay stuck cycling the opener's own wrapped rows.
  viewport.MoveCursorVertical(1);
  Expect(viewport.cursor_line() == 5,
         "down from a collapsed soft-wrapped opener must cross the fold to the next visible line");
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

// Regression: Shift+PageUp / Shift+PageDown must extend the selection like every
// other shifted motion key. Page() used to drop the modifier entirely and always
// move with extend_selection=false, so a paging motion discarded the selection
// instead of growing it.
void TestTextViewportShiftPageExtendsSelection() {
  TextViewport viewport;
  std::string content;
  for (int i = 0; i < 30; ++i) {
    content += "line";
    content += std::to_string(i);
    content += "\n";
  }
  viewport.LoadContent(content, "/tmp/shift-page.txt");
  viewport.SetViewportSize(10, 80);
  viewport.MoveCursorTo(20, 2);
  Expect(!viewport.selection_range().has_value(), "no selection before a paging motion");

  viewport.Page(-1, /*extend_selection=*/true);
  const auto selection = viewport.selection_range();
  Expect(selection.has_value(), "Shift+PageUp must create a selection");
  Expect(viewport.cursor_line() < 20, "the caret must move up a page");
  const SelectionRange normalized = TextViewport::NormalizeRange(*selection);
  Expect(normalized.end.line == 20 && normalized.end.column == 2,
         "the selection anchor stays at the original caret position");
  Expect(normalized.start.line == viewport.cursor_line() &&
             normalized.start.column == viewport.cursor_column(),
         "the selection start follows the paged caret");

  // A paging motion WITHOUT shift collapses the selection, matching arrow keys.
  viewport.Page(1, /*extend_selection=*/false);
  Expect(!viewport.selection_range().has_value(),
         "PageDown without shift must clear the selection");
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

void TestTextViewportSoftWrapVerticalMotionFollowsNonUniformRows() {
  // Regression: caret row resolution must use the builder's actual word-wrapped
  // spans, not uniform caret_visual / wrap_columns division. At wrap 12 this
  // sentence wraps to spans [0,12) [12,16) [16,26) [26,36); visual column 20
  // lives in row 2, but the old division landed on row 1.
  TextViewport viewport;
  viewport.LoadContent("hello brave new wonderful world here\n",
                       "/tmp/soft-wrap-nonuniform.txt");
  viewport.SetViewportSize(10, 12);
  viewport.SetSoftWrap(true);
  viewport.MoveCursorTo(0, 20);

  Expect(viewport.cursor_visual_row() == 2,
         "a caret inside a word-wrapped row resolves via its real span, not uniform division");
  Expect(viewport.VisibleWrappedRowLayout(2).caret_visible,
         "the rendered caret should land on the row whose span actually owns the caret column");
  Expect(!viewport.VisibleWrappedRowLayout(1).caret_visible,
         "the caret should not also render on the uniformly-divided wrong row");

  viewport.MoveCursorVertical(1);
  Expect(viewport.cursor_line() == 0 && viewport.cursor_column() == 30,
         "moving down preserves the preferred column relative to the correct non-uniform row");
  Expect(viewport.cursor_visual_row() == 3,
         "downward motion advances by one real visual row");
}

void TestTextViewportSoftWrapHangingIndentAlignsContinuationRows() {
  // Four leading spaces become the hanging indent for continuation rows. At
  // wrap 12 the indent is min(4, 12/2) = 4.
  TextViewport viewport;
  viewport.LoadContent("    aaaa bbbb cccc dddd eeee\n",
                       "/tmp/soft-wrap-hanging-indent.txt");
  viewport.SetViewportSize(10, 12);
  viewport.SetSoftWrap(true);

  const auto row0 = viewport.WrappedVisualRowLayout(0);
  const auto row1 = viewport.WrappedVisualRowLayout(1);
  Expect(row0.indent == 0, "the first row of a wrapped line carries no hanging indent");
  Expect(row1.indent == 4,
         "continuation rows expose the line's leading-whitespace visual width as hanging indent");
  Expect(row1.indent + (row1.visual_end - row1.visual_start) <= 12,
         "continuation rows pack against the width reduced by the hanging indent");

  const TextPosition gutter_hit = viewport.LogicalPositionForVisualHit(1, 2);
  Expect(gutter_hit == TextPosition{0, row1.visual_start},
         "a click in a continuation row's indent gutter resolves to the row's first column");
  const TextPosition content_hit = viewport.LogicalPositionForVisualHit(1, 6);
  Expect(content_hit == TextPosition{0, row1.visual_start + 2},
         "continuation-row hit-testing subtracts the hanging indent before mapping the column");
}

// Editing under soft-wrap must keep the wrapped-row table in sync with the
// buffer. The wrapped table is built lazily on first access; these tests force
// a build with the pre-edit content, then edit, then assert the post-edit
// visual-row mapping — the exact scenario left stale when the table was keyed
// only on layout_shape_revision (which content edits do not bump).
void TestTextViewportSoftWrapEditGrowsWrapRowCount() {
  TextViewport viewport;
  viewport.LoadContent("abc\nXYZ\n", "/tmp/soft-wrap-edit-grow.txt");
  viewport.SetViewportSize(10, 8);
  viewport.SetSoftWrap(true);

  // Force a build with the pre-edit content: 3 single-row logical lines.
  Expect(viewport.VisualRowCount() == 3, "pre-edit: three single-row lines");

  viewport.MoveCursorTo(0, 3);
  viewport.InsertText("defghijkl");  // line0 -> "abcdefghijkl" (12 cols -> 2 rows)

  Expect(viewport.VisualRowCount() == 4,
         "growing a line past the wrap width must add a visual row");
  Expect(viewport.WrappedVisualRowLayout(0).line_index == 0 &&
             viewport.WrappedVisualRowLayout(1).line_index == 0,
         "the grown line should now own two visual rows");
  Expect(viewport.WrappedVisualRowLayout(2).line_index == 1,
         "the following line's rows must shift down after the edit");
  Expect(viewport.WrappedVisualRowLayout(3).line_index == 2,
         "the trailing empty-line row must remain mapped to the last line");
}

void TestTextViewportSoftWrapEnterSplitUpdatesRowMapping() {
  TextViewport viewport;
  viewport.LoadContent("abcdef\nZZ\n", "/tmp/soft-wrap-edit-split.txt");
  viewport.SetViewportSize(10, 8);
  viewport.SetSoftWrap(true);

  Expect(viewport.VisualRowCount() == 3, "pre-edit: three single-row lines");

  viewport.MoveCursorTo(0, 3);
  viewport.InsertNewline();  // "abcdef" -> "abc" / "def"

  Expect(viewport.VisualRowCount() == 4,
         "splitting a line with Enter must add a logical line and its visual row");
  Expect(viewport.WrappedVisualRowLayout(1).line_index == 1,
         "the second half of the split lands on the new line index 1");
  Expect(viewport.WrappedVisualRowLayout(2).line_index == 2,
         "subsequent lines shift down by one after the split");
  Expect(viewport.WrappedVisualRowLayout(3).line_index == 3,
         "the trailing empty line shifts to index 3 after the split");
}

void TestTextViewportSoftWrapBackspaceMergeUpdatesRowMapping() {
  TextViewport viewport;
  viewport.LoadContent("abc\ndef\nZZ\n", "/tmp/soft-wrap-edit-merge.txt");
  viewport.SetViewportSize(10, 8);
  viewport.SetSoftWrap(true);

  Expect(viewport.VisualRowCount() == 4, "pre-edit: four single-row lines");

  viewport.MoveCursorTo(1, 0);
  viewport.Backspace();  // merges "def" onto "abc" -> "abcdef"

  Expect(viewport.VisualRowCount() == 3,
         "merging two logical lines must drop a visual row");
  Expect(viewport.WrappedVisualRowLayout(0).line_index == 0,
         "the merged line remains at index 0");
  Expect(viewport.WrappedVisualRowLayout(1).line_index == 1,
         "the line after the merge must map to the new index 1 (ZZ)");
  // Would read document_->lines[2] out of bounds if the table kept its stale
  // pre-merge row count / line indices.
  const auto last = viewport.VisibleWrappedRowLayout(2);
  Expect(last.text.empty(),
         "the trailing empty-line row must resolve without a stale line index");
}

#ifndef NDEBUG
void TestTextViewportSoftWrapEditUpdatesWrapIncrementally() {
  TextViewport viewport;
  viewport.LoadContent("hello world one\nsecond line two\nthird line three\n",
                       "/tmp/soft-wrap-edit-incremental.txt");
  viewport.SetViewportSize(10, 8);
  viewport.SetSoftWrap(true);

  (void)viewport.VisualRowCount();  // force the initial full build
  const std::size_t builds = viewport.WrappedRowLayoutBuildCountForDebug();

  viewport.MoveCursorTo(0, 0);
  viewport.InsertText("x");
  (void)viewport.VisualRowCount();

  Expect(viewport.WrappedRowLayoutBuildCountForDebug() == builds,
         "a content edit under soft wrap must update the wrapped table in place, "
         "not trigger a full O(document) rebuild");
}

// TD-2026-07-17A-022: a common single-line edit near the top of a large
// soft-wrapped buffer must not walk the rest of the document. When the edit
// preserves the affected line's wrap-row count and the logical-line count, the
// incremental update takes the O(edit-size) in-place fast path: it overwrites
// only the edited line's rows and its own offset, and touches none of the
// O(document) suffix (no row line_index shift, no offset-table rebuild, no
// visual-column erase/insert memmove).
void TestTextViewportSoftWrapLargeBufferInlineEditIsInPlace() {
  TextViewport viewport;
  std::string content;
  content.reserve(4000 * 4);
  for (int i = 0; i < 4000; ++i) {
    content += "abc\n";  // each a single wrapped row; slack under wrap width
  }
  viewport.LoadContent(content, "/tmp/soft-wrap-large-inline.txt");
  viewport.SetViewportSize(20, 12);
  viewport.SetSoftWrap(true);
  const int rows_before = viewport.VisualRowCount();  // force the initial build
  (void)viewport.max_visual_columns();  // warm the per-line visual-column cache

  const std::size_t builds = viewport.WrappedRowLayoutBuildCountForDebug();
  const std::size_t inplace_before = viewport.WrappedRowIncrementalInplaceCountForDebug();
  const std::size_t splice_before = viewport.WrappedRowIncrementalSpliceCountForDebug();
  const std::size_t vcol_before = viewport.VisualColumnIncrementalInplaceCountForDebug();

  // Insert one char near the top (line 1): "abc" -> "abZc" stays a single row,
  // so neither the wrap-row count nor the logical-line count changes. (Line 0 is
  // avoided only because a content edit at line 0 deliberately drops the whole
  // visible-line + visual-column cache in InvalidateDerivedCaches; the
  // wrapped-row table — the expensive one — still updates in place there too.)
  viewport.MoveCursorTo(1, 2);
  viewport.InsertText("Z");
  (void)viewport.VisualRowCount();
  (void)viewport.max_visual_columns();  // exercise the maintained width cache

  Expect(viewport.WrappedRowLayoutBuildCountForDebug() == builds,
         "an in-line edit under soft wrap must not trigger a full O(document) rebuild");
  Expect(viewport.WrappedRowIncrementalInplaceCountForDebug() == inplace_before + 1,
         "an in-line edit preserving line/row counts must take the in-place fast path");
  Expect(viewport.WrappedRowIncrementalSpliceCountForDebug() == splice_before,
         "the in-place fast path must not fall through to the O(suffix) splice");
  Expect(viewport.VisualColumnIncrementalInplaceCountForDebug() == vcol_before + 1,
         "the visual-column cache must also update the edited line in place");

  // The edited row and a deep-suffix row must both still resolve correctly.
  Expect(viewport.WrappedVisualRowLayout(1).line_index == 1,
         "the edited line stays at visual row 1");
  Expect(viewport.WrappedVisualRowLayout(3999).line_index == 3999,
         "the far-suffix line keeps its untouched mapping");
  Expect(viewport.VisualRowCount() == rows_before,
         "an in-line edit must not change the visual row count");
}

// Correctness companion: incremental splice/fast-path updates over a soft-wrapped
// buffer with genuinely multi-row lines must stay identical to a fresh full
// build after a mix of in-line edits, newline splits, and merges.
void TestTextViewportSoftWrapIncrementalEditsMatchFreshBuild() {
  const char* kSeed =
      "the quick brown fox jumps over the lazy dog\n"
      "pack my box with five dozen liquor jugs today\n"
      "sphinx of black quartz judge my vow right now\n"
      "how vexingly quick daft zebras jump around here\n"
      "the five boxing wizards jump quickly at dawn\n";
  TextViewport viewport;
  viewport.LoadContent(kSeed, "/tmp/soft-wrap-incremental-match.txt");
  viewport.SetViewportSize(20, 12);
  viewport.SetSoftWrap(true);
  (void)viewport.VisualRowCount();

  // In-line insert (in-place or splice depending on whether the row count shifts).
  viewport.MoveCursorTo(0, 4);
  viewport.InsertText("VERYLONGTOKEN");
  // Delete a char (in-line).
  viewport.MoveCursorTo(1, 3);
  viewport.Backspace();
  // Split a line (line_delta > 0 -> splice with a positive line_index shift).
  viewport.MoveCursorTo(2, 10);
  viewport.InsertNewline();
  // Merge two lines (line_delta < 0 -> splice with a negative line_index shift).
  viewport.MoveCursorTo(4, 0);
  viewport.Backspace();
  (void)viewport.VisualRowCount();

  // Reconstruct the post-edit content and build a fresh viewport from it. The
  // buffer's trailing empty line already stands for the final newline, so join
  // the lines WITHOUT a trailing newline to reproduce identical content.
  const std::vector<std::string> snapshot = viewport.lines().Snapshot();
  std::string rebuilt;
  for (std::size_t i = 0; i < snapshot.size(); ++i) {
    rebuilt += snapshot[i];
    if (i + 1 < snapshot.size()) {
      rebuilt += '\n';
    }
  }
  TextViewport fresh;
  fresh.LoadContent(rebuilt, "/tmp/soft-wrap-fresh.txt");
  fresh.SetViewportSize(20, 12);
  fresh.SetSoftWrap(true);
  (void)fresh.VisualRowCount();

  // Sanity: at least one edit exercised each incremental path.
  Expect(viewport.WrappedRowIncrementalInplaceCountForDebug() >= 1 &&
             viewport.WrappedRowIncrementalSpliceCountForDebug() >= 1,
         "the edit mix must exercise both the in-place fast path and the splice path");
  Expect(viewport.VisualRowCount() == fresh.VisualRowCount(),
         "incremental edits must yield the same visual row count as a fresh build");
  const int rows = fresh.VisualRowCount();
  for (int r = 0; r < rows; ++r) {
    const auto a = viewport.WrappedVisualRowLayout(static_cast<std::size_t>(r));
    const auto b = fresh.WrappedVisualRowLayout(static_cast<std::size_t>(r));
    Expect(a.line_index == b.line_index && a.visual_start == b.visual_start &&
               a.visual_end == b.visual_end && a.indent == b.indent,
           "each incremental wrapped row must match the fresh-build row exactly");
  }
}
#endif

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
  Expect(folding_model.Compute(viewport.lines().Snapshot(), DefaultFoldOptions()),
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

void TestTextViewportCollapsedFoldRowSpansTrackHorizontalScroll() {
  // Regression: with a collapsed fold and soft wrap off, a row's visible span
  // must reflect the live horizontal scroll. The fold-but-no-wrap path used to
  // bake horizontal_scroll into the cached span without keying on it, so the
  // span went stale after scrolling and highlights clipped to the wrong window.
  TextViewport viewport;
  viewport.LoadContent(
      "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA\nvoid f() {\n  a();\n  b();\n}\nx();\n",
      "/tmp/fold-hscroll.cpp");
  FoldingModel folding_model;
  Expect(folding_model.Compute(viewport.lines().Snapshot(), DefaultFoldOptions()),
         "fold compute should complete for the horizontal-scroll fixture");
  Expect(folding_model.Collapse(1), "function fold should collapse");
  viewport.SetFoldingModel(&folding_model);
  viewport.SetViewportSize(10, 8);

  Expect(viewport.VisualRowCount() < 7,
         "collapsing the function should hide its body rows");

  viewport.SetHorizontalScroll(5);
  const auto first = viewport.WrappedVisualRowLayout(0);
  Expect(first.line_index == 0 && first.visual_start == 5 && first.visual_end == 13,
         "a fold-but-no-wrap row span should reflect the current horizontal scroll");

  viewport.SetHorizontalScroll(9);
  const auto scrolled = viewport.WrappedVisualRowLayout(0);
  Expect(scrolled.visual_start == 9 && scrolled.visual_end == 17,
         "the span should update with the live horizontal scroll, not return a stale baked value");
}

void TestTextViewportHorizontalMotionCrossesLineBoundaries() {
  TextViewport viewport;
  viewport.LoadContent("abc\nde\nf", "/tmp/hmotion.cpp");  // no trailing newline: 3 lines

  // Right at end-of-line steps to the start of the next line (VS Code semantics).
  viewport.MoveCursorTo(0, 3);  // end of "abc"
  viewport.MoveCursorHorizontal(1);
  Expect(viewport.cursor_line() == 1 && viewport.cursor_column() == 0,
         "Right at end-of-line should move to the start of the next line");

  // Left at column 0 steps to the end of the previous line.
  viewport.MoveCursorHorizontal(-1);
  Expect(viewport.cursor_line() == 0 && viewport.cursor_column() == 3,
         "Left at column 0 should move to the end of the previous line");

  // Left at the very start of the document is a no-op (nothing before it).
  viewport.MoveCursorTo(0, 0);
  viewport.MoveCursorHorizontal(-1);
  Expect(viewport.cursor_line() == 0 && viewport.cursor_column() == 0,
         "Left at the document start should stay put");

  // Right at the very end of the last line is a no-op (does not fall off buffer).
  viewport.MoveCursorTo(2, 1);  // end of "f"
  viewport.MoveCursorHorizontal(1);
  Expect(viewport.cursor_line() == 2 && viewport.cursor_column() == 1,
         "Right at the document end should stay put");
}

void TestTextViewportCollapsedFoldVerticalMotionSkipsHiddenLines() {
  TextViewport viewport;
  viewport.LoadContent("before();\nvoid f() {\n  a();\n  b();\n}\nafter();\n", "/tmp/fold-motion.cpp");
  FoldingModel folding_model;
  Expect(folding_model.Compute(viewport.lines().Snapshot(), DefaultFoldOptions()),
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
  Expect(folding_model.Compute(viewport.lines().Snapshot(), DefaultFoldOptions()),
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
  Expect(folding_model.Compute(viewport.lines().Snapshot(), DefaultFoldOptions()),
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

  // TD 022 (standing backlog #9): batch replace + undo/redo on a large document
  // must stay affected-range-only — no whole-buffer TextBuffer::Snapshot()
  // materialization anywhere in the edit funnel.
  microide::editor::TextBuffer::reset_snapshot_build_count();

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

  Expect(microide::editor::TextBuffer::snapshot_build_count() == 0,
         "replace-all + undo + redo on a large document must never materialize the whole buffer");
}

// TD 022 (standing backlog #9): the single-caret edit funnel — character insert,
// newline split, backspace join, multi-line paste, and a spanning range delete —
// must stay affected-range-only on a large buffer (no TextBuffer::Snapshot()
// whole-document materialization), and the full undo chain must restore the
// original content exactly. Complements the multi-caret grouped-edit snapshot
// guards in EditorMultiCaretTests.
void TestTextViewportLargeBufferSingleCaretEditWorkoutAvoidsSnapshot() {
  using microide::editor::TextBuffer;
  TextViewport viewport;
  std::string content;
  content.reserve(50000 * 10);
  for (int i = 0; i < 50000; ++i) {
    content += "row ";
    content += std::to_string(i);
    content += '\n';
  }
  viewport.LoadContent(content, "/tmp/large-single-caret-workout.txt");
  const std::size_t baseline_lines = viewport.lines().size();
  constexpr std::size_t kMidLine = 25000;
  const std::string original_mid = viewport.lines()[kMidLine];
  const std::string original_before = viewport.lines()[kMidLine - 1];
  const std::string original_after = viewport.lines()[kMidLine + 1];

  TextBuffer::reset_snapshot_build_count();

  viewport.MoveCursorTo(kMidLine, 0);
  viewport.InsertText("Z");     // same-line insert (line-edit fast path)
  viewport.InsertNewline();     // line split (+1)
  viewport.Backspace();         // col-0 join back (-1)
  viewport.InsertText("A\nB\n");  // multi-line paste (+2)
  // Spanning range delete removing exactly the pasted "A\nB\n" (-2).
  viewport.ReplaceRange(SelectionRange{.start = TextPosition{kMidLine, 1},
                                       .end = TextPosition{kMidLine + 2, 0}},
                        "", /*record_undo=*/true);

  Expect(TextBuffer::snapshot_build_count() == 0,
         "the single-caret edit workout must never materialize the whole buffer");
  Expect(viewport.lines().size() == baseline_lines,
         "the workout nets out to a single-character insert (line count unchanged)");
  Expect(viewport.lines()[kMidLine] == "Z" + original_mid,
         "the workout leaves only the single-character insert behind");
  Expect(viewport.lines()[kMidLine - 1] == original_before &&
             viewport.lines()[kMidLine + 1] == original_after,
         "neighboring lines must be untouched by the mid-file workout");

  while (viewport.Undo()) {
  }
  Expect(viewport.lines().size() == baseline_lines &&
             viewport.lines()[kMidLine] == original_mid,
         "draining undo must restore the original large-buffer content");
  Expect(TextBuffer::snapshot_build_count() == 0,
         "draining undo must never materialize the whole buffer");

  while (viewport.Redo()) {
  }
  Expect(viewport.lines().size() == baseline_lines &&
             viewport.lines()[kMidLine] == "Z" + original_mid,
         "draining redo must reproduce the post-workout buffer");
  Expect(TextBuffer::snapshot_build_count() == 0,
         "draining redo must never materialize the whole buffer");
}

void TestTextViewportReplaceAllMultiMatchRespectsUnequalLengths() {
  // Regression for a coordinate-system desync: the per-line loop used to mutate its
  // lowered scratch line and resume searching from the mutated position while still
  // indexing the unmodified source line, so every match after the first was placed
  // wrong whenever replacement.size() != needle.size().
  {
    TextViewport viewport;
    viewport.LoadContent("axaxa", "/tmp/replace-grow.txt");
    const std::size_t replaced = viewport.ReplaceAll("x", "yy");
    Expect(replaced == 2, "replace-all should count both matches with a longer replacement");
    Expect(viewport.lines()[0] == "ayyayya",
           "replace-all with a longer replacement must not drop later matches");
  }
  {
    TextViewport viewport;
    viewport.LoadContent("aaaa", "/tmp/replace-shrink.txt");
    const std::size_t replaced = viewport.ReplaceAll("aa", "b");
    Expect(replaced == 2, "replace-all should count both non-overlapping matches");
    Expect(viewport.lines()[0] == "bb",
           "replace-all with a shorter replacement must not corrupt source bytes");
  }
  {
    // Multiple lines, replacement longer than needle, matches also at line edges.
    TextViewport viewport;
    viewport.LoadContent("xax\nbxb", "/tmp/replace-multiline.txt");
    const std::size_t replaced = viewport.ReplaceAll("x", "zz");
    Expect(replaced == 3, "replace-all should count matches across lines");
    Expect(viewport.lines()[0] == "zzazz" && viewport.lines()[1] == "bzzb",
           "replace-all must rewrite every line correctly with unequal lengths");
  }
}

void TestTextViewportReplaceAllUnicodeCaseInsensitive() {
  // Case-insensitive replace matches non-ASCII case variants. "café" (é) folds to
  // match "CAFÉ" (É) and "Café"; the fold is length-preserving so the offset
  // arithmetic that indexes the source line by folded-buffer offsets stays correct.
  TextViewport viewport;
  viewport.LoadContent("café CAFÉ Café x", "/tmp/replace-unicode.txt");
  const std::size_t replaced = viewport.ReplaceAll("café", "tea");
  Expect(replaced == 3, "replace-all matches every case variant of the accented needle");
  Expect(viewport.lines()[0] == "tea tea tea x",
         "each accented match is replaced and surrounding bytes are preserved");
}

void TestTextViewportReplaceAllMultiLineReplacementSplitsLines() {
  // Regression: a replacement containing a newline used to be stuffed into a single
  // logical line via SetLine, leaving the PieceTree's line_count disagreeing with
  // the buffer (an embedded '\n' inside one "line"), which corrupts later edits and
  // undo. It must splice in as real physical lines instead.
  {
    TextViewport viewport;
    viewport.LoadContent("x", "/tmp/replace-newline.txt");
    const std::size_t replaced = viewport.ReplaceAll("x", "a\nb");
    Expect(replaced == 1, "single match with a multi-line replacement counts once");
    Expect(viewport.line_count() == 2, "a newline replacement must expand into two lines");
    Expect(viewport.lines()[0] == "a" && viewport.lines()[1] == "b",
           "multi-line replacement must produce real lines, not an embedded newline");
    // Line views never contain a newline when line_count is consistent.
    Expect(std::string(viewport.lines()[0]).find('\n') == std::string::npos &&
               std::string(viewport.lines()[1]).find('\n') == std::string::npos,
           "no line should hide an embedded newline");
  }
  {
    // Across several source lines, and CRLF in the replacement is normalized.
    TextViewport viewport;
    viewport.LoadContent("foo\nbar\nfoo", "/tmp/replace-newline-multi.txt");
    const std::size_t replaced = viewport.ReplaceAll("foo", "one\r\ntwo");
    Expect(replaced == 2, "both foo matches replaced");
    Expect(viewport.line_count() == 5,
           "two single->double line expansions plus the untouched middle line");
    Expect(viewport.lines()[0] == "one" && viewport.lines()[1] == "two" &&
               viewport.lines()[2] == "bar" && viewport.lines()[3] == "one" &&
               viewport.lines()[4] == "two",
           "multi-line replacements must splice correctly around unchanged lines");
  }
  {
    // Undo must restore the original buffer exactly after a multi-line replace.
    TextViewport viewport;
    viewport.LoadContent("x\ny", "/tmp/replace-newline-undo.txt");
    viewport.ReplaceAll("x", "a\nb");
    Expect(viewport.line_count() == 3, "replacement expanded the first line");
    viewport.Undo();
    Expect(viewport.line_count() == 2 && viewport.lines()[0] == "x" &&
               viewport.lines()[1] == "y",
           "undo must restore the pre-replacement buffer exactly");
  }
}

// TD-2026-07-17A-028: ReplaceAllRanges applies a precomputed match set as one
// grouped edit and must be byte-identical to the scanning ReplaceAll (including
// undo/redo). It must also validate its input and leave the document untouched
// (returning nullopt) when the caller's ranges are stale/inconsistent.
void TestTextViewportReplaceAllRangesMatchesScanningReplaceAll() {
  using microide::editor::SelectionRange;
  using microide::editor::TextPosition;

  auto make_range = [](std::size_t line, std::size_t start, std::size_t end) {
    return SelectionRange{TextPosition{line, start}, TextPosition{line, end}};
  };

  {
    // Multiple matches per line, across lines, longer replacement. Compare a
    // range-driven apply to a freshly-scanned ReplaceAll on identical content.
    const char* content = "xax\nbetween\nxxb";
    TextViewport range_view;
    range_view.LoadContent(content, "/tmp/replace-ranges-a.txt");
    // Matches of "x": (0,0),(0,2),(2,0),(2,1). Ascending, single-line, non-overlapping.
    const std::vector<SelectionRange> matches = {
        make_range(0, 0, 1), make_range(0, 2, 3), make_range(2, 0, 1), make_range(2, 1, 2)};
    const auto applied = range_view.ReplaceAllRanges(matches, "zz");
    Expect(applied.has_value() && *applied == 4, "all four ranges applied");

    TextViewport scan_view;
    scan_view.LoadContent(content, "/tmp/replace-ranges-b.txt");
    const std::size_t scanned = scan_view.ReplaceAll("x", "zz");
    Expect(scanned == 4, "scanning replace-all finds the same four matches");
    Expect(range_view.lines().Snapshot() == scan_view.lines().Snapshot(),
           "range-driven replace-all must equal scanning replace-all byte-for-byte");

    // Undo restores the exact original for the range-driven edit.
    Expect(range_view.Undo(), "undo after range replace-all succeeds");
    Expect(range_view.lines()[0] == "xax" && range_view.lines()[1] == "between" &&
               range_view.lines()[2] == "xxb",
           "undo restores the pre-replacement buffer exactly");
    Expect(range_view.Redo() && range_view.lines()[0] == "zzazz",
           "redo reapplies the range replace-all");
  }

  {
    // A multi-line replacement splices in real lines, matching ReplaceAll.
    TextViewport range_view;
    range_view.LoadContent("foo\nbar\nfoo", "/tmp/replace-ranges-nl.txt");
    const std::vector<SelectionRange> matches = {make_range(0, 0, 3), make_range(2, 0, 3)};
    const auto applied = range_view.ReplaceAllRanges(matches, "one\r\ntwo");
    Expect(applied.has_value() && *applied == 2, "both matches applied");
    Expect(range_view.line_count() == 5 && range_view.lines()[0] == "one" &&
               range_view.lines()[1] == "two" && range_view.lines()[2] == "bar" &&
               range_view.lines()[3] == "one" && range_view.lines()[4] == "two",
           "multi-line replacement splices real lines around the untouched middle line");
  }

  {
    // Validation: an out-of-range / unsorted / overlapping set must bail with
    // nullopt and leave the buffer untouched.
    TextViewport view;
    view.LoadContent("abc", "/tmp/replace-ranges-invalid.txt");
    Expect(!view.ReplaceAllRanges({make_range(0, 1, 1)}, "z").has_value(),
           "an empty range is rejected");
    Expect(!view.ReplaceAllRanges({make_range(0, 0, 99)}, "z").has_value(),
           "a range past the line end is rejected");
    Expect(!view.ReplaceAllRanges({make_range(0, 2, 3), make_range(0, 0, 1)}, "z").has_value(),
           "a descending range set is rejected");
    Expect(view.lines()[0] == "abc", "a rejected range set leaves the buffer untouched");
  }
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

void TestRuntimeSyntaxCarriesRegionAcrossBlankLine() {
  namespace rs = microide::editor::runtime_syntax;
  const std::filesystem::path path = "/tmp/region.c";
  const std::string opener = "int x; /* start of a block comment";

  // The opener leaves an unterminated block comment open (region depth > 0).
  microide::editor::SyntaxState opened = rs::AdvanceState(opener, path, {}, opener);
  Expect(opened.region_depth > 0, "an unterminated block comment opens a region");

  // Regression: a blank line inside the comment must carry the open region
  // forward, not reset to top level (which would resume the rest of the file as
  // code and mis-highlight everything after the blank line).
  const microide::editor::SyntaxState after_blank = rs::AdvanceState("", path, opened, opener);
  Expect(after_blank.region_depth == opened.region_depth,
         "a blank line inside a block comment must preserve the open region");
  Expect(after_blank.definition_id == opened.definition_id,
         "the definition id is preserved across a blank line");

  // The next line still resumes inside the comment and its */ closes it.
  const microide::editor::SyntaxState after_close =
      rs::AdvanceState("still inside */", path, after_blank, opener);
  Expect(after_close.region_depth == 0, "the closing */ ends the carried region");
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

// One keystroke inside one line must cost a bounded multiple of that line, not an
// open-ended one. On ordinary source lines the difference is invisible; the file
// that makes it visible is the one with no line breaks -- a minified bundle, a
// newline-free JSON blob -- where the "affected line" is the whole document and
// every extra pass over it is megabytes of copying per character typed.
//
// Composing the replacement as `prefix + replacement + suffix` walked that line
// about three times over (into `prefix`, into `suffix`, into the temporary from
// the first `+`, then into the result) on top of the copy the undo entry needs.
// Bytes, not duration, so this cannot go flaky.
void TestTextViewportSingleLineEditAllocatesABoundedMultipleOfTheLine() {
#if MICROIDE_PERF_HARNESS_BUILD
  namespace perf = microide::tests::perf;
  constexpr std::size_t kLineBytes = 4u * 1024u * 1024u;
  microide::editor::TextViewport viewport;
  viewport.LoadContent(std::string(kLineBytes, 'x') + "\n", "/tmp/minified-edit.txt");
  viewport.MoveCursorTo(0, 100);

  // Warm: the first edit pays one-off costs (undo stack growth, cache priming).
  viewport.InsertText("a");

  const auto before = perf::Allocations::Snapshot();
  viewport.InsertText("b");
  const auto delta = perf::Allocations::DeltaSince(before);

  // Two line-sized copies are inherent to the current history model: the undo
  // entry keeps the pre-edit line, and the post-edit line has to be built. The
  // budget is 3x so a third full pass fails; it measured ~5x before.
  // Measured 5x on this path: the undo entry's pre-edit copy, the composed
  // post-edit line, the piece tree's replacement buffer and its add-buffer append,
  // and the undo run's coalesce. 6x leaves headroom for allocator rounding while
  // still failing the 10x and 13x this measured before the surrounding commits.
  // The remaining 5 are the whole-line history model itself, not stray copies.
  // Measured 1.00x in 9 allocations (4,195,125 bytes for a 4 MiB line). The one
  // remaining line-sized copy is the piece tree materializing the edited line into
  // its per-revision cache, which the renderer and the visual-width scan both need
  // as one contiguous view -- it is shared, not per-caller. Everything else on the
  // path is now proportional to the delta:
  //   * the undo entry stores {column, removed_text, inserted_text}, not the line
  //     before and after (TD-2026-08-05-131),
  //   * the piece tree splices bytes at two offsets instead of rebuilding the line
  //     into a replacement buffer and appending that to `add_`,
  //   * the typing run coalesces by appending to those two small strings,
  //   * the encoding upgrade scans the spliced-in bytes, not the rebuilt line.
  // 2x leaves room for allocator rounding while failing the moment a second full
  // pass over the line comes back. It measured 13x, then 5x, before this.
  const std::uint64_t budget = 2u * kLineBytes;
  Expect(static_cast<std::uint64_t>(delta.bytes_allocated) < budget,
         "a one-character edit must cost its delta plus the one shared line "
         "materialization, not a multiple of the line");
#endif
}

// TD-2026-08-05-132 item 2: a keystroke inside a line must not re-measure that
// line's visual width. It does not have to: a line already known to be plain
// ASCII (no tab, no byte >= 0x80) stays plain exactly when the spliced-in text is,
// and a plain line's width is its byte count -- so the new width follows from the
// splice without reading the line at all.
//
// The correctness half is the interesting half: the derivation is only valid while
// both halves of that precondition hold, so this pins the width against a
// from-scratch measurement across the transitions that break it (a tab typed in, a
// multibyte code point typed in, and undo/redo back across both).
void TestTextViewportInlineEditDerivesLineWidthFromTheSplice() {
  using microide::editor::TextLayout;
  microide::editor::TextViewport viewport;
  viewport.LoadContent("plain ascii line\nsecond\n", "/tmp/inline-width.txt");
  const std::size_t tab_size = viewport.tab_size();

  // The width the cache reports must always equal a fresh measurement of the
  // widest line -- that is the whole contract of the incremental table.
  const auto expect_width_matches_a_fresh_measure = [&](const char* what) {
    std::size_t widest = 0;
    for (std::size_t i = 0; i < viewport.line_count(); ++i) {
      widest = std::max(widest, TextLayout::MeasureLineFacts(viewport.lines()[i], tab_size)
                                    .visual_columns);
    }
    Expect(viewport.max_visual_columns() == widest, what);
  };

  expect_width_matches_a_fresh_measure("the initial width table matches a fresh measure");

  viewport.MoveCursorTo(0, 5);
#ifndef NDEBUG
  const std::size_t derived_before = viewport.VisualColumnSpliceDerivedCountForDebug();
#endif
  viewport.InsertText("X");
  expect_width_matches_a_fresh_measure("a plain-ASCII insert keeps the width table exact");
#ifndef NDEBUG
  Expect(viewport.VisualColumnSpliceDerivedCountForDebug() > derived_before,
         "a plain-ASCII insert into a plain-ASCII line derives its width from the splice");
  const std::size_t derived_after_ascii = viewport.VisualColumnSpliceDerivedCountForDebug();
#endif

  // A tab breaks the precondition on the way in: the width must come from a real
  // measurement, and every later edit on that line must keep doing so.
  viewport.InsertText("\t");
  expect_width_matches_a_fresh_measure("a tab typed into a plain line re-measures the line");
#ifndef NDEBUG
  Expect(viewport.VisualColumnSpliceDerivedCountForDebug() == derived_after_ascii,
         "inserting a tab must not take the splice-derived path");
#endif
  viewport.InsertText("y");
  expect_width_matches_a_fresh_measure("a line carrying a tab keeps re-measuring");
#ifndef NDEBUG
  Expect(viewport.VisualColumnSpliceDerivedCountForDebug() == derived_after_ascii,
         "a line that is no longer plain ASCII must not take the splice-derived path");
#endif

  // A multibyte code point breaks it the other way: bytes stop being cells.
  Expect(viewport.Undo(), "undo the trailing character");
  Expect(viewport.Undo(), "undo the tab");
  expect_width_matches_a_fresh_measure("undoing back to a plain line restores its width");
  viewport.InsertText("\xC3\xA9");
  expect_width_matches_a_fresh_measure("a multibyte insert re-measures the line");
  Expect(viewport.Undo(), "undo the multibyte insert");
  expect_width_matches_a_fresh_measure("undoing a multibyte insert restores the plain width");
  Expect(viewport.Redo(), "redo the multibyte insert");
  expect_width_matches_a_fresh_measure("redoing a multibyte insert re-measures the line");
}

// The caret's byte column <-> visual column conversions answer in O(1) on a
// plain-ASCII line by reading the layout cache's per-line facts instead of walking
// the line (TD-2026-08-05-132). That shortcut is only correct while the facts are
// current, and the facts are only present once something has populated the width
// table -- so the two paths must agree whether the table is cold or warm, and
// across the edits that change a line's class.
void TestTextViewportCaretColumnConversionsMatchTheDirectWalk() {
  using microide::editor::TextLayout;
  const std::vector<std::string> rows = {
      "plain ascii row",           // one cell per byte
      "tabbed\tcolumns\there",     // tab stops
      "caf\xC3\xA9 \xE4\xB8\xAD",  // multibyte
      "",                          // empty
  };
  std::string content;
  for (const std::string& row : rows) {
    content += row;
    content += '\n';
  }

  const auto check_every_column = [&](microide::editor::TextViewport& viewport,
                                      const char* context) {
    for (std::size_t line = 0; line < viewport.line_count(); ++line) {
      const std::string_view text = viewport.lines()[line];
      for (std::size_t column = 0; column <= text.size() + 2; ++column) {
        Expect(viewport.VisualColumnAt(line, column) ==
                   TextLayout::VisualColumnForTextColumn(text, column, viewport.tab_size()),
               std::string("VisualColumnAt matches the direct walk (") + context + ")");
        Expect(viewport.TextColumnAtVisualColumn(line, column) ==
                   TextLayout::TextColumnForVisualColumn(text, column, viewport.tab_size()),
               std::string("TextColumnAtVisualColumn matches the direct walk (") + context + ")");
      }
    }
  };

  {
    // Cold: nothing has built the width table yet, so every query must fall back
    // to the walk and still be right.
    microide::editor::TextViewport viewport;
    viewport.LoadContent(content, "/tmp/caret-columns.txt");
    check_every_column(viewport, "cold width table");
  }

  microide::editor::TextViewport viewport;
  viewport.LoadContent(content, "/tmp/caret-columns.txt");
  (void)viewport.max_visual_columns();  // populate the per-line facts
  check_every_column(viewport, "warm width table");

  // Typing a tab into the plain row moves it out of the plain class; typing a
  // multibyte code point does the same by a different route. Both must be
  // reflected before the next conversion, or the caret lands on the wrong cell.
  viewport.MoveCursorTo(0, 5);
  viewport.InsertText("\t");
  check_every_column(viewport, "after a tab is typed into a plain row");
  viewport.InsertText("\xC3\xA9");
  check_every_column(viewport, "after a multibyte code point is typed");
  Expect(viewport.Undo(), "undo the multibyte insert");
  Expect(viewport.Undo(), "undo the tab insert");
  check_every_column(viewport, "after undoing back to the plain row");
}

// Typing deep inside a long plain-ASCII line must not walk the line to work out
// where the caret is -- not once, from any of the caret paths that run per
// keystroke and per rendered row (TD-2026-08-05-132).
//
// The correctness test above cannot see this: a walk and a lookup return the same
// answer. `editor.visual_column_walk_bytes` counts what the fallback actually
// read, so "no walk happened" is assertable, and the number is the blind spot's
// own units -- bytes of line re-read per keystroke.
void TestTextViewportTypingInALongLineDoesNotWalkItForCaretColumns() {
  constexpr std::size_t kLineBytes = 256u * 1024u;
  microide::editor::TextViewport viewport;
  viewport.LoadContent(std::string(kLineBytes, 'x') + "\n", "/tmp/long-line-caret.txt");
  viewport.SetViewportSize(40, 120);
  viewport.MoveCursorTo(0, kLineBytes / 2, false);
  // Warm the width table the way a rendered frame does.
  (void)viewport.max_visual_columns();
  (void)viewport.CaretForLine(0);

  const std::uint64_t before =
      util::ReadPerformanceCounter(util::PerfCounterId::EditorVisualColumnWalkBytes);
  for (int i = 0; i < 8; ++i) {
    viewport.InsertText("y");
    // What a frame asks for: the caret's row decoration and its scroll clamp.
    (void)viewport.CaretForLine(0);
    (void)viewport.cursor_visual_column();
  }
  const std::uint64_t walked =
      util::ReadPerformanceCounter(util::PerfCounterId::EditorVisualColumnWalkBytes) - before;
  Expect(walked == 0,
         "typing in a long plain-ASCII line must not re-walk it for caret columns (walked " +
             std::to_string(walked) + " bytes)");

  // And the fallback is still reachable -- a counter that can never move is not
  // evidence. A tab makes the line non-plain, and the very next conversion walks.
  viewport.InsertText("\t");
  const std::uint64_t before_tabbed =
      util::ReadPerformanceCounter(util::PerfCounterId::EditorVisualColumnWalkBytes);
  (void)viewport.cursor_visual_column();
  Expect(util::ReadPerformanceCounter(util::PerfCounterId::EditorVisualColumnWalkBytes) >
             before_tabbed,
         "a line that is no longer plain ASCII must fall back to the walk");
}

void TestTextViewportLanguageIdMemoIsAllocationFreeOnASettledBuffer() {
  namespace perf = microide::tests::perf;
  microide::editor::TextViewport viewport;
  viewport.LoadContent("int main() { return 0; }\n", "/tmp/memo-settled.cpp");

  // First resolve is the cold one (head scan + owned string).
  Expect(viewport.language_id() == "c++", "cold resolve should detect the C++ definition");

  // Every later ask about an unchanged buffer must be a pure field compare.
  // Before the memo moved onto the viewport, ApplyEditorPreferences re-detected
  // the filetype of every open tab on every settings change (TD-2026-08-03-110).
  const auto before = perf::Allocations::Snapshot();
  for (int i = 0; i < 64; ++i) {
    Expect(viewport.language_id() == "c++", "warm resolve should return the memoized filetype");
  }
  const auto delta = perf::Allocations::DeltaSince(before);
  Expect(delta.allocations == 0, "a memo hit must not allocate");
}

void TestTextViewportLanguageIdMemoInvalidatesOnContentChange() {
  using microide::editor::runtime_syntax::DetectFiletype;
  // No extension, so detection depends entirely on the shebang line. The memo
  // this replaced (LspUiState::language_cache_*) was keyed on the path alone, so
  // it pinned the first answer forever and an edited shebang kept reporting the
  // stale language to every LSP/provider caller.
  const std::filesystem::path path = "/tmp/memo-shebang";
  const std::vector<std::string> shell_lines = {"#!/bin/sh", "echo hi"};
  const std::vector<std::string> python_lines = {"#!/usr/bin/env python3", "print('hi')"};
  const std::string shell_id = DetectFiletype(path, shell_lines);
  const std::string python_id = DetectFiletype(path, python_lines);
  // Guard the guard: if shebang detection ever stops distinguishing these, the
  // rest of the test would pass vacuously.
  Expect(!shell_id.empty() && shell_id != python_id,
         "the two shebangs must detect as different languages for this test to mean anything");

  microide::editor::TextViewport viewport;
  viewport.LoadContent("#!/bin/sh\necho hi\n", path);
  Expect(viewport.language_id() == shell_id, "shebang should drive the initial detection");

  viewport.LoadContent("#!/usr/bin/env python3\nprint('hi')\n", path);
  Expect(viewport.language_id() == python_id,
         "a content change must re-detect; a path-keyed cache would pin the first answer");
}

void TestTextViewportLanguageIdMemoInvalidatesOnPathChange() {
  using microide::editor::runtime_syntax::DetectFiletype;
  const std::vector<std::string> lines = {"value = 1"};
  const std::string python_id = DetectFiletype("/tmp/memo-rename.py", lines);
  const std::string lua_id = DetectFiletype("/tmp/memo-rename.lua", lines);
  Expect(!python_id.empty() && python_id != lua_id,
         "the two extensions must detect as different languages for this test to mean anything");

  microide::editor::TextViewport viewport;
  viewport.LoadContent("value = 1\n", "/tmp/memo-rename.py");
  Expect(viewport.language_id() == python_id, "extension should drive the initial detection");

  viewport.SetPath("/tmp/memo-rename.lua");
  Expect(viewport.language_id() == lua_id, "a rename must re-detect from the new extension");
}

void TestRuntimeSyntaxInitialStateAllocationIsDocumentSizeIndependent() {
  using microide::editor::SyntaxHighlighter;
  using microide::editor::TextBuffer;
  namespace perf = microide::tests::perf;

  // InitialState only inspects a bounded head (kSignatureDetectLineLimit lines),
  // so its allocation cost must be independent of document size. The previous
  // implementation snapshot-copied the whole document (one heap allocation per
  // line) before detection -- this guards against reintroducing that.
  const std::filesystem::path path = "/tmp/initial-state-alloc.txt";

  const auto make_buffer = [](std::size_t line_count) {
    std::string content;
    content.reserve(line_count * 40);
    for (std::size_t i = 0; i < line_count; ++i) {
      content += "the quick brown fox jumps over the lazy\n";
    }
    TextBuffer buffer;
    buffer.ResetFromText(std::move(content));
    return buffer;
  };

  const TextBuffer small = make_buffer(1000);
  const TextBuffer large = make_buffer(100000);

  // Warm one-time registry init so it does not skew the measured deltas.
  (void)SyntaxHighlighter::InitialState(path, small);

  const auto before_small = perf::Allocations::Snapshot();
  (void)SyntaxHighlighter::InitialState(path, small);
  const auto small_delta = perf::Allocations::DeltaSince(before_small);

  const auto before_large = perf::Allocations::Snapshot();
  (void)SyntaxHighlighter::InitialState(path, large);
  const auto large_delta = perf::Allocations::DeltaSince(before_large);

  // The 100x-larger document must not cost ~100x the bytes. A whole-document
  // materialization of the large buffer would allocate megabytes (>> head);
  // generous additive slack still catches that by orders of magnitude.
  Expect(large_delta.bytes_allocated < small_delta.bytes_allocated + 64 * 1024,
         "InitialState bytes must not scale with document size (bounded head only)");
  Expect(large_delta.allocations < small_delta.allocations + 256,
         "InitialState allocation count must not scale with document size");
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

// Regression: a syntax definition file with a runaway top-level loop must not
// hang the reload/startup path. The instruction-count hook turns it into a clean
// load error. (Without the hook this test would loop forever.)
void TestRuntimeSyntaxLoaderBoundsInfiniteLoop() {
#if !MICROIDE_HAS_LUA_PLUGINS
  return;
#endif
  ScopedRuntimeSyntaxRegistryReset syntax_reset;
  TemporaryDirectory temp_dir;
  const std::filesystem::path syntax_dir = temp_dir.path() / "syntax";
  WriteFile(syntax_dir / "loop.lua",
            "local n = 0\nwhile true do n = n + 1 end\n"
            "return { filetype = \"loopy\", files = { \"\\\\.loopy$\" } }\n");

  std::vector<std::string> loader_errors;
  const auto definitions =
      microide::editor::runtime_syntax::LoadDefinitionsFromDirectories({syntax_dir}, &loader_errors);
  Expect(definitions.empty(), "an infinite-loop syntax file loads no definitions");
  Expect(!loader_errors.empty(),
         "the instruction budget surfaces a load error instead of hanging");
}

// Regression: a syntax rule that matches single characters on a long (but under
// the overlong cap) line must not produce an unbounded match list. FindAllRegex
// caps matches per rule per line (8192), so highlighting stays bounded: the first
// matches are colored, bytes past the budget fall back to Plain, and the
// one-token-per-byte contract holds.
void TestRuntimeSyntaxMatchBudgetBoundsPathologicalRule() {
#if !MICROIDE_HAS_LUA_PLUGINS
  return;
#endif
  ScopedRuntimeSyntaxRegistryReset syntax_reset;
  TemporaryDirectory temp_dir;
  const std::filesystem::path syntax_dir = temp_dir.path() / "syntax";
  // A rule whose pattern matches every single 'a' — pathological match density.
  WriteFile(syntax_dir / "dense.lua",
            R"(return {
  filetype = "dense",
  files = { "\\.dense$" },
  rules = {
    { pattern = "a", group = "keyword" }
  }
}
)");
  std::vector<std::string> loader_errors;
  const auto definitions =
      microide::editor::runtime_syntax::LoadDefinitionsFromDirectories({syntax_dir}, &loader_errors);
  std::vector<std::string> reload_errors;
  microide::editor::runtime_syntax::ReloadDefinitions(definitions, &reload_errors);

  std::string line(20000, 'a');  // under the overlong cap, so rules DO run
  TextViewport viewport;
  viewport.LoadContent(line + "\n", "/tmp/dense.dense");
  const auto& tokens = viewport.HighlightedLineTokens(0);
  // Contract preserved and highlighting completed (no hang) despite 20k potential matches.
  Expect(tokens.size() == line.size(), "one token per byte is preserved on a dense-match line");
  Expect(tokens.front() == SyntaxTokenKind::Keyword,
         "the first matches within the budget are highlighted");
  Expect(tokens.back() == SyntaxTokenKind::Plain,
         "bytes past the per-line match budget fall back to Plain instead of matching unbounded");
}

// Regression: a `^`-anchored pattern rule must match only at a TRUE line start,
// not at the head of a mid-line segment (the tail after a region on the same
// line). PCRE2_NOTBOL is passed for mid-line segments so `^foo` colours only the
// leading `foo`, never the `foo` that follows a region on the same line.
void TestRuntimeSyntaxCaretAnchorHonorsTrueLineStart() {
#if !MICROIDE_HAS_LUA_PLUGINS
  return;
#endif
  ScopedRuntimeSyntaxRegistryReset syntax_reset;
  TemporaryDirectory temp_dir;
  const std::filesystem::path syntax_dir = temp_dir.path() / "syntax";
  WriteFile(syntax_dir / "caret.lua",
            R"LUA(return {
  filetype = "caret",
  files = { "\\.caret$" },
  rules = {
    { pattern = "^foo", group = "keyword" },
    { start = "\\(", ["end"] = "\\)", group = "string" }
  }
}
)LUA");
  std::vector<std::string> loader_errors;
  const auto definitions =
      microide::editor::runtime_syntax::LoadDefinitionsFromDirectories({syntax_dir}, &loader_errors);
  Expect(loader_errors.empty(), "the caret syntax definition should load");
  std::vector<std::string> reload_errors;
  microide::editor::runtime_syntax::ReloadDefinitions(definitions, &reload_errors);
  Expect(reload_errors.empty(), "the caret syntax definition should reload cleanly");

  // Layout: foo ( x ) foo  -> a region splits the line into two `foo` segments.
  TextViewport viewport;
  viewport.LoadContent("foo(x)foo\n", "/tmp/sample.caret");
  const auto& tokens = viewport.HighlightedLineTokens(0);
  Expect(tokens.size() == 9, "one token per byte");
  // The leading foo (bytes 0..2) is at a true line start -> Keyword.
  Expect(tokens[0] == SyntaxTokenKind::Keyword && tokens[1] == SyntaxTokenKind::Keyword &&
             tokens[2] == SyntaxTokenKind::Keyword,
         "^foo matches the leading foo at the true line start");
  // The trailing foo (bytes 6..8) follows a region mid-line -> NOT Keyword.
  Expect(tokens[6] != SyntaxTokenKind::Keyword && tokens[7] != SyntaxTokenKind::Keyword &&
             tokens[8] != SyntaxTokenKind::Keyword,
         "^foo must NOT match the foo after a mid-line region (PCRE2_NOTBOL)");
}

// A pathologically long line (a minified bundle with no newline) must not be
// tokenized synchronously on the UI thread: highlighting scans the whole line
// with the syntax rules, O(line) work on every token-cache miss. Over the length
// cap the line renders unhighlighted (all Plain), while the exact
// one-token-per-byte contract is preserved so downstream indexing stays safe.
void TestSyntaxHighlightSkipsOverlongLine() {
#if !MICROIDE_HAS_LUA_PLUGINS
  return;
#endif
  ScopedRuntimeSyntaxRegistryReset syntax_reset;
  TemporaryDirectory temp_dir;
  const std::filesystem::path syntax_dir = temp_dir.path() / "syntax";
  WriteFile(syntax_dir / "todo.lua",
            R"(return {
  filetype = "todo",
  files = { "\\.todo$" },
  rules = {
    { pattern = "\\b(TODO|DONE)\\b", group = "keyword" }
  }
}
)");
  std::vector<std::string> loader_errors;
  const auto definitions =
      microide::editor::runtime_syntax::LoadDefinitionsFromDirectories({syntax_dir}, &loader_errors);
  std::vector<std::string> reload_errors;
  microide::editor::runtime_syntax::ReloadDefinitions(definitions, &reload_errors);

  // Control: a short line with the keyword is highlighted, proving the definition
  // works and that only length disables it below.
  {
    TextViewport shortv;
    shortv.LoadContent("TODO here\n", "/tmp/short.todo");
    const auto& tokens = shortv.HighlightedLineTokens(0);
    Expect(std::any_of(tokens.begin(), tokens.end(),
                       [](SyntaxTokenKind k) { return k == SyntaxTokenKind::Keyword; }),
           "a short line with the keyword should highlight");
  }

  // A >100 KB line beginning with the keyword must NOT be tokenized. It renders
  // all-Plain, and it does so by returning NO tokens rather than one Plain token
  // per byte: every consumer already reads an absent entry as Plain (the same
  // convention HighlightedLineTokens uses when highlighting is off), so filling
  // the vector only made declining to tokenize cost what tokenizing would have —
  // an 8 MB alloc + memset per cache miss on a minified file, then held in the
  // per-line LRU.
  std::string huge = "TODO ";
  huge.append(300000, 'x');
  TextViewport bigv;
  bigv.LoadContent(huge + "\n", "/tmp/big.todo");
  const auto& tokens = bigv.HighlightedLineTokens(0);
  Expect(tokens.empty(),
         "an overlong line must not materialize a token per byte to say 'unhighlighted'");
  Expect(std::all_of(tokens.begin(), tokens.end(),
                     [](SyntaxTokenKind k) { return k == SyntaxTokenKind::Plain; }),
         "an overlong line must be left unhighlighted (all Plain)");

  // The cutoff is per LINE, not per document: a short line in the same buffer is
  // still tokenized. Without this, the assertion above would also pass if the
  // whole file had silently stopped highlighting.
  TextViewport mixedv;
  mixedv.LoadContent(huge + "\nTODO here\n", "/tmp/mixed.todo");
  Expect(mixedv.HighlightedLineTokens(0).empty(),
         "the overlong line stays untokenized when a short line follows it");
  const auto& short_tokens = mixedv.HighlightedLineTokens(1);
  Expect(std::any_of(short_tokens.begin(), short_tokens.end(),
                     [](SyntaxTokenKind k) { return k == SyntaxTokenKind::Keyword; }),
         "a short line sharing the buffer with an overlong one must still highlight");
}

void TestSyntaxHighlightNestedRegionResumesParentScope() {
#if !MICROIDE_HAS_LUA_PLUGINS
  return;
#endif
  ScopedRuntimeSyntaxRegistryReset syntax_reset;
  TemporaryDirectory temp_dir;
  const std::filesystem::path syntax_dir = temp_dir.path() / "syntax";
  WriteFile(syntax_dir / "nest.lua",
            R"LUA(return {
  filetype = "nest",
  files = { "\\.nest$" },
  rules = {
    {
      start = "\\(",
      ["end"] = "\\)",
      group = "comment",
      rules = {
        { start = "\\[", ["end"] = "\\]", group = "string" }
      }
    }
  }
}
)LUA");

  std::vector<std::string> loader_errors;
  const auto definitions =
      microide::editor::runtime_syntax::LoadDefinitionsFromDirectories({syntax_dir}, &loader_errors);
  Expect(loader_errors.empty(), "nested-region syntax should load");
  std::vector<std::string> reload_errors;
  microide::editor::runtime_syntax::ReloadDefinitions(definitions, &reload_errors);
  Expect(reload_errors.empty(), "nested-region syntax should reload");

  using microide::editor::SyntaxHighlighter;
  using microide::editor::SyntaxState;
  const std::filesystem::path path = "/tmp/regions.nest";

  const std::vector<std::string> no_lines;
  SyntaxState state = SyntaxHighlighter::InitialState(path, no_lines);
  // AdvanceState must agree with HighlightLine's end_state at every step.
  const auto step = [&](std::string_view text) {
    const auto highlighted = SyntaxHighlighter::HighlightLine(text, path, state);
    Expect(SyntaxHighlighter::AdvanceState(text, path, state) == highlighted.end_state,
           "AdvanceState should match HighlightLine end_state for nested regions");
    state = highlighted.end_state;
    return highlighted;
  };

  step("(outer");
  Expect(state.region_depth == 1, "entering the outer region pushes one stack frame");

  step("[inner");
  Expect(state.region_depth == 2, "entering the nested region pushes a second stack frame");

  const auto fully_inside = step("still");
  Expect(state.region_depth == 2,
         "a line wholly inside the nested region keeps both stack frames");
  Expect(std::all_of(fully_inside.tokens.begin(), fully_inside.tokens.end(),
                     [](SyntaxTokenKind kind) { return kind == SyntaxTokenKind::String; }),
         "a line wholly inside the nested string region stays string-colored");

  const auto closes_inner = step("]tail");
  Expect(state.region_depth == 1,
         "closing the nested region returns to the parent region, not the top level");
  Expect(closes_inner.tokens.size() == 5 &&
             closes_inner.tokens[1] == SyntaxTokenKind::Comment &&
             closes_inner.tokens[4] == SyntaxTokenKind::Comment,
         "text after the nested region closes should color as the enclosing parent region");

  step(")done");
  Expect(state.region_depth == 0, "closing the outer region returns to the top level");
}

// Built-in language rule regexes are compiled lazily on first highlight of that
// definition. Two threads highlighting the same never-yet-highlighted built-in
// language must not race: EnsureDefinitionCompiled's per-definition call_once
// serializes the compile. Run under TSan to catch a data race on the mutable
// per-rule CompiledRegex fields.
void TestRuntimeSyntaxLazyCompileIsThreadSafeForSameLanguage() {
  using microide::editor::SyntaxHighlighter;
  ScopedRuntimeSyntaxRegistryReset syntax_reset;
  // Fresh registry clone with uncompiled (lazy) built-in definitions and fresh
  // per-definition guards, so the compile genuinely happens under contention.
  microide::editor::runtime_syntax::ReloadDefinitions({});

  const std::filesystem::path path = "/tmp/lazy_race.cpp";
  const std::string line = "int value = 42; // note";

  constexpr int kThreads = 8;
  constexpr int kIterations = 200;
  std::atomic<bool> go{false};
  std::atomic<int> non_plain_seen{0};
  std::vector<std::thread> workers;
  workers.reserve(kThreads);
  for (int t = 0; t < kThreads; ++t) {
    workers.emplace_back([&]() {
      while (!go.load(std::memory_order_acquire)) {
      }
      for (int i = 0; i < kIterations; ++i) {
        const auto highlighted = SyntaxHighlighter::HighlightLine(line, path, {});
        Expect(highlighted.tokens.size() == line.size(),
               "lazy-compiled highlight should still return one token per byte");
        if (std::any_of(highlighted.tokens.begin(), highlighted.tokens.end(),
                        [](SyntaxTokenKind kind) { return kind != SyntaxTokenKind::Plain; })) {
          non_plain_seen.fetch_add(1, std::memory_order_relaxed);
        }
      }
    });
  }
  go.store(true, std::memory_order_release);
  for (auto& worker : workers) {
    worker.join();
  }
  Expect(non_plain_seen.load() == kThreads * kIterations,
         "every concurrent highlight of a built-in language should produce syntax tokens");
}

// After a plugin reload, the built-in definitions live in a fresh registry
// clone (still lazy) with their own guard table. Highlighting a built-in
// language before and after a reload must both compile and colorize correctly.
void TestRuntimeSyntaxLazyCompileSurvivesReload() {
  using microide::editor::SyntaxHighlighter;
  ScopedRuntimeSyntaxRegistryReset syntax_reset;

  const std::filesystem::path path = "/tmp/reload_lazy.cpp";
  const std::string line = "int value = 42;";
  const auto has_syntax = [](const microide::editor::HighlightedLine& highlighted) {
    return std::any_of(highlighted.tokens.begin(), highlighted.tokens.end(),
                       [](SyntaxTokenKind kind) { return kind != SyntaxTokenKind::Plain; });
  };

  Expect(has_syntax(SyntaxHighlighter::HighlightLine(line, path, {})),
         "built-in C++ should colorize before any plugin reload");

  // A valid, in-code plugin definition for an unrelated filetype forces the
  // mutable-registry clone path (built-in defs copied in, lazy, fresh guards).
  microide::editor::runtime_syntax::RuntimeSyntaxDefinitionData plugin;
  plugin.filetype = "lazyfixture";
  plugin.filename_patterns = {"\\.lazyfixture$"};
  microide::editor::runtime_syntax::RuntimeSyntaxRuleData rule;
  rule.kind = microide::editor::runtime_syntax::GeneratedRuleKind::Pattern;
  rule.group_name = "keyword";
  rule.pattern = "\\bTODO\\b";
  plugin.rules.push_back(rule);
  std::vector<std::string> reload_errors;
  microide::editor::runtime_syntax::ReloadDefinitions({plugin}, &reload_errors);
  Expect(reload_errors.empty(), "valid plugin definition should reload without errors");

  Expect(has_syntax(SyntaxHighlighter::HighlightLine(line, path, {})),
         "built-in C++ should still colorize after a plugin reload (lazy compile in the clone)");
}

// Plugin/runtime definitions stay eagerly compiled, so a malformed plugin regex
// must surface as a reload error at reload time (not be silently deferred).
void TestRuntimeSyntaxBadPluginRegexReportsErrorEagerly() {
  ScopedRuntimeSyntaxRegistryReset syntax_reset;

  microide::editor::runtime_syntax::RuntimeSyntaxDefinitionData plugin;
  plugin.filetype = "badfixture";
  plugin.filename_patterns = {"\\.badfixture$"};
  microide::editor::runtime_syntax::RuntimeSyntaxRuleData rule;
  rule.kind = microide::editor::runtime_syntax::GeneratedRuleKind::Pattern;
  rule.group_name = "keyword";
  rule.pattern = "([unterminated";  // invalid: unbalanced group/class
  plugin.rules.push_back(rule);

  std::vector<std::string> reload_errors;
  microide::editor::runtime_syntax::ReloadDefinitions({plugin}, &reload_errors);
  Expect(!reload_errors.empty(),
         "an invalid plugin regex should be reported at reload time (runtime defs stay eager)");
}

// A cold (never-highlighted) registry must still resolve built-in multi-line
// regions: EnsureDefinitionCompiled has to run before the region start/end
// .valid() checks, or the region rules would be silently skipped.
void TestRuntimeSyntaxLazyCompileColorsBuiltInBlockComment() {
  using microide::editor::SyntaxHighlighter;
  using microide::editor::SyntaxState;
  ScopedRuntimeSyntaxRegistryReset syntax_reset;
  microide::editor::runtime_syntax::ReloadDefinitions({});  // cold, uncompiled clone

  const std::filesystem::path path = "/tmp/block_comment.c";
  const std::vector<std::string> no_lines;
  SyntaxState state = SyntaxHighlighter::InitialState(path, no_lines);

  const auto open = SyntaxHighlighter::HighlightLine("/* opening comment", path, state);
  state = open.end_state;
  Expect(state.region_depth >= 1, "a C block-comment open should enter a multi-line region");

  const auto middle = SyntaxHighlighter::HighlightLine("still inside the comment", path, state);
  Expect(std::all_of(middle.tokens.begin(), middle.tokens.end(),
                     [](SyntaxTokenKind kind) { return kind == SyntaxTokenKind::Comment; }),
         "a line wholly inside a C block comment should be comment-colored from a cold registry");
}

// Exercises lazy compilation across many built-in definitions: each first
// highlight compiles a different definition. Guards against a definition whose
// deferred compile fails or is skipped (recovers the eager-validation sweep
// startup used to perform implicitly).
void TestRuntimeSyntaxLazyCompileSweepsCommonLanguages() {
  using microide::editor::SyntaxHighlighter;
  ScopedRuntimeSyntaxRegistryReset syntax_reset;
  microide::editor::runtime_syntax::ReloadDefinitions({});

  struct Sample {
    const char* path;
    const char* line;
  };
  const Sample samples[] = {
      {"/tmp/a.cpp", "int value = 42; // c++"},
      {"/tmp/a.c", "int value = 42; /* c */"},
      {"/tmp/a.py", "def f(x): return x + 1  # python"},
      {"/tmp/a.js", "const x = 42; // javascript"},
      {"/tmp/a.ts", "const x: number = 42; // typescript"},
      {"/tmp/a.go", "func main() { return }"},
      {"/tmp/a.rs", "fn main() -> i32 { 42 }"},
      {"/tmp/a.java", "public class A { int x = 1; }"},
      {"/tmp/a.json", "{ \"key\": 42 }"},
      {"/tmp/a.yaml", "key: value  # yaml"},
      {"/tmp/a.lua", "local x = 42 -- lua"},
      {"/tmp/a.md", "# Heading with `code`"},
      {"/tmp/a.sh", "echo \"hello\" # shell"},
      {"/tmp/a.rb", "def f; 42; end # ruby"},
      {"/tmp/a.toml", "key = 42 # toml"},
  };
  for (const Sample& sample : samples) {
    const std::string line = sample.line;
    const auto highlighted = SyntaxHighlighter::HighlightLine(line, sample.path, {});
    Expect(highlighted.tokens.size() == line.size(),
           "lazy compile should return one token per byte for every built-in language");
    Expect(std::any_of(highlighted.tokens.begin(), highlighted.tokens.end(),
                       [](SyntaxTokenKind kind) { return kind != SyntaxTokenKind::Plain; }),
           "each sampled built-in language should produce at least one syntax token");
  }
}

void TestHighlightPrefetchInstallPopulatesCacheAndRespectsStaleness() {
  std::string content;
  for (int i = 0; i < 60; ++i) {
    content += "int value" + std::to_string(i) + " = " + std::to_string(i) + ";\n";
  }

  TextViewport viewport;
  viewport.LoadContent(content, "/tmp/prefetch.cpp");
  TextViewport reference;
  reference.LoadContent(content, "/tmp/prefetch-ref.cpp");

  // A range well below the viewport top is not in the LRU yet.
  Expect(viewport.HasHighlightPrefetchGap(40, 8), "uncached lines should report a prefetch gap");

  const auto request = viewport.BuildHighlightPrefetchRequest(40, 8);
  Expect(request.lines.size() == 8, "request should snapshot exactly the requested line range");
  Expect(request.content_revision == viewport.content_revision(),
         "request should capture the live content revision");

  const auto result = microide::editor::ComputeHighlightPrefetch(request);
  Expect(result.tokens.size() == 8 && result.end_states.size() == 8,
         "compute should tokenize every snapshot line");

  viewport.InstallPrefetchedHighlights(result);
  Expect(!viewport.HasHighlightPrefetchGap(40, 8),
         "installing prefetched highlights should fill the cache gap");
  Expect(viewport.HighlightedLineTokens(42) == reference.HighlightedLineTokens(42),
         "prefetched tokens must match a synchronous highlight of the same line");

  // A result whose revision no longer matches the document must be discarded.
  auto stale = microide::editor::ComputeHighlightPrefetch(
      viewport.BuildHighlightPrefetchRequest(10, 4));
  stale.content_revision = viewport.content_revision() + 1;
  Expect(viewport.HasHighlightPrefetchGap(10, 4), "the line-10 region should start uncached");
  viewport.InstallPrefetchedHighlights(stale);
  Expect(viewport.HasHighlightPrefetchGap(10, 4),
         "a stale-revision prefetch result should be dropped, leaving the gap intact");
}

void TestHighlightPrefetchServiceTokenizesOnWorkerThread() {
  std::string content;
  for (int i = 0; i < 40; ++i) {
    content += "int v" + std::to_string(i) + " = " + std::to_string(i) + ";\n";
  }
  TextViewport viewport;
  viewport.LoadContent(content, "/tmp/prefetch-service.cpp");

  microide::editor::HighlightPrefetchService service;
  std::atomic<int> wakes{0};
  service.SetWakeCallback([&]() { wakes.fetch_add(1); });

  Expect(viewport.HasHighlightPrefetchGap(30, 6), "the target range should start uncached");
  service.Request(viewport.BuildHighlightPrefetchRequest(30, 6));

  std::vector<microide::editor::HighlightPrefetchResult> results;
  for (int attempt = 0; attempt < 400 && results.empty(); ++attempt) {
    results = service.DrainResults();
    if (results.empty()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
  }
  service.Shutdown();

  Expect(results.size() == 1, "the worker should produce exactly one result");
  Expect(wakes.load() >= 1, "the worker should fire the wake callback");
  viewport.InstallPrefetchedHighlights(results.front());
  Expect(!viewport.HasHighlightPrefetchGap(30, 6),
         "installing the worker result should fill the cache gap");
}

void TestHighlightCheckpointBackfillServiceRunsOnWorkerThread() {
  std::string content;
  for (int i = 0; i < 4000; ++i) {
    if (i == 5) {
      content += "/* open block comment";
    } else if (i == 3000) {
      content += "close */ int x = 1;";
    } else {
      content += "payload payload";
    }
    content.push_back('\n');
  }
  TextViewport viewport;
  viewport.LoadContent(content, "/tmp/checkpoint-service.cpp");
  (void)viewport.HighlightedLineTokens(0);

  microide::editor::HighlightPrefetchService service;
  std::atomic<int> wakes{0};
  service.SetWakeCallback([&]() { wakes.fetch_add(1); });

  // A deep cold jump arms an off-thread checkpoint backfill (the freeze fix path).
  (void)viewport.HighlightedLineTokens(2000);
  auto request = viewport.TakeHighlightCheckpointBackfillRequest();
  Expect(request.has_value(), "a deep jump should arm a checkpoint backfill request");
  service.RequestCheckpoints(std::move(*request));

  std::vector<microide::editor::HighlightCheckpointResult> results;
  for (int attempt = 0; attempt < 400 && results.empty(); ++attempt) {
    results = service.DrainCheckpointResults();
    if (results.empty()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
  }
  service.Shutdown();

  Expect(results.size() == 1, "the worker should produce exactly one checkpoint result");
  Expect(wakes.load() >= 1, "the checkpoint worker should fire the wake callback");
  viewport.InstallHighlightCheckpoints(results.front());

  // Line 2000 sits inside the multiline comment; after the backfill installs the
  // chain, the deep query resolves to the exact comment state (not approximate).
  const auto& inside = viewport.HighlightedLineTokens(2000);
  Expect(!inside.empty(), "deep query inside the comment should produce tokens");
  Expect(std::all_of(inside.begin(), inside.end(),
                     [](SyntaxTokenKind k) { return k == SyntaxTokenKind::Comment; }),
         "installed worker-computed checkpoints should yield exact multiline comment state");
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

// --- Phase 4: large-file direct-load fast path (no split/rejoin round-trip) ---

std::filesystem::path WriteScratchFile(const std::string& name, std::string_view bytes) {
  const std::filesystem::path path =
      std::filesystem::temp_directory_path() / ("microide-phase4-" + name);
  Expect(microide::util::WriteTextFileAtomically(path, bytes),
         "scratch fixture should write");
  return path;
}

void TestTextViewportFastLoadMatchesCrlfDecode() {
  // An LF file and a CRLF file with identical logical lines must open to the
  // same buffer content and line count -- the '\r'-free fast path and the
  // DecodeLines path are required to agree on everything but the line-ending
  // label.
  const std::string lf = "alpha\nbeta\ngamma\n";
  const std::string crlf = "alpha\r\nbeta\r\ngamma\r\n";
  TextViewport lf_view;
  TextViewport crlf_view;
  Expect(lf_view.OpenFile(WriteScratchFile("lf.txt", lf)), "LF file opens");
  Expect(crlf_view.OpenFile(WriteScratchFile("crlf.txt", crlf)), "CRLF file opens");

  Expect(lf_view.line_count() == crlf_view.line_count(),
         "fast path and decode path agree on line count");
  Expect(lf_view.line_count() == 4,
         "trailing newline yields a final empty line (alpha,beta,gamma,\"\")");
  for (std::size_t i = 0; i < lf_view.line_count(); ++i) {
    Expect(lf_view.lines().LineView(i) == crlf_view.lines().LineView(i),
           "fast path and decode path agree on every line's content");
  }
  Expect(lf_view.LineEndingLabel() == "LF", "LF file labels as LF");
  Expect(crlf_view.LineEndingLabel() == "CRLF", "CRLF file labels as CRLF");
}

void TestTextViewportFastLoadPreservesCrOnlyLines() {
  TextViewport view;
  Expect(view.OpenFile(WriteScratchFile("cr-only.txt", "alpha\rbeta\rgamma\r")),
         "CR-only file opens");
  Expect(view.line_count() == 4,
         "CR-only files should keep logical lines plus a trailing empty line");
  Expect(view.lines().LineView(0) == "alpha" && view.lines().LineView(1) == "beta" &&
             view.lines().LineView(2) == "gamma" && view.lines().LineView(3).empty(),
         "lone CR bytes should become line breaks, not disappear");
  Expect(view.LineEndingLabel() == "CR", "CR-only file labels as CR");
}

void TestTextViewportFastLoadDenseCrlfFile() {
  std::string content;
  constexpr int kLineCount = 100000;
  content.reserve(static_cast<std::size_t>(kLineCount) * 3);
  for (int i = 0; i < kLineCount; ++i) {
    content += "x\r\n";
  }

  TextViewport view;
  Expect(view.OpenFile(WriteScratchFile("dense-crlf.txt", content)),
         "dense CRLF file opens without line-vector amplification");
  Expect(view.line_count() == static_cast<std::size_t>(kLineCount + 1),
         "dense CRLF file should preserve every logical row plus the trailing empty line");
  Expect(view.lines().LineView(0) == "x" &&
             view.lines().LineView(static_cast<std::size_t>(kLineCount - 1)) == "x" &&
             view.lines().LineView(static_cast<std::size_t>(kLineCount)).empty(),
         "dense CRLF canonicalization should keep first, last, and trailing rows intact");
  Expect(view.LineEndingLabel() == "CRLF", "dense CRLF file labels as CRLF");
}

void TestTextViewportFastLoadNoTrailingNewline() {
  TextViewport view;
  Expect(view.OpenFile(WriteScratchFile("notrail.txt", "one\ntwo")),
         "file without trailing newline opens via fast path");
  Expect(view.line_count() == 2, "no trailing newline means no extra empty line");
  Expect(view.lines().LineView(0) == "one" && view.lines().LineView(1) == "two",
         "fast path preserves content with no trailing newline");
}

void TestTextViewportFastLoadEmptyFile() {
  TextViewport view;
  Expect(view.OpenFile(WriteScratchFile("empty.txt", "")),
         "empty file opens via fast path");
  Expect(view.line_count() == 1 && view.lines().LineView(0).empty(),
         "empty file is a single empty line");
}

void TestTextViewportFastLoadDetectsEncoding() {
  TextViewport utf8;
  Expect(utf8.OpenFile(WriteScratchFile("utf8.txt", "café\nrésumé\n")),
         "utf-8 file opens via fast path");
  Expect(utf8.EncodingLabel() == "UTF-8",
         "fast path classifies non-ASCII UTF-8 as UTF-8");

  // ASCII is UTF-8: the status bar must not flip its encoding readout the moment
  // the user types a non-ASCII character into an all-ASCII file.
  TextViewport ascii;
  Expect(ascii.OpenFile(WriteScratchFile("ascii.txt", "plain\ntext\n")),
         "ascii file opens via fast path");
  Expect(ascii.EncodingLabel() == "UTF-8",
         "an all-ASCII file reports UTF-8, not a separate ASCII encoding");

  TextViewport bytes;
  const std::string with_nul("a\0b\nc", 5);
  Expect(bytes.OpenFile(WriteScratchFile("nul.bin", with_nul)),
         "NUL-containing file (no '\\r') still opens via fast path");
  Expect(bytes.EncodingLabel() == "Bytes",
         "fast path classifies NUL bytes as Bytes");
  Expect(bytes.line_count() == 2, "NUL is in-line content, only '\\n' splits lines");
}

// Opening a file classifies its line endings and normalizes them to LF. Both
// halves grew fast paths -- the classifier answers an LF-only file from a single
// `memchr` without counting anything, and the normalizer copies the runs between
// carriage returns instead of one byte at a time -- so pin the results against a
// deliberately naive reference implementation over every arrangement of CR, LF
// and CRLF that a real file can present, including the ones that straddle the
// run boundaries: a lone CR at the very end (no byte follows to test for LF), a
// CRLF split so the LF starts the next run, and back-to-back CRs.
void TestTextViewportOpenNormalizesEveryLineEndingMix() {
  const auto reference_normalize = [](std::string_view content) {
    std::string out;
    for (std::size_t i = 0; i < content.size(); ++i) {
      if (content[i] != '\r') {
        out.push_back(content[i]);
        continue;
      }
      out.push_back('\n');
      if (i + 1 < content.size() && content[i + 1] == '\n') {
        ++i;
      }
    }
    return out;
  };

  const std::string kBodies[] = {
      "",         "a",          "abc",         "\n",       "\r",
      "\r\n",     "\n\r",       "\r\r",        "\r\r\n",   "\r\n\r",
      "a\r",      "a\n",        "a\r\nb",      "a\rb\nc",  "a\n\rb",
      "one\r\ntwo\r\nthree\r\n", "one\ntwo\nthree\n",      "one\rtwo\rthree\r",
      "mixed\r\nstyles\nhere\rend",
  };
  int index = 0;
  for (const std::string& body : kBodies) {
    const std::string name = "lineendmix" + std::to_string(index++) + ".txt";
    TextViewport view;
    Expect(view.OpenFile(WriteScratchFile(name, body)), "line-ending mix fixture opens");

    // The buffer must hold exactly the LF-normalized bytes, split on LF.
    const std::string expected_text = reference_normalize(body);
    std::string actual_text;
    for (std::size_t line = 0; line < view.line_count(); ++line) {
      if (line != 0) {
        actual_text.push_back('\n');
      }
      actual_text.append(view.lines().LineView(line));
    }
    Expect(actual_text == expected_text,
           "opening a file must produce exactly the LF-normalized content");

    // And the classifier's verdict must match a plain count of each style.
    std::size_t crlf = 0;
    std::size_t lf = 0;
    std::size_t cr = 0;
    for (std::size_t i = 0; i < body.size(); ++i) {
      if (body[i] == '\r') {
        if (i + 1 < body.size() && body[i + 1] == '\n') {
          ++crlf;
          ++i;
        } else {
          ++cr;
        }
      } else if (body[i] == '\n') {
        ++lf;
      }
    }
    util::LineEnding expected_ending = util::LineEnding::LF;
    if (crlf >= lf && crlf >= cr && crlf > 0) {
      expected_ending = util::LineEnding::CRLF;
    } else if (lf >= cr && lf > 0) {
      expected_ending = util::LineEnding::LF;
    } else if (cr > 0) {
      expected_ending = util::LineEnding::CR;
    }
    const bool expected_mixed =
        ((crlf > 0 ? 1 : 0) + (lf > 0 ? 1 : 0) + (cr > 0 ? 1 : 0)) > 1;
    Expect(view.line_ending() == expected_ending,
           "the dominant line ending must match a plain count of each style");
    Expect(view.has_mixed_line_endings() == expected_mixed,
           "the mixed-endings flag must match a plain count of each style");
  }
}

// Editing line 0 invalidates the highlight state chain (the syntax state chains
// forward from line 0, so everything below it is suspect) by zeroing the
// validity cursors rather than by clearing the per-line state vectors. The
// entries below a zeroed cursor are unreachable, so this must be behaviourally
// identical to wiping them -- pin that a multi-line construct opened ON line 0
// still colours the lines below it correctly after the opening line is edited.
void TestTextViewportFirstLineEditRehighlightsFollowingLines() {
  TextViewport viewport;
  // A block comment opened on line 0 swallows the lines below it.
  viewport.LoadContent("/* comment\nstill inside\nalso inside\n", "/tmp/first-line-syntax.cpp");
  viewport.SetViewportSize(10, 40);

  const auto line_is_comment = [&viewport](std::size_t line) {
    const auto tokens = viewport.HighlightedLineTokens(line);
    return !tokens.empty() && tokens.front() == SyntaxTokenKind::Comment;
  };
  Expect(line_is_comment(1) && line_is_comment(2),
         "lines under a block comment opened on line 0 should start out commented");

  // Close the comment on line 0. Everything below must stop being comment.
  viewport.MoveCursorTo(0, viewport.lines()[0].size());
  viewport.InsertText(" */");
  Expect(!line_is_comment(1) && !line_is_comment(2),
         "closing the block comment on line 0 must re-highlight every line below it — the "
         "state chain from line 0 is exactly what the line-0 invalidation has to drop");

  // And reopening it must bring them back.
  viewport.MoveCursorTo(0, viewport.lines()[0].size());
  for (int i = 0; i < 3; ++i) {
    viewport.Backspace();
  }
  Expect(line_is_comment(1) && line_is_comment(2),
         "reopening the block comment on line 0 must re-comment the lines below it");
}

// A content edit anchored at line 0 must go down the SAME incremental
// visual-column path as an edit anywhere else.
//
// It did not: the line-0 branch of InvalidateDerivedCaches dropped the whole
// per-line width table along with the visible-line layouts, so the next
// MaxVisualColumns() recomputed the width of every line in the buffer. On a
// 50k-line file an identical enter/backspace burst measured 121 full rebuilds
// at line 0 against 1 at line 25000 -- typing at the top of a large file was
// the slowest place to type. Line 0 is special for the highlight state (the
// syntax chain starts there); it is not special for widths, because a line's
// visual width depends on that line's bytes and nothing else.
//
// Pinned two ways: the horizontal-scroll clamp still tracks the widest line
// after editing line 0 (correctness -- a stale table would clamp wrong), and
// the edit takes the in-place incremental branch rather than a rebuild.
void TestTextViewportFirstLineEditKeepsVisualColumnCacheIncremental() {
  TextViewport viewport;
  // Line 0 is the widest line, so the max-column bookkeeping has to follow it.
  viewport.LoadContent(std::string(200, 'w') + "\nshort\nshorter\n", "/tmp/first-line-edit.txt");
  viewport.SetViewportSize(10, 20);

  const auto clamped_horizontal_scroll = [&viewport]() {
    viewport.SetHorizontalScroll(100000);
    return viewport.horizontal_scroll();
  };

  const std::size_t before = clamped_horizontal_scroll();
  Expect(before > 0, "a 200-column line in a 20-column viewport should allow horizontal scroll");

#ifndef NDEBUG
  const std::size_t inplace_before = viewport.VisualColumnIncrementalInplaceCountForDebug();
#endif

  // Widen line 0 by ten columns, in place (no line-count change).
  viewport.MoveCursorTo(0, 0);
  for (int i = 0; i < 10; ++i) {
    viewport.InsertText("x");
  }

#ifndef NDEBUG
  Expect(viewport.VisualColumnIncrementalInplaceCountForDebug() == inplace_before + 10,
         "each single-line edit at line 0 must take the in-place incremental width path, "
         "not drop the table and force a full rebuild");
#endif

  Expect(clamped_horizontal_scroll() == before + 10,
         "widening the widest line by ten columns must widen the horizontal scroll range by "
         "exactly ten");

  // And narrowing it again must pull the range back.
  for (int i = 0; i < 10; ++i) {
    viewport.Backspace();
  }
  Expect(clamped_horizontal_scroll() == before,
         "narrowing the widest line back must restore the horizontal scroll range");
}

// LoadLines (dirty-tab session restore) moves an already line-split buffer straight
// in, skipping the LoadContent(SerializeLines(...)) join-then-resplit. It must be
// exactly equivalent to that old round-trip: same lines, same line ending, dirty.
void TestTextViewportLoadLinesMatchesSerializeRoundTrip() {
  const std::vector<std::vector<std::string>> cases = {
      {"solo"},
      {"a", "b", "c"},
      {"a", "b", "c", ""},  // trailing empty line
      {"", "", ""},         // only empty lines
      {"  indented", "\ttab", "end"},
  };
  const util::LineEnding endings[] = {util::LineEnding::LF, util::LineEnding::CRLF};
  for (const auto& lines : cases) {
    for (util::LineEnding ending : endings) {
      editor::TextViewport via_load_lines;
      via_load_lines.LoadLines(lines, "/tmp/loadlines.txt", ending);

      editor::TextViewport via_round_trip;
      via_round_trip.LoadContent(util::SerializeLines(lines, ending), "/tmp/loadlines.txt", ending);
      via_round_trip.SetDirty(true);

      Expect(via_load_lines.lines().Snapshot() == via_round_trip.lines().Snapshot(),
             "LoadLines content should match the LoadContent(SerializeLines(...)) round-trip");
      Expect(via_load_lines.lines().Snapshot() == lines,
             "LoadLines should preserve the exact input lines");
      Expect(via_load_lines.line_ending() == ending, "LoadLines should preserve the line ending");
      Expect(via_load_lines.dirty(), "LoadLines should mark the restored buffer dirty");
      Expect(via_load_lines.line_ending() == via_round_trip.line_ending(),
             "LoadLines line ending should match the round-trip path");
    }
  }
}

}  // namespace

// The undo history caps total bytes, not just entry count: a handful of
// large-range edits (each copying a big line range into before/after_lines) must
// not accumulate into a multi-GB history. Continuous eviction keeps peak memory
// near the budget, so pushing many large entries leaves far fewer than the
// 128-entry count cap alive.
void TestUndoHistoryEnforcesByteBudget() {
  microide::editor::TextViewportUndoHistory history;
  // ~4 MiB per entry; the 256 MiB byte budget admits ~64 of these — well under the
  // 128-entry count cap, so a survivor count below 128 proves the byte cap acted.
  for (int i = 0; i < 200; ++i) {
    microide::editor::TextViewportUndoHistory::Entry entry;
    entry.after_lines.push_back(std::string(4u * 1024 * 1024, 'x'));
    history.PushUndo(std::move(entry));
  }

  std::size_t survivors = 0;
  while (history.CanUndo()) {
    history.PopUndo();
    ++survivors;
  }
  Expect(survivors > 0, "at least the most recent large edit stays undoable");
  Expect(survivors < 128,
         "the byte budget must evict below the entry-count cap for large entries");
}

// A column/box select across a very tall document must not allocate one caret per
// line without bound (a single-gesture OOM/hang). The caret set is capped.
void TestTextViewportColumnCaretsAreCapped() {
  microide::editor::TextViewport viewport;
  std::string content;
  for (int i = 0; i < 40000; ++i) {
    content += "x\n";
  }
  viewport.LoadContent(content, "/tmp/tall.txt");

  viewport.PlaceColumnCaretsBetweenLines(0, 39000, 0);
  Expect(viewport.secondary_carets().size() <= 10000,
         "a column select over tens of thousands of lines must cap the caret count");
  Expect(!viewport.secondary_carets().empty(),
         "a column select should still place some carets");
}

// Mirrors WorkspaceShell::ApplyLspWorkspaceEdit for a multi-line "remove unused
// includes" fix: several non-contiguous single-line deletions, clamped, sorted
// highest-position-first, applied inside one undo group. A single Undo must fully
// restore the original document (else save-after-undo persists truncated content).
void TestTextViewportGroupedNonContiguousDeletesUndoFully() {
  TextViewport viewport;
  std::string content;
  for (int i = 0; i < 12; ++i) {
    content += "#include \"h" + std::to_string(i) + "\"\n";
  }
  content += "int main() { return 0; }\n";
  viewport.LoadContent(content, "/tmp/grouped-undo.cpp");
  const auto original = viewport.lines().Snapshot();

  // Delete lines 1, 4, 7, 10 (non-contiguous), each as a full-line range.
  std::vector<SelectionRange> edits = {
      {{1, 0}, {2, 0}}, {{4, 0}, {5, 0}}, {{7, 0}, {8, 0}}, {{10, 0}, {11, 0}}};
  std::sort(edits.begin(), edits.end(), [](const SelectionRange& a, const SelectionRange& b) {
    return a.start.line > b.start.line;
  });

  viewport.BeginUndoGroup();
  for (const auto& range : edits) {
    viewport.ReplaceRange(range, "", /*record_undo=*/true);
  }
  viewport.EndUndoGroup();
  Expect(viewport.lines().size() == original.size() - 4,
         "grouped deletes should remove exactly four lines");
  Expect(viewport.dirty(),
         "a grouped edit that changed the buffer must leave it dirty");

  const bool undone = viewport.Undo();
  Expect(undone, "one Undo should reverse the whole grouped edit");
  const auto restored = viewport.lines().Snapshot();
  Expect(restored == original,
         "a single Undo must fully restore the original buffer after grouped non-contiguous deletes");
}

// Data-integrity: after a save, undoing PAST the saved point must mark the buffer
// dirty, because its content now differs from what is on disk. Regression guard for
// the "edit -> save -> undo shows clean -> content silently lost on reopen" bug.
void TestTextViewportUndoPastSaveMarksDirty() {
  const std::filesystem::path path =
      std::filesystem::temp_directory_path() / "microide-undo-save-dirty.txt";
  std::filesystem::remove(path);
  TextViewport viewport;
  viewport.LoadContent("line0\nline1\nline2\n", path);
  Expect(!viewport.dirty(), "a freshly loaded buffer is clean");
  const auto original = viewport.lines().Snapshot();

  viewport.ReplaceRange(SelectionRange{{1, 0}, {2, 0}}, "", /*record_undo=*/true);
  Expect(viewport.dirty(), "deleting a line marks the buffer dirty");
  Expect(viewport.Save(), "save should succeed");
  Expect(!viewport.dirty(), "a successful save clears the dirty flag");

  Expect(viewport.Undo(), "undo should succeed");
  Expect(viewport.lines().Snapshot() == original, "undo restores the deleted line");
  Expect(viewport.dirty(),
         "undoing past a save must mark the buffer dirty; the content now differs from disk");

  // Redoing back to the saved content must return to clean.
  Expect(viewport.Redo(), "redo should succeed");
  Expect(!viewport.dirty(), "redoing back to the saved content returns to clean");
  std::filesystem::remove(path);
}

// Data-integrity (dirty-baseline family): saving while parked ABOVE undone edits must
// make the saved position the sole clean point — redoing forward off it re-dirties, and
// undoing past it dirties, because both differ from what is on disk.
void TestTextViewportSaveAboveUndoneEditsDirtiesRedo() {
  const std::filesystem::path path =
      std::filesystem::temp_directory_path() / "microide-save-above-undone.txt";
  std::filesystem::remove(path);
  TextViewport viewport;
  viewport.LoadContent("L\n", path);

  // Two discrete (non-coalescing) edits on different lines -> two undo entries.
  viewport.ReplaceRange(SelectionRange{{0, 0}, {0, 0}}, "a", /*record_undo=*/true);  // E1
  viewport.ReplaceRange(SelectionRange{{1, 0}, {1, 0}}, "b", /*record_undo=*/true);  // E2
  Expect(viewport.Undo(), "undo should reverse the second insert");  // parked at E1's result

  Expect(viewport.Save(), "saving while above an undone edit should succeed");
  Expect(!viewport.dirty(), "the just-saved (undone-to) position is clean");

  Expect(viewport.Redo(), "redo should re-apply the undone edit");
  Expect(viewport.dirty(),
         "redoing forward off the saved position must dirty (content now differs from disk)");

  Expect(viewport.Undo(), "undo returns to the saved position");
  Expect(!viewport.dirty(), "returning to the saved position is clean again");
  Expect(viewport.Undo(), "undo past the save should succeed");
  Expect(viewport.dirty(), "undoing past the save must dirty");
  std::filesystem::remove(path);
}

// Data-integrity: a contiguous keystroke immediately after a save must start a FRESH
// undo entry, never coalesce into the just-saved top entry. Coalescing would rewrite the
// saved position's after-state back to dirty and make the saved content unreachable by
// undo — silently losing it.
void TestTextViewportSaveThenCoalescingKeystrokeStartsFreshEntry() {
  const std::filesystem::path path =
      std::filesystem::temp_directory_path() / "microide-save-then-type.txt";
  std::filesystem::remove(path);
  TextViewport viewport;
  viewport.LoadContent("", path);

  viewport.InsertCharacter('a');
  Expect(viewport.Save(), "save should succeed");
  Expect(!viewport.dirty(), "save clears dirty");

  viewport.InsertCharacter('b');  // contiguous with 'a' — must NOT fold into the saved entry
  Expect(viewport.dirty(), "typing after a save dirties the buffer");

  Expect(viewport.Undo(), "undo should reverse only the post-save keystroke");
  Expect(viewport.lines()[0] == "a",
         "undo must land on the saved content 'a', proving the keystroke did not coalesce into it");
  Expect(!viewport.dirty(), "landing back on the saved content reads clean");
  std::filesystem::remove(path);
}

// Regression: nested undo groups must not double-count child edits. RecordEntry
// fans each child into every active frame, so when the inner group flushes it must
// NOT re-record its aggregate into the still-active outer frame (which already
// captured the same children). Only the outermost group commits one entry.
void TestTextViewportNestedUndoGroupsDoNotDoubleCount() {
  const std::filesystem::path path =
      std::filesystem::temp_directory_path() / "microide-undo-nested.txt";
  std::filesystem::remove(path);
  TextViewport viewport;
  viewport.LoadContent("hello\n", path);
  viewport.MoveCursorTo(0, 5);

  viewport.BeginUndoGroup();
  viewport.InsertText("A");
  viewport.BeginUndoGroup();  // nested
  viewport.InsertText("B");
  viewport.EndUndoGroup();    // closes inner — must not push into the outer frame
  viewport.InsertText("C");
  viewport.EndUndoGroup();    // closes outer — the single committed entry

  Expect(viewport.lines().LineView(0) == "helloABC", "all three inserts should apply");
  Expect(!viewport.UndoGroupActive(), "no undo group should remain active");
  Expect(viewport.line_count() == 2, "line count should be unchanged");

  Expect(viewport.Undo(), "one undo should reverse the whole nested group");
  Expect(viewport.lines().LineView(0) == "hello",
         "undo should restore the original line exactly (double-fold corrupts the slice)");
  Expect(viewport.line_count() == 2, "undo should restore the original line count");
  Expect(!viewport.Undo(), "exactly one undo entry should have been pushed");

  Expect(viewport.Redo(), "redo should re-apply the grouped edits");
  Expect(viewport.lines().LineView(0) == "helloABC", "redo should restore the grouped result");
  std::filesystem::remove(path);
}

// Data-integrity: a grouped (single-undo) multi-edit interacts with save the same way a
// single edit does — undo past the save dirties, redo back to it cleans.
void TestTextViewportGroupedEditSaveUndoMarksDirty() {
  const std::filesystem::path path =
      std::filesystem::temp_directory_path() / "microide-grouped-save.txt";
  std::filesystem::remove(path);
  TextViewport viewport;
  viewport.LoadContent("l0\nl1\nl2\nl3\n", path);
  const auto original = viewport.lines().Snapshot();

  viewport.BeginUndoGroup();
  viewport.ReplaceRange(SelectionRange{{2, 0}, {3, 0}}, "", /*record_undo=*/true);
  viewport.ReplaceRange(SelectionRange{{0, 0}, {1, 0}}, "", /*record_undo=*/true);
  viewport.EndUndoGroup();
  Expect(viewport.dirty(), "a grouped edit dirties the buffer");
  Expect(viewport.Save(), "save should succeed");
  Expect(!viewport.dirty(), "save clears dirty");

  Expect(viewport.Undo(), "one undo reverses the whole group");
  Expect(viewport.lines().Snapshot() == original, "undo restores the full original buffer");
  Expect(viewport.dirty(), "undoing past the save dirties (content differs from disk)");
  Expect(viewport.Redo(), "redo re-applies the group");
  Expect(!viewport.dirty(), "redoing back to the saved content cleans");
  std::filesystem::remove(path);
}

// Data-integrity: once the undo history evicts the saved entry (history cap), undoing as
// far as possible must NEVER report clean for content that differs from disk. Eviction
// removing the clean marker must fail safe to "dirty", not "clean".
void TestTextViewportSaveEvictionNeverFalseClean() {
  const std::filesystem::path path =
      std::filesystem::temp_directory_path() / "microide-save-eviction.txt";
  std::filesystem::remove(path);
  TextViewport viewport;
  viewport.LoadContent("seed\n", path);

  viewport.ReplaceRange(SelectionRange{{0, 0}, {0, 0}}, "x", /*record_undo=*/true);
  Expect(viewport.Save(), "save should succeed");
  Expect(!viewport.dirty(), "save clears dirty");

  // Push well past the 128-entry cap with discrete (non-coalescing) edits so the saved
  // entry (and its clean-bridging successor) are both evicted from the front of the
  // undo stack.
  for (int i = 0; i < 200; ++i) {
    viewport.ReplaceRange(SelectionRange{{0, 0}, {0, 0}}, "z", /*record_undo=*/true);
  }
  while (viewport.Undo()) {
    // Walk all the way back to the oldest surviving entry.
  }
  Expect(viewport.dirty(),
         "after the saved entry is evicted, the oldest reachable content differs from disk "
         "and must report dirty (no false clean)");
  std::filesystem::remove(path);
}

// Data-integrity: re-saving moves the sole clean point forward. After a second save,
// undoing to the FIRST save's content reads dirty (disk now holds the second), and
// redoing back to the second save reads clean.
void TestTextViewportDoubleSaveRebaselines() {
  const std::filesystem::path path =
      std::filesystem::temp_directory_path() / "microide-double-save.txt";
  std::filesystem::remove(path);
  TextViewport viewport;
  viewport.LoadContent("base\n", path);

  viewport.InsertText("A");
  Expect(viewport.Save(), "first save should succeed");
  viewport.InsertText("B");
  Expect(viewport.Save(), "second save should succeed");
  Expect(!viewport.dirty(), "the second save is the clean point");

  Expect(viewport.Undo(), "undo returns to the first-save content");
  Expect(viewport.dirty(),
         "the first-save content differs from disk (which now holds the second save) -> dirty");
  Expect(viewport.Redo(), "redo returns to the second-save content");
  Expect(!viewport.dirty(), "the second-save content matches disk -> clean");
  std::filesystem::remove(path);
}

// Regression: vertical caret movement must preserve the target column even when
// the view is horizontally scrolled. The non-soft-wrap branch of
// ResolveSoftWrapCursorColumnForTargetRow used to add horizontal_scroll_ to the
// (already absolute) preferred column, double-counting the scroll and marching the
// caret to end-of-line. Invisible whenever horizontal_scroll_ == 0.
// A reload that preserves view state has to preserve the SCROLL too, including
// when there is a selection. The save path used to restore the selection after the
// scroll, and MoveCursorTo runs EnsureCursorVisible — so selecting the whole
// buffer and saving through a formatter snapped the view to the end of the file.
void TestTextViewportReloadPreservingViewStateKeepsScrollUnderSelection() {
  TextViewport viewport;
  std::string text;
  for (int i = 0; i < 400; ++i) {
    text += "line " + std::to_string(i) + "\n";
  }
  viewport.LoadContent(text, "/tmp/reload.txt");
  viewport.SetViewportSize(20, 80);

  // Scroll into the middle, then select the whole buffer (Ctrl+A leaves the
  // selection end at the last line, far below the visible window).
  viewport.SetScrollLine(100);
  viewport.MoveCursorTo(0, 0);
  viewport.SetScrollLine(100);
  viewport.MoveCursorTo(399, 0, /*extend_selection=*/true);
  viewport.SetScrollLine(100);
  const std::size_t scroll_before = viewport.scroll_line();
  const auto selection_before = viewport.selection_range();
  Expect(scroll_before == 100, "the fixture should be scrolled away from the selection end");
  Expect(selection_before.has_value(), "the fixture should hold a selection");

  viewport.ReloadPreservingViewState(text);

  Expect(viewport.scroll_line() == scroll_before,
         "reloading must restore the scroll, not snap it onto the selection end");
  const auto selection_after = viewport.selection_range();
  Expect(selection_after.has_value() &&
             selection_after->start.line == selection_before->start.line &&
             selection_after->end.line == selection_before->end.line,
         "reloading must restore the selection it captured");
}

void TestTextViewportVerticalMovePreservesColumnWhenScrolled() {
  TextViewport viewport;
  const std::string long_line(200, 'a');
  viewport.LoadContent(long_line + "\n" + long_line + "\n" + long_line + "\n", "/tmp/wide.txt");
  viewport.SetSoftWrap(false);
  viewport.SetViewportSize(10, 80);

  viewport.MoveCursorTo(1, 150);
  Expect(viewport.horizontal_scroll() > 0,
         "a caret past the viewport width should force a non-zero horizontal scroll");

  viewport.MoveCursorVertical(1);
  Expect(viewport.cursor_line() == 2 && viewport.cursor_column() == 150,
         "moving down while horizontally scrolled must preserve the target column (150)");

  viewport.MoveCursorTo(1, 150);
  viewport.MoveCursorVertical(-1);
  Expect(viewport.cursor_line() == 0 && viewport.cursor_column() == 150,
         "moving up while horizontally scrolled must preserve the target column (150)");
}

// In soft-wrap mode preferred_column_ is stored relative to the wrapped-row start.
// ClampCursorColumn (called from SetTabSize) used to feed it into
// TextColumnForVisualColumn as an absolute column, snapping the caret to near the
// start of the logical line whenever it sat on a continuation row.
void TestTextViewportSoftWrapTabSizeChangePreservesWrappedColumn() {
  TextViewport viewport;
  const std::string long_line(200, 'a');  // no tabs: wrapping is tab-size-invariant
  viewport.LoadContent(long_line + "\n" + long_line + "\n", "/tmp/soft-wrap-tab.txt");
  viewport.SetViewportSize(10, 80);
  viewport.SetSoftWrap(true);
  viewport.SetTabSize(4);  // known baseline so the change below is not a no-op

  viewport.MoveCursorTo(0, 150);  // column 150 lands on the 2nd wrapped row (row [80,160))
  Expect(viewport.cursor_line() == 0 && viewport.cursor_column() == 150,
         "precondition: caret is at column 150 on a continuation row");

  viewport.SetTabSize(8);  // triggers ClampCursorColumn
  Expect(viewport.cursor_line() == 0 && viewport.cursor_column() == 150,
         "changing tab size must keep the wrapped caret column, not snap to line start");
}

void RegisterTextViewportTests(std::vector<TestCase>& tests) {
  AddTest(tests, "TextViewport/VerticalMovePreservesColumnWhenScrolled",
          TestTextViewportVerticalMovePreservesColumnWhenScrolled);
  AddTest(tests, "TextViewport/ReloadPreservingViewStateKeepsScrollUnderSelection",
          TestTextViewportReloadPreservingViewStateKeepsScrollUnderSelection);
  AddTest(tests, "TextViewport/SoftWrapTabSizeChangePreservesWrappedColumn",
          TestTextViewportSoftWrapTabSizeChangePreservesWrappedColumn);
  AddTest(tests, "TextViewport/UndoPastSaveMarksDirty",
          TestTextViewportUndoPastSaveMarksDirty);
  AddTest(tests, "TextViewport/SaveAboveUndoneEditsDirtiesRedo",
          TestTextViewportSaveAboveUndoneEditsDirtiesRedo);
  AddTest(tests, "TextViewport/SaveThenCoalescingKeystrokeStartsFreshEntry",
          TestTextViewportSaveThenCoalescingKeystrokeStartsFreshEntry);
  AddTest(tests, "TextViewport/GroupedEditSaveUndoMarksDirty",
          TestTextViewportGroupedEditSaveUndoMarksDirty);
  AddTest(tests, "TextViewport/SaveEvictionNeverFalseClean",
          TestTextViewportSaveEvictionNeverFalseClean);
  AddTest(tests, "TextViewport/DoubleSaveRebaselines",
          TestTextViewportDoubleSaveRebaselines);
  AddTest(tests, "TextViewport/GroupedNonContiguousDeletesUndoFully",
          TestTextViewportGroupedNonContiguousDeletesUndoFully);
  AddTest(tests, "TextViewport/ColumnCaretsAreCapped", TestTextViewportColumnCaretsAreCapped);
  AddTest(tests, "TextViewport/UndoHistoryEnforcesByteBudget",
          TestUndoHistoryEnforcesByteBudget);
  AddTest(tests, "TextViewport/LoadLinesMatchesSerializeRoundTrip",
          TestTextViewportLoadLinesMatchesSerializeRoundTrip);
  AddTest(tests, "TextViewport/OpenNormalizesEveryLineEndingMix",
          TestTextViewportOpenNormalizesEveryLineEndingMix);
  AddTest(tests, "TextViewport/FirstLineEditKeepsVisualColumnCacheIncremental",
          TestTextViewportFirstLineEditKeepsVisualColumnCacheIncremental);
  AddTest(tests, "TextViewport/FirstLineEditRehighlightsFollowingLines",
          TestTextViewportFirstLineEditRehighlightsFollowingLines);
  AddTest(tests, "TextViewport/FastLoadMatchesCrlfDecode",
          TestTextViewportFastLoadMatchesCrlfDecode);
  AddTest(tests, "TextViewport/FastLoadPreservesCrOnlyLines",
          TestTextViewportFastLoadPreservesCrOnlyLines);
  AddTest(tests, "TextViewport/FastLoadDenseCrlfFile",
          TestTextViewportFastLoadDenseCrlfFile);
  AddTest(tests, "TextViewport/FastLoadNoTrailingNewline",
          TestTextViewportFastLoadNoTrailingNewline);
  AddTest(tests, "TextViewport/FastLoadEmptyFile",
          TestTextViewportFastLoadEmptyFile);
  AddTest(tests, "TextViewport/FastLoadDetectsEncoding",
          TestTextViewportFastLoadDetectsEncoding);
  AddTest(tests, "TextViewport/SmallFileKeepsSyntaxHighlighting",
          TestTextViewportSmallFileKeepsSyntaxHighlighting);
  AddTest(tests, "TextViewport/SplitSiblingEditRefreshesHighlightTokens",
          TestTextViewportSplitSiblingEditRefreshesHighlightTokens);
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
  AddTest(tests, "TextViewport/CheckpointBackfillConvergesToExactMultilineState",
          TestTextViewportCheckpointBackfillConvergesToExactMultilineState);
  AddTest(tests, "TextViewport/LineCommentEndsAtLineBoundary",
          TestTextViewportLineCommentEndsAtLineBoundary);
  AddTest(tests, "TextViewport/EditingNearTailDoesNotRebuildFarCheckpoints",
          TestTextViewportEditingNearTailDoesNotRebuildFarCheckpoints);
  AddTest(tests, "TextViewport/InsertNewlineCopiesLeadingIndentation",
          TestTextViewportInsertNewlineCopiesLeadingIndentation);
  AddTest(tests, "TextViewport/InsertNewlineOverDownwardSelectionIndentsFromStartLine",
          TestTextViewportInsertNewlineOverDownwardSelectionIndentsFromStartLine);
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
  AddTest(tests, "TextViewport/MultiCaretDeleteLineClearsLastAppliedEdit",
          TestTextViewportMultiCaretDeleteLineClearsLastAppliedEdit);
  AddTest(tests, "TextViewport/LastAppliedEditTracksMultilineReplacement",
          TestTextViewportLastAppliedEditTracksMultilineReplacement);
  AddTest(tests, "TextViewport/LastAppliedEditLineSpanMatchesWholeLineDiff",
          TestTextViewportLastAppliedEditLineSpanMatchesWholeLineDiff);
  AddTest(tests, "TextViewport/UndoRedoPreservesLatestViewState",
          TestTextViewportUndoRedoPreservesLatestViewState);
  AddTest(tests, "TextViewport/UndoRedoPreservesSecondaryCarets",
          TestTextViewportUndoRedoPreservesSecondaryCarets);
  AddTest(tests, "TextViewport/UndoGroupMergesKnownRangeChildEdits",
          TestTextViewportUndoGroupMergesKnownRangeChildEdits);
  AddTest(tests, "TextViewport/UndoGroupFallsBackForDisjointChildEdits",
          TestTextViewportUndoGroupFallsBackForDisjointChildEdits);
  AddTest(tests, "TextViewport/NestedUndoGroupsDoNotDoubleCount",
          TestTextViewportNestedUndoGroupsDoNotDoubleCount);
  AddTest(tests, "TextViewport/TypedCharactersCoalesceIntoWordUndoSteps",
          TestTextViewportTypedCharactersCoalesceIntoWordUndoSteps);
  AddTest(tests, "TextViewport/CaretJumpBreaksTypingCoalesce",
          TestTextViewportCaretJumpBreaksTypingCoalesce);
  AddTest(tests, "TextViewport/CaretRoundTripBreaksTypingCoalesce",
          TestTextViewportCaretRoundTripBreaksTypingCoalesce);
  AddTest(tests, "TextViewport/MoveCursorToSameColumnBreaksTypingCoalesce",
          TestTextViewportMoveCursorToSameColumnBreaksTypingCoalesce);
  AddTest(tests, "TextViewport/BackspaceCoalescesIntoWordUndoSteps",
          TestTextViewportBackspaceCoalescesIntoWordUndoSteps);
  AddTest(tests, "TextViewport/InLineEditsRecordColumnScopedUndoEntries",
          TestTextViewportInLineEditsRecordColumnScopedUndoEntries);
  AddTest(tests, "TextViewport/InlineUndoRedoMatchesNaiveModel",
          TestTextViewportInlineUndoRedoMatchesNaiveModel);
  AddTest(tests, "TextViewport/InlineUndoEntriesCoalesceLikeSequentialApplication",
          TestInlineUndoEntriesCoalesceLikeSequentialApplication);
  AddTest(tests, "TextViewport/InlineUndoEntriesRefuseNonContiguousCoalesce",
          TestInlineUndoEntriesRefuseNonContiguousCoalesce);
  AddTest(tests, "TextViewport/InlineEditPublishesCharacterLevelAppliedEdit",
          TestTextViewportInlineEditPublishesCharacterLevelAppliedEdit);
  AddTest(tests, "TextViewport/InlineEditUpgradesEncodingFromTheDeltaAlone",
          TestTextViewportInlineEditUpgradesEncodingFromTheDeltaAlone);
  AddTest(tests, "TextViewport/GroupedInlineEditsUndoAtomically",
          TestTextViewportGroupedInlineEditsUndoAtomically);
  AddTest(tests, "TextViewport/InlineCoalescedEntryRestampsItsByteSize",
          TestInlineCoalescedEntryRestampsItsByteSize);
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
  AddTest(tests, "TextViewport/SoftWrapCollapsedFoldOpenerVerticalMotionEscapes",
          TestTextViewportSoftWrapCollapsedFoldOpenerVerticalMotionEscapes);
  AddTest(tests, "TextViewport/SoftWrapPageMovesByVisibleRows",
          TestTextViewportSoftWrapPageMovesByVisibleRows);
  AddTest(tests, "TextViewport/ShiftPageExtendsSelection",
          TestTextViewportShiftPageExtendsSelection);
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
  AddTest(tests, "TextViewport/SoftWrapVerticalMotionFollowsNonUniformRows",
          TestTextViewportSoftWrapVerticalMotionFollowsNonUniformRows);
  AddTest(tests, "TextViewport/SoftWrapHangingIndentAlignsContinuationRows",
          TestTextViewportSoftWrapHangingIndentAlignsContinuationRows);
  AddTest(tests, "TextViewport/SoftWrapEditGrowsWrapRowCount",
          TestTextViewportSoftWrapEditGrowsWrapRowCount);
  AddTest(tests, "TextViewport/SoftWrapEnterSplitUpdatesRowMapping",
          TestTextViewportSoftWrapEnterSplitUpdatesRowMapping);
  AddTest(tests, "TextViewport/SoftWrapBackspaceMergeUpdatesRowMapping",
          TestTextViewportSoftWrapBackspaceMergeUpdatesRowMapping);
#ifndef NDEBUG
  AddTest(tests, "TextViewport/SoftWrapEditUpdatesWrapIncrementally",
          TestTextViewportSoftWrapEditUpdatesWrapIncrementally);
  AddTest(tests, "TextViewport/SoftWrapLargeBufferInlineEditIsInPlace",
          TestTextViewportSoftWrapLargeBufferInlineEditIsInPlace);
  AddTest(tests, "TextViewport/SoftWrapIncrementalEditsMatchFreshBuild",
          TestTextViewportSoftWrapIncrementalEditsMatchFreshBuild);
#endif
  AddTest(tests, "TextViewport/CollapsedFoldHidesBodyRows",
          TestTextViewportCollapsedFoldHidesBodyRows);
  AddTest(tests, "TextViewport/CollapsedFoldRowSpansTrackHorizontalScroll",
          TestTextViewportCollapsedFoldRowSpansTrackHorizontalScroll);
  AddTest(tests, "TextViewport/HorizontalMotionCrossesLineBoundaries",
          TestTextViewportHorizontalMotionCrossesLineBoundaries);
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
  AddTest(tests, "TextViewport/LargeBufferSingleCaretEditWorkoutAvoidsSnapshot",
          TestTextViewportLargeBufferSingleCaretEditWorkoutAvoidsSnapshot);
  AddTest(tests, "TextViewport/ReplaceAllMultiMatchRespectsUnequalLengths",
          TestTextViewportReplaceAllMultiMatchRespectsUnequalLengths);
  AddTest(tests, "TextViewport/ReplaceAllUnicodeCaseInsensitive",
          TestTextViewportReplaceAllUnicodeCaseInsensitive);
  AddTest(tests, "TextViewport/ReplaceAllMultiLineReplacementSplitsLines",
          TestTextViewportReplaceAllMultiLineReplacementSplitsLines);
  AddTest(tests, "TextViewport/ReplaceAllRangesMatchesScanningReplaceAll",
          TestTextViewportReplaceAllRangesMatchesScanningReplaceAll);
  AddTest(tests, "TextViewport/RuntimeSyntaxDetectFiletypeDisambiguatesCppHeader",
          TestRuntimeSyntaxDetectFiletypeDisambiguatesCppHeader);
  AddTest(tests, "TextViewport/RuntimeSyntaxCarriesRegionAcrossBlankLine",
          TestRuntimeSyntaxCarriesRegionAcrossBlankLine);
  AddTest(tests, "TextViewport/RuntimeSyntaxDetectFiletypeDisambiguatesObjectiveCSource",
          TestRuntimeSyntaxDetectFiletypeDisambiguatesObjectiveCSource);
  AddTest(tests, "TextViewport/RuntimeSyntaxDetectFiletypeKeepsCMakeLists",
          TestRuntimeSyntaxDetectFiletypeKeepsCMakeLists);
  AddTest(tests, "TextViewport/SingleLineEditAllocatesABoundedMultipleOfTheLine",
          TestTextViewportSingleLineEditAllocatesABoundedMultipleOfTheLine);
  AddTest(tests, "TextViewport/InlineEditDerivesLineWidthFromTheSplice",
          TestTextViewportInlineEditDerivesLineWidthFromTheSplice);
  AddTest(tests, "TextViewport/CaretColumnConversionsMatchTheDirectWalk",
          TestTextViewportCaretColumnConversionsMatchTheDirectWalk);
  AddTest(tests, "TextViewport/TypingInALongLineDoesNotWalkItForCaretColumns",
          TestTextViewportTypingInALongLineDoesNotWalkItForCaretColumns);
  AddTest(tests, "TextViewport/LanguageIdMemoIsAllocationFreeOnASettledBuffer",
          TestTextViewportLanguageIdMemoIsAllocationFreeOnASettledBuffer);
  AddTest(tests, "TextViewport/LanguageIdMemoInvalidatesOnContentChange",
          TestTextViewportLanguageIdMemoInvalidatesOnContentChange);
  AddTest(tests, "TextViewport/LanguageIdMemoInvalidatesOnPathChange",
          TestTextViewportLanguageIdMemoInvalidatesOnPathChange);
  AddTest(tests, "TextViewport/RuntimeSyntaxInitialStateAllocationIsDocumentSizeIndependent",
          TestRuntimeSyntaxInitialStateAllocationIsDocumentSizeIndependent);
  AddTest(tests, "TextViewport/LoadsRuntimeSyntaxDefinitionsFromPluginDataDirectories",
          TestTextViewportLoadsRuntimeSyntaxDefinitionsFromPluginDataDirectories);
  AddTest(tests, "TextViewport/RuntimeSyntaxLoaderBoundsInfiniteLoop",
          TestRuntimeSyntaxLoaderBoundsInfiniteLoop);
  AddTest(tests, "TextViewport/RuntimeSyntaxMatchBudgetBoundsPathologicalRule",
          TestRuntimeSyntaxMatchBudgetBoundsPathologicalRule);
  AddTest(tests, "TextViewport/RuntimeSyntaxCaretAnchorHonorsTrueLineStart",
          TestRuntimeSyntaxCaretAnchorHonorsTrueLineStart);
  AddTest(tests, "TextViewport/SyntaxHighlightNestedRegionResumesParentScope",
          TestSyntaxHighlightNestedRegionResumesParentScope);
  AddTest(tests, "TextViewport/SyntaxHighlightSkipsOverlongLine",
          TestSyntaxHighlightSkipsOverlongLine);
  AddTest(tests, "TextViewport/RuntimeSyntaxLazyCompileIsThreadSafeForSameLanguage",
          TestRuntimeSyntaxLazyCompileIsThreadSafeForSameLanguage);
  AddTest(tests, "TextViewport/RuntimeSyntaxLazyCompileSurvivesReload",
          TestRuntimeSyntaxLazyCompileSurvivesReload);
  AddTest(tests, "TextViewport/RuntimeSyntaxBadPluginRegexReportsErrorEagerly",
          TestRuntimeSyntaxBadPluginRegexReportsErrorEagerly);
  AddTest(tests, "TextViewport/RuntimeSyntaxLazyCompileColorsBuiltInBlockComment",
          TestRuntimeSyntaxLazyCompileColorsBuiltInBlockComment);
  AddTest(tests, "TextViewport/RuntimeSyntaxLazyCompileSweepsCommonLanguages",
          TestRuntimeSyntaxLazyCompileSweepsCommonLanguages);
  AddTest(tests, "TextViewport/HighlightPrefetchInstallPopulatesCacheAndRespectsStaleness",
          TestHighlightPrefetchInstallPopulatesCacheAndRespectsStaleness);
  AddTest(tests, "TextViewport/HighlightPrefetchServiceTokenizesOnWorkerThread",
          TestHighlightPrefetchServiceTokenizesOnWorkerThread);
  AddTest(tests, "TextViewport/HighlightCheckpointBackfillServiceRunsOnWorkerThread",
          TestHighlightCheckpointBackfillServiceRunsOnWorkerThread);
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
  AddTest(tests, "TextViewport/NoOpRangeReplaceDoesNotDirty",
          TestTextViewportNoOpRangeReplaceDoesNotDirty);
  AddTest(tests, "TextViewport/NoOpLineReplaceDoesNotDirty",
          TestTextViewportNoOpLineReplaceDoesNotDirty);
  AddTest(tests, "TextViewport/EmptyLineDeleteIsNotTreatedAsNoOp",
          TestTextViewportEmptyLineDeleteIsNotTreatedAsNoOp);
}

}  // namespace microide::tests
