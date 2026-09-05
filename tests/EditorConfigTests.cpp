#include "TestSupport.h"

#include "project/EditorConfig.h"
#include "util/StringUtil.h"

#include <filesystem>
#include <string>

namespace microide::tests {
namespace {

using microide::project::EditorConfigProperties;
using microide::project::EditorConfigResolver;
using microide::project::ParseEditorConfig;

void TestParsesCommonProperties() {
  const auto file = ParseEditorConfig(R"(root = true

[*]
indent_style = space
indent_size = 2
end_of_line = lf
trim_trailing_whitespace = true
insert_final_newline = true

[*.md]
trim_trailing_whitespace = false
max_line_length = 80
)");

  Expect(file.root, "root = true in the preamble must be captured");
  Expect(file.sections.size() == 2, "two sections should be parsed");

  const EditorConfigProperties& star = file.sections[0].properties;
  Expect(star.soft_tabs.has_value() && *star.soft_tabs, "indent_style = space means soft tabs");
  Expect(star.indent_width.has_value() && *star.indent_width == 2, "indent_size = 2");
  Expect(star.line_ending.has_value() && *star.line_ending == util::LineEnding::LF,
         "end_of_line = lf");
  Expect(star.trim_trailing_whitespace.value_or(false), "trim_trailing_whitespace = true");
  Expect(star.insert_final_newline.value_or(false), "insert_final_newline = true");

  const EditorConfigProperties& markdown = file.sections[1].properties;
  Expect(markdown.trim_trailing_whitespace.has_value() && !*markdown.trim_trailing_whitespace,
         "a later section may turn a boolean back off");
  Expect(markdown.max_line_length.value_or(0) == 80, "max_line_length is parsed");
}

void TestParserIgnoresJunkWithoutFailing() {
  // The EditorConfig spec asks readers to ignore what they do not understand
  // rather than reject the file.
  const auto file = ParseEditorConfig(R"(# a comment
; another comment
not_a_section_line
[*.py
indent_style = tab

[*.py]
unknown_property = whatever
indent_style = tab
indent_size = 4
= novalue
key_with_no_equals
)");

  Expect(file.sections.size() == 1, "the unterminated header must not open a section");
  Expect(file.sections[0].properties.soft_tabs.has_value() &&
             !*file.sections[0].properties.soft_tabs,
         "indent_style = tab means hard tabs");
  Expect(file.sections[0].properties.indent_width.value_or(0) == 4,
         "a valid property after an unknown one is still read");
}

void TestParserIsCaseInsensitiveForKeysAndValues() {
  const auto file = ParseEditorConfig(R"([*]
Indent_Style = SPACE
INDENT_SIZE = 3
End_Of_Line = CRLF
)");
  Expect(file.sections.size() == 1, "one section");
  const EditorConfigProperties& properties = file.sections[0].properties;
  Expect(properties.soft_tabs.value_or(false), "key and value casing must not matter");
  Expect(properties.indent_width.value_or(0) == 3, "uppercase key must still parse");
  Expect(properties.line_ending.has_value() && *properties.line_ending == util::LineEnding::CRLF,
         "uppercase enum value must still parse");
}

void TestResolverAppliesNearestConfigAndSectionOrder() {
  TemporaryDirectory temp;
  const std::filesystem::path root = temp.path() / "proj";
  std::filesystem::create_directories(root / "vendor" / "lib");

  WriteFile(root / ".editorconfig", R"(root = true

[*]
indent_style = space
indent_size = 4

[*.py]
indent_size = 2
)");
  // A nearer .editorconfig overrides the outer one for files beneath it.
  WriteFile(root / "vendor" / ".editorconfig", R"([*]
indent_style = tab
indent_size = 8
)");

  WriteFile(root / "a.py", "x = 1\n");
  WriteFile(root / "a.c", "int main(void){return 0;}\n");
  WriteFile(root / "vendor" / "lib" / "b.c", "int f(void){return 0;}\n");

  EditorConfigResolver resolver;
  resolver.SetProjectRoot(root);

  const EditorConfigProperties& python = resolver.Resolve(root / "a.py");
  Expect(python.soft_tabs.value_or(false), "[*] indent_style applies to a .py file");
  Expect(python.indent_width.value_or(0) == 2,
         "the later [*.py] section must override the earlier [*] indent_size");

  const EditorConfigProperties& c_file = resolver.Resolve(root / "a.c");
  Expect(c_file.indent_width.value_or(0) == 4, "[*.py] must not apply to a .c file");

  const EditorConfigProperties& nested = resolver.Resolve(root / "vendor" / "lib" / "b.c");
  Expect(nested.soft_tabs.has_value() && !*nested.soft_tabs,
         "the nearer vendor/.editorconfig must win on indent_style");
  Expect(nested.indent_width.value_or(0) == 8,
         "the nearer vendor/.editorconfig must win on indent_size");

  Expect(resolver.FoundAnyConfig(), "the resolver should report that it found configs");
}

// Checked against the reference `editorconfig` library over random trees: two
// spec rules were missing. `indent_style = tab` with no indent_size means the
// size follows tab_width (the spec's implicit `indent_size = tab`), and
// `max_line_length = off` in a later section must override an earlier limit
// instead of being dropped at parse time.
void TestResolverDerivesTabIndentSizeAndHonoursMaxLineLengthOff() {
  TemporaryDirectory temp;
  const std::filesystem::path root = temp.path() / "proj";
  std::filesystem::create_directories(root / "sub");
  WriteFile(root / ".editorconfig", R"(root = true

[*]
indent_style = tab
tab_width = 3
max_line_length = 80

[sub/*]
max_line_length = off
)");
  WriteFile(root / "a.c", "\n");
  WriteFile(root / "sub" / "b.c", "\n");

  EditorConfigResolver resolver;
  resolver.SetProjectRoot(root);
  const EditorConfigProperties& top = resolver.Resolve(root / "a.c");
  Expect(top.soft_tabs.has_value() && !*top.soft_tabs, "tab style applies");
  Expect(top.indent_width.value_or(0) == 3, "indent_size follows tab_width for a tab-indented section");
  Expect(top.max_line_length.value_or(0) == 80, "the limit applies at the top");
  const EditorConfigProperties& nested = resolver.Resolve(root / "sub" / "b.c");
  Expect(!nested.max_line_length.has_value(), "a later `off` switches the limit off");
  Expect(nested.indent_width.value_or(0) == 3, "the derived size still applies below");
}

void TestResolverStopsAtRootTrueAndProjectRoot() {
  TemporaryDirectory temp;
  const std::filesystem::path outside = temp.path();
  const std::filesystem::path root = outside / "proj";
  std::filesystem::create_directories(root / "src");

  // A .editorconfig ABOVE the project root must never be consulted: opening a
  // repo should not let an unrelated parent directory reconfigure it.
  WriteFile(outside / ".editorconfig", R"([*]
indent_style = tab
indent_size = 7
)");
  WriteFile(root / "src" / "a.c", "int f(void){return 0;}\n");

  EditorConfigResolver resolver;
  resolver.SetProjectRoot(root);
  Expect(!resolver.Resolve(root / "src" / "a.c").any(),
         "a .editorconfig outside the project root must not apply");

  // With root = true partway down, the walk stops there.
  WriteFile(root / ".editorconfig", R"([*]
indent_size = 4
)");
  WriteFile(root / "src" / ".editorconfig", R"(root = true

[*]
indent_style = space
)");
  resolver.Invalidate();
  const EditorConfigProperties& resolved = resolver.Resolve(root / "src" / "a.c");
  Expect(resolved.soft_tabs.value_or(false), "the root=true file applies");
  Expect(!resolved.indent_width.has_value(),
         "root = true must stop the walk before the parent's indent_size");
}

void TestResolverHandlesGlobShapes() {
  TemporaryDirectory temp;
  const std::filesystem::path root = temp.path() / "proj";
  std::filesystem::create_directories(root / "deep" / "nested");

  WriteFile(root / ".editorconfig", R"(root = true

[*.{js,ts}]
indent_size = 2

[/toplevel.c]
indent_size = 9

[lib/**.c]
indent_size = 5

[exact.py]
indent_size = 7
)");

  EditorConfigResolver resolver;
  resolver.SetProjectRoot(root);

  // A pattern with no '/' matches at any depth.
  Expect(resolver.Resolve(root / "deep" / "nested" / "a.ts").indent_width.value_or(0) == 2,
         "brace alternation must match at any depth");
  Expect(resolver.Resolve(root / "a.js").indent_width.value_or(0) == 2,
         "brace alternation must match at the top level");
  Expect(!resolver.Resolve(root / "a.rb").indent_width.has_value(),
         "an unlisted extension must not match");

  // A leading '/' anchors to the .editorconfig's own directory.
  Expect(resolver.Resolve(root / "toplevel.c").indent_width.value_or(0) == 9,
         "a leading-slash pattern must match at the config's directory");
  Expect(!resolver.Resolve(root / "deep" / "toplevel.c").indent_width.has_value(),
         "a leading-slash pattern must not match deeper");

  // A slash-free header names a basename in any directory, not a suffix of one:
  // "**/exact.py" used to restart its '**' one byte at a time and so applied
  // to "inexact.py".
  Expect(resolver.Resolve(root / "deep" / "exact.py").indent_width.value_or(0) == 7,
         "a slash-free header matches its basename at any depth");
  Expect(!resolver.Resolve(root / "deep" / "inexact.py").indent_width.has_value(),
         "a slash-free header must not match a longer basename ending in it");

  // A pattern containing '/' is anchored too.
  Expect(resolver.Resolve(root / "lib" / "deep" / "x.c").indent_width.value_or(0) == 5,
         "an anchored '**' pattern must match beneath its directory");
  Expect(!resolver.Resolve(root / "other" / "x.c").indent_width.has_value(),
         "an anchored pattern must not match a sibling directory");
}

void TestResolverAppliesIndentSizeTabAndTabWidthDefault() {
  TemporaryDirectory temp;
  const std::filesystem::path root = temp.path() / "proj";
  std::filesystem::create_directories(root);

  WriteFile(root / ".editorconfig", R"(root = true

[follows.c]
indent_style = tab
indent_size = tab
tab_width = 8

[defaults.c]
indent_size = 3
)");

  EditorConfigResolver resolver;
  resolver.SetProjectRoot(root);

  const EditorConfigProperties& follows = resolver.Resolve(root / "follows.c");
  Expect(follows.indent_width.value_or(0) == 8,
         "indent_size = tab must resolve to tab_width");
  Expect(follows.tab_size.value_or(0) == 8, "tab_width is carried through");

  const EditorConfigProperties& defaults = resolver.Resolve(root / "defaults.c");
  Expect(defaults.indent_width.value_or(0) == 3, "indent_size is read");
  Expect(defaults.tab_size.value_or(0) == 3,
         "tab_width must default to indent_size when unset");
}

void TestResolverMemoizesAndInvalidates() {
  TemporaryDirectory temp;
  const std::filesystem::path root = temp.path() / "proj";
  std::filesystem::create_directories(root / "a" / "b");
  WriteFile(root / ".editorconfig", "root = true\n\n[*]\nindent_size = 4\n");

  EditorConfigResolver resolver;
  resolver.SetProjectRoot(root);
  Expect(resolver.CachedPathCountForTesting() == 0, "nothing is cached before the first resolve");

  resolver.Resolve(root / "a" / "b" / "x.c");
  const std::size_t after_first = resolver.CachedPathCountForTesting();
  Expect(after_first == 1, "one path result should be memoized");
  Expect(resolver.CachedDirectoryCountForTesting() >= 3,
         "every directory on the walk should be memoized, not re-stat'd");

  resolver.Resolve(root / "a" / "b" / "x.c");
  Expect(resolver.CachedPathCountForTesting() == after_first,
         "a repeat resolve must hit the memo rather than add an entry");

  // A changed .editorconfig must be picked up after invalidation.
  WriteFile(root / ".editorconfig", "root = true\n\n[*]\nindent_size = 6\n");
  Expect(resolver.Resolve(root / "a" / "b" / "x.c").indent_width.value_or(0) == 4,
         "without invalidation the memoized value stands");
  resolver.Invalidate();
  Expect(resolver.CachedPathCountForTesting() == 0, "invalidation clears the memo");
  Expect(resolver.Resolve(root / "a" / "b" / "x.c").indent_width.value_or(0) == 6,
         "after invalidation the new on-disk value is read");
}

void TestResolverIsFreeWithoutAnyConfig() {
  TemporaryDirectory temp;
  const std::filesystem::path root = temp.path() / "proj";
  std::filesystem::create_directories(root / "src");

  EditorConfigResolver resolver;
  resolver.SetProjectRoot(root);
  Expect(!resolver.Resolve(root / "src" / "a.c").any(),
         "a project with no .editorconfig resolves to no opinion");
  Expect(!resolver.FoundAnyConfig(),
         "FoundAnyConfig must stay false so callers can skip re-applying preferences");
  // The negative result is memoized too, so the second lookup does no filesystem work.
  Expect(resolver.CachedPathCountForTesting() == 1, "the empty result is memoized");
}

void TestResolverBoundsSectionCount() {
  std::string text = "root = true\n";
  for (std::size_t index = 0; index < microide::project::kMaxEditorConfigSections + 50; ++index) {
    text += "[*.x" + std::to_string(index) + "]\nindent_size = 2\n";
  }
  // The sections past the cap carry a property the kept ones do not; it must
  // not land on the last kept section.
  text += "[*.overflow]\ntab_width = 9\n";
  const auto file = ParseEditorConfig(text);
  Expect(file.sections.size() <= microide::project::kMaxEditorConfigSections,
         "a pathological section count must be capped");
  Expect(!file.sections.back().properties.tab_size.has_value(),
         "a dropped section's properties do not leak into the last kept section");
}

// `[]` (and a header the cap dropped) opens a section that matches nothing; the
// properties under it belong to it and must not be folded into the previous one.
void TestParserDoesNotLeakAnEmptySectionsPropertiesIntoThePrevious() {
  const auto file = ParseEditorConfig("[*.py]\nindent_size = 4\n[]\nindent_size = 8\n[*.js]\nindent_size = 2\n");
  Expect(file.sections.size() == 2, "the empty header is not a section");
  Expect(file.sections[0].properties.indent_width == 4,
         "the property under `[]` did not overwrite the previous section's");
  Expect(file.sections[1].properties.indent_width == 2, "the following section is unaffected");
}

}  // namespace

void RegisterEditorConfigTests(std::vector<TestCase>& tests) {
  AddTest(tests, "EditorConfig/ParsesCommonProperties", TestParsesCommonProperties);
  AddTest(tests, "EditorConfig/ResolverDerivesTabIndentSizeAndHonoursMaxLineLengthOff",
          TestResolverDerivesTabIndentSizeAndHonoursMaxLineLengthOff);
  AddTest(tests, "EditorConfig/ParserIgnoresJunkWithoutFailing",
          TestParserIgnoresJunkWithoutFailing);
  AddTest(tests, "EditorConfig/ParserIsCaseInsensitiveForKeysAndValues",
          TestParserIsCaseInsensitiveForKeysAndValues);
  AddTest(tests, "EditorConfig/ResolverAppliesNearestConfigAndSectionOrder",
          TestResolverAppliesNearestConfigAndSectionOrder);
  AddTest(tests, "EditorConfig/ResolverStopsAtRootTrueAndProjectRoot",
          TestResolverStopsAtRootTrueAndProjectRoot);
  AddTest(tests, "EditorConfig/ResolverHandlesGlobShapes", TestResolverHandlesGlobShapes);
  AddTest(tests, "EditorConfig/ResolverAppliesIndentSizeTabAndTabWidthDefault",
          TestResolverAppliesIndentSizeTabAndTabWidthDefault);
  AddTest(tests, "EditorConfig/ResolverMemoizesAndInvalidates",
          TestResolverMemoizesAndInvalidates);
  AddTest(tests, "EditorConfig/ResolverIsFreeWithoutAnyConfig",
          TestResolverIsFreeWithoutAnyConfig);
  AddTest(tests, "EditorConfig/ResolverBoundsSectionCount", TestResolverBoundsSectionCount);
  AddTest(tests, "EditorConfig/ParserDoesNotLeakAnEmptySectionsPropertiesIntoThePrevious",
          TestParserDoesNotLeakAnEmptySectionsPropertiesIntoThePrevious);
}

}  // namespace microide::tests
