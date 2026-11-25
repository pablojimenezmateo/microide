#include "platform/FsOps.h"

#include <system_error>

#include <fcntl.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <cerrno>

#ifndef RENAME_NOREPLACE
#define RENAME_NOREPLACE (1 << 0)
#endif

namespace microide::platform {

std::filesystem::path NormalizeAbsolutePath(const std::filesystem::path& path) {
  std::error_code error;
  const std::filesystem::path absolute = std::filesystem::absolute(path, error);
  return (error ? path : absolute).lexically_normal();
}

bool CopyPath(const std::filesystem::path& source, const std::filesystem::path& destination) {
  std::error_code error;

  const std::filesystem::file_status source_link_status =
      std::filesystem::symlink_status(source, error);
  if (error) {
    return false;
  }
  if (std::filesystem::is_symlink(source_link_status)) {
    const std::filesystem::path link_parent = destination.parent_path();
    if (!link_parent.empty()) {
      std::filesystem::create_directories(link_parent, error);
      if (error) {
        return false;
      }
    }
    std::filesystem::copy_symlink(source, destination, error);
    return !error;
  }

  if (std::filesystem::is_directory(source, error)) {
    if (error) {
      return false;
    }
    std::filesystem::copy(
        source, destination,
        std::filesystem::copy_options::recursive | std::filesystem::copy_options::copy_symlinks,
        error);
    return !error;
  }

  const std::filesystem::path parent = destination.parent_path();
  if (!parent.empty()) {
    std::filesystem::create_directories(parent, error);
    if (error) {
      return false;
    }
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

  const std::filesystem::file_status destination_status =
      std::filesystem::symlink_status(destination, error);
  const bool destination_preexisted =
      destination_status.type() != std::filesystem::file_type::not_found;
  error.clear();
  if (!CopyPath(source, destination)) {
    if (!destination_preexisted) {
      RemovePath(destination);
    }
    return false;
  }
  if (!RemovePath(source)) {
    if (!destination_preexisted) {
      RemovePath(destination);
    }
    return false;
  }
  return true;
}

bool MovePathNoOverwrite(const std::filesystem::path& source,
                         const std::filesystem::path& destination) {
  if (::syscall(SYS_renameat2, AT_FDCWD, source.c_str(), AT_FDCWD, destination.c_str(),
                RENAME_NOREPLACE) == 0) {
    return true;
  }
  const int err = errno;
  if (err == EEXIST || err == ENOTEMPTY) {
    return false;
  }
  if (err != EXDEV && err != EINVAL && err != ENOSYS) {
    return false;
  }
  std::error_code error;
  const std::filesystem::file_status destination_status =
      std::filesystem::symlink_status(destination, error);
  if (destination_status.type() != std::filesystem::file_type::not_found) {
    return false;
  }
  if (!CopyPath(source, destination)) {
    RemovePath(destination);
    return false;
  }
  if (!RemovePath(source)) {
    RemovePath(destination);
    return false;
  }
  return true;
}

}
