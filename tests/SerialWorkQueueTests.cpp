// util::SerialWorkQueue tests.
//
// Cover the serial worker-queue contract shared by the plugin inbound queue and
// the project background executor: FIFO order, PostLatest collapses superseded
// queued work (but never a started job), PostFront jumps the queue, Cancel skips
// queued-not-running jobs, lazy vs eager start, and the on_enqueue/on_complete
// hook balance across every exit path (run, cancel, dedup-drop, shutdown-drain).

#include "TestSupport.h"

#include "util/SerialWorkQueue.h"

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace microide::tests {
namespace {

using microide::util::SerialWorkQueue;

template <typename Predicate>
bool WaitFor(Predicate predicate, std::chrono::milliseconds timeout = std::chrono::seconds(2)) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate()) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return predicate();
}

void TestLazyStartIsZeroCostUntilUsed() {
  SerialWorkQueue queue;  // kLazy by default
  Expect(!queue.started(), "a lazy queue must not spawn its worker before use");
  queue.EnsureStarted();
  Expect(queue.started(), "EnsureStarted spawns the worker");
}

void TestEagerStartRunsImmediately() {
  SerialWorkQueue queue(SerialWorkQueue::StartMode::kEager);
  Expect(queue.started(), "an eager queue spawns its worker in the constructor");
  std::atomic<bool> ran{false};
  queue.Post([&ran]() { ran.store(true); });
  Expect(WaitFor([&ran]() { return ran.load(); }), "an eager queue runs posted jobs");
}

void TestRunsJobsInFifoOrder() {
  SerialWorkQueue queue(SerialWorkQueue::StartMode::kEager);
  std::mutex mutex;
  std::vector<int> order;
  for (int i = 0; i < 8; ++i) {
    queue.Post([&mutex, &order, i]() {
      std::lock_guard lock(mutex);
      order.push_back(i);
    });
  }
  Expect(WaitFor([&mutex, &order]() {
           std::lock_guard lock(mutex);
           return order.size() == 8;
         }),
         "all posted jobs run");
  std::lock_guard lock(mutex);
  for (int i = 0; i < 8; ++i) {
    Expect(order[i] == i, "jobs run in FIFO order");
  }
}

void TestPostLatestCollapsesQueuedButNotStarted() {
  SerialWorkQueue queue(SerialWorkQueue::StartMode::kEager);
  std::mutex mutex;
  std::atomic<bool> release{false};
  std::vector<int> executed;

  // Gate the worker on a flag so the PostLatest jobs pile up behind it.
  queue.Post([&release]() {
    while (!release.load()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  });
  for (int i = 0; i < 5; ++i) {
    queue.PostLatest("refresh", [&mutex, &executed, i]() {
      std::lock_guard lock(mutex);
      executed.push_back(i);
    });
  }
  release.store(true);
  Expect(WaitFor([&mutex, &executed]() {
           std::lock_guard lock(mutex);
           return !executed.empty();
         }),
         "the surviving PostLatest job runs");
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  std::lock_guard lock(mutex);
  Expect(executed.size() == 1, "PostLatest keeps only the most recent queued job");
  Expect(executed.front() == 4, "the survivor is the last-posted job");
}

void TestPostFrontJumpsTheQueue() {
  SerialWorkQueue queue(SerialWorkQueue::StartMode::kEager);
  std::mutex mutex;
  std::atomic<bool> release{false};
  std::vector<std::string> order;

  queue.Post([&release]() {
    while (!release.load()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  });
  queue.Post([&mutex, &order]() {
    std::lock_guard lock(mutex);
    order.push_back("back");
  });
  queue.PostFront([&mutex, &order]() {
    std::lock_guard lock(mutex);
    order.push_back("front");
  });
  release.store(true);
  Expect(WaitFor([&mutex, &order]() {
           std::lock_guard lock(mutex);
           return order.size() == 2;
         }),
         "both queued jobs run");
  std::lock_guard lock(mutex);
  Expect(order[0] == "front" && order[1] == "back",
         "PostFront runs before an already-queued job");
}

void TestCancelSkipsQueuedJobs() {
  SerialWorkQueue queue(SerialWorkQueue::StartMode::kEager);
  std::atomic<bool> release{false};
  std::atomic<int> ran{0};

  queue.Post([&release]() {
    while (!release.load()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  });
  for (int i = 0; i < 4; ++i) {
    queue.Post([&ran]() { ran.fetch_add(1); });
  }
  queue.Cancel();  // mark the 4 queued jobs cancelled (the running gate is in-flight)
  release.store(true);
  std::this_thread::sleep_for(std::chrono::milliseconds(30));
  Expect(ran.load() == 0, "Cancel skips queued (not yet running) jobs");
}

// The accounting contract: on_complete fires exactly once per admitted job on
// every path (normal run, cancelled-skip, PostLatest dedup-drop, and a backlog
// drained during Shutdown), so a global in-flight counter nets to zero.
void TestHookBalanceAcrossAllPaths() {
  std::atomic<int> enqueued{0};
  std::atomic<int> completed{0};
  SerialWorkQueue::Hooks hooks{
      .on_enqueue = [&enqueued]() { enqueued.fetch_add(1); },
      .on_complete = [&completed]() { completed.fetch_add(1); },
  };
  {
    SerialWorkQueue queue(SerialWorkQueue::StartMode::kEager, std::move(hooks));
    std::atomic<bool> release{false};

    // Normal runs + a gate that holds the worker so the rest pile up.
    queue.Post([&release]() {
      while (!release.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    });
    // PostLatest dedup-drops: 5 posted, 4 superseded while queued.
    for (int i = 0; i < 5; ++i) {
      queue.PostLatest("k", []() {});
    }
    // A few plain jobs that will be drained as a backlog at shutdown.
    for (int i = 0; i < 3; ++i) {
      queue.Post([]() {});
    }
    release.store(true);
    // Do not wait for drain: Shutdown must drain-and-complete the backlog too.
    queue.Shutdown();
  }
  Expect(enqueued.load() == completed.load(),
         "on_complete fires exactly once per admitted job across all paths");
  Expect(enqueued.load() == 9, "1 gate + 5 PostLatest + 3 plain = 9 admitted jobs");
}

void TestNeverStartedHookBalance() {
  std::atomic<int> enqueued{0};
  std::atomic<int> completed{0};
  SerialWorkQueue::Hooks hooks{
      .on_enqueue = [&enqueued]() { enqueued.fetch_add(1); },
      .on_complete = [&completed]() { completed.fetch_add(1); },
  };
  {
    // Lazy + never started: jobs queue but no worker runs. Shutdown must still
    // balance the hooks for the abandoned backlog.
    SerialWorkQueue queue(SerialWorkQueue::StartMode::kLazy, std::move(hooks));
    for (int i = 0; i < 3; ++i) {
      queue.Post([]() {});
    }
    Expect(!queue.started(), "queue stayed lazy");
    queue.Shutdown();
  }
  Expect(enqueued.load() == 3 && completed.load() == 3,
         "a never-started queue still balances enqueue hooks at shutdown");
}

}  // namespace

void RegisterSerialWorkQueueTests(std::vector<TestCase>& tests) {
  AddTest(tests, "SerialWorkQueue/LazyStartIsZeroCostUntilUsed", TestLazyStartIsZeroCostUntilUsed);
  AddTest(tests, "SerialWorkQueue/EagerStartRunsImmediately", TestEagerStartRunsImmediately);
  AddTest(tests, "SerialWorkQueue/RunsJobsInFifoOrder", TestRunsJobsInFifoOrder);
  AddTest(tests, "SerialWorkQueue/PostLatestCollapsesQueued",
          TestPostLatestCollapsesQueuedButNotStarted);
  AddTest(tests, "SerialWorkQueue/PostFrontJumpsTheQueue", TestPostFrontJumpsTheQueue);
  AddTest(tests, "SerialWorkQueue/CancelSkipsQueuedJobs", TestCancelSkipsQueuedJobs);
  AddTest(tests, "SerialWorkQueue/HookBalanceAcrossAllPaths", TestHookBalanceAcrossAllPaths);
  AddTest(tests, "SerialWorkQueue/NeverStartedHookBalance", TestNeverStartedHookBalance);
}

}  // namespace microide::tests
