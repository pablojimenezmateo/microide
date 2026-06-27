#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "editor/LineSpan.h"

namespace microide::editor {

enum class SyntaxTokenKind {
  Plain,
  Keyword,
  Type,
  String,
  Comment,
  Number,
  Constant,
  Preprocessor,
  Operator,
};

// Per-line carry state for the highlighter. `region_stack` holds the chain of
// open multi-line regions (outermost at index 0, innermost at depth-1) so a
// region nested inside another resumes against its real parent when the inner
// one closes — a single innermost id would drop the parent. The stack is a
// fixed inline array to keep SyntaxState a small, trivially-copyable POD: it is
// stored per line and per checkpoint, so heap allocation here would be costly.
// Nesting beyond kMaxRegionDepth degrades gracefully (deeper regions are not
// pushed) rather than allocating.
struct SyntaxState {
  static constexpr std::size_t kMaxRegionDepth = 8;

  std::uint32_t definition_id = 0;
  std::uint8_t region_depth = 0;
  std::uint32_t region_stack[kMaxRegionDepth] = {};

  bool operator==(const SyntaxState& other) const {
    if (definition_id != other.definition_id || region_depth != other.region_depth) {
      return false;
    }
    for (std::size_t i = 0; i < region_depth; ++i) {
      if (region_stack[i] != other.region_stack[i]) {
        return false;
      }
    }
    return true;
  }
  bool operator!=(const SyntaxState& other) const { return !(*this == other); }
};

struct HighlightedLine {
  std::vector<SyntaxTokenKind> tokens;
  SyntaxState end_state;
};

class SyntaxHighlighter {
 public:
  // `lines` may be the live (piece-tree-backed) document: detection only ever
  // inspects a bounded head, so passing a TextBuffer here never materializes the
  // whole file. See runtime_syntax::DetectState.
  static SyntaxState InitialState(const std::filesystem::path& path, LineSpan lines);
  static HighlightedLine HighlightLine(std::string_view line,
                                       const std::filesystem::path& path,
                                       const SyntaxState& state = {},
                                       std::string_view first_line = {});
  static SyntaxState AdvanceState(std::string_view line,
                                  const std::filesystem::path& path,
                                  const SyntaxState& state = {},
                                  std::string_view first_line = {});
};

}  // namespace microide::editor
