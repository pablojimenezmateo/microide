#include "TestSupport.h"

#include "plugin/PluginThread.h"

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace microide::tests {
namespace {

using microide::plugin::PluginThread;

template <typename Predicate>
bool WaitFor(Predicate predicate, std::chrono::milliseconds timeout = std::chrono::seconds(2)) {
  return WaitUntil([&predicate]() { return predicate(); }, timeout,
                   std::chrono::milliseconds(1));
}

void TestPluginThreadNeverStartedIsZeroCost() {
  PluginThread thread;
  Expect(!thread.started(), "a fresh PluginThread must not spawn its worker");
  Expect(thread.PendingMainThreadActionCount() == 0, "no actions queued when unused");
  Expect(thread.DrainMainThreadActions() == 0, "draining an unused thread is a no-op");
}

void TestPluginThreadRunsJobAndMarshalsResult() {
  PluginThread thread;
  thread.EnsureStarted();
  Expect(thread.started(), "EnsureStarted spawns the worker");

  std::atomic<bool> ran_on_worker{false};
  thread.Post([&thread, &ran_on_worker]() {
    ran_on_worker.store(true);
    thread.PostToMain([&ran_on_worker]() {
      ran_on_worker.store(false);
    });
  });

  Expect(WaitFor([&thread]() { return thread.PendingMainThreadActionCount() > 0; }),
         "worker should queue a mailbox action");
  Expect(ran_on_worker.load(), "job body must run on the worker before draining");
  const int drained = thread.DrainMainThreadActions();
  Expect(drained == 1, "exactly one mailbox action should drain");
  Expect(!ran_on_worker.load(), "the drained action must have run on this thread");
  Expect(thread.PendingMainThreadActionCount() == 0, "mailbox empties after draining");
}

void TestPluginThreadPostLatestCollapsesSupersededWork() {
  PluginThread thread;
  thread.EnsureStarted();

  std::mutex mutex;
  std::atomic<bool> release{false};
  std::vector<int> executed;

  thread.Post([&release]() {
    while (!release.load()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  });

  for (int i = 0; i < 5; ++i) {
    thread.PostLatest("refresh", [&mutex, &executed, i]() {
      std::lock_guard lock(mutex);
      executed.push_back(i);
    });
  }

  release.store(true);
  Expect(WaitFor([&mutex, &executed]() {
           std::lock_guard lock(mutex);
           return !executed.empty();
         }),
         "the surviving PostLatest job should run");
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  std::lock_guard lock(mutex);
  Expect(executed.size() == 1, "PostLatest must keep only the most recent queued job");
  Expect(executed.front() == 4, "the survivor must be the last-posted job");
}

void TestPluginThreadShutdownIsIdempotent() {
  PluginThread thread;
  thread.EnsureStarted();
  thread.Shutdown();
  thread.Shutdown();
  std::atomic<bool> ran{false};
  thread.Post([&ran]() { ran.store(true); });
  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  Expect(!ran.load(), "jobs posted after shutdown must not run");
}

}

void RegisterPluginThreadTests(std::vector<TestCase>& tests) {
  AddTest(tests, "PluginThread/NeverStartedIsZeroCost", TestPluginThreadNeverStartedIsZeroCost);
  AddTest(tests, "PluginThread/RunsJobAndMarshalsResult",
          TestPluginThreadRunsJobAndMarshalsResult);
  AddTest(tests, "PluginThread/PostLatestCollapsesSupersededWork",
          TestPluginThreadPostLatestCollapsesSupersededWork);
  AddTest(tests, "PluginThread/ShutdownIsIdempotent", TestPluginThreadShutdownIsIdempotent);
}

}
