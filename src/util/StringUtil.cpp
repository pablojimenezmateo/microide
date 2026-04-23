#include "util/StringUtil.h"

#include <algorithm>

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
