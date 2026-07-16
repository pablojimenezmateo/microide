#include "TestSupport.h"

#include "compare/CompareModel.h"
#include "util/Hex.h"
#include "util/SaturatingMath.h"
#include "util/StringUtil.h"

#include <cstdint>
#include <limits>

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

}  // namespace

void RegisterStringUtilTests(std::vector<TestCase>& tests) {
  AddTest(tests, "StringUtil/UnicodeCaseFold", TestStringUtilUnicodeCaseFold);
  AddTest(tests, "StringUtil/AppendUtf8EncodesAllSequenceLengths",
          TestStringUtilAppendUtf8EncodesAllSequenceLengths);
  AddTest(tests, "Util/SaturatingMath", TestSaturatingMath);
  AddTest(tests, "StringUtil/IsAllAsciiDigits", TestStringUtilIsAllAsciiDigits);
  AddTest(tests, "StringUtil/SplitAsciiWhitespace", TestStringUtilSplitAsciiWhitespace);
  AddTest(tests, "StringUtil/BoundedSplitStopsAtCap", TestStringUtilBoundedSplitStopsAtCap);
  AddTest(tests, "StringUtil/DecodeLinesSinglePassRegression",
          TestStringUtilDecodeLinesSinglePassRegression);
  AddTest(tests, "Hex/DigitAndByteParsing", TestHexDigitAndByteParsing);
  AddTest(tests, "Hex/DecodeHexColor", TestDecodeHexColor);
  AddTest(tests, "Hex/PercentDecode", TestPercentDecode);
  AddTest(tests, "StringUtil/CollapseWhitespaceTracksMatchRange",
          TestStringUtilCollapseWhitespaceTracksMatchRange);
  AddTest(tests, "StringUtil/DecodeUtf8CodepointRejectsInvalidScalars",
          TestStringUtilDecodeUtf8CodepointRejectsInvalidScalars);
  AddTest(tests, "StringUtil/Utf8SequenceLengthHandlesAsciiAndEmoji",
          TestStringUtilUtf8SequenceLengthHandlesAsciiAndEmoji);
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
  AddTest(tests, "StringUtil/CompareModelHandlesCrLfInputViaSharedSplitter",
          TestCompareModelHandlesCrLfInputViaSharedSplitter);
}

}  // namespace microide::tests
