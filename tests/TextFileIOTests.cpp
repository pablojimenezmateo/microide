#include "TestSupport.h"

#include "util/TextFileIO.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>

namespace microide::tests {
namespace {

using microide::util::kMaxTextFileBytes;
using microide::util::ReadFileForTextSearch;
using microide::util::ReadTextFile;

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

}  // namespace

void RegisterTextFileIOTests(std::vector<TestCase>& tests) {
  AddTest(tests, "TextFileIO/ReadTextFileRoundTrip", TestReadTextFileRoundTrip);
  AddTest(tests, "TextFileIO/ReadTextFileAcceptsBinaryBytes", TestReadTextFileAcceptsBinaryBytes);
  AddTest(tests, "TextFileIO/ReadTextFileRejectsOversize", TestReadTextFileRejectsOversize);
  AddTest(tests, "TextFileIO/ReadFileForTextSearchGuards", TestReadFileForTextSearchGuards);
}

}  // namespace microide::tests
