#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

// BLAKE3 content hashes: the remote mirror's object address, the manifest row's
// `content_hash`, and the compare-and-swap token a push presents
// (dev-docs/design/remote-projects.md § 6.2). 32 raw bytes on the wire, never the
// 64-character hex, which is for logs and file names only.
namespace microide::util {

struct ContentHash {
  static constexpr std::size_t kBytes = 32;
  std::array<std::uint8_t, kBytes> bytes{};

  friend bool operator==(const ContentHash&, const ContentHash&) = default;
  friend auto operator<=>(const ContentHash&, const ContentHash&) = default;

  std::string_view raw() const {
    return std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size());
  }
  // Exactly kBytes raw bytes, or nullopt.
  static std::optional<ContentHash> FromRaw(std::string_view raw);
  // 64 lowercase hex digits.
  std::string Hex() const;
  static std::optional<ContentHash> FromHex(std::string_view hex);
};

// The digest is already uniformly distributed: its first word is a perfect bucket key.
struct ContentHashHasher {
  std::size_t operator()(const ContentHash& hash) const noexcept {
    std::size_t word = 0;
    std::memcpy(&word, hash.bytes.data(), sizeof(word));
    return word;
  }
};

// Streaming hasher. Opaque storage so the vendored header stays out of every TU
// that names a hash.
class ContentHasher {
 public:
  ContentHasher();
  void Update(std::string_view bytes);
  ContentHash Finish() const;

 private:
  alignas(8) unsigned char state_[1920];
};

ContentHash HashContent(std::string_view bytes);

// A regular file's hash, read in bounded chunks. nullopt when it is not a regular
// file (never blocks on a FIFO or device) or cannot be read. `size_out`, when given,
// receives the number of bytes hashed.
std::optional<ContentHash> HashFileContent(const std::filesystem::path& path,
                                           std::uint64_t* size_out = nullptr);

}  // namespace microide::util
