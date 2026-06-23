#include "TestSupport.h"

#include "project/ProjectBackgroundExecutor.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

namespace microide::tests {
namespace {

using microide::project::ProjectBackgroundExecutor;

void TestProjectBackgroundExecutorRunsPostedTasksInOrder() {
  ProjectBackgroundExecutor executor;
  std::mutex mutex;
  std::condition_variable cv;
  std::vector<int> values;

  for (int i : {1, 2, 3}) {
    executor.Post([i, &mutex, &cv, &values]() {
      std::lock_guard lock(mutex);
      values.push_back(i);
      cv.notify_all();
    });
  }

  std::unique_lock lock(mutex);
  Expect(cv.wait_for(lock, std::chrono::seconds(2),
                     [&]() { return values.size() == 3; }),
         "all posted tasks should run within the timeout");
  Expect(values == std::vector<int>({1, 2, 3}),
         "executor should run posted tasks in FIFO order");
}

void TestProjectBackgroundExecutorPostLatestDeduplicatesQueuedTasks() {
  ProjectBackgroundExecutor executor;
  std::mutex gate_mutex;
  std::condition_variable gate_cv;
  std::condition_variable started_cv;
  bool release_gate = false;
  bool gate_started = false;
  std::atomic<int> gate_runs{0};

  // Block the worker so subsequent PostLatest entries pile up in the queue and
  // can be observed as deduped.
  executor.Post([&]() {
    gate_runs.fetch_add(1);
    std::unique_lock lock(gate_mutex);
    gate_started = true;
    started_cv.notify_all();
    gate_cv.wait(lock, [&]() { return release_gate; });
  });

  {
    std::unique_lock lock(gate_mutex);
    Expect(started_cv.wait_for(lock, std::chrono::seconds(2),
                                [&]() { return gate_started; }),
           "gating task should reach the worker before observing dedup");
  }

  // While the worker is parked on the gate, three PostLatest invocations with
  // the same key collapse to one queued entry.
  std::atomic<int> latest_runs{0};
  for (int i = 0; i < 3; ++i) {
    executor.PostLatest("blame", [&]() { latest_runs.fetch_add(1); });
  }

  // Release the gate and wait for the queue to drain.
  {
    std::lock_guard lock(gate_mutex);
    release_gate = true;
  }
  gate_cv.notify_all();

  std::atomic<bool> drained_marker{false};
  executor.Post([&]() { drained_marker.store(true); });

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (!drained_marker.load() && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  Expect(drained_marker.load(), "executor should drain pending work");
  Expect(gate_runs.load() == 1, "the gating task should run exactly once");
  Expect(latest_runs.load() == 1,
         "PostLatest with the same key should keep only the most recent queued entry");
}

void TestProjectBackgroundExecutorCancelPreventsQueuedTasksFromRunning() {
  ProjectBackgroundExecutor executor;
  std::mutex gate_mutex;
  std::condition_variable gate_cv;
  std::condition_variable started_cv;
  bool release_gate = false;
  bool gate_started = false;
  std::atomic<bool> gate_finished{false};

  executor.Post([&]() {
    std::unique_lock lock(gate_mutex);
    gate_started = true;
    started_cv.notify_all();
    gate_cv.wait(lock, [&]() { return release_gate; });
    gate_finished.store(true);
  });

  {
    std::unique_lock lock(gate_mutex);
    Expect(started_cv.wait_for(lock, std::chrono::seconds(2),
                                [&]() { return gate_started; }),
           "gating task should be in-flight before posting cancellable follow-ups");
  }

  std::atomic<int> after_cancel_runs{0};
  for (int i = 0; i < 3; ++i) {
    executor.Post([&]() { after_cancel_runs.fetch_add(1); });
  }

  executor.Cancel();
  {
    std::lock_guard lock(gate_mutex);
    release_gate = true;
  }
  gate_cv.notify_all();

  std::atomic<bool> drained_marker{false};
  executor.Post([&]() { drained_marker.store(true); });
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (!drained_marker.load() && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  Expect(gate_finished.load(), "in-flight task should still complete after Cancel");
  Expect(after_cancel_runs.load() == 0,
         "queued tasks cancelled before they ran should not execute");
  Expect(drained_marker.load(),
         "executor should continue accepting work after a cancellation cycle");
}

void TestProjectBackgroundExecutorShutdownDropsQueuedWork() {
  std::atomic<int> ran{0};
  std::mutex gate_mutex;
  std::condition_variable gate_cv;
  std::condition_variable started_cv;
  bool release_gate = false;
  bool gate_started = false;
  {
    ProjectBackgroundExecutor executor;
    executor.Post([&]() {
      std::unique_lock lock(gate_mutex);
      gate_started = true;
      started_cv.notify_all();
      gate_cv.wait(lock, [&]() { return release_gate; });
      ran.fetch_add(1);
    });
    {
      std::unique_lock lock(gate_mutex);
      Expect(started_cv.wait_for(lock, std::chrono::seconds(2),
                                  [&]() { return gate_started; }),
             "gating task should be in-flight before queuing droppable follow-ups");
    }
    for (int i = 0; i < 3; ++i) {
      executor.Post([&]() { ran.fetch_add(1); });
    }
    // Drop the queued follow-ups deterministically while the worker is still
    // parked inside the in-flight gate task. Cancel() completes synchronously
    // under the executor lock (the worker holds no lock while running a task),
    // so all three queued entries are marked cancelled *before* the gate is
    // released. Without this, releasing the gate races the destructor's
    // Shutdown: the worker could drain a queued task in the window before
    // Shutdown cancels it, making `ran` non-deterministic.
    executor.Cancel();
    {
      std::lock_guard lock(gate_mutex);
      release_gate = true;
    }
    gate_cv.notify_all();
    // Destructor calls Shutdown: the gated task unblocks and runs to completion,
    // the already-cancelled follow-ups are dropped, and the worker joins.
  }
  Expect(ran.load() == 1,
         "shutdown should drop queued tasks while letting the in-flight task finish");
}

void TestProjectBackgroundExecutorPostAfterShutdownIsNoOp() {
  ProjectBackgroundExecutor executor;
  executor.Shutdown();

  std::atomic<bool> ran{false};
  executor.Post([&]() { ran.store(true); });
  executor.PostLatest("k", [&]() { ran.store(true); });

  // Give a brief grace period; the call is expected to be a no-op so this is
  // a quick reassurance rather than a polling loop.
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  Expect(!ran.load(), "Post after Shutdown should be a no-op");
}

}  // namespace

void RegisterProjectBackgroundExecutorTests(std::vector<TestCase>& tests) {
  AddTest(tests, "ProjectBackgroundExecutor/RunsPostedTasksInOrder",
          TestProjectBackgroundExecutorRunsPostedTasksInOrder);
  AddTest(tests, "ProjectBackgroundExecutor/PostLatestDeduplicatesQueuedTasks",
          TestProjectBackgroundExecutorPostLatestDeduplicatesQueuedTasks);
  AddTest(tests, "ProjectBackgroundExecutor/CancelPreventsQueuedTasksFromRunning",
          TestProjectBackgroundExecutorCancelPreventsQueuedTasksFromRunning);
  AddTest(tests, "ProjectBackgroundExecutor/ShutdownDropsQueuedWork",
          TestProjectBackgroundExecutorShutdownDropsQueuedWork);
  AddTest(tests, "ProjectBackgroundExecutor/PostAfterShutdownIsNoOp",
          TestProjectBackgroundExecutorPostAfterShutdownIsNoOp);
}

}  // namespace microide::tests
