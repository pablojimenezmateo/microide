#include "TestSupport.h"

#include <atomic>
#include <thread>
#include <type_traits>

#include "project/ProjectBackgroundExecutor.h"
#include "workspace/GitRepositoryService.h"

namespace microide::tests {
namespace {

using microide::project::ProjectBackgroundExecutor;
using microide::workspace::GitRepositoryService;
using microide::workspace::GitSidebarRefreshScope;
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

}  // namespace

void RegisterGitRepositoryServiceTests(std::vector<TestCase>& tests) {
  AddTest(tests, "GitRepositoryService/CurrentStateReturnsSnapshotCopy",
          TestCurrentStateReturnsSnapshotCopy);
  AddTest(tests, "GitRepositoryService/CurrentStateReadsDuringRefresh",
          TestCurrentStateReadsRemainConsistentDuringRefresh);
}

}  // namespace microide::tests
