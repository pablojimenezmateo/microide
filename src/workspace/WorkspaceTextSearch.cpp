#include "workspace/WorkspaceTextSearch.h"

#include <algorithm>
#include <cctype>
#include <string>

#include "util/StringUtil.h"

namespace microide::workspace {

bool StartsWith(std::string_view text, std::string_view prefix) {
  return text.substr(0, prefix.size()) == prefix;
}

bool EndsWith(std::string_view text, std::string_view suffix) {
  return text.size() >= suffix.size() &&
         text.substr(text.size() - suffix.size(), suffix.size()) == suffix;
}

std::string ToLower(std::string_view text) {
  std::string lowered(text);
  std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return lowered;
}

std::vector<std::string> SplitSyntaxLines(std::string_view text) {
  std::vector<std::string> lines;
  std::size_t start = 0;
  while (start < text.size()) {
    const std::size_t newline = text.find('\n', start);
    if (newline == std::string_view::npos) {
      lines.emplace_back(text.substr(start));
      break;
    }
    lines.emplace_back(text.substr(start, newline - start));
    start = newline + 1;
  }
  return lines;
}

std::string SerializeLines(const std::vector<std::string>& lines,
                           editor::TextViewport::LineEnding line_ending) {
  const std::string_view separator =
      line_ending == editor::TextViewport::LineEnding::CRLF ? std::string_view{"\r\n"}
      : line_ending == editor::TextViewport::LineEnding::CR   ? std::string_view{"\r"}
                                                              : std::string_view{"\n"};
  return util::JoinLines(lines, separator);
}

editor::TextViewport::LineEnding DetectLineEnding(std::string_view text) {
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
    return editor::TextViewport::LineEnding::CRLF;
  }
  if (lf_count >= cr_count && lf_count > 0) {
    return editor::TextViewport::LineEnding::LF;
  }
  if (cr_count > 0) {
    return editor::TextViewport::LineEnding::CR;
  }
  return editor::TextViewport::LineEnding::LF;
}

bool RemoveLastUtf8Codepoint(std::string* text) {
  if (text == nullptr || text->empty()) {
    return false;
  }

  std::size_t index = text->size();
  do {
    --index;
  } while (index > 0 &&
           (static_cast<unsigned char>((*text)[index]) & 0xC0u) == 0x80u);
  text->erase(index);
  return true;
}

std::size_t Utf8ByteOffsetForCodepointCount(std::string_view text, std::size_t codepoint_count) {
  std::size_t offset = 0;
  for (std::size_t count = 0; count < codepoint_count && offset < text.size(); ++count) {
    offset += util::Utf8SequenceLength(text, offset);
  }
  return offset;
}

std::size_t Utf8CodepointCount(std::string_view text) {
  std::size_t count = 0;
  for (std::size_t offset = 0; offset < text.size();) {
    offset += util::Utf8SequenceLength(text, offset);
    ++count;
  }
  return count;
}

std::string CollapseWhitespace(std::string_view text) {
  std::string collapsed;
  collapsed.reserve(text.size());
  bool space = false;
  for (char c : text) {
    if (std::isspace(static_cast<unsigned char>(c))) {
      space = !collapsed.empty();
      continue;
    }
    if (space) {
      collapsed.push_back(' ');
      space = false;
    }
    collapsed.push_back(c);
  }
  return collapsed;
}

bool QuerySupportsLiteralReplace(std::string_view query) {
  static constexpr std::string_view kRegexMetacharacters = R"(\.^$|()[]{}*+?)";
  return !query.empty() &&
         query.find_first_of(kRegexMetacharacters) == std::string_view::npos;
}

bool UsesCaseSensitiveLiteralMatch(std::string_view query) {
  return std::any_of(query.begin(), query.end(), [](unsigned char c) {
    return std::isupper(c);
  });
}

std::size_t ReplaceLiteralMatchesInText(std::string& content,
                                        std::string_view query,
                                        std::string_view replacement,
                                        bool case_sensitive) {
  if (content.empty() || query.empty()) {
    return 0;
  }

  std::size_t replacements = 0;
  std::size_t search_from = 0;
  if (case_sensitive) {
    while (true) {
      const std::size_t position = content.find(query, search_from);
      if (position == std::string::npos) {
        break;
      }
      content.replace(position, query.size(), replacement);
      search_from = position + replacement.size();
      ++replacements;
    }
    return replacements;
  }

  const std::string lowered_query = ToLower(query);
  std::string lowered_content = ToLower(content);
  const std::string lowered_replacement = ToLower(replacement);
  while (true) {
    const std::size_t position = lowered_content.find(lowered_query, search_from);
    if (position == std::string::npos) {
      break;
    }
    content.replace(position, query.size(), replacement);
    lowered_content.replace(position, query.size(), lowered_replacement);
    search_from = position + replacement.size();
    ++replacements;
  }
  return replacements;
}

std::vector<editor::SelectionRange> FindLiteralSearchMatches(
    const std::vector<std::string>& lines,
    std::string_view query) {
  std::vector<editor::SelectionRange> matches;
  if (query.empty()) {
    return matches;
  }

  const std::string lowered_query = ToLower(query);
  for (std::size_t line_index = 0; line_index < lines.size(); ++line_index) {
    const std::string lowered_line = ToLower(lines[line_index]);
    std::size_t offset = lowered_line.find(lowered_query);
    while (offset != std::string::npos) {
      matches.push_back(editor::SelectionRange{
          .start = editor::TextPosition{line_index, offset},
          .end = editor::TextPosition{line_index, offset + lowered_query.size()},
      });
      offset = lowered_line.find(lowered_query, offset + 1);
    }
  }

  return matches;
}

}  // namespace microide::workspace
