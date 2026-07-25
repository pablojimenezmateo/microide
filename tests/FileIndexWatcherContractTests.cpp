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

// Backend-independent watcher contract suite (TD-2026-07-17-036). One contract
// function runs against every backend selectable on this host — the native
// backend (inotify on Linux, FSEvents on macOS, ReadDirectoryChangesW on
// Windows) and the shared poll fallback (forced via SetForcePollForTesting).
// The backends emit differently-shaped batches (native reports a recursive
// directory delete; poll diffs per-file), so the contract is stated through the
// consumer's end state: batches are applied to a real project::FileIndex — the
// same apply hop production uses — and the assertions are about which files the
// index ends up containing. Adding a new platform backend (006/010) means
// passing this suite unchanged.

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

  explicit WatcherContractFixture(bool force_poll) {
    index.SetRoot(temp.path(), project::FileIndex::RootPopulationMode::Deferred);
    watcher.SetForcePollForTesting(force_poll);
    // Keep the poll backend fast: the production 750ms cadence would make each
    // contract step wait most of a second.
    watcher.SetPollIntervalForTesting(std::chrono::milliseconds(25));
    // The watcher callback fires on its background thread; production hops the
    // batch to the main thread before applying. Mirror that: buffer here, apply
    // in Pump() on the test (main) thread.
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
};

// The shared behavioral contract every backend must satisfy. `force_poll`
// selects the backend under test.
void RunWatcherContract(bool force_poll) {
  WatcherContractFixture fixture(force_poll);
  const fs::path& root = fixture.temp.path();

  // Seed tree: two tracked files, a root .gitignore, and an ignored directory.
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

  // 1. Initial batch: existing tracked files are indexed; gitignored trees are not.
  Expect(fixture.WaitIndexed("a.txt"), "watcher contract: initial batch indexes root files");
  Expect(fixture.IndexHas("sub/b.txt"), "watcher contract: initial batch indexes nested files");
  Expect(fixture.saw_initial, "watcher contract: the first population arrives as is_initial");
  Expect(!fixture.IndexHas("ignored/c.txt"),
         "watcher contract: gitignored directories stay out of the initial batch");

  // 2. Created file is indexed.
  WriteFile(root / "d.txt", "delta\n");
  Expect(fixture.WaitIndexed("d.txt"), "watcher contract: a created file becomes indexed");

  // 3. Modified file's metadata converges (size change is backend-agnostic:
  //    mtime granularity can hide a same-size rewrite from the poll backend).
  const std::string grown = "alpha grew substantially longer\n";
  WriteFile(root / "a.txt", grown);
  Expect(WaitUntil([&] { return fixture.IndexedSize("a.txt") == grown.size(); },
                   std::chrono::seconds(5), std::chrono::milliseconds(5),
                   [&] { fixture.Pump(); }),
         "watcher contract: a modified file's indexed size converges to disk");

  // 4. A new nested directory subtree is picked up (native must extend its
  //    watch set; poll must walk into it).
  fs::create_directories(root / "deep" / "x");
  WriteFile(root / "deep" / "x" / "y.txt", "y\n");
  Expect(fixture.WaitIndexed("deep/x/y.txt"),
         "watcher contract: files in freshly created nested directories are indexed");

  // 5. Rename within the tree: old path leaves the index, new path enters it.
  fs::rename(root / "d.txt", root / "e.txt");
  Expect(fixture.WaitIndexed("e.txt"), "watcher contract: a renamed file appears at its new path");
  Expect(fixture.WaitGone("d.txt"), "watcher contract: a renamed file leaves its old path");

  // 6. Deleted file leaves the index.
  fs::remove(root / "sub" / "b.txt");
  Expect(fixture.WaitGone("sub/b.txt"), "watcher contract: a deleted file leaves the index");

  // 7. Deleting a whole directory removes every file beneath it, however the
  //    backend reports it (recursive directory delete vs per-file diffs).
  fs::remove_all(root / "deep");
  Expect(fixture.WaitGone("deep/x/y.txt"),
         "watcher contract: deleting a directory removes its indexed contents");

  // 8. Churn inside a gitignored directory is never indexed. Ordering trick: the
  //    sentinel write follows the ignored write, so once the sentinel is indexed
  //    the ignored change has had its chance to (wrongly) surface.
  WriteFile(root / "ignored" / "z.txt", "z\n");
  WriteFile(root / "sentinel.txt", "s\n");
  Expect(fixture.WaitIndexed("sentinel.txt"), "watcher contract: sentinel write lands");
  fixture.Pump();
  Expect(!fixture.IndexHas("ignored/z.txt"),
         "watcher contract: gitignored churn stays out of the index");
}

// Startup-window regression: files created WHILE the watcher is coming up must
// still reach the index. The poll fallback used to take two independent tree
// walks — one for the initial batch (on its own thread) and one to seed the diff
// baseline — so a file created between them was missing from the initial batch
// and already present in the baseline, and no later diff would ever mention it.
// Creating a burst of files immediately after Watch() returns lands squarely in
// that window; with a single-walk startup every one of them is either in the
// initial batch or reported by the first diff.
void RunWatcherStartupWindowContract(bool force_poll) {
  WatcherContractFixture fixture(force_poll);
  const fs::path& root = fixture.temp.path();

  // Enough pre-existing files that the startup walk takes real time, widening the
  // window a racing create can land in.
  for (int i = 0; i < 400; ++i) {
    WriteFile(root / ("seed-" + std::to_string(i) + ".txt"), "seed\n");
  }

  Expect(fixture.watcher.Watch(root), "startup-window contract: Watch should start");
  // No wait for the initial batch: create immediately, racing the startup walk.
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

void TestFileIndexWatcherContractNativeBackend() {
  // Runs against the host's preferred backend (native where available; the
  // watcher's own graceful fallback otherwise — the contract holds either way).
  RunWatcherContract(/*force_poll=*/false);
}

void TestFileIndexWatcherContractPollBackend() {
  RunWatcherContract(/*force_poll=*/true);
}

}  // namespace

void RegisterFileIndexWatcherContractTests(std::vector<TestCase>& tests) {
  AddTest(tests, "FileIndexWatcherContract/NativeBackend",
          TestFileIndexWatcherContractNativeBackend);
  AddTest(tests, "FileIndexWatcherContract/PollBackend",
          TestFileIndexWatcherContractPollBackend);
  AddTest(tests, "FileIndexWatcherContract/NativeBackendStartupWindow",
          [] { RunWatcherStartupWindowContract(/*force_poll=*/false); });
  AddTest(tests, "FileIndexWatcherContract/PollBackendStartupWindow",
          [] { RunWatcherStartupWindowContract(/*force_poll=*/true); });
}

}  // namespace microide::tests
