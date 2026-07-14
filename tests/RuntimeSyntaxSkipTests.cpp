#include "TestSupport.h"

#include <string>
#include <string_view>
#include <vector>

#include "editor/RuntimeSyntaxRegistry.h"
#include "editor/SyntaxHighlighter.h"

// Golden/characterization coverage for the `skip`-masking path in
// RuntimeSyntaxRegistry::FindFirstRegex (the region end/start search that masks
// `skip` matches with '\0' before searching). This is the gate the known-tech-debt
// entry required before touching the skip machinery: it snapshots the per-byte
// token output for the tricky cases (escape masking, escaped-backslash, escaped
// quote at EOL, nested regions re-entering a skip region, `^`-anchored skip under
// NOTBOL, and UTF-8 next to an escape) so any change to the masking — including the
// empty-skip fast path — is verified to produce byte-identical highlighting.

namespace microide::tests {
namespace {

using microide::editor::SyntaxTokenKind;
using microide::editor::runtime_syntax::GeneratedRuleKind;
using microide::editor::runtime_syntax::HighlightLine;
using microide::editor::runtime_syntax::ReloadDefinitions;
using microide::editor::runtime_syntax::RuntimeSyntaxDefinitionData;
using microide::editor::runtime_syntax::RuntimeSyntaxRuleData;

// Restores the built-in-only registry (drops the plugin definitions installed by a
// test) when the test scope exits, so one test's grammar never leaks into another.
struct ScopedGoldGrammar {
  ScopedGoldGrammar() { ReloadDefinitions({MakeGoldDefinition()}); }
  ~ScopedGoldGrammar() { ReloadDefinitions({}); }

  static RuntimeSyntaxDefinitionData MakeGoldDefinition() {
    RuntimeSyntaxDefinitionData def;
    def.filetype = "goldskip";
    def.filename_patterns = {"\\.gold$"};
    def.source_path = "goldskip.test";

    // Double-quoted string with `\.` escape skip and a `${ }` interpolation child.
    // The interpolation makes control leave and re-enter the string region mid-line,
    // which is what re-runs the end+skip search over the shrinking tail.
    RuntimeSyntaxRuleData string_rule;
    string_rule.kind = GeneratedRuleKind::Region;
    string_rule.group_name = "constant.string";
    string_rule.limit_group_name = "constant.string";
    string_rule.start_regex = "\"";
    string_rule.end_regex = "\"";
    string_rule.skip_regex = "\\\\.";  // regex: backslash + any char

    RuntimeSyntaxRuleData interp;
    interp.kind = GeneratedRuleKind::Region;
    interp.group_name = "symbol.operator";
    interp.limit_group_name = "symbol.operator";
    interp.start_regex = "\\$\\{";  // regex: ${
    interp.end_regex = "\\}";       // regex: }
    string_rule.children.push_back(interp);
    def.rules.push_back(string_rule);

    // Angle region with a `^`-anchored skip: the skip may only mask a backslash at
    // the very start of the searched segment. Because mid-line segments are searched
    // with PCRE2_NOTBOL, the `^` must NOT assert there, so the backslash stays
    // unmasked. Locks that at_line_start/NOTBOL is threaded through the skip scan.
    RuntimeSyntaxRuleData angle_rule;
    angle_rule.kind = GeneratedRuleKind::Region;
    angle_rule.group_name = "comment";
    angle_rule.limit_group_name = "comment";
    angle_rule.start_regex = "<";
    angle_rule.end_regex = ">";
    angle_rule.skip_regex = "^\\\\";  // regex: ^ then a literal backslash
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
  // The `\"` is masked by the skip so it does not close the string; the final `"`
  // does. All six bytes are one string.
  ExpectKinds("\"a\\\"b\"", "SSSSSS");
}

void TestSkipMasksEscapedBackslashSoNextQuoteCloses() {
  ScopedGoldGrammar grammar;
  // `\\` is an escaped backslash (masked as a unit), so the following `"` is a real
  // close quote — the string is exactly the five bytes.
  ExpectKinds("\"a\\\\\"", "SSSSS");
}

void TestEscapedQuoteAtEndLeavesStringOpen() {
  ScopedGoldGrammar grammar;
  // `"a\"` : the trailing `\"` is masked, so no close quote remains on the line and
  // the string region stays open into the next line.
  const std::string_view line = "\"a\\\"";
  ExpectKinds(line, "SSSS");
  const auto highlighted = Highlight(line);
  Expect(highlighted.end_state.region_depth == 1,
         "an unterminated string (escaped closing quote) must carry the region open");
}

void TestTwoStringsHitEmptySkipFastPath() {
  ScopedGoldGrammar grammar;
  // Neither string nor the gap between them contains an escape, so every end/start
  // search takes the empty-skip fast path. Two strings around a plain `+`.
  ExpectKinds("\"x\"+\"y\"", "SSS.SSS");
}

void TestNestedInterpolationReEntersSkipRegion() {
  ScopedGoldGrammar grammar;
  // The `${b}` interpolation forces control out of the string and back in, which
  // re-runs the end+skip search over the tail after `}` — the exact re-scan the
  // skip fast path optimizes. Coloring must stay: string, operator run, string.
  ExpectKinds("\"a${b}c\"", "SSOOOOSS");
}

void TestNestedInterpolationWithEscapeBeforeAndAfter() {
  ScopedGoldGrammar grammar;
  // Escapes on both sides of an interpolation exercise the masked (non-empty skip)
  // path on re-entry. `"\n${b}\t"` -> string, escape(string), operator, string.
  ExpectKinds("\"\\n${b}\\t\"", "SSSOOOOSSS");
}

void TestCaretAnchoredSkipDoesNotMaskMidLine() {
  ScopedGoldGrammar grammar;
  // `<\>` : inside the angle region the tail `\>` is searched with NOTBOL, so the
  // `^`-anchored skip cannot mask the backslash; the `>` closes the region and all
  // three bytes are comment-colored.
  ExpectKinds("<\\>", "CCC");
}

void TestUtf8NextToEscapeStaysString() {
  ScopedGoldGrammar grammar;
  // A 2-byte é sits right before an escaped quote. Masking must not corrupt the
  // multi-byte codepoint; every byte remains string.
  ExpectKinds("\"\xc3\xa9\\\"x\"", "SSSSSSS");
}

// Stress/liveness guard for the pathological re-scan shape the tech-debt entry
// describes: a single string with many `${}` interpolations makes the string
// region's end+skip search re-run at each `}`. This must still terminate with the
// string correctly closed (no hang, no depth leak) — it is the shape a future O(n)
// rewrite must keep behaving identically.
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

}  // namespace

void RegisterRuntimeSyntaxSkipTests(std::vector<TestCase>& tests) {
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
  AddTest(tests, "RuntimeSyntaxSkip/ManyInterpolationsTerminateWithStringClosed",
          TestManyInterpolationsTerminateWithStringClosed);
}

}  // namespace microide::tests
