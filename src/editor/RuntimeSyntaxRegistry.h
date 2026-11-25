#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "editor/LineSpan.h"
#include "editor/RuntimeSyntaxData.h"
#include "editor/SyntaxHighlighter.h"

namespace microide::editor::runtime_syntax {

inline constexpr std::size_t kMaxHighlightLineBytes = 100000;

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
void EnsureInitialized();
void WarmInBackground();
std::size_t RegistryRevision();
SyntaxState DetectState(const std::filesystem::path& path, LineSpan lines);
SyntaxState DetectState(const std::filesystem::path& path, std::string_view text);
inline constexpr std::size_t kFiletypeDetectHeadLines = 64;

std::string DetectFiletype(const std::filesystem::path& path, LineSpan lines,
                           bool* content_consulted = nullptr);
std::string DetectFiletype(const std::filesystem::path& path);

inline constexpr std::string_view kPlainTextFiletype = "plaintext";
inline constexpr std::uint32_t kPlainTextDefinitionId = 0xFFFFFFFFu;
std::vector<std::string> FiletypeNames();
SyntaxState StateForFiletype(std::string_view filetype);

void CompileDefinition(std::uint32_t definition_id);

HighlightedLine HighlightLine(std::string_view line,
                              const std::filesystem::path& path,
                              const SyntaxState& state,
                              std::string_view first_line);
SyntaxState HighlightLineInto(std::string_view line,
                              const std::filesystem::path& path,
                              const SyntaxState& state,
                              std::vector<SyntaxTokenKind>* tokens,
                              std::string_view first_line = {});
SyntaxState AdvanceState(std::string_view line,
                         const std::filesystem::path& path,
                         const SyntaxState& state,
                         std::string_view first_line = {});

}
