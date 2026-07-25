#include "project/ProjectTraversalFilter.h"

#include <string>
#include <utility>

#include "util/PathMatch.h"
#include "util/StringUtil.h"

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
  auto root_matcher = std::make_shared<IgnoreMatcher>();
  root_matcher->SetRoot(root_);
  // Defaults after the root .gitignore so they take precedence; user excludes last
  // so an explicit "!name/" re-include wins over both.
  root_matcher->AddDefaultRules();
  root_matcher->AddExcludeGlobs(extra_excludes);
  root_matcher_ = std::move(root_matcher);
}

bool ProjectTraversalFilter::Includes(const std::filesystem::path& path, platform::PathType type) {
  const std::filesystem::path normalized_path = path.lexically_normal();
  if (root_.empty() || normalized_path == root_) {
    return true;
  }
  // A path that is not nested under the root escapes the project boundary
  // (lexically_relative would yield a "../…" relative that RelativeToRoot used to
  // accept). Reject it outright so a watcher/scanner/helper event for an
  // out-of-root file cannot pass the filter and feed out-of-root indexing.
  // Both sides are already normalized (normalized_path just above, root_ in the
  // constructor), so use the allocation-free variant: this runs once per
  // filesystem entry of every index walk.
  if (!util::NormalizedPathEqualsOrWithin(normalized_path, root_)) {
    return false;
  }

  const bool is_directory = type == platform::PathType::Directory;

  const std::filesystem::path relative = RelativeToRoot(normalized_path);
  if (relative.empty()) {
    return true;
  }

  const std::shared_ptr<const IgnoreMatcher> matcher =
      MatcherForParentDirectory(normalized_path.parent_path().lexically_normal());
  // `relative` is already lexically-normalized, so the string_view overload skips
  // the per-call re-normalization the path overload would otherwise perform.
  if (matcher->IgnoredNormalized(relative.generic_string(), is_directory)) {
    return false;
  }
  // Each parent_path() of a normalized path is also normalized; no need to
  // re-normalize per ancestor. (Ignored directories are also pruned via
  // disable_recursion_pending in the walk, so this loop is defense-in-depth.)
  static const std::filesystem::path kDot(".");
  for (std::filesystem::path parent = relative.parent_path();
       !parent.empty() && parent != kDot; parent = parent.parent_path()) {
    if (matcher->IgnoredNormalized(parent.generic_string(), true)) {
      return false;
    }
  }
  return true;
}

std::filesystem::path ProjectTraversalFilter::RelativeToRoot(
    const std::filesystem::path& path) const {
  const std::filesystem::path relative = path.lexically_relative(root_);
  if (!relative.empty()) {
    return relative.lexically_normal();
  }
#ifdef _WIN32
  const std::string path_text = path.generic_string();
  const std::string root_text = root_.generic_string();
  if (path_text.size() <= root_text.size()) {
    return {};
  }
  const std::string lower_path = util::ToLowerAscii(path_text);
  const std::string lower_root = util::ToLowerAscii(root_text);
  if (lower_path.rfind(lower_root, 0) != 0 || path_text[root_text.size()] != '/') {
    return {};
  }
  return std::filesystem::path(path_text.substr(root_text.size() + 1)).lexically_normal();
#else
  return {};
#endif
}

std::shared_ptr<const IgnoreMatcher> ProjectTraversalFilter::MatcherForParentDirectory(
    const std::filesystem::path& directory) {
  // Terminate at the project root (normal case) or, defensively, at the filesystem root
  // ("/", which has no relative path) so a root/path mismatch can never recurse forever.
  if (directory.empty() || directory == root_ || !directory.has_relative_path()) {
    return root_matcher_;
  }

  const std::string key = directory.generic_string();
  const auto existing = directory_matchers_.find(key);
  if (existing != directory_matchers_.end()) {
    return existing->second;
  }

  const std::shared_ptr<const IgnoreMatcher> parent_matcher =
      MatcherForParentDirectory(directory.parent_path().lexically_normal());
  // Inherit the ancestor chain as a shared layer and add only this directory's
  // own .gitignore rules — no copy of the inherited rule set (TD-2026-07-17A-055).
  std::shared_ptr<IgnoreMatcher> matcher = IgnoreMatcher::MakeChild(parent_matcher);
  matcher->LoadIgnoreFile(directory / ".gitignore");
  return directory_matchers_.emplace(key, std::move(matcher)).first->second;
}

}  // namespace microide::project
