#include "GitMergeConflictFixtures.h"
#include "TestSupport.h"

#include <algorithm>
#include <atomic>
#include <thread>
#include <type_traits>

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

void TestSyncRefreshBalancesBackgroundTaskCount() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path repo_path = temp_dir.path() / "repo";
  InitializeGitRepo(repo_path);
  WriteFile(repo_path / "tracked.txt", "before\n");
  CommitAll(repo_path, "base", "base");

  ProjectBackgroundExecutor executor;
  GitRepositoryService service(executor);
  int counter = 0;
  int min_counter = 0;
  GitRepositoryService::WakeCallbacks callbacks;
  callbacks.increment_background_task_count = [&]() { ++counter; };
  callbacks.decrement_background_task_count_and_wake = [&]() {
    --counter;
    min_counter = std::min(min_counter, counter);
  };
  service.SetWakeCallbacks(std::move(callbacks));

  // A synchronous refresh publishes via PublishSnapshot, which decrements the
  // background-task counter as the tail of the async flow. Without a matching
  // increment on this path, repeated refreshes drove the shared counter negative
  // and tripped its underflow assert (crashing the perf harness). Each refresh
  // must leave the counter balanced and it must never go negative.
  for (int i = 0; i < 5; ++i) {
    service.RunRefreshSynchronouslyForTesting(repo_path, GitSidebarRefreshScope::Full,
                                              OutgoingBaseChoice{}, false);
    Expect(counter == 0, "each synchronous refresh must balance the background-task counter");
  }
  Expect(min_counter >= 0, "the background-task counter must never go negative");
}

}  // namespace

void RegisterGitRepositoryServiceTests(std::vector<TestCase>& tests) {
  AddTest(tests, "GitRepositoryService/CurrentStateReturnsSnapshotCopy",
          TestCurrentStateReturnsSnapshotCopy);
  AddTest(tests, "GitRepositoryService/CurrentStateReadsDuringRefresh",
          TestCurrentStateReadsRemainConsistentDuringRefresh);
  AddTest(tests, "GitRepositoryService/RefreshSurfacesMergeConflictsInSidebar",
          TestRefreshSurfacesMergeConflictsInSidebar);
  AddTest(tests, "GitRepositoryService/SyncRefreshBalancesBackgroundTaskCount",
          TestSyncRefreshBalancesBackgroundTaskCount);
}

}  // namespace microide::tests
