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

// Presence of the directory entry itself, without following a symlink. exists()
// dereferences the link, so a dangling symlink (a real, renameable entry) reports
// as absent (TD-2026-07-17A-125). A stat error also counts as "present" so
// rename/no-overwrite decisions fail closed rather than clobbering.
bool NodeExists(const std::filesystem::path& path) {
  std::error_code error;
  return std::filesystem::symlink_status(path, error).type() !=
         std::filesystem::file_type::not_found;
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
using platform::MovePathNoOverwrite;
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

  // Create-first, then classify. A prior exists() probe followed by
  // create_directories is a TOCTOU: a racing process can drop a non-directory at
  // the target in the window between the two calls. create_directories is the
  // authoritative step — it returns false without error when the path already
  // exists — so we distinguish "already exists" from a real failure by status.
  std::error_code error;
  const bool created = std::filesystem::create_directories(normalized_path, error);
  if (created) {
    return Success(normalized_path);
  }
  std::error_code status_error;
  const std::filesystem::file_status status =
      std::filesystem::status(normalized_path, status_error);
  if (!status_error && std::filesystem::is_directory(status)) {
    return Failure("The directory already exists");
  }
  if (!status_error && std::filesystem::exists(status)) {
    return Failure("A non-directory already exists at that path");
  }
  return Failure("Failed to create the directory");
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
  if (!NodeExists(normalized_source)) {
    return Failure("The source path does not exist");
  }
  // A pre-check is kept only for a clear early error message; the actual move is
  // no-overwrite and atomic (renameat2 RENAME_NOREPLACE) on the same filesystem,
  // so a destination racing into existence after this check still cannot clobber.
  if (NodeExists(normalized_destination)) {
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

  if (!MovePathNoOverwrite(normalized_source, normalized_destination)) {
    // Distinguish a lost race (destination now exists) from a generic failure.
    if (NodeExists(normalized_destination)) {
      return Failure("The destination path already exists");
    }
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
