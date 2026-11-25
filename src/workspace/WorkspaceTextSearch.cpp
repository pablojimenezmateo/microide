#include "workspace/WorkspaceTextSearch.h"

#include <algorithm>
#include <cctype>
#include <limits>
#include <string>

#include "util/StringUtil.h"

namespace microide::workspace {

bool StartsWith(std::string_view text, std::string_view prefix) {
  return text.substr(0, prefix.size()) == prefix;
}

std::string ToLower(std::string_view text) {
  return util::Utf8CaseFold(text);
}

std::string CollapseWhitespace(std::string_view text) {
  return util::CollapseAsciiWhitespace(text);
}

bool QuerySupportsLiteralReplace(std::string_view query) {
  static constexpr std::string_view kRegexMetacharacters = R"(\.^$|()[]{}*+?)";
  return !query.empty() &&
         query.find_first_of(kRegexMetacharacters) == std::string_view::npos;
}

bool UsesCaseSensitiveLiteralMatch(std::string_view query) {
  return util::Utf8QueryHasCaseVariation(query);
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

namespace {

template <typename OnLine>
void ForEachSearchLine(std::string_view content, OnLine&& on_line) {
  std::size_t line_start = 0;
  while (line_start < content.size()) {
    const std::size_t newline = content.find('\n', line_start);
    const std::size_t line_end = (newline == std::string_view::npos) ? content.size() : newline;
    std::string_view line = content.substr(line_start, line_end - line_start);
    std::string_view cr;
    if (!line.empty() && line.back() == '\r') {
      line.remove_suffix(1);
      cr = "\r";
    }
    const std::string_view eol = (newline == std::string_view::npos) ? std::string_view{} : "\n";
    on_line(line, cr, eol);
    if (newline == std::string_view::npos) {
      break;
    }
    line_start = newline + 1;
  }
}

}

std::optional<std::size_t> ReplaceRegexMatchesInText(std::string& content,
                                                     const util::CompiledRegex& pattern,
                                                     std::string_view replacement) {
  if (content.empty() || !pattern.valid()) {
    return std::size_t{0};
  }

  std::size_t replacements = 0;
  bool errored = false;
  std::string rebuilt;
  rebuilt.reserve(content.size());
  ForEachSearchLine(content, [&](std::string_view line, std::string_view cr, std::string_view eol) {
    if (errored) {
      return;
    }
    const int rc = pattern.SubstituteInto(line, replacement, rebuilt);
    if (rc < 0) {
      errored = true;
      return;
    }
    replacements += static_cast<std::size_t>(rc);
    rebuilt.append(cr);
    rebuilt.append(eol);
  });

  if (errored) {
    return std::nullopt;
  }
  if (replacements == 0) {
    return std::size_t{0};
  }
  content = std::move(rebuilt);
  return replacements;
}

std::vector<editor::SelectionRange> FindRegexSearchMatches(const editor::TextBuffer& buffer,
                                                           const util::CompiledRegex& pattern,
                                                           BufferSearchOptions options,
                                                           bool* truncated) {
  std::vector<editor::SelectionRange> matches;
  if (truncated != nullptr) {
    *truncated = false;
  }
  if (!pattern.valid()) {
    return matches;
  }

  util::RegexMatchData match_data = pattern.CreateMatchData();
  if (!match_data.valid()) {
    return matches;
  }

  const std::size_t line_count = buffer.LineCount();
  if (line_count == 0) {
    return matches;
  }
  std::vector<std::size_t> line_starts;
  line_starts.reserve(line_count);
  std::string content;
  {
    std::size_t total = 0;
    for (std::size_t i = 0; i < line_count; ++i) {
      total += buffer.LineLength(i) + 1;
    }
    content.reserve(total);
    for (std::size_t i = 0; i < line_count; ++i) {
      line_starts.push_back(content.size());
      content.append(buffer.LineView(i));
      if (i + 1 < line_count) {
        content.push_back('\n');
      }
    }
  }

  const auto to_position = [&](std::size_t offset) -> editor::TextPosition {
    const auto it = std::upper_bound(line_starts.begin(), line_starts.end(), offset);
    const std::size_t line = static_cast<std::size_t>(it - line_starts.begin()) - 1;
    return editor::TextPosition{line, offset - line_starts[line]};
  };

  const auto to_offset = [&](const editor::TextPosition& position) -> std::size_t {
    if (position.line >= line_count) {
      return content.size();
    }
    return std::min(line_starts[position.line] + position.column, content.size());
  };
  const std::size_t scope_end = options.scope.has_value() ? to_offset(options.scope->end) : content.size();
  std::size_t search_from = options.scope.has_value() ? to_offset(options.scope->start) : 0;
  std::size_t match_start = 0;
  std::size_t match_end = 0;
  const util::RegexSubject subject =
      util::IsAllAscii(content) ? util::RegexSubject::Ascii : util::RegexSubject::Unknown;
  while (util::FindNextRegexMatchInLine(pattern, content, &search_from, &match_data, &match_start,
                                        &match_end, nullptr, subject)) {
    if (match_end > scope_end) {
      break;
    }
    if (matches.size() >= kMaxBufferSearchMatches) {
      if (truncated != nullptr) {
        *truncated = true;
      }
      return matches;
    }
    if (options.whole_word && !util::SearchMatchStandsAlone(content, match_start, match_end)) {
      continue;
    }
    matches.push_back(editor::SelectionRange{
        .start = to_position(match_start),
        .end = to_position(match_end),
    });
  }
  return matches;
}

std::vector<editor::SelectionRange> SplitRegexMatchHighlightFragments(
    const editor::TextBuffer& buffer, const std::vector<editor::SelectionRange>& matches) {
  std::vector<editor::SelectionRange> fragments;
  fragments.reserve(matches.size());
  const std::size_t line_count = buffer.LineCount();
  for (const editor::SelectionRange& match : matches) {
    if (match.start.line == match.end.line) {
      fragments.push_back(match);
      continue;
    }
    for (std::size_t line = match.start.line; line <= match.end.line && line < line_count; ++line) {
      const std::size_t start_col = (line == match.start.line) ? match.start.column : 0;
      const std::size_t end_col =
          (line == match.end.line) ? match.end.column : buffer.LineLength(line) + 1;
      fragments.push_back(editor::SelectionRange{
          .start = editor::TextPosition{line, start_col},
          .end = editor::TextPosition{line, end_col},
      });
    }
  }
  return fragments;
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
  thread_local std::string lowered_haystack;
  thread_local std::string lowered_needle;
  util::Utf8CaseFoldInto(haystack, lowered_haystack);
  util::Utf8CaseFoldInto(needle, lowered_needle);
  const std::size_t position = lowered_haystack.find(lowered_needle, start_from);
  return position != std::string::npos ? std::optional{position} : std::nullopt;
}

namespace {

template <typename LineAt>
std::optional<editor::TextPosition> FindNextLiteralMatchImpl(std::size_t line_count,
                                                             LineAt&& line_at,
                                                             std::size_t seed_line,
                                                             std::size_t seed_start_col,
                                                             std::size_t seed_end_col,
                                                             std::string_view needle,
                                                             bool case_sensitive) {
  if (needle.empty() || seed_line >= line_count) {
    return std::nullopt;
  }

  const std::string_view seed_line_text = line_at(seed_line);
  if (seed_start_col > seed_end_col || seed_end_col > seed_line_text.size() ||
      seed_start_col > seed_line_text.size()) {
    return std::nullopt;
  }

  auto forward_from_cursor = [&](std::size_t li,
                                 std::size_t start_from_column) -> std::optional<editor::TextPosition> {
    if (li >= line_count) {
      return std::nullopt;
    }
    const auto pos =
        FindLiteralNeedleInLine(line_at(li), start_from_column, needle, case_sensitive);
    if (pos.has_value()) {
      return editor::TextPosition{li, *pos};
    }
    return std::nullopt;
  };

  for (std::size_t li = seed_line; li < line_count; ++li) {
    const std::size_t start_from = (li == seed_line) ? seed_end_col : 0;
    if (auto found = forward_from_cursor(li, start_from); found.has_value()) {
      return found;
    }
  }

  for (std::size_t li = 0; li < seed_line && li < line_count; ++li) {
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

}

std::optional<editor::TextPosition> FindNextLiteralMatchAfterSeedWrapOnce(
    const std::vector<std::string>& lines,
    std::size_t seed_line,
    std::size_t seed_start_col,
    std::size_t seed_end_col,
    std::string_view needle,
    bool case_sensitive) {
  return FindNextLiteralMatchImpl(
      lines.size(), [&](std::size_t i) -> std::string_view { return lines[i]; }, seed_line,
      seed_start_col, seed_end_col, needle, case_sensitive);
}

std::optional<editor::TextPosition> FindNextLiteralMatchAfterSeedWrapOnce(
    const editor::TextBuffer& buffer,
    std::size_t seed_line,
    std::size_t seed_start_col,
    std::size_t seed_end_col,
    std::string_view needle,
    bool case_sensitive) {
  return FindNextLiteralMatchImpl(
      buffer.LineCount(), [&](std::size_t i) { return buffer.LineView(i); }, seed_line,
      seed_start_col, seed_end_col, needle, case_sensitive);
}

namespace {

bool AppendLineLiteralMatches(std::size_t line_index,
                              std::string_view haystack,
                              std::size_t start_offset,
                              std::string_view needle,
                              BufferSearchOptions options,
                              std::vector<editor::SelectionRange>& matches) {
  std::size_t offset = haystack.find(needle, start_offset);
  while (offset != std::string_view::npos) {
    if (options.whole_word && !util::SearchMatchStandsAlone(haystack, offset, offset + needle.size())) {
      offset = haystack.find(needle, offset + 1);
      continue;
    }
    const editor::SelectionRange match{{line_index, offset}, {line_index, offset + needle.size()}};
    if (options.scope.has_value() && !editor::RangeContains(*options.scope, match)) {
      offset = haystack.find(needle, offset + 1);
      continue;
    }
    if (matches.size() >= kMaxBufferSearchMatches) {
      return false;
    }
    matches.push_back(match);
    offset = haystack.find(needle, offset + needle.size());
  }
  return true;
}

template <typename LineAt>
std::vector<editor::SelectionRange> FindLiteralMatchesImpl(std::size_t line_count,
                                                           LineAt&& line_at,
                                                           std::string_view query,
                                                           BufferSearchOptions options,
                                                           bool* truncated) {
  std::vector<editor::SelectionRange> matches;
  if (truncated != nullptr) {
    *truncated = false;
  }
  if (query.empty()) {
    return matches;
  }

  std::string folded_query;
  if (!options.case_sensitive) {
    util::Utf8CaseFoldInto(query, folded_query);
  }
  const std::string_view needle = options.case_sensitive ? query : std::string_view(folded_query);
  std::string folded_line;
  const std::size_t first_line = options.scope.has_value() ? options.scope->start.line : 0;
  const std::size_t end_line =
      options.scope.has_value() ? std::min(line_count, options.scope->end.line + 1) : line_count;
  for (std::size_t line_index = first_line; line_index < end_line; ++line_index) {
    const std::string_view raw_line = line_at(line_index);
    std::string_view haystack = raw_line;
    if (!options.case_sensitive) {
      util::Utf8CaseFoldInto(raw_line, folded_line);
      haystack = folded_line;
    }
    if (!AppendLineLiteralMatches(line_index, haystack, 0, needle, options, matches)) {
      if (truncated != nullptr) {
        *truncated = true;
      }
      return matches;
    }
  }

  return matches;
}

}

std::vector<editor::SelectionRange> FindLiteralSearchMatches(
    const std::vector<std::string>& lines,
    std::string_view query,
    BufferSearchOptions options,
    bool* truncated) {
  return FindLiteralMatchesImpl(
      lines.size(), [&](std::size_t i) -> std::string_view { return lines[i]; }, query, options,
      truncated);
}

std::vector<editor::SelectionRange> FindLiteralSearchMatches(
    const editor::TextBuffer& buffer,
    std::string_view query,
    BufferSearchOptions options,
    bool* truncated) {
  return FindLiteralMatchesImpl(
      buffer.LineCount(), [&](std::size_t i) { return buffer.LineView(i); }, query, options,
      truncated);
}

AddCursorMatchScan CollectAddCursorMatchRanges(const editor::TextBuffer& buffer,
                                               std::size_t seed_line,
                                               std::size_t seed_column,
                                               std::string_view needle,
                                               bool case_sensitive,
                                               std::size_t max_matches) {
  AddCursorMatchScan scan;
  if (needle.empty()) {
    return scan;
  }
  std::string lowered_needle;
  if (!case_sensitive) {
    util::Utf8CaseFoldInto(needle, lowered_needle);
  }
  const std::string_view search_needle = case_sensitive ? needle : std::string_view(lowered_needle);
  std::string lowered_line;
  for (std::size_t li = 0; li < buffer.LineCount(); ++li) {
    const std::string_view raw = buffer.LineView(li);
    std::string_view haystack = raw;
    if (!case_sensitive) {
      util::Utf8CaseFoldInto(raw, lowered_line);
      haystack = lowered_line;
    }
    std::size_t offset = haystack.find(search_needle);
    while (offset != std::string_view::npos) {
      if (!(li == seed_line && offset == seed_column)) {
        if (scan.ranges.size() >= max_matches) {
          scan.truncated = true;
          return scan;
        }
        scan.ranges.push_back(editor::SelectionRange{
            editor::TextPosition{li, offset},
            editor::TextPosition{li, offset + search_needle.size()},
        });
      }
      offset = haystack.find(search_needle, offset + search_needle.size());
    }
  }
  return scan;
}

bool QueryExtendsCaseInsensitive(std::string_view prefix, std::string_view query) {
  if (prefix.size() > query.size()) {
    return false;
  }
  return util::Utf8CaseFold(prefix) == util::Utf8CaseFold(query.substr(0, prefix.size()));
}

std::vector<editor::SelectionRange> RefineLiteralSearchMatches(
    const editor::TextBuffer& buffer,
    std::string_view query,
    const std::vector<editor::SelectionRange>& previous,
    BufferSearchOptions options,
    bool* truncated) {
  std::vector<editor::SelectionRange> matches;
  if (truncated != nullptr) {
    *truncated = false;
  }
  if (query.empty()) {
    return matches;
  }
  if (options.whole_word) {
    return FindLiteralSearchMatches(buffer, query, options, truncated);
  }

  std::string folded_query;
  if (!options.case_sensitive) {
    util::Utf8CaseFoldInto(query, folded_query);
  }
  const std::string_view needle =
      options.case_sensitive ? query : std::string_view(folded_query);
  matches.reserve(previous.size());
  if (previous.empty()) {
    return matches;
  }
  const std::size_t previous_needle_size =
      previous.front().end.column >= previous.front().start.column
          ? previous.front().end.column - previous.front().start.column
          : 0;
  const bool previous_can_self_overlap = [&] {
    if (previous_needle_size == 0 || previous_needle_size > needle.size()) {
      return true;
    }
    const std::string_view prefix = needle.substr(0, previous_needle_size);
    for (std::size_t offset = 1; offset < prefix.size(); ++offset) {
      if (prefix.compare(offset, prefix.size() - offset, prefix, 0, prefix.size() - offset) == 0) {
        return true;
      }
    }
    return false;
  }();
  if (!previous_can_self_overlap && !options.whole_word) {
    std::size_t kept_line = std::numeric_limits<std::size_t>::max();
    std::size_t kept_end = 0;
    thread_local std::string folded_slice;
    for (const editor::SelectionRange& match : previous) {
      const std::size_t line = match.start.line;
      const std::size_t column = match.start.column;
      if (line >= buffer.LineCount()) {
        continue;
      }
      if (line == kept_line && column < kept_end) {
        continue;
      }
      const std::string_view text = buffer.LineView(line);
      if (column > text.size() || needle.size() > text.size() - column) {
        continue;
      }
      std::string_view slice = text.substr(column, needle.size());
      if (!options.case_sensitive) {
        util::Utf8CaseFoldInto(slice, folded_slice);
        slice = folded_slice;
      }
      if (slice != needle) {
        continue;
      }
      const editor::SelectionRange longer{{line, column}, {line, column + needle.size()}};
      if (options.scope.has_value() && !editor::RangeContains(*options.scope, longer)) {
        continue;
      }
      if (matches.size() >= kMaxBufferSearchMatches) {
        if (truncated != nullptr) {
          *truncated = true;
        }
        return matches;
      }
      matches.push_back(longer);
      kept_line = line;
      kept_end = column + needle.size();
    }
    return matches;
  }

  std::size_t last_line = std::numeric_limits<std::size_t>::max();
  thread_local std::string folded_line;
  for (const editor::SelectionRange& match : previous) {
    const std::size_t line = match.start.line;
    if (line == last_line || line >= buffer.LineCount()) {
      continue;
    }
    last_line = line;
    const std::string_view text = buffer.LineView(line);
    if (match.start.column > text.size()) {
      continue;
    }
    std::string_view haystack = text;
    if (!options.case_sensitive) {
      util::Utf8CaseFoldInto(text, folded_line);
      haystack = folded_line;
    }
    if (!AppendLineLiteralMatches(line, haystack, match.start.column, needle, options, matches)) {
      if (truncated != nullptr) {
        *truncated = true;
      }
      return matches;
    }
  }

  return matches;
}

}
