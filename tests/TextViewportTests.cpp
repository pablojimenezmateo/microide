#include "TestSupport.h"

#include "editor/ShapingActions.h"
#include "editor/SyntaxDefinitionLoader.h"
#include "editor/FoldingModel.h"
#include "editor/RuntimeSyntaxRegistry.h"
#include "editor/HighlightPrefetchService.h"
#include "editor/SyntaxHighlighter.h"
#include "editor/TextBuffer.h"
#include "editor/TextLayout.h"
#include "editor/TextViewport.h"
#include "perf/AllocationCounter.h"
#include "util/PerformanceCounters.h"
#include "util/TextFileIO.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
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

void TestTextViewportSplitSiblingEditRefreshesHighlightTokens() {
  TextViewport pane_a;
  pane_a.LoadContent("int a = 1;\nint b = 2;\nint c = 3;\n", "/tmp/split-highlight.cpp");
  (void)pane_a.HighlightedLineTokens(0);
  (void)pane_a.HighlightedLineTokens(1);

  TextViewport pane_b = pane_a;
  const auto before_span = pane_b.HighlightedLineTokens(1);
  const std::vector<SyntaxTokenKind> before(before_span.begin(), before_span.end());
  Expect(std::any_of(before.begin(), before.end(),
                     [](SyntaxTokenKind kind) { return kind != SyntaxTokenKind::Comment; }),
         "line 1 should start as non-comment code");

  pane_a.MoveCursorTo(0, 0);
  pane_a.InsertText("/*");

  const auto& after = pane_b.HighlightedLineTokens(1);
  Expect(!after.empty() &&
             std::all_of(after.begin(), after.end(),
                         [](SyntaxTokenKind kind) { return kind == SyntaxTokenKind::Comment; }),
         "the split sibling pane must recompute tokens after the shared buffer changed");
}

void TestTextViewportSplitSiblingsShareOneUndoHistory() {
  TextViewport pane_a;
  pane_a.LoadContent("alpha\n", "/tmp/split-undo.txt");
  TextViewport pane_b = pane_a;

  pane_a.MoveCursorTo(0, 0);
  pane_a.InsertText("X");
  pane_b.MoveCursorTo(0, 0);
  pane_b.InsertText("Y");
  pane_a.MoveCursorTo(0, 0);
  pane_a.InsertText("Z");
  Expect(pane_b.lines().LineView(0) == "ZYXalpha", "both panes see every edit");

  Expect(pane_b.Undo(), "pane B can undo");
  Expect(pane_b.lines().LineView(0) == "YXalpha",
         "undo from either pane steps the shared history: got " +
             std::string(pane_b.lines().LineView(0)));
  Expect(pane_a.Undo(), "pane A can undo pane B's edit");
  Expect(pane_a.lines().LineView(0) == "Xalpha", "the history is one stack");
  Expect(pane_b.Redo(), "redo is shared too");
  Expect(pane_a.lines().LineView(0) == "YXalpha", "pane A sees pane B's redo");
  Expect(pane_a.dirty(), "the buffer is dirty");

  TextViewport pane_c = pane_a;
  Expect(pane_c.Undo() && pane_c.Undo(), "a new pane can undo edits made before it existed");
  Expect(pane_b.lines().LineView(0) == "alpha", "the document is back to its loaded text");
  Expect(!pane_b.dirty(), "undoing to the saved point cleans every pane");
}

void TestTextViewportSplitSiblingCaretsSurviveAForeignEdit() {
  TextViewport pane_a;
  pane_a.SetViewportSize(20, 200);
  pane_a.LoadContent("alpha\nbravo\ncharlie\ndelta\necho\n", "/tmp/split-carets.txt");
  TextViewport pane_b = pane_a;
  pane_b.SetViewportSize(20, 200);

  pane_b.MoveCursorTo(4, 3);
  pane_b.AddSecondaryCaret(3, 2);
  Expect(pane_b.has_multiple_carets(), "pane B starts with two carets low in the file");

  pane_a.MoveCursorTo(0, 0);
  pane_a.MoveCursorTo(3, 0, true);
  pane_a.Backspace();
  Expect(pane_b.line_count() == 3, "pane B sees the shorter document");

  pane_b.InsertCharacter('Z');
  Expect(pane_b.lines().LineView(0) == "delta" && pane_b.lines().LineView(1) == "echo",
         "the surviving lines are untouched");
  Expect(pane_b.lines().LineView(2) == "Z",
         "one keystroke inserts ONE character, not one per stale caret: got \"" +
             std::string(pane_b.lines().LineView(2)) + "\"");
  Expect(!pane_b.has_multiple_carets(),
         "the two stale carets clamped onto one position and merged into one cursor");
  Expect(pane_b.cursor_line() < pane_b.line_count(),
         "pane B's caret is inside the document");
}

void TestTextViewportStaleSiblingIsReconciledByEveryVerb() {
  struct Verb {
    const char* name;
    void (*run)(TextViewport&);
  };
  static const Verb verbs[] = {
      {"Left", [](TextViewport& v) { v.MoveCursorHorizontal(-1); }},
      {"Right", [](TextViewport& v) { v.MoveCursorHorizontal(1); }},
      {"Up", [](TextViewport& v) { v.MoveCursorVertical(-1); }},
      {"Down", [](TextViewport& v) { v.MoveCursorVertical(1); }},
      {"WordLeft", [](TextViewport& v) { v.MoveCursorWord(-1); }},
      {"WordRight", [](TextViewport& v) { v.MoveCursorWord(1); }},
      {"Home", [](TextViewport& v) { v.MoveCursorLineStart(); }},
      {"End", [](TextViewport& v) { v.MoveCursorLineEnd(); }},
      {"type", [](TextViewport& v) { v.InsertCharacter('q'); }},
      {"newline", [](TextViewport& v) { v.InsertNewline(); }},
      {"tab", [](TextViewport& v) { v.InsertTab(); }},
      {"backspace", [](TextViewport& v) { v.Backspace(); }},
      {"delete", [](TextViewport& v) { v.DeleteForward(); }},
      {"delete-word-left", [](TextViewport& v) { v.DeleteWord(-1); }},
      {"delete-word-right", [](TextViewport& v) { v.DeleteWord(1); }},
      {"paste", [](TextViewport& v) { v.PasteText("p"); }},
      {"select-all", [](TextViewport& v) { v.SelectAll(); }},
      {"select-word", [](TextViewport& v) { v.SelectWordAtCursor(); }},
      {"select-line", [](TextViewport& v) { v.SelectLineAtCursor(); }},
      {"add-caret-below", [](TextViewport& v) { v.AddCaretVertical(1); }},
      {"add-secondary", [](TextViewport& v) { v.AddSecondaryCaret(0, 0); }},
      {"box", [](TextViewport& v) { v.SetBoxSelection({0, 0}, {1, 1}); }},
      {"undo", [](TextViewport& v) { (void)v.Undo(); }},
  };

  for (const Verb& verb : verbs) {
    TextViewport pane_a;
    pane_a.SetViewportSize(20, 200);
    pane_a.LoadContent("alpha\nbravo\ncharlie\ndelta\necho\n", "/tmp/stale-verbs.txt");
    TextViewport pane_b = pane_a;
    pane_b.SetViewportSize(20, 200);
    pane_b.MoveCursorTo(4, 3);
    pane_b.AddSecondaryCaret(3, 2);
    pane_a.MoveCursorTo(0, 0);
    pane_a.MoveCursorTo(3, 0, true);
    pane_a.Backspace();

    verb.run(pane_b);

    const std::string where = std::string(verb.name) + ": ";
    Expect(pane_b.cursor_line() < pane_b.line_count(),
           where + "the primary caret is past the end of the document (line " +
               std::to_string(pane_b.cursor_line()) + " of " +
               std::to_string(pane_b.line_count()) + ")");
    Expect(pane_b.cursor_column() <= pane_b.lines().LineLength(pane_b.cursor_line()),
           where + "the primary caret is past the end of its line");
    for (const auto& caret : pane_b.secondary_caret_ranges()) {
      Expect(caret.position.line < pane_b.line_count(),
             where + "a secondary caret is past the end of the document");
      Expect(caret.position.column <= pane_b.lines().LineLength(caret.position.line),
             where + "a secondary caret is past the end of its line");
      if (caret.selection_anchor.has_value()) {
        Expect(caret.selection_anchor->line < pane_b.line_count(),
               where + "a secondary anchor is past the end of the document");
      }
    }
  }
}

void TestTextViewportSettledEditKeepsTheLinesBelowCurrent() {
  std::string text;
  for (int i = 0; i < 80; ++i) {
    text += "int v" + std::to_string(i) + " = " + std::to_string(i) + ";\n";
  }
  TextViewport viewport;
  viewport.LoadContent(text, "/tmp/settle.cpp");
  for (std::size_t line = 0; line < 80; ++line) {
    (void)viewport.HighlightedLineTokens(line);
  }
  viewport.MoveCursorTo(40, 4);
  viewport.InsertText("x");
  Expect(!viewport.CurrentHighlightedLineTokens(60).has_value(), "the edit stales the lines below it");
  viewport.SettleHighlightAfterLastEdit();
  for (const std::size_t line : {41u, 60u, 79u}) {
    const auto settled = viewport.CurrentHighlightedLineTokens(line);
    Expect(settled.has_value(), "line " + std::to_string(line) + " is current after settling");
    TextViewport fresh;
    fresh.LoadContent(viewport.SerializeText(), "/tmp/settle.cpp");
    const std::vector<SyntaxTokenKind>& expected = fresh.HighlightedLineTokens(line);
    Expect(settled.has_value() && std::vector<SyntaxTokenKind>(settled->begin(), settled->end()) == expected,
           "line " + std::to_string(line) + " settles to what a fresh tokenization gives");
  }
  Expect(viewport.CurrentHighlightedLineTokens(40).has_value(), "the edited line itself was tokenized");

  viewport.MoveCursorTo(50, 0);
  viewport.InsertText("/*");
  viewport.SettleHighlightAfterLastEdit();
  Expect(!viewport.CurrentHighlightedLineTokens(60).has_value(),
         "a changed carry leaves the lines below stale, to be retokenized");
}

void TestTextViewportEditStalesTokenCacheSuffixOnly() {
  TextViewport viewport;
  viewport.LoadContent(
      "int a = 1;\nint b = 2;\nint c = 3;\nint d = 4;\nint e = 5;\n",
      "/tmp/token-suffix.cpp");

  for (std::size_t line = 0; line < 5; ++line) {
    Expect(!viewport.HighlightedLineTokens(line).empty(), "every line should tokenize");
  }
  for (std::size_t line = 0; line < 5; ++line) {
    Expect(!viewport.HighlightedLineTokensIfCached(line).empty(),
           "every line should now be cached");
  }
  const std::vector<SyntaxTokenKind> line0_before = viewport.HighlightedLineTokens(0);

  viewport.MoveCursorTo(2, 0);
  viewport.InsertText("/*");

  Expect(!viewport.HighlightedLineTokensIfCached(0).empty(),
         "an edit at line 2 must leave line 0's cached tokens valid");
  Expect(!viewport.HighlightedLineTokensIfCached(1).empty(),
         "an edit at line 2 must leave line 1's cached tokens valid");
  for (std::size_t line = 2; line < 5; ++line) {
    Expect(viewport.HighlightedLineTokensIfCached(line).empty(),
           "an edit at line 2 must stale line 2 and everything after it");
  }

  Expect(viewport.HighlightedLineTokens(0) == line0_before,
         "the retained entry above the edit must still hold line 0's real tokens");
  for (std::size_t line = 3; line < 5; ++line) {
    const auto& tokens = viewport.HighlightedLineTokens(line);
    Expect(!tokens.empty() && std::all_of(tokens.begin(), tokens.end(),
                                          [](SyntaxTokenKind kind) {
                                            return kind == SyntaxTokenKind::Comment;
                                          }),
           "lines after the opened block comment must retokenize as comment");
  }
}

void TestTextViewportSyntaxReloadStalesEveryTokenCacheEntry() {
  TextViewport viewport;
  viewport.LoadContent("int a = 1;\nint b = 2;\n", "/tmp/token-reload.cpp");
  Expect(!viewport.HighlightedLineTokens(0).empty(), "line 0 should tokenize");
  Expect(!viewport.HighlightedLineTokens(1).empty(), "line 1 should tokenize");
  Expect(!viewport.HighlightedLineTokensIfCached(1).empty(), "line 1 should be cached");

  viewport.InvalidateSyntaxHighlighting();

  Expect(viewport.HighlightedLineTokensIfCached(0).empty(),
         "a syntax invalidation must stale line 0");
  Expect(viewport.HighlightedLineTokensIfCached(1).empty(),
         "a syntax invalidation must stale line 1");
  Expect(!viewport.HighlightedLineTokens(0).empty(),
         "the next query must recompute rather than return the retained buffer");
}

void TestTextViewportEditRecyclesVisibleLineLayouts() {
  TextViewport viewport;
  std::string content;
  for (int i = 0; i < 40; ++i) {
    content += "int value_" + std::to_string(i) + " = " + std::to_string(i) + ";\n";
  }
  viewport.LoadContent(content, "/tmp/layout-recycle.cpp");
  viewport.SetViewportSize(30, 80);
  const auto paint = [&]() {
    for (std::size_t line = 0; line < 30; ++line) {
      (void)viewport.VisibleLineLayoutRef(line);
    }
  };
  paint();

  const std::uint64_t before =
      util::ReadPerformanceCounter(util::PerfCounterId::EditorVisibleLineLayoutRecycled);
  viewport.MoveCursorTo(0, 0);
  viewport.InsertText("x");
  paint();
  const std::uint64_t after =
      util::ReadPerformanceCounter(util::PerfCounterId::EditorVisibleLineLayoutRecycled);

  Expect(after > before,
         "the repaint after an edit must build into the layouts the invalidation retired");

#if MICROIDE_PERF_HARNESS_BUILD
  namespace perf = microide::tests::perf;
  const auto snapshot = perf::Allocations::Snapshot();
  viewport.MoveCursorTo(0, 0);
  viewport.InsertText("y");
  paint();
  const auto delta = perf::Allocations::DeltaSince(snapshot);

  Expect(delta.allocations < 30,
         "an edit plus repaint must rebuild every invalidated row into a pooled "
         "node, not allocate one per visible row");
#endif
}

void TestTextViewportPlainAsciiPrefixMemoMatchesTheDefinition() {
  const auto first_non_plain = [](std::string_view line, std::size_t probe) {
    const std::size_t limit = std::min(probe, line.size());
    for (std::size_t i = 0; i < limit; ++i) {
      const auto byte = static_cast<unsigned char>(line[i]);
      if (byte == '\t' || byte >= 0x80) {
        return i;
      }
    }
    return limit;
  };

  const std::string plain(600, 'x');
  const std::string tabbed = std::string(300, 'a') + "\t" + std::string(300, 'b');
  const std::string wide = std::string(40, 'c') + "\xc3\xa9" + std::string(500, 'd');
  const std::string content = plain + "\n" + tabbed + "\n" + wide + "\n";
  const std::array<std::string_view, 3> lines{plain, tabbed, wide};

  TextViewport viewport;
  viewport.LoadContent(content, "/tmp/prefix-memo.txt");

  const std::array<std::size_t, 8> probes{0, 700, 10, 301, 300, 299, 650, 41};
  for (int round = 0; round < 3; ++round) {
    for (std::size_t line = 0; line < lines.size(); ++line) {
      for (const std::size_t probe : probes) {
        const std::size_t expected = first_non_plain(lines[line], probe);
        Expect(viewport.PlainAsciiPrefixEnd(line, probe) == expected,
               "the memoized plain-ASCII prefix must equal the definition");
      }
    }
  }

  viewport.MoveCursorTo(0, 0);
  viewport.InsertText("\t");
  for (const std::size_t probe : probes) {
    const std::size_t expected = first_non_plain(viewport.lines()[0], probe);
    Expect(viewport.PlainAsciiPrefixEnd(0, probe) == expected,
           "an edit must not leave the prefix memo answering for the old line");
  }
}

void TestTextViewportScrollRecyclesHighlightCacheNodes() {
  TextViewport viewport;
  std::string content;
  for (int i = 0; i < 900; ++i) {
    content += "int value_" + std::to_string(i) + " = " + std::to_string(i) + ";\n";
  }
  viewport.LoadContent(content, "/tmp/highlight-recycle.cpp");
  Expect(viewport.syntax_highlighting_enabled(), "the fixture must actually tokenize");

  for (std::size_t line = 0; line < 300; ++line) {
    (void)viewport.HighlightedLineTokens(line);
  }

  const std::uint64_t evictions_before =
      util::ReadPerformanceCounter(util::PerfCounterId::EditorHighlightCacheEvictions);
  const std::uint64_t recycled_before =
      util::ReadPerformanceCounter(util::PerfCounterId::EditorHighlightCacheNodesRecycled);
  for (std::size_t line = 300; line < 900; ++line) {
    (void)viewport.HighlightedLineTokens(line);
  }
  const std::uint64_t evictions =
      util::ReadPerformanceCounter(util::PerfCounterId::EditorHighlightCacheEvictions) -
      evictions_before;
  const std::uint64_t recycled =
      util::ReadPerformanceCounter(util::PerfCounterId::EditorHighlightCacheNodesRecycled) -
      recycled_before;

  Expect(evictions >= 500,
         "600 fresh lines against a 256-entry cap must evict, or this proves nothing");
  Expect(recycled == evictions,
         "every eviction past the cap must hand its node to the line replacing it");

  const std::vector<SyntaxTokenKind>& last = viewport.HighlightedLineTokens(899);
  Expect(last.size() == viewport.lines()[899].size(),
         "a recycled token buffer must be resized to the line it now caches");
}

void TestTextViewportScrollHighlightCacheStopsAllocatingAtTheCap() {
#if MICROIDE_PERF_HARNESS_BUILD
  namespace perf = microide::tests::perf;
  TextViewport viewport;
  std::string content;
  for (int i = 0; i < 900; ++i) {
    content += "int value_" + std::to_string(i) + " = " + std::to_string(i) + ";\n";
  }
  viewport.LoadContent(content, "/tmp/highlight-recycle-alloc.cpp");

  for (std::size_t line = 0; line < 400; ++line) {
    (void)viewport.HighlightedLineTokens(line);
  }

  const auto before = perf::Allocations::Snapshot();
  for (std::size_t line = 400; line < 900; ++line) {
    (void)viewport.HighlightedLineTokens(line);
  }
  const auto delta = perf::Allocations::DeltaSince(before);

  Expect(delta.allocations < 200,
         "scrolling past the token cache's cap must reuse the evicted entry, not "
         "free and re-allocate one per line");
#endif
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

  viewport.ResetCacheStats();
  const auto& first_tokens = viewport.HighlightedLineTokens(4095);
  Expect(!first_tokens.empty(), "far-line syntax queries should produce tokens immediately");
  const auto first_stats = viewport.CacheStats();
  Expect(first_stats.highlight_state_advances <= 600,
         "a deep cold jump must bound synchronous replay (remainder deferred off-thread)");
  Expect(viewport.TakeHighlightCheckpointBackfillRequest().has_value(),
         "a deep cold jump should arm an off-thread checkpoint backfill");

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
  DrainHighlightCheckpointBackfill(viewport, 4095);
  viewport.MoveCursorTo(4095, viewport.lines().back().size());
  viewport.InsertText(" // tail");
  viewport.ResetCacheStats();

  const auto& tokens = viewport.HighlightedLineTokens(4095);
  Expect(!tokens.empty(), "tail edits should preserve syntax tokens for the edited line");

  const auto stats = viewport.CacheStats();
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

void TestTextViewportInsertNewlineOverDownwardSelectionIndentsFromStartLine() {
  TextViewport viewport;
  viewport.LoadContent("    alpha\nbeta\n", "/tmp/indent-newline-sel.cpp");

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

void TestTextViewportDerivedCacheBytesTracksEachCache() {
  TextViewport viewport;
  std::string content;
  for (int i = 0; i < 400; ++i) {
    content += "int value_" + std::to_string(i) + " = " + std::to_string(i) + ";\n";
  }
  viewport.LoadContent(content, "/tmp/derived-cache.cpp");

  const auto empty = viewport.DerivedCacheBytes();
  Expect(empty.undo_history == 0, "a freshly loaded buffer holds no undo history");

  viewport.SetViewportSize(40, 120);
  (void)viewport.max_visual_columns();
  for (std::size_t row = 0; row < 40; ++row) {
    (void)viewport.VisibleLineLayout(row);
  }
  const auto warmed = viewport.DerivedCacheBytes();
  Expect(warmed.layout_cache > empty.layout_cache,
         "warming the width table and the visible-line LRU must move layout_cache");

  for (std::size_t line = 0; line < 200; ++line) {
    (void)viewport.HighlightedLineTokens(line);
  }
  const auto highlighted = viewport.DerivedCacheBytes();
  Expect(highlighted.highlight_tokens > warmed.highlight_tokens,
         "highlighting lines must move highlight_tokens");
  Expect(highlighted.highlight_states > 0,
         "the per-line highlighter state chain holds the highlighted prefix");

  viewport.MoveCursorTo(0, 0, false);
  for (int i = 0; i < 8; ++i) {
    viewport.InsertCharacter('x');
  }
  const auto edited = viewport.DerivedCacheBytes();
  Expect(edited.undo_history > 0, "typing must move undo_history");

  Expect(edited.total() == edited.layout_cache + edited.highlight_tokens +
                               edited.highlight_states + edited.caret_caches +
                               edited.undo_history,
         "total() must be the sum of every reported component, or a component "
         "that stops being counted disappears silently");
}

void TestTextViewportHighlightStatesGrowWithTheFrontier() {
  TextViewport viewport;
  std::string content;
  constexpr std::size_t kLines = 300'000;
  for (std::size_t i = 0; i < kLines; ++i) {
    content += i % 100 == 0 ? "/* a block\n" : i % 100 == 1 ? "   comment */\n" : "int x = 1;\n";
  }
  viewport.LoadContent(content, "/tmp/frontier.cpp");
  viewport.SetViewportSize(40, 120);
  for (std::size_t line = 0; line < 40; ++line) {
    (void)viewport.HighlightedLineTokens(line);
  }
  const std::size_t bytes = viewport.DerivedCacheBytes().highlight_states;
  Expect(bytes > 0, "painted lines record their states");
  Expect(bytes < kLines * sizeof(microide::editor::SyntaxState) / 4,
         "painting the top must not size the state chain to the document: " +
             std::to_string(bytes) + " bytes");

  viewport.MoveCursorTo(5, 0, false);
  viewport.InsertText("/* opened ");
  viewport.InsertText("*/");
  const std::size_t deep = 200'001;
  const auto deep_tokens = std::vector(viewport.HighlightedLineTokens(deep).begin(),
                                       viewport.HighlightedLineTokens(deep).end());
  TextViewport fresh;
  fresh.LoadContent(viewport.SerializeDocumentText(), "/tmp/frontier.cpp");
  fresh.SetViewportSize(40, 120);
  const auto fresh_tokens = std::vector(fresh.HighlightedLineTokens(deep).begin(),
                                        fresh.HighlightedLineTokens(deep).end());
  Expect(deep_tokens == fresh_tokens, "deep highlighting after an edit matches a fresh buffer");
}

void TestTextViewportTabSizeRoundTripKeepsTheWidthTable() {
  TextViewport viewport;
  std::string content;
  for (int i = 0; i < 2000; ++i) {
    content += i == 700 ? "\t\t\twide with tabs\n" : "plain line\n";
  }
  viewport.LoadContent(content, "/tmp/tabs.txt");
  viewport.SetTabSize(4);
  viewport.PrebuildLineWidths();
  const std::size_t at_four = viewport.max_visual_columns();

  microide::util::ResetPerformanceCounters();
  viewport.SetTabSize(8);
  viewport.SetTabSize(4);
  Expect(viewport.max_visual_columns() == at_four, "the round trip answers the same width");
  Expect(microide::util::ReadPerformanceCounter(microide::util::PerfCounterId::EditorLineWidthFullMeasures) == 0,
         "and measures no line again");

  viewport.SetTabSize(8);
  Expect(viewport.max_visual_columns() == at_four + 12, "three tabs widen by 4 columns each");
  Expect(microide::util::ReadPerformanceCounter(
             microide::util::PerfCounterId::EditorLineWidthFullMeasures) == viewport.line_count(),
         "a new size measures every line once");

  microide::util::ResetPerformanceCounters();
  viewport.SetTabSize(4);
  viewport.PrebuildLineWidths();
  viewport.MoveCursorTo(700, 0, false);
  viewport.InsertCharacter('\t');
  viewport.SetTabSize(8);
  viewport.SetTabSize(4);
  Expect(viewport.max_visual_columns() == at_four + 4, "the edit's extra tab is measured");
  microide::util::ResetPerformanceCounters();
}

void TestTextViewportNoOpRangeReplaceDoesNotDirty() {
  TextViewport viewport;
  viewport.LoadContent("hello world\nsecond line\n", "/tmp/noop-range.txt");
  const std::uint64_t revision_before = viewport.content_revision();
  Expect(!viewport.dirty(), "freshly loaded buffer is clean");

  const microide::editor::SelectionRange range{
      .start = microide::editor::TextPosition{0, 6},
      .end = microide::editor::TextPosition{0, 11}};
  const bool changed = viewport.ReplaceRange(range, "world", true);
  Expect(!changed, "an identical range replacement reports no change");
  Expect(viewport.content_revision() == revision_before,
         "a no-op range replacement must not bump content_revision");
  Expect(!viewport.dirty(), "a no-op range replacement must not dirty the buffer");
  Expect(!viewport.Undo(), "a no-op range replacement must not create an undo entry");

  Expect(viewport.ReplaceRange(range, "WORLD", true),
         "a real range replacement still applies");
  Expect(viewport.content_revision() != revision_before, "a real edit bumps content_revision");
  Expect(viewport.dirty(), "a real edit dirties the buffer");
}

void TestTextViewportNoOpLineReplaceDoesNotDirty() {
  TextViewport viewport;
  viewport.LoadContent("alpha\nbeta\ngamma\n", "/tmp/noop-line.txt");
  const std::uint64_t revision_before = viewport.content_revision();

  const bool changed = viewport.ReplaceLines(0, 2, {"alpha", "beta"}, true);
  Expect(!changed, "an identical line replacement reports no change");
  Expect(viewport.content_revision() == revision_before,
         "a no-op line replacement must not bump content_revision");
  Expect(!viewport.dirty(), "a no-op line replacement must not dirty the buffer");
  Expect(!viewport.Undo(), "a no-op line replacement must not create an undo entry");

  Expect(viewport.ReplaceLines(0, 2, {"beta", "alpha"}, true),
         "a reordering line replacement still applies");
  Expect(viewport.dirty(), "a real line replacement dirties the buffer");
}

void TestTextViewportCenterLinePutsTheLineMidViewportAndClamps() {
  TextViewport viewport;
  std::string text;
  for (int i = 0; i < 200; ++i) {
    text += "line " + std::to_string(i) + "\n";
  }
  viewport.LoadContent(text, "/tmp/center-line.txt");
  viewport.SetViewportSize(20, 80);

  viewport.CenterLine(100);
  Expect(viewport.scroll_line() == 90, "line 100 in a 20-row view scrolls to row 90 (10 above it)");

  viewport.CenterLine(3);
  Expect(viewport.scroll_line() == 0, "a line near the top cannot scroll above row 0");

  viewport.CenterLine(198);
  const std::size_t max_scroll = viewport.lines().size() - 20;
  Expect(viewport.scroll_line() == max_scroll,
         "a line near EOF settles at the last full page, not past it");

  viewport.CenterLine(100000);
  Expect(viewport.scroll_line() == max_scroll, "an out-of-range line clamps to the end");

  TextViewport fresh;
  fresh.LoadContent(text, "/tmp/center-line-fresh.txt");
  fresh.CenterLine(100);
  fresh.SetViewportSize(20, 80);
  Expect(fresh.scroll_line() == 90, "the first sizing after CenterLine re-centres the line");
  fresh.SetScrollLine(10);
  fresh.SetViewportSize(30, 80);
  Expect(fresh.scroll_line() == 10, "the request is one-shot: a later resize does not re-centre");

  TextViewport wrapped;
  std::string wrapped_text;
  for (int i = 0; i < 100; ++i) {
    wrapped_text += std::string(30, 'x') + "\n";
  }
  wrapped.LoadContent(wrapped_text, "/tmp/center-line-wrapped.txt");
  wrapped.SetSoftWrap(true);
  wrapped.SetViewportSize(10, 10);
  wrapped.CenterLine(20);
  Expect(wrapped.scroll_line() == 20 * 3 - 5,
         "under soft wrap the line's first visual row is what gets centred");
}

void TestTextViewportEmptyLineDeleteIsNotTreatedAsNoOp() {
  TextViewport viewport;
  viewport.LoadContent("a\nb\nc\n", "/tmp/line-shrink.txt");
  const std::size_t lines_before = viewport.lines().size();

  const bool changed = viewport.ReplaceLines(0, 2, {"ab"}, true);
  Expect(changed, "collapsing two lines into one is a real edit, not a no-op");
  Expect(viewport.lines().size() == lines_before - 1, "the line count shrinks by one");
  Expect(viewport.dirty(), "a real line-count change dirties the buffer");
}

void TestTextViewportSameLineCountEditInvalidatesSyntaxCache() {
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

void TestTextViewportMultiCaretDeleteLineClearsLastAppliedEdit() {
  TextViewport viewport;
  viewport.LoadContent("alpha\nbeta\ngamma\ndelta\n", "/tmp/multi-delete-applied.txt");

  viewport.MoveCursorTo(0, 2);
  viewport.InsertText("Z");
  Expect(viewport.last_applied_edit().has_value(),
         "single-caret insert should publish an applied edit as the stale baseline");

  viewport.MoveCursorTo(0, 0);
  viewport.AddSecondaryCaret(2, 0);
  Expect(viewport.has_multiple_carets(),
         "test setup should establish a multi-caret configuration");
  Expect(viewport.DeleteCurrentLine(),
         "multi-caret DeleteCurrentLine should report a successful edit");

  Expect(!viewport.last_applied_edit().has_value(),
         "multi-caret line delete must clear last_applied_edit_ so LSP/breakpoints resync");
}

void TestTextViewportReplaceAllClearsLastAppliedEdit() {
  TextViewport viewport;
  viewport.LoadContent("cat\ndog cat\ncat\n", "/tmp/replace-all-applied.txt");
  viewport.MoveCursorTo(0, 0);
  viewport.InsertText("Z");
  Expect(viewport.last_applied_edit().has_value(), "the insert leaves an applied edit as the stale baseline");
  Expect(viewport.ReplaceAll("cat", "cow") == 3, "ReplaceAll should replace the three cats");
  Expect(!viewport.last_applied_edit().has_value(), "ReplaceAll must not leave the previous edit behind");

  viewport.InsertText("Y");
  Expect(viewport.last_applied_edit().has_value(), "a fresh insert publishes again");
  const std::optional<std::size_t> replaced =
      viewport.ReplaceAllRanges({SelectionRange{{1, 4}, {1, 7}}, SelectionRange{{2, 0}, {2, 3}}}, "pig");
  Expect(replaced == std::optional<std::size_t>{2}, "ReplaceAllRanges should apply both ranges");
  Expect(!viewport.last_applied_edit().has_value(), "ReplaceAllRanges must not leave the previous edit behind");
}

void TestTextViewportReplaceAllRangesCarriesTheCarets() {
  TextViewport viewport;
  viewport.LoadContent("aaaa bb aaaa\nkeep\naaaa\n", "/tmp/replace-all-carets.txt");
  viewport.MoveCursorTo(2, 4);
  viewport.AddSecondaryCaret(0, 12);
  viewport.AddSecondaryCaret(0, 6);
  const std::optional<std::size_t> replaced = viewport.ReplaceAllRanges(
      {SelectionRange{{0, 0}, {0, 4}}, SelectionRange{{0, 8}, {0, 12}}, SelectionRange{{2, 0}, {2, 4}}}, "");
  Expect(replaced == std::optional<std::size_t>{3}, "the three runs are replaced");
  Expect(viewport.cursor_line() == 2 && viewport.cursor_column() == 0,
         "the caret after line 3's removed run is at its start: " + std::to_string(viewport.cursor_line()) +
             ":" + std::to_string(viewport.cursor_column()));
  const std::vector<TextPosition> others = viewport.secondary_carets();
  Expect(others.size() == 2, "both other carets survive: " + std::to_string(others.size()));
  bool end_of_line_one = false;
  bool inside_bb = false;
  for (const auto& caret : others) {
    end_of_line_one = end_of_line_one || (caret.line == 0 && caret.column == 4);
    inside_bb = inside_bb || (caret.line == 0 && caret.column == 2);
  }
  Expect(end_of_line_one, "the caret at line 1's end stays at its end, now column 4");
  Expect(inside_bb, "the caret inside \"bb\" moves left with it, to column 2");
  Expect(viewport.CaretBoundsFault().empty(), "no caret is left past its line: " + viewport.CaretBoundsFault());

  TextViewport by_needle;
  by_needle.LoadContent("x\nxyxyxyxy\n", "/tmp/replace-all-needle-carets.txt");
  by_needle.MoveCursorTo(1, 8);
  by_needle.AddSecondaryCaret(1, 2);
  by_needle.AddSecondaryCaret(1, 4);
  Expect(by_needle.ReplaceAll("xy", "") == 4, "the four runs are replaced");
  Expect(by_needle.cursor_line() == 1 && by_needle.cursor_column() == 0,
         "the caret ends on the emptied line's end: " + std::to_string(by_needle.cursor_line()) + ":" +
             std::to_string(by_needle.cursor_column()));
  Expect(by_needle.secondary_carets().empty(), "carets that meet merge into the primary");
  Expect(by_needle.CaretBoundsFault().empty(), "no caret is left past its line: " + by_needle.CaretBoundsFault());
}

void TestTextViewportUndoOntoAPlaceholderBufferKeepsNoHighlightState() {
  TextViewport viewport;
  viewport.SetPlaceholderText("Welcome\nOpen a file to start\nor create one\n");
  Expect(viewport.is_placeholder(), "the buffer starts as a placeholder");
  viewport.MoveCursorTo(1, 0);
  viewport.InsertText("typed ");
  Expect(viewport.Undo(), "the edit can be undone");
  viewport.MoveCursorTo(2, 0);
  viewport.InsertText("again ");
  Expect(viewport.Undo(), "and undone again");
  Expect(viewport.lines()[1] == "Open a file to start", "the text is back as it was");
}

void TestTextViewportSetLineEndingIsAnUndoableEdit() {
  TextViewport viewport;
  viewport.LoadContent("one\r\ntwo\r\n", "/tmp/eol-change.txt");
  Expect(viewport.line_ending() == TextViewport::LineEnding::CRLF && !viewport.dirty(), "a clean CRLF file");
  Expect(viewport.SetLineEnding(TextViewport::LineEnding::LF), "switching to LF is an edit");
  Expect(viewport.line_ending() == TextViewport::LineEnding::LF && viewport.dirty(), "the buffer is LF and dirty");
  Expect(viewport.SerializeDocumentText() == "one\ntwo\n", "and is written with LF");
  Expect(!viewport.SetLineEnding(TextViewport::LineEnding::LF), "switching to the ending it has is nothing");
  Expect(viewport.Undo(), "undo takes the change back");
  Expect(viewport.line_ending() == TextViewport::LineEnding::CRLF && !viewport.dirty(),
         "back to CRLF, and clean at the save point");
  Expect(viewport.Redo(), "redo redoes it");
  Expect(viewport.line_ending() == TextViewport::LineEnding::LF && viewport.dirty(), "LF and dirty again");
  Expect(viewport.lines().size() == 3 && viewport.lines()[1] == "two", "the text itself never changed");
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

void TestTextViewportLastAppliedEditLineSpanMatchesWholeLineDiff() {
  using microide::editor::AppliedEditLineSpan;

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

  Expect(viewport.Undo(), "first undo after a typing run should succeed");
  Expect(viewport.lines()[0] == "foo ",
         "undo should remove a whole word, not a single character");

  Expect(viewport.Undo(), "second undo should remove the first word run");
  Expect(viewport.lines()[0].empty(),
         "second undo should clear the remaining typed run");

  Expect(!viewport.Undo(),
         "two undos should exhaust a 'foo bar' typing run (no per-character entries)");

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

  viewport.InsertCharacter('a');
  viewport.MoveCursorHorizontal(1);
  viewport.MoveCursorHorizontal(-1);
  viewport.InsertCharacter('b');
  Expect(viewport.lines()[0] == "abZ", "fixture should type 'ab' around a caret round-trip");

  Expect(viewport.Undo() && viewport.lines()[0] == "aZ",
         "a same-column caret round-trip should isolate 'b' in its own undo step");
  Expect(viewport.Undo() && viewport.lines()[0] == "Z",
         "the first character should undo separately");
}

void TestTextViewportMoveCursorToSameColumnBreaksTypingCoalesce() {
  TextViewport viewport;
  viewport.LoadContent("", "/tmp/undo-coalesce-clickback.txt");

  viewport.InsertCharacter('a');
  viewport.InsertCharacter('b');
  viewport.InsertCharacter('c');
  viewport.MoveCursorTo(0, 0);
  viewport.MoveCursorTo(0, 3);
  viewport.InsertCharacter('d');
  Expect(viewport.lines()[0] == "abcd", "fixture should append 'd' after a click round-trip");

  Expect(viewport.Undo() && viewport.lines()[0] == "abc",
         "a MoveCursorTo round-trip must isolate 'd' in its own undo step");
  Expect(viewport.Undo() && viewport.lines()[0].empty(),
         "the original 'abc' run should undo as one step");
}

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
        continue;
      }
      if (model[line].compare(start, end - start, replacement) == 0) {
        continue;
      }

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

  viewport.MoveCursorTo(0, 2, false);
  viewport.InsertNewline();
  Expect(!viewport.TopUndoEntryIsColumnScopedForTesting(),
         "splitting a line must fall back to the line-vector entry");

  viewport.MoveCursorTo(1, 0, false);
  viewport.Backspace();
  Expect(!viewport.TopUndoEntryIsColumnScopedForTesting(),
         "joining two lines must fall back to the line-vector entry");

  viewport.MoveCursorTo(0, 4, false);
  viewport.InsertText("multi\nline");
  Expect(!viewport.TopUndoEntryIsColumnScopedForTesting(),
         "a multi-line insertion must fall back to the line-vector entry");

  viewport.BeginUndoGroup();
  Expect(viewport.ReplaceRange(SelectionRange{TextPosition{0, 0}, TextPosition{0, 1}}, "Q", true),
         "the grouped child edit applies");
  viewport.EndUndoGroup();
  Expect(!viewport.TopUndoEntryIsColumnScopedForTesting(),
         "a grouped in-line child must be widened to the line form before it is recorded");
}

void TestApplyEntryToBufferAppliesEveryDisjointPart() {
  using microide::editor::TextBuffer;
  using microide::editor::TextViewportUndoHistory;

  const auto load = [](TextBuffer& buffer, const std::vector<std::string>& lines) {
    std::string text;
    for (std::size_t i = 0; i < lines.size(); ++i) {
      if (i != 0) text.push_back('\n');
      text += lines[i];
    }
    buffer.ResetFromText(std::move(text));
  };
  const auto dump = [](const TextBuffer& buffer) {
    std::vector<std::string> out;
    for (std::size_t i = 0; i < buffer.size(); ++i) {
      out.emplace_back(buffer.LineView(i));
    }
    return out;
  };

  const std::vector<std::string> before = {"line0", "line1", "line2", "line3", "line4",
                                           "line5", "line6", "line7", "line8", "line9"};
  const std::vector<std::string> after = {"line0", "ONE-a", "ONE-b", "line2", "line3",
                                          "line4", "FIVE",  "line6", "line7", "line9"};

  TextViewportUndoHistory::Entry entry;
  entry.start_line = 1;
  entry.before_lines = microide::editor::LineBlob::Of({"line1"});
  entry.after_lines = microide::editor::LineBlob::Of({"ONE-a", "ONE-b"});
  entry.extra_parts.push_back(TextViewportUndoHistory::DisjointPart{
      .before_start = 5,
      .after_start = 6,
      .before_lines = microide::editor::LineBlob::Of({"line5"}),
      .after_lines = microide::editor::LineBlob::Of({"FIVE"})});
  entry.extra_parts.push_back(TextViewportUndoHistory::DisjointPart{
      .before_start = 8, .after_start = 9, .before_lines = microide::editor::LineBlob::Of({"line8"}), .after_lines = {}});
  Expect(entry.is_multi_range(), "the fixture entry must actually be multi-range");

  TextBuffer buffer;
  load(buffer, before);
  TextViewportUndoHistory::ApplyEntryToBuffer(buffer, entry, true);
  Expect(dump(buffer) == after,
         "applying a multi-range entry forward must apply every part, not just the first");

  TextViewportUndoHistory::ApplyEntryToBuffer(buffer, entry, false);
  Expect(dump(buffer) == before,
         "applying it backward must restore every part");
}

namespace save_point {

using microide::editor::CoalesceHint;
using microide::editor::CoalesceKind;
using microide::editor::TextViewportUndoHistory;

TextViewportUndoHistory::Entry Typed(std::size_t column, std::string text) {
  TextViewportUndoHistory::Entry entry;
  entry.is_inline = true;
  entry.start_column = column;
  entry.inserted_text = std::move(text);
  entry.before_state.cursor_column = column;
  entry.after_state.cursor_column = column + entry.inserted_text.size();
  entry.before_state.dirty = true;
  entry.after_state.dirty = true;
  return entry;
}

constexpr CoalesceHint kTyping{.kind = CoalesceKind::Insert};

std::string CleanBoundaries(const TextViewportUndoHistory& history) {
  std::string out;
  const auto note = [&out](const TextViewportUndoHistory::Entry& entry, const char* side) {
    out += entry.inserted_text + ":" + side + " ";
  };
  for (const auto* stack : {&history.UndoStackForTesting(), &history.RedoStackForTesting()}) {
    for (const TextViewportUndoHistory::Entry& entry : *stack) {
      if (!entry.before_state.dirty) note(entry, "before");
      if (!entry.after_state.dirty) note(entry, "after");
    }
  }
  return out;
}

}

void TestASavePointKeepsLaterTypingOutOfTheSavedEntry() {
  using namespace save_point;
  TextViewportUndoHistory history;
  history.RecordEntry(Typed(0, "a"), kTyping);
  const TextViewportUndoHistory::SavePoint point = history.CaptureSavePoint();
  history.RecordEntry(Typed(1, "b"), kTyping);
  Expect(history.UndoStackForTesting().size() == 2,
         "typing after a save point starts its own entry, so the saved state stays a boundary");
  history.MarkSavedAt(point);
  Expect(CleanBoundaries(history) == "a:after b:before ",
         "the boundary the save captured is the clean one: " + CleanBoundaries(history));
  Expect(!history.IsAtSavePoint(point), "the history has moved past the point");
  history.PushRedo(history.PopUndo());
  Expect(history.IsAtSavePoint(point), "and undoing the later typing comes back to it");
}

void TestASavePointSurvivesItsEntryBeingUndone() {
  using namespace save_point;
  TextViewportUndoHistory history;
  history.RecordEntry(Typed(0, "a"), kTyping);
  history.RecordEntry(Typed(5, "b"), kTyping);
  const TextViewportUndoHistory::SavePoint point = history.CaptureSavePoint();
  history.PushRedo(history.PopUndo());
  history.MarkSavedAt(point);
  Expect(CleanBoundaries(history) == "b:after ",
         "an undone saved entry is clean after it, on the redo side: " + CleanBoundaries(history));
}

void TestASavePointAtTheBaseIsLostWithTheOldestEntries() {
  using namespace save_point;
  TextViewportUndoHistory history;
  const TextViewportUndoHistory::SavePoint base = history.CaptureSavePoint();
  history.RecordEntry(Typed(0, "a"), kTyping);
  history.MarkSavedAt(base);
  Expect(CleanBoundaries(history) == "a:before ", "the base is clean before the first entry");
  Expect(!history.IsAtSavePoint(base), "one entry past the base");
  for (std::size_t i = 0; i < 200; ++i) {
    history.RecordEntry(Typed(i * 10, "x"), CoalesceHint{});
  }
  history.MarkSavedAt(base);
  Expect(!history.HasSavedPosition(), "the base fell out of the history with its oldest entries");
}

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
      {"append", 3, "", "a", 4, "", "b"},
      {"prepend", 4, "e", "", 3, "d", ""},
      {"delete-forward", 3, "d", "", 3, "e", ""},
      {"contained", 3, "", "XY", 4, "Y", "Z"},
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
    const std::size_t first_after =
        shape.first_column + std::string(shape.first_inserted).size();
    const TextViewportUndoHistory::Entry first =
        make(shape.first_column, shape.first_removed, shape.first_inserted, shape.first_column,
             first_after);
    const TextViewportUndoHistory::Entry second =
        make(shape.second_column, shape.second_removed, shape.second_inserted, first_after,
             shape.second_column + std::string(shape.second_inserted).size());

    microide::editor::TextBuffer sequential({line});
    TextViewportUndoHistory::ApplyEntryToBuffer(sequential, first, true);
    TextViewportUndoHistory::ApplyEntryToBuffer(sequential, second, true);

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
    TextViewportUndoHistory::ApplyEntryToBuffer(coalesced, merged, true);
    Expect(coalesced.LineView(0) == sequential.LineView(0),
           std::string("the coalesced entry must produce the same text as applying both for ")
               .append(shape.name)
               .c_str());

    TextViewportUndoHistory::ApplyEntryToBuffer(coalesced, merged, false);
    Expect(coalesced.LineView(0) == line,
           std::string("undoing the coalesced entry must restore the original line for ")
               .append(shape.name)
               .c_str());
  }
}

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
  second.start_column = 7;
  second.inserted_text = "b";
  second.before_state.cursor_column = 3;
  second.after_state.cursor_column = 8;

  const CoalesceHint hint{.kind = CoalesceKind::Insert, .changed_is_space = false};
  history.RecordEntry(first, hint);
  history.RecordEntry(second, hint);
  Expect(history.CanUndo(), "the history must hold the second edit");
  (void)history.PopUndo();
  Expect(history.CanUndo(),
         "a non-contiguous in-line edit must start a fresh undo entry, not splice into the run");
}

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

  Expect(viewport.Undo(), "undo must succeed");
  const auto undone = viewport.last_applied_edit();
  Expect(undone.has_value() && undone->replacement_text == "beta",
         "undo must publish the inverse replacement");

  viewport.MoveCursorTo(0, 0, false);
  Expect(viewport.ReplaceRange(
             SelectionRange{TextPosition{0, 0}, TextPosition{0, 5}}, "alpaca", true),
         "the second replacement must apply");
  const auto trimmed = viewport.last_applied_edit();
  Expect(trimmed.has_value(), "the second edit must publish an applied edit");
  Expect(trimmed->range_before.start.column == 3 && trimmed->range_before.end.column == 4,
         "a shared prefix and suffix must be trimmed out of the reported range");
  Expect(trimmed->replacement_text == "ac",
         "only the changed bytes belong in the replacement text");
}

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

void TestTextViewportMultiCaretUndoMatchesSnapshotOracle() {
  auto whole_document = [](const TextViewport& viewport) {
    std::string out;
    for (std::size_t i = 0; i < viewport.lines().size(); ++i) {
      out += viewport.lines().LineView(i);
      out.push_back('\n');
    }
    return out;
  };

  std::string content;
  for (int i = 0; i < 400; ++i) {
    content += "line_" + std::to_string(i) + " value = " + std::to_string(i) + ";\n";
  }
  TextViewport viewport;
  viewport.LoadContent(content, "/tmp/multi-caret-undo-oracle.cpp");

  std::vector<std::string> snapshots;
  snapshots.push_back(whole_document(viewport));
  const auto step = [&](const char* what, auto&& action) {
    action();
    const std::string now = whole_document(viewport);
    Expect(now != snapshots.back(), std::string(what) + " must change the buffer");
    snapshots.push_back(now);
  };

  step("scattered insert", [&] {
    viewport.MoveCursorTo(10, 0);
    viewport.SetSecondaryCarets({{120, 2}, {250, 4}, {399, 0}});
    viewport.InsertText("X");
  });
  step("two carets on one line", [&] {
    viewport.MoveCursorTo(30, 1);
    viewport.SetSecondaryCarets({{30, 5}, {300, 0}});
    viewport.InsertText("Y");
  });
  step("adjacent lines", [&] {
    viewport.MoveCursorTo(60, 0);
    viewport.SetSecondaryCarets({{61, 0}, {62, 0}});
    viewport.InsertText("Z");
  });
  step("scattered newline", [&] {
    viewport.MoveCursorTo(15, 3);
    viewport.SetSecondaryCarets({{150, 3}, {280, 3}});
    viewport.InsertNewline();
  });
  step("scattered joining backspace", [&] {
    viewport.MoveCursorTo(20, 0);
    viewport.SetSecondaryCarets({{200, 0}, {330, 0}});
    viewport.Backspace();
  });
  step("single caret typing", [&] {
    viewport.SetSecondaryCarets({});
    viewport.MoveCursorTo(5, 0);
    viewport.InsertText("plain");
  });
  step("grouped indent across carets", [&] {
    viewport.MoveCursorTo(40, 0);
    viewport.SetSecondaryCarets({{140, 0}, {340, 0}});
    Expect(microide::editor::IndentSelection(viewport), "indent should change the buffer");
  });

  const std::size_t steps = snapshots.size() - 1;
  Expect(steps == 7, "the fixture must have performed every step");

  for (std::size_t i = steps; i-- > 0;) {
    Expect(viewport.Undo(), "undo " + std::to_string(i) + " should succeed");
    Expect(whole_document(viewport) == snapshots[i],
           "undo " + std::to_string(i) + " must restore the recorded document exactly");
  }
  for (std::size_t i = 0; i < steps; ++i) {
    Expect(viewport.Redo(), "redo " + std::to_string(i) + " should succeed");
    Expect(whole_document(viewport) == snapshots[i + 1],
           "redo " + std::to_string(i) + " must reproduce the recorded document exactly");
  }
  for (std::size_t i = steps; i-- > 0;) {
    Expect(viewport.Undo(), "second-pass undo " + std::to_string(i) + " should succeed");
    Expect(whole_document(viewport) == snapshots[i],
           "second-pass undo " + std::to_string(i) + " must restore the document exactly");
  }
}

void TestTextViewportMultiCaretUndoEntryDoesNotSpanTheGap() {
  std::string content;
  for (int i = 0; i < 4000; ++i) {
    content += "line_" + std::to_string(i) + " = " + std::to_string(i) + ";\n";
  }
  TextViewport viewport;
  viewport.LoadContent(content, "/tmp/multi-caret-undo-span.cpp");
  viewport.MoveCursorTo(10, 0);
  viewport.SetSecondaryCarets({{1000, 0}, {2000, 0}, {3900, 0}});

  const std::size_t before_bytes = viewport.DerivedCacheBytes().undo_history;
  viewport.InsertText("X");
  const std::size_t entry_bytes = viewport.DerivedCacheBytes().undo_history - before_bytes;

  Expect(entry_bytes < 4096,
         "an undo entry for four far-apart carets must cost its four ranges, not the "
         "3,891-line span between them (held " +
             std::to_string(entry_bytes) + " bytes)");
  Expect(viewport.Undo(), "the edit must still undo");
  Expect(viewport.lines().LineView(10) == "line_10 = 10;" &&
             viewport.lines().LineView(3900) == "line_3900 = 3900;",
         "undo restores every caret's line");
}

void TestTextViewportGroupedChildEntriesDoNotCopyTheCaretSet() {
#if MICROIDE_PERF_HARNESS_BUILD
  namespace perf = microide::tests::perf;
  constexpr std::size_t kCarets = 256;
  constexpr std::size_t kCaretStride = 5;

  std::string content;
  for (std::size_t i = 0; i < kCarets * kCaretStride + 16; ++i) {
    content += "x\n";
  }
  microide::editor::TextViewport viewport;
  viewport.LoadContent(content, "/tmp/grouped-child-state.txt");
  viewport.SetSoftTabs(true);
  viewport.SetIndentWidth(2);
  viewport.MoveCursorTo(0, 0);
  std::vector<microide::editor::TextPosition> carets;
  carets.reserve(kCarets - 1);
  for (std::size_t i = 1; i < kCarets; ++i) {
    carets.push_back(microide::editor::TextPosition{i * kCaretStride, 0});
  }
  viewport.SetSecondaryCarets(carets);

  Expect(microide::editor::IndentSelection(viewport), "the warm-up indent must apply");

  const auto before = perf::Allocations::Snapshot();
  Expect(microide::editor::IndentSelection(viewport), "the measured indent must apply");
  const auto delta = perf::Allocations::DeltaSince(before);

  constexpr std::uint64_t kBudget = 1u << 20;
  Expect(static_cast<std::uint64_t>(delta.bytes_allocated) < kBudget,
         "a 256-region grouped indent must cost its regions, not carets x regions "
         "copies of the caret set (allocated " +
             std::to_string(delta.bytes_allocated) + " bytes)");

  Expect(viewport.Undo(), "the grouped multi-region indent must undo");
  Expect(viewport.secondary_carets().size() == kCarets - 1,
         "undo restores the whole secondary caret set from the group's own captures");
  Expect(viewport.lines().LineView(kCaretStride) == "  x",
         "undo steps back exactly one indent level");
#endif
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

void TestTextViewportLineVerbsTreatACollapsedFoldAsOneLine() {
  const auto make = [](TextViewport& viewport, FoldingModel& folding_model) {
    viewport.LoadContent("a\n{\n  b\n}\nc\n", "/tmp/fold-line-verbs.c");
    FoldingModel::ComputeOptions fold_options;
    fold_options.bracket_pairs = {{'{', '}'}};
    fold_options.tab_size = 4;
    Expect(folding_model.Compute(viewport.lines().Snapshot(), fold_options), "folds compute");
    Expect(folding_model.Collapse(1), "the brace block collapses");
    viewport.SetFoldingModel(&folding_model);
    viewport.MoveCursorTo(1, 0);
  };
  {
    TextViewport viewport;
    FoldingModel folding_model;
    make(viewport, folding_model);
    Expect(viewport.CollapsedFoldEndAt(1) == 3 && viewport.CollapsedFoldEndAt(0) == 0 &&
               viewport.CollapsedFoldEndAt(2) == 2,
           "only the opener of a collapsed fold names its end");
    Expect(viewport.CurrentLineTextForClipboard() == "{\n  b\n}\n",
           "a line copy of the opener copies the whole block");
    Expect(viewport.DeleteCurrentLine(), "delete line dispatches");
    Expect(viewport.lines().Snapshot() == std::vector<std::string>{"a", "c", ""},
           "delete line removes the whole block");
    Expect(viewport.Undo(), "undo");
    Expect(viewport.lines().Snapshot() == std::vector<std::string>{"a", "{", "  b", "}", "c", ""},
           "undo restores the block");
  }
  {
    TextViewport viewport;
    FoldingModel folding_model;
    make(viewport, folding_model);
    Expect(MoveLineDown(viewport), "move line down dispatches");
    Expect(viewport.lines().Snapshot() == std::vector<std::string>{"a", "c", "{", "  b", "}", ""},
           "the whole block moves down past the next line");
    Expect(viewport.cursor_line() == 2, "the caret stays on the opener");
  }
  {
    TextViewport viewport;
    FoldingModel folding_model;
    make(viewport, folding_model);
    Expect(CopyLines(viewport, true), "duplicate line dispatches");
    Expect(viewport.lines().Snapshot() ==
               std::vector<std::string>{"a", "{", "  b", "}", "{", "  b", "}", "c", ""},
           "the whole block is duplicated");
  }
  {
    TextViewport viewport;
    FoldingModel folding_model;
    make(viewport, folding_model);
    Expect(folding_model.Expand(1), "expand");
    Expect(viewport.CurrentLineTextForClipboard() == "{\n", "an expanded opener is one line");
    Expect(viewport.DeleteCurrentLine(), "delete line dispatches");
    Expect(viewport.lines().Snapshot() == std::vector<std::string>{"a", "  b", "}", "c", ""},
           "and deletes just itself");
  }
}

void TestTextViewportSoftWrapCollapsedFoldOpenerVerticalMotionEscapes() {
  TextViewport viewport;
  viewport.LoadContent(
      "top\nvoid aaaaaaaaaaaaaaaaaaaaaaaa() {\n  x();\n  y();\n}\nbottom\n",
      "/tmp/soft-wrap-fold.cpp");
  viewport.SetViewportSize(6, 8);
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

  viewport.MoveCursorTo(1, 1000);
  Expect(viewport.cursor_line() == 1, "caret starts on the collapsed opener line");

  viewport.MoveCursorVertical(1);
  Expect(viewport.cursor_line() == 5,
         "down from a collapsed soft-wrapped opener must cross the fold to the next visible line");
}

void TestTextViewportHomeAndEndMoveWithinTheWrappedRow() {
  TextViewport viewport;
  viewport.LoadContent("aaaa bbbb cccc dddd eeee ffff gggg hhhh\n", "/tmp/home-end-wrap.txt");
  viewport.SetViewportSize(8, 10);
  viewport.SetSoftWrap(true);

  const std::size_t line_length = viewport.lines().LineLength(0);
  viewport.MoveCursorTo(0, line_length - 2);
  const std::size_t last_row = viewport.cursor_visual_row();
  Expect(last_row > 0, "the fixture must wrap, or view-line and logical-line motion coincide");

  viewport.MoveCursorLineStart();
  Expect(viewport.cursor_visual_row() == last_row,
         "Home must stay on the caret's own wrapped row, not jump to the top of the paragraph");
  Expect(viewport.cursor_column() > 0,
         "Home on a continuation row must land at the ROW's start, not the logical line's");

  viewport.MoveCursorTo(0, viewport.cursor_column() + 1);
  viewport.MoveCursorLineEnd();
  Expect(viewport.cursor_visual_row() == last_row,
         "End must stay on the caret's own wrapped row");
  Expect(viewport.cursor_column() == line_length,
         "End on the LAST wrapped row lands at the end of the logical line");

  viewport.MoveCursorTo(0, 0);
  const std::size_t first_row = viewport.cursor_visual_row();
  viewport.MoveCursorLineEnd();
  Expect(viewport.cursor_column() < line_length,
         "End on a middle wrapped row must stop at the row's end, not the line's");
  Expect(viewport.cursor_visual_row() == first_row,
         "an End that lands exactly on a wrap point must render at that row's trailing edge");
}

void TestTextViewportHomeTogglesBetweenIndentAndColumnZero() {
  TextViewport viewport;
  viewport.LoadContent("    int x = 1;\n", "/tmp/home-indent.txt");
  viewport.SetViewportSize(4, 80);

  viewport.MoveCursorTo(0, 10);
  viewport.MoveCursorLineStart();
  Expect(viewport.cursor_column() == 4, "Home first lands on the first non-whitespace character");
  viewport.MoveCursorLineStart();
  Expect(viewport.cursor_column() == 0, "Home again falls through to column 0");
  viewport.MoveCursorLineStart();
  Expect(viewport.cursor_column() == 4, "and toggles back to the indent");

  viewport.LoadContent("int y = 2;\n", "/tmp/home-no-indent.txt");
  viewport.MoveCursorTo(0, 5);
  viewport.MoveCursorLineStart();
  Expect(viewport.cursor_column() == 0, "with no leading whitespace Home goes straight to 0");
  viewport.MoveCursorLineStart();
  Expect(viewport.cursor_column() == 0, "and stays there");

  viewport.LoadContent("      \n", "/tmp/home-blank.txt");
  viewport.MoveCursorTo(0, 6);
  viewport.MoveCursorLineStart();
  Expect(viewport.cursor_column() == 0, "an all-whitespace line homes to column 0");
}

void TestTextViewportSoftWrapCaretOnFoldHiddenLineEscapesInOneStep() {
  TextViewport viewport;
  viewport.LoadContent(
      "top\nvoid aaaaaaaaaaaaaaaaaaaaaaaa() {\n  x();\n  y();\n}\nbottom\n",
      "/tmp/soft-wrap-fold-hidden.cpp");
  viewport.SetViewportSize(6, 8);
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
  Expect(folding_model.IsLineHidden(2), "line 2 must be inside the collapsed fold");

  Expect(viewport.VisualRowForLine(5) - viewport.VisualRowForLine(1) > 1,
         "the collapsed opener must wrap into several rows for this fixture to test anything");

  viewport.MoveCursorTo(2, 0);
  Expect(viewport.cursor_line() == 2, "caret starts on a fold-hidden line");

  viewport.MoveCursorVertical(1);
  Expect(viewport.cursor_line() == 5,
         "down from a fold-hidden line must cross the fold in one step, not walk the opener's "
         "own wrapped rows");
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

  viewport.Page(-1, true);
  const auto selection = viewport.selection_range();
  Expect(selection.has_value(), "Shift+PageUp must create a selection");
  Expect(viewport.cursor_line() < 20, "the caret must move up a page");
  const SelectionRange normalized = TextViewport::NormalizeRange(*selection);
  Expect(normalized.end.line == 20 && normalized.end.column == 2,
         "the selection anchor stays at the original caret position");
  Expect(normalized.start.line == viewport.cursor_line() &&
             normalized.start.column == viewport.cursor_column(),
         "the selection start follows the paged caret");

  viewport.Page(1, false);
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
void TestTextViewportSoftWrapViewportResizeRebuildsWrapCacheOncePerWidth() {
  TextViewport viewport;
  viewport.LoadContent("abcdefghijklmnopqrst\nabcdefghijklmnopqrst\nabcdefghijklmnopqrst\n",
                       "/tmp/soft-wrap-cache.txt");
  viewport.SetViewportSize(2, 8);
  viewport.SetSoftWrap(true);

  (void)viewport.VisibleWrappedRowLayout(0);
  const std::size_t first_build_count = viewport.WrappedRowLayoutBuildCountForDebug();

  viewport.SetViewportSize(2, 12);
  Expect(viewport.WrappedRowLayoutBuildCountForDebug() == first_build_count,
         "a resize with the view at the top does not eagerly rebuild wrapped rows");

  (void)viewport.VisibleWrappedRowLayout(0);
  Expect(viewport.WrappedRowLayoutBuildCountForDebug() == first_build_count + 1,
         "the first wrapped-row query after that resize rebuilds the cache once");

  (void)viewport.VisibleWrappedRowLayout(0);
  Expect(viewport.WrappedRowLayoutBuildCountForDebug() == first_build_count + 1,
         "repeated wrapped-row queries without edits or resize should reuse the cached layout");

  viewport.SetViewportSize(4, 12);
  (void)viewport.VisibleWrappedRowLayout(0);
  Expect(viewport.WrappedRowLayoutBuildCountForDebug() == first_build_count + 1,
         "changing only the row count leaves the wrapped-row table alone");

  viewport.SetScrollLine(2);
  Expect(viewport.visual_scroll_line() == 2, "the fixture scrolls off the top row");
  const std::size_t scrolled_build_count = viewport.WrappedRowLayoutBuildCountForDebug();
  viewport.SetViewportSize(4, 8);
  Expect(viewport.WrappedRowLayoutBuildCountForDebug() == scrolled_build_count + 1,
         "a width change on a scrolled view rebuilds once, to resolve the new anchor");
  (void)viewport.VisibleWrappedRowLayout(0);
  Expect(viewport.WrappedRowLayoutBuildCountForDebug() == scrolled_build_count + 1,
         "and the first query after it reuses that rebuild");
}
#endif

void TestTextViewportSoftWrapPrefersWhitespaceBoundaries() {
  TextViewport viewport;
  viewport.LoadContent("hello brave new wonderful world here\n",
                       "/tmp/soft-wrap-words.txt");
  viewport.SetViewportSize(10, 12);
  viewport.SetSoftWrap(true);

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
  viewport.LoadContent("abcdefghijklmnopqrst\n",
                       "/tmp/soft-wrap-hard.txt");
  viewport.SetViewportSize(10, 8);
  viewport.SetSoftWrap(true);

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

void TestTextViewportSoftWrapRowsReuseTheVisibleLineCache() {
  TextViewport viewport;
  std::string content(20000, 'a');
  content += '\n';
  TextViewport wrapped;
  wrapped.LoadContent(content, "/tmp/soft-wrap-row-cache.txt");
  wrapped.SetViewportSize(40, 100);
  wrapped.SetSoftWrap(true);
  Expect(wrapped.visual_line_count() > 40, "fixture: the long line wraps past one screenful");

  for (std::size_t row = 0; row < 40; ++row) {
    (void)wrapped.VisibleWrappedRowLayoutRef(row);
  }

  wrapped.ResetCacheStats();
  util::ResetPerformanceCounters();
  for (std::size_t row = 0; row < 40; ++row) {
    (void)wrapped.VisibleWrappedRowLayoutRef(row);
  }
  const auto stats = wrapped.CacheStats();
  Expect(stats.visible_line_queries == 40, "every wrapped row asks the visible-line cache");
  Expect(stats.visible_line_hits == 40, "a repainted wrapped frame rebuilds no rows at all");
  Expect(util::ReadPerformanceCounter(util::PerfCounterId::EditorLineWidthMeasureBytes) == 0,
         "and no row re-measures the logical line it belongs to");

  util::ResetPerformanceCounters();
  for (std::size_t row = 40; row < 80 && row < wrapped.visual_line_count(); ++row) {
    (void)wrapped.VisibleWrappedRowLayoutRef(row);
  }
  Expect(util::ReadPerformanceCounter(util::PerfCounterId::EditorLineWidthMeasureBytes) == 0,
         "building a wrapped row uses the width the wrap table already computed");
}

void TestTextViewportSoftWrapVerticalMotionCrossesShortRows() {
  TextViewport viewport;
  viewport.LoadContent("aaaa bbbb cc dddddddddd\nX\n", "/tmp/soft-wrap-boundary.txt");
  viewport.SetViewportSize(10, 10);
  viewport.SetSoftWrap(true);
  Expect(viewport.WrappedVisualRowLayout(1).visual_start == 10 &&
             viewport.WrappedVisualRowLayout(1).visual_end == 13,
         "fixture: the middle row is the short one, spanning visual columns [10,13)");

  viewport.MoveCursorTo(0, 22);
  Expect(viewport.cursor_visual_row() == 2, "caret starts on the line's last wrapped row");

  viewport.MoveCursorVertical(-1);
  Expect(viewport.cursor_visual_row() == 1,
         "up from a wrapped row must land on the short row above, not bounce back down");
  Expect(viewport.cursor_column() == 13,
         "a preferred column past the short row's width clamps to that row's end");

  viewport.MoveCursorVertical(-1);
  Expect(viewport.cursor_visual_row() == 0 && viewport.cursor_column() == 9,
         "the preferred column survives the clamped row and reapplies on the row above");

  viewport.MoveCursorVertical(1);
  Expect(viewport.cursor_visual_row() == 1,
         "down from a wide row must stop on the short row under it, not skip past it");
  viewport.MoveCursorVertical(1);
  Expect(viewport.cursor_visual_row() == 2 && viewport.cursor_column() == 22,
         "down off the clamped short row restores the preferred column");
}

void TestTextViewportSoftWrapCaretRendersAtWrappedRowEnd() {
  TextViewport viewport;
  viewport.LoadContent("aaaa bbbb cc dddddddddd\n", "/tmp/soft-wrap-boundary-render.txt");
  viewport.SetViewportSize(10, 10);
  viewport.SetSoftWrap(true);
  viewport.MoveCursorTo(0, 22);
  viewport.MoveCursorVertical(-1);

  const auto short_row = viewport.VisibleWrappedRowLayout(1);
  Expect(short_row.caret_visible, "the caret renders on the row vertical motion landed on");
  Expect(short_row.caret_column == 3, "and at that row's trailing edge (its full width)");
  Expect(!viewport.VisibleWrappedRowLayout(2).caret_visible,
         "the row below must not also claim the boundary caret");

  viewport.MoveCursorHorizontal(-1);
  Expect(viewport.cursor_visual_row() == 1 && viewport.cursor_column() == 12,
         "left off the boundary steps back inside the short row");
  viewport.MoveCursorTo(0, 13);
  Expect(viewport.cursor_visual_row() == 2,
         "an ordinary placement at the wrap point resets to the next row");
}

void TestTextViewportSoftWrapClickPastRowEndStaysOnThatRow() {
  TextViewport viewport;
  viewport.LoadContent("aaaa bbbb cc dddddddddd\n", "/tmp/soft-wrap-boundary-click.txt");
  viewport.SetViewportSize(10, 10);
  viewport.SetSoftWrap(true);

  viewport.MoveCursorToVisualHit(1, 8);
  Expect(viewport.cursor_line() == 0 && viewport.cursor_column() == 13,
         "clicking past a wrapped row's end resolves to the wrap point");
  Expect(viewport.cursor_visual_row() == 1,
         "and the caret stays on the row that was clicked, not the one below");
}

void TestTextViewportSoftWrapHangingIndentPreservesScreenColumn() {
  TextViewport viewport;
  viewport.LoadContent("    aaaa bbbb cccc dddd eeee\n", "/tmp/soft-wrap-indent-column.txt");
  viewport.SetViewportSize(10, 12);
  viewport.SetSoftWrap(true);
  const auto row1 = viewport.WrappedVisualRowLayout(1);
  Expect(row1.indent == 4 && row1.visual_start == 9,
         "fixture: the first continuation row carries a 4-column hanging indent");

  viewport.MoveCursorTo(0, 6);
  viewport.MoveCursorVertical(1);
  Expect(viewport.cursor_visual_row() == 1,
         "down from the first row lands on the continuation row");
  Expect(viewport.cursor_column() == 11,
         "the caret keeps its screen column: 4 indent cells + 2 into the row's text");

  viewport.MoveCursorVertical(-1);
  Expect(viewport.cursor_visual_row() == 0 && viewport.cursor_column() == 6,
         "and the round trip back over the indent boundary returns to the same screen column");
}

void TestTextViewportSoftWrapWidthChangeReanchorsScroll() {
  std::string content;
  for (int i = 0; i < 10; ++i) {
    content += std::string(60, static_cast<char>('a' + i));
    content += '\n';
  }
  TextViewport viewport;
  viewport.LoadContent(content, "/tmp/soft-wrap-resize.txt");
  viewport.SetViewportSize(5, 20);
  viewport.SetSoftWrap(true);
  Expect(viewport.visual_line_count() == 31,
         "fixture: ten 60-column lines wrap to three rows each at width 20, plus the tail line");

  viewport.SetScrollLine(12);
  Expect(viewport.VisualRowLineIndex(viewport.visual_scroll_line()) == 4,
         "fixture: row 12 is the top of logical line 4");

  viewport.SetViewportSize(5, 60);
  Expect(viewport.visual_line_count() == 11, "widening the pane unwraps every line");
  Expect(viewport.VisualRowLineIndex(viewport.visual_scroll_line()) == 4,
         "the logical line at the top of the view survives a wrap-width change");
  Expect(viewport.visual_scroll_line() + 5 <= viewport.visual_line_count(),
         "and the re-anchored scroll stays inside the document");
}

void TestTextViewportSoftWrapToggleReanchorsScroll() {
  std::string content;
  for (int i = 0; i < 10; ++i) {
    content += std::string(60, static_cast<char>('a' + i));
    content += '\n';
  }
  TextViewport viewport;
  viewport.LoadContent(content, "/tmp/soft-wrap-toggle-anchor.txt");
  viewport.SetViewportSize(5, 20);
  viewport.SetSoftWrap(true);
  viewport.SetScrollLine(12);
  viewport.SetSoftWrap(false);
  Expect(viewport.VisualRowLineIndex(viewport.visual_scroll_line()) == 4,
         "turning soft wrap off keeps the top of the view on the same logical line");
  viewport.SetSoftWrap(true);
  Expect(viewport.VisualRowLineIndex(viewport.visual_scroll_line()) == 4,
         "and turning it back on re-derives the same anchor in wrapped-row space");
}

void TestTextViewportSoftWrapHangingIndentAlignsContinuationRows() {
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

void TestTextViewportSoftWrapEditGrowsWrapRowCount() {
  TextViewport viewport;
  viewport.LoadContent("abc\nXYZ\n", "/tmp/soft-wrap-edit-grow.txt");
  viewport.SetViewportSize(10, 8);
  viewport.SetSoftWrap(true);

  Expect(viewport.VisualRowCount() == 3, "pre-edit: three single-row lines");

  viewport.MoveCursorTo(0, 3);
  viewport.InsertText("defghijkl");

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
  viewport.InsertNewline();

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
  viewport.Backspace();

  Expect(viewport.VisualRowCount() == 3,
         "merging two logical lines must drop a visual row");
  Expect(viewport.WrappedVisualRowLayout(0).line_index == 0,
         "the merged line remains at index 0");
  Expect(viewport.WrappedVisualRowLayout(1).line_index == 1,
         "the line after the merge must map to the new index 1 (ZZ)");
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

  (void)viewport.VisualRowCount();
  const std::size_t builds = viewport.WrappedRowLayoutBuildCountForDebug();

  viewport.MoveCursorTo(0, 0);
  viewport.InsertText("x");
  (void)viewport.VisualRowCount();

  Expect(viewport.WrappedRowLayoutBuildCountForDebug() == builds,
         "a content edit under soft wrap must update the wrapped table in place, "
         "not trigger a full O(document) rebuild");
}

void TestTextViewportSoftWrapLargeBufferInlineEditIsInPlace() {
  TextViewport viewport;
  std::string content;
  content.reserve(4000 * 4);
  for (int i = 0; i < 4000; ++i) {
    content += "abc\n";
  }
  viewport.LoadContent(content, "/tmp/soft-wrap-large-inline.txt");
  viewport.SetViewportSize(20, 12);
  viewport.SetSoftWrap(true);
  const int rows_before = viewport.VisualRowCount();
  (void)viewport.max_visual_columns();

  const std::size_t builds = viewport.WrappedRowLayoutBuildCountForDebug();
  const std::size_t inplace_before = viewport.WrappedRowIncrementalInplaceCountForDebug();
  const std::size_t splice_before = viewport.WrappedRowIncrementalSpliceCountForDebug();
  const std::size_t vcol_before = viewport.VisualColumnIncrementalInplaceCountForDebug();

  viewport.MoveCursorTo(1, 2);
  viewport.InsertText("Z");
  (void)viewport.VisualRowCount();
  (void)viewport.max_visual_columns();

  Expect(viewport.WrappedRowLayoutBuildCountForDebug() == builds,
         "an in-line edit under soft wrap must not trigger a full O(document) rebuild");
  Expect(viewport.WrappedRowIncrementalInplaceCountForDebug() == inplace_before + 1,
         "an in-line edit preserving line/row counts must take the in-place fast path");
  Expect(viewport.WrappedRowIncrementalSpliceCountForDebug() == splice_before,
         "the in-place fast path must not fall through to the O(suffix) splice");
  Expect(viewport.VisualColumnIncrementalInplaceCountForDebug() == vcol_before + 1,
         "the visual-column cache must also update the edited line in place");

  Expect(viewport.WrappedVisualRowLayout(1).line_index == 1,
         "the edited line stays at visual row 1");
  Expect(viewport.WrappedVisualRowLayout(3999).line_index == 3999,
         "the far-suffix line keeps its untouched mapping");
  Expect(viewport.VisualRowCount() == rows_before,
         "an in-line edit must not change the visual row count");
}

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

  viewport.MoveCursorTo(0, 4);
  viewport.InsertText("VERYLONGTOKEN");
  viewport.MoveCursorTo(1, 3);
  viewport.Backspace();
  viewport.MoveCursorTo(2, 10);
  viewport.InsertNewline();
  viewport.MoveCursorTo(4, 0);
  viewport.Backspace();
  (void)viewport.VisualRowCount();

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
  viewport.LoadContent("abc\nde\nf", "/tmp/hmotion.cpp");

  viewport.MoveCursorTo(0, 3);
  viewport.MoveCursorHorizontal(1);
  Expect(viewport.cursor_line() == 1 && viewport.cursor_column() == 0,
         "Right at end-of-line should move to the start of the next line");

  viewport.MoveCursorHorizontal(-1);
  Expect(viewport.cursor_line() == 0 && viewport.cursor_column() == 3,
         "Left at column 0 should move to the end of the previous line");

  viewport.MoveCursorTo(0, 0);
  viewport.MoveCursorHorizontal(-1);
  Expect(viewport.cursor_line() == 0 && viewport.cursor_column() == 0,
         "Left at the document start should stay put");

  viewport.MoveCursorTo(2, 1);
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

void TestTextViewportTypingOnTheWidestLineDoesNotRescanTheWidthTable() {
  using microide::editor::TextLayout;
  TextViewport viewport;
  std::string content;
  for (int i = 0; i < 400; ++i) {
    content += "short line\n";
  }
  content += "this one is by a wide margin the widest line in the whole document\n";
  for (int i = 0; i < 400; ++i) {
    content += "short line\n";
  }
  viewport.LoadContent(content, "/tmp/widest-line-typing.txt");
  const std::size_t widest_line = 400;
  const std::size_t tab_size = viewport.tab_size();

  const auto fresh_widest = [&]() {
    std::size_t widest = 0;
    for (std::size_t i = 0; i < viewport.line_count(); ++i) {
      widest = std::max(
          widest, TextLayout::MeasureLineFacts(viewport.lines()[i], tab_size).visual_columns);
    }
    return widest;
  };

  (void)viewport.max_visual_columns();
  Expect(viewport.max_visual_columns() == fresh_widest(),
         "the warmed maximum should match a fresh measure");

  const auto before = util::CapturePerformanceCounters();
  viewport.MoveCursorTo(widest_line, viewport.lines()[widest_line].size());
  viewport.InsertText("X");
  const std::size_t after_insert = viewport.max_visual_columns();
  const auto after = util::CapturePerformanceCounters();

  Expect(after_insert == fresh_widest(),
         "the maximum after typing on the widest line must match a fresh measure");
  Expect(after[static_cast<std::size_t>(util::PerfCounterId::EditorLineWidthMaxScanLines)] ==
             before[static_cast<std::size_t>(util::PerfCounterId::EditorLineWidthMaxScanLines)],
         "typing on the widest line must not walk the width table at all");
  Expect(after[static_cast<std::size_t>(util::PerfCounterId::EditorLineWidthMaxKeptOnReplace)] >
             before[static_cast<std::size_t>(util::PerfCounterId::EditorLineWidthMaxKeptOnReplace)],
         "the widened-replacement path is what should have answered, and it should say so");

  viewport.MoveCursorTo(widest_line, 0);
  viewport.ReplaceRange({{widest_line, 0}, {widest_line, viewport.lines()[widest_line].size()}},
                        "tiny");
  Expect(viewport.max_visual_columns() == fresh_widest(),
         "shrinking the widest line must still yield the true maximum");
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
  viewport.InsertText("Z");
  viewport.InsertNewline();
  viewport.Backspace();
  viewport.InsertText("A\nB\n");
  viewport.ReplaceRange(SelectionRange{.start = TextPosition{kMidLine, 1},
                                       .end = TextPosition{kMidLine + 2, 0}},
                        "", true);

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
    TextViewport viewport;
    viewport.LoadContent("xax\nbxb", "/tmp/replace-multiline.txt");
    const std::size_t replaced = viewport.ReplaceAll("x", "zz");
    Expect(replaced == 3, "replace-all should count matches across lines");
    Expect(viewport.lines()[0] == "zzazz" && viewport.lines()[1] == "bzzb",
           "replace-all must rewrite every line correctly with unequal lengths");
  }
}

void TestTextViewportReplaceAllUnicodeCaseInsensitive() {
  TextViewport viewport;
  viewport.LoadContent("café CAFÉ Café x", "/tmp/replace-unicode.txt");
  const std::size_t replaced = viewport.ReplaceAll("café", "tea");
  Expect(replaced == 3, "replace-all matches every case variant of the accented needle");
  Expect(viewport.lines()[0] == "tea tea tea x",
         "each accented match is replaced and surrounding bytes are preserved");
}

void TestTextViewportReplaceAllMultiLineReplacementSplitsLines() {
  {
    TextViewport viewport;
    viewport.LoadContent("x", "/tmp/replace-newline.txt");
    const std::size_t replaced = viewport.ReplaceAll("x", "a\nb");
    Expect(replaced == 1, "single match with a multi-line replacement counts once");
    Expect(viewport.line_count() == 2, "a newline replacement must expand into two lines");
    Expect(viewport.lines()[0] == "a" && viewport.lines()[1] == "b",
           "multi-line replacement must produce real lines, not an embedded newline");
    Expect(std::string(viewport.lines()[0]).find('\n') == std::string::npos &&
               std::string(viewport.lines()[1]).find('\n') == std::string::npos,
           "no line should hide an embedded newline");
  }
  {
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

void TestTextViewportReplaceAllRangesMatchesScanningReplaceAll() {
  using microide::editor::SelectionRange;
  using microide::editor::TextPosition;

  auto make_range = [](std::size_t line, std::size_t start, std::size_t end) {
    return SelectionRange{TextPosition{line, start}, TextPosition{line, end}};
  };

  {
    const char* content = "xax\nbetween\nxxb";
    TextViewport range_view;
    range_view.LoadContent(content, "/tmp/replace-ranges-a.txt");
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

    Expect(range_view.Undo(), "undo after range replace-all succeeds");
    Expect(range_view.lines()[0] == "xax" && range_view.lines()[1] == "between" &&
               range_view.lines()[2] == "xxb",
           "undo restores the pre-replacement buffer exactly");
    Expect(range_view.Redo() && range_view.lines()[0] == "zzazz",
           "redo reapplies the range replace-all");
  }

  {
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

  microide::editor::SyntaxState opened = rs::AdvanceState(opener, path, {}, opener);
  Expect(opened.region_depth > 0, "an unterminated block comment opens a region");

  const microide::editor::SyntaxState after_blank = rs::AdvanceState("", path, opened, opener);
  Expect(after_blank.region_depth == opened.region_depth,
         "a blank line inside a block comment must preserve the open region");
  Expect(after_blank.definition_id == opened.definition_id,
         "the definition id is preserved across a blank line");

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

void TestRuntimeSyntaxDetectFiletypeReadsOnlyABoundedHead() {
  namespace rs = microide::editor::runtime_syntax;
  constexpr std::size_t kLineBytes = 512u * 1024u;
  microide::editor::TextBuffer buffer;
  buffer.ResetFromText("#!/bin/sh\n" + std::string(kLineBytes, 'x'));
  buffer.ReplaceTextRange(1, kLineBytes / 2, 1, kLineBytes / 2, "MID");

  util::ResetPerformanceCounters();
  Expect(rs::DetectFiletype("/tmp/no-extension", buffer) == "shell",
         "content detection still reads the shebang through the bounded head");
  const std::uint64_t materialized_bytes =
      util::ReadPerformanceCounter(util::PerfCounterId::EditorLineMaterializedBytes);
  Expect(materialized_bytes == 0,
         "signature detection must not materialize a line it only reads 4 KiB of (copied " +
             std::to_string(materialized_bytes) + " bytes)");

  (void)buffer.LineView(1);
  Expect(util::ReadPerformanceCounter(util::PerfCounterId::EditorLineMaterializedBytes) >=
             kLineBytes,
         "reading the spanning line whole still copies it (control)");
  util::ResetPerformanceCounters();
}

void TestRuntimeSyntaxDetectFiletypeKeepsCMakeLists() {
  const std::vector<std::string> lines = {
      "cmake_minimum_required(VERSION 3.25)",
      "project(microide)",
  };

  Expect(microide::editor::runtime_syntax::DetectFiletype("/tmp/CMakeLists.txt", lines) == "cmake",
         "CMakeLists.txt should still resolve to the CMake syntax definition");
}

void TestTextViewportSingleLineEditAllocatesABoundedMultipleOfTheLine() {
#if MICROIDE_PERF_HARNESS_BUILD
  namespace perf = microide::tests::perf;
  constexpr std::size_t kLineBytes = 4u * 1024u * 1024u;
  microide::editor::TextViewport viewport;
  viewport.LoadContent(std::string(kLineBytes, 'x') + "\n", "/tmp/minified-edit.txt");
  viewport.MoveCursorTo(0, 100);

  viewport.InsertText("a");

  const auto before = perf::Allocations::Snapshot();
  viewport.InsertText("b");
  const auto delta = perf::Allocations::DeltaSince(before);

  const std::uint64_t budget = 2u * kLineBytes;
  Expect(static_cast<std::uint64_t>(delta.bytes_allocated) < budget,
         "a one-character edit must cost its delta plus the one shared line "
         "materialization, not a multiple of the line");
#endif
}

void TestTextViewportInlineEditDerivesLineWidthFromTheSplice() {
  using microide::editor::TextLayout;
  microide::editor::TextViewport viewport;
  viewport.LoadContent("plain ascii line\nsecond\n", "/tmp/inline-width.txt");
  const std::size_t tab_size = viewport.tab_size();

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

void TestTextViewportWidestLineIsMaintainedAcrossEdits() {
  using microide::editor::TextLayout;
  using microide::util::PerfCounterId;
  microide::editor::TextViewport viewport;
  viewport.LoadContent("short\nthe widest line in this buffer\nmid\ntiny\n",
                       "/tmp/widest-line.txt");
  const std::size_t tab_size = viewport.tab_size();

  const auto fresh_widest = [&] {
    std::size_t widest = 0;
    for (std::size_t i = 0; i < viewport.line_count(); ++i) {
      widest = std::max(
          widest, TextLayout::MeasureLineFacts(viewport.lines()[i], tab_size).visual_columns);
    }
    return widest;
  };
  const auto scans_for = [&](const char* what, const std::function<void()>& edit) {
    edit();
    const std::uint64_t before =
        microide::util::ReadPerformanceCounter(PerfCounterId::EditorLineWidthMaxScans);
    const std::size_t reported = viewport.max_visual_columns();
    Expect(reported == fresh_widest(), what);
    return microide::util::ReadPerformanceCounter(PerfCounterId::EditorLineWidthMaxScans) - before;
  };

  (void)viewport.max_visual_columns();

  Expect(scans_for("widening a narrow line keeps the recorded maximum",
                   [&] {
                     viewport.MoveCursorTo(0, 5);
                     viewport.InsertText("er");
                   }) == 0,
         "an edit that cannot beat the maximum must not rescan the table");

  Expect(scans_for("a line growing past the maximum becomes the new maximum",
                   [&] {
                     viewport.MoveCursorTo(2, 3);
                     viewport.InsertText("this middle line is now the longest one here");
                   }) == 0,
         "an edit that raises the maximum must not rescan the table");

  Expect(scans_for("splitting a line above the maximum keeps the maximum",
                   [&] {
                     viewport.MoveCursorTo(0, 2);
                     viewport.InsertText("\n");
                   }) == 0,
         "a line-count change above the maximum must not rescan the table");

  Expect(scans_for("shrinking the widest line falls back to a rescan",
                   [&] {
                     viewport.MoveCursorTo(3, 3);
                     viewport.MoveCursorTo(3, viewport.lines().LineView(3).size(), true);
                     Expect(viewport.DeleteSelectedText(), "the widest line's tail should delete");
                   }) == 1,
         "rewriting the widest line has to rescan -- nothing else knows the runner-up");

  Expect(scans_for("deleting the widest line falls back to a rescan",
                   [&] {
                     std::size_t widest_index = 0;
                     std::size_t widest = 0;
                     for (std::size_t i = 0; i < viewport.line_count(); ++i) {
                       const std::size_t w =
                           TextLayout::MeasureLineFacts(viewport.lines()[i], tab_size)
                               .visual_columns;
                       if (w > widest) {
                         widest = w;
                         widest_index = i;
                       }
                     }
                     viewport.MoveCursorTo(widest_index, 0);
                     Expect(viewport.DeleteCurrentLine(), "the widest line should delete");
                   }) == 1,
         "deleting the widest line has to rescan");

  for (int i = 0; i < 6 && viewport.Undo(); ++i) {
    Expect(viewport.max_visual_columns() == fresh_widest(),
           "undoing back through the edits keeps the maximum exact");
  }
  while (viewport.Redo()) {
    Expect(viewport.max_visual_columns() == fresh_widest(),
           "redoing forward through the edits keeps the maximum exact");
  }
}

void TestTextViewportCaretColumnConversionsMatchTheDirectWalk() {
  using microide::editor::TextLayout;
  const std::vector<std::string> rows = {
      "plain ascii row",
      "tabbed\tcolumns\there",
      "caf\xC3\xA9 \xE4\xB8\xAD",
      "",
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
    microide::editor::TextViewport viewport;
    viewport.LoadContent(content, "/tmp/caret-columns.txt");
    check_every_column(viewport, "cold width table");
  }

  microide::editor::TextViewport viewport;
  viewport.LoadContent(content, "/tmp/caret-columns.txt");
  (void)viewport.max_visual_columns();
  check_every_column(viewport, "warm width table");

  viewport.MoveCursorTo(0, 5);
  viewport.InsertText("\t");
  check_every_column(viewport, "after a tab is typed into a plain row");
  viewport.InsertText("\xC3\xA9");
  check_every_column(viewport, "after a multibyte code point is typed");
  Expect(viewport.Undo(), "undo the multibyte insert");
  Expect(viewport.Undo(), "undo the tab insert");
  check_every_column(viewport, "after undoing back to the plain row");
}

void TestTextViewportTypingInALongLineCopiesNothing() {
  if (const char* verify = std::getenv("MICROIDE_VERIFY_LINE_WIDTH_TABLE");
      verify != nullptr && verify[0] != '\0' && verify[0] != '0') {
    std::fprintf(stderr,
                 "[long-line] SKIP: MICROIDE_VERIFY_LINE_WIDTH_TABLE reads every line by "
                 "design, so the copy accounting this test asserts on is not measurable\n");
    return;
  }
  constexpr std::size_t kLineBytes = 512u * 1024u;
  microide::editor::TextViewport viewport;
  viewport.LoadContent(std::string(kLineBytes, 'x') + "\n", "/tmp/long-line-copies.txt");
  viewport.SetViewportSize(40, 120);
  viewport.MoveCursorTo(0, kLineBytes / 2, false);
  (void)viewport.max_visual_columns();
  (void)viewport.CaretForLine(0);
  (void)viewport.HighlightedLineTokens(0);
  viewport.InsertText("y");
  (void)viewport.CaretForLine(0);

  util::ResetPerformanceCounters();
  for (int i = 0; i < 8; ++i) {
    viewport.InsertText("y");
    (void)viewport.CaretForLine(0);
    (void)viewport.cursor_visual_column();
    (void)viewport.HighlightedLineTokens(0);
    viewport.Backspace();
    (void)viewport.CaretForLine(0);
  }
  const std::uint64_t copied =
      util::ReadPerformanceCounter(util::PerfCounterId::EditorLineMaterializedBytes);
  Expect(copied == 0, "typing in a long line must copy none of it (copied " +
                          std::to_string(copied) + " bytes over 16 edits)");

  (void)viewport.lines().LineView(0);
  Expect(util::ReadPerformanceCounter(util::PerfCounterId::EditorLineMaterializedBytes) >=
             kLineBytes,
         "asking for the whole line still copies it (control)");
  util::ResetPerformanceCounters();
}

void TestTextViewportTypingInALongLineDoesNotWalkItForCaretColumns() {
  constexpr std::size_t kLineBytes = 256u * 1024u;
  microide::editor::TextViewport viewport;
  viewport.LoadContent(std::string(kLineBytes, 'x') + "\n", "/tmp/long-line-caret.txt");
  viewport.SetViewportSize(40, 120);
  viewport.MoveCursorTo(0, kLineBytes / 2, false);
  (void)viewport.max_visual_columns();
  (void)viewport.CaretForLine(0);

  const std::uint64_t before =
      util::ReadPerformanceCounter(util::PerfCounterId::EditorVisualColumnWalkBytes);
  for (int i = 0; i < 8; ++i) {
    viewport.InsertText("y");
    (void)viewport.CaretForLine(0);
    (void)viewport.cursor_visual_column();
  }
  const std::uint64_t walked =
      util::ReadPerformanceCounter(util::PerfCounterId::EditorVisualColumnWalkBytes) - before;
  Expect(walked == 0,
         "typing in a long plain-ASCII line must not re-walk it for caret columns (walked " +
             std::to_string(walked) + " bytes)");

  viewport.InsertText("\t");
  const std::uint64_t before_tabbed =
      util::ReadPerformanceCounter(util::PerfCounterId::EditorVisualColumnWalkBytes);
  (void)viewport.cursor_visual_column();
  Expect(util::ReadPerformanceCounter(util::PerfCounterId::EditorVisualColumnWalkBytes) >
             before_tabbed,
         "a line that is no longer plain ASCII must fall back to the walk");
}

void TestTextViewportSoftWrapIncrementalRowsMatchAFullRewrap() {
  std::string line;
  while (line.size() < 4000) {
    line += "alpha beta gamma delta epsilon zeta eta theta ";
  }
  const std::string content = line + "\n";

  for (const std::size_t column : {std::size_t{0}, std::size_t{5}, std::size_t{79},
                                   std::size_t{80}, std::size_t{81}, line.size() / 2,
                                   line.size() - 3, line.size()}) {
    for (const char* inserted : {"x", " ", "longword"}) {
      microide::editor::TextViewport incremental;
      incremental.LoadContent(content, "/tmp/wrap-incremental.txt");
      incremental.SetViewportSize(20, 80);
      incremental.SetSoftWrap(true);
      (void)incremental.visual_line_count();
      incremental.MoveCursorTo(0, column, false);
      incremental.InsertText(inserted);

      microide::editor::TextViewport oracle;
      oracle.LoadContent(std::string(incremental.lines().LineView(0)) + "\n",
                         "/tmp/wrap-oracle.txt");
      oracle.SetViewportSize(20, 80);
      oracle.SetSoftWrap(true);

      const std::string where =
          " (column " + std::to_string(column) + ", inserted \"" + inserted + "\")";
      Expect(incremental.visual_line_count() == oracle.visual_line_count(),
             "an incrementally spliced wrap must produce the same row count as a full "
             "re-wrap" + where + ": " + std::to_string(incremental.visual_line_count()) +
                 " vs " + std::to_string(oracle.visual_line_count()));
      for (std::size_t row = 0; row < oracle.visual_line_count(); ++row) {
        const auto spliced = incremental.WrappedVisualRowLayout(row);
        const auto fresh = oracle.WrappedVisualRowLayout(row);
        Expect(spliced.line_index == fresh.line_index &&
                   spliced.visual_start == fresh.visual_start &&
                   spliced.visual_end == fresh.visual_end && spliced.indent == fresh.indent,
               "spliced row " + std::to_string(row) + " must match the full re-wrap" + where +
                   ": [" + std::to_string(spliced.visual_start) + "," +
                   std::to_string(spliced.visual_end) + ") vs [" +
                   std::to_string(fresh.visual_start) + "," + std::to_string(fresh.visual_end) +
                   ")");
      }
    }
  }
}

void TestTextViewportSoftWrappedLongLineDoesNotWalkItForCaretColumns() {
  constexpr std::size_t kLineBytes = 256u * 1024u;
  microide::editor::TextViewport viewport;
  viewport.LoadContent(std::string(kLineBytes, 'x') + "\n", "/tmp/long-line-wrapped.txt");
  viewport.SetViewportSize(40, 120);
  viewport.SetSoftWrap(true);
  viewport.MoveCursorTo(0, kLineBytes / 2, false);

  util::ResetPerformanceCounters();
  for (int i = 0; i < 8; ++i) {
    viewport.InsertText("y");
    (void)viewport.CaretForLine(0);
    (void)viewport.cursor_visual_column();
  }
  const std::uint64_t walked =
      util::ReadPerformanceCounter(util::PerfCounterId::EditorVisualColumnWalkBytes);
  Expect(walked == 0,
         "a soft-wrapped long plain-ASCII line must not be walked for caret columns (walked " +
             std::to_string(walked) + " bytes)");
  Expect(util::ReadPerformanceCounter(util::PerfCounterId::EditorVisualColumnWalkFactsUnknown) == 0,
         "no conversion should have fallen through to a walk at all");

  viewport.MoveCursorTo(0, 16, false);
  viewport.InsertText("\t");
  viewport.MoveCursorTo(0, kLineBytes / 2, false);
  util::ResetPerformanceCounters();
  (void)viewport.cursor_visual_column();
  Expect(util::ReadPerformanceCounter(util::PerfCounterId::EditorVisualColumnWalkBytes) > 0,
         "a line whose prefix is no longer plain ASCII must fall back to the walk (control)");
  util::ResetPerformanceCounters();
}

void TestTextViewportLanguageIdMemoIsAllocationFreeOnASettledBuffer() {
  namespace perf = microide::tests::perf;
  microide::editor::TextViewport viewport;
  viewport.LoadContent("int main() { return 0; }\n", "/tmp/memo-settled.cpp");

  Expect(viewport.language_id() == "c++", "cold resolve should detect the C++ definition");

  const auto before = perf::Allocations::Snapshot();
  for (int i = 0; i < 64; ++i) {
    Expect(viewport.language_id() == "c++", "warm resolve should return the memoized filetype");
  }
  const auto delta = perf::Allocations::DeltaSince(before);
  Expect(delta.allocations == 0, "a memo hit must not allocate");
}

void TestTextViewportLanguageIdMemoInvalidatesOnContentChange() {
  using microide::editor::runtime_syntax::DetectFiletype;
  const std::filesystem::path path = "/tmp/memo-shebang";
  const std::vector<std::string> shell_lines = {"#!/bin/sh", "echo hi"};
  const std::vector<std::string> python_lines = {"#!/usr/bin/env python3", "print('hi')"};
  const std::string shell_id = DetectFiletype(path, shell_lines);
  const std::string python_id = DetectFiletype(path, python_lines);
  Expect(!shell_id.empty() && shell_id != python_id,
         "the two shebangs must detect as different languages for this test to mean anything");

  microide::editor::TextViewport viewport;
  viewport.LoadContent("#!/bin/sh\necho hi\n", path);
  Expect(viewport.language_id() == shell_id, "shebang should drive the initial detection");

  viewport.LoadContent("#!/usr/bin/env python3\nprint('hi')\n", path);
  Expect(viewport.language_id() == python_id,
         "a content change must re-detect; a path-keyed cache would pin the first answer");
}

void TestTextViewportLanguageIdMemoSurvivesEditsWhenTheExtensionDecides() {
  namespace perf = microide::tests::perf;
  microide::editor::TextViewport viewport;
  viewport.LoadContent("int main() { return 0; }\n", "/tmp/memo-typing.cpp");
  Expect(viewport.language_id() == "c++", "cold resolve should detect the C++ definition");

  const std::uint64_t scans_before =
      util::ReadPerformanceCounter(util::PerfCounterId::EditorFiletypeSignatureScans);
  std::size_t query_allocations = 0;
  for (int i = 0; i < 64; ++i) {
    viewport.MoveCursorTo(0, 0);
    viewport.InsertText("x");
    const auto before = perf::Allocations::Snapshot();
    Expect(viewport.language_id() == "c++", "an edit cannot change a filetype the path decided");
    query_allocations += perf::Allocations::DeltaSince(before).allocations;
  }
  Expect(util::ReadPerformanceCounter(util::PerfCounterId::EditorFiletypeSignatureScans) ==
             scans_before,
         "typing must not run a filetype signature scan on an unambiguous extension");
  Expect(query_allocations == 0,
         "a language query after a keystroke must be a memo hit, not a re-detection (" +
             std::to_string(query_allocations) + " allocations)");
}

void TestTextViewportLanguageIdMemoRedetectsOnlyForHeadEdits() {
  namespace perf = microide::tests::perf;
  using microide::editor::runtime_syntax::DetectFiletype;
  using microide::editor::runtime_syntax::kFiletypeDetectHeadLines;
  const std::filesystem::path path = "/tmp/memo-head-scope";
  std::string content = "#!/bin/sh\n";
  for (std::size_t i = 0; i < kFiletypeDetectHeadLines * 4; ++i) {
    content += "echo line\n";
  }
  const std::vector<std::string> shell_lines = {"#!/bin/sh", "echo hi"};
  const std::vector<std::string> python_lines = {"#!/usr/bin/env python3", "print('hi')"};
  const std::string shell_id = DetectFiletype(path, shell_lines);
  const std::string python_id = DetectFiletype(path, python_lines);
  Expect(!shell_id.empty() && shell_id != python_id,
         "the two shebangs must detect as different languages for this test to mean anything");

  microide::editor::TextViewport viewport;
  viewport.LoadContent(content, path);
  Expect(viewport.language_id() == shell_id, "shebang should drive the initial detection");

  std::size_t query_allocations = 0;
  for (int i = 0; i < 32; ++i) {
    viewport.MoveCursorTo(kFiletypeDetectHeadLines * 2, 0);
    viewport.InsertText("x");
    const auto before = perf::Allocations::Snapshot();
    Expect(viewport.language_id() == shell_id, "a deep edit must not change the detected language");
    query_allocations += perf::Allocations::DeltaSince(before).allocations;
  }
  Expect(query_allocations == 0,
         "an edit below the detector's head window must not re-detect (" +
             std::to_string(query_allocations) + " allocations)");

  viewport.MoveCursorTo(0, 0);
  viewport.SelectLineAtCursor();
  viewport.InsertText("#!/usr/bin/env python3\n");
  Expect(viewport.language_id() == python_id,
         "rewriting the shebang must re-detect; the head tier is what makes that possible");
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

  (void)SyntaxHighlighter::InitialState(path, small);

  const auto before_small = perf::Allocations::Snapshot();
  (void)SyntaxHighlighter::InitialState(path, small);
  const auto small_delta = perf::Allocations::DeltaSince(before_small);

  const auto before_large = perf::Allocations::Snapshot();
  (void)SyntaxHighlighter::InitialState(path, large);
  const auto large_delta = perf::Allocations::DeltaSince(before_large);

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

void TestRuntimeSyntaxMatchBudgetBoundsPathologicalRule() {
#if !MICROIDE_HAS_LUA_PLUGINS
  return;
#endif
  ScopedRuntimeSyntaxRegistryReset syntax_reset;
  TemporaryDirectory temp_dir;
  const std::filesystem::path syntax_dir = temp_dir.path() / "syntax";
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

  std::string line(20000, 'a');
  TextViewport viewport;
  viewport.LoadContent(line + "\n", "/tmp/dense.dense");
  const auto& tokens = viewport.HighlightedLineTokens(0);
  Expect(tokens.size() == line.size(), "one token per byte is preserved on a dense-match line");
  Expect(tokens.front() == SyntaxTokenKind::Keyword,
         "the first matches within the budget are highlighted");
  Expect(tokens.back() == SyntaxTokenKind::Plain,
         "bytes past the per-line match budget fall back to Plain instead of matching unbounded");
}

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

  TextViewport viewport;
  viewport.LoadContent("foo(x)foo\n", "/tmp/sample.caret");
  const auto& tokens = viewport.HighlightedLineTokens(0);
  Expect(tokens.size() == 9, "one token per byte");
  Expect(tokens[0] == SyntaxTokenKind::Keyword && tokens[1] == SyntaxTokenKind::Keyword &&
             tokens[2] == SyntaxTokenKind::Keyword,
         "^foo matches the leading foo at the true line start");
  Expect(tokens[6] != SyntaxTokenKind::Keyword && tokens[7] != SyntaxTokenKind::Keyword &&
             tokens[8] != SyntaxTokenKind::Keyword,
         "^foo must NOT match the foo after a mid-line region (PCRE2_NOTBOL)");
}

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

  {
    TextViewport shortv;
    shortv.LoadContent("TODO here\n", "/tmp/short.todo");
    const auto& tokens = shortv.HighlightedLineTokens(0);
    Expect(std::any_of(tokens.begin(), tokens.end(),
                       [](SyntaxTokenKind k) { return k == SyntaxTokenKind::Keyword; }),
           "a short line with the keyword should highlight");
  }

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

void TestRuntimeSyntaxLazyCompileIsThreadSafeForSameLanguage() {
  using microide::editor::SyntaxHighlighter;
  ScopedRuntimeSyntaxRegistryReset syntax_reset;
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

void TestRuntimeSyntaxBadPluginRegexReportsErrorEagerly() {
  ScopedRuntimeSyntaxRegistryReset syntax_reset;

  microide::editor::runtime_syntax::RuntimeSyntaxDefinitionData plugin;
  plugin.filetype = "badfixture";
  plugin.filename_patterns = {"\\.badfixture$"};
  microide::editor::runtime_syntax::RuntimeSyntaxRuleData rule;
  rule.kind = microide::editor::runtime_syntax::GeneratedRuleKind::Pattern;
  rule.group_name = "keyword";
  rule.pattern = "([unterminated";
  plugin.rules.push_back(rule);

  std::vector<std::string> reload_errors;
  microide::editor::runtime_syntax::ReloadDefinitions({plugin}, &reload_errors);
  Expect(!reload_errors.empty(),
         "an invalid plugin regex should be reported at reload time (runtime defs stay eager)");
}

void TestRuntimeSyntaxLazyCompileColorsBuiltInBlockComment() {
  using microide::editor::SyntaxHighlighter;
  using microide::editor::SyntaxState;
  ScopedRuntimeSyntaxRegistryReset syntax_reset;
  microide::editor::runtime_syntax::ReloadDefinitions({});

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

  auto stale = microide::editor::ComputeHighlightPrefetch(
      viewport.BuildHighlightPrefetchRequest(10, 4));
  stale.content_revision = viewport.content_revision() + 1;
  Expect(viewport.HasHighlightPrefetchGap(10, 4), "the line-10 region should start uncached");
  viewport.InstallPrefetchedHighlights(stale);
  Expect(viewport.HasHighlightPrefetchGap(10, 4),
         "a stale-revision prefetch result should be dropped, leaving the gap intact");
}

void TestAHighlightPrefetchWindowFitsTheCacheAndIsNotAskedTwice() {
  const std::size_t capacity = TextViewport::HighlightCacheCapacity();
  std::string content;
  for (std::size_t i = 0; i < capacity * 2; ++i) {
    content += "int value" + std::to_string(i) + " = " + std::to_string(i) + ";\n";
  }
  TextViewport viewport;
  viewport.LoadContent(content, "/tmp/prefetch-tall.cpp");

  const std::size_t too_many = capacity + capacity / 2;
  viewport.InstallPrefetchedHighlights(
      microide::editor::ComputeHighlightPrefetch(viewport.BuildHighlightPrefetchRequest(0, too_many)));
  Expect(viewport.HasHighlightPrefetchGap(0, too_many),
         "a window larger than the cache keeps a gap after it is installed (the loop's cause)");
  TextViewport fits;
  fits.LoadContent(content, "/tmp/prefetch-fits.cpp");
  fits.InstallPrefetchedHighlights(
      microide::editor::ComputeHighlightPrefetch(fits.BuildHighlightPrefetchRequest(0, capacity)));
  Expect(!fits.HasHighlightPrefetchGap(0, capacity), "a window the cache can hold closes once installed");

  microide::editor::HighlightPrefetchService service;
  using Key = microide::editor::HighlightPrefetchService::WindowKey;
  const Key first{.viewport = &viewport, .content_revision = 1, .syntax_revision = 1, .start_line = 0, .count = 50};
  Key scrolled = first;
  scrolled.start_line = 40;
  Expect(service.Admit(first), "a first window is admitted");
  Expect(!service.Admit(first), "the same window again is not: its answer is already on its way");
  Expect(service.Admit(scrolled), "a scrolled window is admitted");
  Expect(service.Admit(first), "and the first again after it");
  Key edited = first;
  edited.content_revision = 2;
  Expect(service.Admit(edited), "an edit makes it a new window");
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
  using microide::editor::TextLayout;
  const std::string line = "ab\tcd\tef";
  const std::size_t tab_size = 4;

  {
    const auto layout = TextLayout::BuildVisibleLine(line, 0,
                                                      80, tab_size);
    for (std::size_t c = 0; c <= line.size(); ++c) {
      const std::size_t walked = TextLayout::VisualColumnForTextColumn(line, c, tab_size);
      const std::size_t clipped = TextLayout::VisualColumnFromLayoutClipped(
          layout, 0, 80, c);
      Expect(clipped == walked,
             "VisualColumnFromLayoutClipped must match VisualColumnForTextColumn inside the row");
    }
  }

  {
    const std::size_t horizontal_scroll = 4;
    const std::size_t visible_columns = 8;
    const auto layout = TextLayout::BuildVisibleLine(line, horizontal_scroll, visible_columns,
                                                      tab_size);
    const std::size_t row_start = horizontal_scroll;
    const std::size_t row_end = horizontal_scroll + visible_columns;

    const std::size_t before = TextLayout::VisualColumnFromLayoutClipped(layout, row_start,
                                                                          row_end, 0);
    Expect(before <= row_start,
           "source columns before the row window must return <= row_start (clip sentinel)");
    Expect(std::max<std::size_t>(before, row_start) == row_start,
           "clipping via std::max with row_start must produce row_start");

    const std::size_t at_end = TextLayout::VisualColumnFromLayoutClipped(
        layout, row_start, row_end, line.size());
    const std::size_t past_end = TextLayout::VisualColumnFromLayoutClipped(
        layout, row_start, row_end, line.size() + 5);
    Expect(at_end == past_end,
           "source columns at/past line end must return the same end-of-line visual column");
    const std::size_t walked_end =
        TextLayout::VisualColumnForTextColumn(line, line.size(), tab_size);
    Expect(at_end == walked_end || at_end >= row_end,
           "end-of-line helper result must agree with the legacy walk or clip beyond row_end");
  }
}

void TestTextLayoutLineVisualColumnMapMatchesWalk() {
  using microide::editor::TextLayout;
  const std::string ascii_only = "hello world";
  const std::string with_tabs = "ab\tcd\tef";
  const std::string with_utf8 = "α\tβγ\tδε";
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

  viewport.AddSecondaryCaret(0, 1);
  const auto third = viewport.secondary_caret_positions();
  Expect(third.size() == 3,
         "secondary_caret_positions reflects the new caret after AddSecondaryCaret");
}

void TestTextViewportTrivialWrappedLayoutFastPath() {
  std::string content;
  for (int i = 0; i < 4000; ++i) {
    content += "line " + std::to_string(i) + "\n";
  }
  TextViewport viewport;
  viewport.LoadContent(content, "/tmp/trivial-layout-large.txt");
  viewport.SetViewportSize(24, 120);
  viewport.SetSoftWrap(false);
  viewport.SetScrollLine(100);

  Expect(viewport.visual_line_count() == viewport.lines().size(),
         "trivial layout: visual row count must equal document line count");

  Expect(viewport.VisualRowLineIndex(2500) == 2500,
         "trivial layout: VisualRowLineIndex must be identity");
  Expect(viewport.VisualRowForLine(1234) == 1234,
         "trivial layout: VisualRowForLine must be identity");

  const auto row = viewport.WrappedVisualRowLayout(500);
  Expect(row.line_index == 500,
         "trivial layout: wrapped visual row layout returns synthesized line index");
  Expect(row.visual_start == viewport.horizontal_scroll(),
         "trivial layout: visual_start equals horizontal_scroll");
  Expect(row.visual_end == viewport.horizontal_scroll() + 120,
         "trivial layout: visual_end equals horizontal_scroll + visible_columns");

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
  viewport.SetTabSize(4);
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
  (void)viewport.WrappedVisualRowLayout(0);
  const std::size_t before_builds = viewport.WrappedRowLayoutBuildCountForDebug();
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
  (void)viewport.HighlightedLineTokens(0);
  (void)viewport.HighlightedLineTokens(0);
  const auto stats_warm = viewport.CacheStats();
  viewport.InvalidateSyntaxHighlighting();
  (void)viewport.HighlightedLineTokens(0);
  const auto stats_after = viewport.CacheStats();
  Expect(stats_after.highlight_queries > stats_warm.highlight_queries,
         "post-syntax-bump read should issue another highlight query");
  Expect(stats_after.highlight_hits == stats_warm.highlight_hits,
         "SyntaxConfig must wipe the highlight cache so the next read misses");
}

void TestTextViewportContentEditInvalidatesBracketAndHighlightCaches() {
  TextViewport viewport;
  viewport.LoadContent("int v = 1;\n", "/tmp/tier-content-edit.cpp");
  (void)viewport.HighlightedLineTokens(0);
  const auto before_bumps = CaptureTierBumps();
  viewport.InsertCharacter('z');
  const auto delta = Delta(before_bumps, CaptureTierBumps());
  Expect(delta.content == 1,
         "ContentEdit bumps content_revision so downstream caches re-key correctly");
}

std::filesystem::path WriteScratchFile(const std::string& name, std::string_view bytes) {
  const std::filesystem::path path =
      std::filesystem::temp_directory_path() / ("microide-phase4-" + name);
  Expect(microide::util::WriteTextFileAtomically(path, bytes),
         "scratch fixture should write");
  return path;
}

void TestTextViewportFastLoadMatchesCrlfDecode() {
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

void TestTextViewportABufferThatBecameBytesSavesItsMarkAndEndings() {
  const std::filesystem::path path =
      WriteScratchFile("bom-crlf.txt", "\xEF\xBB\xBFwith a\r\nmark\r\n");
  TextViewport view;
  Expect(view.OpenFile(path), "the BOM file opens");
  view.InsertText("\x80\x81");
  Expect(view.EncodingLabel() == "Bytes", "the paste makes the buffer bytes");
  const std::string expected = "\xEF\xBB\xBF\x80\x81with a\r\nmark\r\n";
  Expect(view.SerializeDocumentText() == expected, "the buffer's disk form keeps the mark and CRLF");
  Expect(view.Save(microide::project::LocalFileWriteGate()), "it saves");
  Expect(microide::util::ReadTextFile(path).value_or("") == expected,
         "and the file gets what the buffer holds: the mark, the bytes and CRLF");
}

void TestASaveWrittenWhileTheBufferMovedOnLeavesItDirtyAndUndoFindsTheSavedText() {
  const std::filesystem::path path = WriteScratchFile("async-save.txt", "one\n");
  TextViewport view;
  Expect(view.OpenFile(path), "the file opens");
  view.MoveCursorTo(0, 3);
  view.InsertText("A");
  std::optional<TextViewport::PreparedSave> prepared = view.PrepareSave();
  Expect(prepared.has_value(), "a dirty file buffer prepares a save");
  view.InsertText("B");
  TextViewport::WrittenSave written =
      TextViewport::WritePreparedSave(*prepared, microide::project::LocalFileWriteGate());
  Expect(view.ApplyWrittenSave(*prepared, std::move(written)), "the write lands");
  Expect(microide::util::ReadTextFile(path).value_or("") == "oneA\n", "the file holds the text as it was prepared");
  Expect(view.dirty(), "the typing that came after is not saved");
  Expect(view.HasDiskSignature(), "the buffer knows what it wrote");
  Expect(view.Undo(), "undo the later typing");
  Expect(view.SerializeDocumentText() == "oneA\n" && !view.dirty(),
         "and the buffer is back at the saved text, clean");
}

void TestASaveNormalizedWhileTheBufferMovedOnHasNoCleanPoint() {
  const std::filesystem::path path = WriteScratchFile("async-normalize.txt", "one\n");
  TextViewport view;
  Expect(view.OpenFile(path), "the file opens");
  view.SetSaveTrimTrailingWhitespace(true);
  view.MoveCursorTo(0, 3);
  view.InsertText("  ");
  std::optional<TextViewport::PreparedSave> prepared = view.PrepareSave();
  view.InsertText("B");
  Expect(view.ApplyWrittenSave(*prepared, TextViewport::WritePreparedSave(*prepared, microide::project::LocalFileWriteGate())),
         "the write lands");
  Expect(microide::util::ReadTextFile(path).value_or("") == "one\n", "the file holds the trimmed text");
  Expect(view.dirty(), "still unsaved");
  Expect(view.Undo() && view.dirty(), "the untrimmed text the buffer goes back to is not what the file holds");
}

void TestASaveForADocumentThatWasReplacedDoesNotApply() {
  const std::filesystem::path path = WriteScratchFile("async-replaced.txt", "one\n");
  TextViewport view;
  Expect(view.OpenFile(path), "the file opens");
  view.InsertText("A");
  std::optional<TextViewport::PreparedSave> prepared = view.PrepareSave();
  Expect(view.OpenFile(WriteScratchFile("async-other.txt", "two\n")), "another file takes the viewport");
  Expect(!view.ApplyWrittenSave(*prepared, TextViewport::WritePreparedSave(*prepared, microide::project::LocalFileWriteGate())),
         "the finished write is not the new document's");
  Expect(view.SerializeDocumentText() == "two\n" && !view.dirty(), "which is untouched");
}

void TestTextViewportUtf8BomIsStrippedOnOpenAndWrittenOnSave() {
  const std::filesystem::path path =
      WriteScratchFile("bom.txt", "\xEF\xBB\xBF#!/bin/sh\necho hi\n");
  TextViewport view;
  Expect(view.OpenFile(path), "BOM file opens");
  Expect(view.has_utf8_bom(), "the BOM is remembered");
  Expect(view.lines().LineView(0) == "#!/bin/sh",
         "line 0 starts at the text, not at the mark");
  Expect(view.EncodingLabel() == "UTF-8 with BOM", "the status label names the BOM");
  Expect(view.SerializeDocumentText() == "\xEF\xBB\xBF#!/bin/sh\necho hi\n",
         "the disk form reproduces the mark");

  view.InsertText("// top\n");
  Expect(view.Save(microide::project::LocalFileWriteGate()), "BOM file saves");
  Expect(microide::util::ReadTextFile(path).value_or("") ==
             "\xEF\xBB\xBF// top\n#!/bin/sh\necho hi\n",
         "the mark is written back ahead of text typed at the start of the file");
  Expect(view.has_utf8_bom(), "saving keeps the BOM flag");

  TextViewport plain;
  Expect(plain.OpenFile(WriteScratchFile("nobom.txt", "plain\n")), "plain file opens");
  Expect(!plain.has_utf8_bom() && plain.EncodingLabel() == "UTF-8",
         "a file without a mark reports plain UTF-8");
  Expect(plain.Save(microide::project::LocalFileWriteGate()) && microide::util::ReadTextFile(plain.path()).value_or("") == "plain\n",
         "saving a plain file adds no mark");

  TextViewport loaded;
  loaded.LoadContent("\xEF\xBB\xBF" "alpha\r\nbeta\r\n", path);
  Expect(loaded.has_utf8_bom() && loaded.lines().LineView(0) == "alpha",
         "LoadContent strips the mark");
  Expect(loaded.SerializeDocumentText() == "\xEF\xBB\xBF" "alpha\r\nbeta\r\n",
         "the CRLF disk form round-trips through LoadContent");

  TextViewport only;
  const std::filesystem::path only_path = WriteScratchFile("onlybom.txt", "\xEF\xBB\xBF");
  Expect(only.OpenFile(only_path) && only.line_count() == 1 &&
             only.lines().LineView(0).empty() && only.has_utf8_bom(),
         "a BOM-only file is one empty line with the mark remembered");
  Expect(only.Save(microide::project::LocalFileWriteGate()) && microide::util::ReadTextFile(only_path).value_or("") == "\xEF\xBB\xBF",
         "a BOM-only file saves back as the mark");
}

void TestTextViewportFastLoadDetectsEncoding() {
  TextViewport utf8;
  Expect(utf8.OpenFile(WriteScratchFile("utf8.txt", "café\nrésumé\n")),
         "utf-8 file opens via fast path");
  Expect(utf8.EncodingLabel() == "UTF-8",
         "fast path classifies non-ASCII UTF-8 as UTF-8");

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

void TestTextViewportFirstLineEditRehighlightsFollowingLines() {
  TextViewport viewport;
  viewport.LoadContent("/* comment\nstill inside\nalso inside\n", "/tmp/first-line-syntax.cpp");
  viewport.SetViewportSize(10, 40);

  const auto line_is_comment = [&viewport](std::size_t line) {
    const auto tokens = viewport.HighlightedLineTokens(line);
    return !tokens.empty() && tokens.front() == SyntaxTokenKind::Comment;
  };
  Expect(line_is_comment(1) && line_is_comment(2),
         "lines under a block comment opened on line 0 should start out commented");

  viewport.MoveCursorTo(0, viewport.lines()[0].size());
  viewport.InsertText(" */");
  Expect(!line_is_comment(1) && !line_is_comment(2),
         "closing the block comment on line 0 must re-highlight every line below it — the "
         "state chain from line 0 is exactly what the line-0 invalidation has to drop");

  viewport.MoveCursorTo(0, viewport.lines()[0].size());
  for (int i = 0; i < 3; ++i) {
    viewport.Backspace();
  }
  Expect(line_is_comment(1) && line_is_comment(2),
         "reopening the block comment on line 0 must re-comment the lines below it");
}

void TestTextViewportFirstLineEditKeepsVisualColumnCacheIncremental() {
  TextViewport viewport;
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

  for (int i = 0; i < 10; ++i) {
    viewport.Backspace();
  }
  Expect(clamped_horizontal_scroll() == before,
         "narrowing the widest line back must restore the horizontal scroll range");
}

void TestTextViewportLoadLinesMatchesSerializeRoundTrip() {
  const std::vector<std::vector<std::string>> cases = {
      {"solo"},
      {"a", "b", "c"},
      {"a", "b", "c", ""},
      {"", "", ""},
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

void TestAppliedEditRecordAnswersForOneStepOnly() {
  TextViewport viewport;
  viewport.LoadContent("one\ntwo\n", "/tmp/applied-edit-record.txt");
  const std::uint64_t loaded = viewport.content_revision();
  Expect(viewport.LatestAppliedEdit() == nullptr, "a loaded buffer has no edit to slide through");

  Expect(viewport.ReplaceRange({{0, 0}, {0, 0}}, "a"), "one edit");
  const std::uint64_t after_first = viewport.content_revision();
  Expect(viewport.AppliedEditFromRevision(loaded) != nullptr, "it is the whole step from the load");
  Expect(viewport.LatestAppliedEdit() != nullptr, "and the latest change");

  Expect(viewport.ReplaceRange({{1, 0}, {1, 0}}, "b"), "a second edit");
  Expect(viewport.AppliedEditFromRevision(loaded) == nullptr,
         "from the load, the second edit alone is not the step (the first is missing)");
  Expect(viewport.AppliedEditFromRevision(after_first) != nullptr, "from after the first, it is");

  viewport.LoadContent("reloaded\n", "/tmp/applied-edit-record.txt");
  Expect(viewport.LatestAppliedEdit() == nullptr && viewport.AppliedEditFromRevision(after_first) == nullptr,
         "a reload is a change the record does not describe: nothing slides through it");

  Expect(viewport.ReplaceRange({{0, 0}, {0, 0}}, "c"), "an edit after the reload");
  Expect(viewport.Undo(), "undone");
  Expect(viewport.LatestAppliedEdit() != nullptr, "an undo is a step of its own, recorded");
}

void TestTextViewportAGroupedJoinOverAnEditedLineUndoesAndRedoesExactly() {
  TextViewport viewport;
  viewport.LoadContent("abcdefgh\nwxyz\n", "/tmp/grouped-join.cpp");
  viewport.BeginUndoGroup();
  Expect(viewport.ReplaceRange(SelectionRange{TextPosition{1, 1}, TextPosition{1, 3}}, "x", true),
         "the in-line edit applies");
  Expect(viewport.ReplaceRange(SelectionRange{TextPosition{0, 7}, TextPosition{1, 0}}, "ab", true),
         "the join applies");
  viewport.EndUndoGroup();
  Expect(viewport.SerializeText() == "abcdefgabwxz\n", "both edits are in the buffer");
  Expect(viewport.Undo() && viewport.SerializeText() == "abcdefgh\nwxyz\n", "one undo restores the original");
  Expect(viewport.Redo() && viewport.SerializeText() == "abcdefgabwxz\n",
         "the redo replays the group as it was, without a second copy of line 1");
}

void TestTextViewportClangdFormattingEditsUndoAndRedoExactly() {
  struct Edit {
    TextPosition start;
    TextPosition end;
    const char* text;
  };
  static constexpr Edit kEdits[] = {
      {{4, 12}, {5, 4}, "\n  "},
      {{5, 34}, {6, 4}, "\n  "},
      {{6, 27}, {6, 29}, " "},
      {{6, 35}, {7, 4}, "\n  "},
      {{7, 26}, {7, 29}, " "},
      {{7, 35}, {8, 4}, "\n  "},
      {{8, 35}, {9, 4}, "\n  "},
      {{9, 26}, {9, 29}, " "},
      {{9, 35}, {11, 4}, "\n\n  "},
      {{11, 38}, {12, 4}, "\n  "},
      {{12, 55}, {13, 16}, " "},
      {{13, 41}, {13, 42}, "\n              "},
      {{13, 49}, {14, 4}, "\n  "},
  };
  TextViewport viewport;
  viewport.LoadContent("#include \"scheduler.hpp\"\n\n#include <cstdio>\n\nint main() {\n    taskflow::Scheduler scheduler;\n    scheduler.add(\"ingest\",  5, 3);\n    scheduler.add(\"index\",   8, 4);\n    scheduler.add(\"compact\", 2, 2);\n    scheduler.add(\"flush\",   9, 1);\n\n    const int total = scheduler.run();\n    std::printf(\"scheduled %zu tasks, total cost %d\\n\",\n                scheduler.queue().size(), total);\n    return total;\n}\n", "/tmp/format-main.cpp");
  const std::string original = viewport.SerializeText();
  viewport.BeginUndoGroup();
  for (std::size_t i = std::size(kEdits); i-- > 0;) {
    Expect(viewport.ReplaceRange(SelectionRange{kEdits[i].start, kEdits[i].end}, kEdits[i].text, true),
           "every formatting edit applies");
  }
  viewport.EndUndoGroup();
  const std::string formatted = viewport.SerializeText();
  Expect(viewport.Undo() && viewport.SerializeText() == original, "one undo restores the unformatted text");
  Expect(viewport.Redo() && viewport.SerializeText() == formatted,
         "the redo restores the formatted text:\n" + viewport.SerializeText());
}

void TestTextViewportUndoThenRedoIsTheIdentityOverRandomHistories() {
  for (std::uint32_t round = 0; round < 600; ++round) {
    const bool stepwise = (round & 1U) != 0;
    const std::uint32_t seed = round / 2 + 1;
    std::uint32_t state = seed * 2654435761u;
    const auto next = [&](std::uint32_t bound) {
      state = state * 1664525u + 1013904223u;
      return bound == 0 ? 0u : (state >> 8) % bound;
    };
    static constexpr const char* kPieces[] = {"x", "ab", "\n", "  ", "foo(bar);\n", "}", "\n\n", "é", ""};
    TextViewport viewport;
    viewport.LoadContent("int main() {\n  a;\n  b;\n  c;\n}\n", "/tmp/undo-identity.cpp");
    const auto random_position = [&]() {
      const std::size_t line = next(static_cast<std::uint32_t>(viewport.lines().size()));
      const std::size_t length = viewport.lines()[line].size();
      std::size_t column = next(static_cast<std::uint32_t>(length + 1));
      while (column > 0 && column < length &&
             (static_cast<unsigned char>(viewport.lines()[line][column]) & 0xC0) == 0x80) {
        --column;
      }
      return TextPosition{line, column};
    };
    std::string log;
    for (int step = 0; step < 30; ++step) {
      const std::uint32_t op = next(4);
      if (op == 0) {
        const TextPosition at = random_position();
        viewport.MoveCursorTo(at.line, at.column);
        const char* piece = kPieces[next(9)];
        viewport.InsertText(piece);
        log += "type@" + std::to_string(at.line) + ":" + std::to_string(at.column) + " '" + piece + "'\n";
      } else if (op == 1) {
        const TextPosition at = random_position();
        viewport.MoveCursorTo(at.line, at.column);
        viewport.Backspace();
        log += "bs@" + std::to_string(at.line) + ":" + std::to_string(at.column) + "\n";
      } else {
        std::vector<TextPosition> points;
        const std::uint32_t count = 1 + next(4);
        for (std::uint32_t i = 0; i < count * 2; ++i) {
          points.push_back(random_position());
        }
        std::sort(points.begin(), points.end(), [](TextPosition a, TextPosition b) {
          return a.line != b.line ? a.line < b.line : a.column < b.column;
        });
        viewport.BeginUndoGroup();
        for (std::size_t i = points.size(); i >= 2; i -= 2) {
          const SelectionRange range{points[i - 2], points[i - 1]};
          if (i < points.size() && !(points[i - 1].line < points[i].line ||
                                      (points[i - 1].line == points[i].line && points[i - 1].column < points[i].column))) {
            continue;
          }
          const char* piece = kPieces[next(9)];
          log += "  rr " + std::to_string(range.start.line) + ":" + std::to_string(range.start.column) + "-" +
                 std::to_string(range.end.line) + ":" + std::to_string(range.end.column) + " '" + piece + "'\n";
          (void)viewport.ReplaceRange(range, piece, true);
        }
        viewport.EndUndoGroup();
        log += "batch\n";
      }
      if (stepwise) {
        const std::string now = viewport.SerializeText();
        if (viewport.Undo()) {
          const std::string mid = viewport.SerializeText();
          (void)viewport.Redo();
          if (viewport.SerializeText() != now) {
            Expect(false, "seed " + std::to_string(seed) + " step " + std::to_string(step) + " stepwise\n" + log +
                              "--- now\n" + now + "--- mid\n" + mid + "--- after\n" + viewport.SerializeText());
            return;
          }
        }
        continue;
      }
      if (next(3) == 0) {
        const std::string before = viewport.SerializeText();
        const std::uint32_t asked = 1 + next(4);
        std::uint32_t undone = 0;
        for (std::uint32_t i = 0; i < asked; ++i) {
          undone += viewport.Undo() ? 1 : 0;
        }
        for (std::uint32_t i = 0; i < undone; ++i) {
          (void)viewport.Redo();
        }
        log += "check " + std::to_string(undone) + "\n";
        if (viewport.SerializeText() != before) {
          Expect(false, "seed " + std::to_string(seed) + " step " + std::to_string(step) + ": " +
                            std::to_string(undone) + " undos/redos changed the text\n" + log +
                            "--- before\n" + before + "--- after\n" + viewport.SerializeText());
          return;
        }
      }
    }
  }
}

}

void TestUndoHistoryEnforcesByteBudget() {
  microide::editor::TextViewportUndoHistory history;
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

void TestTextViewportGroupedNonContiguousDeletesUndoFully() {
  TextViewport viewport;
  std::string content;
  for (int i = 0; i < 12; ++i) {
    content += "#include \"h" + std::to_string(i) + "\"\n";
  }
  content += "int main() { return 0; }\n";
  viewport.LoadContent(content, "/tmp/grouped-undo.cpp");
  const auto original = viewport.lines().Snapshot();

  std::vector<SelectionRange> edits = {
      {{1, 0}, {2, 0}}, {{4, 0}, {5, 0}}, {{7, 0}, {8, 0}}, {{10, 0}, {11, 0}}};
  std::sort(edits.begin(), edits.end(), [](const SelectionRange& a, const SelectionRange& b) {
    return a.start.line > b.start.line;
  });

  viewport.BeginUndoGroup();
  for (const auto& range : edits) {
    viewport.ReplaceRange(range, "", true);
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

void TestTextViewportUndoPastSaveMarksDirty() {
  const std::filesystem::path path =
      std::filesystem::temp_directory_path() / "microide-undo-save-dirty.txt";
  std::filesystem::remove(path);
  TextViewport viewport;
  viewport.LoadContent("line0\nline1\nline2\n", path);
  Expect(!viewport.dirty(), "a freshly loaded buffer is clean");
  const auto original = viewport.lines().Snapshot();

  viewport.ReplaceRange(SelectionRange{{1, 0}, {2, 0}}, "", true);
  Expect(viewport.dirty(), "deleting a line marks the buffer dirty");
  Expect(viewport.Save(microide::project::LocalFileWriteGate()), "save should succeed");
  Expect(!viewport.dirty(), "a successful save clears the dirty flag");

  Expect(viewport.Undo(), "undo should succeed");
  Expect(viewport.lines().Snapshot() == original, "undo restores the deleted line");
  Expect(viewport.dirty(),
         "undoing past a save must mark the buffer dirty; the content now differs from disk");

  Expect(viewport.Redo(), "redo should succeed");
  Expect(!viewport.dirty(), "redoing back to the saved content returns to clean");
  std::filesystem::remove(path);
}

void TestTextViewportAdoptedDiskBaselineMovesTheCleanPoint() {
  TextViewport viewport;
  viewport.LoadContent("def f(x):\n", "/tmp/microide-adopt-baseline/code.py");
  const auto original = viewport.lines().Snapshot();
  viewport.ReplaceRange(SelectionRange{{0, 0}, {0, 0}}, "        ", true);
  Expect(viewport.dirty(), "the indent marks the buffer dirty");

  TextViewport disk;
  disk.LoadContent("        def f(x):\n", "/tmp/microide-adopt-baseline/code.py");
  Expect(viewport.HoldsSameFileContentAs(disk), "the file now holds the buffer's text");
  viewport.AdoptDiskBaselineOf(disk);
  Expect(!viewport.dirty(), "a buffer equal to its file is clean");

  Expect(viewport.Undo(), "undo should succeed");
  Expect(viewport.lines().Snapshot() == original, "undo restores the loaded text");
  Expect(viewport.dirty(),
         "undoing past an adopted disk baseline must read dirty; the file holds the indent");
  Expect(viewport.Redo(), "redo should succeed");
  Expect(!viewport.dirty(), "redoing back to the file's text returns to clean");
}

void TestTextViewportRevertToDiskKeepsTheCarriageReturnsOfABinaryFile() {
  const std::string bytes("\x00\x01\n\x0b\x0c\r\x0e\x0f\n\x10", 10);
  TextViewport viewport;
  viewport.LoadContent(bytes, "/tmp/microide-revert-binary/binary.bin");
  Expect(viewport.encoding() == TextViewport::TextEncoding::Bytes, "NUL bytes load as an opaque document");
  Expect(viewport.SerializeDocumentText() == bytes, "an opaque document serializes back byte for byte");
  viewport.ReplaceRange(SelectionRange{{0, 0}, {0, 0}}, "x", true);
  Expect(viewport.dirty(), "the edit marks the buffer dirty");

  TextViewport disk;
  disk.LoadContent(bytes, "/tmp/microide-revert-binary/binary.bin");
  Expect(viewport.RevertToDiskContent(disk), "revert should apply");
  Expect(viewport.lines().size() == disk.lines().size(),
         "the reverted buffer has the file's lines; a CR byte is not a line break");
  Expect(viewport.SerializeDocumentText() == bytes, "the reverted buffer holds the file's bytes");
  Expect(!viewport.dirty(), "a reverted buffer is clean");
  Expect(viewport.Undo(), "the revert is one undoable edit");
  Expect(viewport.dirty(), "undoing the revert brings the discarded edit back, dirty");
}

void TestTextViewportSaveAboveUndoneEditsDirtiesRedo() {
  const std::filesystem::path path =
      std::filesystem::temp_directory_path() / "microide-save-above-undone.txt";
  std::filesystem::remove(path);
  TextViewport viewport;
  viewport.LoadContent("L\n", path);

  viewport.ReplaceRange(SelectionRange{{0, 0}, {0, 0}}, "a", true);
  viewport.ReplaceRange(SelectionRange{{1, 0}, {1, 0}}, "b", true);
  Expect(viewport.Undo(), "undo should reverse the second insert");

  Expect(viewport.Save(microide::project::LocalFileWriteGate()), "saving while above an undone edit should succeed");
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

void TestTextViewportSaveThenCoalescingKeystrokeStartsFreshEntry() {
  const std::filesystem::path path =
      std::filesystem::temp_directory_path() / "microide-save-then-type.txt";
  std::filesystem::remove(path);
  TextViewport viewport;
  viewport.LoadContent("", path);

  viewport.InsertCharacter('a');
  Expect(viewport.Save(microide::project::LocalFileWriteGate()), "save should succeed");
  Expect(!viewport.dirty(), "save clears dirty");

  viewport.InsertCharacter('b');
  Expect(viewport.dirty(), "typing after a save dirties the buffer");

  Expect(viewport.Undo(), "undo should reverse only the post-save keystroke");
  Expect(viewport.lines()[0] == "a",
         "undo must land on the saved content 'a', proving the keystroke did not coalesce into it");
  Expect(!viewport.dirty(), "landing back on the saved content reads clean");
  std::filesystem::remove(path);
}

void TestTextViewportNestedUndoGroupsDoNotDoubleCount() {
  const std::filesystem::path path =
      std::filesystem::temp_directory_path() / "microide-undo-nested.txt";
  std::filesystem::remove(path);
  TextViewport viewport;
  viewport.LoadContent("hello\n", path);
  viewport.MoveCursorTo(0, 5);

  viewport.BeginUndoGroup();
  viewport.InsertText("A");
  viewport.BeginUndoGroup();
  viewport.InsertText("B");
  viewport.EndUndoGroup();
  viewport.InsertText("C");
  viewport.EndUndoGroup();

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

void TestTextViewportGroupedEditSaveUndoMarksDirty() {
  const std::filesystem::path path =
      std::filesystem::temp_directory_path() / "microide-grouped-save.txt";
  std::filesystem::remove(path);
  TextViewport viewport;
  viewport.LoadContent("l0\nl1\nl2\nl3\n", path);
  const auto original = viewport.lines().Snapshot();

  viewport.BeginUndoGroup();
  viewport.ReplaceRange(SelectionRange{{2, 0}, {3, 0}}, "", true);
  viewport.ReplaceRange(SelectionRange{{0, 0}, {1, 0}}, "", true);
  viewport.EndUndoGroup();
  Expect(viewport.dirty(), "a grouped edit dirties the buffer");
  Expect(viewport.Save(microide::project::LocalFileWriteGate()), "save should succeed");
  Expect(!viewport.dirty(), "save clears dirty");

  Expect(viewport.Undo(), "one undo reverses the whole group");
  Expect(viewport.lines().Snapshot() == original, "undo restores the full original buffer");
  Expect(viewport.dirty(), "undoing past the save dirties (content differs from disk)");
  Expect(viewport.Redo(), "redo re-applies the group");
  Expect(!viewport.dirty(), "redoing back to the saved content cleans");
  std::filesystem::remove(path);
}

void TestTextViewportSaveEvictionNeverFalseClean() {
  const std::filesystem::path path =
      std::filesystem::temp_directory_path() / "microide-save-eviction.txt";
  std::filesystem::remove(path);
  TextViewport viewport;
  viewport.LoadContent("seed\n", path);

  viewport.ReplaceRange(SelectionRange{{0, 0}, {0, 0}}, "x", true);
  Expect(viewport.Save(microide::project::LocalFileWriteGate()), "save should succeed");
  Expect(!viewport.dirty(), "save clears dirty");

  for (int i = 0; i < 200; ++i) {
    viewport.ReplaceRange(SelectionRange{{0, 0}, {0, 0}}, "z", true);
  }
  while (viewport.Undo()) {
  }
  Expect(viewport.dirty(),
         "after the saved entry is evicted, the oldest reachable content differs from disk "
         "and must report dirty (no false clean)");
  std::filesystem::remove(path);
}

void TestTextViewportDoubleSaveRebaselines() {
  const std::filesystem::path path =
      std::filesystem::temp_directory_path() / "microide-double-save.txt";
  std::filesystem::remove(path);
  TextViewport viewport;
  viewport.LoadContent("base\n", path);

  viewport.InsertText("A");
  Expect(viewport.Save(microide::project::LocalFileWriteGate()), "first save should succeed");
  viewport.InsertText("B");
  Expect(viewport.Save(microide::project::LocalFileWriteGate()), "second save should succeed");
  Expect(!viewport.dirty(), "the second save is the clean point");

  Expect(viewport.Undo(), "undo returns to the first-save content");
  Expect(viewport.dirty(),
         "the first-save content differs from disk (which now holds the second save) -> dirty");
  Expect(viewport.Redo(), "redo returns to the second-save content");
  Expect(!viewport.dirty(), "the second-save content matches disk -> clean");
  std::filesystem::remove(path);
}

void TestTextViewportReloadPreservingViewStateKeepsScrollUnderSelection() {
  TextViewport viewport;
  std::string text;
  for (int i = 0; i < 400; ++i) {
    text += "line " + std::to_string(i) + "\n";
  }
  viewport.LoadContent(text, "/tmp/reload.txt");
  viewport.SetViewportSize(20, 80);

  viewport.SetScrollLine(100);
  viewport.MoveCursorTo(0, 0);
  viewport.SetScrollLine(100);
  viewport.MoveCursorTo(399, 0, true);
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

void TestTextViewportSoftWrapTabSizeChangePreservesWrappedColumn() {
  TextViewport viewport;
  const std::string long_line(200, 'a');
  viewport.LoadContent(long_line + "\n" + long_line + "\n", "/tmp/soft-wrap-tab.txt");
  viewport.SetViewportSize(10, 80);
  viewport.SetSoftWrap(true);
  viewport.SetTabSize(4);

  viewport.MoveCursorTo(0, 150);
  Expect(viewport.cursor_line() == 0 && viewport.cursor_column() == 150,
         "precondition: caret is at column 150 on a continuation row");

  viewport.SetTabSize(8);
  Expect(viewport.cursor_line() == 0 && viewport.cursor_column() == 150,
         "changing tab size must keep the wrapped caret column, not snap to line start");
}

void TestTextViewportAppliedEditReplaysRandomEdits() {
  using microide::editor::SelectionRange;
  using microide::editor::TextPosition;
  using microide::editor::TextViewport;
  const auto serialize = [](const TextViewport& viewport) {
    std::string text;
    const auto& lines = viewport.lines();
    for (std::size_t i = 0; i < lines.size(); ++i) {
      text += lines[i];
      if (i + 1 < lines.size()) {
        text += '\n';
      }
    }
    return text;
  };
  const auto offset_of = [](const std::string& text, TextPosition position) {
    std::size_t offset = 0;
    for (std::size_t line = 0; line < position.line; ++line) {
      const std::size_t newline = text.find('\n', offset);
      if (newline == std::string::npos) {
        return text.size();
      }
      offset = newline + 1;
    }
    return std::min(text.size(), offset + position.column);
  };
  std::uint64_t state = 0x9E3779B97F4A7C15ull;
  const auto next = [&state](std::uint64_t bound) {
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    return bound == 0 ? 0 : static_cast<std::size_t>(state % bound);
  };
  static constexpr const char* kPieces[] = {"a", "b", "\n", "é", "中", "😀", " ", "xy\nz"};
  TextViewport viewport;
  viewport.LoadContent("ab\né中\n", "/tmp/applied-edit.txt");
  std::string previous = serialize(viewport);
  int checked = 0;
  for (int step = 0; step < 600; ++step) {
    const std::size_t line_count = viewport.lines().size();
    const std::size_t line = next(line_count);
    const std::size_t column =
        microide::editor::TextLayout::ClampTextColumn(viewport.lines()[line], next(64));
    const std::size_t kind = next(6);
    if (kind == 0) {
      viewport.MoveCursorTo(line, column, false);
      viewport.InsertText(kPieces[next(std::size(kPieces))]);
    } else if (kind == 1) {
      viewport.MoveCursorTo(line, column, false);
      viewport.Backspace();
    } else if (kind == 2) {
      viewport.MoveCursorTo(line, column, false);
      viewport.DeleteForward();
    } else if (kind == 3) {
      const std::size_t end_line = std::min(line_count - 1, line + next(2));
      const std::size_t end_column =
          microide::editor::TextLayout::ClampTextColumn(viewport.lines()[end_line], next(64));
      SelectionRange range{TextPosition{line, column}, TextPosition{end_line, end_column}};
      viewport.ReplaceRange(TextViewport::NormalizeRange(range), kPieces[next(std::size(kPieces))],
                            true);
    } else if (kind == 4) {
      viewport.Undo();
    } else {
      viewport.Redo();
    }
    const std::string current = serialize(viewport);
    if (const auto& edit = viewport.last_applied_edit(); edit.has_value() && current != previous) {
      const SelectionRange range = TextViewport::NormalizeRange(edit->range_before);
      const std::size_t begin = offset_of(previous, range.start);
      const std::size_t end = std::max(begin, offset_of(previous, range.end));
      const std::string replayed =
          previous.substr(0, begin) + edit->replacement_text + previous.substr(end);
      Expect(replayed == current,
             ("step " + std::to_string(step) + ": replaying the applied edit onto the previous "
              "text gives the new text; previous=[" + previous + "] range=" +
              std::to_string(range.start.line) + ":" + std::to_string(range.start.column) + "-" +
              std::to_string(range.end.line) + ":" + std::to_string(range.end.column) +
              " replacement=[" + edit->replacement_text + "] got=[" + replayed + "] want=[" +
              current + "]")
                 .c_str());
      ++checked;
    }
    previous = current;
  }
  Expect(checked >= 200, "enough edits published an AppliedEdit to mean anything");
}


void TestTextViewportUndoRedoRoundTripRandomEdits() {
  using microide::editor::SelectionRange;
  using microide::editor::TextPosition;
  using microide::editor::TextViewport;
  const auto serialize = [](const TextViewport& viewport) {
    std::string text;
    const auto& lines = viewport.lines();
    for (std::size_t i = 0; i < lines.size(); ++i) {
      text += lines[i];
      if (i + 1 < lines.size()) {
        text += '\n';
      }
    }
    return text;
  };
  std::uint64_t state = 0x7F4A7C159E3779B9ull;
  const auto next = [&state](std::uint64_t bound) {
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    return bound == 0 ? 0 : static_cast<std::size_t>(state % bound);
  };
  static constexpr const char* kPieces[] = {"a", "b", "\n", "é", "中", "😀", " ", "xy\nz"};
  for (int round = 0; round < 12; ++round) {
    TextViewport viewport;
    viewport.LoadContent("ab\né中\n", "/tmp/undo-redo.txt");
    const std::string original = serialize(viewport);
    std::vector<std::string> after_each;
    for (int step = 0; step < 40; ++step) {
      const std::size_t line = next(viewport.lines().size());
      const std::size_t column =
          microide::editor::TextLayout::ClampTextColumn(viewport.lines()[line], next(64));
      const std::size_t kind = next(4);
      if (kind == 0) {
        viewport.MoveCursorTo(line, column, false);
        viewport.InsertText(kPieces[next(std::size(kPieces))]);
      } else if (kind == 1) {
        viewport.MoveCursorTo(line, column, false);
        viewport.Backspace();
      } else if (kind == 2) {
        viewport.MoveCursorTo(line, column, false);
        viewport.DeleteForward();
      } else {
        const std::size_t end_line = std::min(viewport.lines().size() - 1, line + next(2));
        const std::size_t end_column =
            microide::editor::TextLayout::ClampTextColumn(viewport.lines()[end_line], next(64));
        SelectionRange range{TextPosition{line, column}, TextPosition{end_line, end_column}};
        viewport.ReplaceRange(TextViewport::NormalizeRange(range),
                              kPieces[next(std::size(kPieces))], true);
      }
      after_each.push_back(serialize(viewport));
    }
    const std::string final_text = after_each.back();
    for (std::size_t i = after_each.size(); i-- > 0;) {
      const std::string expected = i == 0 ? original : after_each[i - 1];
      if (after_each[i] == expected) {
        continue;
      }
      Expect(viewport.Undo(), "an edit that changed the text can be undone");
      Expect(serialize(viewport) == expected,
             ("round " + std::to_string(round) + " undo " + std::to_string(i) +
              " restores the previous text; got=[" + serialize(viewport) + "] want=[" + expected +
              "]")
                 .c_str());
    }
    Expect(serialize(viewport) == original, "undoing everything restores the original");
    while (viewport.Redo()) {
    }
    Expect(serialize(viewport) == final_text,
           ("round " + std::to_string(round) + " redoing everything restores the final text; got=[" +
            serialize(viewport) + "] want=[" + final_text + "]")
               .c_str());
  }
}

void TestTextViewportCopiedLayoutCacheDoesNotAliasTheSource() {
  std::string content;
  for (int line = 0; line < 400; ++line) {
    content += "line " + std::to_string(line) + " with some text to lay out\n";
  }

  TextViewport reference;
  reference.LoadContent(content);
  reference.SetViewportSize(40, 80);

  TextViewport copy;
  {
    TextViewport source;
    source.LoadContent(content);
    source.SetViewportSize(40, 80);
    for (std::size_t line = 0; line < 200; ++line) {
      (void)source.VisibleLineLayoutRef(line);
    }
    copy = source;
    Expect(source.VisibleLineLruIsConsistentForTesting(),
           "the source's own recency list must be well formed to begin with");
    Expect(copy.VisibleLineLruIsConsistentForTesting(),
           "a copied cache's recency list must link its OWN map nodes");
  }

  for (std::size_t round = 0; round < 3; ++round) {
    for (std::size_t line = 0; line < 400; ++line) {
      const microide::editor::LayoutLine& laid_out = copy.VisibleLineLayoutRef(line);
      const microide::editor::LayoutLine& expected = reference.VisibleLineLayoutRef(line);
      Expect(laid_out.text == expected.text && laid_out.visual_columns == expected.visual_columns,
             "a copied viewport must lay out every line exactly as an uncopied one");
    }
  }

  copy.MoveCursorTo(10, 0);
  copy.InsertText("x");
  for (std::size_t line = 0; line < 400; ++line) {
    (void)copy.VisibleLineLayoutRef(line);
  }
  Expect(copy.lines().size() == reference.lines().size(),
         "the edit above must not have disturbed the document shape");
  Expect(copy.VisibleLineLruIsConsistentForTesting(),
         "eviction and invalidation must leave the recency list well formed");
}

void TestFormattedTextIsOneUndoStepAndKeepsHistory() {
  TextViewport view;
  view.LoadContent("int  a;\nint b;\nint c;\n", "/tmp/format-undo.cpp");
  view.MoveCursorTo(1, 5);
  view.InsertText("x");
  Expect(view.ApplyFormattedText("int a;\nint bx;\nint c;\n"), "the formatting applies");
  Expect(view.SerializeDocumentText() == "int a;\nint bx;\nint c;\n", "formatted");
  Expect(view.cursor_line() == 1 && view.cursor_column() == 6, "the cursor stays where it was");
  Expect(view.Undo() && view.SerializeDocumentText() == "int  a;\nint bx;\nint c;\n",
         "one undo takes back exactly the formatting; got: " + view.SerializeDocumentText());
  Expect(view.Undo() && view.SerializeDocumentText() == "int  a;\nint b;\nint c;\n",
         "and the edit before the save is still undoable");
  Expect(view.Redo() && view.Redo() && view.SerializeDocumentText() == "int a;\nint bx;\nint c;\n",
         "redo replays both");

  TextViewport shrink;
  shrink.LoadContent("a\n\n\nb\n", "/tmp/format-shrink.txt");
  Expect(shrink.ApplyFormattedText("a\nb\n") && shrink.SerializeDocumentText() == "a\nb\n",
         "a formatter that deletes lines leaves no stray blank line");
  Expect(shrink.Undo() && shrink.SerializeDocumentText() == "a\n\n\nb\n", "and it undoes");
  TextViewport grow;
  grow.LoadContent("{}\n", "/tmp/format-grow.txt");
  Expect(grow.ApplyFormattedText("{\n}\n") && grow.SerializeDocumentText() == "{\n}\n",
         "a formatter that adds lines");
  Expect(!grow.ApplyFormattedText("{\n}\n"), "an output equal to the buffer is no edit");

  TextViewport crlf;
  crlf.LoadContent("a\r\nb\r\n", "/tmp/format-crlf.txt");
  Expect(crlf.ApplyFormattedText("a\r\nB\r\n") && crlf.SerializeDocumentText() == "a\r\nB\r\n",
         "CRLF output on a CRLF buffer changes only the line that changed");
}


void TestSelectedTextOfAViewportAnotherViewportEditedStaysInTheDocument() {
  TextViewport pane;
  pane.LoadContent("one\ntwo\nsix\nabcd\nefghi\n", "/tmp/shared-selection.txt");
  pane.MoveCursorTo(3, 4);
  pane.MoveCursorTo(4, 5, true);
  Expect(pane.SelectedText() == "\nefghi", "the pane selects across lines 4 and 5");

  TextViewport tab = pane;
  tab.MoveCursorTo(3, 0);
  tab.MoveCursorTo(4, 5, true);
  tab.Backspace();
  tab.InsertText("\n\n");

  const auto range = pane.selection_range();
  Expect(!range.has_value() || (range->end.line < pane.line_count() &&
                                range->end.column <= pane.lines().LineLength(range->end.line) &&
                                range->start.column <= pane.lines().LineLength(range->start.line)),
         "the pane's selection lies inside the document the other viewport left");
  const std::string read = pane.SelectedText();
  pane.ReconcileCaretsWithDocument();
  Expect(read == pane.SelectedText(),
         "a read before the pane's next use sees what that use will: '" + read + "' vs '" +
             pane.SelectedText() + "'");
}


void TestEveryCutPathLeavesEveryCaretInside() {
  const std::string long_line(800, 'x');
  const std::string text = "a\n" + long_line + "\nshort\n" + long_line + "\nend\n";
  const auto inside = [](const TextViewport& view, const std::string& what) {
    const auto legal = [&](std::size_t line, std::size_t column) {
      return line < view.line_count() && column <= view.lines().LineLength(line);
    };
    Expect(legal(view.cursor_line(), view.cursor_column()), what + ": the primary caret is inside");
    for (const auto& caret : view.secondary_carets()) {
      Expect(legal(caret.line, caret.column),
             what + ": a secondary caret at " + std::to_string(caret.line + 1) + ":" +
                 std::to_string(caret.column + 1) + " is inside (" + std::to_string(view.line_count()) + " lines)");
    }
  };
  {
    TextViewport view;
    view.LoadContent(text, "/tmp/cut-lines.txt");
    view.MoveCursorTo(1, 800);
    view.AddSecondaryCaret(3, 800);
    view.DeleteCurrentLine();
    inside(view, "every caret's line cut");
  }
  {
    TextViewport view;
    view.LoadContent(text, "/tmp/cut-primary.txt");
    view.MoveCursorTo(0, 0);
    view.MoveCursorTo(2, 3, true);
    view.AddSecondaryCaret(3, 800);
    view.DeleteSelectedText();
    inside(view, "the primary's selection cut with a bare secondary caret below it");
  }
  {
    TextViewport view;
    view.LoadContent(text, "/tmp/cut-multi.txt");
    view.MoveCursorTo(0, 0);
    view.MoveCursorTo(1, 800, true);
    view.AddSecondaryCaretWithRange(editor::SelectionRange{{3, 0}, {3, 800}});
    view.DeleteMultiCaretSelections();
    inside(view, "every caret's selection cut");
  }
}


void TestCaretBoundsFaultNamesTheCaretOutsideTheDocument() {
  TextViewport behind;
  behind.LoadContent("one\ntwo three\nfour\n", "/tmp/bounds.txt");
  behind.MoveCursorTo(1, 9);
  Expect(behind.CaretBoundsFault().empty(), "a caret inside its line is no fault");
  TextViewport front = behind;
  front.MoveCursorTo(1, 0);
  front.MoveCursorTo(1, 9, true);
  front.Backspace();
  const std::string fault = behind.CaretBoundsFault();
  Expect(fault == "caret at 2:10 (line length 0, 4 lines)", "the stale caret is described: '" + fault + "'");
  behind.ReconcileCaretsWithDocument();
  Expect(behind.CaretBoundsFault().empty(), "and once reconciled it is inside again");
}

void RegisterTextViewportTests(std::vector<TestCase>& tests) {
  AddTest(tests, "TextViewportSave/ASaveWrittenWhileTheBufferMovedOnLeavesItDirtyAndUndoFindsTheSavedText",
          TestASaveWrittenWhileTheBufferMovedOnLeavesItDirtyAndUndoFindsTheSavedText);
  AddTest(tests, "TextViewportSave/ASaveNormalizedWhileTheBufferMovedOnHasNoCleanPoint",
          TestASaveNormalizedWhileTheBufferMovedOnHasNoCleanPoint);
  AddTest(tests, "TextViewportSave/ASaveForADocumentThatWasReplacedDoesNotApply",
          TestASaveForADocumentThatWasReplacedDoesNotApply);
  AddTest(tests, "TextViewportUndo/ASavePointKeepsLaterTypingOutOfTheSavedEntry",
          TestASavePointKeepsLaterTypingOutOfTheSavedEntry);
  AddTest(tests, "TextViewportUndo/ASavePointSurvivesItsEntryBeingUndone", TestASavePointSurvivesItsEntryBeingUndone);
  AddTest(tests, "TextViewportUndo/ASavePointAtTheBaseIsLostWithTheOldestEntries",
          TestASavePointAtTheBaseIsLostWithTheOldestEntries);
AddTest(tests, "TextViewport/CaretBoundsFaultNamesTheCaretOutsideTheDocument", TestCaretBoundsFaultNamesTheCaretOutsideTheDocument);
AddTest(tests, "TextViewport/EveryCutPathLeavesEveryCaretInside", TestEveryCutPathLeavesEveryCaretInside);
AddTest(tests, "TextViewport/SelectedTextOfAViewportAnotherViewportEditedStaysInTheDocument",
        TestSelectedTextOfAViewportAnotherViewportEditedStaysInTheDocument);
AddTest(tests, "TextViewport/AGroupedJoinOverAnEditedLineUndoesAndRedoesExactly",
        TestTextViewportAGroupedJoinOverAnEditedLineUndoesAndRedoesExactly);
AddTest(tests, "TextViewport/ClangdFormattingEditsUndoAndRedoExactly",
        TestTextViewportClangdFormattingEditsUndoAndRedoExactly);
AddTest(tests, "TextViewport/UndoThenRedoIsTheIdentityOverRandomHistories",
        TestTextViewportUndoThenRedoIsTheIdentityOverRandomHistories);
  AddTest(tests, "TextViewport/AppliedEditRecordAnswersForOneStepOnly", TestAppliedEditRecordAnswersForOneStepOnly);
  AddTest(tests, "TextViewport/FormattedTextIsOneUndoStepAndKeepsHistory",
          TestFormattedTextIsOneUndoStepAndKeepsHistory);
  AddTest(tests, "TextViewport/LineVerbsTreatACollapsedFoldAsOneLine",
          TestTextViewportLineVerbsTreatACollapsedFoldAsOneLine);
  AddTest(tests, "TextViewport/VerticalMovePreservesColumnWhenScrolled",
          TestTextViewportVerticalMovePreservesColumnWhenScrolled);
  AddTest(tests, "TextViewport/ReloadPreservingViewStateKeepsScrollUnderSelection",
          TestTextViewportReloadPreservingViewStateKeepsScrollUnderSelection);
  AddTest(tests, "TextViewport/SoftWrapTabSizeChangePreservesWrappedColumn",
          TestTextViewportSoftWrapTabSizeChangePreservesWrappedColumn);
  AddTest(tests, "TextViewport/AdoptedDiskBaselineMovesTheCleanPoint",
          TestTextViewportAdoptedDiskBaselineMovesTheCleanPoint);
  AddTest(tests, "TextViewport/RevertToDiskKeepsTheCarriageReturnsOfABinaryFile",
          TestTextViewportRevertToDiskKeepsTheCarriageReturnsOfABinaryFile);
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
  AddTest(tests, "TextViewport/CopiedLayoutCacheDoesNotAliasTheSource",
          TestTextViewportCopiedLayoutCacheDoesNotAliasTheSource);
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
  AddTest(tests, "TextViewport/Utf8BomIsStrippedOnOpenAndWrittenOnSave",
          TestTextViewportUtf8BomIsStrippedOnOpenAndWrittenOnSave);
  AddTest(tests, "TextViewport/ABufferThatBecameBytesSavesItsMarkAndEndings",
          TestTextViewportABufferThatBecameBytesSavesItsMarkAndEndings);
  AddTest(tests, "TextViewport/SmallFileKeepsSyntaxHighlighting",
          TestTextViewportSmallFileKeepsSyntaxHighlighting);
  AddTest(tests, "TextViewport/SplitSiblingCaretsSurviveAForeignEdit",
          TestTextViewportSplitSiblingCaretsSurviveAForeignEdit);
  AddTest(tests, "TextViewport/StaleSiblingIsReconciledByEveryVerb",
          TestTextViewportStaleSiblingIsReconciledByEveryVerb);
  AddTest(tests, "TextViewport/SplitSiblingsShareOneUndoHistory",
          TestTextViewportSplitSiblingsShareOneUndoHistory);
  AddTest(tests, "TextViewport/SplitSiblingEditRefreshesHighlightTokens",
          TestTextViewportSplitSiblingEditRefreshesHighlightTokens);
  AddTest(tests, "TextViewport/EditStalesTokenCacheSuffixOnly",
          TestTextViewportEditStalesTokenCacheSuffixOnly);
  AddTest(tests, "TextViewport/SyntaxReloadStalesEveryTokenCacheEntry",
          TestTextViewportSyntaxReloadStalesEveryTokenCacheEntry);
  AddTest(tests, "TextViewport/EditRecyclesVisibleLineLayouts",
          TestTextViewportEditRecyclesVisibleLineLayouts);
  AddTest(tests, "TextViewport/PlainAsciiPrefixMemoMatchesTheDefinition",
          TestTextViewportPlainAsciiPrefixMemoMatchesTheDefinition);
  AddTest(tests, "TextViewport/ScrollRecyclesHighlightCacheNodes",
          TestTextViewportScrollRecyclesHighlightCacheNodes);
  AddTest(tests, "TextViewport/ScrollHighlightCacheStopsAllocatingAtTheCap",
          TestTextViewportScrollHighlightCacheStopsAllocatingAtTheCap);
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
  AddTest(tests, "TextViewport/SettledEditKeepsTheLinesBelowCurrent",
          TestTextViewportSettledEditKeepsTheLinesBelowCurrent);
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
  AddTest(tests, "TextViewport/SetLineEndingIsAnUndoableEdit", TestTextViewportSetLineEndingIsAnUndoableEdit);
  AddTest(tests, "TextViewport/ReplaceAllClearsLastAppliedEdit",
          TestTextViewportReplaceAllClearsLastAppliedEdit);
  AddTest(tests, "TextViewport/ReplaceAllRangesCarriesTheCarets", TestTextViewportReplaceAllRangesCarriesTheCarets);
  AddTest(tests, "TextViewport/UndoOntoAPlaceholderBufferKeepsNoHighlightState",
          TestTextViewportUndoOntoAPlaceholderBufferKeepsNoHighlightState);
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
  AddTest(tests, "TextViewport/ApplyEntryToBufferAppliesEveryDisjointPart",
          TestApplyEntryToBufferAppliesEveryDisjointPart);
  AddTest(tests, "TextViewport/MultiCaretUndoMatchesSnapshotOracle",
          TestTextViewportMultiCaretUndoMatchesSnapshotOracle);
  AddTest(tests, "TextViewport/MultiCaretUndoEntryDoesNotSpanTheGap",
          TestTextViewportMultiCaretUndoEntryDoesNotSpanTheGap);
  AddTest(tests, "TextViewport/MultiCaretInsertAndUndoAreAtomic",
          TestTextViewportMultiCaretInsertAndUndoAreAtomic);
  AddTest(tests, "TextViewport/GroupedChildEntriesDoNotCopyTheCaretSet",
          TestTextViewportGroupedChildEntriesDoNotCopyTheCaretSet);
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
  AddTest(tests, "TextViewport/HomeAndEndMoveWithinTheWrappedRow",
          TestTextViewportHomeAndEndMoveWithinTheWrappedRow);
  AddTest(tests, "TextViewport/HomeTogglesBetweenIndentAndColumnZero",
          TestTextViewportHomeTogglesBetweenIndentAndColumnZero);
  AddTest(tests, "TextViewport/SoftWrapCaretOnFoldHiddenLineEscapesInOneStep",
          TestTextViewportSoftWrapCaretOnFoldHiddenLineEscapesInOneStep);
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
  AddTest(tests, "TextViewport/SoftWrapViewportResizeRebuildsWrapCacheOncePerWidth",
          TestTextViewportSoftWrapViewportResizeRebuildsWrapCacheOncePerWidth);
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
  AddTest(tests, "TextViewport/SoftWrapRowsReuseTheVisibleLineCache",
          TestTextViewportSoftWrapRowsReuseTheVisibleLineCache);
  AddTest(tests, "TextViewport/SoftWrapVerticalMotionCrossesShortRows",
          TestTextViewportSoftWrapVerticalMotionCrossesShortRows);
  AddTest(tests, "TextViewport/SoftWrapCaretRendersAtWrappedRowEnd",
          TestTextViewportSoftWrapCaretRendersAtWrappedRowEnd);
  AddTest(tests, "TextViewport/SoftWrapClickPastRowEndStaysOnThatRow",
          TestTextViewportSoftWrapClickPastRowEndStaysOnThatRow);
  AddTest(tests, "TextViewport/SoftWrapHangingIndentPreservesScreenColumn",
          TestTextViewportSoftWrapHangingIndentPreservesScreenColumn);
  AddTest(tests, "TextViewport/SoftWrapWidthChangeReanchorsScroll",
          TestTextViewportSoftWrapWidthChangeReanchorsScroll);
  AddTest(tests, "TextViewport/SoftWrapToggleReanchorsScroll",
          TestTextViewportSoftWrapToggleReanchorsScroll);
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
  AddTest(tests, "TextViewport/TypingOnTheWidestLineDoesNotRescanTheWidthTable",
          TestTextViewportTypingOnTheWidestLineDoesNotRescanTheWidthTable);
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
  AddTest(tests, "TextViewport/RuntimeSyntaxDetectFiletypeReadsOnlyABoundedHead",
          TestRuntimeSyntaxDetectFiletypeReadsOnlyABoundedHead);
  AddTest(tests, "TextViewport/SingleLineEditAllocatesABoundedMultipleOfTheLine",
          TestTextViewportSingleLineEditAllocatesABoundedMultipleOfTheLine);
  AddTest(tests, "TextViewport/InlineEditDerivesLineWidthFromTheSplice",
          TestTextViewportInlineEditDerivesLineWidthFromTheSplice);
  AddTest(tests, "TextViewport/WidestLineIsMaintainedAcrossEdits",
          TestTextViewportWidestLineIsMaintainedAcrossEdits);
  AddTest(tests, "TextViewport/CaretColumnConversionsMatchTheDirectWalk",
          TestTextViewportCaretColumnConversionsMatchTheDirectWalk);
  AddTest(tests, "TextViewport/TypingInALongLineDoesNotWalkItForCaretColumns",
          TestTextViewportTypingInALongLineDoesNotWalkItForCaretColumns);
  AddTest(tests, "TextViewport/SoftWrapIncrementalRowsMatchAFullRewrap",
          TestTextViewportSoftWrapIncrementalRowsMatchAFullRewrap);
  AddTest(tests, "TextViewport/SoftWrappedLongLineDoesNotWalkItForCaretColumns",
          TestTextViewportSoftWrappedLongLineDoesNotWalkItForCaretColumns);
  AddTest(tests, "TextViewport/TypingInALongLineCopiesNothing",
          TestTextViewportTypingInALongLineCopiesNothing);
  AddTest(tests, "TextViewport/LanguageIdMemoIsAllocationFreeOnASettledBuffer",
          TestTextViewportLanguageIdMemoIsAllocationFreeOnASettledBuffer);
  AddTest(tests, "TextViewport/LanguageIdMemoInvalidatesOnContentChange",
          TestTextViewportLanguageIdMemoInvalidatesOnContentChange);
  AddTest(tests, "TextViewport/LanguageIdMemoInvalidatesOnPathChange",
          TestTextViewportLanguageIdMemoInvalidatesOnPathChange);
  AddTest(tests, "TextViewport/LanguageIdMemoSurvivesEditsWhenTheExtensionDecides",
          TestTextViewportLanguageIdMemoSurvivesEditsWhenTheExtensionDecides);
  AddTest(tests, "TextViewport/LanguageIdMemoRedetectsOnlyForHeadEdits",
          TestTextViewportLanguageIdMemoRedetectsOnlyForHeadEdits);
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
  AddTest(tests, "TextViewport/AHighlightPrefetchWindowFitsTheCacheAndIsNotAskedTwice",
          TestAHighlightPrefetchWindowFitsTheCacheAndIsNotAskedTwice);
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
  AddTest(tests, "TextViewport/DerivedCacheBytesTracksEachCache",
          TestTextViewportDerivedCacheBytesTracksEachCache);
  AddTest(tests, "TextViewport/HighlightStatesGrowWithTheFrontier",
          TestTextViewportHighlightStatesGrowWithTheFrontier);
  AddTest(tests, "TextViewport/TabSizeRoundTripKeepsTheWidthTable",
          TestTextViewportTabSizeRoundTripKeepsTheWidthTable);
  AddTest(tests, "TextViewport/NoOpRangeReplaceDoesNotDirty",
          TestTextViewportNoOpRangeReplaceDoesNotDirty);
  AddTest(tests, "TextViewport/UndoRedoRoundTripRandomEdits",
          TestTextViewportUndoRedoRoundTripRandomEdits);
  AddTest(tests, "TextViewport/AppliedEditReplaysRandomEdits",
          TestTextViewportAppliedEditReplaysRandomEdits);
  AddTest(tests, "TextViewport/NoOpLineReplaceDoesNotDirty",
          TestTextViewportNoOpLineReplaceDoesNotDirty);
  AddTest(tests, "TextViewport/CenterLinePutsTheLineMidViewportAndClamps",
          TestTextViewportCenterLinePutsTheLineMidViewportAndClamps);
  AddTest(tests, "TextViewport/EmptyLineDeleteIsNotTreatedAsNoOp",
          TestTextViewportEmptyLineDeleteIsNotTreatedAsNoOp);
}

}
