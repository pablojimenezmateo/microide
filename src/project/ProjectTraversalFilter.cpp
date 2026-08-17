#include "project/ProjectTraversalFilter.h"

#include <string>
#include <utility>

#include "util/PathMatch.h"
#include "util/PerformanceCounters.h"

namespace microide::project {

ProjectTraversalFilter::ProjectTraversalFilter(std::filesystem::path root,
                                               std::vector<std::string> extra_excludes)
    : root_(std::move(root).lexically_normal()) {
  // A trailing separator leaves path::filename() empty, so `path("/a/b/") != path("/a/b")`.
  // Ancestor walks (parent_path() never yields a trailing separator) would then never
  // compare equal to a trailing-slash root and would recurse past it up to "/", looping
  // forever there. Strip it so MatcherForParentDirectory terminates at the project root.
  if (!root_.has_filename() && root_.has_parent_path()) {
    root_ = root_.parent_path();
  }
  root_native_ = root_.native();
  auto root_matcher = std::make_shared<IgnoreMatcher>();
  root_matcher->SetRoot(root_);
  // Defaults after the root .gitignore so they take precedence; user excludes last
  // so an explicit "!name/" re-include wins over both.
  root_matcher->AddDefaultRules();
  root_matcher->AddExcludeGlobs(extra_excludes);
  root_matcher_ = std::move(root_matcher);
}

bool ProjectTraversalFilter::Includes(const std::filesystem::path& path, platform::PathType type) {
  if (root_.empty()) {
    return true;
  }
  util::AddPerformanceCounter(util::PerfCounterId::ProjectIgnoreFilterQueries);
  // This runs ONCE PER FILESYSTEM ENTRY of every index walk, the poll re-walk, the
  // inotify registration, the sidebar tree and the project scanner — so the whole
  // body below is written to derive its answer from views into the caller's own
  // path text rather than from `path` temporaries. `lexically_normal()` alone is
  // ~12 allocations and a no-op for the normalized paths a directory iterator
  // hands us (TD-2026-08-10-174), so it only runs for an unusually spelled input.
  std::filesystem::path normalized_storage;
  const std::filesystem::path* normalized = &path;
  if (util::PathTextNeedsNormalizing(path.native())) {
    normalized_storage = path.lexically_normal();
    normalized = &normalized_storage;
  }
  const std::string_view normalized_text = normalized->native();
  if (normalized_text == root_native_) {
    return true;
  }
  // A path that is not nested under the root escapes the project boundary
  // (lexically_relative would yield a "../…" relative that the old prefix
  // derivation accepted). Reject it outright so a watcher/scanner/helper event for an
  // out-of-root file cannot pass the filter and feed out-of-root indexing.
  // Both sides are already normalized (`normalized` just above, root_ in the
  // constructor), so use the allocation-free variant.
  if (!util::NormalizedPathEqualsOrWithin(*normalized, root_)) {
    return false;
  }

  const bool is_directory = type == platform::PathType::Directory;

  // The containment check just proved the root is a prefix, so the relative part
  // is that prefix removed — no lexically_relative()/lexically_normal() pair.
  std::string_view relative = util::NormalizedRelativeView(normalized_text, root_native_);
  if (relative.empty()) {
    return true;
  }
#ifdef _WIN32
  // IgnoreMatcher speaks generic (forward-slash) relatives, which on POSIX the
  // native text already is. On Windows it is not, so the separators are swapped
  // into a reused member buffer -- one conversion per entry rather than the
  // lexically_relative()/generic_string() pair, and no per-entry allocation once
  // the buffer has grown.
  generic_relative_scratch_.assign(relative);
  for (char& c : generic_relative_scratch_) {
    if (c == '\\') {
      c = '/';
    }
  }
  relative = generic_relative_scratch_;
#endif

  DirectoryState* const state = StateForParentDirectoryText(
      util::NormalizedParentDirectoryView(normalized_text), *normalized);
  const IgnoreMatcher& matcher = state != nullptr ? *state->matcher : *root_matcher_;

  // Ancestor chain first, because it is one cached byte after the first entry in
  // this directory while the entry's own test below is the full rule set. The two
  // are independent predicates that both veto, so the order only changes which one
  // gets to short-circuit.
  //
  // Walk the chain by trimming `relative` at each separator, innermost first — the
  // same sequence `parent_path()` would yield, without a path per ancestor.
  // (Ignored directories are also pruned via disable_recursion_pending in the
  // walk, so this is defense-in-depth for the callers that ask about one path.)
  // The answer depends only on the parent directory, so it is computed once per
  // directory rather than once per entry: this loop used to be ~70 % of a
  // whole-tree scan, running the whole rule set once per path component of every
  // file (TD-2026-08-17-257).
  if (state != nullptr) {
    if (state->ancestors_ignored == kUnknown) {
      util::AddPerformanceCounter(util::PerfCounterId::ProjectIgnoreFilterAncestorScans);
      state->ancestors_ignored = 0;
      for (std::string_view parent = util::NormalizedParentDirectoryView(relative);
           !parent.empty() && parent != ".";) {
        if (matcher.IgnoredNormalized(parent, true)) {
          state->ancestors_ignored = 1;
          break;
        }
        const std::string_view next = util::NormalizedParentDirectoryView(parent);
        if (next.size() >= parent.size()) {
          break;  // "/" is its own parent. A relative path never reaches it; this
                  // keeps a malformed input from spinning here rather than pruning.
        }
        parent = next;
      }
    }
    if (state->ancestors_ignored == 1) {
      return false;
    }
  }

  // `relative` is already lexically-normalized, so the string_view overload skips
  // the per-call re-normalization the path overload would otherwise perform.
  return !matcher.IgnoredNormalized(relative, is_directory);
}

ProjectTraversalFilter::DirectoryState* ProjectTraversalFilter::StateForParentDirectoryText(
    std::string_view directory_native, const std::filesystem::path& normalized_child) {
  // The two answers that dominate a walk — "this entry sits directly in the
  // project root" and "this directory's state is already cached" — are both
  // reachable from the candidate's own text, so neither builds a path or a key.
  // A root-level entry has no ancestor chain at all, so it needs no entry.
  if (directory_native.empty() || directory_native == root_native_) {
    return nullptr;
  }
  const auto existing = directory_states_.find(directory_native);
  if (existing != directory_states_.end()) {
    return &existing->second;
  }
  // First entry seen in this directory: build the matcher through the general
  // form, which owns the recursion, the .gitignore load and the key insert.
  // `normalized_child`'s parent is normalized because `normalized_child` is.
  MatcherForParentDirectory(normalized_child.parent_path());
  const auto inserted = directory_states_.find(directory_native);
  // MatcherForParentDirectory returns the root matcher without caching when the
  // directory is the root or has no relative path; both leave the chain empty, so
  // a null state (root matcher, no ancestors) is the right answer.
  return inserted != directory_states_.end() ? &inserted->second : nullptr;
}

std::shared_ptr<const IgnoreMatcher> ProjectTraversalFilter::MatcherForParentDirectory(
    const std::filesystem::path& directory) {
  // Terminate at the project root (normal case) or, defensively, at the filesystem root
  // ("/", which has no relative path) so a root/path mismatch can never recurse forever.
  if (directory.empty() || directory == root_ || !directory.has_relative_path()) {
    return root_matcher_;
  }

  // Keyed on the NATIVE text, which is what the per-entry fast path above looks
  // up: a generic key would silently never match it on Windows, turning every
  // cache hit into a full ancestor recursion.
  const std::string& key = directory.native();
  const auto existing = directory_states_.find(key);
  if (existing != directory_states_.end()) {
    return existing->second.matcher;
  }

  // `directory` is normalized by every caller, and each parent_path() of a
  // normalized path is normalized too — re-normalizing per ancestor is ~12
  // allocations for a no-op (TD-2026-08-10-174).
  const std::shared_ptr<const IgnoreMatcher> parent_matcher =
      MatcherForParentDirectory(directory.parent_path());
  // Inherit the ancestor chain as a shared layer and add only this directory's
  // own .gitignore rules — no copy of the inherited rule set (TD-2026-07-17A-055),
  // and no layer at all when the directory has no rules to add.
  std::shared_ptr<const IgnoreMatcher> matcher =
      IgnoreMatcher::ForDirectory(parent_matcher, directory);
  return directory_states_.emplace(key, DirectoryState{.matcher = std::move(matcher)})
      .first->second.matcher;
}

}  // namespace microide::project
