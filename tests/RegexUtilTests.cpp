#include "TestSupport.h"

#include "util/RegexUtil.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace microide::tests {
namespace {

void TestRegexUtilCompilesAndCapturesMatchRanges() {
  microide::util::CompiledRegex regex("alp.a", PCRE2_CASELESS);
  Expect(regex.valid(), "regex util should compile valid patterns");

  auto match_data = regex.CreateMatchData();
  Expect(match_data.valid(), "regex util should create match data for compiled patterns");

  const int rc = regex.Match("Alpha alpha", 0, match_data);
  Expect(rc >= 0, "regex util should match valid input");

  microide::util::RegexMatchRange range;
  Expect(regex.CaptureRange(match_data, std::string_view("Alpha alpha").size(), &range),
         "regex util should expose capture ranges");
  Expect(range.start == 0 && range.end == 5,
         "regex util should report the first capture range correctly");
}

void TestRegexUtilBuildsPrefixedErrorMessages() {
  microide::util::CompiledRegex regex("[alpha", 0, "Invalid project search pattern");
  Expect(!regex.valid(), "regex util should reject invalid patterns");
  Expect(regex.error().find("Invalid project search pattern at offset ") == 0,
         "regex util should preserve the caller-supplied error prefix");
}

void TestRegexUtilCopiesCompiledPatterns() {
  microide::util::CompiledRegex regex("beta", 0);
  Expect(regex.valid(), "copy fixture should compile a valid regex");

  microide::util::CompiledRegex copy = regex;
  auto match_data = copy.CreateMatchData();
  Expect(match_data.valid(), "copied compiled regexes should create match data");

  const int rc = copy.Match("alpha beta gamma", 0, match_data);
  Expect(rc >= 0, "copied compiled regexes should match input");

  microide::util::RegexMatchRange range;
  Expect(copy.CaptureRange(match_data, std::string_view("alpha beta gamma").size(), &range),
         "copied compiled regexes should expose capture ranges");
  Expect(range.start == 6 && range.end == 10,
         "copied compiled regexes should preserve the compiled pattern");
}

// A catastrophic-backtracking pattern must fail fast against the match/depth
// limits instead of spinning near PCRE2's default ceiling. Without the match
// context this call would explore ~2^N paths and hang the search worker.
void TestRegexUtilBoundsCatastrophicBacktracking() {
  microide::util::CompiledRegex regex("(a+)+$", 0);
  Expect(regex.valid(), "the backtracking pattern should compile");

  auto match_data = regex.CreateMatchData();
  Expect(match_data.valid(), "match data should be created");

  // 60 'a's followed by a non-'a' terminator: the pattern can never match, and a
  // naive engine backtracks exponentially. The limits must cut it off.
  const std::string evil(60, 'a');
  const std::string text = evil + "!";

  const int rc = regex.Match(text, 0, match_data);
  Expect(rc == PCRE2_ERROR_MATCHLIMIT || rc == PCRE2_ERROR_DEPTHLIMIT,
         "catastrophic backtracking should hit the match/depth limit, not hang");
}

// The limits must not disturb ordinary matching: a normal pattern over normal
// text still succeeds well within the caps.
void TestRegexUtilNormalMatchWithinLimits() {
  microide::util::CompiledRegex regex("(foo|bar)+baz", 0);
  Expect(regex.valid(), "a normal alternation pattern should compile");

  auto match_data = regex.CreateMatchData();
  const std::string text = "foobarfoobaz";
  const int rc = regex.Match(text, 0, match_data);
  Expect(rc >= 0, "a legitimate pattern should still match under the limits");
}

// SubstituteInto: global capture-group substitution with the extended escapes.
void TestRegexUtilSubstituteIntoCaptureGroups() {
  microide::util::CompiledRegex regex("(\\w+)=(\\w+)", 0);
  Expect(regex.valid(), "substitute fixture should compile");

  std::string out = "prefix ";
  const int rc = regex.SubstituteInto("a=1 b=2", "$2=$1", out);
  Expect(rc == 2, "SubstituteInto should report the global substitution count");
  Expect(out == "prefix 1=a 2=b", "SubstituteInto should expand $1/$2 and append to `out`");
}

void TestRegexUtilSubstituteIntoCaselessAndEscapes() {
  microide::util::CompiledRegex regex("foo", PCRE2_CASELESS);
  Expect(regex.valid(), "caseless substitute fixture should compile");
  std::string out;
  const int rc = regex.SubstituteInto("FOO Foo foo", "x", out);
  Expect(rc == 3 && out == "x x x", "PCRE2_CASELESS should fold every case variant");

  // \n / \t are interpreted under PCRE2_SUBSTITUTE_EXTENDED (VSCode-parity).
  microide::util::CompiledRegex tab("X", 0);
  std::string escaped;
  Expect(tab.SubstituteInto("aXb", "\\t", escaped) == 1 && escaped == "a\tb",
         "extended replacement escapes should be interpreted");
}

void TestRegexUtilSubstituteIntoNoMatchAndBadEscape() {
  microide::util::CompiledRegex regex("zzz", 0);
  std::string out = "keep";
  Expect(regex.SubstituteInto("abc", "X", out) == 0 && out == "keepabc",
         "a no-match subject is copied through unchanged with a count of 0");

  // `\q` is not a valid extended replacement escape -> negative error, out untouched.
  microide::util::CompiledRegex any("a", 0);
  std::string bad = "seed";
  const int rc = any.SubstituteInto("a", "\\q", bad);
  Expect(rc < 0 && bad == "seed",
         "an invalid replacement escape should return a negative rc and not mutate `out`");
}

// ExpandMatchAt: expand ONE match's replacement in full context (lookarounds work).
void TestRegexUtilExpandMatchAt() {
  microide::util::CompiledRegex regex("(\\d+)", 0);
  Expect(regex.valid(), "expand fixture should compile");

  // "a12b34": the match at offset 1 is "12"; expanding "<$1>" yields "<12>".
  const auto expanded = regex.ExpandMatchAt("a12b34", 1, "<$1>");
  Expect(expanded.has_value() && *expanded == "<12>",
         "ExpandMatchAt should expand the single match anchored at the offset");

  // No match anchored at offset 0 ("a" is not a digit) -> nullopt.
  Expect(!regex.ExpandMatchAt("a12", 0, "$1").has_value(),
         "ExpandMatchAt should return nullopt when nothing matches anchored at the offset");
}

// FindNextRegexMatchInLine: the shared per-line iterator (empty-match recovery).
void TestRegexUtilFindNextRegexMatchInLine() {
  microide::util::CompiledRegex regex("a", 0);
  auto match_data = regex.CreateMatchData();
  const std::string_view line = "aXa";
  std::size_t from = 0, start = 0, end = 0;
  Expect(microide::util::FindNextRegexMatchInLine(regex, line, &from, &match_data, &start, &end) &&
             start == 0 && end == 1,
         "first match should be at [0,1)");
  Expect(microide::util::FindNextRegexMatchInLine(regex, line, &from, &match_data, &start, &end) &&
             start == 2 && end == 3,
         "second match should be at [2,3)");
  Expect(!microide::util::FindNextRegexMatchInLine(regex, line, &from, &match_data, &start, &end),
         "the line should be exhausted after both matches");

  // Empty-match / anchored-alternative recovery: `x?|foo` on "foo" finds "foo".
  microide::util::CompiledRegex alt("x?|foo", 0);
  auto alt_data = alt.CreateMatchData();
  std::size_t af = 0, as = 0, ae = 0;
  Expect(microide::util::FindNextRegexMatchInLine(alt, "foo", &af, &alt_data, &as, &ae) &&
             as == 0 && ae == 3,
         "the anchored-alternative recovery should surface the non-empty `foo` match");
}

}  // namespace

void RegisterRegexUtilTests(std::vector<TestCase>& tests) {
  AddTest(tests, "RegexUtil/SubstituteIntoCaptureGroups",
          TestRegexUtilSubstituteIntoCaptureGroups);
  AddTest(tests, "RegexUtil/SubstituteIntoCaselessAndEscapes",
          TestRegexUtilSubstituteIntoCaselessAndEscapes);
  AddTest(tests, "RegexUtil/SubstituteIntoNoMatchAndBadEscape",
          TestRegexUtilSubstituteIntoNoMatchAndBadEscape);
  AddTest(tests, "RegexUtil/ExpandMatchAt", TestRegexUtilExpandMatchAt);
  AddTest(tests, "RegexUtil/FindNextRegexMatchInLine", TestRegexUtilFindNextRegexMatchInLine);
  AddTest(tests, "RegexUtil/BoundsCatastrophicBacktracking",
          TestRegexUtilBoundsCatastrophicBacktracking);
  AddTest(tests, "RegexUtil/NormalMatchWithinLimits", TestRegexUtilNormalMatchWithinLimits);
  AddTest(tests, "RegexUtil/CompilesAndCapturesMatchRanges",
          TestRegexUtilCompilesAndCapturesMatchRanges);
  AddTest(tests, "RegexUtil/BuildsPrefixedErrorMessages",
          TestRegexUtilBuildsPrefixedErrorMessages);
  AddTest(tests, "RegexUtil/CopiesCompiledPatterns",
          TestRegexUtilCopiesCompiledPatterns);
}

}  // namespace microide::tests
