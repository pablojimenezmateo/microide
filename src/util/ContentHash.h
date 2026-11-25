#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace microide::util {

struct ContentHash {
  static constexpr std::size_t kBytes = 32;
  std::array<std::uint8_t, kBytes> bytes{};

  friend bool operator==(const ContentHash&, const ContentHash&) = default;
  friend auto operator<=>(const ContentHash&, const ContentHash&) = default;

  std::string_view raw() const {
    return std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size());
  }
  static std::optional<ContentHash> FromRaw(std::string_view raw);
  std::string Hex() const;
  static std::optional<ContentHash> FromHex(std::string_view hex);
};

struct ContentHashHasher {
  std::size_t operator()(const ContentHash& hash) const noexcept {
    std::size_t word = 0;
    std::memcpy(&word, hash.bytes.data(), sizeof(word));
    return word;
  }
};

class ContentHasher {
 public:
  ContentHasher();
  void Update(std::string_view bytes);
  ContentHash Finish() const;

 private:
  alignas(8) unsigned char state_[1920];
};

ContentHash HashContent(std::string_view bytes);

std::optional<ContentHash> HashFileDescriptor(int fd, std::uint64_t* size_out = nullptr,
                                              const std::function<void(std::string_view)>& sink = {});

std::optional<ContentHash> HashFileContent(const std::filesystem::path& path,
                                           std::uint64_t* size_out = nullptr);

}
