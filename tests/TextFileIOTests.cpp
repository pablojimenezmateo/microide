#include "TestSupport.h"

#include "util/TextFileIO.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

#if defined(__unix__) || defined(__APPLE__)
#include <sys/stat.h>
#include <sys/types.h>
#endif

namespace microide::tests {
namespace {

using microide::util::kMaxTextFileBytes;
using microide::util::ReadFileForTextSearch;
using microide::util::ReadFileLineWindow;
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

// The explicit max_bytes parameter caps the read at the boundary, and the search
// default (kMaxSearchFileBytes) is tighter than the whole-file cap so N search
// workers cannot together hold multiple gigabytes. Uses a tiny cap so the test is
// deterministic and cheap (no gigabyte allocations).
void TestReadFileForTextSearchRespectsMaxBytes() {
  static_assert(microide::util::kMaxSearchFileBytes < kMaxTextFileBytes,
                "search cap must be tighter than the whole-file cap");

  TemporaryDirectory temp_dir;
  const std::filesystem::path path = temp_dir.path() / "capped.txt";
  const std::string content(64, 'x');  // 64 bytes
  WriteFile(path, content);

  std::string out;
  Expect(ReadFileForTextSearch(path, out, 64), "a file exactly at the cap is read");
  Expect(out == content, "the at-cap read returns the exact bytes");
  Expect(ReadFileForTextSearch(path, out, 128), "a file under the cap is read");
  Expect(!ReadFileForTextSearch(path, out, 63), "a file one byte over the cap is skipped");
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

// TD-2026-07-17-039: every text-read entry point must reject a non-regular path
// (directory, FIFO, device) BEFORE opening it. Opening a special file with
// ifstream can block on open/seek before the size guard runs, so the read must
// fail cleanly and quickly on a directory everywhere, and on a FIFO on POSIX.
void TestReadTextFileRejectsNonRegularFiles() {
  TemporaryDirectory temp_dir;

  // A directory is a non-regular path on every platform.
  const std::filesystem::path dir_path = temp_dir.path() / "a_directory";
  std::error_code ec;
  std::filesystem::create_directory(dir_path, ec);
  Expect(!ec, "creating the probe directory should succeed");

  Expect(!ReadTextFile(dir_path).has_value(), "ReadTextFile must reject a directory");
  Expect(ReadTextFileClassified(dir_path).status == TextFileReadStatus::Unreadable,
         "ReadTextFileClassified classifies a directory as Unreadable, not Ok/Missing");
  std::string out;
  Expect(!ReadFileForTextSearch(dir_path, out), "the search reader must reject a directory");

#if defined(__unix__) || defined(__APPLE__)
  // A FIFO is the canonical block-on-open hazard. mkfifo is POSIX-only.
  const std::filesystem::path fifo_path = temp_dir.path() / "a_fifo";
  if (::mkfifo(fifo_path.c_str(), 0600) == 0) {
    Expect(!ReadTextFile(fifo_path).has_value(),
           "ReadTextFile must reject a FIFO without blocking on open");
    Expect(ReadTextFileClassified(fifo_path).status == TextFileReadStatus::Unreadable,
           "ReadTextFileClassified must classify a FIFO as Unreadable, not open it");
    Expect(!ReadFileForTextSearch(fifo_path, out),
           "the search reader must reject a FIFO without blocking");
    std::filesystem::remove(fifo_path, ec);
  }
#endif
}

// TD-2026-07-17A-024: the bounded line-window reader returns exactly the
// requested 1-based range, strips CRLF, and never materializes the whole file.
void TestReadFileLineWindowReturnsRequestedRange() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path path = temp_dir.path() / "src.txt";
  WriteFile(path, "one\r\ntwo\r\nthree\r\nfour\r\nfive\r\n");

  const auto window = ReadFileLineWindow(path, 2, 4);
  Expect(window.size() == 3, "window should return exactly lines [2,4]");
  Expect(window[0] == "two", "first window line should be line 2 with CR stripped");
  Expect(window[1] == "three", "second window line should be line 3");
  Expect(window[2] == "four", "third window line should be line 4");
}

// A single target line (first==last) works, including the very first line.
void TestReadFileLineWindowSingleLine() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path path = temp_dir.path() / "src.txt";
  WriteFile(path, "alpha\nbeta\ngamma\n");

  const auto first = ReadFileLineWindow(path, 1, 1);
  Expect(first.size() == 1 && first[0] == "alpha", "line 1 window should be just 'alpha'");
  const auto mid = ReadFileLineWindow(path, 2, 2);
  Expect(mid.size() == 1 && mid[0] == "beta", "line 2 window should be just 'beta'");
}

// A final line with no trailing newline is still returned; a range past EOF
// yields only the lines that exist.
void TestReadFileLineWindowHandlesEofAndOverrun() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path path = temp_dir.path() / "src.txt";
  WriteFile(path, "first\nsecond\nlast-no-newline");

  const auto tail = ReadFileLineWindow(path, 3, 3);
  Expect(tail.size() == 1 && tail[0] == "last-no-newline",
         "a final newline-less line must still be returned");
  const auto past = ReadFileLineWindow(path, 4, 6);
  Expect(past.empty(), "a range entirely past EOF yields no lines");
  const auto straddle = ReadFileLineWindow(path, 2, 10);
  Expect(straddle.size() == 2, "a range straddling EOF returns only existing lines");
}

// The byte budget bounds the scan: a target line beyond the budget is not
// materialized, so the reader degrades to no snippet instead of slurping the file.
void TestReadFileLineWindowRespectsByteBudget() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path path = temp_dir.path() / "big.txt";
  std::string content;
  for (int i = 0; i < 5000; ++i) {
    content += "0123456789abcdef\n";  // 17 bytes/line
  }
  WriteFile(path, content);

  // Line 4000 sits well past a 1 KiB budget; the reader must give up cleanly.
  const auto capped = ReadFileLineWindow(path, 3999, 4001, /*max_bytes=*/1024);
  Expect(capped.empty(), "a target beyond the byte budget yields an empty window");
  // The same line resolves when the budget covers it.
  const auto full = ReadFileLineWindow(path, 3999, 4001, /*max_bytes=*/kMaxTextFileBytes);
  Expect(full.size() == 3, "with an ample budget the deep window resolves");
  Expect(full[1] == "0123456789abcdef", "the deep line content should match");
}

// Regression: a window that ends inside the FIRST chunk of a multi-chunk file must
// stop there. The reader used to break only the inner byte loop without advancing
// line_number, so the outer loop kept pulling 64 KiB chunks to EOF and flushed one
// spurious extra line per chunk into the window.
void TestReadFileLineWindowStopsAtLastLineInLargeFile() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path path = temp_dir.path() / "multichunk.txt";
  std::string content;
  // 20,000 x 17 bytes = 340 KB, comfortably more than five 64 KiB read chunks.
  for (int i = 0; i < 20000; ++i) {
    content += "0123456789abcdef\n";
  }
  WriteFile(path, content);

  const auto window = ReadFileLineWindow(path, 2, 3);
  Expect(window.size() == 2, "a 2-line window must not grow with the file's size");
  Expect(window[0] == "0123456789abcdef", "window line 2 content");
  Expect(window[1] == "0123456789abcdef", "window line 3 content");

  // The stop is also a byte-budget stop: with a budget that only covers the first
  // few lines the window still resolves, proving the scan does not run past them.
  const auto bounded = ReadFileLineWindow(path, 1, 3, /*max_bytes=*/17 * 3);
  Expect(bounded.size() == 3, "lines 1-3 fit exactly in a 51-byte budget");
}

// A file whose scanned prefix contains a NUL is treated as binary -> empty window.
void TestReadFileLineWindowRejectsBinary() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path path = temp_dir.path() / "bin.dat";
  std::string bytes("line one\nli");
  bytes.push_back('\0');
  bytes.append("ne two\nthree\n");
  WriteFile(path, bytes);

  const auto window = ReadFileLineWindow(path, 1, 3);
  Expect(window.empty(), "a binary prefix must yield an empty window, not garbage");
}

}  // namespace

void RegisterTextFileIOTests(std::vector<TestCase>& tests) {
  AddTest(tests, "TextFileIO/ReadTextFileRoundTrip", TestReadTextFileRoundTrip);
  AddTest(tests, "TextFileIO/ReadTextFileAcceptsBinaryBytes", TestReadTextFileAcceptsBinaryBytes);
  AddTest(tests, "TextFileIO/ReadTextFileRejectsOversize", TestReadTextFileRejectsOversize);
  AddTest(tests, "TextFileIO/ReadFileForTextSearchGuards", TestReadFileForTextSearchGuards);
  AddTest(tests, "TextFileIO/ReadFileForTextSearchRespectsMaxBytes",
          TestReadFileForTextSearchRespectsMaxBytes);
  AddTest(tests, "TextFileIO/ReadTextFileClassifiedDistinguishesCases",
          TestReadTextFileClassifiedDistinguishesCases);
  AddTest(tests, "TextFileIO/ReadFileLineWindowReturnsRequestedRange",
          TestReadFileLineWindowReturnsRequestedRange);
  AddTest(tests, "TextFileIO/ReadFileLineWindowSingleLine",
          TestReadFileLineWindowSingleLine);
  AddTest(tests, "TextFileIO/ReadFileLineWindowHandlesEofAndOverrun",
          TestReadFileLineWindowHandlesEofAndOverrun);
  AddTest(tests, "TextFileIO/ReadFileLineWindowRespectsByteBudget",
          TestReadFileLineWindowRespectsByteBudget);
  AddTest(tests, "TextFileIO/ReadFileLineWindowStopsAtLastLineInLargeFile",
          TestReadFileLineWindowStopsAtLastLineInLargeFile);
  AddTest(tests, "TextFileIO/ReadFileLineWindowRejectsBinary",
          TestReadFileLineWindowRejectsBinary);
  AddTest(tests, "TextFileIO/ReadTextFileRejectsNonRegularFiles",
          TestReadTextFileRejectsNonRegularFiles);
}

}  // namespace microide::tests
