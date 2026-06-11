#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace microide::persistence {

inline constexpr std::array<char, 4> kPersistedRecordMagic = {'M', 'I', 'D', 'E'};
inline constexpr std::uint32_t kPersistedRecordFormatVersion = 1;

enum class PathPlatform : std::uint8_t {
  Posix = 1,
  Windows = 2,
};

struct PersistedRecordHeader {
  std::uint32_t version = kPersistedRecordFormatVersion;
  std::uint32_t capability_flags = 0;
  std::uint32_t crc32c = 0;
};

struct TaggedRecordView {
  std::uint16_t tag = 0;
  std::span<const std::byte> payload;
};

std::uint32_t ComputeCrc32c(std::span<const std::byte> data);
bool BuildPersistedRecordFile(std::span<const std::byte> body,
                              std::uint32_t capability_flags,
                              std::vector<std::byte>* out);
bool ParsePersistedRecordFile(std::span<const std::byte> file,
                              PersistedRecordHeader* header,
                              std::span<const std::byte>* body);

class PrimitiveWriter {
 public:
  explicit PrimitiveWriter(std::vector<std::byte>* out) : out_(out) {}

  bool WriteU8(std::uint8_t value);
  bool WriteU16(std::uint16_t value);
  bool WriteU32(std::uint32_t value);
  bool WriteI32(std::int32_t value);
  bool WriteI64(std::int64_t value);
  bool WriteF32(float value);
  bool WriteBool(bool value);
  bool WriteString(std::string_view value);
  bool WritePath(const std::filesystem::path& path);

  template <typename T, typename WriteItem>
  bool WriteVector(const std::vector<T>& values, WriteItem write_item) {
    if (!WriteU32(static_cast<std::uint32_t>(values.size()))) {
      return false;
    }
    for (const T& value : values) {
      if (!write_item(*this, value)) {
        return false;
      }
    }
    return true;
  }

  template <typename T, typename WriteValue>
  bool WriteOptional(const std::optional<T>& value, WriteValue write_value) {
    if (!WriteBool(value.has_value())) {
      return false;
    }
    return !value.has_value() || write_value(*this, *value);
  }

 private:
  std::vector<std::byte>* out_ = nullptr;
};

class PrimitiveReader {
 public:
  explicit PrimitiveReader(std::span<const std::byte> input) : input_(input) {}

  bool ReadU8(std::uint8_t* value);
  bool ReadU16(std::uint16_t* value);
  bool ReadU32(std::uint32_t* value);
  bool ReadI32(std::int32_t* value);
  bool ReadI64(std::int64_t* value);
  bool ReadF32(float* value);
  bool ReadBool(bool* value);
  bool ReadString(std::string* value);
  bool ReadPath(std::filesystem::path* path);

  template <typename T, typename ReadItem>
  bool ReadVector(std::vector<T>* values, ReadItem read_item) {
    if (values == nullptr) {
      return false;
    }
    std::uint32_t count = 0;
    if (!ReadU32(&count)) {
      return false;
    }
    values->clear();
    // Bound the reservation by the remaining input so a corrupt length field cannot
    // force an unbounded allocation; each element consumes at least one byte.
    values->reserve(std::min<std::size_t>(count, remaining()));
    for (std::uint32_t i = 0; i < count; ++i) {
      T item{};
      if (!read_item(*this, &item)) {
        return false;
      }
      values->push_back(std::move(item));
    }
    return true;
  }

  template <typename T, typename ReadValue>
  bool ReadOptional(std::optional<T>* value, ReadValue read_value) {
    if (value == nullptr) {
      return false;
    }
    bool present = false;
    if (!ReadBool(&present)) {
      return false;
    }
    if (!present) {
      value->reset();
      return true;
    }
    T parsed{};
    if (!read_value(*this, &parsed)) {
      return false;
    }
    *value = std::move(parsed);
    return true;
  }

  std::size_t offset() const { return offset_; }
  std::size_t remaining() const { return offset_ <= input_.size() ? input_.size() - offset_ : 0; }

 private:
  std::span<const std::byte> input_;
  std::size_t offset_ = 0;

  bool ReadBytes(std::span<std::byte> target);
};

bool AppendTaggedRecord(std::uint16_t tag,
                        std::span<const std::byte> payload,
                        std::vector<std::byte>* out);
bool ReadTaggedRecord(std::span<const std::byte> input,
                      std::size_t* offset,
                      TaggedRecordView* record);

}  // namespace microide::persistence
