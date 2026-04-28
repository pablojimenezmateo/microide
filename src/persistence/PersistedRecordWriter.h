#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>

namespace microide::persistence {

enum class PersistedRecordWriterError {
  None = 0,
  InvalidPath,
  CreateDirectoryFailed,
  EncodeFailed,
  OpenFailed,
  WriteFailed,
  FlushFailed,
  BackupFailed,
  RenameFailed,
};

class PersistedRecordWriter {
 public:
  static bool WriteFile(const std::filesystem::path& path,
                        std::span<const std::byte> body,
                        std::uint32_t capability_flags,
                        PersistedRecordWriterError* error = nullptr);
  static std::filesystem::path BackupPathFor(const std::filesystem::path& path);
};

}  // namespace microide::persistence
