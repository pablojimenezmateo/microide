#pragma once

#include "architecture/ArchitectureFileScanner.h"

#include <cstddef>
#include <filesystem>
#include <regex>
#include <string>
#include <string_view>
#include <vector>

namespace microide::tests::architecture {

struct Violation {
  std::filesystem::path path;
  std::size_t line = 0;
  std::string message;
};

struct RuleResult {
  std::string label;
  bool hard_fail = false;
  std::vector<Violation> violations;
};

void ReportRule(const RuleResult& result);
void AppendViolations(RuleResult& result,
                      const std::filesystem::path& path,
                      const std::string& text,
                      const std::regex& pattern,
                      std::string_view message);
void AppendCodeMaskRegexViolations(RuleResult& result,
                                   const std::filesystem::path& path,
                                   const std::string& text,
                                   const std::regex& pattern,
                                   std::string_view message);
RuleResult CheckShellFileSize(const std::filesystem::path& repo_root,
                              std::string_view relative_path,
                              std::size_t limit);

}  // namespace microide::tests::architecture
