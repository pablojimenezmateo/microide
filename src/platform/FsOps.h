#pragma once

#include <filesystem>

namespace microide::platform {

std::filesystem::path NormalizeAbsolutePath(const std::filesystem::path& path);

bool CopyPath(const std::filesystem::path& source, const std::filesystem::path& destination);

bool RemovePath(const std::filesystem::path& path);

bool MovePath(const std::filesystem::path& source, const std::filesystem::path& destination);

bool MovePathNoOverwrite(const std::filesystem::path& source,
                         const std::filesystem::path& destination);

}
