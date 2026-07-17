#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>

// In-process SHA-256. Replaces shelling out to sha256sum/shasum/certutil for
// downloaded-tool verification (TD-2026-07-17-060): a subprocess per hash added a
// serial-lane stall, a cancellation gap, and a hang risk if the platform tool was
// missing/wedged. This is a self-contained, deterministic implementation with
// known-answer test coverage.
namespace microide::util {

// Streaming SHA-256 hasher. Feed bytes with Update(), then read the digest.
class Sha256 {
 public:
  Sha256();

  void Update(std::span<const std::byte> data);
  void Update(std::string_view data);

  // Finalizes and returns the 32-byte digest as a 64-char lowercase hex string.
  // The hasher must not be reused after Finish().
  std::string FinishHex();

 private:
  void ProcessBlock(const std::uint8_t* block);

  std::uint32_t state_[8];
  std::uint8_t buffer_[64];
  std::size_t buffer_len_ = 0;
  std::uint64_t total_bits_ = 0;
};

// Convenience: hex digest of a byte span / string.
std::string Sha256Hex(std::span<const std::byte> data);
std::string Sha256Hex(std::string_view data);

// Hex digest of a file's contents, read in bounded chunks. Returns nullopt if the
// file cannot be opened/read. Rejects non-regular files up front (never blocks on a
// FIFO/device), mirroring util::TextFileIO.
std::optional<std::string> Sha256FileHex(const std::filesystem::path& path);

}  // namespace microide::util
