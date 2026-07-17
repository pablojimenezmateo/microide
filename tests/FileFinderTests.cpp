#include "TestSupport.h"

#include "project/FileFinder.h"
#include "util/PerformanceCounters.h"

#include <algorithm>
#include <filesystem>
#include <initializer_list>
#include <string>
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

// Typing in the finder Refreshes on every keystroke. With the index version
// unchanged, the cheap version check must short-circuit before the O(index)
// snapshot copy/rebuild: the cache is built exactly once across many queries,
// and results stay correct.
void TestFileFinderWarmRefreshDoesNotRebuildPerKeystroke() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "workspace";
  WriteFile(root / "README.md", "root\n");

  FileIndex index;
  Expect(index.SetRoot(root, FileIndex::RootPopulationMode::Deferred),
         "warm-refresh fixture should initialize deferred file index root");
  Expect(index.ApplyBatch(MakeInitialBatch({"src/alpha.cpp", "src/beta.cpp", "src/gamma.cpp"})),
         "warm-refresh fixture should apply initial index batch");

  FileFinder finder;
  finder.SetIndex(&index);

  const std::uint64_t before =
      util::ReadPerformanceCounter(util::PerfCounterId::FileFinderCacheBuildCalls);
  finder.SetQuery("a");
  finder.SetQuery("al");
  finder.SetQuery("alp");
  finder.SetQuery("alph");
  const std::uint64_t after =
      util::ReadPerformanceCounter(util::PerfCounterId::FileFinderCacheBuildCalls);

  Expect(after - before == 1,
         "the finder cache must build once across repeated same-version queries");
  Expect(finder.results().size() == 1 &&
             finder.results().front().relative_path == std::filesystem::path("src/alpha.cpp"),
         "warm narrowing must still return correct results");
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

void TestFileFinderIncrementalTypingMatchesFreshQuery() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "workspace";
  WriteFile(root / "README.md", "root\n");

  FileIndex index;
  Expect(index.SetRoot(root, FileIndex::RootPopulationMode::Deferred),
         "incremental-typing fixture should initialize deferred file index root");
  Expect(index.ApplyBatch(MakeInitialBatch({"src/app/main.cpp", "src/app/render.cpp",
                                            "src/util/string_util.cpp", "src/util/parse.cpp",
                                            "tests/main_test.cpp", "docs/readme.md"})),
         "incremental-typing fixture should apply initial index batch");

  // Forward typing narrows the candidate set; the result must be identical to a
  // fresh finder that jumps straight to the final query (same order and scores).
  FileFinder incremental;
  incremental.SetIndex(&index);
  for (const char* q : {"m", "ma", "mai", "main"}) {
    incremental.SetQuery(q);
  }

  FileFinder fresh;
  fresh.SetIndex(&index);
  fresh.SetQuery("main");

  Expect(incremental.results().size() == fresh.results().size(),
         "incremental typing should yield the same match count as a fresh query");
  for (std::size_t i = 0; i < fresh.results().size(); ++i) {
    Expect(incremental.results()[i].path_string == fresh.results()[i].path_string,
           "incremental typing should yield identical ranked order to a fresh query");
    Expect(incremental.results()[i].score == fresh.results()[i].score,
           "incremental typing should yield identical scores to a fresh query");
  }

  // Backspacing (a shrinking query) must fall back to a full scan and recover the
  // wider match set rather than staying narrowed.
  incremental.SetQuery("ma");
  fresh.SetQuery("ma");
  Expect(incremental.results().size() == fresh.results().size(),
         "backspacing should recover the full match set");
  for (std::size_t i = 0; i < fresh.results().size(); ++i) {
    Expect(incremental.results()[i].path_string == fresh.results()[i].path_string,
           "a backspaced query should match a fresh query of the same text");
  }
}

IndexUpdateBatch MakeInitialBatchFromPaths(const std::vector<std::filesystem::path>& paths) {
  IndexUpdateBatch batch;
  batch.is_initial = true;
  for (const auto& path : paths) {
    batch.changes.push_back(MakeCreateChange(path));
  }
  return batch;
}

// A broad query (here one character) matches nearly the whole index. The finder
// only shows a handful of rows, so the deep-copied result set is capped; the old
// code materialized a FileFinderResult (path + string) per match on every
// keystroke.
void TestFileFinderCapsBroadResultCount() {
  TemporaryDirectory temp_dir;
  const auto root = temp_dir.path();
  FileIndex index;
  Expect(index.SetRoot(root, FileIndex::RootPopulationMode::Deferred),
         "index root should be set");
  std::vector<std::filesystem::path> paths;
  for (int i = 0; i < 600; ++i) {
    paths.push_back(std::filesystem::path("src/aaa" + std::to_string(i) + ".txt"));
  }
  Expect(index.ApplyBatch(MakeInitialBatchFromPaths(paths)), "initial batch should apply");

  FileFinder finder;
  finder.SetIndex(&index);
  finder.SetQuery("a");
  Expect(finder.results().size() == 512,
         "a broad query must cap the materialized result set at kMaxResults (512)");
  for (std::size_t i = 1; i < finder.results().size(); ++i) {
    Expect(finder.results()[i - 1].score <= finder.results()[i].score,
           "capped results must still be score-sorted");
  }
}

// Load-bearing narrowing guard: an entry that ranks past the display cap for a
// broad query must still be reachable when the query narrows to it. This only
// holds because the finder tracks the FULL (uncapped) match set for narrowing —
// capping that set would silently lose the entry.
void TestFileFinderNarrowsToEntryBeyondDisplayCap() {
  TemporaryDirectory temp_dir;
  const auto root = temp_dir.path();
  FileIndex index;
  Expect(index.SetRoot(root, FileIndex::RootPopulationMode::Deferred),
         "index root should be set");
  std::vector<std::filesystem::path> paths;
  for (int i = 0; i < 600; ++i) {
    // No 'z' in these, so they match "a" but not "az".
    paths.push_back(std::filesystem::path("src/aaa" + std::to_string(i) + ".txt"));
  }
  // The single 'z'-bearing target sorts lexicographically last among the "a"
  // matches, so it ranks past the 512-row display cap for query "a".
  paths.push_back(std::filesystem::path("src/aaazzz_target.txt"));
  Expect(index.ApplyBatch(MakeInitialBatchFromPaths(paths)), "initial batch should apply");

  FileFinder finder;
  finder.SetIndex(&index);
  finder.SetQuery("a");
  Expect(finder.results().size() == 512, "broad query result set must be capped");
  const bool target_visible_for_broad_query =
      std::any_of(finder.results().begin(), finder.results().end(), [](const auto& r) {
        return r.path_string.find("aaazzz_target") != std::string::npos;
      });
  Expect(!target_visible_for_broad_query,
         "the target must rank beyond the display cap for the broad query (fixture precondition)");

  finder.SetQuery("az");
  const bool target_found =
      std::any_of(finder.results().begin(), finder.results().end(), [](const auto& r) {
        return r.path_string.find("aaazzz_target") != std::string::npos;
      });
  Expect(target_found,
         "narrowing must reach an entry that ranked beyond the display cap");
  for (const auto& result : finder.results()) {
    Expect(result.path_string.find('z') != std::string::npos,
           "only 'z'-bearing entries should survive the 'az' query");
  }
}

// TD-2026-07-17-076: empty-query recents are deep-copied into results_ BEFORE the
// ranked tail applies its cap. A large (or corrupt persisted) recents list must not
// bypass the visible-result budget, so the recents prefix is now capped too.
void TestFileFinderCapsRecentsOnEmptyQuery() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "workspace";
  WriteFile(root / "README.md", "root\n");

  // Index far more files than the finder's visible cap (512), and make every one a
  // recent so the empty-query recents loop alone would exceed the cap if unbounded.
  constexpr int kFileCount = 900;
  IndexUpdateBatch batch;
  batch.is_initial = true;
  std::vector<std::filesystem::path> recents;
  recents.reserve(kFileCount);
  for (int i = 0; i < kFileCount; ++i) {
    std::filesystem::path rel = std::filesystem::path("src") / ("file_" + std::to_string(i) + ".cpp");
    batch.changes.push_back(MakeCreateChange(rel));
    recents.push_back(std::move(rel));
  }

  FileIndex index;
  Expect(index.SetRoot(root, FileIndex::RootPopulationMode::Deferred),
         "recents-cap fixture should initialize deferred file index root");
  Expect(index.ApplyBatch(std::move(batch)), "recents-cap fixture should apply initial batch");

  FileFinder finder;
  finder.SetIndex(&index);
  finder.SetRecentRelativePaths(recents);
  finder.SetQuery("");

  Expect(finder.results().size() <= 512,
         "empty-query recents must not exceed the finder's visible result cap");
}

void RegisterFileFinderTests(std::vector<TestCase>& tests) {
  AddTest(tests, "FileFinder/CapsRecentsOnEmptyQuery", TestFileFinderCapsRecentsOnEmptyQuery);
  AddTest(tests, "FileFinder/CapsBroadResultCount", TestFileFinderCapsBroadResultCount);
  AddTest(tests, "FileFinder/NarrowsToEntryBeyondDisplayCap",
          TestFileFinderNarrowsToEntryBeyondDisplayCap);
  AddTest(tests, "FileFinder/IncrementalTypingMatchesFreshQuery",
          TestFileFinderIncrementalTypingMatchesFreshQuery);
  AddTest(tests, "FileFinder/PrependsRecentsWhenQueryEmpty",
          TestFileFinderPrependsRecentsWhenQueryEmpty);
  AddTest(tests, "FileFinder/RebuildsCacheWhenFileIndexVersionChanges",
          TestFileFinderRebuildsCacheWhenFileIndexVersionChanges);
  AddTest(tests, "FileFinder/PreservesQueryAcrossIndexChanges",
          TestFileFinderPreservesQueryAcrossIndexChanges);
  AddTest(tests, "FileFinder/WarmRefreshDoesNotRebuildPerKeystroke",
          TestFileFinderWarmRefreshDoesNotRebuildPerKeystroke);
  AddTest(tests, "FileFinder/IncludesHiddenIndexedPaths",
          TestFileFinderIncludesHiddenIndexedPaths);
}

}  // namespace microide::tests
