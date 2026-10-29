#include "project/GitRepositoryMetadataTracker.h"
#include "project/ProjectChangeCoalescer.h"
#include "project/ProjectChangeNormalizer.h"
#include "platform/FileIndexWatcher.h"
#include "TestSupport.h"
#include "project/GitMetadataSource.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <thread>

#include <sys/stat.h>

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

// The coalescer finds a pending change by its normalized path in an index
// (TD-2026-10-28-418); it used to scan every pending change. Over random bursts --
// repeated paths, the same file spelled `a/./x`, every kind in every order,
// consumes in between -- it must produce exactly what the scan produced: the
// changes in first-seen order, each merged by the same rules. The reference below
// IS the scan.
void TestProjectChangeCoalescerIndexMatchesTheLinearMerge() {
  using project::ProjectFileChange;
  using project::ProjectFileChangeKind;
  const auto reference_merge = [](std::vector<ProjectFileChange>& pending, ProjectFileChange change) {
    const auto normal = [](const std::filesystem::path& path) { return path.lexically_normal(); };
    auto existing = std::find_if(pending.begin(), pending.end(), [&](const ProjectFileChange& candidate) {
      return normal(candidate.relative_path) == normal(change.relative_path);
    });
    if (existing == pending.end()) {
      pending.push_back(std::move(change));
      return;
    }
    if (change.kind == ProjectFileChangeKind::Deleted) {
      existing->kind = ProjectFileChangeKind::Deleted;
      return;
    }
    if (existing->kind == ProjectFileChangeKind::Deleted && change.kind == ProjectFileChangeKind::Modified) {
      existing->kind = ProjectFileChangeKind::Created;
      existing->absolute_path = change.absolute_path;
      return;
    }
    existing->kind = change.kind;
    existing->absolute_path = change.absolute_path;
  };
  std::uint64_t seed = 0xD1B54A32D192ED03ull;
  const auto next = [&seed](std::uint64_t bound) {
    seed ^= seed << 13;
    seed ^= seed >> 7;
    seed ^= seed << 17;
    return seed % bound;
  };
  project::ProjectChangeCoalescer coalescer;
  std::vector<ProjectFileChange> reference;
  for (int round = 0; round < 300; ++round) {
    project::ProjectChangeBatch batch;
    const std::size_t count = 1 + next(40);
    for (std::size_t c = 0; c < count; ++c) {
      const std::string name = "f" + std::to_string(next(25)) + ".txt";
      const std::string dir = next(2) == 0 ? "src" : "lib";
      const std::filesystem::path relative =
          next(4) == 0 ? std::filesystem::path(dir + "/./" + name) : std::filesystem::path(dir + "/" + name);
      const ProjectFileChangeKind kind = static_cast<ProjectFileChangeKind>(next(3));
      ProjectFileChange change{.kind = kind,
                               .relative_path = relative,
                               .absolute_path = "/r/" + dir + "/" + name + "#" + std::to_string(round)};
      reference_merge(reference, change);
      batch.file_changes.push_back(std::move(change));
    }
    coalescer.Ingest(std::move(batch));
    if (next(3) == 0 || round == 299) {
      const std::optional<project::ProjectChangeBatch> ready = coalescer.ConsumeReady();
      Expect(ready.has_value(), "a ready batch");
      bool same = ready->file_changes.size() == reference.size();
      for (std::size_t i = 0; same && i < reference.size(); ++i) {
        same = ready->file_changes[i].kind == reference[i].kind &&
               ready->file_changes[i].relative_path == reference[i].relative_path &&
               ready->file_changes[i].absolute_path == reference[i].absolute_path;
      }
      Expect(same, "round " + std::to_string(round) + ": the index merges as the scan did (" +
                       std::to_string(ready->file_changes.size()) + " vs " + std::to_string(reference.size()) + ")");
      if (!same) {
        return;
      }
      reference.clear();
    }
  }
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
  // The rescan alone does not say that paths were DROPPED: a rescan that still
  // carries 1024 itemized changes looked to the apply like a complete list, and an
  // open file past the cap was never reloaded.
  Expect(ready->file_changes_truncated,
         "a flood past the cap must say its itemized list is incomplete");

  project::ProjectChangeBatch small;
  small.file_changes.push_back(project::ProjectFileChange{
      .kind = project::ProjectFileChangeKind::Modified,
      .relative_path = "a.txt",
      .absolute_path = "/tmp/a.txt",
  });
  small.tree_rescan_requested = true;
  coalescer.Ingest(std::move(small));
  const std::optional<project::ProjectChangeBatch> next = coalescer.ConsumeReady();
  Expect(next.has_value() && !next->file_changes_truncated,
         "the flag clears with the batch that carried it; a rescan under the cap is complete");
}

// The apply rate (TD-2026-10-28-419): the first kApplyBurst batches apply at once,
// then one per kApplyIntervalMs while changes keep coming, the held-back changes
// merging into the next apply rather than being dropped; a quiet spell refills the
// burst; and NextReadyDelayMs names exactly when the held batch becomes ready.
void TestProjectChangeCoalescerRateLimitsAppliesWhileChangesKeepArriving() {
  using project::ProjectChangeCoalescer;
  ProjectChangeCoalescer coalescer;
  int serial = 0;
  const auto write = [&] {
    project::ProjectChangeBatch batch;
    const std::string name = "f" + std::to_string(serial++) + ".txt";
    batch.file_changes.push_back(project::ProjectFileChange{
        .kind = project::ProjectFileChangeKind::Modified,
        .relative_path = name,
        .absolute_path = "/r/" + name,
    });
    coalescer.Ingest(std::move(batch));
  };
  std::uint64_t now = 1000;
  Expect(!coalescer.NextReadyDelayMs(now).has_value(), "nothing pending, no wake");
  for (std::size_t i = 0; i < ProjectChangeCoalescer::kApplyBurst; ++i) {
    write();
    Expect(coalescer.NextReadyDelayMs(now) == std::optional<std::uint64_t>(0), "a burst batch is ready now");
    Expect(coalescer.ConsumeReadyAt(now).has_value(), "the burst applies at once");
    now += 10;
  }
  write();
  Expect(!coalescer.ConsumeReadyAt(now).has_value(), "past the burst the batch is held");
  const std::optional<std::uint64_t> delay = coalescer.NextReadyDelayMs(now);
  Expect(delay.has_value() && *delay > 0 && *delay <= ProjectChangeCoalescer::kApplyIntervalMs,
         "and the held batch names a wake within one interval");
  write();
  Expect(!coalescer.ConsumeReadyAt(now + *delay - 1).has_value(), "not a millisecond early");
  const std::optional<project::ProjectChangeBatch> held = coalescer.ConsumeReadyAt(now + *delay);
  Expect(held.has_value() && held->file_changes.size() == 2,
         "on time, with both held changes merged into one apply");
  now += *delay;
  write();
  Expect(!coalescer.ConsumeReadyAt(now).has_value(), "the steady rate holds the next one");
  Expect(coalescer.NextReadyDelayMs(now) == std::optional<std::uint64_t>(ProjectChangeCoalescer::kApplyIntervalMs),
         "for a whole interval");
  Expect(coalescer.ConsumeReady().has_value(), "an unforced hold never blocks a forced consume");
  now += ProjectChangeCoalescer::kApplyBurst * ProjectChangeCoalescer::kApplyIntervalMs;
  for (std::size_t i = 0; i < ProjectChangeCoalescer::kApplyBurst; ++i) {
    write();
    Expect(coalescer.ConsumeReadyAt(now).has_value(), "a quiet spell refills the whole burst");
  }
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
// Regression: HEAD and the branch ref were compared by modification tick, so
// rewriting either with identical bytes reported a repository change and spawned a
// `git status` with nothing to find. `git checkout <the branch already checked
// out>` does exactly that. Not user-visible — which is why it survived the pass
// that fixed the three visible instances — but it is a subprocess per event, and
// for a remote project it would be a round trip. (TD-2026-09-29-306.)
void TestGitRepositoryMetadataTrackerIgnoresIdenticalHeadRewrite() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  std::filesystem::create_directories(root / ".git/refs/heads");
  WriteFile(root / ".git/HEAD", "ref: refs/heads/main\n");
  WriteFile(root / ".git/refs/heads/main", "1111111111111111111111111111111111111111\n");
  WriteFile(root / ".git/index", "index\n");

  project::GitRepositoryMetadataTracker tracker;
  tracker.SetProjectRoot(root);
  Expect(tracker.SampleChanges().empty(), "the first sample only establishes the baseline");

  // Rewrite BOTH with the same bytes. Their modification ticks move; the
  // repository did not.
  std::this_thread::sleep_for(std::chrono::milliseconds(5));
  WriteFile(root / ".git/HEAD", "ref: refs/heads/main\n");
  WriteFile(root / ".git/refs/heads/main", "1111111111111111111111111111111111111111\n");
  Expect(tracker.SampleChanges().empty(),
         "rewriting HEAD and the branch ref with identical bytes is not a repository change");

  // A real commit — the branch ref advances — must still be reported, or the fix
  // above would just be a way of never noticing anything.
  std::this_thread::sleep_for(std::chrono::milliseconds(5));
  WriteFile(root / ".git/refs/heads/main", "2222222222222222222222222222222222222222\n");
  const std::vector<project::RepositoryChange> advanced = tracker.SampleChanges();
  Expect(std::any_of(advanced.begin(), advanced.end(),
                     [](const project::RepositoryChange& change) {
                       return change.kind == project::RepositoryChangeKind::HeadChanged;
                     }),
         "an advancing branch ref is still a HEAD change");
}

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

// TD-2026-08-06-158: the repository APPEARING or DISAPPEARING is head movement,
// and the tracker used to swallow both — ReadCurrentTicks returns nullopt with no
// usable `.git`, and either transition hit the "no baseline" branch, which
// re-baselines silently. So an in-session `git init` produced no repository
// change, and everything keyed on one (the sidebar's staleness mark, the status
// bar's availability probe) kept the pre-init answer.
void TestGitRepositoryMetadataTrackerDetectsRepositoryAppearingAndDisappearing() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  std::filesystem::create_directories(root);

  project::GitRepositoryMetadataTracker tracker;
  tracker.SetProjectRoot(root);  // no .git yet — an ordinary directory
  Expect(tracker.SampleChanges().empty(), "a project with no repository reports no changes");
  Expect(tracker.SampleChanges().empty(), "and keeps reporting none while that stays true");

  // `git init`.
  std::filesystem::create_directories(root / ".git");
  WriteFile(root / ".git/HEAD", "ref: refs/heads/main\n");
  WriteFile(root / ".git/index", "index\n");
  const std::vector<project::RepositoryChange> appeared = tracker.SampleChanges();
  Expect(std::any_of(appeared.begin(), appeared.end(),
                     [](const project::RepositoryChange& change) {
                       return change.kind == project::RepositoryChangeKind::HeadChanged;
                     }),
         "an in-session git init must publish a repository change");
  Expect(tracker.SampleChanges().empty(), "the transition fires once, not every sample");

  // `rm -rf .git`.
  std::filesystem::remove_all(root / ".git");
  const std::vector<project::RepositoryChange> vanished = tracker.SampleChanges();
  Expect(std::any_of(vanished.begin(), vanished.end(),
                     [](const project::RepositoryChange& change) {
                       return change.kind == project::RepositoryChangeKind::HeadChanged;
                     }),
         "removing the repository must publish a repository change too");
  Expect(tracker.SampleChanges().empty(), "and that transition also fires once");
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

// ReadHeadBranchName backs the status bar's branch label before the first
// `git status` snapshot exists, so it has to handle the same layouts the tracker
// does (ordinary checkout, linked worktree/submodule `.git` file) and refuse to
// invent a branch for a detached HEAD.
void TestReadHeadBranchNameResolvesLayouts() {
  TemporaryDirectory temp_dir;

  const std::filesystem::path plain = temp_dir.path() / "plain";
  std::filesystem::create_directories(plain / ".git");
  WriteFile(plain / ".git" / "HEAD", "ref: refs/heads/main\n");
  Expect(project::ReadHeadBranchName(plain, project::LocalGitMetadataSource()).value_or("") == "main",
         "an ordinary checkout should report its branch");

  // git's short name drops the `refs/heads/` namespace and keeps the slashes
  // (`# branch.head feature/stable-sort`); the label must not change when the
  // first snapshot replaces this fallback.
  WriteFile(plain / ".git" / "HEAD", "ref: refs/heads/feature/stable-sort\n");
  Expect(project::ReadHeadBranchName(plain, project::LocalGitMetadataSource()).value_or("") == "feature/stable-sort",
         "a namespaced branch should report git's short name, slashes included");

  // Detached HEAD: a raw object id, no branch to name.
  WriteFile(plain / ".git" / "HEAD",
            "9f2c1b7a4e6d8c0f1a2b3c4d5e6f708192a3b4c5\n");
  Expect(!project::ReadHeadBranchName(plain, project::LocalGitMetadataSource()).has_value(),
         "a detached HEAD has no branch name");

  // A linked worktree's `.git` is a FILE pointing at the real gitdir.
  const std::filesystem::path worktree = temp_dir.path() / "worktree";
  const std::filesystem::path real_gitdir = temp_dir.path() / "main/.git/worktrees/wt";
  std::filesystem::create_directories(worktree);
  std::filesystem::create_directories(real_gitdir);
  WriteFile(real_gitdir / "HEAD", "ref: refs/heads/wt-branch\n");
  WriteFile(worktree / ".git", "gitdir: " + real_gitdir.string() + "\n");
  Expect(project::ReadHeadBranchName(worktree, project::LocalGitMetadataSource()).value_or("") == "wt-branch",
         "a linked worktree should resolve its own gitdir's HEAD");

  // Not a repository at all.
  const std::filesystem::path bare = temp_dir.path() / "bare";
  std::filesystem::create_directories(bare);
  Expect(!project::ReadHeadBranchName(bare, project::LocalGitMetadataSource()).has_value(),
         "a directory outside any repository has no branch");

  // An unsafe symbolic ref must not resolve (same guard the tracker applies).
  WriteFile(plain / ".git" / "HEAD", "ref: ../../../etc/passwd\n");
  Expect(!project::ReadHeadBranchName(plain, project::LocalGitMetadataSource()).has_value(),
         "an escaping symbolic ref must be refused");
}

}  // namespace

void RegisterProjectChangeTests(std::vector<TestCase>& tests) {
  AddTest(tests, "ProjectChange/ReadHeadBranchNameResolvesLayouts",
          TestReadHeadBranchNameResolvesLayouts);
  AddTest(tests, "ProjectChange/GitMetadataRejectsUnsafeSymbolicRef",
          TestGitRepositoryMetadataTrackerRejectsUnsafeSymbolicRef);
  AddTest(tests, "ProjectChange/GitMetadataSkipsFifoHead",
          TestGitRepositoryMetadataTrackerSkipsFifoHeadWithoutBlocking);
  AddTest(tests, "ProjectChange/GitMetadataSameBranchCommit",
          TestGitRepositoryMetadataTrackerDetectsSameBranchCommit);
  AddTest(tests, "ProjectChange/GitMetadataPackedRefsChange",
          TestGitRepositoryMetadataTrackerDetectsPackedRefsChange);
  AddTest(tests, "ProjectChange/NormalizerMapsIndexUpdates", TestProjectChangeNormalizerMapsIndexUpdates);
  AddTest(tests, "ProjectChange/CoalescerMergesBursts", TestProjectChangeCoalescerMergesBursts);
  AddTest(tests, "ProjectChange/CoalescerIndexMatchesTheLinearMerge",
          TestProjectChangeCoalescerIndexMatchesTheLinearMerge);
  AddTest(tests, "ProjectChange/CoalescerCollapsesFloodToRescan",
          TestProjectChangeCoalescerCollapsesFloodToRescan);
  AddTest(tests, "ProjectChange/CoalescerGeneration", TestProjectChangeCoalescerSuppressesStaleGeneration);
  AddTest(tests, "ProjectChange/CoalescerRateLimitsAppliesWhileChangesKeepArriving",
          TestProjectChangeCoalescerRateLimitsAppliesWhileChangesKeepArriving);
  AddTest(tests, "ProjectChange/GitMetadataHeadChange", TestGitRepositoryMetadataTrackerDetectsHeadChanges);
  AddTest(tests, "ProjectChange/GitMetadataIgnoresIdenticalHeadRewrite",
          TestGitRepositoryMetadataTrackerIgnoresIdenticalHeadRewrite);
  AddTest(tests, "ProjectChange/GitMetadataWorktreeGitFile",
          TestGitRepositoryMetadataTrackerFollowsWorktreeGitFile);
  AddTest(tests, "ProjectChange/GitMetadataRepositoryAppearsAndDisappears",
          TestGitRepositoryMetadataTrackerDetectsRepositoryAppearingAndDisappearing);
}

}  // namespace microide::tests
