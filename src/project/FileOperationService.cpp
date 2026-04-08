#include "project/FileOperationService.h"

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <sstream>
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

std::filesystem::path NormalizeAbsolutePath(const std::filesystem::path& path) {
  std::error_code error;
  const std::filesystem::path absolute = std::filesystem::absolute(path, error);
  return (error ? path : absolute).lexically_normal();
}

bool IsReservedPathComponent(const std::filesystem::path& path) {
  for (const auto& component : path) {
    const std::string name = component.string();
    if (name == "." || name == "..") {
      return true;
    }
  }
  return false;
}

std::filesystem::path UniquePathInDirectory(const std::filesystem::path& directory,
                                            const std::string& base_name) {
  const std::filesystem::path desired = directory / base_name;
  std::error_code error;
  if (!std::filesystem::exists(desired, error)) {
    return desired;
  }

  const std::filesystem::path base_path(base_name);
  const std::string stem = base_path.stem().string();
  const std::string extension = base_path.extension().string();
  const std::string fallback_stem = stem.empty() ? base_name : stem;
  for (int attempt = 2; attempt < 10000; ++attempt) {
    const std::string candidate_name =
        fallback_stem + " " + std::to_string(attempt) + extension;
    const std::filesystem::path candidate = directory / candidate_name;
    if (!std::filesystem::exists(candidate, error)) {
      return candidate;
    }
  }
  return desired;
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

std::filesystem::path UserHomeDirectory() {
  if (const char* home = std::getenv("HOME"); home != nullptr && home[0] != '\0') {
    return std::filesystem::path(home);
  }
#if defined(_WIN32)
  if (const char* profile = std::getenv("USERPROFILE");
      profile != nullptr && profile[0] != '\0') {
    return std::filesystem::path(profile);
  }
  const char* drive = std::getenv("HOMEDRIVE");
  const char* path = std::getenv("HOMEPATH");
  if (drive != nullptr && drive[0] != '\0' && path != nullptr && path[0] != '\0') {
    return std::filesystem::path(std::string(drive) + std::string(path));
  }
#endif
  return {};
}

std::string FormatDeletionTimestamp() {
  const std::time_t now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
  std::tm local_time{};
#if defined(_WIN32)
  localtime_s(&local_time, &now);
#else
  localtime_r(&now, &local_time);
#endif
  std::ostringstream stream;
  stream << std::put_time(&local_time, "%Y-%m-%dT%H:%M:%S");
  return stream.str();
}

FileOperationResult TrashPathLinux(const std::filesystem::path& source) {
  const std::filesystem::path home = UserHomeDirectory();
  if (home.empty()) {
    return Failure("Could not resolve the home directory for trash");
  }

  const char* xdg_data_home = std::getenv("XDG_DATA_HOME");
  const std::filesystem::path data_home =
      xdg_data_home != nullptr && xdg_data_home[0] != '\0'
          ? std::filesystem::path(xdg_data_home)
          : home / ".local" / "share";
  const std::filesystem::path trash_root = data_home / "Trash";
  const std::filesystem::path trash_files = trash_root / "files";
  const std::filesystem::path trash_info = trash_root / "info";

  std::error_code error;
  std::filesystem::create_directories(trash_files, error);
  if (error) {
    return Failure("Failed to prepare the trash directory");
  }
  std::filesystem::create_directories(trash_info, error);
  if (error) {
    return Failure("Failed to prepare trash metadata");
  }

  const std::string base_name = source.filename().string().empty() ? "item" : source.filename().string();
  const std::filesystem::path trashed_path = UniquePathInDirectory(trash_files, base_name);
  const std::string trashed_name = trashed_path.filename().string();
  const std::filesystem::path info_path = trash_info / (trashed_name + ".trashinfo");

  std::ofstream info_stream(info_path, std::ios::binary | std::ios::trunc);
  if (!info_stream) {
    return Failure("Failed to write trash metadata");
  }
  info_stream << "[Trash Info]\n";
  info_stream << "Path=" << source.generic_string() << "\n";
  info_stream << "DeletionDate=" << FormatDeletionTimestamp() << "\n";
  if (!info_stream.good()) {
    return Failure("Failed to write trash metadata");
  }

  if (!MovePath(source, trashed_path)) {
    std::filesystem::remove(info_path, error);
    return Failure("Failed to move the path to trash");
  }

  return Success(trashed_path);
}

#if defined(__APPLE__)
FileOperationResult TrashPathMac(const std::filesystem::path& source) {
  const std::filesystem::path home = UserHomeDirectory();
  if (home.empty()) {
    return Failure("Could not resolve the home directory for trash");
  }

  const std::filesystem::path trash_root = home / ".Trash";
  std::error_code error;
  std::filesystem::create_directories(trash_root, error);
  if (error) {
    return Failure("Failed to prepare the trash directory");
  }

  const std::string base_name = source.filename().string().empty() ? "item" : source.filename().string();
  const std::filesystem::path trashed_path = UniquePathInDirectory(trash_root, base_name);
  if (!MovePath(source, trashed_path)) {
    return Failure("Failed to move the path to trash");
  }
  return Success(trashed_path);
}
#endif

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
  const std::filesystem::path normalized_path = NormalizeAbsolutePath(path);
  if (normalized_path.empty()) {
    return Failure("No path was provided");
  }

  std::error_code error;
  if (!std::filesystem::exists(normalized_path, error)) {
    return Failure("The path does not exist");
  }

#if defined(__APPLE__)
  return TrashPathMac(normalized_path);
#elif defined(__linux__)
  return TrashPathLinux(normalized_path);
#else
  return Failure("Trash is not implemented on this platform yet");
#endif
}

}  // namespace microide::project
