#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace microide::project {

class IgnoreMatcher {
 public:
  bool SetRoot(const std::filesystem::path& root);
  void LoadIgnoreFile(const std::filesystem::path& path);
  bool Ignored(const std::filesystem::path& relative_path, bool is_directory) const;

 private:
  struct Rule {
    std::string base_relative;
    std::string pattern;
    bool negated = false;
    bool directory_only = false;
    bool match_basename = false;

    bool Matches(std::string relative_path, bool is_directory) const;
  };

  static bool ParseRule(std::string base_relative, std::string line, Rule& out_rule);

  std::filesystem::path root_;
  std::vector<Rule> rules_;
};

}  // namespace microide::project
