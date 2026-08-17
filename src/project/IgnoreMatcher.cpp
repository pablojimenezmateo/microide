#include "project/IgnoreMatcher.h"

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

// The characters GlobMatches gives meaning to. '\\' is included because it escapes
// the next character, so a pattern containing one is never a plain literal.
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

}  // namespace

std::shared_ptr<IgnoreMatcher> IgnoreMatcher::MakeChild(
    const std::shared_ptr<const IgnoreMatcher>& parent) {
  auto child = std::make_shared<IgnoreMatcher>();
  child->parent_ = parent;
  if (parent != nullptr) {
    // Share the parent's root so LoadIgnoreFile computes the same base-relative
    // prefixes it would in a copied matcher.
    child->root_ = parent->root_;
  }
  return child;
}

std::shared_ptr<const IgnoreMatcher> IgnoreMatcher::ForDirectory(
    const std::shared_ptr<const IgnoreMatcher>& parent, const std::filesystem::path& directory) {
  std::shared_ptr<IgnoreMatcher> child = MakeChild(parent);
  child->LoadIgnoreFile(directory / ".gitignore");
  if (child->rules_.empty()) {
    // No rules of its own, so the child layer would only add a pointer hop to
    // every query made beneath this directory. Its verdicts are the parent's.
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

void IgnoreMatcher::AddDefaultRules() {
  // Directory-only, basename-matched (no '/') rules, so each name is pruned at any
  // depth exactly as the equivalent ".name/" line in a .gitignore would be.
  static constexpr std::string_view kDefaultDirRules[] = {
      // Version-control metadata.
      ".git/", ".svn/", ".hg/", ".bzr/",
      // Dependency / cache / virtualenv trees.
      "node_modules/", ".cache/", ".venv/", "__pycache__/",
      // Common build-output trees. Grayed + unindexed by default (never hidden).
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

  // Skip an absurdly large ignore file rather than allocate/parse it: a real
  // .gitignore is KB-scale, so a multi-MB one (or a single multi-GB line with no
  // newline, which getline would buffer whole) is hostile/degenerate. 4 MiB is
  // far above any legitimate ignore file.
  // Reject a non-regular ignore file (FIFO/device/socket) before opening: a project
  // can contain a special file named `.gitignore`, and opening/getline on it could
  // block the tree/scanner/traversal/watcher thread building ignore matchers
  // (TD-2026-07-17A-112). status() stats without opening; a stat failure also fails
  // closed here. A symlink to a real file is still allowed (status follows it).
  std::error_code status_error;
  if (!std::filesystem::is_regular_file(std::filesystem::status(path, status_error))) {
    return;
  }

  // Skip an absurdly large ignore file rather than allocate/parse it: a real
  // .gitignore is KB-scale, so a multi-MB one (or a single multi-GB line with no
  // newline, which getline would buffer whole) is hostile/degenerate. 4 MiB is
  // far above any legitimate ignore file.
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

bool IgnoreMatcher::IgnoredNormalized(std::string_view normalized_relative_path,
                                      bool is_directory) const {
  // Same traversal the per-rule basename loop used to run, so a trailing '/' still
  // yields no empty final component and "a//b" still yields the empty middle one.
  PathComponents components;
  for (std::size_t start = 0; start < normalized_relative_path.size();) {
    const std::size_t end = normalized_relative_path.find('/', start);
    if (end == std::string_view::npos) {
      components.push_back(normalized_relative_path.substr(start));
      break;
    }
    components.push_back(normalized_relative_path.substr(start, end - start));
    start = end + 1;
  }
  return IgnoredWithComponents(normalized_relative_path, components, is_directory);
}

bool IgnoreMatcher::IgnoredWithComponents(std::string_view normalized_relative_path,
                                          const PathComponents& components,
                                          bool is_directory) const {
  // Evaluate the inherited ancestor layers first, then this directory's own
  // rules on top. Because a child's rules apply strictly after its ancestors'
  // (gitignore last-match-wins), this parent-then-local recursion is exactly
  // equivalent to a single flattened ancestor-first rule list — with no rule
  // copied into the child (TD-2026-07-17A-055).
  // One bump per LAYER, so a matcher with a nested .gitignore chain reports the
  // rule sets it actually ran rather than the query it ran them for.
  util::AddPerformanceCounter(util::PerfCounterId::ProjectIgnoreFilterRuleSetEvaluations);
  bool ignored =
      parent_ != nullptr
          ? parent_->IgnoredWithComponents(normalized_relative_path, components, is_directory)
          : false;
  for (const auto& rule : rules_) {
    if (!rule.Matches(normalized_relative_path, components, is_directory)) {
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
      // Everything before the pattern's first metacharacter is literal, so a text
      // that does not start with it cannot match however the rest expands. This is
      // a pure short-circuit of GlobMatches, not a second matching rule.
      if (!literal.empty() && !text.starts_with(literal)) {
        return false;
      }
      return GlobMatches(pattern, text);
  }
  return false;
}

bool IgnoreMatcher::Rule::Matches(std::string_view relative_path,
                                  const PathComponents& components, bool is_directory) const {
  if (directory_only && !is_directory) {
    return false;
  }

  // base_prefix is empty when base_relative is empty or ".", i.e. the rule applies
  // from the root and needs no path stripping.
  std::size_t first_component = 0;
  if (!base_prefix.empty()) {
    if (relative_path == base_relative) {
      return false;
    }
    if (!relative_path.starts_with(base_prefix)) {
      return false;
    }
    relative_path.remove_prefix(base_prefix.size());
    // The prefix ends on a separator, so the stripped remainder is exactly the
    // component list minus its first `base_component_count` entries.
    first_component = base_component_count;
  }

  if (relative_path.empty()) {
    return false;
  }

  if (match_basename) {
    for (std::size_t index = first_component; index < components.size(); ++index) {
      if (MatchesText(components[index])) {
        return true;
      }
    }
    return false;
  }

  // Anchored (slash-bearing) patterns are pinned to the .gitignore's directory:
  // match against the whole base-relative path only. Any "match at any depth"
  // semantics a pattern wants must be spelled with an explicit '**' (which
  // GlobMatches folds, including zero directories). Do NOT float the pattern
  // across path suffixes — git does not, and doing so over-excludes files
  // (e.g. `a/b` must not match `x/a/b`).
  return MatchesText(relative_path);
}

bool IgnoreMatcher::ParseRule(std::string base_relative, std::string line, Rule& out_rule) {
  // gitignore(5): only TRAILING whitespace is stripped; leading whitespace is
  // significant (a pattern like "  build.log" matches a name that begins with
  // spaces). Trimming both ends over-normalized leading-space patterns.
  //
  // CR/LF are line terminators (getline may leave a CR on CRLF input) and are always
  // removed.
  while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
    line.pop_back();
  }
  // Trailing spaces/tabs are ignored UNLESS the whitespace is backslash-escaped. A
  // trailing whitespace char is escaped when preceded by an ODD run of backslashes;
  // an escaped trailing space is a literal part of the pattern (e.g. "foo\ " matches
  // the name "foo "). Keep the escaping backslash in place — GlobMatches resolves
  // "\ " to a literal space — and stop trimming at the first escaped whitespace.
  while (!line.empty() && (line.back() == ' ' || line.back() == '\t')) {
    std::size_t backslashes = 0;
    std::size_t index = line.size() - 1;  // the trailing whitespace char
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
    // gitignore(5): "\#"/"\!" escape a literal leading '#'/'!' so the line is a
    // pattern, not a comment or negation. Drop the single escaping backslash; '#'
    // and '!' are not glob-special, so the remainder matches them literally.
    line.erase(line.begin());
  }

  bool directory_only = line.ends_with('/');
  if (directory_only) {
    line.pop_back();
  }
  // A separator at the beginning or middle of the pattern anchors it to the
  // .gitignore's directory (gitignore(5)); only a slash-free pattern floats and
  // matches by basename at any depth. Detect a mid-slash before normalization,
  // then strip a leading '/' (it only signalled anchoring).
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
  std::size_t base_component_count = 0;
  if (!base.empty() && base != ".") {
    base_prefix = base + "/";
    base_component_count = 1;
    for (const char c : base) {
      if (c == '/') {
        ++base_component_count;
      }
    }
  }

  // Classify the pattern's shape once, here, so the per-entry test below is a
  // compare for the overwhelmingly common forms. Suffix/Prefix are restricted to
  // basename rules on purpose: their candidate text is a single path component and
  // so contains no '/', which is what makes "*x" equivalent to ends_with("x") — a
  // '*' in an anchored pattern must not cross a separator and has no such
  // equivalence.
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
      .base_component_count = base_component_count,
      .shape = shape,
      .negated = negated,
      .directory_only = directory_only,
      .match_basename = !anchored,
  };
  return true;
}

}  // namespace microide::project
