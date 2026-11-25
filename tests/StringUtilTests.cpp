#include "TestSupport.h"

#include "compare/CompareModel.h"
#include "util/Hex.h"
#include "util/PerformanceCounters.h"
#include "util/SaturatingMath.h"
#include "util/Sha256.h"
#include "util/StringUtil.h"
#include "util/PerformanceTrace.h"
#include "util/TraceChannel.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>
#include <limits>
#include <thread>

#include <utility>
#include <vector>

namespace microide::tests {
namespace {

using microide::util::IsValidUtf8;
using microide::util::JoinLines;
using microide::util::LineEnding;
using microide::util::LineEndingLabel;
using microide::util::NormalizeLineEndings;
using microide::util::ParseLineEndingLabel;
using microide::util::SerializeLines;
using microide::util::SplitLines;
using microide::util::Utf8SequenceLength;

void TestStringUtilDecodeUtf8CodepointRejectsInvalidScalars() {
  using microide::util::DecodeUtf8Codepoint;
  Expect(DecodeUtf8Codepoint("A") == U'A', "ASCII decodes to itself");
  Expect(DecodeUtf8Codepoint("\xC3\xA9") == 0x00E9, "2-byte é decodes correctly");
  Expect(DecodeUtf8Codepoint("\xE2\x82\xAC") == 0x20AC, "3-byte € decodes correctly");
  Expect(DecodeUtf8Codepoint("\xF0\x9F\x98\x80") == 0x1F600, "4-byte 😀 decodes correctly");

  Expect(DecodeUtf8Codepoint("\xED\xA0\x80") == 0xFFFD,
         "UTF-16 high surrogate U+D800 must be rejected");
  Expect(DecodeUtf8Codepoint("\xE0\x80\x80") == 0xFFFD,
         "overlong 3-byte encoding of U+0000 must be rejected");
  Expect(DecodeUtf8Codepoint("\xC0\x80") == 0xFFFD,
         "overlong 2-byte encoding of U+0000 must be rejected");
  Expect(DecodeUtf8Codepoint("\xF4\xBF\xBF\xBF") == 0xFFFD,
         "codepoint above U+10FFFF must be rejected");
  Expect(DecodeUtf8Codepoint("\xF0\x80\x80\x80") == 0xFFFD,
         "overlong 4-byte encoding must be rejected");
}

void TestStringUtilUtf8SequenceLengthHandlesAsciiAndEmoji() {
  const std::string text = "a😀z";
  Expect(Utf8SequenceLength(text, 0) == 1,
         "utf8 sequence length should treat ascii as a single byte");
  Expect(Utf8SequenceLength(text, 1) == 4,
         "utf8 sequence length should return four bytes for emoji codepoints");
  Expect(Utf8SequenceLength(text, text.size()) == 0,
         "utf8 sequence length should report zero past the end");
}

void TestStringUtilUtf8ByteBudgetTruncatesOnBoundary() {
  using microide::util::TruncateUtf8ToByteBudget;
  using microide::util::Utf8ByteBudgetPrefixLength;

  Expect(Utf8ByteBudgetPrefixLength("abc", 3) == 3, "exact-fit keeps all bytes");
  Expect(Utf8ByteBudgetPrefixLength("abc", 100) == 3, "under-budget keeps all bytes");
  Expect(Utf8ByteBudgetPrefixLength("", 0) == 0, "empty stays empty");

  const std::string mixed = "a😀z";
  Expect(mixed.size() == 6, "fixture is six bytes");
  Expect(Utf8ByteBudgetPrefixLength(mixed, 2) == 1, "budget mid-emoji backs off to boundary");
  Expect(Utf8ByteBudgetPrefixLength(mixed, 3) == 1, "budget mid-emoji backs off to boundary");
  Expect(Utf8ByteBudgetPrefixLength(mixed, 4) == 1, "budget mid-emoji backs off to boundary");
  Expect(Utf8ByteBudgetPrefixLength(mixed, 5) == 5, "budget at codepoint boundary keeps it whole");

  std::string s = mixed;
  Expect(TruncateUtf8ToByteBudget(s, 3), "over-budget reports truncation");
  Expect(s == "a", "truncation drops the straddling multi-byte sequence whole");
  std::string t = "abc";
  Expect(!TruncateUtf8ToByteBudget(t, 3), "exact-fit reports no truncation");
  Expect(t == "abc", "exact-fit leaves the string intact");
}

void TestStringUtilUtf8ValidationRejectsBrokenSequences() {
  Expect(IsValidUtf8("hello 😀"),
         "utf8 validation should accept valid multibyte sequences");

  const std::string truncated = std::string("x") + static_cast<char>(0xE2) +
                                static_cast<char>(0x82);
  Expect(!IsValidUtf8(truncated),
         "utf8 validation should reject truncated multibyte sequences");

  const std::string surrogate = std::string("x") + static_cast<char>(0xED) +
                                static_cast<char>(0xA0) + static_cast<char>(0x80);
  Expect(!IsValidUtf8(surrogate),
         "utf8 validation should reject surrogate codepoint encodings");
}

void TestStringUtilSplitLinesNormalizesMixedLineEndings() {
  const std::vector<std::string> lines = SplitLines("alpha\r\nbeta\rgamma\n");
  Expect(lines.size() == 4,
         "line splitting should preserve a trailing empty line after the final newline");
  Expect(lines[0] == "alpha" && lines[1] == "beta" && lines[2] == "gamma" &&
             lines[3].empty(),
         "line splitting should normalize CRLF, CR, and LF separators consistently");
}

void TestStringUtilNormalizeLineEndingsPreservesCrOnlyLineBreaks() {
  Expect(NormalizeLineEndings("alpha\r\nbeta\rgamma\n") == "alpha\nbeta\ngamma\n",
         "normalization should convert CRLF and lone CR separators to LF separators");
}

void TestStringUtilJoinLinesHonorsSeparatorsAndEmptyInput() {
  const std::vector<std::string> lines = {"alpha", "beta", "gamma"};
  Expect(JoinLines(lines, "::") == "alpha::beta::gamma",
         "join lines should place the requested separator between every line");
  Expect(JoinLines({}, "\n").empty(),
         "join lines should keep empty inputs empty");

  const std::vector<std::string> one = {"solo"};
  Expect(JoinLines(one, "::") == "solo", "a single line joins to itself with no separator");

  std::vector<std::string> many;
  std::string expected;
  for (int i = 0; i < 500; ++i) {
    std::string token(static_cast<std::size_t>(i % 7), 'x');
    if (i != 0) {
      expected += "|";
    }
    expected += token;
    many.push_back(std::move(token));
  }
  Expect(JoinLines(many, "|") == expected,
         "join lines should produce the exact concatenation for many varied-length lines");
}

void TestStringUtilLineEndingHelpersRoundTrip() {
  const auto decoded = microide::util::DecodeLines("alpha\r\nbeta\rgamma\n");
  Expect(decoded.lines.size() == 4,
         "decoded text should preserve logical lines plus the trailing empty row");
  Expect(decoded.line_ending == LineEnding::CRLF,
         "decoded text should prefer the dominant line ending style");
  Expect(decoded.mixed_line_endings,
         "decoded text should report mixed line endings when multiple styles are present");

  const std::vector<std::string> lines = {"alpha", "beta", "gamma"};
  Expect(SerializeLines(lines, LineEnding::LF) == "alpha\nbeta\ngamma",
         "serialize lines should honor LF separators");
  Expect(SerializeLines(lines, LineEnding::CRLF) == "alpha\r\nbeta\r\ngamma",
         "serialize lines should honor CRLF separators");
  Expect(SerializeLines(lines, LineEnding::CR) == "alpha\rbeta\rgamma",
         "serialize lines should honor CR separators");
  Expect(LineEndingLabel(LineEnding::CRLF) == "crlf",
         "line ending labels should expose persistence-friendly lowercase forms");
  Expect(ParseLineEndingLabel("cr") == LineEnding::CR,
         "line ending labels should parse back into enum values");

  for (const LineEnding ending : {LineEnding::LF, LineEnding::CRLF, LineEnding::CR}) {
    Expect(microide::util::SerializeLinesStreaming(lines, ending) == SerializeLines(lines, ending),
           "streaming serializer matches SerializeLines for all line endings");
  }
  const std::vector<std::string> single = {"only"};
  Expect(microide::util::SerializeLinesStreaming(single, LineEnding::LF) == "only",
         "streaming serializer emits no trailing separator for a single line");
  const std::vector<std::string> empty_lines;
  Expect(microide::util::SerializeLinesStreaming(empty_lines, LineEnding::LF).empty(),
         "streaming serializer keeps empty input empty");
}

void TestCompareModelHandlesCrLfInputViaSharedSplitter() {
  const auto model =
      microide::compare::BuildCompareModel("alpha\r\nbeta\r\n", "alpha\r\nbeta\r\ngamma\r\n");
  Expect(model.rows.size() == 4,
         "compare model should preserve logical lines plus the shared trailing empty row");
  Expect(model.rows[0].left_text == "alpha" && model.rows[1].left_text == "beta",
         "compare model should split CRLF input into plain logical lines");
  Expect(model.rows[2].kind == microide::compare::CompareRowKind::Added &&
             model.rows[2].right_text == "gamma",
         "compare model should preserve added CRLF lines under the shared splitter");
  Expect(model.rows[3].left_text.empty() && model.rows[3].right_text.empty(),
         "compare model should preserve the trailing empty row from TextViewport splitting");
}

void TestStringUtilCollapseWhitespaceTracksMatchRange() {
  const std::string_view raw = "  alpha   beta ";
  const std::size_t raw_match_start = raw.find("beta");
  std::size_t start = 99;
  std::size_t length = 99;
  const std::string collapsed = microide::util::CollapseAsciiWhitespaceTrackingMatch(
      raw, raw_match_start, raw_match_start + 4, &start, &length);
  Expect(collapsed == "alpha beta", "collapse should drop leading/duplicate whitespace");
  Expect(collapsed.substr(start, length) == "beta",
         "tracked match range should land on the collapsed match text");

  std::size_t lead_start = 99;
  std::size_t lead_length = 99;
  const std::string lead = microide::util::CollapseAsciiWhitespaceTrackingMatch(
      "alpha beta", 0, 5, &lead_start, &lead_length);
  Expect(lead_start == 0 && lead.substr(lead_start, lead_length) == "alpha",
         "leading match should map to the start of the collapsed preview");

  std::size_t empty_start = 99;
  std::size_t empty_length = 99;
  const std::string empty = microide::util::CollapseAsciiWhitespaceTrackingMatch(
      "alpha", 100, 100, &empty_start, &empty_length);
  Expect(empty == "alpha" && empty_length == 0 && empty_start <= empty.size(),
         "an out-of-range match should clamp to a safe zero-length highlight");

  std::size_t trail_start = 99;
  std::size_t trail_length = 99;
  const std::string trail = microide::util::CollapseAsciiWhitespaceTrackingMatch(
      "a  b", 0, 3, &trail_start, &trail_length);
  Expect(trail == "a b", "adjacent whitespace collapses to a single space");
  Expect(trail_start == 0 && trail_length == 2 && trail.substr(trail_start, trail_length) == "a ",
         "a match ending in a consumed whitespace run keeps the collapsed space");
}

void TestStringUtilAppendUtf8EncodesAllSequenceLengths() {
  std::string out;
  microide::util::AppendUtf8(out, U'A');
  microide::util::AppendUtf8(out, char32_t{0x00E9});
  microide::util::AppendUtf8(out, char32_t{0x20AC});
  microide::util::AppendUtf8(out, char32_t{0x1F600});
  const std::string expected = {
      'A',
      static_cast<char>(0xC3), static_cast<char>(0xA9),
      static_cast<char>(0xE2), static_cast<char>(0x82), static_cast<char>(0xAC),
      static_cast<char>(0xF0), static_cast<char>(0x9F), static_cast<char>(0x98),
      static_cast<char>(0x80)};
  Expect(out == expected,
         "AppendUtf8 should encode 1-, 2-, 3-, and 4-byte sequences");
  Expect(microide::util::IsValidUtf8(out),
         "AppendUtf8 output should be valid UTF-8");

  std::string astral;
  microide::util::AppendUtf8(astral, char32_t{0x1F600});
  Expect(microide::util::DecodeUtf8Codepoint(astral) == char32_t{0x1F600},
         "AppendUtf8 should round-trip through DecodeUtf8Codepoint");

  const std::string replacement = {
      static_cast<char>(0xEF), static_cast<char>(0xBF), static_cast<char>(0xBD)};
  std::string surrogate;
  microide::util::AppendUtf8(surrogate, char32_t{0xD800});
  Expect(surrogate == replacement, "AppendUtf8 should fold a lone surrogate to U+FFFD");
  std::string above_max;
  microide::util::AppendUtf8(above_max, char32_t{0x110000});
  Expect(above_max == replacement, "AppendUtf8 should fold >U+10FFFF to U+FFFD");
  Expect(microide::util::IsValidUtf8(surrogate) && microide::util::IsValidUtf8(above_max),
         "AppendUtf8 replacement output should be valid UTF-8");
}

void TestStringUtilIsAllAsciiDigits() {
  Expect(microide::util::IsAllAsciiDigits("0123456789"),
         "all-digit strings should be recognized");
  Expect(!microide::util::IsAllAsciiDigits(""),
         "empty input is not all-digits");
  Expect(!microide::util::IsAllAsciiDigits("12a3"),
         "embedded letters should disqualify");
  Expect(!microide::util::IsAllAsciiDigits(" 12"),
         "leading whitespace should disqualify");
}

void TestStringUtilSplitAsciiWhitespace() {
  const auto parts = microide::util::SplitAsciiWhitespace("  alpha\tbeta   gamma  ");
  Expect(parts.size() == 3, "split should drop leading/trailing/duplicate whitespace runs");
  Expect(parts[0] == "alpha" && parts[1] == "beta" && parts[2] == "gamma",
         "split should preserve token contents across mixed whitespace");
  Expect(microide::util::SplitAsciiWhitespace("   ").empty(),
         "whitespace-only input should yield no tokens");
  Expect(microide::util::SplitAsciiWhitespace("").empty(),
         "empty input should yield no tokens");
}

void TestStringUtilBoundedSplitStopsAtCap() {
  const std::string nul = std::string("a\0b\0c\0d\0e", 9);
  const auto records = microide::util::SplitNulDelimited(nul, 2);
  Expect(records.size() == 2, "bounded NUL split stops at the record cap");
  Expect(records[0] == "a" && records[1] == "b", "bounded NUL split keeps leading records");
  Expect(microide::util::SplitNulDelimited(nul, 0).empty(), "a zero cap yields no records");
  Expect(microide::util::SplitNulDelimited(nul).size() == 5, "unbounded NUL split is unchanged");

  const std::string_view lines = "l1\nl2\nl3\nl4";
  const auto capped = microide::util::SplitLineViews(lines, 2);
  Expect(capped.size() == 2, "bounded line split stops at the line cap");
  Expect(capped[0] == "l1" && capped[1] == "l2", "bounded line split keeps leading lines");
  Expect(microide::util::SplitLineViews(lines, 0).empty(), "a zero line cap yields no lines");
}

void TestStringUtilDecodeLinesSinglePassRegression() {
  const auto lf = microide::util::DecodeLines("a\nb\n");
  Expect(lf.lines.size() == 3 && lf.lines[0] == "a" && lf.lines[1] == "b" && lf.lines[2].empty(),
         "LF decode should keep the trailing empty row");
  Expect(lf.line_ending == LineEnding::LF && !lf.mixed_line_endings,
         "pure LF input should report LF and no mixing");

  const auto crlf = microide::util::DecodeLines("a\r\nb");
  Expect(crlf.lines.size() == 2 && crlf.lines[0] == "a" && crlf.lines[1] == "b",
         "CRLF decode should split without a trailing empty row when input lacks one");
  Expect(crlf.line_ending == LineEnding::CRLF && !crlf.mixed_line_endings,
         "pure CRLF input should report CRLF and no mixing");

  const auto empty = microide::util::DecodeLines("");
  Expect(empty.lines.size() == 1 && empty.lines[0].empty(),
         "empty input should yield a single empty line");
  Expect(empty.line_ending == LineEnding::LF,
         "empty input should default to LF");
}

void TestMeasureLinesAgreesWithSplitLines() {
  const std::string_view cases[] = {
      "", "a", "a\nb", "a\n", "\n", "a\r\nb", "a\rb", "a\r\n", "a\r",
      "\r\n\r\n", "line1\nline2\r\nline3\rline4", "no breaks at all", "\n\n\n",
  };
  for (const std::string_view text : cases) {
    const auto shape = microide::util::MeasureLines(text);
    const auto split = microide::util::SplitLines(text);
    Expect(shape.count == split.size(),
           std::string("MeasureLines line count must match SplitLines for ") + std::string(text));
    Expect(shape.last_line_size == split.back().size(),
           std::string("MeasureLines last-line size must match SplitLines for ") +
               std::string(text));
    const auto normalized = microide::util::SplitLines(microide::util::NormalizeLineEndings(text));
    Expect(shape.count == normalized.size() && shape.last_line_size == normalized.back().size(),
           "MeasureLines must match SplitLines(NormalizeLineEndings(x)) as well");
  }
  Expect(microide::util::MeasureLines("").count == 1,
         "empty input measures as one (empty) line, matching SplitLines");
}

void TestHexDigitAndByteParsing() {
  Expect(microide::util::HexDigitValue('0') == 0 && microide::util::HexDigitValue('9') == 9,
         "decimal hex digits map to their value");
  Expect(microide::util::HexDigitValue('a') == 10 && microide::util::HexDigitValue('F') == 15,
         "lower- and upper-case hex letters map correctly");
  Expect(microide::util::HexDigitValue('g') == -1 && microide::util::HexDigitValue(' ') == -1,
         "non-hex characters return -1");
  Expect(microide::util::ParseHexByte('4', '1') == static_cast<std::uint8_t>(0x41),
         "two hex digits combine into a byte");
  Expect(!microide::util::ParseHexByte('z', '1').has_value(),
         "an invalid high nibble rejects the byte");
}

void TestDecodeHexColor() {
  const auto white = microide::util::DecodeHexColor("#ffffff");
  Expect(white.has_value() && (*white)[0] == 0xFF && (*white)[1] == 0xFF && (*white)[2] == 0xFF,
         "#ffffff decodes to all-255 components");
  const auto mixed = microide::util::DecodeHexColor("#1F2a3B");
  Expect(mixed.has_value() && (*mixed)[0] == 0x1F && (*mixed)[1] == 0x2A && (*mixed)[2] == 0x3B,
         "mixed-case hex colour decodes case-insensitively");
  Expect(!microide::util::DecodeHexColor("1f2a3b").has_value(),
         "missing leading '#' rejects the colour");
  Expect(!microide::util::DecodeHexColor("#1f2a3").has_value(),
         "short colour strings are rejected");
  Expect(!microide::util::DecodeHexColor("#1f2a3g").has_value(),
         "non-hex digits reject the colour");
}

void TestPercentDecode() {
  Expect(microide::util::PercentDecode("/a%20b") == "/a b",
         "percent escapes decode to their byte");
  Expect(microide::util::PercentDecode("x%2F") == "x/",
         "a trailing %XX at the end of the string still decodes");
  Expect(microide::util::PercentDecode("100%") == "100%",
         "a lone trailing '%' is left verbatim");
  Expect(microide::util::PercentDecode("%zz") == "%zz",
         "invalid escape digits are left verbatim");
  Expect(microide::util::PercentDecode("plain") == "plain",
         "input without escapes is returned unchanged");
}

void TestSaturatingMath() {
  using microide::util::SaturatingAdd;
  using microide::util::SaturatingSub;
  using U64 = std::uint64_t;
  constexpr U64 kMax = std::numeric_limits<U64>::max();

  Expect(SaturatingSub<U64>(10, 3) == 7, "normal subtraction");
  Expect(SaturatingSub<U64>(3, 10) == 0, "underflow floors at zero rather than wrapping");
  Expect(SaturatingSub<U64>(0, 0) == 0, "zero minus zero is zero");
  Expect(SaturatingSub<U64>(kMax, 1) == kMax - 1, "subtracting from the max is exact");

  Expect(SaturatingAdd<U64>(10, 3) == 13, "normal addition");
  Expect(SaturatingAdd<U64>(kMax, 1) == kMax, "overflow clamps to the max");
  Expect(SaturatingAdd<U64>(kMax - 5, 5) == kMax, "addition up to the max is exact");
  Expect(SaturatingAdd<U64>(kMax, kMax) == kMax, "doubling the max clamps to the max");
}

void TestStringUtilCodepointDisplayWidth() {
  using microide::util::CodepointDisplayWidth;
  Expect(CodepointDisplayWidth(U'a') == 1, "ASCII is one column");
  Expect(CodepointDisplayWidth(0x0000) == 0, "NUL has no advance");
  Expect(CodepointDisplayWidth(0x001B) == 0, "a C0 control has no advance");
  Expect(CodepointDisplayWidth(0x00AD) == 1, "SOFT HYPHEN is one column, as in glibc");
  Expect(CodepointDisplayWidth(0x0301) == 0, "a combining acute is zero width");
  Expect(CodepointDisplayWidth(0x093C) == 0, "Devanagari nukta is zero width");
  Expect(CodepointDisplayWidth(0x093F) == 1, "Devanagari vowel sign I is a SPACING mark: one column");
  Expect(CodepointDisplayWidth(0x0BC0) == 0, "Tamil vowel sign II is zero width");
  Expect(CodepointDisplayWidth(0x0E31) == 0, "Thai MAI HAN-AKAT is zero width");
  Expect(CodepointDisplayWidth(0x1160) == 0, "a conjoining Hangul jungseong is zero width");
  Expect(CodepointDisplayWidth(0x200D) == 0, "ZERO WIDTH JOINER is zero width");
  Expect(CodepointDisplayWidth(0xFE0F) == 0, "VARIATION SELECTOR-16 is zero width");
  Expect(CodepointDisplayWidth(0xE0100) == 0, "a supplementary variation selector is zero width");
  Expect(CodepointDisplayWidth(0x4E2D) == 2, "CJK 中 is two columns");
  Expect(CodepointDisplayWidth(0xAC00) == 2, "Hangul syllable 가 is two columns");
  Expect(CodepointDisplayWidth(0xFF21) == 2, "fullwidth Ａ is two columns");
  Expect(CodepointDisplayWidth(0x26A1) == 2, "⚡ is two columns (it was one)");
  Expect(CodepointDisplayWidth(0x2705) == 2, "✅ is two columns (it was one)");
  Expect(CodepointDisplayWidth(0x274C) == 2, "❌ is two columns (it was one)");
  Expect(CodepointDisplayWidth(0x231A) == 2, "⌚ is two columns");
  Expect(CodepointDisplayWidth(0x2603) == 1, "☃ is one column (East Asian Neutral)");
  Expect(CodepointDisplayWidth(0x1F600) == 2, "😀 is two columns");
  Expect(CodepointDisplayWidth(0x1F3FB) == 2, "an emoji skin-tone modifier is two columns");
  Expect(CodepointDisplayWidth(0x17000) == 2, "Tangut is two columns (it was one)");
  Expect(CodepointDisplayWidth(0x20000) == 2, "CJK extension B is two columns");
  Expect(CodepointDisplayWidth(0x00E9) == 1, "é is one column");
  Expect(CodepointDisplayWidth(0x0416) == 1, "Cyrillic Ж is one column");
}

void TestStringUtilUnicodeCaseFold() {
  using microide::util::SimpleFoldCodepoint;
  using microide::util::Utf8CaseFold;
  using microide::util::Utf8IsIdentifierCodepoint;
  using microide::util::Utf8QueryHasCaseVariation;

  Expect(SimpleFoldCodepoint(U'A') == U'a', "ASCII A folds to a");
  Expect(SimpleFoldCodepoint(U'z') == U'z', "ASCII z unchanged");
  Expect(SimpleFoldCodepoint(U'5') == U'5', "digit unchanged");

  Expect(SimpleFoldCodepoint(0x00C9) == 0x00E9, "É folds to é");
  Expect(SimpleFoldCodepoint(0x00E9) == 0x00E9, "é is stable");
  Expect(SimpleFoldCodepoint(0x00C4) == 0x00E4, "Ä folds to ä");
  Expect(SimpleFoldCodepoint(0x0100) == 0x0101, "Ā folds to ā");
  Expect(SimpleFoldCodepoint(0x0139) == 0x013A, "Ĺ folds to ĺ");
  Expect(SimpleFoldCodepoint(0x0391) == 0x03B1, "Greek Α folds to α");
  Expect(SimpleFoldCodepoint(0x0394) == 0x03B4, "Greek Δ folds to δ");
  Expect(SimpleFoldCodepoint(0x0410) == 0x0430, "Cyrillic А folds to а");
  Expect(SimpleFoldCodepoint(0x042F) == 0x044F, "Cyrillic Я folds to я");
  Expect(SimpleFoldCodepoint(0x0401) == 0x0451, "Cyrillic Ё folds to ё");

  Expect(SimpleFoldCodepoint(0x0130) == 0x0130, "İ intentionally unfolded");
  Expect(SimpleFoldCodepoint(0x0131) == 0x0131, "ı intentionally unfolded");
  Expect(SimpleFoldCodepoint(0x00D7) == 0x00D7, "× multiplication sign unchanged");
  Expect(SimpleFoldCodepoint(0x03A2) == 0x03A2, "the Greek capital hole is not a letter");

  Expect(SimpleFoldCodepoint(0x1EA0) == 0x1EA1, "Vietnamese Ạ folds to ạ");
  Expect(SimpleFoldCodepoint(0x0386) == 0x03AC, "accented Greek Ά folds to ά");
  Expect(SimpleFoldCodepoint(0x0460) == 0x0461, "extended Cyrillic Ѡ folds to ѡ");
  Expect(SimpleFoldCodepoint(0x04D0) == 0x04D1, "Cyrillic Ӑ folds to ӑ");
  Expect(SimpleFoldCodepoint(0x0531) == 0x0561, "Armenian Ա folds to ա");
  Expect(SimpleFoldCodepoint(0x10A0) == 0x2D00, "Georgian Ⴀ folds to ⴀ");
  Expect(SimpleFoldCodepoint(0xFF21) == 0xFF41, "fullwidth Ａ folds to ａ");
  Expect(SimpleFoldCodepoint(0x0182) == 0x0183, "Latin Extended-B Ƃ folds to ƃ");
  Expect(SimpleFoldCodepoint(0x1E9E) == 0x1E9E,
         "ẞ stays unfolded: its fold ß is a byte shorter and would desync offsets");
  Expect(SimpleFoldCodepoint(0x212A) == 0x212A, "KELVIN SIGN stays unfolded for the same reason");
  Expect(Utf8CaseFold("VIỆT NAM") == Utf8CaseFold("việt nam"),
         "Vietnamese case-insensitive match after folding");

  {
    std::string upper;
    std::string lower;
    bool length_preserved = true;
    for (char32_t cp = 0x80; cp <= 0x10FFFF && length_preserved; ++cp) {
      if (cp >= 0xD800 && cp <= 0xDFFF) {
        continue;
      }
      const char32_t folded = SimpleFoldCodepoint(cp);
      if (folded == cp) {
        continue;
      }
      upper.clear();
      lower.clear();
      microide::util::AppendUtf8(upper, cp);
      microide::util::AppendUtf8(lower, folded);
      length_preserved = upper.size() == lower.size();
    }
    Expect(length_preserved, "every fold in the table preserves the UTF-8 byte length");
  }

  Expect(Utf8CaseFold("CafÉ") == Utf8CaseFold("café"),
         "mixed-case É matches lowercase é after folding");
  Expect(Utf8CaseFold("ПРИВЕТ") == Utf8CaseFold("привет"),
         "Cyrillic case-insensitive match after folding");
  Expect(Utf8CaseFold("A\xFF" "B") == std::string("a\xFF" "b"),
         "invalid byte 0xFF preserved between folded ASCII");

  Expect(Utf8QueryHasCaseVariation("café") == false, "lowercase-only query has no case variation");
  Expect(Utf8QueryHasCaseVariation("cafÉ") == true, "É triggers case-sensitive smart-case");
  Expect(Utf8QueryHasCaseVariation("привет") == false, "lowercase Cyrillic has no case variation");
  Expect(Utf8QueryHasCaseVariation("Привет") == true, "uppercase Cyrillic triggers case-sensitivity");

  Expect(Utf8IsIdentifierCodepoint(U'a'), "ascii letter is identifier content");
  Expect(Utf8IsIdentifierCodepoint(U'_'), "underscore is identifier content");
  Expect(Utf8IsIdentifierCodepoint(0x00E9), "é is identifier content");
  Expect(Utf8IsIdentifierCodepoint(0x53D8), "CJK 变 is identifier content");
  Expect(!Utf8IsIdentifierCodepoint(U' '), "space is not identifier content");
  Expect(!Utf8IsIdentifierCodepoint(0x2013), "en-dash is not identifier content");
  Expect(!Utf8IsIdentifierCodepoint(0x1F600), "😀 is not identifier content");
  Expect(!Utf8IsIdentifierCodepoint(0x1F468), "👨 is not identifier content");
  Expect(!Utf8IsIdentifierCodepoint(0x1F1E9), "a regional indicator is not identifier content");
  Expect(!Utf8IsIdentifierCodepoint(0x1F3FB), "a skin-tone modifier is not identifier content");
  Expect(!Utf8IsIdentifierCodepoint(0x1FAE0), "a newer pictograph is not identifier content");
  Expect(Utf8IsIdentifierCodepoint(0x1D49C), "a mathematical letter (𝒜) is still identifier content");

  using microide::util::Utf8CodepointStartAt;
  using microide::util::Utf8IdentifierRunEnd;
  using microide::util::Utf8IdentifierRunStart;
  const std::string_view text = "x=gr\xc3\xb6\xc3\x9f" "e+\xe2\x86\x92y";
  const std::size_t g = 2;
  const std::size_t word_end = g + 7;
  Expect(Utf8CodepointStartAt(text, 5) == 4, "a continuation byte snaps to its lead byte");
  Expect(Utf8CodepointStartAt(text, 4) == 4, "a lead byte is its own start");
  Expect(Utf8IdentifierRunStart(text, word_end) == g, "run start walks back over ö and ß");
  Expect(Utf8IdentifierRunStart(text, g) == g, "`=` bounds the run on the left");
  Expect(Utf8IdentifierRunEnd(text, g) == word_end, "run end walks forward over ö and ß");
  Expect(Utf8IdentifierRunEnd(text, word_end) == word_end, "`+` bounds the run on the right");
  Expect(Utf8IdentifierRunStart(text, text.size()) == text.size() - 1,
         "the arrow (U+2192) is a symbol, so the last run is just `y`");
  Expect(Utf8IdentifierRunEnd(text, text.size()) == text.size() &&
             Utf8IdentifierRunStart(text, 0) == 0,
         "runs at the text bounds stay in bounds");
}


void TestStringUtilLeadingByteRun() {
  const auto reference = [](std::string_view text, char byte) {
    for (std::size_t i = 0; i < text.size(); ++i) {
      if (text[i] != byte) {
        return i;
      }
    }
    return text.size();
  };
  const char kBytes[] = {' ', '\0', 'a'};
  for (char byte : kBytes) {
    for (std::size_t length = 0; length <= 24; ++length) {
      const std::string all(length, byte);
      Expect(util::LeadingByteRun(all, byte) == length,
             "an all-matching span returns its whole size");
      Expect(util::LeadingByteRun(all, byte) == reference(all, byte),
             "an all-matching span matches the reference");
      for (std::size_t at = 0; at < length; ++at) {
        std::string text = all;
        text[at] = byte == 'z' ? 'y' : 'z';
        Expect(util::LeadingByteRun(text, byte) == reference(text, byte),
               "the run must end at the first non-matching byte, at every offset");
        Expect(util::LeadingByteRun(text, byte) == at,
               "the run length is the offset of the first non-matching byte");
      }
    }
  }
  std::string high(20, ' ');
  high[13] = static_cast<char>(0xC3);
  Expect(util::LeadingByteRun(high, ' ') == 13, "a non-ASCII byte ends the run");
  for (char byte : kBytes) {
    for (int other = 0; other < 256; ++other) {
      if (static_cast<char>(other) == byte) {
        continue;
      }
      for (std::size_t at = 0; at < 16; ++at) {
        std::string text(16, byte);
        text[at] = static_cast<char>(other);
        Expect(util::LeadingByteRun(text, byte) == at,
               "every non-matching byte value ends the run at its own offset");
      }
    }
  }
  Expect(util::LeadingByteRun("  !      x", ' ') == 2,
         "a '!' two bytes into a space run ends the run there");
  Expect(util::LeadingByteRun("\t\x08\t\t\t\t\t\tz", '\t') == 1,
         "a backspace one byte into a tab run ends the run there");
}

void TestStringUtilFirstNonAsciiOrByte() {
  const auto reference = [](std::string_view text, char also_match) {
    for (std::size_t i = 0; i < text.size(); ++i) {
      if (static_cast<unsigned char>(text[i]) >= 0x80 || text[i] == also_match) {
        return i;
      }
    }
    return text.size();
  };
  const char kTargets[] = {'\t', '\0', 'q'};
  for (char target : kTargets) {
    for (std::size_t length = 0; length <= 24; ++length) {
      const std::string plain(length, 'a');
      Expect(util::FirstNonAsciiOrByte(plain, target) == reference(plain, target),
             "a span with no match returns its size");
      for (std::size_t at = 0; at < length; ++at) {
        std::string with_target = plain;
        with_target[at] = target;
        Expect(util::FirstNonAsciiOrByte(with_target, target) == reference(with_target, target),
               "the target byte is found at every offset");
        std::string with_high = plain;
        with_high[at] = static_cast<char>(0xC3);
        Expect(util::FirstNonAsciiOrByte(with_high, target) == reference(with_high, target),
               "a non-ASCII byte is found at every offset");
        if (at + 1 < length) {
          std::string both = plain;
          both[at] = target;
          both[at + 1] = static_cast<char>(0xC3);
          Expect(util::FirstNonAsciiOrByte(both, target) == reference(both, target),
                 "the earliest matching byte wins");
        }
      }
    }
  }
}

void TestReplaceInvalidUtf8BytesKeepsColumns() {
  std::string latin = "// caf\xe9 au lait \xc3\xa9 ok \xe2\x82 cut \xf0\x9f\x98\x80\n";
  const std::size_t size = latin.size();
  Expect(microide::util::ReplaceInvalidUtf8Bytes(latin), "a stray byte should be replaced");
  Expect(latin.size() == size, "the length is kept");
  Expect(latin == "// caf? au lait \xc3\xa9 ok ?? cut \xf0\x9f\x98\x80\n",
         "only the invalid bytes change, valid sequences stay: " + latin);
  Expect(IsValidUtf8(latin), "the result is valid UTF-8");
  std::string clean = "ascii and \xc3\xa9";
  Expect(!microide::util::ReplaceInvalidUtf8Bytes(clean) && clean == "ascii and \xc3\xa9",
         "valid text is untouched");
}

}

void TestStringUtilConfigTokenSets() {
  using microide::util::IsFalseyToken;
  using microide::util::IsTruthyToken;

  for (const std::string_view falsey : {"0", "false", "no", "off", "FALSE", "No", "Off", "OFF"}) {
    Expect(IsFalseyToken(falsey),
           std::string("`") + std::string(falsey) + "` must read as falsey in any casing");
    Expect(!IsTruthyToken(falsey),
           std::string("`") + std::string(falsey) + "` must not also read as truthy");
  }
  for (const std::string_view truthy : {"1", "true", "yes", "on", "TRUE", "Yes", "ON"}) {
    Expect(IsTruthyToken(truthy),
           std::string("`") + std::string(truthy) + "` must read as truthy in any casing");
    Expect(!IsFalseyToken(truthy),
           std::string("`") + std::string(truthy) + "` must not also read as falsey");
  }
  for (const std::string_view other : {"", "word", "/tmp/dap.log", "offset", "nope", "falsey"}) {
    Expect(!IsFalseyToken(other) && !IsTruthyToken(other),
           std::string("`") + std::string(other) + "` is neither falsey nor truthy");
    Expect(!microide::util::ParseBoolToken(other).has_value(),
           std::string("`") + std::string(other) + "` does not parse as a boolean");
  }
  Expect(microide::util::ParseBoolToken("On") == std::optional<bool>(true) &&
             microide::util::ParseBoolToken("NO") == std::optional<bool>(false),
         "ParseBoolToken answers the truthy and falsey sets in any casing");
  Expect(!IsFalseyToken("word"), "a non-boolean setting payload must not read as disabled");
}

void TestStringUtilAsciiClassifiersAreLocaleIndependent() {
  using microide::util::IsAsciiAlnum;
  using microide::util::IsAsciiAlpha;
  using microide::util::IsAsciiDigit;
  using microide::util::IsAsciiHexDigit;
  using microide::util::IsAsciiLower;
  using microide::util::IsAsciiSpace;
  using microide::util::IsAsciiUpper;
  using microide::util::ToLowerAsciiChar;
  using microide::util::ToUpperAsciiChar;

  for (int c = 0; c < 256; ++c) {
    const auto byte = static_cast<unsigned char>(c);
    const bool expected_space = (c == ' ' || (c >= 0x09 && c <= 0x0D));
    Expect(IsAsciiSpace(byte) == expected_space, "IsAsciiSpace matches the C-locale isspace set");
    Expect(IsAsciiDigit(byte) == (c >= '0' && c <= '9'), "IsAsciiDigit covers 0-9 only");
    Expect(IsAsciiUpper(byte) == (c >= 'A' && c <= 'Z'), "IsAsciiUpper covers A-Z only");
    Expect(IsAsciiLower(byte) == (c >= 'a' && c <= 'z'), "IsAsciiLower covers a-z only");
    Expect(IsAsciiAlpha(byte) == (IsAsciiUpper(byte) || IsAsciiLower(byte)),
           "IsAsciiAlpha is the union of the two letter ranges");
    Expect(IsAsciiAlnum(byte) == (IsAsciiAlpha(byte) || IsAsciiDigit(byte)),
           "IsAsciiAlnum is letters plus digits");
  }

  for (int c = 0x80; c < 256; ++c) {
    const auto byte = static_cast<unsigned char>(c);
    Expect(!IsAsciiSpace(byte) && !IsAsciiAlnum(byte) && !IsAsciiHexDigit(byte),
           "bytes >= 0x80 are never space/alnum/hex regardless of process locale");
    Expect(ToLowerAsciiChar(static_cast<char>(byte)) == static_cast<char>(byte) &&
               ToUpperAsciiChar(static_cast<char>(byte)) == static_cast<char>(byte),
           "case mapping leaves non-ASCII bytes untouched (no signed-char UB)");
  }

  Expect(IsAsciiHexDigit('0') && IsAsciiHexDigit('f') && IsAsciiHexDigit('F') &&
             !IsAsciiHexDigit('g'),
         "IsAsciiHexDigit covers both hex letter cases");
  Expect(ToLowerAsciiChar('Q') == 'q' && ToLowerAsciiChar('q') == 'q',
         "ToLowerAsciiChar is idempotent over the letter range");
  Expect(ToUpperAsciiChar('q') == 'Q' && ToUpperAsciiChar('Q') == 'Q',
         "ToUpperAsciiChar is idempotent over the letter range");
}

void TestStringUtilContainsCaseInsensitiveAscii() {
  using microide::util::ContainsCaseInsensitiveAscii;
  Expect(ContainsCaseInsensitiveAscii("Editor Wrap", "wrap"), "matches ignoring case");
  Expect(ContainsCaseInsensitiveAscii("Editor Wrap", "EDITOR"), "matches an uppercase needle");
  Expect(ContainsCaseInsensitiveAscii("abc", ""), "an empty needle always matches");
  Expect(ContainsCaseInsensitiveAscii("abc", "abc"), "a whole-string needle matches");
  Expect(!ContainsCaseInsensitiveAscii("abc", "abcd"), "an over-long needle cannot match");
  Expect(!ContainsCaseInsensitiveAscii("", "a"), "an empty haystack matches nothing");
  Expect(ContainsCaseInsensitiveAscii("ax ay aZq", "azq"),
         "a repeated first byte does not hide a later match");
  Expect(!ContainsCaseInsensitiveAscii("aaaa", "aab "), "a near-miss tail still rejects");
}

void TestPerformanceCounterNamesStayAlignedWithIds() {
  using microide::util::PerfCounterId;
  using microide::util::PerformanceCounterName;

  std::vector<std::string_view> all;
  for (std::size_t i = 0; i < microide::util::kPerfCounterCount; ++i) {
    const std::string_view name = PerformanceCounterName(static_cast<PerfCounterId>(i));
    Expect(!name.empty(), "every perf counter id must have a name");
    const std::size_t dot = name.find('.');
    Expect(dot != std::string_view::npos && dot > 0 && dot + 1 < name.size(),
           "perf counter names must be <subsystem>.<event>");
    all.push_back(name);
  }
  std::sort(all.begin(), all.end());
  Expect(std::adjacent_find(all.begin(), all.end()) == all.end(),
         "perf counter names must be unique");
}

void TestTraceChannelAggregatesSelfTimeBelowChildren() {
  using microide::util::TraceChannel;
  using microide::util::TraceScope;

  ::setenv("MICROIDE_TEST_TRACE_AGGREGATE", "1", 1);
  TraceChannel channel("test", nullptr, "MICROIDE_TEST_TRACE_AGGREGATE",
                       nullptr);
  Expect(channel.AggregateEnabled(), "aggregate mode follows its env flag");
  Expect(!channel.StreamEnabled(), "a null stream env leaves streaming off");

  {
    TraceScope outer(channel, "outer");
    for (int i = 0; i < 3; ++i) {
      TraceScope inner(channel, "inner");
      const auto start = std::chrono::steady_clock::now();
      while (std::chrono::steady_clock::now() - start < std::chrono::milliseconds(2)) {
      }
    }
  }

  const std::vector<TraceChannel::AggregateEntry> entries = channel.AggregateSnapshot();
  Expect(entries.size() == 2, "one entry per distinct label");
  Expect(entries.front().label == "inner", "the summary ranks by self time, not total");
  Expect(entries.front().count == 3, "repeat scopes accumulate into one row");

  const TraceChannel::AggregateEntry& inner = entries[0];
  const TraceChannel::AggregateEntry& outer = entries[1];
  Expect(outer.label == "outer", "the containing scope is the second row");
  Expect(outer.total_ms >= inner.total_ms, "the parent's total includes its children");
  Expect(outer.self_ms < inner.self_ms, "child time is subtracted from parent self time");
  Expect(inner.max_ms <= inner.total_ms, "max is one call, total is all of them");
  Expect(inner.count == 3 && inner.total_ms >= inner.max_ms, "total covers every call");

  channel.ResetAggregate();
  Expect(channel.AggregateSnapshot().empty(), "ResetAggregate drops accumulated labels");
  ::unsetenv("MICROIDE_TEST_TRACE_AGGREGATE");
}

void TestTraceChannelCapsDistinctLabels() {
  using microide::util::TraceChannel;
  using microide::util::TraceScope;

  ::setenv("MICROIDE_TEST_TRACE_CAP", "1", 1);
  TraceChannel channel("test", nullptr, "MICROIDE_TEST_TRACE_CAP",
                       nullptr);

  const std::size_t over = TraceChannel::kMaxAggregateLabels + 64;
  for (std::size_t i = 0; i < over; ++i) {
    TraceScope scope(channel, "label-" + std::to_string(i));
  }

  const std::vector<TraceChannel::AggregateEntry> entries = channel.AggregateSnapshot();
  Expect(entries.size() <= TraceChannel::kMaxAggregateLabels + 1,
         "distinct labels stay bounded by the cap plus the overflow bucket");
  const auto overflow = std::find_if(entries.begin(), entries.end(),
                                     [](const TraceChannel::AggregateEntry& entry) {
                                       return entry.label == "<aggregate-overflow>";
                                     });
  Expect(overflow != entries.end(), "labels past the cap fold into the overflow bucket");
  Expect(overflow->count == over - TraceChannel::kMaxAggregateLabels,
         "the overflow bucket keeps a truthful call count");
  ::unsetenv("MICROIDE_TEST_TRACE_CAP");
}

void TestTraceChannelKeepsNestingPerThread() {
  using microide::util::TraceChannel;
  using microide::util::TraceScope;

  ::setenv("MICROIDE_TEST_TRACE_THREADS", "1", 1);
  TraceChannel channel("test", nullptr, "MICROIDE_TEST_TRACE_THREADS",
                       nullptr);

  {
    TraceScope held(channel, "main-outer");
    std::thread worker([&channel] {
      for (int i = 0; i < 32; ++i) {
        TraceScope scope(channel, "worker");
      }
    });
    worker.join();
  }

  const std::vector<TraceChannel::AggregateEntry> entries = channel.AggregateSnapshot();
  const auto outer = std::find_if(entries.begin(), entries.end(),
                                  [](const TraceChannel::AggregateEntry& entry) {
                                    return entry.label == "main-outer";
                                  });
  Expect(outer != entries.end(), "the main-thread scope is recorded");
  Expect(outer->self_ms == outer->total_ms,
         "another thread's scopes are not charged as this scope's children");

  const auto worker = std::find_if(entries.begin(), entries.end(),
                                   [](const TraceChannel::AggregateEntry& entry) {
                                     return entry.label == "worker";
                                   });
  Expect(worker != entries.end(), "the worker scopes are recorded");
  Expect(microide::util::TracingMainThreadIsKnown(),
         "the test binary marks its main thread, so the split is meaningful");
  Expect(outer->main_thread_self_ms == outer->self_ms,
         "a scope that ran on the main thread charges all of its self time there");
  Expect(worker->main_thread_self_ms == 0.0,
         "a scope that only ever ran off the main thread charges none");
  ::unsetenv("MICROIDE_TEST_TRACE_THREADS");
}

void TestScopeLabelBuildsNothingWhenTheChannelIsOff() {
  using microide::util::PerformanceTrace;
  using microide::util::TraceChannel;

  ::unsetenv("MICROIDE_TEST_LABEL_OFF");
  TraceChannel off("test", "MICROIDE_TEST_LABEL_OFF", nullptr,
                   nullptr);
  Expect(!off.Enabled(), "an unset env leaves the channel off");

  PerformanceTrace::ScopeLabel label(off, "Some::Scope");
  label.Field("path", std::filesystem::path("/a/very/long/path/that/would/allocate"));
  label.Field("index", 7);
  Expect(label.View().empty(), "an off channel builds no label text at all");

  ::setenv("MICROIDE_TEST_LABEL_ON", "1", 1);
  TraceChannel on("test", "MICROIDE_TEST_LABEL_ON", nullptr,
                  nullptr);
  PerformanceTrace::ScopeLabel built(on, "Some::Scope");
  built.Field("path", std::filesystem::path("/a/b"));
  built.Field("index", 7);
  Expect(built.View() == "Some::Scope(path=/a/b,index=7)",
         "fields are comma-joined inside one parenthesized group");
  Expect(built.View() == "Some::Scope(path=/a/b,index=7)", "View() is idempotent");

  PerformanceTrace::ScopeLabel bare(on, "Bare::Scope");
  Expect(bare.View() == "Bare::Scope", "a label with no fields gains no parentheses");
  ::unsetenv("MICROIDE_TEST_LABEL_ON");
}


void TestUtilSha256MatchesReferenceVectors() {
  using microide::util::Sha256Hex;
  std::string pattern(200, '\0');
  for (std::size_t i = 0; i < pattern.size(); ++i) {
    pattern[i] = static_cast<char>((i * 7 + 3) % 256);
  }
  struct Vector {
    std::size_t length;
    std::string_view digest;
  };
  static constexpr Vector kVectors[] = {
      {0, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"},
      {1, "084fed08b978af4d7d196a7446a86b58009e636b611db16211b65a9aadff29c5"},
      {3, "6ab0dba1f4f1dfbb37b4f9eeb092c09fca4900ad32bdcd147d8dde35d6c87c35"},
      {55, "e7313d333c272e639f790978283f9eb392e843d0f29b7016828bb1daa4aac70b"},
      {56, "4324d65f3c103567f5589c710bc08f8523f929a9272e3af36fc968e52abc6c27"},
      {57, "35df609437dcfea3279283ab79fd554e2bf78f8f7ae2de532d8ee300b09e8f73"},
      {63, "81c80242132f230c3bd41b3e63bbcff16107339549214a99614ff26664625055"},
      {64, "39e3d7b6b5d075d37d053ad89b24b41bef4f3c29760c84447cab3f3be1882241"},
      {65, "aacca6ff74fdbb296d165a45cecfa04e5127bc008770fbbdd48006f2d2fae95e"},
      {119, "9ce7368e4daf32341631b492e80359dc9f594b48453cd0dd5bf0b19279cc177e"},
      {120, "7836b787757e95e58b3ca5aec90b1b004e8deba1e50e9675af9cabf1a13a04b5"},
      {127, "a8d23e75d936f303d248888d9b165ee543f4cbafcad3c9dd2a79bd84faa11d07"},
      {128, "d2742f1f4ac6bb7ca2b239ee18402ba8b3f9f8e652d2a72973c2b9ba11c08cf6"},
      {129, "307f8fc2c1622b92762e818d39a185d4d667ad49a4b07ceae1f4afa008a93ec4"},
      {200, "2c7e18c942ef065b526a2d4e5546283749cd3ddfb51d8fc71f42717363685f46"},
  };
  for (const Vector& vector : kVectors) {
    const std::string digest = Sha256Hex(std::string_view(pattern).substr(0, vector.length));
    Expect(digest == vector.digest,
           "sha256 of " + std::to_string(vector.length) + " pattern bytes: got " + digest);
  }
  Expect(Sha256Hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
         "sha256(\"abc\") matches FIPS 180-2");
  Expect(Sha256Hex(std::string(1000000, 'a')) ==
             "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0",
         "sha256 of one million 'a' matches FIPS 180-2");
}

void TestReplaceInvalidUtf8WithReplacementCharacter() {
  std::string cut = "compare: .git\xF0\x9F\x98\x80\xF0\x9F x\xFF";
  Expect(microide::util::ReplaceInvalidUtf8WithReplacementCharacter(cut), "the bad bytes are replaced");
  Expect(cut == "compare: .git\xF0\x9F\x98\x80\xEF\xBF\xBD\xEF\xBF\xBD x\xEF\xBF\xBD",
         "each bad byte is one U+FFFD, the emoji kept: " + cut);
  Expect(microide::util::IsValidUtf8(cut), "the result is valid UTF-8");
  std::string clean = "caf\xC3\xA9 \xE2\x80\x94 ok";
  Expect(!microide::util::ReplaceInvalidUtf8WithReplacementCharacter(clean) && clean == "caf\xC3\xA9 \xE2\x80\x94 ok",
         "valid text is left as it is");
}

void RegisterStringUtilTests(std::vector<TestCase>& tests) {
  AddTest(tests, "StringUtil/ReplaceInvalidUtf8WithReplacementCharacter",
          TestReplaceInvalidUtf8WithReplacementCharacter);
  AddTest(tests, "StringUtil/ReplaceInvalidUtf8BytesKeepsColumns", TestReplaceInvalidUtf8BytesKeepsColumns);
  AddTest(tests, "Util/Sha256MatchesReferenceVectors", TestUtilSha256MatchesReferenceVectors);
  AddTest(tests, "StringUtil/AsciiClassifiersAreLocaleIndependent",
          TestStringUtilAsciiClassifiersAreLocaleIndependent);
  AddTest(tests, "StringUtil/ContainsCaseInsensitiveAscii",
          TestStringUtilContainsCaseInsensitiveAscii);
  AddTest(tests, "StringUtil/CodepointDisplayWidth", TestStringUtilCodepointDisplayWidth);
  AddTest(tests, "StringUtil/UnicodeCaseFold", TestStringUtilUnicodeCaseFold);
  AddTest(tests, "StringUtil/AppendUtf8EncodesAllSequenceLengths",
          TestStringUtilAppendUtf8EncodesAllSequenceLengths);
  AddTest(tests, "Util/SaturatingMath", TestSaturatingMath);
  AddTest(tests, "Util/TraceChannelAggregatesSelfTimeBelowChildren",
          TestTraceChannelAggregatesSelfTimeBelowChildren);
  AddTest(tests, "Util/TraceChannelCapsDistinctLabels", TestTraceChannelCapsDistinctLabels);
  AddTest(tests, "Util/TraceChannelKeepsNestingPerThread",
          TestTraceChannelKeepsNestingPerThread);
  AddTest(tests, "Util/ScopeLabelBuildsNothingWhenTheChannelIsOff",
          TestScopeLabelBuildsNothingWhenTheChannelIsOff);
  AddTest(tests, "StringUtil/IsAllAsciiDigits", TestStringUtilIsAllAsciiDigits);
  AddTest(tests, "StringUtil/FirstNonAsciiOrByte", TestStringUtilFirstNonAsciiOrByte);
  AddTest(tests, "StringUtil/LeadingByteRun", TestStringUtilLeadingByteRun);
  AddTest(tests, "StringUtil/SplitAsciiWhitespace", TestStringUtilSplitAsciiWhitespace);
  AddTest(tests, "StringUtil/BoundedSplitStopsAtCap", TestStringUtilBoundedSplitStopsAtCap);
  AddTest(tests, "StringUtil/DecodeLinesSinglePassRegression",
          TestStringUtilDecodeLinesSinglePassRegression);
  AddTest(tests, "StringUtil/MeasureLinesAgreesWithSplitLines",
          TestMeasureLinesAgreesWithSplitLines);
  AddTest(tests, "Hex/DigitAndByteParsing", TestHexDigitAndByteParsing);
  AddTest(tests, "Hex/DecodeHexColor", TestDecodeHexColor);
  AddTest(tests, "Hex/PercentDecode", TestPercentDecode);
  AddTest(tests, "StringUtil/CollapseWhitespaceTracksMatchRange",
          TestStringUtilCollapseWhitespaceTracksMatchRange);
  AddTest(tests, "StringUtil/DecodeUtf8CodepointRejectsInvalidScalars",
          TestStringUtilDecodeUtf8CodepointRejectsInvalidScalars);
  AddTest(tests, "StringUtil/Utf8SequenceLengthHandlesAsciiAndEmoji",
          TestStringUtilUtf8SequenceLengthHandlesAsciiAndEmoji);
  AddTest(tests, "StringUtil/Utf8ByteBudgetTruncatesOnBoundary",
          TestStringUtilUtf8ByteBudgetTruncatesOnBoundary);
  AddTest(tests, "StringUtil/Utf8ValidationRejectsBrokenSequences",
          TestStringUtilUtf8ValidationRejectsBrokenSequences);
  AddTest(tests, "StringUtil/SplitLinesNormalizesMixedLineEndings",
          TestStringUtilSplitLinesNormalizesMixedLineEndings);
  AddTest(tests, "StringUtil/NormalizeLineEndingsPreservesCrOnlyLineBreaks",
          TestStringUtilNormalizeLineEndingsPreservesCrOnlyLineBreaks);
  AddTest(tests, "StringUtil/JoinLinesHonorsSeparatorsAndEmptyInput",
          TestStringUtilJoinLinesHonorsSeparatorsAndEmptyInput);
  AddTest(tests, "StringUtil/LineEndingHelpersRoundTrip",
          TestStringUtilLineEndingHelpersRoundTrip);
  AddTest(tests, "StringUtil/ConfigTokenSets", TestStringUtilConfigTokenSets);
  AddTest(tests, "Util/PerformanceCounterNamesStayAlignedWithIds",
          TestPerformanceCounterNamesStayAlignedWithIds);
  AddTest(tests, "StringUtil/CompareModelHandlesCrLfInputViaSharedSplitter",
          TestCompareModelHandlesCrLfInputViaSharedSplitter);
}

}
