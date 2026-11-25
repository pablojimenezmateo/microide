#include "GitMergeConflictFixtures.h"
#include "TestSupport.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <mutex>
#include <thread>
#include <type_traits>
#include <vector>

#include "util/BackgroundTaskCounter.h"
#include "project/ProjectBackgroundExecutor.h"
#include "workspace/git/AutomaticGitRefreshPacer.h"
#include "workspace/git/GitRepositoryService.h"
#include "workspace/git/GitSidebarCommandCenter.h"

namespace microide::tests {
namespace {

using microide::project::ProjectBackgroundExecutor;
using microide::workspace::GitRepositoryService;
using microide::workspace::GitSidebarEntry;
using microide::workspace::GitSidebarRefreshScope;
using microide::workspace::GitSidebarState;
using microide::workspace::OutgoingBaseChoice;

void TestRefreshMarksFileDirectoryConflict() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path repo_path = temp_dir.path() / "repo";
  InitializeGitRepo(repo_path);
  WriteFile(repo_path / "seed.txt", "seed\n");
  CommitAll(repo_path, "base", "base");

  RequireGitCommandSuccess(repo_path, {"checkout", "-b", "file-side"}, "branch file-side");
  WriteFile(repo_path / "thing", "i am a file\n");
  CommitAll(repo_path, "file", "file");

  RequireGitCommandSuccess(repo_path, {"checkout", "main"}, "checkout main");
  std::filesystem::create_directories(repo_path / "thing");
  WriteFile(repo_path / "thing" / "inner.txt", "i am inside a directory\n");
  CommitAll(repo_path, "dir", "dir");
  (void)RunGitCommand(repo_path, {"merge", "file-side"});

  Expect(std::filesystem::is_directory(repo_path / "thing"),
         "git should leave the directory in place for a D/F conflict");

  ProjectBackgroundExecutor executor;
  GitRepositoryService service(executor);
  service.RunRefreshSynchronouslyForTesting(repo_path, platform::LocalProcessLauncher(),
                                            GitSidebarRefreshScope::Full,
                                            OutgoingBaseChoice{}, false);
  const auto state = service.CurrentState();

  bool saw_conflicted_entry = false;
  bool saw_directory_conflict = false;
  for (const auto& entry : state.entries) {
    if (entry.conflicted) {
      saw_conflicted_entry = true;
      if (entry.path_is_directory) {
        saw_directory_conflict = true;
      }
    } else {
      Expect(!entry.path_is_directory, "the probe must only run for conflicted entries");
    }
  }
  Expect(saw_conflicted_entry, "the D/F merge should leave a conflicted entry");
  Expect(saw_directory_conflict,
         "a D/F conflict must be marked path_is_directory — git names the moved-aside "
         "file side, so the probe has to test the prefix before the `~`");
}

void TestRefreshDoesNotMistakeTildeDirectoryForFileDirectoryConflict() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path repo_path = temp_dir.path() / "repo";
  InitializeGitRepo(repo_path);
  std::filesystem::create_directories(repo_path / "dir");
  std::filesystem::create_directories(repo_path / "dir~x");
  WriteFile(repo_path / "dir" / "keep.txt", "keep\n");
  WriteFile(repo_path / "dir~x" / "file.txt", "base\n");
  CommitAll(repo_path, "base", "base");

  RequireGitCommandSuccess(repo_path, {"checkout", "-b", "side"}, "branch side");
  WriteFile(repo_path / "dir~x" / "file.txt", "side\n");
  CommitAll(repo_path, "side", "side");
  RequireGitCommandSuccess(repo_path, {"checkout", "main"}, "checkout main");
  WriteFile(repo_path / "dir~x" / "file.txt", "main\n");
  CommitAll(repo_path, "main", "main");
  (void)RunGitCommand(repo_path, {"merge", "side"});

  ProjectBackgroundExecutor executor;
  GitRepositoryService service(executor);
  service.RunRefreshSynchronouslyForTesting(repo_path, platform::LocalProcessLauncher(),
                                            GitSidebarRefreshScope::Full,
                                            OutgoingBaseChoice{}, false);
  const auto state = service.CurrentState();

  bool saw_conflict = false;
  for (const auto& entry : state.entries) {
    if (!entry.conflicted) {
      continue;
    }
    saw_conflict = true;
    Expect(!entry.path_is_directory,
           "a content conflict under a directory whose NAME contains `~` must not be "
           "mistaken for a file/directory conflict");
  }
  Expect(saw_conflict, "the fixture should produce a conflicted entry");
}

void TestRefreshMarksSubmoduleConflict() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path inner = temp_dir.path() / "inner";
  InitializeGitRepo(inner);
  WriteFile(inner / "a.txt", "base\n");
  CommitAll(inner, "base", "base");
  RequireGitCommandSuccess(inner, {"checkout", "-b", "left"}, "inner left");
  WriteFile(inner / "a.txt", "left\n");
  CommitAll(inner, "left", "left");
  RequireGitCommandSuccess(inner, {"checkout", "main"}, "inner main");
  RequireGitCommandSuccess(inner, {"checkout", "-b", "right"}, "inner right");
  WriteFile(inner / "a.txt", "right\n");
  CommitAll(inner, "right", "right");
  RequireGitCommandSuccess(inner, {"checkout", "main"}, "inner back to main");

  const std::filesystem::path outer = temp_dir.path() / "outer";
  InitializeGitRepo(outer);
  if (RunGitCommand(outer, {"-c", "protocol.file.allow=always", "submodule", "add", "-q",
                            inner.string(), "dep"}) != 0) {
    return;
  }
  CommitAll(outer, "add submodule", "add submodule");

  RequireGitCommandSuccess(outer, {"checkout", "-b", "side"}, "outer side");
  RequireGitCommandSuccess(outer / "dep", {"checkout", "left"}, "dep left");
  RequireGitCommandSuccess(outer, {"add", "dep"}, "stage dep left");
  CommitAll(outer, "side moves dep", "side moves dep");
  RequireGitCommandSuccess(outer, {"checkout", "main"}, "outer main");
  RequireGitCommandSuccess(outer / "dep", {"checkout", "right"}, "dep right");
  RequireGitCommandSuccess(outer, {"add", "dep"}, "stage dep right");
  CommitAll(outer, "main moves dep", "main moves dep");
  (void)RunGitCommand(outer, {"merge", "side"});

  ProjectBackgroundExecutor executor;
  GitRepositoryService service(executor);
  service.RunRefreshSynchronouslyForTesting(outer, platform::LocalProcessLauncher(),
                                            GitSidebarRefreshScope::Full,
                                            OutgoingBaseChoice{}, false);
  const auto state = service.CurrentState();

  bool saw_conflicted_submodule = false;
  for (const auto& entry : state.entries) {
    if (entry.conflicted && entry.submodule) {
      saw_conflicted_submodule = true;
      Expect(entry.path_is_directory,
             "a submodule checkout is a directory, so the D/F probe fires too — which is "
             "exactly why submodule must be classified first");
    }
  }
  Expect(saw_conflicted_submodule,
         "a divergent submodule pointer must produce a conflicted entry marked submodule");
}

void TestCurrentStateReturnsSnapshotCopy() {
  static_assert(
      !std::is_reference_v<decltype(std::declval<const GitRepositoryService&>().CurrentState())>,
      "CurrentState must return an owned snapshot");

  TemporaryDirectory temp_dir;
  const std::filesystem::path repo_path = temp_dir.path() / "repo";
  InitializeGitRepo(repo_path);
  const auto file_path = repo_path / "tracked.txt";
  WriteFile(file_path, "before\n");
  CommitAll(repo_path, "base", "base");

  ProjectBackgroundExecutor executor;
  GitRepositoryService service(executor);
  service.RunRefreshSynchronouslyForTesting(repo_path, platform::LocalProcessLauncher(),
                                            GitSidebarRefreshScope::Full,
                                            OutgoingBaseChoice{}, false);

  const auto first_snapshot = service.CurrentState();
  Expect(first_snapshot.repo_available, "fixture repository should be available");
  const std::uint64_t first_generation = first_snapshot.generation;

  WriteFile(file_path, "before\nafter\n");
  service.RunRefreshSynchronouslyForTesting(repo_path, platform::LocalProcessLauncher(),
                                            GitSidebarRefreshScope::Full,
                                            OutgoingBaseChoice{}, false);

  const auto second_snapshot = service.CurrentState();
  Expect(second_snapshot.generation != first_generation,
         "refresh should advance repository generation");
  Expect(first_snapshot.generation == first_generation,
         "prior snapshot must remain immutable after refresh");
  Expect(!second_snapshot.entries.empty(),
         "modified repository should expose status entries after refresh");
}

void TestCurrentStateReadsRemainConsistentDuringRefresh() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path repo_path = temp_dir.path() / "repo";
  InitializeGitRepo(repo_path);
  WriteFile(repo_path / "tracked.txt", "line\n");
  CommitAll(repo_path, "base", "base");

  ProjectBackgroundExecutor executor;
  GitRepositoryService service(executor);
  service.RunRefreshSynchronouslyForTesting(repo_path, platform::LocalProcessLauncher(),
                                            GitSidebarRefreshScope::Full,
                                            OutgoingBaseChoice{}, false);

  std::atomic<bool> stop{false};
  std::thread refresher([&]() {
    while (!stop.load()) {
      service.RunRefreshSynchronouslyForTesting(repo_path, platform::LocalProcessLauncher(),
                                                GitSidebarRefreshScope::StatusOnly,
                                                OutgoingBaseChoice{}, false);
    }
  });

  for (int i = 0; i < 200; ++i) {
    const auto snapshot = service.CurrentState();
    Expect(snapshot.repository_root == repo_path, "snapshot root should stay stable");
    Expect(snapshot.generation > 0, "snapshot generation should remain valid");
  }

  stop.store(true);
  refresher.join();
  executor.Shutdown();
}

void TestRefreshSurfacesMergeConflictsInSidebar() {
  const GitMergeConflictFixture fixture = CreateBothModifiedConflictRepo();

  ProjectBackgroundExecutor executor;
  GitRepositoryService service(executor);
  service.RunRefreshSynchronouslyForTesting(fixture.root, platform::LocalProcessLauncher(),
                                            GitSidebarRefreshScope::Full,
                                            OutgoingBaseChoice{}, false);

  const auto repository_state = service.CurrentState();
  bool saw_conflicted_entry = false;
  for (const auto& entry : repository_state.entries) {
    if (entry.path.relative_path == std::filesystem::path("conflict.txt") && entry.conflicted) {
      saw_conflicted_entry = true;
      break;
    }
  }
  Expect(saw_conflicted_entry,
         "repository refresh should parse unmerged conflict.txt from porcelain v2");

  GitSidebarState::RefreshSnapshot snapshot;
  Expect(service.ConsumePendingSidebarSnapshot(&snapshot),
         "synchronous refresh should publish a sidebar snapshot");
  std::size_t conflict_rows = 0;
  std::size_t outgoing_rows = 0;
  for (const auto& entry : snapshot.entries) {
    if (entry.relative_path == std::filesystem::path("conflict.txt")) {
      if (entry.section == GitSidebarEntry::Section::Conflicts) {
        ++conflict_rows;
      } else if (entry.section == GitSidebarEntry::Section::Outgoing) {
        ++outgoing_rows;
      }
    }
  }
  Expect(conflict_rows == 1,
         "conflicted file should appear once in the Conflicts section");
  Expect(outgoing_rows == 0,
         "conflicted file should not be duplicated in Outgoing");
}

void TestAsyncRefreshCountsGlobalCounterOnce() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path repo_path = temp_dir.path() / "repo";
  InitializeGitRepo(repo_path);
  WriteFile(repo_path / "tracked.txt", "before\n");
  CommitAll(repo_path, "base", "base");

  ProjectBackgroundExecutor executor;
  GitRepositoryService service(executor);

  const int base = microide::util::GetBackgroundTaskCount();

  std::promise<void> running;
  std::promise<void> gate;
  executor.Post([&]() {
    running.set_value();
    gate.get_future().wait();
  });
  running.get_future().wait();

  const int before = microide::util::GetBackgroundTaskCount();
  Expect(before == base + 1, "the occupying task should raise the global counter by one");

  service.RequestRefresh(repo_path, platform::LocalProcessLauncher(),
                         GitSidebarRefreshScope::StatusOnly, OutgoingBaseChoice{}, false);
  Expect(microide::util::GetBackgroundTaskCount() == before + 1,
         "an async git refresh must raise the global counter by exactly one (no double-count)");

  gate.set_value();
  executor.Shutdown();
  Expect(microide::util::GetBackgroundTaskCount() == base,
         "the global counter must drain back to its baseline");
}

void TestSyncRefreshLeavesGlobalCounterUntouched() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path repo_path = temp_dir.path() / "repo";
  InitializeGitRepo(repo_path);
  WriteFile(repo_path / "tracked.txt", "before\n");
  CommitAll(repo_path, "base", "base");

  ProjectBackgroundExecutor executor;
  GitRepositoryService service(executor);

  const int base = microide::util::GetBackgroundTaskCount();
  for (int i = 0; i < 5; ++i) {
    service.RunRefreshSynchronouslyForTesting(repo_path, platform::LocalProcessLauncher(),
                                              GitSidebarRefreshScope::Full,
                                              OutgoingBaseChoice{}, false);
    Expect(microide::util::GetBackgroundTaskCount() == base,
           "a synchronous refresh must leave the global counter untouched");
  }
}

void TestConcurrentRefreshBurstStaysLiveAndBalanced() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path repo_path = temp_dir.path() / "repo";
  InitializeGitRepo(repo_path);
  WriteFile(repo_path / "tracked.txt", "before\n");
  CommitAll(repo_path, "base", "base");

  ProjectBackgroundExecutor executor;
  GitRepositoryService service(executor);

  const int base = microide::util::GetBackgroundTaskCount();

  constexpr int kThreads = 4;
  constexpr int kPerThread = 60;
  std::vector<std::thread> threads;
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&]() {
      for (int i = 0; i < kPerThread; ++i) {
        service.RequestRefresh(repo_path, platform::LocalProcessLauncher(),
                               GitSidebarRefreshScope::StatusOnly,
                               OutgoingBaseChoice{}, false);
      }
    });
  }
  for (auto& thread : threads) {
    thread.join();
  }

  int stable_base = 0;
  for (int i = 0; i < 1000 && stable_base < 4; ++i) {
    stable_base = (microide::util::GetBackgroundTaskCount() == base) ? stable_base + 1 : 0;
    std::this_thread::sleep_for(std::chrono::milliseconds(3));
  }
  Expect(stable_base >= 4,
         "concurrent refresh burst must drain the global background-task counter to baseline");

  const std::uint64_t generation_before = service.CurrentState().generation;
  service.RequestRefresh(repo_path, platform::LocalProcessLauncher(), GitSidebarRefreshScope::Full,
                         OutgoingBaseChoice{}, false);
  bool published = false;
  for (int i = 0; i < 1000 && !published; ++i) {
    GitSidebarState::RefreshSnapshot snapshot;
    if (service.CurrentState().generation > generation_before &&
        service.ConsumePendingSidebarSnapshot(&snapshot)) {
      published = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(3));
  }
  Expect(published, "a refresh after the burst must still schedule and publish (no freeze)");

  executor.Shutdown();
}

void TestBlockingRepositoryStateProviderIsAsync() {
  ProjectBackgroundExecutor executor;
  GitRepositoryService service(executor);

  std::atomic<bool> release{false};
  std::atomic<int> provider_calls{0};
  service.SetRepositoryStateProviderForTesting(
      [&](const std::filesystem::path& ,
          std::uint64_t generation) -> microide::project::GitRepositoryState {
        provider_calls.fetch_add(1);
        while (!release.load()) {
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        microide::project::GitRepositoryState state;
        state.repo_available = true;
        state.generation = generation;
        microide::project::GitRepositoryEntry entry;
        entry.kind = microide::project::GitRepositoryEntryKind::Ordinary;
        entry.status = microide::project::GitFileStatus::Modified;
        entry.path = microide::project::MakeGitRepositoryPathIdentity("tracked.txt");
        state.entries.push_back(std::move(entry));
        return state;
      });

  std::atomic<bool> woke{false};
  service.SetWakeCallbacks({.push_refresh_ready_event = [&]() {
    woke.store(true);
    return true;
  }});

  OutgoingBaseChoice choice;
  choice.kind = OutgoingBaseChoice::Kind::SpecificRef;
  choice.custom_ref = "HEAD~1";
  service.RequestRefresh("/fake/repo", platform::LocalProcessLauncher(),
                         GitSidebarRefreshScope::StatusOnly, choice, false);

  for (int i = 0; i < 2000 && provider_calls.load() == 0; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  Expect(provider_calls.load() == 1, "refresh must dispatch the status query to the worker");
  Expect(service.IsRefreshing(), "the sidebar must be refreshing while git is still running");
  GitSidebarState::RefreshSnapshot early;
  Expect(!service.ConsumePendingSidebarSnapshot(&early),
         "no snapshot may publish before the git query returns");

  release.store(true);

  bool published = false;
  GitSidebarState::RefreshSnapshot snapshot;
  for (int i = 0; i < 2000 && !published; ++i) {
    if (service.ConsumePendingSidebarSnapshot(&snapshot)) {
      published = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  Expect(published, "the refresh must publish a snapshot once the git query returns");
  Expect(woke.load(), "publishing a snapshot must fire the refresh-ready wake");
  bool saw_entry = false;
  for (const auto& entry : snapshot.entries) {
    if (entry.relative_path == std::filesystem::path("tracked.txt")) {
      saw_entry = true;
      break;
    }
  }
  Expect(saw_entry, "the published snapshot must carry the injected status entry");

  executor.Shutdown();
}

void TestOutgoingBaseResolutionIsMemoizedByRepositoryIdentity() {
  ProjectBackgroundExecutor executor;
  GitRepositoryService service(executor);

  std::string head_oid = "aaaaaaa";
  service.SetRepositoryStateProviderForTesting(
      [&](const std::filesystem::path& ,
          std::uint64_t generation) -> microide::project::GitRepositoryState {
        microide::project::GitRepositoryState state;
        state.repo_available = true;
        state.generation = generation;
        state.branch.branch_name = "feature";
        state.branch.head_oid = head_oid;
        return state;
      });

  OutgoingBaseChoice choice;
  choice.kind = OutgoingBaseChoice::Kind::SpecificRef;
  choice.custom_ref = "origin/main";

  service.RunRefreshSynchronouslyForTesting("/fake/repo", platform::LocalProcessLauncher(),
                                            GitSidebarRefreshScope::StatusOnly,
                                            choice, false);
  Expect(service.OutgoingBaseResolveCountForTesting() == 1, "first refresh resolves the base once");

  service.RunRefreshSynchronouslyForTesting("/fake/repo", platform::LocalProcessLauncher(),
                                            GitSidebarRefreshScope::StatusOnly,
                                            choice, false);
  Expect(service.OutgoingBaseResolveCountForTesting() == 1,
         "an unchanged repository identity must hit the outgoing-base cache");

  head_oid = "bbbbbbb";
  service.RunRefreshSynchronouslyForTesting("/fake/repo", platform::LocalProcessLauncher(),
                                            GitSidebarRefreshScope::StatusOnly,
                                            choice, false);
  Expect(service.OutgoingBaseResolveCountForTesting() == 2,
         "a HEAD change must invalidate the cache and re-resolve");

  executor.Shutdown();
}

void TestResetDoesNotCancelUnrelatedQueuedWork() {
  ProjectBackgroundExecutor executor;
  GitRepositoryService service(executor);

  std::mutex m;
  std::condition_variable cv;
  bool release = false;
  std::atomic<bool> victim_ran{false};

  executor.Post([&] {
    std::unique_lock<std::mutex> lock(m);
    cv.wait(lock, [&] { return release; });
  });
  executor.Post([&] { victim_ran.store(true); });

  service.Reset();

  {
    std::lock_guard<std::mutex> lock(m);
    release = true;
  }
  cv.notify_all();

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (!victim_ran.load() && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  Expect(victim_ran.load(),
         "git Reset() must not cancel unrelated queued project background work");

  executor.Shutdown();
}

void TestAutomaticGitRefreshPacerKeepsWhatItTurnsAway() {
  using microide::workspace::AutomaticGitRefreshPacer;
  constexpr std::uint64_t kInterval = AutomaticGitRefreshPacer::kIntervalMs;
  const microide::util::WakeChannel no_channel = 0;
  AutomaticGitRefreshPacer pacer(no_channel);

  Expect(pacer.Request(1000, false), "the first request is issued at once");
  Expect(!pacer.Request(1100, false), "a second inside the interval waits");
  Expect(pacer.pending() && pacer.wake_armed_for_ms() == 1000 + kInterval,
         "and is kept, with a wake armed for the interval's end");
  Expect(!pacer.Resume(1500, false), "before the interval ends it is not due");
  Expect(pacer.pending(), "and still kept");
  Expect(pacer.Resume(1000 + kInterval, false), "at the interval's end it is due");
  Expect(pacer.Request(1000 + kInterval, false), "and the caller's request issues it");
  Expect(!pacer.pending(), "nothing is kept after it ran");

  AutomaticGitRefreshPacer running(no_channel);
  Expect(!running.Request(5000, true), "a request while a refresh runs waits");
  Expect(running.pending() && running.wake_armed_for_ms() == 0,
         "kept, and woken by the running refresh's snapshot rather than a timer");
  Expect(!running.Resume(5100, true), "not due while that one still runs");
  Expect(running.Resume(5200, false), "due once it has landed (the interval had passed)");

  Expect(!AutomaticGitRefreshPacer(no_channel).Resume(9000, false),
         "with nothing kept, Resume issues nothing");
}

}

void RegisterGitRepositoryServiceTests(std::vector<TestCase>& tests) {
  AddTest(tests, "GitRepositoryService/AutomaticRefreshPacerKeepsWhatItTurnsAway",
          TestAutomaticGitRefreshPacerKeepsWhatItTurnsAway);
  AddTest(tests, "GitRepositoryService/ResetDoesNotCancelUnrelatedQueuedWork",
          TestResetDoesNotCancelUnrelatedQueuedWork);
  AddTest(tests, "GitRepositoryService/RefreshMarksSubmoduleConflict",
          TestRefreshMarksSubmoduleConflict);
  AddTest(tests, "GitRepositoryService/RefreshDoesNotMistakeTildeDirectory",
          TestRefreshDoesNotMistakeTildeDirectoryForFileDirectoryConflict);
  AddTest(tests, "GitRepositoryService/RefreshMarksFileDirectoryConflict",
          TestRefreshMarksFileDirectoryConflict);
  AddTest(tests, "GitRepositoryService/CurrentStateReturnsSnapshotCopy",
          TestCurrentStateReturnsSnapshotCopy);
  AddTest(tests, "GitRepositoryService/CurrentStateReadsDuringRefresh",
          TestCurrentStateReadsRemainConsistentDuringRefresh);
  AddTest(tests, "GitRepositoryService/RefreshSurfacesMergeConflictsInSidebar",
          TestRefreshSurfacesMergeConflictsInSidebar);
  AddTest(tests, "GitRepositoryService/AsyncRefreshCountsGlobalCounterOnce",
          TestAsyncRefreshCountsGlobalCounterOnce);
  AddTest(tests, "GitRepositoryService/SyncRefreshLeavesGlobalCounterUntouched",
          TestSyncRefreshLeavesGlobalCounterUntouched);
  AddTest(tests, "GitRepositoryService/ConcurrentRefreshBurstStaysLiveAndBalanced",
          TestConcurrentRefreshBurstStaysLiveAndBalanced);
  AddTest(tests, "GitRepositoryService/BlockingRepositoryStateProviderIsAsync",
          TestBlockingRepositoryStateProviderIsAsync);
  AddTest(tests, "GitRepositoryService/OutgoingBaseResolutionIsMemoizedByRepositoryIdentity",
          TestOutgoingBaseResolutionIsMemoizedByRepositoryIdentity);
}

}
