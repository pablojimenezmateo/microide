#include "TestSupport.h"

#include "util/TaskExecutor.h"

#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <vector>

namespace microide::tests {
namespace {

using microide::util::CancellationToken;
using microide::util::TaskExecutor;

void TestTaskExecutorRunsSubmittedTasksInOrder() {
  TaskExecutor executor;
  std::mutex mutex;
  std::vector<int> values;

  executor.Submit([&](const CancellationToken& token) {
    Expect(!token.IsCancellationRequested(),
           "fresh task executor work should not start in a cancelled state");
    std::lock_guard lock(mutex);
    values.push_back(1);
  });
  executor.Submit([&](const CancellationToken& token) {
    Expect(!token.IsCancellationRequested(),
           "queued task executor work should stay active when not cancelled");
    std::lock_guard lock(mutex);
    values.push_back(2);
  });

  executor.WaitForIdle();
  Expect(values == std::vector<int>({1, 2}),
         "task executor should run submitted work sequentially in queue order");
}

void TestTaskExecutorCancelsActiveWorkAndDropsQueuedTasks() {
  TaskExecutor executor;
  std::atomic<bool> first_started = false;
  std::atomic<bool> first_cancelled = false;
  std::atomic<bool> second_ran = false;
  std::atomic<bool> ran_after_cancel = false;

  executor.Submit([&](const CancellationToken& token) {
    first_started.store(true);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline) {
      if (token.IsCancellationRequested()) {
        first_cancelled.store(true);
        return;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    Expect(false, "cancelled executor work should not be allowed to finish normally");
  });

  const auto start_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
  while (!first_started.load() && std::chrono::steady_clock::now() < start_deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  Expect(first_started.load(), "task executor should start active work before cancellation");

  executor.Submit([&](const CancellationToken&) { second_ran.store(true); });
  executor.CancelAll();
  executor.WaitForIdle();

  Expect(first_cancelled.load(),
         "task executor cancellation should notify the active task through its token");
  Expect(!second_ran.load(),
         "task executor cancellation should discard queued work that has not started");

  executor.Submit([&](const CancellationToken& token) {
    Expect(!token.IsCancellationRequested(),
           "task executor should accept fresh work after a cancellation cycle");
    ran_after_cancel.store(true);
  });
  executor.WaitForIdle();

  Expect(ran_after_cancel.load(),
         "task executor should remain reusable after cancelling earlier work");
}

}  // namespace

void RegisterTaskExecutorTests(std::vector<TestCase>& tests) {
  AddTest(tests, "TaskExecutor/RunsSubmittedTasksInOrder",
          TestTaskExecutorRunsSubmittedTasksInOrder);
  AddTest(tests, "TaskExecutor/CancelsActiveWorkAndDropsQueuedTasks",
          TestTaskExecutorCancelsActiveWorkAndDropsQueuedTasks);
}

}  // namespace microide::tests
