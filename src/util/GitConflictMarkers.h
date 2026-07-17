#pragma once

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace microide::util {

// Git textual conflict-marker helpers, shared by the merge resolver, merge
// result validation, and commit pre-checks so the marker literals live in one
// place. The "complete" and "any" variants intentionally differ:
//
//  - ContainsCompleteConflictMarkers: all of `<<<<<<<`, `=======`, `>>>>>>>`
//    are present. Used to decide whether a merge *result* still embeds an
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
  bool complete = false;                         // all of `<<<<<<<`/`=======`/`>>>>>>>` present
  std::optional<std::size_t> first_marker_line;  // first line beginning with `<<<<<<<`
};

// Scan any lines-like source (`size()` + `operator[]` yielding something
// string_view-convertible, e.g. `editor::LineSpan` over a live TextBuffer) for the
// complete conflict markers in ONE pass, without serializing or materializing the
// whole document. `complete` reproduces ContainsCompleteConflictMarkers on the
// serialized text (substring-anywhere: a marker never spans a line break, so a
// per-line `find` is equivalent); `first_marker_line` reproduces
// FirstConflictMarkerLine (line-anchored `<<<<<<<`). Templated so `util` keeps no
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
    if (!has_ours && line.find("<<<<<<<") != std::string_view::npos) {
      has_ours = true;
    }
    if (!has_base && line.find("=======") != std::string_view::npos) {
      has_base = true;
    }
    if (!has_theirs && line.find(">>>>>>>") != std::string_view::npos) {
      has_theirs = true;
    }
    if (!first_line.has_value() && line.starts_with("<<<<<<<")) {
      first_line = i;
    }
  }
  return ConflictMarkerScan{has_ours && has_base && has_theirs, first_line};
}

}  // namespace microide::util
