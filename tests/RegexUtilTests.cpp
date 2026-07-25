#include "TestSupport.h"

#include "util/RegexUtil.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
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

// The empty-match advance in FindNextRegexMatchInLine must step a whole
// CHARACTER when the pattern is compiled with PCRE2_UTF, not one byte.
//
// A byte step lands inside a multi-byte sequence; pcre2_match then rejects that
// start offset with PCRE2_ERROR_BADUTFOFFSET, which the loop's `rc < 0` arm
// treats as "line exhausted" — silently dropping every remaining match on the
// line. PCRE2_MATCH_INVALID_UTF (>= 10.34) tolerates such an offset and masks
// the bug, but SearchRegexCompileOptions deliberately falls back to plain
// UTF|UCP on older PCRE2, and there a case-insensitive non-ASCII query that can
// match empty returned ZERO matches from the first multi-byte character onward.
//
// So this asserts BOTH option sets explicitly. The second one is the older-PCRE2
// fallback, which is otherwise unreachable (and therefore untestable) on a host
// whose PCRE2 defines PCRE2_MATCH_INVALID_UTF.
void TestRegexUtilEmptyMatchAdvanceStepsWholeCharacters() {
  using microide::util::CompiledRegex;
  using microide::util::FindNextRegexMatchInLine;

  // Targets deliberately sit immediately after multi-byte characters, so a
  // mid-sequence start offset is reached before any of them.
  const std::string line = "\xE6\x97\xA5" "aa" "\xE6\x9C\xAC" "aa" "\xE8\xAA\x9E" "aa";
  const std::vector<std::pair<std::size_t, std::size_t>> expected = {{3, 5}, {8, 10}, {13, 15}};

  const auto collect = [&](const std::string& pattern, std::uint32_t options) {
    CompiledRegex regex(pattern, options, "bad pattern");
    Expect(regex.valid(), "fixture pattern should compile");
    Expect(regex.utf_mode() == ((options & PCRE2_UTF) != 0),
           "utf_mode() must report whether PCRE2_UTF was requested");
    auto match_data = regex.CreateMatchData();
    std::vector<std::pair<std::size_t, std::size_t>> found;
    std::size_t from = 0;
    std::size_t start = 0;
    std::size_t end = 0;
    while (FindNextRegexMatchInLine(regex, line, &from, &match_data, &start, &end)) {
      found.emplace_back(start, end);
      if (found.size() > 8) {
        break;  // runaway guard
      }
    }
    return found;
  };

  // `é?|aa` matches EMPTY at offset 0, so the advance runs before any real match.
  const std::string pattern = "\xC3\xA9?|aa";

  std::uint32_t with_invalid_utf = PCRE2_CASELESS | PCRE2_UTF | PCRE2_UCP;
#ifdef PCRE2_MATCH_INVALID_UTF
  with_invalid_utf |= PCRE2_MATCH_INVALID_UTF;
#endif
  Expect(collect(pattern, with_invalid_utf) == expected,
         "every match must be found under the current PCRE2 option set");

  // The older-PCRE2 fallback: UTF without MATCH_INVALID_UTF. This is the
  // configuration where a byte-wise advance silently returned no matches at all.
  Expect(collect(pattern, PCRE2_CASELESS | PCRE2_UTF | PCRE2_UCP) == expected,
         "every match must be found without PCRE2_MATCH_INVALID_UTF too — a byte-wise "
         "advance yields BADUTFOFFSET there and drops the rest of the line");

  // Non-UTF (byte-oriented) matching must KEEP its 1-byte advance. Without
  // PCRE2_UTF, PCRE2 may legally start a match at any byte — including one that
  // happens to be a UTF-8 continuation byte — so stepping to a character
  // boundary would skip over real matches in binary / Latin-1 / mixed-encoding
  // content, which project search does scan.
  Expect(collect("x?|aa", PCRE2_CASELESS) == expected,
         "byte-oriented matching must still find every match");

  // The load-bearing case for that guard: the only match begins at offset 2,
  // which is the THIRD byte of the leading 3-byte character. A character-wise
  // advance from offset 0 jumps straight to 3 and misses it entirely.
  {
    const std::string binary_line = "\xE6\x97\xA5" "aa";  // match "\xA5a" starts at byte 2
    CompiledRegex regex("x?|\xA5" "a", PCRE2_CASELESS, "bad pattern");
    Expect(regex.valid(), "byte-oriented fixture pattern should compile");
    Expect(!regex.utf_mode(), "the byte-oriented fixture must not be in UTF mode");
    auto match_data = regex.CreateMatchData();
    std::size_t from = 0;
    std::size_t start = 0;
    std::size_t end = 0;
    Expect(FindNextRegexMatchInLine(regex, binary_line, &from, &match_data, &start, &end),
           "a byte-oriented match starting mid-sequence must be found");
    Expect(start == 2 && end == 4,
           "byte-oriented advance must not skip a match that begins on a continuation byte");
  }

  // A pattern that ONLY matches empty yields no matches and still terminates.
  CompiledRegex empty_only("\xC3\xA9*", with_invalid_utf, "bad");
  Expect(empty_only.valid(), "empty-only pattern should compile");
  auto empty_data = empty_only.CreateMatchData();
  std::size_t from = 0;
  std::size_t s0 = 0;
  std::size_t e0 = 0;
  Expect(!FindNextRegexMatchInLine(empty_only, line, &from, &empty_data, &s0, &e0),
         "a pattern matching only empty should report no matches");
  Expect(from > line.size(), "the scan must run to completion rather than stalling");
}

void RegisterRegexUtilTests(std::vector<TestCase>& tests) {
  AddTest(tests, "RegexUtil/EmptyMatchAdvanceStepsWholeCharacters",
          TestRegexUtilEmptyMatchAdvanceStepsWholeCharacters);
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
