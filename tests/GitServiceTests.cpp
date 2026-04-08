#include "TestSupport.h"

#include "compare/CompareModel.h"
#include "project/GitCompareService.h"
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
using microide::project::GitDiscardPath;
using microide::project::GitFileStatus;
using microide::project::GitStagePath;
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
  Expect(compare_model.rows.size() == 26, "git session fixture should produce 26 rows");
  Expect(compare_summary.unchanged == 18,
         "git session fixture should produce 18 unchanged rows");
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

}  // namespace

void RegisterGitServiceTests(std::vector<TestCase>& tests) {
  AddTest(tests, "Git/CompareFixture", TestGitCompareFixture);
  AddTest(tests, "Git/WorkingTreeStatusAndActions", TestGitWorkingTreeStatusAndActions);
  AddTest(tests, "Git/OutgoingBranchFiles", TestGitOutgoingBranchFiles);
}

}  // namespace microide::tests
