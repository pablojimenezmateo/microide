#pragma once

#include "persistence/PersistedRecord.h"

#include <cstddef>
#include <filesystem>
#include <optional>
#include <span>
#include <vector>

namespace microide::persistence {

enum class PersistedRecordReaderError {
  None = 0,
  NotFound,
  ReadFailed,
  ParseFailed,
  UnsupportedVersion,
};

struct PersistedRecordReadResult {
  PersistedRecordHeader header;
  std::vector<std::byte> body;
  bool used_backup = false;
  // When used_backup is true, this carries the primary file's failure that
  // triggered the backup fallback so callers can surface/log that recovery
  // happened (and why). None when the primary was read successfully.
  PersistedRecordReaderError primary_error = PersistedRecordReaderError::None;
};

class PersistedRecordReader {
 public:
  static std::optional<PersistedRecordReadResult> Decode(
      std::span<const std::byte> file_bytes,
      PersistedRecordReaderError* error = nullptr);

  // Reads `path`, falling back to its `.bak` sibling when the primary cannot be
  // read/parsed. A version-mismatched primary (UnsupportedVersion) is NOT
  // silently replaced by a possibly-older backup unless
  // `allow_backup_for_unsupported_version` opts into an explicit downgrade
  // recovery path; otherwise the version mismatch is surfaced and no fallback
  // occurs. On a successful backup read, `error` reports None (success) while
  // the returned result carries `used_backup=true` and `primary_error`.
  static std::optional<PersistedRecordReadResult> ReadFile(
      const std::filesystem::path& path,
      PersistedRecordReaderError* error = nullptr,
      bool allow_backup_for_unsupported_version = false);
};

}  // namespace microide::persistence
