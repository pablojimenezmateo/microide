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

bool EndsWith(std::string_view text, std::string_view suffix) {
  return text.size() >= suffix.size() &&
         text.substr(text.size() - suffix.size(), suffix.size()) == suffix;
}

std::string ToLower(std::string_view text) {
  // UTF-8 case fold (length-preserving for the covered scripts) so buffer literal
  // search matches the case-insensitive behavior of project search and ReplaceAll:
  // café/CAFÉ, Δ/δ, etc. Byte offsets stay valid because the supported folds do not
  // change byte length. (TD-2026-07-16-58.)
  return util::Utf8CaseFold(text);
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
  return util::CollapseAsciiWhitespace(text);
}

bool QuerySupportsLiteralReplace(std::string_view query) {
  static constexpr std::string_view kRegexMetacharacters = R"(\.^$|()[]{}*+?)";
  return !query.empty() &&
         query.find_first_of(kRegexMetacharacters) == std::string_view::npos;
}

bool UsesCaseSensitiveLiteralMatch(std::string_view query) {
  // Unicode-aware smart-case: any covered-script uppercase (not just ASCII) makes the
  // query case-sensitive, matching project search. (TD-2026-07-16-58.)
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
  // Lower both sides once (ASCII fold) and reuse the linear find, instead of the
  // previous O(haystack * needle) per-position re-lowering nested loop.
  thread_local std::string lowered_haystack;
  thread_local std::string lowered_needle;
  util::Utf8CaseFoldInto(haystack, lowered_haystack);
  util::Utf8CaseFoldInto(needle, lowered_needle);
  const std::size_t position = lowered_haystack.find(lowered_needle, start_from);
  return position != std::string::npos ? std::optional{position} : std::nullopt;
}

namespace {

// Shared seed-relative next-match scan. `line_at(i)` yields line `i` as a
// string_view; the same body serves the vector<string> and TextBuffer overloads.
// Held line views stay valid for the duration: neither backing store mutates here.
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

}  // namespace

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

// Shared all-occurrences case-insensitive scan. `line_at(i)` yields line `i` as a
// string_view; the same body serves both the vector<string> and TextBuffer
// overloads so the two cannot drift.
template <typename LineAt>
std::vector<editor::SelectionRange> FindLiteralMatchesImpl(std::size_t line_count,
                                                           LineAt&& line_at,
                                                           std::string_view query,
                                                           bool* truncated) {
  std::vector<editor::SelectionRange> matches;
  if (truncated != nullptr) {
    *truncated = false;
  }
  if (query.empty()) {
    return matches;
  }

  const std::string lowered_query = ToLower(query);
  std::string lowered_line;
  for (std::size_t line_index = 0; line_index < line_count; ++line_index) {
    util::Utf8CaseFoldInto(line_at(line_index), lowered_line);
    std::size_t offset = lowered_line.find(lowered_query);
    while (offset != std::string::npos) {
      // TD-2026-07-17A-029: cap the retained match set so a one-character query in a
      // huge minified buffer cannot allocate millions of ranges. Navigation re-scans
      // via FindNextLiteralMatchAfterSeedWrapOnce, so it stays correct past the cap.
      if (matches.size() >= kMaxBufferSearchMatches) {
        if (truncated != nullptr) {
          *truncated = true;
        }
        return matches;
      }
      matches.push_back(editor::SelectionRange{
          .start = editor::TextPosition{line_index, offset},
          .end = editor::TextPosition{line_index, offset + lowered_query.size()},
      });
      // Advance past the whole match so self-overlapping needles (e.g. "aa" in
      // "aaaa") yield non-overlapping ranges, matching find-next/replace which
      // advance by the needle length (see ReplaceLiteralMatchesInText).
      offset = lowered_line.find(lowered_query, offset + lowered_query.size());
    }
  }

  return matches;
}

}  // namespace

std::vector<editor::SelectionRange> FindLiteralSearchMatches(
    const std::vector<std::string>& lines,
    std::string_view query,
    bool* truncated) {
  return FindLiteralMatchesImpl(
      lines.size(), [&](std::size_t i) -> std::string_view { return lines[i]; }, query, truncated);
}

std::vector<editor::SelectionRange> FindLiteralSearchMatches(
    const editor::TextBuffer& buffer,
    std::string_view query,
    bool* truncated) {
  return FindLiteralMatchesImpl(
      buffer.LineCount(), [&](std::size_t i) { return buffer.LineView(i); }, query, truncated);
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
  // Fold the needle once (case-insensitive). The fold is length-preserving, so
  // the folded needle's byte length equals the raw needle's, keeping the end
  // column correct.
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
      // Fold this line exactly ONCE, then reuse the folded buffer for every match
      // on the line instead of re-folding per occurrence.
      util::Utf8CaseFoldInto(raw, lowered_line);
      haystack = lowered_line;
    }
    std::size_t offset = haystack.find(search_needle);
    while (offset != std::string_view::npos) {
      // Skip the seeded span; the primary caret already covers it.
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
  // Length-preserving UTF-8 fold: fold(prefix) must equal the fold of query's first
  // prefix.size() bytes. Compare folded byte ranges so café extends CAFÉ. Malformed
  // partial byte sequences at a boundary are folded verbatim identically on both
  // sides, so the comparison stays consistent. (TD-2026-07-16-58.)
  return util::Utf8CaseFold(prefix) == util::Utf8CaseFold(query.substr(0, prefix.size()));
}

std::vector<editor::SelectionRange> RefineLiteralSearchMatches(
    const editor::TextBuffer& buffer,
    std::string_view query,
    const std::vector<editor::SelectionRange>& previous,
    bool* truncated) {
  std::vector<editor::SelectionRange> matches;
  if (truncated != nullptr) {
    // `previous` is already a capped set (it fed back from FindLiteralSearchMatches),
    // and the refined subset is never larger — so refine can only preserve, never
    // introduce, truncation. Carry the prior state forward via the cap guard below.
    *truncated = false;
  }
  if (query.empty()) {
    return matches;
  }

  const std::string lowered_query = ToLower(query);
  const std::size_t needle = lowered_query.size();
  matches.reserve(previous.size());
  // Reproduce the cold path's advance-by-needle de-overlap: `previous` (the shorter
  // prefix query's match set) holds a hit at EVERY offset, so a self-overlapping
  // longer needle (e.g. "aa" over "aaaa") would keep overlapping ranges here while a
  // fresh scan yields non-overlapping ones — inflating the count and desyncing
  // next/prev/replace. `previous` is ordered ascending by (line, column), so skip any
  // candidate that would start inside the last kept match on the same line.
  std::size_t last_line = std::numeric_limits<std::size_t>::max();
  std::size_t last_end = 0;
  for (const editor::SelectionRange& match : previous) {
    const std::size_t line = match.start.line;
    const std::size_t column = match.start.column;
    if (line >= buffer.LineCount()) {
      continue;
    }
    if (line == last_line && column < last_end) {
      continue;  // would overlap the previously kept match
    }
    const std::string_view text = buffer.LineView(line);
    if (column > text.size() || needle > text.size() - column) {
      continue;
    }
    // Fold the candidate slice (length-preserving) and compare to the folded query,
    // so a growing non-ASCII case-insensitive query refines correctly. The slice
    // starts/ends on scalar boundaries (match offsets came from a length-preserving
    // folded find). (TD-2026-07-16-58.)
    thread_local std::string folded_slice;
    util::Utf8CaseFoldInto(text.substr(column, needle), folded_slice);
    const bool still_matches = folded_slice == lowered_query;
    if (still_matches) {
      if (matches.size() >= kMaxBufferSearchMatches) {
        if (truncated != nullptr) {
          *truncated = true;
        }
        return matches;
      }
      matches.push_back(editor::SelectionRange{
          .start = editor::TextPosition{line, column},
          .end = editor::TextPosition{line, column + needle},
      });
      last_line = line;
      last_end = column + needle;
    }
  }

  return matches;
}

}  // namespace microide::workspace
