#include "TestSupport.h"

#include "workspace/ReviewSessionCoordinator.h"
#include "workspace/WorkspaceShellTestAccess.h"

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace microide::tests {
namespace {

using microide::workspace::TabEntry;
using microide::workspace::WorkspaceShell;
using TestAccess = microide::workspace::WorkspaceShell::TestAccess;

std::size_t CountTabsOfKind(const WorkspaceShell& shell, TabEntry::Kind kind) {
  std::size_t count = 0;
  for (const TabEntry& tab : TestAccess::OpenTabs(shell)) {
    if (tab.kind == kind) {
      ++count;
    }
  }
  return count;
}

bool HasMergeTabFor(const WorkspaceShell& shell, const std::filesystem::path& absolute) {
  const std::filesystem::path normalized = absolute.lexically_normal();
  for (const TabEntry& tab : TestAccess::OpenTabs(shell)) {
    if (tab.kind == TabEntry::Kind::Merge && tab.merge.has_value() &&
        tab.merge->output_path == normalized) {
      return true;
    }
  }
  return false;
}

bool HasCompareTabFor(const WorkspaceShell& shell, const std::filesystem::path& absolute) {
  const std::filesystem::path normalized = absolute.lexically_normal();
  for (const TabEntry& tab : TestAccess::OpenTabs(shell)) {
    if (tab.kind == TabEntry::Kind::Compare && tab.compare.has_value() &&
        tab.compare->path == normalized) {
      return true;
    }
  }
  return false;
}

// review-commit opens one compare tab per file changed by the last commit, and a
// rerun reuses them rather than opening duplicates.
void TestReviewCommitOpensCompareTabsPerChangedFile() {
  TemporaryDirectory temp_dir;
  const auto repo_path = temp_dir.path() / "repo";
  WriteFile(repo_path / "a.txt", "a1\n");
  WriteFile(repo_path / "b.txt", "b1\n");
  InitializeGitRepo(repo_path);
  CommitAll(repo_path, "first", "first commit");

  WriteFile(repo_path / "a.txt", "a2\n");        // modify a
  WriteFile(repo_path / "c.txt", "c1\n");        // add c
  CommitAll(repo_path, "second", "second commit");

  WorkspaceShell shell;
  TestAccess::SetProjectRoot(shell, repo_path);

  Expect(TestAccess::ExecuteCommandLine(shell, "review-commit"), "review-commit should succeed");
  Expect(CountTabsOfKind(shell, TabEntry::Kind::Compare) == 2,
         "review-commit opens a compare tab for each file in the last commit");
  Expect(HasCompareTabFor(shell, repo_path / "a.txt"), "modified file gets a compare tab");
  Expect(HasCompareTabFor(shell, repo_path / "c.txt"), "added file gets a compare tab");

  // Rerun: dedup, no duplicate tabs.
  Expect(TestAccess::ExecuteCommandLine(shell, "review-commit"), "review-commit rerun should succeed");
  Expect(CountTabsOfKind(shell, TabEntry::Kind::Compare) == 2,
         "rerunning review-commit reuses the existing compare tabs (no duplicates)");
}

// review-branch opens compare tabs for files differing from a ref, then closes
// stale (clean) review tabs when a file stops differing on a rerun.
void TestReviewBranchOpensAndCleansCompareTabs() {
  TemporaryDirectory temp_dir;
  const auto repo_path = temp_dir.path() / "repo";
  WriteFile(repo_path / "a.txt", "a1\n");
  WriteFile(repo_path / "b.txt", "b1\n");
  InitializeGitRepo(repo_path);
  CommitAll(repo_path, "base", "base commit");

  RequireGitCommandSuccess(repo_path, {"checkout", "-b", "feature"}, "create feature branch");
  WriteFile(repo_path / "a.txt", "a2\n");
  WriteFile(repo_path / "b.txt", "b2\n");
  CommitAll(repo_path, "feature edits", "feature commit");
  RequireGitCommandSuccess(repo_path, {"checkout", "main"}, "back to main");

  WorkspaceShell shell;
  TestAccess::SetProjectRoot(shell, repo_path);

  Expect(TestAccess::ExecuteCommandLine(shell, "review-branch feature"),
         "review-branch should succeed");
  Expect(CountTabsOfKind(shell, TabEntry::Kind::Compare) == 2,
         "review-branch opens a compare tab per differing file");
  Expect(HasCompareTabFor(shell, repo_path / "a.txt"), "a.txt differs from feature");
  Expect(HasCompareTabFor(shell, repo_path / "b.txt"), "b.txt differs from feature");

  // Make b.txt match feature's content; it no longer differs.
  WriteFile(repo_path / "b.txt", "b2\n");
  Expect(TestAccess::ExecuteCommandLine(shell, "review-branch feature"),
         "review-branch rerun should succeed");
  Expect(CountTabsOfKind(shell, TabEntry::Kind::Compare) == 1,
         "the no-longer-differing file's stale compare tab is closed on rerun");
  Expect(HasCompareTabFor(shell, repo_path / "a.txt"), "still-differing file's tab is kept");
  Expect(!HasCompareTabFor(shell, repo_path / "b.txt"), "stale compare tab was cleaned up");
}

// review-conflicts opens one merge tab per conflicted working-tree file, and a
// rerun reuses them.
void TestReviewConflictsOpensMergeTabsPerConflict() {
  TemporaryDirectory temp_dir;
  const auto repo_path = temp_dir.path() / "repo";
  WriteFile(repo_path / "conflict.txt", "base\n");
  InitializeGitRepo(repo_path);
  CommitAll(repo_path, "base", "base commit");

  RequireGitCommandSuccess(repo_path, {"checkout", "-b", "theirs"}, "create theirs branch");
  WriteFile(repo_path / "conflict.txt", "theirs change\n");
  CommitAll(repo_path, "theirs", "theirs commit");

  RequireGitCommandSuccess(repo_path, {"checkout", "main"}, "back to main");
  WriteFile(repo_path / "conflict.txt", "ours change\n");
  CommitAll(repo_path, "ours", "ours commit");

  // Conflicting merge; non-zero exit is expected (the merge leaves a conflict).
  RunGitCommand(repo_path, {"merge", "theirs"});

  WorkspaceShell shell;
  TestAccess::SetProjectRoot(shell, repo_path);

  Expect(TestAccess::ExecuteCommandLine(shell, "review-conflicts"),
         "review-conflicts should succeed");
  Expect(CountTabsOfKind(shell, TabEntry::Kind::Merge) == 1,
         "review-conflicts opens a merge tab for the conflicted file");
  Expect(HasMergeTabFor(shell, repo_path / "conflict.txt"), "conflicted file gets a merge tab");

  Expect(TestAccess::ExecuteCommandLine(shell, "review-conflicts"),
         "review-conflicts rerun should succeed");
  Expect(CountTabsOfKind(shell, TabEntry::Kind::Merge) == 1,
         "rerunning review-conflicts reuses the existing merge tab (no duplicates)");
}

// A review verb against a very large changed-file set must not open one tab per
// file: it caps at kMaxReviewSessionOpenTabs and reports the remainder as
// truncated instead of stalling the shell building thousands of compare models.
// TD-2026-07-17A-041.
void TestReviewCommitCapsOpenedTabs() {
  using microide::workspace::ReviewSessionCoordinator;
  const std::size_t cap = ReviewSessionCoordinator::kMaxReviewSessionOpenTabs;
  const std::size_t file_count = cap + 5;

  TemporaryDirectory temp_dir;
  const auto repo_path = temp_dir.path() / "repo";
  WriteFile(repo_path / "seed.txt", "seed\n");
  InitializeGitRepo(repo_path);
  CommitAll(repo_path, "base", "base commit");

  for (std::size_t i = 0; i < file_count; ++i) {
    WriteFile(repo_path / ("f" + std::to_string(i) + ".txt"), "content\n");
  }
  CommitAll(repo_path, "bulk", "bulk add");

  WorkspaceShell shell;
  TestAccess::SetProjectRoot(shell, repo_path);

  Expect(TestAccess::ExecuteCommandLine(shell, "review-commit"),
         "review-commit should succeed even for a huge changed-file set");
  Expect(CountTabsOfKind(shell, TabEntry::Kind::Compare) == cap,
         "review opens at most kMaxReviewSessionOpenTabs compare tabs");
  Expect(TestAccess::CommandFeedbackText(shell).find("not opened") != std::string::npos,
         "the review summary reports the un-opened remainder as truncated");
}

}  // namespace

void RegisterReviewSessionTests(std::vector<TestCase>& tests) {
  AddTest(tests, "ReviewSession/CommitCapsOpenedTabs",
          TestReviewCommitCapsOpenedTabs);
  AddTest(tests, "ReviewSession/CommitOpensCompareTabsPerChangedFile",
          TestReviewCommitOpensCompareTabsPerChangedFile);
  AddTest(tests, "ReviewSession/BranchOpensAndCleansCompareTabs",
          TestReviewBranchOpensAndCleansCompareTabs);
  AddTest(tests, "ReviewSession/ConflictsOpensMergeTabsPerConflict",
          TestReviewConflictsOpensMergeTabsPerConflict);
}

}  // namespace microide::tests
