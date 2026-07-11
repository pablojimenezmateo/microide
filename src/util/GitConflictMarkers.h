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

}  // namespace microide::util
