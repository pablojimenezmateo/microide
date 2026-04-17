#include "TestSupport.h"

#include "platform/FileWatcher.h"
#include "platform/Filesystem.h"

#include <chrono>
#include <filesystem>

namespace microide::tests {
namespace {

using microide::platform::DirectoryEntry;
using microide::platform::FileTreeWatcher;
using microide::platform::ListDirectory;
using microide::platform::PathType;

void TestFilesystemListDirectorySortsAndClassifiesEntries() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "workspace";
  WriteFile(root / "zeta.txt", "zeta\n");
  WriteFile(root / "alpha" / "nested.txt", "nested\n");
  WriteFile(root / "beta.lua", "return {}\n");

  const std::vector<DirectoryEntry> entries = ListDirectory(root);
  Expect(entries.size() == 3, "filesystem directory listing should include direct child entries");
  Expect(entries[0].path.filename() == "alpha" && entries[0].type == PathType::Directory,
         "filesystem directory listing should classify directories");
  Expect(entries[1].path.filename() == "beta.lua" && entries[1].type == PathType::RegularFile,
         "filesystem directory listing should sort files lexically");
  Expect(entries[2].path.filename() == "zeta.txt" && entries[2].type == PathType::RegularFile,
         "filesystem directory listing should keep later entries in lexical order");
}

void TestFileWatcherDetectsNestedCreatesUpdatesAndDeletes() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "plugins";
  std::filesystem::create_directories(root);

  FileTreeWatcher watcher(std::chrono::milliseconds::zero());
  watcher.SetRoots({root});
  Expect(!watcher.Poll(), "fresh file watcher roots should not report changes immediately");

  WriteFile(root / "reloadable" / "init.lua", "return 1\n");
  Expect(watcher.Poll(), "file watcher should detect nested file creation");
  Expect(!watcher.Poll(), "file watcher should clear the change flag after polling");

  WriteFile(root / "reloadable" / "init.lua", "return 1, 2, 3\n");
  Expect(watcher.Poll(), "file watcher should detect nested file updates");

  std::filesystem::remove(root / "reloadable" / "init.lua");
  Expect(watcher.Poll(), "file watcher should detect nested file deletions");
}

void TestFileWatcherDetectsCreationOfMissingRoots() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path missing_root = temp_dir.path() / "config" / "microide" / "plugins";

  FileTreeWatcher watcher(std::chrono::milliseconds::zero());
  watcher.SetRoots({missing_root});
  Expect(!watcher.Poll(),
         "watching a missing root should not report a change before the root exists");

  WriteFile(missing_root / "dynamic" / "init.lua", "return true\n");
  Expect(watcher.Poll(), "file watcher should detect when a previously missing root appears");
}

}  // namespace

void RegisterFilesystemTests(std::vector<TestCase>& tests) {
  AddTest(tests, "Filesystem/ListDirectorySortsAndClassifiesEntries",
          TestFilesystemListDirectorySortsAndClassifiesEntries);
  AddTest(tests, "FileWatcher/DetectsNestedCreatesUpdatesAndDeletes",
          TestFileWatcherDetectsNestedCreatesUpdatesAndDeletes);
  AddTest(tests, "FileWatcher/DetectsCreationOfMissingRoots",
          TestFileWatcherDetectsCreationOfMissingRoots);
}

}  // namespace microide::tests
