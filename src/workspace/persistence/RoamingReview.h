#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "workspace/git/BranchReviewPersistence.h"

namespace microide::workspace {

// A remote project's review marks and notes, for the user's other machine: a
// review begun on the work laptop goes on from the personal one. Like the files
// that were open (RoamingSession) it is left on the host (`state/put`, key
// "review") and read by the machine that connects next.
//
// It carries the review targets of the repository at the project's root -- base,
// head and merge-base name the same commits on every machine, and every path in
// a target is relative to the repository -- without the root itself, which is
// each machine's own mirror. Taking it is a merge, one target at a time: a
// target this machine does not hold is added, and one it does is replaced when
// the other copy was changed later (BranchReviewTargetState::last_accessed, set
// by every mark, note and clear). Nothing is asked: marks are not a layout.

// Past this the oldest targets are left out. The host keeps at most
// remote::kMaxWorkspaceStateBytes a key.
inline constexpr std::size_t kMaxRoamingReviewBytes = 384u * 1024;

// The text kept on the host (JSON): `state`'s targets whose repository is
// `root`, the most recently changed first, whole targets while they fit in
// `max_bytes`. The same marks give the same text, whichever machine holds them.
// Empty when the repository has no target.
std::string EncodeRoamingReview(const PersistedBranchReviewState& state, const std::filesystem::path& root,
                                std::size_t max_bytes = kMaxRoamingReviewBytes);

// Untrusted -- another machine wrote it, and anything on the host could have:
// nullopt unless it is that form. Each target comes back as one of repository
// `root`; an entry whose path leaves the repository or whose fields are not what
// they should be is dropped, and every list is cut at the review state's own caps.
std::optional<std::vector<PersistedBranchReviewTarget>> DecodeRoamingReview(std::string_view text,
                                                                           const std::filesystem::path& root);

// Merge `theirs` into `state` by the rule above. True when `state` changed.
bool MergeRoamingReview(std::vector<PersistedBranchReviewTarget> theirs, PersistedBranchReviewState* state);

}  // namespace microide::workspace
