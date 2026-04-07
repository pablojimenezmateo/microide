#pragma once

#include <filesystem>
#include <string_view>
#include <vector>

#include "editor/SyntaxHighlighter.h"

namespace microide::editor::runtime_syntax {

SyntaxState DetectState(const std::filesystem::path& path, const std::vector<std::string>& lines);

HighlightedLine HighlightLine(std::string_view line,
                              const std::filesystem::path& path,
                              const SyntaxState& state,
                              std::string_view first_line);

}  // namespace microide::editor::runtime_syntax
