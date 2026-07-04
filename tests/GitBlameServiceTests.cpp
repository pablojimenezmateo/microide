#include "TestSupport.h"

#include "project/GitBlameService.h"
#include "GitBlameServiceTestAccess.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace microide::tests {
namespace {

using microide::project::GitBlameRequest;
using microide::project::GitBlameService;
using microide::project::ParseGitBlameIncrementalOutput;

microide::project::GitBlameSnapshot WaitForSnapshot(GitBlameService& service,
                                                    const GitBlameRequest& request) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (std::chrono::steady_clock::now() < deadline) {
    service.Request(request);
    const auto snapshot = service.Snapshot(request);
    if (!snapshot.loading) {
      return snapshot;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return service.Snapshot(request);
}

void TestGitBlameIncrementalParserHandlesRepeatedCommitsAndBoundary() {
  const std::string output =
      "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa 1 1 2\n"
      "author Alice\n"
      "author-time 1700000000\n"
      "summary Add alpha\n"
      "boundary\n"
      "filename main.cpp\n"
      "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb 7 3 1\n"
      "author Bob\n"
      "author-time 1701000000\n"
      "summary Rename beta\n"
      "previous cccccccccccccccccccccccccccccccccccccccc old.cpp\n"
      "filename main.cpp\n"
      "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb 8 4 2\n"
      "filename main.cpp\n";

  const auto attributions = ParseGitBlameIncrementalOutput(output);
  Expect(attributions.size() == 3, "incremental parser should emit one attribution per entry");
  Expect(attributions[0].author == "Alice",
         "incremental parser should keep author metadata");
  Expect(attributions[0].line_count == 2,
         "incremental parser should keep grouped line counts");
  Expect(attributions[0].boundary,
         "incremental parser should mark boundary entries");
  Expect(attributions[1].author == "Bob",
         "incremental parser should capture metadata for later commits");
  Expect(attributions[1].summary == "Rename beta",
         "incremental parser should capture commit summaries");
  Expect(attributions[2].author == "Bob",
         "incremental parser should reuse prior commit metadata when it is omitted");
  Expect(attributions[2].line_count == 2,
         "incremental parser should preserve repeated commit line counts");
}

void TestGitBlameIncrementalParserAcceptsSha256ObjectIds() {
  // Regression: a SHA-256 repo (extensions.objectFormat=sha256) emits 64-char
  // object ids in blame-incremental headers. The header detector previously
  // hardcoded 40 hex chars, so every header was rejected and blame produced
  // zero attributions.
  const std::string sha256(64, 'a');
  const std::string output =
      sha256 + " 1 1 2\n"
      "author Alice\n"
      "author-time 1700000000\n"
      "summary Add alpha\n"
      "filename main.cpp\n";

  const auto attributions = ParseGitBlameIncrementalOutput(output);
  Expect(attributions.size() == 1, "SHA-256 blame header should be recognized");
  Expect(attributions[0].commit_id == sha256,
         "SHA-256 commit id should round-trip in full");
  Expect(attributions[0].author == "Alice",
         "SHA-256 entry should carry its author metadata");
  Expect(attributions[0].line_count == 2, "SHA-256 entry should keep its line count");

  // A malformed 50-char hex run (neither 40 nor 64) must still be rejected.
  const std::string bad = std::string(50, 'a') + " 1 1 1\nfilename main.cpp\n";
  Expect(ParseGitBlameIncrementalOutput(bad).empty(),
         "a non-40/64 hex run should not be treated as a commit header");
}

void TestGitBlameServiceLoadsVisibleLinesForCleanTrackedFile() {
  TemporaryDirectory temp_dir;
  const auto repo_path = temp_dir.path() / "repo";
  const auto file_path = repo_path / "src" / "main.cpp";
  WriteFile(file_path, "line 1\nline 2\nline 3\nline 4\n");

  InitializeGitRepo(repo_path);
  CommitAll(repo_path, "Add blame fixture", "blame fixture");

  GitBlameService service;
  const GitBlameRequest request{
      .root = repo_path,
      .absolute_path = file_path,
      .visible_start_line = 1,
      .visible_line_count = 2,
      .total_line_count = 4,
      .dirty = false,
  };

  const auto snapshot = WaitForSnapshot(service, request);
  service.Stop();

  Expect(snapshot.eligible, "clean tracked file should be blame-eligible");
  Expect(!snapshot.loading, "clean tracked file snapshot should finish loading");
  Expect(snapshot.lines.size() == 2,
         "clean tracked file snapshot should include every visible line");
  Expect(snapshot.lines[0].line == 1 && snapshot.lines[1].line == 2,
         "clean tracked file snapshot should keep visible line indices");
  Expect(snapshot.lines[0].text.find("Microide Tests") != std::string::npos,
         "clean tracked file snapshot should include author text");
  Expect(snapshot.lines[0].summary == "Add blame fixture",
         "clean tracked file snapshot should keep the commit summary metadata");
  Expect(!snapshot.lines[0].commit_id.empty(),
         "clean tracked file snapshot should keep the full commit id");
}

void TestGitBlameServiceSuppressesDirtyAndUntrackedFiles() {
  TemporaryDirectory temp_dir;
  const auto repo_path = temp_dir.path() / "repo";
  const auto tracked_file = repo_path / "tracked.cpp";
  const auto untracked_file = repo_path / "scratch.cpp";
  WriteFile(tracked_file, "tracked\n");

  InitializeGitRepo(repo_path);
  CommitAll(repo_path, "Add tracked file", "tracked file");
  WriteFile(untracked_file, "scratch\n");

  GitBlameService service;
  const GitBlameRequest dirty_request{
      .root = repo_path,
      .absolute_path = tracked_file,
      .visible_start_line = 0,
      .visible_line_count = 1,
      .total_line_count = 1,
      .dirty = true,
  };
  const auto dirty_snapshot = service.Snapshot(dirty_request);
  Expect(!dirty_snapshot.eligible && !dirty_snapshot.loading,
         "dirty buffers should be suppressed before any blame request runs");

  const GitBlameRequest untracked_request{
      .root = repo_path,
      .absolute_path = untracked_file,
      .visible_start_line = 0,
      .visible_line_count = 1,
      .total_line_count = 1,
      .dirty = false,
  };
  const auto untracked_snapshot = WaitForSnapshot(service, untracked_request);
  service.Stop();

  Expect(!untracked_snapshot.eligible, "untracked files should not show blame");
  Expect(untracked_snapshot.lines.empty(), "untracked files should not publish blame text");
}

void TestGitBlameServiceUsesWorkingTreeContentsForSavedTrackedChanges() {
  TemporaryDirectory temp_dir;
  const auto repo_path = temp_dir.path() / "repo";
  const auto file_path = repo_path / "main.cpp";
  WriteFile(file_path, "one\ntwo\nthree\n");

  InitializeGitRepo(repo_path);
  CommitAll(repo_path, "Initial blame", "initial blame");
  WriteFile(file_path, "one\nTWO\nthree\n");

  GitBlameService service;
  const GitBlameRequest request{
      .root = repo_path,
      .absolute_path = file_path,
      .visible_start_line = 0,
      .visible_line_count = 3,
      .total_line_count = 3,
      .dirty = false,
  };

  const auto snapshot = WaitForSnapshot(service, request);
  service.Stop();

  Expect(snapshot.eligible, "saved tracked working-tree changes should still be blame-eligible");
  Expect(snapshot.lines.size() == 3,
         "saved tracked working-tree changes should publish the whole visible range");
  Expect(snapshot.lines[0].text.find("Microide Tests") != std::string::npos,
         "unchanged saved lines should keep normal commit attribution");
  Expect(snapshot.lines[1].text == "Saved changes",
         "modified saved lines should use the synthetic saved-changes label");
  Expect(snapshot.lines[1].synthetic && snapshot.lines[1].commit_id.empty(),
         "modified saved lines should not expose synthetic blame ids as normal commits");
  Expect(snapshot.lines[2].text.find("Microide Tests") != std::string::npos,
         "later unchanged saved lines should still keep commit attribution");
}

void TestGitBlameServiceHandlesQuotedAndSpacedPaths() {
  TemporaryDirectory temp_dir;
  const auto repo_path = temp_dir.path() / "repo";
  const auto file_path = repo_path / "dir with spaces" / "quote's file.cpp";
  WriteFile(file_path, "alpha\nbeta\n");

  InitializeGitRepo(repo_path);
  CommitAll(repo_path, "Add weird blame path", "weird blame path");

  GitBlameService service;
  const GitBlameRequest request{
      .root = repo_path,
      .absolute_path = file_path,
      .visible_start_line = 0,
      .visible_line_count = 2,
      .total_line_count = 2,
      .dirty = false,
  };

  const auto snapshot = WaitForSnapshot(service, request);
  service.Stop();

  Expect(snapshot.eligible, "quoted tracked file should remain blame-eligible");
  Expect(snapshot.lines.size() == 2,
         "quoted tracked file should load visible blame lines");
  Expect(snapshot.lines[0].summary == "Add weird blame path",
         "quoted tracked file should preserve blame commit metadata");
}

void TestGitBlameServiceInvalidateDropsStaleCache() {
  TemporaryDirectory temp_dir;
  const auto repo_path = temp_dir.path() / "repo";
  const auto file_path = repo_path / "main.cpp";
  WriteFile(file_path, "int main() {\n  return 1;\n}\n");

  InitializeGitRepo(repo_path);
  CommitAll(repo_path, "Initial blame", "initial blame");

  GitBlameService service;
  const GitBlameRequest request{
      .root = repo_path,
      .absolute_path = file_path,
      .visible_start_line = 1,
      .visible_line_count = 1,
      .total_line_count = 3,
      .dirty = false,
  };

  const auto first_snapshot = WaitForSnapshot(service, request);
  Expect(first_snapshot.eligible && first_snapshot.lines.size() == 1,
         "initial blame snapshot should load the visible line");
  Expect(first_snapshot.lines[0].summary == "Initial blame",
         "initial snapshot should reflect the first commit metadata");

  WriteFile(file_path, "int main() {\n  return 2;\n}\n");
  CommitAll(repo_path, "Change return value", "change return value");
  service.InvalidatePath(repo_path, file_path);

  const auto second_snapshot = WaitForSnapshot(service, request);
  service.Stop();

  Expect(second_snapshot.eligible && second_snapshot.lines.size() == 1,
         "blame snapshot after invalidation should reload");
  Expect(second_snapshot.lines[0].summary == "Change return value",
         "invalidated blame snapshot should reflect the new commit metadata");
}

void TestGitBlameServiceInvalidateDropsInFlightResults() {
  TemporaryDirectory temp_dir;
  const auto repo_path = temp_dir.path() / "repo";
  const auto file_path = repo_path / "main.cpp";
  WriteFile(file_path, "int main() {\n  return 1;\n}\n");

  InitializeGitRepo(repo_path);
  CommitAll(repo_path, "Initial blame", "initial blame");

  GitBlameService service;
  std::mutex hook_mutex;
  std::condition_variable hook_cv;
  bool hook_entered = false;
  bool release_hook = false;
  bool hook_used = false;
  GitBlameServiceTestAccess::SetBeforeCacheApplyHook(service, [&]() {
    std::unique_lock lock(hook_mutex);
    if (hook_used) {
      return;
    }
    hook_used = true;
    hook_entered = true;
    hook_cv.notify_all();
    hook_cv.wait(lock, [&]() { return release_hook; });
  });

  const GitBlameRequest request{
      .root = repo_path,
      .absolute_path = file_path,
      .visible_start_line = 1,
      .visible_line_count = 1,
      .total_line_count = 3,
      .dirty = false,
  };

  service.Request(request);
  {
    std::unique_lock lock(hook_mutex);
    const bool entered = hook_cv.wait_for(lock, std::chrono::seconds(2),
                                          [&]() { return hook_entered; });
    Expect(entered, "test hook should observe the in-flight blame request");
  }

  WriteFile(file_path, "int main() {\n  return 2;\n}\n");
  CommitAll(repo_path, "Change return value", "change return value");
  service.InvalidatePath(repo_path, file_path);

  {
    std::lock_guard lock(hook_mutex);
    release_hook = true;
  }
  hook_cv.notify_all();

  const auto stale_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  microide::project::GitBlameSnapshot stale_snapshot;
  while (std::chrono::steady_clock::now() < stale_deadline) {
    stale_snapshot = service.Snapshot(request);
    if (!stale_snapshot.loading) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  Expect(stale_snapshot.lines.empty(),
         "invalidating an in-flight blame request should not repopulate stale blame lines");

  const auto refreshed_snapshot = WaitForSnapshot(service, request);
  service.Stop();

  Expect(refreshed_snapshot.eligible && refreshed_snapshot.lines.size() == 1,
         "requesting after invalidation should still reload blame");
  Expect(refreshed_snapshot.lines[0].summary == "Change return value",
         "reloaded blame after invalidation should reflect the newer commit");
}

void TestGitBlameServiceClearDropsInFlightResults() {
  TemporaryDirectory temp_dir;
  const auto repo_path = temp_dir.path() / "repo";
  const auto file_path = repo_path / "main.cpp";
  WriteFile(file_path, "int main() {\n  return 1;\n}\n");

  InitializeGitRepo(repo_path);
  CommitAll(repo_path, "Initial blame", "initial blame");

  GitBlameService service;
  std::mutex hook_mutex;
  std::condition_variable hook_cv;
  bool hook_entered = false;
  bool release_hook = false;
  bool hook_used = false;
  GitBlameServiceTestAccess::SetBeforeCacheApplyHook(service, [&]() {
    std::unique_lock lock(hook_mutex);
    if (hook_used) {
      return;
    }
    hook_used = true;
    hook_entered = true;
    hook_cv.notify_all();
    hook_cv.wait(lock, [&]() { return release_hook; });
  });

  const GitBlameRequest request{
      .root = repo_path,
      .absolute_path = file_path,
      .visible_start_line = 1,
      .visible_line_count = 1,
      .total_line_count = 3,
      .dirty = false,
  };

  service.Request(request);
  {
    std::unique_lock lock(hook_mutex);
    const bool entered = hook_cv.wait_for(lock, std::chrono::seconds(2),
                                          [&]() { return hook_entered; });
    Expect(entered, "test hook should observe the in-flight blame request before clear");
  }

  service.Clear();

  {
    std::lock_guard lock(hook_mutex);
    release_hook = true;
  }
  hook_cv.notify_all();

  const auto stale_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  microide::project::GitBlameSnapshot stale_snapshot;
  while (std::chrono::steady_clock::now() < stale_deadline) {
    stale_snapshot = service.Snapshot(request);
    if (!stale_snapshot.loading) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  Expect(stale_snapshot.lines.empty(),
         "clearing during an in-flight blame request should drop the stale result");

  const auto reloaded_snapshot = WaitForSnapshot(service, request);
  service.Stop();

  Expect(reloaded_snapshot.eligible && reloaded_snapshot.lines.size() == 1,
         "requesting after clear should reload blame");
  Expect(reloaded_snapshot.lines[0].summary == "Initial blame",
         "reloaded blame after clear should still reflect the current commit");
}

void TestGitBlameServiceLatestRequestSupersedesPendingWindow() {
  TemporaryDirectory temp_dir;
  const auto repo_path = temp_dir.path() / "repo";
  const auto file_path = repo_path / "main.cpp";

  std::ostringstream content;
  for (int i = 0; i < 600; ++i) {
    content << "line " << i << '\n';
  }
  WriteFile(file_path, content.str());

  InitializeGitRepo(repo_path);
  CommitAll(repo_path, "Initial blame", "initial blame");

  GitBlameService service;
  std::mutex hook_mutex;
  std::condition_variable hook_cv;
  bool hook_entered = false;
  bool release_hook = false;
  bool hook_used = false;
  GitBlameServiceTestAccess::SetBeforeCacheApplyHook(service, [&]() {
    std::unique_lock lock(hook_mutex);
    if (hook_used) {
      return;
    }
    hook_used = true;
    hook_entered = true;
    hook_cv.notify_all();
    hook_cv.wait(lock, [&]() { return release_hook; });
  });

  const GitBlameRequest early_request{
      .root = repo_path,
      .absolute_path = file_path,
      .visible_start_line = 0,
      .visible_line_count = 1,
      .total_line_count = 600,
      .dirty = false,
  };
  const GitBlameRequest latest_request{
      .root = repo_path,
      .absolute_path = file_path,
      .visible_start_line = 500,
      .visible_line_count = 1,
      .total_line_count = 600,
      .dirty = false,
  };

  service.Request(early_request);
  {
    std::unique_lock lock(hook_mutex);
    const bool entered = hook_cv.wait_for(lock, std::chrono::seconds(2),
                                          [&]() { return hook_entered; });
    Expect(entered, "supersede fixture should observe the first in-flight request");
  }

  service.Request(latest_request);
  {
    std::lock_guard lock(hook_mutex);
    release_hook = true;
  }
  hook_cv.notify_all();

  const auto latest_snapshot = WaitForSnapshot(service, latest_request);
  const auto early_snapshot = service.Snapshot(early_request);
  service.Stop();

  Expect(latest_snapshot.eligible && !latest_snapshot.loading,
         "latest blame request should complete after superseding an older pending window");
  Expect(!latest_snapshot.lines.empty() && latest_snapshot.lines.front().line >= 500,
         "latest blame snapshot should keep the most recent requested window");
  Expect(early_snapshot.lines.empty(),
         "superseded older blame windows should not be applied to the cache");
}

}  // namespace

void RegisterGitBlameServiceTests(std::vector<TestCase>& tests) {
  AddTest(tests, "GitBlame/IncrementalParserAcceptsSha256ObjectIds",
          TestGitBlameIncrementalParserAcceptsSha256ObjectIds);
  AddTest(tests, "GitBlame/IncrementalParserHandlesRepeatedCommitsAndBoundary",
          TestGitBlameIncrementalParserHandlesRepeatedCommitsAndBoundary);
  AddTest(tests, "GitBlame/LoadsVisibleLinesForCleanTrackedFile",
          TestGitBlameServiceLoadsVisibleLinesForCleanTrackedFile);
  AddTest(tests, "GitBlame/SuppressesDirtyAndUntrackedFiles",
          TestGitBlameServiceSuppressesDirtyAndUntrackedFiles);
  AddTest(tests, "GitBlame/UsesWorkingTreeContentsForSavedTrackedChanges",
          TestGitBlameServiceUsesWorkingTreeContentsForSavedTrackedChanges);
  AddTest(tests, "GitBlame/HandlesQuotedAndSpacedPaths",
          TestGitBlameServiceHandlesQuotedAndSpacedPaths);
  AddTest(tests, "GitBlame/InvalidateDropsStaleCache",
          TestGitBlameServiceInvalidateDropsStaleCache);
  AddTest(tests, "GitBlame/InvalidateDropsInFlightResults",
          TestGitBlameServiceInvalidateDropsInFlightResults);
  AddTest(tests, "GitBlame/ClearDropsInFlightResults",
          TestGitBlameServiceClearDropsInFlightResults);
  AddTest(tests, "GitBlame/LatestRequestSupersedesPendingWindow",
          TestGitBlameServiceLatestRequestSupersedesPendingWindow);
}

}  // namespace microide::tests
