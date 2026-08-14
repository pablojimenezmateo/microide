#pragma once

#include <cstdint>
#include <string_view>

// One FNV-1a. Six translation units had grown their own byte-identical copy of
// the offset basis, the prime and the accumulate loop (compare model line
// hashing, branch-review hunk ids, syntax-definition digests, project state
// directory names, tab-strip fingerprints, overview-ruler theme keys). They are
// all the same function, so they are one function now; every helper here is
// constexpr and inline, so the migration costs nothing at runtime.
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

// Folds a 64-bit value in little-endian byte order, so a fingerprint built from
// mixed strings and numbers stays stable across builds.
constexpr std::uint64_t Fnv1aValue(std::uint64_t hash, std::uint64_t value) {
  for (int shift = 0; shift < 64; shift += 8) {
    hash = Fnv1aByte(hash, static_cast<unsigned char>((value >> shift) & 0xFFULL));
  }
  return hash;
}

constexpr std::uint64_t Fnv1aHash(std::string_view bytes) {
  return Fnv1aBytes(kFnv1aOffsetBasis, bytes);
}

}  // namespace microide::util
