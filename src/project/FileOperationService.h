#pragma once

#include <filesystem>
#include <string>

namespace microide::project {

struct FileOperationResult {
  bool ok = false;
  std::filesystem::path resulting_path;
  std::string error_message;
};

class FileOperationService {
 public:
  static FileOperationResult CreateFile(const std::filesystem::path& path);
  static FileOperationResult CreateDirectory(const std::filesystem::path& path);
  static FileOperationResult RenamePath(const std::filesystem::path& source,
                                        const std::filesystem::path& destination);
  static FileOperationResult TrashPath(const std::filesystem::path& path);
};

}  // namespace microide::project
