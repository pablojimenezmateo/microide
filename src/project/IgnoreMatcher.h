#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace microide::project {

class IgnoreMatcher {
 public:
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
  struct Rule {
    std::string base_relative;
    // base_relative + "/" precomputed once at parse time; empty when the rule has
    // no base directory (root .gitignore) so Matches avoids a per-call allocation.
    std::string base_prefix;
    std::string pattern;
    bool negated = false;
    bool directory_only = false;
    bool match_basename = false;

    bool Matches(std::string_view relative_path, bool is_directory) const;
  };

  static bool ParseRule(std::string base_relative, std::string line, Rule& out_rule);

  std::filesystem::path root_;
  std::vector<Rule> rules_;
};

}  // namespace microide::project
