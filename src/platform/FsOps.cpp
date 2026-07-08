#include "platform/FsOps.h"

#include <system_error>

namespace microide::platform {

std::filesystem::path NormalizeAbsolutePath(const std::filesystem::path& path) {
  std::error_code error;
  const std::filesystem::path absolute = std::filesystem::absolute(path, error);
  return (error ? path : absolute).lexically_normal();
}

bool CopyPath(const std::filesystem::path& source, const std::filesystem::path& destination) {
  std::error_code error;
  if (std::filesystem::is_directory(source, error)) {
    if (error) {
      return false;
    }
    std::filesystem::create_directories(destination, error);
    if (error) {
      return false;
    }
    for (const auto& entry : std::filesystem::recursive_directory_iterator(source, error)) {
      if (error) {
        return false;
      }
      const std::filesystem::path relative = std::filesystem::relative(entry.path(), source, error);
      if (error) {
        return false;
      }
      const std::filesystem::path target = destination / relative;
      if (entry.is_directory(error)) {
        if (error) {
          return false;
        }
        std::filesystem::create_directories(target, error);
        if (error) {
          return false;
        }
        continue;
      }

      std::filesystem::create_directories(target.parent_path(), error);
      if (error) {
        return false;
      }
      std::filesystem::copy_file(entry.path(), target, std::filesystem::copy_options::none, error);
      if (error) {
        return false;
      }
    }
    return true;
  }

  std::filesystem::create_directories(destination.parent_path(), error);
  if (error) {
    return false;
  }
  std::filesystem::copy_file(source, destination, std::filesystem::copy_options::none, error);
  return !error;
}

bool RemovePath(const std::filesystem::path& path) {
  std::error_code error;
  if (std::filesystem::is_directory(path, error)) {
    if (error) {
      return false;
    }
    std::filesystem::remove_all(path, error);
    return !error;
  }

  std::filesystem::remove(path, error);
  return !error;
}

bool MovePath(const std::filesystem::path& source, const std::filesystem::path& destination) {
  std::error_code error;
  std::filesystem::rename(source, destination, error);
  if (!error) {
    return true;
  }

  if (!CopyPath(source, destination)) {
    return false;
  }
  if (!RemovePath(source)) {
    return false;
  }
  return true;
}

}  // namespace microide::platform
