#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "util/SmallVector.h"

namespace microide::project {

class IgnoreMatcher {
 public:
  // Create a child matcher that inherits `parent` as a shared, immutable layer
  // (its rules are referenced, never copied) and shares the parent's root. The
  // child starts with no local rules — load this directory's `.gitignore` (and
  // any local additions) into it. A whole-tree walk therefore stores each
  // directory's inherited context as a parent link plus its own rules, instead
  // of copying the full ancestor rule set into every visited directory
  // (TD-2026-07-17A-055). `parent` must outlive every descendant it is linked to
  // and must not be mutated after it becomes a parent (it is read as const).
  static std::shared_ptr<IgnoreMatcher> MakeChild(
      const std::shared_ptr<const IgnoreMatcher>& parent);

  bool SetRoot(const std::filesystem::path& root);
  void LoadIgnoreFile(const std::filesystem::path& path);
  // Seed built-in ignore defaults: VCS metadata (.git/.svn/.hg/.bzr), dependency
  // and cache trees (node_modules, .cache, .venv, __pycache__), and common
  // build-output directory names (build, builds, out, dist, target,
  // cmake-build-*, .vs, bin, obj). All are directory-only, basename-matched at
  // any depth, and expressed as ordinary gitignore rules. Callers append these
  // after SetRoot() so they take precedence over a project's root .gitignore; a
  // project that keeps source under one of these names can re-include it with a
  // "!name/" entry via AddExcludeGlobs(). These directories are only skipped by
  // the file index / finder / watcher — the sidebar tree still lists them grayed.
  void AddDefaultRules();
  // Append user/project-configured ignore rules (gitignore syntax, root-anchored).
  // Called after AddDefaultRules() so an explicit "!name/" re-include wins.
  void AddExcludeGlobs(const std::vector<std::string>& globs);
  bool Ignored(const std::filesystem::path& relative_path, bool is_directory) const;
  // Fast path for callers that already hold a forward-slash, lexically-normalized
  // relative path (file watchers / monitors on the traversal hot path): skips the
  // per-call lexically_normal()+generic_string() that the path overload performs.
  // Distinct name (not an overload) so string-literal callers stay unambiguous.
  bool IgnoredNormalized(std::string_view normalized_relative_path, bool is_directory) const;

 private:
  // What shape a rule's pattern has, decided once at parse time. Every whole-tree
  // walk in the app runs the full rule set (project defaults + the root
  // `.gitignore`, ~45 rules here) against every filesystem entry AND, for a
  // basename rule, against every path component of it — so the per-rule test is
  // one of the hottest inner loops a project open executes. Nearly all real rules
  // are a literal name (`node_modules`), a suffix (`*.pyc`) or a prefix
  // (`cmake-build-*`); those answer in a compare instead of a run through the
  // general backtracking glob matcher.
  enum class PatternShape : unsigned char {
    Exact,    // no metacharacters: text == literal
    Suffix,   // "*rest", basename rules only: text ends with literal
    Prefix,   // "rest*", basename rules only: text starts with literal
    Glob,     // anything else: GlobMatches, gated by a literal-prefix reject
  };

  // The '/'-separated components of a candidate path, split once per query rather
  // than once per basename rule. A whole-tree walk evaluates ~45 rules per entry
  // and most of them are basename rules, so the old shape rescanned the same path
  // text for separators forty-odd times per file. 16 inline slots cover any real
  // project path; a deeper one spills and still answers correctly.
  using PathComponents = util::SmallVector<std::string_view, 16>;

  struct Rule {
    std::string base_relative;
    // base_relative + "/" precomputed once at parse time; empty when the rule has
    // no base directory (root .gitignore) so Matches avoids a per-call allocation.
    std::string base_prefix;
    std::string pattern;
    // Exact/Suffix/Prefix: the literal text the shape compares against. Glob: the
    // leading run of literal characters before the first metacharacter, which the
    // text must start with for any match to be possible — the cheap reject that
    // keeps an anchored pattern like "tests/fuzz/corpora/*/*" from running the
    // full matcher over every path in the tree.
    std::string literal;
    // How many leading components `base_prefix` consumes, so a based rule can skip
    // that many entries of the shared component list instead of re-splitting the
    // stripped remainder.
    std::size_t base_component_count = 0;
    PatternShape shape = PatternShape::Glob;
    bool negated = false;
    bool directory_only = false;
    bool match_basename = false;

    bool Matches(std::string_view relative_path, const PathComponents& components,
                 bool is_directory) const;
    // The shape test for one already-stripped candidate (a whole relative path for
    // an anchored rule, a single path component for a basename rule).
    bool MatchesText(std::string_view text) const;
  };

  static bool ParseRule(std::string base_relative, std::string line, Rule& out_rule);
  // Shared body of IgnoredNormalized and its parent-chain recursion: every layer
  // reads the same candidate, so the split is done once by the public entry point.
  bool IgnoredWithComponents(std::string_view normalized_relative_path,
                             const PathComponents& components, bool is_directory) const;

  // Inherited ancestor context, shared and immutable. Null for a standalone /
  // root matcher. Evaluated before this matcher's own rules so a child's rules
  // (later in gitignore order) still override an inherited decision.
  std::shared_ptr<const IgnoreMatcher> parent_;
  std::filesystem::path root_;
  // This matcher's OWN rules only (this directory's .gitignore + any local
  // defaults/excludes); the ancestor chain lives behind parent_.
  std::vector<Rule> rules_;
};

}  // namespace microide::project
