#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>

namespace microide::util {

class Sha256 {
 public:
  Sha256();

  void Update(std::span<const std::byte> data);
  void Update(std::string_view data);

  std::string FinishHex();

 private:
  void ProcessBlock(const std::uint8_t* block);

  std::uint32_t state_[8];
  std::uint8_t buffer_[64];
  std::size_t buffer_len_ = 0;
  std::uint64_t total_bits_ = 0;
};

std::string Sha256Hex(std::span<const std::byte> data);
std::string Sha256Hex(std::string_view data);

std::optional<std::string> Sha256FileHex(const std::filesystem::path& path);

}
