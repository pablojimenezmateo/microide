#include "project/IgnoreMatcher.h"

#include <fnmatch.h>

#include <cctype>
#include <fstream>
#include <system_error>

namespace microide::project {

namespace {

std::string Trim(std::string value) {
  while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) {
    value.erase(value.begin());
  }
  while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) {
    value.pop_back();
  }
  return value;
}

std::string ToSlash(const std::filesystem::path& path) {
  return path.lexically_normal().generic_string();
}

bool GlobMatches(std::string_view pattern, std::string_view text) {
  return fnmatch(std::string(pattern).c_str(), std::string(text).c_str(), FNM_PATHNAME) == 0;
}

}  // namespace

bool IgnoreMatcher::SetRoot(const std::filesystem::path& root) {
  std::error_code error;
  const auto absolute_root = std::filesystem::absolute(root, error);
  if (error || absolute_root.empty()) {
    return false;
  }

  root_ = absolute_root.lexically_normal();
  rules_.clear();
  LoadIgnoreFile(root_ / ".gitignore");
  return true;
}

void IgnoreMatcher::LoadIgnoreFile(const std::filesystem::path& path) {
  if (root_.empty()) {
    return;
  }

  std::ifstream input(path);
  if (!input) {
    return;
  }

  std::error_code error;
  const auto base_relative = std::filesystem::relative(path.parent_path(), root_, error);
  if (error) {
    return;
  }

  const std::string base = base_relative == "." ? "" : ToSlash(base_relative);
  std::string line;
  while (std::getline(input, line)) {
    Rule rule;
    if (ParseRule(base, line, rule)) {
      rules_.push_back(std::move(rule));
    }
  }
}

bool IgnoreMatcher::Ignored(const std::filesystem::path& relative_path, bool is_directory) const {
  const std::string rel = ToSlash(relative_path);
  bool ignored = false;
  for (const auto& rule : rules_) {
    if (!rule.Matches(rel, is_directory)) {
      continue;
    }
    ignored = !rule.negated;
  }
  return ignored;
}

bool IgnoreMatcher::Rule::Matches(std::string relative_path, bool is_directory) const {
  if (directory_only && !is_directory) {
    return false;
  }

  std::string base = base_relative;
  if (base == ".") {
    base.clear();
  }
  if (!base.empty()) {
    if (relative_path == base) {
      return false;
    }
    const std::string prefix = base + "/";
    if (!relative_path.starts_with(prefix)) {
      return false;
    }
    relative_path.erase(0, prefix.size());
  }

  if (relative_path.empty()) {
    return false;
  }

  if (match_basename) {
    std::size_t start = 0;
    while (start <= relative_path.size()) {
      const std::size_t end = relative_path.find('/', start);
      const std::string_view part = end == std::string::npos
                                        ? std::string_view(relative_path).substr(start)
                                        : std::string_view(relative_path).substr(start, end - start);
      if (GlobMatches(pattern, part)) {
        return true;
      }
      if (end == std::string::npos) {
        break;
      }
      start = end + 1;
    }
    return false;
  }

  if (GlobMatches(pattern, relative_path)) {
    return true;
  }

  std::size_t slash = relative_path.find('/');
  while (slash != std::string::npos) {
    const std::string_view suffix(relative_path.c_str() + slash + 1,
                                  relative_path.size() - slash - 1);
    if (GlobMatches(pattern, suffix)) {
      return true;
    }
    slash = relative_path.find('/', slash + 1);
  }

  return false;
}

bool IgnoreMatcher::ParseRule(std::string base_relative, std::string line, Rule& out_rule) {
  line = Trim(std::move(line));
  if (line.empty() || line.starts_with('#')) {
    return false;
  }

  bool negated = false;
  if (line.starts_with('!')) {
    negated = true;
    line.erase(line.begin());
  }

  bool directory_only = line.ends_with('/');
  if (directory_only) {
    line.pop_back();
  }
  if (!line.empty() && line.front() == '/') {
    line.erase(line.begin());
  }

  const std::string pattern = ToSlash(std::filesystem::path(line));
  if (pattern.empty() || pattern == ".") {
    return false;
  }

  out_rule = Rule{
      .base_relative = ToSlash(std::filesystem::path(std::move(base_relative))),
      .pattern = pattern,
      .negated = negated,
      .directory_only = directory_only,
      .match_basename = pattern.find('/') == std::string::npos,
  };
  return true;
}

}  // namespace microide::project
