#include "project/EditorConfig.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

// `.editorconfig` is read out of the OPENED REPOSITORY, so its bytes are supplied
// by whoever wrote the checkout — the same trust level as `.gitignore`, and lower
// than anything the user typed. The parser is hand-rolled (sections, globs,
// key=value, an unknown-property rule, a byte cap and a section cap), which is the
// combination worth fuzzing: every cap is a subtraction and every section header
// is a bracket search.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  if (data == nullptr) {
    return 0;
  }
  const std::string_view input(reinterpret_cast<const char*>(data), size);
  const microide::project::EditorConfigFile parsed = microide::project::ParseEditorConfig(input);
  // Touch the results so the parse cannot be optimized away, and assert the two
  // structural caps the parser promises.
  if (parsed.sections.size() > microide::project::kMaxEditorConfigSections) {
    __builtin_trap();
  }
  std::size_t pattern_bytes = 0;
  for (const auto& section : parsed.sections) {
    // Read every parsed field so the parse cannot be optimized away, and so a
    // pattern list built out of bounds is actually touched under the sanitizer.
    for (const std::string& pattern : section.patterns) {
      pattern_bytes += pattern.size();
    }
    if (section.properties.any() && pattern_bytes == 0xFFFFFFFFu) {
      __builtin_trap();  // unreachable; keeps the reads live
    }
  }
  (void)parsed.root;
  return 0;
}
