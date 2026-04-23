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

}  // namespace

void RegisterRegexUtilTests(std::vector<TestCase>& tests) {
  AddTest(tests, "RegexUtil/CompilesAndCapturesMatchRanges",
          TestRegexUtilCompilesAndCapturesMatchRanges);
  AddTest(tests, "RegexUtil/BuildsPrefixedErrorMessages",
          TestRegexUtilBuildsPrefixedErrorMessages);
  AddTest(tests, "RegexUtil/CopiesCompiledPatterns",
          TestRegexUtilCopiesCompiledPatterns);
}

}  // namespace microide::tests
