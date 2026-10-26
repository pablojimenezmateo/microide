#include "project/ProjectFileScanner.h"

#include <algorithm>
#include <memory>
#include <system_error>

#include "platform/DirectoryWalk.h"
#include "platform/Filesystem.h"
#include "project/ProjectTraversalFilter.h"
#include "project/SymlinkLoopGuard.h"
#include "util/PathMatch.h"
#include "util/PerformanceTrace.h"
#include "util/PerformanceCounters.h"

namespace microide::project {

namespace {

// The project scan as a walk visitor. What to keep is ProjectTraversalFilter's
// decision, the one every whole-tree walk shares; this used to run its own
// IgnoreMatcher chain beside it, "deliberately identical" by hand (TD-2026-08-17-256).
// What is the scanner's own: the hidden-name mode, following directory symlinks
// under a loop guard, the entry budget, and the relative paths it returns.
class ScanVisitor final : public platform::TreeWalkVisitor {
 public:
  ScanVisitor(const std::filesystem::path& root, ProjectFileScanMode mode,
              const std::vector<std::string>& exclude_globs, bool follow_out_of_root_symlinks,
              std::size_t entry_budget, std::vector<std::filesystem::path>& files,
              ProjectFileScanStatus& status)
      : root_native_(root.native()),
        mode_(mode),
        filter_(root, exclude_globs),
        loop_guard_(root, /*enforce_containment=*/!follow_out_of_root_symlinks),
        entry_budget_(entry_budget),
        files_(files),
        status_(status) {}

  platform::WalkAction Visit(const platform::WalkEntry& entry) override {
    if (visited_ >= entry_budget_) {
      // Entry budget exhausted: stop indexing an unaffordably large tree. The
      // list returned so far is only a prefix of the real tree.
      status_.truncated_by_budget = true;
      return platform::WalkAction::Stop;
    }
    ++visited_;
    const bool is_directory = entry.type == platform::PathType::Directory;
    if (mode_ == ProjectFileScanMode::ExcludeHidden && entry.name.starts_with('.')) {
      return platform::WalkAction::Prune;
    }
    if (is_directory) {
      if (!filter_.Includes(entry.path, platform::PathType::Directory)) {
        return platform::WalkAction::Prune;
      }
      // A real directory cannot start a cycle and enters untracked; a symlinked
      // one is followed only to a target inside the root that is not already on
      // this branch. Leave pops the scope, so the branch is exactly the stack.
      SymlinkLoopGuard::Scope scope =
          loop_guard_.TryEnter(std::filesystem::path(entry.path), entry.is_symlink);
      if (!scope.entered()) {
        return platform::WalkAction::Prune;
      }
      branch_.push_back(std::move(scope));
      return platform::WalkAction::Continue;
    }
    if (entry.type == platform::PathType::RegularFile &&
        filter_.Includes(entry.path, platform::PathType::RegularFile)) {
      // The walk spells every entry as the (normalized) root plus its relative
      // part, so the relative path is the prefix removed.
      files_.emplace_back(util::NormalizedRelativeView(entry.path, root_native_));
    }
    return platform::WalkAction::Continue;
  }

  void Leave(const platform::WalkEntry& directory) override {
    (void) directory;
    branch_.pop_back();
  }

 private:
  std::string_view root_native_;
  ProjectFileScanMode mode_;
  ProjectTraversalFilter filter_;
  SymlinkLoopGuard loop_guard_;
  std::vector<SymlinkLoopGuard::Scope> branch_;
  std::size_t entry_budget_;
  std::size_t visited_ = 0;
  std::vector<std::filesystem::path>& files_;
  ProjectFileScanStatus& status_;
};

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

  std::vector<std::filesystem::path> files;
  ProjectFileScanStatus status;
  ScanVisitor visitor(normalized_root, mode, exclude_globs, follow_out_of_root_symlinks,
                      entry_budget, files, status);
  const platform::WalkStatus walk = platform::WalkDirectoryTree(
      normalized_root, platform::WalkOptions{.follow_directory_symlinks = true}, visitor);
  // The root itself became unreadable between the validation above and the walk.
  status.error = status.error || walk.root_unreadable;
  // A directory that could not be opened is REPORTED, not silently missing, whatever
  // the reason it could not be.
  status.permission_limited = status.permission_limited || walk.permission_limited ||
                              walk.incomplete;
  status.truncated_by_depth = status.truncated_by_depth || walk.truncated_by_depth;
  std::sort(files.begin(), files.end(), [](const auto& lhs, const auto& rhs) {
    return lhs.native() < rhs.native();
  });
  if (out_status != nullptr) {
    *out_status = status;
  }
  return files;
}

}  // namespace microide::project
