#pragma once

#include <filesystem>
#include <optional>

namespace microide::project {

// Locate the directory containing a clangd compilation database
// (compile_commands.json) for `project_root`, so the host can point clangd at a
// non-standard build directory via `--compile-commands-dir`.
//
// Bounded, fixed-candidate search (no recursive walk): the project root itself,
// then conventional build directories (build/, builds/, out/, cmake-build-*/),
// then any immediate child directory that holds a build.ninja or CMakeCache.txt.
// clangd already finds root and build/ on its own, but not the others.
//
// Returns the directory that contains the newest compile_commands.json, or
// nullopt when none exists. Pure filesystem; no subprocess.
std::optional<std::filesystem::path> DiscoverCompileCommandsDir(
    const std::filesystem::path& project_root);

}  // namespace microide::project
