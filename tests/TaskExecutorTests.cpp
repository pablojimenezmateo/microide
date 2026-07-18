#include "TestSupport.h"

#include "util/TaskExecutor.h"

#include <atomic>
#include <chrono>
#include <mutex>
#include <stdexcept>
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

// TD-2026-07-16-09: a task that throws must not terminate the process or hang
// WaitForIdle; the worker must keep draining and later tasks must still run.
void TestTaskExecutorThrowingTaskDoesNotKillWorker() {
  TaskExecutor executor;
  std::atomic<bool> normal_ran = false;

  executor.Submit(
      [&](const CancellationToken&) { throw std::runtime_error("boom from a task"); });
  executor.Submit([&](const CancellationToken&) { normal_ran.store(true); });

  executor.WaitForIdle();  // must return despite the throwing task
  Expect(normal_ran.load(),
         "a task submitted after a throwing task must still run (worker survived)");
}

// TD-2026-07-17A-005: a keyed (coalescing) submit must drop still-queued work
// for the same key so a superseding request replaces obsolete queued tasks
// instead of leaving them to wake the worker only to early-out on staleness.
void TestTaskExecutorCoalescesQueuedTasksByKey() {
  TaskExecutor executor;  // single worker: keeps later submits queued behind one
  std::atomic<bool> started = false;
  std::atomic<bool> gate = false;
  std::atomic<int> stale_runs = 0;
  std::atomic<int> latest_runs = 0;
  std::atomic<bool> other_ran = false;

  // Occupy the single worker so subsequent submits sit in the queue where
  // coalescing can act on them.
  executor.Submit([&](const CancellationToken&) {
    started.store(true);
    while (!gate.load()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  });

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
  while (!started.load() && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  Expect(started.load(), "blocker task should occupy the worker before we queue coalescing work");

  // Two superseded "file:A" tasks then the latest; only the latest survives.
  executor.Submit("file:A", [&](const CancellationToken& token) {
    if (token.IsCancellationRequested()) return;
    stale_runs.fetch_add(1);
  });
  executor.Submit("file:A", [&](const CancellationToken& token) {
    if (token.IsCancellationRequested()) return;
    stale_runs.fetch_add(1);
  });
  executor.Submit("file:A", [&](const CancellationToken&) { latest_runs.fetch_add(1); });
  // A different key must not be coalesced away.
  executor.Submit("file:B", [&](const CancellationToken&) { other_ran.store(true); });

  gate.store(true);
  executor.WaitForIdle();

  Expect(stale_runs.load() == 0,
         "coalesced-away queued tasks for a key must not run");
  Expect(latest_runs.load() == 1,
         "the latest coalescing task for a key must run exactly once");
  Expect(other_ran.load(), "a task with a different coalesce key must not be dropped");
}

}  // namespace

void RegisterTaskExecutorTests(std::vector<TestCase>& tests) {
  AddTest(tests, "TaskExecutor/ThrowingTaskDoesNotKillWorker",
          TestTaskExecutorThrowingTaskDoesNotKillWorker);
  AddTest(tests, "TaskExecutor/RunsSubmittedTasksInOrder",
          TestTaskExecutorRunsSubmittedTasksInOrder);
  AddTest(tests, "TaskExecutor/CancelsActiveWorkAndDropsQueuedTasks",
          TestTaskExecutorCancelsActiveWorkAndDropsQueuedTasks);
  AddTest(tests, "TaskExecutor/CoalescesQueuedTasksByKey",
          TestTaskExecutorCoalescesQueuedTasksByKey);
}

}  // namespace microide::tests
