#include "persistence/PersistedRecord.h"

#include <algorithm>
#include <cstring>
#include <limits>

namespace microide::persistence {

namespace {

template <typename T>
void AppendLe(std::vector<std::byte>* out, T value) {
  for (std::size_t i = 0; i < sizeof(T); ++i) {
    out->push_back(std::byte(static_cast<unsigned char>((value >> (i * 8)) & 0xFFu)));
  }
}

template <typename T>
bool ReadLe(std::span<const std::byte> input, std::size_t* offset, T* value) {
  if (offset == nullptr || value == nullptr || *offset + sizeof(T) > input.size()) {
    return false;
  }
  T parsed = 0;
  for (std::size_t i = 0; i < sizeof(T); ++i) {
    const std::uint64_t octet =
        static_cast<std::uint64_t>(std::to_integer<unsigned char>(input[*offset + i]));
    parsed |= static_cast<T>(octet << (i * 8));
  }
  *offset += sizeof(T);
  *value = parsed;
  return true;
}

const std::array<std::uint32_t, 256>& CrcTable() {
  static const std::array<std::uint32_t, 256> table = [] {
    std::array<std::uint32_t, 256> result{};
    for (std::uint32_t i = 0; i < 256; ++i) {
      std::uint32_t crc = i;
      for (int bit = 0; bit < 8; ++bit) {
        crc = (crc & 1u) != 0u ? (crc >> 1u) ^ 0x82F63B78u : (crc >> 1u);
      }
      result[i] = crc;
    }
    return result;
  }();
  return table;
}

std::string PathToUtf8String(const std::filesystem::path& path) {
#if defined(_WIN32)
  const std::u8string path_u8 = path.generic_u8string();
  std::string value;
  value.reserve(path_u8.size());
  for (char8_t ch : path_u8) {
    value.push_back(static_cast<char>(ch));
  }
  return value;
#else
  return path.generic_string();
#endif
}

PathPlatform HostPathPlatform() {
#if defined(_WIN32)
  return PathPlatform::Windows;
#else
  return PathPlatform::Posix;
#endif
}

}  // namespace

std::uint32_t ComputeCrc32c(std::span<const std::byte> data) {
  std::uint32_t crc = 0xFFFFFFFFu;
  const auto& table = CrcTable();
  for (const std::byte byte : data) {
    const std::uint8_t index =
        static_cast<std::uint8_t>(crc ^ std::to_integer<std::uint8_t>(byte));
    crc = table[index] ^ (crc >> 8u);
  }
  return ~crc;
}

bool BuildPersistedRecordFile(std::span<const std::byte> body,
                              std::uint32_t capability_flags,
                              std::vector<std::byte>* out) {
  if (out == nullptr) {
    return false;
  }
  out->clear();
  out->reserve(kPersistedRecordMagic.size() + sizeof(std::uint32_t) * 3 + body.size());
  for (char ch : kPersistedRecordMagic) {
    out->push_back(std::byte(static_cast<unsigned char>(ch)));
  }
  AppendLe<std::uint32_t>(out, kPersistedRecordFormatVersion);
  AppendLe<std::uint32_t>(out, capability_flags);
  AppendLe<std::uint32_t>(out, ComputeCrc32c(body));
  out->insert(out->end(), body.begin(), body.end());
  return true;
}

bool ParsePersistedRecordFile(std::span<const std::byte> file,
                              PersistedRecordHeader* header,
                              std::span<const std::byte>* body) {
  if (header == nullptr || body == nullptr) {
    return false;
  }
  constexpr std::size_t kHeaderBytes = 4 + sizeof(std::uint32_t) * 3;
  if (file.size() < kHeaderBytes) {
    return false;
  }
  for (std::size_t i = 0; i < kPersistedRecordMagic.size(); ++i) {
    if (std::to_integer<char>(file[i]) != kPersistedRecordMagic[i]) {
      return false;
    }
  }

  std::size_t offset = kPersistedRecordMagic.size();
  if (!ReadLe<std::uint32_t>(file, &offset, &header->version) ||
      !ReadLe<std::uint32_t>(file, &offset, &header->capability_flags) ||
      !ReadLe<std::uint32_t>(file, &offset, &header->crc32c)) {
    return false;
  }

  *body = file.subspan(offset);
  return ComputeCrc32c(*body) == header->crc32c;
}

bool PrimitiveWriter::WriteU8(std::uint8_t value) {
  if (out_ == nullptr) {
    return false;
  }
  out_->push_back(std::byte(value));
  return true;
}

bool PrimitiveWriter::WriteU16(std::uint16_t value) {
  if (out_ == nullptr) {
    return false;
  }
  AppendLe<std::uint16_t>(out_, value);
  return true;
}

bool PrimitiveWriter::WriteU32(std::uint32_t value) {
  if (out_ == nullptr) {
    return false;
  }
  AppendLe<std::uint32_t>(out_, value);
  return true;
}

bool PrimitiveWriter::WriteI32(std::int32_t value) {
  std::uint32_t encoded = 0;
  static_assert(sizeof(encoded) == sizeof(value));
  std::memcpy(&encoded, &value, sizeof(value));
  return WriteU32(encoded);
}

bool PrimitiveWriter::WriteI64(std::int64_t value) {
  if (out_ == nullptr) {
    return false;
  }
  std::uint64_t encoded = 0;
  static_assert(sizeof(encoded) == sizeof(value));
  std::memcpy(&encoded, &value, sizeof(value));
  AppendLe<std::uint64_t>(out_, encoded);
  return true;
}

bool PrimitiveWriter::WriteF32(float value) {
  std::uint32_t encoded = 0;
  static_assert(sizeof(encoded) == sizeof(value));
  std::memcpy(&encoded, &value, sizeof(value));
  return WriteU32(encoded);
}

bool PrimitiveWriter::WriteBool(bool value) {
  return WriteU8(value ? 1u : 0u);
}

bool PrimitiveWriter::WriteString(std::string_view value) {
  if (value.size() > std::numeric_limits<std::uint32_t>::max()) {
    return false;
  }
  if (!WriteU32(static_cast<std::uint32_t>(value.size()))) {
    return false;
  }
  if (out_ == nullptr) {
    return false;
  }
  out_->reserve(out_->size() + value.size());
  for (char ch : value) {
    out_->push_back(std::byte(static_cast<unsigned char>(ch)));
  }
  return true;
}

bool PrimitiveWriter::WritePath(const std::filesystem::path& path) {
  return WriteU8(static_cast<std::uint8_t>(HostPathPlatform())) &&
         WriteString(PathToUtf8String(path));
}

bool PrimitiveReader::ReadBytes(std::span<std::byte> target) {
  if (offset_ + target.size() > input_.size()) {
    return false;
  }
  std::copy_n(input_.begin() + static_cast<std::ptrdiff_t>(offset_), target.size(),
              target.begin());
  offset_ += target.size();
  return true;
}

bool PrimitiveReader::ReadU8(std::uint8_t* value) {
  if (value == nullptr || offset_ + 1 > input_.size()) {
    return false;
  }
  *value = std::to_integer<std::uint8_t>(input_[offset_]);
  ++offset_;
  return true;
}

bool PrimitiveReader::ReadU16(std::uint16_t* value) {
  return ReadLe<std::uint16_t>(input_, &offset_, value);
}

bool PrimitiveReader::ReadU32(std::uint32_t* value) {
  return ReadLe<std::uint32_t>(input_, &offset_, value);
}

bool PrimitiveReader::ReadI32(std::int32_t* value) {
  if (value == nullptr) {
    return false;
  }
  std::uint32_t encoded = 0;
  if (!ReadU32(&encoded)) {
    return false;
  }
  std::memcpy(value, &encoded, sizeof(encoded));
  return true;
}

bool PrimitiveReader::ReadI64(std::int64_t* value) {
  if (value == nullptr) {
    return false;
  }
  std::uint64_t encoded = 0;
  if (!ReadLe<std::uint64_t>(input_, &offset_, &encoded)) {
    return false;
  }
  std::memcpy(value, &encoded, sizeof(encoded));
  return true;
}

bool PrimitiveReader::ReadF32(float* value) {
  if (value == nullptr) {
    return false;
  }
  std::uint32_t encoded = 0;
  if (!ReadU32(&encoded)) {
    return false;
  }
  std::memcpy(value, &encoded, sizeof(encoded));
  return true;
}

bool PrimitiveReader::ReadBool(bool* value) {
  if (value == nullptr) {
    return false;
  }
  std::uint8_t encoded = 0;
  if (!ReadU8(&encoded)) {
    return false;
  }
  if (encoded > 1) {
    return false;
  }
  *value = encoded != 0;
  return true;
}

bool PrimitiveReader::ReadString(std::string* value) {
  if (value == nullptr) {
    return false;
  }
  std::uint32_t size = 0;
  if (!ReadU32(&size) || offset_ + size > input_.size()) {
    return false;
  }
  value->clear();
  value->reserve(size);
  for (std::uint32_t i = 0; i < size; ++i) {
    value->push_back(static_cast<char>(
        std::to_integer<unsigned char>(input_[offset_ + i])));
  }
  offset_ += size;
  return true;
}

bool PrimitiveReader::ReadPath(std::filesystem::path* path) {
  if (path == nullptr) {
    return false;
  }
  std::uint8_t platform_tag = 0;
  std::string encoded;
  if (!ReadU8(&platform_tag) || !ReadString(&encoded)) {
    return false;
  }
  if (platform_tag != static_cast<std::uint8_t>(PathPlatform::Posix) &&
      platform_tag != static_cast<std::uint8_t>(PathPlatform::Windows)) {
    return false;
  }

#if defined(_WIN32)
  std::u8string decoded;
  decoded.reserve(encoded.size());
  for (char ch : encoded) {
    decoded.push_back(static_cast<char8_t>(ch));
  }
  *path = std::filesystem::path(decoded);
#else
  *path = std::filesystem::path(encoded);
#endif
  return true;
}

bool AppendTaggedRecord(std::uint16_t tag,
                        std::span<const std::byte> payload,
                        std::vector<std::byte>* out) {
  if (out == nullptr || payload.size() > std::numeric_limits<std::uint32_t>::max()) {
    return false;
  }
  PrimitiveWriter writer(out);
  if (!writer.WriteU16(tag) || !writer.WriteU32(static_cast<std::uint32_t>(payload.size()))) {
    return false;
  }
  out->insert(out->end(), payload.begin(), payload.end());
  return true;
}

bool ReadTaggedRecord(std::span<const std::byte> input,
                      std::size_t* offset,
                      TaggedRecordView* record) {
  if (offset == nullptr || record == nullptr || *offset > input.size()) {
    return false;
  }
  PrimitiveReader reader(input.subspan(*offset));
  std::uint16_t tag = 0;
  std::uint32_t length = 0;
  if (!reader.ReadU16(&tag) || !reader.ReadU32(&length) || reader.remaining() < length) {
    return false;
  }
  const std::size_t payload_offset = *offset + reader.offset();
  record->tag = tag;
  record->payload = input.subspan(payload_offset, length);
  *offset = payload_offset + length;
  return true;
}

}  // namespace microide::persistence
