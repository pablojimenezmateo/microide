#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>

namespace microide::workspace {

std::string RelativePathLabel(const std::filesystem::path& root,
                              const std::filesystem::path& path);
std::string PathLineReference(std::string_view path_label, std::size_t first_line, std::size_t last_line);
bool PathEqualsOrWithin(const std::filesystem::path& candidate,
                        const std::filesystem::path& root);

}
