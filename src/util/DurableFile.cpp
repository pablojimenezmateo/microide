#include "util/DurableFile.h"

#include "util/CrashPoint.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cerrno>
#include <cstdint>
#include <limits>
#include <string>
#include <system_error>

#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

namespace microide::util {
namespace {

int OpenForBinaryWrite(const std::filesystem::path& path) {
  return ::open(path.c_str(), O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC, 0644);
}

std::error_code LastError() { return std::error_code(errno != 0 ? errno : EIO, std::generic_category()); }

bool WriteAll(int fd, std::span<const std::byte> bytes) {
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    const std::size_t remaining = bytes.size() - offset;
    const std::size_t chunk = std::min<std::size_t>(remaining, static_cast<std::size_t>(1u << 20));
    const ssize_t wrote = ::write(fd, bytes.data() + static_cast<std::ptrdiff_t>(offset), chunk);
    if (wrote < 0 && errno == EINTR) {
      continue;
    }
    if (wrote <= 0) {
      if (wrote == 0) {
        errno = EIO;
      }
      return false;
    }
    offset += static_cast<std::size_t>(wrote);
  }
  return true;
}

bool FsyncFile(int fd) {
  return fsync(fd) == 0;
}

bool CloseFile(int fd) {
  return close(fd) == 0;
}

}

bool WriteFileBytesDurable(const std::filesystem::path& path, std::span<const std::byte> bytes,
                           std::error_code* error) {
  const auto fail = [error] {
    if (error != nullptr) {
      *error = LastError();
    }
    return false;
  };
  const int fd = OpenForBinaryWrite(path);
  if (fd < 0) {
    return fail();
  }
  if (!WriteAll(fd, bytes) || !FsyncFile(fd)) {
    const std::error_code why = LastError();
    (void)CloseFile(fd);
    if (error != nullptr) {
      *error = why;
    }
    return false;
  }
  if (!CloseFile(fd)) {
    return fail();
  }
  CrashPoint("durable.written", path.native());
  return true;
}

bool RenameReplacing(const std::filesystem::path& from, const std::filesystem::path& to,
                     std::error_code* error_out) {
  std::error_code error;
  CrashPoint("durable.renaming-source", from.native());
  CrashPoint("durable.renaming", to.native());
  std::filesystem::rename(from, to, error);
  if (error && error_out != nullptr) {
    *error_out = error;
  }
  if (!error) {
    CrashPoint("durable.renamed", to.native());
  }
  return !error;
}

bool RenameNoReplace(const std::filesystem::path& from, const std::filesystem::path& to,
                     std::error_code* error_out) {
  const auto fail = [error_out](int code) {
    if (error_out != nullptr) {
      *error_out = std::error_code(code, std::generic_category());
    }
    return false;
  };
  CrashPoint("durable.renaming-source", from.native());
  CrashPoint("durable.renaming", to.native());
#if defined(RENAME_NOREPLACE)
  if (::renameat2(AT_FDCWD, from.c_str(), AT_FDCWD, to.c_str(), RENAME_NOREPLACE) == 0) {
    CrashPoint("durable.renamed", to.native());
    return true;
  }
  if (errno != EINVAL && errno != ENOSYS) {
    return fail(errno);
  }
#endif
  if (::linkat(AT_FDCWD, from.c_str(), AT_FDCWD, to.c_str(), 0) != 0) {
    return fail(errno);
  }
  ::unlink(from.c_str());
  CrashPoint("durable.renamed", to.native());
  return true;
}

bool FsyncDirectory(const std::filesystem::path& directory) {
  const int fd = ::open(directory.empty() ? "." : directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (fd < 0) {
    return false;
  }
  const bool synced = ::fsync(fd) == 0;
  (void)CloseFile(fd);
  CrashPoint("durable.directory-synced", directory.native());
  return synced;
}

std::filesystem::path UniqueTemporaryPath(const std::filesystem::path& path) {
  static std::atomic<std::uint64_t> counter{0};
  const std::uint64_t seq = counter.fetch_add(1, std::memory_order_relaxed);
  const long long pid = static_cast<long long>(::getpid());
  std::string name(".");
  name += path.filename().string();
  name += kTemporaryStagingMarker;
  name += std::to_string(pid);
  name += '.';
  name += std::to_string(seq);
  return path.parent_path() / name;
}

bool TemporaryStagingIsAbandoned(std::string_view filename) {
  if (!IsTemporaryStagingFilename(filename)) {
    return false;
  }
  const std::size_t marker = filename.rfind(kTemporaryStagingMarker);
  std::string_view rest = filename.substr(marker + kTemporaryStagingMarker.size());
  const std::size_t dot = rest.find('.');
  if (dot == std::string_view::npos || dot == 0 || dot > 18) {
    return false;
  }
  long long pid = 0;
  for (const char c : rest.substr(0, dot)) {
    if (c < '0' || c > '9') {
      return false;
    }
    pid = pid * 10 + (c - '0');
  }
  if (pid <= 0 || pid > std::numeric_limits<pid_t>::max()) {
    return true;
  }
  return static_cast<pid_t>(pid) != ::getpid() && ::kill(static_cast<pid_t>(pid), 0) != 0 && errno == ESRCH;
}

bool SweepAbandonedTemporaryStaging(std::string_view absolute_path, std::chrono::seconds min_age) {
  const std::size_t slash = absolute_path.rfind('/');
  const std::string_view name = slash == std::string_view::npos ? absolute_path : absolute_path.substr(slash + 1);
  if (name.empty() || name.front() != '.' || !TemporaryStagingIsAbandoned(name)) {
    return false;
  }
  const std::string path(absolute_path);
  struct stat info {};
  if (::lstat(path.c_str(), &info) != 0 || !S_ISREG(info.st_mode)) {
    return false;
  }
  const auto modified = std::chrono::system_clock::time_point(std::chrono::seconds(info.st_mtim.tv_sec));
  if (std::chrono::system_clock::now() - modified < min_age) {
    return false;
  }
  return ::unlink(path.c_str()) == 0;
}

bool IsTemporaryStagingFilename(std::string_view filename) {
  return filename.starts_with('.') &&
         filename.find(kTemporaryStagingMarker) != std::string_view::npos;
}

FilePermissions CaptureFilePermissions(const std::filesystem::path& path) {
  struct stat status{};
  if (::stat(path.c_str(), &status) != 0) {
    return FilePermissions{};
  }
  return FilePermissions{
      .valid = true,
      .mode = static_cast<std::uint32_t>(status.st_mode & 07777),
      .uid = static_cast<std::uint32_t>(status.st_uid),
      .gid = static_cast<std::uint32_t>(status.st_gid),
  };
}

void ApplyFilePermissions(const std::filesystem::path& path, const FilePermissions& permissions) {
  if (!permissions.valid) {
    return;
  }
  const bool chown_ok = ::chown(path.c_str(), static_cast<uid_t>(permissions.uid),
                                static_cast<gid_t>(permissions.gid)) == 0;
  mode_t mode = static_cast<mode_t>(permissions.mode);
  if (!chown_ok) {
    mode &= ~static_cast<mode_t>(S_ISUID | S_ISGID);
  }
  ::chmod(path.c_str(), mode);
}

}
