#include "persistence/PersistedRecordWriter.h"

#include "persistence/PersistedRecord.h"
#include "util/DurableFile.h"
#include "util/PerformanceCounters.h"
#include "util/PerformanceTrace.h"
#include "util/TextFileIO.h"

#include <cstddef>
#include <cstdint>
#include <string>
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

bool FileExists(const std::filesystem::path& path) {
  std::error_code error;
  const bool exists = std::filesystem::exists(path, error);
  return exists && !error;
}

void SetCause(std::error_code* cause, const std::error_code& value) {
  if (cause != nullptr) {
    *cause = value;
  }
}

thread_local std::uint64_t t_writes_on_this_thread = 0;

}

const char* PersistedRecordWriterErrorName(PersistedRecordWriterError error) {
  switch (error) {
    case PersistedRecordWriterError::None: return "none";
    case PersistedRecordWriterError::InvalidPath: return "invalid-path";
    case PersistedRecordWriterError::CreateDirectoryFailed: return "create-directory";
    case PersistedRecordWriterError::EncodeFailed: return "encode";
    case PersistedRecordWriterError::OpenFailed: return "open";
    case PersistedRecordWriterError::WriteFailed: return "write";
    case PersistedRecordWriterError::FlushFailed: return "flush";
    case PersistedRecordWriterError::BackupFailed: return "backup";
    case PersistedRecordWriterError::RenameFailed: return "rename";
  }
  return "unknown";
}

std::uint64_t PersistedRecordWriter::WritesOnThisThread() { return t_writes_on_this_thread; }

std::filesystem::path PersistedRecordWriter::BackupPathFor(const std::filesystem::path& path) {
  return path.string() + ".bak";
}

bool PersistedRecordWriter::WriteFile(const std::filesystem::path& path,
                                      std::span<const std::byte> body,
                                      std::uint32_t capability_flags,
                                      PersistedRecordWriterError* error,
                                      std::error_code* cause) {
  util::PerformanceTrace::ScopeLabel perf_label("persistence::WriteFile");
  perf_label.Field("file", path.stem().native());
  util::PerformanceTrace::Scope perf_scope(perf_label.View());
  util::AddPerformanceCounter(util::PerfCounterId::PersistenceRecordWrites);
  ++t_writes_on_this_thread;
  util::AddPerformanceCounter(util::PerfCounterId::PersistenceRecordBytesWritten, body.size());

  SetError(error, PersistedRecordWriterError::None);
  SetCause(cause, {});
  if (path.empty()) {
    SetError(error, PersistedRecordWriterError::InvalidPath);
    return false;
  }

  std::filesystem::path target;
  {
    util::PerformanceTrace::Scope scope("persistence::WriteFile::ResolveAndMakeDirs");
    target = util::ResolveSymlinkTarget(path);
    const std::filesystem::path parent = target.parent_path();
    if (!parent.empty()) {
      std::error_code mkdir_error;
      std::filesystem::create_directories(parent, mkdir_error);
      if (mkdir_error) {
        SetCause(cause, mkdir_error);
        SetError(error, PersistedRecordWriterError::CreateDirectoryFailed);
        return false;
      }
    }
  }

  std::vector<std::byte> file_bytes;
  if (!BuildPersistedRecordFile(body, capability_flags, &file_bytes)) {
    SetError(error, PersistedRecordWriterError::EncodeFailed);
    return false;
  }

  const std::filesystem::path temp_path = util::UniqueTemporaryPath(target);
  const std::filesystem::path backup_path = BackupPathFor(target);
  std::error_code fs_error;
  std::filesystem::remove(temp_path, fs_error);
  fs_error.clear();

  {
    util::PerformanceTrace::Scope scope("persistence::WriteFile::DurableWrite");
    std::error_code write_error;
    if (!WriteFileBytesDurable(temp_path, file_bytes, &write_error)) {
      SetCause(cause, write_error);
      std::filesystem::remove(temp_path, fs_error);
      SetError(error, PersistedRecordWriterError::WriteFailed);
      return false;
    }
  }

  util::PerformanceTrace::Scope rotate_scope("persistence::WriteFile::RotateAndRename");
  const bool destination_exists = FileExists(target);
  if (destination_exists) {
    std::filesystem::remove(backup_path, fs_error);
    fs_error.clear();
    std::error_code backup_error;
    if (!RenameReplacing(target, backup_path, &backup_error)) {
      SetCause(cause, backup_error);
      std::filesystem::remove(temp_path, fs_error);
      SetError(error, PersistedRecordWriterError::BackupFailed);
      return false;
    }
  }

  std::error_code rename_error;
  if (!RenameReplacing(temp_path, target, &rename_error)) {
    SetCause(cause, rename_error);
    if (destination_exists && !FileExists(target) && FileExists(backup_path)) {
      RenameReplacing(backup_path, target);
    }
    std::filesystem::remove(temp_path, fs_error);
    SetError(error, PersistedRecordWriterError::RenameFailed);
    return false;
  }
  (void)util::FsyncDirectory(target.parent_path());

  return true;
}

}
