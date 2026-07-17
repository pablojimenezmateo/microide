#include "TestSupport.h"

#include "project/DirectoryTree.h"

#include <algorithm>
#include <filesystem>
#include <optional>
#include <string>
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

// The user's requirement: VCS metadata and build-output dirs are treated exactly
// like gitignored entries — shown GRAYED (ignored=true), not hidden — even when no
// .gitignore exists. A user exclude glob grays a custom dir the same way.
void TestDirectoryTreeGraysBuildAndVcsDirsNotHidden() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "svn_project";
  // No .gitignore (SVN-style checkout).
  WriteFile(root / "src" / "main.cpp", "int main() {}\n");
  WriteFile(root / ".svn" / "wc.db", "svn\n");
  WriteFile(root / "builds" / "artifact.o", "bin\n");
  WriteFile(root / "vendored" / "lib.c", "// v\n");

  DirectoryTree tree;
  tree.SetExcludeGlobs({"vendored/"});
  Expect(tree.SetRoot(root), "directory tree should open fixture root");

  const auto* svn = FindEntry(tree, root / ".svn");
  Expect(svn != nullptr, ".svn should remain visible in the tree model (not hidden)");
  Expect(svn != nullptr && svn->ignored, ".svn should be grayed (tagged ignored)");

  const auto* builds = FindEntry(tree, root / "builds");
  Expect(builds != nullptr, "builds/ should remain visible (not hidden)");
  Expect(builds != nullptr && builds->ignored, "builds/ should be grayed (tagged ignored)");

  const auto* vendored = FindEntry(tree, root / "vendored");
  Expect(vendored != nullptr, "a user-excluded dir should remain visible (not hidden)");
  Expect(vendored != nullptr && vendored->ignored,
         "a user-excluded dir should be grayed (tagged ignored)");

  const auto* src = FindEntry(tree, root / "src");
  Expect(src != nullptr && !src->ignored, "ordinary source dirs stay non-ignored");
}

void TestDirectoryTreeSelectPathExpandsAncestors() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path nested = root / "src" / "main.cpp";
  WriteFile(nested, "int main() {}\n");

  DirectoryTree tree;
  Expect(tree.SetRoot(root), "directory tree should open nested-path fixture root");
  Expect(tree.SelectPath(nested), "directory tree should select the nested file path");
  Expect(FindEntry(tree, nested) != nullptr,
         "selecting a nested file path should expand ancestors and materialize the file row");
}

void TestDirectoryTreeStopsExpandingSymlinkCycle() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  WriteFile(root / "a" / "marker.txt", "marker\n");

  // `loop` is a directory symlink back to the project root, so loop/loop/loop/…
  // all resolve to the same real directory. The tree may follow it once, but
  // expanding `loop/loop` must not re-materialize the root subtree underneath
  // it — otherwise expansion through a cycle grows without bound.
  std::error_code link_error;
  std::filesystem::create_directory_symlink(root, root / "loop", link_error);
  Expect(!link_error, "fixture should create a root-referential directory symlink");

  DirectoryTree tree;
  Expect(tree.SetRoot(root), "directory tree should open the symlink-cycle fixture root");

  Expect(tree.SelectPathIfVisible(root / "loop"), "the symlink directory should be visible");
  tree.ExpandSelection();
  Expect(FindEntry(tree, root / "loop" / "a") != nullptr,
         "following the symlink once should materialize its real children");

  Expect(tree.SelectPathIfVisible(root / "loop" / "loop"),
         "the nested symlink entry should appear after the first expansion");
  tree.ExpandSelection();
  Expect(FindEntry(tree, root / "loop" / "loop" / "a") == nullptr,
         "expanding a symlink that loops back to an ancestor must not re-enter the cycle");
}

}  // namespace

// Expanded/collapsed key sets accumulate forever otherwise; a deleted directory's
// key must be pruned on the fs-resync Refresh so the sets stay bounded and a
// deleted-then-recreated dir renders collapsed (like VSCode) instead of expanded.
// Restored expansion/collapse keys are validated for root containment before storing, so
// a tampered or cross-root session file cannot seed absolute or `..`-escaping keys that
// PruneDeletedDirectoryKeys would stat or that would be re-serialized as outside-root
// relatives (TD-2026-07-17A-089).
void TestDirectoryTreeRestoreRejectsOutsideRootExpansionKeys() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  WriteFile(root / "alpha" / "child.txt", "a\n");
  WriteFile(root / "beta" / "child.txt", "b\n");

  DirectoryTree tree;
  Expect(tree.SetRoot(root), "directory tree should open fixture root");

  // One contained key plus hostile ones: an absolute path, a `..` escape, and a deep
  // traversal that normalizes to a sibling of the root.
  tree.RestoreExpansionState({"alpha", "/etc", "../../outside", "beta/../../escape"},
                             {"../sibling-collapsed", "/var"});

  const auto has = [](const std::vector<std::string>& keys, const std::string& key) {
    return std::find(keys.begin(), keys.end(), key) != keys.end();
  };
  const std::vector<std::string> expanded = tree.ExpandedRelativePaths();
  Expect(has(expanded, "alpha"), "a contained expansion key must be restored");
  for (const std::string& key : expanded) {
    Expect(!key.empty() && key.front() != '/',
           "no restored expansion key may be an absolute path");
    Expect(key.find("..") == std::string::npos,
           "no restored expansion key may contain a `..` escape");
  }
  for (const std::string& key : tree.ManuallyCollapsedRelativePaths()) {
    Expect(!key.empty() && key.front() != '/' && key.find("..") == std::string::npos,
           "no restored collapsed key may escape the root");
  }
}

void TestDirectoryTreePrunesDeletedDirectoryKeysOnRefresh() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  WriteFile(root / "alpha" / "child.txt", "a\n");
  WriteFile(root / "collapsed" / "child.txt", "c\n");
  WriteFile(root / "survivor" / "child.txt", "s\n");

  DirectoryTree tree;
  Expect(tree.SetRoot(root), "directory tree should open fixture root");

  const auto has = [](const std::vector<std::string>& keys, const std::string& key) {
    return std::find(keys.begin(), keys.end(), key) != keys.end();
  };

  Expect(tree.SelectPathIfVisible(root / "alpha"), "alpha should be visible");
  tree.ExpandSelection();
  Expect(tree.SelectPathIfVisible(root / "survivor"), "survivor should be visible");
  tree.ExpandSelection();
  Expect(tree.SelectPathIfVisible(root / "collapsed"), "collapsed should be visible");
  tree.ExpandSelection();
  tree.CollapseSelection();  // Populates the manually-collapsed key set.

  Expect(has(tree.ExpandedRelativePaths(), "alpha"), "alpha should be recorded expanded");
  Expect(has(tree.ExpandedRelativePaths(), "survivor"), "survivor should be recorded expanded");
  Expect(has(tree.ManuallyCollapsedRelativePaths(), "collapsed"),
         "collapsed should be recorded manually-collapsed");

  std::error_code error;
  std::filesystem::remove_all(root / "alpha", error);
  std::filesystem::remove_all(root / "collapsed", error);
  tree.Refresh();

  Expect(!has(tree.ExpandedRelativePaths(), "alpha"),
         "a deleted directory's expanded key must be pruned on refresh");
  Expect(!has(tree.ManuallyCollapsedRelativePaths(), "collapsed"),
         "a deleted directory's collapsed key must be pruned on refresh");
  Expect(FindEntry(tree, root / "alpha") == nullptr,
         "the deleted directory should be gone from the tree");
  Expect(has(tree.ExpandedRelativePaths(), "survivor"),
         "an existing expanded directory must survive refresh (no over-pruning)");
}

void RegisterDirectoryTreeTests(std::vector<TestCase>& tests) {
  AddTest(tests, "DirectoryTree/PrunesDeletedDirectoryKeysOnRefresh",
          TestDirectoryTreePrunesDeletedDirectoryKeysOnRefresh);
  AddTest(tests, "DirectoryTree/RestoreRejectsOutsideRootExpansionKeys",
          TestDirectoryTreeRestoreRejectsOutsideRootExpansionKeys);
  AddTest(tests, "DirectoryTree/TracksIgnoredStatusIndependentlyFromVisibility",
          TestDirectoryTreeTracksIgnoredStatusIndependentlyFromVisibility);
  AddTest(tests, "DirectoryTree/TracksMaterializationIndependentlyFromIgnoredStatus",
          TestDirectoryTreeTracksMaterializationIndependentlyFromIgnoredStatus);
  AddTest(tests, "DirectoryTree/ShowsHiddenIgnoredEntries",
          TestDirectoryTreeShowsHiddenIgnoredEntries);
  AddTest(tests, "DirectoryTree/GraysBuildAndVcsDirsNotHidden",
          TestDirectoryTreeGraysBuildAndVcsDirsNotHidden);
  AddTest(tests, "DirectoryTree/SelectPathExpandsAncestors",
          TestDirectoryTreeSelectPathExpandsAncestors);
  AddTest(tests, "DirectoryTree/StopsExpandingSymlinkCycle",
          TestDirectoryTreeStopsExpandingSymlinkCycle);
}

}  // namespace microide::tests
