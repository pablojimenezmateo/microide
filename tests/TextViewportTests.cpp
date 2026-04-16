#include "TestSupport.h"

#include "editor/TextViewport.h"

#include <algorithm>
#include <vector>

namespace microide::tests {
namespace {

using microide::editor::SyntaxTokenKind;
using microide::editor::TextViewport;

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
}

}  // namespace microide::tests
