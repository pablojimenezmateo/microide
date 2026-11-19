#include "TestSupport.h"
#include "TestSupportSdl.h"
#include "support/RetainedRedraw.h"
#include "support/SoftwareCanvas.h"

#include "platform/ProcessLauncher.h"
#include "project/GitRepository.h"
#include "project/GitWorkingTreeSnapshot.h"
#include "workspace/git/ReviewSessionCoordinator.h"
#include "workspace/shell/WorkspaceShellTestAccess.h"

#include <algorithm>
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
  TestAccess::FlushPendingFileReads(shell);  // the review's git work runs off-thread
  Expect(CountTabsOfKind(shell, TabEntry::Kind::Compare) == 2,
         "review-commit opens a compare tab for each file in the last commit");
  Expect(HasCompareTabFor(shell, repo_path / "a.txt"), "modified file gets a compare tab");
  Expect(HasCompareTabFor(shell, repo_path / "c.txt"), "added file gets a compare tab");

  // Rerun: dedup, no duplicate tabs.
  Expect(TestAccess::ExecuteCommandLine(shell, "review-commit"), "review-commit rerun should succeed");
  TestAccess::FlushPendingFileReads(shell);  // the review's git work runs off-thread
  Expect(CountTabsOfKind(shell, TabEntry::Kind::Compare) == 2,
         "rerunning review-commit reuses the existing compare tabs (no duplicates)");
}

// QA diff-review-files: a review opened one tab per file and left the LAST one
// active, so the reviewer started at the end and Next File had nowhere to go.
// It opens on its first file, as VS Code's commit changes do, and Next/Previous
// File step through the rest in order.
void TestReviewCommitOpensOnItsFirstFile() {
  TemporaryDirectory temp_dir;
  const auto repo_path = temp_dir.path() / "repo";
  WriteFile(repo_path / "a.txt", "a1\n");
  WriteFile(repo_path / "b.txt", "b1\n");
  WriteFile(repo_path / "c.txt", "c1\n");
  InitializeGitRepo(repo_path);
  CommitAll(repo_path, "first", "first commit");
  WriteFile(repo_path / "a.txt", "a2\n");
  WriteFile(repo_path / "b.txt", "b2\n");
  WriteFile(repo_path / "c.txt", "c2\n");
  CommitAll(repo_path, "second", "second commit");

  WorkspaceShell shell;
  TestAccess::SetProjectRoot(shell, repo_path);
  Expect(TestAccess::ExecuteCommandLine(shell, "review-commit"), "review-commit should succeed");
  TestAccess::FlushPendingFileReads(shell);
  const auto* active = TestAccess::ActiveCompareOrNull(shell);
  Expect(active != nullptr && active->path == (repo_path / "a.txt").lexically_normal(),
         "the review opens on its first file");
  Expect(active != nullptr && active->review_file_index == 0, "and that is file 1 of the review");
  Expect(TestAccess::ExecuteCommandLine(shell, "diff-next-file"), "diff-next-file should run");
  active = TestAccess::ActiveCompareOrNull(shell);
  Expect(active != nullptr && active->path == (repo_path / "b.txt").lexically_normal(),
         "Next File goes to the second");
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
  TestAccess::FlushPendingFileReads(shell);  // the review's git work runs off-thread
  Expect(CountTabsOfKind(shell, TabEntry::Kind::Compare) == 2,
         "review-branch opens a compare tab per differing file");
  Expect(HasCompareTabFor(shell, repo_path / "a.txt"), "a.txt differs from feature");
  Expect(HasCompareTabFor(shell, repo_path / "b.txt"), "b.txt differs from feature");

  // Make b.txt match feature's content; it no longer differs.
  WriteFile(repo_path / "b.txt", "b2\n");
  Expect(TestAccess::ExecuteCommandLine(shell, "review-branch feature"),
         "review-branch rerun should succeed");
  TestAccess::FlushPendingFileReads(shell);  // the review's git work runs off-thread
  Expect(CountTabsOfKind(shell, TabEntry::Kind::Compare) == 1,
         "the no-longer-differing file's stale compare tab is closed on rerun");
  Expect(HasCompareTabFor(shell, repo_path / "a.txt"), "still-differing file's tab is kept");
  Expect(!HasCompareTabFor(shell, repo_path / "b.txt"), "stale compare tab was cleaned up");
}

// TD-2026-10-07-323: a review's git work -- the changed-file list and the bulk
// blob read -- runs on the file reader, not the shell thread; the tabs and the
// summary land with the completion.
void TestReviewRunsItsGitOffTheShellThread() {
  TemporaryDirectory temp_dir;
  const auto repo_path = temp_dir.path() / "repo";
  WriteFile(repo_path / "a.txt", "a1\n");
  InitializeGitRepo(repo_path);
  CommitAll(repo_path, "first", "first commit");
  WriteFile(repo_path / "a.txt", "a2\n");
  CommitAll(repo_path, "second", "second commit");

  WorkspaceShell shell;
  TestAccess::SetProjectRoot(shell, repo_path);
  const std::uint64_t posted_before = TestAccess::PostedFileReadCount(shell);
  Expect(TestAccess::ExecuteCommandLine(shell, "review-commit"), "the command is accepted");
  Expect(TestAccess::PostedFileReadCount(shell) == posted_before + 1 &&
             CountTabsOfKind(shell, TabEntry::Kind::Compare) == 0,
         "the git work is posted to the reader; no tab is built on the shell thread yet");
  TestAccess::FlushPendingFileReads(shell);
  Expect(CountTabsOfKind(shell, TabEntry::Kind::Compare) == 1,
         "the compare tab opens when the git work lands");
  Expect(TestAccess::CommandFeedbackText(shell).find("review-commit HEAD: opened 1") !=
             std::string::npos,
         "and the summary is reported then: " + TestAccess::CommandFeedbackText(shell));
}

// Opening one conflicted file (the git sidebar's click) reads its three index
// stages on the file reader, not the shell thread: three `git show` runs are three
// round trips in a remote project, with the window frozen for all of them
// (TD-2026-09-29-312). The tab appears when the stages are in hand.
void TestConflictMergeOpensOffTheShellThread() {
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
  RunGitCommand(repo_path, {"merge", "theirs"});

  WorkspaceShell shell;
  TestAccess::SetProjectRoot(shell, repo_path);
  const std::uint64_t posted_before = TestAccess::PostedFileReadCount(shell);
  Expect(TestAccess::OpenGitConflictMerge(shell, repo_path / "conflict.txt"),
         "the open is accepted");
  Expect(TestAccess::PostedFileReadCount(shell) == posted_before + 1 &&
             !TestAccess::ActiveTabIsMerge(shell),
         "the stages are read on the file reader; nothing is built on the shell thread yet");
  TestAccess::FlushPendingFileReads(shell);
  Expect(TestAccess::ActiveTabIsMerge(shell), "the merge tab opens when the stages arrive");
  Expect(!TestAccess::ActiveMerge(shell).conflicts.empty(),
         "and it holds the conflict between ours and theirs");
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
  TestAccess::FlushPendingFileReads(shell);  // the review's git work runs off-thread
  Expect(CountTabsOfKind(shell, TabEntry::Kind::Merge) == 1,
         "review-conflicts opens a merge tab for the conflicted file");
  Expect(HasMergeTabFor(shell, repo_path / "conflict.txt"), "conflicted file gets a merge tab");

  Expect(TestAccess::ExecuteCommandLine(shell, "review-conflicts"),
         "review-conflicts rerun should succeed");
  TestAccess::FlushPendingFileReads(shell);  // the review's git work runs off-thread
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
  TestAccess::FlushPendingFileReads(shell);  // the review's git work runs off-thread
  Expect(CountTabsOfKind(shell, TabEntry::Kind::Compare) == cap,
         "review opens at most kMaxReviewSessionOpenTabs compare tabs");
  Expect(TestAccess::CommandFeedbackText(shell).find("not opened") != std::string::npos,
         "the review summary reports the un-opened remainder as truncated");
}

// QA flow compare-merge: a merge result goes stale (or saved) asynchronously --
// a git refresh, the file watcher -- and the merge header's status read the old
// state, because whatever damage brought the change never covered the header.
void TestMergeHeaderRepaintsWhenTheResultGoesStale() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  EnsureDummySdlVideoInitialized();
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
  RunGitCommand(repo_path, {"merge", "theirs"});

  constexpr int kWidth = 1280;
  constexpr int kHeight = 720;
  WorkspaceShell shell;
  TestAccess::SetProjectRoot(shell, repo_path);
  TestAccess::SetWindowSize(shell, kWidth, kHeight);
  Expect(TestAccess::OpenGitConflictMerge(shell, repo_path / "conflict.txt"), "the open is accepted");
  TestAccess::FlushPendingFileReads(shell);
  Expect(TestAccess::ActiveTabIsMerge(shell), "the merge tab opens");
  TestAccess::NoteLayoutInputsChanged(shell);
  SoftwareCanvas canvas(kWidth, kHeight);
  const auto pin = [&]() { TestAccess::SetCaretBlinkEpochMs(shell, SDL_GetTicks()); };
  pin();
  shell.Render(canvas.renderer(), kWidth, kHeight);
  (void)shell.ConsumePendingRenderInvalidation();

  // The change arrives with some unrelated damage (here: the menu bar strip).
  TestAccess::ActiveMerge(shell).index_stale = true;
  workspace::RenderInvalidation unrelated;
  unrelated.rects.push_back(SDL_FRect{0.0f, 0.0f, static_cast<float>(kWidth), 20.0f});
  const auto diff = PaintAndDiffWithFullRepaint(shell, canvas, kWidth, kHeight, unrelated, pin);
  Expect(diff.identical(), "the merge header should repaint its new status; " + DescribePixelDiff(diff));
}

}  // namespace

// Every review compare tab carries the file list the next/previous-review-file
// jump walks, and its own position in that list. The position used to be computed
// by looking for the tab's ABSOLUTE path in a list of PROJECT-RELATIVE ones, which
// never matched: every tab opened at index 0, so "next review file" went to the
// second file of the review no matter which one you were looking at.
void TestReviewCommitTabsCarryTheirOwnReviewFileIndex() {
  TemporaryDirectory temp_dir;
  const auto repo_path = temp_dir.path() / "repo";
  WriteFile(repo_path / "a.txt", "a1\n");
  WriteFile(repo_path / "b.txt", "b1\n");
  WriteFile(repo_path / "c.txt", "c1\n");
  InitializeGitRepo(repo_path);
  CommitAll(repo_path, "first", "first commit");

  WriteFile(repo_path / "a.txt", "a2\n");
  WriteFile(repo_path / "b.txt", "b2\n");
  WriteFile(repo_path / "c.txt", "c2\n");
  CommitAll(repo_path, "second", "second commit");

  WorkspaceShell shell;
  TestAccess::SetProjectRoot(shell, repo_path);
  Expect(TestAccess::ExecuteCommandLine(shell, "review-commit"), "review-commit should succeed");
  TestAccess::FlushPendingFileReads(shell);  // the review's git work runs off-thread

  std::size_t compare_tabs = 0;
  std::vector<std::size_t> indices;
  for (const TabEntry& tab : TestAccess::OpenTabs(shell)) {
    if (tab.kind != TabEntry::Kind::Compare || !tab.compare.has_value()) {
      continue;
    }
    ++compare_tabs;
    // The review list is the COMMIT's files — not `<commit>~1...HEAD`, which for a
    // historical commit is everything committed since.
    Expect(tab.compare->review_files.size() == 3,
           "each review tab lists exactly the commit's changed files");
    Expect(tab.compare->review_file_index < tab.compare->review_files.size(),
           "the review index must address the review list");
    const std::filesystem::path listed =
        (repo_path / tab.compare->review_files[tab.compare->review_file_index]).lexically_normal();
    Expect(listed == tab.compare->path,
           "a tab's review index must point at that tab's own file");
    indices.push_back(tab.compare->review_file_index);
  }
  Expect(compare_tabs == 3, "the fixture opens three compare tabs");
  std::sort(indices.begin(), indices.end());
  Expect(indices == std::vector<std::size_t>({0, 1, 2}),
         "the three tabs occupy three distinct positions, not all of them position 0");
}

// Git: Switch Worktree…: the pick opens loading, git lists the repository's
// working trees off the shell thread, and choosing one opens it as a project --
// where a parallel agent's work is one command away from the tree it is in.
void TestWorktreePickListsTheTreesAndOpensTheChosenOne() {
  TemporaryDirectory temp_dir;
  const auto repo_path = temp_dir.path() / "repo";
  const auto agent_path = temp_dir.path() / "repo-agent";
  WriteFile(repo_path / "a.txt", "a1\n");
  InitializeGitRepo(repo_path);
  CommitAll(repo_path, "base", "base commit");
  RequireGitCommandSuccess(repo_path, {"worktree", "add", "-b", "agent", agent_path.string()},
                           "add a linked working tree");

  WorkspaceShell shell;
  TestAccess::SetProjectRoot(shell, repo_path);
  Expect(TestAccess::ExecuteCommandLine(shell, "git-worktrees"), "git-worktrees is accepted");
  Expect(TestAccess::QuickPick(shell).loading && TestAccess::QuickPickMatchLabels(shell).empty(),
         "the pick opens before git has answered");
  TestAccess::FlushPendingFileReads(shell);  // git's answer arrives off-thread
  const std::vector<std::string> labels = TestAccess::QuickPickMatchLabels(shell);
  Expect(labels == std::vector<std::string>({"repo", "repo-agent"}),
         "both working trees are listed, the main one first: " + std::to_string(labels.size()));
  if (labels.size() != 2) {
    return;
  }
  Expect(TestAccess::QuickPick(shell).matches[0].secondary_label.ends_with("this project") &&
             TestAccess::QuickPick(shell).matches[1].secondary_label.starts_with("agent"),
         "each says its branch, and the open one that it is open");

  TestAccess::SelectAndConfirmLaunchConfig(shell, 1);  // Enter on the second row
  Expect(TestAccess::ProjectRoot(shell) == agent_path.lexically_normal(),
         "choosing a tree opens it as the project: " + TestAccess::ProjectRoot(shell).string());
}

// "Since I last looked": review-checkpoint-mark records the working tree as it
// is -- untracked files included -- and review-checkpoint opens one compare tab
// per file that differs from it now, whatever was committed in between. A file
// that was untracked then and is unchanged is NOT listed, and one created since
// is: the two answers a diff against a commit gets wrong. Nothing in the index or
// the tree is touched.
void TestReviewCheckpointShowsWhatChangedSinceItWasMarked() {
  TemporaryDirectory temp_dir;
  const auto repo_path = temp_dir.path() / "repo";
  WriteFile(repo_path / "committed.txt", "c1\n");
  WriteFile(repo_path / "edited.txt", "e1\n");
  InitializeGitRepo(repo_path);
  CommitAll(repo_path, "base", "base commit");
  WriteFile(repo_path / "untracked-same.txt", "same\n");
  WriteFile(repo_path / "untracked-edited.txt", "u1\n");
  // A file git cannot read is left out of the snapshot: `git add` stopped at it,
  // and one such file anywhere in the tree failed every checkpoint.
  WriteFile(repo_path / "unreadable.bin", "no permission\n");
  std::filesystem::permissions(repo_path / "unreadable.bin", std::filesystem::perms::none);

  WorkspaceShell shell;
  TestAccess::SetProjectRoot(shell, repo_path);
  Expect(TestAccess::ExecuteCommandLine(shell, "review-checkpoint"), "the command is accepted");
  TestAccess::FlushPendingFileReads(shell);
  Expect(TestAccess::CommandFeedbackText(shell).find("no checkpoint marked") != std::string::npos,
         "with no checkpoint it says so: " + TestAccess::CommandFeedbackText(shell));

  Expect(TestAccess::ExecuteCommandLine(shell, "review-checkpoint-mark"), "marking is accepted");
  TestAccess::FlushPendingFileReads(shell);
  Expect(TestAccess::CommandFeedbackText(shell).find("Review checkpoint marked") != std::string::npos,
         "and reports the mark: " + TestAccess::CommandFeedbackText(shell));
  Expect(microide::project::ResolveGitReviewCheckpoint(repo_path, microide::platform::LocalProcessLauncher())
             .has_value(),
         "the checkpoint is a ref of this working tree");

  Expect(TestAccess::ExecuteCommandLine(shell, "review-checkpoint"), "a review straight after the mark");
  TestAccess::FlushPendingFileReads(shell);
  Expect(CountTabsOfKind(shell, TabEntry::Kind::Compare) == 0 &&
             TestAccess::CommandFeedbackText(shell).find("no changes since the checkpoint") != std::string::npos,
         "finds nothing changed: " + TestAccess::CommandFeedbackText(shell));

  // An agent's afternoon: a commit, an uncommitted edit, a new file, an edit to a
  // file git never tracked.
  WriteFile(repo_path / "committed.txt", "c2\n");
  RequireGitCommandSuccess(repo_path, {"commit", "-m", "agent", "--", "committed.txt"}, "the agent's commit");
  WriteFile(repo_path / "edited.txt", "e2\n");
  WriteFile(repo_path / "created.txt", "new\n");
  WriteFile(repo_path / "untracked-edited.txt", "u2\n");

  Expect(TestAccess::ExecuteCommandLine(shell, "review-checkpoint"), "the review is accepted");
  TestAccess::FlushPendingFileReads(shell);
  Expect(HasCompareTabFor(shell, repo_path / "committed.txt"), "a change that was committed since");
  Expect(HasCompareTabFor(shell, repo_path / "edited.txt"), "an uncommitted edit");
  Expect(HasCompareTabFor(shell, repo_path / "created.txt"), "a file created since, never tracked");
  Expect(HasCompareTabFor(shell, repo_path / "untracked-edited.txt"), "an edit to an untracked file");
  Expect(!HasCompareTabFor(shell, repo_path / "untracked-same.txt") &&
             CountTabsOfKind(shell, TabEntry::Kind::Compare) == 4,
         "and not the untracked file that did not change");

  const std::string status =
      microide::project::GitRepository(repo_path, microide::platform::LocalProcessLauncher())
          .Execute({"status", "--porcelain"})
          .output;
  Expect(status.find("?? created.txt") != std::string::npos && status.find("?? untracked-same.txt") != std::string::npos &&
             status.find("?? untracked-edited.txt") != std::string::npos &&
             status.find(" M edited.txt") != std::string::npos,
         "the repository's own index is as it was: untracked files are still untracked\n" + status);
}

// What a review found goes back to the agent that wrote the code: the notes of
// the review the active tab belongs to -- every file's, not just this tab's --
// are copied as `path:line: note` lines, or typed into the running terminal.
void TestReviewNotesAreHandedOverAsReferences() {
  TemporaryDirectory temp_dir;
  const auto repo_path = temp_dir.path() / "repo";
  WriteFile(repo_path / "one.txt", "a\nb\nc\n");
  WriteFile(repo_path / "two.txt", "x\n");
  InitializeGitRepo(repo_path);
  CommitAll(repo_path, "base", "base commit");
  WorkspaceShell shell;
  TestAccess::SetProjectRoot(shell, repo_path);
  std::string copied;
  TestAccess::SetClipboardTextWriter(shell, [&](std::string_view text) {
    copied = std::string(text);
    return true;
  });
  Expect(TestAccess::ExecuteCommandLine(shell, "review-checkpoint-mark"), "a checkpoint is marked");
  TestAccess::FlushPendingFileReads(shell);
  WriteFile(repo_path / "one.txt", "a\nB\nc\n");
  WriteFile(repo_path / "two.txt", "y\n");
  Expect(TestAccess::ExecuteCommandLine(shell, "review-checkpoint"), "the review opens");
  TestAccess::FlushPendingFileReads(shell);
  Expect(CountTabsOfKind(shell, TabEntry::Kind::Compare) == 2, "one tab per changed file");
  Expect(!TestAccess::IsActionEnabled(shell, WorkspaceShell::ActionId::CopyBranchReviewNotes) &&
             !TestAccess::ExecuteCommandLine(shell, "copy-review-notes"),
         "with no notes there is nothing to copy, and the command says so");

  // A note on each file's change, whichever tab is in front first.
  const auto note_on_active_tab = [&](std::string_view text) {
    Expect(TestAccess::ExecuteCommandLine(shell, "branch-review-note " + std::string(text)), "a note is taken");
  };
  std::vector<std::size_t> review_tabs;
  for (std::size_t i = 0; i < TestAccess::OpenTabs(shell).size(); ++i) {
    if (TestAccess::OpenTabs(shell)[i].kind == TabEntry::Kind::Compare) {
      review_tabs.push_back(i);
    }
  }
  if (review_tabs.size() != 2) {
    return;
  }
  TestAccess::ActivateTab(shell, review_tabs[0]);
  note_on_active_tab("first note");
  TestAccess::ActivateTab(shell, review_tabs[1]);
  note_on_active_tab("second note");

  Expect(TestAccess::IsActionEnabled(shell, WorkspaceShell::ActionId::CopyBranchReviewNotes) &&
             TestAccess::ExecuteCommandLine(shell, "copy-review-notes"),
         "Copy Review Notes");
  const bool one_first = copied.find("first note") < copied.find("second note");
  Expect(copied == (one_first ? "one.txt:2: first note\ntwo.txt:1: second note"
                              : "one.txt:2: second note\ntwo.txt:1: first note"),
         "both files' notes, by path, each at its hunk's line:\n" + copied);

  Expect(!TestAccess::IsActionEnabled(shell, WorkspaceShell::ActionId::TerminalInsertBranchReviewNotes) &&
             !TestAccess::ExecuteCommandLine(shell, "term-insert-review-notes"),
         "with no terminal there is nowhere to type them");
}

void RegisterReviewSessionTests(std::vector<TestCase>& tests) {
  AddTest(tests, "ReviewSession/NotesAreHandedOverAsReferences", TestReviewNotesAreHandedOverAsReferences);
  AddTest(tests, "ReviewSession/CheckpointShowsWhatChangedSinceItWasMarked",
          TestReviewCheckpointShowsWhatChangedSinceItWasMarked);
  AddTest(tests, "ReviewSession/WorktreePickListsTheTreesAndOpensTheChosenOne",
          TestWorktreePickListsTheTreesAndOpensTheChosenOne);
  AddTest(tests, "ReviewSession/ReviewRunsItsGitOffTheShellThread",
          TestReviewRunsItsGitOffTheShellThread);
  AddTest(tests, "ReviewSession/ConflictMergeOpensOffTheShellThread",
          TestConflictMergeOpensOffTheShellThread);
  AddTest(tests, "ReviewSession/CommitOpensOnItsFirstFile", TestReviewCommitOpensOnItsFirstFile);
  AddTest(tests, "ReviewSession/CommitCapsOpenedTabs",
          TestReviewCommitCapsOpenedTabs);
  AddTest(tests, "ReviewSession/CommitTabsCarryTheirOwnReviewFileIndex",
          TestReviewCommitTabsCarryTheirOwnReviewFileIndex);
  AddTest(tests, "ReviewSession/CommitOpensCompareTabsPerChangedFile",
          TestReviewCommitOpensCompareTabsPerChangedFile);
  AddTest(tests, "ReviewSession/BranchOpensAndCleansCompareTabs",
          TestReviewBranchOpensAndCleansCompareTabs);
  AddTest(tests, "ReviewSession/MergeHeaderRepaintsWhenTheResultGoesStale",
          TestMergeHeaderRepaintsWhenTheResultGoesStale);
  AddTest(tests, "ReviewSession/ConflictsOpensMergeTabsPerConflict",
          TestReviewConflictsOpensMergeTabsPerConflict);
}

}  // namespace microide::tests
