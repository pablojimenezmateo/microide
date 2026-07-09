#include "project/IgnoreMatcher.h"

#include <cctype>
#include <fstream>
#include <string_view>
#include <system_error>

#include "util/StringUtil.h"

namespace microide::project {

namespace {

std::string ToSlash(const std::filesystem::path& path) {
  return path.lexically_normal().generic_string();
}

// Glob matcher honoring fnmatch's FNM_PATHNAME semantics: a single '*' and '?' do
// not cross '/' boundaries, '[...]' classes match a single non-separator char, and
// '\\' escapes the next pattern character. A segment-anchored '**' (git's "match
// across any number of directories" wildcard — at pattern start, at end, or bounded
// by '/' on both sides) does cross '/', and a '**/' prefix/segment may also match
// zero directories. Implemented in-tree because MinGW/UCRT does not ship
// <fnmatch.h>.
bool GlobMatches(std::string_view pattern, std::string_view text) {
  const std::size_t plen = pattern.size();
  const std::size_t tlen = text.size();
  std::size_t pi = 0;
  std::size_t ti = 0;
  std::size_t star_pi = std::string_view::npos;
  std::size_t star_ti = 0;
  bool star_cross_slash = false;  // the pending '*' backtrack may consume '/'

  while (ti < tlen) {
    if (pi < plen) {
      const char pc = pattern[pi];
      if (pc == '*') {
        // Consume the whole run of '*'. A run of two or more that forms a complete
        // path segment ('**') matches across '/'; anything else behaves as '*'.
        const std::size_t star_start = pi;
        std::size_t star_count = 0;
        while (pi < plen && pattern[pi] == '*') {
          ++pi;
          ++star_count;
        }
        const bool before_ok = star_start == 0 || pattern[star_start - 1] == '/';
        const bool after_ok = pi == plen || pattern[pi] == '/';
        const bool segment_doublestar = star_count >= 2 && before_ok && after_ok;
        if (segment_doublestar && pi < plen && pattern[pi] == '/') {
          // '**/' can match zero directories: fold the following '/' into the
          // wildcard so the pattern can resume with nothing consumed from text.
          ++pi;
        }
        star_pi = pi;
        star_ti = ti;
        star_cross_slash = segment_doublestar;
        continue;
      }
      if (pc == '?') {
        if (text[ti] == '/') {
          // fall through to backtrack
        } else {
          ++pi;
          ++ti;
          continue;
        }
      } else if (pc == '[') {
        const char tc = text[ti];
        if (tc == '/') {
          // fall through to backtrack
        } else {
          std::size_t scan = pi + 1;
          bool negate = false;
          if (scan < plen && (pattern[scan] == '!' || pattern[scan] == '^')) {
            negate = true;
            ++scan;
          }
          bool matched = false;
          bool closed = false;
          while (scan < plen) {
            if (pattern[scan] == ']' && scan != pi + 1 + (negate ? 1 : 0)) {
              closed = true;
              ++scan;
              break;
            }
            char lo = pattern[scan++];
            if (lo == '\\' && scan < plen) {
              lo = pattern[scan++];
            }
            char hi = lo;
            if (scan + 1 < plen && pattern[scan] == '-' && pattern[scan + 1] != ']') {
              ++scan;
              hi = pattern[scan++];
              if (hi == '\\' && scan < plen) {
                hi = pattern[scan++];
              }
            }
            if (tc >= lo && tc <= hi) {
              matched = true;
            }
          }
          if (closed && (matched != negate)) {
            pi = scan;
            ++ti;
            continue;
          }
        }
      } else {
        char lit = pc;
        std::size_t advance = 1;
        if (lit == '\\' && pi + 1 < plen) {
          lit = pattern[pi + 1];
          advance = 2;
        }
        if (text[ti] == lit) {
          pi += advance;
          ++ti;
          continue;
        }
      }
    }
    if (star_pi != std::string_view::npos && (star_cross_slash || text[star_ti] != '/')) {
      pi = star_pi;
      ti = ++star_ti;
      continue;
    }
    return false;
  }
  while (pi < plen && pattern[pi] == '*') {
    ++pi;
  }
  return pi == plen;
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
  bool ignored = false;
  for (const auto& rule : rules_) {
    if (!rule.Matches(normalized_relative_path, is_directory)) {
      continue;
    }
    ignored = !rule.negated;
  }
  return ignored;
}

bool IgnoreMatcher::Rule::Matches(std::string_view relative_path, bool is_directory) const {
  if (directory_only && !is_directory) {
    return false;
  }

  // base_prefix is empty when base_relative is empty or ".", i.e. the rule applies
  // from the root and needs no path stripping.
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

  if (match_basename) {
    std::size_t start = 0;
    while (start < relative_path.size()) {
      const std::size_t end = relative_path.find('/', start);
      const std::string_view part = end == std::string_view::npos
                                        ? relative_path.substr(start)
                                        : relative_path.substr(start, end - start);
      if (GlobMatches(pattern, part)) {
        return true;
      }
      if (end == std::string_view::npos) {
        break;
      }
      start = end + 1;
    }
    return false;
  }

  // Anchored (slash-bearing) patterns are pinned to the .gitignore's directory:
  // match against the whole base-relative path only. Any "match at any depth"
  // semantics a pattern wants must be spelled with an explicit '**' (which
  // GlobMatches folds, including zero directories). Do NOT float the pattern
  // across path suffixes — git does not, and doing so over-excludes files
  // (e.g. `a/b` must not match `x/a/b`).
  return GlobMatches(pattern, relative_path);
}

bool IgnoreMatcher::ParseRule(std::string base_relative, std::string line, Rule& out_rule) {
  // gitignore(5): only TRAILING whitespace is stripped; leading whitespace is
  // significant (a pattern like "  build.log" matches a name that begins with
  // spaces). Trimming both ends over-normalized leading-space patterns.
  while (!line.empty() && (line.back() == ' ' || line.back() == '\t' ||
                           line.back() == '\r' || line.back() == '\n')) {
    line.pop_back();
  }
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
  if (!base.empty() && base != ".") {
    base_prefix = base + "/";
  }

  out_rule = Rule{
      .base_relative = std::move(base),
      .base_prefix = std::move(base_prefix),
      .pattern = pattern,
      .negated = negated,
      .directory_only = directory_only,
      .match_basename = !anchored,
  };
  return true;
}

}  // namespace microide::project
