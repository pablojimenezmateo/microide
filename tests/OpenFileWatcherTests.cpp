#include "TestSupport.h"

#include "platform/OpenFileWatcher.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <thread>
#include <vector>

namespace microide::tests {
namespace {

using microide::platform::OpenFileWatcher;

struct Reports {
  std::mutex mutex;
  std::condition_variable changed;
  std::vector<std::filesystem::path> paths;

  OpenFileWatcher::Callback Callback() {
    return [this](std::vector<std::filesystem::path> batch) {
      std::lock_guard lock(mutex);
      paths.insert(paths.end(), batch.begin(), batch.end());
      changed.notify_all();
    };
  }
  bool WaitFor(const std::filesystem::path& path) {
    std::unique_lock lock(mutex);
    return changed.wait_for(lock, std::chrono::seconds(5), [&]() {
      return std::find(paths.begin(), paths.end(), path) != paths.end();
    });
  }
  bool Holds(const std::filesystem::path& path) {
    std::lock_guard lock(mutex);
    return std::find(paths.begin(), paths.end(), path) != paths.end();
  }
};

void TestOpenFileWatcherReportsAnAtomicReplaceInsideGitMetadata() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path head = temp_dir.path() / "project" / ".git" / "HEAD";
  const std::filesystem::path other = temp_dir.path() / "project" / ".git" / "ORIG_HEAD";
  WriteFile(head, "ref: refs/heads/main\n");
  WriteFile(other, "0000\n");
  Reports reports;
  OpenFileWatcher watcher(reports.Callback());
  watcher.SetFiles(7, {head});
  Expect(watcher.files_signature() == 7, "the signature is what the caller named the set with");
  WriteFile(other, "1111\n");
  WriteFile(head.parent_path() / "HEAD.lock", "ref: refs/heads/topic\n");
  std::filesystem::rename(head.parent_path() / "HEAD.lock", head);
  Expect(reports.WaitFor(head), "a rename over the open file is reported");
  Expect(!reports.Holds(other), "a file nobody has open is not");
}

void TestOpenFileWatcherReportsASymlinkedFileUnderItsOpenPath() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path target = temp_dir.path() / "elsewhere" / "data.txt";
  const std::filesystem::path link = temp_dir.path() / "project" / "link.txt";
  WriteFile(target, "one\n");
  std::filesystem::create_directories(link.parent_path());
  std::filesystem::create_symlink(target, link);
  Reports reports;
  OpenFileWatcher watcher(reports.Callback());
  watcher.SetFiles(1, {link});
  WriteFile(target, "two\n");
  Expect(reports.WaitFor(link), "the write to the target is reported as the link");
}

void TestOpenFileWatcherReArmsAfterItsDirectoryGoes() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path file = temp_dir.path() / "tree" / "notes.txt";
  WriteFile(file, "one\n");
  Reports reports;
  OpenFileWatcher watcher(reports.Callback());
  watcher.SetFiles(3, {file});
  std::filesystem::remove_all(file.parent_path());
  Expect(reports.WaitFor(file), "the deleted file is reported");
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (watcher.files_signature() != 0 && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  Expect(watcher.files_signature() == 0, "the set reads as owing a re-arm");
  WriteFile(file, "two\n");
  {
    std::lock_guard lock(reports.mutex);
    reports.paths.clear();
  }
  watcher.SetFiles(3, {file});
  WriteFile(file, "three\n");
  Expect(reports.WaitFor(file), "the recreated directory is watched again");
}

void TestOpenFileWatcherWaitsForADirectoryThatIsGoneWhenArmed() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path file = temp_dir.path() / "tree" / "notes.txt";
  WriteFile(file, "one\n");
  Reports reports;
  OpenFileWatcher watcher(reports.Callback());
  watcher.SetFiles(3, {file});
  std::filesystem::remove_all(file.parent_path());
  Expect(reports.WaitFor(file), "the deleted file is reported");
  {
    std::lock_guard lock(reports.mutex);
    reports.paths.clear();
  }
  watcher.SetFiles(3, {file});

  WriteFile(file, "two\n");
  Expect(reports.WaitFor(file), "the file that came back is reported");
  {
    std::lock_guard lock(reports.mutex);
    reports.paths.clear();
  }
  WriteFile(file, "three\n");
  Expect(reports.WaitFor(file), "and its directory is watched again");
}

void TestOpenFileWatcherFollowsADeepDirectoryBackDown() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path top = temp_dir.path() / "a";
  const std::filesystem::path file = top / "b" / "c" / "notes.txt";
  std::filesystem::create_directories(temp_dir.path());
  Reports reports;
  OpenFileWatcher watcher(reports.Callback());
  watcher.SetFiles(5, {file});
  std::filesystem::create_directory(top);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  std::filesystem::create_directory(top / "b");
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  std::filesystem::create_directory(top / "b" / "c");
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  WriteFile(file, "here\n");
  Expect(reports.WaitFor(file), "the file is reported once its directory exists");
}

}

void RegisterOpenFileWatcherTests(std::vector<TestCase>& tests) {
  AddTest(tests, "OpenFileWatcher/ReportsAnAtomicReplaceInsideGitMetadata",
          TestOpenFileWatcherReportsAnAtomicReplaceInsideGitMetadata);
  AddTest(tests, "OpenFileWatcher/ReportsASymlinkedFileUnderItsOpenPath",
          TestOpenFileWatcherReportsASymlinkedFileUnderItsOpenPath);
  AddTest(tests, "OpenFileWatcher/ReArmsAfterItsDirectoryGoes",
          TestOpenFileWatcherReArmsAfterItsDirectoryGoes);
  AddTest(tests, "OpenFileWatcher/WaitsForADirectoryThatIsGoneWhenArmed",
          TestOpenFileWatcherWaitsForADirectoryThatIsGoneWhenArmed);
  AddTest(tests, "OpenFileWatcher/FollowsADeepDirectoryBackDown",
          TestOpenFileWatcherFollowsADeepDirectoryBackDown);
}

}
