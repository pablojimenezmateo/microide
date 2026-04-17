#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace microide::util {

std::optional<std::string> ReadTextFile(const std::filesystem::path& path);
bool WriteTextFileAtomically(const std::filesystem::path& path, std::string_view text);

}  // namespace microide::util
