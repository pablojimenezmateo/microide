#include "persistence/PersistedRecordReader.h"

#include "persistence/PersistedRecordWriter.h"
#include "util/PerformanceCounters.h"
#include "util/PerformanceTrace.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <optional>
#include <span>
#include <system_error>
#include <vector>

namespace microide::persistence {
namespace {

constexpr std::uintmax_t kMaxPersistedFileBytes = 256ull * 1024 * 1024;

void SetError(PersistedRecordReaderError* error, PersistedRecordReaderError value) {
  if (error != nullptr) {
    *error = value;
  }
}

bool ReadAllBytes(const std::filesystem::path& path,
                  std::vector<std::byte>* bytes,
                  PersistedRecordReaderError* error) {
  if (bytes == nullptr) {
    SetError(error, PersistedRecordReaderError::ReadFailed);
    return false;
  }

  std::error_code status_error;
  const std::filesystem::file_status status = std::filesystem::status(path, status_error);
  if (status_error || !std::filesystem::is_regular_file(status)) {
    SetError(error, PersistedRecordReaderError::ReadFailed);
    return false;
  }

  std::ifstream file(path, std::ios::binary);
  if (!file) {
    SetError(error, PersistedRecordReaderError::ReadFailed);
    return false;
  }

  file.seekg(0, std::ios::end);
  const std::streamoff size = file.tellg();
  if (size < 0) {
    SetError(error, PersistedRecordReaderError::ReadFailed);
    return false;
  }
  if (static_cast<std::uintmax_t>(size) > kMaxPersistedFileBytes) {
    SetError(error, PersistedRecordReaderError::ReadFailed);
    return false;
  }
  file.seekg(0, std::ios::beg);

  constexpr std::size_t kHeaderBytes = kPersistedRecordMagic.size() + sizeof(std::uint32_t) * 3;
  if (static_cast<std::uintmax_t>(size) >= kHeaderBytes) {
    std::array<std::byte, kHeaderBytes> head{};
    file.read(reinterpret_cast<char*>(head.data()), static_cast<std::streamsize>(kHeaderBytes));
    if (!file) {
      SetError(error, PersistedRecordReaderError::ReadFailed);
      return false;
    }
    for (std::size_t i = 0; i < kPersistedRecordMagic.size(); ++i) {
      if (std::to_integer<char>(head[i]) != kPersistedRecordMagic[i]) {
        SetError(error, PersistedRecordReaderError::ParseFailed);
        return false;
      }
    }
    const std::size_t v = kPersistedRecordMagic.size();
    const std::uint32_t version = static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(head[v])) |
                                  (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(head[v + 1])) << 8) |
                                  (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(head[v + 2])) << 16) |
                                  (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(head[v + 3])) << 24);
    if (version != kPersistedRecordFormatVersion) {
      SetError(error, PersistedRecordReaderError::UnsupportedVersion);
      return false;
    }
    file.seekg(0, std::ios::beg);
  }

  bytes->assign(static_cast<std::size_t>(size), std::byte{0});
  if (!bytes->empty()) {
    file.read(reinterpret_cast<char*>(bytes->data()), size);
    if (!file) {
      SetError(error, PersistedRecordReaderError::ReadFailed);
      return false;
    }
  }
  return true;
}

bool DecodeRecordFile(std::span<const std::byte> file_bytes,
                      PersistedRecordReadResult* result,
                      PersistedRecordReaderError* error) {
  if (result == nullptr) {
    SetError(error, PersistedRecordReaderError::ParseFailed);
    return false;
  }

  PersistedRecordHeader header;
  std::span<const std::byte> body;
  if (!ParsePersistedRecordFile(file_bytes, &header, &body)) {
    SetError(error, PersistedRecordReaderError::ParseFailed);
    return false;
  }

  if (header.version != kPersistedRecordFormatVersion) {
    SetError(error, PersistedRecordReaderError::UnsupportedVersion);
    return false;
  }

  result->header = header;
  result->body.assign(body.begin(), body.end());
  return true;
}

std::optional<PersistedRecordReadResult> TryReadSpecificFile(
    const std::filesystem::path& path,
    PersistedRecordReaderError* error) {
  std::error_code exists_error;
  const bool present = std::filesystem::exists(path, exists_error);
  if (exists_error) {
    SetError(error, PersistedRecordReaderError::ReadFailed);
    return std::nullopt;
  }
  if (!present) {
    SetError(error, PersistedRecordReaderError::NotFound);
    return std::nullopt;
  }

  util::PerformanceTrace::Scope perf_scope("persistence::ReadRecordFile");
  std::vector<std::byte> file_bytes;
  PersistedRecordReaderError read_error = PersistedRecordReaderError::None;
  if (!ReadAllBytes(path, &file_bytes, &read_error)) {
    SetError(error, read_error);
    return std::nullopt;
  }
  util::AddPerformanceCounter(util::PerfCounterId::PersistenceRecordReads);
  util::AddPerformanceCounter(util::PerfCounterId::PersistenceRecordBytesRead, file_bytes.size());

  PersistedRecordReadResult decoded;
  PersistedRecordReaderError decode_error = PersistedRecordReaderError::None;
  if (!DecodeRecordFile(file_bytes, &decoded, &decode_error)) {
    SetError(error, decode_error);
    return std::nullopt;
  }

  SetError(error, PersistedRecordReaderError::None);
  return decoded;
}

PersistedRecordReaderError MergeErrors(PersistedRecordReaderError primary,
                                       PersistedRecordReaderError backup) {
  if (primary == PersistedRecordReaderError::UnsupportedVersion ||
      backup == PersistedRecordReaderError::UnsupportedVersion) {
    return PersistedRecordReaderError::UnsupportedVersion;
  }
  if (primary == PersistedRecordReaderError::ParseFailed ||
      backup == PersistedRecordReaderError::ParseFailed) {
    return PersistedRecordReaderError::ParseFailed;
  }
  if (primary == PersistedRecordReaderError::ReadFailed ||
      backup == PersistedRecordReaderError::ReadFailed) {
    return PersistedRecordReaderError::ReadFailed;
  }
  return PersistedRecordReaderError::NotFound;
}

}

std::optional<PersistedRecordReadResult> PersistedRecordReader::ReadFile(
    const std::filesystem::path& path,
    PersistedRecordReaderError* error,
    bool allow_backup_for_unsupported_version) {
  SetError(error, PersistedRecordReaderError::None);
  PersistedRecordReaderError primary_error = PersistedRecordReaderError::None;
  if (auto primary = TryReadSpecificFile(path, &primary_error); primary.has_value()) {
    return primary;
  }

  if (primary_error == PersistedRecordReaderError::UnsupportedVersion &&
      !allow_backup_for_unsupported_version) {
    SetError(error, PersistedRecordReaderError::UnsupportedVersion);
    return std::nullopt;
  }

  PersistedRecordReaderError backup_error = PersistedRecordReaderError::None;
  const std::filesystem::path backup_path = PersistedRecordWriter::BackupPathFor(path);
  if (auto backup = TryReadSpecificFile(backup_path, &backup_error); backup.has_value()) {
    backup->used_backup = true;
    backup->primary_error = primary_error;
    SetError(error, PersistedRecordReaderError::None);
    return backup;
  }

  SetError(error, MergeErrors(primary_error, backup_error));
  return std::nullopt;
}

std::optional<PersistedRecordReadResult> PersistedRecordReader::Decode(
    std::span<const std::byte> file_bytes,
    PersistedRecordReaderError* error) {
  PersistedRecordReadResult decoded;
  PersistedRecordReaderError decode_error = PersistedRecordReaderError::None;
  if (!DecodeRecordFile(file_bytes, &decoded, &decode_error)) {
    SetError(error, decode_error);
    return std::nullopt;
  }
  SetError(error, PersistedRecordReaderError::None);
  return decoded;
}

}
