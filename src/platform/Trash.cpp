#include "platform/Trash.h"

#include "platform/AppDirectories.h"
#include "platform/FsOps.h"

#include <chrono>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <system_error>

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>

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

std::string FormatDeletionTimestamp() {
  const std::time_t now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
  std::tm local_time{};
  localtime_r(&now, &local_time);
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

  const std::string base_name =
      source.filename().string().empty() ? "item" : source.filename().string();
  const std::filesystem::path base_path(base_name);
  const std::string stem = base_path.stem().string();
  const std::string extension = base_path.extension().string();
  const std::string fallback_stem = stem.empty() ? base_name : stem;

  for (int attempt = 1; attempt < 10000; ++attempt) {
    const std::string candidate_name =
        (attempt == 1) ? base_name
                       : (fallback_stem + " " + std::to_string(attempt) + extension);
    std::error_code exists_error;
    if (std::filesystem::exists(trash_files / candidate_name, exists_error)) {
      continue;
    }
    const std::filesystem::path candidate_info = trash_info / (candidate_name + ".trashinfo");
    const int fd = ::open(candidate_info.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0) {
      if (errno == EEXIST) {
        continue;
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

    const std::filesystem::path trashed_path = trash_files / candidate_name;
    if (MovePathNoOverwrite(source, trashed_path)) {
      return Success(trashed_path);
    }
    std::error_code dest_status_error;
    const std::filesystem::file_status dest_status =
        std::filesystem::symlink_status(trashed_path, dest_status_error);
    if (dest_status.type() != std::filesystem::file_type::not_found) {
      std::filesystem::remove(candidate_info, error);
      continue;
    }
    std::filesystem::remove(candidate_info, error);
    return Failure("Failed to move the path to trash");
  }
  return Failure("Failed to reserve a trash slot");
}

}

TrashOperationResult MovePathToTrash(const std::filesystem::path& path) {
  const std::filesystem::path normalized_path = NormalizeAbsolutePath(path);
  if (normalized_path.empty()) {
    return Failure("No path was provided");
  }

  std::error_code error;
  const std::filesystem::file_status link_status =
      std::filesystem::symlink_status(normalized_path, error);
  if (error || link_status.type() == std::filesystem::file_type::not_found ||
      link_status.type() == std::filesystem::file_type::none) {
    return Failure("The path does not exist");
  }

  return MovePathToTrashLinux(normalized_path);
}

}
