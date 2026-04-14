#include "TestSupport.h"

#include "compare/CompareModel.h"
#include "project/GitCompareService.h"
#include "project/GitPorcelainParser.h"
#include "project/GitRepository.h"
#include "project/GitStatusService.h"

#include <algorithm>
#include <filesystem>
#include <string>
#include <string_view>

namespace microide::tests {
namespace {

using microide::compare::BuildCompareModel;
using microide::compare::CompareModel;
using microide::compare::CompareRowKind;
using microide::project::CollectGitBranchOutgoingFiles;
using microide::project::CollectGitFileHistory;
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

  const auto history = CollectGitFileHistory(repo_path, tracked_file);
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
  Expect(compare_model.rows.size() == 27,
         "git session fixture should preserve the terminal empty row");
  Expect(compare_summary.unchanged == 19,
         "git session fixture should keep unchanged rows plus the terminal empty row");
  Expect(compare_summary.modified == 2, "git session fixture should produce 2 modified rows");
  Expect(compare_summary.added == 6, "git session fixture should produce 6 added rows");
  Expect(compare_summary.deleted == 0, "git session fixture should produce 0 deleted rows");
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
}

void TestGitOutgoingBranchFiles() {
  TemporaryDirectory temp_dir;
  const auto repo_path = temp_dir.path() / "repo";
  const auto base_dir = FixturePath("diff/git/base");
  const auto head_dir = FixturePath("diff/git/head");
  CopyTree(base_dir, repo_path);

  InitializeGitRepo(repo_path);
  CommitAll(repo_path, "base fixture", "base fixture");
  RequireCommandSuccess(
      "git -C '" + EscapedRepoPath(repo_path) +
          "' checkout -b feature/git-view >/dev/null 2>/dev/null",
      "git checkout feature branch");

  WriteFile(repo_path / "src/session.cpp", ReadFile(head_dir / "src/session.cpp"));
  WriteFile(repo_path / "src/new_panel.cpp", ReadFile(head_dir / "src/new_panel.cpp"));
  CommitAll(repo_path, "feature fixture", "feature fixture");

  const auto base_ref = ResolveGitBaseReference(repo_path);
  Expect(base_ref.has_value(), "git base reference should resolve in a main-based repo");
  Expect(base_ref->ref == "main",
         "local repo without remotes should resolve main as the base branch");

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

void TestGitResolvePrBaseReferenceFromGhMergeBase() {
  TemporaryDirectory temp_dir;
  const auto repo_path = temp_dir.path() / "repo";
  const auto base_dir = FixturePath("diff/git/base");
  CopyTree(base_dir, repo_path);

  InitializeGitRepo(repo_path);
  CommitAll(repo_path, "base fixture", "base fixture");
  RequireCommandSuccess(
      "git -C '" + EscapedRepoPath(repo_path) +
          "' checkout -b release/2.0 >/dev/null 2>/dev/null",
      "git checkout release branch");
  WriteFile(repo_path / "README.md", ReadFile(repo_path / "README.md") + "\nrelease-only\n");
  CommitAll(repo_path, "release fixture", "release fixture");

  RequireCommandSuccess(
      "git -C '" + EscapedRepoPath(repo_path) +
          "' checkout -b feature/pr-base >/dev/null 2>/dev/null",
      "git checkout feature branch");
  WriteFile(repo_path / "src/pr_only.cpp", "int main() { return 0; }\n");
  CommitAll(repo_path, "feature fixture", "feature fixture");
  RequireCommandSuccess(
      "git -C '" + EscapedRepoPath(repo_path) +
          "' config branch.feature/pr-base.gh-merge-base release/2.0 >/dev/null 2>/dev/null",
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

  const auto history = repo.GetFileHistory("README.md");
  Expect(history.size() == 1, "git repository wrapper should return commit history");
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

void TestGitPorcelainParserStatusV1() {
  std::string output;
  output += "R  old/name.cpp";
  output.push_back('\0');
  output += "src/renamed.cpp";
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
  output += "R  old/path.cpp";
  output.push_back('\0');
  output += "src/renamed.cpp";
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
  const std::string output =
      "0123456789abcdef\t0123456\tfirst subject\n"
      "malformed line\n"
      "\n"
      "fedcba9876543210\tfedcba9\tsecond subject\n";

  const auto commits = GitPorcelainParser::ParseLog(output);
  Expect(commits.size() == 2, "log parser should skip malformed lines");
  Expect(commits[0].hash == "0123456789abcdef", "first parsed commit hash mismatch");
  Expect(commits[0].short_hash == "0123456", "first parsed short hash mismatch");
  Expect(commits[0].subject == "first subject", "first parsed subject mismatch");
  Expect(commits[1].hash == "fedcba9876543210", "second parsed commit hash mismatch");
  Expect(commits[1].subject == "second subject", "second parsed subject mismatch");
}

}  // namespace

void RegisterGitServiceTests(std::vector<TestCase>& tests) {
  AddTest(tests, "Git/CompareFixture", TestGitCompareFixture);
  AddTest(tests, "Git/WorkingTreeStatusAndActions", TestGitWorkingTreeStatusAndActions);
  AddTest(tests, "Git/OutgoingBranchFiles", TestGitOutgoingBranchFiles);
  AddTest(tests, "Git/ResolvePrBaseReferenceFromGhMergeBase",
          TestGitResolvePrBaseReferenceFromGhMergeBase);
  AddTest(tests, "Git/BulkStageAndDiscard", TestGitBulkStageAndDiscard);
  AddTest(tests, "Git/RepositoryDirectApi", TestGitRepositoryDirectApi);
  AddTest(tests, "Git/PorcelainParserStatusV1", TestGitPorcelainParserStatusV1);
  AddTest(tests, "Git/PorcelainParserWorkingTreeEntries", TestGitPorcelainParserWorkingTreeEntries);
  AddTest(tests, "Git/PorcelainParserLog", TestGitPorcelainParserLog);
}

}  // namespace microide::tests
