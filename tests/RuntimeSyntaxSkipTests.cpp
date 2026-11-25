#include "TestSupport.h"

#include <cstdint>
#include <filesystem>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

#include "editor/RuntimeSyntaxRegistry.h"
#include "editor/SyntaxHighlighter.h"

namespace microide::tests {
namespace {

using microide::editor::SyntaxTokenKind;
using microide::editor::runtime_syntax::GeneratedRuleKind;
using microide::editor::runtime_syntax::HighlightLine;
using microide::editor::runtime_syntax::ReloadDefinitions;
using microide::editor::runtime_syntax::RuntimeSyntaxDefinitionData;
using microide::editor::runtime_syntax::RuntimeSyntaxRuleData;

struct ScopedGoldGrammar {
  ScopedGoldGrammar() { ReloadDefinitions({MakeGoldDefinition()}); }
  ~ScopedGoldGrammar() { ReloadDefinitions({}); }

  static RuntimeSyntaxDefinitionData MakeGoldDefinition() {
    RuntimeSyntaxDefinitionData def;
    def.filetype = "goldskip";
    def.filename_patterns = {"\\.gold$"};
    def.source_path = "goldskip.test";

    RuntimeSyntaxRuleData string_rule;
    string_rule.kind = GeneratedRuleKind::Region;
    string_rule.group_name = "constant.string";
    string_rule.limit_group_name = "constant.string";
    string_rule.start_regex = "\"";
    string_rule.end_regex = "\"";
    string_rule.skip_regex = "\\\\.";

    RuntimeSyntaxRuleData interp;
    interp.kind = GeneratedRuleKind::Region;
    interp.group_name = "symbol.operator";
    interp.limit_group_name = "symbol.operator";
    interp.start_regex = "\\$\\{";
    interp.end_regex = "\\}";
    string_rule.children.push_back(interp);
    def.rules.push_back(string_rule);

    RuntimeSyntaxRuleData angle_rule;
    angle_rule.kind = GeneratedRuleKind::Region;
    angle_rule.group_name = "comment";
    angle_rule.limit_group_name = "comment";
    angle_rule.start_regex = "<";
    angle_rule.end_regex = ">";
    angle_rule.skip_regex = "^\\\\";
    def.rules.push_back(angle_rule);

    return def;
  }
};

char KindChar(SyntaxTokenKind kind) {
  switch (kind) {
    case SyntaxTokenKind::Plain:
      return '.';
    case SyntaxTokenKind::Keyword:
      return 'K';
    case SyntaxTokenKind::Type:
      return 'T';
    case SyntaxTokenKind::String:
      return 'S';
    case SyntaxTokenKind::Comment:
      return 'C';
    case SyntaxTokenKind::Number:
      return 'N';
    case SyntaxTokenKind::Constant:
      return 'c';
    case SyntaxTokenKind::Preprocessor:
      return 'P';
    case SyntaxTokenKind::Operator:
      return 'O';
  }
  return '?';
}

microide::editor::HighlightedLine Highlight(std::string_view line) {
  return HighlightLine(line, "buffer.gold", {}, "");
}

std::string RenderKinds(std::string_view line) {
  const auto highlighted = Highlight(line);
  std::string rendered;
  rendered.reserve(highlighted.tokens.size());
  for (const SyntaxTokenKind kind : highlighted.tokens) {
    rendered.push_back(KindChar(kind));
  }
  return rendered;
}

void ExpectKinds(std::string_view line, std::string_view expected) {
  const std::string rendered = RenderKinds(line);
  Expect(rendered == expected,
         std::string("skip-masking golden mismatch for [") + std::string(line) + "]: got '" +
             rendered + "' want '" + std::string(expected) + "'");
}

void TestSkipMasksEscapedQuoteInsideString() {
  ScopedGoldGrammar grammar;
  ExpectKinds("\"a\\\"b\"", "SSSSSS");
}

void TestSkipMasksEscapedBackslashSoNextQuoteCloses() {
  ScopedGoldGrammar grammar;
  ExpectKinds("\"a\\\\\"", "SSSSS");
}

void TestEscapedQuoteAtEndLeavesStringOpen() {
  ScopedGoldGrammar grammar;
  const std::string_view line = "\"a\\\"";
  ExpectKinds(line, "SSSS");
  const auto highlighted = Highlight(line);
  Expect(highlighted.end_state.region_depth == 1,
         "an unterminated string (escaped closing quote) must carry the region open");
}

void TestTwoStringsHitEmptySkipFastPath() {
  ScopedGoldGrammar grammar;
  ExpectKinds("\"x\"+\"y\"", "SSS.SSS");
}

void TestNestedInterpolationReEntersSkipRegion() {
  ScopedGoldGrammar grammar;
  ExpectKinds("\"a${b}c\"", "SSOOOOSS");
}

void TestNestedInterpolationWithEscapeBeforeAndAfter() {
  ScopedGoldGrammar grammar;
  ExpectKinds("\"\\n${b}\\t\"", "SSSOOOOSSS");
}

void TestCaretAnchoredSkipDoesNotMaskMidLine() {
  ScopedGoldGrammar grammar;
  ExpectKinds("<\\>", "CCC");
}

void TestUtf8NextToEscapeStaysString() {
  ScopedGoldGrammar grammar;
  ExpectKinds("\"\xc3\xa9\\\"x\"", "SSSSSSS");
}

void TestInvalidUtf8LineStillHighlights() {
  ScopedGoldGrammar grammar;
  const std::string line = "\"a\xe9?b\"";
  const std::string rendered = RenderKinds(line);
  Expect(rendered.size() == line.size(),
         "every byte of an invalid-UTF-8 line must receive a token kind");
  Expect(rendered == std::string(line.size(), 'S'),
         std::string("an invalid UTF-8 byte must not disable highlighting for the whole "
                     "line: got '") + rendered + "'");
}

void TestOverLongLineSkipsTokenMaterialization() {
  ScopedGoldGrammar grammar;
  const std::string under = "\"" + std::string(90000, 'a') + "\"";
  const auto under_result = Highlight(under);
  Expect(under_result.tokens.size() == under.size(),
         "a line under the tokenization cutoff must still carry one token per byte");
  Expect(under_result.tokens.front() == SyntaxTokenKind::String,
         "a line under the cutoff must still be highlighted");

  const std::string over = "\"" + std::string(150000, 'a') + "\"";
  const auto over_result = Highlight(over);
  Expect(over_result.tokens.empty(),
         "a line past the tokenization cutoff must not materialize a token per byte");

  microide::editor::SyntaxState open_state;
  open_state.definition_id = under_result.end_state.definition_id;
  open_state.region_depth = 1;
  open_state.region_stack[0] = 7;
  const auto carried = HighlightLine(over, "buffer.gold", open_state, "");
  Expect(carried.end_state.region_depth == 1 && carried.end_state.region_stack[0] == 7,
         "an over-long line must carry the incoming open-region stack forward");
}

void TestManyInterpolationsTerminateWithStringClosed() {
  ScopedGoldGrammar grammar;
  std::string line = "\"";
  constexpr int kInterpolations = 1500;
  for (int i = 0; i < kInterpolations; ++i) {
    line += "${0}";
  }
  line += "\"";

  const auto highlighted = Highlight(line);
  Expect(highlighted.tokens.size() == line.size(),
         "every byte of the stress line must receive a token kind");
  Expect(highlighted.end_state.region_depth == 0,
         "the string must close on the same line (no leaked open region)");
  Expect(!highlighted.tokens.empty() && highlighted.tokens.back() == SyntaxTokenKind::String,
         "the trailing close quote must be highlighted as string");
  Expect(highlighted.tokens.front() == SyntaxTokenKind::String,
         "the leading open quote must be highlighted as string");
}

void TestMatchDataCacheInvalidatesOnReload() {
  using microide::editor::runtime_syntax::RegistryRevision;

  const std::size_t rev0 = RegistryRevision();
  ReloadDefinitions({ScopedGoldGrammar::MakeGoldDefinition()});
  const std::size_t rev1 = RegistryRevision();
  Expect(rev1 != rev0, "reloading definitions must advance the registry revision");

  Expect(RenderKinds("\"a\\\"b\"") == "SSSSSS", "gold grammar highlights the string");

  ReloadDefinitions({ScopedGoldGrammar::MakeGoldDefinition()});
  const std::size_t rev2 = RegistryRevision();
  Expect(rev2 != rev1, "a second reload must advance the revision again");
  Expect(RenderKinds("\"a\\\"b\"") == "SSSSSS",
         "highlighting stays correct after the reload (no stale match-data reuse)");

  ReloadDefinitions({});
}

struct ScopedWordBoundaryGrammar {
  ScopedWordBoundaryGrammar() { ReloadDefinitions({MakeDefinition()}); }
  ~ScopedWordBoundaryGrammar() { ReloadDefinitions({}); }

  static RuntimeSyntaxDefinitionData MakeDefinition() {
    RuntimeSyntaxDefinitionData def;
    def.filetype = "goldword";
    def.filename_patterns = {"\\.goldword$"};
    def.source_path = "goldword.test";

    RuntimeSyntaxRuleData keyword;
    keyword.kind = GeneratedRuleKind::Pattern;
    keyword.group_name = "statement";
    keyword.pattern = "\\b(int)\\b";
    def.rules.push_back(keyword);
    return def;
  }
};

std::string RenderWordKinds(std::string_view line) {
  const auto highlighted = HighlightLine(line, "buffer.goldword", {}, "");
  std::string rendered;
  rendered.reserve(highlighted.tokens.size());
  for (const SyntaxTokenKind kind : highlighted.tokens) {
    rendered.push_back(KindChar(kind));
  }
  return rendered;
}

void TestAsciiAndUnicodeCompilationsAgreeOnAsciiInput() {
  ScopedWordBoundaryGrammar grammar;
  Expect(RenderWordKinds("int x") == "KKK..",
         "a standalone ASCII keyword is highlighted on the byte-mode path");
  Expect(RenderWordKinds("intptr x") == "........",
         "an ASCII keyword embedded in a longer word is not highlighted");
}

void TestNonAsciiLineKeepsUnicodeWordBoundaries() {
  ScopedWordBoundaryGrammar grammar;
  const std::string accented = "int\xc3\xa9gr\xc3\xa9 x";
  const std::string rendered = RenderWordKinds(accented);
  Expect(rendered.find('K') == std::string::npos,
         std::string("a keyword inside a non-ASCII identifier must stay unhighlighted, got '") +
             rendered + "'");
  Expect(RenderWordKinds("int") == "KKK", "the keyword rule fires on a bare keyword line");
}

}


void TestSyntaxHighlighterRandomLinesKeepPerByteCoverage() {
  using microide::editor::SyntaxHighlighter;
  using microide::editor::SyntaxState;
  static constexpr const char* kExtensions[] = {".cpp", ".h", ".c", ".py", ".js", ".ts",
                                                ".rs", ".go", ".lua", ".md", ".json", ".sh",
                                                ".html", ".css", ".java", ".yaml", ".toml",
                                                ".txt", ".rb", ".php"};
  static constexpr const char* kAtoms[] = {"a", "b", " ", "\"", "'", "/", "*", "#", "{", "}", "(", ")", "\\", "`", "<", ">", "-", "=", "é", "中", "0", "\t", "//", "/*", "*/", "\"\"\"", "'''"};
  std::uint64_t state = 0x5DEECE66Dull;
  const auto next = [&state](std::uint64_t bound) {
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    return bound == 0 ? 0 : static_cast<std::size_t>(state % bound);
  };
  std::size_t checked = 0;
  for (const char* extension : kExtensions) {
    const std::filesystem::path path = std::string("/tmp/random") + extension;
    SyntaxState carried;
    for (int line_index = 0; line_index < 120; ++line_index) {
      std::string line;
      const std::size_t atoms = next(14);
      for (std::size_t i = 0; i < atoms; ++i) {
        line += kAtoms[next(std::size(kAtoms))];
      }
      const auto first = SyntaxHighlighter::HighlightLine(line, path, carried);
      const auto second = SyntaxHighlighter::HighlightLine(line, path, carried);
      const SyntaxState advanced = SyntaxHighlighter::AdvanceState(line, path, carried);
      Expect(first.tokens.size() == line.size(),
             (std::string(extension) + ": one token kind per byte of [" + line + "] (got " +
              std::to_string(first.tokens.size()) + " for " + std::to_string(line.size()) + ")")
                 .c_str());
      Expect(first.tokens == second.tokens && first.end_state == second.end_state,
             (std::string(extension) + ": highlighting is deterministic for [" + line + "]").c_str());
      Expect(advanced == first.end_state,
             (std::string(extension) + ": AdvanceState agrees with the full highlight for [" +
              line + "]")
                 .c_str());
      carried = first.end_state;
      ++checked;
    }
  }
  Expect(checked == std::size(kExtensions) * 120, "every language was exercised");
}

void RegisterRuntimeSyntaxSkipTests(std::vector<TestCase>& tests) {
  AddTest(tests, "RuntimeSyntaxSkip/RandomLinesKeepPerByteCoverage",
          TestSyntaxHighlighterRandomLinesKeepPerByteCoverage);
  AddTest(tests, "RuntimeSyntaxSkip/AsciiAndUnicodeCompilationsAgreeOnAsciiInput",
          TestAsciiAndUnicodeCompilationsAgreeOnAsciiInput);
  AddTest(tests, "RuntimeSyntaxSkip/NonAsciiLineKeepsUnicodeWordBoundaries",
          TestNonAsciiLineKeepsUnicodeWordBoundaries);
  AddTest(tests, "RuntimeSyntaxSkip/MatchDataCacheInvalidatesOnReload",
          TestMatchDataCacheInvalidatesOnReload);
  AddTest(tests, "RuntimeSyntaxSkip/MasksEscapedQuoteInsideString",
          TestSkipMasksEscapedQuoteInsideString);
  AddTest(tests, "RuntimeSyntaxSkip/MasksEscapedBackslashSoNextQuoteCloses",
          TestSkipMasksEscapedBackslashSoNextQuoteCloses);
  AddTest(tests, "RuntimeSyntaxSkip/EscapedQuoteAtEndLeavesStringOpen",
          TestEscapedQuoteAtEndLeavesStringOpen);
  AddTest(tests, "RuntimeSyntaxSkip/TwoStringsHitEmptySkipFastPath",
          TestTwoStringsHitEmptySkipFastPath);
  AddTest(tests, "RuntimeSyntaxSkip/NestedInterpolationReEntersSkipRegion",
          TestNestedInterpolationReEntersSkipRegion);
  AddTest(tests, "RuntimeSyntaxSkip/NestedInterpolationWithEscapeBeforeAndAfter",
          TestNestedInterpolationWithEscapeBeforeAndAfter);
  AddTest(tests, "RuntimeSyntaxSkip/CaretAnchoredSkipDoesNotMaskMidLine",
          TestCaretAnchoredSkipDoesNotMaskMidLine);
  AddTest(tests, "RuntimeSyntaxSkip/Utf8NextToEscapeStaysString",
          TestUtf8NextToEscapeStaysString);
  AddTest(tests, "RuntimeSyntaxSkip/InvalidUtf8LineStillHighlights",
          TestInvalidUtf8LineStillHighlights);
  AddTest(tests, "RuntimeSyntaxSkip/OverLongLineSkipsTokenMaterialization",
          TestOverLongLineSkipsTokenMaterialization);
  AddTest(tests, "RuntimeSyntaxSkip/ManyInterpolationsTerminateWithStringClosed",
          TestManyInterpolationsTerminateWithStringClosed);
}

}
