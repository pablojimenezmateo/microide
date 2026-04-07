#include "editor/SyntaxHighlighter.h"

#include "editor/RuntimeSyntaxRegistry.h"

namespace microide::editor {

SyntaxState SyntaxHighlighter::InitialState(const std::filesystem::path& path,
                                            const std::vector<std::string>& lines) {
  return runtime_syntax::DetectState(path, lines);
}

HighlightedLine SyntaxHighlighter::HighlightLine(std::string_view line,
                                                 const std::filesystem::path& path,
                                                 const SyntaxState& state,
                                                 std::string_view first_line) {
  return runtime_syntax::HighlightLine(line, path, state, first_line);
}

}  // namespace microide::editor
