#include "TestSupport.h"

#include "server/WorkspaceTree.h"

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

#if defined(__unix__) || defined(__APPLE__)
#include <unistd.h>
#endif

namespace microide::tests {
namespace {

using project::remote::ManifestEntryKind;
using project::remote::ManifestRow;
using server::WorkspaceTree;

std::vector<std::string> Paths(const WorkspaceTree::Manifest& manifest) {
  std::vector<std::string> paths;
  for (const ManifestRow& row : manifest.rows) {
    paths.push_back(row.path);
  }
  return paths;
}

const ManifestRow* Find(const WorkspaceTree::Manifest& manifest, std::string_view path) {
  for (const ManifestRow& row : manifest.rows) {
    if (row.path == path) {
      return &row;
    }
  }
  return nullptr;
}

// The content set is git's: tracked plus untracked-not-ignored, minus what the
// worktree deleted; hashes are of the bytes on disk.
void TestWorkspaceTreeGitContentSet() {
  TemporaryDirectory temp;
  const std::filesystem::path root = temp.path() / "repo";
  InitializeGitRepo(root);
  WriteFile(root / ".gitignore", "build/\n");
  WriteFile(root / "src/a.cpp", "int a;\n");
  WriteFile(root / "gone.txt", "soon deleted\n");
  CommitAll(root, "init", "workspace tree");
  WriteFile(root / "untracked.txt", "new\n");
  WriteFile(root / "build/out.o", "ignored\n");
  std::filesystem::remove(root / "gone.txt");

  WorkspaceTree tree(root);
  std::string error;
  const auto manifest = tree.BuildManifest(&error);
  Expect(manifest.has_value(), "a repository's manifest builds: " + error);
  Expect(manifest->git, "the content set came from git");
  Expect(Paths(*manifest) == std::vector<std::string>{".gitignore", "src/a.cpp", "untracked.txt"},
         "tracked + untracked-not-ignored, sorted, without the deleted or ignored file");
  const ManifestRow* a = Find(*manifest, "src/a.cpp");
  Expect(a != nullptr && a->hash == util::HashContent("int a;\n") && a->size == 7 &&
             a->kind == ManifestEntryKind::File,
         "a row carries the BLAKE3 of the file's bytes and its size");
}

void TestWorkspaceTreeWalksWithoutGit() {
  TemporaryDirectory temp;
  const std::filesystem::path root = temp.path() / "plain";
  WriteFile(root / "main.c", "x");
  WriteFile(root / "lib/util.c", "y");
  WriteFile(root / "node_modules/dep/index.js", "z");
  WorkspaceTree tree(root);
  std::string error;
  const auto manifest = tree.BuildManifest(&error);
  Expect(manifest.has_value() && !manifest->git, "a plain directory walks: " + error);
  Expect(Paths(*manifest) == std::vector<std::string>{"lib/util.c", "main.c"},
         "the walk uses the scanner's skip rules (node_modules is not content)");
}

void TestWorkspaceTreeHashCache() {
  TemporaryDirectory temp;
  const std::filesystem::path root = temp.path() / "plain";
  for (int i = 0; i < 100; ++i) {
    WriteFile(root / ("f" + std::to_string(i) + ".txt"), std::string(1000 + i, 'a'));
  }
  WorkspaceTree tree(root, WorkspaceTree::Options{.racy_window_ns = 0});
  std::string error;
  const auto first = tree.BuildManifest(&error);
  Expect(first.has_value() && tree.last_hashed_files() == 100, "a cold tree hashes every file");
  const auto second = tree.BuildManifest(&error);
  Expect(second.has_value() && tree.last_hashed_files() == 0, "an unchanged tree hashes nothing");
  Expect(second->rows == first->rows, "and reports the same rows");
  Expect(second->id > first->id, "each manifest has a new id");
  WriteFile(root / "f7.txt", "changed");
  const auto third = tree.BuildManifest(&error);
  Expect(third.has_value() && tree.last_hashed_files() == 1, "one changed file is one hash");
  const ManifestRow* changed = Find(*third, "f7.txt");
  Expect(changed != nullptr && changed->hash == util::HashContent("changed"),
         "the changed file's row has the new hash");
}

// A file written moments ago is racily clean: a same-size rewrite in the same
// timestamp tick would leave (size, mtime, ctime) as they were, so its cached hash
// would be served for different bytes and the change never synced. Within the
// window the cache does not vouch for it.
void TestWorkspaceTreeDoesNotTrustARacyCacheEntry() {
  TemporaryDirectory temp;
  const std::filesystem::path root = temp.path() / "plain";
  WriteFile(root / "hot.txt", "one\n");
  WorkspaceTree tree(root);  // the default window
  std::string error;
  Expect(tree.BuildManifest(&error).has_value() && tree.last_hashed_files() == 1, "hashed once");
  Expect(tree.BuildManifest(&error).has_value() && tree.last_hashed_files() == 1,
         "and again: a just-written file is not served from the cache");
  WriteFile(root / "hot.txt", "two\n");  // same size
  const auto manifest = tree.BuildManifest(&error);
  Expect(manifest.has_value() && manifest->rows[0].hash == util::HashContent("two\n"),
         "a same-size rewrite is seen at once");
}

// A watch batch re-stats only what it names and asks git only about new paths —
// and lands on exactly the manifest a full rebuild would.
void TestWorkspaceTreeIncrementalUpdateMatchesAFullBuild() {
  TemporaryDirectory temp;
  const std::filesystem::path root = temp.path() / "repo";
  InitializeGitRepo(root);
  WriteFile(root / ".gitignore", "*.log\n");
  for (int i = 0; i < 50; ++i) {
    WriteFile(root / "src" / ("f" + std::to_string(i) + ".c"), "int f" + std::to_string(i) + ";\n");
  }
  WriteFile(root / "doomed/a.c", "a");
  WriteFile(root / "doomed/b.c", "b");
  CommitAll(root, "init", "incremental");
  WorkspaceTree tree(root, WorkspaceTree::Options{.racy_window_ns = 0});
  std::string error;
  Expect(tree.BuildManifest(&error).has_value(), "built: " + error);

  WriteFile(root / "src/f3.c", "changed\n");
  WriteFile(root / "src/new.c", "new\n");
  WriteFile(root / "debug.log", "ignored\n");
  std::filesystem::remove(root / "src/f7.c");
  std::filesystem::remove_all(root / "doomed");
  const auto updated = tree.UpdateManifest(
      WorkspaceTree::Changes{.touched = {"src/f3.c", "src/new.c", "debug.log", "src/f7.c", "doomed"},
                             .deleted_directories = {"doomed"}},
      &error);
  Expect(updated.has_value() && !tree.last_update_was_full(), "an incremental update: " + error);
  Expect(tree.last_hashed_files() == 2, "only the changed and the new file are hashed");
  WorkspaceTree fresh(root);
  const auto full = fresh.BuildManifest(&error);
  Expect(full.has_value() && updated->rows == full->rows,
         "the same rows as a full build: the ignored file out, the deleted ones gone");

  WriteFile(root / ".gitignore", "*.log\nsrc/new.c\n");
  const auto after_rules = tree.UpdateManifest(WorkspaceTree::Changes{.touched = {".gitignore"}}, &error);
  Expect(after_rules.has_value() && tree.last_update_was_full(),
         "a .gitignore edit rebuilds: membership rules changed");
  Expect(std::none_of(after_rules->rows.begin(), after_rules->rows.end(),
                      [](const ManifestRow& row) { return row.path == "src/new.c"; }),
         "and a newly ignored untracked file leaves the set");
}

// The hash cache outlives the server process: an on-demand server that idled out
// and was started again hashes nothing a predecessor already hashed.
void TestWorkspaceTreeCachePersists() {
  TemporaryDirectory temp;
  const std::filesystem::path root = temp.path() / "plain";
  for (int i = 0; i < 20; ++i) {
    WriteFile(root / ("f" + std::to_string(i)), std::string(100 + i, 'p'));
  }
  const WorkspaceTree::Options options{.racy_window_ns = 0, .cache_path = temp.path() / "cache" / "c"};
  std::string error;
  {
    WorkspaceTree tree(root, options);
    Expect(tree.BuildManifest(&error).has_value() && tree.last_hashed_files() == 20, "a cold build");
  }
  WorkspaceTree again(root, options);
  Expect(again.BuildManifest(&error).has_value() && again.last_hashed_files() == 0,
         "a new process with the saved cache hashes nothing");
  WorkspaceTree other(temp.path(), options);  // another root, the same file
  Expect(other.BuildManifest(&error).has_value() && other.last_hashed_files() > 0,
         "a cache saved for another root is not used");
}

// A member that cannot be hashed (unreadable, or gone between the stat and the
// read) is left out — and only it: the rows before it keep their paths. (The
// compaction once self-moved them, emptying their paths into a manifest the
// client rejected, which dropped the connection.)
void TestWorkspaceTreeDropsOnlyWhatItCannotHash() {
  TemporaryDirectory temp;
  const std::filesystem::path root = temp.path() / "plain";
  WriteFile(root / "a.txt", "a");
  WriteFile(root / "b.txt", "b");
  WriteFile(root / "c.txt", "c");
  std::filesystem::permissions(root / "c.txt", std::filesystem::perms::none);
  if (::access((root / "c.txt").c_str(), R_OK) == 0) {
    return;  // root reads everything
  }
  WorkspaceTree tree(root);
  std::string error;
  const auto manifest = tree.BuildManifest(&error);
  Expect(manifest.has_value() && Paths(*manifest) == std::vector<std::string>{"a.txt", "b.txt"},
         "the unreadable file is out, the others intact");
  std::filesystem::permissions(root / "c.txt", std::filesystem::perms::owner_all);
}

// A failure to decide the set is an error, never an empty manifest.
void TestWorkspaceTreeFailsLoudly() {
  TemporaryDirectory temp;
  WorkspaceTree missing(temp.path() / "nope");
  std::string error;
  Expect(!missing.BuildManifest(&error).has_value() && !error.empty(),
         "a missing root is an error, not zero files");

  const std::filesystem::path root = temp.path() / "big";
  for (int i = 0; i < 12; ++i) {
    WriteFile(root / ("f" + std::to_string(i)), "x");
  }
  WorkspaceTree capped(root, WorkspaceTree::Options{.max_files = 10});
  error.clear();
  Expect(!capped.BuildManifest(&error).has_value() &&
             error.find("remote.max_manifest_files") != std::string::npos,
         "a set over the limit fails naming the setting, never truncates");

  WorkspaceTree cancelled(root);
  error.clear();
  Expect(!cancelled.BuildManifest(&error, [] { return true; }).has_value() && error == "cancelled",
         "a cancelled build reports cancelled");
}

void TestWorkspaceTreeSymlinks() {
  TemporaryDirectory temp;
  const std::filesystem::path root = temp.path() / "links";
  WriteFile(root / "real.txt", "real");
  WriteFile(temp.path() / "outside.txt", "outside");
  std::filesystem::create_directories(root / "dir");
  std::filesystem::create_symlink("real.txt", root / "inside_link");
  std::filesystem::create_symlink("../../outside.txt", root / "dir/escape_link");
  std::filesystem::create_symlink("/nonexistent/x", root / "dangling");
  WorkspaceTree tree(root);
  std::string error;
  const auto manifest = tree.BuildManifest(&error);
  Expect(manifest.has_value(), "a tree with links builds: " + error);
  const ManifestRow* inside = Find(*manifest, "inside_link");
  Expect(inside != nullptr && inside->kind == ManifestEntryKind::Symlink &&
             inside->link_target == "real.txt",
         "a link inside the root is a link row");
  const ManifestRow* escape = Find(*manifest, "dir/escape_link");
  Expect(escape != nullptr && escape->kind == ManifestEntryKind::File &&
             escape->hash == util::HashContent("outside"),
         "a link out of the root ships the file it names");
  Expect(Find(*manifest, "dangling") == nullptr, "a dangling out-of-root link is not content");
}

}  // namespace

void RegisterWorkspaceTreeTests(std::vector<TestCase>& tests) {
  AddTest(tests, "WorkspaceTree/GitContentSet", TestWorkspaceTreeGitContentSet);
  AddTest(tests, "WorkspaceTree/WalksWithoutGit", TestWorkspaceTreeWalksWithoutGit);
  AddTest(tests, "WorkspaceTree/HashCache", TestWorkspaceTreeHashCache);
  AddTest(tests, "WorkspaceTree/FailsLoudly", TestWorkspaceTreeFailsLoudly);
  AddTest(tests, "WorkspaceTree/DropsOnlyWhatItCannotHash", TestWorkspaceTreeDropsOnlyWhatItCannotHash);
  AddTest(tests, "WorkspaceTree/CachePersists", TestWorkspaceTreeCachePersists);
  AddTest(tests, "WorkspaceTree/IncrementalUpdateMatchesAFullBuild",
          TestWorkspaceTreeIncrementalUpdateMatchesAFullBuild);
  AddTest(tests, "WorkspaceTree/DoesNotTrustARacyCacheEntry",
          TestWorkspaceTreeDoesNotTrustARacyCacheEntry);
  AddTest(tests, "WorkspaceTree/Symlinks", TestWorkspaceTreeSymlinks);
}

}  // namespace microide::tests
