#include "TestSupport.h"

#include "editor/SyntaxDefinitionLoader.h"
#include "editor/RuntimeSyntaxRegistry.h"
#include "editor/TextViewport.h"

#include <algorithm>
#include <vector>

namespace microide::tests {
namespace {

using microide::editor::SyntaxTokenKind;
using microide::editor::TextViewport;

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
  AddTest(tests, "TextViewport/UndoRedoPreservesLatestViewState",
          TestTextViewportUndoRedoPreservesLatestViewState);
  AddTest(tests, "TextViewport/ReplaceLinesAppendMovesCursorToInsertedBlock",
          TestTextViewportReplaceLinesAppendMovesCursorToInsertedBlock);
  AddTest(tests, "TextViewport/MaxVisualColumnsUpdatesIncrementally",
          TestTextViewportMaxVisualColumnsUpdatesIncrementally);
  AddTest(tests, "TextViewport/LoadsRuntimeSyntaxDefinitionsFromPluginDataDirectories",
          TestTextViewportLoadsRuntimeSyntaxDefinitionsFromPluginDataDirectories);
}

}  // namespace microide::tests
