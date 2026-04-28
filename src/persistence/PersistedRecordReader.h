#pragma once

#include "persistence/PersistedRecord.h"

#include <cstddef>
#include <filesystem>
#include <optional>
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
};

class PersistedRecordReader {
 public:
  static std::optional<PersistedRecordReadResult> ReadFile(
      const std::filesystem::path& path,
      PersistedRecordReaderError* error = nullptr);
};

}  // namespace microide::persistence
