#include "editor/SyntaxHighlighter.h"

#include "editor/RuntimeSyntaxRegistry.h"
#include "util/PerformanceTrace.h"

namespace microide::editor {

SyntaxState SyntaxHighlighter::InitialState(const std::filesystem::path& path, LineSpan lines) {
  util::PerformanceTrace::Scope perf_scope("SyntaxHighlighter::InitialState");
  return runtime_syntax::DetectState(path, lines);
}

SyntaxState SyntaxHighlighter::InitialState(const std::filesystem::path& path,
                                            std::string_view text) {
  util::PerformanceTrace::Scope perf_scope("SyntaxHighlighter::InitialState");
  return runtime_syntax::DetectState(path, text);
}

HighlightedLine SyntaxHighlighter::HighlightLine(std::string_view line,
                                                 const std::filesystem::path& path,
                                                 const SyntaxState& state,
                                                 std::string_view first_line) {
  util::PerformanceTrace::Scope perf_scope("SyntaxHighlighter::HighlightLine");
  return runtime_syntax::HighlightLine(line, path, state, first_line);
}

SyntaxState SyntaxHighlighter::AdvanceState(std::string_view line,
                                            const std::filesystem::path& path,
                                            const SyntaxState& state,
                                            std::string_view first_line) {
  util::PerformanceTrace::Scope perf_scope("SyntaxHighlighter::AdvanceState");
  return runtime_syntax::AdvanceState(line, path, state, first_line);
}

}  // namespace microide::editor
