#include "project/CompileCommandsLocator.h"

#include <string>
#include <system_error>

#include "platform/ProcessLauncher.h"

namespace microide::project {

namespace {

constexpr const char* kCompileCommands = "compile_commands.json";

std::optional<std::filesystem::file_time_type> CompileCommandsMtime(
    const std::filesystem::path& dir) {
  std::error_code error;
  const std::filesystem::path candidate = dir / kCompileCommands;
  if (!std::filesystem::is_regular_file(candidate, error) || error) {
    return std::nullopt;
  }
  const auto mtime = std::filesystem::last_write_time(candidate, error);
  if (error) {
    return std::filesystem::file_time_type{};
  }
  return mtime;
}

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

}

std::optional<std::filesystem::path> DiscoverCompileCommandsDir(
    const std::filesystem::path& project_root) {
  std::error_code error;
  const std::filesystem::path root = project_root.lexically_normal();
  if (root.empty() || !std::filesystem::is_directory(root, error) || error) {
    return std::nullopt;
  }

  std::optional<std::filesystem::path> best_dir;
  std::optional<std::filesystem::file_time_type> best_mtime;

  ConsiderCandidate(root, best_dir, best_mtime);
  for (const char* name : {"build", "builds", "out"}) {
    ConsiderCandidate(root / name, best_dir, best_mtime);
  }

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

ClangdCommandPlan PlanClangdCommand(std::vector<std::string> command,
                                    const std::filesystem::path& project_root,
                                    const platform::ProcessLauncher& launcher) {
  ClangdCommandPlan plan{.command = std::move(command)};
  if (plan.command.empty() ||
      std::filesystem::path(plan.command.front()).filename().string().rfind("clangd", 0) != 0) {
    return plan;
  }
  for (const std::string& arg : plan.command) {
    if (arg.rfind("--compile-commands-dir", 0) == 0) {
      return plan;
    }
  }
  if (!launcher.is_local()) {
    plan.note = "cpp-lsp: clangd runs on " + std::string(launcher.description()) +
                " and finds compile_commands.json there itself (next to the sources or in "
                "build/); for another build directory, name it in the project's .clangd "
                "(CompileFlags: CompilationDatabase: <dir>)";
    return plan;
  }
  const std::optional<std::filesystem::path> dir = DiscoverCompileCommandsDir(project_root);
  if (!dir.has_value()) {
    plan.note = "cpp-lsp: no compile_commands.json found under " + project_root.generic_string() +
                " -- clangd cross-file features will be limited until one is generated";
    return plan;
  }
  plan.command.push_back("--compile-commands-dir=" +
                         launcher.ResolveWorkingDirectory(*dir).generic_string());
  plan.note = "cpp-lsp: pointing clangd at compile_commands.json in " + dir->generic_string();
  return plan;
}

}
