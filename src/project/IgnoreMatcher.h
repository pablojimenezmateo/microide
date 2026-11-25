#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace microide::project {

class IgnoreMatcher {
 public:
  static std::shared_ptr<IgnoreMatcher> MakeChild(
      const std::shared_ptr<const IgnoreMatcher>& parent);

  static std::shared_ptr<const IgnoreMatcher> ForDirectory(
      const std::shared_ptr<const IgnoreMatcher>& parent,
      const std::filesystem::path& directory);

  static std::shared_ptr<IgnoreMatcher> ForProjectRoot(
      const std::filesystem::path& root, const std::vector<std::string>& exclude_globs);

  bool SetRoot(const std::filesystem::path& root);
  void LoadIgnoreFile(const std::filesystem::path& path);
  void AddDefaultRules();
  void AddExcludeGlobs(const std::vector<std::string>& globs);
  bool Ignored(const std::filesystem::path& relative_path, bool is_directory) const;
  bool IgnoredNormalized(std::string_view normalized_relative_path, bool is_directory) const;
  bool IgnoredEntryNormalized(std::string_view normalized_relative_path,
                              bool is_directory) const;

 private:
  enum class PatternShape : unsigned char {
    Exact,
    Suffix,
    Prefix,
    Glob,
  };

  struct Rule {
    std::string base_relative;
    std::string base_prefix;
    std::string pattern;
    std::string literal;
    PatternShape shape = PatternShape::Glob;
    bool negated = false;
    bool directory_only = false;
    bool match_basename = false;

    bool Matches(std::string_view relative_path, std::string_view last_component,
                 bool is_directory) const;
    bool MatchesText(std::string_view text) const;
  };

  static bool ParseRule(std::string base_relative, std::string line, Rule& out_rule);
  static bool GlobMatchesAfterLiteral(std::string_view pattern, std::string_view literal,
                                      std::string_view text);
  bool IgnoredEntry(std::string_view normalized_relative_path, std::string_view last_component,
                    bool is_directory) const;

  std::shared_ptr<const IgnoreMatcher> parent_;
  std::filesystem::path root_;
  std::vector<Rule> rules_;
};

}
