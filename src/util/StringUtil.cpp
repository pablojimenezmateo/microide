#include "util/StringUtil.h"

namespace microide::util {

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
      if ((byte & 0xC0u) != 0x80u) {
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

std::vector<std::string> SplitLines(std::string_view content) {
  std::vector<std::string> lines;
  std::size_t line_start = 0;
  for (std::size_t i = 0; i < content.size(); ++i) {
    if (content[i] != '\r' && content[i] != '\n') {
      continue;
    }

    lines.emplace_back(content.substr(line_start, i - line_start));
    if (content[i] == '\r' && i + 1 < content.size() && content[i + 1] == '\n') {
      ++i;
    }
    line_start = i + 1;
  }

  if (line_start <= content.size()) {
    lines.emplace_back(content.substr(line_start));
  }
  if (lines.empty()) {
    lines.push_back("");
  }
  return lines;
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

}  // namespace microide::util
