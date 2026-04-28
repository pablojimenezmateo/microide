#include "persistence/PersistedRecordWriter.h"

#include "persistence/PersistedRecord.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <system_error>
#include <vector>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace microide::persistence {
namespace {

void SetError(PersistedRecordWriterError* error, PersistedRecordWriterError value) {
  if (error != nullptr) {
    *error = value;
  }
}

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

bool FlushFile(int fd) {
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

std::filesystem::path TemporaryPathFor(const std::filesystem::path& path) {
  return path.string() + ".tmp";
}

bool FileExists(const std::filesystem::path& path) {
  std::error_code error;
  const bool exists = std::filesystem::exists(path, error);
  return exists && !error;
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

}  // namespace

std::filesystem::path PersistedRecordWriter::BackupPathFor(const std::filesystem::path& path) {
  return path.string() + ".bak";
}

bool PersistedRecordWriter::WriteFile(const std::filesystem::path& path,
                                      std::span<const std::byte> body,
                                      std::uint32_t capability_flags,
                                      PersistedRecordWriterError* error) {
  SetError(error, PersistedRecordWriterError::None);
  if (path.empty()) {
    SetError(error, PersistedRecordWriterError::InvalidPath);
    return false;
  }

  const std::filesystem::path parent = path.parent_path();
  if (!parent.empty()) {
    std::error_code mkdir_error;
    std::filesystem::create_directories(parent, mkdir_error);
    if (mkdir_error) {
      SetError(error, PersistedRecordWriterError::CreateDirectoryFailed);
      return false;
    }
  }

  std::vector<std::byte> file_bytes;
  if (!BuildPersistedRecordFile(body, capability_flags, &file_bytes)) {
    SetError(error, PersistedRecordWriterError::EncodeFailed);
    return false;
  }

  const std::filesystem::path temp_path = TemporaryPathFor(path);
  const std::filesystem::path backup_path = BackupPathFor(path);
  std::error_code fs_error;
  std::filesystem::remove(temp_path, fs_error);
  fs_error.clear();

  const int fd = OpenForBinaryWrite(temp_path);
  if (fd < 0) {
    SetError(error, PersistedRecordWriterError::OpenFailed);
    return false;
  }

  bool write_ok = WriteAll(fd, file_bytes);
  bool flush_ok = write_ok && FlushFile(fd);
  CloseFile(fd);
  if (!write_ok || !flush_ok) {
    std::filesystem::remove(temp_path, fs_error);
    SetError(error, write_ok ? PersistedRecordWriterError::FlushFailed
                             : PersistedRecordWriterError::WriteFailed);
    return false;
  }

  const bool destination_exists = FileExists(path);
  if (destination_exists) {
    std::filesystem::remove(backup_path, fs_error);
    fs_error.clear();
    if (!RenameReplacing(path, backup_path)) {
      std::filesystem::remove(temp_path, fs_error);
      SetError(error, PersistedRecordWriterError::BackupFailed);
      return false;
    }
  }

  if (!RenameReplacing(temp_path, path)) {
    if (destination_exists && !FileExists(path) && FileExists(backup_path)) {
      RenameReplacing(backup_path, path);
    }
    std::filesystem::remove(temp_path, fs_error);
    SetError(error, PersistedRecordWriterError::RenameFailed);
    return false;
  }

  return true;
}

}  // namespace microide::persistence
