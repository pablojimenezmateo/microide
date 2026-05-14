#include "TestSupport.h"

#include "platform/FileWatcher.h"
#include "platform/Filesystem.h"

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <thread>

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
#endif

}  // namespace

void RegisterFilesystemTests(std::vector<TestCase>& tests) {
  AddTest(tests, "Filesystem/ListDirectorySortsAndClassifiesEntries",
          TestFilesystemListDirectorySortsAndClassifiesEntries);
  AddTest(tests, "FileWatcher/DetectsNestedCreatesUpdatesAndDeletes",
          TestFileWatcherDetectsNestedCreatesUpdatesAndDeletes);
  AddTest(tests, "FileWatcher/DetectsCreationOfMissingRoots",
          TestFileWatcherDetectsCreationOfMissingRoots);
  AddTest(tests, "FileWatcher/EntryFilterSkipsIgnoredDirectories",
          TestFileWatcherEntryFilterSkipsIgnoredDirectories);
#if defined(__linux__) || defined(__APPLE__) || defined(_WIN32)
  AddTest(tests, "FileWatcher/WakeCallbackSignalsNestedChanges",
          TestFileWatcherWakeCallbackSignalsNestedChanges);
  AddTest(tests, "FileWatcher/NativeWakeDoesNotForceZeroDelayPoll",
          TestFileWatcherNativeWakeDoesNotForceZeroDelayPoll);
  AddTest(tests, "FileWatcher/DeferredInitialSnapshotArmsWithoutReportingIgnoredWake",
          TestFileWatcherDeferredInitialSnapshotArmsWithoutReportingIgnoredWake);
#endif
}

}  // namespace microide::tests
