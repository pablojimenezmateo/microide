#pragma once

#include <filesystem>
#include <string>

namespace microide::platform {

struct TrashOperationResult {
  bool ok = false;
  std::filesystem::path resulting_path;
  std::string error_message;
};

TrashOperationResult MovePathToTrash(const std::filesystem::path& path);

}  // namespace microide::platform
