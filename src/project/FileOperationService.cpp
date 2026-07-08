#include "project/FileOperationService.h"

#include "platform/FsOps.h"
#include "platform/Trash.h"

#include <fstream>
#include <system_error>

namespace microide::project {

namespace {

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

  std::error_code error;
  if (std::filesystem::exists(normalized_path, error)) {
    return Failure("The file already exists");
  }

  const std::filesystem::path parent = normalized_path.parent_path();
  if (parent.empty()) {
    return Failure("The file path has no parent directory");
  }
  std::filesystem::create_directories(parent, error);
  if (error) {
    return Failure("Failed to create the parent directory");
  }

  std::ofstream stream(normalized_path, std::ios::binary | std::ios::trunc);
  if (!stream) {
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
