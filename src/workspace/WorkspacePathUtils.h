#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>

namespace microide::workspace {

std::string RelativePathLabel(const std::filesystem::path& root,
                              const std::filesystem::path& path);
// How a place in a file is named to a terminal program -- an agent's prompt, a
// compiler, grep: `src/main.cpp:12`, or `src/main.cpp:12-40` for a span. Lines
// are 0-based here and 1-based in the text.
std::string PathLineReference(std::string_view path_label, std::size_t first_line, std::size_t last_line);
bool PathEqualsOrWithin(const std::filesystem::path& candidate,
                        const std::filesystem::path& root);

}  // namespace microide::workspace
