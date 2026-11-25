#pragma once

#include <atomic>
#include <cstddef>
#include <string_view>

#include "util/RegexUtil.h"

namespace microide::project::search_internal {

constexpr std::size_t kRegexCancelPollInterval = util::kRegexMatchCancelPollInterval;

bool FindNextRegexMatch(const util::CompiledRegex& pattern, std::string_view line,
                        std::size_t* search_from, util::RegexMatchData* match_data,
                        std::size_t* match_start, std::size_t* match_end,
                        const std::atomic<bool>& cancel_requested,
                        util::RegexSubject subject = util::RegexSubject::Unknown);

}
