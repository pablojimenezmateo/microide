#include "TestSupport.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include "project/FileWriteService.h"

namespace microide::tests {
namespace {

using microide::project::FileWriteService;

void TestWritesRunInTheOrderTheyWereAskedForAndFinishOnTheCallersThread() {
  FileWriteService service;
  std::vector<int> ran;
  std::vector<int> finished;
  const std::thread::id caller = std::this_thread::get_id();
  bool finished_on_caller = true;
  for (int i = 0; i < 5; ++i) {
    service.Begin([&ran, &finished, &finished_on_caller, caller, i]() -> FileWriteService::Continuation {
      std::this_thread::sleep_for(std::chrono::milliseconds(5 - i));
      ran.push_back(i);
      return [&finished, &finished_on_caller, caller, i] {
        finished_on_caller = finished_on_caller && std::this_thread::get_id() == caller;
        finished.push_back(i);
      };
    });
  }
  service.Flush();
  Expect(ran == std::vector<int>({0, 1, 2, 3, 4}), "one writer, in order: a later save never lands first");
  Expect(finished == ran, "every write's continuation ran");
  Expect(finished_on_caller, "on the thread that flushed, never the writer's");
  Expect(service.PendingCount() == 0 && !service.HasCompletions(), "nothing left");
}

void TestAContinuationWaitsForTheDrain() {
  FileWriteService service;
  std::atomic<bool> written{false};
  bool applied = false;
  service.Begin([&written, &applied]() -> FileWriteService::Continuation {
    written.store(true);
    return [&applied] { applied = true; };
  });
  for (int i = 0; i < 200 && !service.HasCompletions(); ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  Expect(written.load() && service.HasCompletions(), "the write ran on its own");
  Expect(!applied, "and its result waits for the main thread");
  Expect(service.DrainCompletions() == 1 && applied, "which applies it at the drain");
}

void TestTearingTheServiceDownWaitsForEveryWrite() {
  std::atomic<int> written{0};
  {
    FileWriteService service;
    for (int i = 0; i < 3; ++i) {
      service.Begin([&written]() -> FileWriteService::Continuation {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        written.fetch_add(1);
        return {};
      });
    }
  }
  Expect(written.load() == 3, "a save asked for is never dropped, however soon the app goes");
}

}

void RegisterFileWriteServiceTests(std::vector<TestCase>& tests) {
  AddTest(tests, "FileWriteService/WritesRunInOrderAndFinishOnTheCallersThread",
          TestWritesRunInTheOrderTheyWereAskedForAndFinishOnTheCallersThread);
  AddTest(tests, "FileWriteService/AContinuationWaitsForTheDrain", TestAContinuationWaitsForTheDrain);
  AddTest(tests, "FileWriteService/TearingTheServiceDownWaitsForEveryWrite",
          TestTearingTheServiceDownWaitsForEveryWrite);
}

}
