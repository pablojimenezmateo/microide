#pragma once

#include <string_view>

namespace microide::tests::parity {

// Parity rows that cannot pass yet, each with what removes it. The runner fails on
// a listed row that now passes, so this list only shrinks. Phase 2b ships with it
// EMPTY (dev-docs/design/remote-projects.md § 10.1).
struct KnownGap {
  std::string_view scenario;
  std::string_view removed_by;
};

inline constexpr KnownGap kParityKnownGaps[] = {
    // The mirror has no `.git`, and `GitRepositoryService::IsGitRepoValid` /
    // `GitRepository::IsValid` stat one locally, so a non-local project reads "not
    // a git repository" without ever spawning git.
    {"Parity/GitSidebarShowsTheWorkingTree", "TD-2026-10-06-319 (G9 first slice)"},
};

}  // namespace microide::tests::parity
