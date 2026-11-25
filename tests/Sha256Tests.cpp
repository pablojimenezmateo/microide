#include "TestSupport.h"

#include "util/Sha256.h"

#include <filesystem>
#include <optional>
#include <string>

namespace microide::tests {
namespace {

using microide::util::Sha256FileHex;
using microide::util::Sha256Hex;

void TestSha256KnownAnswers() {
  Expect(Sha256Hex("") ==
             "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
         "SHA-256 of the empty string matches the standard vector");
  Expect(Sha256Hex("abc") ==
             "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
         "SHA-256 of \"abc\" matches the standard vector");
  Expect(Sha256Hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
             "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1",
         "SHA-256 of the 448-bit multi-block vector matches");
}

void TestSha256MultiBlockAndBoundary() {
  const std::string million_a(1000000, 'a');
  Expect(Sha256Hex(million_a) ==
             "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0",
         "SHA-256 of one million 'a' matches the standard long-message vector");

  const std::string block64(64, 'x');
  Expect(Sha256Hex(block64).size() == 64, "a 64-byte input hashes to a 64-hex-char digest");
}

void TestSha256FileMatchesInMemory() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path path = temp_dir.path() / "payload.bin";
  const std::string content = "the quick brown fox\n\0binary\x01\x02 tail";
  const std::string content_with_nul(content.data(), content.size());
  WriteFile(path, content_with_nul);

  const std::optional<std::string> file_digest = Sha256FileHex(path);
  Expect(file_digest.has_value(), "hashing a regular file succeeds");
  Expect(*file_digest == Sha256Hex(content_with_nul),
         "the file digest matches the in-memory digest of the same bytes");

  Expect(!Sha256FileHex(temp_dir.path()).has_value(),
         "hashing a directory path returns nullopt");
  Expect(!Sha256FileHex(temp_dir.path() / "does_not_exist").has_value(),
         "hashing a missing file returns nullopt");
}

}

void RegisterSha256Tests(std::vector<TestCase>& tests) {
  AddTest(tests, "Sha256/KnownAnswers", TestSha256KnownAnswers);
  AddTest(tests, "Sha256/MultiBlockAndBoundary", TestSha256MultiBlockAndBoundary);
  AddTest(tests, "Sha256/FileMatchesInMemory", TestSha256FileMatchesInMemory);
}

}
