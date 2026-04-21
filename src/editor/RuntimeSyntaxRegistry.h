#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "editor/RuntimeSyntaxData.h"
#include "editor/SyntaxHighlighter.h"

namespace microide::editor::runtime_syntax {

struct RuntimeSyntaxRuleData {
  GeneratedRuleKind kind = GeneratedRuleKind::Pattern;
  std::string group_name;
  std::string limit_group_name;
  std::string pattern;
  std::string start_regex;
  std::string end_regex;
  std::string skip_regex;
  std::vector<RuntimeSyntaxRuleData> children;
};

struct RuntimeSyntaxDefinitionData {
  std::string filetype;
  std::vector<std::string> filename_patterns;
  std::vector<std::string> header_patterns;
  std::vector<std::string> signature_patterns;
  std::vector<RuntimeSyntaxRuleData> rules;
  std::filesystem::path source_path;
};

struct RuntimeSyntaxReloadResult {
  std::size_t built_in_definition_count = 0;
  std::size_t plugin_definition_count = 0;
  std::size_t error_count = 0;
};

RuntimeSyntaxReloadResult ReloadDefinitions(
    const std::vector<RuntimeSyntaxDefinitionData>& definitions,
    std::vector<std::string>* errors = nullptr);
std::size_t RegistryRevision();
SyntaxState DetectState(const std::filesystem::path& path, const std::vector<std::string>& lines);
std::string DetectFiletype(const std::filesystem::path& path,
                           const std::vector<std::string>& lines);

HighlightedLine HighlightLine(std::string_view line,
                              const std::filesystem::path& path,
                              const SyntaxState& state,
                              std::string_view first_line);

}  // namespace microide::editor::runtime_syntax
