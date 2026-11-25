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
  const std::uint64_t id = service.Begin({.path = file, .on_complete = collector.Sink()});
  Expect(id != 0, "a posted read has a non-zero id");
  service.FlushPendingReads();

  Expect(collector.completions.size() == 1, "one completion per posted read");
  const FileReadService::Completion& completion = collector.completions.front();
  Expect(completion.id == id, "the completion carries the id Begin returned");
  Expect(completion.path == file, "and the path it was posted for");
  Expect(completion.status == Status::Ok && completion.bytes == contents,
         "the reader hands back the file's bytes unchanged");
}

void TestAnEmptyFileReadsAsEmptyAndSucceeds() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path file = temp_dir.path() / "empty.txt";
  WriteFile(file, "");

  FileReadService service;
  Collector collector;
  service.Begin({.path = file, .on_complete = collector.Sink()});
  service.FlushPendingReads();

  Expect(collector.completions.size() == 1, "one completion");
  Expect(collector.completions.front().status == Status::Ok,
         "an empty file is a successful read, not a failure");
  Expect(collector.completions.front().bytes.empty(), "with no bytes");
}

void TestUnreadableAndOversizedAreDistinctFromOk() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path missing = temp_dir.path() / "nope.txt";
  const std::filesystem::path directory = temp_dir.path() / "subdir";
  std::filesystem::create_directories(directory);
  const std::filesystem::path small = temp_dir.path() / "small.txt";
  WriteFile(small, "0123456789");

  FileReadService service;
  Collector collector;
  service.Begin({.path = missing, .on_complete = collector.Sink()});
  service.Begin({.path = directory, .on_complete = collector.Sink()});
  service.Begin({.path = small, .on_complete = collector.Sink(), .max_bytes = 4});
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
    ids.push_back(service.Begin({.path = file, .on_complete = collector.Sink()}));
  }
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

void TestCancellingAnUnknownIdIsHarmless() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path file = temp_dir.path() / "main.txt";
  WriteFile(file, "hello");

  FileReadService service;
  Collector collector;
  service.Cancel(0);
  service.Cancel(999999);
  const std::uint64_t id = service.Begin({.path = file, .on_complete = collector.Sink()});
  service.FlushPendingReads();
  service.Cancel(id);

  Expect(collector.completions.size() == 1, "the real read still completed once");
  Expect(collector.completions.front().status == Status::Ok, "and succeeded");
}

void TestANoReadRequestRunsItsHookWithoutTouchingThePath() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path missing = temp_dir.path() / "not-there.txt";

  FileReadService service;
  Collector collector;
  bool hook_ran = false;
  bool hook_saw_empty = false;
  service.Begin({.path = missing,
                 .on_worker =
                     [&](std::string& bytes) {
                       hook_ran = true;
                       hook_saw_empty = bytes.empty();
                     },
                 .on_complete = collector.Sink(),
                 .read_path = false});
  service.FlushPendingReads();

  Expect(collector.completions.size() == 1, "one completion");
  Expect(collector.completions.front().status == Status::Ok,
         "a load with nothing to read is Ok, not Unreadable");
  Expect(collector.completions.front().path == missing, "and still names its path");
  Expect(hook_ran && hook_saw_empty, "the hook ran, with no bytes");
}

void TestAFlushNeverReturnsBeforeTheCompletionLands() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path missing = temp_dir.path() / "not-there.txt";
  FileReadService service;
  int misses = 0;
  for (int attempt = 0; attempt < 2000; ++attempt) {
    Collector collector;
    service.Begin({.path = missing, .on_complete = collector.Sink(), .read_path = false});
    while (service.PendingCount() != 0) {
    }
    service.FlushPendingReads();
    if (collector.completions.size() != 1) {
      ++misses;
      service.FlushPendingReads();
      (void)service.DrainCompletions();
    }
  }
  Expect(misses == 0, "every flush should deliver its read's completion; " + std::to_string(misses) +
                          " of 2000 returned first");
}

}

void RegisterFileReadServiceTests(std::vector<TestCase>& tests) {
  AddTest(tests, "FileReadService/AReadReturnsTheBytes", TestAReadReturnsTheBytes);
  AddTest(tests, "FileReadService/AFlushNeverReturnsBeforeTheCompletionLands",
          TestAFlushNeverReturnsBeforeTheCompletionLands);
  AddTest(tests, "FileReadService/AnEmptyFileReadsAsEmptyAndSucceeds",
          TestAnEmptyFileReadsAsEmptyAndSucceeds);
  AddTest(tests, "FileReadService/UnreadableAndOversizedAreDistinctFromOk",
          TestUnreadableAndOversizedAreDistinctFromOk);
  AddTest(tests, "FileReadService/EveryPostedReadIsReportedExactlyOnce",
          TestEveryPostedReadIsReportedExactlyOnce);
  AddTest(tests, "FileReadService/ANoReadRequestRunsItsHookWithoutTouchingThePath",
          TestANoReadRequestRunsItsHookWithoutTouchingThePath);
  AddTest(tests, "FileReadService/CancellingAnUnknownIdIsHarmless",
          TestCancellingAnUnknownIdIsHarmless);
}

}
