#include "TestSupport.h"

#include "workspace/RecentsService.h"

#include <cstddef>
#include <filesystem>
#include <string>

namespace microide::tests {
namespace {

using microide::workspace::RecentsService;

// Without Configure() the service is in-memory only (Save() no-ops), so these tests
// exercise the MRU dedup/ordering/bounds logic without touching disk.

void TestRecentsProjectsAreNewestFirstAndDeduped() {
  RecentsService recents;
  recents.RecordProjectOpen("/a");
  recents.RecordProjectOpen("/b");
  recents.RecordProjectOpen("/c");
  recents.RecordProjectOpen("/a");  // re-open promotes to front, no duplicate

  const auto& projects = recents.RecentProjects();
  Expect(projects.size() == 3, "re-opening a project should not duplicate it");
  Expect(projects[0] == std::filesystem::path("/a"), "most recent project should be first");
  Expect(projects[1] == std::filesystem::path("/c"), "second-most-recent project should follow");
  Expect(projects[2] == std::filesystem::path("/b"), "least recent project should be last");
}

void TestRecentsProjectsAreBounded() {
  RecentsService recents;
  for (std::size_t i = 0; i < RecentsService::MaxProjects() + 5; ++i) {
    recents.RecordProjectOpen("/p" + std::to_string(i));
  }
  Expect(recents.RecentProjects().size() == RecentsService::MaxProjects(),
         "recent projects should be capped at MaxProjects()");
  Expect(recents.RecentProjects().front() ==
             std::filesystem::path("/p" + std::to_string(RecentsService::MaxProjects() + 4)),
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
  recents.RecordProjectOpen(one);
  recents.RecordProjectOpen(missing);  // never created on disk

  const auto& existing = recents.ExistingRecentProjects();
  Expect(existing.size() == 1 && existing[0] == one,
         "only on-disk recent projects are surfaced");
  // Second call returns the same cached vector (same MRU revision).
  Expect(&recents.ExistingRecentProjects() == &existing,
         "an unchanged MRU reuses the cached validated list");

  // Recording another project bumps the revision and re-validates.
  recents.RecordProjectOpen(two);
  const auto& after = recents.ExistingRecentProjects();
  Expect(after.size() == 2, "recording a new project invalidates the validated cache");
  Expect(after[0] == two, "the newest recorded project is first");
}

void RegisterRecentsServiceTests(std::vector<TestCase>& tests) {
  AddTest(tests, "RecentsService/ExistingProjectsFilterAndCacheInvalidation",
          TestRecentsExistingProjectsFilterAndCacheInvalidation);
  AddTest(tests, "RecentsService/ProjectsNewestFirstAndDeduped",
          TestRecentsProjectsAreNewestFirstAndDeduped);
  AddTest(tests, "RecentsService/ProjectsBounded", TestRecentsProjectsAreBounded);
  AddTest(tests, "RecentsService/FilesScopedToProjectRoot",
          TestRecentsFilesAreScopedToProjectRoot);
  AddTest(tests, "RecentsService/FilesDedupeOnReopen", TestRecentsFilesDedupeOnReopen);
}

}  // namespace microide::tests
