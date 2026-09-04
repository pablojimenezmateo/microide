#include "project/ProjectFileScanner.h"

#include <algorithm>
#include <memory>
#include <system_error>

#include "platform/Filesystem.h"
#include "project/IgnoreMatcher.h"
#include "project/SymlinkLoopGuard.h"
#include "util/PathMatch.h"
#include "util/PerformanceTrace.h"
#include "util/PerformanceCounters.h"

namespace microide::project {

namespace {

bool IsHiddenRelativeName(std::string_view relative_text) {
  const std::string_view name = util::NormalizedFileNameView(relative_text);
  return !name.empty() && name.front() == '.';
}

void CollectFiles(const std::filesystem::path& root,
                  const std::filesystem::path& directory,
                  const std::shared_ptr<const IgnoreMatcher>& matcher,
                  ProjectFileScanMode mode,
                  std::vector<std::filesystem::path>& files,
                  SymlinkLoopGuard& loop_guard,
                  int depth,
                  std::size_t& visited,
                  std::size_t entry_budget,
                  ProjectFileScanStatus& status) {
  if (depth > platform::kMaxTreeWalkDepth) {
    // Too deep: stop descending rather than risk a stack overflow. The subtree
    // beneath this point is dropped, so the resulting list is a prefix.
    status.truncated_by_depth = true;
    return;
  }
  // Handle the directory-open error explicitly (rather than relying on
  // skip_permission_denied to swallow it silently) so a directory we cannot read
  // is REPORTED as permission-limited instead of vanishing without a trace. The
  // skip behavior is preserved: on any open error we simply do not descend.
  std::error_code error;
  std::filesystem::directory_iterator iterator(directory, error);
  std::filesystem::directory_iterator end;
  if (error) {
    if (error == std::errc::permission_denied || error == std::errc::operation_not_permitted) {
      status.permission_limited = true;
    } else if (directory == root) {
      // The root itself became unreadable between the caller's validation and now.
      status.error = true;
    } else {
      status.permission_limited = true;
    }
    return;
  }

  // Reused across this directory's entries rather than allocated per entry; see
  // the relative-text derivation below for what they are for.
  std::filesystem::path relative_storage;
#ifdef _WIN32
  std::string generic_relative_scratch;
#endif

  while (!error && iterator != end) {
    if (visited >= entry_budget) {
      // Entry budget exhausted: stop indexing an unaffordably large tree. The
      // list returned so far is only a prefix of the real tree.
      status.truncated_by_budget = true;
      return;
    }
    ++visited;
    // A REFERENCE into the iterator's own entry. Taking a copy cost two
    // allocations per filesystem entry (libstdc++ builds a path's component list
    // eagerly) for a value nothing outlives the loop body with — the recursion
    // below opens its own iterator and leaves this one untouched.
    const std::filesystem::path& path = iterator->path();
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
    //
    // This ran `path.lexically_normal().lexically_relative(root.lexically_normal())`
    // per entry: two normalizations at ~12 allocations each — one of them of the
    // ROOT, which is constant and is now normalized once by the caller — plus a
    // component-walking relative. A directory iterator hands back
    // `directory / name`, which is already normal and already textually under the
    // root, so the relative part is the root prefix removed and nothing else. The
    // authoritative form stays as the fallback for an input that is not.
    std::string_view relative_text;
    if (!util::PathTextNeedsNormalizing(path.native()) &&
        util::NormalizedPathEqualsOrWithin(path, root)) {
      relative_text = util::NormalizedRelativeView(path.native(), root.native());
    } else {
      relative_storage = path.lexically_normal().lexically_relative(root);
      relative_text = relative_storage.native();
    }
    if (relative_text.empty()) {
      iterator.increment(error);
      continue;
    }

    if (mode == ProjectFileScanMode::ExcludeHidden && IsHiddenRelativeName(relative_text)) {
      iterator.increment(error);
      continue;
    }

    // `relative_text` stays the entry's own (native) spelling — that is what gets
    // stored. `matcher_relative_text` is the forward-slash spelling IgnoreMatcher
    // speaks, which the native text already is on POSIX; on Windows it is the
    // conversion, into the same reused buffer ProjectTraversalFilter::Includes
    // uses. Keeping them as two names is the point: overwriting the one view with
    // the other stored the matcher's spelling in the index.
    std::string_view matcher_relative_text = relative_text;
#ifdef _WIN32
    generic_relative_scratch.assign(relative_text);
    for (char& c : generic_relative_scratch) {
      if (c == '\\') {
        c = '/';
      }
    }
    matcher_relative_text = generic_relative_scratch;
#endif

    if (is_directory) {
      // Decide the directory's own ignore status with the PARENT matcher first: a rule
      // loaded from `dir/.gitignore` can never change `dir`'s own status (Rule::Matches
      // bails via the base==relative guard), so the decision is identical either way.
      // Only build the child matcher + stat/open `dir/.gitignore` for directories we
      // actually descend into — otherwise every ignored dir (node_modules, build, .git,
      // target, dist, __pycache__) pays a rules-vector copy + 2 syscalls per refresh just
      // to be discarded. Mirrors DirectoryTree::AppendDirectory.
      if (matcher->IgnoredEntryNormalized(matcher_relative_text, true)) {
        iterator.increment(error);
        continue;
      }
      // Inherit the parent matcher as a shared layer and add only this
      // directory's own .gitignore — no copy of the inherited rules
      // (TD-2026-07-17A-055), and no layer at all when there is nothing to add.
      const std::shared_ptr<const IgnoreMatcher> child_matcher =
          IgnoreMatcher::ForDirectory(matcher, path);
      std::error_code link_error;
      const bool is_symlink = iterator->is_symlink(link_error);
      const SymlinkLoopGuard::Scope scope = loop_guard.TryEnter(path, is_symlink && !link_error);
      if (scope.entered()) {
        CollectFiles(root, path, child_matcher, mode, files, loop_guard, depth + 1, visited,
                     entry_budget, status);
      }
      iterator.increment(error);
      continue;
    }

    if (matcher->IgnoredEntryNormalized(matcher_relative_text, false)) {
      iterator.increment(error);
      continue;
    }

    std::error_code regular_error;
    if (iterator->is_regular_file(regular_error) && !regular_error) {
      // Already lexically normal (see the derivation above), so no second
      // normalize — and from the NATIVE view, not the matcher's.
      files.emplace_back(relative_text);
    }
    iterator.increment(error);
  }
}

}  // namespace

std::vector<std::filesystem::path> CollectProjectFiles(const std::filesystem::path& root,
                                                       ProjectFileScanMode mode,
                                                       bool follow_out_of_root_symlinks,
                                                       const std::vector<std::string>& exclude_globs,
                                                       ProjectFileScanStatus* out_status,
                                                       std::size_t entry_budget) {
  if (out_status != nullptr) {
    *out_status = ProjectFileScanStatus{};
  }
  // The whole-tree rescan behind the sidebar Refresh button, an exclude-glob
  // change, and the forced project-change check. It is the single most expensive
  // thing any of those do and it had no scope of its own -- only a call counter,
  // which cannot say whether a refresh took 5 ms or 500.
  util::PerformanceTrace::Scope perf_scope("project::CollectProjectFiles");
  util::AddPerformanceCounter(util::PerfCounterId::ProjectFileScannerCollectProjectFilesCalls);
  std::error_code error;
  const std::filesystem::path absolute_root = std::filesystem::absolute(root, error);
  if (error || absolute_root.empty() || !std::filesystem::exists(absolute_root, error) || error ||
      !std::filesystem::is_directory(absolute_root, error)) {
    if (out_status != nullptr) {
      out_status->error = true;
    }
    return {};
  }

  // Normalized ONCE, here. Every entry of the walk derives its relative path
  // against it, and re-normalizing a constant per entry was the single most
  // expensive thing the scan did.
  const std::filesystem::path normalized_root = util::NormalizedPath(absolute_root);

  auto matcher = std::make_shared<IgnoreMatcher>();
  matcher->SetRoot(normalized_root);
  // Defaults after the root .gitignore (take precedence), user excludes last.
  matcher->AddDefaultRules();
  matcher->AddExcludeGlobs(exclude_globs);

  std::vector<std::filesystem::path> files;
  SymlinkLoopGuard loop_guard(normalized_root,
                              /*enforce_containment=*/!follow_out_of_root_symlinks);
  std::size_t visited = 0;
  ProjectFileScanStatus status;
  CollectFiles(normalized_root, normalized_root, matcher, mode, files, loop_guard, 1, visited,
               entry_budget, status);
  std::sort(files.begin(), files.end(), [](const auto& lhs, const auto& rhs) {
    return lhs.native() < rhs.native();
  });
  if (out_status != nullptr) {
    *out_status = status;
  }
  return files;
}

}  // namespace microide::project
