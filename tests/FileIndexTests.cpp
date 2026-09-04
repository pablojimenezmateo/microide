#include "TestSupport.h"

#include "project/FileIndex.h"
#include "util/DurableFile.h"
#include "util/PathMatch.h"

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

namespace microide::tests {
namespace {

using microide::platform::IndexUpdateBatch;
using microide::project::FileIndex;
using microide::project::ProjectFileScanMode;

IndexUpdateBatch::Change MakeCreateChange(std::filesystem::path relative_path,
                                          std::uintmax_t size = 0) {
  IndexUpdateBatch::Change change;
  change.kind = IndexUpdateBatch::Kind::CreatedOrModified;
  change.entry.relative_path = std::move(relative_path);
  change.entry.size = size;
  return change;
}

IndexUpdateBatch::Change MakeDeleteChange(std::filesystem::path relative_path) {
  IndexUpdateBatch::Change change;
  change.kind = IndexUpdateBatch::Kind::Deleted;
  change.entry.relative_path = std::move(relative_path);
  return change;
}

void TestFileIndexInitialBatchPopulatesSortedUniqueFilesAndVersion() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "workspace";
  WriteFile(root / "README.md", "root\n");

  FileIndex index;
  Expect(index.SetRoot(root, FileIndex::RootPopulationMode::Deferred),
         "file index root should initialize for deferred population");
  const std::uint64_t set_root_version = index.version();

  IndexUpdateBatch batch;
  batch.is_initial = true;
  batch.changes.push_back(MakeCreateChange("src/b.cpp", 20));
  batch.changes.push_back(MakeCreateChange(".hidden.cpp", 5));
  batch.changes.push_back(MakeCreateChange("src/a.cpp", 10));
  batch.changes.push_back(MakeCreateChange("src/a.cpp", 11));
  batch.changes.push_back(MakeCreateChange(".git/index", 100));
  Expect(index.ApplyBatch(batch), "initial batch should populate the file index");

  const auto files = index.Snapshot();
  Expect(index.version() == set_root_version + 1,
         "applying a changed initial batch should increment the file index version once");
  Expect(files.size() == 3, "initial batch should keep unique non-.git files");
  Expect(files[0].relative_path == std::filesystem::path(".hidden.cpp") &&
             files[1].relative_path == std::filesystem::path("src/a.cpp") &&
             files[2].relative_path == std::filesystem::path("src/b.cpp"),
         "initial batch should normalize and sort stored relative paths");

  const auto with_hidden = index.SnapshotPaths(ProjectFileScanMode::IncludeHidden);
  const auto without_hidden = index.SnapshotPaths(ProjectFileScanMode::ExcludeHidden);
  Expect(with_hidden.size() == 3,
         "include-hidden snapshot should keep hidden paths from the index");
  Expect(without_hidden.size() == 2 &&
             without_hidden[0] == std::filesystem::path("src/a.cpp") &&
             without_hidden[1] == std::filesystem::path("src/b.cpp"),
         "exclude-hidden snapshot should filter hidden entries from the same index content");
}

void TestFileIndexIncrementalBatchVersionChangesOnlyOnRealMutations() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "workspace";
  WriteFile(root / "README.md", "root\n");

  FileIndex index;
  Expect(index.SetRoot(root, FileIndex::RootPopulationMode::Deferred),
         "incremental fixture should initialize deferred file index root");

  IndexUpdateBatch initial;
  initial.is_initial = true;
  initial.changes.push_back(MakeCreateChange("tracked.txt", 5));
  Expect(index.ApplyBatch(initial), "incremental fixture should accept initial batch");
  const std::uint64_t initial_version = index.version();

  IndexUpdateBatch noop_update;
  noop_update.is_initial = false;
  noop_update.changes.push_back(MakeCreateChange("tracked.txt", 5));
  Expect(!index.ApplyBatch(noop_update),
         "incremental no-op update should not report index changes");
  Expect(index.version() == initial_version,
         "incremental no-op update should not change index version");

  IndexUpdateBatch changed_update;
  changed_update.is_initial = false;
  changed_update.changes.push_back(MakeCreateChange("tracked.txt", 9));
  Expect(index.ApplyBatch(changed_update),
         "incremental metadata update should report index changes");
  const std::uint64_t after_metadata_version = index.version();
  Expect(after_metadata_version == initial_version + 1,
         "metadata update should increment index version");

  IndexUpdateBatch add_file;
  add_file.is_initial = false;
  add_file.changes.push_back(MakeCreateChange("new.txt", 1));
  Expect(index.ApplyBatch(add_file),
         "incremental create should report index changes");
  const std::uint64_t after_add_version = index.version();
  Expect(after_add_version == after_metadata_version + 1,
         "incremental create should increment index version");

  IndexUpdateBatch remove_file;
  remove_file.is_initial = false;
  remove_file.changes.push_back(MakeDeleteChange("tracked.txt"));
  Expect(index.ApplyBatch(remove_file),
         "incremental delete should report index changes");
  const std::uint64_t after_delete_version = index.version();
  Expect(after_delete_version == after_add_version + 1,
         "incremental delete should increment index version");

  IndexUpdateBatch remove_missing_file;
  remove_missing_file.is_initial = false;
  remove_missing_file.changes.push_back(MakeDeleteChange("missing.txt"));
  Expect(!index.ApplyBatch(remove_missing_file),
         "deleting a missing path should not report index changes");
  Expect(index.version() == after_delete_version,
         "deleting a missing path should not increment index version");
}

void TestFileIndexExcludesGitMetadataFromInitialAndIncrementalBatches() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "workspace";
  WriteFile(root / "README.md", "root\n");

  FileIndex index;
  Expect(index.SetRoot(root, FileIndex::RootPopulationMode::Deferred),
         "git metadata fixture should initialize deferred file index root");

  IndexUpdateBatch initial;
  initial.is_initial = true;
  initial.changes.push_back(MakeCreateChange(".git/index", 100));
  initial.changes.push_back(MakeCreateChange("src/main.cpp", 7));
  Expect(index.ApplyBatch(initial),
         "initial batch with tracked files should apply");
  auto snapshot = index.Snapshot();
  Expect(snapshot.size() == 1 &&
             snapshot.front().relative_path == std::filesystem::path("src/main.cpp"),
         "initial batch should skip .git metadata paths");

  const std::uint64_t before_git_update = index.version();
  IndexUpdateBatch incremental_git;
  incremental_git.is_initial = false;
  incremental_git.changes.push_back(MakeCreateChange(".git/HEAD", 200));
  Expect(!index.ApplyBatch(incremental_git),
         "incremental .git metadata update should not affect file index");
  Expect(index.version() == before_git_update,
         "incremental .git metadata update should not change file index version");
}

// Regression: on a case-insensitive host (Windows / default macOS) `.GIT` names
// the same metadata directory as `.git`, so it must also be excluded — otherwise
// repository internals leak into the file finder / search index. On a
// case-sensitive host `.GIT` is a real, different directory and is NOT excluded.
void TestFileIndexExcludesGitMetadataCaseInsensitiveOnFoldingHosts() {
  const auto index_with_git_variant = [](const char* variant_path) {
    TemporaryDirectory temp_dir;
    const std::filesystem::path root = temp_dir.path() / "workspace";
    WriteFile(root / "README.md", "root\n");
    FileIndex index;
    index.SetRoot(root, FileIndex::RootPopulationMode::Deferred);
    IndexUpdateBatch batch;
    batch.is_initial = true;
    batch.changes.push_back(MakeCreateChange(variant_path, 100));
    batch.changes.push_back(MakeCreateChange("src/main.cpp", 7));
    index.ApplyBatch(batch);
    return index.Snapshot();
  };

  {
    ScopedHostPlatformOverride windows(platform::HostPlatform::Windows);
    const auto snapshot = index_with_git_variant(".GIT/config");
    Expect(snapshot.size() == 1 &&
               snapshot.front().relative_path == std::filesystem::path("src/main.cpp"),
           "a case-insensitive host must exclude .GIT metadata like .git");
  }
  {
    ScopedHostPlatformOverride linux(platform::HostPlatform::Linux);
    const auto snapshot = index_with_git_variant(".GIT/config");
    // On Linux `.GIT` is a genuine directory; it is indexed, not excluded.
    Expect(snapshot.size() == 2, "a case-sensitive host treats .GIT as a real directory");
  }
}

void TestFileIndexInitialBatchAbortsWhenCancelled() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "workspace";
  WriteFile(root / "README.md", "root\n");

  FileIndex index;
  Expect(index.SetRoot(root, FileIndex::RootPopulationMode::Deferred),
         "cancel fixture should initialize deferred file index root");
  const std::uint64_t set_root_version = index.version();

  // A batch large enough to cross the cancel-check stride so the abort path runs.
  IndexUpdateBatch batch;
  batch.is_initial = true;
  for (int i = 0; i < 10000; ++i) {
    batch.changes.push_back(MakeCreateChange("src/file_" + std::to_string(i) + ".cpp", 1));
  }

  Expect(!index.ApplyBatch(batch, []() { return true; }),
         "a cancelled initial bulk load should report no work applied");
  Expect(index.version() == set_root_version,
         "a cancelled initial bulk load must leave the index version unchanged");
  Expect(index.Snapshot().empty(),
         "a cancelled initial bulk load must leave the index contents unchanged");

  // The same batch applies normally when the predicate never cancels.
  Expect(index.ApplyBatch(batch, []() { return false; }),
         "a non-cancelled initial bulk load should apply");
  Expect(index.Snapshot().size() == 10000,
         "a completed initial bulk load should populate every file");
}

// A recursive Deleted change (a directory removed or moved out) must drop the
// directory and every file beneath it — not just an exact path match — otherwise
// the removed subtree's files linger in the index as ghosts.
void TestFileIndexRecursiveDeleteRemovesSubtree() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "workspace";
  WriteFile(root / "README.md", "root\n");

  FileIndex index;
  Expect(index.SetRoot(root, FileIndex::RootPopulationMode::Deferred), "index root initializes");

  IndexUpdateBatch initial;
  initial.is_initial = true;
  initial.changes.push_back(MakeCreateChange("keep.txt"));
  initial.changes.push_back(MakeCreateChange("sub/a.cpp"));
  initial.changes.push_back(MakeCreateChange("sub/nested/b.cpp"));
  initial.changes.push_back(MakeCreateChange("subtly/kept.cpp"));  // prefix-similar, must survive
  Expect(index.ApplyBatch(initial), "initial batch populates");
  Expect(index.Snapshot().size() == 4, "four files indexed");

  IndexUpdateBatch remove;
  IndexUpdateBatch::Change dir_delete = MakeDeleteChange("sub");
  dir_delete.recursive = true;
  remove.changes.push_back(dir_delete);
  Expect(index.ApplyBatch(remove), "recursive directory delete should mutate the index");

  const auto files = index.Snapshot();
  Expect(files.size() == 2, "the whole 'sub' subtree should be removed");
  bool has_keep = false;
  bool has_subtly = false;
  bool has_sub_file = false;
  for (const auto& file : files) {
    if (file.relative_path == std::filesystem::path("keep.txt")) has_keep = true;
    if (file.relative_path == std::filesystem::path("subtly/kept.cpp")) has_subtly = true;
    const auto rel = file.relative_path.lexically_relative("sub");
    if (!rel.empty() && *rel.begin() != "..") has_sub_file = true;
  }
  Expect(has_keep, "unrelated files survive a recursive delete");
  Expect(has_subtly,
         "a prefix-similar sibling directory ('subtly') must NOT be removed by deleting 'sub'");
  Expect(!has_sub_file, "no file under the deleted directory should remain");
}

// The range-bounded recursive delete (two binary searches over the sorted index)
// has to agree with the containment predicate it replaced on EVERY input, and the
// interesting inputs are the byte neighbours of the separator: '.' (0x2E) and '-'
// (0x2D) sort before '/' (0x2F), digits and letters sort after, so a name like
// "sub.txt" lands between "sub" and "sub/a.cpp" in the index while "sub0" lands
// past the whole run. A scan could not get those wrong; a bounded erase can.
//
// Stated as a differential test against the predicate itself rather than a list
// of hand-computed survivors: the oracle is the definition of "at or below", so
// this stays honest if the ordering or the separator handling ever moves.
void TestFileIndexRecursiveDeleteMatchesContainmentOracle() {
  const std::vector<std::filesystem::path> corpus = {
      "keep.txt",      "sub",           "sub-dash.txt",  "sub.txt",
      "sub/a.cpp",     "sub/nested/b.cpp",               "sub/z",
      "sub0.txt",      "subtly/kept.cpp",                "subZ/x.cpp",
      "zz/sub/deep.c",
  };

  for (const std::filesystem::path& target : {std::filesystem::path("sub"),
                                              std::filesystem::path("subtly"),
                                              std::filesystem::path("zz/sub"),
                                              std::filesystem::path("missing")}) {
    TemporaryDirectory temp_dir;
    const std::filesystem::path root = temp_dir.path() / "workspace";
    WriteFile(root / "README.md", "root\n");

    FileIndex index;
    Expect(index.SetRoot(root, FileIndex::RootPopulationMode::Deferred),
           "oracle fixture root initializes");
    IndexUpdateBatch initial;
    initial.is_initial = true;
    for (const std::filesystem::path& relative_path : corpus) {
      initial.changes.push_back(MakeCreateChange(relative_path));
    }
    Expect(index.ApplyBatch(initial), "oracle fixture populates");

    std::vector<std::filesystem::path> expected;
    for (const std::filesystem::path& relative_path : corpus) {
      if (!microide::util::NormalizedPathEqualsOrWithin(relative_path, target)) {
        expected.push_back(relative_path);
      }
    }
    std::sort(expected.begin(), expected.end());

    IndexUpdateBatch remove;
    IndexUpdateBatch::Change dir_delete = MakeDeleteChange(target);
    dir_delete.recursive = true;
    remove.changes.push_back(dir_delete);
    index.ApplyBatch(remove);

    std::vector<std::filesystem::path> actual;
    for (const auto& file : index.Snapshot()) {
      actual.push_back(file.relative_path);
    }
    std::sort(actual.begin(), actual.end());
    Expect(actual == expected,
           "removing '" + target.generic_string() +
               "' must drop exactly what the containment predicate says (kept " +
               std::to_string(actual.size()) + ", expected " + std::to_string(expected.size()) +
               ")");
  }
}

// A run of recursive deletions in one batch is applied as a single compaction
// pass over the index, so the union of overlapping and NESTED ranges has to come
// out identical to applying them one at a time. Nested is the interesting case:
// deleting "sub" and "sub/nested" together produces two ranges where one encloses
// the other, and a sweep that does not notice would double-count the overlap and
// drop surviving entries with it.
void TestFileIndexRecursiveDeleteRunMatchesSequentialApplication() {
  const std::vector<std::filesystem::path> corpus = {
      "keep.txt",       "a/1.cpp",        "a/deep/2.cpp",   "a/deep/deeper/3.cpp",
      // Sorts AFTER the whole "a/deep" run but still inside "a", so deleting both
      // gives a range STRICTLY enclosed by another -- the one shape a left-to-right
      // sweep can get wrong, by rewinding its read cursor into ground it already
      // erased and resurrecting the entries in between.
      "a/zzz.cpp",
      "ab/4.cpp",       "b/5.cpp",        "b/6.cpp",        "c/7.cpp",
      "cc/8.cpp",       "z/9.cpp",
  };
  const std::vector<std::filesystem::path> targets = {"a", "a/deep", "b", "a", "c"};

  const auto populate = [&corpus](FileIndex& index) {
    IndexUpdateBatch initial;
    initial.is_initial = true;
    for (const std::filesystem::path& relative_path : corpus) {
      initial.changes.push_back(MakeCreateChange(relative_path));
    }
    Expect(index.ApplyBatch(initial), "run fixture populates");
  };
  const auto sorted_paths = [](const FileIndex& index) {
    std::vector<std::filesystem::path> paths;
    for (const auto& file : index.Snapshot()) {
      paths.push_back(file.relative_path);
    }
    std::sort(paths.begin(), paths.end());
    return paths;
  };

  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "workspace";
  WriteFile(root / "README.md", "root\n");

  // One batch carrying the whole run: the coalesced path.
  FileIndex coalesced;
  Expect(coalesced.SetRoot(root, FileIndex::RootPopulationMode::Deferred), "coalesced root");
  populate(coalesced);
  IndexUpdateBatch run;
  for (const std::filesystem::path& target : targets) {
    IndexUpdateBatch::Change change = MakeDeleteChange(target);
    change.recursive = true;
    run.changes.push_back(change);
  }
  Expect(coalesced.ApplyBatch(run), "a run of recursive deletes should mutate the index");

  // The same deletions, one batch each: the sequential reference.
  FileIndex sequential;
  Expect(sequential.SetRoot(root, FileIndex::RootPopulationMode::Deferred), "sequential root");
  populate(sequential);
  for (const std::filesystem::path& target : targets) {
    IndexUpdateBatch single;
    IndexUpdateBatch::Change change = MakeDeleteChange(target);
    change.recursive = true;
    single.changes.push_back(change);
    sequential.ApplyBatch(single);
  }

  Expect(sorted_paths(coalesced) == sorted_paths(sequential),
         "a coalesced run must leave exactly what applying the deletions one at a time does");
  Expect(sorted_paths(coalesced).size() == 4,
         "only keep.txt, ab/4.cpp, cc/8.cpp and z/9.cpp survive the run");
}

// A recursive delete followed by a create UNDER the deleted directory is how a
// directory move-in reaches the index, so the run must not be hoisted past the
// creates that follow it.
void TestFileIndexRecursiveDeleteRunStopsAtTheNextCreate() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "workspace";
  WriteFile(root / "README.md", "root\n");

  FileIndex index;
  Expect(index.SetRoot(root, FileIndex::RootPopulationMode::Deferred), "index root initializes");
  IndexUpdateBatch initial;
  initial.is_initial = true;
  initial.changes.push_back(MakeCreateChange("sub/old.cpp"));
  initial.changes.push_back(MakeCreateChange("other/x.cpp"));
  Expect(index.ApplyBatch(initial), "move-in fixture populates");

  IndexUpdateBatch batch;
  IndexUpdateBatch::Change drop_sub = MakeDeleteChange("sub");
  drop_sub.recursive = true;
  IndexUpdateBatch::Change drop_other = MakeDeleteChange("other");
  drop_other.recursive = true;
  batch.changes.push_back(drop_sub);
  batch.changes.push_back(drop_other);
  batch.changes.push_back(MakeCreateChange("sub/moved-in.cpp"));
  Expect(index.ApplyBatch(batch), "the move-in batch should mutate the index");

  const auto files = index.Snapshot();
  Expect(files.size() == 1, "only the moved-in file survives");
  Expect(files.front().relative_path == std::filesystem::path("sub/moved-in.cpp"),
         "a create after a recursive delete of its parent must survive the run");
}

// "." is the project root spelled as a relative path: the containment predicate
// says every entry is within it, and no byte prefix can express that, so the
// bounded erase has to special-case it. A batch that recursively deletes "."
// empties the index.
void TestFileIndexRecursiveDeleteOfDotClearsTheIndex() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "workspace";
  WriteFile(root / "README.md", "root\n");

  FileIndex index;
  Expect(index.SetRoot(root, FileIndex::RootPopulationMode::Deferred), "index root initializes");
  IndexUpdateBatch initial;
  initial.is_initial = true;
  initial.changes.push_back(MakeCreateChange("keep.txt"));
  initial.changes.push_back(MakeCreateChange("sub/a.cpp"));
  Expect(index.ApplyBatch(initial), "dot-delete fixture populates");

  IndexUpdateBatch remove;
  IndexUpdateBatch::Change dir_delete = MakeDeleteChange(".");
  dir_delete.recursive = true;
  remove.changes.push_back(dir_delete);
  Expect(index.ApplyBatch(remove), "a recursive delete of '.' should mutate the index");
  Expect(index.Snapshot().empty(), "a recursive delete of '.' removes every entry");

  // ...and reports "no change" the second time, so it cannot spin the index
  // version (and every cache keyed on it) on a repeat.
  Expect(!index.ApplyBatch(remove), "a repeated recursive delete of '.' is not a change");
}

}  // namespace

// Regression: the hand-rolled FileIndex move ctor/assignment (required by the
// files_mutex_) dropped the follow_out_of_root_symlinks_ atomic, so a project
// switch (which move-assigns FileIndex) silently reverted a user who enabled
// following out-of-root symlinks back to containment-enforced.
void TestFileIndexMovePreservesFollowSymlinksFlag() {
  FileIndex source;
  source.SetFollowOutOfRootSymlinks(true);
  Expect(source.FollowOutOfRootSymlinks(), "source starts with the flag set");

  FileIndex moved_ctor(std::move(source));
  Expect(moved_ctor.FollowOutOfRootSymlinks(),
         "the move constructor must carry the follow-symlinks flag");

  FileIndex moved_assign;
  moved_assign = std::move(moved_ctor);
  Expect(moved_assign.FollowOutOfRootSymlinks(),
         "the move assignment must carry the follow-symlinks flag");

  // A false flag must also survive (guards against unconditionally forcing true).
  FileIndex off;
  off.SetFollowOutOfRootSymlinks(false);
  FileIndex off_moved(std::move(off));
  Expect(!off_moved.FollowOutOfRootSymlinks(), "a cleared flag must also survive the move");
}

// A forced rescan (ScanFiles -> ReplaceScannedFiles) carries the scan's status
// into truncated()/scan_status(): a budget-truncated status asserts it (with the
// specific cause preserved), and a later complete rescan clears it rather than
// leaving the prior root's "truncated" state asserted over a freshly scanned tree
// (TD-2026-07-17-008/033).
void TestFileIndexReplaceScannedFilesCarriesTruncation() {
  using microide::project::ProjectFile;
  using microide::project::ProjectFileScanStatus;
  FileIndex index;

  std::vector<ProjectFile> files;
  ProjectFile a;
  a.relative_path = "a.txt";
  files.push_back(a);

  index.ReplaceScannedFiles(files, ProjectFileScanStatus{.truncated_by_budget = true});
  Expect(index.truncated(), "an incomplete rescan marks the index truncated");
  Expect(index.scan_status().truncated_by_budget,
         "the specific truncation cause is preserved on the index");

  index.ReplaceScannedFiles(files, ProjectFileScanStatus{});
  Expect(!index.truncated(),
         "a subsequent complete rescan clears the stale truncation flag");
  Expect(!index.scan_status().incomplete(), "a complete rescan clears every cause");
}

// Every document save stages `.＜name＞.microide-staging.<pid>.<seq>` beside its
// target inside the project tree and renames it into place. A watcher batch that
// lands inside that window used to leave the staging path in the index, so it
// showed up in the Ctrl+P file finder and in the project-search candidate set
// until the next full rescan (it also made the replace-all read-count test flaky,
// ~12% of runs). Both scan modes must filter it — "include hidden files" too,
// since the leading dot alone only covers the default mode.
void TestFileIndexHidesAtomicWriteStagingFiles() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "workspace";
  WriteFile(root / "README.md", "root\n");

  FileIndex index;
  Expect(index.SetRoot(root, FileIndex::RootPopulationMode::Deferred),
         "staging-filter fixture should initialize the file index root");

  const std::filesystem::path staging =
      microide::util::UniqueTemporaryPath(root / "main.cpp").filename();
  IndexUpdateBatch batch;
  batch.is_initial = true;
  batch.changes.push_back(MakeCreateChange("main.cpp", 10));
  batch.changes.push_back(MakeCreateChange(staging, 10));
  Expect(index.ApplyBatch(batch), "the batch should populate the index");

  for (const ProjectFileScanMode mode :
       {ProjectFileScanMode::ExcludeHidden, ProjectFileScanMode::IncludeHidden}) {
    const auto paths = index.SnapshotPaths(mode);
    Expect(std::find(paths.begin(), paths.end(), staging) == paths.end(),
           "an in-flight staging temp must never appear in a file index snapshot");
    Expect(std::find(paths.begin(), paths.end(), std::filesystem::path("main.cpp")) != paths.end(),
           "the real file beside the staging temp must still be indexed");
  }
}

void RegisterFileIndexTests(std::vector<TestCase>& tests) {
  AddTest(tests, "FileIndex/HidesAtomicWriteStagingFiles",
          TestFileIndexHidesAtomicWriteStagingFiles);
  AddTest(tests, "FileIndex/ReplaceScannedFilesCarriesTruncation",
          TestFileIndexReplaceScannedFilesCarriesTruncation);
  AddTest(tests, "FileIndex/MovePreservesFollowSymlinksFlag",
          TestFileIndexMovePreservesFollowSymlinksFlag);
  AddTest(tests, "FileIndex/RecursiveDeleteRemovesSubtree",
          TestFileIndexRecursiveDeleteRemovesSubtree);
  AddTest(tests, "FileIndex/RecursiveDeleteMatchesContainmentOracle",
          TestFileIndexRecursiveDeleteMatchesContainmentOracle);
  AddTest(tests, "FileIndex/RecursiveDeleteRunMatchesSequentialApplication",
          TestFileIndexRecursiveDeleteRunMatchesSequentialApplication);
  AddTest(tests, "FileIndex/RecursiveDeleteRunStopsAtTheNextCreate",
          TestFileIndexRecursiveDeleteRunStopsAtTheNextCreate);
  AddTest(tests, "FileIndex/RecursiveDeleteOfDotClearsTheIndex",
          TestFileIndexRecursiveDeleteOfDotClearsTheIndex);
  AddTest(tests, "FileIndex/InitialBatchPopulatesSortedUniqueFilesAndVersion",
          TestFileIndexInitialBatchPopulatesSortedUniqueFilesAndVersion);
  AddTest(tests, "FileIndex/IncrementalBatchVersionChangesOnlyOnRealMutations",
          TestFileIndexIncrementalBatchVersionChangesOnlyOnRealMutations);
  AddTest(tests, "FileIndex/ExcludesGitMetadataFromInitialAndIncrementalBatches",
          TestFileIndexExcludesGitMetadataFromInitialAndIncrementalBatches);
  AddTest(tests, "FileIndex/ExcludesGitMetadataCaseInsensitiveOnFoldingHosts",
          TestFileIndexExcludesGitMetadataCaseInsensitiveOnFoldingHosts);
  AddTest(tests, "FileIndex/InitialBatchAbortsWhenCancelled",
          TestFileIndexInitialBatchAbortsWhenCancelled);
}

}  // namespace microide::tests
