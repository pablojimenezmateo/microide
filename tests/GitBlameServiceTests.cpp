#include "TestSupport.h"

#include "project/GitBlameService.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

namespace microide::tests {
namespace {

using microide::project::GitBlameRequest;
using microide::project::GitBlameService;
using microide::project::ParseGitBlameIncrementalOutput;

std::string EscapedRepoPath(const std::filesystem::path& repo_path) {
  return ShellEscape(repo_path.string());
}

void InitializeGitRepo(const std::filesystem::path& repo_path) {
  const std::string escaped_repo = EscapedRepoPath(repo_path);
  RequireCommandSuccess(
      "git -c init.defaultBranch=main init '" + escaped_repo + "' >/dev/null 2>/dev/null",
      "git init");
  RequireCommandSuccess(
      "git -C '" + escaped_repo + "' config user.name 'Microide Tests' >/dev/null 2>/dev/null",
      "git config user.name");
  RequireCommandSuccess(
      "git -C '" + escaped_repo +
          "' config user.email 'microide-tests@example.com' >/dev/null 2>/dev/null",
      "git config user.email");
}

void CommitAll(const std::filesystem::path& repo_path,
               std::string_view message,
               std::string_view context) {
  const std::string escaped_repo = EscapedRepoPath(repo_path);
  RequireCommandSuccess("git -C '" + escaped_repo + "' add . >/dev/null 2>/dev/null",
                        std::string(context) + " add");
  RequireCommandSuccess(
      "git -C '" + escaped_repo + "' commit -m '" + std::string(message) +
          "' >/dev/null 2>/dev/null",
      std::string(context) + " commit");
}

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
      .large_file_mode = false,
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
  Expect(snapshot.lines[0].text.find("Add blame fixture") != std::string::npos,
         "clean tracked file snapshot should include commit summary text");
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
      .large_file_mode = false,
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
      .large_file_mode = false,
  };
  const auto untracked_snapshot = WaitForSnapshot(service, untracked_request);
  service.Stop();

  Expect(!untracked_snapshot.eligible, "untracked files should not show blame");
  Expect(untracked_snapshot.lines.empty(), "untracked files should not publish blame text");
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
      .large_file_mode = false,
  };

  const auto first_snapshot = WaitForSnapshot(service, request);
  Expect(first_snapshot.eligible && first_snapshot.lines.size() == 1,
         "initial blame snapshot should load the visible line");
  Expect(first_snapshot.lines[0].text.find("Initial blame") != std::string::npos,
         "initial snapshot should reflect the first commit");

  WriteFile(file_path, "int main() {\n  return 2;\n}\n");
  CommitAll(repo_path, "Change return value", "change return value");
  service.InvalidatePath(repo_path, file_path);

  const auto second_snapshot = WaitForSnapshot(service, request);
  service.Stop();

  Expect(second_snapshot.eligible && second_snapshot.lines.size() == 1,
         "blame snapshot after invalidation should reload");
  Expect(second_snapshot.lines[0].text.find("Change return value") != std::string::npos,
         "invalidated blame snapshot should reflect the new commit");
}

}  // namespace

void RegisterGitBlameServiceTests(std::vector<TestCase>& tests) {
  AddTest(tests, "GitBlame/IncrementalParserHandlesRepeatedCommitsAndBoundary",
          TestGitBlameIncrementalParserHandlesRepeatedCommitsAndBoundary);
  AddTest(tests, "GitBlame/LoadsVisibleLinesForCleanTrackedFile",
          TestGitBlameServiceLoadsVisibleLinesForCleanTrackedFile);
  AddTest(tests, "GitBlame/SuppressesDirtyAndUntrackedFiles",
          TestGitBlameServiceSuppressesDirtyAndUntrackedFiles);
  AddTest(tests, "GitBlame/InvalidateDropsStaleCache",
          TestGitBlameServiceInvalidateDropsStaleCache);
}

}  // namespace microide::tests
