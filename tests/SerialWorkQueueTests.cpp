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

// TD-2026-07-16-14: the lazy-start contract says a kLazy queue starts on first
// EnsureStarted()/Post*. Posting to a default (lazy) queue must therefore start the
// worker and actually run the work — not leave it stranded in an unstarted queue.
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

// A truly untouched lazy queue (never posted, never EnsureStarted) stays threadless
// and Shutdown is a clean no-op.
void TestUntouchedLazyQueueStaysUnstarted() {
  SerialWorkQueue queue(SerialWorkQueue::StartMode::kLazy);
  Expect(!queue.started(), "an untouched lazy queue never spawns a worker");
  queue.Shutdown();
  Expect(!queue.started(), "shutting down an untouched lazy queue stays unstarted");
}

// A job that throws must not escape the worker functor (that is a std::terminate
// for the whole process). The worker survives, on_complete still fires for the
// throwing job, and later jobs keep running.
void TestThrowingJobDoesNotKillWorker() {
  std::atomic<int> completed{0};
  SerialWorkQueue::Hooks hooks{
      .on_enqueue = []() {},
      .on_complete = [&completed]() { completed.fetch_add(1); },
  };
  SerialWorkQueue queue(SerialWorkQueue::StartMode::kEager, std::move(hooks));
  std::atomic<bool> ran_after{false};

  queue.Post([]() { throw std::runtime_error("boom"); });
  queue.Post([]() { throw 42; });  // non-std exception
  queue.Post([&ran_after]() { ran_after.store(true); });

  Expect(WaitFor([&ran_after]() { return ran_after.load(); }),
         "a job posted after throwing jobs still runs (worker survived)");
  Expect(WaitFor([&completed]() { return completed.load() == 3; }),
         "on_complete fires once per job even when the job throws");
}

// Drain quiesces the worker (in-flight job finishes, queued jobs are cancelled)
// WITHOUT stopping it: new jobs posted afterwards still run.
void TestDrainQuiescesButKeepsWorkerUsable() {
  SerialWorkQueue queue(SerialWorkQueue::StartMode::kEager);
  std::atomic<bool> gate_started{false};
  std::atomic<bool> release{false};
  std::atomic<int> queued_ran{0};
  std::atomic<bool> in_flight_done{false};

  // In-flight gate + queued jobs that Drain must cancel.
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

  // The gate must be genuinely in-flight before Drain(): Drain cancels every
  // QUEUED job, so if the worker hadn't dequeued the gate yet it would be
  // cancelled too and in_flight_done would never be set.
  Expect(WaitFor([&gate_started]() { return gate_started.load(); }),
         "gate job started running before Drain");
  std::thread releaser([&release]() {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    release.store(true);
  });
  queue.Drain();  // blocks until the gate finishes + the barrier drains
  releaser.join();

  Expect(in_flight_done.load(), "Drain waits for the in-flight job to finish");
  Expect(queued_ran.load() == 0, "Drain cancels queued (not yet running) jobs");

  std::atomic<bool> post_drain{false};
  queue.Post([&post_drain]() { post_drain.store(true); });
  Expect(WaitFor([&post_drain]() { return post_drain.load(); }),
         "the worker is still usable after Drain (not permanently stopped)");
}

// TD-2026-07-17A-074: PostLatest dedup is now O(1) via a key->node index rather
// than a full-queue scan+erase. This pins the observable contract the index must
// preserve: distinct keys each keep their own latest, and the survivor per key is
// the last-posted closure, with plain FIFO Post()s enqueued between two same-key
// PostLatest calls still running in order (append-at-tail, not replace-in-place).
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
  // Interleave two dedup keys and a plain Post. Only the latest per key survives;
  // the plain Post enqueued before the second "a" must still run before it.
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
  // b1 (posted first, never superseded) runs first; the plain Post keeps its slot
  // ahead of the re-appended a2; a1 was dropped by the second "a".
  Expect(order[0] == "b1", "the un-superseded key keeps its original position");
  Expect(order[1] == "plain", "a plain Post stays ahead of a later same-key PostLatest");
  Expect(order[2] == "a2", "a superseded key re-appends its latest at the tail");
}

// The depth budget sheds only jobs a caller marks kDroppable, and sheds the
// OLDEST droppable job first, so admitting new work holds the queue near max_depth
// without ever losing a critical (default) job. Hooks stay balanced across sheds.
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

  // In-flight gate so everything else piles up behind it (depth counts only queued
  // jobs; the gate is dequeued and running, not queued). Wait until it is genuinely
  // running so the queued-depth arithmetic below is deterministic.
  queue.Post([&gate_started, &release]() {
    gate_started.store(true);
    while (!release.load()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  });
  Expect(WaitFor([&gate_started]() { return gate_started.load(); }),
         "gate job is in-flight before the budget fills");
  // Fill to budget with droppable jobs 0,1,2, then admit two more droppable (3,4):
  // each admit at capacity sheds the oldest droppable, so 0 and 1 are shed.
  for (int i = 0; i < 5; ++i) {
    queue.Post([&mutex, &ran, i]() {
      std::lock_guard lock(mutex);
      ran.push_back(i);
    }, SerialWorkQueue::Shedding::kDroppable);
  }
  // A critical job admitted at capacity sheds the oldest droppable (2), never
  // itself, so it must run.
  std::atomic<bool> critical_ran{false};
  queue.Post([&critical_ran]() { critical_ran.store(true); },
             SerialWorkQueue::Shedding::kCritical);

  release.store(true);
  Expect(WaitFor([&critical_ran]() { return critical_ran.load(); }),
         "a critical job is never shed and runs");
  std::this_thread::sleep_for(std::chrono::milliseconds(20));

  std::lock_guard lock(mutex);
  // Survivors are the newest droppables 3 and 4 (0,1 shed by 3,4; 2 shed by the
  // critical job). Oldest-first shedding keeps the latest speculative work.
  Expect(ran.size() == 2, "the depth budget shed the older droppable jobs");
  Expect(ran[0] == 3 && ran[1] == 4, "shedding drops the OLDEST droppable jobs first");
  Expect(enqueued.load() == completed.load(),
         "on_complete fires once per admitted job even when shed under the budget");
}

// A queue saturated with CRITICAL jobs exceeds max_depth rather than dropping any:
// losing a critical job (a WorkspaceEdit apply / save participant) is a
// correctness bug, so the budget is best-effort against critical work.
void TestDepthBudgetNeverShedsCriticalWork() {
  SerialWorkQueue queue(SerialWorkQueue::StartMode::kEager, {}, SerialWorkQueue::Limits{2});
  std::mutex mutex;
  std::atomic<bool> release{false};
  std::atomic<int> ran{0};

  queue.Post([&release]() {
    while (!release.load()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  });
  for (int i = 0; i < 6; ++i) {
    queue.Post([&ran]() { ran.fetch_add(1); });  // critical (default)
  }
  release.store(true);
  Expect(WaitFor([&ran]() { return ran.load() == 6; }),
         "every critical job runs even past the depth budget");
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

}  // namespace microide::tests
