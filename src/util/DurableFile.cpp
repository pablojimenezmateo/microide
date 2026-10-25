#include "util/DurableFile.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <string>
#include <system_error>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace microide::util {
namespace {

int OpenForBinaryWrite(const std::filesystem::path& path) {
  // O_CLOEXEC: a concurrent fork+exec (terminal shell, git, LSP/DAP adapter) must not
  // inherit a live writable handle to this durable-write staging file and pin it open.
  return ::open(path.c_str(), O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC, 0644);
}

bool WriteAll(int fd, std::span<const std::byte> bytes) {
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    const std::size_t remaining = bytes.size() - offset;
    const std::size_t chunk = std::min<std::size_t>(remaining, static_cast<std::size_t>(1u << 20));
    const ssize_t wrote = ::write(fd, bytes.data() + static_cast<std::ptrdiff_t>(offset), chunk);
    if (wrote < 0 && errno == EINTR) {
      continue;  // interrupted by a signal before any byte was written; retry
    }
    if (wrote <= 0) {
      return false;
    }
    offset += static_cast<std::size_t>(wrote);
  }
  return true;
}

bool FsyncFile(int fd) {
  return fsync(fd) == 0;
}

void CloseFile(int fd) {
  close(fd);
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
  // rename already atomically replaces a regular-file destination, so a failure is
  // something other than "destination exists" (a directory, a cross-device link, a
  // permission or I/O error, an invalid source). Removing `to` cannot help any of
  // those and could silently delete a valid directory or file, so the failure is
  // surfaced instead.
  std::error_code error;
  std::filesystem::rename(from, to, error);
  return !error;
}

std::filesystem::path UniqueTemporaryPath(const std::filesystem::path& path) {
  static std::atomic<std::uint64_t> counter{0};
  const std::uint64_t seq = counter.fetch_add(1, std::memory_order_relaxed);
  const long long pid = static_cast<long long>(::getpid());
  // Leading dot + a distinctive marker. The staging file lives INSIDE the project
  // tree for the duration of a save, so anything that walks the tree can observe
  // it: the dot makes it hidden (the file index already excludes hidden paths) and
  // the marker lets IsTemporaryStagingFilename recognise it even when the user has
  // asked to include hidden files.
  std::string name(".");
  name += path.filename().string();
  name += kTemporaryStagingMarker;
  name += std::to_string(pid);
  name += '.';
  name += std::to_string(seq);
  return path.parent_path() / name;
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
  // Ownership first: chown can strip setuid/setgid, so restore mode afterwards. Both
  // are best-effort — a non-root save of a file it does not own keeps whatever the
  // fresh temp got, which is strictly no worse than today's unconditional 0644.
  const bool chown_ok = ::chown(path.c_str(), static_cast<uid_t>(permissions.uid),
                                static_cast<gid_t>(permissions.gid)) == 0;
  mode_t mode = static_cast<mode_t>(permissions.mode);
  if (!chown_ok) {
    // EPERM for an unprivileged user is expected and non-fatal, but ownership was NOT
    // restored — the file is now owned by the saving user. Re-applying setuid/setgid
    // in that state would create a set-id file under the WRONG owner, effectively
    // granting the saver's identity elevated execution; that is worse than dropping
    // the bits. Strip S_ISUID/S_ISGID so an ownership-restore failure can never
    // manufacture a set-id binary. The remaining permission bits are still restored.
    //
    // util/ has no logging seam reachable here and this entry point's signature is
    // fixed (its header is a shared contract), so this hardening is silent by
    // necessity; the observable guarantee is "no setuid/setgid survives a failed
    // ownership restore." (See bug inventory C7.)
    mode &= ~static_cast<mode_t>(S_ISUID | S_ISGID);
  }
  ::chmod(path.c_str(), mode);
}

}  // namespace microide::util
