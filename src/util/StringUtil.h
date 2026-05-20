#pragma once

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace microide::util {

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
bool IsUtf8ContinuationByte(unsigned char byte);
std::size_t PreviousUtf8Boundary(std::string_view text, std::size_t offset);
std::size_t NextUtf8Boundary(std::string_view text, std::size_t offset);
bool RemoveLastUtf8Codepoint(std::string* text);
std::size_t Utf8ByteOffsetForCodepointCount(std::string_view text, std::size_t codepoint_count);
std::size_t Utf8CodepointCount(std::string_view text);
bool IsValidUtf8(std::string_view content);
std::string NormalizeLineEndings(std::string_view text);
void TrimTrailingLineEndings(std::string* text);
// Trim leading and trailing ASCII whitespace (`std::isspace` semantics on the
// unsigned char value: space, tab, CR, LF, VT, FF).
std::string TrimAsciiWhitespace(std::string_view text);
// Lowercase the ASCII A-Z range, leaving every other byte untouched. Safe for
// UTF-8 input because the multi-byte sequences never include 'A'..'Z'.
std::string ToLowerAscii(std::string_view text);
// Replace each run of ASCII whitespace with a single space. Leading whitespace
// is dropped; trailing whitespace, if any, is also dropped.
std::string CollapseAsciiWhitespace(std::string_view text);
LineEnding DetectLineEnding(std::string_view text);
DecodedText DecodeLines(std::string_view content);
std::string_view LineEndingSeparator(LineEnding line_ending);
std::string LineEndingLabel(LineEnding line_ending);
LineEnding ParseLineEndingLabel(std::string_view text);
std::vector<std::string> SplitLines(std::string_view content);
std::string JoinLines(std::span<const std::string> lines, std::string_view separator);
std::string SerializeLines(std::span<const std::string> lines, LineEnding line_ending);

}  // namespace microide::util
