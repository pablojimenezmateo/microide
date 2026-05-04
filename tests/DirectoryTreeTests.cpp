#include "TestSupport.h"

#include "project/DirectoryTree.h"

#include <filesystem>
#include <optional>
#include <vector>

namespace microide::tests {
namespace {

using microide::project::DirectoryTree;
using microide::project::TreeEntry;

const TreeEntry* FindEntry(const DirectoryTree& tree, const std::filesystem::path& path) {
  const auto normalized = path.lexically_normal();
  for (const auto& entry : tree.entries()) {
    if (entry.path == normalized) {
      return &entry;
    }
  }
  return nullptr;
}

void TestDirectoryTreeTracksIgnoredStatusIndependentlyFromVisibility() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  WriteFile(root / ".gitignore", "ignored-dir/\nignored-file.txt\n");
  WriteFile(root / "ignored-dir" / "nested.txt", "nested\n");
  WriteFile(root / "ignored-file.txt", "ignored\n");
  WriteFile(root / "tracked.txt", "tracked\n");

  DirectoryTree tree;
  Expect(tree.SetRoot(root), "directory tree should open fixture root");

  const auto* ignored_dir = FindEntry(tree, root / "ignored-dir");
  Expect(ignored_dir != nullptr,
         "ignored directories should remain visible in the tree model");
  Expect(ignored_dir != nullptr && ignored_dir->ignored,
         "ignored directories should be tagged as ignored");
  Expect(ignored_dir != nullptr && !ignored_dir->children_materialized,
         "collapsed directories should report children as unmaterialized");

  const auto* ignored_file = FindEntry(tree, root / "ignored-file.txt");
  Expect(ignored_file != nullptr,
         "ignored files should remain visible in the tree model");
  Expect(ignored_file != nullptr && ignored_file->ignored,
         "ignored files should be tagged as ignored");

  const auto* tracked_file = FindEntry(tree, root / "tracked.txt");
  Expect(tracked_file != nullptr, "tracked files should remain visible");
  Expect(tracked_file != nullptr && !tracked_file->ignored,
         "tracked files should not be tagged as ignored");
}

void TestDirectoryTreeTracksMaterializationIndependentlyFromIgnoredStatus() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  WriteFile(root / ".gitignore", "ignored-dir/\n");
  WriteFile(root / "ignored-dir" / "child-a.txt", "a\n");
  WriteFile(root / "ignored-dir" / "child-b.txt", "b\n");

  DirectoryTree tree;
  Expect(tree.SetRoot(root), "directory tree should open fixture root");

  const auto* ignored_dir_before = FindEntry(tree, root / "ignored-dir");
  Expect(ignored_dir_before != nullptr,
         "ignored directory should be visible before expansion");
  Expect(ignored_dir_before != nullptr && ignored_dir_before->ignored,
         "ignored directory should keep ignored tag before expansion");
  Expect(ignored_dir_before != nullptr && !ignored_dir_before->children_materialized,
         "ignored directory should start with unmaterialized children");

  const bool selected = tree.SelectPathIfVisible(root / "ignored-dir");
  Expect(selected, "ignored directory should be selectable");
  tree.ExpandSelection();

  const auto* ignored_dir_after = FindEntry(tree, root / "ignored-dir");
  Expect(ignored_dir_after != nullptr,
         "ignored directory should stay visible after expansion");
  Expect(ignored_dir_after != nullptr && ignored_dir_after->ignored,
         "ignored directory ignored tag should be stable across expansion");
  Expect(ignored_dir_after != nullptr && ignored_dir_after->children_materialized,
         "expanding a directory should mark its children as materialized");

  Expect(FindEntry(tree, root / "ignored-dir" / "child-a.txt") != nullptr,
         "expanding should materialize immediate children");
  Expect(FindEntry(tree, root / "ignored-dir" / "child-b.txt") != nullptr,
         "expanding should materialize all immediate children");
}

void TestDirectoryTreeShowsHiddenIgnoredEntries() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  WriteFile(root / ".gitignore", ".env.local\n.cache/\n");
  WriteFile(root / ".env.local", "TOKEN=abc\n");
  WriteFile(root / ".cache" / "stamp.txt", "ok\n");

  DirectoryTree tree;
  Expect(tree.SetRoot(root), "directory tree should open fixture root");

  const auto* hidden_file = FindEntry(tree, root / ".env.local");
  Expect(hidden_file != nullptr,
         "hidden ignored files should remain visible in the tree model");
  Expect(hidden_file != nullptr && hidden_file->ignored,
         "hidden ignored files should be tagged as ignored");

  const auto* hidden_dir = FindEntry(tree, root / ".cache");
  Expect(hidden_dir != nullptr,
         "hidden ignored directories should remain visible in the tree model");
  Expect(hidden_dir != nullptr && hidden_dir->ignored,
         "hidden ignored directories should be tagged as ignored");
}

}  // namespace

void RegisterDirectoryTreeTests(std::vector<TestCase>& tests) {
  AddTest(tests, "DirectoryTree/TracksIgnoredStatusIndependentlyFromVisibility",
          TestDirectoryTreeTracksIgnoredStatusIndependentlyFromVisibility);
  AddTest(tests, "DirectoryTree/TracksMaterializationIndependentlyFromIgnoredStatus",
          TestDirectoryTreeTracksMaterializationIndependentlyFromIgnoredStatus);
  AddTest(tests, "DirectoryTree/ShowsHiddenIgnoredEntries",
          TestDirectoryTreeShowsHiddenIgnoredEntries);
}

}  // namespace microide::tests
