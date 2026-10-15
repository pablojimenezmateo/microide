#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace microide::platform {
class ProcessLauncher;
}

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

// A language-server command with clangd's `--compile-commands-dir` filled in when
// the host can find the database clangd would not, plus one line for the log.
struct ClangdCommandPlan {
  std::vector<std::string> command;
  std::string note;  // empty: nothing worth saying (not clangd, or already set)
};

// clangd finds compile_commands.json next to the sources and in build/ on its own,
// but not in builds/, out/ or cmake-build-*; when DiscoverCompileCommandsDir finds
// one there, point clangd at it -- as the machine running clangd sees the path.
// Only clangd understands the flag, so other servers pass through untouched, as
// does a command that already names a directory.
//
// A REMOTE project (a non-local launcher) is not searched: the tree on this
// machine is the mirror, which never holds the host's build output (it is
// ignored, so never mirrored), so a search here can only report a database
// missing that the host may well have (TD-2026-10-16-344). clangd runs on the
// host and does its own discovery there.
ClangdCommandPlan PlanClangdCommand(std::vector<std::string> command,
                                    const std::filesystem::path& project_root,
                                    const platform::ProcessLauncher& launcher);

}  // namespace microide::project
