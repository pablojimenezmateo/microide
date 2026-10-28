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
  // How many WriteFile calls the CALLING thread has made. The process-wide
  // `persistence.record_writes` counter also counts every other thread's saves (a
  // background project-state write still in flight), so a "this did not write"
  // assertion read from it fails whenever anything else is persisting.
  static std::uint64_t WritesOnThisThread();
};

}  // namespace microide::persistence
