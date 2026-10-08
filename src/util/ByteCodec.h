#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace microide::util {

// Little-endian fixed-width and LEB128 varint encoding over a std::string, for
// the remote wire's binary payloads (project/remote/RemoteFrame, terminal/
// TerminalHostWire). One spelling of "put a u64 in N bytes" instead of one per
// codec.

inline void PutLe(std::string& out, std::uint64_t value, std::size_t bytes) {
  for (std::size_t i = 0; i < bytes; ++i) {
    out.push_back(static_cast<char>((value >> (8 * i)) & 0xffu));
  }
}

// Caller guarantees `offset + bytes <= in.size()`.
inline std::uint64_t GetLe(std::string_view in, std::size_t offset, std::size_t bytes) {
  std::uint64_t value = 0;
  for (std::size_t i = 0; i < bytes; ++i) {
    value |= static_cast<std::uint64_t>(static_cast<unsigned char>(in[offset + i])) << (8 * i);
  }
  return value;
}

inline void PutVarint(std::string& out, std::uint64_t value) {
  while (value >= 0x80u) {
    out.push_back(static_cast<char>((value & 0x7fu) | 0x80u));
    value >>= 7;
  }
  out.push_back(static_cast<char>(value));
}

inline void PutBytes(std::string& out, std::string_view bytes) {
  PutVarint(out, bytes.size());
  out.append(bytes);
}

// A bounds-checked cursor over untrusted bytes. Every read past the end, or a
// varint longer than ten bytes, latches `failed()` and returns zero/empty from
// then on, so a decoder can read a whole record and check once at the end.
class ByteReader {
 public:
  explicit ByteReader(std::string_view in) : in_(in) {}

  std::uint64_t Le(std::size_t bytes) {
    if (failed_ || in_.size() - offset_ < bytes) {
      failed_ = true;
      return 0;
    }
    const std::uint64_t value = GetLe(in_, offset_, bytes);
    offset_ += bytes;
    return value;
  }

  std::uint64_t Varint() {
    std::uint64_t value = 0;
    for (unsigned shift = 0; shift < 64; shift += 7) {
      if (failed_ || offset_ >= in_.size()) {
        break;
      }
      const auto byte = static_cast<unsigned char>(in_[offset_++]);
      value |= static_cast<std::uint64_t>(byte & 0x7fu) << shift;
      if ((byte & 0x80u) == 0) {
        return value;
      }
    }
    failed_ = true;
    return 0;
  }

  // Exactly `count` bytes, unprefixed.
  std::string_view Raw(std::size_t count) {
    if (failed_ || in_.size() - offset_ < count) {
      failed_ = true;
      return {};
    }
    const std::string_view bytes = in_.substr(offset_, count);
    offset_ += count;
    return bytes;
  }

  // A varint length, then that many bytes; at most `max_bytes` (a forged length
  // fails rather than being trusted).
  std::string_view Bytes(std::size_t max_bytes) {
    const std::uint64_t size = Varint();
    if (failed_ || size > max_bytes || size > in_.size() - offset_) {
      failed_ = true;
      return {};
    }
    const std::string_view bytes = in_.substr(offset_, static_cast<std::size_t>(size));
    offset_ += static_cast<std::size_t>(size);
    return bytes;
  }

  bool failed() const { return failed_; }
  bool at_end() const { return offset_ == in_.size(); }
  std::size_t remaining() const { return in_.size() - offset_; }
  void Fail() { failed_ = true; }

 private:
  std::string_view in_;
  std::size_t offset_ = 0;
  bool failed_ = false;
};

}  // namespace microide::util
