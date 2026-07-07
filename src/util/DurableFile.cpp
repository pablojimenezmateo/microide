#include "util/DurableFile.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <string>
#include <system_error>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#include <process.h>
#include <sys/stat.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace microide::util {
namespace {

int OpenForBinaryWrite(const std::filesystem::path& path) {
#if defined(_WIN32)
  return _wopen(path.c_str(), _O_BINARY | _O_CREAT | _O_TRUNC | _O_WRONLY,
                _S_IREAD | _S_IWRITE);
#else
  return ::open(path.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0644);
#endif
}

bool WriteAll(int fd, std::span<const std::byte> bytes) {
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    const std::size_t remaining = bytes.size() - offset;
    const std::size_t chunk = std::min<std::size_t>(remaining, static_cast<std::size_t>(1u << 20));
#if defined(_WIN32)
    const int wrote = _write(fd, bytes.data() + static_cast<std::ptrdiff_t>(offset),
                             static_cast<unsigned int>(chunk));
#else
    const ssize_t wrote = ::write(fd, bytes.data() + static_cast<std::ptrdiff_t>(offset), chunk);
#endif
    if (wrote <= 0) {
      return false;
    }
    offset += static_cast<std::size_t>(wrote);
  }
  return true;
}

bool FsyncFile(int fd) {
#if defined(_WIN32)
  return _commit(fd) == 0;
#else
  return fsync(fd) == 0;
#endif
}

void CloseFile(int fd) {
#if defined(_WIN32)
  _close(fd);
#else
  close(fd);
#endif
}

}  // namespace

bool WriteFileBytesDurable(const std::filesystem::path& path, std::span<const std::byte> bytes) {
  const int fd = OpenForBinaryWrite(path);
  if (fd < 0) {
    return false;
  }
  const bool write_ok = WriteAll(fd, bytes);
  const bool flush_ok = write_ok && FsyncFile(fd);
  CloseFile(fd);
  return write_ok && flush_ok;
}

bool RenameReplacing(const std::filesystem::path& from, const std::filesystem::path& to) {
  std::error_code error;
  std::filesystem::rename(from, to, error);
  if (!error) {
    return true;
  }

  std::filesystem::remove(to, error);
  error.clear();
  std::filesystem::rename(from, to, error);
  return !error;
}

std::filesystem::path UniqueTemporaryPath(const std::filesystem::path& path) {
  static std::atomic<std::uint64_t> counter{0};
  const std::uint64_t seq = counter.fetch_add(1, std::memory_order_relaxed);
#if defined(_WIN32)
  const long long pid = static_cast<long long>(_getpid());
#else
  const long long pid = static_cast<long long>(::getpid());
#endif
  std::string suffix = ".tmp.";
  suffix += std::to_string(pid);
  suffix += '.';
  suffix += std::to_string(seq);
  return path.string() + suffix;
}

FilePermissions CaptureFilePermissions(const std::filesystem::path& path) {
#if defined(_WIN32)
  (void)path;
  return FilePermissions{};
#else
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
#endif
}

void ApplyFilePermissions(const std::filesystem::path& path, const FilePermissions& permissions) {
  if (!permissions.valid) {
    return;
  }
#if defined(_WIN32)
  (void)path;
#else
  // Ownership first: chown can strip setuid/setgid, so restore mode afterwards. Both
  // are best-effort — a non-root save of a file it does not own keeps whatever the
  // fresh temp got, which is strictly no worse than today's unconditional 0644.
  if (::chown(path.c_str(), static_cast<uid_t>(permissions.uid),
              static_cast<gid_t>(permissions.gid)) != 0) {
    // Ignore: EPERM for an unprivileged user is expected and non-fatal.
  }
  ::chmod(path.c_str(), static_cast<mode_t>(permissions.mode));
#endif
}

}  // namespace microide::util
