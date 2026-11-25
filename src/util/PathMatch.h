#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

namespace microide::util {

[[nodiscard]] inline bool SamePathNormalized(const std::filesystem::path& a,
                                             const std::filesystem::path& b) {
  return a == b || a.lexically_normal() == b.lexically_normal();
}

[[nodiscard]] inline bool RelativeTextClimbsOut(std::string_view relative) {
  return relative == ".." || relative.starts_with("../");
}
[[nodiscard]] inline bool RelativePathClimbsOut(const std::filesystem::path& relative) {
  return !relative.empty() && *relative.begin() == "..";
}

[[nodiscard]] inline bool IsPortableRelativePath(std::string_view path, std::size_t max_bytes) {
  if (path.empty() || path.size() > max_bytes || path.front() == '/' || path.find('\0') != std::string_view::npos) {
    return false;
  }
  while (!path.empty()) {
    const std::size_t slash = path.find('/');
    const std::string_view component = path.substr(0, slash);
    if (component.empty() || component == "." || component == "..") {
      return false;
    }
    path.remove_prefix(slash == std::string_view::npos ? path.size() : slash + 1);
    if (slash != std::string_view::npos && path.empty()) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] inline bool NormalizedPathTextEqualsOrWithin(std::string_view candidate_text,
                                                           std::string_view root_text) {
  if (candidate_text.empty() || root_text.empty()) {
    return false;
  }
  const auto is_separator = [](char c) {
    return c == '/';
  };
  const auto drop_trailing_separator = [&](std::string_view& text) {
    if (text.size() > 1 && is_separator(text.back())) {
      text.remove_suffix(1);
    }
  };
  drop_trailing_separator(candidate_text);
  drop_trailing_separator(root_text);
  if (candidate_text == root_text) {
    return true;
  }
  if (root_text.size() == 1 && root_text[0] == '.') {
    if (is_separator(candidate_text.front())) {
      return false;
    }
    const bool climbs_out = candidate_text.size() >= 2 && candidate_text[0] == '.' &&
                            candidate_text[1] == '.' &&
                            (candidate_text.size() == 2 || is_separator(candidate_text[2]));
    return !climbs_out;
  }
  if (candidate_text.size() <= root_text.size() ||
      candidate_text.compare(0, root_text.size(), root_text) != 0) {
    return false;
  }
  return is_separator(root_text.back()) || is_separator(candidate_text[root_text.size()]);
}

[[nodiscard]] inline bool NormalizedPathEqualsOrWithin(const std::filesystem::path& candidate,
                                                       const std::filesystem::path& root) {
  return NormalizedPathTextEqualsOrWithin(candidate.native(), root.native());
}

[[nodiscard]] inline bool SameAsNormalizedPath(const std::filesystem::path& candidate,
                                               const std::filesystem::path& normalized_reference);

[[nodiscard]] inline std::string_view NormalizedRelativeView(std::string_view candidate_text,
                                                             std::string_view root_text) {
  const auto is_separator = [](char c) {
    return c == '/';
  };
  if (root_text.empty() || candidate_text == root_text) {
    return {};
  }
  if (root_text.size() == 1 && root_text[0] == '.') {
    return candidate_text;
  }
  std::size_t offset = root_text.size();
  if (!is_separator(root_text.back())) {
    ++offset;
  }
  if (offset >= candidate_text.size()) {
    return {};
  }
  return candidate_text.substr(offset);
}

[[nodiscard]] inline std::string_view NormalizedParentDirectoryView(std::string_view text) {
  const std::size_t last_separator = text.find_last_of('/');
  if (last_separator == std::string_view::npos) {
    return {};
  }
  if (last_separator == 0) {
    return text.substr(0, 1);
  }
  return text.substr(0, last_separator);
}

[[nodiscard]] inline bool PathTextNeedsNormalizing(std::string_view text) {
  const auto is_separator = [](char c) {
    return c == '/';
  };
  if (text.empty()) {
    return false;
  }
  std::size_t begin = 0;
  while (true) {
    std::size_t end = begin;
    while (end < text.size() && !is_separator(text[end])) {
      ++end;
    }
    const std::string_view component = text.substr(begin, end - begin);
    if (component.empty() && begin != 0 && end != text.size()) {
      return true;
    }
    if (component == "." || component == "..") {
      return true;
    }
    if (end >= text.size()) {
      return false;
    }
    begin = end + 1;
  }
}

[[nodiscard]] inline std::filesystem::path NormalizedPath(const std::filesystem::path& path) {
  return PathTextNeedsNormalizing(path.native()) ? path.lexically_normal() : path;
}

[[nodiscard]] inline const std::filesystem::path& NormalizedPathView(
    const std::filesystem::path& path,
    std::filesystem::path& scratch) {
  if (!PathTextNeedsNormalizing(path.native())) {
    return path;
  }
  scratch = path.lexically_normal();
  return scratch;
}

inline bool SameAsNormalizedPath(const std::filesystem::path& candidate,
                                 const std::filesystem::path& normalized_reference) {
  if (candidate.native() == normalized_reference.native()) {
    return true;
  }
  if (!PathTextNeedsNormalizing(candidate.native())) {
    return false;
  }
  return candidate.lexically_normal().native() == normalized_reference.native();
}

[[nodiscard]] inline bool PathEqualsOrWithinNormalized(const std::filesystem::path& candidate,
                                                       const std::filesystem::path& root);

[[nodiscard]] inline bool PathEqualsOrWithinResolvingSymlinks(
    const std::filesystem::path& candidate,
    const std::filesystem::path& root) {
  if (candidate.empty() || root.empty()) {
    return false;
  }
  std::error_code candidate_error;
  const std::filesystem::path canonical_candidate =
      std::filesystem::weakly_canonical(candidate, candidate_error);
  if (candidate_error) {
    return false;
  }
  std::error_code root_error;
  const std::filesystem::path canonical_root = std::filesystem::weakly_canonical(root, root_error);
  if (root_error) {
    return false;
  }
  return PathEqualsOrWithinNormalized(canonical_candidate.lexically_normal(),
                                      canonical_root.lexically_normal());
}

[[nodiscard]] inline bool PathEqualsOrWithin(const std::filesystem::path& candidate,
                                             const std::filesystem::path& root) {
  if (candidate.empty() || root.empty()) {
    return false;
  }
  if (candidate == root) {
    return true;
  }
  std::filesystem::path normalized_candidate = candidate.lexically_normal();
  std::filesystem::path normalized_root = root.lexically_normal();
  if (normalized_candidate.empty() || normalized_root.empty()) {
    return false;
  }
  for (std::filesystem::path* path : {&normalized_candidate, &normalized_root}) {
    if (path->has_relative_path() && !path->has_filename()) {
      *path = path->parent_path();
    }
  }
  if (normalized_candidate == normalized_root) {
    return true;
  }
  const std::filesystem::path relative = normalized_candidate.lexically_relative(normalized_root);
  if (relative.empty()) {
    return false;
  }
  const std::string relative_text = relative.generic_string();
  return relative_text != "." && relative_text != ".." && relative_text.rfind("../", 0) != 0;
}

[[nodiscard]] inline std::optional<std::string> RelativePathWithin(
    const std::filesystem::path& candidate, const std::filesystem::path& root) {
  if (candidate.empty() || root.empty()) {
    return std::nullopt;
  }
  const std::filesystem::path relative =
      candidate.lexically_normal().lexically_relative(root.lexically_normal());
  if (relative.empty()) {
    return std::nullopt;
  }
  std::string text = relative.generic_string();
  if (text == "." || text == ".." || text.rfind("../", 0) == 0) {
    return std::nullopt;
  }
  return text;
}

[[nodiscard]] inline std::filesystem::path ReplacePathPrefix(
    const std::filesystem::path& path,
    const std::filesystem::path& old_prefix,
    const std::filesystem::path& new_prefix) {
  const std::filesystem::path normalized_path = path.lexically_normal();
  const std::filesystem::path normalized_old_prefix = old_prefix.lexically_normal();
  const std::filesystem::path normalized_new_prefix = new_prefix.lexically_normal();
  if (!NormalizedPathEqualsOrWithin(normalized_path, normalized_old_prefix)) {
    return normalized_path;
  }
  if (normalized_path == normalized_old_prefix) {
    return normalized_new_prefix;
  }
  const std::filesystem::path relative =
      normalized_path.lexically_relative(normalized_old_prefix);
  if (relative.empty()) {
    return normalized_path;
  }
  return (normalized_new_prefix / relative).lexically_normal();
}

inline bool PathEqualsOrWithinNormalized(const std::filesystem::path& candidate,
                                         const std::filesystem::path& root) {
  std::filesystem::path scratch;
  return PathEqualsOrWithin(NormalizedPathView(candidate, scratch), root);
}

}
