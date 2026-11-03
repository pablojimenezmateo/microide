#pragma once

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace microide::util {

// The marker lines of git's textual conflicts, as git writes them: anchored at the
// start of the line, with a trailing '\r' (a CRLF file split on '\n') ignored.
// `<<<<<<<` opens a conflict, `|||||||` opens the base (diff3), a line of seven or
// more '=' (merge.conflictMarkerSize may raise the count) separates the sides, and
// `>>>>>>>` closes it. These are the ONE definition: the scanner below, the
// complete-marker test and the merge editor's parser of git's output all use them,
// so what "markers remain" refuses is exactly what the parser would read as a
// conflict. A substring test read a `// =======` banner plus a marker quoted in a
// comment or a test fixture as an unresolved conflict.
inline bool IsConflictStartLine(std::string_view line) { return line.starts_with("<<<<<<<"); }
inline bool IsConflictBaseLine(std::string_view line) { return line.starts_with("|||||||"); }
inline bool IsConflictEndLine(std::string_view line) { return line.starts_with(">>>>>>>"); }
inline bool IsConflictSeparatorLine(std::string_view line) {
  if (!line.empty() && line.back() == '\r') {
    line.remove_suffix(1);
  }
  return line.size() >= 7 && line.find_first_not_of('=') == std::string_view::npos;
}

// Git textual conflict-marker helpers, shared by the merge resolver, merge
// result validation, and commit pre-checks so the marker literals live in one
// place. The "complete" and "any" variants intentionally differ:
//
//  - ContainsCompleteConflictMarkers: a start, a separator and an end marker LINE
//    are all present. Used to decide whether a merge *result* still embeds an
//    unresolved conflict.
//  - StagedDiffIntroducesConflictMarker: scans `git diff --cached` output for an
//    *added* line beginning with the unambiguous `<<<<<<<` / `>>>>>>>` sigils.
//    Used to block commits when a real conflict marker leaks into staged text
//    without false-positiving on a bare `=======` divider in staged content.
bool ContainsCompleteConflictMarkers(std::string_view text);
bool StagedDiffIntroducesConflictMarker(std::string_view diff);

// Index of the first line beginning with `<<<<<<<`, if any.
std::optional<std::size_t> FirstConflictMarkerLine(std::span<const std::string> lines);

// Result of a single-pass conflict-marker scan over already-split lines.
struct ConflictMarkerScan {
  bool complete = false;                         // a start, a separator and an end marker line
  std::optional<std::size_t> first_marker_line;  // first line beginning with `<<<<<<<`
};

// Scan any lines-like source (`size()` + `operator[]` yielding something
// string_view-convertible, e.g. `editor::LineSpan` over a live TextBuffer) for the
// complete conflict markers in ONE pass, without serializing or materializing the
// whole document. `complete` reproduces ContainsCompleteConflictMarkers on the
// serialized text; `first_marker_line` reproduces FirstConflictMarkerLine. Templated so `util` keeps no
// editor dependency (mirrors SerializeLinesStreaming, TD-2026-07-17-095), letting the
// merge validator pass a zero-copy LineSpan instead of `lines().Snapshot()`
// (TD-2026-07-17A-009).
template <typename LinesLike>
ConflictMarkerScan ScanConflictMarkers(const LinesLike& lines) {
  bool has_ours = false;
  bool has_base = false;
  bool has_theirs = false;
  std::optional<std::size_t> first_line;
  const std::size_t count = lines.size();
  for (std::size_t i = 0; i < count; ++i) {
    const std::string_view line = lines[i];
    if (IsConflictStartLine(line)) {
      has_ours = true;
      if (!first_line.has_value()) {
        first_line = i;
      }
    } else if (IsConflictSeparatorLine(line)) {
      has_base = true;
    } else if (IsConflictEndLine(line)) {
      has_theirs = true;
    }
  }
  return ConflictMarkerScan{has_ours && has_base && has_theirs, first_line};
}

}  // namespace microide::util
