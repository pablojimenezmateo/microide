#include "persistence/PersistedRecordReader.h"

#include "persistence/PersistedRecordWriter.h"

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <ios>
#include <optional>
#include <system_error>
#include <vector>

namespace microide::persistence {
namespace {

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
  file.seekg(0, std::ios::beg);

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

bool DecodeRecordFile(const std::vector<std::byte>& file_bytes,
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
  if (!std::filesystem::exists(path, exists_error)) {
    SetError(error, PersistedRecordReaderError::NotFound);
    return std::nullopt;
  }
  if (exists_error) {
    SetError(error, PersistedRecordReaderError::ReadFailed);
    return std::nullopt;
  }

  std::vector<std::byte> file_bytes;
  PersistedRecordReaderError read_error = PersistedRecordReaderError::None;
  if (!ReadAllBytes(path, &file_bytes, &read_error)) {
    SetError(error, read_error);
    return std::nullopt;
  }

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

}  // namespace

std::optional<PersistedRecordReadResult> PersistedRecordReader::ReadFile(
    const std::filesystem::path& path,
    PersistedRecordReaderError* error) {
  SetError(error, PersistedRecordReaderError::None);
  PersistedRecordReaderError primary_error = PersistedRecordReaderError::None;
  if (auto primary = TryReadSpecificFile(path, &primary_error); primary.has_value()) {
    return primary;
  }

  PersistedRecordReaderError backup_error = PersistedRecordReaderError::None;
  const std::filesystem::path backup_path = PersistedRecordWriter::BackupPathFor(path);
  if (auto backup = TryReadSpecificFile(backup_path, &backup_error); backup.has_value()) {
    backup->used_backup = true;
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
  const std::vector<std::byte> owned_bytes(file_bytes.begin(), file_bytes.end());
  if (!DecodeRecordFile(owned_bytes, &decoded, &decode_error)) {
    SetError(error, decode_error);
    return std::nullopt;
  }
  SetError(error, PersistedRecordReaderError::None);
  return decoded;
}

}  // namespace microide::persistence
