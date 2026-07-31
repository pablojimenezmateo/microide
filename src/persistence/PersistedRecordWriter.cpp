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

}  // namespace

std::filesystem::path PersistedRecordWriter::BackupPathFor(const std::filesystem::path& path) {
  return path.string() + ".bak";
}

bool PersistedRecordWriter::WriteFile(const std::filesystem::path& path,
                                      std::span<const std::byte> body,
                                      std::uint32_t capability_flags,
                                      PersistedRecordWriterError* error) {
  // Session/config saves run on the shell thread at project switch and shutdown,
  // where a slow durable write shows up to the user as a stall on close. Nothing
  // measured them before.
  // Labelled by file stem: the store holds a handful of well-known files (project
  // state, user config, session), so this ranks "which state file is being
  // rewritten, how often" instead of collapsing every save into one row. Without
  // it a hot row here says only that something is writing.
  util::PerformanceTrace::ScopeLabel perf_label("persistence::WriteFile");
  perf_label.Field("file", path.stem().native());
  util::PerformanceTrace::Scope perf_scope(perf_label.View());
  util::AddPerformanceCounter(util::PerfCounterId::PersistenceRecordWrites);
  util::AddPerformanceCounter(util::PerfCounterId::PersistenceRecordBytesWritten, body.size());

  SetError(error, PersistedRecordWriterError::None);
  if (path.empty()) {
    SetError(error, PersistedRecordWriterError::InvalidPath);
    return false;
  }

  // Rotate through the symlink's target, not the link node. POSIX rename moves the
  // link node itself, so backing up + replacing `path` when it is a symlink would move
  // the user's config/session symlink to `.bak` and publish a fresh regular file at the
  // link path. Resolving here lands every temp/backup/rename on the real target and
  // preserves the link (same fix already applied to editor text saves, TD-2026-07-17A-127).
  const std::filesystem::path target = util::ResolveSymlinkTarget(path);

  const std::filesystem::path parent = target.parent_path();
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

  const std::filesystem::path temp_path = util::UniqueTemporaryPath(target);
  const std::filesystem::path backup_path = BackupPathFor(target);
  std::error_code fs_error;
  std::filesystem::remove(temp_path, fs_error);
  fs_error.clear();

  if (!WriteFileBytesDurable(temp_path, file_bytes)) {
    std::filesystem::remove(temp_path, fs_error);
    SetError(error, PersistedRecordWriterError::WriteFailed);
    return false;
  }

  const bool destination_exists = FileExists(target);
  if (destination_exists) {
    std::filesystem::remove(backup_path, fs_error);
    fs_error.clear();
    if (!RenameReplacing(target, backup_path)) {
      std::filesystem::remove(temp_path, fs_error);
      SetError(error, PersistedRecordWriterError::BackupFailed);
      return false;
    }
  }

  if (!RenameReplacing(temp_path, target)) {
    if (destination_exists && !FileExists(target) && FileExists(backup_path)) {
      RenameReplacing(backup_path, target);
    }
    std::filesystem::remove(temp_path, fs_error);
    SetError(error, PersistedRecordWriterError::RenameFailed);
    return false;
  }

  return true;
}

}  // namespace microide::persistence
