#include "project/DirectoryTree.h"

#include <algorithm>
#include <cctype>
#include <system_error>
#include <utility>

#include "platform/Filesystem.h"
#include "project/GitStatusService.h"
#include "project/IgnoreMatcher.h"
#include "project/SymlinkLoopGuard.h"
#include "util/PathMatch.h"
#include "util/PerformanceTrace.h"
#include "util/StartupTrace.h"
#include "util/StringUtil.h"

namespace microide::project {

namespace {

struct SortableEntry {
  std::filesystem::path path;
  std::string sort_key;
  bool is_directory = false;
  bool is_symlink = false;
  bool ignored = false;
  // Parent-linked matcher for descending into this directory (null for files and
  // collapsed dirs, which are never descended); shares the ancestor rule chain
  // instead of copying it (TD-2026-07-17A-055).
  std::shared_ptr<const IgnoreMatcher> matcher;
};

std::string DisplayName(const std::filesystem::path& path) {
  const auto name = path.filename().string();
  if (!name.empty()) {
    return name;
  }
  return path.string();
}

}  // namespace

bool DirectoryTree::SetRoot(const std::filesystem::path& root) {
  util::StartupTrace::Scope trace_scope("DirectoryTree::SetRoot");
  // Non-throwing probes throughout: selecting a root that is inaccessible, on a
  // flaky/unmounted volume, or otherwise un-stattable (ENAMETOOLONG, a parent dir
  // losing +x) must fail the open cleanly rather than throw out of sidebar setup.
  // Mirrors FileIndex::SetRoot / CollectProjectFiles.
  std::error_code error;
  const auto absolute_root = std::filesystem::absolute(root, error);
  if (error || !std::filesystem::exists(absolute_root, error) || error ||
      !std::filesystem::is_directory(absolute_root, error) || error) {
    return false;
  }

  root_ = absolute_root;
  root_generic_ = root_.generic_string();
  expanded_paths_.clear();
  expanded_paths_.insert(NormalizePathKey(root_));
  manually_collapsed_paths_.clear();
  RebuildEntries(false);
  return true;
}

void DirectoryTree::PruneDeletedDirectoryKeys() {
  const std::string root_key = NormalizePathKey(root_);
  const auto prune = [&](PathKeySet& keys) {
    for (auto it = keys.begin(); it != keys.end();) {
      if (*it == root_key) {
        ++it;  // Never drop the root's own expanded key.
        continue;
      }
      std::error_code error;
      if (!std::filesystem::is_directory(*it, error)) {
        it = keys.erase(it);
      } else {
        ++it;
      }
    }
  };
  prune(expanded_paths_);
  prune(manually_collapsed_paths_);
}

void DirectoryTree::MaybePruneDeletedDirectoryKeys() {
  // While the remembered set is small the sweep is trivial, so keep pruning every refresh
  // (a deleted-then-recreated dir then renders collapsed immediately). Once the set grows
  // past that — a large session restore or heavy manual expansion — sweep only at a bounded
  // interval so a refresh no longer pays an O(history) stat sweep for one changed row.
  static constexpr std::size_t kImmediatePruneKeyCount = 64;
  static constexpr std::size_t kKeyPruneInterval = 32;
  const std::size_t remembered = expanded_paths_.size() + manually_collapsed_paths_.size();
  if (remembered > kImmediatePruneKeyCount && ++refreshes_since_key_prune_ < kKeyPruneInterval) {
    return;
  }
  refreshes_since_key_prune_ = 0;
  PruneDeletedDirectoryKeys();
}

void DirectoryTree::Refresh() {
  util::PerformanceTrace::Scope perf_scope("DirectoryTree::Refresh");
  if (root_.empty()) {
    return;
  }
  MaybePruneDeletedDirectoryKeys();
  RebuildEntries(false);
}


void DirectoryTree::ApplyGitStatuses(SharedGitTreeStatusMap statuses) {
  git_statuses_ = std::move(statuses);
  // One scratch buffer for the whole sweep — after the first few entries it is
  // already large enough, so the sweep stops allocating entirely.
  std::string scratch;
  for (auto& entry : entries_) {
    entry.git_status = EntryGitStatus(entry.path, scratch);
  }
}

void DirectoryTree::MoveSelection(int delta) {
  if (entries_.empty() || delta == 0) {
    return;
  }

  const int current = static_cast<int>(selected_index_);
  const int max_index = static_cast<int>(entries_.size()) - 1;
  selected_index_ = static_cast<std::size_t>(std::clamp(current + delta, 0, max_index));
}

void DirectoryTree::SetSelectedIndex(std::size_t index) {
  if (entries_.empty()) {
    selected_index_ = 0;
    return;
  }
  selected_index_ = std::min(index, entries_.size() - 1);
}

bool DirectoryTree::SelectPath(const std::filesystem::path& path) {
  if (root_.empty() || path.empty()) {
    return false;
  }

  std::error_code error;
  const auto absolute_path = std::filesystem::absolute(path, error);
  if (error || absolute_path.empty()) {
    return false;
  }

  const auto normalized_path = absolute_path.lexically_normal();
  if (normalized_path == root_) {
    selected_index_ = 0;
    return true;
  }

  const auto relative = normalized_path.lexically_relative(root_);
  if (relative.empty() ||
      (relative.begin() != relative.end() &&
       *relative.begin() == std::filesystem::path(".."))) {
    return false;
  }

  for (auto current = normalized_path.parent_path(); !current.empty() && current != root_;
       current = current.parent_path()) {
    const auto current_relative = current.lexically_relative(root_);
    if (current_relative.empty() ||
        (current_relative.begin() != current_relative.end() &&
         *current_relative.begin() == std::filesystem::path(".."))) {
      break;
    }
    expanded_paths_.insert(NormalizePathKey(current));
    manually_collapsed_paths_.erase(NormalizePathKey(current));
  }

  RebuildEntries(false);
  for (std::size_t i = 0; i < entries_.size(); ++i) {
    if (entries_[i].path == normalized_path) {
      selected_index_ = i;
      return true;
    }
  }
  return false;
}

bool DirectoryTree::SelectPathIfVisible(const std::filesystem::path& path) {
  if (root_.empty() || path.empty()) {
    return false;
  }

  std::error_code error;
  const auto absolute_path = std::filesystem::absolute(path, error);
  if (error || absolute_path.empty()) {
    return false;
  }
  const auto normalized_path = absolute_path.lexically_normal();
  for (std::size_t i = 0; i < entries_.size(); ++i) {
    if (entries_[i].path == normalized_path) {
      selected_index_ = i;
      return true;
    }
  }
  return false;
}

bool DirectoryTree::HasManuallyCollapsedAncestor(const std::filesystem::path& path) const {
  if (root_.empty() || path.empty()) {
    return false;
  }

  std::error_code error;
  const auto absolute_path = std::filesystem::absolute(path, error);
  if (error || absolute_path.empty()) {
    return false;
  }

  const auto normalized_path = absolute_path.lexically_normal();
  // Component-wise containment via the sanctioned helper, not a raw byte-prefix
  // test: rfind(root,0)==0 would treat `/a/project/...` as inside root `/a/proj`.
  for (auto current = normalized_path.parent_path();
       !current.empty() && current != root_ && util::PathEqualsOrWithin(current, root_);
       current = current.parent_path()) {
    if (ContainsPathKey(manually_collapsed_paths_, current)) {
      return true;
    }
  }
  return false;
}

void DirectoryTree::ExpandSelection() {
  if (entries_.empty()) {
    return;
  }

  const auto& entry = entries_[selected_index_];
  if (!entry.is_directory) {
    return;
  }

  const auto key = NormalizePathKey(entry.path);
  if (expanded_paths_.insert(key).second) {
    manually_collapsed_paths_.erase(key);
    RebuildEntries(false);
    return;
  }

  if (selected_index_ + 1 < entries_.size()) {
    ++selected_index_;
  }
}

void DirectoryTree::CollapseSelection() {
  if (entries_.empty()) {
    return;
  }

  const auto& entry = entries_[selected_index_];
  if (entry.is_directory && entry.expanded && entry.path != root_) {
    manually_collapsed_paths_.insert(NormalizePathKey(entry.path));
    expanded_paths_.erase(NormalizePathKey(entry.path));
    RebuildEntries(false);
    return;
  }

  if (entry.depth == 0) {
    return;
  }

  for (std::size_t i = selected_index_; i > 0; --i) {
    const auto parent_index = i - 1;
    if (entries_[parent_index].depth == entry.depth - 1) {
      selected_index_ = parent_index;
      return;
    }
  }
}

void DirectoryTree::CollapseAll() {
  if (root_.empty()) {
    return;
  }

  const auto selected_path =
      entries_.empty() ? root_ : entries_[selected_index_].path.lexically_normal();
  manually_collapsed_paths_.clear();
  for (const auto& entry : entries_) {
    if (entry.is_directory && entry.path != root_) {
      manually_collapsed_paths_.insert(NormalizePathKey(entry.path));
    }
  }
  expanded_paths_.clear();
  expanded_paths_.insert(NormalizePathKey(root_));
  RebuildEntries(false);

  std::filesystem::path visible_path = selected_path;
  while (!visible_path.empty()) {
    for (std::size_t i = 0; i < entries_.size(); ++i) {
      if (entries_[i].path == visible_path) {
        selected_index_ = i;
        return;
      }
    }
    if (visible_path == root_) {
      break;
    }
    visible_path = visible_path.parent_path().lexically_normal();
  }

  selected_index_ = 0;
}

std::optional<std::filesystem::path> DirectoryTree::ActivateSelection() {
  if (entries_.empty()) {
    return std::nullopt;
  }

  const auto& entry = entries_[selected_index_];
  if (entry.is_directory) {
    const auto key = NormalizePathKey(entry.path);
    if (entry.expanded && entry.path != root_) {
      manually_collapsed_paths_.insert(key);
      expanded_paths_.erase(key);
    } else {
      manually_collapsed_paths_.erase(key);
      expanded_paths_.insert(key);
    }
    RebuildEntries(false);
    return std::nullopt;
  }

  return entry.path;
}

void DirectoryTree::RebuildEntries(bool refresh_git_statuses) {
  util::StartupTrace::Scope trace_scope("DirectoryTree::RebuildEntries");
  util::PerformanceTrace::Scope perf_scope("DirectoryTree::RebuildEntries");
  ++entries_revision_;
  const auto selected_path =
      entries_.empty() ? std::filesystem::path{} : entries_[selected_index_].path;

  entries_.clear();
  if (root_.empty()) {
    git_statuses_.reset();
    selected_index_ = 0;
    return;
  }

  auto matcher = std::make_shared<IgnoreMatcher>();
  matcher->SetRoot(root_);
  // Seed the same defaults + user excludes the index/finder use, so VCS metadata,
  // dependency, and build-output dirs render grayed (ignored) here rather than as
  // normal entries — and stay consistent with what the finder indexes.
  matcher->AddDefaultRules();
  matcher->AddExcludeGlobs(exclude_globs_);
  if (refresh_git_statuses) {
    git_statuses_ = std::make_shared<const GitTreeStatusMap>(CollectGitStatuses(root_));
  }
  entries_.push_back(TreeEntry{
      .path = root_,
      .label = DisplayName(root_),
      .depth = 0,
      .is_directory = true,
      .expanded = true,
      .ignored = false,
      .children_materialized = true,
      .git_status = GitFileStatus::Clean,
  });
  SymlinkLoopGuard loop_guard(root_,
                              /*enforce_containment=*/!follow_out_of_root_symlinks_);
  AppendDirectory(root_, 1, matcher, loop_guard);

  selected_index_ = 0;
  for (std::size_t i = 0; i < entries_.size(); ++i) {
    if (entries_[i].path == selected_path) {
      selected_index_ = i;
      break;
    }
  }
}

void DirectoryTree::AppendDirectory(const std::filesystem::path& directory,
                                    int depth,
                                    const std::shared_ptr<const IgnoreMatcher>& matcher,
                                    SymlinkLoopGuard& loop_guard) {
  util::PerformanceTrace::Scope perf_scope("DirectoryTree::AppendDirectory");
  if (depth > platform::kMaxTreeWalkDepth) {
    return;  // bound native-stack recursion on a pathologically deep tree
  }
  if (directory != root_ && !IsExpanded(directory)) {
    return;
  }

  std::error_code error;
  std::vector<SortableEntry> children;
  std::filesystem::directory_iterator iterator(
      directory, std::filesystem::directory_options::skip_permission_denied, error);
  std::filesystem::directory_iterator end;
  while (!error && iterator != end) {
    const auto path = iterator->path();
    const std::string sort_key = path.filename().string();
    const auto relative = path.lexically_relative(root_);
    if (relative.empty()) {
      iterator.increment(error);
      continue;
    }

    std::error_code type_error;
    const bool is_directory = iterator->is_directory(type_error);
    if (type_error) {
      iterator.increment(error);
      continue;
    }

    // .git/.svn/build-output/etc. are no longer special-cased: the seeded matcher
    // marks them ignored, so they render grayed (and stay lazily expandable).
    const bool ignored = matcher->Ignored(relative, is_directory);
    if (sort_key.starts_with('.') && !ignored) {
      iterator.increment(error);
      continue;
    }

    if (is_directory) {
      std::error_code link_error;
      const bool is_symlink = iterator->is_symlink(link_error);
      // The dir's own .gitignore only affects its GRANDCHILDREN, which are walked
      // only when the dir is expanded (AppendDirectory early-returns for a collapsed
      // dir). The child's own `ignored` flag above uses the parent `matcher`, not
      // this one. So skip the matcher copy + .gitignore stat/open for collapsed rows
      // — otherwise every refresh pays ~2 syscalls + a rules-vector copy per collapsed
      // directory that is immediately discarded (a monorepo has many at the root).
      std::shared_ptr<const IgnoreMatcher> child_matcher;
      if (IsExpanded(path)) {
        // Inherit the parent as a shared layer + this directory's own .gitignore;
        // no copy of the inherited rule set (TD-2026-07-17A-055).
        std::shared_ptr<IgnoreMatcher> child = IgnoreMatcher::MakeChild(matcher);
        child->LoadIgnoreFile(path / ".gitignore");
        child_matcher = std::move(child);
      }
      children.push_back(SortableEntry{
          .path = path,
          .sort_key = sort_key,
          .is_directory = true,
          .is_symlink = is_symlink && !link_error,
          .ignored = ignored,
          .matcher = std::move(child_matcher),
      });
      iterator.increment(error);
      continue;
    }

    children.push_back(SortableEntry{
        .path = path,
        .sort_key = sort_key,
        .is_directory = false,
        .ignored = ignored,
        .matcher = nullptr,
    });
    iterator.increment(error);
  }

  std::sort(children.begin(), children.end(), [](const SortableEntry& lhs, const SortableEntry& rhs) {
    if (lhs.is_directory != rhs.is_directory) {
      return lhs.is_directory > rhs.is_directory;
    }
    return lhs.sort_key < rhs.sort_key;
  });

  // Reused across this directory's children (see EntryGitStatus).
  std::string git_key_scratch;
  for (const auto& child : children) {
    const bool expanded = child.is_directory && IsExpanded(child.path);
    entries_.push_back(TreeEntry{
        .path = child.path,
        .label = DisplayName(child.path),
        .depth = depth,
        .is_directory = child.is_directory,
        .expanded = expanded,
        .ignored = child.ignored,
        .children_materialized = expanded,
        .git_status = EntryGitStatus(child.path, git_key_scratch),
    });

    if (child.is_directory) {
      const SymlinkLoopGuard::Scope scope = loop_guard.TryEnter(child.path, child.is_symlink);
      if (scope.entered()) {
        AppendDirectory(child.path, depth + 1, child.matcher, loop_guard);
      }
    }
  }
}

namespace {

std::vector<std::string> RelativeKeysExcludingRoot(
    const DirectoryTree::PathKeySet& keys,
    const std::filesystem::path& root) {
  // Non-throwing overload with a lexical fallback, matching NormalizePathKey and
  // every other absolute() probe in this file: this runs on the persistence-save
  // path, where the throwing overload could abort the process if current_path()
  // fails (unmounted CWD, ENOMEM) instead of degrading to a best-effort key.
  std::error_code root_error;
  const auto absolute_root = std::filesystem::absolute(root, root_error);
  const std::string root_key =
      (root_error ? root.lexically_normal() : absolute_root.lexically_normal()).generic_string();
  std::vector<std::string> relative;
  relative.reserve(keys.size());
  for (const auto& key : keys) {
    if (key == root_key) {
      continue;
    }
    const auto rel = std::filesystem::path(key).lexically_relative(root);
    if (rel.empty() || rel == std::filesystem::path(".")) {
      continue;
    }
    relative.push_back(rel.generic_string());
  }
  // Deterministic order so persisted records are stable across saves.
  std::sort(relative.begin(), relative.end());
  return relative;
}

}  // namespace

std::vector<std::string> DirectoryTree::ExpandedRelativePaths() const {
  if (root_.empty()) {
    return {};
  }
  return RelativeKeysExcludingRoot(expanded_paths_, root_);
}

std::vector<std::string> DirectoryTree::ManuallyCollapsedRelativePaths() const {
  if (root_.empty()) {
    return {};
  }
  return RelativeKeysExcludingRoot(manually_collapsed_paths_, root_);
}

std::optional<std::filesystem::path> DirectoryTree::SelectedPath() const {
  if (entries_.empty() || selected_index_ >= entries_.size()) {
    return std::nullopt;
  }
  return entries_[selected_index_].path;
}

void DirectoryTree::RestoreExpansionState(const std::vector<std::string>& expanded_relative,
                                          const std::vector<std::string>& collapsed_relative) {
  if (root_.empty()) {
    return;
  }
  // Reject restored keys that are absolute, escape the root via `..`, or otherwise
  // resolve outside the project. A tampered or cross-root session file could otherwise
  // seed outside-root keys that PruneDeletedDirectoryKeys would stat (a syscall on an
  // arbitrary path) or that would be re-serialized as outside-root relatives
  // (TD-2026-07-17A-089). Purely lexical — no filesystem access.
  const auto insert_contained = [this](PathKeySet& target,
                                       const std::string& relative) {
    if (relative.empty()) {
      return;
    }
    const std::filesystem::path candidate = root_ / std::filesystem::path(relative);
    if (!util::PathEqualsOrWithin(candidate, root_)) {
      return;
    }
    target.insert(NormalizePathKey(candidate));
  };

  expanded_paths_.clear();
  expanded_paths_.insert(NormalizePathKey(root_));
  for (const auto& relative : expanded_relative) {
    insert_contained(expanded_paths_, relative);
  }
  manually_collapsed_paths_.clear();
  for (const auto& relative : collapsed_relative) {
    insert_contained(manually_collapsed_paths_, relative);
  }
  RebuildEntries(false);
}

bool DirectoryTree::has_dirty_files() const {
  if (git_statuses_ == nullptr) {
    return false;
  }
  for (const auto& [_, status] : *git_statuses_) {
    if (status != GitFileStatus::Clean) {
      return true;
    }
  }
  return false;
}

GitFileStatus DirectoryTree::EntryGitStatus(const std::filesystem::path& path,
                                            std::string& scratch) const {
  if (git_statuses_ == nullptr || git_statuses_->empty() || root_generic_.empty()) {
    return GitFileStatus::Clean;
  }

  // Match the writer's key form: GitPorcelainParser stores keys in generic
  // ('/'-separated) form, relative to the repository root. Using the native string
  // here would look up backslash-separated keys on Windows and never hit, so no
  // badge would ever resolve.
  //
  // Every tree entry is enumerated from root_, so its path is root_ plus a relative
  // tail — the key is a prefix strip, not the lexically_relative + lexically_normal
  // + generic_string chain this used to run (measured 12x faster over a 5000-entry
  // sweep, identical hits). A path that is NOT under root_ (an out-of-root symlink
  // target, when following is enabled) has no repository-relative key at all, which
  // is the Clean the old code also produced via a "../.." relative that never hit.
#if defined(_WIN32)
  scratch = path.generic_string();
#else
  scratch.assign(path.native());  // POSIX: the native form IS the generic form.
#endif
  // A root recorded with a trailing separator (e.g. "/") owns its separator, so the
  // tail starts immediately after the prefix instead of one byte later.
  const bool root_ends_with_separator = root_generic_.back() == '/';
  const std::size_t tail = root_generic_.size() + (root_ends_with_separator ? 0 : 1);
  if (scratch.size() <= tail || scratch.compare(0, root_generic_.size(), root_generic_) != 0 ||
      (!root_ends_with_separator && scratch[root_generic_.size()] != '/')) {
    return GitFileStatus::Clean;
  }
  scratch.erase(0, tail);

  const auto it = git_statuses_->find(scratch);
  return it == git_statuses_->end() ? GitFileStatus::Clean : it->second;
}

bool DirectoryTree::ContainsPathKey(const PathKeySet& keys, const std::filesystem::path& path) {
#if !defined(_WIN32)
  // POSIX: the native spelling IS the generic spelling, and `absolute()` returns
  // an already-absolute path unchanged, so an absolute path whose text is already
  // normal is byte-identical to its NormalizePathKey.
  if (const std::string& text = path.native();
      !text.empty() && text.front() == '/' && !util::PathTextNeedsNormalizing(text)) {
    return keys.contains(std::string_view(text));
  }
#endif
  return keys.contains(NormalizePathKey(path));
}

bool DirectoryTree::IsExpanded(const std::filesystem::path& path) const {
  return ContainsPathKey(expanded_paths_, path);
}

bool DirectoryTree::CanCollapseAll() const {
  return !root_.empty() && expanded_paths_.size() > 1;
}

std::string DirectoryTree::NormalizePathKey(const std::filesystem::path& path) {
  std::error_code error;
  const auto absolute = std::filesystem::absolute(path, error);
  std::string key =
      error ? path.lexically_normal().generic_string() : absolute.lexically_normal().generic_string();
#ifdef _WIN32
  key = util::ToLowerAscii(key);
#endif
  return key;
}

}  // namespace microide::project
