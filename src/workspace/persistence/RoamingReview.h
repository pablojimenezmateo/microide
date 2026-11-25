#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "workspace/git/BranchReviewPersistence.h"

namespace microide::workspace {

inline constexpr std::size_t kMaxRoamingReviewBytes = 384u * 1024;

std::string EncodeRoamingReview(const PersistedBranchReviewState& state, const std::filesystem::path& root,
                                std::size_t max_bytes = kMaxRoamingReviewBytes);

std::optional<std::vector<PersistedBranchReviewTarget>> DecodeRoamingReview(std::string_view text,
                                                                           const std::filesystem::path& root);

bool MergeRoamingReview(std::vector<PersistedBranchReviewTarget> theirs, PersistedBranchReviewState* state);

}
