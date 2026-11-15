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

// What the watcher reported, and a wait for one path to be among it.
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

// The project watcher never looks inside `.git`; a document open there (HEAD,
// COMMIT_EDITMSG) is reported by this one, including git's lock-and-rename write.
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

// A document open through a symlink changes when its target is written.
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

// A watched directory that goes takes its watch with it: its files are reported,
// and the signature resets so the caller re-arms what comes back.
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

// Soak adbdbecb317b: `git stash -u` took the whole untracked folder an open file
// lived in, the caller's re-arm came while it was still gone -- the watch failed
// and the set read as armed all the same -- and `git stash pop` brought the
// folder and the file back with nobody listening: the clean buffer kept text the
// file no longer held. A missing directory is waited for from the nearest one
// that exists, and what reappears is reported once it is watched again.
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
  watcher.SetFiles(3, {file});  // the re-arm, while the directory is still gone

  WriteFile(file, "two\n");  // the folder and its file come back, and nothing re-arms
  Expect(reports.WaitFor(file), "the file that came back is reported");
  {
    std::lock_guard lock(reports.mutex);
    reports.paths.clear();
  }
  WriteFile(file, "three\n");
  Expect(reports.WaitFor(file), "and its directory is watched again");
}

// Two levels gone, coming back one at a time (a checkout makes the parent before
// the child): the wait follows them down.
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

}  // namespace

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

}  // namespace microide::tests
