#include "persistence/PersistedRecordWriter.h"

#include "persistence/PersistedRecord.h"
#include "util/DurableFile.h"

#include <cstddef>
#include <system_error>
#include <vector>

namespace microide::persistence {
namespace {

using util::RenameReplacing;
using util::WriteFileBytesDurable;

void SetError(PersistedRecordWriterError* error, PersistedRecordWriterError value) {
  if (error != nullptr) {
    *error = value;
  }
}

std::filesystem::path TemporaryPathFor(const std::filesystem::path& path) {
  return path.string() + ".tmp";
}

bool FileExists(const std::filesystem::path& path) {
  std::error_code error;
  const bool exists = std::filesystem::exists(path, error);
  return exists && !error;
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

  if (!WriteFileBytesDurable(temp_path, file_bytes)) {
    std::filesystem::remove(temp_path, fs_error);
    SetError(error, PersistedRecordWriterError::WriteFailed);
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
