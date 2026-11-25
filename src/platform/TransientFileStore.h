#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace microide::platform {

bool EnsurePrivateDirectory(const std::filesystem::path& directory, std::string* error);

std::optional<std::filesystem::path> StoreTransientFile(const std::filesystem::path& root,
                                                        const std::string& name, std::string_view content,
                                                        std::string* error);

}
