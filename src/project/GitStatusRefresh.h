#pragma once

#include <cstdint>
#include <filesystem>

#include "project/GitRepositoryState.h"

namespace microide::project {

class GitRepository;

// Run `git status --porcelain=v2` through `repo` and turn its output into a
// `GitRepositoryState`, including every way that can fail.
//
// Split out of `GitRepositoryService::BuildRepositoryState` to make those failure
// branches reachable from a test. The service has a test seam
// (`repository_state_provider_for_testing_`), but it substitutes this WHOLE step
// and hands back a finished state — so it can drive what the sidebar does with a
// state and cannot drive how git's OUTPUT becomes one, which is where the
// not-a-repository, error-classification and truncation branches live. Taking the
// repository (and therefore its launcher) as a parameter puts them all behind a
// scripted launcher instead.
//
// `refreshed_at_ms` is passed in rather than read here: this is kernel-side code
// and the clock is the shell's.
GitRepositoryState BuildGitRepositoryStateFromStatus(const GitRepository& repo,
                                                     const std::filesystem::path& project_root,
                                                     std::uint64_t generation,
                                                     std::uint64_t refreshed_at_ms);

}  // namespace microide::project
