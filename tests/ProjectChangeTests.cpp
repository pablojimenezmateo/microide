#include "project/GitRepositoryMetadataTracker.h"
#include "project/ProjectChangeCoalescer.h"
#include "project/ProjectChangeNormalizer.h"
#include "platform/FileIndexWatcher.h"
#include "TestSupport.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <thread>

#if defined(__unix__) || defined(__APPLE__)
#include <sys/stat.h>
#endif

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

// TD-2026-07-16-63: an ORDINARY same-branch commit leaves `.git/HEAD` text unchanged
// but advances `refs/heads/<branch>`. The tracker must detect that via the resolved
// branch-ref tick, not only branch switches that rewrite HEAD.
void TestGitRepositoryMetadataTrackerDetectsSameBranchCommit() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  std::filesystem::create_directories(root / ".git/refs/heads");
  WriteFile(root / ".git/HEAD", "ref: refs/heads/main\n");
  WriteFile(root / ".git/index", "index\n");
  WriteFile(root / ".git/refs/heads/main", "1111111111111111111111111111111111111111\n");

  project::GitRepositoryMetadataTracker tracker;
  tracker.SetProjectRoot(root);
  Expect(tracker.SampleChanges().empty(), "baseline established with no changes");

  std::this_thread::sleep_for(std::chrono::milliseconds(5));
  // Same branch, new commit: HEAD text is UNCHANGED, only the branch ref advances.
  WriteFile(root / ".git/refs/heads/main", "2222222222222222222222222222222222222222\n");
  const std::vector<project::RepositoryChange> changes = tracker.SampleChanges();
  Expect(std::any_of(changes.begin(), changes.end(),
                     [](const project::RepositoryChange& change) {
                       return change.kind == project::RepositoryChangeKind::HeadChanged;
                     }),
         "a same-branch commit advancing refs/heads/main must be detected as a head change");
}

// A packed-refs change (branch ref stored packed, no loose file) is also treated as
// possible head movement.
void TestGitRepositoryMetadataTrackerDetectsPackedRefsChange() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  std::filesystem::create_directories(root / ".git");
  WriteFile(root / ".git/HEAD", "ref: refs/heads/main\n");
  WriteFile(root / ".git/index", "index\n");
  WriteFile(root / ".git/packed-refs", "# pack-refs with: peeled fully-peeled sorted\n");

  project::GitRepositoryMetadataTracker tracker;
  tracker.SetProjectRoot(root);
  Expect(tracker.SampleChanges().empty(), "baseline established with no changes");

  std::this_thread::sleep_for(std::chrono::milliseconds(5));
  WriteFile(root / ".git/packed-refs",
            "# pack-refs with: peeled fully-peeled sorted\n"
            "3333333333333333333333333333333333333333 refs/heads/main\n");
  const std::vector<project::RepositoryChange> changes = tracker.SampleChanges();
  Expect(std::any_of(changes.begin(), changes.end(),
                     [](const project::RepositoryChange& change) {
                       return change.kind == project::RepositoryChangeKind::HeadChanged;
                     }),
         "a packed-refs change must be treated as possible head movement");
}

// TD-2026-07-17A-110: a malformed symbolic HEAD with an absolute (unsafe) ref must not
// be followed. Otherwise `common_dir / ref` resolves to a path outside the git dir and
// that external file would drive change detection.
void TestGitRepositoryMetadataTrackerRejectsUnsafeSymbolicRef() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  std::filesystem::create_directories(root / ".git");
  WriteFile(root / ".git/index", "index\n");
  // A file OUTSIDE the git dir that a malformed absolute ref points at.
  const std::filesystem::path external = temp_dir.path() / "external_target";
  WriteFile(external, "one\n");
  WriteFile(root / ".git/HEAD", "ref: " + external.string() + "\n");

  project::GitRepositoryMetadataTracker tracker;
  tracker.SetProjectRoot(root);
  Expect(tracker.SampleChanges().empty(), "baseline established from the (rejected) unsafe ref");

  std::this_thread::sleep_for(std::chrono::milliseconds(5));
  WriteFile(external, "two\n");  // mutate the external target the unsafe ref named
  const std::vector<project::RepositoryChange> changes = tracker.SampleChanges();
  Expect(std::none_of(changes.begin(), changes.end(),
                      [](const project::RepositoryChange& change) {
                        return change.kind == project::RepositoryChangeKind::HeadChanged;
                      }),
         "an absolute (unsafe) symbolic HEAD ref must not be followed for change detection");
}

#if defined(__unix__) || defined(__APPLE__)
// TD-2026-07-17A-113: a special file (FIFO) named HEAD must be rejected before any
// stream open, or opening it O_RDONLY would block the sampling thread forever. If the
// guard regressed, this test would hang and be killed by the ctest timeout.
void TestGitRepositoryMetadataTrackerSkipsFifoHeadWithoutBlocking() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  std::filesystem::create_directories(root / ".git");
  WriteFile(root / ".git/index", "index\n");
  const std::filesystem::path head = root / ".git/HEAD";
  Expect(::mkfifo(head.c_str(), 0644) == 0, "FIFO HEAD fixture created");

  project::GitRepositoryMetadataTracker tracker;
  tracker.SetProjectRoot(root);  // must return promptly, not block on the FIFO
  // Index still ticks normally; a FIFO HEAD is simply treated as unavailable.
  Expect(tracker.SampleChanges().empty(), "sampling a FIFO HEAD returns without blocking");
}
#endif

// ReadHeadBranchName backs the status bar's branch label before the first
// `git status` snapshot exists, so it has to handle the same layouts the tracker
// does (ordinary checkout, linked worktree/submodule `.git` file) and refuse to
// invent a branch for a detached HEAD.
void TestReadHeadBranchNameResolvesLayouts() {
  TemporaryDirectory temp_dir;

  const std::filesystem::path plain = temp_dir.path() / "plain";
  std::filesystem::create_directories(plain / ".git");
  WriteFile(plain / ".git" / "HEAD", "ref: refs/heads/main\n");
  Expect(project::ReadHeadBranchName(plain).value_or("") == "main",
         "an ordinary checkout should report its branch");

  // A branch name with slashes keeps only the leaf, matching git's short name.
  WriteFile(plain / ".git" / "HEAD", "ref: refs/heads/feature/stable-sort\n");
  Expect(project::ReadHeadBranchName(plain).value_or("") == "stable-sort",
         "a namespaced branch should report its short name");

  // Detached HEAD: a raw object id, no branch to name.
  WriteFile(plain / ".git" / "HEAD",
            "9f2c1b7a4e6d8c0f1a2b3c4d5e6f708192a3b4c5\n");
  Expect(!project::ReadHeadBranchName(plain).has_value(),
         "a detached HEAD has no branch name");

  // A linked worktree's `.git` is a FILE pointing at the real gitdir.
  const std::filesystem::path worktree = temp_dir.path() / "worktree";
  const std::filesystem::path real_gitdir = temp_dir.path() / "main/.git/worktrees/wt";
  std::filesystem::create_directories(worktree);
  std::filesystem::create_directories(real_gitdir);
  WriteFile(real_gitdir / "HEAD", "ref: refs/heads/wt-branch\n");
  WriteFile(worktree / ".git", "gitdir: " + real_gitdir.string() + "\n");
  Expect(project::ReadHeadBranchName(worktree).value_or("") == "wt-branch",
         "a linked worktree should resolve its own gitdir's HEAD");

  // Not a repository at all.
  const std::filesystem::path bare = temp_dir.path() / "bare";
  std::filesystem::create_directories(bare);
  Expect(!project::ReadHeadBranchName(bare).has_value(),
         "a directory outside any repository has no branch");

  // An unsafe symbolic ref must not resolve (same guard the tracker applies).
  WriteFile(plain / ".git" / "HEAD", "ref: ../../../etc/passwd\n");
  Expect(!project::ReadHeadBranchName(plain).has_value(),
         "an escaping symbolic ref must be refused");
}

}  // namespace

void RegisterProjectChangeTests(std::vector<TestCase>& tests) {
  AddTest(tests, "ProjectChange/ReadHeadBranchNameResolvesLayouts",
          TestReadHeadBranchNameResolvesLayouts);
  AddTest(tests, "ProjectChange/GitMetadataRejectsUnsafeSymbolicRef",
          TestGitRepositoryMetadataTrackerRejectsUnsafeSymbolicRef);
#if defined(__unix__) || defined(__APPLE__)
  AddTest(tests, "ProjectChange/GitMetadataSkipsFifoHead",
          TestGitRepositoryMetadataTrackerSkipsFifoHeadWithoutBlocking);
#endif
  AddTest(tests, "ProjectChange/GitMetadataSameBranchCommit",
          TestGitRepositoryMetadataTrackerDetectsSameBranchCommit);
  AddTest(tests, "ProjectChange/GitMetadataPackedRefsChange",
          TestGitRepositoryMetadataTrackerDetectsPackedRefsChange);
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
