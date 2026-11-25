#include "TestSupport.h"
#include "project/GitMetadataSource.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <utility>

#include "project/GitCommandUtil.h"
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
    if (entry.path.relative_path == "README.md") {
      saw_modified = entry.status == GitFileStatus::Modified;
    }
    if (entry.path.relative_path == "src/session.cpp") {
      saw_deleted = entry.status == GitFileStatus::Deleted;
    }
    if (entry.path.relative_path == "file with spaces.txt") {
      saw_spaced_path = true;
      saw_untracked = entry.kind == GitRepositoryEntryKind::Untracked;
      Expect(DisplayLabelView(entry.path) == "file with spaces.txt",
             "spaced path label should preserve spaces");
    }
  }
  Expect(saw_modified, "fixture should include modified README.md");
  Expect(saw_deleted, "fixture should include deleted src/session.cpp");
  Expect(saw_untracked && saw_spaced_path, "fixture should include spaced untracked path");
  Expect(state.tree_git_statuses->at("README.md") == GitFileStatus::Modified,
         "tree status map should include modified file");
}

void TestPorcelainV2TreeStatusesAreSharedNotCopied() {
  const std::string output = ReadBinaryFixture("git/porcelain-v2/working-tree.bin");
  const auto state = GitPorcelainV2Parser::Parse(output, "/repo", 1, 0);
  Expect(state.tree_git_statuses != nullptr && !state.tree_git_statuses->empty(),
         "the parser should publish a non-empty tree-status map");
  const microide::project::GitRepositoryState published = state;
  Expect(published.tree_git_statuses.get() == state.tree_git_statuses.get(),
         "copying a repository state must share the tree-status map, not duplicate it");
}

void TestPorcelainV2PathWithLeadingSpace() {
  std::string output = "1 .M. N... 100644 100644 100644 abc def  leading.cpp";
  output.push_back('\0');
  const auto state = GitPorcelainV2Parser::Parse(output, "/repo", 1, 0);
  bool saw_leading_space = false;
  for (const auto& entry : state.entries) {
    if (entry.path.relative_path == " leading.cpp") {
      saw_leading_space = true;
    }
  }
  Expect(saw_leading_space,
         "a path beginning with a space must be preserved verbatim, not trimmed");
}

void TestPorcelainV2UntrackedPathWithLeadingSpace() {
  std::string output = "?  leading.txt";
  output.push_back('\0');
  const auto state = GitPorcelainV2Parser::Parse(output, "/repo", 1, 0);
  bool saw_untracked_leading_space = false;
  for (const auto& entry : state.entries) {
    if (entry.kind == GitRepositoryEntryKind::Untracked &&
        entry.path.relative_path == " leading.txt") {
      saw_untracked_leading_space = true;
    }
  }
  Expect(saw_untracked_leading_space,
         "an untracked path beginning with a space must be preserved, not dropped");
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
  Expect(state.entries.front().path.relative_path == "new-name.txt",
         "rename fixture should preserve the new path");
  Expect(state.entries.front().old_path.has_value(), "rename fixture should include old path");
  Expect(state.entries.front().old_path->relative_path == "old.txt",
         "rename fixture old path mismatch");
  Expect(state.tree_git_statuses->count("old.txt") == 1 &&
             state.tree_git_statuses->at("old.txt") == GitFileStatus::Deleted,
         "rename source should be badged Deleted in the tree status map");
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
        entry.path.relative_path == "new name.txt") {
      saw_rename = entry.old_path.has_value() &&
                    entry.old_path->relative_path == "old path.txt";
    }
  }
  Expect(saw_rename, "rename pair should preserve spaced paths");
}

void TestPorcelainV2RenameSourceStartingWithStatusSigil() {
  std::string output = "2 R. N... 100644 100644 100644 abc def R100 renamed_a.txt";
  output.push_back('\0');
  output.append("2data.txt");
  output.push_back('\0');
  output.append("2 R. N... 100644 100644 100644 abc def R100 renamed_b.txt");
  output.push_back('\0');
  output.append("user.txt");
  output.push_back('\0');

  const auto state = GitPorcelainV2Parser::Parse(output, "/repo", 5, 0);
  Expect(state.entries.size() == 2,
         "both renames should parse without the source being misread as an entry");

  bool saw_a = false;
  bool saw_b = false;
  for (const auto& entry : state.entries) {
    Expect(entry.kind == GitRepositoryEntryKind::Renamed, "both entries should be renames");
    if (entry.path.relative_path == "renamed_a.txt") {
      saw_a = entry.old_path.has_value() &&
              entry.old_path->relative_path == "2data.txt";
    }
    if (entry.path.relative_path == "renamed_b.txt") {
      saw_b = entry.old_path.has_value() &&
              entry.old_path->relative_path == "user.txt";
    }
  }
  Expect(saw_a, "rename source '2data.txt' should be preserved as old_path");
  Expect(saw_b, "rename source 'user.txt' should be preserved as old_path");
  Expect(state.tree_git_statuses->count("2data.txt") == 1,
         "rename source should receive a tree git status");
  Expect(state.tree_git_statuses->count("user.txt") == 1,
         "rename source should receive a tree git status");
}

void TestPorcelainV2ConflictClassification() {
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
  Expect(state.entries.front().path.relative_path == "conflict.txt",
         "conflict fixture should preserve path");
  Expect(state.entries.front().conflicted, "conflict entry should be marked conflicted");
  Expect(state.entries.front().conflict_kind == GitConflictKind::BothModified,
         "UU should classify as both modified");
}

void TestPorcelainV2ShortRecordDoesNotThrow() {
  std::string output = "1";
  const auto state = GitPorcelainV2Parser::Parse(output, "/repo", 1, 0);
  Expect(state.entries.empty(), "one-byte record should be skipped, not parsed");

  std::string mixed = "1 M. N... 100644 100644 100644 abc def README.md";
  mixed.push_back('\0');
  mixed.append("2");
  const auto mixed_state = GitPorcelainV2Parser::Parse(mixed, "/repo", 1, 0);
  Expect(mixed_state.entries.size() == 1,
         "valid entry should parse while the short trailing record is skipped");
}

void TestPorcelainV2AheadBehindParsing() {
  const auto parse_ab = [](std::string_view token) {
    std::string output = "# branch.ab ";
    output.append(token);
    output.push_back('\0');
    const auto state = GitPorcelainV2Parser::Parse(output, "/repo", 1, 0);
    return std::pair<int, int>{state.branch.ahead, state.branch.behind};
  };

  Expect(parse_ab("+12-3") == (std::pair<int, int>{12, 3}),
         "\"+12-3\" should parse ahead=12 behind=3");
  Expect(parse_ab("+2 -0") == (std::pair<int, int>{2, 0}),
         "genuine git \"+2 -0\" must parse ahead=2 behind=0");

  Expect(parse_ab("+12x-3") == (std::pair<int, int>{0, 0}),
         "\"+12x-3\" must be rejected, not truncated to ahead=12");
  Expect(parse_ab("+12-3x") == (std::pair<int, int>{0, 0}),
         "\"+12-3x\" must be rejected, not truncated to behind=3");
  Expect(parse_ab("+x-3") == (std::pair<int, int>{0, 0}),
         "\"+x-3\" (non-numeric ahead) must be rejected");
  Expect(parse_ab("+12-x") == (std::pair<int, int>{0, 0}),
         "\"+12-x\" (non-numeric behind) must be rejected");
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

void TestPorcelainV2CapturesSubmoduleField() {
  const auto parse_one = [](std::string record) {
    record.push_back('\0');
    return GitPorcelainV2Parser::Parse(record, "/repo", 1, 0);
  };

  const auto gitlink = parse_one(
      "u UU S.M. 160000 160000 160000 160000 aaa bbb ccc vendor/dep");
  Expect(gitlink.entries.size() == 1, "one unmerged record parses to one entry");
  Expect(gitlink.entries.front().submodule,
         "an `S`-prefixed <sub> field marks the entry as a submodule");

  const auto ordinary = parse_one(
      "u UU N... 100644 100644 100644 100644 aaa bbb ccc src/main.cpp");
  Expect(!ordinary.entries.front().submodule,
         "an `N`-prefixed <sub> field is not a submodule");

  const auto ordinary_submodule =
      parse_one("1 .M SC.. 160000 160000 160000 aaa bbb vendor/dep");
  Expect(ordinary_submodule.entries.front().submodule,
         "a `1` record's <sub> field is read from the same position");
}

void TestReadFirstLineOfGitFile() {
  namespace gitutil = microide::project::internal;
  TemporaryDirectory temp_dir;
  const std::filesystem::path base = temp_dir.path();

  WriteFile(base / "plain", "refs/heads/main\nsecond line\n");
  Expect(gitutil::ReadFirstLineOfGitFile(base / "plain") == "refs/heads/main",
         "the first line is returned without its newline");

  WriteFile(base / "crlf", "ref: refs/heads/main\r\n");
  Expect(gitutil::ReadFirstLineOfGitFile(base / "crlf") == "ref: refs/heads/main",
         "a trailing CR is stripped — a repo touched from Windows still resolves");

  WriteFile(base / "no-newline", "0123456789abcdef");
  Expect(gitutil::ReadFirstLineOfGitFile(base / "no-newline") == "0123456789abcdef",
         "a file with no trailing newline still yields its line");

  WriteFile(base / "empty", "");
  Expect(gitutil::ReadFirstLineOfGitFile(base / "empty") == std::string(),
         "an empty file reads as an empty line, not as unreadable");

  Expect(!gitutil::ReadFirstLineOfGitFile(base / "absent").has_value(),
         "a missing file is unreadable");
  Expect(!gitutil::ReadFirstLineOfGitFile(base).has_value(),
         "a directory is not a metadata file");

  WriteFile(base / "huge", std::string(gitutil::kMaxGitMetadataLineBytes * 4, 'a'));
  const std::optional<std::string> capped = gitutil::ReadFirstLineOfGitFile(base / "huge");
  Expect(capped.has_value() && capped->size() == gitutil::kMaxGitMetadataLineBytes,
         "an unterminated line is truncated at the cap rather than read whole");
}

void TestResolveGitDirectoryLayouts() {
  namespace gitutil = microide::project::internal;
  TemporaryDirectory temp_dir;
  const std::filesystem::path base = temp_dir.path();

  Expect(!gitutil::ResolveGitDirectory(base / "not-a-repo").has_value(),
         "a directory with no .git marker has no git directory");

  const std::filesystem::path plain = base / "plain";
  std::filesystem::create_directories(plain / ".git");
  Expect(gitutil::ResolveGitDirectory(plain) == plain / ".git",
         "an ordinary checkout resolves to <root>/.git");

  const std::filesystem::path linked = base / "linked";
  std::filesystem::create_directories(linked);
  std::filesystem::create_directories(base / "store" / "wt");
  WriteFile(linked / ".git", "gitdir: ../store/wt\n");
  Expect(gitutil::ResolveGitDirectory(linked) == (base / "store" / "wt").lexically_normal(),
         "a relative gitdir: target resolves against the worktree root");

  WriteFile(linked / ".git", "gitdir: " + (base / "store" / "wt" / ".." / "wt").string() + "\n");
  Expect(gitutil::ResolveGitDirectory(linked) == (base / "store" / "wt").lexically_normal(),
         "an absolute gitdir: target is normalized too, not returned verbatim");

  WriteFile(linked / ".git", "something else\n");
  Expect(!gitutil::ResolveGitDirectory(linked).has_value(),
         "a .git file without a gitdir: prefix names no git directory");
}

void TestReadPendingMergeHeadId() {
  namespace gitutil = microide::project::internal;
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  std::filesystem::create_directories(root / ".git");

  Expect(!gitutil::ReadPendingMergeHeadId(root, project::LocalGitMetadataSource()).has_value(),
         "no MERGE_HEAD means no pending merge");

  const std::string oid = "0123456789abcdef0123456789abcdef01234567";
  WriteFile(root / ".git/MERGE_HEAD", oid + "\n");
  Expect(gitutil::ReadPendingMergeHeadId(root, project::LocalGitMetadataSource()) == oid, "a plain MERGE_HEAD id is read verbatim");

  const std::string second = "89abcdef0123456789abcdef0123456789abcdef";
  WriteFile(root / ".git/MERGE_HEAD", oid + "\n" + second + "\n");
  Expect(gitutil::ReadPendingMergeHeadId(root, project::LocalGitMetadataSource()) == oid, "an octopus merge reports the first id");

  WriteFile(root / ".git/MERGE_HEAD", oid + "  \r\n");
  Expect(gitutil::ReadPendingMergeHeadId(root, project::LocalGitMetadataSource()) == oid, "trailing whitespace/CR is trimmed");

  for (const std::string_view junk : {"ref: refs/heads/topic", "not-hex-at-all-xxxxx", "abc", ""}) {
    WriteFile(root / ".git/MERGE_HEAD", std::string(junk) + "\n");
    Expect(!gitutil::ReadPendingMergeHeadId(root, project::LocalGitMetadataSource()).has_value(),
           "a MERGE_HEAD that is not a plain object id is rejected");
  }

  const std::filesystem::path worktree = temp_dir.path() / "wt";
  const std::filesystem::path linked_gitdir = temp_dir.path() / "gitdir";
  std::filesystem::create_directories(worktree);
  std::filesystem::create_directories(linked_gitdir);
  WriteFile(worktree / ".git", "gitdir: " + linked_gitdir.string() + "\n");
  WriteFile(linked_gitdir / "MERGE_HEAD", oid + "\n");
  Expect(gitutil::ReadPendingMergeHeadId(worktree, project::LocalGitMetadataSource()) == oid,
         "a `.git` file indirection resolves to the linked git directory");
  Expect(gitutil::ResolveGitDirectory(worktree) == linked_gitdir,
         "ResolveGitDirectory follows the gitdir: pointer");

  const std::filesystem::path relative_worktree = temp_dir.path() / "rel";
  std::filesystem::create_directories(relative_worktree / "store");
  WriteFile(relative_worktree / ".git", "gitdir: store\n");
  WriteFile(relative_worktree / "store/MERGE_HEAD", oid + "\n");
  Expect(gitutil::ReadPendingMergeHeadId(relative_worktree, project::LocalGitMetadataSource()) == oid,
         "a relative gitdir: pointer resolves against the worktree root");

  WriteFile(relative_worktree / ".git", "not a gitdir pointer\n");
  Expect(!gitutil::ResolveGitDirectory(relative_worktree).has_value(),
         "a `.git` file without a gitdir: pointer yields no git directory");
}

void TestDetectGitOperationState() {
  using microide::project::DetectGitOperationState;
  using microide::project::GitOperationStateKind;
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  std::filesystem::create_directories(root / ".git");

  Expect(DetectGitOperationState(root, project::LocalGitMetadataSource()) == GitOperationStateKind::None,
         "a clean repository reports no in-flight operation");
  Expect(DetectGitOperationState(temp_dir.path() / "missing", project::LocalGitMetadataSource()) == GitOperationStateKind::None,
         "a path with no git directory reports no in-flight operation");

  WriteFile(root / ".git/AUTO_MERGE", "0123456789abcdef0123456789abcdef01234567\n");
  WriteFile(root / ".git/MERGE_MSG", "Merge branch 'side'\n");
  Expect(DetectGitOperationState(root, project::LocalGitMetadataSource()) == GitOperationStateKind::None,
         "AUTO_MERGE/MERGE_MSG alone are not an in-flight merge");

  WriteFile(root / ".git/BISECT_LOG", "log\n");
  Expect(DetectGitOperationState(root, project::LocalGitMetadataSource()) == GitOperationStateKind::Bisect,
         "BISECT_LOG reports a bisect");
  WriteFile(root / ".git/REVERT_HEAD", "0123456789abcdef0123456789abcdef01234567\n");
  Expect(DetectGitOperationState(root, project::LocalGitMetadataSource()) == GitOperationStateKind::Revert,
         "REVERT_HEAD outranks BISECT_LOG");
  WriteFile(root / ".git/CHERRY_PICK_HEAD", "0123456789abcdef0123456789abcdef01234567\n");
  Expect(DetectGitOperationState(root, project::LocalGitMetadataSource()) == GitOperationStateKind::CherryPick,
         "CHERRY_PICK_HEAD outranks REVERT_HEAD");
  std::filesystem::create_directories(root / ".git/rebase-merge");
  Expect(DetectGitOperationState(root, project::LocalGitMetadataSource()) == GitOperationStateKind::Rebase,
         "a rebase state directory outranks the sequencer heads");
  WriteFile(root / ".git/MERGE_HEAD", "0123456789abcdef0123456789abcdef01234567\n");
  Expect(DetectGitOperationState(root, project::LocalGitMetadataSource()) == GitOperationStateKind::Merge,
         "MERGE_HEAD outranks every other marker, matching git's own precedence");

  const std::filesystem::path apply_root = temp_dir.path() / "apply";
  std::filesystem::create_directories(apply_root / ".git/rebase-apply");
  Expect(DetectGitOperationState(apply_root, project::LocalGitMetadataSource()) == GitOperationStateKind::Rebase,
         "rebase-apply also reports a rebase");
}

}

void RegisterGitRepositoryStateTests(std::vector<TestCase>& tests) {
  AddTest(tests, "GitRepositoryState/PorcelainV2WorkingTreeFixture",
          TestPorcelainV2WorkingTreeFixture);
  AddTest(tests, "GitRepositoryState/PorcelainV2TreeStatusesAreSharedNotCopied",
          TestPorcelainV2TreeStatusesAreSharedNotCopied);
  AddTest(tests, "GitRepositoryState/PorcelainV2PathWithLeadingSpace",
          TestPorcelainV2PathWithLeadingSpace);
  AddTest(tests, "GitRepositoryState/PorcelainV2UntrackedPathWithLeadingSpace",
          TestPorcelainV2UntrackedPathWithLeadingSpace);
  AddTest(tests, "GitRepositoryState/PorcelainV2RenamePair", TestPorcelainV2RenamePair);
  AddTest(tests, "GitRepositoryState/PorcelainV2RenamePathWithSpaces",
          TestPorcelainV2RenamePathWithSpaces);
  AddTest(tests, "GitRepositoryState/PorcelainV2RenameSourceStartingWithStatusSigil",
          TestPorcelainV2RenameSourceStartingWithStatusSigil);
  AddTest(tests, "GitRepositoryState/PorcelainV2ConflictClassification",
          TestPorcelainV2ConflictClassification);
  AddTest(tests, "GitRepositoryState/PorcelainV2ShortRecordDoesNotThrow",
          TestPorcelainV2ShortRecordDoesNotThrow);
  AddTest(tests, "GitRepositoryState/PorcelainV2AheadBehindParsing",
          TestPorcelainV2AheadBehindParsing);
  AddTest(tests, "GitRepositoryState/RefreshFailureClassification",
          TestRefreshFailureClassification);
  AddTest(tests, "GitRepositoryState/PorcelainV2CapturesSubmoduleField",
          TestPorcelainV2CapturesSubmoduleField);
  AddTest(tests, "GitRepositoryState/ReadFirstLineOfGitFile", TestReadFirstLineOfGitFile);
  AddTest(tests, "GitRepositoryState/ResolveGitDirectoryLayouts", TestResolveGitDirectoryLayouts);
  AddTest(tests, "GitRepositoryState/ReadPendingMergeHeadId", TestReadPendingMergeHeadId);
  AddTest(tests, "GitRepositoryState/DetectGitOperationState", TestDetectGitOperationState);
}

}
