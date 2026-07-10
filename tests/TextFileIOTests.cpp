#include "TestSupport.h"

#include "util/TextFileIO.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

namespace microide::tests {
namespace {

using microide::util::kMaxTextFileBytes;
using microide::util::ReadFileForTextSearch;
using microide::util::ReadTextFile;
using microide::util::ReadTextFileClassified;
using microide::util::TextFileReadStatus;

// A normal small text file round-trips.
void TestReadTextFileRoundTrip() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path path = temp_dir.path() / "hello.txt";
  WriteFile(path, "line one\nline two\n");

  const auto content = ReadTextFile(path);
  Expect(content.has_value(), "a normal file should read");
  Expect(*content == "line one\nline two\n", "content should round-trip verbatim");
}

// Binary / NUL-laden content is returned as bytes (classification happens later);
// the reader itself must not crash or truncate at the NUL.
void TestReadTextFileAcceptsBinaryBytes() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path path = temp_dir.path() / "binary.bin";
  std::string bytes;
  bytes.push_back('\x00');
  bytes.push_back('\xFF');
  bytes.append("\x01\x02mixed\x00tail", 12);
  WriteFile(path, bytes);

  const auto content = ReadTextFile(path);
  Expect(content.has_value(), "a binary file should still be read into memory");
  Expect(content->size() == bytes.size(), "all bytes including NULs should be preserved");
}

// The OOM guard: a file whose reported size exceeds the cap is refused before any
// allocation. A sparse file (resize_file) gives us the large logical size without
// writing gigabytes to disk.
void TestReadTextFileRejectsOversize() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path path = temp_dir.path() / "huge.txt";
  WriteFile(path, "seed");
  std::error_code ec;
  std::filesystem::resize_file(path, kMaxTextFileBytes + 1, ec);
  Expect(!ec, "sparse resize should succeed");

  const auto content = ReadTextFile(path);
  Expect(!content.has_value(), "an oversize file must be refused, not allocated");
}

// Search reader: normal file reads, NUL-containing file is rejected as binary,
// and an oversize file is skipped rather than crashing the worker.
void TestReadFileForTextSearchGuards() {
  TemporaryDirectory temp_dir;

  const std::filesystem::path text_path = temp_dir.path() / "src.txt";
  WriteFile(text_path, "findable text");
  std::string out;
  Expect(ReadFileForTextSearch(text_path, out), "a text file should be searchable");
  Expect(out == "findable text", "search buffer should hold the file content");

  const std::filesystem::path bin_path = temp_dir.path() / "blob.bin";
  WriteFile(bin_path, std::string("abc\0def", 7));
  Expect(!ReadFileForTextSearch(bin_path, out), "a NUL-containing file is treated as binary");

  const std::filesystem::path huge_path = temp_dir.path() / "huge.txt";
  WriteFile(huge_path, "seed");
  std::error_code ec;
  std::filesystem::resize_file(huge_path, kMaxTextFileBytes + 1, ec);
  Expect(!ec, "sparse resize should succeed");
  Expect(!ReadFileForTextSearch(huge_path, out), "an oversize file must be skipped");
}

// H11/J41: the classifying reader distinguishes absent, unreadable, binary, and
// oversized files so callers (e.g. compare's working-tree side) map only true
// absence to empty content instead of masking a real error as a deleted file.
void TestReadTextFileClassifiedDistinguishesCases() {
  TemporaryDirectory temp_dir;

  const std::filesystem::path text_path = temp_dir.path() / "ok.txt";
  WriteFile(text_path, "hello\nworld\n");
  const auto ok = ReadTextFileClassified(text_path);
  Expect(ok.status == TextFileReadStatus::Ok && ok.ok(), "a normal text file classifies as Ok");
  Expect(ok.content == "hello\nworld\n", "Ok content round-trips");
  Expect(!ok.is_error() && !ok.missing(), "Ok is neither an error nor missing");

  const std::filesystem::path missing_path = temp_dir.path() / "does_not_exist.txt";
  const auto missing = ReadTextFileClassified(missing_path);
  Expect(missing.status == TextFileReadStatus::Missing && missing.missing(),
         "an absent file classifies as Missing");
  Expect(missing.content.empty() && !missing.is_error(),
         "Missing is an empty, non-error state (a legitimate deleted side)");

  // Binary: early AND late NUL bytes (J41) must both classify as Binary.
  const std::filesystem::path early_nul = temp_dir.path() / "early.bin";
  WriteFile(early_nul, std::string("\0abcdef", 7));
  Expect(ReadTextFileClassified(early_nul).status == TextFileReadStatus::Binary,
         "an early NUL byte classifies as Binary");
  const std::filesystem::path late_nul = temp_dir.path() / "late.bin";
  WriteFile(late_nul, std::string("abcdef\0", 7));
  const auto late = ReadTextFileClassified(late_nul);
  Expect(late.status == TextFileReadStatus::Binary && late.is_error(),
         "a late NUL byte still classifies as Binary and is an error state");
  Expect(late.content.empty(), "Binary content is not surfaced as text");

  const std::filesystem::path huge_path = temp_dir.path() / "huge.txt";
  WriteFile(huge_path, "seed");
  std::error_code ec;
  std::filesystem::resize_file(huge_path, kMaxTextFileBytes + 1, ec);
  Expect(!ec, "sparse resize should succeed");
  const auto huge = ReadTextFileClassified(huge_path);
  Expect(huge.status == TextFileReadStatus::TooLarge && huge.is_error(),
         "an oversize file classifies as TooLarge and is an error state");

  // Unreadable: a file that exists but cannot be opened. chmod 000 is bypassed by
  // root, so guard the assertion when the harness runs privileged.
  const std::filesystem::path locked_path = temp_dir.path() / "locked.txt";
  WriteFile(locked_path, "secret");
  std::filesystem::permissions(locked_path, std::filesystem::perms::none,
                               std::filesystem::perm_options::replace, ec);
  if (!ec) {
    std::ifstream probe(locked_path, std::ios::binary);
    const bool actually_locked = !probe.good();
    probe.close();
    if (actually_locked) {
      const auto locked = ReadTextFileClassified(locked_path);
      Expect(locked.status == TextFileReadStatus::Unreadable && locked.is_error(),
             "an existing but unreadable file classifies as Unreadable, not Missing");
    }
    // Restore permissions so the temp dir can be cleaned up.
    std::filesystem::permissions(locked_path, std::filesystem::perms::owner_all,
                                 std::filesystem::perm_options::replace, ec);
  }
}

}  // namespace

void RegisterTextFileIOTests(std::vector<TestCase>& tests) {
  AddTest(tests, "TextFileIO/ReadTextFileRoundTrip", TestReadTextFileRoundTrip);
  AddTest(tests, "TextFileIO/ReadTextFileAcceptsBinaryBytes", TestReadTextFileAcceptsBinaryBytes);
  AddTest(tests, "TextFileIO/ReadTextFileRejectsOversize", TestReadTextFileRejectsOversize);
  AddTest(tests, "TextFileIO/ReadFileForTextSearchGuards", TestReadFileForTextSearchGuards);
  AddTest(tests, "TextFileIO/ReadTextFileClassifiedDistinguishesCases",
          TestReadTextFileClassifiedDistinguishesCases);
}

}  // namespace microide::tests
