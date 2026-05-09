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
  if (case_sensitive) {
    std::size_t search_from = 0;
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
  std::string rebuilt_content;
  rebuilt_content.reserve(content.size());
  std::size_t copy_from = 0;
  while (true) {
    const std::size_t position = lowered_content.find(lowered_query, copy_from);
    if (position == std::string::npos) {
      break;
    }
    rebuilt_content.append(content, copy_from, position - copy_from);
    rebuilt_content.append(replacement);
    copy_from = position + query.size();
    ++replacements;
  }
  if (replacements == 0) {
    return 0;
  }
  rebuilt_content.append(content, copy_from);
  content = std::move(rebuilt_content);
  return replacements;
}

std::optional<std::size_t> FindLiteralNeedleInLine(std::string_view haystack,
                                                   std::size_t start_from,
                                                   std::string_view needle,
                                                   bool case_sensitive) {
  if (needle.empty() || start_from > haystack.size()) {
    return std::nullopt;
  }
  if (case_sensitive) {
    const std::size_t position = haystack.find(needle, start_from);
    return position != std::string_view::npos ? std::optional{position} : std::nullopt;
  }
  for (std::size_t i = start_from; i + needle.size() <= haystack.size(); ++i) {
    bool match = true;
    for (std::size_t j = 0; j < needle.size(); ++j) {
      if (std::tolower(static_cast<unsigned char>(haystack[i + j])) !=
          std::tolower(static_cast<unsigned char>(needle[j]))) {
        match = false;
        break;
      }
    }
    if (match) {
      return i;
    }
  }
  return std::nullopt;
}

std::optional<editor::TextPosition> FindNextLiteralMatchAfterSeedWrapOnce(
    const std::vector<std::string>& lines,
    std::size_t seed_line,
    std::size_t seed_start_col,
    std::size_t seed_end_col,
    std::string_view needle,
    bool case_sensitive) {
  if (needle.empty() || seed_line >= lines.size()) {
    return std::nullopt;
  }

  const std::string& seed_line_text = lines[seed_line];

  auto forward_from_cursor = [&](std::size_t li, std::size_t start_from_column) -> std::optional<editor::TextPosition> {
    if (li >= lines.size()) {
      return std::nullopt;
    }
    const auto pos =
        FindLiteralNeedleInLine(lines[li], start_from_column, needle, case_sensitive);
    if (pos.has_value()) {
      return editor::TextPosition{li, *pos};
    }
    return std::nullopt;
  };

  for (std::size_t li = seed_line; li < lines.size(); ++li) {
    const std::size_t start_from = (li == seed_line) ? seed_end_col : 0;
    if (auto found = forward_from_cursor(li, start_from); found.has_value()) {
      return found;
    }
  }

  for (std::size_t li = 0; li < seed_line && li < lines.size(); ++li) {
    if (auto found = forward_from_cursor(li, 0); found.has_value()) {
      return found;
    }
  }

  std::size_t from = 0;
  while (true) {
    const auto pos =
        FindLiteralNeedleInLine(seed_line_text, from, needle, case_sensitive);
    if (!pos.has_value() || *pos >= seed_end_col) {
      break;
    }
    if (*pos == seed_start_col) {
      from = *pos + needle.size();
      continue;
    }
    return editor::TextPosition{seed_line, *pos};
  }

  return std::nullopt;
}

std::vector<editor::SelectionRange> FindLiteralSearchMatches(
    const std::vector<std::string>& lines,
    std::string_view query) {
  std::vector<editor::SelectionRange> matches;
  if (query.empty()) {
    return matches;
  }

  const std::string lowered_query = ToLower(query);
  std::string lowered_line;
  for (std::size_t line_index = 0; line_index < lines.size(); ++line_index) {
    const std::string& line = lines[line_index];
    lowered_line.resize(line.size());
    std::transform(line.begin(), line.end(), lowered_line.begin(), [](unsigned char c) {
      return static_cast<char>(std::tolower(c));
    });
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
