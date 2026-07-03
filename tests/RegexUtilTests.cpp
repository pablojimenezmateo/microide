#include "TestSupport.h"

#include "util/RegexUtil.h"

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

}  // namespace

void RegisterRegexUtilTests(std::vector<TestCase>& tests) {
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
