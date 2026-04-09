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

}  // namespace

void RegisterTextViewportTests(std::vector<TestCase>& tests) {
  AddTest(tests, "TextViewport/SmallFileKeepsSyntaxHighlighting",
          TestTextViewportSmallFileKeepsSyntaxHighlighting);
  AddTest(tests, "TextViewport/LargeCodeFixtureUsesLargeFileMode",
          TestTextViewportLargeCodeFixtureUsesLargeFileMode);
  AddTest(tests, "TextViewport/LargePlainFixtureUsesLargeFileMode",
          TestTextViewportLargePlainFixtureUsesLargeFileMode);
}

}  // namespace microide::tests
