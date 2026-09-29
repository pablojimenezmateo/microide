// The dedicated off-thread file reader behind asynchronous open.
//
// What matters here is the accounting, not the timing: every posted read must
// produce exactly one completion whatever happens to it, and no completion that
// is not `Ok` may hand back bytes — a partially-read buffer sitting in `bytes`
// is one missed branch away from being opened as the document. The tests are
// written to be deterministic under cancellation races (a cancel may or may not
// beat a fast local read), so they assert the invariant rather than the winner.

#include "TestSupport.h"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "project/FileReadService.h"

namespace microide::tests {
namespace {

using microide::project::FileReadService;
using Status = microide::project::FileReadService::Status;

// Collects completions on the calling (shell) thread, which is where the
// mailbox runs them.
struct Collector {
  std::vector<FileReadService::Completion> completions;

  std::function<void(FileReadService::Completion)> Sink() {
    return [this](FileReadService::Completion completion) {
      completions.push_back(std::move(completion));
    };
  }
};

void TestAReadReturnsTheBytes() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path file = temp_dir.path() / "main.cpp";
  const std::string contents = "int main() { return 0; }\nsecond line\r\nthird\n";
  WriteFile(file, contents);

  FileReadService service;
  Collector collector;
  const std::uint64_t id = service.Begin(file, collector.Sink());
  Expect(id != 0, "a posted read has a non-zero id");
  service.FlushPendingReads();

  Expect(collector.completions.size() == 1, "one completion per posted read");
  const FileReadService::Completion& completion = collector.completions.front();
  Expect(completion.id == id, "the completion carries the id Begin returned");
  Expect(completion.path == file, "and the path it was posted for");
  // The bytes are verbatim: the CRLF is still there, because classification
  // happens on the shell thread against exactly what was on disk.
  Expect(completion.status == Status::Ok && completion.bytes == contents,
         "the reader hands back the file's bytes unchanged");
}

void TestAnEmptyFileReadsAsEmptyAndSucceeds() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path file = temp_dir.path() / "empty.txt";
  WriteFile(file, "");

  FileReadService service;
  Collector collector;
  service.Begin(file, collector.Sink());
  service.FlushPendingReads();

  Expect(collector.completions.size() == 1, "one completion");
  Expect(collector.completions.front().status == Status::Ok,
         "an empty file is a successful read, not a failure");
  Expect(collector.completions.front().bytes.empty(), "with no bytes");
}

// A missing path, a directory and a file above the cap are three different
// answers, and collapsing them would make the tab say the wrong thing.
void TestUnreadableAndOversizedAreDistinctFromOk() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path missing = temp_dir.path() / "nope.txt";
  const std::filesystem::path directory = temp_dir.path() / "subdir";
  std::filesystem::create_directories(directory);
  const std::filesystem::path small = temp_dir.path() / "small.txt";
  WriteFile(small, "0123456789");

  FileReadService service;
  Collector collector;
  service.Begin(missing, collector.Sink());
  service.Begin(directory, collector.Sink());
  service.Begin(small, collector.Sink(), /*max_bytes=*/4);
  service.FlushPendingReads();

  Expect(collector.completions.size() == 3, "three posted reads, three completions");
  Expect(collector.completions[0].status == Status::Unreadable, "an absent path is unreadable");
  Expect(collector.completions[1].status == Status::Unreadable, "a directory is unreadable");
  Expect(collector.completions[2].status == Status::TooLarge,
         "a file over the cap is refused as too large, not as unreadable");
  for (const FileReadService::Completion& completion : collector.completions) {
    Expect(completion.bytes.empty(), "a failed read hands back no bytes at all");
  }
}

// The accounting invariant under cancellation. Whether a cancel beats a read on
// this machine is a race; that each read is reported exactly once, and that a
// non-Ok completion carries no bytes, is not.
void TestEveryPostedReadIsReportedExactlyOnce() {
  TemporaryDirectory temp_dir;
  std::vector<std::filesystem::path> files;
  for (int i = 0; i < 8; ++i) {
    const std::filesystem::path file = temp_dir.path() / ("file" + std::to_string(i) + ".txt");
    WriteFile(file, std::string(4096, 'a' + i));
    files.push_back(file);
  }

  FileReadService service;
  Collector collector;
  std::vector<std::uint64_t> ids;
  for (const std::filesystem::path& file : files) {
    ids.push_back(service.Begin(file, collector.Sink()));
  }
  // Cancel half of them by id, then cancel the rest wholesale on flush.
  for (std::size_t i = 0; i < ids.size(); i += 2) {
    service.Cancel(ids[i]);
  }
  service.CancelAllAndFlush();

  Expect(collector.completions.size() == files.size(),
         "a cancelled read still reports, so the caller's bookkeeping always clears");
  Expect(service.PendingCount() == 0, "and nothing is left outstanding");
  std::vector<std::uint64_t> seen;
  for (const FileReadService::Completion& completion : collector.completions) {
    Expect(completion.status == Status::Ok || completion.status == Status::Cancelled,
           "a readable file either reads or is cancelled");
    Expect(completion.status == Status::Ok || completion.bytes.empty(),
           "a cancelled read hands back no partial buffer");
    seen.push_back(completion.id);
  }
  std::sort(seen.begin(), seen.end());
  Expect(std::adjacent_find(seen.begin(), seen.end()) == seen.end(),
         "no read is reported twice");
}

// Cancelling an id the service has never heard of, or has already finished, must
// be a no-op rather than corrupting the next read's bookkeeping.
void TestCancellingAnUnknownIdIsHarmless() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path file = temp_dir.path() / "main.txt";
  WriteFile(file, "hello");

  FileReadService service;
  Collector collector;
  service.Cancel(0);
  service.Cancel(999999);
  const std::uint64_t id = service.Begin(file, collector.Sink());
  service.FlushPendingReads();
  service.Cancel(id);  // already finished

  Expect(collector.completions.size() == 1, "the real read still completed once");
  Expect(collector.completions.front().status == Status::Ok, "and succeeded");
}

}  // namespace

void RegisterFileReadServiceTests(std::vector<TestCase>& tests) {
  AddTest(tests, "FileReadService/AReadReturnsTheBytes", TestAReadReturnsTheBytes);
  AddTest(tests, "FileReadService/AnEmptyFileReadsAsEmptyAndSucceeds",
          TestAnEmptyFileReadsAsEmptyAndSucceeds);
  AddTest(tests, "FileReadService/UnreadableAndOversizedAreDistinctFromOk",
          TestUnreadableAndOversizedAreDistinctFromOk);
  AddTest(tests, "FileReadService/EveryPostedReadIsReportedExactlyOnce",
          TestEveryPostedReadIsReportedExactlyOnce);
  AddTest(tests, "FileReadService/CancellingAnUnknownIdIsHarmless",
          TestCancellingAnUnknownIdIsHarmless);
}

}  // namespace microide::tests
