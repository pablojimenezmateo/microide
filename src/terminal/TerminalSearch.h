#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "terminal/TerminalCell.h"

// Literal match scanning over terminal grid rows, shared by the session-wide
// scrollback scan and its tests.
//
// The grid is cells, not text: a row has no backing string to run `find` over,
// double-width glyphs occupy two cells (the second carrying no glyph), and empty
// cells render as spaces. Rather than materialize a `std::string` per row and a
// second structure to map byte offsets back to columns, a scan flattens each row
// into reusable scratch that carries both, so a full-scrollback scan allocates
// nothing after its first row.
namespace microide::terminal {

// A match expressed in grid coordinates. `column` is the first cell covered and
// `length` the number of cells, including the trailing spacer of a double-width
// glyph, so the highlight rect is `[column, column + length)` with no further
// adjustment at paint time.
struct TerminalSearchMatch {
  std::size_t row = 0;
  std::size_t column = 0;
  std::size_t length = 0;
};

// A prepared needle. Build it once per scan with MakeTerminalSearchQuery, which
// applies the same case folding the row flattener does, so the comparison is a
// plain byte search rather than a per-character case check.
struct TerminalSearchQuery {
  std::string needle;
  bool case_sensitive = false;
  bool whole_word = false;

  bool empty() const { return needle.empty(); }
};

TerminalSearchQuery MakeTerminalSearchQuery(std::string_view text,
                                            bool case_sensitive,
                                            bool whole_word);

// Reusable per-scan buffers. `text` is one row flattened to bytes (case-folded
// unless the query is case sensitive) and `columns[i]` is the grid column byte
// `i` came from, so a match's byte range maps back to cells without a second
// pass. Held by the caller across rows and across scans.
struct TerminalSearchScratch {
  std::string text;
  std::vector<std::uint32_t> columns;
};

// Flattens `line` into `scratch`. Exposed for tests and for callers that want the
// row text without matching; FindTerminalLineMatches calls it internally.
void FlattenTerminalLine(const TerminalLine& line, bool fold_case, TerminalSearchScratch& scratch);

// Appends every match in `line` to `out` with `row` as the reported row, stopping
// as soon as `out` holds `max_matches` entries. Returns false when that cap was
// reached (the row may hold further matches), true otherwise.
bool FindTerminalLineMatches(const TerminalLine& line,
                             const TerminalSearchQuery& query,
                             std::size_t row,
                             std::size_t max_matches,
                             TerminalSearchScratch& scratch,
                             std::vector<TerminalSearchMatch>& out);

}  // namespace microide::terminal
