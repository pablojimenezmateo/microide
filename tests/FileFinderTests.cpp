#include "TestSupport.h"

#include "project/FileFinder.h"

#include <filesystem>
#include <initializer_list>
#include <vector>

namespace microide::tests {
namespace {

using microide::platform::IndexUpdateBatch;
using microide::project::FileFinder;
using microide::project::FileIndex;

IndexUpdateBatch::Change MakeCreateChange(std::filesystem::path relative_path,
                                          std::uintmax_t size = 0) {
  IndexUpdateBatch::Change change;
  change.kind = IndexUpdateBatch::Kind::CreatedOrModified;
  change.entry.relative_path = std::move(relative_path);
  change.entry.size = size;
  return change;
}

IndexUpdateBatch MakeInitialBatch(
    std::initializer_list<std::filesystem::path> relative_paths) {
  IndexUpdateBatch batch;
  batch.is_initial = true;
  for (const auto& path : relative_paths) {
    batch.changes.push_back(MakeCreateChange(path));
  }
  return batch;
}

void TestFileFinderRebuildsCacheWhenFileIndexVersionChanges() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "workspace";
  WriteFile(root / "README.md", "root\n");

  FileIndex index;
  Expect(index.SetRoot(root, FileIndex::RootPopulationMode::Deferred),
         "file finder fixture should initialize deferred file index root");
  Expect(index.ApplyBatch(MakeInitialBatch({"src/foo.cpp"})),
         "file finder fixture should apply initial index batch");

  FileFinder finder;
  finder.SetIndex(&index);
  finder.SetQuery("foo");
  Expect(finder.results().size() == 1,
         "file finder should include initial indexed matches");

  IndexUpdateBatch incremental;
  incremental.is_initial = false;
  incremental.changes.push_back(MakeCreateChange("src/foobar.cpp"));
  Expect(index.ApplyBatch(incremental),
         "incremental create should mutate the file index");

  finder.Refresh();
  Expect(finder.results().size() == 2,
         "file finder should rebuild cached entries after file index version changes");
}

void TestFileFinderPreservesQueryAcrossIndexChanges() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "workspace";
  WriteFile(root / "README.md", "root\n");

  FileIndex index;
  Expect(index.SetRoot(root, FileIndex::RootPopulationMode::Deferred),
         "query-preservation fixture should initialize deferred file index root");
  Expect(index.ApplyBatch(MakeInitialBatch({"src/alpha.cpp"})),
         "query-preservation fixture should apply initial index batch");

  FileFinder finder;
  finder.SetIndex(&index);
  finder.SetQuery("needle");
  Expect(finder.query() == "needle",
         "file finder should persist the query text after initial refresh");
  Expect(finder.results().empty(),
         "query-preservation fixture should start without matching files");

  IndexUpdateBatch incremental;
  incremental.is_initial = false;
  incremental.changes.push_back(MakeCreateChange("src/needle_file.cpp"));
  Expect(index.ApplyBatch(incremental),
         "query-preservation fixture should mutate index contents");

  finder.Refresh();
  Expect(finder.query() == "needle",
         "file finder should preserve query text after index cache rebuild");
  Expect(finder.results().size() == 1 &&
             finder.results().front().relative_path ==
                 std::filesystem::path("src/needle_file.cpp"),
         "file finder should surface new matches using the existing query");
}

void TestFileFinderIncludesHiddenIndexedPaths() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "workspace";
  WriteFile(root / "README.md", "root\n");

  FileIndex index;
  Expect(index.SetRoot(root, FileIndex::RootPopulationMode::Deferred),
         "hidden-path fixture should initialize deferred file index root");
  Expect(index.ApplyBatch(MakeInitialBatch({".hidden/config.json", "src/main.cpp"})),
         "hidden-path fixture should apply initial index batch including hidden entries");

  FileFinder finder;
  finder.SetIndex(&index);
  finder.SetQuery("hidden");
  Expect(finder.results().size() == 1 &&
             finder.results().front().relative_path ==
                 std::filesystem::path(".hidden/config.json"),
         "file finder hidden-path behavior should remain compatible with indexed hidden entries");
}

}  // namespace

void TestFileFinderPrependsRecentsWhenQueryEmpty() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "workspace";
  WriteFile(root / "README.md", "root\n");

  FileIndex index;
  Expect(index.SetRoot(root, FileIndex::RootPopulationMode::Deferred),
         "recents fixture should initialize deferred file index root");
  Expect(index.ApplyBatch(MakeInitialBatch({"src/a.cpp", "src/b.cpp", "src/c.cpp"})),
         "recents fixture should apply initial index batch");

  FileFinder finder;
  finder.SetIndex(&index);
  // A recent that exists, a duplicate, and one that is no longer indexed.
  finder.SetRecentRelativePaths({"src/c.cpp", "src/c.cpp", "gone/missing.cpp"});
  finder.SetQuery("");

  Expect(finder.results().size() == 3,
         "empty finder should list every indexed file exactly once alongside recents");
  Expect(finder.results().front().path_string == "src/c.cpp",
         "the recent file should lead the empty finder");
  std::size_t c_occurrences = 0;
  for (const auto& result : finder.results()) {
    if (result.path_string == "src/c.cpp") {
      ++c_occurrences;
    }
  }
  Expect(c_occurrences == 1, "a recent file should not be duplicated in the ranked listing");

  // Once the user types, recents no longer force ordering — normal ranking applies.
  finder.SetQuery("a");
  Expect(finder.results().size() == 1 && finder.results().front().path_string == "src/a.cpp",
         "typing a query should fall back to ranked matching");
}

void RegisterFileFinderTests(std::vector<TestCase>& tests) {
  AddTest(tests, "FileFinder/PrependsRecentsWhenQueryEmpty",
          TestFileFinderPrependsRecentsWhenQueryEmpty);
  AddTest(tests, "FileFinder/RebuildsCacheWhenFileIndexVersionChanges",
          TestFileFinderRebuildsCacheWhenFileIndexVersionChanges);
  AddTest(tests, "FileFinder/PreservesQueryAcrossIndexChanges",
          TestFileFinderPreservesQueryAcrossIndexChanges);
  AddTest(tests, "FileFinder/IncludesHiddenIndexedPaths",
          TestFileFinderIncludesHiddenIndexedPaths);
}

}  // namespace microide::tests
