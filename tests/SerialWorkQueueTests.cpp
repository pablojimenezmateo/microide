#include "TestSupport.h"

#include "util/SerialWorkQueue.h"

#include <atomic>
#include <chrono>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace microide::tests {
namespace {

using microide::util::SerialWorkQueue;

template <typename Predicate>
bool WaitFor(Predicate predicate, std::chrono::milliseconds timeout = std::chrono::seconds(2)) {
  return WaitUntil([&predicate]() { return predicate(); }, timeout,
                   std::chrono::milliseconds(1));
}

void TestLazyStartIsZeroCostUntilUsed() {
  SerialWorkQueue queue;
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
  queue.Cancel();
  release.store(true);
  std::this_thread::sleep_for(std::chrono::milliseconds(30));
  Expect(ran.load() == 0, "Cancel skips queued (not yet running) jobs");
}

void TestDurableJobSurvivesCancelNotDrain() {
  SerialWorkQueue queue(SerialWorkQueue::StartMode::kEager);
  std::atomic<bool> release{false};
  std::atomic<int> refreshes{0};
  std::atomic<int> writes{0};
  queue.Post([&release]() {
    while (!release.load()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  });
  queue.Post([&refreshes]() { refreshes.fetch_add(1); });
  queue.PostDurable([&writes]() { writes.fetch_add(1); });
  queue.Post([&refreshes]() { refreshes.fetch_add(1); });
  queue.Cancel();
  release.store(true);
  queue.Flush();
  Expect(refreshes.load() == 0, "Cancel skips the ordinary queued jobs");
  Expect(writes.load() == 1, "and runs the durable one");

  release.store(false);
  queue.Post([&release]() {
    while (!release.load()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  });
  queue.PostDurable([&writes]() { writes.fetch_add(1); });
  std::thread unblock([&release]() {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    release.store(true);
  });
  queue.Drain();
  unblock.join();
  Expect(writes.load() == 1, "Drain drops even a durable job (it reaches idle by cancelling)");
}

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

    queue.Post([&release]() {
      while (!release.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    });
    for (int i = 0; i < 5; ++i) {
      queue.PostLatest("k", []() {});
    }
    for (int i = 0; i < 3; ++i) {
      queue.Post([]() {});
    }
    release.store(true);
    queue.Shutdown();
  }
  Expect(enqueued.load() == completed.load(),
         "on_complete fires exactly once per admitted job across all paths");
  Expect(enqueued.load() == 9, "1 gate + 5 PostLatest + 3 plain = 9 admitted jobs");
}

void TestLazyQueueStartsOnFirstPost() {
  std::atomic<int> enqueued{0};
  std::atomic<int> completed{0};
  std::atomic<int> ran{0};
  SerialWorkQueue::Hooks hooks{
      .on_enqueue = [&enqueued]() { enqueued.fetch_add(1); },
      .on_complete = [&completed]() { completed.fetch_add(1); },
  };
  {
    SerialWorkQueue queue(SerialWorkQueue::StartMode::kLazy, std::move(hooks));
    Expect(!queue.started(), "a freshly constructed lazy queue is not yet started");
    for (int i = 0; i < 3; ++i) {
      queue.Post([&ran]() { ran.fetch_add(1); });
    }
    Expect(queue.started(), "posting to a lazy queue must start its worker");
    Expect(WaitFor([&ran]() { return ran.load() == 3; }),
           "a lazy queue must run jobs posted before any explicit start");
    queue.Shutdown();
  }
  Expect(ran.load() == 3, "a lazy queue must run jobs posted before any explicit start");
  Expect(enqueued.load() == 3 && completed.load() == 3,
         "hooks stay balanced across the lazy auto-start path");
}

void TestUntouchedLazyQueueStaysUnstarted() {
  SerialWorkQueue queue(SerialWorkQueue::StartMode::kLazy);
  Expect(!queue.started(), "an untouched lazy queue never spawns a worker");
  queue.Shutdown();
  Expect(!queue.started(), "shutting down an untouched lazy queue stays unstarted");
}

void TestThrowingJobDoesNotKillWorker() {
  std::atomic<int> completed{0};
  SerialWorkQueue::Hooks hooks{
      .on_enqueue = []() {},
      .on_complete = [&completed]() { completed.fetch_add(1); },
  };
  SerialWorkQueue queue(SerialWorkQueue::StartMode::kEager, std::move(hooks));
  std::atomic<bool> ran_after{false};

  queue.Post([]() { throw std::runtime_error("boom"); });
  queue.Post([]() { throw 42; });
  queue.Post([&ran_after]() { ran_after.store(true); });

  Expect(WaitFor([&ran_after]() { return ran_after.load(); }),
         "a job posted after throwing jobs still runs (worker survived)");
  Expect(WaitFor([&completed]() { return completed.load() == 3; }),
         "on_complete fires once per job even when the job throws");
}

void TestDrainQuiescesButKeepsWorkerUsable() {
  SerialWorkQueue queue(SerialWorkQueue::StartMode::kEager);
  std::atomic<bool> gate_started{false};
  std::atomic<bool> release{false};
  std::atomic<int> queued_ran{0};
  std::atomic<bool> in_flight_done{false};

  queue.Post([&gate_started, &release, &in_flight_done]() {
    gate_started.store(true);
    while (!release.load()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    in_flight_done.store(true);
  });
  for (int i = 0; i < 4; ++i) {
    queue.Post([&queued_ran]() { queued_ran.fetch_add(1); });
  }

  Expect(WaitFor([&gate_started]() { return gate_started.load(); }),
         "gate job started running before Drain");
  std::thread releaser([&release]() {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    release.store(true);
  });
  queue.Drain();
  releaser.join();

  Expect(in_flight_done.load(), "Drain waits for the in-flight job to finish");
  Expect(queued_ran.load() == 0, "Drain cancels queued (not yet running) jobs");

  std::atomic<bool> post_drain{false};
  queue.Post([&post_drain]() { post_drain.store(true); });
  Expect(WaitFor([&post_drain]() { return post_drain.load(); }),
         "the worker is still usable after Drain (not permanently stopped)");
}

void TestPostLatestKeyedDedupIsPerKeyAndOrdered() {
  SerialWorkQueue queue(SerialWorkQueue::StartMode::kEager);
  std::mutex mutex;
  std::atomic<bool> release{false};
  std::vector<std::string> order;

  queue.Post([&release]() {
    while (!release.load()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  });
  queue.PostLatest("a", [&mutex, &order]() {
    std::lock_guard lock(mutex);
    order.push_back("a1");
  });
  queue.PostLatest("b", [&mutex, &order]() {
    std::lock_guard lock(mutex);
    order.push_back("b1");
  });
  queue.Post([&mutex, &order]() {
    std::lock_guard lock(mutex);
    order.push_back("plain");
  });
  queue.PostLatest("a", [&mutex, &order]() {
    std::lock_guard lock(mutex);
    order.push_back("a2");
  });
  release.store(true);

  Expect(WaitFor([&mutex, &order]() {
           std::lock_guard lock(mutex);
           return order.size() == 3;
         }),
         "one job per key plus the plain job run");
  std::lock_guard lock(mutex);
  Expect(order[0] == "b1", "the un-superseded key keeps its original position");
  Expect(order[1] == "plain", "a plain Post stays ahead of a later same-key PostLatest");
  Expect(order[2] == "a2", "a superseded key re-appends its latest at the tail");
}

void TestDepthBudgetShedsOldestDroppableOnly() {
  std::atomic<int> enqueued{0};
  std::atomic<int> completed{0};
  SerialWorkQueue::Hooks hooks{
      .on_enqueue = [&enqueued]() { enqueued.fetch_add(1); },
      .on_complete = [&completed]() { completed.fetch_add(1); },
  };
  SerialWorkQueue queue(SerialWorkQueue::StartMode::kEager, std::move(hooks),
                        SerialWorkQueue::Limits{3});
  std::mutex mutex;
  std::atomic<bool> gate_started{false};
  std::atomic<bool> release{false};
  std::vector<int> ran;

  queue.Post([&gate_started, &release]() {
    gate_started.store(true);
    while (!release.load()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  });
  Expect(WaitFor([&gate_started]() { return gate_started.load(); }),
         "gate job is in-flight before the budget fills");
  for (int i = 0; i < 5; ++i) {
    queue.Post([&mutex, &ran, i]() {
      std::lock_guard lock(mutex);
      ran.push_back(i);
    }, SerialWorkQueue::Shedding::kDroppable);
  }
  std::atomic<bool> critical_ran{false};
  queue.Post([&critical_ran]() { critical_ran.store(true); },
             SerialWorkQueue::Shedding::kCritical);

  release.store(true);
  Expect(WaitFor([&critical_ran]() { return critical_ran.load(); }),
         "a critical job is never shed and runs");
  std::this_thread::sleep_for(std::chrono::milliseconds(20));

  std::lock_guard lock(mutex);
  Expect(ran.size() == 2, "the depth budget shed the older droppable jobs");
  Expect(ran[0] == 3 && ran[1] == 4, "shedding drops the OLDEST droppable jobs first");
  Expect(enqueued.load() == completed.load(),
         "on_complete fires once per admitted job even when shed under the budget");
}

void TestDepthBudgetNeverShedsCriticalWork() {
  SerialWorkQueue queue(SerialWorkQueue::StartMode::kEager, {}, SerialWorkQueue::Limits{2});
  std::atomic<bool> release{false};
  std::atomic<int> ran{0};

  queue.Post([&release]() {
    while (!release.load()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  });
  for (int i = 0; i < 6; ++i) {
    queue.Post([&ran]() { ran.fetch_add(1); });
  }
  release.store(true);
  Expect(WaitFor([&ran]() { return ran.load() == 6; }),
         "every critical job runs even past the depth budget");
}

}

void RegisterSerialWorkQueueTests(std::vector<TestCase>& tests) {
  AddTest(tests, "SerialWorkQueue/LazyStartIsZeroCostUntilUsed", TestLazyStartIsZeroCostUntilUsed);
  AddTest(tests, "SerialWorkQueue/EagerStartRunsImmediately", TestEagerStartRunsImmediately);
  AddTest(tests, "SerialWorkQueue/RunsJobsInFifoOrder", TestRunsJobsInFifoOrder);
  AddTest(tests, "SerialWorkQueue/PostLatestCollapsesQueued",
          TestPostLatestCollapsesQueuedButNotStarted);
  AddTest(tests, "SerialWorkQueue/PostFrontJumpsTheQueue", TestPostFrontJumpsTheQueue);
  AddTest(tests, "SerialWorkQueue/DurableJobSurvivesCancelNotDrain", TestDurableJobSurvivesCancelNotDrain);
  AddTest(tests, "SerialWorkQueue/CancelSkipsQueuedJobs", TestCancelSkipsQueuedJobs);
  AddTest(tests, "SerialWorkQueue/HookBalanceAcrossAllPaths", TestHookBalanceAcrossAllPaths);
  AddTest(tests, "SerialWorkQueue/LazyQueueStartsOnFirstPost", TestLazyQueueStartsOnFirstPost);
  AddTest(tests, "SerialWorkQueue/UntouchedLazyQueueStaysUnstarted",
          TestUntouchedLazyQueueStaysUnstarted);
  AddTest(tests, "SerialWorkQueue/ThrowingJobDoesNotKillWorker", TestThrowingJobDoesNotKillWorker);
  AddTest(tests, "SerialWorkQueue/DrainQuiescesButKeepsWorkerUsable",
          TestDrainQuiescesButKeepsWorkerUsable);
  AddTest(tests, "SerialWorkQueue/PostLatestKeyedDedupIsPerKeyAndOrdered",
          TestPostLatestKeyedDedupIsPerKeyAndOrdered);
  AddTest(tests, "SerialWorkQueue/DepthBudgetShedsOldestDroppableOnly",
          TestDepthBudgetShedsOldestDroppableOnly);
  AddTest(tests, "SerialWorkQueue/DepthBudgetNeverShedsCriticalWork",
          TestDepthBudgetNeverShedsCriticalWork);
}

}
