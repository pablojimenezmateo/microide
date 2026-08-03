#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace microide::project {

// Glob matcher honoring fnmatch's FNM_PATHNAME semantics: a single '*' and '?' do
// not cross '/' boundaries, '[...]' classes match a single non-separator char, and
// '\\' escapes the next pattern character. A segment-anchored '**' (git's "match
// across any number of directories" wildcard — at pattern start, at end, or bounded
// by '/' on both sides) does cross '/', and a '**/' prefix/segment may also match
// zero directories. Implemented in-tree because MinGW/UCRT does not ship
// <fnmatch.h>.
//
// Shared primitive: `IgnoreMatcher` evaluates gitignore rules with it and `GlobSet`
// evaluates search scope filters with it, so both surfaces agree on what a pattern
// means instead of drifting apart behind two private copies.
bool GlobMatches(std::string_view pattern, std::string_view text);

// Upper bound on how many patterns one entry may expand to. Brace alternation is
// multiplicative ("{a,b}/{c,d}/{e,f}" is 8), so a pathological entry could
// otherwise expand without bound. Past it the remaining alternatives are dropped.
inline constexpr std::size_t kMaxExpandedGlobPatterns = 256;

// Expand `{a,b}` alternation into concrete patterns, appending to `out` and
// stopping once `out` reaches kMaxExpandedGlobPatterns. Braces are matched with
// nesting awareness and '\\' escapes are respected, so "a\\{b" is a literal brace;
// an unbalanced '{' is kept as literal text rather than dropping the pattern.
//
// `GlobMatches` itself has no brace support, so every caller that accepts
// brace syntax must run its patterns through here first. Shared because the
// search scope box and the LSP `didChangeWatchedFiles` watcher registry both
// take VSCode-style globs and must agree on what `{a,b}` means.
void ExpandGlobBraces(std::string pattern, std::vector<std::string>& out);

// A parsed, VSCode-style "files to include" / "files to exclude" filter: a
// comma-separated list of globs matched against forward-slash, root-relative paths.
//
// Parsing follows VSCode's search-box conventions:
//   - Entries are split on ',' and trimmed; empty entries are dropped.
//   - `{a,b}` alternation is expanded at parse time into separate patterns (the
//     underlying matcher has no brace support), including nested braces.
//   - A leading "./" or "/" is stripped; a trailing "/" is dropped.
//   - A pattern with no '/' floats: "*.cpp" becomes "**/*.cpp", so it matches at
//     any depth. A pattern containing '/' stays anchored to the project root.
//   - A pattern with no wildcard characters names a file OR a directory subtree:
//     "src/util" matches that exact path and everything beneath it, and "tests"
//     matches any path segment named "tests" and everything beneath it.
//
// Matching is allocation-free; all storage is built once at parse time.
class GlobSet {
 public:
  GlobSet() = default;

  // Parse a comma-separated pattern list. Never fails: unparseable/degenerate
  // entries are dropped, yielding an empty (inactive) set.
  static GlobSet Parse(std::string_view comma_separated);

  // An inactive set matches nothing and should be skipped by callers entirely
  // (an empty "files to include" box means "no include filter", not "exclude
  // everything").
  bool empty() const { return patterns_.empty(); }
  std::size_t size() const { return patterns_.size(); }

  // `normalized_relative_path` must already be forward-slash and root-relative
  // (no leading "./" or "/"). Returns true when ANY pattern matches.
  bool Matches(std::string_view normalized_relative_path) const;

  const std::vector<std::string>& patterns() const { return patterns_; }

 private:
  std::vector<std::string> patterns_;
};

}  // namespace microide::project
