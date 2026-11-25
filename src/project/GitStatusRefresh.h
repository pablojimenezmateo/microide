#pragma once

#include <cstdint>
#include <filesystem>

#include "project/GitRepositoryState.h"

namespace microide::project {

class GitRepository;

GitRepositoryState BuildGitRepositoryStateFromStatus(const GitRepository& repo,
                                                     const std::filesystem::path& project_root,
                                                     std::uint64_t generation,
                                                     std::uint64_t refreshed_at_ms);

}
