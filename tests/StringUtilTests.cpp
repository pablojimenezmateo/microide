#include "TestSupport.h"

#include "compare/CompareModel.h"
#include "util/Hex.h"
#include "util/PerformanceCounters.h"
#include "util/SaturatingMath.h"
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
  // Well-formed sequences decode to their scalar value.
  Expect(DecodeUtf8Codepoint("A") == U'A', "ASCII decodes to itself");
  Expect(DecodeUtf8Codepoint("\xC3\xA9") == 0x00E9, "2-byte é decodes correctly");
  Expect(DecodeUtf8Codepoint("\xE2\x82\xAC") == 0x20AC, "3-byte € decodes correctly");
  Expect(DecodeUtf8Codepoint("\xF0\x9F\x98\x80") == 0x1F600, "4-byte 😀 decodes correctly");

  // Regression: the lead-byte-only length classification used to accept these
  // malformed encodings and yield an invalid scalar. They must all map to U+FFFD.
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

  // Fits within budget: no truncation, full length returned.
  Expect(Utf8ByteBudgetPrefixLength("abc", 3) == 3, "exact-fit keeps all bytes");
  Expect(Utf8ByteBudgetPrefixLength("abc", 100) == 3, "under-budget keeps all bytes");
  Expect(Utf8ByteBudgetPrefixLength("", 0) == 0, "empty stays empty");

  // "a😀z": 'a'(1) + 😀(4 bytes: F0 9F 98 80) + 'z'(1) = 6 bytes total.
  const std::string mixed = "a😀z";
  Expect(mixed.size() == 6, "fixture is six bytes");
  // Budget lands mid-emoji (2..4): the whole 4-byte sequence is dropped, keeping "a".
  Expect(Utf8ByteBudgetPrefixLength(mixed, 2) == 1, "budget mid-emoji backs off to boundary");
  Expect(Utf8ByteBudgetPrefixLength(mixed, 3) == 1, "budget mid-emoji backs off to boundary");
  Expect(Utf8ByteBudgetPrefixLength(mixed, 4) == 1, "budget mid-emoji backs off to boundary");
  // Budget at the emoji's trailing edge (5) keeps 'a' + full emoji = 5 bytes.
  Expect(Utf8ByteBudgetPrefixLength(mixed, 5) == 5, "budget at codepoint boundary keeps it whole");

  // In-place variant reports whether it truncated and never splits a codepoint.
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

  // A single line has no separators; the reserve-size loop (which adds one
  // separator per line beyond the first) must not append a trailing separator.
  const std::vector<std::string> one = {"solo"};
  Expect(JoinLines(one, "::") == "solo", "a single line joins to itself with no separator");

  // TD-2026-07-17-053: the reserve-size accumulation was hardened with saturating
  // adds. Verify a larger, multi-length input still joins exactly so the refactor
  // did not disturb the fast path (correct total reserve, no off-by-one).
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

  // TD-2026-07-17-095: the streaming serializer (used by LSP over a zero-copy
  // LineSpan) must produce byte-identical output to SerializeLines for every line
  // ending, so switching the LSP path off Snapshot() cannot change the payload.
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
  // "  alpha   beta " collapses to "alpha beta"; the match "beta" in the raw
  // string (bytes 9..13) should map onto the collapsed "beta" (bytes 6..10).
  const std::string_view raw = "  alpha   beta ";
  const std::size_t raw_match_start = raw.find("beta");
  std::size_t start = 99;
  std::size_t length = 99;
  const std::string collapsed = microide::util::CollapseAsciiWhitespaceTrackingMatch(
      raw, raw_match_start, raw_match_start + 4, &start, &length);
  Expect(collapsed == "alpha beta", "collapse should drop leading/duplicate whitespace");
  Expect(collapsed.substr(start, length) == "beta",
         "tracked match range should land on the collapsed match text");

  // Leading-token match maps to offset 0.
  std::size_t lead_start = 99;
  std::size_t lead_length = 99;
  const std::string lead = microide::util::CollapseAsciiWhitespaceTrackingMatch(
      "alpha beta", 0, 5, &lead_start, &lead_length);
  Expect(lead_start == 0 && lead.substr(lead_start, lead_length) == "alpha",
         "leading match should map to the start of the collapsed preview");

  // Out-of-range / empty match degrades to a zero-length range, never OOB.
  std::size_t empty_start = 99;
  std::size_t empty_length = 99;
  const std::string empty = microide::util::CollapseAsciiWhitespaceTrackingMatch(
      "alpha", 100, 100, &empty_start, &empty_length);
  Expect(empty == "alpha" && empty_length == 0 && empty_start <= empty.size(),
         "an out-of-range match should clamp to a safe zero-length highlight");

  // Regression: a match whose last byte is whitespace and that consumes the whole
  // trailing whitespace run (match_end points at the next word) used to lose its end
  // (highlight collapsed to zero length). "a  b" -> "a b"; match "a  " maps to "a ".
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
  microide::util::AppendUtf8(out, U'A');           // U+0041, 1 byte
  microide::util::AppendUtf8(out, char32_t{0x00E9});   // é, 2 bytes
  microide::util::AppendUtf8(out, char32_t{0x20AC});   // €, 3 bytes
  microide::util::AppendUtf8(out, char32_t{0x1F600});  // 😀, 4 bytes
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

  // Round-trips against the decoder for an astral (4-byte) codepoint.
  std::string astral;
  microide::util::AppendUtf8(astral, char32_t{0x1F600});
  Expect(microide::util::DecodeUtf8Codepoint(astral) == char32_t{0x1F600},
         "AppendUtf8 should round-trip through DecodeUtf8Codepoint");

  // Unencodable scalars (lone surrogate, above U+10FFFF) must fold to U+FFFD so
  // AppendUtf8 never emits invalid UTF-8 downstream.
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

// TD-2026-07-16-30: bounded SplitNulDelimited/SplitLineViews stop materializing token
// views once the cap is reached, so git parsers can't be forced to allocate millions
// of slots before their own entry cap.
void TestStringUtilBoundedSplitStopsAtCap() {
  // NUL-delimited: 5 records, ask for 2.
  const std::string nul = std::string("a\0b\0c\0d\0e", 9);
  const auto records = microide::util::SplitNulDelimited(nul, 2);
  Expect(records.size() == 2, "bounded NUL split stops at the record cap");
  Expect(records[0] == "a" && records[1] == "b", "bounded NUL split keeps leading records");
  Expect(microide::util::SplitNulDelimited(nul, 0).empty(), "a zero cap yields no records");
  // The unbounded form still returns everything.
  Expect(microide::util::SplitNulDelimited(nul).size() == 5, "unbounded NUL split is unchanged");

  // Line views: 4 lines, ask for 2.
  const std::string_view lines = "l1\nl2\nl3\nl4";
  const auto capped = microide::util::SplitLineViews(lines, 2);
  Expect(capped.size() == 2, "bounded line split stops at the line cap");
  Expect(capped[0] == "l1" && capped[1] == "l2", "bounded line split keeps leading lines");
  Expect(microide::util::SplitLineViews(lines, 0).empty(), "a zero line cap yields no lines");
}

void TestStringUtilDecodeLinesSinglePassRegression() {
  // Trailing newline keeps a final empty line; line ending detection must agree
  // with the dominant style after the single-pass rewrite.
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

// MeasureLines exists so a caller that wants only "how many lines" and "how long
// is the last one" does not split the text to find out. It is only correct while
// it agrees with SplitLines/SplitLineViews on every input, so assert the
// agreement rather than the two numbers, and pin the mixed/degenerate cases the
// surround path can actually hand it.
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
    // The normalized round trip the surround path used to take must agree too:
    // that is the exact expression MeasureLines replaced.
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

void TestStringUtilUnicodeCaseFold() {
  using microide::util::SimpleFoldCodepoint;
  using microide::util::Utf8CaseFold;
  using microide::util::Utf8IsIdentifierCodepoint;
  using microide::util::Utf8QueryHasCaseVariation;

  // ASCII folds toward lowercase; non-letters untouched.
  Expect(SimpleFoldCodepoint(U'A') == U'a', "ASCII A folds to a");
  Expect(SimpleFoldCodepoint(U'z') == U'z', "ASCII z unchanged");
  Expect(SimpleFoldCodepoint(U'5') == U'5', "digit unchanged");

  // Covered scripts fold uppercase to lowercase and are idempotent on lowercase.
  Expect(SimpleFoldCodepoint(0x00C9) == 0x00E9, "É folds to é");        // Latin-1
  Expect(SimpleFoldCodepoint(0x00E9) == 0x00E9, "é is stable");
  Expect(SimpleFoldCodepoint(0x00C4) == 0x00E4, "Ä folds to ä");
  Expect(SimpleFoldCodepoint(0x0100) == 0x0101, "Ā folds to ā");        // Latin Ext-A even-upper
  Expect(SimpleFoldCodepoint(0x0139) == 0x013A, "Ĺ folds to ĺ");        // Latin Ext-A odd-upper
  Expect(SimpleFoldCodepoint(0x0391) == 0x03B1, "Greek Α folds to α");  // Greek
  Expect(SimpleFoldCodepoint(0x0394) == 0x03B4, "Greek Δ folds to δ");
  Expect(SimpleFoldCodepoint(0x0410) == 0x0430, "Cyrillic А folds to а");
  Expect(SimpleFoldCodepoint(0x042F) == 0x044F, "Cyrillic Я folds to я");
  Expect(SimpleFoldCodepoint(0x0401) == 0x0451, "Cyrillic Ё folds to ё");

  // Turkish dotted/dotless I is deliberately excluded (locale-sensitive).
  Expect(SimpleFoldCodepoint(0x0130) == 0x0130, "İ intentionally unfolded");
  Expect(SimpleFoldCodepoint(0x0131) == 0x0131, "ı intentionally unfolded");
  // The 0x3A2 Greek hole and × (0xD7) are not letters.
  Expect(SimpleFoldCodepoint(0x00D7) == 0x00D7, "× multiplication sign unchanged");

  // Whole-string folding matches across case for covered scripts.
  Expect(Utf8CaseFold("CafÉ") == Utf8CaseFold("café"),
         "mixed-case É matches lowercase é after folding");
  Expect(Utf8CaseFold("ПРИВЕТ") == Utf8CaseFold("привет"),
         "Cyrillic case-insensitive match after folding");
  // Malformed bytes are preserved verbatim (byte alignment for match mapping).
  Expect(Utf8CaseFold("A\xFF" "B") == std::string("a\xFF" "b"),
         "invalid byte 0xFF preserved between folded ASCII");

  // Smart-case detection is Unicode-aware.
  Expect(Utf8QueryHasCaseVariation("café") == false, "lowercase-only query has no case variation");
  Expect(Utf8QueryHasCaseVariation("cafÉ") == true, "É triggers case-sensitive smart-case");
  Expect(Utf8QueryHasCaseVariation("привет") == false, "lowercase Cyrillic has no case variation");
  Expect(Utf8QueryHasCaseVariation("Привет") == true, "uppercase Cyrillic triggers case-sensitivity");

  // Identifier classification spans multi-byte letters, excludes punctuation.
  Expect(Utf8IsIdentifierCodepoint(U'a'), "ascii letter is identifier content");
  Expect(Utf8IsIdentifierCodepoint(U'_'), "underscore is identifier content");
  Expect(Utf8IsIdentifierCodepoint(0x00E9), "é is identifier content");
  Expect(Utf8IsIdentifierCodepoint(0x53D8), "CJK 变 is identifier content");
  Expect(!Utf8IsIdentifierCodepoint(U' '), "space is not identifier content");
  Expect(!Utf8IsIdentifierCodepoint(0x2013), "en-dash is not identifier content");
}


// LeadingByteRun reads eight bytes at a time, so the cases that matter are the
// run ending at every offset inside a word, on a word boundary, and in the
// sub-word tail — plus the all-matching span, which is the branch the indent
// scan uses to recognise a blank line.
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
        // A byte that differs from the searched one in every case above.
        text[at] = byte == 'z' ? 'y' : 'z';
        Expect(util::LeadingByteRun(text, byte) == reference(text, byte),
               "the run must end at the first non-matching byte, at every offset");
        Expect(util::LeadingByteRun(text, byte) == at,
               "the run length is the offset of the first non-matching byte");
      }
    }
  }
  // A high byte must terminate the run like any other non-match (the word-at-a-
  // time trick must not confuse the sign bit for a match).
  std::string high(20, ' ');
  high[13] = static_cast<char>(0xC3);
  Expect(util::LeadingByteRun(high, ' ') == 13, "a non-ASCII byte ends the run");
}

// FirstNonAsciiOrByte reads eight bytes at a time, so the cases that matter are
// exactly the ones a handful of literals miss: the target byte landing at every
// offset inside a word, on a word boundary, and in the sub-word tail. Check it
// against a plain byte-at-a-time reference at every length and every position,
// for both bytes production passes it ('\t' for layout, '\0' for encoding).
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
        // The searched-for byte itself.
        std::string with_target = plain;
        with_target[at] = target;
        Expect(util::FirstNonAsciiOrByte(with_target, target) == reference(with_target, target),
               "the target byte is found at every offset");
        // A high byte, which must be reported even though it is not the target.
        std::string with_high = plain;
        with_high[at] = static_cast<char>(0xC3);
        Expect(util::FirstNonAsciiOrByte(with_high, target) == reference(with_high, target),
               "a non-ASCII byte is found at every offset");
        // A high byte AFTER the target: the earlier of the two must win.
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

}  // namespace

// The falsey/truthy token sets are shared by the env-var tracing switches and by
// bool-typed settings (workspace::SettingFlagEnabled). Before consolidation the
// settings copy was case-SENSITIVE and omitted "no", so `FALSE` and `no` both
// read as ENABLED — the opposite of what the user wrote.
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
  // Neither set: an arbitrary payload (a log path, a mode name) is not a boolean.
  // Callers that accept such values rely on the two predicates being distinct.
  for (const std::string_view other : {"", "word", "/tmp/dap.log", "offset", "nope", "falsey"}) {
    Expect(!IsFalseyToken(other) && !IsTruthyToken(other),
           std::string("`") + std::string(other) + "` is neither falsey nor truthy");
  }
  // SettingFlagEnabled is "enabled unless falsey", so a non-boolean payload
  // ("word" for editor.wrap) stays enabled.
  Expect(!IsFalseyToken("word"), "a non-boolean setting payload must not read as disabled");
}

// The ASCII classifiers replaced `<cctype>` at every byte-scanning call site.
// Two properties have to hold for that swap to be safe: they must agree with the
// "C" locale over the ASCII range (so no parser changed meaning), and they must
// reject every byte >= 0x80 (so UTF-8 continuation bytes can never be classified
// as word/identifier content the way a non-C locale's `isalnum` would).
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

  // Exact "C" locale isspace set: space plus \t \n \v \f \r.
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

  // No byte outside ASCII is ever classified as content. This is the property a
  // locale-sensitive `<cctype>` could violate (e.g. Latin-1 letters under a
  // non-C locale), which would silently move word/identifier boundaries.
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
  // The first-byte fast reject must not skip a later occurrence: 'a' appears
  // three times before the only real match.
  Expect(ContainsCaseInsensitiveAscii("ax ay aZq", "azq"),
         "a repeated first byte does not hide a later match");
  Expect(!ContainsCaseInsensitiveAscii("aaaa", "aab "), "a near-miss tail still rejects");
}

// Ids and names are now expanded from one MICROIDE_PERF_COUNTERS list, so the
// old failure mode — inserting an id without inserting its name at the matching
// position, silently relabelling every counter after that point — is gone by
// construction and the positional anchors this test used to pin are no longer
// meaningful. What the macro cannot check is that two entries did not get the
// same *name*: a copy-pasted row compiles, and then two subsystems' numbers land
// under one label in every readout. That, and the naming convention, is what is
// left to test.
void TestPerformanceCounterNamesStayAlignedWithIds() {
  using microide::util::PerfCounterId;
  using microide::util::PerformanceCounterName;

  std::vector<std::string_view> all;
  for (std::size_t i = 0; i < microide::util::kPerfCounterCount; ++i) {
    const std::string_view name = PerformanceCounterName(static_cast<PerfCounterId>(i));
    Expect(!name.empty(), "every perf counter id must have a name");
    // "<subsystem>.<event>": the dump groups by this prefix, and a name with no
    // dot sorts into nobody's group.
    const std::size_t dot = name.find('.');
    Expect(dot != std::string_view::npos && dot > 0 && dot + 1 < name.size(),
           "perf counter names must be <subsystem>.<event>");
    all.push_back(name);
  }
  std::sort(all.begin(), all.end());
  Expect(std::adjacent_find(all.begin(), all.end()) == all.end(),
         "perf counter names must be unique");
}

// The aggregating mode is the one that has to be right: a self-time column that
// double-counts nested scopes would rank every outer scope above the inner one
// actually burning the time, which is the exact mistake the summary exists to
// avoid.
void TestTraceChannelAggregatesSelfTimeBelowChildren() {
  using microide::util::TraceChannel;
  using microide::util::TraceScope;

  ::setenv("MICROIDE_TEST_TRACE_AGGREGATE", "1", 1);
  TraceChannel channel("test", /*stream_env=*/nullptr, "MICROIDE_TEST_TRACE_AGGREGATE",
                       /*min_duration_env=*/nullptr);
  Expect(channel.AggregateEnabled(), "aggregate mode follows its env flag");
  Expect(!channel.StreamEnabled(), "a null stream env leaves streaming off");

  {
    TraceScope outer(channel, "outer");
    for (int i = 0; i < 3; ++i) {
      TraceScope inner(channel, "inner");
      // Busy-wait rather than sleep: sleeping is not guaranteed to advance the
      // steady clock by the requested amount on a loaded machine, and the
      // assertions below only need inner to dominate outer.
      const auto start = std::chrono::steady_clock::now();
      while (std::chrono::steady_clock::now() - start < std::chrono::milliseconds(2)) {
      }
    }
  }

  const std::vector<TraceChannel::AggregateEntry> entries = channel.AggregateSnapshot();
  Expect(entries.size() == 2, "one entry per distinct label");
  // Ranked by self time, so the child that burned the time must come first even
  // though the parent's total wall time is larger.
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

// Labels embed paths and counts, so a long session can mint unbounded distinct
// labels. The cap must fold the excess into one bucket rather than growing the
// map forever — an instrumentation leak is worse than the pitfall it hunts.
void TestTraceChannelCapsDistinctLabels() {
  using microide::util::TraceChannel;
  using microide::util::TraceScope;

  ::setenv("MICROIDE_TEST_TRACE_CAP", "1", 1);
  TraceChannel channel("test", /*stream_env=*/nullptr, "MICROIDE_TEST_TRACE_CAP",
                       /*min_duration_env=*/nullptr);

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

// Nesting depth used to live in one shared, mutex-guarded int, so a scope on a
// background thread shifted the indentation of a concurrent main-thread scope
// and both threads' self-time attribution. Depth is per-thread now.
void TestTraceChannelKeepsNestingPerThread() {
  using microide::util::TraceChannel;
  using microide::util::TraceScope;

  ::setenv("MICROIDE_TEST_TRACE_THREADS", "1", 1);
  TraceChannel channel("test", /*stream_env=*/nullptr, "MICROIDE_TEST_TRACE_THREADS",
                       /*min_duration_env=*/nullptr);

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
  // The worker's scopes are not nested inside `main-outer` — they ran on another
  // thread — so none of their time may be subtracted from its self time.
  Expect(outer->self_ms == outer->total_ms,
         "another thread's scopes are not charged as this scope's children");

  // Main-thread attribution: a background scope costs the user no frames, so it
  // must not be counted against the thread whose stalls they feel. Ranking on
  // self time alone put a 161 ms background tree walk above a 32 ms render stall.
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

// ScopeLabel replaced ten hand-rolled `if (Enabled()) label += ...` chains. Its
// whole reason to exist is that a missed guard is a heap allocation per call in
// production, so the off-path contract is the assertion that matters.
void TestScopeLabelBuildsNothingWhenTheChannelIsOff() {
  using microide::util::PerformanceTrace;
  using microide::util::TraceChannel;

  ::unsetenv("MICROIDE_TEST_LABEL_OFF");
  TraceChannel off("test", "MICROIDE_TEST_LABEL_OFF", /*aggregate_env=*/nullptr,
                   /*min_duration_env=*/nullptr);
  Expect(!off.Enabled(), "an unset env leaves the channel off");

  PerformanceTrace::ScopeLabel label(off, "Some::Scope");
  label.Field("path", std::filesystem::path("/a/very/long/path/that/would/allocate"));
  label.Field("index", 7);
  Expect(label.View().empty(), "an off channel builds no label text at all");

  ::setenv("MICROIDE_TEST_LABEL_ON", "1", 1);
  TraceChannel on("test", "MICROIDE_TEST_LABEL_ON", /*aggregate_env=*/nullptr,
                  /*min_duration_env=*/nullptr);
  PerformanceTrace::ScopeLabel built(on, "Some::Scope");
  built.Field("path", std::filesystem::path("/a/b"));
  built.Field("index", 7);
  Expect(built.View() == "Some::Scope(path=/a/b,index=7)",
         "fields are comma-joined inside one parenthesized group");
  // View() must be idempotent: a caller that reads it twice (or logs it before
  // passing it to a Scope) must not get a second closing paren.
  Expect(built.View() == "Some::Scope(path=/a/b,index=7)", "View() is idempotent");

  PerformanceTrace::ScopeLabel bare(on, "Bare::Scope");
  Expect(bare.View() == "Bare::Scope", "a label with no fields gains no parentheses");
  ::unsetenv("MICROIDE_TEST_LABEL_ON");
}

void RegisterStringUtilTests(std::vector<TestCase>& tests) {
  AddTest(tests, "StringUtil/AsciiClassifiersAreLocaleIndependent",
          TestStringUtilAsciiClassifiersAreLocaleIndependent);
  AddTest(tests, "StringUtil/ContainsCaseInsensitiveAscii",
          TestStringUtilContainsCaseInsensitiveAscii);
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

}  // namespace microide::tests
