#include "TestSupport.h"

#include "platform/FileWatcher.h"
#include "platform/Filesystem.h"
#include "platform/FsOps.h"
#include "platform/HostIntegration.h"
#include "platform/Trash.h"

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>

namespace microide::tests {
namespace {

using microide::platform::CaptureTreeSnapshot;
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

void TestFileWatcherEntryFilterSkipsIgnoredDirectories() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "workspace";
  std::filesystem::create_directories(root / "node_modules" / "pkg");
  std::filesystem::create_directories(root / "src");
  WriteFile(root / "node_modules" / "pkg" / "index.js", "module.exports = 1;\n");
  WriteFile(root / "src" / "main.ts", "export const main = 1;\n");

  FileTreeWatcher watcher(std::chrono::milliseconds::zero());
  watcher.SetEntryFilter([&root](const std::filesystem::path& path, PathType) {
    if (path == root) {
      return true;
    }

    const std::filesystem::path relative = path.lexically_relative(root);
    if (relative.empty()) {
      return true;
    }

    for (const auto& component : relative) {
      if (component == "node_modules") {
        return false;
      }
    }
    return true;
  });
  watcher.SetRoots({root});
  Expect(!watcher.Poll(), "filtered watcher roots should start clean");

  WriteFile(root / "node_modules" / "pkg" / "index.js", "module.exports = 2;\n");
  Expect(!watcher.Poll(), "entry filters should suppress changes under ignored directories");

  WriteFile(root / "src" / "main.ts", "export const main = 2;\nexport {};\n");
  Expect(watcher.Poll(), "entry filters should still allow visible project changes");
}

void TestCaptureTreeSnapshotRespectsEntryBudget() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "tree";
  for (int i = 0; i < 12; ++i) {
    WriteFile(root / ("file_" + std::to_string(i) + ".txt"), "x\n");
  }

  bool truncated = true;
  const auto full = CaptureTreeSnapshot({root}, {}, 0, &truncated);
  Expect(full.size() == 12, "unbounded snapshot should capture every file");
  Expect(!truncated, "unbounded snapshot should not report truncation");

  truncated = false;
  const auto bounded = CaptureTreeSnapshot({root}, {}, 5, &truncated);
  Expect(truncated, "snapshot should report truncation when the entry budget is exceeded");
  Expect(bounded.size() <= 5, "snapshot should not exceed the entry budget");
}

void TestFileWatcherTreeTooLargeSuppressesPolling() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "huge";
  std::filesystem::create_directories(root);
  for (int i = 0; i < 12; ++i) {
    WriteFile(root / ("dir_" + std::to_string(i)) / "f.txt", "x\n");
  }

  FileTreeWatcher watcher(std::chrono::milliseconds(500));
  watcher.SetEntryBudget(4);  // trip the budget well below the tree size
  watcher.SetRoots({root});
  Expect(watcher.TreeTooLarge(), "watcher should flag a tree that exceeds the entry budget");
  Expect(!watcher.NextPollDelay().has_value(),
         "a too-large watcher should never schedule a periodic poll");
  Expect(!watcher.Poll(), "a too-large watcher should not report spurious changes");

  watcher.Clear();
  Expect(!watcher.TreeTooLarge(), "Clear should reset the too-large flag");
}

#if defined(__linux__) || defined(__APPLE__) || defined(_WIN32)
void TestFileWatcherWakeCallbackSignalsNestedChanges() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "plugins";
  std::filesystem::create_directories(root);

  std::mutex mutex;
  std::condition_variable condition;
  bool notified = false;
  FileTreeWatcher watcher(std::chrono::milliseconds(500));
  watcher.SetWakeCallback([&]() {
    {
      std::lock_guard lock(mutex);
      notified = true;
    }
    condition.notify_one();
  });
  watcher.SetRoots({root});
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  WriteFile(root / "reloadable" / "init.lua", "return 1\n");

  std::unique_lock lock(mutex);
  const bool woke = condition.wait_for(lock, std::chrono::seconds(2), [&]() { return notified; });
  Expect(woke, "native file watcher should notify when nested files change");
  lock.unlock();
  Expect(watcher.Poll(),
         "file watcher wake notifications should correspond to detectable tree changes");
  watcher.SetWakeCallback({});
  watcher.Clear();
}

void TestFileWatcherNativeWakeDoesNotForceZeroDelayPoll() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "plugins";
  std::filesystem::create_directories(root);

  std::mutex mutex;
  std::condition_variable condition;
  bool notified = false;
  FileTreeWatcher watcher(std::chrono::milliseconds(500));
  watcher.SetWakeCallback([&]() {
    {
      std::lock_guard lock(mutex);
      notified = true;
    }
    condition.notify_one();
  });
  watcher.SetRoots({root});
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  WriteFile(root / "reloadable" / "init.lua", "return 1\n");

  std::unique_lock lock(mutex);
  const bool woke = condition.wait_for(lock, std::chrono::seconds(2), [&]() { return notified; });
  Expect(woke, "native file watcher should signal a wake for nested changes");
  lock.unlock();

  Expect(!watcher.NextPollDelay().has_value(),
         "native wake-backed watchers should stay blocked instead of arming a zero-delay poll");
  Expect(watcher.Poll(), "polling after the wake should still consume the detected change");
  watcher.SetWakeCallback({});
  watcher.Clear();
}

#if defined(_WIN32)
void TestFileWatcherDeferredInitialSnapshotArmsWithoutReportingIgnoredWake() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "workspace";
  std::filesystem::create_directories(root / "node_modules" / "pkg");
  WriteFile(root / "node_modules" / "pkg" / "index.js", "module.exports = 1;\n");
  WriteFile(root / "src" / "main.ts", "export const main = 1;\n");

  std::mutex mutex;
  std::condition_variable condition;
  bool notified = false;
  FileTreeWatcher watcher(std::chrono::milliseconds(500));
  watcher.SetDeferInitialSnapshot(true);
  watcher.SetEntryFilter([&root](const std::filesystem::path& path, PathType) {
    if (path == root) {
      return true;
    }

    const std::filesystem::path relative = path.lexically_relative(root);
    if (relative.empty()) {
      return true;
    }

    for (const auto& component : relative) {
      if (component == "node_modules") {
        return false;
      }
    }
    return true;
  });
  watcher.SetWakeCallback([&]() {
    {
      std::lock_guard lock(mutex);
      notified = true;
    }
    condition.notify_one();
  });
  watcher.SetRoots({root});
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  WriteFile(root / "node_modules" / "pkg" / "index.js", "module.exports = 2;\n");

  std::unique_lock lock(mutex);
  const bool woke = condition.wait_for(lock, std::chrono::seconds(2), [&]() { return notified; });
  Expect(woke, "deferred watcher should still receive a native wake for ignored directory churn");
  lock.unlock();

  Expect(!watcher.Poll(),
         "deferred watcher should arm its first filtered snapshot instead of reporting ignored changes");
  watcher.SetWakeCallback({});
  watcher.Clear();
}
#endif  // _WIN32
#endif  // __linux__ || __APPLE__ || _WIN32

#if defined(__unix__) || defined(__APPLE__)
// CopyPath must not throw when the tree contains a symlink (previously it walked
// with a throwing recursive_directory_iterator and dereferenced symlinks). It
// should copy symlinks as symlinks, not follow them into real files/dirs.
void TestCopyPathPreservesSymlinksAndDoesNotThrow() {
  namespace fs = std::filesystem;
  std::error_code ec;
  const fs::path base = fs::temp_directory_path(ec) / "microide_copypath_test";
  fs::remove_all(base, ec);
  const fs::path source = base / "src";
  const fs::path outside = base / "outside";
  fs::create_directories(source / "sub", ec);
  fs::create_directories(outside, ec);
  { std::ofstream(source / "file.txt") << "hello"; }
  { std::ofstream(outside / "target.txt") << "external"; }
  // A symlink pointing outside the copied tree: dereferencing would copy the
  // external file's contents; copy_symlinks must reproduce it as a symlink.
  fs::create_symlink(outside / "target.txt", source / "link.txt", ec);
  Expect(!ec, "symlink fixture created");

  const fs::path destination = base / "dst";
  const bool ok = microide::platform::CopyPath(source, destination);
  Expect(ok, "CopyPath copies a directory containing a symlink without throwing");
  Expect(fs::exists(destination / "file.txt"), "regular file copied");
  Expect(fs::exists(destination / "sub"), "subdirectory copied");
  Expect(fs::is_symlink(destination / "link.txt"),
         "symlink is copied as a symlink, not dereferenced into a real file");

  fs::remove_all(base, ec);
}

// TD-2026-07-17A-131: a top-level symlink to a file must be reproduced as a link,
// not dereferenced into a regular file. Previously is_directory() followed the link
// and CopyPath fell through to copy_file, copying the target bytes — so cross-device
// trash/rename fallbacks turned a file symlink into a real file and dropped the link.
void TestCopyPathPreservesTopLevelFileSymlink() {
  namespace fs = std::filesystem;
  std::error_code ec;
  const fs::path base = fs::temp_directory_path(ec) / "microide_copypath_toplevel_link";
  fs::remove_all(base, ec);
  fs::create_directories(base, ec);
  const fs::path target = base / "target.txt";
  { std::ofstream(target) << "external"; }
  const fs::path link = base / "link.txt";
  fs::create_symlink(target, link, ec);
  Expect(!ec, "top-level file symlink fixture created");

  const fs::path destination = base / "copied_link.txt";
  const bool ok = microide::platform::CopyPath(link, destination);
  Expect(ok, "CopyPath copies a top-level file symlink");
  Expect(fs::is_symlink(fs::symlink_status(destination)),
         "a top-level file symlink is copied as a symlink, not dereferenced into a real file");
  Expect(fs::read_symlink(destination, ec) == target, "the copied symlink keeps its target");

  fs::remove_all(base, ec);
}

// TD-2026-07-17A-132: the portable no-overwrite move fallback must refuse a dangling
// destination symlink. exists() follows the link and reports the broken target as
// absent, so the fallback would clobber the existing link node; symlink_status sees
// the node itself. The cross-device fallback is forced by moving between two temp
// subtrees (same device here, but the exists()/symlink_status distinction is what the
// test pins — the atomic RENAME_NOREPLACE path already refuses a present node).
void TestMovePathNoOverwriteRefusesDanglingDestinationSymlink() {
  namespace fs = std::filesystem;
  std::error_code ec;
  const fs::path base = fs::temp_directory_path(ec) / "microide_move_nooverwrite_link";
  fs::remove_all(base, ec);
  fs::create_directories(base, ec);
  const fs::path source = base / "source.txt";
  { std::ofstream(source) << "payload"; }
  const fs::path destination = base / "dest_link";
  fs::create_symlink(base / "no_such_target", destination, ec);
  Expect(!ec, "dangling destination symlink fixture created");
  Expect(!fs::exists(destination), "the destination symlink target is absent (dangling)");

  const bool moved = microide::platform::MovePathNoOverwrite(source, destination);
  Expect(!moved, "a dangling destination symlink must block a no-overwrite move");
  Expect(fs::is_symlink(fs::symlink_status(destination)),
         "the pre-existing destination link node is left intact");
  Expect(fs::exists(source), "the source is left in place when the move is refused");

  fs::remove_all(base, ec);
}
#endif

// TD-2026-07-17-064: CopyPath must accept a bare-filename destination (empty
// parent_path). Previously it unconditionally called create_directories("") which
// libstdc++ fails with EINVAL, breaking an otherwise valid copy-into-cwd.
void TestCopyPathAcceptsBareFilenameDestination() {
  namespace fs = std::filesystem;
  std::error_code ec;
  const fs::path previous_cwd = fs::current_path(ec);
  TemporaryDirectory temp_dir;
  const fs::path source = temp_dir.path() / "source.txt";
  WriteFile(source, "bare filename copy");

  // Run with the process cwd inside the temp dir so a bare relative filename
  // resolves to a writable location, then restore it no matter the outcome.
  fs::current_path(temp_dir.path(), ec);
  Expect(!ec, "should be able to chdir into the temp dir");
  const bool ok = microide::platform::CopyPath(source, fs::path("copy.txt"));
  const bool copied_exists = fs::exists(temp_dir.path() / "copy.txt", ec);
  const std::string copied = copied_exists ? ReadFile(temp_dir.path() / "copy.txt") : std::string();
  fs::current_path(previous_cwd, ec);

  Expect(ok, "CopyPath should succeed for a bare-filename destination");
  Expect(copied_exists, "the bare-filename copy should exist in the cwd");
  Expect(copied == "bare filename copy", "the bare-filename copy should hold the source bytes");
}

#if defined(__linux__)
// TD-2026-07-17-063: a dangling symlink is a real, removable directory entry that a
// normal file manager can trash. MovePathToTrash validated the source with
// std::filesystem::exists, which follows the link and reports a broken link as
// absent — so it could never be trashed. It now validates the link node itself.
// XDG_DATA_HOME is redirected to a temp dir so the test never touches the real trash.
void TestTrashAcceptsDanglingSymlink() {
  namespace fs = std::filesystem;
  TemporaryDirectory temp_dir;
  const fs::path data_home = temp_dir.path() / "xdg_data";
  std::error_code ec;
  fs::create_directories(data_home, ec);
  ScopedEnvVar data_home_env("XDG_DATA_HOME", data_home.string());

  const fs::path link = temp_dir.path() / "broken_link";
  fs::create_symlink(temp_dir.path() / "no_such_target", link, ec);
  Expect(!ec, "dangling symlink fixture should be created");
  Expect(fs::is_symlink(fs::symlink_status(link)), "fixture should be a symlink");
  Expect(!fs::exists(link), "the symlink target should be absent (dangling)");

  const microide::platform::TrashOperationResult result = microide::platform::MovePathToTrash(link);
  Expect(result.ok, "a dangling symlink should be accepted for trashing, not rejected as absent");
  Expect(!fs::is_symlink(fs::symlink_status(link)),
         "the dangling symlink should be removed from its original location");
}
#endif

// TD-2026-07-17-027: OpenUrl is a trust boundary. Unsupported schemes and
// over-long URLs must be refused before reaching the OS opener. Only the
// rejection paths are exercised here (an allowed scheme would launch a real
// browser); each rejection returns before any SDL/xdg call.
void TestOpenUrlRejectsUnsafeSchemesAndOverlongInput() {
  Expect(!microide::platform::OpenUrl("").ok, "an empty URL is refused");
  Expect(!microide::platform::OpenUrl("javascript:alert(1)").ok,
         "a javascript: URL is refused");
  Expect(!microide::platform::OpenUrl("file:///etc/passwd").ok, "a file: URL is refused");
  Expect(!microide::platform::OpenUrl("data:text/html,<script>").ok, "a data: URL is refused");
  Expect(!microide::platform::OpenUrl("customscheme://x").ok, "an unknown scheme is refused");
  Expect(!microide::platform::OpenUrl("no-scheme-here").ok, "a schemeless string is refused");
  const std::string overlong = "https://example.com/" + std::string(9000, 'a');
  Expect(!microide::platform::OpenUrl(overlong).ok, "an over-long URL is refused");
}

}  // namespace

void RegisterFilesystemTests(std::vector<TestCase>& tests) {
  AddTest(tests, "Platform/OpenUrlRejectsUnsafeSchemesAndOverlongInput",
          TestOpenUrlRejectsUnsafeSchemesAndOverlongInput);
  AddTest(tests, "Filesystem/CopyPathAcceptsBareFilenameDestination",
          TestCopyPathAcceptsBareFilenameDestination);
#if defined(__linux__)
  AddTest(tests, "Trash/AcceptsDanglingSymlink", TestTrashAcceptsDanglingSymlink);
#endif
#if defined(__unix__) || defined(__APPLE__)
  AddTest(tests, "Filesystem/CopyPathPreservesSymlinksAndDoesNotThrow",
          TestCopyPathPreservesSymlinksAndDoesNotThrow);
  AddTest(tests, "Filesystem/CopyPathPreservesTopLevelFileSymlink",
          TestCopyPathPreservesTopLevelFileSymlink);
  AddTest(tests, "Filesystem/MovePathNoOverwriteRefusesDanglingDestinationSymlink",
          TestMovePathNoOverwriteRefusesDanglingDestinationSymlink);
#endif
  AddTest(tests, "Filesystem/ListDirectorySortsAndClassifiesEntries",
          TestFilesystemListDirectorySortsAndClassifiesEntries);
  AddTest(tests, "FileWatcher/DetectsNestedCreatesUpdatesAndDeletes",
          TestFileWatcherDetectsNestedCreatesUpdatesAndDeletes);
  AddTest(tests, "FileWatcher/DetectsCreationOfMissingRoots",
          TestFileWatcherDetectsCreationOfMissingRoots);
  AddTest(tests, "FileWatcher/EntryFilterSkipsIgnoredDirectories",
          TestFileWatcherEntryFilterSkipsIgnoredDirectories);
  AddTest(tests, "Filesystem/CaptureTreeSnapshotRespectsEntryBudget",
          TestCaptureTreeSnapshotRespectsEntryBudget);
  AddTest(tests, "FileWatcher/TreeTooLargeSuppressesPolling",
          TestFileWatcherTreeTooLargeSuppressesPolling);
#if defined(__linux__) || defined(__APPLE__) || defined(_WIN32)
  AddTest(tests, "FileWatcher/WakeCallbackSignalsNestedChanges",
          TestFileWatcherWakeCallbackSignalsNestedChanges);
  AddTest(tests, "FileWatcher/NativeWakeDoesNotForceZeroDelayPoll",
          TestFileWatcherNativeWakeDoesNotForceZeroDelayPoll);
#endif
#if defined(_WIN32)
  // Windows watches the full subtree via FindFirstChangeNotificationW(subtree=TRUE),
  // so churn under a filtered subdirectory still wakes the watcher. inotify/kqueue
  // place per-directory watches and skip filtered subtrees entirely, so this
  // wake-on-ignored-churn contract is Windows-specific.
  AddTest(tests, "FileWatcher/DeferredInitialSnapshotArmsWithoutReportingIgnoredWake",
          TestFileWatcherDeferredInitialSnapshotArmsWithoutReportingIgnoredWake);
#endif
}

}  // namespace microide::tests
