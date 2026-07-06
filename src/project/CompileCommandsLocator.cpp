#include "project/CompileCommandsLocator.h"

#include <string>
#include <system_error>

namespace microide::project {

namespace {

constexpr const char* kCompileCommands = "compile_commands.json";

// Returns the compile_commands.json mtime for `dir` if the file exists, else nullopt.
std::optional<std::filesystem::file_time_type> CompileCommandsMtime(
    const std::filesystem::path& dir) {
  std::error_code error;
  const std::filesystem::path candidate = dir / kCompileCommands;
  if (!std::filesystem::is_regular_file(candidate, error) || error) {
    return std::nullopt;
  }
  const auto mtime = std::filesystem::last_write_time(candidate, error);
  if (error) {
    // The file exists but we can't stat it; still a valid location, treat as epoch.
    return std::filesystem::file_time_type{};
  }
  return mtime;
}

// Consider `dir` as a candidate; keep it if it has the newest compile_commands.json seen.
void ConsiderCandidate(const std::filesystem::path& dir,
                       std::optional<std::filesystem::path>& best_dir,
                       std::optional<std::filesystem::file_time_type>& best_mtime) {
  const auto mtime = CompileCommandsMtime(dir);
  if (!mtime.has_value()) {
    return;
  }
  if (!best_dir.has_value() || *mtime > *best_mtime) {
    best_dir = dir.lexically_normal();
    best_mtime = mtime;
  }
}

}  // namespace

std::optional<std::filesystem::path> DiscoverCompileCommandsDir(
    const std::filesystem::path& project_root) {
  std::error_code error;
  const std::filesystem::path root = project_root.lexically_normal();
  if (root.empty() || !std::filesystem::is_directory(root, error) || error) {
    return std::nullopt;
  }

  std::optional<std::filesystem::path> best_dir;
  std::optional<std::filesystem::file_time_type> best_mtime;

  // 1) The project root and conventional fixed-name build directories.
  ConsiderCandidate(root, best_dir, best_mtime);
  for (const char* name : {"build", "builds", "out"}) {
    ConsiderCandidate(root / name, best_dir, best_mtime);
  }

  // 2) Immediate children: cmake-build-* dirs, and any dir holding a
  //    build.ninja / CMakeCache.txt (a real out-of-source build tree).
  std::error_code iter_error;
  constexpr auto options = std::filesystem::directory_options::skip_permission_denied;
  for (std::filesystem::directory_iterator it(root, options, iter_error), end;
       !iter_error && it != end; it.increment(iter_error)) {
    std::error_code dir_error;
    if (!it->is_directory(dir_error) || dir_error) {
      continue;
    }
    const std::filesystem::path& child = it->path();
    const std::string name = child.filename().string();
    const bool is_cmake_build = name.rfind("cmake-build", 0) == 0;
    std::error_code exists_error;
    const bool has_build_marker =
        std::filesystem::is_regular_file(child / "build.ninja", exists_error) ||
        std::filesystem::is_regular_file(child / "CMakeCache.txt", exists_error);
    if (is_cmake_build || has_build_marker) {
      ConsiderCandidate(child, best_dir, best_mtime);
    }
  }

  return best_dir;
}

}  // namespace microide::project
