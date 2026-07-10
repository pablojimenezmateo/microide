#include "project/FileOperationService.h"

#include "platform/FsOps.h"
#include "platform/Trash.h"

#include <cerrno>
#include <fstream>
#include <system_error>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace microide::project {

namespace {

// Atomically create `path` failing if it already exists. Returns 1 on success, 0
// if the file already existed (EEXIST), -1 on any other error. O_CREAT|O_EXCL is
// a single kernel operation, so unlike an exists() probe followed by a truncating
// open it cannot clobber a file another process creates in the meantime.
int ExclusiveCreateEmptyFile(const std::filesystem::path& path) {
#ifdef _WIN32
  int fd = -1;
  const errno_t open_error = _wsopen_s(&fd, path.c_str(),
                                       _O_BINARY | _O_CREAT | _O_EXCL | _O_WRONLY,
                                       _SH_DENYNO, _S_IREAD | _S_IWRITE);
  if (open_error != 0 || fd < 0) {
    return (open_error == EEXIST) ? 0 : -1;
  }
  _close(fd);
  return 1;
#else
  const int fd = ::open(path.c_str(), O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0644);
  if (fd < 0) {
    return (errno == EEXIST) ? 0 : -1;
  }
  ::close(fd);
  return 1;
#endif
}

FileOperationResult Failure(std::string message) {
  return FileOperationResult{
      .ok = false,
      .resulting_path = {},
      .error_message = std::move(message),
  };
}

FileOperationResult Success(const std::filesystem::path& path) {
  return FileOperationResult{
      .ok = true,
      .resulting_path = path,
      .error_message = {},
  };
}

using platform::MovePath;
using platform::NormalizeAbsolutePath;

bool IsReservedPathComponent(const std::filesystem::path& path) {
  for (const auto& component : path) {
    const std::string name = component.string();
    if (name == "." || name == "..") {
      return true;
    }
  }
  return false;
}

}  // namespace

FileOperationResult FileOperationService::CreateFile(const std::filesystem::path& path) {
  const std::filesystem::path normalized_path = NormalizeAbsolutePath(path);
  if (normalized_path.empty()) {
    return Failure("No file path was provided");
  }
  if (IsReservedPathComponent(normalized_path.filename())) {
    return Failure("Invalid file name");
  }

  const std::filesystem::path parent = normalized_path.parent_path();
  if (parent.empty()) {
    return Failure("The file path has no parent directory");
  }
  std::error_code error;
  std::filesystem::create_directories(parent, error);
  if (error) {
    return Failure("Failed to create the parent directory");
  }

  // Exclusive create: never truncate an existing file. A plain exists()-then-open
  // sequence would silently overwrite a file that a racing process created between
  // the two steps.
  const int created = ExclusiveCreateEmptyFile(normalized_path);
  if (created == 0) {
    return Failure("The file already exists");
  }
  if (created < 0) {
    return Failure("Failed to create the file");
  }
  return Success(normalized_path);
}

FileOperationResult FileOperationService::CreateDirectory(const std::filesystem::path& path) {
  const std::filesystem::path normalized_path = NormalizeAbsolutePath(path);
  if (normalized_path.empty()) {
    return Failure("No directory path was provided");
  }
  if (IsReservedPathComponent(normalized_path.filename())) {
    return Failure("Invalid directory name");
  }

  std::error_code error;
  if (std::filesystem::exists(normalized_path, error)) {
    return Failure("The directory already exists");
  }

  std::filesystem::create_directories(normalized_path, error);
  if (error) {
    return Failure("Failed to create the directory");
  }
  return Success(normalized_path);
}

FileOperationResult FileOperationService::RenamePath(const std::filesystem::path& source,
                                                     const std::filesystem::path& destination) {
  const std::filesystem::path normalized_source = NormalizeAbsolutePath(source);
  const std::filesystem::path normalized_destination = NormalizeAbsolutePath(destination);
  if (normalized_source.empty() || normalized_destination.empty()) {
    return Failure("A source and destination path are required");
  }
  if (normalized_source == normalized_destination) {
    return Failure("The new path matches the current path");
  }
  if (IsReservedPathComponent(normalized_destination.filename())) {
    return Failure("Invalid destination name");
  }

  std::error_code error;
  if (!std::filesystem::exists(normalized_source, error)) {
    return Failure("The source path does not exist");
  }
  if (std::filesystem::exists(normalized_destination, error)) {
    return Failure("The destination path already exists");
  }

  const std::filesystem::path parent = normalized_destination.parent_path();
  if (parent.empty()) {
    return Failure("The destination path has no parent directory");
  }
  std::filesystem::create_directories(parent, error);
  if (error) {
    return Failure("Failed to prepare the destination directory");
  }

  if (!MovePath(normalized_source, normalized_destination)) {
    return Failure("Failed to rename the path");
  }
  return Success(normalized_destination);
}

FileOperationResult FileOperationService::TrashPath(const std::filesystem::path& path) {
  const platform::TrashOperationResult result = platform::MovePathToTrash(path);
  return FileOperationResult{
      .ok = result.ok,
      .resulting_path = result.resulting_path,
      .error_message = result.error_message,
  };
}

}  // namespace microide::project
