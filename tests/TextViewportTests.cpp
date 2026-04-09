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

  Expect(!viewport.large_file_mode(), "small files should not enter large file mode");
  Expect(viewport.syntax_highlighting_enabled(),
         "small files should keep syntax highlighting enabled");

  const auto& tokens = viewport.HighlightedLineTokens(0);
  Expect(!tokens.empty(), "small files should still produce syntax tokens");
  const bool saw_non_plain =
      std::any_of(tokens.begin(), tokens.end(),
                  [](SyntaxTokenKind kind) { return kind != SyntaxTokenKind::Plain; });
  Expect(saw_non_plain, "small C++ files should preserve non-plain syntax tokens");
}

void TestTextViewportLargeCodeFixtureUsesLargeFileMode() {
  TextViewport viewport;
  Expect(viewport.OpenFile(FixturePath("large/code/large_sample.cpp")),
         "large code fixture should open");

  Expect(viewport.large_file_mode(),
         "large line-count fixtures should enter explicit large file mode");
  Expect(!viewport.syntax_highlighting_enabled(),
         "large file mode should disable syntax highlighting");
  Expect(viewport.HighlightedLineTokens(0).empty(),
         "large file mode should stop producing per-line syntax tokens");
}

void TestTextViewportLargePlainFixtureUsesLargeFileMode() {
  TextViewport viewport;
  Expect(viewport.OpenFile(FixturePath("large/plain/large_story.txt")),
         "large plain-text fixture should open");

  Expect(viewport.large_file_mode(),
         "large byte-size fixtures should enter explicit large file mode");
  Expect(!viewport.syntax_highlighting_enabled(),
         "large byte-size fixtures should disable syntax highlighting");
  Expect(viewport.HighlightedLineTokens(0).empty(),
         "large byte-size fixtures should skip syntax token generation");
}

void TestTextViewportEditingAcrossLargeFileLineThresholdReevaluatesMode() {
  TextViewport viewport;
  std::string content;
  for (int i = 0; i < 3999; ++i) {
    if (!content.empty()) {
      content.push_back('\n');
    }
    content += "int value = 42;";
  }
  viewport.LoadContent(content, "/tmp/threshold.cpp");

  Expect(!viewport.large_file_mode(),
         "content just below the large-file line threshold should stay in normal mode");
  Expect(viewport.syntax_highlighting_enabled(),
         "content just below the line threshold should keep syntax highlighting");

  viewport.MoveCursorTo(viewport.line_count() - 1, viewport.lines().back().size());
  viewport.InsertNewline();

  Expect(viewport.large_file_mode(),
         "editing across the line threshold should enter large-file mode immediately");
  Expect(!viewport.syntax_highlighting_enabled(),
         "editing across the line threshold should disable syntax highlighting");
  Expect(viewport.HighlightedLineTokens(0).empty(),
         "editing across the line threshold should stop producing syntax tokens");

  Expect(viewport.Undo(), "undo should succeed after crossing the line threshold");
  Expect(!viewport.large_file_mode(),
         "undo below the line threshold should leave large-file mode");
  Expect(viewport.syntax_highlighting_enabled(),
         "undo below the line threshold should restore syntax highlighting");
  Expect(!viewport.HighlightedLineTokens(0).empty(),
         "undo below the line threshold should restore syntax tokens");
}

void TestTextViewportEditingAcrossLargeFileByteThresholdReevaluatesMode() {
  TextViewport viewport;
  viewport.LoadContent("int value = 42;\n", "/tmp/threshold.cpp");

  Expect(!viewport.large_file_mode(),
         "small content should start outside large-file mode before byte-threshold growth");

  viewport.MoveCursorTo(0, viewport.lines().front().size());
  viewport.InsertText(std::string(400000, 'a'));

  Expect(viewport.large_file_mode(),
         "editing across the byte threshold should enter large-file mode immediately");
  Expect(!viewport.syntax_highlighting_enabled(),
         "editing across the byte threshold should disable syntax highlighting");
  Expect(viewport.HighlightedLineTokens(0).empty(),
         "editing across the byte threshold should stop producing syntax tokens");

  Expect(viewport.Undo(), "undo should succeed after crossing the byte threshold");
  Expect(!viewport.large_file_mode(),
         "undo below the byte threshold should leave large-file mode");
  Expect(viewport.syntax_highlighting_enabled(),
         "undo below the byte threshold should restore syntax highlighting");
  Expect(!viewport.HighlightedLineTokens(0).empty(),
         "undo below the byte threshold should restore syntax tokens");
}

}  // namespace

void RegisterTextViewportTests(std::vector<TestCase>& tests) {
  AddTest(tests, "TextViewport/SmallFileKeepsSyntaxHighlighting",
          TestTextViewportSmallFileKeepsSyntaxHighlighting);
  AddTest(tests, "TextViewport/LargeCodeFixtureUsesLargeFileMode",
          TestTextViewportLargeCodeFixtureUsesLargeFileMode);
  AddTest(tests, "TextViewport/LargePlainFixtureUsesLargeFileMode",
          TestTextViewportLargePlainFixtureUsesLargeFileMode);
  AddTest(tests, "TextViewport/EditingAcrossLargeFileLineThresholdReevaluatesMode",
          TestTextViewportEditingAcrossLargeFileLineThresholdReevaluatesMode);
  AddTest(tests, "TextViewport/EditingAcrossLargeFileByteThresholdReevaluatesMode",
          TestTextViewportEditingAcrossLargeFileByteThresholdReevaluatesMode);
}

}  // namespace microide::tests
