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
#elif !defined(__APPLE__)
#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
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

// The freedesktop trash spec requires the `Path` value to be stored as a URI:
// percent-encoded per RFC 2396, with '/' left unescaped. Encoding it also makes
// the single-line `Path=...` field robust against filenames containing newlines
// (legal on Linux), which would otherwise inject arbitrary lines into the
// .trashinfo file and corrupt DeletionDate or spoof fields.
[[maybe_unused]] std::string PercentEncodeTrashPath(const std::string& path) {
  static constexpr char kHex[] = "0123456789ABCDEF";
  std::string out;
  out.reserve(path.size());
  for (const unsigned char c : path) {
    const bool unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' ||
                            c == '~' || c == '/';
    if (unreserved) {
      out.push_back(static_cast<char>(c));
    } else {
      out.push_back('%');
      out.push_back(kHex[c >> 4]);
      out.push_back(kHex[c & 0x0F]);
    }
  }
  return out;
}

#if defined(__APPLE__)
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
#endif

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

#if !defined(_WIN32) && !defined(__APPLE__)
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

  const std::string base_name =
      source.filename().string().empty() ? "item" : source.filename().string();
  const std::filesystem::path base_path(base_name);
  const std::string stem = base_path.stem().string();
  const std::string extension = base_path.extension().string();
  const std::string fallback_stem = stem.empty() ? base_name : stem;

  // Reserve the `.trashinfo` name atomically with O_EXCL BEFORE writing metadata
  // or moving the file. Per the freedesktop trash spec the .trashinfo file is the
  // reservation: creating it exclusively is the atomic gate that makes two
  // concurrent trashers of same-named files pick distinct slots. The prior
  // exists()-check + truncating ofstream was a TOCTOU that let both racers land on
  // the same name and silently overwrite each other's file and metadata.
  for (int attempt = 1; attempt < 10000; ++attempt) {
    const std::string candidate_name =
        (attempt == 1) ? base_name
                       : (fallback_stem + " " + std::to_string(attempt) + extension);
    std::error_code exists_error;
    if (std::filesystem::exists(trash_files / candidate_name, exists_error)) {
      continue;  // A leftover file occupies the slot (best-effort); try the next.
    }
    const std::filesystem::path candidate_info = trash_info / (candidate_name + ".trashinfo");
    const int fd = ::open(candidate_info.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0) {
      if (errno == EEXIST) {
        continue;  // Name reserved by a concurrent trasher — the atomic gate fired.
      }
      return Failure("Failed to write trash metadata");
    }

    const std::string contents = std::string("[Trash Info]\n") + "Path=" +
                                 PercentEncodeTrashPath(source.generic_string()) + "\n" +
                                 "DeletionDate=" + FormatDeletionTimestamp() + "\n";
    const char* cursor = contents.data();
    std::size_t remaining = contents.size();
    bool write_ok = true;
    while (remaining > 0) {
      const ssize_t written = ::write(fd, cursor, remaining);
      if (written < 0) {
        if (errno == EINTR) {
          continue;
        }
        write_ok = false;
        break;
      }
      cursor += written;
      remaining -= static_cast<std::size_t>(written);
    }
    ::close(fd);
    if (!write_ok) {
      std::filesystem::remove(candidate_info, error);
      return Failure("Failed to write trash metadata");
    }

    // Final content move must be NO-OVERWRITE: the reservation only gated the
    // `.trashinfo` slot, so a file can still appear at Trash/files/<name> between the
    // exists() check above and here (TOCTOU). An overwrite rename would clobber it.
    // If the slot is now taken, drop our reserved metadata and try the next suffix
    // instead of overwriting or failing the whole delete. (TD-2026-07-16-49.)
    const std::filesystem::path trashed_path = trash_files / candidate_name;
    if (MovePathNoOverwrite(source, trashed_path)) {
      return Success(trashed_path);
    }
    std::error_code dest_exists_error;
    if (std::filesystem::exists(trashed_path, dest_exists_error)) {
      std::filesystem::remove(candidate_info, error);
      continue;  // Slot taken during the window — reserve the next one.
    }
    // The move failed for another reason (source gone, permission, cross-device copy
    // failure with rollback): clean up the reserved metadata and report failure.
    std::filesystem::remove(candidate_info, error);
    return Failure("Failed to move the path to trash");
  }
  return Failure("Failed to reserve a trash slot");
}
#endif

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
