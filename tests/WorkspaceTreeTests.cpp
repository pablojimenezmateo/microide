#include "TestSupport.h"

#include "server/WorkspaceTree.h"

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

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
  WorkspaceTree tree(root);
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
  AddTest(tests, "WorkspaceTree/Symlinks", TestWorkspaceTreeSymlinks);
}

}  // namespace microide::tests
