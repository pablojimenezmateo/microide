#include "project/IgnoreMatcher.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <string_view>
#include <system_error>

#include "project/GlobMatch.h"
#include "util/PerformanceCounters.h"
#include "util/StringUtil.h"

namespace microide::project {

namespace {

std::string ToSlash(const std::filesystem::path& path) {
  return path.lexically_normal().generic_string();
}

bool IsGlobMetacharacter(char c) {
  return c == '*' || c == '?' || c == '[' || c == '\\';
}

std::size_t LiteralPrefixLength(std::string_view pattern) {
  std::size_t index = 0;
  while (index < pattern.size() && !IsGlobMetacharacter(pattern[index])) {
    ++index;
  }
  return index;
}

}

std::shared_ptr<IgnoreMatcher> IgnoreMatcher::MakeChild(
    const std::shared_ptr<const IgnoreMatcher>& parent) {
  auto child = std::make_shared<IgnoreMatcher>();
  child->parent_ = parent;
  if (parent != nullptr) {
    child->root_ = parent->root_;
  }
  return child;
}

std::shared_ptr<const IgnoreMatcher> IgnoreMatcher::ForDirectory(
    const std::shared_ptr<const IgnoreMatcher>& parent, const std::filesystem::path& directory) {
  std::shared_ptr<IgnoreMatcher> child = MakeChild(parent);
  child->LoadIgnoreFile(directory / ".gitignore");
  if (child->rules_.empty()) {
    return parent;
  }
  return child;
}

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

std::shared_ptr<IgnoreMatcher> IgnoreMatcher::ForProjectRoot(
    const std::filesystem::path& root, const std::vector<std::string>& exclude_globs) {
  auto matcher = std::make_shared<IgnoreMatcher>();
  matcher->SetRoot(root);
  matcher->AddDefaultRules();
  matcher->AddExcludeGlobs(exclude_globs);
  return matcher;
}

void IgnoreMatcher::AddDefaultRules() {
  static constexpr std::string_view kDefaultDirRules[] = {
      ".git/", ".svn/", ".hg/", ".bzr/",
      "node_modules/", ".cache/", ".venv/", "__pycache__/",
      "build/", "builds/", "out/", "dist/", "target/", "cmake-build-*/", ".vs/",
      "bin/", "obj/",
  };
  for (const std::string_view rule_text : kDefaultDirRules) {
    Rule rule;
    if (ParseRule(std::string(), std::string(rule_text), rule)) {
      rules_.push_back(std::move(rule));
    }
  }
}

void IgnoreMatcher::AddExcludeGlobs(const std::vector<std::string>& globs) {
  for (const std::string& glob : globs) {
    Rule rule;
    if (ParseRule(std::string(), glob, rule)) {
      rules_.push_back(std::move(rule));
    }
  }
}

void IgnoreMatcher::LoadIgnoreFile(const std::filesystem::path& path) {
  if (root_.empty()) {
    return;
  }

  std::error_code status_error;
  if (!std::filesystem::is_regular_file(std::filesystem::status(path, status_error))) {
    return;
  }

  constexpr std::uintmax_t kMaxIgnoreFileBytes = 4ull * 1024 * 1024;
  std::error_code size_error;
  const std::uintmax_t file_bytes = std::filesystem::file_size(path, size_error);
  if (!size_error && file_bytes > kMaxIgnoreFileBytes) {
    return;
  }

  std::ifstream input(path);
  if (!input) {
    return;
  }

  const auto base_relative = path.parent_path().lexically_relative(root_);
  if (base_relative.empty() && path.parent_path().lexically_normal() != root_) {
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
  return IgnoredNormalized(ToSlash(relative_path), is_directory);
}

namespace {

std::string_view LastComponentOf(std::string_view path) {
  while (!path.empty() && path.back() == '/') {
    path.remove_suffix(1);
  }
  const std::size_t slash = path.rfind('/');
  return slash == std::string_view::npos ? path : path.substr(slash + 1);
}

}

bool IgnoreMatcher::IgnoredNormalized(std::string_view normalized_relative_path,
                                      bool is_directory) const {
  std::size_t component_start = 0;
  for (std::size_t i = 0; i < normalized_relative_path.size(); ++i) {
    if (normalized_relative_path[i] != '/') {
      continue;
    }
    if (i > component_start &&
        IgnoredEntry(normalized_relative_path.substr(0, i),
                     normalized_relative_path.substr(component_start, i - component_start),
                     true)) {
      return true;
    }
    component_start = i + 1;
  }
  return IgnoredEntryNormalized(normalized_relative_path, is_directory);
}

bool IgnoreMatcher::IgnoredEntryNormalized(std::string_view normalized_relative_path,
                                           bool is_directory) const {
  return IgnoredEntry(normalized_relative_path, LastComponentOf(normalized_relative_path),
                      is_directory);
}

bool IgnoreMatcher::IgnoredEntry(std::string_view normalized_relative_path,
                                 std::string_view last_component, bool is_directory) const {
  util::AddPerformanceCounter(util::PerfCounterId::ProjectIgnoreFilterRuleSetEvaluations);
  bool ignored = parent_ != nullptr
                     ? parent_->IgnoredEntry(normalized_relative_path, last_component, is_directory)
                     : false;
  for (const auto& rule : rules_) {
    if (!rule.Matches(normalized_relative_path, last_component, is_directory)) {
      continue;
    }
    ignored = !rule.negated;
  }
  return ignored;
}

bool IgnoreMatcher::Rule::MatchesText(std::string_view text) const {
  switch (shape) {
    case PatternShape::Exact:
      return text == literal;
    case PatternShape::Suffix:
      return text.ends_with(literal);
    case PatternShape::Prefix:
      return text.starts_with(literal);
    case PatternShape::Glob:
      return GlobMatchesAfterLiteral(pattern, literal, text);
  }
  return false;
}

bool IgnoreMatcher::GlobMatchesAfterLiteral(std::string_view pattern, std::string_view literal,
                                            std::string_view text) {
  if (!text.starts_with(literal)) {
    return false;
  }
  return GlobMatches(pattern.substr(literal.size()), text.substr(literal.size()));
}

bool IgnoreMatcher::Rule::Matches(std::string_view relative_path,
                                  std::string_view last_component, bool is_directory) const {
  if (!base_prefix.empty()) {
    if (relative_path == base_relative) {
      return false;
    }
    if (!relative_path.starts_with(base_prefix)) {
      return false;
    }
    relative_path.remove_prefix(base_prefix.size());
  }

  if (relative_path.empty()) {
    return false;
  }

  if (directory_only && !is_directory) {
    return false;
  }

  if (match_basename) {
    return MatchesText(last_component);
  }

  if (MatchesText(relative_path)) {
    return true;
  }
  if (is_directory && !directory_only && !negated && shape == PatternShape::Glob &&
      pattern.size() > 3 && pattern.ends_with("/**")) {
    const std::string_view stem = std::string_view(pattern).substr(0, pattern.size() - 3);
    return GlobMatchesAfterLiteral(stem, std::string_view(literal).substr(0, std::min(literal.size(), stem.size())),
                                   relative_path);
  }
  return false;
}

bool IgnoreMatcher::ParseRule(std::string base_relative, std::string line, Rule& out_rule) {
  while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
    line.pop_back();
  }
  while (!line.empty() && (line.back() == ' ' || line.back() == '\t')) {
    std::size_t backslashes = 0;
    std::size_t index = line.size() - 1;
    while (index > 0 && line[index - 1] == '\\') {
      ++backslashes;
      --index;
    }
    if (backslashes % 2 == 1) {
      break;
    }
    line.pop_back();
  }
  if (line.empty() || line.front() == '#') {
    return false;
  }

  bool negated = false;
  if (line.front() == '!') {
    negated = true;
    line.erase(line.begin());
  } else if (line.size() >= 2 && line.front() == '\\' &&
             (line[1] == '#' || line[1] == '!')) {
    line.erase(line.begin());
  }

  bool directory_only = line.ends_with('/');
  if (directory_only) {
    line.pop_back();
  }
  bool anchored = !line.empty() && line.front() == '/';
  if (anchored) {
    line.erase(line.begin());
  }
  if (line.find('/') != std::string::npos) {
    anchored = true;
  }

  const std::string pattern = ToSlash(std::filesystem::path(line));
  if (pattern.empty() || pattern == ".") {
    return false;
  }

  std::string base = ToSlash(std::filesystem::path(std::move(base_relative)));
  std::string base_prefix;
  if (!base.empty() && base != ".") {
    base_prefix = base + "/";
  }

  const std::size_t literal_length = LiteralPrefixLength(pattern);
  PatternShape shape = PatternShape::Glob;
  std::string literal;
  if (literal_length == pattern.size()) {
    shape = PatternShape::Exact;
    literal = pattern;
  } else if (!anchored && pattern[0] == '*' && LiteralPrefixLength(pattern.substr(1)) ==
                                                   pattern.size() - 1) {
    shape = PatternShape::Suffix;
    literal = pattern.substr(1);
  } else if (!anchored && pattern.back() == '*' && literal_length == pattern.size() - 1) {
    shape = PatternShape::Prefix;
    literal = pattern.substr(0, literal_length);
  } else {
    literal = pattern.substr(0, literal_length);
  }

  out_rule = Rule{
      .base_relative = std::move(base),
      .base_prefix = std::move(base_prefix),
      .pattern = pattern,
      .literal = std::move(literal),
      .shape = shape,
      .negated = negated,
      .directory_only = directory_only,
      .match_basename = !anchored,
  };
  return true;
}

}
