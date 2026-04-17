#pragma once

#include <filesystem>
#include <string>

namespace microide::workspace {

std::string RelativePathLabel(const std::filesystem::path& root,
                              const std::filesystem::path& path);
bool PathEqualsOrWithin(const std::filesystem::path& candidate,
                        const std::filesystem::path& root);
std::filesystem::path ReplacePathPrefix(const std::filesystem::path& path,
                                        const std::filesystem::path& old_prefix,
                                        const std::filesystem::path& new_prefix);

}  // namespace microide::workspace
