#pragma once

#include <optional>
#include <cstddef>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "util/SaturatingMath.h"

namespace microide::util {

inline constexpr std::string_view kUtf8ReplacementChar = "\xEF\xBF\xBD";

inline constexpr std::string_view kUtf8Bom = "\xEF\xBB\xBF";

[[nodiscard]] inline bool HasUtf8Bom(std::string_view text) {
  return text.starts_with(kUtf8Bom);
}

enum class LineEnding {
  LF,
  CRLF,
  CR,
};

struct DecodedText {
  std::vector<std::string> lines;
  LineEnding line_ending = LineEnding::LF;
  bool mixed_line_endings = false;
};

std::size_t Utf8SequenceLength(unsigned char lead_byte);
std::size_t Utf8SequenceLength(std::string_view text, std::size_t offset);

std::size_t FirstNonAsciiOrByte(std::string_view text, char also_match);

[[nodiscard]] bool IsAllAscii(std::string_view text);

std::size_t LeadingByteRun(std::string_view text, char byte);
char32_t DecodeUtf8Codepoint(std::string_view glyph);
void AppendUtf8(std::string& out, char32_t codepoint);

inline void AppendUnsigned(std::string& out, std::size_t value) {
  char buffer[24];
  char* const end = buffer + sizeof(buffer);
  char* cursor = end;
  do {
    *--cursor = static_cast<char>('0' + (value % 10));
    value /= 10;
  } while (value != 0);
  out.append(cursor, static_cast<std::size_t>(end - cursor));
}

[[nodiscard]] inline constexpr bool IsAsciiSpace(unsigned char c) {
  return c == ' ' || (c >= 0x09 && c <= 0x0D);
}
[[nodiscard]] inline constexpr bool IsAsciiDigit(unsigned char c) {
  return c >= '0' && c <= '9';
}
[[nodiscard]] inline constexpr bool IsAsciiUpper(unsigned char c) {
  return c >= 'A' && c <= 'Z';
}
[[nodiscard]] inline constexpr bool IsAsciiLower(unsigned char c) {
  return c >= 'a' && c <= 'z';
}
[[nodiscard]] inline constexpr bool IsAsciiAlpha(unsigned char c) {
  return IsAsciiUpper(c) || IsAsciiLower(c);
}
[[nodiscard]] inline constexpr bool IsAsciiAlnum(unsigned char c) {
  return IsAsciiAlpha(c) || IsAsciiDigit(c);
}
[[nodiscard]] inline constexpr bool IsAsciiHexDigit(unsigned char c) {
  return IsAsciiDigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}
[[nodiscard]] inline constexpr char ToLowerAsciiChar(char c) {
  return IsAsciiUpper(static_cast<unsigned char>(c)) ? static_cast<char>(c + ('a' - 'A')) : c;
}
[[nodiscard]] inline constexpr char ToUpperAsciiChar(char c) {
  return IsAsciiLower(static_cast<unsigned char>(c)) ? static_cast<char>(c - ('a' - 'A')) : c;
}

[[nodiscard]] inline bool EqualsAsciiCaseInsensitive(std::string_view text,
                                                     std::string_view lowercase_token) {
  if (text.size() != lowercase_token.size()) {
    return false;
  }
  for (std::size_t i = 0; i < text.size(); ++i) {
    if (ToLowerAsciiChar(text[i]) != lowercase_token[i]) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] inline bool IsFalseyToken(std::string_view text) {
  if (text.size() > 5) {
    return false;
  }
  return EqualsAsciiCaseInsensitive(text, "0") ||
         EqualsAsciiCaseInsensitive(text, "false") ||
         EqualsAsciiCaseInsensitive(text, "no") ||
         EqualsAsciiCaseInsensitive(text, "off");
}

[[nodiscard]] inline bool IsTruthyToken(std::string_view text) {
  if (text.size() > 4) {
    return false;
  }
  return EqualsAsciiCaseInsensitive(text, "1") ||
         EqualsAsciiCaseInsensitive(text, "true") ||
         EqualsAsciiCaseInsensitive(text, "yes") ||
         EqualsAsciiCaseInsensitive(text, "on");
}
[[nodiscard]] inline std::optional<bool> ParseBoolToken(std::string_view text) {
  if (IsTruthyToken(text)) {
    return true;
  }
  if (IsFalseyToken(text)) {
    return false;
  }
  return std::nullopt;
}
int CodepointDisplayWidth(char32_t codepoint);
[[nodiscard]] inline std::size_t GridCellWidth(char32_t codepoint) {
  if (codepoint < 0x300 || codepoint == 0xFFFD) {
    return 1;
  }
  const int width = CodepointDisplayWidth(codepoint);
  return width <= 0 ? 0 : static_cast<std::size_t>(width);
}
std::size_t GridCellCount(std::string_view text);
bool IsUtf8ContinuationByte(unsigned char byte);
std::size_t PreviousUtf8Boundary(std::string_view text, std::size_t offset);
std::size_t NextUtf8Boundary(std::string_view text, std::size_t offset);
std::size_t Utf8ByteBudgetPrefixLength(std::string_view text, std::size_t max_bytes);
bool TruncateUtf8ToByteBudget(std::string& text, std::size_t max_bytes);
bool RemoveLastUtf8Codepoint(std::string* text);
std::size_t Utf8ByteOffsetForCodepointCount(std::string_view text, std::size_t codepoint_count);
std::size_t Utf8CodepointCount(std::string_view text);
bool IsValidUtf8(std::string_view content);
bool ReplaceInvalidUtf8Bytes(std::string& text, char replacement = '?');
bool ReplaceInvalidUtf8WithReplacementCharacter(std::string& text);
std::string NormalizeLineEndings(std::string_view text);
void TrimTrailingLineEndings(std::string* text);
std::string_view TrimAsciiWhitespaceView(std::string_view text);
std::string TrimAsciiWhitespace(std::string_view text);
std::string ToLowerAscii(std::string_view text);
void ToLowerAsciiInto(std::string_view text, std::string& out);
bool QueryHasUppercaseAscii(std::string_view text);

char32_t SimpleFoldCodepoint(char32_t cp);
void Utf8CaseFoldInto(std::string_view text, std::string& out);
void Utf8CaseFoldAppend(std::string_view text, std::string& out);
std::string Utf8CaseFold(std::string_view text);
bool Utf8QueryHasCaseVariation(std::string_view text);
bool Utf8IsIdentifierCodepoint(char32_t cp);
std::size_t Utf8CodepointStartAt(std::string_view text, std::size_t index);
bool Utf8IdentifierCodepointAt(std::string_view text, std::size_t start, std::size_t* length);
std::size_t Utf8IdentifierRunStart(std::string_view text, std::size_t end);
std::size_t Utf8IdentifierRunEnd(std::string_view text, std::size_t start);
bool IsSearchWordByte(char c);
bool SearchMatchStandsAlone(std::string_view text, std::size_t start, std::size_t end);
bool ContainsCaseInsensitiveAscii(std::string_view haystack, std::string_view needle);
bool IsAllAsciiDigits(std::string_view text);
std::vector<std::string_view> SplitAsciiWhitespace(std::string_view text);
std::vector<std::string_view> SplitOnByte(std::string_view text, char delimiter,
                                          std::size_t max_records = static_cast<std::size_t>(-1));
std::vector<std::string_view> SplitNulDelimited(std::string_view text);
std::vector<std::string_view> SplitNulDelimited(std::string_view text, std::size_t max_records);
std::string CollapseAsciiWhitespace(std::string_view text);
std::string CollapseAsciiWhitespaceTrackingMatch(std::string_view text,
                                                 std::size_t match_start,
                                                 std::size_t match_end,
                                                 std::size_t* out_match_start,
                                                 std::size_t* out_match_length);
LineEnding DetectLineEnding(std::string_view text);
DecodedText DecodeLines(std::string_view content);
std::string_view LineEndingSeparator(LineEnding line_ending);
std::string LineEndingLabel(LineEnding line_ending);
LineEnding ParseLineEndingLabel(std::string_view text);
std::vector<std::string> SplitLines(std::string_view content);
struct LineShape {
  std::size_t count = 1;
  std::size_t last_line_size = 0;
};
LineShape MeasureLines(std::string_view content);
std::vector<std::string_view> SplitLineViews(std::string_view content);
void SplitLineViewsInto(std::string_view content, std::vector<std::string_view>& lines);
std::vector<std::string_view> SplitLineViews(std::string_view content, std::size_t max_lines);
std::string JoinLines(std::span<const std::string> lines, std::string_view separator);
std::string SerializeLines(std::span<const std::string> lines, LineEnding line_ending);

template <typename LinesLike>
std::string SerializeLinesStreaming(const LinesLike& lines, LineEnding line_ending) {
  const std::string_view separator = LineEndingSeparator(line_ending);
  const std::size_t count = lines.size();
  if (count == 0) {
    return {};
  }
  std::size_t total = 0;
  for (std::size_t i = 0; i < count; ++i) {
    total = SaturatingAdd(total, std::string_view(lines[i]).size());
  }
  for (std::size_t i = 1; i < count; ++i) {
    total = SaturatingAdd(total, separator.size());
  }
  std::string out;
  if (total != std::numeric_limits<std::size_t>::max()) {
    out.reserve(total);
  }
  for (std::size_t i = 0; i < count; ++i) {
    if (i != 0) {
      out += separator;
    }
    out += std::string_view(lines[i]);
  }
  return out;
}

int NaturalCompareIgnoreCase(std::string_view a, std::string_view b);

}
