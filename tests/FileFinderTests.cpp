#include "TestSupport.h"

#include "project/FileFinder.h"
#include "util/PerformanceCounters.h"
#include "util/StringUtil.h"

#include <algorithm>
#include <filesystem>
#include <initializer_list>
#include <cstdint>
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
             finder.results().front().path_string == "src/needle_file.cpp",
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
             finder.results().front().path_string == ".hidden/config.json",
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
             finder.results().front().path_string == "src/alpha.cpp",
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
// Recents lead the empty finder in NEWEST-FIRST order, which is the order they
// were handed in — not the order they happen to sit in the file index. The
// resolution used to be a lookup per recent against a path -> index map built over
// every indexed file; it is a single scan of the index against a map of the
// recents now, so the emitted order comes from an explicit rank rather than from
// the iteration. Give it recents whose newest-first order is the REVERSE of index
// order, so an implementation that emits in scan order fails.
void TestFileFinderEmitsRecentsNewestFirstRegardlessOfIndexOrder() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "workspace";
  WriteFile(root / "README.md", "root\n");

  FileIndex index;
  Expect(index.SetRoot(root, FileIndex::RootPopulationMode::Deferred),
         "recents-order fixture should initialize deferred file index root");
  Expect(index.ApplyBatch(MakeInitialBatch({"src/a.cpp", "src/b.cpp", "src/c.cpp",
                                            "src/d.cpp"})),
         "recents-order fixture should apply initial index batch");

  FileFinder finder;
  finder.SetIndex(&index);
  finder.SetRecentRelativePaths({"src/d.cpp", "src/b.cpp", "src/a.cpp"});
  finder.SetQuery("");

  Expect(finder.results().size() == 4, "every indexed file should be listed exactly once");
  Expect(finder.results()[0].path_string == "src/d.cpp" &&
             finder.results()[1].path_string == "src/b.cpp" &&
             finder.results()[2].path_string == "src/a.cpp",
         "recents must lead in newest-first order, not in file-index order");
  Expect(finder.results()[3].path_string == "src/c.cpp",
         "the non-recent file should follow the recents");
}

// The cached folded filename is a suffix of the cached folded path rather than a
// separately folded string. That is only sound if case folding cannot move the
// component boundary — it cannot introduce or remove an ASCII separator, but it
// CAN change a component's byte length. Pin it with a filename whose fold is
// longer than the original (U+0130 folds to two code points) and with a file that
// has no directory component at all.
void TestFileFinderFoldedFilenameMatchesSeparateFold() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "workspace";
  WriteFile(root / "README.md", "root\n");

  FileIndex index;
  Expect(index.SetRoot(root, FileIndex::RootPopulationMode::Deferred),
         "fold fixture should initialize deferred file index root");
  // "\xC4\xB0" is U+0130 LATIN CAPITAL LETTER I WITH DOT ABOVE, whose full case
  // fold is two code points (three bytes) — longer than the original two.
  Expect(index.ApplyBatch(MakeInitialBatch({"toplevel.cpp", "SRC/\xC4\xB0stanbul.cpp",
                                            "deep/nested/dir/Widget.cpp"})),
         "fold fixture should apply initial index batch");

  FileFinder finder;
  finder.SetIndex(&index);

  // A filename-only query must still match each file through the filename path,
  // including the one whose folded filename changed length and the one with no
  // directory component.
  finder.SetQuery("widget");
  Expect(finder.results().size() == 1 &&
             finder.results().front().path_string == "deep/nested/dir/Widget.cpp",
         "a filename query should match through the folded filename suffix");

  finder.SetQuery("toplevel");
  Expect(finder.results().size() == 1 &&
             finder.results().front().path_string == "toplevel.cpp",
         "a file with no directory component should match on its whole path as filename");

  finder.SetQuery("stanbul");
  Expect(finder.results().size() == 1,
         "a filename whose case fold changes byte length should still match");
}

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

// The finder reports its backing index's truncation so the overlay can flag an
// incomplete result set instead of presenting a prefix of a huge tree as the
// authoritative file list (TD-2026-07-17-008/033).
// Drives the real finder over a fixed path set and reports the ranked order, so
// a ranking assertion reads as "this path must come before that one" rather than
// as arithmetic on scores nobody can check by eye.
class RankingFixture {
 public:
  explicit RankingFixture(const std::vector<std::filesystem::path>& paths) {
    root_ = temp_.path() / "workspace";
    WriteFile(root_ / "README.md", "root\n");
    Expect(index_.SetRoot(root_, FileIndex::RootPopulationMode::Deferred),
           "ranking fixture should initialize a deferred index root");
    Expect(index_.ApplyBatch(MakeInitialBatchFromPaths(paths)),
           "ranking fixture should apply its initial batch");
    finder_.SetIndex(&index_);
  }

  // Position of `path` in the ranked results, or a large sentinel when absent —
  // an assertion comparing two positions then fails loudly on a missing match
  // instead of passing on two equal sentinels.
  std::size_t RankOf(const std::string& query, const std::string& path) {
    finder_.SetQuery("");  // full scan, not the narrowing path
    finder_.SetQuery(query);
    const auto& results = finder_.results();
    for (std::size_t i = 0; i < results.size(); ++i) {
      if (results[i].path_string == path) {
        return i;
      }
    }
    return std::numeric_limits<std::size_t>::max();
  }

  void ExpectRanksAbove(const std::string& query, const std::string& better,
                        const std::string& worse) {
    const std::size_t better_rank = RankOf(query, better);
    const std::size_t worse_rank = RankOf(query, worse);
    Expect(better_rank != std::numeric_limits<std::size_t>::max(),
           ("query '" + query + "' should match '" + better + "' at all").c_str());
    Expect(worse_rank != std::numeric_limits<std::size_t>::max(),
           ("query '" + query + "' should match '" + worse + "' at all").c_str());
    Expect(better_rank < worse_rank,
           ("query '" + query + "': '" + better + "' should rank above '" + worse + "'").c_str());
  }

 private:
  TemporaryDirectory temp_;
  std::filesystem::path root_;
  FileIndex index_;
  FileFinder finder_;
};

// The old scorer had two terms — where the first matched character landed, and
// the total gap — so a query that appears CONTIGUOUSLY late in a name lost to
// one scattered across an earlier one. Contiguity is the strongest signal a
// quick-open has, so the run bonus is superlinear in run length.
void TestFileFinderRanksContiguousMatchesFirst() {
  RankingFixture fixture({
      "src/project/FileFinder.cpp",
      "src/foo/find_error.cpp",
      "src/f/i/n/d/e/r.cpp",
  });
  fixture.ExpectRanksAbove("finder", "src/project/FileFinder.cpp", "src/foo/find_error.cpp");
  fixture.ExpectRanksAbove("finder", "src/project/FileFinder.cpp", "src/f/i/n/d/e/r.cpp");
}

// The user-visible complaint that started this: with two equally good filename
// matches, the shorter and shallower path is the one that was meant.
void TestFileFinderPrefersShorterShallowerPaths() {
  RankingFixture fixture({
      "index.ts",
      "app/index.ts",
      "app/features/deeply/nested/area/index.ts",
  });
  fixture.ExpectRanksAbove("index", "index.ts", "app/index.ts");
  fixture.ExpectRanksAbove("index", "app/index.ts",
                           "app/features/deeply/nested/area/index.ts");
}

// camelCase initials are how people type a long identifier-shaped filename. The
// fold destroys the case that defines a hump, so this only works because the
// entry keeps the original bytes and the scorer reads them when the fold left
// the offsets aligned.
void TestFileFinderRanksCamelCaseInitials() {
  RankingFixture fixture({
      "src/FooBarConfig.cpp",
      "src/fabric_cache.cpp",
  });
  fixture.ExpectRanksAbove("fbc", "src/FooBarConfig.cpp", "src/fabric_cache.cpp");
}

// A match on the filename outranks a match that only exists across the path.
// VSCode's rule (label before description), and what people mean when they say a
// finder "found the wrong file".
void TestFileFinderPrefersFilenameMatchesOverPathMatches() {
  RankingFixture fixture({
      "src/utils/index.ts",
      "src/deeply/nested/area/utils.ts",
  });
  fixture.ExpectRanksAbove("utils", "src/deeply/nested/area/utils.ts", "src/utils/index.ts");
}

// A word start beats mid-word, and an exact filename beats everything.
void TestFileFinderPrefersWordStartsAndExactNames() {
  RankingFixture fixture({
      "src/config.ts",
      "src/preconfigured.ts",
      "src/config_loader.ts",
  });
  fixture.ExpectRanksAbove("config", "src/config.ts", "src/preconfigured.ts");
  fixture.ExpectRanksAbove("config", "src/config.ts", "src/config_loader.ts");

  RankingFixture exact({
      "README.md",
      "docs/readme_generator.md",
      "docs/old/README.md.bak",
  });
  exact.ExpectRanksAbove("readme.md", "README.md", "docs/old/README.md.bak");
  exact.ExpectRanksAbove("readme", "README.md", "docs/readme_generator.md");
}

// A query that spans a separator is a path query; it must still find the file.
void TestFileFinderMatchesAcrossPathSeparators() {
  RankingFixture fixture({
      "src/project/FileFinder.cpp",
      "src/projection/file_finder_helper.cpp",
  });
  fixture.ExpectRanksAbove("project/filefinder", "src/project/FileFinder.cpp",
                           "src/projection/file_finder_helper.cpp");
}


// A query containing '/' names a DIRECTORY and a FILE, and the tail past the last
// separator has to be scored against the basename with the full filename
// signal — the exact-name bonus, the prefix bonus, the filename-length term.
//
// Before the split, a separator query could only ever land in the path-only class
// (a '/' cannot appear in a filename), which threw all of that away: `editor/tv`
// scored `TextViewportInternal.h` within two points of `TextViewport.cpp`, and
// `util/str` tied `StringUtil.h` with `StringUtil.cpp` exactly. VSCode splits at
// the last separator for the same reason.
void TestFileFinderSplitsQueryAtTheLastSeparator() {
  RankingFixture fixture({
      "src/editor/TextViewport.cpp",
      "src/editor/TextViewport.h",
      "src/editor/TextViewportInternal.h",
      "src/editor/TextViewportEditEngine.cpp",
      "src/editor/TextViewportMultiCaret.cpp",
      "tests/TextViewportTests.cpp",
  });
  // The tail is a filename PREFIX of TextViewport.*, and only a scattered
  // interior match of the longer names.
  fixture.ExpectRanksAbove("editor/tv", "src/editor/TextViewport.h",
                           "src/editor/TextViewportInternal.h");
  fixture.ExpectRanksAbove("editor/tv", "src/editor/TextViewportInternal.h",
                           "src/editor/TextViewportEditEngine.cpp");
  // The head still filters: a file whose directory cannot match is excluded even
  // though its basename does.
  Expect(fixture.RankOf("editor/textviewport", "tests/TextViewportTests.cpp") ==
             std::numeric_limits<std::size_t>::max(),
         "the directory half of a separator query must exclude a non-matching directory");
  Expect(fixture.RankOf("tests/textviewport", "tests/TextViewportTests.cpp") == 0,
         "the file whose directory AND basename both match leads");
  Expect(fixture.RankOf("tests/textviewport", "src/editor/TextViewport.cpp") ==
             std::numeric_limits<std::size_t>::max(),
         "a better basename under a non-matching directory is still excluded");
}

// A separator query must still outrank the same files reached without one, i.e.
// the split must not demote a match into the path-only class. `util/str` used to
// tie `StringUtil.h` and `StringUtil.cpp` at exactly the same path-only score,
// because neither the filename length nor the prefix bonus was in play.
void TestFileFinderSeparatorQueryKeepsFilenameOrdering() {
  RankingFixture fixture({
      "src/util/StringUtil.cpp",
      "src/util/StringUtil.h",
      "src/util/PathMatch.h",
      "src/util/Parse.h",
  });
  // Shorter basename first, as it is for the same query without a directory.
  fixture.ExpectRanksAbove("util/str", "src/util/StringUtil.h", "src/util/StringUtil.cpp");
  // A basename match beats a directory-only match, which is what "label before
  // description" means.
  fixture.ExpectRanksAbove("src/util", "src/util/StringUtil.h", "src/util/Parse.h");
  // A trailing separator has no filename half at all and stays a pure directory
  // filter, so everything under it still matches.
  Expect(fixture.RankOf("util/", "src/util/Parse.h") != std::numeric_limits<std::size_t>::max(),
         "a trailing-separator query stays a directory filter");
}

void TestFileFinderReportsTruncatedIndex() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "workspace";
  WriteFile(root / "README.md", "root\n");

  FileIndex index;
  Expect(index.SetRoot(root, FileIndex::RootPopulationMode::Deferred),
         "truncation fixture should initialize deferred file index root");

  FileFinder finder;
  finder.SetIndex(&index);
  Expect(!finder.index_truncated(), "a fresh index is not truncated");

  IndexUpdateBatch batch;
  batch.is_initial = true;
  batch.truncated = true;
  batch.changes.push_back(MakeCreateChange("src/foo.cpp"));
  Expect(index.ApplyBatch(std::move(batch)),
         "truncation fixture should apply the truncated initial batch");

  Expect(finder.index_truncated(),
         "the finder must surface the backing index's truncation status");
}

// RankMatchCached rejects an entry outright when the query's character-presence
// bitmask is not a subset of the entry's. That is only sound in one direction:
// the mask may pass a candidate that cannot actually match (bucket collisions —
// bytes 64 apart share a bit), which the real subsequence scan then rejects, but
// it must NEVER reject a candidate that would have matched. This drives the
// finder against an independently-computed subsequence oracle so a false
// negative shows up as a missing result.
void TestFileFinderPresenceMaskNeverRejectsAMatch() {
  // Case-insensitive subsequence test, restated independently of the finder.
  const auto is_subsequence = [](std::string_view text, std::string_view query) {
    std::size_t q = 0;
    for (std::size_t i = 0; i < text.size() && q < query.size(); ++i) {
      const char folded = microide::util::ToLowerAsciiChar(text[i]);
      if (folded == microide::util::ToLowerAsciiChar(query[q])) {
        ++q;
      }
    }
    return q == query.size();
  };

  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "workspace";
  WriteFile(root / "README.md", "root\n");

  // Paths chosen to stress the 64-bucket mask: '!'(0x21)/'a'(0x61),
  // '.'(0x2E)/'n'(0x6E), and '/'(0x2F)/'o'(0x6F) each collide, and mixed case
  // exercises the fold applied before the mask is built.
  const std::vector<std::filesystem::path> paths = {
      "src/Alpha.cpp",      "src/alpha_impl.h",   "src/a!b/Node.cpp",
      "tests/Beta.test.cc", "docs/read.me.md",    "tools/run-checks.sh",
      "a/b/c/d/e/deep.txt", "UPPER/CASE/File.MD", "x!y.z",
      "no-vowels/xyz.qq",   "src/workspace/Shell.cpp",
  };

  FileIndex index;
  Expect(index.SetRoot(root, FileIndex::RootPopulationMode::Deferred),
         "presence-mask fixture should initialize a deferred index root");
  IndexUpdateBatch batch;
  batch.is_initial = true;
  for (const auto& path : paths) {
    batch.changes.push_back(MakeCreateChange(path));
  }
  Expect(index.ApplyBatch(batch), "presence-mask fixture should apply its initial batch");

  FileFinder finder;
  finder.SetIndex(&index);

  const std::vector<std::string> queries = {
      "a",     "alpha", "ac",   "acp",   "shell", "SHELL", "md",   "!",     "a!b",
      "ne",    "x.z",   "xyz",  "qq",    "deep",  "abcde", "zzzz", "",      "u/c/f",
      "run",   "sh",    "Node", "readme", "..",   "//",    "cpp",
  };

  for (const std::string& query : queries) {
    finder.SetQuery("");  // force a full scan rather than the narrowing path
    finder.SetQuery(query);

    // Every path the oracle says should match must be present in the results.
    // (Results are capped at 512; this fixture is far below that, so the cap
    // cannot mask a false negative here.)
    for (const std::filesystem::path& path : paths) {
      const std::string path_string = path.string();
      const std::string filename = path.filename().string();
      if (!is_subsequence(path_string, query) && !is_subsequence(filename, query)) {
        continue;
      }
      const bool found =
          std::any_of(finder.results().begin(), finder.results().end(),
                      [&](const auto& result) { return result.path_string == path_string; });
      Expect(found, ("the presence-mask prefilter dropped a real match: query='" + query +
                     "' path='" + path_string + "'")
                        .c_str());
    }

    // And nothing the oracle rejects may appear (the mask must not widen the
    // match set either — the real scan still has to run on everything it passes).
    for (const auto& result : finder.results()) {
      const std::string filename = std::filesystem::path(result.path_string).filename().string();
      Expect(is_subsequence(result.path_string, query) || is_subsequence(filename, query),
             ("the finder returned a non-matching path: query='" + query + "' path='" +
              result.path_string + "'")
                 .c_str());
    }
  }
}


// Every result of a fuzzy query must contain the query's characters in order,
// case-insensitively (that is what "fuzzy" promises), a file whose name equals
// the query must be the first result, and the result list must never hold a
// path the index does not.
void TestFileFinderResultsAreSubsequenceMatchesWithExactNameFirst() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "workspace";
  WriteFile(root / "README.md", "root\n");
  std::uint64_t state = 0x1F2E3D4C5B6A7988ull;
  const auto next = [&state](std::uint64_t bound) {
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    return bound == 0 ? 0 : static_cast<std::size_t>(state % bound);
  };
  static constexpr const char* kSegments[] = {"src", "lib", "app", "Test", "util", "docs"};
  static constexpr const char* kNames[] = {"main", "Main", "index", "reader", "writer",
                                           "readme", "abc", "aBc", "config", "x"};
  static constexpr const char* kExts[] = {".cpp", ".h", ".md", ".txt", ".py"};
  std::vector<std::string> paths;
  IndexUpdateBatch batch;
  batch.is_initial = true;
  for (int i = 0; i < 60; ++i) {
    std::string path;
    const std::size_t depth = next(3);
    for (std::size_t d = 0; d < depth; ++d) {
      path += kSegments[next(std::size(kSegments))];
      path += '/';
    }
    path += kNames[next(std::size(kNames))];
    path += kExts[next(std::size(kExts))];
    if (std::find(paths.begin(), paths.end(), path) != paths.end()) {
      continue;
    }
    paths.push_back(path);
    batch.changes.push_back(MakeCreateChange(path));
  }
  FileIndex index;
  Expect(index.SetRoot(root, FileIndex::RootPopulationMode::Deferred), "index root");
  Expect(index.ApplyBatch(batch), "initial batch");
  FileFinder finder;
  finder.SetIndex(&index);

  const auto lower = [](std::string text) {
    for (char& c : text) {
      if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
    }
    return text;
  };
  const auto is_subsequence = [&](const std::string& query, const std::string& path) {
    const std::string q = lower(query);
    const std::string p = lower(path);
    std::size_t at = 0;
    for (const char c : q) {
      at = p.find(c, at);
      if (at == std::string::npos) return false;
      ++at;
    }
    return true;
  };

  static constexpr const char* kQueries[] = {"main", "MAIN", "rd", "abc", "aBc", "src/main",
                                             "x", "cfg", "readme.md", "zzz", "i", "u/r"};
  for (const char* query : kQueries) {
    finder.SetQuery(query);
    const auto results = finder.results();
    for (const auto& result : results) {
      Expect(std::find(paths.begin(), paths.end(), result.path_string) != paths.end(),
             (std::string("[") + query + "] result " + result.path_string + " is an indexed path")
                 .c_str());
      Expect(is_subsequence(query, result.path_string),
             (std::string("[") + query + "] result " + result.path_string +
              " contains the query as a subsequence")
                 .c_str());
    }
    std::size_t expected = 0;
    for (const auto& path : paths) {
      expected += is_subsequence(query, path) ? 1 : 0;
    }
    Expect((expected == 0) == results.empty(),
           (std::string("[") + query + "] finds something iff a path matches").c_str());
  }
  // A file whose whole name is the query ranks first.
  if (std::find(paths.begin(), paths.end(), "abc.md") == paths.end()) {
    IndexUpdateBatch more;
    more.changes.push_back(MakeCreateChange("abc.md"));
    Expect(index.ApplyBatch(more), "add the exact-name file");
    finder.Refresh();
    paths.push_back("abc.md");
  }
  finder.SetQuery("abc.md");
  std::string top;
  for (std::size_t i = 0; i < std::min<std::size_t>(4, finder.results().size()); ++i) {
    top += finder.results()[i].path_string + "(" + std::to_string(finder.results()[i].score) + ") ";
  }
  Expect(!finder.results().empty() && finder.results().front().path_string == "abc.md",
         ("an exact file-name match ranks first; top=" + top).c_str());
}

void RegisterFileFinderTests(std::vector<TestCase>& tests) {
  AddTest(tests, "FileFinder/ResultsAreSubsequenceMatchesWithExactNameFirst",
          TestFileFinderResultsAreSubsequenceMatchesWithExactNameFirst);
  AddTest(tests, "FileFinder/RanksContiguousMatchesFirst",
          TestFileFinderRanksContiguousMatchesFirst);
  AddTest(tests, "FileFinder/PrefersShorterShallowerPaths",
          TestFileFinderPrefersShorterShallowerPaths);
  AddTest(tests, "FileFinder/RanksCamelCaseInitials", TestFileFinderRanksCamelCaseInitials);
  AddTest(tests, "FileFinder/PrefersFilenameMatchesOverPathMatches",
          TestFileFinderPrefersFilenameMatchesOverPathMatches);
  AddTest(tests, "FileFinder/PrefersWordStartsAndExactNames",
          TestFileFinderPrefersWordStartsAndExactNames);
  AddTest(tests, "FileFinder/SplitsQueryAtTheLastSeparator",
          TestFileFinderSplitsQueryAtTheLastSeparator);
  AddTest(tests, "FileFinder/SeparatorQueryKeepsFilenameOrdering",
          TestFileFinderSeparatorQueryKeepsFilenameOrdering);
  AddTest(tests, "FileFinder/MatchesAcrossPathSeparators",
          TestFileFinderMatchesAcrossPathSeparators);
  AddTest(tests, "FileFinder/PresenceMaskNeverRejectsAMatch",
          TestFileFinderPresenceMaskNeverRejectsAMatch);
  AddTest(tests, "FileFinder/ReportsTruncatedIndex", TestFileFinderReportsTruncatedIndex);
  AddTest(tests, "FileFinder/CapsRecentsOnEmptyQuery", TestFileFinderCapsRecentsOnEmptyQuery);
  AddTest(tests, "FileFinder/CapsBroadResultCount", TestFileFinderCapsBroadResultCount);
  AddTest(tests, "FileFinder/NarrowsToEntryBeyondDisplayCap",
          TestFileFinderNarrowsToEntryBeyondDisplayCap);
  AddTest(tests, "FileFinder/IncrementalTypingMatchesFreshQuery",
          TestFileFinderIncrementalTypingMatchesFreshQuery);
  AddTest(tests, "FileFinder/PrependsRecentsWhenQueryEmpty",
          TestFileFinderPrependsRecentsWhenQueryEmpty);
  AddTest(tests, "FileFinder/EmitsRecentsNewestFirstRegardlessOfIndexOrder",
          TestFileFinderEmitsRecentsNewestFirstRegardlessOfIndexOrder);
  AddTest(tests, "FileFinder/FoldedFilenameMatchesSeparateFold",
          TestFileFinderFoldedFilenameMatchesSeparateFold);
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
