#include "TestSupport.h"

#include "platform/FileIndexWatcher.h"
#include "project/FileIndex.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace microide::tests {
namespace {

namespace fs = std::filesystem;

struct WatcherContractFixture {
  TemporaryDirectory temp;
  platform::FileIndexWatcher watcher;
  project::FileIndex index;
  std::mutex mutex;
  std::vector<platform::IndexUpdateBatch> pending;
  bool saw_initial = false;
  bool saw_tree_structure_change = false;
  int shape_change_batches = 0;

  explicit WatcherContractFixture(bool force_poll) {
    index.SetRoot(temp.path(), project::FileIndex::RootPopulationMode::Deferred);
    watcher.SetForcePollForTesting(force_poll);
    watcher.SetPollIntervalForTesting(std::chrono::milliseconds(25));
    watcher.SetCallback([this](platform::IndexUpdateBatch batch) {
      std::lock_guard<std::mutex> lock(mutex);
      pending.push_back(std::move(batch));
    });
  }

  ~WatcherContractFixture() { watcher.Unwatch(); }

  void Pump() {
    std::vector<platform::IndexUpdateBatch> local;
    {
      std::lock_guard<std::mutex> lock(mutex);
      local.swap(pending);
    }
    for (platform::IndexUpdateBatch& batch : local) {
      saw_initial = saw_initial || batch.is_initial;
      if (batch.tree_structure_changed) {
        ++shape_change_batches;
        saw_tree_structure_change = true;
      }
      index.ApplyBatch(batch);
    }
  }

  bool IndexHas(const char* relative) {
    const fs::path needle(relative);
    for (const project::ProjectFile& file : index.Snapshot()) {
      if (file.relative_path == needle) {
        return true;
      }
    }
    return false;
  }

  std::uintmax_t IndexedSize(const char* relative) {
    const fs::path needle(relative);
    for (const project::ProjectFile& file : index.Snapshot()) {
      if (file.relative_path == needle) {
        return file.size;
      }
    }
    return 0;
  }

  bool WaitIndexed(const char* relative) {
    return WaitUntil([&] { return IndexHas(relative); }, std::chrono::seconds(5),
                     std::chrono::milliseconds(5), [&] { Pump(); });
  }

  bool WaitGone(const char* relative) {
    return WaitUntil([&] { return !IndexHas(relative); }, std::chrono::seconds(5),
                     std::chrono::milliseconds(5), [&] { Pump(); });
  }

  bool WaitTreeStructureChange() {
    return WaitUntil([&] { return saw_tree_structure_change; }, std::chrono::seconds(5),
                     std::chrono::milliseconds(5), [&] { Pump(); });
  }

  bool Quiesce(const char* marker) {
    WriteFile(temp.path() / marker, "quiesce\n");
    if (!WaitIndexed(marker)) {
      return false;
    }
    Pump();
    saw_tree_structure_change = false;
    return true;
  }
};

void RunWatcherContract(bool force_poll) {
  WatcherContractFixture fixture(force_poll);
  const fs::path& root = fixture.temp.path();

  WriteFile(root / "a.txt", "alpha\n");
  fs::create_directories(root / "sub");
  WriteFile(root / "sub" / "b.txt", "beta\n");
  WriteFile(root / ".gitignore", "ignored/\n");
  fs::create_directories(root / "ignored");
  WriteFile(root / "ignored" / "c.txt", "c\n");

  Expect(fixture.watcher.Watch(root), "watcher contract: Watch should start");
  if (force_poll) {
    Expect(!fixture.watcher.IsNative(),
           "watcher contract: forcing the poll fallback must not report native");
  }

  Expect(fixture.WaitIndexed("a.txt"), "watcher contract: initial batch indexes root files");
  Expect(fixture.IndexHas("sub/b.txt"), "watcher contract: initial batch indexes nested files");
  Expect(fixture.saw_initial, "watcher contract: the first population arrives as is_initial");
  Expect(!fixture.IndexHas("ignored/c.txt"),
         "watcher contract: gitignored directories stay out of the initial batch");

  WriteFile(root / "d.txt", "delta\n");
  Expect(fixture.WaitIndexed("d.txt"), "watcher contract: a created file becomes indexed");

  const std::string grown = "alpha grew substantially longer\n";
  WriteFile(root / "a.txt", grown);
  Expect(WaitUntil([&] { return fixture.IndexedSize("a.txt") == grown.size(); },
                   std::chrono::seconds(5), std::chrono::milliseconds(5),
                   [&] { fixture.Pump(); }),
         "watcher contract: a modified file's indexed size converges to disk");

  fs::create_directories(root / "deep" / "x");
  WriteFile(root / "deep" / "x" / "y.txt", "y\n");
  Expect(fixture.WaitIndexed("deep/x/y.txt"),
         "watcher contract: files in freshly created nested directories are indexed");

  fs::rename(root / "d.txt", root / "e.txt");
  Expect(fixture.WaitIndexed("e.txt"), "watcher contract: a renamed file appears at its new path");
  Expect(fixture.WaitGone("d.txt"), "watcher contract: a renamed file leaves its old path");

  fs::create_directories(root / "movedir");
  WriteFile(root / "movedir" / "m.txt", "m\n");
  Expect(fixture.WaitIndexed("movedir/m.txt"), "watcher contract: the directory to move is indexed");
  fs::rename(root / "movedir", root / "renamed");
  Expect(fixture.WaitIndexed("renamed/m.txt"),
         "watcher contract: a renamed directory's files appear at the new path");
  Expect(fixture.WaitGone("movedir/m.txt"),
         "watcher contract: a renamed directory's files leave the old path");
  WriteFile(root / "renamed-sentinel.txt", "s\n");
  Expect(fixture.WaitIndexed("renamed-sentinel.txt"), "watcher contract: sentinel write lands");
  fixture.Pump();
  Expect(fixture.IndexHas("renamed/m.txt"),
         "watcher contract: a renamed directory's files STAY indexed after its own move event");
  WriteFile(root / "renamed" / "n.txt", "n\n");
  Expect(fixture.WaitIndexed("renamed/n.txt"),
         "watcher contract: a renamed directory is still watched at its new path");

  fs::remove(root / "sub" / "b.txt");
  Expect(fixture.WaitGone("sub/b.txt"), "watcher contract: a deleted file leaves the index");

  fs::remove_all(root / "deep");
  Expect(fixture.WaitGone("deep/x/y.txt"),
         "watcher contract: deleting a directory removes its indexed contents");

  WriteFile(root / "ignored" / "z.txt", "z\n");
  WriteFile(root / "sentinel.txt", "s\n");
  Expect(fixture.WaitIndexed("sentinel.txt"), "watcher contract: sentinel write lands");
  fixture.Pump();
  Expect(!fixture.IndexHas("ignored/z.txt"),
         "watcher contract: gitignored churn stays out of the index");
}

void RunWatcherStartupWindowContract(bool force_poll) {
  WatcherContractFixture fixture(force_poll);
  const fs::path& root = fixture.temp.path();

  for (int i = 0; i < 400; ++i) {
    WriteFile(root / ("seed-" + std::to_string(i) + ".txt"), "seed\n");
  }

  Expect(fixture.watcher.Watch(root), "startup-window contract: Watch should start");
  std::vector<std::string> racing;
  racing.reserve(64);
  for (int i = 0; i < 64; ++i) {
    const std::string name = "racing-" + std::to_string(i) + ".txt";
    WriteFile(root / name, "racing\n");
    racing.push_back(name);
  }

  const bool all_indexed = WaitUntil(
      [&] {
        for (const std::string& name : racing) {
          if (!fixture.IndexHas(name.c_str())) {
            return false;
          }
        }
        return true;
      },
      std::chrono::seconds(10), std::chrono::milliseconds(5), [&] { fixture.Pump(); });
  if (!all_indexed) {
    std::string missing;
    for (const std::string& name : racing) {
      if (!fixture.IndexHas(name.c_str())) {
        missing += name;
        missing += ' ';
      }
    }
    Expect(false, "startup-window contract: files created during watcher startup were "
                  "never indexed: " + missing);
  }
  Expect(fixture.saw_initial, "startup-window contract: an is_initial batch still arrives");
}

void RunWatcherTreeShapeContract(bool force_poll) {
  WatcherContractFixture fixture(force_poll);
  const fs::path& root = fixture.temp.path();

  WriteFile(root / "a.txt", "alpha\n");
  WriteFile(root / ".gitignore", "ignored/\n");

  Expect(fixture.watcher.Watch(root), "tree-shape contract: Watch should start");
  Expect(fixture.WaitIndexed("a.txt"), "tree-shape contract: the initial batch lands");
  Expect(!fixture.saw_tree_structure_change,
         "tree-shape contract: opening a project is not itself a shape change");

  fs::create_directories(root / "empty-dir");
  Expect(fixture.WaitTreeStructureChange(),
         "tree-shape contract: creating an empty directory reports a shape change");

  Expect(fixture.Quiesce("quiesce-create.txt"),
         "tree-shape contract: the barrier after the create drains");
  fs::remove(root / "empty-dir");
  Expect(fixture.WaitTreeStructureChange(),
         "tree-shape contract: removing an empty directory reports a shape change");

  Expect(fixture.Quiesce("quiesce-remove.txt"),
         "tree-shape contract: the barrier before the negative assertion drains");
  const int shape_batches_before = fixture.shape_change_batches;
  fs::create_directories(root / "ignored" / "nested");
  WriteFile(root / "sentinel.txt", "s\n");
  Expect(fixture.WaitIndexed("sentinel.txt"), "tree-shape contract: sentinel write lands");
  fixture.Pump();
  Expect(!fixture.saw_tree_structure_change,
         "tree-shape contract: a gitignored directory is not a shape change (shape batches "
         "before the ignored mkdir=" + std::to_string(shape_batches_before) +
             ", after=" + std::to_string(fixture.shape_change_batches) + ")");
}

void TestFileIndexWatcherContractNativeBackend() {
  RunWatcherContract(false);
}

void TestFileIndexWatcherContractPollBackend() {
  RunWatcherContract(true);
}

}

void RegisterFileIndexWatcherContractTests(std::vector<TestCase>& tests) {
  AddTest(tests, "FileIndexWatcherContract/NativeBackend",
          TestFileIndexWatcherContractNativeBackend);
  AddTest(tests, "FileIndexWatcherContract/PollBackend",
          TestFileIndexWatcherContractPollBackend);
  AddTest(tests, "FileIndexWatcherContract/NativeBackendStartupWindow",
          [] { RunWatcherStartupWindowContract(false); });
  AddTest(tests, "FileIndexWatcherContract/PollBackendStartupWindow",
          [] { RunWatcherStartupWindowContract(true); });
  AddTest(tests, "FileIndexWatcherContract/NativeBackendTreeShape",
          [] { RunWatcherTreeShapeContract(false); });
  AddTest(tests, "FileIndexWatcherContract/PollBackendTreeShape",
          [] { RunWatcherTreeShapeContract(true); });
}

}
