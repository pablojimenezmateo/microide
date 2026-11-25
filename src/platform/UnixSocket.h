#pragma once

#include <cstddef>
#include <filesystem>
#include <string>

namespace microide::platform {

inline constexpr std::size_t kMaxUnixSocketPathBytes = 107;

int ConnectUnixSocket(const std::filesystem::path& path);

int ListenUnixSocket(const std::filesystem::path& path, std::string* error);

bool ClearStaleSocketPath(const std::filesystem::path& path);

}
