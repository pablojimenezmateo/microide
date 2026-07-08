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
    // Use the error_code overload of the recursive copy rather than hand-rolling a
    // recursive_directory_iterator loop: the range-for form advances with the
    // THROWING operator++, so a permission-denied subdirectory or an entry removed
    // mid-walk would throw std::filesystem_error straight out of here (and out of
    // MovePath's cross-device fallback / RenamePath). copy_symlinks preserves
    // symlinks instead of dereferencing them into real files/dirs.
    std::filesystem::copy(
        source, destination,
        std::filesystem::copy_options::recursive | std::filesystem::copy_options::copy_symlinks,
        error);
    return !error;
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
