#include "TestSupport.h"

#include "workspace/WorkspaceProjectFileMonitor.h"

#include <chrono>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace microide::tests {
namespace {

using microide::workspace::WorkspaceProjectFileMonitor;

// Drives the monitor without a real ProjectBackgroundExecutor: captures the posted
// tasks so the test can run them deterministically (standing in for the executor
// thread). No SDL wake event type is set, so PushWakeEvent is a no-op and the
// change result is delivered purely through the atomic flag the executor sets.
void TestProjectFileMonitorPollRunsOffThreadAndDeliversChange() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "proj";
  WriteFile(root / "a.txt", "1\n");

  WorkspaceProjectFileMonitor monitor;
  monitor.SetPollInterval(std::chrono::milliseconds::zero());

  std::vector<std::string> keys;
  std::vector<std::function<void()>> tasks;
  monitor.SetBackgroundPoster([&](std::string key, std::function<void()> task) {
    keys.push_back(std::move(key));
    tasks.push_back(std::move(task));  // capture, run later (simulate the executor)
  });
  monitor.SetProjectRoot(root);  // non-deferred: arms synchronously

  Expect(!monitor.PollForChanges(), "monitor should report no change before any edit");
  Expect(!keys.empty() && keys.back() == "project-file-monitor-poll",
         "monitor should schedule an off-thread poll with the expected dedup key");

  // Coalescing: further shell-thread polls while one walk is in flight must not pile
  // up additional background tasks.
  const std::size_t scheduled_after_first = tasks.size();
  Expect(!monitor.PollForChanges(), "repeat poll should still report no change");
  Expect(!monitor.PollForChanges(), "repeat poll should still report no change");
  Expect(tasks.size() == scheduled_after_first,
         "an in-flight background walk should coalesce repeated poll scheduling");

  // A new file is an unambiguous change; run the captured background task(s).
  WriteFile(root / "b.txt", "new\n");
  for (auto& task : tasks) {
    task();
  }
  tasks.clear();

  Expect(monitor.PollForChanges(), "monitor should deliver the change detected off-thread");
  // Run anything scheduled by the delivering poll, then confirm the flag is cleared.
  for (auto& task : tasks) {
    task();
  }
  tasks.clear();
  Expect(!monitor.PollForChanges(), "monitor should clear the change after delivery");
}

// Without a background poster wired (headless/tests), polling falls back to a
// synchronous walk so behavior is preserved.
void TestProjectFileMonitorPollFallsBackToSynchronousWhenNoPoster() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "proj";
  WriteFile(root / "a.txt", "1\n");

  WorkspaceProjectFileMonitor monitor;
  monitor.SetPollInterval(std::chrono::milliseconds::zero());
  monitor.SetProjectRoot(root);

  Expect(!monitor.PollForChanges(), "synchronous monitor should start clean");
  WriteFile(root / "b.txt", "new\n");
  // First poll runs the synchronous walk (detects change, sets the flag); the change
  // is then delivered on the following poll.
  monitor.PollForChanges();
  Expect(monitor.PollForChanges(), "synchronous fallback should still detect changes");
}

void TestProjectFileMonitorLargeTreeDegradesAndNotifiesOnce() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "big";
  for (int i = 0; i < 12; ++i) {
    WriteFile(root / ("d" + std::to_string(i)) / "f.txt", "x\n");
  }

  WorkspaceProjectFileMonitor monitor;
  monitor.SetEntryBudget(4);  // trip the budget far below the tree size
  monitor.SetPollInterval(std::chrono::milliseconds::zero());
  monitor.SetProjectRoot(root);  // non-deferred arm trips the budget during SetRoots

  Expect(monitor.ConsumeTreeTooLargeNotice(),
         "monitor should raise a one-time too-large notice after arming an oversized tree");
  Expect(!monitor.ConsumeTreeTooLargeNotice(),
         "the too-large notice should fire only once per root");
  Expect(!monitor.NextPollDelay().has_value(),
         "an oversized tree should suppress periodic polling at the monitor");
}

}  // namespace

void RegisterWorkspaceProjectFileMonitorTests(std::vector<TestCase>& tests) {
  AddTest(tests, "WorkspaceProjectFileMonitor/PollRunsOffThreadAndDeliversChange",
          TestProjectFileMonitorPollRunsOffThreadAndDeliversChange);
  AddTest(tests, "WorkspaceProjectFileMonitor/PollFallsBackToSynchronousWhenNoPoster",
          TestProjectFileMonitorPollFallsBackToSynchronousWhenNoPoster);
  AddTest(tests, "WorkspaceProjectFileMonitor/LargeTreeDegradesAndNotifiesOnce",
          TestProjectFileMonitorLargeTreeDegradesAndNotifiesOnce);
}

}  // namespace microide::tests
