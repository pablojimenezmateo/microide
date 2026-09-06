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

namespace {

// Splits `content` into logical lines (on '\n'), invoking `on_line(line, cr, eol)`
// for each: `line` is the match window (trailing '\r' removed), `cr` is "\r" when
// one was stripped else "", and `eol` is "\n" for every line but a final one with
// no trailing newline. Matches the project-search worker's getline framing.
template <typename OnLine>
void ForEachSearchLine(std::string_view content, OnLine&& on_line) {
  std::size_t line_start = 0;
  // `<` (not `<=`) matches the search worker's getline framing: the final '\n' is
  // the terminator of the preceding line, NOT the start of a phantom empty line.
  // This keeps an empty-matching pattern (e.g. `^` or `x?`) from substituting on a
  // trailing line the search never reported.
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

}  // namespace

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
    // SubstituteInto appends the substituted line (or, on a no-match line, the
    // subject copied through unchanged) to `rebuilt`; on error it leaves `rebuilt`
    // untouched and returns a negative rc.
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
    return std::size_t{0};  // Leave `content` byte-identical.
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

  // Whole-buffer scan (not per-line) so a pattern can span line breaks — `\n`, `$`
  // (with PCRE2_MULTILINE), or a multi-line construct like `foo\nbar`. Build the
  // '\n'-joined content once (the buffer is internally CRLF-normalized) plus the
  // byte offset of each line start, then map every match's byte span back to
  // (line, column) positions. A match may span multiple lines.
  const std::size_t line_count = buffer.LineCount();
  if (line_count == 0) {
    return matches;  // No lines -> no offsets to map against.
  }
  std::vector<std::size_t> line_starts;
  line_starts.reserve(line_count);
  std::string content;
  {
    std::size_t total = 0;
    for (std::size_t i = 0; i < line_count; ++i) {
      total += buffer.LineLength(i) + 1;  // + newline
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

  // Byte offset -> (line, column). `line_starts` is ascending, so the owning line is
  // the last start <= offset.
  const auto to_position = [&](std::size_t offset) -> editor::TextPosition {
    const auto it = std::upper_bound(line_starts.begin(), line_starts.end(), offset);
    const std::size_t line = static_cast<std::size_t>(it - line_starts.begin()) - 1;
    return editor::TextPosition{line, offset - line_starts[line]};
  };

  std::size_t search_from = 0;
  std::size_t match_start = 0;
  std::size_t match_end = 0;
  while (util::FindNextRegexMatchInLine(pattern, content, &search_from, &match_data, &match_start,
                                        &match_end, /*cancel=*/nullptr)) {
    if (matches.size() >= kMaxBufferSearchMatches) {
      if (truncated != nullptr) {
        *truncated = true;
      }
      return matches;
    }
    // Whole-word is a filter over the joined content, not a `\b`-wrapped pattern:
    // the same predicate the literal path and the terminal find bar use, so one
    // `ab` toggle means one thing everywhere.
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
      // The trailing newline of every line but the match's last is part of the match.
      // Encode it as one column past the content; the renderer draws a newline marker
      // there so a `\n`-spanning match is visible at the line end.
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

// The one per-line scan every literal match set is built from. Appends the
// non-overlapping occurrences of `needle` in `haystack` (already folded when the
// search is case-insensitive) from `start_offset` on. Returns false once the
// retained set hits its cap, which the caller reports as truncation.
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
    // TD-2026-07-17A-029: cap the retained match set so a one-character query in a
    // huge minified buffer cannot allocate millions of ranges. Navigation re-scans
    // via FindNextLiteralMatchAfterSeedWrapOnce, so it stays correct past the cap.
    if (matches.size() >= kMaxBufferSearchMatches) {
      return false;
    }
    matches.push_back(editor::SelectionRange{
        .start = editor::TextPosition{line_index, offset},
        .end = editor::TextPosition{line_index, offset + needle.size()},
    });
    // Advance past the whole match so self-overlapping needles (e.g. "aa" in
    // "aaaa") yield non-overlapping ranges, matching find-next/replace which
    // advance by the needle length (see ReplaceLiteralMatchesInText).
    offset = haystack.find(needle, offset + needle.size());
  }
  return true;
}

// Shared all-occurrences case-insensitive scan. `line_at(i)` yields line `i` as a
// string_view; the same body serves both the vector<string> and TextBuffer
// overloads so the two cannot drift.
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

  // Case-sensitive search compares the raw bytes; case-insensitive folds both
  // sides. The fold is length-preserving, so match offsets are byte offsets into
  // the raw line either way (which is what the whole-word check needs).
  std::string folded_query;
  if (!options.case_sensitive) {
    util::Utf8CaseFoldInto(query, folded_query);
  }
  const std::string_view needle = options.case_sensitive ? query : std::string_view(folded_query);
  std::string folded_line;
  for (std::size_t line_index = 0; line_index < line_count; ++line_index) {
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

}  // namespace

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
    BufferSearchOptions options,
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

  std::string folded_query;
  if (!options.case_sensitive) {
    util::Utf8CaseFoldInto(query, folded_query);
  }
  const std::string_view needle =
      options.case_sensitive ? query : std::string_view(folded_query);
  matches.reserve(previous.size());
  // Every occurrence of the longer query starts at an occurrence of the prefix,
  // and the prefix's FIRST occurrence on a line is always in `previous` — but the
  // later ones are not: the cold scan advances past each hit by the needle length,
  // so with "aa" over "aaab" the occurrence at column 1 was never recorded, and a
  // refine that only re-checked the recorded columns lost the "aab" at column 1
  // (the widget showed no match for text that was right there). So the unit of
  // reuse is the LINE, not the hit: a line with no prefix hit cannot hold the
  // longer query and is skipped without a read, and a line with one is rescanned
  // by the cold scan's own loop from its first hit — equal to a fresh scan by
  // construction, at the cost of touching only the lines that matched.
  std::size_t last_line = std::numeric_limits<std::size_t>::max();
  std::string folded_line;
  for (const editor::SelectionRange& match : previous) {
    const std::size_t line = match.start.line;
    if (line == last_line || line >= buffer.LineCount()) {
      continue;
    }
    last_line = line;
    const std::string_view text = buffer.LineView(line);
    if (match.start.column > text.size()) {
      continue;  // a stale hit past the line's end can only drop, never invent
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

}  // namespace microide::workspace
