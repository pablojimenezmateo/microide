#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

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

struct SyntaxState {
  std::uint32_t definition_id = 0;
  std::uint32_t region_id = 0;
};

struct HighlightedLine {
  std::vector<SyntaxTokenKind> tokens;
  SyntaxState end_state;
};

class SyntaxHighlighter {
 public:
  static SyntaxState InitialState(const std::filesystem::path& path,
                                  const std::vector<std::string>& lines);
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
