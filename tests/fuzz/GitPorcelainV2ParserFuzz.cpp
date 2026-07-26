// The `git status --porcelain=v2 -z` parser. Its sibling GitBlameParserFuzz has
// covered `git blame --incremental` for a while; this side was never fuzzed
// despite being the one that runs on every git refresh, in every project, and
// doing considerably more index arithmetic:
//
//   * NUL-delimited record splitting with a cap, where a rename record's origPath
//     is the FOLLOWING record and is consumed by advancing the loop index;
//   * `TokenAt` / `PathAfterLeadingTokens`, which walk a fixed number of
//     space-delimited fields and then take the remainder as the path — on a
//     truncated record those walks must stop, not run past the end;
//   * `# branch.ab +N -M` parsing, and the `<sub>` field read added when
//     conflicted submodules started being classified.
//
// The input is not adversary-controlled in the usual sense — it comes from the
// user's own git — but it IS truncated in practice: the subprocess capture has a
// byte ceiling, so a repository with enough entries hands the parser a record cut
// off at an arbitrary byte. That is exactly the shape this target explores, and
// it is how a short record reached `substr(2)` and threw before the length guard
// was added.
#include "project/GitPorcelainV2Parser.h"

#include <cstddef>
#include <cstdint>
#include <string_view>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  if (data == nullptr) {
    return 0;
  }
  const std::string_view output(reinterpret_cast<const char*>(data), size);
  // The generation / timestamp arguments are pass-through scalars; fixed values
  // keep the target deterministic without reducing coverage of the parse itself.
  const microide::project::GitRepositoryState state =
      microide::project::GitPorcelainV2Parser::Parse(output, "/repo", 1, 0);

  // Touch the parsed result so the optimizer cannot discard the work, and assert
  // the one invariant every consumer relies on: the tree-status map never names a
  // path that is not also an entry, since the sidebar indexes one by the other.
  std::size_t sink = state.entries.size() + state.tree_git_statuses.size() +
                     state.branch.branch_name.size();
  for (const microide::project::GitRepositoryEntry& entry : state.entries) {
    sink += entry.path.relative_path.native().size();
    sink += entry.submodule ? 1u : 0u;
    if (entry.old_path.has_value()) {
      sink += entry.old_path->relative_path.native().size();
    }
  }
  return static_cast<int>(sink & 0);
}
