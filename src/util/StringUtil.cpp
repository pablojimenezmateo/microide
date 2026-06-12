#include "util/StringUtil.h"

#include <algorithm>
#include <cctype>

namespace microide::util {

namespace {

std::size_t ClampUtf8Offset(std::string_view text, std::size_t offset) {
  return std::min(offset, text.size());
}

}  // namespace

std::size_t Utf8SequenceLength(unsigned char lead_byte) {
  if (lead_byte <= 0x7F) {
    return 1;
  }
  if (lead_byte >= 0xC2 && lead_byte <= 0xDF) {
    return 2;
  }
  if (lead_byte >= 0xE0 && lead_byte <= 0xEF) {
    return 3;
  }
  if (lead_byte >= 0xF0 && lead_byte <= 0xF4) {
    return 4;
  }
  return 0;
}

bool IsUtf8ContinuationByte(unsigned char byte) {
  return (byte & 0xC0u) == 0x80u;
}

char32_t DecodeUtf8Codepoint(std::string_view glyph) {
  if (glyph.empty()) {
    return 0xFFFD;
  }
  const unsigned char lead = static_cast<unsigned char>(glyph[0]);
  const std::size_t length = Utf8SequenceLength(lead);
  if (length == 0 || length > glyph.size()) {
    return 0xFFFD;
  }
  if (length == 1) {
    return lead;
  }
  static constexpr unsigned char kLeadMask[] = {0, 0, 0x1F, 0x0F, 0x07};
  char32_t codepoint = lead & kLeadMask[length];
  for (std::size_t i = 1; i < length; ++i) {
    const unsigned char byte = static_cast<unsigned char>(glyph[i]);
    if (!IsUtf8ContinuationByte(byte)) {
      return 0xFFFD;
    }
    codepoint = (codepoint << 6) | (byte & 0x3Fu);
  }
  return codepoint;
}

namespace {

struct CodepointRange {
  char32_t low;
  char32_t high;
};

bool InSortedRanges(char32_t cp, const CodepointRange* ranges, std::size_t count) {
  std::size_t lo = 0;
  std::size_t hi = count;
  while (lo < hi) {
    const std::size_t mid = lo + (hi - lo) / 2;
    if (cp < ranges[mid].low) {
      hi = mid;
    } else if (cp > ranges[mid].high) {
      lo = mid + 1;
    } else {
      return true;
    }
  }
  return false;
}

}  // namespace

int CodepointDisplayWidth(char32_t codepoint) {
  // Fast path: printable ASCII is always one column. This is the hot case for
  // typical terminal output, so it skips the range searches below.
  if (codepoint >= 0x20 && codepoint < 0x7F) {
    return 1;
  }
  // NUL and C0/C1 control codes contribute no advance (they are normally
  // handled before reaching the grid, but stay defensive here).
  if (codepoint == 0) {
    return 0;
  }
  if (codepoint < 0x20 || (codepoint >= 0x7F && codepoint < 0xA0)) {
    return 0;
  }

  // Zero-width: combining marks, joiners, variation selectors. Sorted by `low`.
  static constexpr CodepointRange kZeroWidth[] = {
      {0x0300, 0x036F}, {0x0483, 0x0489}, {0x0591, 0x05BD}, {0x05BF, 0x05BF},
      {0x05C1, 0x05C2}, {0x05C4, 0x05C5}, {0x0610, 0x061A}, {0x064B, 0x065F},
      {0x0670, 0x0670}, {0x06D6, 0x06DC}, {0x06DF, 0x06E4}, {0x06E7, 0x06E8},
      {0x06EA, 0x06ED}, {0x0711, 0x0711}, {0x0730, 0x074A}, {0x07A6, 0x07B0},
      {0x07EB, 0x07F3}, {0x0816, 0x0819}, {0x081B, 0x0823}, {0x0825, 0x0827},
      {0x0829, 0x082D}, {0x0859, 0x085B}, {0x08E3, 0x0903}, {0x093A, 0x093C},
      {0x093E, 0x094F}, {0x0951, 0x0957}, {0x0962, 0x0963}, {0x0E31, 0x0E31},
      {0x0E34, 0x0E3A}, {0x0E47, 0x0E4E}, {0x200B, 0x200F}, {0x202A, 0x202E},
      {0x2060, 0x2064}, {0x20D0, 0x20F0}, {0xFE00, 0xFE0F}, {0xFE20, 0xFE2F},
  };
  if (InSortedRanges(codepoint, kZeroWidth, std::size(kZeroWidth))) {
    return 0;
  }

  // East Asian Wide / Fullwidth + emoji presentation. Sorted by `low`.
  static constexpr CodepointRange kWide[] = {
      {0x1100, 0x115F},   {0x2329, 0x232A},   {0x2E80, 0x303E},   {0x3041, 0x33FF},
      {0x3400, 0x4DBF},   {0x4E00, 0x9FFF},   {0xA000, 0xA4CF},   {0xAC00, 0xD7A3},
      {0xF900, 0xFAFF},   {0xFE10, 0xFE19},   {0xFE30, 0xFE6F},   {0xFF00, 0xFF60},
      {0xFFE0, 0xFFE6},   {0x1F004, 0x1F004}, {0x1F0CF, 0x1F0CF}, {0x1F18E, 0x1F18E},
      {0x1F191, 0x1F19A}, {0x1F200, 0x1F2FF}, {0x1F300, 0x1F64F}, {0x1F680, 0x1F6FF},
      {0x1F900, 0x1F9FF}, {0x1FA70, 0x1FAFF}, {0x20000, 0x3FFFD},
  };
  if (InSortedRanges(codepoint, kWide, std::size(kWide))) {
    return 2;
  }

  return 1;
}

std::size_t Utf8SequenceLength(std::string_view text, std::size_t offset) {
  if (offset >= text.size()) {
    return 0;
  }

  const unsigned char lead = static_cast<unsigned char>(text[offset]);
  if (lead <= 0x7F) {
    return 1;
  }

  auto continuation = [&](std::size_t count) {
    if (offset + count >= text.size()) {
      return false;
    }
    for (std::size_t i = 1; i <= count; ++i) {
      const unsigned char byte = static_cast<unsigned char>(text[offset + i]);
      if (!IsUtf8ContinuationByte(byte)) {
        return false;
      }
    }
    return true;
  };

  if (lead >= 0xC2 && lead <= 0xDF && continuation(1)) {
    return 2;
  }
  if (lead == 0xE0 && continuation(2)) {
    const unsigned char second = static_cast<unsigned char>(text[offset + 1]);
    if (second >= 0xA0 && second <= 0xBF) {
      return 3;
    }
  }
  if (((lead >= 0xE1 && lead <= 0xEC) || (lead >= 0xEE && lead <= 0xEF)) &&
      continuation(2)) {
    return 3;
  }
  if (lead == 0xED && continuation(2)) {
    const unsigned char second = static_cast<unsigned char>(text[offset + 1]);
    if (second >= 0x80 && second <= 0x9F) {
      return 3;
    }
  }
  if (lead == 0xF0 && continuation(3)) {
    const unsigned char second = static_cast<unsigned char>(text[offset + 1]);
    if (second >= 0x90 && second <= 0xBF) {
      return 4;
    }
  }
  if (lead >= 0xF1 && lead <= 0xF3 && continuation(3)) {
    return 4;
  }
  if (lead == 0xF4 && continuation(3)) {
    const unsigned char second = static_cast<unsigned char>(text[offset + 1]);
    if (second >= 0x80 && second <= 0x8F) {
      return 4;
    }
  }

  return 1;
}

std::size_t PreviousUtf8Boundary(std::string_view text, std::size_t offset) {
  offset = ClampUtf8Offset(text, offset);
  if (offset == 0) {
    return 0;
  }
  std::size_t previous = offset - 1;
  while (previous > 0 &&
         IsUtf8ContinuationByte(static_cast<unsigned char>(text[previous]))) {
    --previous;
  }
  return previous;
}

std::size_t NextUtf8Boundary(std::string_view text, std::size_t offset) {
  offset = ClampUtf8Offset(text, offset);
  if (offset >= text.size()) {
    return text.size();
  }
  return std::min(text.size(), offset + Utf8SequenceLength(text, offset));
}

bool RemoveLastUtf8Codepoint(std::string* text) {
  if (text == nullptr || text->empty()) {
    return false;
  }
  text->erase(PreviousUtf8Boundary(*text, text->size()));
  return true;
}

std::size_t Utf8ByteOffsetForCodepointCount(std::string_view text,
                                            std::size_t codepoint_count) {
  std::size_t offset = 0;
  for (std::size_t count = 0; count < codepoint_count && offset < text.size(); ++count) {
    offset += Utf8SequenceLength(text, offset);
  }
  return offset;
}

std::size_t Utf8CodepointCount(std::string_view text) {
  std::size_t count = 0;
  for (std::size_t offset = 0; offset < text.size();) {
    offset += Utf8SequenceLength(text, offset);
    ++count;
  }
  return count;
}

bool IsValidUtf8(std::string_view content) {
  for (std::size_t offset = 0; offset < content.size();) {
    const std::size_t sequence_length = Utf8SequenceLength(content, offset);
    if (sequence_length == 0) {
      return false;
    }

    const unsigned char lead = static_cast<unsigned char>(content[offset]);
    if (lead > 0x7F && sequence_length == 1) {
      return false;
    }

    offset += sequence_length;
  }
  return true;
}

std::string NormalizeLineEndings(std::string_view text) {
  std::string normalized;
  normalized.reserve(text.size());
  for (char character : text) {
    if (character != '\r') {
      normalized.push_back(character);
    }
  }
  return normalized;
}

void TrimTrailingLineEndings(std::string* text) {
  if (text == nullptr) {
    return;
  }
  while (!text->empty() && (text->back() == '\n' || text->back() == '\r')) {
    text->pop_back();
  }
}

std::string ToLowerAscii(std::string_view text) {
  std::string lowered(text);
  for (char& c : lowered) {
    if (c >= 'A' && c <= 'Z') {
      c = static_cast<char>(c + ('a' - 'A'));
    }
  }
  return lowered;
}

std::string CollapseAsciiWhitespace(std::string_view text) {
  std::string collapsed;
  collapsed.reserve(text.size());
  bool saw_whitespace = false;
  for (unsigned char c : text) {
    if (std::isspace(c)) {
      saw_whitespace = !collapsed.empty();
      continue;
    }
    if (saw_whitespace) {
      collapsed.push_back(' ');
      saw_whitespace = false;
    }
    collapsed.push_back(static_cast<char>(c));
  }
  return collapsed;
}

std::string TrimAsciiWhitespace(std::string_view text) {
  std::size_t start = 0;
  while (start < text.size() && std::isspace(static_cast<unsigned char>(text[start]))) {
    ++start;
  }
  std::size_t end = text.size();
  while (end > start && std::isspace(static_cast<unsigned char>(text[end - 1]))) {
    --end;
  }
  return std::string(text.substr(start, end - start));
}

LineEnding DetectLineEnding(std::string_view text) {
  std::size_t crlf_count = 0;
  std::size_t lf_count = 0;
  std::size_t cr_count = 0;
  for (std::size_t i = 0; i < text.size(); ++i) {
    if (text[i] == '\r') {
      if (i + 1 < text.size() && text[i + 1] == '\n') {
        ++crlf_count;
        ++i;
      } else {
        ++cr_count;
      }
    } else if (text[i] == '\n') {
      ++lf_count;
    }
  }

  if (crlf_count >= lf_count && crlf_count >= cr_count && crlf_count > 0) {
    return LineEnding::CRLF;
  }
  if (lf_count >= cr_count && lf_count > 0) {
    return LineEnding::LF;
  }
  if (cr_count > 0) {
    return LineEnding::CR;
  }
  return LineEnding::LF;
}

DecodedText DecodeLines(std::string_view content) {
  DecodedText decoded;

  std::size_t crlf_count = 0;
  std::size_t lf_count = 0;
  std::size_t cr_count = 0;
  for (std::size_t i = 0; i < content.size(); ++i) {
    if (content[i] == '\r') {
      if (i + 1 < content.size() && content[i + 1] == '\n') {
        ++crlf_count;
        ++i;
      } else {
        ++cr_count;
      }
    } else if (content[i] == '\n') {
      ++lf_count;
    }
  }

  const std::size_t present_styles =
      (crlf_count > 0 ? 1 : 0) + (lf_count > 0 ? 1 : 0) + (cr_count > 0 ? 1 : 0);
  decoded.mixed_line_endings = present_styles > 1;
  decoded.line_ending = DetectLineEnding(content);

  std::size_t line_start = 0;
  for (std::size_t i = 0; i < content.size(); ++i) {
    if (content[i] != '\r' && content[i] != '\n') {
      continue;
    }

    decoded.lines.emplace_back(content.substr(line_start, i - line_start));
    if (content[i] == '\r' && i + 1 < content.size() && content[i + 1] == '\n') {
      ++i;
    }
    line_start = i + 1;
  }

  if (line_start <= content.size()) {
    decoded.lines.emplace_back(content.substr(line_start));
  }
  if (decoded.lines.empty()) {
    decoded.lines.push_back("");
  }
  return decoded;
}

std::string_view LineEndingSeparator(LineEnding line_ending) {
  switch (line_ending) {
    case LineEnding::CRLF:
      return "\r\n";
    case LineEnding::CR:
      return "\r";
    case LineEnding::LF:
    default:
      return "\n";
  }
}

std::string LineEndingLabel(LineEnding line_ending) {
  switch (line_ending) {
    case LineEnding::CRLF:
      return "crlf";
    case LineEnding::CR:
      return "cr";
    case LineEnding::LF:
    default:
      return "lf";
  }
}

LineEnding ParseLineEndingLabel(std::string_view text) {
  if (text == "crlf") {
    return LineEnding::CRLF;
  }
  if (text == "cr") {
    return LineEnding::CR;
  }
  return LineEnding::LF;
}

std::vector<std::string> SplitLines(std::string_view content) {
  return DecodeLines(content).lines;
}

std::string JoinLines(std::span<const std::string> lines, std::string_view separator) {
  if (lines.empty()) {
    return {};
  }

  std::size_t size = 0;
  for (const std::string& line : lines) {
    size += line.size();
  }
  size += separator.size() * (lines.size() - 1);

  std::string joined;
  joined.reserve(size);
  for (std::size_t i = 0; i < lines.size(); ++i) {
    if (i > 0) {
      joined += separator;
    }
    joined += lines[i];
  }
  return joined;
}

std::string SerializeLines(std::span<const std::string> lines, LineEnding line_ending) {
  return JoinLines(lines, LineEndingSeparator(line_ending));
}

}  // namespace microide::util
