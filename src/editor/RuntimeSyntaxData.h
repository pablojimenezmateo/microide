#pragma once

#include <cstddef>

namespace microide::editor::runtime_syntax {

enum class GeneratedRuleKind {
  Pattern,
  Region,
};

struct GeneratedRuleData {
  GeneratedRuleKind kind = GeneratedRuleKind::Pattern;
  const char* group_name = nullptr;
  const char* limit_group_name = nullptr;
  const char* pattern = nullptr;
  const char* start_regex = nullptr;
  const char* end_regex = nullptr;
  const char* skip_regex = nullptr;
  std::size_t first_child = 0;
  std::size_t child_count = 0;
  std::size_t parent_region_id = 0;
};

struct GeneratedDefinitionData {
  const char* filetype = nullptr;
  const char* filename_regex = nullptr;
  const char* header_regex = nullptr;
  const char* signature_regex = nullptr;
  std::size_t first_rule = 0;
  std::size_t rule_count = 0;
};

extern const GeneratedDefinitionData kGeneratedDefinitions[];
extern const std::size_t kGeneratedDefinitionCount;
extern const GeneratedRuleData kGeneratedRules[];
extern const std::size_t kGeneratedRuleCount;

}  // namespace microide::editor::runtime_syntax
