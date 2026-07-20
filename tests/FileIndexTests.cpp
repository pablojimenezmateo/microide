#include "TestSupport.h"

#include "project/FileIndex.h"

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

  const auto snapshot = index.SnapshotWithVersion();
  Expect(snapshot.version == set_root_version + 1,
         "applying a changed initial batch should increment the file index version once");
  Expect(snapshot.files.size() == 3,
         "initial batch should keep unique non-.git files");
  Expect(snapshot.files[0].relative_path == std::filesystem::path(".hidden.cpp") &&
             snapshot.files[1].relative_path == std::filesystem::path("src/a.cpp") &&
             snapshot.files[2].relative_path == std::filesystem::path("src/b.cpp"),
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

void RegisterFileIndexTests(std::vector<TestCase>& tests) {
  AddTest(tests, "FileIndex/ReplaceScannedFilesCarriesTruncation",
          TestFileIndexReplaceScannedFilesCarriesTruncation);
  AddTest(tests, "FileIndex/MovePreservesFollowSymlinksFlag",
          TestFileIndexMovePreservesFollowSymlinksFlag);
  AddTest(tests, "FileIndex/RecursiveDeleteRemovesSubtree",
          TestFileIndexRecursiveDeleteRemovesSubtree);
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
