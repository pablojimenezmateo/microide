#pragma once

#include <filesystem>

namespace microide::project {

std::filesystem::path DetectProjectRoot(const std::filesystem::path& start);

}  // namespace microide::project
