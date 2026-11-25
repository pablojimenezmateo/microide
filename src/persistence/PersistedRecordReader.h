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
  PersistedRecordReaderError primary_error = PersistedRecordReaderError::None;
};

class PersistedRecordReader {
 public:
  static std::optional<PersistedRecordReadResult> Decode(
      std::span<const std::byte> file_bytes,
      PersistedRecordReaderError* error = nullptr);

  static std::optional<PersistedRecordReadResult> ReadFile(
      const std::filesystem::path& path,
      PersistedRecordReaderError* error = nullptr,
      bool allow_backup_for_unsupported_version = false);
};

}
