#include "project/ProjectFileScanner.h"

#include <algorithm>
#include <system_error>

#include "platform/Filesystem.h"
#include "project/IgnoreMatcher.h"
#include "project/SymlinkLoopGuard.h"
#include "util/PerformanceCounters.h"

namespace microide::project {

namespace {

bool IsHiddenName(const std::filesystem::path& path) {
  const std::string name = path.filename().string();
  return !name.empty() && name[0] == '.';
}

void CollectFiles(const std::filesystem::path& root,
                  const std::filesystem::path& directory,
                  const IgnoreMatcher& matcher,
                  ProjectFileScanMode mode,
                  std::vector<std::filesystem::path>& files,
                  SymlinkLoopGuard& loop_guard,
                  int depth,
                  std::size_t& visited) {
  if (depth > platform::kMaxTreeWalkDepth) {
    return;  // too deep: stop descending rather than risk a stack overflow
  }
  std::error_code error;
  std::filesystem::directory_iterator iterator(
      directory, std::filesystem::directory_options::skip_permission_denied, error);
  std::filesystem::directory_iterator end;

  while (!error && iterator != end) {
    if (visited >= platform::kTreeTraversalEntryBudget) {
      return;  // entry budget exhausted: stop indexing an unaffordably large tree
    }
    ++visited;
    const std::filesystem::path path = iterator->path();
    // Use the non-throwing overloads: is_directory()/is_regular_file() follow the
    // symlink and stat it, which THROWS filesystem_error on ELOOP (a symlink cycle
    // or an over-40-deep chain). The scan runs on the shell thread with no catch, so
    // the throwing overload turned a self-referential symlink — input the codebase
    // explicitly expects, hence SymlinkLoopGuard — into a std::terminate. Classify
    // safely and skip the entry on error, mirroring DirectoryTree::AppendDirectory.
    std::error_code type_error;
    const bool is_directory = iterator->is_directory(type_error);
    if (type_error) {
      iterator.increment(error);
      continue;
    }

    // .git/.svn/build-output/etc. are pruned by the seeded matcher below (default
    // rules), so no name is special-cased here.
    const std::filesystem::path relative =
        path.lexically_normal().lexically_relative(root.lexically_normal());
    if (relative.empty()) {
      iterator.increment(error);
      continue;
    }

    if (mode == ProjectFileScanMode::ExcludeHidden && IsHiddenName(path)) {
      iterator.increment(error);
      continue;
    }

    if (is_directory) {
      // Decide the directory's own ignore status with the PARENT matcher first: a rule
      // loaded from `dir/.gitignore` can never change `dir`'s own status (Rule::Matches
      // bails via the base==relative guard), so the decision is identical either way.
      // Only build the child matcher + stat/open `dir/.gitignore` for directories we
      // actually descend into — otherwise every ignored dir (node_modules, build, .git,
      // target, dist, __pycache__) pays a rules-vector copy + 2 syscalls per refresh just
      // to be discarded. Mirrors DirectoryTree::AppendDirectory.
      if (matcher.Ignored(relative, true)) {
        iterator.increment(error);
        continue;
      }
      IgnoreMatcher child_matcher = matcher;
      child_matcher.LoadIgnoreFile(path / ".gitignore");
      std::error_code link_error;
      const bool is_symlink = iterator->is_symlink(link_error);
      const SymlinkLoopGuard::Scope scope = loop_guard.TryEnter(path, is_symlink && !link_error);
      if (scope.entered()) {
        CollectFiles(root, path, child_matcher, mode, files, loop_guard, depth + 1, visited);
      }
      iterator.increment(error);
      continue;
    }

    if (matcher.Ignored(relative, false)) {
      iterator.increment(error);
      continue;
    }

    std::error_code regular_error;
    if (iterator->is_regular_file(regular_error) && !regular_error) {
      files.push_back(relative.lexically_normal());
    }
    iterator.increment(error);
  }
}

}  // namespace

std::vector<std::filesystem::path> CollectProjectFiles(const std::filesystem::path& root,
                                                       ProjectFileScanMode mode,
                                                       bool follow_out_of_root_symlinks,
                                                       const std::vector<std::string>& exclude_globs) {
  util::AddPerformanceCounter(util::PerfCounterId::ProjectFileScannerCollectProjectFilesCalls);
  std::error_code error;
  const std::filesystem::path absolute_root = std::filesystem::absolute(root, error);
  if (error || absolute_root.empty() || !std::filesystem::exists(absolute_root, error) || error ||
      !std::filesystem::is_directory(absolute_root, error)) {
    return {};
  }

  IgnoreMatcher matcher;
  matcher.SetRoot(absolute_root);
  // Defaults after the root .gitignore (take precedence), user excludes last.
  matcher.AddDefaultRules();
  matcher.AddExcludeGlobs(exclude_globs);

  std::vector<std::filesystem::path> files;
  SymlinkLoopGuard loop_guard(absolute_root,
                              /*enforce_containment=*/!follow_out_of_root_symlinks);
  std::size_t visited = 0;
  CollectFiles(absolute_root, absolute_root, matcher, mode, files, loop_guard, 1, visited);
  std::sort(files.begin(), files.end(), [](const auto& lhs, const auto& rhs) {
    return lhs.native() < rhs.native();
  });
  return files;
}

}  // namespace microide::project
