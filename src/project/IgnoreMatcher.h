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
