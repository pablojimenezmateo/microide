#pragma once

#include <filesystem>
#include <string>

namespace microide::editor {

inline std::string NormalizedPathKey(const std::filesystem::path& path) {
  return path.empty() ? std::string{} : path.lexically_normal().generic_string();
}

}
