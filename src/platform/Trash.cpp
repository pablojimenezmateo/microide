#include "platform/Trash.h"

#include "platform/AppDirectories.h"
#include "platform/FsOps.h"

#include <chrono>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <system_error>

#if defined(_WIN32)
#include <windows.h>
#include <shellapi.h>
#endif

namespace microide::platform {

namespace {

TrashOperationResult Failure(std::string message) {
  return TrashOperationResult{
      .ok = false,
      .resulting_path = {},
      .error_message = std::move(message),
  };
}

TrashOperationResult Success(const std::filesystem::path& path) {
  return TrashOperationResult{
      .ok = true,
      .resulting_path = path,
      .error_message = {},
  };
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

TrashOperationResult MovePathToTrashLinux(const std::filesystem::path& source) {
  const std::filesystem::path data_home = ResolveUserDirectory(UserDirectoryKind::Data);
  if (data_home.empty()) {
    return Failure("Could not resolve the home directory for trash");
  }

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
TrashOperationResult MovePathToTrashMac(const std::filesystem::path& source) {
  const std::filesystem::path home = ResolveUserHomeDirectory();
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

#if defined(_WIN32)
TrashOperationResult MovePathToTrashWindows(const std::filesystem::path& source) {
  std::wstring from = source.wstring();
  from.push_back(L'\0');

  SHFILEOPSTRUCTW operation{};
  operation.wFunc = FO_DELETE;
  operation.pFrom = from.c_str();
  operation.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI;
  const int result = SHFileOperationW(&operation);
  if (result != 0 || operation.fAnyOperationsAborted) {
    return Failure("Failed to move the path to the recycle bin");
  }
  return Success(source);
}
#endif

}  // namespace

TrashOperationResult MovePathToTrash(const std::filesystem::path& path) {
  const std::filesystem::path normalized_path = NormalizeAbsolutePath(path);
  if (normalized_path.empty()) {
    return Failure("No path was provided");
  }

  std::error_code error;
  if (!std::filesystem::exists(normalized_path, error) || error) {
    return Failure("The path does not exist");
  }

#if defined(_WIN32)
  return MovePathToTrashWindows(normalized_path);
#elif defined(__APPLE__)
  return MovePathToTrashMac(normalized_path);
#else
  return MovePathToTrashLinux(normalized_path);
#endif
}

}  // namespace microide::platform
