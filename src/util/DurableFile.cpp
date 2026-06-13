#include "util/DurableFile.h"

#include <algorithm>
#include <system_error>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
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

}  // namespace microide::util
