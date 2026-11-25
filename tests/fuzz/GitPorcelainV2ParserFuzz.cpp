#include "project/GitPorcelainV2Parser.h"

#include <cstddef>
#include <cstdint>
#include <string_view>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  if (data == nullptr) {
    return 0;
  }
  const std::string_view output(reinterpret_cast<const char*>(data), size);
  const microide::project::GitRepositoryState state =
      microide::project::GitPorcelainV2Parser::Parse(output, "/repo", 1, 0);

  std::size_t sink = state.entries.size() +
                     (state.tree_git_statuses ? state.tree_git_statuses->size() : 0) +
                     state.branch.branch_name.size();
  for (const microide::project::GitRepositoryEntry& entry : state.entries) {
    sink += entry.path.relative_path.size();
    sink += entry.submodule ? 1u : 0u;
    if (entry.old_path.has_value()) {
      sink += entry.old_path->relative_path.size();
    }
  }
  return static_cast<int>(sink & 0);
}
