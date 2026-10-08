#include "TestSupport.h"

#include "util/ContentHash.h"

#include <string>

namespace microide::tests {
namespace {

using util::ContentHash;
using util::ContentHasher;
using util::HashContent;
using util::HashFileContent;

// The official BLAKE3 test-vector input: bytes 0..250 repeating.
std::string VectorInput(std::size_t length) {
  std::string input(length, '\0');
  for (std::size_t i = 0; i < length; ++i) {
    input[i] = static_cast<char>(i % 251);
  }
  return input;
}

// Upstream test_vectors.json (1.5.4), first 32 bytes of each `hash`. Lengths that
// straddle the 1 KiB chunk and the multi-chunk tree so the SIMD backends' many-chunk
// paths are on the line, not only the portable single-chunk one.
void TestContentHashKnownAnswers() {
  struct Vector {
    std::size_t length;
    const char* hex;
  };
  constexpr Vector kVectors[] = {
      {0, "af1349b9f5f9a1a6a0404dea36dcc9499bcb25c9adc112b7cc9a93cae41f3262"},
      {1, "2d3adedff11b61f14c886e35afa036736dcd87a74d27b5c1510225d0f592e213"},
      {1023, "10108970eeda3eb932baac1428c7a2163b0e924c9a9e25b35bba72b28f70bd11"},
      {1024, "42214739f095a406f3fc83deb889744ac00df831c10daa55189b5d121c855af7"},
      {1025, "d00278ae47eb27b34faecf67b4fe263f82d5412916c1ffd97c8cb7fb814b8444"},
      {2048, "e776b6028c7cd22a4d0ba182a8bf62205d2ef576467e838ed6f2529b85fba24a"},
      {8192, "aae792484c8efe4f19e2ca7d371d8c467ffb10748d8a5a1ae579948f718a2a63"},
      {31744, "62b6960e1a44bcc1eb1a611a8d6235b6b4b78f32e7abc4fb4c6cdcce94895c47"},
      {102400, "bc3e3d41a1146b069abffad3c0d44860cf664390afce4d9661f7902e7943e085"},
  };
  for (const Vector& vector : kVectors) {
    const std::string input = VectorInput(vector.length);
    Expect(HashContent(input).Hex() == vector.hex,
           "BLAKE3 of the " + std::to_string(vector.length) + "-byte vector matches upstream");
    // Streaming in uneven pieces must agree with the one-shot hash.
    ContentHasher hasher;
    for (std::size_t offset = 0; offset < input.size(); offset += 777) {
      hasher.Update(std::string_view(input).substr(offset, 777));
    }
    Expect(hasher.Finish().Hex() == vector.hex,
           "streamed BLAKE3 of the " + std::to_string(vector.length) + "-byte vector matches");
  }
}

void TestContentHashHexAndRawRoundTrip() {
  const ContentHash hash = HashContent("hello");
  Expect(ContentHash::FromHex(hash.Hex()) == hash, "hex round-trips");
  Expect(ContentHash::FromRaw(hash.raw()) == hash, "raw round-trips");
  Expect(!ContentHash::FromHex("xyz").has_value(), "short hex is refused");
  Expect(!ContentHash::FromHex(std::string(64, 'g')).has_value(), "non-hex digits are refused");
  Expect(!ContentHash::FromRaw(std::string(31, 'a')).has_value(), "31 raw bytes are refused");
}

void TestContentHashFileMatchesInMemory() {
  TemporaryDirectory temp_dir;
  const std::string content = VectorInput(200 * 1024 + 3);
  WriteFile(temp_dir.path() / "f.bin", content);
  std::uint64_t size = 0;
  const auto file_hash = HashFileContent(temp_dir.path() / "f.bin", &size);
  Expect(file_hash.has_value() && *file_hash == HashContent(content),
         "a file hashes to the hash of its bytes");
  Expect(size == content.size(), "the hashed size is reported");
  Expect(!HashFileContent(temp_dir.path()).has_value(), "a directory has no content hash");
  Expect(!HashFileContent(temp_dir.path() / "missing").has_value(), "a missing file has none");
}

}  // namespace

void RegisterContentHashTests(std::vector<TestCase>& tests) {
  AddTest(tests, "ContentHash/KnownAnswers", TestContentHashKnownAnswers);
  AddTest(tests, "ContentHash/HexAndRawRoundTrip", TestContentHashHexAndRawRoundTrip);
  AddTest(tests, "ContentHash/FileMatchesInMemory", TestContentHashFileMatchesInMemory);
}

}  // namespace microide::tests
