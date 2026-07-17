#include "TestSupport.h"

#include "compare/CompareModel.h"
#include "project/GitCompareService.h"
#include "project/GitPorcelainParser.h"
#include "project/GitRepository.h"
#include "project/GitStatusService.h"
#include "workspace/WorkspaceGitOutgoingBase.h"

#include <algorithm>
#include <filesystem>
#include <string>
#include <string_view>

namespace microide::tests {
namespace {

using microide::compare::BuildCompareModel;
using microide::compare::CompareModel;
using microide::compare::CompareRowKind;
using microide::project::CollectGitBranches;
using microide::project::CollectGitBranchOutgoingFiles;
using microide::project::CollectGitFileHistory;
using microide::project::CollectGitRecentCommits;
using microide::project::CollectGitWorkingTreeEntries;
using microide::project::GitDiscardAll;
using microide::project::GitDiscardPath;
using microide::project::GitFileStatus;
using microide::project::GitPorcelainParser;
using microide::project::GitRepository;
using microide::project::GitStageAll;
using microide::project::GitStagePath;
using microide::project::GitUnstagePath;
using microide::project::ReadGitFileAtCommit;
using microide::project::ResolveGitBaseReference;
using microide::workspace::OutgoingBaseChoice;
using microide::workspace::ResolveGitOutgoingBase;

struct CompareSummary {
  int unchanged = 0;
  int added = 0;
  int deleted = 0;
  int modified = 0;
};

CompareSummary Summarize(const CompareModel& model) {
  CompareSummary summary;
  for (const auto& row : model.rows) {
    switch (row.kind) {
      case CompareRowKind::Unchanged:
        ++summary.unchanged;
        break;
      case CompareRowKind::Added:
        ++summary.added;
        break;
      case CompareRowKind::Deleted:
        ++summary.deleted;
        break;
      case CompareRowKind::Modified:
        ++summary.modified;
        break;
    }
  }
  return summary;
}

void TestGitCompareFixture() {
  TemporaryDirectory temp_dir;
  const auto repo_path = temp_dir.path() / "repo";
  const auto base_dir = FixturePath("diff/git/base");
  const auto head_dir = FixturePath("diff/git/head");
  CopyTree(base_dir, repo_path);

  InitializeGitRepo(repo_path);
  CommitAll(repo_path, "base fixture", "base fixture");

  const auto tracked_file = repo_path / "src/session.cpp";
  const auto new_file = repo_path / "src/new_panel.cpp";
  WriteFile(tracked_file, ReadFile(head_dir / "src/session.cpp"));
  WriteFile(new_file, ReadFile(head_dir / "src/new_panel.cpp"));
  CommitAll(repo_path, "head fixture", "head fixture");

  const auto history = CollectGitFileHistory(repo_path, tracked_file).commits;
  Expect(history.size() == 2, "tracked file should have two commits in history");
  Expect(history[0].subject == "head fixture", "newest history entry subject mismatch");
  Expect(history[1].subject == "base fixture", "oldest history entry subject mismatch");

  const auto latest = ReadGitFileAtCommit(repo_path, tracked_file, history[0].hash);
  const auto original = ReadGitFileAtCommit(repo_path, tracked_file, history[1].hash);
  Expect(latest.has_value(), "latest commit read should succeed");
  Expect(original.has_value(), "base commit read should succeed");
  Expect(latest->exists, "latest tracked file should exist");
  Expect(original->exists, "base tracked file should exist");
  Expect(latest->content == ReadFile(head_dir / "src/session.cpp"), "latest content mismatch");
  Expect(original->content == ReadFile(base_dir / "src/session.cpp"), "base content mismatch");

  const auto missing_in_base = ReadGitFileAtCommit(repo_path, new_file, history[1].hash);
  Expect(missing_in_base.has_value(), "missing file at base commit should produce result");
  Expect(!missing_in_base->exists, "new file should not exist in base commit");
  Expect(missing_in_base->content.empty(), "missing file should have empty content");

  const auto compare_model =
      BuildCompareModel(ReadFile(base_dir / "src/session.cpp"), ReadFile(head_dir / "src/session.cpp"));
  const auto compare_summary = Summarize(compare_model);
  Expect(compare_model.hunks.size() == 3, "git session fixture should produce 3 hunks");
  Expect(compare_model.rows.size() == 28,
         "git session fixture should preserve the terminal empty row");
  Expect(compare_summary.unchanged == 19,
         "git session fixture should keep unchanged rows plus the terminal empty row");
  Expect(compare_summary.modified == 1, "git session fixture should produce 1 modified row");
  Expect(compare_summary.added == 7, "git session fixture should produce 7 added rows");
  Expect(compare_summary.deleted == 1, "git session fixture should produce 1 deleted row");
}

void TestGitWorkingTreeStatusAndActions() {
  TemporaryDirectory temp_dir;
  const auto repo_path = temp_dir.path() / "repo";
  const auto base_dir = FixturePath("diff/git/base");
  CopyTree(base_dir, repo_path);

  InitializeGitRepo(repo_path);
  CommitAll(repo_path, "base fixture", "base fixture");

  const auto modified_file = repo_path / "README.md";
  const auto deleted_file = repo_path / "src/session.cpp";
  const auto untracked_file = repo_path / "scratch.txt";
  WriteFile(modified_file, ReadFile(modified_file) + "\nworking tree change\n");
  std::filesystem::remove(deleted_file);
  WriteFile(untracked_file, "untracked content\n");

  auto entries = CollectGitWorkingTreeEntries(repo_path);
  Expect(entries.size() == 3, "working tree fixture should report three changed files");

  bool saw_deleted = false;
  bool saw_modified = false;
  bool saw_untracked = false;
  for (const auto& entry : entries) {
    if (entry.relative_path == std::filesystem::path("README.md")) {
      saw_modified = true;
      Expect(entry.status == GitFileStatus::Modified,
             "modified file should be reported as modified");
      Expect(!entry.staged, "modified file should start unstaged");
    }
    if (entry.relative_path == std::filesystem::path("src/session.cpp")) {
      saw_deleted = true;
      Expect(entry.status == GitFileStatus::Deleted,
             "deleted file should be reported as deleted");
    }
    if (entry.relative_path == std::filesystem::path("scratch.txt")) {
      saw_untracked = true;
      Expect(entry.status == GitFileStatus::Untracked,
             "untracked file should be reported as untracked");
    }
  }
  Expect(saw_modified && saw_deleted && saw_untracked,
         "working tree fixture should include modified, deleted, and untracked files");

  Expect(GitStagePath(repo_path, modified_file), "git stage should succeed for modified file");
  entries = CollectGitWorkingTreeEntries(repo_path);
  const auto staged_it = std::find_if(entries.begin(), entries.end(), [&](const auto& entry) {
    return entry.relative_path == std::filesystem::path("README.md");
  });
  Expect(staged_it != entries.end(),
         "staged modified file should still appear in working tree view");
  Expect(staged_it->staged, "modified file should report staged after git add");

  Expect(GitUnstagePath(repo_path, modified_file), "git unstage should succeed for modified file");
  entries = CollectGitWorkingTreeEntries(repo_path);
  const auto unstaged_it = std::find_if(entries.begin(), entries.end(), [&](const auto& entry) {
    return entry.relative_path == std::filesystem::path("README.md");
  });
  Expect(unstaged_it != entries.end(),
         "unstaged modified file should still appear in working tree view");
  Expect(!unstaged_it->staged, "modified file should report unstaged after git unstage");

  Expect(GitDiscardPath(repo_path, untracked_file), "discard should remove untracked file");
  Expect(!std::filesystem::exists(untracked_file), "discard should delete untracked file");

  Expect(GitDiscardPath(repo_path, deleted_file),
         "discard should restore deleted tracked file");
  Expect(std::filesystem::exists(deleted_file),
         "discard should restore deleted tracked file on disk");

  Expect(GitDiscardPath(repo_path, modified_file),
         "discard should restore modified tracked file");
  Expect(ReadFile(modified_file) == ReadFile(base_dir / "README.md"),
         "discard should restore tracked file to HEAD content");

  // Regression: a single-row discard whose path points at an untracked directory
  // must NOT recursively `git clean -fd` the whole subtree (silent data loss). It
  // refuses and leaves the directory and its contents intact.
  const auto untracked_dir = repo_path / "untracked_pkg";
  std::filesystem::create_directories(untracked_dir);
  WriteFile(untracked_dir / "keep.txt", "precious\n");
  Expect(!GitDiscardPath(repo_path, untracked_dir),
         "discarding an untracked directory row is refused, not a recursive clean");
  Expect(std::filesystem::exists(untracked_dir / "keep.txt"),
         "the untracked directory and its files survive a refused discard");
}

void TestGitOutgoingBranchFiles() {
  TemporaryDirectory temp_dir;
  const auto repo_path = temp_dir.path() / "repo";
  const auto base_dir = FixturePath("diff/git/base");
  const auto head_dir = FixturePath("diff/git/head");
  CopyTree(base_dir, repo_path);

  InitializeGitRepo(repo_path);
  CommitAll(repo_path, "base fixture", "base fixture");
  RequireGitCommandSuccess(repo_path, {"checkout", "-b", "feature/git-view"},
                           "git checkout feature branch");

  WriteFile(repo_path / "src/session.cpp", ReadFile(head_dir / "src/session.cpp"));
  WriteFile(repo_path / "src/new_panel.cpp", ReadFile(head_dir / "src/new_panel.cpp"));
  CommitAll(repo_path, "feature fixture", "feature fixture");

  const auto base_ref = ResolveGitBaseReference(repo_path);
  Expect(base_ref.has_value(), "git base reference should resolve in a main-based repo");
  // TD-2026-07-17A-025: the default-branch fallback keeps the FULL ref as identity
  // (so a same-named tag cannot shadow it in `git diff`), and the short name as label.
  Expect(base_ref->ref == "refs/heads/main",
         "local repo without remotes should resolve the full refs/heads/main as the base ref");
  Expect(base_ref->label == "main", "the base label stays the short branch name");

  const auto outgoing = CollectGitBranchOutgoingFiles(repo_path, base_ref->ref);
  Expect(outgoing.size() == 2, "feature branch should report two outgoing files");

  bool saw_modified = false;
  bool saw_added = false;
  for (const auto& entry : outgoing) {
    if (entry.relative_path == std::filesystem::path("src/session.cpp")) {
      saw_modified = true;
      Expect(entry.status == GitFileStatus::Modified,
             "tracked changed file should be outgoing modified");
    }
    if (entry.relative_path == std::filesystem::path("src/new_panel.cpp")) {
      saw_added = true;
      Expect(entry.status == GitFileStatus::Added, "new file should be outgoing added");
    }
  }
  Expect(saw_modified && saw_added,
         "outgoing branch list should include both changed files");
}

void TestGitOutgoingBaseChoiceResolution() {
  TemporaryDirectory temp_dir;
  const auto repo_path = temp_dir.path() / "repo";
  WriteFile(repo_path / "src" / "alpha.cpp", "int alpha() {\n  return 1;\n}\n");
  WriteFile(repo_path / "src" / "beta.cpp", "int beta() {\n  return 2;\n}\n");

  InitializeGitRepo(repo_path);
  CommitAll(repo_path, "base fixture", "base fixture");
  RequireGitCommandSuccess(repo_path, {"checkout", "-b", "feature/outgoing-base"},
                           "git checkout feature branch");

  WriteFile(repo_path / "src" / "alpha.cpp", "int alpha() {\n  return 10;\n}\n");
  CommitAll(repo_path, "feature alpha", "feature alpha");
  WriteFile(repo_path / "src" / "beta.cpp", "int beta() {\n  return 20;\n}\n");
  CommitAll(repo_path, "feature beta", "feature beta");

  const auto auto_base = ResolveGitOutgoingBase(
      repo_path, OutgoingBaseChoice{.kind = OutgoingBaseChoice::Kind::Auto, .custom_ref = {}}, true);
  Expect(auto_base.repo_available && auto_base.base_ref == "refs/heads/main" &&
             auto_base.base_label == "main",
         "auto outgoing base should resolve the full base ref with the short label");
  const auto auto_outgoing = CollectGitBranchOutgoingFiles(repo_path, auto_base.base_ref);
  Expect(auto_outgoing.size() == 2,
         "auto outgoing base should include both commits ahead of the base branch");

  const auto previous_commit = ResolveGitOutgoingBase(
      repo_path,
      OutgoingBaseChoice{.kind = OutgoingBaseChoice::Kind::PreviousCommit, .custom_ref = {}}, true);
  Expect(previous_commit.repo_available && previous_commit.base_ref == "HEAD~1" &&
             previous_commit.base_label == "HEAD~1",
         "previous-commit outgoing base should map to HEAD~1");
  const auto previous_outgoing =
      CollectGitBranchOutgoingFiles(repo_path, previous_commit.base_ref);
  Expect(previous_outgoing.size() == 1 &&
             previous_outgoing.front().relative_path == std::filesystem::path("src/beta.cpp"),
         "previous-commit outgoing base should limit results to the latest commit delta");

  const auto specific_ref = ResolveGitOutgoingBase(
      repo_path,
      OutgoingBaseChoice{.kind = OutgoingBaseChoice::Kind::SpecificRef, .custom_ref = "HEAD~2"},
      true);
  Expect(specific_ref.repo_available && specific_ref.base_ref == "HEAD~2" &&
             specific_ref.base_label == "HEAD~2",
         "specific-ref outgoing base should preserve the exact ref string");
  const auto specific_outgoing = CollectGitBranchOutgoingFiles(repo_path, specific_ref.base_ref);
  Expect(specific_outgoing.size() == 2,
         "specific-ref outgoing base should pass the custom ref through unchanged");
}

void TestGitBranchAndRecentCommitCollection() {
  TemporaryDirectory temp_dir;
  const auto repo_path = temp_dir.path() / "repo";
  WriteFile(repo_path / "README.md", "hello\n");
  InitializeGitRepo(repo_path);
  CommitAll(repo_path, "first commit", "first commit");
  WriteFile(repo_path / "README.md", "hello world\n");
  CommitAll(repo_path, "second commit", "second commit");
  RequireGitCommandSuccess(repo_path, {"checkout", "-b", "feature/topic"},
                           "git checkout feature branch");

  const auto branches = CollectGitBranches(repo_path);
  bool saw_main = false;
  bool saw_feature = false;
  for (const auto& branch : branches) {
    Expect(branch.label.find("/HEAD") == std::string::npos,
           "branch collection should skip symbolic HEAD refs");
    if (branch.label == "main") {
      saw_main = true;
      // The ref carries the full, unambiguous refname; the label is the short form.
      Expect(branch.ref == "refs/heads/main",
             "a local branch's ref is the full refs/heads/ ref, not the short label");
    }
    if (branch.label == "feature/topic") {
      saw_feature = true;
      Expect(branch.ref == "refs/heads/feature/topic",
             "feature branch ref is the full refname");
    }
  }
  Expect(saw_main && saw_feature,
         "branch collection should list both local branches by short label");

  const auto recent = CollectGitRecentCommits(repo_path, 10);
  Expect(recent.size() == 2, "recent commit collection should return both commits on HEAD");
  Expect(recent[0].subject == "second commit",
         "recent commit collection should list newest commit first");
  Expect(!recent[0].short_hash.empty() && !recent[0].relative_date.empty(),
         "recent commit collection should populate short hash and relative date");

  const auto capped = CollectGitRecentCommits(repo_path, 1);
  Expect(capped.size() == 1, "recent commit collection should honor the limit");

  // A caller-supplied limit far above the helper-level cap (kMaxRecentCommits =
  // 1000) must be clamped before it reaches `git log -n` / the unbounded
  // ParseLog. This repo only has two commits, so an over-cap request must still
  // succeed and return at most the cap (here, the two available commits).
  static constexpr std::size_t kRecentCommitHelperCap = 1000;
  const auto over_cap = CollectGitRecentCommits(repo_path, 1'000'000);
  Expect(over_cap.size() <= kRecentCommitHelperCap,
         "over-cap recent-commit request must be clamped to the helper cap");
  Expect(over_cap.size() == 2,
         "over-cap request should still return every available commit on HEAD");
}

// Regression guard for the removed `rev-parse --verify HEAD` pre-check in
// CollectGitRecentCommits: on an unborn branch (git init, no commit) the direct
// `git log HEAD` must still resolve to an empty result rather than surfacing a
// fatal or garbage row. Locks the behavior the dropped spawn used to provide.
void TestRecentCommitsOnUnbornBranchIsEmpty() {
  TemporaryDirectory temp_dir;
  const auto repo_path = temp_dir.path() / "repo";
  InitializeGitRepo(repo_path);  // No commit yet: HEAD is unborn.

  const auto recent = CollectGitRecentCommits(repo_path, 10);
  Expect(recent.empty(), "recent-commit collection on an unborn branch must be empty");
}

void TestGitResolvePrBaseReferenceFromGhMergeBase() {
  TemporaryDirectory temp_dir;
  const auto repo_path = temp_dir.path() / "repo";
  const auto base_dir = FixturePath("diff/git/base");
  CopyTree(base_dir, repo_path);

  InitializeGitRepo(repo_path);
  CommitAll(repo_path, "base fixture", "base fixture");
  RequireGitCommandSuccess(repo_path, {"checkout", "-b", "release/2.0"},
                           "git checkout release branch");
  WriteFile(repo_path / "README.md", ReadFile(repo_path / "README.md") + "\nrelease-only\n");
  CommitAll(repo_path, "release fixture", "release fixture");

  RequireGitCommandSuccess(repo_path, {"checkout", "-b", "feature/pr-base"},
                           "git checkout feature branch");
  WriteFile(repo_path / "src/pr_only.cpp", "int main() { return 0; }\n");
  CommitAll(repo_path, "feature fixture", "feature fixture");
  RequireGitCommandSuccess(repo_path,
                           {"config", "branch.feature/pr-base.gh-merge-base", "release/2.0"},
                           "git config gh merge base");

  const auto base_ref = ResolveGitBaseReference(repo_path);
  Expect(base_ref.has_value(), "git base reference should resolve from gh-merge-base");
  Expect(base_ref->ref == "refs/heads/release/2.0",
         "gh-merge-base should override the default branch when resolving the PR base");

  const auto outgoing = CollectGitBranchOutgoingFiles(repo_path, base_ref->ref);
  Expect(outgoing.size() == 1,
         "outgoing files should only include commits ahead of the configured PR base");
  Expect(outgoing[0].relative_path == std::filesystem::path("src/pr_only.cpp"),
         "outgoing files should exclude changes that already exist on the PR base branch");
  Expect(outgoing[0].status == GitFileStatus::Added,
         "PR-only file should be reported with the correct outgoing status");
}

void TestGitBulkStageAndDiscard() {
  TemporaryDirectory temp_dir;
  const auto repo_path = temp_dir.path() / "repo";
  const auto base_dir = FixturePath("diff/git/base");
  CopyTree(base_dir, repo_path);

  InitializeGitRepo(repo_path);
  CommitAll(repo_path, "base fixture", "base fixture");

  const auto modified_file = repo_path / "README.md";
  const auto deleted_file = repo_path / "src/session.cpp";
  const auto staged_added_file = repo_path / "src/staged_new.cpp";
  const auto untracked_file = repo_path / "notes.txt";
  WriteFile(modified_file, ReadFile(modified_file) + "\nrestage me\n");
  std::filesystem::remove(deleted_file);
  WriteFile(staged_added_file, "int staged = 1;\n");
  WriteFile(untracked_file, "temporary notes\n");

  Expect(GitStageAll(repo_path), "git stage all should succeed");
  auto entries = CollectGitWorkingTreeEntries(repo_path);
  Expect(entries.size() == 4, "bulk stage fixture should still report four changes");
  for (const auto& entry : entries) {
    Expect(entry.staged, "git stage all should stage every working-tree entry");
  }

  Expect(GitDiscardAll(repo_path), "git discard all should succeed");
  entries = CollectGitWorkingTreeEntries(repo_path);
  Expect(entries.empty(), "git discard all should leave a clean working tree");
  Expect(ReadFile(modified_file) == ReadFile(base_dir / "README.md"),
         "git discard all should restore tracked modifications");
  Expect(std::filesystem::exists(deleted_file),
         "git discard all should restore tracked deletions");
  Expect(!std::filesystem::exists(staged_added_file),
         "git discard all should remove staged added files");
  Expect(!std::filesystem::exists(untracked_file),
         "git discard all should remove untracked files");
}

// Regression: git pathspec magic must not fire for user filenames. A file named
// with a magic prefix like ":(glob)…" is staged literally because every git
// command runs with --literal-pathspecs; without it, git would interpret the
// prefix as magic and stage the wrong path (or nothing).
void TestGitStageHonorsLiteralPathspecs() {
  TemporaryDirectory temp_dir;
  const auto repo_path = temp_dir.path() / "repo";
  std::filesystem::create_directories(repo_path);
  InitializeGitRepo(repo_path);
  WriteFile(repo_path / "seed.txt", "seed\n");
  CommitAll(repo_path, "base", "base");

  const std::filesystem::path magic_rel(":(glob)weird.txt");
  WriteFile(repo_path / magic_rel, "content\n");
  Expect(GitStagePath(repo_path, repo_path / magic_rel),
         "staging a pathspec-magic-named file should succeed");

  const auto entries = CollectGitWorkingTreeEntries(repo_path);
  bool found_staged = false;
  for (const auto& entry : entries) {
    if (entry.relative_path == magic_rel && entry.staged) {
      found_staged = true;
    }
  }
  Expect(found_staged, "the literal magic-named file must be staged, not interpreted as magic");
}

void TestGitDiscardStagedRenameRestoresSource() {
  TemporaryDirectory temp_dir;
  const auto repo_path = temp_dir.path() / "repo";
  InitializeGitRepo(repo_path);
  const auto old_file = repo_path / "old.txt";
  WriteFile(old_file, "original content\nline2\n");
  CommitAll(repo_path, "base", "base");

  // Stage a rename (surfaced as a single Staged entry keyed on the destination).
  Expect(RunGitCommand(repo_path, {"mv", "old.txt", "new.txt"}) == 0,
         "git mv should stage a rename");

  GitRepository repo(repo_path);
  Expect(repo.Discard("new.txt"), "discarding a staged rename should succeed");

  // Regression: the destination-only discard previously removed `new` and orphaned
  // `old`'s staged deletion, destroying the original content. The source must be
  // restored and the destination removed, leaving a clean tree.
  Expect(std::filesystem::exists(old_file),
         "discarding a staged rename must restore the source file");
  Expect(ReadFile(old_file) == "original content\nline2\n",
         "the restored source must retain its original content");
  Expect(!std::filesystem::exists(repo_path / "new.txt"),
         "discarding a staged rename must remove the destination");
  Expect(CollectGitWorkingTreeEntries(repo_path).empty(),
         "discarding a staged rename must leave a clean working tree");
}

void TestGitUnstageStagedRenameResetsBothSides() {
  TemporaryDirectory temp_dir;
  const auto repo_path = temp_dir.path() / "repo";
  InitializeGitRepo(repo_path);
  WriteFile(repo_path / "old.txt", "original content\n");
  CommitAll(repo_path, "base", "base");
  Expect(RunGitCommand(repo_path, {"mv", "old.txt", "new.txt"}) == 0, "git mv should stage a rename");

  GitRepository repo(repo_path);
  Expect(repo.Unstage("new.txt"), "unstaging a staged rename should succeed");

  // Unstaging must reset both sides to HEAD: nothing left staged (the source's
  // staged deletion is no longer orphaned), and the renamed file stays in the tree.
  for (const auto& entry : CollectGitWorkingTreeEntries(repo_path)) {
    Expect(!entry.staged, "unstaging a rename must leave nothing staged");
  }
  Expect(std::filesystem::exists(repo_path / "new.txt"),
         "unstage keeps the renamed file in the working tree");
}

void TestGitRepositoryDirectApi() {
  TemporaryDirectory temp_dir;
  const auto repo_path = temp_dir.path() / "repo";
  const auto base_dir = FixturePath("diff/git/base");
  CopyTree(base_dir, repo_path);

  InitializeGitRepo(repo_path);
  CommitAll(repo_path, "base fixture", "base fixture");

  GitRepository repo(repo_path);
  Expect(repo.IsValid(), "git repository wrapper should detect a valid repository");
  Expect(repo.ToRelative(repo_path / "README.md") == std::filesystem::path("README.md"),
         "git repository wrapper should convert absolute paths to repo-relative paths");
  Expect(repo.ToAbsolute("README.md") == repo_path / "README.md",
         "git repository wrapper should convert relative paths back to absolute ones");

  const auto history_result = repo.GetFileHistory("README.md");
  const auto& history = history_result.commits;
  Expect(history.size() == 1, "git repository wrapper should return commit history");
  Expect(!history_result.truncated,
         "a short history must not be flagged as truncated");
  Expect(history[0].subject == "base fixture",
         "git repository wrapper should preserve commit subjects");

  const auto head_content = repo.ReadFileAtRevision("README.md");
  Expect(head_content.has_value(), "git repository wrapper should read HEAD file contents");
  Expect(*head_content == ReadFile(base_dir / "README.md"),
         "git repository wrapper should read the committed file contents");

  WriteFile(repo_path / "README.md", ReadFile(base_dir / "README.md") + "\nwrapper change\n");
  WriteFile(repo_path / "notes.txt", "wrapper note\n");
  const auto statuses = repo.GetStatuses();
  Expect(statuses.at("README.md") == GitFileStatus::Modified,
         "git repository wrapper should report modified files");
  Expect(statuses.at("notes.txt") == GitFileStatus::Untracked,
         "git repository wrapper should report untracked files");
}

void TestGitRepositoryHandlesQuotedAndSpacedPaths() {
  TemporaryDirectory temp_dir;
  const auto repo_path = temp_dir.path() / "repo";
  InitializeGitRepo(repo_path);

  const auto weird_file = repo_path / "dir with spaces" / "quote's file.cpp";
  WriteFile(weird_file, "int value = 1;\n");
  CommitAll(repo_path, "Add weird path", "weird path");

  GitRepository repo(repo_path);
  const auto relative = repo.ToRelative(weird_file);
  Expect(relative.has_value(), "git repository wrapper should relativize quoted paths");
  Expect(*relative == std::filesystem::path("dir with spaces") / "quote's file.cpp",
         "git repository wrapper should preserve the exact relative path");

  const auto history = repo.GetFileHistory(*relative).commits;
  Expect(history.size() == 1, "quoted path history should be readable through git repository");
  Expect(history[0].subject == "Add weird path",
         "quoted path history should preserve commit metadata");

  const auto head_content = repo.ReadFileAtRevision(*relative);
  Expect(head_content.has_value(), "quoted path content should be readable at HEAD");
  Expect(*head_content == "int value = 1;\n",
         "quoted path content should round-trip through git show");
}

void TestGitPorcelainParserStatusV1() {
  // Real `git status --porcelain=v1 -z` emits the rename destination first (in
  // the code record) and the source path in the following NUL record.
  std::string output;
  output += "R  src/renamed.cpp";
  output.push_back('\0');
  output += "old/name.cpp";
  output.push_back('\0');
  output += " M src/nested/file.cpp";
  output.push_back('\0');
  output += "A  include/new_header.h";
  output.push_back('\0');
  output += "UU src/conflict.cpp";
  output.push_back('\0');
  output += "?? scratch.txt";
  output.push_back('\0');

  const auto statuses = GitPorcelainParser::ParseStatusV1(output);
  Expect(!statuses.contains("old/name.cpp"),
         "status parser should report rename targets instead of source paths");
  Expect(statuses.at("src/renamed.cpp") == GitFileStatus::Modified,
         "status parser should classify renamed files as modified");
  Expect(statuses.at("src/nested/file.cpp") == GitFileStatus::Modified,
         "status parser should classify unstaged edits as modified");
  Expect(statuses.at("include/new_header.h") == GitFileStatus::Added,
         "status parser should classify added files");
  Expect(statuses.at("scratch.txt") == GitFileStatus::Untracked,
         "status parser should classify untracked files");
  Expect(statuses.at("src/conflict.cpp") == GitFileStatus::Conflicted,
         "status parser should classify conflicted files");
  Expect(statuses.at("src") == GitFileStatus::Conflicted,
         "directory statuses should preserve the strongest child status");
  Expect(statuses.at("src/nested") == GitFileStatus::Modified,
         "directory statuses should include nested parents");
}

void TestGitPorcelainParserWorkingTreeEntries() {
  std::string output;
  output += "?? z-last.txt";
  output.push_back('\0');
  output += "M  src/staged.cpp";
  output.push_back('\0');
  output += "UU src/conflict.cpp";
  output.push_back('\0');
  output += "R  src/renamed.cpp";
  output.push_back('\0');
  output += "old/path.cpp";
  output.push_back('\0');

  const auto entries = GitPorcelainParser::ParseWorkingTreeEntries(output);
  Expect(entries.size() == 4, "working-tree parser should return every parsed entry");
  Expect(entries[0].relative_path == std::filesystem::path("src/renamed.cpp"),
         "staged rename should sort first by target path");
  Expect(entries[0].staged, "staged rename should remain staged");
  Expect(!entries[0].conflicted, "staged rename should not be conflicted");

  Expect(entries[1].relative_path == std::filesystem::path("src/staged.cpp"),
         "staged paths should sort ahead of unstaged ones");
  Expect(entries[1].staged, "staged modification should report staged");

  Expect(entries[2].relative_path == std::filesystem::path("src/conflict.cpp"),
         "unstaged conflicted entries should sort by path after staged items");
  Expect(entries[2].conflicted, "conflicted entry should report conflicted");
  Expect(!entries[2].staged, "conflicted entry should not report staged");

  Expect(entries[3].relative_path == std::filesystem::path("z-last.txt"),
         "untracked entries should remain in alphabetical order");
  Expect(entries[3].status == GitFileStatus::Untracked,
         "untracked entry should keep its status");
}

void TestGitPorcelainParserLog() {
  // Format: <hash>\x1f<short>\x1f<author>\x1f<relative_date>\x1f<subject> (subject
  // last, so it may contain the delimiter). US (0x1f) is the delimiter, not a tab,
  // so a tab embedded in an author name cannot shift the fields.
  const std::string output =
      "0123456789abcdef\x1f" "0123456\x1f" "Ada\x1f" "2 days ago\x1f" "first subject\n"
      "malformed line\n"
      "\n"
      "fedcba9876543210\x1f" "fedcba9\x1f" "Grace\x1f" "3 weeks ago\x1f" "second\tsubject\n";

  const auto commits = GitPorcelainParser::ParseLog(output);
  Expect(commits.size() == 2, "log parser should skip malformed lines");
  Expect(commits[0].hash == "0123456789abcdef", "first parsed commit hash mismatch");
  Expect(commits[0].short_hash == "0123456", "first parsed short hash mismatch");
  Expect(commits[0].author == "Ada", "first parsed author mismatch");
  Expect(commits[0].relative_date == "2 days ago", "first parsed relative date mismatch");
  Expect(commits[0].subject == "first subject", "first parsed subject mismatch");
  Expect(commits[1].hash == "fedcba9876543210", "second parsed commit hash mismatch");
  Expect(commits[1].subject == "second\tsubject",
         "second parsed subject should preserve embedded tabs");

  // Regression: an author name containing a literal tab must not shift the
  // author/date/subject fields (the old tab-delimited format truncated the author
  // and absorbed the rest into later fields).
  const std::string tab_author =
      "aaaabbbbccccdddd\x1f" "aaaabbb\x1f" "Jane\tQ\x1f" "5 minutes ago\x1f" "tabbed author\n";
  const auto tab_commits = GitPorcelainParser::ParseLog(tab_author);
  Expect(tab_commits.size() == 1, "tab-in-author line should parse as one commit");
  Expect(tab_commits[0].author == "Jane\tQ",
         "author with an embedded tab should be preserved intact");
  Expect(tab_commits[0].relative_date == "5 minutes ago",
         "relative date must not absorb the author's tail");
  Expect(tab_commits[0].subject == "tabbed author", "subject must stay intact");

  // The parser-level cap bounds the result so a hostile/corrupt log stream cannot
  // materialize an unbounded vector, regardless of caller.
  std::string many;
  for (int i = 0; i < 50; ++i) {
    many += "aaaabbbbccccdddd\x1f" "aaaabbb\x1f" "Ada\x1f" "now\x1f" "s\n";
  }
  const auto capped = GitPorcelainParser::ParseLog(many, /*max_entries=*/10);
  Expect(capped.size() == 10, "ParseLog must stop at the requested cap");
  const auto uncapped = GitPorcelainParser::ParseLog(many);
  Expect(uncapped.size() == 50, "the default cap is generous enough for a normal log");
}

// Git-independent regression for the `-z` name-status parser. The prior parser
// split on whitespace, truncating paths with spaces and mis-handling renames.
void TestGitBranchDiffNameStatusZParser() {
  using microide::project::ParseGitBranchDiffNameStatusZ;
  // status NUL path NUL records, including a rename (status NUL old NUL new) and
  // paths containing spaces. Built token-by-token to avoid fragile byte counts.
  std::string output;
  const auto add_token = [&](std::string_view token) {
    output.append(token.data(), token.size());
    output.push_back('\0');
  };
  add_token("M");
  add_token("src/file one.txt");
  add_token("R100");
  add_token("old name.txt");
  add_token("new name.txt");
  add_token("A");
  add_token("added.txt");
  const auto entries = ParseGitBranchDiffNameStatusZ(output);
  Expect(entries.size() == 3, "parser should yield three entries");
  // Sorted by path: added.txt, new name.txt, src/file one.txt
  Expect(entries[0].relative_path.generic_string() == "added.txt", "added path");
  Expect(entries[0].status == GitFileStatus::Added, "added status");
  Expect(entries[1].relative_path.generic_string() == "new name.txt",
         "rename reports new path with space preserved");
  Expect(entries[1].status == GitFileStatus::Modified, "rename status is modified");
  Expect(entries[2].relative_path.generic_string() == "src/file one.txt",
         "modified path with space preserved");
  Expect(entries[2].status == GitFileStatus::Modified, "modified status");
}

// Folder-aggregated status must be single-sourced through GitStatusPriority.
// A previous inline table in BuildGitStatusMap ranked Added == Untracked, so a
// folder holding both could aggregate to either depending on entry order.
void TestBuildGitStatusMapFolderPriorityIsSingleSourced() {
  using microide::project::BuildGitStatusMap;
  using microide::project::GitWorkingTreeEntry;

  // Added (priority 2) must outrank Untracked (priority 1) for the shared folder,
  // regardless of which entry is recorded first.
  const std::vector<GitWorkingTreeEntry> untracked_first{
      {.relative_path = "pkg/new.txt", .status = GitFileStatus::Untracked},
      {.relative_path = "pkg/staged.txt", .status = GitFileStatus::Added, .staged = true},
  };
  const std::vector<GitWorkingTreeEntry> added_first{
      {.relative_path = "pkg/staged.txt", .status = GitFileStatus::Added, .staged = true},
      {.relative_path = "pkg/new.txt", .status = GitFileStatus::Untracked},
  };

  const auto a = BuildGitStatusMap(untracked_first);
  const auto b = BuildGitStatusMap(added_first);
  Expect(a.at("pkg") == GitFileStatus::Added,
         "folder with Added + Untracked should aggregate to Added (Added outranks Untracked)");
  Expect(a.at("pkg") == b.at("pkg"),
         "folder aggregation must not depend on working-tree entry order");
}

void TestGitStatusCodePrecedenceIsUnified() {
  // v1 and v2 porcelain mappers share one change-code core so they cannot drift:
  // Deleted > Added(A|C) > Modified(M|R|T) > Clean.
  Expect(GitPorcelainParser::StatusFromChangeCodeChars("D ") == GitFileStatus::Deleted,
         "D maps to Deleted");
  Expect(GitPorcelainParser::StatusFromChangeCodeChars(" D") == GitFileStatus::Deleted,
         "trailing D maps to Deleted");
  Expect(GitPorcelainParser::StatusFromChangeCodeChars("A ") == GitFileStatus::Added,
         "A maps to Added");
  Expect(GitPorcelainParser::StatusFromChangeCodeChars("C ") == GitFileStatus::Added,
         "status C (copy) maps to Added");
  Expect(GitPorcelainParser::StatusFromChangeCodeChars("M ") == GitFileStatus::Modified,
         "M maps to Modified");
  Expect(GitPorcelainParser::StatusFromChangeCodeChars("R ") == GitFileStatus::Modified,
         "R maps to Modified");
  Expect(GitPorcelainParser::StatusFromChangeCodeChars("T ") == GitFileStatus::Modified,
         "T maps to Modified");
  Expect(GitPorcelainParser::StatusFromChangeCodeChars("AD") == GitFileStatus::Deleted,
         "Deleted takes precedence over Added");
  Expect(GitPorcelainParser::StatusFromChangeCodeChars("  ") == GitFileStatus::Clean,
         "no change code maps to Clean");
  // The v1 full mapper classifies untracked and conflicted states before the core.
  Expect(GitPorcelainParser::StatusFromPorcelainCode("??") == GitFileStatus::Untracked,
         "?? maps to Untracked");
  Expect(GitPorcelainParser::StatusFromPorcelainCode("UU") == GitFileStatus::Conflicted,
         "UU maps to Conflicted");
  // Diff codes deliberately differ: there C (copy) is Modified, not Added.
  Expect(GitPorcelainParser::StatusFromDiffCode('C') == GitFileStatus::Modified,
         "diff-code C maps to Modified, distinct from status C");
}

// A hostile repo can emit `git status` output with millions of records and/or a
// single pathologically deep path. The status parser must cap the entry count
// (bounds the downstream UI-thread sort + heap) and cap ancestor-badge depth
// (the folder-badge walk was quadratic in path length → OOM).
void TestGitPorcelainParserBoundsHostileStatus() {
  // Entry-count cap: 60000 untracked records collapse to <= the 50000 cap.
  {
    std::string output;
    for (int i = 0; i < 60000; ++i) {
      output += "?? f";
      output += std::to_string(i);
      output += ".txt";
      output.push_back('\0');
    }
    const auto statuses = GitPorcelainParser::ParseStatusV1(output);
    Expect(statuses.size() <= 50000,
           "status parser must cap the number of entries from a hostile repo");
  }
  // Ancestor-badge depth cap: one very deep path must not create a map key per
  // ancestor level (previously O(depth) allocations → quadratic OOM).
  {
    std::unordered_map<std::string, GitFileStatus> statuses;
    std::string deep;
    for (int i = 0; i < 5000; ++i) {
      deep += "d/";
    }
    deep += "leaf.txt";
    GitPorcelainParser::RecordGitStatus(statuses, std::filesystem::path(deep),
                                        GitFileStatus::Modified);
    // Leaf + at most kMaxBadgeAncestorDepth (64) ancestor keys, not ~5000.
    Expect(statuses.size() <= 66,
           "ancestor-badge propagation must be depth-capped for a deep path");
  }
}

// A git invocation killed for exceeding its wall-clock timeout must report
// timed_out (not a bare non-zero exit), so callers can distinguish a spurious
// timeout from a real failure instead of showing a generic error.
void TestGitCommandTimeoutReportsTimedOut() {
#if defined(_WIN32)
  return;  // POSIX sh hook + SIGKILL semantics
#else
  TemporaryDirectory temp_dir;
  const auto repo_path = temp_dir.path() / "repo";
  WriteFile(repo_path / "a.txt", "a1\n");
  InitializeGitRepo(repo_path);
  CommitAll(repo_path, "first commit", "seed repo for timeout test");

  // A pre-commit hook that sleeps far past the timeout makes `git commit` block
  // deterministically.
  const auto hook = repo_path / ".git" / "hooks" / "pre-commit";
  WriteFile(hook, "#!/bin/sh\nsleep 30\n");
  std::filesystem::permissions(hook, std::filesystem::perms::owner_all,
                               std::filesystem::perm_options::add);

  WriteFile(repo_path / "a.txt", "a2\n");
  RunGitCommand(repo_path, {"add", "-A"});

  const auto result = microide::project::internal::ReadGitCommandOutput(
      repo_path, {"commit", "-m", "blocked"}, /*silence_stderr=*/false, /*timeout_ms=*/300);
  Expect(result.timed_out, "a git commit blocked by a slow hook must report timed_out");
  Expect(!result.success(), "a timed-out git command must not report success");
#endif
}

}  // namespace

// J1: `git show <rev>:<path>` can be killed for hitting the subprocess capture
// ceiling, leaving a partial blob with a non-zero exit and result.truncated set.
// The interpretation must surface that partial content WITH the truncated flag (so
// callers can refuse to diff it as truth) while a genuine failure stays nullopt and
// a clean read stays untruncated. Tested through the pure static helper so the
// contract is verified without committing a 128 MiB blob to force a real kill.
void TestGitReadFileAtRevisionSurfacesTruncation() {
  using CommandResult = GitRepository::CommandResult;

  // A clean success: full content, not truncated.
  const auto clean = GitRepository::InterpretBlobResult(
      CommandResult{.exit_code = 0, .output = "full file contents\n", .truncated = false});
  Expect(clean.has_value(), "a successful git show should yield a blob");
  Expect(clean->content == "full file contents\n" && !clean->truncated,
         "a clean read is the whole file and is not flagged truncated");

  // A capture-ceiling kill: non-zero exit but truncated set -> partial + flagged.
  const auto truncated = GitRepository::InterpretBlobResult(
      CommandResult{.exit_code = 137, .output = "partial prefix", .truncated = true});
  Expect(truncated.has_value(),
         "a truncated blob is still surfaced (partial content is available)");
  Expect(truncated->truncated,
         "a capture-ceiling kill must be flagged truncated, not treated as a full read");
  Expect(truncated->content == "partial prefix",
         "the partial prefix is preserved for a 'content truncated' state");

  // A genuine failure (e.g. missing revision) is nullopt, distinct from an empty
  // blob and from a truncation.
  const auto failure = GitRepository::InterpretBlobResult(
      CommandResult{.exit_code = 128, .output = "", .truncated = false});
  Expect(!failure.has_value(),
         "a real non-zero-exit failure stays nullopt, not a bogus empty blob");
}

void TestGitExplicitRevisionArgsUseEndOfOptions() {
  using microide::project::CollectGitCommitChangedFiles;

  TemporaryDirectory temp_dir;
  const auto repo_path = temp_dir.path() / "repo";
  InitializeGitRepo(repo_path);
  WriteFile(repo_path / "kept.txt", "one\n");
  CommitAll(repo_path, "base", "base");
  WriteFile(repo_path / "added.txt", "two\n");
  CommitAll(repo_path, "add file", "second");

  const auto recent = CollectGitRecentCommits(repo_path, 10);
  Expect(recent.size() == 2, "fixture should have two commits");
  const std::string& head_hash = recent[0].hash;
  Expect(!head_hash.empty(), "HEAD commit hash should be resolvable");

  // diff-tree over an explicit commit hash now passes the revision after
  // `--end-of-options`; the changed-file list must still parse correctly.
  const auto changed = CollectGitCommitChangedFiles(repo_path, head_hash);
  Expect(std::find(changed.begin(), changed.end(), std::filesystem::path("added.txt")) !=
             changed.end(),
         "commit changed-files should list the added path with an explicit revision");

  // cat-file -e / show <rev>:<path> now also carry `--end-of-options`; existence
  // and content reads at an explicit revision must keep working.
  GitRepository repo(repo_path);
  Expect(repo.FileExistsAtRevision("added.txt", head_hash),
         "an existing file must be detected at an explicit revision");
  Expect(!repo.FileExistsAtRevision("added.txt", recent[1].hash),
         "a file absent from the base commit must not be detected there");
  const auto content = repo.ReadFileAtRevision("added.txt", head_hash);
  Expect(content.has_value() && *content == "two\n",
         "file content must round-trip through show at an explicit revision");
}

void RegisterGitServiceTests(std::vector<TestCase>& tests) {
  AddTest(tests, "Git/ReadFileAtRevisionSurfacesTruncation",
          TestGitReadFileAtRevisionSurfacesTruncation);
  AddTest(tests, "Git/ExplicitRevisionArgsUseEndOfOptions",
          TestGitExplicitRevisionArgsUseEndOfOptions);
  AddTest(tests, "Git/PorcelainParserBoundsHostileStatus",
          TestGitPorcelainParserBoundsHostileStatus);
  AddTest(tests, "Git/BuildStatusMapFolderPriorityIsSingleSourced",
          TestBuildGitStatusMapFolderPriorityIsSingleSourced);
  AddTest(tests, "Git/BranchDiffNameStatusZParser", TestGitBranchDiffNameStatusZParser);
  AddTest(tests, "Git/CompareFixture", TestGitCompareFixture);
  AddTest(tests, "Git/WorkingTreeStatusAndActions", TestGitWorkingTreeStatusAndActions);
  AddTest(tests, "Git/OutgoingBranchFiles", TestGitOutgoingBranchFiles);
  AddTest(tests, "Git/OutgoingBaseChoiceResolution", TestGitOutgoingBaseChoiceResolution);
  AddTest(tests, "Git/BranchAndRecentCommitCollection", TestGitBranchAndRecentCommitCollection);
  AddTest(tests, "Git/RecentCommitsOnUnbornBranchIsEmpty", TestRecentCommitsOnUnbornBranchIsEmpty);
  AddTest(tests, "Git/ResolvePrBaseReferenceFromGhMergeBase",
          TestGitResolvePrBaseReferenceFromGhMergeBase);
  AddTest(tests, "Git/BulkStageAndDiscard", TestGitBulkStageAndDiscard);
  AddTest(tests, "Git/StageHonorsLiteralPathspecs", TestGitStageHonorsLiteralPathspecs);
  AddTest(tests, "Git/DiscardStagedRenameRestoresSource", TestGitDiscardStagedRenameRestoresSource);
  AddTest(tests, "Git/UnstageStagedRenameResetsBothSides", TestGitUnstageStagedRenameResetsBothSides);
  AddTest(tests, "Git/RepositoryDirectApi", TestGitRepositoryDirectApi);
  AddTest(tests, "Git/RepositoryHandlesQuotedAndSpacedPaths",
          TestGitRepositoryHandlesQuotedAndSpacedPaths);
  AddTest(tests, "Git/PorcelainParserStatusV1", TestGitPorcelainParserStatusV1);
  AddTest(tests, "Git/PorcelainParserWorkingTreeEntries", TestGitPorcelainParserWorkingTreeEntries);
  AddTest(tests, "Git/PorcelainParserLog", TestGitPorcelainParserLog);
  AddTest(tests, "Git/StatusCodePrecedenceUnified", TestGitStatusCodePrecedenceIsUnified);
  AddTest(tests, "Git/CommandTimeoutReportsTimedOut", TestGitCommandTimeoutReportsTimedOut);
}

}  // namespace microide::tests
