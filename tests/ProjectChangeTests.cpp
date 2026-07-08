#include "project/GitRepositoryMetadataTracker.h"
#include "project/ProjectChangeCoalescer.h"
#include "project/ProjectChangeNormalizer.h"
#include "platform/FileIndexWatcher.h"
#include "TestSupport.h"

#include <chrono>
#include <filesystem>
#include <thread>

namespace microide::tests {
namespace {

void TestProjectChangeNormalizerMapsIndexUpdates() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  WriteFile(root / "tracked.txt", "one\n");

  platform::IndexUpdateBatch batch;
  batch.changes.push_back(platform::IndexUpdateBatch::Change{
      .kind = platform::IndexUpdateBatch::Kind::CreatedOrModified,
      .entry = {.relative_path = std::filesystem::path("tracked.txt")},
  });
  batch.changes.push_back(platform::IndexUpdateBatch::Change{
      .kind = platform::IndexUpdateBatch::Kind::Deleted,
      .entry = {.relative_path = std::filesystem::path("removed.txt")},
  });

  const project::ProjectChangeBatch normalized =
      project::NormalizeIndexUpdateBatch(root, batch);
  Expect(normalized.file_changes.size() == 2, "normalized batch should include all file changes");
  Expect(normalized.file_changes[0].kind == project::ProjectFileChangeKind::Modified,
         "created/modified index updates should map to modified file changes");
  Expect(normalized.file_changes[1].kind == project::ProjectFileChangeKind::Deleted,
         "deleted index updates should map to deleted file changes");
}

void TestProjectChangeCoalescerMergesBursts() {
  project::ProjectChangeCoalescer coalescer;
  project::ProjectChangeBatch first;
  first.file_changes.push_back(project::ProjectFileChange{
      .kind = project::ProjectFileChangeKind::Modified,
      .relative_path = std::filesystem::path("src/a.cpp"),
      .absolute_path = std::filesystem::path("/tmp/src/a.cpp"),
  });
  coalescer.Ingest(std::move(first));

  project::ProjectChangeBatch second;
  second.file_changes.push_back(project::ProjectFileChange{
      .kind = project::ProjectFileChangeKind::Modified,
      .relative_path = std::filesystem::path("src/a.cpp"),
      .absolute_path = std::filesystem::path("/tmp/src/a.cpp"),
  });
  second.file_changes.push_back(project::ProjectFileChange{
      .kind = project::ProjectFileChangeKind::Modified,
      .relative_path = std::filesystem::path("src/b.cpp"),
      .absolute_path = std::filesystem::path("/tmp/src/b.cpp"),
  });
  coalescer.Ingest(std::move(second));

  const std::optional<project::ProjectChangeBatch> ready = coalescer.ConsumeReady();
  Expect(ready.has_value(), "coalescer should produce one ready batch");
  Expect(ready->file_changes.size() == 2,
         "coalescer should collapse duplicate paths and retain distinct paths");
  Expect(ready->generation == 1, "coalescer should assign a monotonic generation id");
}

// A file-change flood (thousands of distinct paths) must not grow the pending
// list without bound or make the merge O(N^2). Past the cap the coalescer
// collapses to a single full-tree rescan and stops tracking individual paths.
void TestProjectChangeCoalescerCollapsesFloodToRescan() {
  project::ProjectChangeCoalescer coalescer;
  project::ProjectChangeBatch flood;
  for (int i = 0; i < 20000; ++i) {
    const std::string name = "gen/file_" + std::to_string(i) + ".o";
    flood.file_changes.push_back(project::ProjectFileChange{
        .kind = project::ProjectFileChangeKind::Created,
        .relative_path = std::filesystem::path(name),
        .absolute_path = std::filesystem::path("/tmp/" + name),
    });
  }
  coalescer.Ingest(std::move(flood));

  const std::optional<project::ProjectChangeBatch> ready = coalescer.ConsumeReady();
  Expect(ready.has_value(), "the flood should still produce a ready batch");
  Expect(ready->tree_rescan_requested,
         "an over-cap flood must collapse to a full-tree rescan");
  Expect(ready->file_changes.size() <= 1024,
         "pending file changes must stay bounded under a flood");
}

void TestProjectChangeCoalescerSuppressesStaleGeneration() {
  project::ProjectChangeCoalescer coalescer;
  project::ProjectChangeBatch batch;
  batch.file_changes.push_back(project::ProjectFileChange{
      .kind = project::ProjectFileChangeKind::Modified,
      .relative_path = std::filesystem::path("README.md"),
      .absolute_path = std::filesystem::path("/tmp/README.md"),
  });
  coalescer.Ingest(std::move(batch));
  const std::optional<project::ProjectChangeBatch> first = coalescer.ConsumeReady();
  Expect(first.has_value() && first->generation == 1, "first consume should publish generation 1");

  project::ProjectChangeBatch replay = *first;
  coalescer.Ingest(std::move(replay));
  const std::optional<project::ProjectChangeBatch> second = coalescer.ConsumeReady();
  Expect(second.has_value() && second->generation == 2,
         "re-ingested work should receive a newer generation");
}

void TestGitRepositoryMetadataTrackerDetectsHeadChanges() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  std::filesystem::create_directories(root / ".git");
  WriteFile(root / ".git/HEAD", "ref: refs/heads/main\n");
  WriteFile(root / ".git/index", "index\n");

  project::GitRepositoryMetadataTracker tracker;
  tracker.SetProjectRoot(root);
  Expect(tracker.SampleChanges().empty(),
         "initial metadata sample should establish baseline without emitting changes");

  std::this_thread::sleep_for(std::chrono::milliseconds(5));
  WriteFile(root / ".git/HEAD", "ref: refs/heads/feature\n");
  const std::vector<project::RepositoryChange> changes = tracker.SampleChanges();
  Expect(!changes.empty(), "HEAD updates should publish a repository change");
  Expect(std::any_of(changes.begin(), changes.end(),
                     [](const project::RepositoryChange& change) {
                       return change.kind == project::RepositoryChangeKind::HeadChanged;
                     }),
         "HEAD updates should be classified as head changes");
}

// Regression: a linked worktree (or submodule) has a `.git` *file* that points
// at the real git directory via `gitdir: <path>`. The tracker used to stat
// `<root>/.git/HEAD` as if `.git` were a directory, silently producing a
// {head:0, index:0} tick that never changes — so commits/stages in a worktree
// never triggered an auto-refresh. The tracker now follows the pointer.
void TestGitRepositoryMetadataTrackerFollowsWorktreeGitFile() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "worktree";
  const std::filesystem::path real_gitdir = temp_dir.path() / "main/.git/worktrees/wt";
  std::filesystem::create_directories(root);
  std::filesystem::create_directories(real_gitdir);
  WriteFile(real_gitdir / "HEAD", "ref: refs/heads/main\n");
  WriteFile(real_gitdir / "index", "index\n");
  // A linked worktree's `.git` is a FILE pointing at the real gitdir.
  WriteFile(root / ".git", "gitdir: " + real_gitdir.string() + "\n");

  project::GitRepositoryMetadataTracker tracker;
  tracker.SetProjectRoot(root);
  Expect(tracker.SampleChanges().empty(),
         "worktree baseline should be established from the resolved gitdir");

  std::this_thread::sleep_for(std::chrono::milliseconds(5));
  WriteFile(real_gitdir / "HEAD", "ref: refs/heads/feature\n");
  const std::vector<project::RepositoryChange> changes = tracker.SampleChanges();
  Expect(std::any_of(changes.begin(), changes.end(),
                     [](const project::RepositoryChange& change) {
                       return change.kind == project::RepositoryChangeKind::HeadChanged;
                     }),
         "a HEAD change in a worktree's resolved gitdir must be detected");
}

}  // namespace

void RegisterProjectChangeTests(std::vector<TestCase>& tests) {
  AddTest(tests, "ProjectChange/NormalizerMapsIndexUpdates", TestProjectChangeNormalizerMapsIndexUpdates);
  AddTest(tests, "ProjectChange/CoalescerMergesBursts", TestProjectChangeCoalescerMergesBursts);
  AddTest(tests, "ProjectChange/CoalescerCollapsesFloodToRescan",
          TestProjectChangeCoalescerCollapsesFloodToRescan);
  AddTest(tests, "ProjectChange/CoalescerGeneration", TestProjectChangeCoalescerSuppressesStaleGeneration);
  AddTest(tests, "ProjectChange/GitMetadataHeadChange", TestGitRepositoryMetadataTrackerDetectsHeadChanges);
  AddTest(tests, "ProjectChange/GitMetadataWorktreeGitFile",
          TestGitRepositoryMetadataTrackerFollowsWorktreeGitFile);
}

}  // namespace microide::tests
