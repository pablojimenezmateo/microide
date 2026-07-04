#include "TestSupport.h"

#include <filesystem>
#include <fstream>
#include <string>

#include "project/GitPorcelainV2Parser.h"
#include "project/GitRepositoryState.h"
namespace microide::tests {
namespace {

using microide::project::GitConflictKind;
using microide::project::GitFileStatus;
using microide::project::GitHeadKind;
using microide::project::GitPorcelainV2Parser;
using microide::project::GitRefreshErrorCategory;
using microide::project::GitRepositoryEntryKind;

std::string ReadBinaryFixture(std::string_view relative_path) {
  std::ifstream stream(FixturePath(relative_path), std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
}

void TestPorcelainV2WorkingTreeFixture() {
  const std::string output = ReadBinaryFixture("git/porcelain-v2/working-tree.bin");
  const auto state = GitPorcelainV2Parser::Parse(output, "/repo", 1, 0);
  Expect(state.repo_available, "fixture should parse as available repository");
  Expect(state.branch.head_kind == GitHeadKind::Normal, "fixture should report normal branch head");
  Expect(state.branch.branch_name == "master", "fixture branch name mismatch");

  bool saw_modified = false;
  bool saw_deleted = false;
  bool saw_untracked = false;
  bool saw_spaced_path = false;
  for (const auto& entry : state.entries) {
    if (entry.path.relative_path == std::filesystem::path("README.md")) {
      saw_modified = entry.status == GitFileStatus::Modified;
    }
    if (entry.path.relative_path == std::filesystem::path("src/session.cpp")) {
      saw_deleted = entry.status == GitFileStatus::Deleted;
    }
    if (entry.path.relative_path == std::filesystem::path("file with spaces.txt")) {
      saw_spaced_path = true;
      saw_untracked = entry.kind == GitRepositoryEntryKind::Untracked;
      Expect(entry.path.display_label == "file with spaces.txt",
             "spaced path label should preserve spaces");
    }
  }
  Expect(saw_modified, "fixture should include modified README.md");
  Expect(saw_deleted, "fixture should include deleted src/session.cpp");
  Expect(saw_untracked && saw_spaced_path, "fixture should include spaced untracked path");
  Expect(state.tree_git_statuses.at("README.md") == GitFileStatus::Modified,
         "tree status map should include modified file");
}

void TestPorcelainV2RenamePair() {
  std::string output =
      "2 .R. N... 100644 100644 100644 abc def 100 new-name.txt";
  output.push_back('\0');
  output.append("old.txt");
  output.push_back('\0');
  std::size_t nul_count = 0;
  for (const char ch : output) {
    if (ch == '\0') {
      ++nul_count;
    }
  }
  Expect(nul_count == 2, "rename fixture should contain two NUL terminators");
  const auto state = GitPorcelainV2Parser::Parse(output, "/repo", 2, 0);
  Expect(state.entries.size() == 1, "rename fixture should produce one entry");
  Expect(state.entries.front().kind == GitRepositoryEntryKind::Renamed,
         "rename fixture entry should be classified as renamed");
  Expect(state.entries.front().path.relative_path == std::filesystem::path("new-name.txt"),
         "rename fixture should preserve the new path");
  Expect(state.entries.front().old_path.has_value(), "rename fixture should include old path");
  Expect(state.entries.front().old_path->relative_path == std::filesystem::path("old.txt"),
         "rename fixture old path mismatch");
}

void TestPorcelainV2RenamePathWithSpaces() {
  std::string output = "2 .R. N... 100644 100644 100644 abc def 100 new name.txt";
  output.push_back('\0');
  output.append("old path.txt");
  output.push_back('\0');
  const auto state = GitPorcelainV2Parser::Parse(output, "/repo", 4, 0);
  bool saw_rename = false;
  for (const auto& entry : state.entries) {
    if (entry.kind == GitRepositoryEntryKind::Renamed &&
        entry.path.relative_path == std::filesystem::path("new name.txt")) {
      saw_rename = entry.old_path.has_value() &&
                    entry.old_path->relative_path == std::filesystem::path("old path.txt");
    }
  }
  Expect(saw_rename, "rename pair should preserve spaced paths");
}

void TestPorcelainV2ConflictClassification() {
  // Match real `git status --porcelain=v2 -z` unmerged output (modes + object ids).
  std::string output =
      "u UU N... 100644 100644 100644 100644 "
      "89b24ecec50c07aef0d6640a2a9f6dc354a33125 "
      "6a58bd908e84644755a5a589e5f602267f5b6bbf5 "
      "f19b5ba9248e8c06a7c3c7d5f3127928028fe9ae "
      "conflict.txt";
  output.push_back('\0');
  const auto state = GitPorcelainV2Parser::Parse(output, "/repo", 3, 0);
  Expect(!state.entries.empty(), "conflict fixture should produce an entry");
  Expect(state.entries.front().kind == GitRepositoryEntryKind::Unmerged,
         "conflict fixture entry should be unmerged");
  Expect(state.entries.front().path.relative_path == std::filesystem::path("conflict.txt"),
         "conflict fixture should preserve path");
  Expect(state.entries.front().conflicted, "conflict entry should be marked conflicted");
  Expect(state.entries.front().conflict_kind == GitConflictKind::BothModified,
         "UU should classify as both modified");
}

void TestPorcelainV2ShortRecordDoesNotThrow() {
  // A truncated/adversarial `git status --porcelain=v2 -z` stream — e.g. a repo
  // with millions of entries whose final record is clipped to a single byte by
  // the 128 MiB capture cap — must not throw std::out_of_range from substr(2)
  // (which would propagate off the background worker and abort the process).
  std::string output = "1";  // one-byte record, no trailing NUL
  const auto state = GitPorcelainV2Parser::Parse(output, "/repo", 1, 0);
  Expect(state.entries.empty(), "one-byte record should be skipped, not parsed");

  // A valid entry followed by a one-byte trailing record: the good entry parses,
  // the short record is skipped without throwing.
  std::string mixed = "1 M. N... 100644 100644 100644 abc def README.md";
  mixed.push_back('\0');
  mixed.append("2");  // clipped trailing record
  const auto mixed_state = GitPorcelainV2Parser::Parse(mixed, "/repo", 1, 0);
  Expect(mixed_state.entries.size() == 1,
         "valid entry should parse while the short trailing record is skipped");
}

void TestRefreshFailureClassification() {
  Expect(microide::project::ClassifyGitRefreshFailure(
             128, "fatal: not a git repository (or any of the parent directories)") ==
             GitRefreshErrorCategory::NotARepo,
         "not a repo stderr should classify correctly");
  Expect(microide::project::ClassifyGitRefreshFailure(128, "Unable to create index.lock") ==
             GitRefreshErrorCategory::RepoLocked,
         "index lock stderr should classify as repo locked");
}

}  // namespace

void RegisterGitRepositoryStateTests(std::vector<TestCase>& tests) {
  AddTest(tests, "GitRepositoryState/PorcelainV2WorkingTreeFixture",
          TestPorcelainV2WorkingTreeFixture);
  AddTest(tests, "GitRepositoryState/PorcelainV2RenamePair", TestPorcelainV2RenamePair);
  AddTest(tests, "GitRepositoryState/PorcelainV2RenamePathWithSpaces",
          TestPorcelainV2RenamePathWithSpaces);
  AddTest(tests, "GitRepositoryState/PorcelainV2ConflictClassification",
          TestPorcelainV2ConflictClassification);
  AddTest(tests, "GitRepositoryState/PorcelainV2ShortRecordDoesNotThrow",
          TestPorcelainV2ShortRecordDoesNotThrow);
  AddTest(tests, "GitRepositoryState/RefreshFailureClassification",
          TestRefreshFailureClassification);
}

}  // namespace microide::tests
