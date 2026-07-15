#include "GitMergeConflictFixtures.h"
#include "TestSupport.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <future>
#include <mutex>
#include <thread>
#include <type_traits>
#include <vector>

#include "app/BackgroundTaskCounter.h"
#include "project/ProjectBackgroundExecutor.h"
#include "workspace/GitRepositoryService.h"
#include "workspace/GitSidebarCommandCenter.h"

namespace microide::tests {
namespace {

using microide::project::ProjectBackgroundExecutor;
using microide::workspace::GitRepositoryService;
using microide::workspace::GitSidebarEntry;
using microide::workspace::GitSidebarRefreshScope;
using microide::workspace::GitSidebarState;
using microide::workspace::OutgoingBaseChoice;

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
  service.RunRefreshSynchronouslyForTesting(repo_path, GitSidebarRefreshScope::Full,
                                            OutgoingBaseChoice{}, false);

  const auto first_snapshot = service.CurrentState();
  Expect(first_snapshot.repo_available, "fixture repository should be available");
  const std::uint64_t first_generation = first_snapshot.generation;

  WriteFile(file_path, "before\nafter\n");
  service.RunRefreshSynchronouslyForTesting(repo_path, GitSidebarRefreshScope::Full,
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
  service.RunRefreshSynchronouslyForTesting(repo_path, GitSidebarRefreshScope::Full,
                                            OutgoingBaseChoice{}, false);

  std::atomic<bool> stop{false};
  std::thread refresher([&]() {
    while (!stop.load()) {
      service.RunRefreshSynchronouslyForTesting(repo_path, GitSidebarRefreshScope::StatusOnly,
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
  service.RunRefreshSynchronouslyForTesting(fixture.root, GitSidebarRefreshScope::Full,
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

// The service no longer counts the global background-task counter itself — the
// counter is owned entirely by ProjectBackgroundExecutor's queue hooks. An async
// refresh must therefore raise the global counter by EXACTLY one (the enqueue),
// never two (the old double-count where the service also incremented manually).
void TestAsyncRefreshCountsGlobalCounterOnce() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path repo_path = temp_dir.path() / "repo";
  InitializeGitRepo(repo_path);
  WriteFile(repo_path / "tracked.txt", "before\n");
  CommitAll(repo_path, "base", "base");

  ProjectBackgroundExecutor executor;
  GitRepositoryService service(executor);

  const int base = microide::app::GetBackgroundTaskCount();

  // Occupy the single serial worker so the git refresh job stays queued (its
  // on_enqueue hook has fired, but it has not run/completed) while we sample.
  std::promise<void> running;
  std::promise<void> gate;
  executor.Post([&]() {
    running.set_value();
    gate.get_future().wait();
  });
  running.get_future().wait();

  const int before = microide::app::GetBackgroundTaskCount();
  Expect(before == base + 1, "the occupying task should raise the global counter by one");

  service.RequestRefresh(repo_path, GitSidebarRefreshScope::StatusOnly, OutgoingBaseChoice{}, false);
  // Both the executor enqueue and — under the old bug — the service's manual
  // increment happen synchronously before this line, so the old code reads
  // before+2 here and fails.
  Expect(microide::app::GetBackgroundTaskCount() == before + 1,
         "an async git refresh must raise the global counter by exactly one (no double-count)");

  gate.set_value();
  executor.Shutdown();
  Expect(microide::app::GetBackgroundTaskCount() == base,
         "the global counter must drain back to its baseline");
}

// The synchronous test path bypasses ProjectBackgroundExecutor entirely, so it is
// counter-neutral: it must not move the global counter at all.
void TestSyncRefreshLeavesGlobalCounterUntouched() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path repo_path = temp_dir.path() / "repo";
  InitializeGitRepo(repo_path);
  WriteFile(repo_path / "tracked.txt", "before\n");
  CommitAll(repo_path, "base", "base");

  ProjectBackgroundExecutor executor;
  GitRepositoryService service(executor);

  const int base = microide::app::GetBackgroundTaskCount();
  for (int i = 0; i < 5; ++i) {
    service.RunRefreshSynchronouslyForTesting(repo_path, GitSidebarRefreshScope::Full,
                                              OutgoingBaseChoice{}, false);
    Expect(microide::app::GetBackgroundTaskCount() == base,
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

  // The global background-task counter is owned by the executor's queue hooks and
  // starts at this test's baseline. Every enqueued refresh must eventually drain
  // back to it, even across superseded generations.
  const int base = microide::app::GetBackgroundTaskCount();

  // Hammer RequestRefresh from several threads. Each in-flight refresh that is
  // superseded by a newer generation (bumped by a concurrent RequestRefresh)
  // must still (a) decrement the background-task counter it incremented and
  // (b) hand off any deferred follow-up. The pre-fix PublishSnapshot early-return
  // and ScheduleRefresh early-out skipped one or both, leaking the counter and
  // freezing refresh_in_flight_ so the sidebar silently stopped updating until
  // Reset(). This exercises those superseded exits under real contention.
  constexpr int kThreads = 4;
  constexpr int kPerThread = 60;
  std::vector<std::thread> threads;
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&]() {
      for (int i = 0; i < kPerThread; ++i) {
        service.RequestRefresh(repo_path, GitSidebarRefreshScope::StatusOnly,
                               OutgoingBaseChoice{}, false);
      }
    });
  }
  for (auto& thread : threads) {
    thread.join();
  }

  // Quiesce: with the fix every enqueued task eventually completes, so once the
  // deferred-follow-up chain drains the global counter returns to a *stable* base.
  // Require several consecutive base reads so the brief complete->re-enqueue
  // hand-off window between a superseded task and its follow-up is not mistaken
  // for a drained queue. Bounded so a regression (leaked counter / frozen chain)
  // fails instead of hanging.
  int stable_base = 0;
  for (int i = 0; i < 1000 && stable_base < 4; ++i) {
    stable_base = (microide::app::GetBackgroundTaskCount() == base) ? stable_base + 1 : 0;
    std::this_thread::sleep_for(std::chrono::milliseconds(3));
  }
  Expect(stable_base >= 4,
         "concurrent refresh burst must drain the global background-task counter to baseline");

  // Liveness: after the burst, refresh_in_flight_ must not be stuck. A fresh
  // request must schedule, run, and publish a new snapshot. A frozen state
  // machine would defer this forever and never publish.
  const std::uint64_t generation_before = service.CurrentState().generation;
  service.RequestRefresh(repo_path, GitSidebarRefreshScope::Full, OutgoingBaseChoice{}, false);
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

// Fake-git seam: a refresh must open the sidebar in a refreshing state and
// dispatch the status query to the background executor WITHOUT blocking the
// caller. Injecting a provider that stalls on a gate lets the test assert the
// service is refreshing (and has published nothing) while git is "still running",
// then completes once released — all without spawning real git. This mirrors the
// async compare-picker provider seam and closes the "no cheap fake git/executor
// seam" gap for the sidebar refresh path.
void TestBlockingRepositoryStateProviderIsAsync() {
  ProjectBackgroundExecutor executor;
  GitRepositoryService service(executor);

  std::atomic<bool> release{false};
  std::atomic<int> provider_calls{0};
  service.SetRepositoryStateProviderForTesting(
      [&](const std::filesystem::path& /*root*/,
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

  // SpecificRef keeps ResolveGitOutgoingBase git-free; StatusOnly scope skips the
  // outgoing-files diff — so the entire refresh path runs without real git once the
  // status producer is injected.
  OutgoingBaseChoice choice;
  choice.kind = OutgoingBaseChoice::Kind::SpecificRef;
  choice.custom_ref = "HEAD~1";
  service.RequestRefresh("/fake/repo", GitSidebarRefreshScope::StatusOnly, choice, false);

  // The caller returned immediately; wait until the worker is actually parked
  // inside the injected provider so the assertions below prove async behavior
  // rather than a not-yet-started task.
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

// The outgoing-base resolution (several git subprocesses on the Auto path) is
// memoized across refreshes: a plain status refresh after a file edit leaves the
// branch/HEAD/upstream unchanged and must be served from cache, while a HEAD
// movement re-resolves.
void TestOutgoingBaseResolutionIsMemoizedByRepositoryIdentity() {
  ProjectBackgroundExecutor executor;
  GitRepositoryService service(executor);

  std::string head_oid = "aaaaaaa";
  service.SetRepositoryStateProviderForTesting(
      [&](const std::filesystem::path& /*root*/,
          std::uint64_t generation) -> microide::project::GitRepositoryState {
        microide::project::GitRepositoryState state;
        state.repo_available = true;
        state.generation = generation;
        state.branch.branch_name = "feature";
        state.branch.head_oid = head_oid;
        return state;
      });

  // SpecificRef keeps ResolveGitOutgoingBase git-free; StatusOnly skips the
  // outgoing-files diff. The cache key/hit/miss logic is identical for every kind.
  OutgoingBaseChoice choice;
  choice.kind = OutgoingBaseChoice::Kind::SpecificRef;
  choice.custom_ref = "origin/main";

  service.RunRefreshSynchronouslyForTesting("/fake/repo", GitSidebarRefreshScope::StatusOnly,
                                            choice, false);
  Expect(service.OutgoingBaseResolveCountForTesting() == 1, "first refresh resolves the base once");

  // Same branch + HEAD + choice: served from cache, no re-resolution.
  service.RunRefreshSynchronouslyForTesting("/fake/repo", GitSidebarRefreshScope::StatusOnly,
                                            choice, false);
  Expect(service.OutgoingBaseResolveCountForTesting() == 1,
         "an unchanged repository identity must hit the outgoing-base cache");

  // HEAD moved (commit/checkout/reset): the memo must invalidate and re-resolve.
  head_oid = "bbbbbbb";
  service.RunRefreshSynchronouslyForTesting("/fake/repo", GitSidebarRefreshScope::StatusOnly,
                                            choice, false);
  Expect(service.OutgoingBaseResolveCountForTesting() == 2,
         "a HEAD change must invalidate the cache and re-resolve");

  executor.Shutdown();
}

}  // namespace

void RegisterGitRepositoryServiceTests(std::vector<TestCase>& tests) {
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

}  // namespace microide::tests
