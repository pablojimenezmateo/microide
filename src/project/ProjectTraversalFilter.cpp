#include "project/ProjectTraversalFilter.h"

#include <string>
#include <utility>

#include "util/PathMatch.h"
#include "util/PerformanceCounters.h"

namespace microide::project {

ProjectTraversalFilter::ProjectTraversalFilter(std::filesystem::path root,
                                               std::vector<std::string> extra_excludes)
    : root_(std::move(root).lexically_normal()) {
  if (!root_.has_filename() && root_.has_parent_path()) {
    root_ = root_.parent_path();
  }
  root_native_ = root_.native();
  root_matcher_ = IgnoreMatcher::ForProjectRoot(root_, extra_excludes);
}

bool ProjectTraversalFilter::Includes(const std::filesystem::path& path, platform::PathType type) {
  if (util::PathTextNeedsNormalizing(path.native())) {
    return IncludesNormalized(path.lexically_normal().native(), type);
  }
  return IncludesNormalized(path.native(), type);
}

bool ProjectTraversalFilter::Includes(std::string_view path_native, platform::PathType type) {
  if (util::PathTextNeedsNormalizing(path_native)) {
    return IncludesNormalized(std::filesystem::path(path_native).lexically_normal().native(), type);
  }
  return IncludesNormalized(path_native, type);
}

bool ProjectTraversalFilter::IncludesNormalized(std::string_view normalized_text,
                                                platform::PathType type) {
  if (root_.empty()) {
    return true;
  }
  util::AddPerformanceCounter(util::PerfCounterId::ProjectIgnoreFilterQueries);
  if (normalized_text == root_native_) {
    return true;
  }
  if (!util::NormalizedPathTextEqualsOrWithin(normalized_text, root_native_)) {
    return false;
  }

  const bool is_directory = type == platform::PathType::Directory;

  std::string_view relative = util::NormalizedRelativeView(normalized_text, root_native_);
  if (relative.empty()) {
    return true;
  }

  DirectoryState* const state =
      StateForParentDirectoryText(util::NormalizedParentDirectoryView(normalized_text));
  const IgnoreMatcher& matcher = state != nullptr ? *state->matcher : *root_matcher_;

  if (state != nullptr) {
    if (state->ancestors_ignored == kUnknown) {
      util::AddPerformanceCounter(util::PerfCounterId::ProjectIgnoreFilterAncestorScans);
      state->ancestors_ignored = 0;
      for (std::string_view parent = util::NormalizedParentDirectoryView(relative);
           !parent.empty() && parent != ".";) {
        if (matcher.IgnoredEntryNormalized(parent, true)) {
          state->ancestors_ignored = 1;
          break;
        }
        const std::string_view next = util::NormalizedParentDirectoryView(parent);
        if (next.size() >= parent.size()) {
          break;
        }
        parent = next;
      }
    }
    if (state->ancestors_ignored == 1) {
      return false;
    }
  }

  return !matcher.IgnoredEntryNormalized(relative, is_directory);
}

ProjectTraversalFilter::DirectoryState* ProjectTraversalFilter::StateForParentDirectoryText(
    std::string_view directory_native) {
  if (directory_native.empty() || directory_native == root_native_) {
    return nullptr;
  }
  const auto existing = directory_states_.find(directory_native);
  if (existing != directory_states_.end()) {
    return &existing->second;
  }
  MatcherForParentDirectory(std::filesystem::path(directory_native));
  const auto inserted = directory_states_.find(directory_native);
  return inserted != directory_states_.end() ? &inserted->second : nullptr;
}

std::shared_ptr<const IgnoreMatcher> ProjectTraversalFilter::MatcherForParentDirectory(
    const std::filesystem::path& directory) {
  if (directory.empty() || directory == root_ || !directory.has_relative_path()) {
    return root_matcher_;
  }

  const std::string& key = directory.native();
  const auto existing = directory_states_.find(key);
  if (existing != directory_states_.end()) {
    return existing->second.matcher;
  }

  const std::shared_ptr<const IgnoreMatcher> parent_matcher =
      MatcherForParentDirectory(directory.parent_path());
  std::shared_ptr<const IgnoreMatcher> matcher =
      IgnoreMatcher::ForDirectory(parent_matcher, directory);
  return directory_states_.emplace(key, DirectoryState{.matcher = std::move(matcher)})
      .first->second.matcher;
}

}
