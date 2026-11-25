#pragma once

#include <cstdint>
#include <string_view>

namespace microide::util {

inline constexpr std::uint64_t kFnv1aOffsetBasis = 1469598103934665603ULL;
inline constexpr std::uint64_t kFnv1aPrime = 1099511628211ULL;

constexpr std::uint64_t Fnv1aByte(std::uint64_t hash, unsigned char byte) {
  return (hash ^ static_cast<std::uint64_t>(byte)) * kFnv1aPrime;
}

constexpr std::uint64_t Fnv1aBytes(std::uint64_t hash, std::string_view bytes) {
  for (const char byte : bytes) {
    hash = Fnv1aByte(hash, static_cast<unsigned char>(byte));
  }
  return hash;
}

constexpr std::uint64_t Fnv1aValue(std::uint64_t hash, std::uint64_t value) {
  for (int shift = 0; shift < 64; shift += 8) {
    hash = Fnv1aByte(hash, static_cast<unsigned char>((value >> shift) & 0xFFULL));
  }
  return hash;
}

constexpr std::uint64_t Fnv1aHash(std::string_view bytes) {
  return Fnv1aBytes(kFnv1aOffsetBasis, bytes);
}

}
