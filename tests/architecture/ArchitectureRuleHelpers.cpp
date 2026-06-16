#include "architecture/ArchitectureRuleHelpers.h"

#include <fstream>
#include <iostream>
#include <sstream>

namespace microide::tests::architecture {

void AppendViolations(RuleResult& result,
                      const std::filesystem::path& path,
                      const std::string& text,
                      const std::regex& pattern,
                      std::string_view message) {
  for (std::sregex_iterator it(text.begin(), text.end(), pattern), end; it != end; ++it) {
    result.violations.push_back(Violation{
        .path = path,
        .line = LineNumberAt(text, static_cast<std::size_t>(it->position())),
        .message = std::string(message),
    });
  }
}

void AppendCodeMaskRegexViolations(RuleResult& result,
                                   const std::filesystem::path& path,
                                   const std::string& text,
                                   const std::regex& pattern,
                                   std::string_view message) {
  const auto is_code = BuildCodeMask(text);
  for (std::sregex_iterator it(text.begin(), text.end(), pattern), end; it != end; ++it) {
    const std::size_t start = static_cast<std::size_t>(it->position());
    const std::size_t len = static_cast<std::size_t>(it->length());
    bool in_code = true;
    for (std::size_t i = 0; i < len; ++i) {
      if (start + i >= is_code.size() || !is_code[start + i]) {
        in_code = false;
        break;
      }
    }
    if (!in_code) {
      continue;
    }
    result.violations.push_back(Violation{
        .path = path,
        .line = LineNumberAt(text, start),
        .message = std::string(message),
    });
  }
}

void AppendTrailingCodeRegexViolations(RuleResult& result,
                                       const std::filesystem::path& path,
                                       const std::string& text,
                                       const std::regex& pattern,
                                       std::string_view message) {
  const auto is_code = BuildCodeMask(text);
  for (std::sregex_iterator it(text.begin(), text.end(), pattern), end; it != end; ++it) {
    const std::size_t start = static_cast<std::size_t>(it->position());
    const std::size_t len = static_cast<std::size_t>(it->length());
    if (len == 0) {
      continue;
    }
    const std::size_t last = start + len - 1;
    if (last >= is_code.size() || !is_code[last]) {
      continue;
    }
    result.violations.push_back(Violation{
        .path = path,
        .line = LineNumberAt(text, start),
        .message = std::string(message),
    });
  }
}

RuleResult CheckShellFileSize(const std::filesystem::path& repo_root,
                              std::string_view relative_path,
                              std::size_t limit) {
  RuleResult result;
  result.label = std::string(relative_path) + " size";
  result.hard_fail = true;
  const std::filesystem::path path = repo_root / relative_path;
  const std::size_t lines = CountCodeLinesInFile(path);
  if (lines > limit) {
    result.violations.push_back(Violation{
        .path = path,
        .line = 1,
        .message = std::string(relative_path) + " should stay at or below " +
                   std::to_string(limit) + " code lines (comments and blank lines excluded)",
    });
  }
  return result;
}

void ReportRule(const RuleResult& result) {
  if (result.violations.empty()) {
    return;
  }
  std::cerr << "ArchitectureInvariants warning: " << result.label << '\n';
  const std::filesystem::path repo_root = RepoRoot().lexically_normal();
  for (const Violation& violation : result.violations) {
    const std::filesystem::path relative =
        violation.path.lexically_normal().lexically_relative(repo_root);
    const std::filesystem::path display_path =
        relative.empty() ? violation.path.lexically_normal() : relative;
    std::cerr << "  " << display_path.generic_string() << ':' << violation.line << ": "
              << violation.message << '\n';
  }
}

}  // namespace microide::tests::architecture
