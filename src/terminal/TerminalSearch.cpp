#include "terminal/TerminalSearch.h"

#include "util/StringUtil.h"

namespace microide::terminal {
namespace {

// Word constituents for the whole-word toggle. Every non-ASCII byte counts as a
// word byte so a match inside a multi-byte word is not reported as standing
// alone; the alternative (decoding each boundary codepoint) buys nothing for the
// paths that use this.
bool IsWordByte(char c) {
  const auto byte = static_cast<unsigned char>(c);
  return byte >= 0x80 || (byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'z') ||
         (byte >= 'A' && byte <= 'Z') || byte == '_';
}

bool MatchStandsAlone(std::string_view text, std::size_t start, std::size_t end) {
  if (start > 0 && IsWordByte(text[start - 1])) {
    return false;
  }
  return end >= text.size() || !IsWordByte(text[end]);
}

}  // namespace

TerminalSearchQuery MakeTerminalSearchQuery(const std::string_view text,
                                            const bool case_sensitive,
                                            const bool whole_word) {
  TerminalSearchQuery query;
  query.case_sensitive = case_sensitive;
  query.whole_word = whole_word;
  if (case_sensitive) {
    query.needle.assign(text);
  } else {
    util::Utf8CaseFoldInto(text, query.needle);
  }
  return query;
}

void FlattenTerminalLine(const TerminalLine& line,
                         const bool fold_case,
                         TerminalSearchScratch& scratch) {
  scratch.text.clear();
  scratch.columns.clear();
  // Reserve against the cell count rather than the byte count: ASCII rows (all of
  // them, in practice) then fill without a single reallocation, and the buffers
  // stay warm for every later row.
  scratch.text.reserve(line.cells.size());
  scratch.columns.reserve(line.cells.size());

  std::string folded_glyph;
  for (std::size_t column = 0; column < line.cells.size(); ++column) {
    const TerminalCell& cell = line.cells[column];
    if (cell.style.wide_trailing()) {
      // The spacer half of a double-width glyph carries no text of its own; the
      // lead cell's entry already covers both columns via the end-column mapping
      // below.
      continue;
    }
    const std::string_view glyph = cell.DisplayText();
    if (glyph.empty()) {
      // An untouched cell reads as a blank, matching how the row paints and how
      // selection copy slices it, so a query with a space still lines up.
      scratch.text.push_back(' ');
      scratch.columns.push_back(static_cast<std::uint32_t>(column));
      continue;
    }
    if (glyph.size() == 1) {
      const char byte = fold_case ? util::ToLowerAsciiChar(glyph[0]) : glyph[0];
      scratch.text.push_back(byte);
      scratch.columns.push_back(static_cast<std::uint32_t>(column));
      continue;
    }
    // Multi-byte glyph. Fold it through the shared UTF-8 folder (so `É` finds
    // `é`, matching the file-search literal path) rather than copying bytes: this
    // branch is rare enough that the per-glyph call costs nothing measurable.
    const std::string_view appended = [&]() -> std::string_view {
      if (!fold_case) {
        return glyph;
      }
      util::Utf8CaseFoldInto(glyph, folded_glyph);
      return folded_glyph;
    }();
    scratch.text.append(appended);
    scratch.columns.insert(scratch.columns.end(), appended.size(),
                           static_cast<std::uint32_t>(column));
  }
}

bool FindTerminalLineMatches(const TerminalLine& line,
                             const TerminalSearchQuery& query,
                             const std::size_t row,
                             const std::size_t max_matches,
                             TerminalSearchScratch& scratch,
                             std::vector<TerminalSearchMatch>& out) {
  if (query.empty() || out.size() >= max_matches) {
    return out.size() < max_matches;
  }
  FlattenTerminalLine(line, !query.case_sensitive, scratch);
  const std::string_view text = scratch.text;
  if (text.size() < query.needle.size()) {
    return true;
  }

  std::size_t from = 0;
  while (true) {
    const std::size_t start = text.find(query.needle, from);
    if (start == std::string_view::npos) {
      return true;
    }
    const std::size_t end = start + query.needle.size();
    // Advance by one byte, not by the needle length: overlapping occurrences
    // ("aa" in "aaa") are distinct matches, and the caller navigates between them.
    from = start + 1;
    if (query.whole_word && !MatchStandsAlone(text, start, end)) {
      continue;
    }
    const std::size_t first_column = scratch.columns[start];
    // The column the NEXT glyph starts at bounds the match, so a wide glyph's
    // trailing spacer is covered and no gap is left mid-highlight.
    const std::size_t end_column =
        end < scratch.columns.size() ? scratch.columns[end] : line.cells.size();
    out.push_back(TerminalSearchMatch{
        .row = row,
        .column = first_column,
        .length = end_column > first_column ? end_column - first_column : 1,
    });
    if (out.size() >= max_matches) {
      return false;
    }
  }
}

}  // namespace microide::terminal
