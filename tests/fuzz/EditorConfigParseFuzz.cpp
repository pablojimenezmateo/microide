#include "project/EditorConfig.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  if (data == nullptr) {
    return 0;
  }
  const std::string_view input(reinterpret_cast<const char*>(data), size);
  const microide::project::EditorConfigFile parsed = microide::project::ParseEditorConfig(input);
  if (parsed.sections.size() > microide::project::kMaxEditorConfigSections) {
    __builtin_trap();
  }
  std::size_t pattern_bytes = 0;
  for (const auto& section : parsed.sections) {
    for (const std::string& pattern : section.patterns) {
      pattern_bytes += pattern.size();
    }
    if (section.properties.any() && pattern_bytes == 0xFFFFFFFFu) {
      __builtin_trap();
    }
  }
  (void)parsed.root;
  return 0;
}
