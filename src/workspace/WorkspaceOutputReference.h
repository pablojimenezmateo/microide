#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string_view>

namespace microide::workspace {

// A parsed "<path>:<line>:<column>" reference from a tool-output line (compiler
// errors, grep results, task output, ...). `line`/`column` are 1-based, exactly
// as tools emit them; callers convert to 0-based when driving the editor.
struct OutputReference {
  std::filesystem::path path;
  std::size_t line = 0;
  std::size_t column = 0;
};

// Parses a trailing "<path>:<line>:<column>" reference. Returns nullopt when the
// two ':' delimiters are missing, the path is empty, the line/column are not
// unsigned integers, or the line is 0. Shared by the output-channel parser, the
// bottom-panel click handler, and the cursor hit-test so all three agree on what
// counts as a navigable output line.
std::optional<OutputReference> ParseOutputReference(std::string_view text);

}  // namespace microide::workspace
