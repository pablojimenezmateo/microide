#pragma once

// Internal seam for ProjectSearchService: exposes the file-local regex advance
// scan so its cancellation behaviour can be unit-tested deterministically
// (without a timing-dependent Start/Stop race). Not part of the public service
// API — only the service TU and its test include this header.

#include <atomic>
#include <cstddef>
#include <string_view>

#include "util/RegexUtil.h"

namespace microide::project::search_internal {

// Poll interval (loop iterations) for the cancellation checks in the worker's
// over-cap count loop. Aliases the shared match-engine constant so the two stay
// in lockstep (the empty-match advance loop itself now lives in RegexUtil).
constexpr std::size_t kRegexCancelPollInterval = util::kRegexMatchCancelPollInterval;

// Find the next regex match in `line` starting at *search_from, advancing past
// empty matches (recovering a non-empty alternative anchored at the same offset).
// Polls `cancel_requested` every kRegexCancelPollInterval iterations of the
// advance loop so a pattern that matches empty at every offset (e.g. `x?`) on a
// very large single line can be interrupted; on cancel it returns false leaving
// *search_from short of line.size()+1. Returns true and fills *match_start /
// *match_end / *search_from on a real match.
bool FindNextRegexMatch(const util::CompiledRegex& pattern, std::string_view line,
                        std::size_t* search_from, util::RegexMatchData* match_data,
                        std::size_t* match_start, std::size_t* match_end,
                        const std::atomic<bool>& cancel_requested);

}  // namespace microide::project::search_internal
