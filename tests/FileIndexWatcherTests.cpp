#include "TestSupport.h"

#include "platform/FileIndexWatcher.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <thread>
#include <vector>

namespace microide::tests {
namespace {

using microide::platform::FileIndexWatcher;
using microide::platform::IndexUpdateBatch;

// Helper: wait up to deadline_ms for a condition to become true.
// Returns true if condition was met.
template <typename Pred>
bool WaitFor(std::mutex& mutex, std::condition_variable& cv, Pred pred,
             std::chrono::milliseconds timeout) {
  std::unique_lock lock(mutex);
  return cv.wait_for(lock, timeout, pred);
}

bool BatchContainsPath(const IndexUpdateBatch& batch, const std::filesystem::path& relative_path) {
  for (const auto& change : batch.changes) {
    if (change.entry.relative_path == relative_path) {
      return true;
    }
  }
  return false;
}

void TestFileIndexWatcherInitialBatchContainsExistingFiles() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  WriteFile(root / "src" / "main.cpp", "int main() {}\n");
  WriteFile(root / "src" / "util.h", "// util\n");
  WriteFile(root / "README.md", "# Project\n");

  std::mutex mutex;
  std::condition_variable cv;
  std::vector<IndexUpdateBatch> batches;

  FileIndexWatcher watcher;
  watcher.SetCallback([&](IndexUpdateBatch batch) {
    std::lock_guard lock(mutex);
    batches.push_back(std::move(batch));
    cv.notify_all();
  });

  const bool ok = watcher.Watch(root);
  Expect(ok, "FileIndexWatcher::Watch should succeed on an existing directory");

  const bool found_initial_batch = WaitFor(
      mutex, cv,
      [&] {
        for (const auto& batch : batches) {
          if (batch.is_initial) {
            return true;
          }
        }
        return false;
      },
      std::chrono::milliseconds(1200));
  Expect(found_initial_batch, "FileIndexWatcher should emit an initial batch with is_initial=true");

  bool found_initial = false;
  {
    std::lock_guard lock(mutex);
    for (const auto& batch : batches) {
      if (batch.is_initial) {
        found_initial = true;
        // Should contain at least the 3 files
        std::size_t file_count = 0;
        for (const auto& change : batch.changes) {
          if (change.kind == IndexUpdateBatch::Kind::CreatedOrModified) {
            ++file_count;
          }
        }
        Expect(file_count >= 3,
               "initial IndexUpdateBatch should include all pre-existing files");
      }
    }
  }
  Expect(found_initial, "initial batch should remain available after emission");

  watcher.Unwatch();
}

void TestFileIndexWatcherDetectsCreatedFile() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "watch_create";
  std::filesystem::create_directories(root);

  std::mutex mutex;
  std::condition_variable cv;
  bool saw_created = false;
  std::filesystem::path created_rel;

  FileIndexWatcher watcher;
  watcher.SetCallback([&](IndexUpdateBatch batch) {
    if (batch.is_initial) {
      return;
    }
    std::lock_guard lock(mutex);
    for (const auto& change : batch.changes) {
      if (change.kind == IndexUpdateBatch::Kind::CreatedOrModified) {
        saw_created = true;
        created_rel = change.entry.relative_path;
      }
    }
    cv.notify_all();
  });

  Expect(watcher.Watch(root), "Watch should succeed on empty directory");

  // Create a file after watching starts
  WriteFile(root / "new_file.txt", "hello\n");

  const bool notified =
      WaitFor(mutex, cv, [&] { return saw_created; }, std::chrono::milliseconds(600));
  Expect(notified,
         "FileIndexWatcher should detect newly created file within 600ms");
  if (notified) {
    Expect(created_rel == std::filesystem::path("new_file.txt"),
           "FileIndexWatcher should report correct relative path for created file");
  }

  watcher.Unwatch();
}

void TestFileIndexWatcherSkipsGitMetadataInInitialBatch() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  WriteFile(root / ".git" / "HEAD", "ref: refs/heads/main\n");
  WriteFile(root / ".git" / "config", "[core]\n");
  WriteFile(root / "README.md", "# Project\n");

  std::mutex mutex;
  std::condition_variable cv;
  std::vector<IndexUpdateBatch> batches;

  FileIndexWatcher watcher;
  watcher.SetCallback([&](IndexUpdateBatch batch) {
    std::lock_guard lock(mutex);
    batches.push_back(std::move(batch));
    cv.notify_all();
  });

  Expect(watcher.Watch(root), "Watch should succeed on a git-backed directory");

  const bool found_initial_batch = WaitFor(
      mutex, cv,
      [&] {
        for (const auto& batch : batches) {
          if (batch.is_initial) {
            return true;
          }
        }
        return false;
      },
      std::chrono::milliseconds(1200));
  Expect(found_initial_batch, "FileIndexWatcher should emit an initial batch for git-backed projects");

  bool found_initial = false;
  bool saw_readme = false;
  bool saw_git_head = false;
  bool saw_git_config = false;
  {
    std::lock_guard lock(mutex);
    for (const auto& batch : batches) {
      if (!batch.is_initial) {
        continue;
      }
      found_initial = true;
      saw_readme = saw_readme || BatchContainsPath(batch, std::filesystem::path("README.md"));
      saw_git_head = saw_git_head || BatchContainsPath(batch, std::filesystem::path(".git/HEAD"));
      saw_git_config =
          saw_git_config || BatchContainsPath(batch, std::filesystem::path(".git/config"));
    }
  }

  Expect(found_initial, "initial batch should remain available for git-backed projects");
  Expect(saw_readme, "initial IndexUpdateBatch should still include visible project files");
  Expect(!saw_git_head,
         "initial IndexUpdateBatch should skip .git metadata files such as HEAD");
  Expect(!saw_git_config,
         "initial IndexUpdateBatch should skip .git metadata files such as config");

  watcher.Unwatch();
}

void TestFileIndexWatcherIgnoresGitMetadataUpdates() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "watch_git_metadata";
  std::filesystem::create_directories(root / ".git");

  std::mutex mutex;
  std::condition_variable cv;
  bool saw_project_file = false;
  bool saw_git_metadata = false;

  FileIndexWatcher watcher;
  watcher.SetCallback([&](IndexUpdateBatch batch) {
    if (batch.is_initial) {
      return;
    }
    std::lock_guard lock(mutex);
    for (const auto& change : batch.changes) {
      if (change.entry.relative_path == std::filesystem::path("visible.txt")) {
        saw_project_file = true;
      }
      if (!change.entry.relative_path.empty() &&
          *change.entry.relative_path.begin() == std::filesystem::path(".git")) {
        saw_git_metadata = true;
      }
    }
    cv.notify_all();
  });

  Expect(watcher.Watch(root), "Watch should succeed on a repository root");

  WriteFile(root / ".git" / "index.lock", "lock\n");
  WriteFile(root / "visible.txt", "tracked\n");

  const bool notified =
      WaitFor(mutex, cv, [&] { return saw_project_file; }, std::chrono::milliseconds(1200));
  Expect(notified,
         "FileIndexWatcher should still detect visible project files after git metadata changes");
  Expect(!saw_git_metadata,
         "FileIndexWatcher should not report .git metadata updates such as index.lock");

  watcher.Unwatch();
}

void TestFileIndexWatcherDetectsDeletedFile() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "watch_delete";
  WriteFile(root / "target.txt", "to be deleted\n");

  std::mutex mutex;
  std::condition_variable cv;
  bool saw_deleted = false;

  FileIndexWatcher watcher;
  watcher.SetCallback([&](IndexUpdateBatch batch) {
    if (batch.is_initial) {
      return;
    }
    std::lock_guard lock(mutex);
    for (const auto& change : batch.changes) {
      if (change.kind == IndexUpdateBatch::Kind::Deleted &&
          change.entry.relative_path == std::filesystem::path("target.txt")) {
        saw_deleted = true;
      }
    }
    cv.notify_all();
  });

  Expect(watcher.Watch(root), "Watch should succeed");

  std::filesystem::remove(root / "target.txt");

  const bool notified =
      WaitFor(mutex, cv, [&] { return saw_deleted; }, std::chrono::milliseconds(600));
  Expect(notified,
         "FileIndexWatcher should detect deleted file within 600ms");

  watcher.Unwatch();
}

void TestFileIndexWatcherUnwatchStopsNotifications() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "watch_stop";
  std::filesystem::create_directories(root);

  std::atomic<int> call_count{0};

  FileIndexWatcher watcher;
  watcher.SetCallback([&](IndexUpdateBatch /*batch*/) {
    ++call_count;
  });

  Expect(watcher.Watch(root), "Watch should succeed");
  watcher.Unwatch();

  const int count_after_unwatch = call_count.load();

  // Create a file after Unwatch; should NOT trigger callback
  WriteFile(root / "late_file.txt", "late\n");
  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  Expect(call_count.load() == count_after_unwatch,
         "FileIndexWatcher should not fire callback after Unwatch()");
}

void TestFileIndexWatcherSkipsGitignoredDirectoriesInInitialBatch() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  WriteFile(root / ".gitignore", "build/\nnode_modules/\n");
  WriteFile(root / "src" / "main.cpp", "int main() {}\n");
  WriteFile(root / "build" / "out.o", "binary\n");
  WriteFile(root / "node_modules" / "pkg" / "index.js", "module.exports = 1;\n");
  WriteFile(root / "README.md", "# Project\n");

  std::mutex mutex;
  std::condition_variable cv;
  std::vector<IndexUpdateBatch> batches;

  FileIndexWatcher watcher;
  watcher.SetCallback([&](IndexUpdateBatch batch) {
    std::lock_guard lock(mutex);
    batches.push_back(std::move(batch));
    cv.notify_all();
  });

  Expect(watcher.Watch(root), "Watch should succeed on .gitignore-backed project");

  const bool got_initial = WaitFor(
      mutex, cv,
      [&] {
        for (const auto& batch : batches) {
          if (batch.is_initial) {
            return true;
          }
        }
        return false;
      },
      std::chrono::milliseconds(1500));
  Expect(got_initial, "FileIndexWatcher should emit an initial batch when .gitignore is present");

  bool saw_main = false;
  bool saw_readme = false;
  bool saw_build_artifact = false;
  bool saw_node_modules = false;
  {
    std::lock_guard lock(mutex);
    for (const auto& batch : batches) {
      if (!batch.is_initial) {
        continue;
      }
      saw_main = saw_main || BatchContainsPath(batch, std::filesystem::path("src/main.cpp"));
      saw_readme = saw_readme || BatchContainsPath(batch, std::filesystem::path("README.md"));
      saw_build_artifact =
          saw_build_artifact || BatchContainsPath(batch, std::filesystem::path("build/out.o"));
      saw_node_modules = saw_node_modules ||
                         BatchContainsPath(batch, std::filesystem::path("node_modules/pkg/index.js"));
    }
  }
  Expect(saw_main, "initial batch should contain non-ignored sources");
  Expect(saw_readme, "initial batch should contain non-ignored top-level files");
  Expect(!saw_build_artifact,
         "initial batch should skip files under .gitignored directories (build/)");
  Expect(!saw_node_modules,
         "initial batch should skip files under .gitignored directories (node_modules/)");

  watcher.Unwatch();
}

void TestFileIndexWatcherIgnoresChangesInsideGitignoredDirectory() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "watch_ignored_changes";
  WriteFile(root / ".gitignore", "ignored/\n");
  std::filesystem::create_directories(root / "ignored");

  std::mutex mutex;
  std::condition_variable cv;
  bool saw_visible = false;
  bool saw_ignored = false;

  FileIndexWatcher watcher;
  watcher.SetCallback([&](IndexUpdateBatch batch) {
    if (batch.is_initial) {
      return;
    }
    std::lock_guard lock(mutex);
    for (const auto& change : batch.changes) {
      const auto first = change.entry.relative_path.empty()
                             ? std::filesystem::path{}
                             : *change.entry.relative_path.begin();
      if (first == std::filesystem::path("ignored")) {
        saw_ignored = true;
      } else if (change.entry.relative_path == std::filesystem::path("visible.txt")) {
        saw_visible = true;
      }
    }
    cv.notify_all();
  });

  Expect(watcher.Watch(root), "Watch should succeed");

  WriteFile(root / "ignored" / "secret.txt", "hidden\n");
  WriteFile(root / "visible.txt", "tracked\n");

  const bool notified =
      WaitFor(mutex, cv, [&] { return saw_visible; }, std::chrono::milliseconds(1500));
  Expect(notified, "FileIndexWatcher should still notify for non-ignored files");
  Expect(!saw_ignored,
         "FileIndexWatcher should not emit change events for files under .gitignored dirs");

  watcher.Unwatch();
}

void TestFileIndexWatcherWatchReturnsPromptly() {
  // Build a deep, wide tree (~2000 directories) so that a synchronous recursive
  // walk would be observable. The async setup path should keep Watch() under a
  // generous threshold even on slow machines.
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "wide_tree";
  std::filesystem::create_directories(root);
  for (int outer = 0; outer < 40; ++outer) {
    for (int inner = 0; inner < 50; ++inner) {
      const auto sub = root / ("d" + std::to_string(outer)) / ("s" + std::to_string(inner));
      std::filesystem::create_directories(sub);
      WriteFile(sub / "file.txt", "x\n");
    }
  }

  FileIndexWatcher watcher;
  watcher.SetCallback([](IndexUpdateBatch /*batch*/) {});

  const auto start = std::chrono::steady_clock::now();
  const bool ok = watcher.Watch(root);
  const auto elapsed =
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
  Expect(ok, "Watch should succeed on wide tree");
  // The watch-tree build runs on a background thread; Watch() itself should be fast.
  // 200ms is loose to absorb sanitizer / loaded-CI overhead while still catching the
  // pre-fix behavior where this took ~1s on real projects.
  Expect(elapsed.count() < 200,
         "FileIndexWatcher::Watch should return promptly while watches build asynchronously");

  watcher.Unwatch();
}

void TestFileIndexWatcherUnwatchDuringBootstrapIsSafe() {
  // Build a sizable tree, then immediately Unwatch() to exercise the
  // stop-during-setup path in the Linux backend. Any race or use-after-free
  // would surface under ASAN/TSAN here.
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "bootstrap_cancel";
  std::filesystem::create_directories(root);
  for (int outer = 0; outer < 30; ++outer) {
    for (int inner = 0; inner < 30; ++inner) {
      const auto sub = root / ("a" + std::to_string(outer)) / ("b" + std::to_string(inner));
      std::filesystem::create_directories(sub);
    }
  }

  for (int trial = 0; trial < 5; ++trial) {
    FileIndexWatcher watcher;
    watcher.SetCallback([](IndexUpdateBatch /*batch*/) {});
    Expect(watcher.Watch(root), "Watch should succeed");
    watcher.Unwatch();  // No assertion: success is "did not crash / hang".
  }
}

}  // namespace

void RegisterFileIndexWatcherTests(std::vector<TestCase>& tests) {
  AddTest(tests, "FileIndexWatcher/InitialBatchContainsExistingFiles",
          TestFileIndexWatcherInitialBatchContainsExistingFiles);
  AddTest(tests, "FileIndexWatcher/DetectsCreatedFile",
          TestFileIndexWatcherDetectsCreatedFile);
  AddTest(tests, "FileIndexWatcher/SkipsGitMetadataInInitialBatch",
          TestFileIndexWatcherSkipsGitMetadataInInitialBatch);
  AddTest(tests, "FileIndexWatcher/IgnoresGitMetadataUpdates",
          TestFileIndexWatcherIgnoresGitMetadataUpdates);
  AddTest(tests, "FileIndexWatcher/DetectsDeletedFile",
          TestFileIndexWatcherDetectsDeletedFile);
  AddTest(tests, "FileIndexWatcher/UnwatchStopsNotifications",
          TestFileIndexWatcherUnwatchStopsNotifications);
  AddTest(tests, "FileIndexWatcher/SkipsGitignoredDirectoriesInInitialBatch",
          TestFileIndexWatcherSkipsGitignoredDirectoriesInInitialBatch);
  AddTest(tests, "FileIndexWatcher/IgnoresChangesInsideGitignoredDirectory",
          TestFileIndexWatcherIgnoresChangesInsideGitignoredDirectory);
  AddTest(tests, "FileIndexWatcher/WatchReturnsPromptly",
          TestFileIndexWatcherWatchReturnsPromptly);
  AddTest(tests, "FileIndexWatcher/UnwatchDuringBootstrapIsSafe",
          TestFileIndexWatcherUnwatchDuringBootstrapIsSafe);
}

}  // namespace microide::tests
