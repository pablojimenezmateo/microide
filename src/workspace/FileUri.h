#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace microide::workspace {

std::string FileUriForPath(const std::filesystem::path& path);

std::optional<std::filesystem::path> PathFromFileUri(std::string_view uri);

}
