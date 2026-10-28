#include "TestSupport.h"

#include "persistence/PersistedRecordWriter.h"
#include "workspace/persistence/RecentsService.h"
#include "workspace/persistence/PersistenceService.h"
#include "project/remote/RemoteProject.h"

#include <cstddef>
#include <filesystem>
#include <span>
#include <string>

namespace microide::tests {
namespace {

using microide::workspace::RecentsService;
using microide::project::ProjectId;

ProjectId Local(const std::filesystem::path& root) { return ProjectId::ForLocal(root); }

// Without Configure() the service is in-memory only (Save() no-ops), so these tests
// exercise the MRU dedup/ordering/bounds logic without touching disk.

void TestRecentsProjectsAreNewestFirstAndDeduped() {
  RecentsService recents;
  recents.RecordProjectOpen(Local("/a"));
  recents.RecordProjectOpen(Local("/b"));
  recents.RecordProjectOpen(Local("/c"));
  recents.RecordProjectOpen(Local("/a"));  // re-open promotes to front, no duplicate

  const auto& projects = recents.RecentProjects();
  Expect(projects.size() == 3, "re-opening a project should not duplicate it");
  Expect(projects[0] == Local("/a"), "most recent project should be first");
  Expect(projects[1] == Local("/c"), "second-most-recent project should follow");
  Expect(projects[2] == Local("/b"), "least recent project should be last");
}

void TestRecentsProjectsAreBounded() {
  RecentsService recents;
  for (std::size_t i = 0; i < RecentsService::MaxProjects() + 5; ++i) {
    recents.RecordProjectOpen(Local("/p" + std::to_string(i)));
  }
  Expect(recents.RecentProjects().size() == RecentsService::MaxProjects(),
         "recent projects should be capped at MaxProjects()");
  Expect(recents.RecentProjects().front() ==
             Local("/p" + std::to_string(RecentsService::MaxProjects() + 4)),
         "the newest project should survive the bound");
}

void TestRecentsFilesAreScopedToProjectRoot() {
  RecentsService recents;
  recents.RecordFileOpen("/proj-a/one.cpp", "/proj-a");
  recents.RecordFileOpen("/proj-b/two.cpp", "/proj-b");
  recents.RecordFileOpen("/proj-a/three.cpp", "/proj-a");

  const auto a_files = recents.RecentFilesFor("/proj-a", 10);
  Expect(a_files.size() == 2, "RecentFilesFor should return only the matching project's files");
  Expect(a_files[0] == std::filesystem::path("/proj-a/three.cpp"),
         "recent files should be newest-first");
  Expect(a_files[1] == std::filesystem::path("/proj-a/one.cpp"),
         "older files should follow");

  const auto b_files = recents.RecentFilesFor("/proj-b", 10);
  Expect(b_files.size() == 1 && b_files[0] == std::filesystem::path("/proj-b/two.cpp"),
         "the other project's recents should be isolated");

  Expect(recents.RecentFilesFor("/proj-a", 1).size() == 1,
         "RecentFilesFor should honor the limit");
}

void TestRecentsFilesDedupeOnReopen() {
  RecentsService recents;
  recents.RecordFileOpen("/proj/a.cpp", "/proj");
  recents.RecordFileOpen("/proj/b.cpp", "/proj");
  recents.RecordFileOpen("/proj/a.cpp", "/proj");  // promote, no duplicate

  const auto files = recents.RecentFilesFor("/proj", 10);
  Expect(files.size() == 2, "re-opening a file should not duplicate it");
  Expect(files[0] == std::filesystem::path("/proj/a.cpp"),
         "re-opened file should move to the front");
}

}  // namespace

// TD-2026-07-17A-014: ExistingRecentProjects filters to on-disk paths and caches the
// result against an MRU revision, so the welcome surface never re-stats per paint.
// The cache must invalidate when the MRU changes.
void TestRecentsExistingProjectsFilterAndCacheInvalidation() {
  TemporaryDirectory temp;
  std::filesystem::create_directories(temp.path() / "one");
  std::filesystem::create_directories(temp.path() / "two");
  const std::filesystem::path one = temp.path() / "one";
  const std::filesystem::path two = temp.path() / "two";
  const std::filesystem::path missing = temp.path() / "gone";

  RecentsService recents;
  recents.RecordProjectOpen(Local(one));
  recents.RecordProjectOpen(Local(missing));  // never created on disk

  const auto& existing = recents.ExistingRecentProjects();
  Expect(existing.size() == 1 && existing[0] == Local(one),
         "only on-disk recent projects are surfaced");
  // Second call returns the same cached vector (same MRU revision).
  Expect(&recents.ExistingRecentProjects() == &existing,
         "an unchanged MRU reuses the cached validated list");

  // Recording another project bumps the revision and re-validates.
  recents.RecordProjectOpen(Local(two));
  const auto& after = recents.ExistingRecentProjects();
  Expect(after.size() == 2, "recording a new project invalidates the validated cache");
  Expect(after[0] == Local(two), "the newest recorded project is first");
}

// The MRU coalesces its durable writes, so what lands on disk is no longer a
// straight consequence of RecordFileOpen. Nothing covered the round trip at all
// before -- every other test in this file skips Configure() and stays in memory --
// so the coalescing had no way to be caught if it dropped writes.
void TestRecentsSurviveFlushAndReload() {
  TemporaryDirectory temp_dir;
  ScopedEnvVar scoped_state("XDG_STATE_HOME", temp_dir.path().string());
  const microide::workspace::PersistenceService persistence;

  {
    RecentsService recents;
    recents.Configure(persistence);
    recents.RecordProjectOpen(Local("/proj"));
    recents.RecordFileOpen("/proj/a.cpp", "/proj");
    recents.RecordFileOpen("/proj/b.cpp", "/proj");
    recents.FlushPendingSave();
  }

  RecentsService reloaded;
  reloaded.Configure(persistence);
  const auto files = reloaded.RecentFilesFor("/proj", 10);
  Expect(files.size() == 2, "a flushed MRU should reload both recorded files");
  Expect(files[0] == std::filesystem::path("/proj/b.cpp"),
         "the reloaded MRU should keep newest-first ordering");
  Expect(!reloaded.RecentProjects().empty() &&
             reloaded.RecentProjects().front() == Local("/proj"),
         "a flushed MRU should reload the recorded project");
}

// A burst of opens -- session restore, a multi-select open, arrowing through the
// file finder -- must fold into one durable write, not one per file. Each write is
// a temp+fsync+backup+rename on the shell thread.
void TestRecentsBurstCoalescesIntoOneWrite() {
  TemporaryDirectory temp_dir;
  ScopedEnvVar scoped_state("XDG_STATE_HOME", temp_dir.path().string());
  const microide::workspace::PersistenceService persistence;

  RecentsService recents;
  recents.Configure(persistence);
  const std::uint64_t before =
      microide::persistence::PersistedRecordWriter::WritesOnThisThread();
  for (std::size_t i = 0; i < 40; ++i) {
    recents.RecordFileOpen("/proj/f" + std::to_string(i) + ".cpp", "/proj");
  }
  const std::uint64_t after =
      microide::persistence::PersistedRecordWriter::WritesOnThisThread();
  Expect(after - before <= 1, "a burst of opens must not cost one durable write per file");

  // Nothing is lost: the pending state still lands on flush.
  recents.FlushPendingSave();
  RecentsService reloaded;
  reloaded.Configure(persistence);
  Expect(reloaded.RecentFilesFor("/proj", 100).size() == 40,
         "the coalesced burst must be fully persisted by the flush");
}

// Re-opening what is already newest is a no-op, and a no-op must not write.
void TestRecentsReopeningNewestEntryDoesNotWrite() {
  TemporaryDirectory temp_dir;
  ScopedEnvVar scoped_state("XDG_STATE_HOME", temp_dir.path().string());
  const microide::workspace::PersistenceService persistence;

  RecentsService recents;
  recents.Configure(persistence);
  recents.RecordFileOpen("/proj/a.cpp", "/proj");
  recents.FlushPendingSave();

  const std::uint64_t before =
      microide::persistence::PersistedRecordWriter::WritesOnThisThread();
  for (int i = 0; i < 10; ++i) {
    recents.RecordFileOpen("/proj/a.cpp", "/proj");
    recents.RecordProjectOpen(Local("/proj-x"));
    recents.RecordProjectOpen(Local("/proj-x"));
  }
  recents.FlushPendingSave();
  const std::uint64_t after =
      microide::persistence::PersistedRecordWriter::WritesOnThisThread();
  Expect(after - before <= 1,
         "re-recording the entry that is already newest must not write");
}

// A file written before identity records names its recent projects by path; each
// is named on load as a typed path is, so a mirror's own tree comes back as the
// remote project it mirrors and a plain folder as a local one.
void TestRecentsLegacyPathsResolveOnLoad() {
  TemporaryDirectory state_dir;
  TemporaryDirectory data_dir;
  ScopedEnvVar scoped_state("XDG_STATE_HOME", state_dir.path().string());
  ScopedEnvVar scoped_data("XDG_DATA_HOME", data_dir.path().string());
  const std::filesystem::path plain = state_dir.path() / "plain";
  std::filesystem::create_directories(plain);
  const std::filesystem::path mirror_tree = project::remote::RemoteProject::DefaultTree("dev@box", "/home/dev/app");
  std::filesystem::create_directories(mirror_tree);
  std::filesystem::create_directories(mirror_tree.parent_path() / "meta");
  Expect(project::remote::RemoteProject::WriteRecord(mirror_tree, {.host = "dev@box", .host_root = "/home/dev/app"}),
         "the mirror's record is written");
  const microide::workspace::PersistenceService persistence;
  microide::workspace::PersistedMruState legacy;
  legacy.recent_project_roots = {mirror_tree, plain};
  persistence.SaveMruState(state_dir.path() / "microide" / "recents", legacy);

  RecentsService recents;
  recents.Configure(persistence);
  const auto& projects = recents.ExistingRecentProjects();
  Expect(projects.size() == 2 && projects[0] == ProjectId::ForRemote("dev@box", "/home/dev/app") &&
             projects[1] == Local(plain),
         "the mirror reads back as its remote project, the folder as a local one");
  Expect(projects[0].Location() == "dev@box:/home/dev/app", "and is shown as host:/path");
}

// A remote project outlives its mirror: the entry is its host and folder, and
// opening it makes the mirror again. A local folder that is gone stays hidden.
// Forgetting a remote project (Remote: Remove Mirror) drops it and its files.
void TestRecentsKeepARemoteProjectWhoseMirrorIsGone() {
  TemporaryDirectory temp;
  RecentsService recents;
  const ProjectId remote = ProjectId::ForRemote("dev@box", "/srv/gone");
  recents.RecordProjectOpen(remote);
  recents.RecordProjectOpen(Local(temp.path() / "deleted-local"));
  recents.RecordFileOpen(remote.IoRoot() / "main.c", remote.IoRoot());
  Expect(!std::filesystem::exists(remote.IoRoot()), "the mirror does not exist");
  const auto& existing = recents.ExistingRecentProjects();
  Expect(existing.size() == 1 && existing[0] == remote,
         "the remote project is still offered; the missing local folder is not");

  recents.ForgetProject(remote);
  Expect(recents.ExistingRecentProjects().empty() && recents.RecentFilesFor(remote.IoRoot(), 10).empty(),
         "forgetting it drops the project and its files");
}

// The new file still restores in an older build: each identity is written beside
// its legacy path, in the same order.
void TestRecentsDualWriteTheLegacyRoots() {
  TemporaryDirectory state_dir;
  ScopedEnvVar scoped_state("XDG_STATE_HOME", state_dir.path().string());
  const microide::workspace::PersistenceService persistence;
  const ProjectId remote = ProjectId::ForRemote("dev@box", "/srv/app");
  {
    RecentsService recents;
    recents.Configure(persistence);
    recents.RecordProjectOpen(Local("/proj"));
    recents.RecordProjectOpen(remote);
    recents.FlushPendingSave();
  }
  microide::workspace::PersistedMruState raw;
  Expect(persistence.LoadMruState(state_dir.path() / "microide" / "recents", &raw), "the file loads");
  Expect(raw.recent_projects.size() == 2 && raw.recent_projects[0] == remote &&
             raw.recent_projects[1] == Local("/proj"),
         "the identities are written, newest first");
  Expect(raw.recent_project_roots.size() == 2 && raw.recent_project_roots[0] == remote.IoRoot() &&
             raw.recent_project_roots[1] == std::filesystem::path("/proj"),
         "beside each one's I/O root under the legacy tag");
}

void RegisterRecentsServiceTests(std::vector<TestCase>& tests) {
  AddTest(tests, "RecentsService/LegacyPathsResolveOnLoad", TestRecentsLegacyPathsResolveOnLoad);
  AddTest(tests, "RecentsService/KeepARemoteProjectWhoseMirrorIsGone",
          TestRecentsKeepARemoteProjectWhoseMirrorIsGone);
  AddTest(tests, "RecentsService/DualWriteTheLegacyRoots", TestRecentsDualWriteTheLegacyRoots);
  AddTest(tests, "RecentsService/ExistingProjectsFilterAndCacheInvalidation",
          TestRecentsExistingProjectsFilterAndCacheInvalidation);
  AddTest(tests, "RecentsService/ProjectsNewestFirstAndDeduped",
          TestRecentsProjectsAreNewestFirstAndDeduped);
  AddTest(tests, "RecentsService/ProjectsBounded", TestRecentsProjectsAreBounded);
  AddTest(tests, "RecentsService/FilesScopedToProjectRoot",
          TestRecentsFilesAreScopedToProjectRoot);
  AddTest(tests, "RecentsService/FilesDedupeOnReopen", TestRecentsFilesDedupeOnReopen);
  AddTest(tests, "RecentsService/SurviveFlushAndReload", TestRecentsSurviveFlushAndReload);
  AddTest(tests, "RecentsService/BurstCoalescesIntoOneWrite",
          TestRecentsBurstCoalescesIntoOneWrite);
  AddTest(tests, "RecentsService/ReopeningNewestEntryDoesNotWrite",
          TestRecentsReopeningNewestEntryDoesNotWrite);
}

}  // namespace microide::tests
