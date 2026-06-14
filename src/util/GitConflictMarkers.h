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
//  - ContainsAnyConflictMarker: any single marker is present. More
//    conservative; used to block commits when a marker leaks into staged text.
bool ContainsCompleteConflictMarkers(std::string_view text);
bool ContainsAnyConflictMarker(std::string_view text);

// Index of the first line beginning with `<<<<<<<`, if any.
std::optional<std::size_t> FirstConflictMarkerLine(std::span<const std::string> lines);

}  // namespace microide::util
