#pragma once

#include <string_view>

#include "project/GitRepositoryState.h"

namespace microide::project {

class GitPorcelainV2Parser {
 public:
  static GitRepositoryState Parse(std::string_view output,
                                  std::filesystem::path repository_root,
                                  std::uint64_t generation,
                                  std::uint64_t refreshed_at_ms);
};

}  // namespace microide::project
